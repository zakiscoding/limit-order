#include "lob/latency.hpp"

#include "lob/book.hpp"
#include "lob/engine.hpp"
#include "lob/timing.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace lob {
namespace {

// Nearest-rank percentile of an ascending-sorted vector: the smallest value
// with at least p of the samples at or below it.
double percentile(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) {
        return 0;
    }
    std::size_t rank = static_cast<std::size_t>(std::ceil(p * static_cast<double>(sorted.size())));
    rank = std::min(std::max<std::size_t>(rank, 1), sorted.size());
    return sorted[rank - 1];
}

double median_of(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

struct Stats {
    double p50 = 0, p90 = 0, p95 = 0, p99 = 0, p999 = 0, p9999 = 0, max = 0, mean = 0;
    uint64_t n = 0;
};

Stats summarize(std::vector<double>& ns) {
    Stats s;
    s.n = ns.size();
    if (ns.empty()) {
        return s;
    }
    std::sort(ns.begin(), ns.end());
    long double sum = 0;
    for (double v : ns) {
        sum += v;
    }
    s.mean = static_cast<double>(sum / ns.size());
    s.p50 = percentile(ns, 0.50);
    s.p90 = percentile(ns, 0.90);
    s.p95 = percentile(ns, 0.95);
    s.p99 = percentile(ns, 0.99);
    s.p999 = percentile(ns, 0.999);
    s.p9999 = percentile(ns, 0.9999);
    s.max = ns.back();
    return s;
}

// TSC ticks per nanosecond, from five 100 ms spins against the monotonic clock.
struct Calibration {
    double ticks_per_ns = 1.0;
    double min = 1.0;
    double max = 1.0;
};

Calibration calibrate() {
    Calibration c;
    if (!kHaveTsc) {
        return c;
    }
    std::vector<double> estimates;
    for (int i = 0; i < 5; ++i) {
        const uint64_t t0 = monotonic_ns();
        const uint64_t c0 = tsc_begin();
        while (monotonic_ns() - t0 < 100000000ull) {
        }
        const uint64_t c1 = tsc_end();
        const uint64_t t1 = monotonic_ns();
        estimates.push_back(static_cast<double>(c1 - c0) / static_cast<double>(t1 - t0));
    }
    c.min = *std::min_element(estimates.begin(), estimates.end());
    c.max = *std::max_element(estimates.begin(), estimates.end());
    c.ticks_per_ns = median_of(estimates);
    return c;
}

// Cost of one empty tsc_begin/tsc_end pair, in ticks.
struct Overhead {
    uint64_t median = 0;
    uint64_t min = 0;
};

Overhead measure_overhead() {
    constexpr int kSamples = 1000000;
    std::vector<uint64_t> deltas(kSamples);
    for (int i = 0; i < kSamples; ++i) {
        const uint64_t a = tsc_begin();
        const uint64_t b = tsc_end();
        deltas[static_cast<std::size_t>(i)] = b - a;
    }
    std::nth_element(deltas.begin(), deltas.begin() + kSamples / 2, deltas.end());
    Overhead o;
    o.median = deltas[kSamples / 2];
    o.min = *std::min_element(deltas.begin(), deltas.end());
    return o;
}

struct RunStats {
    Stats all;
    Stats add, cancel, modify;
    uint64_t book_hash = 0;
    uint64_t trade_hash = 0;
    std::size_t slowest_index = 0;  // event index of the slowest apply in this run
    uint64_t over_1us = 0;          // events slower than 1 microsecond
};

const char* kMetricNames[] = {"p50", "p90", "p95", "p99", "p99.9", "p99.99", "max", "mean"};

double metric(const Stats& s, int i) {
    const double values[] = {s.p50, s.p90, s.p95, s.p99, s.p999, s.p9999, s.max, s.mean};
    return values[i];
}

void json_stats(std::ostream& out, const Stats& s) {
    out << "{\"n\": " << s.n << ", \"p50\": " << s.p50 << ", \"p90\": " << s.p90 << ", \"p95\": " << s.p95
        << ", \"p99\": " << s.p99 << ", \"p99.9\": " << s.p999 << ", \"p99.99\": " << s.p9999
        << ", \"max\": " << s.max << ", \"mean\": " << s.mean << "}";
}

}  // namespace

double nearest_rank_percentile(const std::vector<double>& ascending, double p) { return percentile(ascending, p); }

