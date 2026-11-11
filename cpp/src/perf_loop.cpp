#include "lob/perf_loop.hpp"

#include "lob/alloc_stats.hpp"
#include "lob/book.hpp"
#include "lob/engine.hpp"
#include "lob/timing.hpp"

#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <vector>

#if !defined(_WIN32)
#include <sys/resource.h>
#endif

namespace lob {
namespace {

struct Usage {
    uint64_t minor_faults = 0;
    uint64_t major_faults = 0;
    uint64_t max_rss_kib = 0;
};

Usage read_usage() {
    Usage u;
#if !defined(_WIN32)
    rusage r;
    if (getrusage(RUSAGE_SELF, &r) == 0) {
        u.minor_faults = static_cast<uint64_t>(r.ru_minflt);
        u.major_faults = static_cast<uint64_t>(r.ru_majflt);
        u.max_rss_kib = static_cast<uint64_t>(r.ru_maxrss);  // KiB on Linux
    }
#endif
    return u;
}

}  // namespace

int run_apply_loop(uint64_t seed, uint64_t count, uint64_t passes) {
    if (count == 0) {
        throw std::runtime_error("count must be positive");
    }
    const std::vector<Event> events = generate(seed, count);

    uint64_t ref_book = 0;
    uint64_t ref_trade = 0;
    uint64_t new_calls = 0;
    uint64_t delete_calls = 0;
    uint64_t bytes_allocated = 0;
    uint64_t bytes_freed = 0;
    uint64_t peak_growth = 0;
    uint64_t live_at_end = 0;

    const Usage before = read_usage();
    const uint64_t start = monotonic_ns();
    for (uint64_t pass = 0; pass < passes; ++pass) {
        Book book;
        reset_alloc_peak();
        const AllocStats a0 = alloc_stats();
        for (const Event& event : events) {
            book.apply(event);
        }
        const AllocStats a1 = alloc_stats();  // before the book is torn down
        new_calls += a1.new_calls - a0.new_calls;
        delete_calls += a1.delete_calls - a0.delete_calls;
        bytes_allocated += a1.bytes_allocated - a0.bytes_allocated;
        bytes_freed += a1.bytes_freed - a0.bytes_freed;
        peak_growth = a1.peak_live_bytes > a0.live_bytes ? a1.peak_live_bytes - a0.live_bytes : 0;
        live_at_end = a1.live_bytes > a0.live_bytes ? a1.live_bytes - a0.live_bytes : 0;
        if (pass == 0) {
            ref_book = book.book_hash();
            ref_trade = book.trade_hash();
        } else if (book.book_hash() != ref_book || book.trade_hash() != ref_trade) {
            throw std::runtime_error("apply-loop passes diverged; the book is not deterministic");
        }
    }
    const double seconds = static_cast<double>(monotonic_ns() - start) / 1e9;
    const Usage after = read_usage();

    std::cout << "APPLY_LOOP seed=" << seed << " events=" << count << " passes=" << passes << std::fixed
              << std::setprecision(6) << " wall_seconds=" << seconds << std::hex << " book=" << ref_book
              << " trade_hash=" << ref_trade << std::dec << " alloc_counting=" << (alloc_stats().enabled ? "on" : "off")
              << " new_calls=" << new_calls << " delete_calls=" << delete_calls
              << " bytes_allocated=" << bytes_allocated << " bytes_freed=" << bytes_freed
              << " peak_live_growth_bytes=" << peak_growth << " live_growth_bytes=" << live_at_end
              << " minor_faults=" << (after.minor_faults - before.minor_faults)
              << " major_faults=" << (after.major_faults - before.major_faults)
              << " max_rss_kib=" << after.max_rss_kib << '\n';
    return 0;
}

}  // namespace lob
