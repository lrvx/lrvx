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
| `add_symbol(exchange, symbol, tick_size=None, lot_size=None)` | `Symbol` | Register a symbol and return its `Symbol` object |
| `symbol_count()` | `int` | Number of registered symbols |
| `tick_size(symbol)` | `float` | The price tick the entry holds now |
| `tick_size_declared(symbol)` | `bool` | Whether order prices on the symbol are checked against it |
| `lot_size(symbol)` | `float \| None` | The quantity step the entry holds now, `None` when it declares none |

`tick_size` is the venue's price increment, and a backtest rejects orders priced off it. Left out, the symbol's book uses 0.01 and order prices are not checked; `Symbol.tick_size` then reads 0.01.

The three getters take a `Symbol` or its id and read the registry entry, where a `Symbol` only carries what `add_symbol` was given: `tick_size(symbol)` is 0.01 with `tick_size_declared(symbol)` `False` for a symbol registered without a tick, and both follow a tick or lot a connector applies to the entry later. An id that is not registered raises `ValueError`.

`lot_size` is the venue's quantity step, and a backtest rejects an order whose quantity is not a whole number of it (`reject_reason` `"quantity_off_lot"`). Left out, quantities are not checked and `Symbol.lot_size` is `None`. A `lot_size` that is not positive raises `ValueError`.

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

#### `on_touch(ctx: SymbolContext)`

Called when the touch changes: price or size on either side, or a side
appearing or emptying. Fires on a top-of-book frame, and right after
`on_book_update` on a depth frame that moved the touch. `ctx.best_bid` /
`ctx.best_ask` are the new touch. A top that repeats the touch fires nothing.

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

`ctx.position` in `on_fill` and `on_order_update` already counts the fill
being delivered: the runner's position tracker takes a fill before the
strategy hears of it, so `ctx.position` and `position()` agree there, in a
backtest and in paper mode. After a buy of 1 from flat, `ctx.position` in
that fill's `on_fill` is 1.

#### `on_order_update(ctx: SymbolContext, event: OrderEventData)`

Called on every order-lifecycle status change: `NEW`, `ACCEPTED`, `CANCELED`,
`REJECTED`, `REPLACED`, `TRIGGERED`, `TRAILING_UPDATED` and the rest. A fill
does not fire here: a `PARTIALLY_FILLED` or `FILLED` event reaches `on_fill`
and only `on_fill`, in a backtest and in paper mode alike. A strategy that
tracks an order to its end needs both callbacks.

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
| `modify_order(order_id, new_price, new_qty)` | Modify a resting order, or a stop / take-profit / trailing order still waiting on its trigger. It keeps its id, symbol, side and type; `new_qty` is the quantity left to work. `new_price` is the trigger of a stop-market, take-profit-market or trailing-stop order and the limit price of every other type. A refused modify (off-tick price, an order already filled or cancelled) arrives in `on_order_update` as `REPLACE_REJECTED` with `reject_reason` set |

`tif` accepts `"gtc"`, `"ioc"`, `"fok"` and `"post_only"`. Any other value raises `ValueError` naming it, and nothing is emitted; it used to place a GTC order, `"gtd"` included (these methods take no expiry time, so a good-till-date order cannot be expressed through them). The `side` of the stop, take-profit and trailing methods is `"buy"` or `"sell"`; anything else raises `ValueError` where it used to mean sell. Both are checked before anything else: a strategy that is not attached to a runner returns 0 from an emit, and it used to return that 0 for an unknown `side` or `tif` too, without looking at it.