int latency_selftest() {
    int failures = 0;
    auto expect = [&](const char* what, double got, double want) {
        if (got != want) {
            std::cerr << "FAIL " << what << ": got " << got << ", want " << want << '\n';
            ++failures;
        }
    };

    // Nearest rank on 1..N: the p-th percentile is ceil(p * N).
    std::vector<double> hundred;
    for (int i = 1; i <= 100; ++i) {
        hundred.push_back(i);
    }
    expect("p50 of 1..100", percentile(hundred, 0.50), 50);
    expect("p95 of 1..100", percentile(hundred, 0.95), 95);
    expect("p99 of 1..100", percentile(hundred, 0.99), 99);
    expect("p99.9 of 1..100", percentile(hundred, 0.999), 100);
    expect("p0 of 1..100", percentile(hundred, 0.0), 1);

    std::vector<double> thousand;
    for (int i = 1; i <= 1000; ++i) {
        thousand.push_back(i);
    }
    expect("p99.9 of 1..1000", percentile(thousand, 0.999), 999);
    expect("p99 of 1..1000", percentile(thousand, 0.99), 990);

    std::vector<double> one = {7};
    expect("p99 of one sample", percentile(one, 0.99), 7);

    // A heavy tail: 99 fast samples and one slow one. p99 must stay fast; max must not.
    std::vector<double> tail(99, 10.0);
    tail.push_back(5000.0);
    Stats t = summarize(tail);
    expect("tail p99", t.p99, 10);
    expect("tail max", t.max, 5000);
    expect("tail p50", t.p50, 10);

    // Timer: spin for 2 microseconds of monotonic time and time it with the TSC pair.
    // The minimum over many trials must land near 2000 ns. This checks both the
    // calibration and the overhead subtraction.
    const Calibration cal = calibrate();
    const Overhead overhead = measure_overhead();
    double best = 1e18;
    for (int trial = 0; trial < 300; ++trial) {
        const uint64_t t0 = tsc_begin();
        const uint64_t m0 = monotonic_ns();
        while (monotonic_ns() - m0 < 2000) {
        }
        const uint64_t t1 = tsc_end();
        const uint64_t d = (t1 - t0) > overhead.median ? (t1 - t0) - overhead.median : 0;
        best = std::min(best, static_cast<double>(d) / cal.ticks_per_ns);
    }
    std::cout << "timer check: 2000 ns spin measured as min " << std::fixed << std::setprecision(1) << best
              << " ns (ticks_per_ns " << cal.ticks_per_ns << ")\n";
    if (best < 1950 || best > 2600) {
        std::cerr << "FAIL timer: expected about 2000 ns, got " << best << '\n';
        ++failures;
    }

    if (failures == 0) {
        std::cout << "latency selftest OK\n";
    }
    return failures == 0 ? 0 : 1;
}

