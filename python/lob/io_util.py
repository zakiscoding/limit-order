"""Binary and text event IO.

Binary layout, little-endian:

Header, 32 bytes:
  0  char[8]  magic "LOBREPV1"
  8  uint32   version = 1
  12 uint32   header_size = 32
  16 uint64   event_count
  24 uint64   seed

Event, 32 bytes:
  0  uint8    type   1 add, 2 cancel, 3 modify, 4 reset
  1  uint8    side   0 buy, 1 sell
  2  uint8    tif    0 gtc, 1 ioc
  3  uint8    pad
  4  uint32   quantity
  8  uint64   order id
  16 int64    price in ticks
  24 uint64   timestamp (logical, from the recording)
"""

from __future__ import annotations

import struct
from pathlib import Path

from lob.hashutil import MASK, FNV_OFFSET, mix

ADD, CANCEL, MODIFY, RESET = 1, 2, 3, 4
BUY, SELL = 0, 1
GTC, IOC = 0, 1

HEADER = struct.Struct("<8sIIQQ")
EVENT = struct.Struct("<BBBBIQqQ")
MAGIC = b"LOBREPV1"


class Event:
    __slots__ = ("type", "id", "side", "price", "qty", "tif", "ts")

    def __init__(
        self,
        type: int,
        id: int = 0,
        side: int = 0,
        price: int = 0,
        qty: int = 0,
        tif: int = 0,
        ts: int = 0,
    ) -> None:
        self.type = type
        self.id = id
        self.side = side
        self.price = price
        self.qty = qty
        self.tif = tif
        self.ts = ts

    def fields(self) -> tuple:
        return (self.type, self.id, self.side, self.price, self.qty, self.tif, self.ts)


def parse_side(token: str) -> int:
    t = token.upper()
    if t in ("B", "BUY"):
        return BUY
    if t in ("S", "SELL"):
        return SELL
    raise ValueError(f"bad side {token}")


def parse_tif(token: str) -> int:
    t = token.upper()
    if t == "GTC":
        return GTC
    if t == "IOC":
        return IOC
    raise ValueError(f"bad tif {token}")


def parse_event_line(line: str) -> Event:
    parts = line.split()
    if not parts:
        raise ValueError("empty event")
    op = parts[0].upper()
    if op == "RESET":
        if len(parts) != 1:
            raise ValueError(line)
        return Event(RESET)
    if op == "ADD":
        if len(parts) != 7:
            raise ValueError(line)
        return Event(
            ADD,
            id=int(parts[1]),
            side=parse_side(parts[2]),
            price=int(parts[3]),
            qty=int(parts[4]),
            tif=parse_tif(parts[5]),
            ts=int(parts[6]),
        )
    if op == "CANCEL":
        if len(parts) != 3:
            raise ValueError(line)
        return Event(CANCEL, id=int(parts[1]), ts=int(parts[2]))
    if op == "MODIFY":
        if len(parts) != 5:
            raise ValueError(line)
        return Event(
            MODIFY,
            id=int(parts[1]),
            price=int(parts[2]),
            qty=int(parts[3]),
            ts=int(parts[4]),
        )
    raise ValueError(line)


def read_text(path: str | Path) -> list[Event]:
    events: list[Event] = []
    for lineno, raw in enumerate(Path(path).read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        try:
            events.append(parse_event_line(line))
        except ValueError as exc:
            raise ValueError(f"{path}:{lineno}: {exc}") from exc
    return events


def load_scenario_blocks(path: str | Path) -> list[dict]:
    """Split a scenario file on RESET. RESET itself is not part of a block."""
    blocks: list[dict] = []
    name = None
    events: list[Event] = []

    def flush() -> None:
        nonlocal name, events
        if name is not None or events:
            blocks.append({"name": name, "events": events})
        name = None
        events = []

    for lineno, raw in enumerate(Path(path).read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line:
            continue
        if line.startswith("# name:"):
            name = line.split(":", 1)[1].strip()
            continue
        if line.startswith("#"):
            continue
        if line == "RESET":
            flush()
            continue
        try:
            events.append(parse_event_line(line))
        except ValueError as exc:
            raise ValueError(f"{path}:{lineno}: {exc}") from exc
    flush()
    return blocks


def write_binary(path: str | Path, seed: int, events: list[Event]) -> None:
    with open(path, "wb") as handle:
        handle.write(HEADER.pack(MAGIC, 1, 32, len(events), seed & MASK))
        for event in events:
            handle.write(
                EVENT.pack(
                    event.type & 0xFF,
                    event.side & 0xFF,
                    event.tif & 0xFF,
                    0,
                    event.qty & 0xFFFFFFFF,
                    event.id & MASK,
                    event.price,
                    event.ts & MASK,
                )
            )


def read_binary(path: str | Path) -> tuple[int, list[Event]]:
    data = Path(path).read_bytes()
    if len(data) < 32:
        raise ValueError(f"{path}: file shorter than header")
    magic, version, header_size, count, seed = HEADER.unpack_from(data, 0)
    if magic != MAGIC or version != 1 or header_size != 32:
        raise ValueError(f"{path}: bad header")
    need = 32 + count * 32
    if len(data) != need:
        raise ValueError(f"{path}: size {len(data)} != {need}")
    events: list[Event] = []
    offset = 32
    for _ in range(count):
        typ, side, tif, _pad, qty, oid, price, ts = EVENT.unpack_from(data, offset)
        events.append(Event(typ, id=oid, side=side, price=price, qty=qty, tif=tif, ts=ts))
        offset += 32
    return seed, events


def read_any(path: str | Path) -> tuple[int | None, list[Event]]:
    raw = Path(path).read_bytes()[:8]
    if raw.startswith(MAGIC):
        seed, events = read_binary(path)
        return seed, events
    return None, read_text(path)


def fingerprint_line(index: int, book) -> str:
    bid_px, bid_qty, ask_px, ask_qty = book.top()
    return (
        f"{index} {book.book_hash():016x} {book.trade_hash:016x} "
        f"{book.trade_count} {bid_px} {bid_qty} {ask_px} {ask_qty} "
        f"{book.order_count()} {book.reject_count}"
    )


def replay_lines(events: list[Event], checkpoint: int, verify: bool = False) -> list[str]:
    from lob.book import Book

    if checkpoint < 1:
        raise ValueError("checkpoint must be >= 1")
    book = Book()
    lines: list[str] = []
    last = len(events)
    for index, event in enumerate(events, 1):
        book.apply(event)
        if index % checkpoint == 0 or index == last:
            if verify:
                book.assert_consistent()
            lines.append(fingerprint_line(index, book))
    return lines


def count_types(events: list[Event]) -> dict[str, int]:
    counts = {"add": 0, "cancel": 0, "modify": 0, "reset": 0, "other": 0}
    for event in events:
        if event.type == ADD:
            counts["add"] += 1
        elif event.type == CANCEL:
            counts["cancel"] += 1
        elif event.type == MODIFY:
            counts["modify"] += 1
        elif event.type == RESET:
            counts["reset"] += 1
        else:
            counts["other"] += 1
    return counts


def event_stream_hash(events: list[Event]) -> int:
    h = FNV_OFFSET
    for event in events:
        for field in event.fields():
            h = mix(h, field)
    return h
