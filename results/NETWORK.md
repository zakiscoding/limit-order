# Network latency results

Wire-to-book latency over loopback, measured with `scripts/net_bench.sh`. Raw data and the machine description are in `results/linux-vm-2026-11-11/net/`.

## Setup

- Intel i7-14700F exposed as a Hyper-V guest with 2 logical CPUs and 4 GB RAM, Ubuntu 22.04, kernel 6.8, g++ 11.4 `-O3`. See `environment.txt`.
- Sender and receiver run on the same 2 CPUs, unpinned. They compete with each other and with the hypervisor.
- 500,000 events (seed 42), paced at 100,000 events/s, 3 runs per case. Every run finished with `status=ok` and no gaps. `net.txt` records the final book hash of each run; all 15 are identical and equal the offline `replay` hash for the same 500,000 events.

## Definition

The sender stamps CLOCK_MONOTONIC just before `send()`. The receiver reads the clock after the packet's events have been applied. One sample per packet. With batch 32 the sample includes applying up to 32 events, so batch 1 and batch 32 are not comparable per event. Percentiles are nearest-rank. The table gives the range across the 3 runs.

## Results (ns)

| Case | p50 | p95 | p99 | p99.9 | max |
|---|---|---|---|---|---|
| UDP, batch 1, poll | 10,919 to 12,755 | 20,944 to 22,498 | 36,490 to 48,329 | 615,556 to 921,606 | ~2.0M to 2.2M |
| UDP, batch 32, poll | 24,827 to 25,957 | 50,605 to 58,386 | 64,763 to 92,266 | 118,416 to 183,198 | ~1.0M to 1.5M |
| UDP, batch 32, busy-poll | 5,591 to 5,806 | 12,526 to 14,751 | 17,642 to 22,649 | 45,382 to 68,355 | ~0.9M to 1.0M |
| TCP, batch 1, poll | 12,447 to 12,811 | 24,850 to 25,198 | 41,986 to 43,878 | 296,713 to 323,783 | ~1.0M to 3.4M |
| TCP, batch 32, poll | 33,517 to 37,413 | 81,588 to 96,106 | 138,056 to 151,430 | 213,105 to 238,885 | ~1.0M |

## Reading these numbers

- Busy-poll cuts UDP p50 from about 25 µs to about 5.6 µs because the receiver no longer waits for a `poll()` wakeup. The cost is a core at 100%.
- Batch 32 is slower per packet than batch 1 here, as expected: the sample ends after all events in the packet are applied. The engine itself takes about 50 ns per event (see `LATENCY.md`).
- Maxima of 1 to 3 ms appear in every case. This is a shared 2-CPU guest; the cause was not investigated, so no claim is made about it beyond that.
- These are loopback numbers. They exclude the NIC, the switch and any real network jitter, and say nothing about a production feed.
- The Windows build and a bare-metal pinned run were not measured.

## Reproduce

```
bash scripts/net_bench.sh --out results/runs/net-mine --repeat 3
bash scripts/net_bench.sh --cpu-serve 2 --cpu-send 3   # on a machine with spare cores
```
