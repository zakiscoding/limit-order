#pragma once

#include <cstdint>
#include <string>

namespace lob {

// Market-data ingestion over UDP or TCP into the matching engine (POSIX only;
// on Windows serve and send report that they are unavailable). See
// docs/NETWORK.md for the design and results/NETWORK.md for measurements.

struct ServeConfig {
    std::string proto = "udp";  // "udp" or "tcp"
    std::string bind_addr = "127.0.0.1";
    int port = 0;               // UDP line A, or the TCP listen port; 0 picks a free port
    int port_b = -1;            // UDP line B; -1 disables it, 0 picks a free port
    std::string fingerprints_path;  // same format as `replay`; empty to skip
    uint64_t checkpoint = 1000;
    bool verify = false;            // check book invariants at every checkpoint
    std::string ready_file;         // written (atomically) once the sockets are bound
    int idle_timeout_ms = 3000;     // give up when nothing valid arrives for this long
    bool busy_poll = false;         // spin on non-blocking recv instead of poll()
};

struct SendConfig {
    std::string proto = "udp";
    std::string target_a;           // host:port (IPv4)
    std::string target_b;           // optional second UDP line
    uint64_t seed = 42;
    uint64_t count = 100000;
    std::string input;              // replay file; overrides seed and count when set
    int batch = 32;                 // events per packet, 1..40
    double rate = 0;                // events per second; 0 sends as fast as possible
    // UDP impairment simulation, deterministic in impair_seed. A packet is never
    // dropped on both lines, so a run with two lines always remains recoverable.
    double drop_a = 0;
    double drop_b = 0;
    double dup = 0;                 // probability of sending an extra copy
    int reorder = 1;                // shuffle packets within windows of this many
    uint64_t impair_seed = 1;
};

// Exit codes of serve: 0 complete, 4 unrecoverable gap, 5 idle timeout,
// 6 protocol error on a TCP stream, 7 TCP connection closed before END.
int run_serve(const ServeConfig& config);
int run_send(const SendConfig& config);

// Wire format, sequencer, and decoder checks that need no sockets. Returns 0 on success.
int net_selftest();

}  // namespace lob
