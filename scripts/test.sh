#!/usr/bin/env bash
# Build and run every correctness check. Exits non-zero on the first failure.
#
#   scripts/test.sh [--no-build] [--bench-smoke] [--sanitize] [--perf-harness] [--net]
#
# Checks, in order:
#   1. Python unit tests for the reference matcher
#   2. C++ selftest against testdata/expected.txt (21 scripted scenarios), and the
#      latency selftest (percentile math on known arrays, TSC timer on a 2 us spin)
#   3. C++ and Python generators write byte-identical files (seed 42, 1,000 events)
#   4. C++ and Python replay 100,000 events to identical fingerprints
#   5. The final 100k fingerprint matches testdata/golden.txt, so a compiler,
#      OS, or standard-library change that alters matching is caught
#
# --sanitize builds with AddressSanitizer and UBSan (cpp/build-asan) and runs
# the same checks, which catches memory errors and undefined behavior in the
# matcher.
#
# --perf-harness also checks scripts/perf_profile.sh: the allocation counts are
# reproducible and the counting build produces the golden hashes, and the
# stat/record/summary plumbing gives hand-checkable results against a fake perf
# (scripts/tests/fake_perf.sh, synthetic numbers). It does not need real perf.
#
# --net also runs scripts/net_test.sh (serve + send over loopback sockets).
#
# --bench-smoke also runs a short bench and checks its hashes against the
# golden 2,000,000-event values (takes a few seconds).
#
# Environment: CXX, BUILD_TYPE, BUILD_DIR (see build.sh), PYTHON (default python3).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PYTHON="${PYTHON:-python3}"
build=1
smoke=0
perf_harness=0
net=0
build_args=()
for arg in "$@"; do
    case "$arg" in
        --no-build) build=0 ;;
        --bench-smoke) smoke=1 ;;
        --sanitize) build_args+=(--sanitize) ;;
        --perf-harness) perf_harness=1 ;;
        --net) net=1 ;;
        -h|--help) sed -n '2,28p' "$0"; exit 0 ;;
        *) echo "test.sh: unknown argument: $arg" >&2; exit 2 ;;
    esac
done

cd "$ROOT"
export PYTHONPATH="$ROOT/python"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

step() { printf '\n== %s\n' "$*"; }
fail() { echo "FAIL: $*" >&2; exit 1; }

# Value of key=VALUE in a line of key=value tokens.
field() { printf '%s\n' "$2" | tr ' ' '\n' | sed -n "s/^$1=//p" | head -n1; }

step "python unit tests"
"$PYTHON" -m unittest discover -s python/tests

if [ "$build" = 1 ]; then
    step "build"
    engine="$("$ROOT/scripts/build.sh" ${build_args[@]+"${build_args[@]}"} | tail -n1)"
else
    default_dir="$ROOT/cpp/build"
    [ "${#build_args[@]}" -gt 0 ] && default_dir="$ROOT/cpp/build-asan"
    engine="${BUILD_DIR:-$default_dir}/lob_engine"
fi
[ -x "$engine" ] || fail "engine binary not found at $engine"

step "C++ selftest"
"$engine" selftest --scenarios testdata/scenarios.txt --expected testdata/expected.txt

step "latency math and timer selftest"
"$engine" latency-selftest

step "wire format, sequencer and decoder selftest"
"$engine" net-selftest

step "generator byte identity (seed 42, 1000 events)"
"$engine" generate --seed 42 --count 1000 --output "$work/cpp_1000.bin" >/dev/null
"$PYTHON" -m lob generate --seed 42 --count 1000 --output "$work/py_1000.bin" >/dev/null
cmp "$work/cpp_1000.bin" "$work/py_1000.bin" || fail "generator bytes differ"
echo "generator bytes match ($(wc -c < "$work/cpp_1000.bin") bytes)"

