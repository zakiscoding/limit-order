# Network ingestion

`lob_engine serve` receives events over TCP or UDP and applies them to the same `Book` that `replay` uses. `lob_engine send` is the matching sender, with deterministic loss, duplication and reordering for testing. The goal is that a book built from the wire ends with the same fingerprints as an offline replay of the same events.

## Wire format

All integers are little-endian. A packet is a 24-byte header followed by `count` events.

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `LOBM` |
| 4 | 1 | version (1) |
| 5 | 1 | flags (bit 0 = END) |
| 6 | 2 | event count, at most 40 |
| 8 | 8 | sequence number of the first event |
| 16 | 8 | sender timestamp, ns (CLOCK_MONOTONIC) |

Events are the 32-byte records of the replay file, so a replay file can be sent as is. The largest packet is 1,304 bytes, under a 1,500-byte Ethernet MTU. The decoder requires the exact length and rejects bad magic, version, unknown flags and counts over 40. END carries no events; its `seq` is the total event count.

## Sequencing

Sequence numbers are event indices starting at 0. The receiver applies events strictly in order and never skips one.

- One UDP line: loss is a gap. The sequencer buffers up to 64 packets ahead; when the window is full with the next sequence still missing, `serve` stops with exit 4. There is no retransmit, so a single lossy line cannot recover.
- Two UDP lines (A/B, `--port-b`): the sender puts every packet on both lines. The receiver merges by sequence number and discards the second copy. The test sender never drops the same packet on both lines.
- Duplicates and partial overlaps are trimmed; duplicates are counted.
- END is sent 5 times per UDP line, because losing it would stall the receiver until the idle timeout. The first END wins; a conflicting one is counted.
- TCP uses the same packets as a stream. A closed connection before END exits 7.

Exit codes: 0 ok, 4 gap, 5 idle timeout, 6 protocol error, 7 TCP closed early.

## Receive path

One thread, no queue, no allocation per packet. `recv` fills a packet, the sequencer hands in-order events to a callback, and the callback calls `Book::apply`. `--busy-poll` spins on non-blocking `recv` instead of `poll()`; it uses a full core. `--cpu N` pins the thread.

`serve` writes a fingerprint line at every multiple of `--checkpoint` plus the final event, in the `replay` format, so `python -m lob compare` works on the two files. Its last line is `SERVE ...` with counters, `book=`, `trade_hash=` and wire-to-book latency percentiles.

## Limits

- IPv4 only. No multicast, no authentication, no retransmit or snapshot recovery.
- UDP on loopback drops when the sender outruns the receiver's socket buffer. Unpaced, 100,000 events at batch 32 produced a gap in a trial run, so tests and benchmarks pace the sender with `--rate`.
- Not built on Windows (`serve` and `send` return an error).

## Tests

- `lob_engine net-selftest`: byte layout, rejection paths, in-order and reversed delivery, duplicates, overlap, gaps, END cases, a randomized stress test, a decoder fuzz and bit-flip run, and a check that events through the wire produce the same hashes.
- `scripts/net_test.sh`: loopback TCP, UDP, UDP A/B with 10% loss per line, 5% duplication and reordering within 8 packets, and a single lossy line that must exit 4. Each is compared with the offline replay and the golden hashes. Part of `scripts/test.sh --net` and CI.
