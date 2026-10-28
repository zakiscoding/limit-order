"""Reference limit order book.

Price-time priority, single symbol, single thread.

Bids are ordered by price descending, then by time ascending.
Asks are ordered by price ascending, then by time ascending.
Time priority is the intrusive FIFO at each price, not a sort at match time.

Trades print at the resting order's price. An incoming limit walks the
opposite side while it crosses. A GTC remainder rests. An IOC remainder
is discarded.

Modify:
  same price and same remaining quantity: no-op, priority unchanged
  same price and smaller quantity: size down in place, priority unchanged
  any price change, or a quantity increase: detach and re-enter at the tail
  with the modify timestamp. The re-entered order may take liquidity.

A quantity of zero is rejected. Cancel is a separate event.
Order ids may be reused after the id has left the book.
"""

from __future__ import annotations

from lob.hashutil import FNV_OFFSET, mix
from lob.io_util import ADD, BUY, CANCEL, GTC, IOC, MODIFY, RESET, SELL, Event


class Order:
    __slots__ = ("id", "side", "price", "qty", "ts", "prev", "next", "level")

    def __init__(self, oid: int, side: int, price: int, qty: int, ts: int) -> None:
        self.id = oid
        self.side = side
        self.price = price
        self.qty = qty
        self.ts = ts
        self.prev = None
        self.next = None
        self.level = None


class Level:
    __slots__ = ("price", "total_qty", "head", "tail")

    def __init__(self, price: int) -> None:
        self.price = price
        self.total_qty = 0
        self.head = None
        self.tail = None


