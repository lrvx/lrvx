# Codon Bindings

Codon is an ahead-of-time compiled Python-like language that produces native code.
lrvx provides Codon bindings via the C API, so strategies compile to binaries with no interpreter overhead.

## Prerequisites

- [Codon compiler](https://github.com/exaloop/codon) installed
- lrvx built with `-DLRVX_BUILD_CAPI=ON`

## Build

```bash
cmake -B build \
  -DLRVX_ENABLE_BACKTEST=ON \
  -DLRVX_BUILD_CAPI=ON \
  -DLRVX_BUILD_CODON=ON \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build
```

Produces `build/src/capi/liblrvx_capi.so` and the example binaries in `build/codon/`.

Compile a strategy:

```bash
codon build -exe \
  -o my_strategy \
  -L build/src/capi \
  -llrvx_capi \
  my_strategy.codon
```

## Symbols

```codon
from lrvx.runner import Runner, lrvx_registry_create, lrvx_registry_add_symbol

reg = lrvx_registry_create()
btc = lrvx_registry_add_symbol(reg, "binance".c_str(), "BTCUSDT".c_str(), 0.01)
# btc is a u32 symbol ID — pass it anywhere a symbol is expected
```

## Writing a Strategy

Subclass `Strategy` and override `on_trade`. Order methods default to the primary symbol.

```codon
from lrvx.strategy import Strategy
from lrvx.context import SymbolContext
from lrvx.types import TradeData
from lrvx.indicators import SMA

class SMAcross(Strategy):
    fast: SMA
    slow: SMA

    def __init__(self, symbols: List[int]):
        super().__init__(symbols)
        self.fast = SMA(10)
        self.slow = SMA(30)

    def on_start(self):
        print("started")

    def on_stop(self):
        print("stopped")

    def on_trade(self, ctx: SymbolContext, trade: TradeData):
        f = self.fast.update(trade.price.to_double())
        s = self.slow.update(trade.price.to_double())
        if not self.slow.ready:
            return
        if f > s and self.position() == 0.0:
            self.market_buy(0.01)
        elif f < s and self.position() > 0.0:
            self.close_position()
```

### Order methods

| Method | Description |
|--------|-------------|
| `market_buy(qty)` | Market buy (primary symbol) |
| `market_sell(qty)` | Market sell |
| `limit_buy(price, qty)` | Limit buy (GTC) |
| `limit_sell(price, qty)` | Limit sell (GTC) |
| `stop_market(side, trigger, qty)` | Stop market. `side`: `"buy"` / `"sell"` |
| `take_profit_market(side, trigger, qty)` | Take-profit market |
| `trailing_stop(side, offset, qty)` | Trailing stop by offset |
| `close_position()` | Reduce-only close |
| `cancel_order(order_id)` | Cancel by ID |
| `cancel_all_orders()` | Cancel all |

All methods accept an optional `symbol` kwarg to target a specific symbol by name.

### Context queries

| Method | Description |
|--------|-------------|
| `position()` | Current position (float) |
| `last_price()` | Last trade price |
| `best_bid()` | Best bid, `None` when the bid side has no level (`0.0` is a price) |
| `best_ask()` | Best ask, `None` when the ask side has no level |
| `mid_price()` | Mid price, `None` unless both sides have a level |

## Runner (live)

```codon
from lrvx.runner import Runner

def on_signal(sig):
    # sig.side: "buy" | "sell"
    # sig.order_type: "market" | "limit" | ...
    # sig.quantity, sig.price
    print(sig.side, sig.quantity, "@", sig.price)

runner = Runner(reg, on_signal)           # synchronous
# runner = Runner(reg, on_signal, True)   # Disruptor background thread

runner.add_strategy(SMAcross([int(btc)]))
runner.start()

# Feed market data:
runner.on_trade(int(btc), price, qty, True, ts_ns)
runner.on_book_snapshot(int(btc), bid_prices, bid_qtys, ask_prices, ask_qtys, ts_ns)

runner.stop()
```

## Backtest

The Codon binding exposes the bare `BacktestRunner` — flat fee, no funding, no liquidation, no rate limits. Good for indicator sanity checks; not enough for a decision about real capital.

```codon
from lrvx.runner import BacktestRunner

bt = BacktestRunner(reg, fee_rate=0.0004, initial_capital=10_000.0)
bt.set_strategy(SMAcross([int(btc)]))

stats = bt.run_csv("/path/to/btcusdt_1m.csv", "BTCUSDT")
print(stats.return_pct, stats.sharpe_ratio, stats.max_drawdown_pct)
```

`run_csv` reads OHLCV **bars**: one header line, then `timestamp,open,high,low,close,volume`. Only the timestamp and close columns are used; each bar is replayed as one trade, so `on_trade` fires and `on_bar` does not.

Or pass raw arrays / a recorded tape:

```codon
stats = bt.run_ohlcv(timestamps_ns, closes, "BTCUSDT")
stats = bt.run_tape("/path/to/tape.lrvx")
```

The realistic venue stack (cross-margin account, MM tier ladder + ADL, VIP fee schedule, funding settlement, rate limits, venue availability) is a separate simulation in `lrvx.backtest`. It is not an argument to `BacktestRunner`:

```codon
from lrvx.backtest import binance_um_futures

stack = binance_um_futures(account_id=42, equity=10_000.0)
acct = stack.account_handle()   # raw handle; wrap with the matching codon class if needed
```

Codon has no `@classmethod`, so the factories are module-level functions: `binance_um_futures`, `bybit_linear`, `okx_swap`, `deribit`, `from_venue(name, account_id, equity)`. See [Realistic backtest in one call](../how-to/realistic-backtest.md).

### BacktestStats fields

| Field | Description |
|-------|-------------|
| `return_pct` | Net return percentage |
| `net_pnl` | Net P&L after fees |
| `total_trades` | Round-trip trade count |
| `win_rate` | Winning trade fraction |
| `sharpe_ratio` | Annualized Sharpe ratio |
| `sortino_ratio` | Annualized Sortino ratio |
| `calmar_ratio` | Calmar ratio |
| `max_drawdown_pct` | Peak-to-trough drawdown (%) |
| `profit_factor` | Gross profit / gross loss |
| `final_capital` | Capital at end of backtest |

## Indicators

Indicator classes offer both a batch (`compute`) and an incremental (`update` / `value` / `ready` / `reset`) surface:

```codon
from lrvx.indicators import SMA, EMA

sma = SMA(20)
val = sma.update(price)   # returns float; sma.ready is True once primed

ema = EMA(12)
val = ema.update(price)
```

Each instance holds one C handle (`lrvx_streaming_indicator_*`) that owns the C++ streaming indicator, and `update()` is one FFI call that runs one step of that indicator. No history is kept: the cost of an update depends on the indicator's window, never on how many values it has seen, and the values, `ready` and NaN behaviour are the C++ class's, bit for bit. `count` is the number of updates since construction or `reset()`.

`close()` releases the handle at once; an instance that is never closed is released by `__del__` when the collector reclaims it. One still reachable at program exit is not finalized and its memory goes back with the process. After `close()`, `value` is NaN, `ready` is False and `count` is 0.

Batch indicators via C API (`ema`, `sma`, `rsi`, `atr`, `macd`, `bollinger`) — see [Codon Indicators](../reference/codon/indicators.md).

## Full Example

```codon
from lrvx.runner import Runner, BacktestRunner, lrvx_registry_create, lrvx_registry_add_symbol
from lrvx.strategy import Strategy
from lrvx.context import SymbolContext
from lrvx.types import TradeData
from lrvx.indicators import SMA

class SMAcross(Strategy):
    fast: SMA
    slow: SMA

    def __init__(self, symbols: List[int]):
        super().__init__(symbols)
        self.fast = SMA(10)
        self.slow = SMA(30)

    def on_trade(self, ctx: SymbolContext, trade: TradeData):
        f = self.fast.update(trade.price.to_double())
        s = self.slow.update(trade.price.to_double())
        if not self.slow.ready:
            return
        if f > s and self.position() == 0.0:
            self.market_buy(0.01)
        elif f < s and self.position() > 0.0:
            self.close_position()

def on_signal(sig):
    print(f"signal  {sig.side}  qty={sig.quantity:.4f}  [{sig.order_type}]")

def main():
    reg = lrvx_registry_create()
    btc = lrvx_registry_add_symbol(reg, "binance".c_str(), "BTCUSDT".c_str(), 0.01)
    btc_id = int(btc)

    # --- Backtest ---
    bt = BacktestRunner(reg, fee_rate=0.0004, initial_capital=10_000.0)
    bt.set_strategy(SMAcross([btc_id]))
    stats = bt.run_csv("btcusdt_1m.csv", "BTCUSDT")
    print(f"Return: {stats.return_pct:.2f}%  Sharpe: {stats.sharpe_ratio:.3f}")

    # --- Live ---
    runner = Runner(reg, on_signal)
    runner.add_strategy(SMAcross([btc_id]))
    runner.start()
    # runner.on_trade(btc_id, 67000.0, 0.01, True, 0)
    runner.stop()

main()
```