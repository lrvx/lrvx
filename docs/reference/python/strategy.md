# Strategy API

Event-driven strategy classes for Python. Mirrors C++ `lrvx::Strategy`.

## Class: `lrvx.Symbol`

Returned by `SymbolRegistry.add_symbol`. Works transparently as an `int` wherever a symbol ID is expected.

| Property | Type | Description |
|----------|------|-------------|
| `id` | `int` | Numeric symbol ID |
| `name` | `str` | Symbol name, e.g. `"BTCUSDT"` |
| `exchange` | `str` | Exchange name, e.g. `"binance"` |
| `tick_size` | `float` | Minimum price increment |

```python
int(btc)   # 1
print(btc) # Symbol(binance:BTCUSDT, id=1)
```

## Class: `lrvx.SymbolRegistry`

```python
registry = lrvx.SymbolRegistry()
btc = registry.add_symbol("binance", "BTCUSDT", tick_size=0.01)
```

| Method | Returns | Description |
|--------|---------|-------------|
| `add_symbol(exchange, symbol, tick_size=0.01)` | `Symbol` | Register a symbol and return its `Symbol` object |
| `symbol_count()` | `int` | Number of registered symbols |

## Class: `lrvx.Strategy`

### Constructor

```python
Strategy(symbols: list[Symbol | int])
```

`symbols` — list of `Symbol` objects or raw integer IDs to subscribe to.

### Overridable Callbacks

#### `on_trade(ctx: SymbolContext, trade: TradeData)`

Called on each trade event for subscribed symbols.

```python
def on_trade(self, ctx, trade):
    if ctx.is_flat():
        self.market_buy(0.01)
```

#### `on_book_update(ctx: SymbolContext)`

Called on each order book update.

#### `on_bar(ctx: SymbolContext, bar: BarData)`

Called on each closed OHLC bar. `bar` exposes `open`, `high`, `low`, `close`,
`volume`, `buy_volume`, `start_time_ns`, `end_time_ns`, `bar_type`,
`bar_type_param`, `close_reason`. Use `BacktestRunner.run_bars(...)` to replay
historical bars or `Runner.on_bar(...)` to push live bars.

```python
def on_bar(self, ctx, bar):
    # detect breakout on bar close
    if bar.close > self.prev_high and ctx.is_flat():
        self.market_buy(0.01)
    self.prev_high = max(getattr(self, "prev_high", 0.0), bar.high)
```

#### `on_fill(ctx: SymbolContext, event: OrderEventData)`

Called on every fill this strategy's own orders produce (status
`PARTIALLY_FILLED` or `FILLED`).

#### `on_order_update(ctx: SymbolContext, event: OrderEventData)`

Called on every order-lifecycle status change: `NEW`, `ACCEPTED`, `CANCELED`,
`REJECTED`, `REPLACED`, `TRIGGERED`, `TRAILING_UPDATED`. Fills fire here too —
use `on_fill` if you only care about those.

#### `on_queue_position_change(ctx: SymbolContext, event: OrderEventData)`

Called when a resting limit order's queue position moved with no other
lifecycle transition. `event.queue_ahead` and `event.queue_total` carry the
snapshot. Backtest only.

#### `on_market_position_change(ctx: SymbolContext, event: OrderEventData)`

Called when a resting limit order's categorical market position transitioned.
Backtest only.

#### `on_start()` / `on_stop()`

Lifecycle callbacks.

### Order Emission — Shorthand

These methods use the first registered symbol when `symbol` is omitted. `symbol` is a symbol name.

| Method | Description |
|--------|-------------|
| `market_buy(qty, symbol=None)` | Market buy |
| `market_sell(qty, symbol=None)` | Market sell |
| `limit_buy(price, qty, symbol=None, tif='gtc')` | Limit buy |
| `limit_sell(price, qty, symbol=None, tif='gtc')` | Limit sell |
| `stop_market(side, trigger, qty, symbol=None)` | Stop market |
| `stop_limit(side, trigger, limit_price, qty, symbol=None)` | Stop limit |
| `take_profit_market(side, trigger, qty, symbol=None)` | Take profit market |
| `take_profit_limit(side, trigger, limit_price, qty, symbol=None)` | Take profit limit |
| `trailing_stop(side, offset, qty, symbol=None)` | Trailing stop, fixed offset |
| `trailing_stop_percent(side, callback_bps, qty, symbol=None)` | Trailing stop, bps callback |
| `close_position(symbol=None)` | Close position (reduce-only) |
| `cancel_order(order_id)` | Cancel an order |
| `cancel_all_orders(symbol=None)` | Cancel every order for a symbol |
| `modify_order(order_id, new_price, new_qty)` | Modify a resting order |

