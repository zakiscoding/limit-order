"""Load the hand-written scenario spec and compare it to a book."""

from __future__ import annotations

from pathlib import Path

from lob.book import Book
from lob.io_util import load_scenario_blocks


def load_expected(path: str | Path) -> list[dict]:
    blocks: list[dict] = []
    current = _blank()
    for raw in Path(path).read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line:
            continue
        if line.startswith("# name:"):
            current["name"] = line.split(":", 1)[1].strip()
            continue
        if line.startswith("#"):
            continue
        if line == "END":
            blocks.append(current)
            current = _blank()
            continue
        parts = line.split()
        tag = parts[0]
        if tag == "T":
            current["trades"].append(tuple(int(x) for x in parts[1:5]))
        elif tag == "B":
            current["bids"].append(tuple(int(x) for x in parts[1:4]))
        elif tag == "A":
            current["asks"].append(tuple(int(x) for x in parts[1:4]))
        elif tag == "R":
            current["rejects"] = int(parts[1])
        else:
            raise ValueError(line)
    if current["name"] or current["trades"] or current["bids"] or current["asks"] or current["rejects"]:
        raise ValueError("expected file is missing a final END")
    return blocks


def _blank() -> dict:
    return {"name": None, "trades": [], "bids": [], "asks": [], "rejects": 0}


def check_spec(scenarios: str | Path, expected: str | Path) -> None:
    blocks = load_scenario_blocks(scenarios)
    spec = load_expected(expected)
    if len(blocks) != len(spec):
        raise AssertionError(f"{len(blocks)} scenario blocks vs {len(spec)} expected blocks")
    for block, want in zip(blocks, spec):
        if block["name"] != want["name"]:
            raise AssertionError(f"name {block['name']!r} != {want['name']!r}")
        book = Book()
        book.set_record(True)
        for event in block["events"]:
            book.apply(event)
        book.assert_consistent()
        got_trades = [(t, m, p, q) for t, m, p, q, _ts in book.trades]
        bids, asks = book.snapshot()
        if got_trades != want["trades"]:
            raise AssertionError(f"{want['name']} trades {got_trades} != {want['trades']}")
        if bids != want["bids"]:
            raise AssertionError(f"{want['name']} bids {bids} != {want['bids']}")
        if asks != want["asks"]:
            raise AssertionError(f"{want['name']} asks {asks} != {want['asks']}")
        if book.reject_count != want["rejects"]:
            raise AssertionError(
                f"{want['name']} rejects {book.reject_count} != {want['rejects']}"
            )
