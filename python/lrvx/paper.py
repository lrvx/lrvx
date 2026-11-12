"""Paper / sandbox trading mode — drive a strategy against a live
market data feed with orders filled by the backtest's simulator instead
of a real exchange.

Paper mode is a backtest run fed live. The broker owns an
``lrvx.BacktestRunner`` and opens a live session on it: each trade or
book snapshot you observe goes where a tick replay (``run_tape``) sends
it — the runner's clock, the built-in ``SimulatedExecutor``, then the
strategy. So paper mode has the backtest's semantics, not an
approximation of them:

* every order type the simulator supports is routed (market, limit,
  stop / take-profit market and limit, trailing stops, modify, cancel,
  cancel-all, OCO), through the same pre-trade gate;
* order events reach the strategy through the same callbacks with the
  same fields and in the same order (``on_order_update``, ``on_fill``,
  ``on_queue_position_change``, ``on_market_position_change``), after
  the runner's position tracker has seen them, so ``ctx.position``
  inside ``on_fill`` is the position after the fill, as in a backtest;
* the registry's ticks are applied: an order priced off a declared tick
  is rejected with ``price_off_tick`` / ``trigger_off_tick`` /
  ``trailing_offset_off_tick``.

Wiring::

    your feed ──► broker.observe_trade ──► runner clock ─► SimulatedExecutor ─► strategy
                                                              │
    strategy ──► signal ──► BacktestRunner.onSignal ──► SimulatedExecutor
                                                              │
                       position tracker ◄── order events ◄────┘
                              │
                              └─► strategy.on_order_update / on_fill
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Callable, Optional


@dataclass
class PaperBrokerStats:
    """Surfaced via ``broker.stats`` for testing and diagnostics."""

    trades_observed: int = 0
    orders_simulated: int = 0
    fills_emitted: int = 0
    error: Optional[str] = None


_SLIPPAGE_MODELS = {"none", "fixed_ticks", "fixed_bps", "volume_impact"}

# Signal types that put an order into the simulator (as opposed to
# cancel / modify / liquidity, which act on one).
_ORDER_SIGNALS = {
    "market", "limit", "stop_market", "stop_limit", "take_profit_market",
    "take_profit_limit", "trailing_stop", "oco",
}


def _slippage_kwargs(model: str, **params: float) -> dict:
    """Translate a friendly model name + params dict into the
    ``lrvx.BacktestRunner`` slippage keywords."""
    if model not in _SLIPPAGE_MODELS:
        raise ValueError(
            f"slippage model {model!r} not in {sorted(_SLIPPAGE_MODELS)}"
        )
    return {
        "slippage_model": model,
        "slippage_ticks": int(params.get("ticks", 0)),
        "slippage_tick_size": float(params.get("tick_size", 0.0)),
        "slippage_bps": float(params.get("bps", 0.0)),
        "slippage_impact_coeff": float(params.get("impact_coeff", 0.0)),
    }


class _StrategySlot:
    """``broker.runner``: where the strategy is attached.

    A paper session runs one strategy, as a backtest does: the runner's
    position tracker and order ids belong to it.
    """

    def __init__(self, broker: "PaperBroker") -> None:
        self._broker = broker

    def add_strategy(self, strategy: Any) -> None:
        self._broker.set_strategy(strategy)


@dataclass
class PaperBroker:
    """Drive a strategy off a live market data source with orders filled
    by the backtest's simulator. Routes nothing to a real exchange.

    Pass the same ``SymbolRegistry`` your strategy uses; its ticks are
    applied to order prices as in a backtest. Attach the strategy with
    ``broker.set_strategy(s)`` (or ``broker.runner.add_strategy(s)``),
    call ``start()``, feed market data with ``observe_trade`` /
    ``observe_book_snapshot`` from your feed loop, and ``stop()``.

    The broker does not own a market data source: wire ccxt, a replay or
    a synthetic feed yourself. ``on_signal`` is an optional callback that
    receives each ``lrvx.Signal`` after the simulator has taken it.

    ``default_slippage_model`` / ``default_slippage_params`` and
    ``runner_options`` (any other ``lrvx.BacktestRunner`` keyword, e.g.
    ``submit_ack_latency_ns``) configure the simulator before
    ``start()``; ``set_symbol_slippage`` works at any time.
    """

    registry: Any
    on_signal: Optional[Callable[[Any], None]] = None

    # Configurable slippage; default = no slip.
    default_slippage_model: str = "none"
    default_slippage_params: dict = field(default_factory=dict)
    fee_rate: float = 0.0
    initial_capital: float = 100_000.0
    runner_options: dict = field(default_factory=dict)

    _runner: Any = field(default=None, init=False, repr=False)
    _strategy: Any = field(default=None, init=False, repr=False)
    _symbol_slippage: list = field(default_factory=list, init=False, repr=False)
    _started: bool = field(default=False, init=False, repr=False)
    _result: Optional[dict] = field(default=None, init=False, repr=False)
    stats: PaperBrokerStats = field(default_factory=PaperBrokerStats, init=False)

    def __post_init__(self) -> None:
        # Validated here so a bad model fails at construction.
        _slippage_kwargs(self.default_slippage_model, **self.default_slippage_params)

    # ── Strategy ──────────────────────────────────────────────────────

    @property
    def runner(self) -> _StrategySlot:
        return _StrategySlot(self)

    def set_strategy(self, strategy: Any) -> None:
        if self._started:
            raise RuntimeError("PaperBroker.set_strategy: the session has started")
        if self._strategy is not None and self._strategy is not strategy:
            raise RuntimeError(
                "PaperBroker runs one strategy, as a backtest does; run a "
                "second broker for a second strategy"
            )
        self._strategy = strategy

    @property
    def backtest_runner(self) -> Any:
        """The ``lrvx.BacktestRunner`` the session runs on (after start())."""
        return self._runner

    # ── Lifecycle ─────────────────────────────────────────────────────

    def start(self) -> None:
        import lrvx

        if self._started:
            raise RuntimeError("PaperBroker.start: already started")
        if self._strategy is None:
            raise RuntimeError("PaperBroker.start: attach a strategy first")
        kwargs = dict(self.runner_options)
        kwargs.update(
            _slippage_kwargs(self.default_slippage_model, **self.default_slippage_params)
        )
        self._runner = lrvx.BacktestRunner(
            self.registry, self.fee_rate, self.initial_capital, **kwargs
        )
        for args in self._symbol_slippage:
            self._runner.set_symbol_slippage(*args)
        self._runner.set_strategy(self._strategy)
        self._runner.begin_session(self._route_signal)
        self._started = True

    def stop(self) -> Optional[dict]:
        """End the session; returns its stats dict (``run_tape``'s shape)."""
        if not self._started or not self._runner.session_open:
            return self._result
        self._result = self._runner.end_session()
        self.stats.fills_emitted = len(self._runner.session_fills_list())
        return self._result

    def _route_signal(self, sig: Any) -> None:
        # The runner has already routed the signal; this only counts it and
        # hands it to the user's callback.
        if getattr(sig, "order_type", "") in _ORDER_SIGNALS:
            self.stats.orders_simulated += 1
        if self.on_signal is not None:
            try:
                self.on_signal(sig)
            except Exception as exc:  # the session must not die in user code
                self.stats.error = f"on_signal raised {type(exc).__name__}: {exc}"

    # ── Slippage tuning ───────────────────────────────────────────────

    def set_default_slippage(self, model: str, **params: float) -> None:
        if self._started:
            raise RuntimeError(
                "PaperBroker.set_default_slippage: set it before start(); "
                "set_symbol_slippage works during the session"
            )
        _slippage_kwargs(model, **params)
        self.default_slippage_model = model
        self.default_slippage_params = dict(params)

    def set_symbol_slippage(self, symbol_id: int, model: str, **params: float) -> None:
        kw = _slippage_kwargs(model, **params)
        args = (int(symbol_id), model, kw["slippage_ticks"], kw["slippage_tick_size"],
                kw["slippage_bps"], kw["slippage_impact_coeff"])
        if self._started:
            self._runner.set_symbol_slippage(*args)
        else:
            self._symbol_slippage.append(args)

    # ── Market data ───────────────────────────────────────────────────

    def _require_session(self, method: str) -> None:
        if not self._started or not self._runner.session_open:
            raise RuntimeError(f"PaperBroker.{method}: call start() first")

    def observe_trade(
        self,
        symbol_id: int,
        price: float,
        qty: float,
        is_buy: bool,
        ts_ns: int = 0,
    ) -> None:
        """Forward a live trade: the simulator matches resting orders
        against it, then the strategy sees it. Wire this into your feed."""
        self._require_session("observe_trade")
        self._runner.session_trade(
            int(symbol_id), float(price), float(qty), bool(is_buy), int(ts_ns)
        )
        self.stats.trades_observed += 1

    def observe_book_snapshot(
        self,
        symbol_id: int,
        bid_levels,
        ask_levels,
        ts_ns: int = 0,
    ) -> None:
        """Forward a book snapshot. ``bid_levels`` / ``ask_levels`` are
        sequences of ``(price, qty)`` tuples, best first."""
        self._require_session("observe_book_snapshot")
        self._runner.session_book_snapshot(
            int(symbol_id),
            [(float(p), float(q)) for p, q in bid_levels],
            [(float(p), float(q)) for p, q in ask_levels],
            int(ts_ns),
        )

    # ── Fill access ──────────────────────────────────────────────────

    def fills(self):
        """Return all simulated fills as a numpy structured array."""
        if self._runner is None:
            raise RuntimeError("PaperBroker.fills: no session has started")
        return self._runner.session_fills()

    def fills_list(self):
        """Return all simulated fills as a list of dicts."""
        if self._runner is None:
            return []
        return self._runner.session_fills_list()


__all__ = [
    "PaperBroker",
    "PaperBrokerStats",
]
