# Python Bindings

## Strategy API

Event-driven live trading and backtesting using `Strategy`, `Runner`, and `BacktestRunner`.

### Build

```bash
cmake -B build \
  -DLRVX_BUILD_PYTHON=ON \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build
```

Requires: Python 3.10+, pybind11 3.1 or newer (`pip install "pybind11>=3.1"`). The
prebuilt wheels on PyPI cover CPython 3.10 to 3.15.

The module builds at `build/python/lrvx.cpython-*.so`.

### Symbols

```python
import lrvx

registry = lrvx.SymbolRegistry()
btc = registry.add_symbol("binance", "BTCUSDT", tick_size=0.01)
# btc.id, btc.name, btc.exchange, btc.tick_size
# int(btc) → 1 — works as int everywhere
```

### Writing a Strategy

```python
class SMAcross(lrvx.Strategy):
    def __init__(self, symbols):
        super().__init__(symbols)
        self.fast = lrvx.SMA(10)
        self.slow = lrvx.SMA(30)

    def on_start(self): ...
    def on_stop(self): ...

    def on_trade(self, ctx, trade):
        f = self.fast.update(trade.price)
        s = self.slow.update(trade.price)
        if f is None or s is None:
            return
        if f > s and ctx.is_flat():
            self.market_buy(0.01)
        elif f < s and ctx.is_long():
            self.close_position()

    def on_book_update(self, ctx): ...
```

Order methods: `market_buy(qty)`, `market_sell(qty)`, `limit_buy(price, qty)`, `limit_sell(price, qty)`, `stop_market(side, trigger, qty)`, `close_position()`. All accept an optional `symbol` argument; without it the first registered symbol is used.

Where an API takes an order type as a string -- `SimulatedExecutor.submit_order`, `VenueExecutor.submit_order`, the bracket legs -- the accepted names are `lrvx.ORDER_TYPE_NAMES`: `limit`, `market`, `stop_market`, `stop_limit`, `tp_market`, `tp_limit`, `trailing_stop`, `iceberg`. `order_type_name(code)` and `order_type_code(name)` convert between a name and its wire code. Anything outside the set raises `ValueError` naming the accepted names, so a typo cannot become a market order.

### Live Runner

```python
def on_signal(sig):
    # sig.side, sig.order_type, sig.quantity, sig.price, sig.order_id
    send_to_exchange(sig)

runner = lrvx.Runner(registry, on_signal)                  # synchronous
runner = lrvx.Runner(registry, on_signal, threaded=True)   # Disruptor background thread

runner.add_strategy(SMAcross([btc]))
runner.start()

# Inject market data (from your feed):
runner.on_trade(btc, price, qty, is_buy, ts_ns)
runner.on_book_snapshot(btc, bid_prices, bid_qtys, ask_prices, ask_qtys, ts_ns)

runner.stop()
```

### Backtest

```python
bt = lrvx.BacktestRunner(registry, fee_rate=0.0004, initial_capital=10_000)
bt.set_strategy(SMAcross([btc]))

stats = bt.run_csv("btcusdt_1m.csv", "BTCUSDT")
print(stats["return_pct"], stats["sharpe_ratio"], stats["max_drawdown_pct"])
```

The constructor takes `(registry, fee_rate=0.0004, initial_capital=100000.0)` — nothing else. Flat fee rate, no funding, no liquidation, no rate limits, no queue position. Useful for an indicator sanity check; not enough for a decision about real capital.

