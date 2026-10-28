"""Command line for generation, replay, and fingerprint comparison."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from lob.book import Book
from lob.generate import generate
from lob.io_util import count_types, read_any, replay_lines, write_binary
from lob.spec import check_spec


def _cmd_generate(args: argparse.Namespace) -> int:
    events = generate(args.seed, args.count)
    write_binary(args.output, args.seed, events)
    counts = count_types(events)
    print(
        f"wrote {args.output} events={len(events)} seed={args.seed} "
        f"adds={counts['add']} cancels={counts['cancel']} modifies={counts['modify']}"
    )
    return 0


def _cmd_replay(args: argparse.Namespace) -> int:
    _seed, events = read_any(args.input)
    lines = replay_lines(events, args.checkpoint, verify=args.verify)
    Path(args.output).write_text("\n".join(lines) + ("\n" if lines else ""), encoding="utf-8")
    book = Book()
    for event in events:
        book.apply(event)
    if args.verify:
        book.assert_consistent()
    counts = count_types(events)
    depth = book.depth()
    print(
        "summary "
        f"events={len(events)} adds={counts['add']} cancels={counts['cancel']} "
        f"modifies={counts['modify']} resets={counts['reset']} "
        f"trades={book.trade_count} rejects={book.reject_count} "
        f"orders={book.order_count()} "
        f"bid_levels={depth['bid_levels']} ask_levels={depth['ask_levels']} "
        f"bid_orders={depth['bid_orders']} ask_orders={depth['ask_orders']} "
        f"bid_qty={depth['bid_qty']} ask_qty={depth['ask_qty']} "
        f"book={book.book_hash():016x} trade_hash={book.trade_hash:016x}"
    )
    print(f"wrote {args.output} lines={len(lines)}")
    return 0


def _cmd_compare(args: argparse.Namespace) -> int:
    left = Path(args.left).read_text(encoding="utf-8").splitlines()
    right = Path(args.right).read_text(encoding="utf-8").splitlines()
    shown = 0
    mismatches = 0
    limit = min(len(left), len(right))
    for index in range(limit):
        if left[index] != right[index]:
            mismatches += 1
            if shown < 5:
                print(f"mismatch line {index + 1}")
                print(f"  left  {left[index]}")
                print(f"  right {right[index]}")
                shown += 1
    if len(left) != len(right):
        print(f"line count {len(left)} vs {len(right)}")
        mismatches += abs(len(left) - len(right))
    if mismatches == 0:
        print(f"OK {len(left)} fingerprints")
        return 0
    print(f"FAIL mismatches={mismatches}")
    return 1


def _cmd_spec(args: argparse.Namespace) -> int:
    check_spec(args.scenarios, args.expected)
    print("python spec OK")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="lob")
    sub = parser.add_subparsers(dest="cmd", required=True)

    gen = sub.add_parser("generate")
    gen.add_argument("--seed", type=int, required=True)
    gen.add_argument("--count", type=int, required=True)
    gen.add_argument("--output", required=True)
    gen.set_defaults(func=_cmd_generate)

    replay = sub.add_parser("replay")
    replay.add_argument("--input", required=True)
    replay.add_argument("--output", required=True)
    replay.add_argument("--checkpoint", type=int, default=1000)
    replay.add_argument("--verify", action="store_true")
    replay.set_defaults(func=_cmd_replay)

    cmp_ = sub.add_parser("compare")
    cmp_.add_argument("left")
    cmp_.add_argument("right")
    cmp_.set_defaults(func=_cmd_compare)

    spec = sub.add_parser("spec")
    spec.add_argument("--scenarios", required=True)
    spec.add_argument("--expected", required=True)
    spec.set_defaults(func=_cmd_spec)

    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
