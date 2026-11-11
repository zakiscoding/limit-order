#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lob {

struct LatencyConfig {
    uint64_t seed = 42;
    uint64_t count = 2000000;
    int runs = 5;
    std::string json_path;  // empty: do not write JSON
};

// Per-event latency of Book::apply over a generated log. Prints human-readable
// lines and one LATENCY summary line to stdout. Throws if a run's final book or
// trade hash differs from the first run's (the book must be deterministic).
int run_latency(const LatencyConfig& config);

// Nearest-rank percentile (p in [0, 1]) of an ascending-sorted vector.
double nearest_rank_percentile(const std::vector<double>& ascending, double p);

// Checks the nearest-rank percentile math on known arrays and the TSC timer on a
// known 2 microsecond spin. Returns 0 on success.
int latency_selftest();

}  // namespace lob