`run_csv` reads OHLCV **bars**: one header line, then `timestamp,open,high,low,close,volume`. Only the timestamp and close columns are used; each bar is replayed as one trade, so `on_trade` fires and `on_bar` does not. Timestamps below `1e12` are treated as seconds and scaled to nanoseconds, because a raw CSV column's unit is genuinely unknown. `run_ohlcv(ts, close, symbol)` and `run_bars(...)` (full OHLC, fires `on_bar`) are both different: `ts` and `start_time_ns` / `end_time_ns` are nanoseconds by contract — the same contract the C ABI, Node, and Codon bindings use for the same calls — so they are used exactly as given, with no unit guessing at all. Other entry points: `run_tape(path)` and `run_tapes(paths)` replay `.lrvx` tapes tick by tick (`on_trade`), `run_tape_bars(path, interval_seconds=..., symbol=...)` replays a tape as time bars (`on_bar`, `run_bars`' fill rules), and `run_tape_bars_on_ticks(path, interval_seconds=..., symbol=...)` shows the strategy the same bars while the executor fills on the tape's prints.

Returns a plain dict. Keys: `total_trades`, `winning_trades`, `losing_trades`, `initial_capital`, `final_capital`, `total_pnl`, `total_fees`, `net_pnl`, `gross_profit`, `gross_loss`, `max_drawdown`, `max_drawdown_pct`, `win_rate`, `profit_factor`, `sharpe_ratio`, `sortino_ratio`, `calmar_ratio`, `return_pct`, `unrealized_pnl`, `final_equity`. A position still open when the data ends is valued at the last price the run replayed for its symbol (a bar's close on bar replay, the last trade on tick replay; book records do not move it): `unrealized_pnl` is that valuation, with no exit fee, and `final_equity` is `initial_capital + net_pnl + unrealized_pnl`. `net_pnl`, `final_capital` and `return_pct` count closed trades only, and `open_positions()` lists what was left open. `Engine.run` returns a `Stats` object carrying the same two fields.

Hooks attach after construction: `set_executor(executor)` (a subclass of `lrvx.Executor`), `set_risk_manager`, `set_kill_switch`, `set_order_validator`, `set_pnl_tracker`, `add_execution_listener`.

#### Venue physics

`VenueStack` is a separate, self-contained venue simulation, built from one factory call:

```python
stack = lrvx.VenueStack.binance_um_futures(account_id=42, equity=10_000.0)

acct = stack.account()
liq = stack.liquidation()
fees = stack.fees()
funding = stack.funding()
exec_ = stack.executor()
```

One call wires the cross-margin account, MM tiers and ADL, the VIP fee schedule (bound to the account, so realized notional moves the tier), funding settlement on the venue's interval, rate limits, and a venue-availability hook. Other factories: `bybit_linear`, `okx_swap`, `deribit`, plus `VenueStack.from_venue(name, account_id, equity)`. Non-canonical venues go through [`lrvx.assemble_custom_venue(...)`](../how-to/realistic-backtest.md#fully-custom-venue).

Two ways to use a `VenueStack`:

- **Standalone.** Drive the returned subsystems directly — `acct.open_position(...)`, `liq.on_marks(...)`, `fees.record_fill(...)`, `funding.tick(...)`, `exec_.submit_order(...)`. See `docs/examples/python_realistic_backtest.py`.
- **Attached to `BacktestRunner`.** Call `runner.set_venue_stack(stack)` before `run_csv`/`run_bars`/`run_tape(s)`. The runner then feeds the stack's executor market data and harvests its fills, so the run picks up the venue's fill mechanics — queue model and depth, iceberg refresh latency, venue availability, and rate limits — instead of the flat `fee_rate` fill model. Fees come from the stack's `FeeSchedule` too: each fill is billed at the tier the account's 30-day notional resolves to, with the fill's maker/taker flag picking the side. Funding and liquidation stay driven by explicit calls, not the replay loop. Pass `None` to revert to the built-in executor. Passing a bare `VenueExecutor` to `set_executor` is refused with a pointer back here, because the executor alone does not carry the clock the runner has to advance. Full walkthrough: [Realistic backtest in one call](../how-to/realistic-backtest.md#attach-a-venuestack-to-backtestrunner).

Full pattern and pieces: [Realistic backtest in one call](../how-to/realistic-backtest.md), [Cross-margin accounts](../how-to/cross-margin.md), [Liquidation and ADL](../how-to/liquidation-and-adl.md).

### Paper trading

Same strategy class, live feed, simulated fills. `PaperBroker` lives in `lrvx.paper`. It runs a `BacktestRunner` session fed live: you attach the strategy with `broker.set_strategy(...)` (or `broker.runner.add_strategy(...)`) and push market data through `broker.observe_trade(...)`, which goes where a tick replay sends it, so orders, order events and positions behave as in `run_tape`.

```python
from lrvx.paper import PaperBroker

broker = PaperBroker(registry=registry)
broker.runner.add_strategy(SMAcross([btc]))
broker.start()

# Feed live trades from your data source (websocket, ccxt.pro, etc.)
broker.observe_trade(btc, price, qty, is_buy, ts_ns=ts_ns)

broker.stop()
print(broker.fills_list(), broker.stats)
```

`PaperBroker` is a dataclass; `on_signal=` is an optional user callback that fires *after* the runner has routed a signal, not a routing method. Slippage is tunable via `set_default_slippage(model, **params)` before `start()` and `set_symbol_slippage(symbol_id, model, **params)` at any time.

See [Paper trading](../how-to/paper-trading.md) for the full feed-wiring pattern.

### Live

`CcxtBroker` lives in `lrvx.ccxt` and routes orders through a [ccxt.pro](https://github.com/ccxt/ccxt) exchange. It is async and owns its own registry and runner — construct it from an exchange *id*, not an exchange object. The strategy class is unchanged.

```python
import asyncio
from lrvx.ccxt import CcxtBroker

async def main():
    broker = CcxtBroker("binance", api_key=..., secret=..., sandbox=True)
    async with broker:
        btc = await broker.add_symbol("BTC/USDT")
        broker.add_strategy(SMAcross([btc]))
        await broker.run(streams=("trades", "orders"))

asyncio.run(main())
```

One strategy class runs backtest, paper, and live. See [Connect lrvx to a CCXT exchange](../how-to/ccxt-adapter.md) and `docs/examples/python_ccxt_live.py`.

### When a callback raises

Every Python callable the engine calls back into — the ten `Strategy` callbacks, the extension hooks (`PnLTracker`, `StorageSink`, `Executor`, `ExecutionListener`, `MarketDataRecorderHook`), and the three pre-trade gates — runs inside a C call. An exception is never allowed to unwind out of there: out of a frame with C linkage that is undefined behaviour, and on `threaded=True` it kills the bus consumer delivering the event, so the strategy would stop receiving events for the rest of the run. One rule applies to all of them:

1. **The exception stops at the bridge.** The engine carries on with the next event, and the call that drove it — `Runner.on_trade`, `BacktestRunner.run_bars`, `runner.start()` — returns normally.
2. **A pre-trade gate that did not answer denies.** `RiskManager.allow`, `KillSwitch.check` and `OrderValidator.validate` drop the signal when the callable raises, or returns something that is not a bool. That is the C ABI's own rule — "a call that was stopped this way returns its failure value" (`include/lrvx/capi/lrvx_capi.h`) — read against what a gate's return values mean: "Returning 0 (deny) drops the signal entirely" (`include/lrvx/capi/lrvx_capi_spec.hpp`). A broken gate is a closed gate, never an open one.
3. **The description survives.** The exception text, with type and traceback, goes to the engine log at error level. `lrvx.set_log_callback(fn)` is how it reaches Python; with no callback installed the bundled console logger writes it to stderr.

```python
def sink(level, msg):      # level: 0=info, 1=warn, 2=error
    print(msg)

lrvx.set_log_callback(sink)

class Gate(lrvx.RiskManager):
    def allow(self, sig):
        raise RuntimeError("limits service is down")

runner.set_risk_manager(Gate())
runner.on_trade(btc, 100.0, 1.0, True, ts_ns)
# on_signal never fires, and sink() receives
# "lrvx: RiskManager.allow raised: RuntimeError: limits service is down"
```

So a callback that raises costs you that event, not the run. If you want anything else to happen — retry, halt, alert — catch it in your own callback and decide there.

The log sink is detached at interpreter shutdown, so leaving one installed is safe.

---

## Vectorised Engine

`lrvx.Engine` runs a backtest against a pre-built signal list instead of an event-driven `Strategy`. Bars are loaded once and can be re-run against many signal sets.

### Quick Start

```python
import lrvx

engine = lrvx.Engine(initial_capital=100_000, fee_rate=0.0001)
engine.load_csv("btcusdt_1m.csv", symbol="BTCUSDT")

signals = lrvx.SignalBuilder()
signals.buy(1704067200_000000000, 0.5, symbol="BTCUSDT")
signals.sell(1704068400_000000000, 0.5, symbol="BTCUSDT")

stats = engine.run(signals)
print(f"PnL: {stats.net_pnl:.2f}, Sharpe: {stats.sharpe_ratio:.4f}")
```

### Loading Bar Data

| Method | Input |
|--------|-------|
| `load_csv(path, symbol="")` | OHLCV CSV; symbol inferred from the filename when omitted |
| `load_ohlcv(data, symbol="")` | dict with `ts` (or `timestamp`) plus `open`, `high`, `low`, `close`, `volume` arrays |
| `load_df(df, symbol="")` | pandas DataFrame; timestamp taken from `ts`/`timestamp`/`open_time`/`time`, else the index |

Call any of them once per symbol. `resample(symbol, target, interval)` derives a coarser series from a loaded one. Read-back accessors: `symbols`, `bar_count(symbol="")`, `ts`, `open`, `high`, `low`, `close`, `volume`.

### Creating Signals

`lrvx.SignalBuilder` accumulates signals in call order:

| Method | Arguments |
|--------|-----------|
| `buy` | `ts`, `qty`, `symbol=""` |
| `sell` | `ts`, `qty`, `symbol=""` |
| `limit_buy` | `ts`, `price`, `qty`, `symbol=""` |
| `limit_sell` | `ts`, `price`, `qty`, `symbol=""` |

`len(builder)` gives the count; `clear()` resets it. An empty `symbol` falls back to `run`'s `default_symbol`.

### Run

```python
stats = engine.run(signals, default_symbol=0)
```

Returns a `lrvx.Stats` object. Fields are readable as attributes or via `stats["key"]`; `stats.to_dict()` returns the whole set.

| Key | Description |
|-----|-------------|
| `total_trades` | Round-trip trade count |
| `net_pnl` | Gross PnL minus all fees |
| `total_fees` | Total execution fees |
| `sharpe_ratio` | Annualized Sharpe ratio |
| `sortino_ratio` | Annualized Sortino ratio |
| `calmar_ratio` | Calmar ratio |
| `max_drawdown` | Peak-to-trough drawdown |
| `max_drawdown_pct` | Drawdown as percentage |
| `win_rate` | Winning trade fraction |
| `profit_factor` | Gross profit / gross loss |
| `return_pct` | Net return percentage |

`Engine` exposes a single `run`; there is no batch or multi-threaded entry point. For parameter sweeps see [Grid Search](../how-to/grid-search.md).

## See Also

- [Python API Reference](../reference/python/index.md) — complete Python API documentation
- [Realistic backtest in one call](../how-to/realistic-backtest.md) — venue stack
- [Cross-margin accounts](../how-to/cross-margin.md) — shared equity across positions
- [Paper trading](../how-to/paper-trading.md) — same strategy class against a live feed
- [Connect lrvx to a CCXT exchange](../how-to/ccxt-adapter.md) — promote to live
- [Inspect a tape and run in the replay viewer](../how-to/replay-viewer.md)
- [Control engine over MCP](../how-to/mcp-control-plane.md) — scoped AI control
- [Grid Search](../how-to/grid-search.md) — parameter optimization
