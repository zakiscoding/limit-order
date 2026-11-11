#!/usr/bin/env bash
# Wire-to-book latency over loopback. For each configuration, starts `serve`,
# sends COUNT events at RATE events/s, and records the SERVE summary line.
# wire_to_book is measured per packet: the sender stamps CLOCK_MONOTONIC just
# before send(), the receiver reads the same clock after the packet's events
# are applied to the book. One sample per packet, not per event. Both ends run
# on one machine, so the clock is shared; the number includes the kernel
# loopback path, the scheduler, and (batch > 1) the time spent applying the
# earlier events in the same packet.
#
#   scripts/net_bench.sh [--out DIR] [--repeat N] [--cpu-serve N --cpu-send N]
#
# Env: ENGINE (skip the build), COUNT (default 1000000), RATE (default 100000).
set -euo pipefail
cd "$(dirname "$0")/.."
out=""; repeat=3; cs=""; cd_=""
while [ $# -gt 0 ]; do
  case "$1" in
    --out) out="$2"; shift ;;
    --repeat) repeat="$2"; shift ;;
    --cpu-serve) cs="--cpu $2"; shift ;;
    --cpu-send) cd_="--cpu $2"; shift ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
  shift
done
engine="${ENGINE:-$(bash scripts/build.sh | tail -n1)}"
COUNT="${COUNT:-1000000}"; RATE="${RATE:-100000}"
out="${out:-results/runs/net-$(date +%Y%m%d-%H%M%S)-$(hostname)}"
mkdir -p "$out"
bash scripts/capture_env.sh "$out/environment.txt" "$(dirname "$engine")" "net_bench: count=$COUNT rate=$RATE repeat=$repeat serve_cpu=${cs:-unpinned} send_cpu=${cd_:-unpinned}"
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
"$engine" generate --seed 42 --count "$COUNT" --output "$work/e.bin" >/dev/null

: > "$out/net.txt"
run_case() {  # label, proto, batch, extra serve args
  local label="$1" proto="$2" batch="$3"; shift 3
  for i in $(seq "$repeat"); do
    rm -f "$work/ready"
    "$engine" serve --proto "$proto" --port 0 --ready-file "$work/ready" --idle-timeout-ms 5000 $cs "$@" > "$work/s.out" 2>/dev/null &
    pid=$!
    for _ in $(seq 100); do [ -s "$work/ready" ] && break; sleep 0.05; done
    port="$(sed -E 's/.*(udp_a|tcp)=([0-9]+).*/\2/' "$work/ready")"
    "$engine" send --proto "$proto" --target "127.0.0.1:$port" --input "$work/e.bin" --batch "$batch" --rate "$RATE" $cd_ > /dev/null
    wait "$pid" || true
    echo "case=$label run=$i $(grep '^SERVE ' "$work/s.out")" | tee -a "$out/net.txt"
  done
}
run_case udp_batch1  udp 1
run_case udp_batch32 udp 32
run_case udp_batch32_busypoll udp 32 --busy-poll
run_case tcp_batch1  tcp 1
run_case tcp_batch32 tcp 32
echo "wrote $out"
