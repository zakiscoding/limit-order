#pragma once

#include <cstdint>

namespace lob {

// A bare replay loop for profiling. It generates the log, then applies it to a
// fresh book `passes` times and prints one APPLY_LOOP line with the book and
// trade hashes, allocation counts (when built with LOB_COUNT_ALLOC), and page
// faults and peak RSS from getrusage. passes may be 0, which measures only the
// setup; `perf stat` runs at 0 and N passes can then be subtracted to isolate
// the loop. Allocation counts cover the apply calls only, not log generation or
// the book's teardown.
int run_apply_loop(uint64_t seed, uint64_t count, uint64_t passes);

}  // namespace lob
