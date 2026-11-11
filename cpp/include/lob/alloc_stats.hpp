#pragma once

#include <cstdint>

namespace lob {

// Counters for global operator new and delete. They count only when the binary
// is built with -DLOB_COUNT_ALLOC=ON, which replaces the global operators. In
// any other build enabled is false and every counter reads zero.
struct AllocStats {
    bool enabled = false;
    uint64_t new_calls = 0;
    uint64_t delete_calls = 0;
    uint64_t bytes_allocated = 0;  // usable size of each block handed out
    uint64_t bytes_freed = 0;
    uint64_t live_bytes = 0;
    uint64_t peak_live_bytes = 0;  // highest live_bytes since the last reset_alloc_peak
};

AllocStats alloc_stats();

// Sets the peak to the current live byte count, so a later read gives the
// growth above that baseline.
void reset_alloc_peak();

}  // namespace lob
