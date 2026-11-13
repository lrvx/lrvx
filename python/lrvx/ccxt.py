"""CCXT live broker for lrvx.

``CcxtBroker`` wraps a ``ccxt.pro`` exchange. Market data flows in
(trades, L2 books, order updates), orders flow out
(``self.market_buy(...)``, ``self.limit_sell(...)``,
``self.stop_market(...)`` from inside the strategy translate into
``create_*_order`` calls on the exchange).

Quick start
-----------

.. code-block:: python

    import asyncio
    import lrvx
    from lrvx.ccxt import CcxtBroker

    class SMA(lrvx.Strategy):
        def __init__(self, symbols):
            super().__init__(symbols)
            self.fast = lrvx.SMA(10)
            self.slow = lrvx.SMA(30)

        def on_trade(self, ctx, trade):
            f = self.fast.update(trade.price)
            s = self.slow.update(trade.price)
            if f is None or s is None:
                return
            if f > s and ctx.is_flat():
                self.market_buy(0.001)
            elif f < s and ctx.is_long():
                self.market_sell(0.001)

    async def main():
        async with CcxtBroker("binance") as broker:
            btc = await broker.add_symbol("BTC/USDT")
            broker.add_strategy(SMA([btc]))
            await broker.run(streams=("trades", "book"))

    asyncio.run(main())

What the broker does
--------------------

- ``add_symbol(ccxt_sym)`` calls ``exchange.load_markets()`` once and
  reads ``markets[sym]["precision"]["price"]`` to register the symbol
  in an internal ``lrvx.SymbolRegistry`` with the right tick size.
- ``add_strategy(strategy)`` attaches the strategy to an internal
  ``lrvx.Runner``. The runner's signal callback is wired to
  ``_handle_signal``; emitting a signal from inside the strategy ends
  up in a ``create_*_order`` call on the exchange.
- ``run(streams=...)`` spawns one asyncio task per (symbol × stream)
  pair. ``"trades"`` and ``"book"`` forward into ``runner.on_trade`` /
  ``runner.on_book_snapshot``; ``"bbo"`` forwards ``watch_bids_asks``
  into ``runner.on_book_top``. ``"orders"`` watches the account's order
  updates; the broker starts it by itself before it places its first
  order, so it need not be named.
- Order updates and fills reach the strategy through the callbacks a
  backtest and paper mode use: ``on_fill(ctx, event)`` for every fill
  (``event.status`` ``"PARTIALLY_FILLED"`` or ``"FILLED"``, and only
  ``on_fill``), ``on_order_update(ctx, event)`` for ``"ACCEPTED"``,
  ``"CANCELED"``, ``"EXPIRED"`` and ``"REJECTED"``. ``event`` is a
  ``lrvx.OrderEventData``; ``event.order_id`` is the id the emit
  returned, and ``ctx.position`` inside ``on_fill`` counts the fill. See
  "Order updates" below.
- Before streams start, the broker calls ``fetch_balance()`` and
  ``fetch_positions()``. For each open position on a registered symbol
  it seeds the runner (``Runner.seed_position``), so the strategy reads
  the position from ``ctx.position`` and ``self.position()`` and later
  fills add to it, and it dispatches
  ``strategy.on_initial_position(ccxt_sym, qty, avg_price)`` as before.
  The seed is signed (a short is negative; ``qty`` in the callback is
  what the exchange reports). A runner keeps a position per strategy, so
  the seed goes to the one attached strategy that trades the symbol;
  with several, none is seeded and ``on_error`` says so: call
  ``broker.runner.seed_position`` for the one that holds it.
- Stream errors are reported via ``on_error`` and retried with
  exponential backoff (configurable via ``retry_initial_delay``,
  ``retry_max_delay``, ``retry_multiplier``). Delay resets to
  ``retry_initial_delay`` after each successful yield.

Order types
-----------

Mapped to ccxt's unified ``create_order`` API:

  ``MARKET``                  → ``create_market_<side>_order``
  ``LIMIT``                   → ``create_limit_<side>_order``
  ``STOP_MARKET``             → ``create_order(type='stop_market', params={'stopPrice': trigger})``
  ``STOP_LIMIT``              → ``create_order(type='stop_limit',  params={'stopPrice': trigger}, price=limit)``
  ``TAKE_PROFIT_MARKET``      → ``create_order(type='take_profit_market', params={'stopPrice': trigger})``
  ``TAKE_PROFIT_LIMIT``       → ``create_order(type='take_profit_limit',  params={'stopPrice': trigger}, price=limit)``
  ``TRAILING_STOP``           → ``create_order(type='trailing_stop_market', params={'trailingDelta': offset})``
  ``TRAILING_STOP_PERCENT``   → ``create_order(type='trailing_stop_market', params={'callbackRate': bps/100})``
  ``CLOSE_POSITION``          → ``create_order(type='market', params={'reduceOnly': True})``
  ``CANCEL``                  → ``cancel_order(ccxt_id, ccxt_sym)``
  ``CANCEL_ALL``              → ``cancel_order`` for every tracked id on this symbol
  ``MODIFY``                  → ``edit_order(ccxt_id, ccxt_sym, type, side, new_qty, new_price)``

A ``CANCEL`` or ``MODIFY`` signal names an order id and nothing else, so
the symbol, side and type of the call are the ones the order was placed
with. The new price of a modify is the limit price, or the trigger of a
stop-market, take-profit-market or trailing order, passed as
``params={'stopPrice': new_price}``.

Stop, take-profit, and trailing param shapes differ across
exchanges (Binance futures: ``triggerPrice``; OKX: ``triggerPx``;
Bybit: ``triggerBy``). The mappings above are ccxt's unified-API
defaults. To customise per exchange, subclass and override the
relevant ``_place_*`` method.

Order types outside the table dispatch to ``on_error`` as
``UnsupportedOrderType``. The exchange call is skipped.

Order updates
-------------

Every order the exchange reports -- in the answer to a placement or a
cancel, and on the orders stream -- is turned into the events a backtest
produces and handed to ``Runner.on_order_event``:

  ccxt ``status`` ``open``, first time seen      → ``ACCEPTED``  → ``on_order_update``
  ``filled`` grew, order still working           → ``PARTIALLY_FILLED`` → ``on_fill``
  ``filled`` grew, ``closed`` or nothing remains → ``FILLED``    → ``on_fill``
  ``canceled`` / ``expired`` / ``rejected``      → ``CANCELED`` / ``EXPIRED`` / ``REJECTED`` → ``on_order_update``

ccxt reports an order's running totals. ``event.fill_qty`` is the
quantity of this fill alone (the growth of ``filled`` since the last
report of the order) and ``event.fill_price`` its price, worked back
from the change in ``average``. An update that changes nothing produces
no event, so the answer to a placement and the stream may both report an
order. The strategy that emitted the order receives its events; an order
the broker did not place (it carries ``order_id`` 0) goes to every
strategy subscribed to its symbol.

The orders stream starts before the first order is placed, whatever
``run(streams=...)`` named: without it a resting order was never
reported filled or closed. A broker that only watches market data never
opens it, so it needs no credentials.

Compatibility: a strategy whose ``on_order_update`` takes four arguments,
``(ccxt_sym, status, filled, avg_price)``, the form this broker used to
call, keeps receiving exactly that call for every update on the orders
stream, with a ``DeprecationWarning`` when it is attached. Its
``on_order_update`` is not called with ``(ctx, event)``; its ``on_fill``
is called like any other strategy's.
"""

