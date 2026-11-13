# Backtest Components

Standalone simulated exchange for step-by-step backtesting and fill analysis.

## SimulatedExecutor

A simulated exchange that matches orders against bar closes and trade prices. Supports market, limit, stop, take-profit, and trailing stop orders.

```python
executor = lrvx.SimulatedExecutor()
```

### Methods

#### `submit_order(id, side, price, quantity, type="market", symbol=1, tif="gtc", reduce_only=False, expires_at_ns=0, account_id=0, trigger=0.0, trailing_offset=0.0, trailing_bps=0)`

Submit an order to the simulated exchange.

```python
executor.submit_order(id=1, side="buy", price=0.0, quantity=1.0,
                       type="market", symbol=1)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `id` | `int` | — | Order ID |
| `side` | `str` | — | `"buy"` or `"sell"` |
| `price` | `float` | — | Limit/trigger price (0 for market) |
| `quantity` | `float` | — | Order size |
| `type` | `str` | `"market"` | Order type (see below) |
| `symbol` | `int` | `1` | Symbol ID |
| `tif` | `str` | `"gtc"` | Time in force: `"gtc"`, `"ioc"`, `"fok"`, `"gtd"`, `"post_only"` |
| `reduce_only` | `bool` | `False` | Only reduce an existing position |
| `expires_at_ns` | `int` | `0` | GTD deadline |
| `account_id` | `int` | `0` | Self-trade-prevention account identifier |
| `trigger` | `float` | `0.0` | Trigger price for the conditional order types; falls back to `price` when unset |
| `trailing_offset` | `float` | `0.0` | Fixed-price offset for `"trailing_stop"` |
| `trailing_bps` | `int` | `0` | Bps offset for `"trailing_stop"` — set exactly one of the two |

**Order types:** `"limit"`, `"market"`, `"stop_market"`, `"stop_limit"`, `"tp_market"`, `"tp_limit"`, `"trailing_stop"`, `"iceberg"` — the canonical set, also readable as `lrvx.ORDER_TYPE_NAMES`. Anything else raises `ValueError` naming the accepted set; it is never quietly turned into a market order.

**`side` and `tif`** are read the same way: any `side` but `"buy"` or `"sell"` and any `tif` outside the five above raises `ValueError` naming the value. They used to fall back, `side` to a sell and `tif` to GTC, so `side="Buy"` sold and `tif="day"` rested until cancelled.

#### Strings that name a choice

Every string argument of the Python bindings that names one of a fixed set of values is read strictly and raises `ValueError` naming what it was given and what it accepts. Each of these used to fall back to one of its values:

| Argument | Accepts | Used to fall back to |
|----------|---------|----------------------|
| `side` of `SimulatedExecutor.submit_order` / `submit_bracket` / `submit_iceberg`, the venue executor's `submit_order`, `BacktestResult.record_fill`, `L3Book.add_order` / `build_from_snapshot`, `PositionTracker.on_fill`, `PositionGroupTracker.open_position` | `"buy"`, `"sell"` | sell |
| `tif` of `SimulatedExecutor.submit_order` and the venue executor's `submit_order` | `"gtc"`, `"ioc"`, `"fok"`, `"gtd"`, `"post_only"` | GTC |
| `PositionTracker(cost_basis)` | `"fifo"`, `"lifo"`, `"average"` | FIFO |
| `SimulatedExecutor.set_stp_mode(mode)` | `"none"`, `"cancel_newest"`, `"cancel_oldest"`, `"cancel_both"`, `"decrement"` | none |
| `SimulatedExecutor.set_bracket_child_arm_mode(mode)` | `"on_full_fill"`, `"on_partial_fill"` | on full fill |
| `RateLimitPolicy.try_consume(action, ...)` | `"submit"`, `"cancel"`, `"replace"`, `"query_account"`, `"query_market_data"` | submit |
| `LiveQueuePositionEstimator.set_hidden_order_policy(policy)` | `"ignore"`, `"trust_trade_flag"`, `"infer_if_trade_exceeds_visible"` | ignore |
| `VenueAvailability` outage type | `"total"`, `"submit_only_down"` (`"submit_only"`), `"cancel_only_down"` (`"cancel_only"`), `"slow_degradation"`, `"stale_book"`, `"wrong_side_recovery"` | total |
| `VenueAvailability` open-order policy | `"cancel_all"`, `"hold"`, `"expire_gtc_after"` | cancel all |
| `WalkForwardRunner(mode)` | `"anchored"`, `"sliding"` | anchored |
| `OrderGroup.mark_action_dispatched(leg_index, kind)` | `"cancel"`, `"revert"` | revert |
| `lrvx.merge` / `recompress` `compression` | `"none"` (or `""`), `"lz4"` | none |
| `lrvx.split` `mode` | `"time"`, `"event_count"`, `"size"`, `"symbol"` | time |
| `lrvx.export_data` `format` | `"csv"`, `"json"`, `"jsonlines"`, `"binary"` | CSV |
| calendar partition `unit` | `"hour"`, `"day"`, `"week"`, `"month"` | hour |
| a `ReplaySource` event's `type` | `"trade"`, `"book_snapshot"`, `"book_delta"` | a book delta |

#### `cancel_order(order_id)`

Cancel an order by ID.

#### `cancel_all(symbol)`

Cancel all orders for a symbol.

#### `on_bar(symbol, close_price)`

Feed a bar close price for order matching. Limit orders are matched against this price.

```python
executor.on_bar(symbol=1, close_price=50000.0)
```

#### `on_bar_ohlc(symbol, open, high, low, close)`

The manual bar path: feed a full OHLC bar instead of a close price alone. Moves
the market to `open` — releasing any order held from a prior
`begin_bar_callback_window()` / `end_bar_callback_window()` pair at that
price — then walks `low -> high -> close` so resting stops and targets match
the bar's intrabar extremes, not just its close. Use this, not `on_bar`, when
driving `SimulatedExecutor` by hand: `BacktestRunner.run_bars` (and `run_tape_bars`) uses this
overload internally, so `on_bar` alone gives a caller neither the bar's open
nor its intrabar extremes.

```python
executor.on_bar_ohlc(symbol=1, open=50000.0, high=50500.0, low=49800.0, close=50200.0)
```

#### `begin_bar_callback_window()` / `end_bar_callback_window()`

Bracket a hand-driven strategy callback so orders it submits are held instead
of matched immediately — matching against a bar's own high/low/close from
inside its callback is look-ahead, since the market has already been walked
through that bar by the time the callback runs. Held orders release at the
next `on_bar_ohlc` call's open.

```python
executor.on_bar_ohlc(1, open=50000.0, high=50500.0, low=49800.0, close=50200.0)
executor.begin_bar_callback_window()
executor.submit_order(1, "buy", 0.0, 1.0, "market", symbol=1)
executor.end_bar_callback_window()
```

#### `on_trade(symbol, price, is_buy)`

Feed a trade for order matching.

```python
executor.on_trade(symbol=1, price=50000.0, is_buy=True)
```

#### `on_trade_qty(symbol, price, qty, is_buy)`

Feed a trade with quantity. Required for `QUEUE_FULL` queue simulation.

```python
executor.on_trade_qty(symbol=1, price=50000.0, qty=0.5, is_buy=True)
```

#### `on_best_levels(symbol, bid_price, bid_qty, ask_price, ask_qty)`

Feed both best levels in one call. Routed to the touch path: depth from the
last `on_book_snapshot` survives instead of being replaced by a one-level
book.

```python
executor.on_best_levels(1, 49999.0, 2.0, 50001.0, 1.5)
```

#### `on_top_of_book(symbol, bid, ask)`

Feed a touch-only frame: moves the touch, keeps deeper depth from the last
snapshot. `bid` / `ask` are `(price, qty)` tuples; `None` means that side
of the book is empty.

```python
executor.on_top_of_book(1, (49999.5, 2.0), (50000.5, 1.5))
```

#### `set_default_slippage(model, ticks=0, tick_size=0.0, bps=0.0, impact_coeff=0.0)`

Configure slippage for all symbols. `model` is a string.

```python
executor.set_default_slippage("fixed_bps", bps=2.0)
```

| `model` | Description |
|---------|-------------|
| `"none"` | No slippage |
| `"fixed_ticks"` | Fixed tick count — uses `ticks` and `tick_size` |
| `"fixed_bps"` | Fixed basis points — uses `bps` |
| `"volume_impact"` | Volume-proportional impact — uses `impact_coeff` |

`tick_size` is in price units; `0.0` falls back to one raw price unit.

`BacktestRunner` takes the default profile as constructor keywords instead
(`slippage_model`, `slippage_ticks`, `slippage_tick_size`, `slippage_bps`,
`slippage_impact_coeff`), stricter than this setter: an unknown model, a missing
parameter or one the model does not read raises there.

#### `set_symbol_slippage(symbol, model, ticks=0, tick_size=0.0, bps=0.0, impact_coeff=0.0)`

Per-symbol slippage override. Same parameters as `set_default_slippage`.

#### `set_queue_model(model, depth=1)`

Configure limit order queue simulation. `model` is one of the names below; any other string
raises `LrvxError` (`E_VAL_002`) listing them, and the model installed before stays.

```python
executor.set_queue_model("tob", depth=1)
```

| `model` | Description |
|---------|-------------|
| `"none"` | Fill limit orders immediately at price |
| `"tob"` | Fill only when price trades through level |
| `"full"` | Model queue position; fill as volume passes |
| `"pro_rata"` | Split each trade across the level proportionally to size |
| `"pro_rata_with_fifo"` | First N orders at a level fill FIFO (`set_queue_fifo_top_n`), the rest pro-rata |
| `"top_pro_lmm"` | Reserve a share for the queue front (`set_top_priority_share`), then pro-rata with an LMM bonus |
| `"pro_rata_with_priority"` | Pro-rata weighted by `set_order_priority_multiplier` |

#### `advance_clock(timestamp_ns)`

Advance the simulation clock.

```python
executor.advance_clock(timestamp_ns=1704067200_000_000_000)
```

#### `reset()`

Drop fills and run-scoped state (held/conditional orders, queue positions,
net positions, brackets, in-flight acks, market state) while keeping
installed configuration — slippage, queue model, latency distributions, rate
limits, STP, callbacks, attached venue availability. Seeded RNGs go back to
their configured seeds. Call between passes when replaying several
hand-driven runs through one executor, so each reports only its own fills.

```python
executor.reset()
assert executor.fill_count == 0
```

#### `fills() -> ndarray`

Get all fills as a numpy structured array with `PyFill` dtype.

```python
fills = executor.fills()
for i in range(len(fills)):
    print(f"Fill: order={fills[i]['order_id']}, price={fills[i]['price_raw']/1e8}")
