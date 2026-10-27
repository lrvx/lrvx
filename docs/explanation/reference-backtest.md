# The reference backtest

One strategy, one data file, one configuration, and the numbers the engine produces for them. It is the check to run against any other engine, and the fixed point every change to the execution model has to move on purpose: a C++ test and a Python test pin every value on this page, and the runnable example below fails when they drift.

## What is fixed

| | |
|---|---|
| Data | `docs/examples/data/btcusdt_1m.csv`: 3000 one-minute bars, millisecond timestamps, `timestamp,open,high,low,close,volume` |
| Strategy | SMA(10) and SMA(40) over the close; long 0.01 while the fast average is above the slow one, flat otherwise; no stops |
| Entry | `market_buy(0.01)` from the bar callback when flat and fast is above slow |
| Exit | `close_position()` from the bar callback when long and fast is below slow |
| Fees | taker 0.0004 of notional on every fill (`fee_rate=0.0004`) |
| Capital | 10 000 |
| Replay | `run_bars` over the arrays, nanosecond timestamps, one symbol |

## How a bar run executes

These are the rules the numbers below follow. They are the behaviour of `BacktestRunner.run_bars` and `SimulatedExecutor`; the tests hold them.

- **A bar is walked before the strategy sees it.** For each bar the simulator takes the open, then the low, then the high, then the close into its market state. Resting stops and take-profits match against the intrabar low and high, not only the close.
- **The strategy is called once per bar, after the walk.** `on_bar` runs with the bar fully in the past, and every price it can react to is a price that has already printed.
- **An order from the callback fills at the next bar's open.** While the callback runs, every order it submits is held; the next bar for that symbol releases it at its open step, where a market order takes the open price. Nothing fills on the bar that produced the signal, and an order submitted from the last bar's callback never fills.
- **Fill timestamps are bar end times.** The runner moves its clock to a bar's end time before walking it, so a fill in bar N carries bar N's end time, not the signal bar's.
- **Fees are charged per fill on notional.** A round trip pays the rate twice: once on the entry notional and once on the exit notional.
- **A trade is an entry fill matched with the exit fill that flattens it.** `pnl` is price difference times quantity, before fees; `fee` is the two fills' fees; `net_pnl` across the run is `total_pnl` minus `total_fees`, and `final_capital` is the initial capital plus `net_pnl`.

In this run SMA(40) is ready on the 40th close (bar index 39). The buy that close produces fills at the open of bar 40, 75822.85, and the trade's `entry_time_ns` is bar 40's end time.

## The numbers

| Statistic | Value |
|---|---|
| `total_trades` | 47 |
| `winning_trades` / `losing_trades` | 16 / 31 |
| `gross_profit` / `gross_loss` | 28.0608 / 42.7754 |
| `total_pnl` | -14.7146 |
| `total_fees` | 28.37314883 |
| `net_pnl` | -43.08774883 |
| `final_capital` | 9956.91225117 |
| `return_pct` | -0.4308774883 |

| Trade | Entry time (ns) | Entry | Exit time (ns) | Exit | `pnl` | `fee` |
|---|---|---|---|---|---|---|
| first | 1776609420000000000 | 75822.85 | 1776611280000000000 | 75909.76 | 0.8691 | 0.60693044 |
| last | 1776781140000000000 | 76185.83 | 1776783420000000000 | 76005.21 | -1.8062 | 0.60876416 |

The strategy loses money on this file. That is not what the page is for: the figures are a fixed point, not a result.

## The run

The example loads the file, runs the strategy and compares what it gets with the table above; it exits non-zero on any difference.

```python
--8<-- "examples/python_reference_backtest.py"
```

The same strategy in C++ runs through `BacktestRunner::runBars` and is pinned to the same values in `tests/test_reference_backtest.cpp`:

```cpp
#include "lrvx/indicator/sma.h"
#include "lrvx/strategy/strategy.h"

class SmaCross : public lrvx::Strategy
{
 public:
  using Strategy::Strategy;

 protected:
  void onSymbolBar(SymbolContext& ctx, const BarEvent& ev) override
  {
    const double close = ev.bar.close.toDouble();
    _fast.update(close);
    _slow.update(close);
    if (!_fast.ready() || !_slow.ready())
    {
      return;
    }
    const double f = _fast.value();
    const double s = _slow.value();
    if (f > s && ctx.position.raw() == 0)
    {
      emitMarketBuy(ev.symbol, Quantity::fromDouble(0.01));
    }
    else if (f < s && ctx.position.raw() > 0)
    {
      emitClosePosition(ev.symbol);
    }
  }

 private:
  lrvx::indicator::SMA _fast{10};
  lrvx::indicator::SMA _slow{40};
};
```

## Checking another engine against it

Run the same rules on the same file: fill at the next bar's open, fee on every fill's notional, trades closed by the fill that flattens. Compare the trade list before the statistics: a mismatch in `total_trades` or in the first entry's time and price locates the convention that differs, where a mismatch in `return_pct` alone does not.

## See also

- [Sweep parameters over bars](../how-to/sweep-over-bars.md)
- [Backtesting](../how-to/backtest.md)
- [Realistic fills](../how-to/backtest-realistic-fills.md)
- [Replay-equivalence gate](replay-equivalence-gate.md)