class Book:
    def __init__(self) -> None:
        self.bids: dict[int, Level] = {}
        self.asks: dict[int, Level] = {}
        self.orders: dict[int, Order] = {}
        self.best_bid: int | None = None
        self.best_ask: int | None = None
        self.trade_hash = FNV_OFFSET
        self.trade_count = 0
        self.reject_count = 0
        self.trades: list[tuple[int, int, int, int, int]] | None = None

    def set_record(self, on: bool) -> None:
        self.trades = [] if on else None

    def order_count(self) -> int:
        return len(self.orders)

    def apply(self, event: Event) -> None:
        if event.type == ADD:
            self._add(event)
        elif event.type == CANCEL:
            self._cancel(event)
        elif event.type == MODIFY:
            self._modify(event)
        elif event.type == RESET:
            self.clear_book()
        else:
            self._reject()

    def clear_book(self) -> None:
        self.bids.clear()
        self.asks.clear()
        self.orders.clear()
        self.best_bid = None
        self.best_ask = None

    def top(self) -> tuple[int, int, int, int]:
        if self.best_bid is None:
            bid_px, bid_qty = 0, 0
        else:
            bid_px = self.best_bid
            bid_qty = self.bids[bid_px].total_qty
        if self.best_ask is None:
            ask_px, ask_qty = 0, 0
        else:
            ask_px = self.best_ask
            ask_qty = self.asks[ask_px].total_qty
        return bid_px, bid_qty, ask_px, ask_qty

    def snapshot(self) -> tuple[list[tuple[int, int, int]], list[tuple[int, int, int]]]:
        bids: list[tuple[int, int, int]] = []
        asks: list[tuple[int, int, int]] = []
        for px in sorted(self.bids, reverse=True):
            order = self.bids[px].head
            while order is not None:
                bids.append((order.id, order.price, order.qty))
                order = order.next
        for px in sorted(self.asks):
            order = self.asks[px].head
            while order is not None:
                asks.append((order.id, order.price, order.qty))
                order = order.next
        return bids, asks

    def book_hash(self) -> int:
        h = FNV_OFFSET
        for _px in sorted(self.bids, reverse=True):
            h = self._mix_level(h, self.bids[_px])
        for _px in sorted(self.asks):
            h = self._mix_level(h, self.asks[_px])
        return h

    def depth(self) -> dict[str, int]:
        def qty_of(levels: dict[int, Level]) -> int:
            return sum(level.total_qty for level in levels.values())

        def orders_of(levels: dict[int, Level]) -> int:
            n = 0
            for level in levels.values():
                order = level.head
                while order is not None:
                    n += 1
                    order = order.next
            return n

        return {
            "bid_levels": len(self.bids),
            "ask_levels": len(self.asks),
            "bid_orders": orders_of(self.bids),
            "ask_orders": orders_of(self.asks),
            "bid_qty": qty_of(self.bids),
            "ask_qty": qty_of(self.asks),
        }

    def assert_consistent(self) -> None:
        seen: set[int] = set()

        def walk(book: dict[int, Level], side: int, best: int | None, pick) -> None:
            if best is None:
                if book:
                    raise AssertionError("best is empty while levels remain")
            else:
                if best not in book or best != pick(book):
                    raise AssertionError(f"best price {best} out of date")
            for px, level in book.items():
                if level.price != px or level.head is None or level.tail is None:
                    raise AssertionError(f"bad level {px}")
                qty = 0
                prev = None
                order = level.head
                while order is not None:
                    if (
                        order.prev is not prev
                        or order.level is not level
                        or order.side != side
                        or order.price != px
                        or order.qty <= 0
                        or self.orders.get(order.id) is not order
                        or order.id in seen
                    ):
                        raise AssertionError(f"bad order {order.id}")
                    seen.add(order.id)
                    qty += order.qty
                    prev = order
                    order = order.next
                if level.tail is not prev or qty != level.total_qty:
                    raise AssertionError(f"level {px} qty {qty} != {level.total_qty}")

        walk(self.bids, BUY, self.best_bid, max)
        walk(self.asks, SELL, self.best_ask, min)
        if seen != set(self.orders):
            raise AssertionError("order index does not match the ladders")

    def _reject(self) -> None:
        self.reject_count += 1

    def _emit(self, ts: int, taker: int, maker: int, price: int, qty: int) -> None:
        self.trade_count += 1
        h = mix(self.trade_hash, ts)
        h = mix(h, taker)
        h = mix(h, maker)
        h = mix(h, price)
        h = mix(h, qty)
        self.trade_hash = h
        if self.trades is not None:
            self.trades.append((taker, maker, price, qty, ts))

    def _mix_level(self, h: int, level: Level) -> int:
        order = level.head
        while order is not None:
            h = mix(h, order.id)
            h = mix(h, order.price)
            h = mix(h, order.qty)
            h = mix(h, order.side)
            h = mix(h, order.ts)
            order = order.next
        return h

    def _unlink(self, order: Order) -> Level:
        level = order.level
        if order.prev is not None:
            order.prev.next = order.next
        else:
            level.head = order.next
        if order.next is not None:
            order.next.prev = order.prev
        else:
            level.tail = order.prev
        order.prev = None
        order.next = None
        order.level = None
        return level

    def _discard_level_if_empty(self, level: Level, side: int) -> None:
        if level.head is not None:
            return
        if side == BUY:
            del self.bids[level.price]
            if self.best_bid == level.price:
                self.best_bid = max(self.bids) if self.bids else None
        else:
            del self.asks[level.price]
            if self.best_ask == level.price:
                self.best_ask = min(self.asks) if self.asks else None

    def _rest(self, order: Order) -> None:
        book = self.bids if order.side == BUY else self.asks
        level = book.get(order.price)
        if level is None:
            level = Level(order.price)
            book[order.price] = level
            if order.side == BUY:
                if self.best_bid is None or order.price > self.best_bid:
                    self.best_bid = order.price
            elif self.best_ask is None or order.price < self.best_ask:
                self.best_ask = order.price
        order.prev = level.tail
        order.next = None
        if level.tail is not None:
            level.tail.next = order
        else:
            level.head = order
        level.tail = order
        order.level = level
        level.total_qty += order.qty
        self.orders[order.id] = order

    def _match_level(self, taker: Order, level: Level, ts: int) -> None:
        maker = level.head
        while taker.qty > 0 and maker is not None:
            nxt = maker.next
            fill = taker.qty if taker.qty < maker.qty else maker.qty
            self._emit(ts, taker.id, maker.id, maker.price, fill)
            taker.qty -= fill
            maker.qty -= fill
            level.total_qty -= fill
            if maker.qty == 0:
                self._unlink(maker)
                del self.orders[maker.id]
            maker = nxt

    def _match(self, taker: Order, ts: int) -> None:
        if taker.side == BUY:
            while (
                taker.qty > 0
                and self.best_ask is not None
                and self.best_ask <= taker.price
            ):
                level = self.asks[self.best_ask]
                self._match_level(taker, level, ts)
                if level.head is None:
                    self._discard_level_if_empty(level, SELL)
        else:
            while (
                taker.qty > 0
                and self.best_bid is not None
                and self.best_bid >= taker.price
            ):
                level = self.bids[self.best_bid]
                self._match_level(taker, level, ts)
                if level.head is None:
                    self._discard_level_if_empty(level, BUY)

    def _add(self, event: Event) -> None:
        if event.qty <= 0 or event.price <= 0:
            self._reject()
            return
        if event.side not in (BUY, SELL) or event.tif not in (GTC, IOC):
            self._reject()
            return
        if event.id in self.orders:
            self._reject()
            return
        order = Order(event.id, event.side, event.price, event.qty, event.ts)
        self._match(order, event.ts)
        if order.qty > 0 and event.tif == GTC:
            self._rest(order)

    def _cancel(self, event: Event) -> None:
        order = self.orders.get(event.id)
        if order is None:
            self._reject()
            return
        level = order.level
        side = order.side
        level.total_qty -= order.qty
        self._unlink(order)
        del self.orders[order.id]
        self._discard_level_if_empty(level, side)

    def _modify(self, event: Event) -> None:
        order = self.orders.get(event.id)
        if order is None or event.qty <= 0 or event.price <= 0:
            self._reject()
            return
        if event.price == order.price and event.qty == order.qty:
            return
        if event.price == order.price and event.qty < order.qty:
            order.level.total_qty -= order.qty - event.qty
            order.qty = event.qty
            return
        level = order.level
        side = order.side
        level.total_qty -= order.qty
        self._unlink(order)
        del self.orders[order.id]
        self._discard_level_if_empty(level, side)
        order.price = event.price
        order.qty = event.qty
        order.ts = event.ts
        self._match(order, event.ts)
        if order.qty > 0:
            self._rest(order)
