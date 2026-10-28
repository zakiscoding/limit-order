#include "lob/engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

struct Options {
    std::unordered_map<std::string, std::string> kv;

    bool has(const std::string& key) const { return kv.find(key) != kv.end(); }

    std::string get(const std::string& key, const std::string& fallback = "") const {
        auto it = kv.find(key);
        if (it == kv.end()) {
            return fallback;
        }
        return it->second;
    }

    uint64_t u64(const std::string& key, uint64_t fallback) const {
        if (!has(key)) {
            return fallback;
        }
        return std::stoull(get(key));
    }

    bool flag(const std::string& key) const { return has(key) && get(key) != "false"; }
};

Options parse_options(int argc, char** argv) {
    Options options;
    for (int i = 2; i < argc; ++i) {
        std::string token = argv[i];
        if (token.rfind("--", 0) != 0) {
            throw std::runtime_error("unexpected argument " + token);
        }
        std::string key = token.substr(2);
        if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) {
            options.kv[key] = argv[++i];
        } else {
            options.kv[key] = "true";
        }
    }
    return options;
}

void usage() {
    std::cerr
        << "usage:\n"
        << "  lob_engine selftest --scenarios FILE --expected FILE\n"
        << "  lob_engine generate --seed N --count N --output FILE\n"
        << "  lob_engine replay --input FILE --output FILE [--checkpoint N] [--verify]\n"
        << "  lob_engine bench --seed N --count N [--runs N]\n"
        << "  lob_engine profile --seed N --count N\n";
}

#if defined(_WIN32)
void cpuid(uint32_t leaf, uint32_t sub, uint32_t out[4]) {
#if defined(_MSC_VER)
    int info[4];
    __cpuidex(info, static_cast<int>(leaf), static_cast<int>(sub));
    for (int i = 0; i < 4; ++i) {
        out[i] = static_cast<uint32_t>(info[i]);
    }
#else
    __asm__ volatile("cpuid"
                     : "=a"(out[0]), "=b"(out[1]), "=c"(out[2]), "=d"(out[3])
                     : "a"(leaf), "c"(sub));
#endif
}

std::string cpu_brand() {
    uint32_t regs[4] = {};
    cpuid(0x80000000u, 0, regs);
    if (regs[0] < 0x80000004u) {
        return "unknown";
    }
    char brand[49] = {};
    for (uint32_t leaf = 0x80000002u, offset = 0; leaf <= 0x80000004u; ++leaf, offset += 16) {
        cpuid(leaf, 0, regs);
        std::memcpy(brand + offset, regs, 16);
    }
    return std::string(brand);
}

uint64_t qpc_now() {
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return static_cast<uint64_t>(counter.QuadPart);
}

uint64_t qpc_freq() {
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    return static_cast<uint64_t>(freq.QuadPart);
}

void pin_thread() {
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    DWORD_PTR mask = info.dwNumberOfProcessors >= 4 ? (DWORD_PTR{1} << 2) : DWORD_PTR{1};
    if (SetThreadAffinityMask(GetCurrentThread(), mask) == 0) {
        std::cerr << "warning: could not pin the benchmark thread\n";
    }
    if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST)) {
        std::cerr << "warning: could not raise thread priority\n";
    }
}

void print_hardware() {
    MEMORYSTATUSEX memory;
    memory.dwLength = sizeof(memory);
    GlobalMemoryStatusEx(&memory);
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    std::cout << "hardware cpu=\"" << cpu_brand() << "\""
              << " ram_bytes=" << memory.ullTotalPhys
              << " logical_processors=" << info.dwNumberOfProcessors
              << " qpc_hz=" << qpc_freq() << '\n';
}

uint64_t qpc_overhead_ticks() {
    constexpr int kSamples = 200000;
    std::vector<uint64_t> deltas(static_cast<std::size_t>(kSamples));
    for (int i = 0; i < kSamples; ++i) {
        uint64_t start = qpc_now();
        uint64_t stop = qpc_now();
        deltas[static_cast<std::size_t>(i)] = stop - start;
    }
    std::nth_element(deltas.begin(), deltas.begin() + kSamples / 2, deltas.end());
    return deltas[static_cast<std::size_t>(kSamples / 2)];
}

uint32_t percentile_sorted(const std::vector<uint32_t>& samples, double p) {
    if (samples.empty()) {
        return 0;
    }
    std::size_t rank = static_cast<std::size_t>(std::ceil(p * static_cast<double>(samples.size())));
    if (rank == 0) {
        rank = 1;
    }
    std::size_t index = std::min(samples.size(), rank) - 1;
    return samples[index];
}

