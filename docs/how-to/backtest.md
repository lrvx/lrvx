# Running a Backtest

Replay an SMA crossover strategy against historical data and read the stats out.

## Build / install

=== "Python"

    ```bash
    pip install lrvx
    ```

    Or build from source: `cmake -B build -DLRVX_BUILD_PYTHON=ON -DLRVX_ENABLE_BACKTEST=ON && cmake --build build` and put `build/python` on `PYTHONPATH`.

=== "Node.js"

    ```bash
    npm install @lrvx/lrvx
    ```

=== "Codon"

    Build lrvx with `-DLRVX_BUILD_CAPI=ON -DLRVX_ENABLE_BACKTEST=ON`, then point Codon at `codon/lrvx`.

=== "C++"

    ```bash
    cmake -B build -DLRVX_ENABLE_BACKTEST=ON -DCMAKE_BUILD_TYPE=Release
    cmake --build build -j
    ```

## Strategy

A 10/20 SMA crossover. Buy when fast crosses above slow, sell on the reverse.

=== "Python"

    ```python
    import lrvx
    from collections import deque

    class SmaCrossover(lrvx.Strategy):
        def __init__(self, symbols, fast=10, slow=20, size=1.0):
            super().__init__(symbols)
            self.fast, self.slow, self.size = fast, slow, size
            self.prices = deque(maxlen=slow)
            self.prev_above = False

        def on_trade(self, ctx, trade):
            self.prices.append(trade.price)
            if len(self.prices) < self.slow:
                return
            fast_sma = sum(list(self.prices)[-self.fast:]) / self.fast
            slow_sma = sum(self.prices) / self.slow
            above = fast_sma > slow_sma
            if above and not self.prev_above and ctx.is_flat():
                self.market_buy(self.size)
            elif not above and self.prev_above and ctx.is_long():
                self.market_sell(self.size)
            self.prev_above = above
    ```

=== "Node.js"

    ```javascript
    const lrvx = require('@lrvx/lrvx');

    class SmaCrossover {
      constructor(symbols, fast = 10, slow = 20, size = 1.0) {
        this.symbols = symbols;
        this.fast = fast; this.slow = slow; this.size = size;
        this.prices = []; this.prevAbove = false;
      }
      onTrade(ctx, trade, emit) {
        this.prices.push(trade.price);
        if (this.prices.length > this.slow) this.prices.shift();
        if (this.prices.length < this.slow) return;
        const fastSma = this.prices.slice(-this.fast).reduce((a,b)=>a+b)/this.fast;
        const slowSma = this.prices.reduce((a,b)=>a+b)/this.slow;
        const above = fastSma > slowSma;
        if (above && !this.prevAbove && ctx.position === 0) emit.marketBuy(this.size);
        else if (!above && this.prevAbove && ctx.position > 0) emit.marketSell(this.size);
        this.prevAbove = above;
      }
    }
    ```

=== "Codon"

    ```python
    from lrvx.strategy import Strategy
    from lrvx.context import SymbolContext
    from lrvx.types import TradeData
    from lrvx.indicators import SMA

    class SmaCrossover(Strategy):
        fast: SMA
        slow: SMA
        size: float
        prev_above: bool

        def __init__(self, symbols: List[int], fast_n: int = 10, slow_n: int = 20, size: float = 1.0):
            super().__init__(symbols)
            self.fast = SMA(fast_n)
            self.slow = SMA(slow_n)
            self.size = size
            self.prev_above = False

        def on_trade(self, ctx: SymbolContext, trade: TradeData):
            f = self.fast.update(trade.price.to_double())
            s = self.slow.update(trade.price.to_double())
            if not self.slow.ready:
                return
            above = f > s
            if above and not self.prev_above and self.position() == 0.0:
                self.market_buy(self.size)
            elif not above and self.prev_above and self.position() > 0.0:
                self.market_sell(self.size)
            self.prev_above = above
    ```

