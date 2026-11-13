"""An in-process stand-in for a ``ccxt.pro`` exchange that keeps orders.

No network, no credentials: every method answers from memory. A placement
answers ``open`` (or ``closed`` at once for a market order when
``fill_market_at`` is set), and a test moves an order along with ``fill``,
``end`` and ``push``; the unified order dicts those produce arrive through
``watch_orders`` the way an exchange's private stream would deliver them.
"""
from __future__ import annotations

import asyncio


class FakeOrderExchange:
    def __init__(self, *, fill_market_at=None, bare_cancel_answer=False):
        self.markets = {"BTC/USDT": {"precision": {"price": 0.01}},
                        "ETH/USDT": {"precision": {"price": 0.01}}}
        self.precisionMode = None
        self.fill_market_at = fill_market_at
        self.bare_cancel_answer = bare_cancel_answer
        self.orders = {}
        self.calls = []
        self.orders_watched = 0
        self.fail_cancel = set()
        self._next_id = 100
        self._queues = {}

    def _queue(self, name):
        if name not in self._queues:
            self._queues[name] = asyncio.Queue()
        return self._queues[name]

    # Market data: fed by the test.
    def feed_trades(self, trades):
        self._queue("trades").put_nowait(trades)

    async def load_markets(self):
        return self.markets

    async def watch_trades(self, symbol):
        return await self._queue("trades").get()

    async def watch_order_book(self, symbol, limit=20):
        return await self._queue("book").get()

    async def watch_bids_asks(self, symbols):
        return await self._queue("bbo").get()

    async def watch_orders(self):
        self.orders_watched += 1
        return await self._queue("orders").get()

    async def fetch_balance(self):
        return {}

    async def fetch_positions(self):
        return []

    async def close(self):
        pass

    # Orders.
    def _place(self, sym, type_, side, qty, price):
        oid = str(self._next_id)
        self._next_id += 1
        order = {"id": oid, "symbol": sym, "type": type_, "side": side, "amount": qty,
                 "price": price, "status": "open", "filled": 0.0, "remaining": qty,
                 "average": None, "timestamp": 1_700_000_000_000 + len(self.orders)}
        self.orders[oid] = order
        self.calls.append(("create", oid, sym, type_, side, qty, price))
        if type_ == "market" and self.fill_market_at is not None:
            order.update(status="closed", filled=qty, remaining=0.0,
                         average=self.fill_market_at)
        return dict(order)

    async def create_market_buy_order(self, sym, qty):
        return self._place(sym, "market", "buy", qty, None)

    async def create_market_sell_order(self, sym, qty):
        return self._place(sym, "market", "sell", qty, None)

    async def create_limit_buy_order(self, sym, qty, price):
        return self._place(sym, "limit", "buy", qty, price)

    async def create_limit_sell_order(self, sym, qty, price):
        return self._place(sym, "limit", "sell", qty, price)

    async def create_order(self, sym, type_, side, qty, price=None, params=None):
        return self._place(sym, type_, side, qty, price)

    async def cancel_order(self, oid, sym=None):
        self.calls.append(("cancel", oid, sym))
        if oid in self.fail_cancel:
            raise RuntimeError(f"cancel of {oid} failed")
        self.orders[oid]["status"] = "canceled"
        if self.bare_cancel_answer:
            return {"id": oid}
        return dict(self.orders[oid])

    # Driven by the test.
    def push(self, oid):
        """Deliver the order's present state on the orders stream."""
        self._queue("orders").put_nowait([dict(self.orders[oid])])

    def fill(self, oid, qty, price):
        """Fill `qty` of the order at `price` and deliver the update."""
        order = self.orders[oid]
        cost = (order["average"] or 0.0) * order["filled"] + qty * price
        order["filled"] += qty
        order["remaining"] = order["amount"] - order["filled"]
        order["average"] = cost / order["filled"]
        if order["remaining"] <= 1e-12:
            order["status"] = "closed"
        self.push(oid)

    def end(self, oid, status):
        """Finish the order as "canceled", "expired" or "rejected"."""
        self.orders[oid]["status"] = status
        self.push(oid)