`tif` accepts `"gtc"`, `"ioc"`, `"fok"`, `"gtd"`, `"post_only"`.

### Order Emission — Explicit (emit_* variants)

| Method | Returns | Description |
|--------|---------|-------------|
| `emit_market_buy(symbol, qty)` | `int` | Market buy, returns order ID |
| `emit_market_sell(symbol, qty)` | `int` | Market sell |
| `emit_limit_buy(symbol, price, qty)` | `int` | Limit buy |
| `emit_limit_sell(symbol, price, quantity)` | `int` | Limit sell |
| `emit_limit_buy_tif(symbol, price, quantity, tif='gtc')` | `int` | Limit buy with an explicit time in force |
| `emit_limit_sell_tif(symbol, price, quantity, tif='gtc')` | `int` | Limit sell with an explicit time in force |
| `emit_cancel(order_id)` | `None` | Cancel order |
| `emit_cancel_all(symbol)` | `None` | Cancel all orders for symbol |
| `emit_modify(order_id, new_price, new_quantity)` | `None` | Modify existing order |
| `emit_stop_market(symbol, side, trigger, quantity)` | `int` | Stop market order |
| `emit_stop_limit(symbol, side, trigger, limit_price, quantity)` | `int` | Stop limit order |
| `emit_take_profit_market(symbol, side, trigger, quantity)` | `int` | Take profit market |
| `emit_take_profit_limit(symbol, side, trigger, limit_price, quantity)` | `int` | Take profit limit |
| `emit_trailing_stop(symbol, side, offset, quantity)` | `int` | Trailing stop, fixed offset |
| `emit_trailing_stop_percent(symbol, side, callback_bps, quantity)` | `int` | Trailing stop, bps callback |
| `emit_provide_liquidity(pool, price_lower, price_upper, liquidity)` | `int` | Provide AMM liquidity in a price range |
| `emit_withdraw_liquidity(pool, liquidity)` | `int` | Withdraw AMM liquidity |
| `emit_close_position(symbol)` | `int` | Close position (reduce-only) |

`emit_market_buy` and the rest take a numeric symbol ID; `side` is `"buy"` or `"sell"`.

### Context Queries

`symbol` defaults to the first registered symbol when omitted.

| Method / Property | Returns | Description |
|-------------------|---------|-------------|
| `position(symbol=None)` | `float` | Current position quantity |
| `pos(symbol=None)` | `float` | Alias of `position` |
| `ctx(symbol=None)` | `SymbolContext` | Per-symbol context snapshot |
| `last_price(symbol=None)` | `float` | Last trade price |
| `best_bid(symbol=None)` | `float \| None` | Best bid; `None` when the bid side has no level. A book may be quoted at exactly zero, so `0.0` is a price, never "no quote" |
| `best_ask(symbol=None)` | `float \| None` | Best ask; `None` when the ask side has no level |
| `mid_price(symbol=None)` | `float \| None` | Mid price; `None` unless both sides have a level |
| `get_order_status(order_id)` | `int` | Order status (-1 if not found) |
| `order_status(order_id)` | `int` | Alias of `get_order_status` |
| `symbols` | `list[int]` | Subscribed symbol IDs (property) |
| `symbol_names` | `list[str]` | Subscribed symbol names (property) |
| `primary_symbol_name` | `str` | Name of the first subscribed symbol (property) |

### Closed-bar history

| Method | Returns | Description |
|--------|---------|-------------|
| `last_closed_bar(symbol_id, bar_type=0, param=0)` | `dict \| None` | Last closed bar for `(symbol, bar_type, param)`, or `None` if that timeframe has emitted nothing yet |
| `last_n_closed_bars(symbol_id, bar_type, param, n)` | `list` | Up to `n` most recent closed bars for that timeframe, oldest first |
| `bar_ring_capacity()` | `int` | Current per-timeframe ring size |
| `set_bar_ring_capacity(n)` | `None` | Resize the per-timeframe ring |

`bar_type`: 0=Time, 1=Tick, 2=Volume, 3=Renko, 4=Range, 5=HeikinAshi,
6=BpsRange. `param` is the time-bar interval in nanoseconds, or the tick /
volume / range threshold the aggregator was configured with.

## Class: `lrvx.SymbolContext`

