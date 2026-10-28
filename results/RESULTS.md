# Throughput and latency

Measured on 27 Oct 2026. Both engines were the Release build described below. The book hash and the trade hash were identical before and after the index change, and the same 2,000,000-event log was used for both.

## Hardware and build

| Item | Value |
| --- | --- |
| CPU | Intel Core i7-14700F, 20 cores, 28 logical processors, 33 MB L3 |
| Frequency | Windows reports base clock 2100 MHz. The run was not pinned to a measured turbo frequency. |
| Memory | 34,189,832,192 bytes (31.8 GiB) |
| OS | Windows 11 Home, build 26200 |
| Compiler | Clang 23.1.1, `-O3 -DNDEBUG -flto=thin -std=c++17` |
| Thread | Pinned to logical processor 2, priority highest |
| Clock | `QueryPerformanceCounter` at 10,000,000 Hz (100 ns ticks) |

The timed region is `Book::apply` only. Events are generated before the timer starts. One warmup pass is discarded. The reported throughput is the median of five runs. Each run checks that the book hash, trade hash, trade count, and reject count match the warmup; a mismatch aborts the bench.

This is in-process replay on one thread. It is not a wire-to-wire matching-engine latency.

## Workload

Seed 42, 2,000,000 events, one symbol. While the generator still has ids outstanding, the mix is 55% add, 35% cancel, 10% modify. Thirty percent of adds are priced through the generator's own fair value (a one-tick random walk from 8,000 to 12,000). Quantity is 1..200. Every generated order is GTC. The generator does not track fills, so a later cancel or modify often names an order the book has already removed. Those rejects are part of the log.

| | Count |
| --- | ---: |
| Adds | 1,099,915 |
| Cancels | 700,207 |
| Modifies | 199,878 |
| Trades | 948,128 |
| Rejects | 776,742 |
| Resting orders at the end | 51,095 |
| Bid levels / ask levels | 409 / 450 |
| Bid quantity / ask quantity | 2,512,815 / 2,449,539 |

`operator new` was called 633,079 times and `delete` 581,984 times. The difference is the 51,095 orders still on the book.

## What the profile showed

The instrumented build (`-DLOB_INSTRUMENT=ON`) times each phase with `rdtsc` and subtracts a calibrated empty-scope cost of 26 cycles. Scopes overlap the outer `apply` timer, so a large share of `apply` cycles is probe overhead and is not in the table. The shares below are of the cycles that remained after that subtraction. Figures are from the timed run, not the warmup, on the same 2,000,000-event log.

| Phase | Calls | Baseline adjusted cycles | Share | After the index change |
| --- | ---: | ---: | ---: | ---: |
| Index insert/erase | 1,229,575 | 106,440,272 | 48.9% | not visible (raw 24 cycles/call, under the 26-cycle probe) |
| `operator new` | 633,079 | 27,588,958 | 12.7% | 26,906,821 |
| Id lookup | 2,000,000 | 25,006,922 | 11.5% | 27,272,348 |
| Rest on a price level | 640,335 | 23,363,910 | 10.7% | 24,579,226 |
| `delete` | 581,984 | 16,913,208 | 7.8% | 15,677,744 |
| Match walk | 1,163,580 | 10,270,418 | 4.7% | 9,756,836 |
| New price level | 51,818 | 4,502,618 | 2.1% | 4,151,528 |

Index insert and erase were the large cost: about 113 raw cycles per call, against about 24 raw cycles after the change. Lookup cost did not move (about 39 raw cycles per call either way). Creating a price level was under 1% of `apply`, so the `std::map` of prices was left alone.

The replacement is `IdIndex` in `cpp/include/lob/id_index.hpp`: one array of slots, linear probing, tombstones, and a multiply hash (`id * 0x9E3779B97F4A7C15`). The table rebuilds when live entries plus tombstones exceed three quarters of capacity. Order objects are still individual heap allocations. That is now a similar cost to lookup and to linking an order into a level, and it is spread across several phases rather than one map.

## Throughput

Back-to-back runs, baseline first, then the open-addressed index. Median is the third of the five throughputs.

| Run | Baseline events/s | Open-address events/s |
| --- | ---: | ---: |
| 1 | 12,406,248 | 16,637,676 |
| 2 | 11,975,187 | 17,053,017 |
| 3 | 12,495,533 | 17,175,491 |
| 4 | 12,368,331 | 18,075,371 |
| 5 | 12,403,216 | 17,855,660 |
| Median | 12,403,216 | 17,175,491 |

17,175,491 / 12,403,216 = 1.385, or 38% more events per second. A later repeat of only the optimized binary, same command, had median 17,980,680 (runs 17.98, 18.21, 17.68, 18.22, 17.73 million). The comparison used above is the slower of the two optimized sessions, the one taken immediately after the baseline.

Both sessions finished on the same book:

- book hash `510a7332d2e46339`
- trade hash `f5122ddf7ab89668`

## Latency

Mean service time is `1e9 / median_events_per_second` from the outer timer. That is the number to use. Per-event percentiles come from a separate pass that brackets each `apply` with `QueryPerformanceCounter` and subtracts the median cost of an empty pair. The counter ticks every 100 ns, and the empty-pair median was 0 ticks, so the percentiles are multiples of 100 ns. A p50 of 100 ns means "inside one tick," not a measured 100.000 ns.

| | Baseline | Open-address index |
| --- | ---: | ---: |
| Mean service | 80.6 ns | 58.2 ns |
| p50 | 100 ns | 100 ns |
| p99 | 300 ns | 200 ns |
| p99.9 | 500 ns | 400 ns |
| max observed | 1.19 ms | 1.95 ms |

The max is a single outlier on a general-purpose OS, not a matching-engine tail budget. The confirmation run of the optimized binary measured mean service 55.6 ns, p99 200 ns, p99.9 300 ns.

## Correctness beside the bench

- 21 scripted scenarios in `testdata/` match the hand-written trades, book, and reject counts in both languages.
- 1,000 generated events are byte-identical from the C++ and Python generators (seed 42).
- 100,000 generated events, checkpointed every 1,000, produce 100 identical fingerprint lines. Final book hash `e8b6334c0b7a8d25`, 43,239 trades, 33,973 rejects, 3,311 resting orders.
