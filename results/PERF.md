# CPU profiling and allocation behavior

`scripts/perf_profile.sh` measures cache misses, branch misses, and allocation behavior of the replay loop on Linux. This file records what has been measured and what has not.

## Status

| Measurement | Status |
| --- | --- |
| Allocation counts, bytes, and page faults | Measured (below) |
| Cache misses, branch misses, IPC, TLB misses (`perf stat`) | Not measured. No numbers are recorded in this repository yet. |
| Per-function attribution of cycles, branch misses, cache misses (`perf record`) | Not measured. |

The development machine for this work was a virtual machine, and `perf` cannot count hardware events there:

- `kernel.perf_event_paranoid` is 4 and the account is not root.
- `/sys/bus/event_source/devices` lists only `breakpoint`, `kprobe`, `msr`, `power`, `software`, `tracepoint`, and `uprobe`. There is no `cpu` PMU, and the guest CPU flags do not include `arch_perfmon`.
- `perf stat -e cycles` fails with the paranoid-level error.

The script detects this, writes what it can (`alloc.txt`, `environment.txt`, `summary.md`), prints the reason, and exits with status 3. That run is saved in `results/linux-vm-2026-11-11/perf/`. The `perf stat` and `perf record` stages were tested only against `scripts/tests/fake_perf.sh`, a stand-in whose numbers are synthetic, so they check file handling and arithmetic but not the real `perf` output format. Expect to fix small parsing details the first time it runs against a real PMU.

To fill in the missing rows, run the script on bare-metal Linux (a VM only works if the hypervisor exposes the PMU):

```bash
sudo sysctl kernel.perf_event_paranoid=1     # or run the script as root
scripts/perf_profile.sh --cpu 2              # 2,000,000 events, 5 passes, 5 repeats
```

On a hybrid CPU such as the i7-14700F, pin to a P-core (check `lscpu -e`; P-cores are usually CPUs 0 to 15 on this model). The parser sums the `cpu_core` and `cpu_atom` lines and ignores `<not counted>`, but a thread that migrates between core types gives mixed counts.

## What the script measures

1. Allocation. A build with `-DLOB_COUNT_ALLOC=ON` replaces global `operator new` and `delete` with counting versions (glibc only; it uses `malloc_usable_size` to track live bytes). `lob_engine apply-loop` snapshots the counters around the apply calls of one pass, before the book is torn down, so log generation and teardown are excluded. Page faults and peak RSS come from `getrusage`.
2. Counters. `perf stat -x,` runs the same `apply-loop` command at 0 passes (generate the log, apply nothing) and at N passes, 5 times each, for four event groups: `core` (cycles, instructions, branches, branch-misses), `cache` (cache-references, cache-misses, L1-dcache-loads, L1-dcache-load-misses), `llc_tlb` (LLC-loads, LLC-load-misses, dTLB-loads, dTLB-load-misses), and `software` (task-clock, page-faults, context-switches, cpu-migrations). The setup runs are subtracted, and the result is divided by the events applied. Each event is probed first, and unsupported ones are listed as not measured. Hardware events count user space only (`:u`).
3. Attribution. `perf record -F 2999 --call-graph fp` on cycles, branch-misses, and cache-misses, followed by `perf report --stdio --no-children --sort symbol`.
4. Build. Counters and samples come from a RelWithDebInfo build with `-O3 -g -fno-omit-frame-pointer`; this is close to the Release build but not identical, because frame pointers cost a register.

Known limits of the method:

- Four events per group keeps each group inside the counters of one core. Events from different groups come from different runs, so a ratio such as cache miss rate is computed only within a group.
- `cache-references` and `cache-misses` are the kernel's generic events. On Intel they usually mean last-level cache accesses and misses, but the kernel documentation says the meaning varies by CPU.
- The counters include building and tearing down the `Book` on every pass, which the allocation counts exclude.
- Sampled attribution has skid: a miss is charged to an instruction a few cycles after the one that caused it.

## Allocation behavior (measured)

Linux VM described in `results/LATENCY.md`, GCC 11.4.0 with glibc, seed 42, 2,000,000 events, one pass, apply calls only. Raw output: `results/linux-vm-2026-11-11/perf/alloc.txt`. The counts do not depend on timing, and `test.sh --perf-harness` checks that two runs give identical counts and the golden book and trade hashes.

| Measure | Value |
| --- | ---: |
| `operator new` calls | 684,912 (0.34 per event) |
| `operator delete` calls | 632,958 |
| Bytes allocated (usable size) | 55,964,744 |
| Peak live bytes above the start of the loop | 14,531,416 |
| Live bytes above the start at the end of the loop | 11,315,640 |
| Minor page faults during the loop | 2,052 |
| Major page faults | 0 |
| Peak RSS of the process | 93,452 KiB |

The peak RSS includes the 2,000,000-event log (64 MB at 32 bytes per event), so it says little about the book itself.

Where the calls come from. The instrumented build (`-DLOB_INSTRUMENT=ON`, `lob_engine profile`) gives the same phase call counts on Linux as the Windows run in `results/RESULTS.md`:

| Source | New | Delete |
| --- | ---: | ---: |
| `Order` nodes (`order_new`, `order_delete`) | 633,079 | 581,984 |
| Price-level map nodes (`level_insert`, `level_erase`) | 51,818 | 50,959 |
| Id table rebuilds (`IdIndex::grow`, 15 growths) | 15 | 15 |
| Total | 684,912 | 632,958 |

Both totals match the counting build exactly. The 51,954 calls by which `new` exceeds `delete` are the 51,095 orders and 859 price levels (409 bid and 450 ask) still resting at the end.

Of the 11,315,640 live bytes at the end of the loop, 8,388,608 are the id table (524,288 slots of 16 bytes, see the tail analysis in `results/LATENCY.md`). The other 2,927,032 are the orders and price levels. The 2,052 minor faults are close to the 8 MiB of the final id table in 4 KiB pages; that fits first-touch of the table but was not tested separately.

Every order node is its own heap allocation. Adds are about twice as slow as cancels at p50 in `results/LATENCY.md`, and node allocation happens only on the add path, but how much of that gap is `malloc` was not isolated.