=== "C++"

    ```cpp
    #include "lrvx/strategy/strategy.h"
    #include <deque>

    using namespace lrvx;

    class SmaCrossover : public Strategy {
    public:
      SmaCrossover(SymbolId symbol, size_t fast, size_t slow, Quantity size,
                   const SymbolRegistry& registry)
          : Strategy(1, symbol, registry), _fast(fast), _slow(slow), _size(size) {}

      void start() override { _running = true; }
      void stop() override  { _running = false; }

    protected:
      void onSymbolTrade(SymbolContext& /*ctx*/, const TradeEvent& ev) override
      {
        if (!_running) return;
        _prices.push_back(ev.trade.price.toDouble());
        if (_prices.size() > _slow) _prices.pop_front();
        if (_prices.size() < _slow) return;

        double fast_sma = sma(_fast), slow_sma = sma(_slow);
        bool above = fast_sma > slow_sma;
        if (above && !_prev_above && !_long) { emitMarketBuy(symbol(), _size); _long = true; _short = false; }
        else if (!above && _prev_above && !_short) { emitMarketSell(symbol(), _size); _short = true; _long = false; }
        _prev_above = above;
      }

    private:
      double sma(size_t n) const {
        double sum = 0; auto it = _prices.end();
        for (size_t i = 0; i < n; ++i) sum += *--it;
        return sum / n;
      }
      size_t _fast, _slow;
      Quantity _size;
      std::deque<double> _prices;
      bool _running{false}, _prev_above{false}, _long{false}, _short{false};
    };
    ```

## Run the backtest

=== "Python"

    ```python
    reg = lrvx.SymbolRegistry()
    btc = reg.add_symbol("binance", "BTCUSDT", tick_size=0.01)

    strat = SmaCrossover([btc])
    bt = lrvx.BacktestRunner(reg, fee_rate=0.0004, initial_capital=10_000)
    bt.set_strategy(strat)

    stats = bt.run_csv("data/btcusdt_1m.csv", "BTCUSDT")
    print(f"Final ${stats['final_capital']:.2f}  return {stats['return_pct']:.2f}%  "
          f"trades {stats['total_trades']}  Sharpe {stats['sharpe_ratio']:.2f}  "
          f"DD {stats['max_drawdown_pct']:.2f}%")
    ```

=== "Node.js"

    ```javascript
    const reg = new lrvx.SymbolRegistry();
    const btc = reg.addSymbol("binance", "BTCUSDT", 0.01);

    const strat = new SmaCrossover([btc]);
    const bt = new lrvx.BacktestRunner(reg, 0.0004, 10000);
    bt.setStrategy(strat);

    const stats = bt.runCsv("data/btcusdt_1m.csv", "BTCUSDT");
    console.log(`Final $${stats.finalCapital.toFixed(2)}  return ${stats.returnPct.toFixed(2)}%  ` +
                `trades ${stats.totalTrades}  Sharpe ${stats.sharpeRatio.toFixed(2)}  ` +
                `DD ${stats.maxDrawdownPct.toFixed(2)}%`);
    ```

=== "Codon"

    ```python
    from lrvx.runner import BacktestRunner

    reg = ...                                # see lrvx.engine in your runner code
    btc = reg.add_symbol("binance", "BTCUSDT", 0.01)

    strat = SmaCrossover([btc])
    bt = BacktestRunner(reg, fee_rate=0.0004, initial_capital=10_000.0)
    bt.set_strategy(strat)
    stats = bt.run_csv("data/btcusdt_1m.csv", "BTCUSDT")
    print(f"Final ${stats.final_capital:.2f}  return {stats.return_pct:.2f}%")
    ```

=== "C++"

    ```cpp
    #include "lrvx/backtest/backtest_runner.h"
    #include "lrvx/replay/abstract_event_reader.h"

    int main() {
      replay::ReaderFilter filter;
      filter.symbols = {1};
      auto reader = replay::createMultiSegmentReader("/data/btcusdt", filter);

      BacktestConfig config{ .initialCapital = 10000.0, .feeRate = 0.0004 };
      BacktestRunner runner(config);

      SymbolRegistry registry;
      SymbolInfo info{ .exchange = "binance", .symbol = "BTCUSDT",
                        .tickSize = Price::fromDouble(0.01) };
      registry.registerSymbol(info);

      SmaCrossover strat(1, 10, 20, Quantity::fromDouble(1.0), registry);
      runner.setStrategy(&strat);

      auto result = runner.run(*reader);
      auto stats = result.computeStats();
      std::cout << "Final " << stats.finalCapital
                << "  return " << stats.returnPct << "%\n";
    }
    ```

