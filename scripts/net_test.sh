#!/usr/bin/env bash
# Network ingestion tests over loopback. Starts `serve`, feeds it with `send`,
# and checks the book against the offline replay of the same events.
#
#   scripts/net_test.sh [--no-build] [--repeat N] [--sanitize]
#
# Cases: tcp, udp (one line), udp A/B with loss + duplication + reordering on
# both lines, and a single udp line with loss, which must fail with exit 4.
# UDP is paced (RATE events/s) because loopback UDP drops when the sender
# outruns the receiver's socket buffer; that is a property of UDP, not a bug.
set -euo pipefail
cd "$(dirname "$0")/.."

build=1; repeat=1; build_args=()
while [ $# -gt 0 ]; do
  case "$1" in
    --no-build) build=0 ;;
    --repeat) repeat="$2"; shift ;;
    --sanitize) build_args+=(--sanitize) ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
  shift
done
PYTHON="${PYTHON:-python3}"
RATE="${RATE:-200000}"
COUNT="${COUNT:-100000}"

if [ "$build" = 1 ]; then
  engine="$(bash scripts/build.sh ${build_args[@]+"${build_args[@]}"} | tail -n1)"
else
  engine="${ENGINE:?--no-build needs ENGINE=/path/to/lob_engine}"
fi
[ -x "$engine" ] || { echo "engine not found: $engine" >&2; exit 2; }

work="$(mktemp -d)"
server_pid=""
cleanup() { [ -n "$server_pid" ] && kill "$server_pid" 2>/dev/null || true; rm -rf "$work"; }
trap cleanup EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }
field() { tr ' ' '\n' <<<"$1" | grep "^$2=" | head -n1 | cut -d= -f2; }

"$engine" generate --seed 42 --count "$COUNT" --output "$work/e.bin" >/dev/null
"$engine" replay --input "$work/e.bin" --output "$work/ref.fp" --checkpoint 1000 --verify >/dev/null
golden="$(grep '^replay ' testdata/golden.txt | head -n1)"
[ "$COUNT" = 100000 ] || golden=""

start_server() {  # name, serve args...
  local name="$1"; shift
  rm -f "$work/ready"
  "$engine" serve --ready-file "$work/ready" --fingerprints "$work/$name.fp" \
      --checkpoint 1000 --verify --idle-timeout-ms 4000 "$@" >"$work/$name.out" 2>"$work/$name.err" &
  server_pid=$!
  for _ in $(seq 100); do [ -s "$work/ready" ] && return 0; sleep 0.05; done
  fail "$name: server did not become ready"
}

finish_server() {  # name, expected exit code
  local rc=0
  wait "$server_pid" || rc=$?
  server_pid=""
  [ "$rc" = "$2" ] || { cat "$work/$1.err" >&2; fail "$1: serve exit $rc, expected $2"; }
}

check_match() {  # name
  local out; out="$(grep '^SERVE ' "$work/$1.out")"
  "$PYTHON" -m lob compare "$work/ref.fp" "$work/$1.fp" >/dev/null || fail "$1: fingerprints differ from offline replay"
  if [ -n "$golden" ]; then
    for k in book trade_hash trades rejects orders; do
      want="$(field "$golden" "$k")"; got="$(field "$out" "$k")"
      [ "$want" = "$got" ] || fail "$1: $k=$got, golden $want"
    done
  fi
  echo "  $1: ok ($(field "$out" events) events, dup=$(field "$out" duplicates) reordered=$(field "$out" reordered) p50=$(field "$out" wire_to_book_ns_p50)ns)"
}

for run in $(seq "$repeat"); do
  echo "run $run/$repeat"

  start_server tcp --proto tcp --port 0
  port="$(sed 's/tcp=//' "$work/ready")"
  "$engine" send --proto tcp --target "127.0.0.1:$port" --input "$work/e.bin" --batch 32 >/dev/null
  finish_server tcp 0; check_match tcp

  start_server udp --proto udp --port 0
  port="$(sed 's/udp_a=//' "$work/ready")"
  "$engine" send --proto udp --target "127.0.0.1:$port" --input "$work/e.bin" --batch 32 --rate "$RATE" >/dev/null
  finish_server udp 0; check_match udp

  start_server ab --proto udp --port 0 --port-b 0
  pa="$(sed -E 's/.*udp_a=([0-9]+).*/\1/' "$work/ready")"; pb="$(sed -E 's/.*udp_b=([0-9]+).*/\1/' "$work/ready")"
  "$engine" send --proto udp --target "127.0.0.1:$pa" --target-b "127.0.0.1:$pb" --input "$work/e.bin" \
      --batch 16 --rate "$RATE" --drop-a 0.10 --drop-b 0.10 --dup 0.05 --reorder 8 --impair-seed "$run" >/dev/null
  finish_server ab 0; check_match ab
  [ "$(field "$(grep '^SERVE ' "$work/ab.out")" duplicates)" -gt 0 ] || fail "ab: impairments produced no duplicates"

  # One line, 10% loss: the receiver must report a gap and exit 4, not skip ahead.
  start_server gap --proto udp --port 0
  port="$(sed 's/udp_a=//' "$work/ready")"
  "$engine" send --proto udp --target "127.0.0.1:$port" --input "$work/e.bin" --batch 32 --rate "$RATE" \
      --drop-a 0.10 --impair-seed "$run" >/dev/null
  finish_server gap 4
  grep -q 'status=gap' "$work/gap.out" || fail "gap: no gap status"
  echo "  gap: ok (exit 4 as expected)"
done
echo "net tests passed"
