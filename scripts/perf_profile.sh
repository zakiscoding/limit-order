#!/usr/bin/env bash
# Profile the replay loop with Linux perf and count its allocations.
#
#   scripts/perf_profile.sh [--count N] [--passes N] [--repeats N] [--cpu N] [--out DIR] [--alloc-only]
#
# What it does:
#   1. Builds a counting-allocator binary and records operator new/delete counts,
#      bytes, peak live bytes, and page faults for one pass (alloc.txt). This step
#      does not need perf.
#   2. Builds a RelWithDebInfo binary (-O3 -g -fno-omit-frame-pointer) for perf.
#   3. Runs `perf stat` for four event groups (core, cache, llc_tlb, software), each
#      at 0 passes (setup only) and at N passes, --repeats times. The setup runs
#      are subtracted so the figures describe the loop, not log generation.
#   4. Runs `perf record` on cycles, branch-misses, and cache-misses and writes a
#      per-symbol report for each, so misses are attributed to functions.
#   5. Writes summary.md from the numbers above (python -m lob.perfstat).
#
# Output goes to results/runs/perf-<UTC time>-<host>/ unless --out is given.
#
# Requirements for steps 3 and 4: the perf tool, a kernel that exposes the CPU's
# performance counters to your user (perf_event_paranoid <= 2 for user-space
# counting; root or lower paranoid for more), and real PMU access. Virtual
# machines often have none. The script probes each event first, measures only
# those the machine supports, and lists the rest as not measured. It never
# invents a value. If perf cannot run at all it still writes alloc.txt and
# environment.txt, says why, and exits with status 3 (0 with --alloc-only).
#
# Environment: PERF (perf binary, default "perf"), CXX, PYTHON (default python3),
# ALLOC_BUILD_DIR and PERF_BUILD_DIR (default cpp/build-alloc and cpp/build-perf).
# BUILD_DIR is ignored here so a caller's build directory is never overwritten.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PERF="${PERF:-perf}"
PYTHON="${PYTHON:-python3}"
count=2000000
passes=5
repeats=5
seed=42
cpu=""
out=""
alloc_only=0
while [ $# -gt 0 ]; do
    case "$1" in
        --count) count="$2"; shift 2 ;;
        --passes) passes="$2"; shift 2 ;;
        --repeats) repeats="$2"; shift 2 ;;
        --seed) seed="$2"; shift 2 ;;
        --cpu) cpu="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        --alloc-only) alloc_only=1; shift ;;
        -h|--help) sed -n '2,34p' "$0"; exit 0 ;;
        *) echo "perf_profile.sh: unknown argument: $1" >&2; exit 2 ;;
    esac
done

[ "$(uname -s)" = Linux ] || { echo "perf_profile.sh: Linux only" >&2; exit 1; }

stamp="$(date -u +%Y%m%dT%H%M%SZ)"
out="${out:-$ROOT/results/runs/perf-$stamp-$(hostname -s)}"
mkdir -p "$out"

cpu_args=()
[ -n "$cpu" ] && cpu_args=(--cpu "$cpu")

echo "== allocation counts"
alloc_engine="$(BUILD_DIR="${ALLOC_BUILD_DIR:-$ROOT/cpp/build-alloc}" "$ROOT/scripts/build.sh" --count-alloc | tail -n1)"
"$alloc_engine" apply-loop --seed "$seed" --count "$count" --passes 1 ${cpu_args[@]+"${cpu_args[@]}"} | tee "$out/alloc.txt"

perf_dir="${PERF_BUILD_DIR:-$ROOT/cpp/build-perf}"
perf_engine="$(BUILD_TYPE=RelWithDebInfo BUILD_DIR="$perf_dir" CMAKE_EXTRA="-DCMAKE_CXX_FLAGS=-fno-omit-frame-pointer" \
    "$ROOT/scripts/build.sh" | tail -n1)"
"$ROOT/scripts/capture_env.sh" "$out/environment.txt" "$perf_dir" \
    "perf_args: seed=$seed count=$count passes=$passes repeats=$repeats cpu=${cpu:-default}"

