# Realistic backtest fills

Configure a backtest with realistic execution: slippage on market orders, queue simulation for limit orders, and an exported equity curve. Same model in every binding.

## What the fill model does before you configure anything

Six rules hold in every backtest, with or without a slippage profile or a queue model:

| Rule | Meaning |
|---|---|
| A bar-callback order fills at the next bar's open | The strategy is shown a bar the simulator has already walked, so an order it emits there is held and released at the next open for that symbol. One emitted on the last bar never fills. |
| A taker walks the book | A crossing order consumes the visible ladder and pays the volume-weighted price of the levels it took; what it ate is gone for the next order until the following snapshot. Size past the ladder pays the deepest visible level, for the whole order. |
| A triggered stop or target cannot fill better than its trigger | It may fill worse — a sell stop armed at 95 on a bar that traded to 90 still books 90 — but a take-profit armed at 105 books 105, not the bar's high. |
| A queue model fills on bars | A bar that trades strictly through a resting limit's price consumes that level's queue, per the configured model. A bar that only touches the price fills nothing. |
| An order priced off the tick is rejected | A limit price, stop or take-profit trigger, or trailing distance that is not a whole number of the symbol's ticks is refused at submit with `REJECTED` and a reason (`price_off_tick`, `trigger_off_tick`, `trailing_offset_off_tick`), as a venue refuses it. A symbol registered without a tick is not checked. |
| Each run starts clean | A second `run()` / `run_bars()` on the same runner reports that run, not the sum of every run so far. Configuration survives; state does not. |

A strategy that prices an exit from its fill, such as a take-profit at `entry * 1.005`, rounds the
result to the tick before sending it: `round(entry * 1.005 / tick) * tick`, with `tick` read from
`Symbol.tick_size` in Python.

Prices the simulator computes itself -- a fill moved by a slippage model, a percentage trailing
trigger -- are snapped onto the tick against the strategy instead.