step "C++ vs Python replay (seed 42, 100000 events)"
"$engine" generate --seed 42 --count 100000 --output "$work/e.bin" >/dev/null
"$PYTHON" -m lob replay --input "$work/e.bin" --output "$work/py.fp" --checkpoint 1000 --verify >/dev/null
cpp_summary="$("$engine" replay --input "$work/e.bin" --output "$work/cpp.fp" --checkpoint 1000 --verify | grep '^summary ')"
"$PYTHON" -m lob compare "$work/py.fp" "$work/cpp.fp"

step "golden fingerprint (testdata/golden.txt)"
golden="$(grep '^replay ' testdata/golden.txt | head -n1)"
for key in events book trade_hash trades rejects orders; do
    want="$(field "$key" "$golden")"
    got="$(field "$key" "$cpp_summary")"
    [ "$want" = "$got" ] || fail "$key: golden $want, got $got"
done
echo "matches golden: book=$(field book "$golden") trade_hash=$(field trade_hash "$golden")"

if [ "$smoke" = 1 ]; then
    step "bench smoke (seed 42, 2000000 events, 1 run)"
    bench_golden="$(grep '^bench ' testdata/golden.txt | head -n1)"
    result="$("$engine" bench --seed 42 --count 2000000 --runs 1 | grep '^RESULT ')"
    for key in book trade_hash; do
        want="$(field "$key" "$bench_golden")"
        got="$(field "$key" "$result")"
        [ "$want" = "$got" ] || fail "bench $key: golden $want, got $got"
    done
    echo "bench hashes match golden"
fi

if [ "$perf_harness" = 1 ]; then
    export ALLOC_BUILD_DIR="$work/build-alloc" PERF_BUILD_DIR="$work/build-perf"

    step "perf harness: allocation counts are reproducible and hashes match golden"
    bench_golden="$(grep '^bench ' testdata/golden.txt | head -n1)"
    for run in 1 2; do
        scripts/perf_profile.sh --alloc-only --count 2000000 --passes 1 --out "$work/alloc$run" >/dev/null
        line="$(grep '^APPLY_LOOP ' "$work/alloc$run/alloc.txt")"
        eval "alloc_line_$run=\$line"
    done
    for key in new_calls delete_calls bytes_allocated peak_live_growth_bytes; do
        a="$(field "$key" "$alloc_line_1")"
        b="$(field "$key" "$alloc_line_2")"
        [ -n "$a" ] && [ "$a" = "$b" ] || fail "alloc $key differs between runs: '$a' vs '$b'"
    done
    [ "$(field alloc_counting "$alloc_line_1")" = on ] || fail "allocation counting is not on"
    for key in book trade_hash; do
        [ "$(field "$key" "$alloc_line_1")" = "$(field "$key" "$bench_golden")" ] || fail "counting build $key differs from golden"
    done
    echo "new_calls=$(field new_calls "$alloc_line_1") delete_calls=$(field delete_calls "$alloc_line_1") (same on both runs)"

    step "perf harness: plumbing against fake perf"
    PERF="$ROOT/scripts/tests/fake_perf.sh" scripts/perf_profile.sh --count 20000 --passes 2 --repeats 2 \
        --out "$work/fake" >/dev/null
    summary="$work/fake/summary.md"
    for want in "| cycles | 150 [150..150] | 6,000,000 |" \
                "| instructions per cycle | 2.000 [2.000..2.000] |" \
                "| branch miss rate | 1.000% [1.000%..1.000%] |" \
                "| cache miss rate (cache-misses / cache-references) | 10.000% [10.000%..10.000%] |" \
                "Not measured (unsupported or not counted on this machine): LLC-load-misses, LLC-loads."; do
        grep -qF -- "$want" "$summary" || fail "summary is missing: $want"
    done
    ls "$work"/fake/report_cycles.txt "$work"/fake/report_branch-misses.txt "$work"/fake/report_cache-misses.txt >/dev/null
    echo "summary arithmetic and file layout match the fake perf's synthetic values"
fi

if [ "$net" = 1 ]; then
    step "network ingestion over loopback (tcp, udp, udp A/B with loss)"
    ENGINE="$engine" bash scripts/net_test.sh --no-build
fi

printf '\nall checks passed\n'
