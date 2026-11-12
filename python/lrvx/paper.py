"""Paper / sandbox trading mode — drive a strategy against a live
market data feed but route orders to ``lrvx.SimulatedExecutor``
instead of a real exchange.

The recipe is the same one a backtest uses for fills, applied to a
live source. Slippage and queue position behave the same as in
backtest, so a strategy that looks plausible in backtest can be
rehearsed against current market conditions before any real capital
is at stake.

Wiring:

    market data (ccxt) ──► runner.on_trade ──► SimulatedExecutor
                                                 │
                                                 ▼
                                              fills
                                                 │
              strategy.on_order_update ◄─── on_fill callback

The ``PaperBroker`` does the teeing: every trade the runner sees is
also fed into the simulator so its queue tracker stays current. Limit
fills, stop triggers, slippage, and TIF rules are all handled by
``SimulatedExecutor`` — the broker is a thin transport.
"""
from __future__ import annotations

import asyncio
from dataclasses import dataclass, field
from typing import Any, Callable, Optional


@dataclass
class PaperBrokerStats:
    """Surfaced via ``broker.stats`` for testing and diagnostics."""

    trades_observed: int = 0
    orders_simulated: int = 0
    fills_emitted: int = 0
    error: Optional[str] = None


def _slippage_kwargs(model: str, **params: float) -> dict:
    """Translate a friendly model name + params dict into the args
    ``SimulatedExecutor.set_default_slippage`` expects."""
    allowed = {"none", "fixed_ticks", "fixed_bps", "volume_impact"}
    if model not in allowed:
        raise ValueError(
            f"slippage model {model!r} not in {sorted(allowed)}"
        )
    return {
        "model": model,
        "ticks": int(params.get("ticks", 0)),
        "tick_size": float(params.get("tick_size", 0.0)),
        "bps": float(params.get("bps", 0.0)),
        "impact_coeff": float(params.get("impact_coeff", 0.0)),
    }


_SIDE_BUY = "buy"
_SIDE_SELL = "sell"


def _normalize_side(raw: Any) -> str:
    s = (raw or "").lower()
    if s in (_SIDE_BUY, _SIDE_SELL):
        return s
    raise ValueError(f"unknown side {raw!r}; expected buy / sell")


def _normalize_order_type(raw: Any) -> str:
    t = (raw or "").lower()
    # SimulatedExecutor.submit_order accepts 'market' or 'limit'. Other
    # signal types are passed through to user post-routing callbacks
    # so the user can observe / log; only market and limit reach the
    # simulator in this build.
    return t