| Property | Type | Description |
|----------|------|-------------|
| `symbol_id` | `int` | Symbol identifier |
| `symbol` | `str` | Symbol name |
| `position` | `float` | Current position |
| `last_trade_price` | `float` | Last trade price |
| `best_bid` | `float \| None` | Best bid price; `None` when the bid side has no level (`0.0` is a price) |
| `best_ask` | `float \| None` | Best ask price; `None` when the ask side has no level |
| `mid_price` | `float \| None` | Mid price; `None` unless both sides have a level |
| `unrealized_pnl` | `float` | Unrealized P&L, or `NaN` when the position manager reports no entry price (see below) |
| `last_update_ns` | `int` | Exchange timestamp (ns) of the last dispatched event: trade, depth frame or bar |
| `last_touch_ns` | `int` | Exchange timestamp (ns) of the frame that last wrote the touch. A `TOP_OF_BOOK` frame moves it without firing `on_book_update`, so it can be fresher than `last_update_ns` |
| `book_spread()` | `float \| None` | Bid-ask spread; `None` unless both sides have a level |
| `is_long()` | `bool` | True if long |
| `is_short()` | `bool` | True if short |
| `is_flat()` | `bool` | True if no position |

`unrealized_pnl` needs an average entry price, which comes from the position
manager attached to the strategy. A manager that keeps no cost basis reports
none, and the field is `NaN` rather than a number. Check it with
`math.isnan()` before comparing against a threshold; the two shipped managers,
`PositionTracker` and `MultiModePositionTracker`, always report one while a
position is open.

## Class: `lrvx.TradeData`

| Property | Type | Description |
|----------|------|-------------|
| `symbol` | `int` | Symbol ID |
| `symbol_name` | `str` | Symbol name |
| `price` | `float` | Trade price |
| `quantity` | `float` | Trade quantity |
| `is_buy` | `bool` | Buy-side aggressor |
| `side` | `str` | `"buy"` or `"sell"` |
| `timestamp_ns` | `int` | Local timestamp (nanoseconds) |
| `exchange_ts_ns` | `int` | Exchange timestamp (nanoseconds) |

## Class: `lrvx.Runner`

Feeds market data into strategies and routes emitted signals to a callback.

```python
runner = lrvx.Runner(registry, on_signal)                  # synchronous
runner = lrvx.Runner(registry, on_signal, threaded=True)   # Disruptor background thread

runner.add_strategy(strategy)
runner.start()
runner.on_trade(symbol, price, qty, is_buy, ts_ns)
runner.on_book_snapshot(symbol, bid_prices, bid_qtys, ask_prices, ask_qtys, ts_ns)
runner.on_bar(symbol, open, high, low, close, volume, ...)
runner.stop()
```

| Method | Description |
|--------|-------------|
| `add_strategy(strategy)` | Register a strategy instance |
| `replace_strategy(index, strategy)` | Atomically swap the strategy at `index`. The old strategy's `on_stop` fires, the bridge's internal state survives, the new strategy's `on_start` fires afterwards. WebSocket / gRPC connections are unaffected |
| `start()` | Start the runner |
| `stop()` | Stop the runner |
| `on_trade(symbol, price, qty, is_buy, ts_ns=0)` | Inject a trade event |
| `on_book_snapshot(symbol, bid_prices, bid_qtys, ask_prices, ask_qtys, ts_ns=0)` | Inject an order book snapshot |
| `on_bar(symbol, open, high, low, close, volume=0, buy_volume=0, start_time_ns=0, end_time_ns=0, bar_type=0, bar_type_param=0, close_reason=0)` | Inject a closed OHLC bar |

`symbol` accepts a `Symbol` object or a raw `int`.

### Hook setters

| Method | Description |
|--------|-------------|
| `set_pnl_tracker(tracker)` | Attach a `PnLTracker` |
| `set_storage_sink(sink)` | Attach a `StorageSink` |
| `set_risk_manager(rm)` | Attach a `RiskManager` |
| `set_kill_switch(ks)` | Attach a `KillSwitch` |
| `set_order_validator(ov)` | Attach an `OrderValidator` |
| `set_market_data_recorder(recorder)` | Attach a `MarketDataRecorderHook` or `BinaryLogRecorderHook` |
| `set_executor(executor)` | Replace the executor |

### Trace recording

| Method | Description |
|--------|-------------|
| `attach_trace_recorder(recorder)` | Auto-capture every signal into a `.lrvxrun` recorder |
| `set_trace_feed_ts_ns(feed_ts_ns)` | Stamp every recorded signal with this `feed_ts_ns` until the next call |
| `trace_order_event(order_id, parent_signal_id, symbol_id, event_kind, side, order_type, price, qty, flags=0)` | Mirror an order event into the attached recorder |
| `trace_fill(order_id, fill_id, price, qty, fee, symbol_id, side, liquidity=0)` | Mirror a fill into the attached recorder |

### Signal object

Passed to the `on_signal` callback.