from __future__ import annotations

import asyncio
import inspect
import logging
import math
import time
import warnings
from collections import OrderedDict
from dataclasses import dataclass, field
from typing import Any, Callable, Iterable, Mapping, Optional

logger = logging.getLogger(__name__)

# ccxt's three `exchange.precisionMode` values. Mirrored here as plain
# ints (rather than imported) so this module keeps working without
# ccxt installed -- `ccxt.pro` itself is only imported lazily, inside
# `_ensure_exchange`, for exactly that reason. Stable across every
# ccxt language port; see ccxt's `Exchange` base class.
_DECIMAL_PLACES = 2
_SIGNIFICANT_DIGITS = 3
_TICK_SIZE = 4


# ── Helpers ────────────────────────────────────────────────────────────


def _ccxt_ts_ns(t: Any) -> int:
    """Convert a ccxt object's millisecond timestamp to nanoseconds.

    Some exchanges emit updates without a ``timestamp`` field; fall
    back to wall clock so downstream code never sees a zero ts.
    """
    ts_ms = t.get("timestamp") if isinstance(t, dict) else None
    if ts_ms:
        return int(ts_ms) * 1_000_000
    return time.time_ns()


def _trade_is_buy(t: Mapping[str, Any]) -> bool:
    return (t.get("side") or "").lower() == "buy"


def _precision_step(raw: Any, precision_mode: Optional[int] = None) -> Optional[float]:
    """Resolve one ccxt ``precision.*`` entry (``price`` or ``amount``)
    to a step size, using ``exchange.precisionMode`` to disambiguate
    when the caller has it.

    ccxt exposes three incompatible conventions for this field
    (``ccxt.DECIMAL_PLACES``, ``ccxt.SIGNIFICANT_DIGITS``,
    ``ccxt.TICK_SIZE``) and does not normalise them; which one a given
    exchange uses is exactly what ``exchange.precisionMode`` says.
    Without it, this used to guess "values >= 1 are a digit count" --
    which is correct for ``DECIMAL_PLACES`` but silently wrong by
    10x-10^11x for a ``TICK_SIZE`` exchange whose tick happens to be
    >= 1 (a $1, $2, $5, $10 tick is common on options and inverse
    futures). Also refuses a non-integer "digit count" (``2.5``)
    instead of truncating it, since that value can only be honest
    under ``TICK_SIZE`` -- the same ambiguity in miniature.
    """
    if raw is None:
        return None
    try:
        v = float(raw)
    except (TypeError, ValueError):
        return None
    if v <= 0:
        return None

    if precision_mode == _TICK_SIZE:
        return v
    if precision_mode in (_DECIMAL_PLACES, _SIGNIFICANT_DIGITS):
        if v != int(v):
            return None
        return 10.0 ** -int(v)

    # No precisionMode available -- fall back to the previous
    # heuristic (values >= 1 read as a decimal-digit count, values < 1
    # read as the tick itself), but validated: a non-integer "digit
    # count" no longer gets silently truncated.
    if v >= 1:
        if v != int(v):
            return None
        return 10.0 ** -int(v)
    return v


def _market_tick_size(market: Mapping[str, Any], precision_mode: Optional[int] = None) -> float:
    """Extract a tick size from a ccxt ``market`` dict. Falls back to
    0.01 if the ``precision.price`` field is missing or does not
    parse under :func:`_precision_step`."""
    p = market.get("precision") or {}
    step = _precision_step(p.get("price"), precision_mode)
    return step if step is not None else 0.01


def _precision_digits(step: Optional[float]) -> int:
    """Decimal digit count implied by a step/tick size -- the inverse
    of the ``DECIMAL_PLACES`` branch of :func:`_precision_step` --
    e.g. ``0.001`` -> 3, ``1e-8`` -> 8, ``1.0`` -> 0. Feeds the
    market-data recorder's ``price_precision`` / ``qty_precision``
    fields from the tick this adapter actually resolved and
    registered, rather than a hardcoded guess."""
    if step is None or step <= 0:
        return 0
    if step >= 1:
        return 0
    digits = round(-math.log10(step))
    return max(0, digits)


class UnsupportedOrderType(RuntimeError):
    """Raised when a strategy emits an order type the adapter can't route."""


# The ccxt `type` each signal type is placed with (see the _place_* methods).
_CCXT_ORDER_TYPE = {
    "MARKET": "market",
    "LIMIT": "limit",
    "STOP_MARKET": "stop_market",
    "STOP_LIMIT": "stop_limit",
    "TAKE_PROFIT_MARKET": "take_profit_market",
    "TAKE_PROFIT_LIMIT": "take_profit_limit",
    "TRAILING_STOP": "trailing_stop_market",
    "TRAILING_STOP_PERCENT": "trailing_stop_market",
    "CLOSE_POSITION": "market",
}

# ccxt's unified statuses for an order that will not trade again.
_TERMINAL_ORDER_STATUSES = frozenset({"closed", "canceled", "expired", "rejected"})

# The order-event status each of ccxt's terminal statuses other than
# "closed" is reported as. "closed" is the last fill, not a status of its own.
_ENDED_WITHOUT_FILL = {"canceled": "CANCELED", "expired": "EXPIRED", "rejected": "REJECTED"}

# The lrvx order type name (as OrderEventData.order_type spells it) of a
# ccxt `type`, for an order the broker did not place.
_LRVX_ORDER_TYPE = {
    "market": "market",
    "limit": "limit",
    "stop_market": "stop_market",
    "stop_limit": "stop_limit",
    "take_profit_market": "take_profit_market",
    "take_profit_limit": "take_profit_limit",
    "trailing_stop_market": "trailing_stop",
}

# How many finished ccxt order ids are remembered, so a late repeat of an
# order's last update is not reported twice.
_FINISHED_ORDERS_KEPT = 4096

# Types with a trigger and no limit price.
_TRIGGER_AT_MARKET_TYPES = frozenset(
    {"stop_market", "take_profit_market", "trailing_stop_market"})


@dataclass
class _PlacedOrder:
    symbol: Optional[str]
    side: str
    ccxt_type: str
    trigger_price: float


@dataclass
class _OrderProgress:
    """What has been reported of one exchange order so far."""

    lrvx_id: int
    symbol: Optional[str]
    side: str
    order_type: str
    filled: float = 0.0
    cost: float = 0.0
    accepted: bool = False


def _takes_legacy_order_update(strategy: Any) -> bool:
    """True for a strategy whose ``on_order_update`` has the four-argument
    form ``(ccxt_sym, status, filled, avg_price)`` this broker used to call,
    not ``lrvx.Strategy``'s ``(ctx, event)``.

    Judged on the bound callable, by the positional arguments it requires:
    exactly four. A parameter with a default is optional, so
    ``on_order_update(self, ctx, event, a=1, b=2)`` requires two and is the
    new form; ``*args`` and keyword-only parameters do not count either.
    """
    fn = getattr(strategy, "on_order_update", None)
    if fn is None or not callable(fn):
        return False
    try:
        params = list(inspect.signature(fn).parameters.values())
    except (TypeError, ValueError):
        # A built-in with no signature: lrvx.Strategy's own method.
        return False
    required = [p for p in params
                if p.kind in (p.POSITIONAL_ONLY, p.POSITIONAL_OR_KEYWORD)
                and p.default is p.empty]
    return len(required) == 4