In a backtest, `price`, `trigger`, `limit_price`, `offset` and `modify_order`'s `new_price` must be whole numbers of the symbol's tick when the symbol was registered with one. An order off the tick is rejected: `on_order_update` receives `status == "REJECTED"` and `reject_reason` `"price_off_tick"`, `"trigger_off_tick"` or `"trailing_offset_off_tick"` (a modify gets `"REPLACE_REJECTED"`). Round a computed price before sending it, for example `round(price / tick) * tick`. See [SimulatedExecutor](../api/backtest/simulated_executor.md#order-prices-and-the-tick).

Quantities are checked the same way when the symbol was registered with a quantity step, `registry.add_symbol(exchange, name, tick_size=..., lot_size=...)`: an order whose `qty`, or a `modify_order` whose `new_qty`, is not a whole number of `lot_size` gets `"REJECTED"` (`"REPLACE_REJECTED"` for the modify) with `reject_reason` `"quantity_off_lot"`. Without `lot_size` quantities are not checked. The returned symbol carries it as `symbol.lot_size` (`None` when not given). See [SimulatedExecutor](../api/backtest/simulated_executor.md#order-quantities-and-the-lot).

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
| `add_strategy(strategy)` | Register a strategy instance. The runner gives each strategy its own subscriber id, 1, 2, ... in the order added, and order ids are `(id << 32) + n`, so two strategies on one runner never issue the same order id. Raises `LrvxError` when the engine refuses the strategy |
| `replace_strategy(index, strategy)` | Atomically swap the strategy at `index`. The old strategy's `on_stop` fires, the bridge's internal state survives, the new strategy's `on_start` fires afterwards. WebSocket / gRPC connections are unaffected |
| `start()` | Start the runner |
| `stop()` | Stop the runner |
| `on_trade(symbol, price, qty, is_buy, ts_ns=0)` | Inject a trade event |
| `on_book_snapshot(symbol, bid_prices, bid_qtys, ask_prices, ask_qtys, ts_ns=0)` | Inject an order book snapshot |
| `on_book_top(symbol, bid, ask, ts_ns=0)` | Inject a touch-only frame: moves the strategy's touch without a book callback. `bid` / `ask` are `(price, qty)` tuples; `None` means that side of the book is empty |
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
| `order_closed(order_id)` | Tell the runner an order it sent to the executor is finished |
| `on_order_event(order_id, symbol, side, status, ...)` | Report a venue's order update or fill to the strategy that issued the order |
| `seed_position(strategy, symbol, position, avg_entry_price=0.0)` | Set what a strategy holds before any fill is reported |
| `drain()` | Wait until a threaded runner's strategies have been handed everything published so far |
| `replay_tape(path, simulator, *, tape_symbol_id=None, symbol=None, lock=None, after_event=None, on_start=None)` | Replay a `.lrvx` tape into the runner's strategies with a `SimulatedExecutor` standing in for the venue, through the loop `BacktestRunner.run_tape` uses: every trade and book event, in time order, reaches the simulator before the strategies. `on_start()` is called once the simulator has been reset and before its first event; `after_event()` after each event. On a threaded runner the replay waits after each event until the strategies have been handed it, so it fills where a synchronous replay does. Returns a dict of counts. `lrvx engine sim` is built on it: see [What the simulator sees and does](../../how-to/engine-sim.md#what-the-simulator-sees-and-does) |
| `on_order_event(order_id, symbol, side, status, order_type='limit', fill_qty=0.0, fill_price=0.0, ts_ns=0, reject_reason='', is_maker=False)` | Report one order event a venue sent. `PARTIALLY_FILLED` / `FILLED` reach the strategy's `on_fill(ctx, event)` and only `on_fill`; every other status reaches `on_order_update(ctx, event)` |

With an executor attached the runner keeps every order that can rest, by id, so that a strategy's `modify_order` reaches `Executor.replace` as that order with its price and quantity changed. Nothing tells the runner of a fill, so call `order_closed(order_id)` from wherever the executor learns an order has reached a terminal state (filled, cancelled by the venue, rejected, expired); the entry is dropped, and an id the runner does not hold is ignored. A `cancel_order` from the strategy drops the entry by itself, and market, IOC and FOK orders are never kept. The two legs of an OCO leave the record together: a cancel of either leg, or `order_closed` with either leg's id, drops both, because the venue cancels the other leg when one fills or is cancelled.

The `Order` an `Executor` is handed carries a trailing stop's fixed distance in `trailing_offset` and the callback rate of a `trailing_stop_percent` in `trailing_bps` (basis points); exactly one of the two is above 0 for a trailing stop. `trailing_bps` used to be missing, so a percent trailing stop reached an executor with nothing to trail by. In C the order struct keeps its shape and the rate is read with `lrvx_order_trailing_bps(order)` inside the callback that was handed the order.

`CcxtBroker` calls `order_closed` for every terminal status (`closed`, `canceled`, `expired`, `rejected`) its orders stream reports, and for an order the exchange already reports finished in the answer to its placement. It drops the order from its own maps at the same moment, so they hold open orders only; they used to hold every order ever placed. A `cancel_order` or `modify_order` the strategy still sends for such an order is not sent to the exchange and is reported through `on_error` as `ValueError("CANCEL: no ccxt order id tracked ...")` / `"MODIFY: ..."`.

`on_order_event` is how a live order's updates reach a strategy through the callbacks a backtest
uses. The runner keeps a position tracker for each strategy and shows it the event first, so
`ctx.position` and `self.position()` inside `on_fill` count the fill; before this a live runner's
strategies always read a position of 0. Positions are per strategy, as in a backtest: a strategy
reads the net of the fills reported for its own orders, not the account's and not another
strategy's on the same runner. `side` is `'buy'` or `'sell'`, `status` and `order_type` are
the names `OrderEventData` carries, and `fill_qty` / `fill_price` are this fill's own, not the
order's running totals. The strategy that issued the order receives the event (an order id is the
strategy's id shifted left 32 bits, plus a counter), and the second leg of an OCO goes to the
strategy of its first leg while the pair is on record, so report a leg's events before
`order_closed`. An id no attached strategy issued goes to every strategy subscribed to the symbol
and counts in no position.

`seed_position(strategy, symbol, position, avg_entry_price=0.0)` sets what a strategy holds before
any fill is reported: after a restart the account may not be flat, and no `on_order_event` will tell
of that position. `position` is signed, negative for a short. The strategy reads it from
`self.position()` and `ctx.position`, and the fills reported afterwards add to it. A position is
seeded once: `ValueError` for a strategy that already holds one in the symbol and for one that is
not attached to this runner. `CcxtBroker` seeds the position it finds at start.

Which thread runs the callbacks depends on the runner:

| | `Runner(..., threaded=False)` | `Runner(..., threaded=True)` |
|---|---|---|
| Thread | the calling thread, inside `on_order_event` | the strategy's order-event consumer thread; `on_order_event` queues the event and returns |
| Order among order events | as reported | as reported |
| Order against market data | as called | after every trade, book and bar published before the report has been handed to that strategy; market data published after the report may arrive on either side of it |
| Not started, or stopped | the calling thread | the calling thread; an event queued before `stop()` is delivered before `stop()` returns |

`runner.drain()` returns once every strategy has been handed everything published so far and
whatever those callbacks published in turn; it is the barrier for a feeder that needs a threaded
strategy to have finished with one event before it sends the next. Never call it from a strategy
callback.

An unknown side, status or order type raises `ValueError`. `CcxtBroker` calls it for
every order the exchange reports: see [Order updates and fills](../../how-to/ccxt-adapter.md#order-updates-and-fills).

A `modify_order` that names an id the runner does not hold (never sent, not an order that rests, cancelled, or reported closed) is not sent to the executor, and after it `lrvx.last_error()` reads `modify: order <id> is not on record with this executor ... [unknown_order]`.

#### Reading why a signal went nowhere

Every emit method (`market_buy`, `limit_sell`, `stop_market`, `cancel_order`, `modify_order`, `close_position`, the `emit_*` forms, ...) clears `lrvx.last_error()` on entry. Read it right after the emit, inside the callback that emitted (it is per thread): `''` means nothing refused the signal, otherwise it says what did.

| `lrvx.last_error()` ends in | What happened |
|-----------------------------|---------------|
| `[kill_switch]` | `KillSwitch.check` returned `False` |
| `[order_validator]` | `OrderValidator.validate` returned `False` |
| `[risk_manager]` | `RiskManager.allow` returned `False` |
| `[unknown_order]` | a `modify_order` named an order the runner's executor record does not hold |

A dropped signal reads `emit: <type> signal for order <id> on symbol <n> was dropped by the <gate>; <outcome> [<token>]`. The gates are asked in the order kill switch, order validator, risk manager, and the first that says no is the one named. The emit still returns the order id it assigned.

What is put to the gates depends on what runs the strategy:

| | `Runner` and `LiveEngine` | `BacktestRunner` (and `PaperBroker`, which runs one) |
|---|---|---|
| Signals asked | signals that create an order (market, limit, stop and take-profit, trailing stop, an OCO as one signal) and the liquidity pair | signals that create an order: market, limit, stop and take-profit (market and limit), trailing stop, both legs of an OCO |
| Pass unasked | cancel, cancel-all, modify, and any reduce-only order (a `close_position`): the same rule as the backtest runner, so a triggered kill switch cannot hold back a cancel | cancel, cancel-all, modify, and any reduce-only order (a `close_position`), so a tightened limit cannot hold a strategy in a position |
| A drop skips | `on_signal`, the executor and the PnL / storage observers | the simulator or the attached executor |
| `<outcome>` | `it reached neither on_signal nor the executor` | `it reached no executor` |
| Kill switch | asked again for every signal it is asked about | stays triggered for the rest of the run once it said no |

`on_signal` is an observer: a runner created without one gates and forwards its signals the same way.

```python
def on_trade(self, ctx, trade):
    order_id = self.limit_buy(trade.price - 1.0, 0.1)
    why = lrvx.last_error()
    if why:
        print("order", order_id, "went nowhere:", why)
```

A `MarketDataRecorderHook` subclass overrides any of `on_trade(trade)`,
`on_book_update(symbol, is_snapshot, bids, asks, ts_ns)`,
`on_book_top(symbol, bid, ask, ts_ns)`, `on_start()` and `on_stop()`.
`on_book_update` receives snapshots and deltas, with `bids` / `asks` lists of
`(price, qty)`. A top-of-book frame goes to `on_book_top` instead: `bid` /
`ask` are one `(price, qty)` each, or `None` for an empty side. Defining
`on_book_top` does not change `on_book_update`, which keeps receiving every
snapshot and delta. A hook that does not define it is not called for tops.

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

Runs a strategy against historical data: CSV, bar arrays, or `.lrvx` tapes.

`fee_rate` defaults to `0.0004` here, a taker rate. `lrvx.BacktestResult` and the C++
`BacktestConfig` default to `0.0001`, so pass the rate explicitly when a runner result is
compared with a result scored by hand.

```python
bt = lrvx.BacktestRunner(registry, fee_rate=0.0004, initial_capital=10_000)
bt.set_strategy(strategy)

stats = bt.run_csv("data.csv")             # auto-detects symbol from registry
stats = bt.run_csv("data.csv", "BTCUSDT")  # explicit symbol name
stats = bt.run_tape_bars("data/tape", interval_seconds=300, symbol="BTCUSDT")
```

### Constructor

`BacktestRunner(registry, fee_rate=0.0004, initial_capital=100000.0, *, ...)`

The keyword-only arguments configure the built-in simulated executor. Each is the `BacktestConfig`
field of the same name (snake_cased) and means what it means there. All defaults reproduce the
instant, slippage-free runner.

| Keyword | Default | Meaning |
|---------|---------|---------|
| `slippage_model` | `"none"` | `"none"`, `"fixed_ticks"`, `"fixed_bps"` or `"volume_impact"`; every model moves a taker fill against the taker |
| `slippage_ticks`, `slippage_tick_size` | `0`, `0.0` | `fixed_ticks`: both required and positive; the fill moves `ticks x tick_size` |
| `slippage_bps` | `0.0` | `fixed_bps`: required and positive |
| `slippage_impact_coeff` | `0.0` | `volume_impact`: required and positive; moves `coeff x order / level size`. Needs book depth, so `run_bars`, `run_tape_bars`, `run_csv` and `run_ohlcv` refuse it |
| `submit_ack_latency_ns`, `submit_ack_jitter_ns` | `0`, `0` | Venue ack delay before a submitted order can match, drawn uniformly from `[latency - jitter, latency + jitter]` floored at 0 |
| `cancel_ack_latency_ns`, `cancel_ack_jitter_ns` | `0`, `0` | Same for cancels; the order can still fill in the window |
| `replace_ack_latency_ns`, `replace_ack_jitter_ns` | `0`, `0` | Same for replaces |
| `cancel_ack_seed` | `42` | Seeds the ack-latency draws |
| `latency` | `None` | A `lrvx.LatencyModel` (`ConstantLatency`, `GaussianLatency`, `ExponentialLatency`, `EmpiricalLatency` or a subclass); its `order_delay()` is added to every submit's deferral |

A setting the chosen slippage model does not read, an unknown model, or a negative latency raises
`LrvxError` (`E_VAL_002`). A runner built with any of these settings refuses `set_executor` and
`set_venue_stack`, whose executors would not use them. On bar replay any positive submit deferral
moves a fill to a later bar's open: see
[Latency on bar replay](../../how-to/backtest-with-latency.md#latency-on-bar-replay).

`run_ohlcv` and `run_bars` take a registered symbol's name. With `symbol` omitted or `''` they
replay for the first symbol registered. `run_csv` without a symbol takes the name from the file
name.

| Method | Description |
|--------|-------------|
| `set_strategy(strategy)` | Set the strategy to backtest |
| `run_csv(path, symbol='')` | Run backtest against a CSV file, returns stats dict |
| `run_ohlcv(ts, close, symbol='')` | Replay close-only bars as synthetic trades (`Strategy.on_trade` fires) |
| `run_bars(start_time_ns, end_time_ns, open, high, low, close, volume, symbol='', bar_type=0, bar_type_param=0)` | Replay full OHLCV bars (`Strategy.on_bar` fires) |
| `run_tape(path)` | Run against a `.lrvx` tape directory — the canonical recorded artifact `lrvx tape record` writes. Same stats shape as `run_csv`, plus `skipped_option_quotes` and `skipped_pool_states`: option-quote and pool-state records on the tape reach no strategy callback and do not move the simulated executor, so they are skipped and counted here |
| `run_tape_bars(path, *, interval_seconds, symbol='')` | Replay a `.lrvx` tape as time bars of `interval_seconds` (`Strategy.on_bar` fires, `on_trade` does not). The trades are aggregated while the tape streams, never materialised in Python. Same bars, fills, `trades()` and `equity_curve()` as `read_trades` -> `aggregate_time_bars` -> `run_bars` for the same interval. `symbol` is required when the tape holds more than one. See [Bars from a tape in one call](../../how-to/backtest.md#bars-from-a-tape-in-one-call) |
| `run_tape_bars_on_ticks(path, *, interval_seconds, symbol='')` | Decide on bars, fill on ticks: `on_bar` receives the bars `run_tape_bars` builds for the same interval, while the simulated executor is fed every trade and book record as `run_tape` feeds it (`on_trade` and `on_book_update` do not fire). Bar n reaches `on_bar` when the first trade stamped at or after its end arrives, before that trade reaches the executor; an order sent from `on_bar` can first match against that trade, or with a submit latency L against the first record at or after the bar's end + L. Working stops and limits fill on the prints inside the next bars, a stop never better than its trigger. `symbol` as for `run_tape_bars`. Stats shape as `run_tape`, including `skipped_option_quotes` and `skipped_pool_states`. See [Decide on bars, fill on ticks](../../how-to/backtest.md#decide-on-bars-fill-on-ticks) |
| `run_tapes(paths)` | Run against N `.lrvx` tapes merged on read. Symbols are rekeyed by `(metadata.exchange, name)`, so two captures of the same venue/symbol collapse and two venues stay distinct. `run_tapes([t])` equals `run_tape(t)`, skipped-record counts included |
| `equity_curve()` | Equity curve from the most recent run, as a dict of numpy arrays (`timestamp_ns`, `equity`, `drawdown_pct`). One point per closed trade; a position still open at the end adds none (see [A position open at the end](#a-position-open-at-the-end)) |
| `trades()` | Closed trades from the most recent run, as a dict of numpy arrays (`symbol`, `side`, `entry_price`, `exit_price`, `quantity`, `pnl`, `fee`, `entry_time_ns`, `exit_time_ns`). A position still open at the end is not a trade and is not listed. On bar replay (`run_bars`, `run_tape_bars`) `entry_time_ns` / `exit_time_ns` are the end of the bar in which the fill happened, wherever in the bar it matched: a fill inside a daily bar carries the next day's 00:00. On tick replay, and with `run_tape_bars_on_ticks`, they are the timestamp of the filling event |

An empty `symbol` resolves to `"default"`.

### Hook setters

| Method | Description |
|--------|-------------|
| `set_executor(executor)` | Replace the built-in simulated executor |
| `add_execution_listener(listener)` | Attach an `ExecutionListener` |
| `set_pnl_tracker(tracker)` | Attach a PnL tracker; fires `on_signal(signal)` for every fill the simulator dispatches |
| `set_risk_manager(rm)` | Attach a pre-trade risk manager |
| `set_kill_switch(ks)` | Attach a kill switch |
| `set_order_validator(ov)` | Attach an order validator |

Each setter takes `None` to detach. Which signals the three gates are asked about, which pass unasked
(cancels, modifies and reduce-only orders) and what a dropped signal leaves in `lrvx.last_error()` are
in [Reading why a signal went nowhere](#reading-why-a-signal-went-nowhere), next to the live runner's.

### Stats dict keys

| Key | Description |
|-----|-------------|
| `return_pct` | Net return percentage, `net_pnl / initial_capital * 100` |
| `total_pnl` | Realized P&L of the closed trades, before fees |
| `total_fees` | Fees of every fill, including the entry fill of a position still open at the end |
| `net_pnl` | `total_pnl - total_fees`. Carries the entry fee of a position still open at the end, but not its unrealized P&L |
| `final_capital` | `initial_capital + net_pnl` |
| `total_trades` | Round-trip trade count |
| `win_rate` | Winning trade fraction |
| `sharpe_ratio` | Sharpe ratio over the returns between closed trades, scaled by sqrt(252); one period is one closed trade, not one day, so it is not annualized by time |
| `max_drawdown_pct` | Peak-to-trough drawdown (%) |

The dict carries the other `BacktestResult.stats()` keys too (`winning_trades`,
`losing_trades`, `initial_capital`, `gross_profit`, `gross_loss`,
`max_drawdown`, `profit_factor`, `sortino_ratio`, `calmar_ratio`).

### A position open at the end

A run can end with a position still open: the data ran out before the exit.
`trades()` and `equity_curve()` hold closed trades only, and nothing values the
open position, so its unrealized P&L is in none of the numbers. Its entry fee
is: `total_fees` counts every fill, so `net_pnl`, `final_capital` and
`return_pct` include it, while the closed trades do not.

```
net_pnl = sum(trades()["pnl"] - trades()["fee"]) - fee of the open entry
```

For 0.01 BTC entered at 78,000 with a 4 bp fee that is 0.312 below the closed
trades' net, and below the last `equity_curve()` point minus `initial_capital`.
When no trade has closed at all, the statistics are all zero, `total_fees` and
`net_pnl` included, even though an entry was filled and charged.

To tell that a run ended with a position open, read `position()` in the
strategy's `on_stop()`, which the runner calls at the end of the run, or call
`strategy.position(symbol)` after the run while the runner that ran it is
still alive. A non-zero value is the open quantity. To value it, mark it at the
last price yourself, or close it from the strategy before the data ends so it
becomes a trade.

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