Details and the exact bounds: [SimulatedExecutor](../reference/api/backtest/simulated_executor.md#execution-logic),
[Queue simulation](../reference/api/backtest/queue_simulation.md#bar-data),
[BacktestRunner](../reference/api/backtest/backtest_runner.md#bar-callbacks-fill-at-the-next-open).

## 1. Configure the simulator

`BacktestRunner` takes the default slippage profile as constructor arguments --
`slippage_model` (`none`, `fixed_ticks`, `fixed_bps`, `volume_impact`) and the
one parameter that model reads: `slippage_ticks` with `slippage_tick_size`,
`slippage_bps`, or `slippage_impact_coeff` (camelCase in Node). Every model
moves the fill against the taker. A parameter the chosen model does not read is
refused rather than ignored, and `volume_impact` is refused by the bar and
trade-only runs (`run_bars`, `run_tape_bars`, `run_csv`, `run_ohlcv`), which
carry no book depth for it to divide by.

=== "Python"

    ```python
    import lrvx

    bt = lrvx.BacktestRunner(reg, fee_rate=0.0002, initial_capital=100_000,
                             slippage_model="fixed_bps", slippage_bps=1.0)
    ```

=== "Node.js"

    ```javascript
    const bt = new lrvx.BacktestRunner(reg, 0.0002, 100_000,
        {slippageModel: "fixed_bps", slippageBps: 1.0});
    ```

=== "Codon"

    ```python
    bt = BacktestRunner(reg, 0.0002, 100_000.0,
                        slippage_model="fixed_bps", slippage_bps=1.0)
    ```

The queue model, per-symbol slippage and the rest of the executor's knobs are
set on a `SimulatedExecutor` you drive yourself, or through `BacktestConfig` in
C++:

=== "Python"

    ```python
    import lrvx

    ex = lrvx.SimulatedExecutor()
    ex.set_default_slippage("fixed_bps", bps=1.0)   # 1 bps default
    ex.set_queue_model("tob")                       # top-of-book queue
    # Per-symbol override:
    ex.set_symbol_slippage(eth_usd, "volume_impact", impact_coeff=0.01)
    ```

=== "Node.js"

    ```javascript
    const ex = new lrvx.SimulatedExecutor();
    ex.setDefaultSlippage("fixed_bps", 0, 0, 1.0, 0);    // ticks, tickSize, bps, impactCoeff
    ex.setQueueModel("tob", 1);
    ```

=== "Codon"

    ```python
    from lrvx.backtest import SimulatedExecutor, SLIPPAGE_FIXED_BPS, QUEUE_TOB

    ex = SimulatedExecutor()
    ex.set_default_slippage(SLIPPAGE_FIXED_BPS, 0, 0.0, 1.0, 0.0)
    ex.set_queue_model(QUEUE_TOB, 1)
    ```

=== "C++"

    ```cpp
    #include "lrvx/backtest/backtest_config.h"
    #include "lrvx/backtest/backtest_runner.h"

    lrvx::BacktestConfig cfg;
    cfg.initialCapital = 100'000.0;
    cfg.feeRate = 0.0002;                                                                 // 2 bps per fill
    cfg.defaultSlippage = { lrvx::SlippageModel::FIXED_BPS, 0, lrvx::Price{}, 1.0, 0.0 };  // 1 bps default
    cfg.queueModel = lrvx::QueueModel::TOB;
    cfg.riskFreeRate = 0.0;
    cfg.metricsAnnualizationFactor = 252.0;

    lrvx::BacktestRunner runner(cfg);

    cfg.perSymbolSlippage.emplace_back(
        kEthUsd, lrvx::SlippageProfile{lrvx::SlippageModel::VOLUME_IMPACT, 0, 0.0, 0.01});
    ```

## 2. Run it

=== "Python"

    ```python
    bt = lrvx.BacktestRunner(reg, fee_rate=0.0002, initial_capital=100_000,
                             slippage_model="fixed_bps", slippage_bps=1.0)
    bt.set_strategy(my_strategy)
    stats = bt.run_tape_bars("data/tape", interval_seconds=300, symbol="BTCUSDT")
    ```

=== "Node.js"

    ```javascript
    const bt = new lrvx.BacktestRunner(reg, 0.0002, 100_000,
        {slippageModel: "fixed_bps", slippageBps: 1.0});
    bt.setStrategy(myStrategy);
    const stats = bt.runTapeBars("data/tape", 300, "BTCUSDT");
    ```

=== "C++"

    ```cpp
    runner.setStrategy(&yourStrategy);
    auto result = runner.run(*reader);   // reader: replay::IMultiSegmentReader
    auto stats  = result.computeStats();
    ```

## 3. Inspect stats

These are the fields on the dict returned by `run_csv` / `run_ohlcv` / `run_bars` / `run_tape` / `run_tapes` (snake_case in Python/Codon, camelCase in Node, `BacktestStats` struct in C++).

| Python / Codon | Node.js | Description |
|---|---|---|
| `total_trades` | `totalTrades` | Number of closed trades |
| `net_pnl` | `netPnl` | Realized P&L of the closed trades minus the fees of every fill, including the entry fee of a position still open at the end ([details](../reference/python/strategy.md#a-position-open-at-the-end)) |
| `return_pct` | `returnPct` | Total return % |
| `sharpe_ratio` | `sharpeRatio` | Annualised Sharpe |
| `sortino_ratio` | `sortinoRatio` | Annualised Sortino |
| `calmar_ratio` | `calmarRatio` | Calmar ratio |
| `max_drawdown_pct` | `maxDrawdownPct` | Worst drawdown |
| `win_rate` | `winRate` | Win rate |
| `profit_factor` | `profitFactor` | Gross profit / gross loss |

`BacktestResult.stats()` is a *different* dict with a wider field set, but the same key names. See [Running a backtest](backtest.md#two-stats-shapes-one-set-of-key-names).

## 4. Export the equity curve

=== "Python"

    ```python
    curve = bt.equity_curve()   # dict of numpy arrays
    ts, eq, dd = curve["timestamp_ns"], curve["equity"], curve["drawdown_pct"]

    # BacktestRunner has no CSV writer. write_equity_curve_csv lives on
    # BacktestResult, which you drive from a SimulatedExecutor:
    res = lrvx.BacktestResult(initial_capital=100_000.0, fee_rate=0.0002)
    res.ingest_executor(ex)
    res.write_equity_curve_csv("equity.csv")   # or res.equity_curve() -> structured array
    ```

=== "Node.js"

    ```javascript
    const curve = bt.equityCurve();   // { timestampNs, equity, drawdownPct }

    // Node has no equity-curve CSV writer on either BacktestRunner or
    // BacktestResult. Write the rows yourself from the arrays above.
    ```

=== "C++"

    ```cpp
    for (const auto& pt : result.equityCurve()) {
      fmt::print("{},{:.2f},{:.2f}\n", pt.timestampNs, pt.equity, pt.drawdownPct);
    }
    result.writeEquityCurveCsv("equity.csv");
    ```

CSV header: `timestamp_ns,equity,drawdown_pct`. One row per closed trade.

## See also

- [Backtesting](backtest.md)
- [Slippage reference](../reference/api/backtest/slippage.md)
- [Queue simulation](../reference/api/backtest/queue_simulation.md)