@dataclass
class PaperBroker:
    """Drive a strategy off a live market data source while routing
    orders to a simulator. Looks like a broker to the runner; routes
    nothing to a real exchange.

    Pass the same ``Registry`` your strategy uses. The broker creates
    a ``SimulatedExecutor``, attaches it via the runner's signal
    callback, and tees observed trades into the simulator's queue
    tracker. Use ``set_default_slippage`` / ``set_symbol_slippage``
    to dial fill realism; the defaults are no-slip (model ``none``).

    The broker is intentionally headless — it does not own a market
    data source. The user wires the source (ccxt, replay, synthetic)
    and calls ``broker.observe_trade(symbol_id, price, qty, is_buy,
    ts_ns)`` from inside their feed loop. This keeps the abstraction
    independent of any one feed library.

    ``on_signal`` is an optional user callback that fires *after* the
    simulator has accepted a signal — useful for logging or wiring up
    your own paper-mode dashboards.
    """

    registry: Any
    on_signal: Optional[Callable[[Any], None]] = None

    # Configurable slippage; default = no slip.
    default_slippage_model: str = "none"
    default_slippage_params: dict = field(default_factory=dict)

    # Set after __post_init__.
    _runner: Any = field(default=None, init=False, repr=False)
    _sim: Any = field(default=None, init=False, repr=False)
    _next_order_id: int = field(default=1, init=False, repr=False)
    stats: PaperBrokerStats = field(default_factory=PaperBrokerStats, init=False)

    def __post_init__(self) -> None:
        import lrvx

        self._sim = lrvx.SimulatedExecutor()
        self._sim.set_symbol_registry(self.registry)
        self._sim.set_default_slippage(
            **_slippage_kwargs(
                self.default_slippage_model,
                **self.default_slippage_params,
            )
        )
        # Internal signal callback routes to the simulator first, then
        # optionally fans out to the user's callback for observability.
        self._runner = lrvx.Runner(
            self.registry, on_signal=self._route_signal
        )

    def _route_signal(self, sig: Any) -> None:
        """Default runner.on_signal: send orders to the simulator,
        then call the user's on_signal callback if set."""
        try:
            order_type = _normalize_order_type(getattr(sig, "order_type", ""))
            if order_type in ("market", "limit"):
                side = _normalize_side(getattr(sig, "side", ""))
                qty = float(getattr(sig, "quantity", 0.0))
                price = float(getattr(sig, "price", 0.0))
                sym = int(getattr(sig, "symbol", 1))
                order_id = int(getattr(sig, "order_id", 0) or 0)
                if order_id == 0:
                    order_id = self._next_order_id
                    self._next_order_id += 1
                self._sim.submit_order(
                    order_id, side, price, qty,
                    type=order_type, symbol=sym,
                )
                self.stats.orders_simulated += 1
            elif order_type == "cancel":
                order_id = int(getattr(sig, "order_id", 0) or 0)
                if order_id:
                    self._sim.cancel_order(order_id)
            elif order_type == "cancel_all":
                sym = int(getattr(sig, "symbol", 0))
                self._sim.cancel_all(sym)
            # Other order types (stop/take-profit/trailing) aren't
            # handled by SimulatedExecutor's submit_order surface.
            # They fall through to the user callback so they can be
            # logged / approximated externally.
        except Exception as exc:  # surface to stats; do not crash runner
            self.stats.error = f"signal-route failed: {type(exc).__name__}: {exc}"
        if self.on_signal is not None:
            self.on_signal(sig)

    @property
    def runner(self) -> Any:
        return self._runner

    @property
    def simulated_executor(self) -> Any:
        return self._sim

    # ── Lifecycle ─────────────────────────────────────────────────────

    def start(self) -> None:
        self._runner.start()

    def stop(self) -> None:
        self._runner.stop()

    # ── Slippage tuning ───────────────────────────────────────────────

    def set_default_slippage(self, model: str, **params: float) -> None:
        self._sim.set_default_slippage(**_slippage_kwargs(model, **params))

    def set_symbol_slippage(
        self, symbol_id: int, model: str, **params: float
    ) -> None:
        self._sim.set_symbol_slippage(
            int(symbol_id),
            **_slippage_kwargs(model, **params),
        )

    # ── Market data tee ──────────────────────────────────────────────

    def observe_trade(
        self,
        symbol_id: int,
        price: float,
        qty: float,
        is_buy: bool,
        ts_ns: int = 0,
    ) -> None:
        """Forward a live trade into both the runner (so strategies
        see it) and the simulator (so its queue tracker stays
        current). Wire this into your feed callback."""
        self._sim.advance_clock(int(ts_ns))
        self._sim.on_trade_qty(int(symbol_id), float(price), float(qty), bool(is_buy))
        self._runner.on_trade(int(symbol_id), float(price), float(qty), bool(is_buy), int(ts_ns))
        self.stats.trades_observed += 1

    def observe_book_snapshot(
        self,
        symbol_id: int,
        bid_levels,
        ask_levels,
        ts_ns: int = 0,
    ) -> None:
        """Forward a book snapshot. ``bid_levels`` / ``ask_levels``
        are sequences of ``(price, qty)`` tuples."""
        self._sim.advance_clock(int(ts_ns))
        self._sim.on_book_snapshot(int(symbol_id), bid_levels, ask_levels)
        # Runner takes parallel arrays; unpack.
        bp = [float(p) for p, _ in bid_levels]
        bq = [float(q) for _, q in bid_levels]
        ap = [float(p) for p, _ in ask_levels]
        aq = [float(q) for _, q in ask_levels]
        self._runner.on_book_snapshot(int(symbol_id), bp, bq, ap, aq, int(ts_ns))

    # ── Fill access ──────────────────────────────────────────────────

    def fills(self):
        """Return all simulated fills as a numpy structured array."""
        return self._sim.fills()

    def fills_list(self):
        """Return all simulated fills as a list of dicts."""
        return self._sim.fills_list()


__all__ = [
    "PaperBroker",
    "PaperBrokerStats",
]
