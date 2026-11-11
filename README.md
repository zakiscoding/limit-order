# Limit order book and market-data replay

Single-symbol limit order book with deterministic replay. The C++ engine and the Python reference apply the same rules to the same event log and emit the same fingerprints.

## Matching rules

Prices are integer ticks. Quantities are positive integers. The book never reads the wall clock. Time priority is the event timestamp, stored as position in a FIFO at each price.

- Bids match highest price first. Asks match lowest price first. The same price matches oldest order first.
- A trade prints at the resting order's price. A GTC order rests whatever quantity does not trade. An IOC order does not rest.
- Cancel removes a resting order. A missing id is rejected and the book does not change.
- Modify keeps priority only when the price is unchanged and the remaining quantity gets smaller. Any price change, or any increase in quantity, pulls the order out and places it at the tail of its new level. That re-entry can take liquidity. Quantity zero is rejected; cancel is a separate event.
- `RESET` is a replay marker. It clears resting orders and leaves the cumulative trade fingerprint in place.

Rejects cover a duplicate id, an unknown id, a non-positive price or quantity, a bad side, a bad time-in-force, and an unknown event type.

## Replay file

`LOBREPV1`, little-endian. A 32-byte header (`magic`, version 1, header size 32, event count, seed) is followed by 32-byte events:

| offset | field |
| --- | --- |
| 0 | type: 1 add, 2 cancel, 3 modify, 4 reset |
| 1 | side: 0 buy, 1 sell |
| 2 | time in force: 0 GTC, 1 IOC |
| 4 | quantity, uint32 |
| 8 | order id, uint64 |
| 16 | price ticks, int64 |
| 24 | logical timestamp, uint64 |

Text scenarios live in `testdata/scenarios.txt`. The hand-checked trades and book states are in `testdata/expected.txt`. Both engines read that file.

The generator is SplitMix64. For a non-empty live id set the next event is cancel (35%), modify (10%), or add (55%). Adds walk a fair price by one tick inside 8000..12000; 30% are priced through that fair value. The generator does not know about fills, so many cancels and modifies name orders the book has already removed. Both engines must reject those the same way. The C++ generator is a direct port of `python/lob/generate.py`.

## Build and check

### Linux

Requirements: CMake 3.20 or newer, a C++17 compiler (GCC or Clang), Python 3, and optionally Ninja (used when present, otherwise CMake picks Makefiles).

```bash
scripts/test.sh                  # unit tests, build, selftest, Python/C++ replay compare, golden check
scripts/test.sh --bench-smoke    # also runs the 2,000,000-event bench once and checks its hashes
scripts/test.sh --sanitize       # same checks under AddressSanitizer and UBSan
scripts/build.sh                 # build only; prints the binary path (cpp/build/lob_engine)
CXX=clang++ scripts/build.sh     # pick a compiler
```

`testdata/golden.txt` holds the expected end-of-run hashes for seed 42. A Linux GCC build and the original Windows Clang build produce the same book hash for both the 100,000-event replay and the 2,000,000-event bench log, so the golden values are not tied to one platform. Change them only on purpose, because a mismatch means matching behavior changed.

GitHub Actions (`.github/workflows/ci.yml`) runs `scripts/test.sh --bench-smoke` on Ubuntu 22.04 and 24.04 with both GCC and Clang, plus a sanitizer job. The bench is the same code on both platforms. It uses `QueryPerformanceCounter` on Windows and `clock_gettime(CLOCK_MONOTONIC_RAW)` on Linux; pass `--cpu N` to pin to a specific logical CPU.

### Windows

Requirements: Python 3.12, CMake, Ninja, Clang.

```powershell
$env:PYTHONPATH = "python"
python -m unittest discover -s python/tests -v

cmake -S cpp -B cpp/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++
cmake --build cpp/build
.\cpp\build\lob_engine.exe selftest --scenarios testdata\scenarios.txt --expected testdata\expected.txt
```

`scripts/run_all.ps1` runs the unit tests, the byte-level generator check, a 100,000-event fingerprint compare, and the benchmark.

```powershell
.\cpp\build\lob_engine.exe generate --seed 42 --count 100000 --output data\events.bin
python -m lob replay --input data\events.bin --output data\py.fp --checkpoint 1000 --verify
.\cpp\build\lob_engine.exe replay --input data\events.bin --output data\cpp.fp --checkpoint 1000 --verify
python -m lob compare data\py.fp data\cpp.fp

.\cpp\build\lob_engine.exe bench --seed 42 --count 2000000 --runs 5
```

A fingerprint line is the event index, the book hash, the trade hash, trade count, best bid and ask with size, resting order count, and reject count. The book hash walks bids from best to worst, then asks from best to worst, and each order front to back. The mix is a 64-bit xor-then-multiply, the same function in both languages.

