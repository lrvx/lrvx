# Runner, LiveEngine, BacktestRunner

```codon
from lrvx.runner import Runner, BacktestRunner
```

---

## Signal

Emitted by a strategy when it places an order.

| Field | Type | Description |
|-------|------|-------------|
| `order_id` | `int` | Order ID |
| `symbol` | `int` | Symbol ID |
| `side` | `str` | `"buy"` or `"sell"` |
| `order_type` | `str` | `"market"`, `"limit"`, `"stop_market"`, `"stop_limit"`, `"tp_market"`, `"tp_limit"`, `"trailing_stop"`, `"cancel"`, `"cancel_all"`, `"modify"` |
| `price` | `float` | Limit price (0 for market orders) |
| `quantity` | `float` | Order quantity |
| `trigger_price` | `float` | Stop/take-profit trigger |
| `trailing_offset` | `float` | Trailing stop absolute offset |
| `trailing_bps` | `int` | Trailing stop callback rate (basis points) |
| `new_price` | `float` | Modify: updated price |
| `new_quantity` | `float` | Modify: updated quantity |

---

## Runner

Synchronous strategy host. Push market data; strategy callbacks fire before the call returns. Pass `threaded=True` to use a Disruptor ring buffer with strategy callbacks running in a background C++ thread.

```codon
from lrvx.runner import Runner

def on_signal(sig: Signal):
    print(sig.side, sig.quantity, sig.price)

runner = Runner(registry, on_signal)          # synchronous
runner = Runner(registry, on_signal, True)    # threaded

runner.add_strategy(my_strategy)
runner.start()
runner.on_trade(btc, 67000.0, 0.01, True, ts_ns)
runner.on_book_snapshot(btc, bid_prices, bid_qtys, ask_prices, ask_qtys, ts_ns)
runner.stop()
```

### Constructor

```codon
Runner(registry: cobj, on_signal, threaded: bool = False)
```

| Parameter | Description |
|-----------|-------------|
| `registry` | Handle from `lrvx_registry_create()` |
| `on_signal` | Called when a strategy emits an order |
| `threaded` | If `True`, use Disruptor-based consumer thread |

### Methods

| Method | Description |
|--------|-------------|
| `add_strategy(strategy)` | Register a strategy instance and install its event callbacks |
| `start()` | Start the runner |
| `stop()` | Stop and clean up |
| `on_trade(symbol, price, qty, is_buy, ts_ns)` | Push a trade tick |
| `on_book_snapshot(symbol, bid_prices, bid_qtys, ask_prices, ask_qtys, ts_ns)` | Push a full L2 snapshot |
| `on_book_top(symbol, has_bid, bid_price, bid_qty, has_ask, ask_price, ask_qty, ts_ns)` | Push a touch-only frame: moves the strategy's touch without a book callback. A side whose has-flag is false is empty |
| `on_bar(symbol, open, high, low, close, volume=0.0, buy_volume=0.0, start_time_ns=0, end_time_ns=0, bar_type=0, bar_type_param=0, close_reason=0)` | Push a closed bar |
| `set_market_data_recorder(recorder_handle)` | Attach a market-data recorder handle so pushed events are also written to a tape |

`symbol` is typed `int`. Codon compiles statically, so there is no duck-typed overload accepting an
object with a `symbol_id` property — resolve the id yourself before calling.

---

## BacktestRunner

Replays OHLCV data through a strategy. Emitted orders go to `SimulatedExecutor` automatically.

```codon
from lrvx.runner import BacktestRunner

bt = BacktestRunner(registry, fee_rate=0.0004, initial_capital=10_000.0)
bt.set_strategy(my_strategy)

stats = bt.run_csv("data/btcusdt.csv", "BTCUSDT")
stats = bt.run_ohlcv(timestamps, closes, "BTCUSDT")
stats = bt.run_tape_bars("data/tape", 300.0, "BTCUSDT")
print(stats.return_pct, stats.sharpe_ratio)
```

### Constructor

```codon
BacktestRunner(registry: cobj, fee_rate: float = 0.0004, initial_capital: float = 10_000.0,
               slippage_model: str = "none", slippage_ticks: int = 0,
               slippage_tick_size: float = 0.0, slippage_bps: float = 0.0,
               slippage_impact_coeff: float = 0.0,
               submit_ack_latency_ns: int = 0, submit_ack_jitter_ns: int = 0,
               cancel_ack_latency_ns: int = 0, cancel_ack_jitter_ns: int = 0,
               cancel_ack_seed: int = 42,
               replace_ack_latency_ns: int = 0, replace_ack_jitter_ns: int = 0,
               latency = None)
```

The arguments after `initial_capital` set the built-in executor's slippage and latency with the
names and meaning of the Python runner (see
[BacktestRunner constructor](../python/strategy.md#constructor)); `latency` is a model from
`lrvx.latency` and must outlive the runner. An invalid setting raises `ValueError`.

### Methods

| Method | Returns | Description |
|--------|---------|-------------|
| `set_strategy(strategy)` | `None` | Attach a strategy and install its event callbacks |
| `run_csv(path, symbol)` | `BacktestStats` | Replay a CSV file (columns: timestamp, open, high, low, close, volume) |
| `run_ohlcv(timestamps, closes, symbol)` | `BacktestStats` | Replay raw arrays (`List[int]` timestamps in ns, `List[float]` closes) |
| `run_bars(start_ns, end_ns, opens, highs, lows, closes, volumes, symbol, bar_type=0, bar_type_param=0)` | `BacktestStats` | Replay full OHLCV bars. `Strategy.on_bar` fires; `on_trade` does **not** |
| `run_tape(path)` | `BacktestStats` | Replay one `.lrvx` tape directory |
| `run_tape_bars(path, interval_seconds, symbol="")` | `BacktestStats` | Replay a `.lrvx` tape as time bars, aggregated while it streams; each closed bar goes to `on_bar` with `run_bars`' fill rules. Same result as `aggregate_time_bars` -> `run_bars` on the tape's trades. `symbol` is required when the tape holds more than one |
| `run_tapes(paths)` | `BacktestStats` | Replay N `.lrvx` tapes merged on read |
| `equity_curve()` | `Tuple[List[int], List[float], List[float]]` | `(timestamps_ns, equity, drawdown_pct)` from the most recent run, all the same length. Raises if no run has completed |
| `trades()` | 9 parallel lists | `(symbol, side, entry_price, exit_price, quantity, pnl, fee, entry_time_ns, exit_time_ns)` for the closed trades of the most recent run. `side` is 0 for long, 1 for short |
| `close()` | `None` | Free resources |

See [Backtest](backtest.md) for the `BacktestStats` field reference.