# Per-exchange override for ccxt's order-book depth. ccxt.pro's default
# for bitget USDT-FUTURES is `books50`, which the venue rejects with
# error 30016 (`books15` / `books5` only). Other exchanges use ccxt's
# own default when not listed here.
_BOOK_DEPTH_OVERRIDE = {
    "bitget": 15,
}


# ── Broker ─────────────────────────────────────────────────────────────


@dataclass
class CcxtBroker:
    """Async live broker glue for lrvx strategies on top of ccxt.pro.

    Owns its own ``SymbolRegistry`` and ``Runner`` — call
    ``add_symbol`` to register symbols, ``add_strategy`` to attach
    strategies, then ``await run(...)`` to start streams.

    Parameters
    ----------
    exchange_id
        ccxt.pro exchange class name, e.g. ``"binance"`` or ``"bybit"``.
    api_key, secret, password
        Optional credentials. Leave blank for public market data only.
    sandbox
        Whether to call ``exchange.set_sandbox_mode(True)``. Not every
        exchange supports a sandbox; if not, ccxt raises and the
        broker reports it through ``on_error``.
    retry_initial_delay, retry_max_delay, retry_multiplier
        Exponential backoff parameters for stream errors. Delay starts
        at ``retry_initial_delay`` seconds, multiplies by
        ``retry_multiplier`` after each consecutive failure, caps at
        ``retry_max_delay``, and resets to the initial value after a
        successful yield.
    on_error
        Callable ``(context_str, exception) -> None`` for stream and
        order errors. ``context_str`` is the ccxt symbol or a short
        tag like ``"balance"`` / ``"orders"``. Default logs at WARN.
    exchange
        Dependency injection for tests. Production callers leave this
        ``None`` and the broker constructs the ccxt.pro exchange in
        ``_ensure_exchange``.
    """

    exchange_id: str
    api_key: Optional[str] = None
    secret: Optional[str] = None
    password: Optional[str] = None
    sandbox: bool = False

    retry_initial_delay: float = 1.0
    retry_max_delay: float = 60.0
    retry_multiplier: float = 2.0

    on_error: Optional[Callable[[str, BaseException], None]] = None
    exchange: Any = None

    _registry: Any = field(default=None, init=False, repr=False)
    _runner: Any = field(default=None, init=False, repr=False)
    _strategies: list = field(default_factory=list, init=False, repr=False)
    _market_data_recorder: Any = field(default=None, init=False, repr=False)
    _sym_to_ccxt: dict = field(default_factory=dict, init=False, repr=False)
    _ccxt_to_sym: dict = field(default_factory=dict, init=False, repr=False)
    # lrvx symbol id -> (price_precision, qty_precision) resolved at
    # add_symbol() time, so set_market_data_recorder() -- called after
    # symbols already exist -- can mirror the same precision into a
    # recorder attached late instead of the hardcoded (2, 8) it used to
    # always send regardless of the market's actual precision.
    _sym_precision: dict = field(default_factory=dict, init=False, repr=False)
    _lrvx_to_ccxt_order: dict = field(default_factory=dict, init=False, repr=False)
    # lrvx order id -> _PlacedOrder: what a later CANCEL or MODIFY, which
    # carries the id alone, has to tell the exchange about the order.
    _placed_orders: dict = field(default_factory=dict, init=False, repr=False)
    _ccxt_orders_by_sym: dict = field(default_factory=dict, init=False, repr=False)
    # ccxt order id -> _OrderProgress, for every order seen and not finished.
    _order_progress: dict = field(default_factory=dict, init=False, repr=False)
    # ccxt ids of finished orders, oldest first, at most _FINISHED_ORDERS_KEPT.
    _finished_orders: Any = field(default_factory=OrderedDict, init=False, repr=False)
    _orders_task: Any = field(default=None, init=False, repr=False)
    # Bound four-argument on_order_update methods (see "Compatibility").
    _legacy_order_updates: list = field(default_factory=list, init=False, repr=False)
    _tasks: list = field(default_factory=list, init=False, repr=False)
    _loop: Optional[asyncio.AbstractEventLoop] = field(default=None, init=False, repr=False)
    _markets_loaded: bool = field(default=False, init=False, repr=False)

    def __post_init__(self) -> None:
        if self.retry_initial_delay <= 0:
            raise ValueError("retry_initial_delay must be > 0")
        if self.retry_max_delay < self.retry_initial_delay:
            raise ValueError("retry_max_delay must be >= retry_initial_delay")
        if self.retry_multiplier < 1.0:
            raise ValueError("retry_multiplier must be >= 1")
        if self.on_error is None:
            self.on_error = self._default_on_error
        import lrvx
        self._registry = lrvx.SymbolRegistry()

    @staticmethod
    def _default_on_error(context: str, exc: BaseException) -> None:
        logger.warning("ccxt broker error [%s]: %r", context, exc)

    # ── Lifecycle ─────────────────────────────────────────────────────

    @property
    def runner(self) -> Any:
        """The ``lrvx.Runner`` the strategies are attached to; ``None``
        until ``run()`` has started it."""
        return self._runner

    async def __aenter__(self) -> "CcxtBroker":
        await self._ensure_exchange()
        return self

    async def __aexit__(self, *exc_info: Any) -> None:
        await self.close()

    async def _ensure_exchange(self) -> None:
        if self.exchange is not None:
            return
        try:
            import ccxt.pro as ccxtpro  # type: ignore
        except ImportError as e:
            raise ImportError(
                "CcxtBroker requires ccxt>=4 with the .pro async API. "
                "Install with: pip install ccxt"
            ) from e
        cls = getattr(ccxtpro, self.exchange_id, None)
        if cls is None:
            raise ValueError(f"unknown ccxt exchange {self.exchange_id!r}")
        config: dict = {}
        if self.api_key:
            config["apiKey"] = self.api_key
        if self.secret:
            config["secret"] = self.secret
        if self.password:
            config["password"] = self.password
        self.exchange = cls(config)
        if self.sandbox:
            try:
                self.exchange.set_sandbox_mode(True)
            except BaseException as e:
                if self.on_error:
                    self.on_error("sandbox", e)

    async def close(self) -> None:
        """Cancel all stream tasks, stop the runner, close the exchange."""
        await self.stop()
        if self._runner is not None:
            try:
                self._runner.stop()
            except BaseException as e:
                if self.on_error:
                    self.on_error("runner.stop", e)
            self._runner = None
        if self.exchange is not None:
            close = getattr(self.exchange, "close", None)
            if close is not None:
                try:
                    res = close()
                    if asyncio.iscoroutine(res):
                        await res
                except BaseException as e:
                    if self.on_error:
                        self.on_error("exchange.close", e)

    async def stop(self) -> None:
        """Cancel all stream tasks. Idempotent. Does not close exchange."""
        for t in self._tasks:
            t.cancel()
        if self._tasks:
            await asyncio.gather(*self._tasks, return_exceptions=True)
        self._tasks.clear()
        self._orders_task = None

    # ── Symbol registration ──────────────────────────────────────────

    async def add_symbol(self, ccxt_sym: str) -> Any:
        """Register a ccxt symbol, return the lrvx ``Symbol`` object.

        First call triggers a one-time ``exchange.load_markets()``
        round-trip; subsequent calls reuse the cached metadata. Tick
        size comes from ``markets[ccxt_sym]["precision"]["price"]``
        via :func:`_market_tick_size`.
        """
        await self._ensure_exchange()
        if not self._markets_loaded:
            await self.exchange.load_markets()
            self._markets_loaded = True
        markets = getattr(self.exchange, "markets", None) or {}
        market = markets.get(ccxt_sym)
        if market is None:
            raise ValueError(
                f"symbol {ccxt_sym!r} is not listed on {self.exchange_id} "
                f"(after load_markets())"
            )
        precision_mode = getattr(self.exchange, "precisionMode", None)
        tick = _market_tick_size(market, precision_mode)
        sym = self._registry.add_symbol(self.exchange_id, ccxt_sym, tick)
        self._sym_to_ccxt[int(sym)] = ccxt_sym
        self._ccxt_to_sym[ccxt_sym] = sym

        # price_precision comes from the same tick just registered with
        # the symbol, not a re-derivation, so the recorder and the
        # registry can never disagree about it; qty_precision comes
        # from the market's `precision.amount` the same way. Both used
        # to be the hardcoded constants 2 and 8 regardless of the
        # market's actual precision. Resolved and remembered here even
        # with no recorder attached yet, so a recorder attached later
        # (set_market_data_recorder) mirrors the right values too.
        amount_step = _precision_step((market.get("precision") or {}).get("amount"), precision_mode)
        price_precision = _precision_digits(tick)
        qty_precision = _precision_digits(amount_step) if amount_step is not None else 8
        self._sym_precision[int(sym)] = (price_precision, qty_precision)

        # Mirror the symbol into the attached recorder (if any) so
        # tapes recorded directly via the broker carry a populated
        # metadata.json::symbols. set_market_data_recorder() handles
        # the symmetric case (recorder attached after symbols added).
        recorder = self._market_data_recorder
        add_symbol = getattr(recorder, "add_symbol", None) if recorder else None
        if callable(add_symbol):
            flat_name = ccxt_sym.replace("/", "").split(":")[0]
            try:
                add_symbol(int(sym), flat_name, "", "", price_precision, qty_precision)
            except Exception:
                pass
        return sym

    def add_strategy(self, strategy: Any) -> None:
        """Queue a strategy to attach when ``run()`` starts the runner."""
        self._strategies.append(strategy)
        if _takes_legacy_order_update(strategy):
            warnings.warn(
                f"{type(strategy).__name__}.on_order_update(ccxt_sym, status, filled, "
                "avg_price) is the form CcxtBroker used to call; it is still called for "
                "every update of the orders stream, but lrvx.Strategy's form is "
                "on_order_update(ctx, event), with fills in on_fill(ctx, event), as in a "
                "backtest. See 'Order updates' in lrvx.ccxt.",
                DeprecationWarning, stacklevel=2)
            legacy = strategy.on_order_update
            self._legacy_order_updates.append(legacy)

            # The runner calls on_order_update(ctx, event); the old form has
            # no place for that call.
            def _old_form_only(*args: Any) -> None:
                if len(args) == 4:
                    legacy(*args)

            strategy.on_order_update = _old_form_only

    def set_market_data_recorder(self, recorder: Any) -> None:
        """Attach a ``MarketDataRecorderHook`` subclass to capture
        every trade / book update flowing through the broker.

        The recorder is wired into the internal ``Runner`` when
        ``run()`` starts. Used by ``lrvx tape record`` to dump live
        feeds to a ``.lrvx`` directory; downstream code can pass
        any subclass that implements the hook interface.

        If the recorder is a :class:`lrvx.BinaryLogRecorderHook`
        (the canonical sink for ``.lrvx`` capture) and the broker
        already has symbols registered, this method auto-registers
        each ``(symbol_id, ccxt_name)`` pair with the recorder so the
        resulting tape's ``metadata.json`` carries the symbol table.
        Without this step a script that records directly via the
        broker leaves ``metadata.json::symbols`` empty and
        :class:`lrvx.MergedTapeReader` can't rekey events on read.
        """
        self._market_data_recorder = recorder
        # Best-effort: skip if the hook lacks an `add_symbol`
        # method (e.g. a user-defined `MarketDataRecorderHook`
        # subclass that doesn't speak the BinaryLogRecorderHook
        # surface).
        add_symbol = getattr(recorder, "add_symbol", None)
        if not callable(add_symbol):
            return
        for ccxt_name, lrvx_sid in self._ccxt_to_sym.items():
            # Strip the perp suffix so MergedTapeReader's
            # (exchange, name) key matches across exchanges that
            # label perps differently (e.g. ":USDT" appended).
            flat_name = ccxt_name.replace("/", "").split(":")[0]
            price_precision, qty_precision = self._sym_precision.get(
                int(lrvx_sid), (2, 8)
            )
            try:
                add_symbol(int(lrvx_sid), flat_name, "", "", price_precision, qty_precision)
            except Exception:
                # Hook may reject duplicate adds; not our problem.
                pass

    # ── Run loop ──────────────────────────────────────────────────────

    async def run(
        self,
        *,
        streams: Iterable[str] = ("trades", "book"),
        book_depth: int | None = None,
        reconcile: bool = True,
    ) -> None:
        """Start the runner, dispatch positions, spawn streams, await.

        ``streams`` may include ``"trades"``, ``"book"``, ``"bbo"``,
        and ``"orders"``. ``"bbo"`` runs ``watch_bids_asks`` next to
        the book stream and forwards into ``runner.on_book_top``: the
        touch stays fresh between (often throttled) depth updates
        without the one-level frame replacing the depth book. The
        orders stream is a single ``watch_orders()`` task — ccxt
        aggregates updates across symbols. Returns when every task is
        cancelled (typically via ``Ctrl+C`` or an outer ``async
        with`` exit).
        """
        await self._ensure_exchange()
        valid = {"trades", "book", "bbo", "orders"}
        unknown = [s for s in streams if s not in valid]
        if unknown:
            raise ValueError(
                f"unknown stream(s) {unknown}; expected subset of {sorted(valid)}"
            )
        if not self._ccxt_to_sym:
            raise ValueError(
                "no symbols registered — call await broker.add_symbol(...) first"
            )

        if book_depth is None:
            book_depth = _BOOK_DEPTH_OVERRIDE.get(self.exchange_id)

        import lrvx
        self._runner = lrvx.Runner(self._registry, on_signal=self._on_signal)
        for strat in self._strategies:
            self._runner.add_strategy(strat)
        if self._market_data_recorder is not None:
            self._runner.set_market_data_recorder(self._market_data_recorder)
        self._runner.start()

        self._loop = asyncio.get_running_loop()

        if reconcile:
            await self._reconcile_positions()

        if "trades" in streams:
            for ccxt_sym, lrvx_sym in self._ccxt_to_sym.items():
                self._tasks.append(asyncio.create_task(
                    self._trades_loop(ccxt_sym, lrvx_sym),
                    name=f"ccxt-trades-{ccxt_sym}",
                ))
        if "book" in streams:
            for ccxt_sym, lrvx_sym in self._ccxt_to_sym.items():
                self._tasks.append(asyncio.create_task(
                    self._book_loop(ccxt_sym, lrvx_sym, book_depth),
                    name=f"ccxt-book-{ccxt_sym}",
                ))
        if "bbo" in streams:
            for ccxt_sym, lrvx_sym in self._ccxt_to_sym.items():
                self._tasks.append(asyncio.create_task(
                    self._bbo_loop(ccxt_sym, lrvx_sym),
                    name=f"ccxt-bbo-{ccxt_sym}",
                ))
        if "orders" in streams:
            self._ensure_orders_stream()

        try:
            await asyncio.gather(*self._tasks)
        except asyncio.CancelledError:
            for t in self._tasks:
                t.cancel()
            raise

    # ── Stream loops ──────────────────────────────────────────────────

    async def _stream(
        self,
        fn: Callable[..., Any],
        context: str,
        *args: Any,
        **kwargs: Any,
    ):
        """Async generator: yield results from ``fn`` with backoff retry.

        After a non-cancel exception, sleep ``delay``, multiply
        ``delay`` by ``retry_multiplier`` (capped at
        ``retry_max_delay``), retry. Reset to ``retry_initial_delay``
        after each successful yield.
        """
        delay = self.retry_initial_delay
        while True:
            try:
                result = await fn(*args, **kwargs)
            except asyncio.CancelledError:
                raise
            except BaseException as e:
                if self.on_error:
                    self.on_error(context, e)
                await asyncio.sleep(delay)
                delay = min(delay * self.retry_multiplier, self.retry_max_delay)
                continue
            delay = self.retry_initial_delay
            yield result

    async def _trades_loop(self, ccxt_sym: str, lrvx_sym: Any) -> None:
        sym_id = int(lrvx_sym)
        async for trades in self._stream(
            self.exchange.watch_trades, ccxt_sym, ccxt_sym
        ):
            for t in trades:
                self._runner.on_trade(
                    sym_id,
                    float(t["price"]),
                    float(t["amount"]),
                    _trade_is_buy(t),
                    _ccxt_ts_ns(t),
                )

    async def _book_loop(
        self, ccxt_sym: str, lrvx_sym: Any, depth: int | None
    ) -> None:
        sym_id = int(lrvx_sym)
        # depth=None → let ccxt pick the venue-appropriate default channel.
        # Bitget USDT-FUTURES rejects books50 with code 30016, so we don't
        # want to force a depth unless the caller asked for one explicitly.
        stream_kwargs = {} if depth is None else {"limit": depth}
        async for ob in self._stream(
            self.exchange.watch_order_book, ccxt_sym, ccxt_sym, **stream_kwargs
        ):
            bids_raw = ob.get("bids") or []
            asks_raw = ob.get("asks") or []
            if depth is not None:
                bids_raw = bids_raw[:depth]
                asks_raw = asks_raw[:depth]
            self._runner.on_book_snapshot(
                sym_id,
                [float(b[0]) for b in bids_raw],
                [float(b[1]) for b in bids_raw],
                [float(a[0]) for a in asks_raw],
                [float(a[1]) for a in asks_raw],
                _ccxt_ts_ns(ob),
            )

    async def _bbo_loop(self, ccxt_sym: str, lrvx_sym: Any) -> None:
        sym_id = int(lrvx_sym)
        async for tickers in self._stream(
            self.exchange.watch_bids_asks, ccxt_sym, [ccxt_sym]
        ):
            # watch_bids_asks returns a dict of tickers keyed by symbol on
            # most venues; a few hand back the single ticker directly. A
            # frame with neither shape carries no quote and is skipped —
            # publishing it would erase the live touch under a fresh stamp.
            t = tickers if "bid" in tickers or "ask" in tickers else tickers.get(ccxt_sym)
            if t is None:
                continue
            bid = t.get("bid")
            ask = t.get("ask")
            if bid is None and ask is None:
                continue
            self._runner.on_book_top(
                sym_id,
                (float(bid), float(t.get("bidVolume") or 0.0)) if bid is not None else None,
                (float(ask), float(t.get("askVolume") or 0.0)) if ask is not None else None,
                _ccxt_ts_ns(t),
            )

    async def _orders_loop(self) -> None:
        async for orders in self._stream(self.exchange.watch_orders, "orders"):
            for o in orders:
                self._dispatch_order_update(o)

    def _ensure_orders_stream(self) -> None:
        """Start the orders stream unless it is running.

        Called for ``streams=(..., "orders")`` and before the first order
        is placed: a broker that places orders needs their updates, or a
        resting order is never reported filled and never leaves its maps.
        """
        if self._orders_task is not None and not self._orders_task.done():
            return
        self._orders_task = asyncio.create_task(self._orders_loop(), name="ccxt-orders")
        self._tasks.append(self._orders_task)

    # ── Outbound: signal → exchange order ─────────────────────────────

    def _on_signal(self, sig: Any) -> None:
        """Runner signal callback. Schedules order placement on the loop.

        Called synchronously from inside the C++ runner — sync runner
        means same asyncio thread, threaded runner means a worker
        thread. Both paths funnel into ``_handle_signal`` via
        ``run_coroutine_threadsafe`` so the ccxt call always runs on
        the broker's event loop.
        """
        try:
            sym_id = int(sig.symbol)
        except BaseException as e:
            if self.on_error:
                self.on_error("?", e)
            return
        ccxt_sym = self._sym_to_ccxt.get(sym_id)
        if ccxt_sym is None and (sig.order_type or "").upper() not in ("CANCEL", "MODIFY"):
            # CANCEL and MODIFY signals carry an order id and no symbol;
            # _handle_signal finds the symbol the order was placed on.
            if self.on_error:
                self.on_error(
                    f"sym_id={sym_id}",
                    RuntimeError("signal for unmapped symbol"),
                )
            return
        coro = self._handle_signal(ccxt_sym, sig)
        loop = self._loop
        if loop is None:
            if self.on_error:
                self.on_error(
                    str(ccxt_sym),
                    RuntimeError("signal received before broker.run() started"),
                )
            return
        try:
            running = asyncio.get_running_loop()
        except RuntimeError:
            running = None
        if running is loop:
            loop.create_task(coro)
        else:
            asyncio.run_coroutine_threadsafe(coro, loop)

    async def _handle_signal(self, ccxt_sym: Optional[str], sig: Any) -> None:
        order_type = (getattr(sig, "order_type", "") or "").upper()
        side = (getattr(sig, "side", "") or "").lower()
        try:
            if order_type in _CCXT_ORDER_TYPE or order_type == "MODIFY":
                self._ensure_orders_stream()
            if order_type == "MARKET":
                await self._place_market(ccxt_sym, side, sig)
            elif order_type == "LIMIT":
                await self._place_limit(ccxt_sym, side, sig)
            elif order_type == "STOP_MARKET":
                await self._place_stop_market(ccxt_sym, side, sig)
            elif order_type == "STOP_LIMIT":
                await self._place_stop_limit(ccxt_sym, side, sig)
            elif order_type == "TAKE_PROFIT_MARKET":
                await self._place_tp_market(ccxt_sym, side, sig)
            elif order_type == "TAKE_PROFIT_LIMIT":
                await self._place_tp_limit(ccxt_sym, side, sig)
            elif order_type in ("TRAILING_STOP", "TRAILING_STOP_PERCENT"):
                await self._place_trailing(ccxt_sym, side, sig, order_type)
            elif order_type == "CLOSE_POSITION":
                await self._place_close(ccxt_sym, side, sig)
            elif order_type == "CANCEL":
                await self._cancel(ccxt_sym, sig)
            elif order_type == "CANCEL_ALL":
                await self._cancel_all(ccxt_sym)
            elif order_type == "MODIFY":
                await self._modify(ccxt_sym, side, sig)
            else:
                raise UnsupportedOrderType(
                    f"order_type {order_type!r} not handled by CcxtBroker"
                )
        except asyncio.CancelledError:
            raise
        except BaseException as e:
            if self.on_error:
                self.on_error(str(ccxt_sym or "?"), e)

    # ── Order placement helpers ───────────────────────────────────────

    @staticmethod
    def _qty(sig: Any) -> float:
        q = float(getattr(sig, "quantity", 0.0))
        if q <= 0:
            raise ValueError(f"non-positive quantity {q!r} on signal")
        return q

    @staticmethod
    def _price(sig: Any) -> float:
        p = float(getattr(sig, "price", 0.0))
        if p <= 0:
            raise ValueError(f"non-positive price {p!r} on signal")
        return p

    @staticmethod
    def _trigger(sig: Any) -> float:
        p = float(getattr(sig, "trigger_price", 0.0))
        if p <= 0:
            raise ValueError(f"non-positive trigger_price {p!r} on signal")
        return p

    def _track_order(self, ccxt_sym: Optional[str], sig: Any, res: Any) -> None:
        ccxt_id = res.get("id") if isinstance(res, dict) else None
        lrvx_id = int(getattr(sig, "order_id", 0) or 0)
        if not ccxt_id:
            return
        order_type = (getattr(sig, "order_type", "") or "").upper()
        # The stream may have reported the order before this answer arrived;
        # what it reported then stays reported, under the id known now.
        progress = self._order_progress.get(str(ccxt_id))
        if progress is None and str(ccxt_id) not in self._finished_orders:
            progress = self._order_progress[str(ccxt_id)] = _OrderProgress(
                lrvx_id=lrvx_id, symbol=ccxt_sym,
                side=(getattr(sig, "side", "") or "").lower(),
                order_type=_LRVX_ORDER_TYPE.get(_CCXT_ORDER_TYPE.get(order_type, "limit"),
                                                "limit"))
        elif progress is not None:
            progress.lrvx_id = lrvx_id
        self._report_order(res)
        if str(res.get("status", "")) in _TERMINAL_ORDER_STATUSES:
            # Finished in the answer to its placement (a market order
            # usually is): there is nothing left to cancel or modify, and no
            # update for it may ever arrive, so it is not kept.
            if lrvx_id and self._runner is not None:
                self._runner.order_closed(lrvx_id)
            return
        if lrvx_id:
            self._lrvx_to_ccxt_order[lrvx_id] = str(ccxt_id)
            self._placed_orders[lrvx_id] = _PlacedOrder(
                symbol=ccxt_sym,
                side=(getattr(sig, "side", "") or "").lower(),
                ccxt_type=_CCXT_ORDER_TYPE.get(order_type, "limit"),
                trigger_price=float(getattr(sig, "trigger_price", 0.0) or 0.0),
            )
        if ccxt_sym:
            self._ccxt_orders_by_sym.setdefault(ccxt_sym, set()).add(str(ccxt_id))

    async def _place_market(self, sym: Optional[str], side: str, sig: Any) -> None:
        if not sym:
            raise ValueError("MARKET order requires a tracked symbol")
        qty = self._qty(sig)
        if side == "buy":
            res = await self.exchange.create_market_buy_order(sym, qty)
        elif side == "sell":
            res = await self.exchange.create_market_sell_order(sym, qty)
        else:
            raise ValueError(f"unknown side {side!r}")
        self._track_order(sym, sig, res)

    async def _place_limit(self, sym: Optional[str], side: str, sig: Any) -> None:
        if not sym:
            raise ValueError("LIMIT order requires a tracked symbol")
        qty = self._qty(sig)
        price = self._price(sig)
        if side == "buy":
            res = await self.exchange.create_limit_buy_order(sym, qty, price)
        elif side == "sell":
            res = await self.exchange.create_limit_sell_order(sym, qty, price)
        else:
            raise ValueError(f"unknown side {side!r}")
        self._track_order(sym, sig, res)

    async def _place_stop_market(self, sym: Optional[str], side: str, sig: Any) -> None:
        if not sym:
            raise ValueError("STOP_MARKET requires a tracked symbol")
        res = await self.exchange.create_order(
            sym, "stop_market", side, self._qty(sig), None,
            {"stopPrice": self._trigger(sig)},
        )
        self._track_order(sym, sig, res)

    async def _place_stop_limit(self, sym: Optional[str], side: str, sig: Any) -> None:
        if not sym:
            raise ValueError("STOP_LIMIT requires a tracked symbol")
        res = await self.exchange.create_order(
            sym, "stop_limit", side, self._qty(sig), self._price(sig),
            {"stopPrice": self._trigger(sig)},
        )
        self._track_order(sym, sig, res)

    async def _place_tp_market(self, sym: Optional[str], side: str, sig: Any) -> None:
        if not sym:
            raise ValueError("TAKE_PROFIT_MARKET requires a tracked symbol")
        res = await self.exchange.create_order(
            sym, "take_profit_market", side, self._qty(sig), None,
            {"stopPrice": self._trigger(sig)},
        )
        self._track_order(sym, sig, res)

    async def _place_tp_limit(self, sym: Optional[str], side: str, sig: Any) -> None:
        if not sym:
            raise ValueError("TAKE_PROFIT_LIMIT requires a tracked symbol")
        res = await self.exchange.create_order(
            sym, "take_profit_limit", side, self._qty(sig), self._price(sig),
            {"stopPrice": self._trigger(sig)},
        )
        self._track_order(sym, sig, res)

    async def _place_trailing(
        self, sym: Optional[str], side: str, sig: Any, kind: str,
    ) -> None:
        if not sym:
            raise ValueError(f"{kind} requires a tracked symbol")
        if kind == "TRAILING_STOP_PERCENT":
            bps = int(getattr(sig, "trailing_bps", 0) or 0)
            if bps <= 0:
                raise ValueError(f"non-positive trailing_bps {bps!r}")
            params = {"callbackRate": bps / 100.0}
        else:
            offset = float(getattr(sig, "trailing_offset", 0.0) or 0.0)
            if offset <= 0:
                raise ValueError(f"non-positive trailing_offset {offset!r}")
            params = {"trailingDelta": offset}
        res = await self.exchange.create_order(
            sym, "trailing_stop_market", side, self._qty(sig), None, params,
        )
        self._track_order(sym, sig, res)

    async def _place_close(self, sym: Optional[str], side: str, sig: Any) -> None:
        if not sym:
            raise ValueError("CLOSE_POSITION requires a tracked symbol")
        res = await self.exchange.create_order(
            sym, "market", side, self._qty(sig), None, {"reduceOnly": True},
        )
        self._track_order(sym, sig, res)

    async def _cancel(self, sym: Optional[str], sig: Any) -> None:
        lrvx_id = int(getattr(sig, "order_id", 0) or 0)
        ccxt_id = self._lrvx_to_ccxt_order.get(lrvx_id)
        if ccxt_id is None:
            raise ValueError(
                f"CANCEL: no ccxt order id tracked for lrvx order_id={lrvx_id}"
            )
        placed = self._placed_orders.pop(lrvx_id, None)
        if not sym and placed is not None:
            sym = placed.symbol
        res = await self.exchange.cancel_order(ccxt_id, sym)
        self._lrvx_to_ccxt_order.pop(lrvx_id, None)
        if sym and sym in self._ccxt_orders_by_sym:
            self._ccxt_orders_by_sym[sym].discard(ccxt_id)
        self._report_cancelled(ccxt_id, res)
        if self._runner is not None:
            self._runner.order_closed(lrvx_id)

    async def _cancel_all(self, sym: Optional[str]) -> None:
        if not sym:
            raise ValueError("CANCEL_ALL requires a tracked symbol")
        ids = list(self._ccxt_orders_by_sym.get(sym, set()))
        if not ids:
            return
        cancelled = []
        for ccxt_id in ids:
            try:
                res = await self.exchange.cancel_order(ccxt_id, sym)
            except asyncio.CancelledError:
                raise
            except BaseException as e:
                # Still on the exchange as far as anyone knows: it stays
                # tracked, so a later cancel or the orders stream can end it.
                if self.on_error:
                    self.on_error(sym, e)
                continue
            cancelled.append(ccxt_id)
            self._report_cancelled(ccxt_id, res)
        remaining = self._ccxt_orders_by_sym.get(sym)
        if remaining is not None:
            remaining.difference_update(cancelled)
            if not remaining:
                del self._ccxt_orders_by_sym[sym]
        # Drop reverse-lookup entries that pointed at the cancelled ids, and
        # tell the runner each of those orders is finished.
        for fid in [f for f, c in self._lrvx_to_ccxt_order.items() if c in cancelled]:
            self._lrvx_to_ccxt_order.pop(fid, None)
            self._placed_orders.pop(fid, None)
            if self._runner is not None:
                self._runner.order_closed(fid)

    async def _modify(self, sym: Optional[str], side: str, sig: Any) -> None:
        lrvx_id = int(getattr(sig, "order_id", 0) or 0)
        ccxt_id = self._lrvx_to_ccxt_order.get(lrvx_id)
        if ccxt_id is None:
            raise ValueError(
                f"MODIFY: no ccxt order id tracked for lrvx order_id={lrvx_id}"
            )
        new_price = float(getattr(sig, "new_price", 0.0) or 0.0)
        new_qty = float(getattr(sig, "new_quantity", 0.0) or 0.0)
        if new_qty <= 0:
            raise ValueError(f"MODIFY: non-positive new_quantity {new_qty!r}")
        edit = getattr(self.exchange, "edit_order", None)
        if edit is None:
            raise UnsupportedOrderType(
                f"exchange {self.exchange_id} does not support edit_order"
            )
        # The signal carries no symbol, side or type: they are the order's.
        # An order placed outside _track_order falls back to what the signal
        # says and to 'limit'.
        placed = self._placed_orders.get(lrvx_id)
        ccxt_type = "limit"
        if placed is not None:
            sym = placed.symbol or sym
            side = placed.side or side
            ccxt_type = placed.ccxt_type
        price = new_price if new_price > 0 else None
        if ccxt_type in _TRIGGER_AT_MARKET_TYPES:
            # No limit price: the one price a modify carries is the trigger.
            res = await edit(ccxt_id, sym, ccxt_type, side, new_qty, None,
                             {"stopPrice": price})
        elif placed is not None and placed.trigger_price > 0:
            res = await edit(ccxt_id, sym, ccxt_type, side, new_qty, price,
                             {"stopPrice": placed.trigger_price})
        else:
            res = await edit(ccxt_id, sym, ccxt_type, side, new_qty, price)
        # An exchange that edits by cancel-and-replace answers with a new id.
        new_id = res.get("id") if isinstance(res, dict) else None
        if new_id and str(new_id) != ccxt_id:
            self._lrvx_to_ccxt_order[lrvx_id] = str(new_id)
            progress = self._order_progress.pop(ccxt_id, None)
            if progress is not None:
                # A new order on the exchange: nothing of it is filled yet.
                progress.filled = progress.cost = 0.0
                self._order_progress[str(new_id)] = progress
            if sym and sym in self._ccxt_orders_by_sym:
                self._ccxt_orders_by_sym[sym].discard(ccxt_id)
                self._ccxt_orders_by_sym[sym].add(str(new_id))
        # The answer says where the order stands now, like the answer to a
        # placement: a fill it carries is reported, and an order it reports
        # finished is closed.
        if isinstance(res, dict):
            answer = dict(res)
            answer.setdefault("id", self._lrvx_to_ccxt_order.get(lrvx_id, ccxt_id))
            self._report_order(answer)
            if str(answer.get("status", "")) in _TERMINAL_ORDER_STATUSES:
                self._note_order_closed(answer)

    # ── Position reconciliation ───────────────────────────────────────

    async def _reconcile_positions(self) -> None:
        """Fetch balance / positions, dispatch to strategy callbacks.

        Both calls are best-effort. ``fetch_positions`` is a derivatives
        concept; spot-only exchanges raise ``NotSupported``, which the
        broker swallows and proceeds with an empty position list.
        """
        try:
            await self.exchange.fetch_balance()
        except asyncio.CancelledError:
            raise
        except BaseException as e:
            if self.on_error:
                self.on_error("balance", e)

        positions: list = []
        try:
            res = await self.exchange.fetch_positions()
            if isinstance(res, list):
                positions = res
        except asyncio.CancelledError:
            raise
        except BaseException as e:
            msg = str(e).lower()
            if "notsupported" not in type(e).__name__.lower() and "not support" not in msg:
                if self.on_error:
                    self.on_error("positions", e)

        for pos in positions:
            ccxt_sym = pos.get("symbol", "")
            if ccxt_sym not in self._ccxt_to_sym:
                continue
            qty = float(pos.get("contracts") or pos.get("amount") or 0.0)
            avg = float(pos.get("entryPrice") or pos.get("average") or 0.0)
            if qty == 0:
                continue
            self._seed_position(ccxt_sym, pos, qty, avg)
            for strat in self._strategies:
                fn = getattr(strat, "on_initial_position", None)
                if callable(fn):
                    try:
                        fn(ccxt_sym, qty, avg)
                    except BaseException as e:
                        if self.on_error:
                            self.on_error(ccxt_sym, e)

    def _seed_position(self, ccxt_sym: str, pos: Mapping[str, Any], qty: float,
                       avg: float) -> None:
        """Tell the runner what the account already holds, so the strategy
        reads it from ``ctx.position`` and ``self.position()``.

        A runner keeps a position per strategy and an account's position
        names none, so it is seeded only when exactly one attached strategy
        trades the symbol. With several, none is seeded and ``on_error``
        says so: call ``runner.seed_position`` for the one it belongs to.
        """
        if self._runner is None:
            return
        # ccxt reports the size in ``contracts`` and the direction in
        # ``side``; some exchanges sign the size instead.
        signed = -abs(qty) if str(pos.get("side") or "").lower() == "short" else qty
        sym = int(self._ccxt_to_sym[ccxt_sym])
        owners = [s for s in self._strategies
                  if sym in [int(x) for x in getattr(s, "symbols", [])]]
        try:
            if len(owners) > 1:
                raise ValueError(
                    f"the account's position of {signed!r} in {ccxt_sym} could belong to any "
                    f"of {len(owners)} strategies attached to it; none was given it. Call "
                    "broker.runner.seed_position(strategy, symbol, position, "
                    "avg_entry_price) for the one that holds it")
            if owners:
                self._runner.seed_position(owners[0], sym, signed, avg)
        except BaseException as e:
            if self.on_error:
                self.on_error(ccxt_sym, e)

    # ── Order updates ─────────────────────────────────────────────────

    def _dispatch_order_update(self, o: Mapping[str, Any]) -> None:
        """One update of the orders stream: to the strategies, then, when
        the order is finished, out of the broker's maps."""
        if self._legacy_order_updates:
            ccxt_sym = str(o.get("symbol", ""))
            status = str(o.get("status", ""))
            filled = float(o.get("filled") or 0.0)
            avg = float(o.get("average") or o.get("price") or 0.0)
            for fn in self._legacy_order_updates:
                try:
                    fn(ccxt_sym, status, filled, avg)
                except BaseException as e:
                    if self.on_error:
                        self.on_error(ccxt_sym, e)
        self._report_order(o)
        if str(o.get("status", "")) in _TERMINAL_ORDER_STATUSES:
            self._note_order_closed(o)

    def _report_cancelled(self, ccxt_id: str, res: Any) -> None:
        """Report the answer to a cancel. An exchange that answers with no
        status has still cancelled the order."""
        update = dict(res) if isinstance(res, dict) else {}
        update["id"] = ccxt_id
        if not update.get("status") or update["status"] == "open":
            update["status"] = "canceled"
        self._report_order(update)

    def _report_order(self, o: Mapping[str, Any]) -> None:
        """Turn what the exchange says of one order into order events and
        hand them to the runner (see "Order updates" in the module
        docstring). Reports only what is new since the last call for the
        order, so the same state arriving twice produces nothing."""
        if not isinstance(o, Mapping):
            return
        ccxt_id = str(o.get("id") or "")
        if ccxt_id and ccxt_id in self._finished_orders:
            return
        progress = self._order_progress.get(ccxt_id) if ccxt_id else None
        if progress is None:
            progress = _OrderProgress(
                lrvx_id=0, symbol=o.get("symbol"),
                side=str(o.get("side") or "").lower(),
                order_type=_LRVX_ORDER_TYPE.get(str(o.get("type") or ""), "limit"))
            if ccxt_id:
                self._order_progress[ccxt_id] = progress
        status = str(o.get("status") or "")
        events = []  # (status, fill_qty, fill_price, reject_reason)

        filled = float(o.get("filled") or 0.0)
        grew = filled - progress.filled
        if grew > 1e-12:
            avg = float(o.get("average") or o.get("price") or 0.0)
            cost = avg * filled
            price = (cost - progress.cost) / grew if avg > 0 else 0.0
            if price <= 0:
                price = avg
            progress.filled, progress.cost = filled, cost
            remaining = o.get("remaining")
            done = status == "closed" or (remaining is not None and float(remaining) <= 0)
            events.append(("FILLED" if done else "PARTIALLY_FILLED", grew, price, ""))
        elif status == "open" and not progress.accepted:
            events.append(("ACCEPTED", 0.0, 0.0, ""))
        if status:
            progress.accepted = True
        if status in _ENDED_WITHOUT_FILL:
            reason = str(o.get("reason") or "") if status == "rejected" else ""
            events.append((_ENDED_WITHOUT_FILL[status], 0.0, 0.0, reason))

        if status in _TERMINAL_ORDER_STATUSES and ccxt_id:
            self._order_progress.pop(ccxt_id, None)
            self._finished_orders[ccxt_id] = None
            while len(self._finished_orders) > _FINISHED_ORDERS_KEPT:
                self._finished_orders.popitem(last=False)

        ccxt_sym = progress.symbol or o.get("symbol")
        sym = self._ccxt_to_sym.get(ccxt_sym)
        if not events or sym is None or self._runner is None:
            return
        if progress.side not in ("buy", "sell"):
            if self.on_error:
                self.on_error(str(ccxt_sym), ValueError(
                    f"order update for ccxt order {ccxt_id or '?'} names no side; "
                    f"its {events[0][0]} event was not delivered"))
            return
        ts_ns = _ccxt_ts_ns(o)
        for event_status, qty, price, reason in events:
            try:
                self._runner.on_order_event(
                    progress.lrvx_id, int(sym), progress.side, event_status,
                    order_type=progress.order_type, fill_qty=qty, fill_price=price,
                    ts_ns=ts_ns, reject_reason=reason)
            except BaseException as e:
                if self.on_error:
                    self.on_error(str(ccxt_sym), e)

    def _note_order_closed(self, o: Mapping[str, Any]) -> None:
        """Forget an order the exchange reports finished, and pass the news
        on to the runner by lrvx order id.

        The runner drops the order from the record it keeps for an attached
        executor (``Runner.order_closed``). The broker drops it from its own
        maps, which otherwise held every order ever placed: a CANCEL or
        MODIFY the strategy still sends for it is reported through
        ``on_error`` ("no ccxt order id tracked") and nothing is sent.
        """
        ccxt_id = o.get("id")
        if ccxt_id is None:
            return
        ccxt_id = str(ccxt_id)
        for lrvx_id, known in list(self._lrvx_to_ccxt_order.items()):
            if known != ccxt_id:
                continue
            del self._lrvx_to_ccxt_order[lrvx_id]
            self._placed_orders.pop(lrvx_id, None)
            if self._runner is not None:
                self._runner.order_closed(lrvx_id)
        # By the exchange's id as well: an order placed with no lrvx id is
        # in this set only.
        for ccxt_sym in [s for s, ids in self._ccxt_orders_by_sym.items() if ccxt_id in ids]:
            ids = self._ccxt_orders_by_sym[ccxt_sym]
            ids.discard(ccxt_id)
            if not ids:
                del self._ccxt_orders_by_sym[ccxt_sym]


__all__ = ["CcxtBroker", "UnsupportedOrderType"]