write_meta() {
    {
        echo "seed=$seed"
        echo "events=$count"
        echo "passes=$passes"
        echo "repeats=$repeats"
        echo "cpu=${cpu:-default}"
        echo "groups=$1"
    } > "$out/meta.txt"
}

summarize() {
    PYTHONPATH="$ROOT/python" "$PYTHON" -m lob.perfstat "$out"
}

# perf_unusable REASON: explain, summarize what exists, and stop.
perf_unusable() {
    echo "perf cannot be used here: $1" >&2
    write_meta ""
    summarize > /dev/null
    echo "wrote allocation results and environment to $out (no perf data)" >&2
    if [ "$alloc_only" = 1 ]; then exit 0; fi
    exit 3
}

if [ "$alloc_only" = 1 ]; then
    write_meta ""
    summarize
    exit 0
fi

command -v "$PERF" >/dev/null 2>&1 || perf_unusable "'$PERF' is not installed (on Ubuntu: apt install linux-tools-generic)"

probe_out="$("$PERF" stat -x, -e task-clock -- true 2>&1 || true)"
if ! printf '%s\n' "$probe_out" | grep -qE '^[0-9.]+,'; then
    paranoid="$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo unknown)"
    perf_unusable "perf stat failed (perf_event_paranoid=$paranoid; lower it with 'sudo sysctl kernel.perf_event_paranoid=1' or run as root). First line: $(printf '%s\n' "$probe_out" | head -n1)"
fi

# Event groups come from the Python module so the two never disagree.
mapfile -t group_lines < <(PYTHONPATH="$ROOT/python" "$PYTHON" -c \
    'from lob.perfstat import EVENT_GROUPS as G
for name, events in G.items():
    print(name, *events)')

event_arg() {  # event name -> perf event spec
    case "$1" in
        task-clock|page-faults|context-switches|cpu-migrations) echo "$1" ;;
        *) echo "$1:u" ;;
    esac
}

supported() {  # does this machine count the event?
    local spec
    spec="$(event_arg "$1")"
    "$PERF" stat -x, -e "$spec" -- true 2>&1 | grep -qE '^[0-9.]+,'
}

groups_ok=""
echo "== perf stat"
for line in "${group_lines[@]}"; do
    read -r -a words <<< "$line"
    group="${words[0]}"
    specs=()
    for event in "${words[@]:1}"; do
        if supported "$event"; then
            specs+=("$(event_arg "$event")")
        else
            echo "  not supported: $event" >&2
        fi
    done
    [ "${#specs[@]}" -gt 0 ] || continue
    list="$(IFS=,; echo "${specs[*]}")"
    groups_ok="$groups_ok $group"
    for p in 0 "$passes"; do
        for r in $(seq 1 "$repeats"); do
            "$PERF" stat -x, -e "$list" -o "$out/stat_${group}_p${p}_r${r}.csv" -- \
                "$perf_engine" apply-loop --seed "$seed" --count "$count" --passes "$p" \
                ${cpu_args[@]+"${cpu_args[@]}"} > /dev/null
        done
    done
    echo "  $group: $list"
done
write_meta "${groups_ok# }"

echo "== perf record"
for event in cycles branch-misses cache-misses; do
    data="$out/perf_$event.data"
    if "$PERF" record -e "$event:u" -F 2999 --call-graph fp -o "$data" -- \
        "$perf_engine" apply-loop --seed "$seed" --count "$count" --passes "$passes" \
        ${cpu_args[@]+"${cpu_args[@]}"} > /dev/null 2> "$out/record_$event.log"; then
        "$PERF" report -i "$data" --stdio --no-children --sort symbol --percent-limit 1 \
            > "$out/report_$event.txt" 2>/dev/null || echo "  report failed for $event" >&2
        rm -f "$data"
        echo "  $event: $out/report_$event.txt"
    else
        echo "  record failed for $event (see $out/record_$event.log)" >&2
        rm -f "$data"
    fi
done

echo "== summary"
summarize
echo
echo "results in $out"