## Bars from a tape in one call

A bar strategy over a recorded `.lrvx` tape is one call: `run_tape_bars`
aggregates the tape's trades into time bars while it streams and hands each
closed bar to `on_bar`. The trades never reach Python, so a three-month tape
runs in the memory its bars need.

=== "Python"

    ```python
    bt = lrvx.BacktestRunner(reg, fee_rate=0.0004, initial_capital=10_000,
                             slippage_model="fixed_bps", slippage_bps=1.0)
    bt.set_strategy(strat)
    stats = bt.run_tape_bars("data/tape", interval_seconds=300, symbol="BTCUSDT")
    ```

=== "Node.js"

    ```javascript
    const bt = new lrvx.BacktestRunner(reg, 0.0004, 10000,
        {slippageModel: "fixed_bps", slippageBps: 1.0});
    bt.setStrategy(strat);
    const stats = bt.runTapeBars("data/tape", 300, "BTCUSDT");
    ```

=== "Codon"

    ```python
    bt = BacktestRunner(reg, 0.0004, 10_000.0,
                        slippage_model="fixed_bps", slippage_bps=1.0)
    bt.set_strategy(strat)
    stats = bt.run_tape_bars("data/tape", 300.0, "BTCUSDT")
    ```

=== "C++"

    ```cpp
    BacktestRunner runner(config);
    runner.setStrategy(&strat);
    auto result = runner.runTapeBars("data/tape", std::chrono::minutes(5), btcId);
    ```

What it does, exactly:

- Bars are wall-clock aligned (a 300 s bar starts on a multiple of five
  minutes, UTC), built by the same time-bar policy as `aggregate_time_bars`: a
  trade later than its bar's bucket is dropped, and the last bar, still open
  when the tape ends, is not delivered. An interval with no trades has no bar.
- The executor is fed the bars, not the ticks, so the fill rules are
  `run_bars`' rules: an order sent from `on_bar` fills at that symbol's next
  bar open, and resting and conditional orders walk open, low, high, close.
  `on_trade` does not fire, and book records in the tape are not used.
- One symbol per run. `symbol` names the registered symbol whose registry id is
  the tape's symbol id; it may be left out only when the tape holds a single
  symbol, and a tape with more than one raises instead of mixing them.
- Each bar carries `bar_type` 0 (time), `bar_type_param` = the interval in
  nanoseconds, and its buy volume, as a live time aggregator would set them.

The result is the result of the explicit route -- `DataReader.read_trades`,
`aggregate_time_bars`, `run_bars` -- for the same interval: same fills, same
`trades()`, same `equity_curve()`. This example runs both and checks it:

```python
--8<-- "examples/python_run_tape_bars.py"
```

Two differences a strategy can see, neither of which reaches a fill: the
explicit route passes no buy volume (`run_bars` takes none), and it carries
each bar's volume through a float, so a bar whose notional exceeds about
9e7 quote units arrives rounded to the nearest representable value where the
one-call route delivers it exact. Use the explicit form when the bars are
reused across many runs -- load or aggregate them once and hand the same arrays
to every run, as in [Sweep parameters over bars](sweep-over-bars.md).