Phase timers are a separate build. They are not the throughput numbers.

```powershell
cmake -S cpp -B cpp/build-prof -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++ -DLOB_INSTRUMENT=ON
cmake --build cpp/build-prof
.\cpp\build-prof\lob_engine.exe profile --seed 42 --count 2000000 --runs 1
```

## Latency

`lob_engine latency` times every `Book::apply` call with serialized `rdtsc`/`rdtscp`, subtracts the measured timer overhead, and reports p50, p90, p95, p99, p99.9, p99.99, max, and mean, overall and per event type, for each of N passes and as a median across passes. `scripts/bench.sh` builds, records the machine and build in `environment.txt`, and runs the latency and throughput benchmarks on a fixed seed (Linux only).

```bash
scripts/bench.sh                                          # seed 42, 2,000,000 events, 5 passes
./cpp/build/lob_engine latency --seed 42 --count 2000000 --runs 5 --cpu 2 --json out.json
```

On the Linux virtual machine described in [results/LATENCY.md](results/LATENCY.md), three invocations gave p50 of 47.8 to 53.0 ns, p95 of 131 to 166 ns, and p99 of 197 to 268 ns per event. That machine has no frequency control and shares its core with a hypervisor, so the file also gives the spread, the method, and an explanation of the slowest event.

## Profiling

`scripts/perf_profile.sh` counts allocations (works anywhere on Linux), and, where the machine exposes performance counters, runs `perf stat` for cycles, instructions, branch misses, cache misses, and TLB misses, and `perf record` for per-function attribution of cycles, branch misses, and cache misses.

```bash
scripts/perf_profile.sh --alloc-only     # allocation counts, page faults, RSS; no perf needed
scripts/perf_profile.sh --cpu 2          # everything; needs perf and PMU access
```

The replay loop makes 684,912 `operator new` calls for 2,000,000 events (0.34 per event); 92% of them are `Order` nodes. Hardware-counter results have not been collected yet, because the machine used for development was a virtual machine without PMU access. [results/PERF.md](results/PERF.md) says what was measured, what was not, and how the script works.

## Network

`lob_engine serve` ingests events over TCP or UDP (optionally two redundant UDP lines merged by sequence number) and applies them to the same book as `replay`; `lob_engine send` is the test sender, with seeded loss, duplication and reordering. A gap is a hard failure, not a skip. Wire format, design and limits: [docs/NETWORK.md](docs/NETWORK.md). Loopback latency on the VM used here (UDP, busy-poll, batch 32: p50 5.6 µs): [results/NETWORK.md](results/NETWORK.md).

```
bash scripts/net_test.sh        # loopback tcp, udp, udp A/B with loss; compares with offline replay
bash scripts/net_bench.sh       # wire-to-book latency
```

## Benchmark limits

All Linux numbers in `results/` come from a Hyper-V guest (2 logical CPUs, 4 GB RAM, i7-14700F host), not bare metal.

- Timing: the guest TSC is virtualized and the vCPUs can be descheduled by the hypervisor, so tail latencies (p99.9 and above, and the 1 to 3 ms maxima in the network runs) include host noise. CPU frequency and SMT state are not controllable from the guest.
- Hardware counters: the guest exposes no PMU, so cache-miss and branch-miss counts are not measured. Run `scripts/perf_profile.sh` on bare metal or a VM with PMU passthrough.
- Network: sender and receiver share the same 2 vCPUs over loopback. The numbers exclude the NIC, driver, switch and any real wire jitter, and UDP loopback needs a paced sender to avoid socket-buffer loss.
- Use the ranges across runs, not single values, and re-run on your own hardware before comparing.

## Layout

- `cpp/`: matcher, generator, replay, benchmark, latency, wire format and network ingestion
- `scripts/`: `build.sh`, `test.sh`, `bench.sh` for Linux; `run_all.ps1` for Windows
- `.github/workflows/ci.yml`: Linux CI (GCC and Clang, sanitizers)
- `python/lob/`: reference matcher and the same generator
- `testdata/`: scripted events and the expected trades and book
- `results/RESULTS.md`: hardware, workload, profile, and timings (Windows)
- `results/LATENCY.md`: Linux latency percentiles and method
- `docs/NETWORK.md`, `results/NETWORK.md`: wire format, sequencing design, loopback latency
- `results/PERF.md`: allocation measurements and the perf profiling method

## Performance

On this machine the back-to-back measurement, seed 42 and 2,000,000 events, moved from 12.40 million events/s with `std::unordered_map` to 17.18 million after the id index became an open-addressed table. The book hash and trade hash did not change (`510a7332d2e46339` and `f5122ddf7ab89668`). Mean time per event went from 80.6 ns to 58.2 ns. The full setup, the profile that picked the bottleneck, and the timer limits are in [results/RESULTS.md](results/RESULTS.md).