int run_latency(const LatencyConfig& config) {
    if (config.count == 0 || config.runs <= 0) {
        throw std::runtime_error("count and runs must be positive");
    }

    const Calibration cal = calibrate();
    const Overhead overhead = measure_overhead();
    const double overhead_ns = static_cast<double>(overhead.median) / cal.ticks_per_ns;

    std::cout << std::fixed << std::setprecision(3);
    std::cout << "latency_config seed=" << config.seed << " events=" << config.count << " runs=" << config.runs
              << " timer=" << (kHaveTsc ? "rdtsc_lfence/rdtscp_lfence" : "steady_clock")
              << " ticks_per_ns=" << cal.ticks_per_ns << " ticks_per_ns_min=" << cal.min
              << " ticks_per_ns_max=" << cal.max << " timer_overhead_ticks_median=" << overhead.median
              << " timer_overhead_ticks_min=" << overhead.min << " timer_overhead_ns=" << overhead_ns
              << " percentile_method=nearest_rank\n";

    const std::vector<Event> events = generate(config.seed, config.count);
    const TypeCounts types = count_types(events);
    std::cout << "stream adds=" << types.add << " cancels=" << types.cancel << " modifies=" << types.modify << '\n';

    const std::size_t n = events.size();
    std::vector<uint32_t> ticks(n);
    std::vector<RunStats> runs;
    uint64_t ref_book = 0;
    uint64_t ref_trade = 0;

    // Run 0 is a warmup and is discarded; its hashes are the determinism reference.
    for (int run = 0; run <= config.runs; ++run) {
        Book book;
        for (std::size_t i = 0; i < n; ++i) {
            const uint64_t t0 = tsc_begin();
            book.apply(events[i]);
            const uint64_t t1 = tsc_end();
            uint64_t d = t1 - t0;
            d = d > overhead.median ? d - overhead.median : 0;
            ticks[i] = d > std::numeric_limits<uint32_t>::max() ? std::numeric_limits<uint32_t>::max()
                                                                : static_cast<uint32_t>(d);
        }
        if (run == 0) {
            ref_book = book.book_hash();
            ref_trade = book.trade_hash();
            continue;
        }
        if (book.book_hash() != ref_book || book.trade_hash() != ref_trade) {
            throw std::runtime_error("latency run diverged from the warmup; the book is not deterministic");
        }

        std::vector<double> all, add, cancel, modify;
        all.reserve(n);
        std::size_t slowest = 0;
        uint64_t over_1us = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const double ns = static_cast<double>(ticks[i]) / cal.ticks_per_ns;
            if (ticks[i] > ticks[slowest]) {
                slowest = i;
            }
            if (ns > 1000.0) {
                ++over_1us;
            }
            all.push_back(ns);
            switch (events[i].type) {
                case EventType::Add: add.push_back(ns); break;
                case EventType::Cancel: cancel.push_back(ns); break;
                case EventType::Modify: modify.push_back(ns); break;
                default: break;
            }
        }
        RunStats r;
        r.all = summarize(all);
        r.add = summarize(add);
        r.cancel = summarize(cancel);
        r.modify = summarize(modify);
        r.book_hash = book.book_hash();
        r.trade_hash = book.trade_hash();
        r.slowest_index = slowest;
        r.over_1us = over_1us;
        runs.push_back(r);
        std::cout << "run " << run << " p50_ns=" << r.all.p50 << " p90_ns=" << r.all.p90 << " p95_ns=" << r.all.p95
                  << " p99_ns=" << r.all.p99 << " p99.9_ns=" << r.all.p999 << " p99.99_ns=" << r.all.p9999
                  << " max_ns=" << r.all.max << " mean_ns=" << r.all.mean << " slowest_event=" << r.slowest_index
                  << " over_1us=" << r.over_1us << '\n';
    }

    // Across runs: median of each run's value, with the min and max run.
    auto across = [&](const Stats RunStats::*group, int m, double* lo, double* hi) {
        std::vector<double> v;
        for (const RunStats& r : runs) {
            v.push_back(metric(r.*group, m));
        }
        *lo = *std::min_element(v.begin(), v.end());
        *hi = *std::max_element(v.begin(), v.end());
        return median_of(v);
    };

    struct Group {
        const char* name;
        Stats RunStats::*member;
    };
    const Group groups[] = {{"all", &RunStats::all},
                            {"add", &RunStats::add},
                            {"cancel", &RunStats::cancel},
                            {"modify", &RunStats::modify}};

    std::cout << "summary (ns, median of " << config.runs << " runs, [min..max] across runs)\n";
    for (const Group& g : groups) {
        for (int m : {0, 2, 3, 4, 5, 6}) {
            double lo = 0, hi = 0;
            const double med = across(g.member, m, &lo, &hi);
            std::cout << "  " << std::setw(8) << std::left << g.name << std::setw(8) << kMetricNames[m]
                      << std::right << std::setw(12) << med << "  [" << lo << ".." << hi << "]\n";
        }
    }

    double lo = 0, hi = 0;
    const double p50 = across(&RunStats::all, 0, &lo, &hi);
    const double p95 = across(&RunStats::all, 2, &lo, &hi);
    const double p99 = across(&RunStats::all, 3, &lo, &hi);
    const double p999 = across(&RunStats::all, 4, &lo, &hi);
    std::cout << std::hex << "LATENCY events=" << std::dec << n << " seed=" << config.seed << " runs=" << config.runs
              << std::fixed << std::setprecision(3) << " p50_ns=" << p50 << " p95_ns=" << p95 << " p99_ns=" << p99
              << " p999_ns=" << p999 << std::hex << " book=" << ref_book << " trade_hash=" << ref_trade << std::dec
              << '\n';

    if (!config.json_path.empty()) {
        std::ofstream out(config.json_path);
        if (!out) {
            throw std::runtime_error("cannot write " + config.json_path);
        }
        out << std::fixed << std::setprecision(3);
        out << "{\n  \"unit\": \"ns\",\n  \"percentile_method\": \"nearest_rank\",\n"
            << "  \"seed\": " << config.seed << ",\n  \"events\": " << n << ",\n  \"runs\": " << config.runs
            << ",\n  \"timer\": \"" << (kHaveTsc ? "rdtsc_lfence/rdtscp_lfence" : "steady_clock") << "\",\n"
            << "  \"ticks_per_ns\": " << cal.ticks_per_ns << ",\n  \"timer_overhead_ns\": " << overhead_ns << ",\n"
            << "  \"book_hash\": \"" << std::hex << ref_book << "\",\n  \"trade_hash\": \"" << ref_trade << std::dec
            << "\",\n  \"per_run\": [\n";
        for (std::size_t r = 0; r < runs.size(); ++r) {
            out << "    {\"all\": ";
            json_stats(out, runs[r].all);
            out << ", \"add\": ";
            json_stats(out, runs[r].add);
            out << ", \"cancel\": ";
            json_stats(out, runs[r].cancel);
            out << ", \"modify\": ";
            json_stats(out, runs[r].modify);
            out << ", \"slowest_event_index\": " << runs[r].slowest_index << ", \"events_over_1us\": "
                << runs[r].over_1us << "}" << (r + 1 < runs.size() ? "," : "") << "\n";
        }
        out << "  ]\n}\n";
        std::cout << "wrote " << config.json_path << '\n';
    }
    return 0;
}

}  // namespace lob
