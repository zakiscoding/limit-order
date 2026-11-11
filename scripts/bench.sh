#!/usr/bin/env bash
# Reproducible throughput and latency benchmark. Linux only.
#
#   scripts/bench.sh [--count N] [--runs N] [--seed N] [--cpu N] [--out DIR]
#
# Builds a Release binary, records the machine and build, then runs:
#   lob_engine latency   p50/p95/p99 per-event latency (also per event type)
#   lob_engine bench     throughput (median events/s of N runs) and a single-pass latency check
# Everything lands in one directory (default results/runs/<UTC time>-<host>/):
#   environment.txt  hardware, OS, compiler, flags, git commit, and tuning state
#   latency.txt      human-readable latency output
#   latency.json     per-run latency statistics
#   throughput.txt   bench output
#
# Defaults: seed 42, 2,000,000 events, 5 runs. The same seed always produces the
# same event log and the same final book hash, so two runs differ only in timing.
# The script prints warnings when the machine is not set up for stable numbers
# (frequency governor, turbo, SMT, shared CPU). It does not change system settings.
#
# Environment: CXX, BUILD_TYPE, BUILD_DIR (see build.sh).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
count=2000000
runs=5
seed=42
cpu=""
out=""
while [ $# -gt 0 ]; do
    case "$1" in
        --count) count="$2"; shift 2 ;;
        --runs) runs="$2"; shift 2 ;;
        --seed) seed="$2"; shift 2 ;;
        --cpu) cpu="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        -h|--help) sed -n '2,22p' "$0"; exit 0 ;;
        *) echo "bench.sh: unknown argument: $1" >&2; exit 2 ;;
    esac
done

[ "$(uname -s)" = Linux ] || { echo "bench.sh: Linux only (use the bench command directly elsewhere)" >&2; exit 1; }

stamp="$(date -u +%Y%m%dT%H%M%SZ)"
out="${out:-$ROOT/results/runs/$stamp-$(hostname -s)}"
mkdir -p "$out"

engine="$("$ROOT/scripts/build.sh" | tail -n1)"
build_dir="$(dirname "$engine")"

cpu_args=()
[ -n "$cpu" ] && cpu_args=(--cpu "$cpu")

read_file() { cat "$1" 2>/dev/null || echo "unavailable"; }

"$ROOT/scripts/capture_env.sh" "$out/environment.txt" "$build_dir" \
    "bench_args: seed=$seed count=$count runs=$runs cpu=${cpu:-default}"

governor="$(read_file /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)"
if [ "$governor" = unavailable ]; then
    echo "warning: no cpufreq governor visible (virtual machine or locked-down host); frequency is not controlled" >&2
elif [ "$governor" != performance ]; then
    echo "warning: cpufreq governor is '$governor', not 'performance'; expect run-to-run frequency drift" >&2
fi
grep -qm1 hypervisor /proc/cpuinfo && echo "warning: running under a hypervisor; tail latencies include host scheduling noise" >&2
[ "$(read_file /sys/devices/system/cpu/smt/active)" = 1 ] && echo "warning: SMT is on; a sibling thread sharing the core adds noise" >&2

echo "== latency ($out/latency.txt)"
"$engine" latency --seed "$seed" --count "$count" --runs "$runs" ${cpu_args[@]+"${cpu_args[@]}"} \
    --json "$out/latency.json" | tee "$out/latency.txt"

echo "== throughput ($out/throughput.txt)"
"$engine" bench --seed "$seed" --count "$count" --runs "$runs" ${cpu_args[@]+"${cpu_args[@]}"} | tee "$out/throughput.txt"

echo
echo "results in $out"
