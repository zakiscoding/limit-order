#include "lob/engine.hpp"
#include "lob/latency.hpp"
#include "lob/net.hpp"
#include "lob/perf_loop.hpp"

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
#else
#include <sched.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include <fstream>
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
        << "  lob_engine bench --seed N --count N [--runs N] [--cpu N]\n"
        << "  lob_engine latency-selftest\n"
        << "  lob_engine latency --seed N --count N [--runs N] [--cpu N] [--json FILE]\n"
        << "  lob_engine apply-loop --seed N --count N [--passes N] [--cpu N]\n"
        << "  lob_engine net-selftest\n"
        << "  lob_engine serve --proto udp|tcp [--port N] [--port-b N] [--bind ADDR] [--fingerprints FILE]\n"
        << "                   [--checkpoint N] [--verify] [--ready-file FILE] [--idle-timeout-ms N]\n"
        << "                   [--busy-poll] [--cpu N]\n"
        << "  lob_engine send --proto udp|tcp --target HOST:PORT [--target-b HOST:PORT]\n"
        << "                  [--seed N --count N | --input FILE] [--batch N] [--rate EVENTS_PER_S]\n"
        << "                  [--drop-a P] [--drop-b P] [--dup P] [--reorder K] [--impair-seed N]\n"
        << "  lob_engine profile --seed N --count N [--cpu N]\n";
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

uint64_t tick_now() {
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return static_cast<uint64_t>(counter.QuadPart);
}

uint64_t tick_freq() {
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    return static_cast<uint64_t>(freq.QuadPart);
}

void pin_thread(int cpu) {
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    DWORD_PTR mask = info.dwNumberOfProcessors >= 4 ? (DWORD_PTR{1} << 2) : DWORD_PTR{1};
    if (cpu >= 0) {
        mask = DWORD_PTR{1} << cpu;
    }
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
              << " tick_hz=" << tick_freq() << '\n';
}

#else
std::string cpu_brand() {
    std::ifstream in("/proc/cpuinfo");
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("model name", 0) == 0) {
            std::size_t colon = line.find(':');
            if (colon == std::string::npos) {
                break;
            }
            std::size_t start = line.find_first_not_of(" \t", colon + 1);
            return start == std::string::npos ? "unknown" : line.substr(start);
        }
    }
    return "unknown";
}

uint64_t tick_now() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

uint64_t tick_freq() { return 1000000000ull; }

// Pins to `cpu` when given, otherwise to the third allowed CPU (the second
// logical processor pair on a typical SMT machine), or the first if fewer
// than four are allowed. This mirrors the Windows default.
void pin_thread(int cpu) {
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) {
        std::cerr << "warning: could not read CPU affinity\n";
        return;
    }
    std::vector<int> usable;
    for (int i = 0; i < CPU_SETSIZE; ++i) {
        if (CPU_ISSET(i, &allowed)) {
            usable.push_back(i);
        }
    }
    if (usable.empty()) {
        return;
    }
    int target = cpu >= 0 ? cpu : (usable.size() >= 4 ? usable[2] : usable[0]);
    cpu_set_t one;
    CPU_ZERO(&one);
    CPU_SET(target, &one);
    if (sched_setaffinity(0, sizeof(one), &one) != 0) {
        std::cerr << "warning: could not pin the benchmark thread to cpu " << target << '\n';
        return;
    }
    std::cout << "pinned cpu=" << target << '\n';
}

void print_hardware() {
    utsname uts;
    std::string kernel = uname(&uts) == 0 ? std::string(uts.sysname) + " " + uts.release : "unknown";
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long page_size = sysconf(_SC_PAGESIZE);
    const unsigned long long ram =
        pages > 0 && page_size > 0 ? static_cast<unsigned long long>(pages) * static_cast<unsigned long long>(page_size) : 0;
    std::cout << "hardware cpu=\"" << cpu_brand() << "\""
              << " ram_bytes=" << ram << " logical_processors=" << sysconf(_SC_NPROCESSORS_ONLN)
              << " tick_hz=" << tick_freq() << " os=\"" << kernel << "\"\n";
}
#endif

