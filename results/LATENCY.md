# Linux latency results

Measured on 11 Nov 2026 with `scripts/bench.sh`. The raw output of each invocation is in `results/linux-vm-2026-11-11/run1` to `run3` (`environment.txt`, `latency.txt`, `latency.json`, `throughput.txt`). These are the only latency figures in this repository taken on Linux. The Windows figures in `results/RESULTS.md` came from a different machine and are not comparable.

## Environment

| Item | Value |
| --- | --- |
| CPU as reported to the guest | Intel Core i7-14700F, 1 core with 2 threads visible (2 logical CPUs), L1d 48 KiB, L2 2 MiB, L3 33 MiB |
| Virtualization | Guest under a Microsoft hypervisor (`lscpu`: full virtualization, hypervisor flag set) |
| Memory | 4,009,408 KiB (3.8 GiB) |
| OS | Ubuntu 22.04.5, kernel 6.8.0-138-generic |
| Compiler | GCC 11.4.0, `-O3 -DNDEBUG -std=c++17`, `-flto=auto` |
| Frequency control | None visible: no cpufreq governor, no `intel_pstate` files |
| TSC | 2.112 ticks/ns, identical in all five calibration spins of every invocation |
| Pinning | The benchmark thread is pinned to CPU 0 (the default when fewer than four CPUs are allowed) |
| Source | Commit `82481f1` plus the uncommitted changes in this tree (`git_tree: dirty` in `environment.txt`) |

This is a virtual machine, not an isolated bare-metal core. Another guest or the host can take the CPU between two events, and the SMT sibling shares the core. Tail percentiles here include that noise. Run `scripts/bench.sh` on a quiet bare-metal machine to get numbers that describe the code rather than the VM.

## Method

- Workload: seed 42, 2,000,000 events (1,099,915 adds, 700,207 cancels, 199,878 modifies). The log is generated before any timing starts.
- Timed region: one call to `Book::apply`. It includes node allocation and free, the id index, the price maps, matching, and the trade hash. It excludes event decoding, file I/O, and any network. This is in-process replay on one thread, not wire-to-wire latency.
- Timer: `lfence; rdtsc; lfence` before the call and `rdtscp; lfence` after it. Ticks convert to nanoseconds with a TSC rate calibrated against `steady_clock` (five 100 ms spins; the table above gives the result).
- Timer overhead: the median of 1,000,000 empty begin/end pairs (14.7 to 15.6 ns across the three invocations) is subtracted from every sample, clamped at zero.
- Passes: one warmup pass is discarded. Five timed passes follow, each from an empty book. Every pass must end on the same book hash and trade hash as the warmup, or the run aborts.
- Percentiles: nearest rank over all 2,000,000 samples of a pass. The value reported is the median of the five passes, with the lowest and highest pass in brackets. `lob_engine latency-selftest` checks the percentile code on known arrays and times a 2,000 ns spin (it measured 2,021.8 ns and 2,027.5 ns).
- Three separate invocations of the script are shown because the first-order result moved between them.

## Results

All figures are nanoseconds for one `Book::apply`, median of five passes [lowest..highest pass].

| Invocation | p50 | p95 | p99 | p99.9 | p99.99 |
| --- | ---: | ---: | ---: | ---: | ---: |
| run1 | 53.0 [45.9..59.7] | 165.7 [124.5..207.4] | 268.0 [182.8..334.8] | 456.0 [300.7..577.6] | 5,557 [4,918..8,019] |
| run2 | 47.8 [46.9..49.7] | 131.2 [125.0..137.3] | 197.4 [183.2..207.9] | 324.8 [303.0..332.9] | 4,923 [4,851..5,171] |
| run3 | 52.1 [47.3..55.4] | 156.2 [129.3..173.8] | 257.6 [191.8..280.3] | 433.2 [312.5..495.7] | 5,193 [4,787..5,393] |

By event type, range of the three invocations' medians:

| Event type | Count | p50 | p95 | p99 |
| --- | ---: | ---: | ---: | ---: |
| Add | 1,099,915 | 62.5 to 70.1 | 142.5 to 179.9 | 213.1 to 283.6 |
| Cancel | 700,207 | 31.7 to 33.6 | 98.0 to 139.7 | 159.6 to 248.6 |
| Modify | 199,878 | 33.1 to 35.0 | 114.6 to 150.6 | 188.0 to 262.3 |

Throughput from the same invocations (`bench`, median of five passes): 21.73, 19.72, and 20.96 million events/s, which is a mean service time of 46.0, 50.7, and 47.7 ns. The final book hash `510a7332d2e46339` and trade hash `f5122ddf7ab89668` are the same ones recorded on Windows with Clang for this log.

Reading the numbers:

- The three invocations disagree by about 10% at p50 and about 30% at p99, and passes within one invocation disagree almost as much. Treat any single p99 from this machine as good to roughly one part in three.
- The mean per-event time in the latency passes was 57 to 82 ns, above the 46 to 51 ns mean service time from the throughput passes. The fences in the timer stop the CPU from overlapping one event with the next, so the latency passes measure each event in isolation and the throughput passes do not. Both are real; they answer different questions.
- Cancels and modifies cost about half of an add at p50. Adds allocate a node.

## The tail

- The slowest event was event number 1,486,287 in all 15 timed passes (three invocations, five passes each). Its time was 1.17 to 1.27 ms (median of the five passes' maxima per invocation), against a p99.99 near 5 µs.
- A scratch build of the engine with a counter inside `IdIndex::grow` (not part of this repository) showed that event 1,486,287 is the rebuild that grows the id table from 262,144 to 524,288 slots. In that build the table grew 15 times after construction, at events 16, 46, 130, 325, 718, 1,571, 3,413, 7,316, 16,344, 33,636, 74,929, 161,925, 344,604, 723,937, and 1,486,287.
- At the end of the log the table holds 51,095 live orders in 524,288 slots of 16 bytes (8 MiB), about 9.7% full. `IdIndex::insert` doubles the capacity whenever live entries plus tombstones pass 75%, even when most of those are tombstones. This was not changed, because it is outside the scope of this work and the engine's behavior is meant to stay as it was.
- Between 255 and 700 events per pass took longer than 1 µs. Fifteen growth events cannot account for that many, and the 5 µs p99.99 is about the 200th slowest event in a pass. What causes the rest was not investigated. The hypervisor is a possible source and was not tested.

## Reproducing

```bash
scripts/bench.sh                      # seed 42, 2,000,000 events, 5 passes
scripts/bench.sh --cpu 3 --runs 10    # pick the CPU and the pass count
```

`environment.txt` records the CPU, kernel, compiler, flags, git commit, governor, turbo, and SMT state. The script prints a warning for each of governor, hypervisor, and SMT that is not set up for stable timing, and it changes no system setting. For quieter numbers, use a bare-metal machine, set the `performance` governor, disable turbo or fix its frequency, boot with `isolcpus` for the benchmark CPU, and keep that core's SMT sibling idle.