| Property | Type | Description |
|----------|------|-------------|
| `order_id` | `int` | Internal order ID |
| `symbol` | `int` | Symbol ID |
| `side` | `str` | `"buy"` or `"sell"` |
| `order_type` | `str` | `"market"`, `"limit"`, etc. |
| `price` | `float` | Limit price (0 for market) |
| `quantity` | `float` | Order quantity |
| `trigger_price` | `float` | Trigger price for conditional orders |
| `trailing_offset` | `float` | Fixed-price trailing offset |
| `trailing_bps` | `int` | Bps trailing callback |
| `new_price` | `float` | Replacement price on a modify |
| `new_quantity` | `float` | Replacement quantity on a modify |

## Class: `lrvx.BacktestRunner`

Runs a strategy against historical CSV data.

`fee_rate` defaults to `0.0004` here, a taker rate. `lrvx.BacktestResult` and the C++
`BacktestConfig` default to `0.0001`, so pass the rate explicitly when a runner result is
compared with a result scored by hand.

```python
bt = lrvx.BacktestRunner(registry, fee_rate=0.0004, initial_capital=10_000)
bt.set_strategy(strategy)

stats = bt.run_csv("data.csv")             # auto-detects symbol from registry
stats = bt.run_csv("data.csv", "BTCUSDT")  # explicit symbol name
```

| Method | Description |
|--------|-------------|
| `set_strategy(strategy)` | Set the strategy to backtest |
| `run_csv(path, symbol='')` | Run backtest against a CSV file, returns stats dict |
| `run_ohlcv(ts, close, symbol='')` | Replay close-only bars as synthetic trades (`Strategy.on_trade` fires) |
| `run_bars(start_time_ns, end_time_ns, open, high, low, close, volume, symbol='', bar_type=0, bar_type_param=0)` | Replay full OHLCV bars (`Strategy.on_bar` fires) |
| `run_tape(path)` | Run against a `.lrvx` tape directory — the canonical recorded artifact `lrvx tape record` writes. Same stats shape as `run_csv` |
| `run_tapes(paths)` | Run against N `.lrvx` tapes merged on read. Symbols are rekeyed by `(metadata.exchange, name)`, so two captures of the same venue/symbol collapse and two venues stay distinct. `run_tapes([t])` equals `run_tape(t)` |
| `equity_curve()` | Equity curve from the most recent run, as a dict of numpy arrays (`timestamp_ns`, `equity`, `drawdown_pct`) |
| `trades()` | Closed trades from the most recent run, as a dict of numpy arrays (`symbol`, `side`, `entry_price`, `exit_price`, `quantity`, `pnl`, `fee`, `entry_time_ns`, `exit_time_ns`) |

An empty `symbol` resolves to `"default"`.

### Hook setters

| Method | Description |
|--------|-------------|
| `set_executor(executor)` | Replace the built-in simulated executor |
| `add_execution_listener(listener)` | Attach an `ExecutionListener` |
| `set_pnl_tracker(tracker)` | Attach a PnL tracker; fires `on_signal(signal)` for every fill the simulator dispatches |
| `set_risk_manager(rm)` | Attach a pre-trade risk manager. Reduce-only orders bypass the gate by design |
| `set_kill_switch(ks)` | Attach a kill switch. Reduce-only orders bypass, so tightening caps cannot strand a position |
| `set_order_validator(ov)` | Attach an order validator. Reduce-only orders bypass |

Each setter takes `None` to detach.

### Stats dict keys

| Key | Description |
|-----|-------------|
| `return_pct` | Net return percentage |
| `net_pnl` | Net P&L after fees |
| `total_trades` | Round-trip trade count |
| `win_rate` | Winning trade fraction |
| `sharpe_ratio` | Annualized Sharpe ratio |
| `max_drawdown_pct` | Peak-to-trough drawdown (%) |

## Example

```python
import lrvx

registry = lrvx.SymbolRegistry()
btc = registry.add_symbol("binance", "BTCUSDT", tick_size=0.01)

class SMAcross(lrvx.Strategy):
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
            self.market_buy(0.01)
        elif f < s and ctx.is_long():
            self.close_position()

# Live
def on_signal(sig):
    print(sig.side, sig.order_type, sig.quantity)

runner = lrvx.Runner(registry, on_signal)
runner.add_strategy(SMAcross([btc]))
runner.start()

# Backtest
bt = lrvx.BacktestRunner(registry, fee_rate=0.0004, initial_capital=10_000)
bt.set_strategy(SMAcross([btc]))
stats = bt.run_csv("btcusdt_trades.csv", "BTCUSDT")
print(stats)
```
