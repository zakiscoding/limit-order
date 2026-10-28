"""Deterministic event generator.

The C++ generator in cpp/src/engine.cpp is a line-by-line port of `generate`.
Do not reorder SplitMix64 draws here without updating that function.
"""

from lob.hashutil import MASK
from lob.io_util import ADD, BUY, CANCEL, GTC, MODIFY, SELL, Event


def clamp(value: int, lo: int, hi: int) -> int:
    if value < lo:
        return lo
    if value > hi:
        return hi
    return value


class SplitMix64:
    def __init__(self, seed: int) -> None:
        self.state = seed & MASK

    def next(self) -> int:
        self.state = (self.state + 0x9E3779B97F4A7C15) & MASK
        z = self.state
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & MASK
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & MASK
        return (z ^ (z >> 31)) & MASK


def generate(seed: int, count: int) -> list[Event]:
    """Build `count` events from `seed`.

    When the generator still has ids it has not cancelled:
      roll 0..99
        < 35  cancel that id
        < 45  modify that id
        else  add a new limit
    An empty live set always adds. Cancels and modifies name ids the generator
    has added and not yet cancelled. The book may already have filled them, in
    which case the engine rejects the event. That reject is part of the stream.
    """
    rng = SplitMix64(seed)
    fair = 10000
    live: list[int] = []
    meta: dict[int, tuple[int, int]] = {}
    next_id = 1
    events: list[Event] = []

    for i in range(1, count + 1):
        ts = i
        if live:
            roll = rng.next() % 100
            if roll < 35:
                idx = rng.next() % len(live)
                oid = live[idx]
                live[idx] = live[-1]
                live.pop()
                del meta[oid]
                events.append(Event(CANCEL, id=oid, ts=ts))
                continue
            if roll < 45:
                idx = rng.next() % len(live)
                oid = live[idx]
                old_price, old_qty = meta[oid]
                mode = rng.next() % 2
                rprice = rng.next()
                rqty = rng.next()
                if mode == 0 and old_qty > 1:
                    new_price = old_price
                    new_qty = 1 + (rqty % (old_qty - 1))
                else:
                    off = (rprice % 41) - 20
                    new_price = clamp(fair + off, 1, 20000)
                    new_qty = 1 + (rqty % 200)
                meta[oid] = (new_price, new_qty)
                events.append(
                    Event(MODIFY, id=oid, price=new_price, qty=new_qty, ts=ts)
                )
                continue

        step = rng.next() % 3
        fair = clamp(fair + step - 1, 8000, 12000)
        side = BUY if (rng.next() & 1) == 0 else SELL
        aggressive = (rng.next() % 10) < 3
        off = 1 + (rng.next() % 20)
        if side == BUY:
            price = fair + off if aggressive else fair - off
        else:
            price = fair - off if aggressive else fair + off
        price = clamp(price, 1, 20000)
        qty = 1 + (rng.next() % 200)
        oid = next_id
        next_id += 1
        live.append(oid)
        meta[oid] = (price, qty)
        events.append(
            Event(ADD, id=oid, side=side, price=price, qty=qty, tif=GTC, ts=ts)
        )

    return events