```

#### `fills_list() -> list[dict]`

Get fills as a list of dicts (more convenient, less performant).

```python
for fill in executor.fills_list():
    print(f"{fill['side']} {fill['quantity']} @ {fill['price']}")
```

| Key | Type | Description |
|-----|------|-------------|
| `order_id` | `int` | Order ID |
| `symbol` | `int` | Symbol ID |
| `side` | `str` | `"buy"` or `"sell"` |
| `price` | `float` | Fill price |
| `quantity` | `float` | Fill quantity |
| `timestamp_ns` | `int` | Fill timestamp |

### Properties

| Property | Type | Description |
|----------|------|-------------|
| `fill_count` | `int` | Number of fills |

---

## BacktestResult

Computes statistics and equity curve from a `SimulatedExecutor`'s fills.

```python
result = lrvx.BacktestResult(initial_capital=10_000.0, fee_rate=0.0004)
result.ingest_executor(executor)
stats = result.stats()
print(stats['net_pnl'], stats['sharpe_ratio'])
```

### Constructor

```python
lrvx.BacktestResult(
    initial_capital=100000.0,
    fee_rate=0.0001,
    use_percentage_fee=True,
    fixed_fee_per_trade=0.0,
    risk_free_rate=0.0,
    annualization_factor=252.0,
)
```

### Methods

| Method | Returns | Description |
|--------|---------|-------------|
| `record_fill(order_id, symbol, side, price, quantity, timestamp_ns)` | `None` | Record a single fill |
| `ingest_executor(executor)` | `None` | Drain all fills from a `SimulatedExecutor` in FIFO order |
| `stats()` | `dict` | Compute and return statistics |
| `trades()` | `ndarray` | Closed trades as a `PyTradeRecord` structured array. A position still open after the last fill is not listed |
| `equity_curve()` | `ndarray` | Equity curve as a `PyEquityPoint` structured array, one point per closed trade |
| `write_equity_curve_csv(path)` | `bool` | Write equity curve to CSV (`timestamp_ns,equity,drawdown_pct` header) |

### `stats()` keys

The [`Stats`](engine.md#stats) object returned by `Engine.run()` is a different, smaller set, but the key names are the same.

| Key | Type | Description |
|-----|------|-------------|
| `total_trades` | `int` | Round-trip trade count |
| `winning_trades` | `int` | Profitable trade count |
| `losing_trades` | `int` | Losing trade count |
| `max_consecutive_wins` | `int` | Longest winning streak |
| `max_consecutive_losses` | `int` | Longest losing streak |
| `initial_capital` | `float` | Starting capital |
| `final_capital` | `float` | Ending capital |
| `total_pnl` | `float` | Realized PnL of the closed trades, before fees |
| `total_fees` | `float` | Fees of every recorded fill, including the entry fill of a position still open |
| `net_pnl` | `float` | `total_pnl - total_fees`; see [A position open after the last fill](#a-position-open-after-the-last-fill) |
| `gross_profit` | `float` | Sum of winning trades |
| `gross_loss` | `float` | Sum of losing trades |
| `max_drawdown` | `float` | Maximum drawdown (absolute) |
| `max_drawdown_pct` | `float` | Maximum drawdown (percentage) |
| `win_rate` | `float` | Fraction of winning trades |
| `profit_factor` | `float` | Gross profit / gross loss |
| `avg_win` | `float` | Average winning trade |
| `avg_loss` | `float` | Average losing trade |
| `avg_win_loss_ratio` | `float` | Average win over average loss |
| `avg_trade_duration_ns` | `int` | Mean trade holding time (ns) |
| `median_trade_duration_ns` | `int` | Median trade holding time (ns) |
| `max_trade_duration_ns` | `int` | Longest trade holding time (ns) |
| `sharpe_ratio` | `float` | Sharpe ratio over the returns between closed trades, scaled by sqrt(252); one period is one closed trade, not one day, so it is not annualized by time |
| `sortino_ratio` | `float` | Sortino ratio over the same per-trade returns, scaled the same way |
| `calmar_ratio` | `float` | Calmar ratio |
| `time_weighted_return` | `float` | Time-weighted return |
| `return_pct` | `float` | Net return percentage |
| `start_time_ns` | `int` | First fill timestamp |
| `end_time_ns` | `int` | Last fill timestamp |

#### A position open after the last fill

`trades()` and `equity_curve()` hold closed trades only, and the result never
values an open position, so the unrealized PnL of a position still open after
the last fill is in none of the numbers. Its entry fee is: `total_fees` counts
every fill, so `net_pnl`, `final_capital` and `return_pct` include it.

```
net_pnl = sum(trades()["pnl"] - trades()["fee"]) - fee of the open entry
```

For 0.01 BTC entered at 78,000 with a 4 bp fee, `net_pnl` is 0.312 below the
closed trades' net and below the last equity point minus `initial_capital`.
When no trade has closed, every statistic is zero, `total_fees` included.

A position is open when the signed fill quantities for a symbol do not sum to
zero: sum `quantity` over `executor.fills_list()` with `+` for `"buy"` and
`-` for `"sell"`, over the same fills you ingested. Under `BacktestRunner`, use
the strategy's `position()` instead (see
[strategy.md](strategy.md#a-position-open-at-the-end)).

---

## Structured Dtypes

### PyFill

| Field | Type | Description |
|-------|------|-------------|
| `order_id` | `uint64` | Order ID |
| `symbol` | `uint32` | Symbol ID |
| `side` | `uint8` | 0 = buy, 1 = sell |
| `price_raw` | `int64` | Fill price * 10^8 |
| `quantity_raw` | `int64` | Fill quantity * 10^8 |
| `timestamp_ns` | `int64` | Fill timestamp (ns) |

### PyTradeRecord

| Field | Type | Description |
|-------|------|-------------|
| `symbol` | `uint32` | Symbol ID |
| `side` | `uint8` | 0 = buy, 1 = sell |
| `entry_price_raw` | `int64` | Entry price * 10^8 |
| `exit_price_raw` | `int64` | Exit price * 10^8 |
| `quantity_raw` | `int64` | Trade quantity * 10^8 |
| `entry_time_ns` | `int64` | Entry timestamp |
| `exit_time_ns` | `int64` | Exit timestamp |
| `pnl_raw` | `int64` | PnL * 10^8 |
| `fee_raw` | `int64` | Fee * 10^8 |

### PyEquityPoint

| Field | Type | Description |
|-------|------|-------------|
| `timestamp_ns` | `int64` | Sample timestamp (ns) |
| `equity` | `float64` | Account equity |
| `drawdown_pct` | `float64` | Drawdown from peak (percentage) |

---

## Example

```python
import lrvx

executor = lrvx.SimulatedExecutor()

# Simulate a simple trade
executor.advance_clock(1_000_000_000)  # t=1s
executor.submit_order(id=1, side="buy", price=0, quantity=1.0)
executor.on_bar(symbol=1, close_price=50000.0)

executor.advance_clock(2_000_000_000)  # t=2s
executor.submit_order(id=2, side="sell", price=0, quantity=1.0)
executor.on_bar(symbol=1, close_price=51000.0)

for fill in executor.fills_list():
    print(f"{fill['side']} {fill['quantity']} @ {fill['price']}")
```