const char* compiler_id() {
#if defined(__clang__)
    return "clang " __clang_version__;
#elif defined(_MSC_VER)
    return "msvc";
#elif defined(__GNUC__)
    return "gcc";
#else
    return "unknown";
#endif
}

int run_bench(const Options& options, bool instrumented_profile) {
    const uint64_t seed = options.u64("seed", 42);
    const uint64_t count = options.u64("count", instrumented_profile ? 1000000 : 2000000);
    const int runs = static_cast<int>(options.u64("runs", instrumented_profile ? 1 : 5));
    if (count == 0 || runs <= 0) {
        throw std::runtime_error("count and runs must be positive");
    }

    pin_thread();
    print_hardware();
    std::cout << "compiler " << compiler_id()
#ifdef NDEBUG
              << " config=Release"
#else
              << " config=Debug"
#endif
#ifdef LOB_INSTRUMENT
              << " instrument=on"
#else
              << " instrument=off"
#endif
              << '\n';
    std::cout << "workload generator=splitmix64 seed=" << seed << " events=" << count
              << " actions=add:55% cancel:35% modify:10%_when_live aggressive_adds=30%"
              << " price_ticks=1 fair_walk=8000..12000 qty=1..200 tif=GTC"
              << " timing=in_process_single_thread pinned=logical_cpu_2_if_available\n";

    std::vector<lob::Event> events = lob::generate(seed, count);
    lob::TypeCounts types = lob::count_types(events);
    std::cout << "stream adds=" << types.add << " cancels=" << types.cancel << " modifies=" << types.modify
              << '\n';

    auto time_once = [&]() {
        lob::Book book;
        uint64_t start = qpc_now();
        for (const lob::Event& event : events) {
            book.apply(event);
        }
        uint64_t stop = qpc_now();
        struct Result {
            double seconds = 0;
            uint64_t book_hash = 0;
            uint64_t trade_hash = 0;
            uint64_t trades = 0;
            uint64_t rejects = 0;
            uint64_t orders = 0;
            lob::Book::Depth depth;
        };
        Result result;
        result.seconds = static_cast<double>(stop - start) / static_cast<double>(qpc_freq());
        result.book_hash = book.book_hash();
        result.trade_hash = book.trade_hash();
        result.trades = book.trade_count();
        result.rejects = book.reject_count();
        result.orders = book.order_count();
        result.depth = book.depth();
        if (instrumented_profile) {
            book.dump_profile(std::cout);
        }
        return result;
    };

    auto warmup = time_once();
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "warmup_eps " << (static_cast<double>(count) / warmup.seconds) << " book " << std::hex
              << warmup.book_hash << std::dec << '\n';

    struct Run {
        double seconds = 0;
        double eps = 0;
        uint64_t book_hash = 0;
        uint64_t trade_hash = 0;
        uint64_t trades = 0;
        uint64_t rejects = 0;
        uint64_t orders = 0;
        lob::Book::Depth depth;
    };
    std::vector<Run> measured;
    measured.reserve(static_cast<std::size_t>(runs));
    for (int i = 0; i < runs; ++i) {
        auto result = time_once();
        if (result.book_hash != warmup.book_hash || result.trade_hash != warmup.trade_hash ||
            result.trades != warmup.trades || result.rejects != warmup.rejects) {
            throw std::runtime_error("benchmark runs diverged; the book is not deterministic");
        }
        Run run;
        run.seconds = result.seconds;
        run.eps = static_cast<double>(count) / result.seconds;
        run.book_hash = result.book_hash;
        run.trade_hash = result.trade_hash;
        run.trades = result.trades;
        run.rejects = result.rejects;
        run.orders = result.orders;
        run.depth = result.depth;
        measured.push_back(run);
        std::cout << "run " << (i + 1) << " seconds=" << run.seconds << " eps=" << run.eps << '\n';
    }

    std::vector<int> order(measured.size());
    for (int i = 0; i < runs; ++i) {
        order[static_cast<std::size_t>(i)] = i;
    }
    std::sort(order.begin(), order.end(), [&](int a, int b) { return measured[a].eps < measured[b].eps; });
    const Run& median = measured[static_cast<std::size_t>(order[order.size() / 2])];
    const Run& best = measured[static_cast<std::size_t>(order.back())];
    const double mean_service_ns = 1e9 / median.eps;

    const uint64_t freq = qpc_freq();
    const uint64_t overhead_ticks = qpc_overhead_ticks();
    const double overhead_ns = static_cast<double>(overhead_ticks) * 1e9 / static_cast<double>(freq);
    std::vector<uint32_t> samples(static_cast<std::size_t>(count));
    {
        lob::Book book;
        for (uint64_t i = 0; i < count; ++i) {
            uint64_t start = qpc_now();
            book.apply(events[static_cast<std::size_t>(i)]);
            uint64_t stop = qpc_now();
            uint64_t delta = stop - start;
            uint64_t net = delta > overhead_ticks ? delta - overhead_ticks : 0;
            double ns = static_cast<double>(net) * 1e9 / static_cast<double>(freq);
            if (ns > 4294967295.0) {
                samples[static_cast<std::size_t>(i)] = 0xffffffffu;
            } else {
                samples[static_cast<std::size_t>(i)] = static_cast<uint32_t>(ns);
            }
        }
        if (book.book_hash() != warmup.book_hash) {
            throw std::runtime_error("latency run diverged from the throughput runs");
        }
    }
    std::vector<uint32_t> sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    auto mean_of = [&]() {
        long double sum = 0;
        for (uint32_t sample : sorted) {
            sum += sample;
        }
        return static_cast<double>(sum / sorted.size());
    };
    uint32_t p50 = percentile_sorted(sorted, 0.50);
    uint32_t p90 = percentile_sorted(sorted, 0.90);
    uint32_t p99 = percentile_sorted(sorted, 0.99);
    uint32_t p999 = percentile_sorted(sorted, 0.999);
    std::cout << "latency_ns min=" << sorted.front() << " p50=" << p50 << " p90=" << p90 << " p99=" << p99
              << " p999=" << p999 << " max=" << sorted.back() << " mean=" << mean_of()
              << " qpc_pair_overhead_ns=" << overhead_ns << '\n';
    std::cout << std::hex;
    std::cout << "RESULT impl=open_address_index events=" << std::dec << count << " seed=" << seed << " runs=" << runs
              << std::fixed << std::setprecision(3) << " median_eps=" << median.eps << " best_eps=" << best.eps
              << " mean_service_ns=" << mean_service_ns << " p50_ns=" << p50 << " p99_ns=" << p99
              << " p999_ns=" << p999 << " trades=" << median.trades << " rejects=" << median.rejects
              << " final_orders=" << median.orders << " bid_levels=" << median.depth.bid_levels
              << " ask_levels=" << median.depth.ask_levels << " bid_orders=" << median.depth.bid_orders
              << " ask_orders=" << median.depth.ask_orders << " bid_qty=" << median.depth.bid_qty
              << " ask_qty=" << median.depth.ask_qty << std::hex << " book=" << median.book_hash
              << " trade_hash=" << median.trade_hash << std::dec << '\n';
    return 0;
}
#endif

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            usage();
            return 2;
        }
        std::string command = argv[1];
        Options options = parse_options(argc, argv);
        if (command == "selftest") {
            if (!options.has("scenarios") || !options.has("expected")) {
                usage();
                return 2;
            }
            return lob::check_spec(options.get("scenarios"), options.get("expected"));
        }
        if (command == "generate") {
            uint64_t seed = options.u64("seed", 0);
            uint64_t count = options.u64("count", 0);
            if (!options.has("seed") || count == 0 || !options.has("output")) {
                usage();
                return 2;
            }
            std::vector<lob::Event> events = lob::generate(seed, count);
            lob::write_binary(options.get("output"), seed, events);
            lob::TypeCounts counts = lob::count_types(events);
            std::cout << "wrote " << options.get("output") << " events=" << events.size() << " seed=" << seed
                      << " adds=" << counts.add << " cancels=" << counts.cancel << " modifies=" << counts.modify
                      << '\n';
            return 0;
        }
        if (command == "replay") {
            if (!options.has("input") || !options.has("output")) {
                usage();
                return 2;
            }
            lob::replay_to_file(options.get("input"), options.get("output"), options.u64("checkpoint", 1000),
                                options.flag("verify"));
            return 0;
        }
        if (command == "bench" || command == "profile") {
#if defined(_WIN32)
            return run_bench(options, command == "profile");
#else
            throw std::runtime_error("bench is implemented for Windows QPC in this tree");
#endif
        }
        usage();
        return 2;
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
