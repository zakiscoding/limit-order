#!/usr/bin/env bash
# A stand-in for `perf`, used only to test scripts/perf_profile.sh without
# hardware counters. Every number it prints is synthetic: a fixed setup value
# plus a fixed amount per applied pass, so the script's subtraction and the
# summary arithmetic can be checked by hand. It is not a measurement.
#
# Per pass: cycles 3,000,000; instructions 6,000,000; branches 1,000,000;
# branch-misses 10,000; cache-references 200,000; cache-misses 20,000;
# L1-dcache-loads 2,000,000; L1-dcache-load-misses 100,000; dTLB-loads 2,000,000;
# dTLB-load-misses 1,000; page-faults 100. Setup (0 passes) is 1,000 of each.
# LLC-loads and LLC-load-misses are reported unsupported.
set -euo pipefail

mode="$1"; shift
events=""; outfile=""; data=""
while [ $# -gt 0 ]; do
    case "$1" in
        -e) events="$2"; shift 2 ;;
        -o) outfile="$2"; data="$2"; shift 2 ;;
        -x,|-F|--call-graph) [ "$1" = "-x," ] && shift || shift 2 ;;
        -i) data="$2"; shift 2 ;;
        --stdio|--no-children|--sort|--percent-limit) [ "$1" = "--sort" -o "$1" = "--percent-limit" ] && shift 2 || shift ;;
        --) shift; break ;;
        *) shift ;;
    esac
done

passes=""
args=("$@")
for i in "${!args[@]}"; do
    [ "${args[$i]}" = "--passes" ] && passes="${args[$((i + 1))]}"
done

per_pass() {
    case "$1" in
        cycles) echo 3000000 ;; instructions) echo 6000000 ;; branches) echo 1000000 ;;
        branch-misses) echo 10000 ;; cache-references) echo 200000 ;; cache-misses) echo 20000 ;;
        L1-dcache-loads) echo 2000000 ;; L1-dcache-load-misses) echo 100000 ;;
        dTLB-loads) echo 2000000 ;; dTLB-load-misses) echo 1000 ;;
        page-faults) echo 100 ;; *) echo 0 ;;
    esac
}

emit() {
    local list="$1" p="${2:-}" spec name
    IFS=',' read -r -a specs <<< "$list"
    for spec in "${specs[@]}"; do
        name="${spec%%:*}"
        case "$name" in
            LLC-loads|LLC-load-misses) echo "<not supported>,,$spec,0,100.00,,"; continue ;;
        esac
        if [ -z "$p" ]; then
            echo "1,,$spec,1000,100.00,,"
        else
            echo "$((1000 + p * $(per_pass "$name"))),,$spec,1000,100.00,,"
        fi
    done
}

case "$mode" in
    stat)
        if [ -z "$outfile" ]; then
            emit "$events" ""   # probe run: `perf stat -x, -e EVENT -- true`
        else
            emit "$events" "$passes" > "$outfile"
        fi
        ;;
    record)
        echo "synthetic perf.data" > "$data"
        ;;
    report)
        echo "# synthetic report (fake_perf.sh)"
        echo "    90.00%  lob::Book::apply"
        ;;
    *) echo "fake_perf: unsupported mode $mode" >&2; exit 2 ;;
esac