uint64_t tick_overhead_ticks() {
    constexpr int kSamples = 200000;
    std::vector<uint64_t> deltas(static_cast<std::size_t>(kSamples));
    for (int i = 0; i < kSamples; ++i) {
        uint64_t start = tick_now();
        uint64_t stop = tick_now();
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
    return "gcc " __VERSION__;
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

    pin_thread(options.has("cpu") ? static_cast<int>(options.u64("cpu", 0)) : -1);
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
        uint64_t start = tick_now();
        for (const lob::Event& event : events) {
            book.apply(event);
        }
        uint64_t stop = tick_now();
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
        result.seconds = static_cast<double>(stop - start) / static_cast<double>(tick_freq());
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

    const uint64_t freq = tick_freq();
    const uint64_t overhead_ticks = tick_overhead_ticks();
    const double overhead_ns = static_cast<double>(overhead_ticks) * 1e9 / static_cast<double>(freq);
    std::vector<uint32_t> samples(static_cast<std::size_t>(count));
    {
        lob::Book book;
        for (uint64_t i = 0; i < count; ++i) {
            uint64_t start = tick_now();
            book.apply(events[static_cast<std::size_t>(i)]);
            uint64_t stop = tick_now();
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
    uint32_t p95 = percentile_sorted(sorted, 0.95);
    uint32_t p99 = percentile_sorted(sorted, 0.99);
    uint32_t p999 = percentile_sorted(sorted, 0.999);
    std::cout << "latency_ns min=" << sorted.front() << " p50=" << p50 << " p90=" << p90 << " p95=" << p95 << " p99=" << p99
              << " p999=" << p999 << " max=" << sorted.back() << " mean=" << mean_of()
              << " tick_pair_overhead_ns=" << overhead_ns << '\n';
    std::cout << std::hex;
    std::cout << "RESULT impl=open_address_index events=" << std::dec << count << " seed=" << seed << " runs=" << runs
              << std::fixed << std::setprecision(3) << " median_eps=" << median.eps << " best_eps=" << best.eps
              << " mean_service_ns=" << mean_service_ns << " p50_ns=" << p50 << " p95_ns=" << p95 << " p99_ns=" << p99
              << " p999_ns=" << p999 << " trades=" << median.trades << " rejects=" << median.rejects
              << " final_orders=" << median.orders << " bid_levels=" << median.depth.bid_levels
              << " ask_levels=" << median.depth.ask_levels << " bid_orders=" << median.depth.bid_orders
              << " ask_orders=" << median.depth.ask_orders << " bid_qty=" << median.depth.bid_qty
              << " ask_qty=" << median.depth.ask_qty << std::hex << " book=" << median.book_hash
              << " trade_hash=" << median.trade_hash << std::dec << '\n';
    return 0;
}

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
        if (command == "net-selftest") {
            return lob::net_selftest();
        }
        if (command == "serve") {
            lob::ServeConfig config;
            config.proto = options.get("proto", "udp");
            config.bind_addr = options.get("bind", "127.0.0.1");
            config.port = static_cast<int>(options.u64("port", 0));
            config.port_b = options.has("port-b") ? static_cast<int>(options.u64("port-b", 0)) : -1;
            config.fingerprints_path = options.get("fingerprints");
            config.checkpoint = options.u64("checkpoint", 1000);
            config.verify = options.flag("verify");
            config.ready_file = options.get("ready-file");
            config.idle_timeout_ms = static_cast<int>(options.u64("idle-timeout-ms", 3000));
            config.busy_poll = options.flag("busy-poll");
            if (options.has("cpu")) {
                pin_thread(static_cast<int>(options.u64("cpu", 0)));
            }
            return lob::run_serve(config);
        }
        if (command == "send") {
            lob::SendConfig config;
            config.proto = options.get("proto", "udp");
            config.target_a = options.get("target");
            config.target_b = options.get("target-b");
            config.seed = options.u64("seed", 42);
            config.count = options.u64("count", 100000);
            config.input = options.get("input");
            config.batch = static_cast<int>(options.u64("batch", 32));
            config.rate = options.has("rate") ? std::stod(options.get("rate")) : 0.0;
            config.drop_a = options.has("drop-a") ? std::stod(options.get("drop-a")) : 0.0;
            config.drop_b = options.has("drop-b") ? std::stod(options.get("drop-b")) : 0.0;
            config.dup = options.has("dup") ? std::stod(options.get("dup")) : 0.0;
            config.reorder = static_cast<int>(options.u64("reorder", 1));
            config.impair_seed = options.u64("impair-seed", 1);
            if (options.has("cpu")) {
                pin_thread(static_cast<int>(options.u64("cpu", 0)));
            }
            return lob::run_send(config);
        }
        if (command == "apply-loop") {
            pin_thread(options.has("cpu") ? static_cast<int>(options.u64("cpu", 0)) : -1);
            return lob::run_apply_loop(options.u64("seed", 42), options.u64("count", 2000000),
                                       options.u64("passes", 1));
        }
        if (command == "latency-selftest") {
            return lob::latency_selftest();
        }
        if (command == "latency") {
            lob::LatencyConfig config;
            config.seed = options.u64("seed", 42);
            config.count = options.u64("count", 2000000);
            config.runs = static_cast<int>(options.u64("runs", 5));
            config.json_path = options.get("json");
            pin_thread(options.has("cpu") ? static_cast<int>(options.u64("cpu", 0)) : -1);
            print_hardware();
            std::cout << "compiler " << compiler_id()
#ifdef NDEBUG
                      << " config=Release"
#else
                      << " config=Debug"
#endif
                      << '\n';
            return lob::run_latency(config);
        }
        if (command == "bench" || command == "profile") {
            return run_bench(options, command == "profile");
        }
        usage();
        return 2;
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