Slippage and latency arguments are covered in
[Realistic backtest fills](backtest-realistic-fills.md) and
[Add latency to a backtest](backtest-with-latency.md). On bars, any positive
submit latency moves a fill to a later bar's open:
[Latency on bar replay](backtest-with-latency.md#latency-on-bar-replay).

## Output

```
Final $10245.30  return 2.45%  trades 47  Sharpe 1.23  DD 3.21%
```

### Two stats shapes, one set of key names

Every producer names the risk ratios the same way: `sharpe_ratio` /
`sortino_ratio` / `calmar_ratio` in Python and Codon, `sharpeRatio` /
`sortinoRatio` / `calmarRatio` in Node. The C++ `BacktestStats` field name is
the source both are derived from, so the snake_case to camelCase rule holds
with no exceptions.

What still differs between producers is the *field set*, not the names:

| Producer | Field set |
|---|---|
| `BacktestRunner.run_csv` / `run_ohlcv` / `run_bars` / `run_tape` / `run_tapes` | Headline metrics plus all three risk ratios |
| `BacktestResult.stats()` | The full set |
| `GridSearch.run()[i]["stats"]` | Python: whatever the factory returned (normally the `run_*` shape). Node: the fold shape |
| `WalkForwardRunner` fold stats | The fold shape: the `run_*` set minus `avg_win` / `avg_loss` |

`BacktestResult.stats()` is the only one carrying the full metric set:
`time_weighted_return`, `avg_win_loss_ratio`, `max_consecutive_wins` /
`max_consecutive_losses`, `avg_trade_duration_ns` /
`median_trade_duration_ns` / `max_trade_duration_ns`, `start_time_ns` and
`end_time_ns`. In Node these three shapes are the `BacktestStats`,
`RunnerStats` and `FoldStats` interfaces in `index.d.ts`.

## Fees: one rate, or one per side

`fee_rate` charges every fill the same. That is a maker/taker *average*, and it
understates any strategy that mostly posts on a venue where making is cheaper
than taking — sometimes a rebate.

Fills carry `is_maker`, so both sides can be priced separately:

=== "Python"

    ```python
    r = lrvx.BacktestResult(
        initial_capital=10_000.0,
        fee_rate=0.0004,          # still the fallback for either side
        maker_fee_rate=0.0001,    # charged when the fill provided liquidity
        taker_fee_rate=0.0005,    # charged when it took liquidity
    )
    ```

=== "C++"

    ```cpp
    BacktestConfig config{};
    config.feeRate = 0.0004;
    config.makerFeeRate = 0.0001;
    config.takerFeeRate = 0.0005;
    ```

Both default to *unset* (`None` in Python, an empty `std::optional` in C++),
in which case `fee_rate` applies to both sides and results are unchanged. A
rate you set is used as given, sign and all: zero is a real maker rate, and a
negative one is a rebate the venue pays you for posting. Fixed-fee mode
(`use_percentage_fee=False`) ignores the side entirely.

=== "Python"

    ```python
    r = lrvx.BacktestResult(
        initial_capital=10_000.0,
        fee_rate=0.0004,
        maker_fee_rate=-0.00005,  # the venue pays 0.5 bp for posting
        taker_fee_rate=0.0005,
    )
    ```

=== "C++"

    ```cpp
    BacktestConfig config{};
    config.feeRate = 0.0004;
    config.makerFeeRate = -0.00005;
    config.takerFeeRate = 0.0005;
    ```

`is_maker` is also on every fill in `fills_list()` and in the `fills()` numpy
dtype, so a fee model of your own can read it directly. What this does *not*
give you is a venue's 30-day volume tiering — for that see
[Realistic backtest in one call](realistic-backtest.md).

## Pre-aggregated bars (faster replay)

For repeated parameter sweeps over the same data, pre-aggregate bars once and replay them. Python (`run_bars`), Node.js (`runBars`) and Codon (`run_bars`) all take closed OHLC bars; C++ has dedicated mmap storage.

`runBars` is implemented by the Node addon but is not declared in `index.d.ts`, so TypeScript callers need a cast.

=== "Python"

    ```python
    import numpy as np
    bt.run_bars(
        start_time_ns = ts_start_arr.astype(np.int64),
        end_time_ns   = ts_end_arr.astype(np.int64),
        open  = opens,  high = highs, low = lows, close = closes, volume = vols,
        symbol = "BTCUSDT",
    )
    ```

=== "Node.js"

    ```javascript
    // startNs / endNs are BigInt64Array; OHLCV arrays are Float64Array.
    bt.runBars(startNs, endNs, opens, highs, lows, closes, vols, "BTCUSDT");
    ```

=== "C++"

    Pre-aggregate offline with the `preagg_bars` tool, then replay via `MmapBarReplaySource`:

    ```cpp
    MmapBarStorage storage("/data/BTCUSDT/bars");
    MmapBarReplaySource source(storage, symbol_id);
    source.replay([&](const BarEvent& ev) { strat.onBar(ev); });
    ```

## See Also

- [Grid Search Optimization](grid-search.md) — parameter optimization
- [Bar Aggregation Pipeline](bar-aggregation.md) — pre-aggregating bars
- [Interactive Mode](interactive-backtest.md) — step-by-step execution
- [Realistic fills](backtest-realistic-fills.md) — slippage and queue position
