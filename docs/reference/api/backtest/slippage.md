# Slippage

The backtest engine applies slippage to market-style fills: market orders, triggered stop-market orders, triggered take-profit-market orders, and triggered trailing stops (`fillsAsMarket` in `simulated_executor.cpp`). Limit orders trade at their posted price; queue simulation handles their realism separately.

Slippage is configured per `BacktestConfig` with a default profile and optional per-symbol overrides.

## SlippageProfile

```cpp
enum class SlippageModel : uint8_t { NONE, FIXED_TICKS, FIXED_BPS, VOLUME_IMPACT };

struct SlippageProfile
{
  SlippageModel model{SlippageModel::NONE};
  int32_t ticks{0};         // FIXED_TICKS: number of ticks against the taker
  Price tickSize{};         // FIXED_TICKS: price per tick; zero falls back to one raw price unit
  double bps{0.0};          // FIXED_BPS: basis points against the taker
  double impactCoeff{0.0};  // VOLUME_IMPACT: coeff * (orderQty / levelQty)
};
```

| Model | Meaning |
|-------|---------|
| `NONE` | Fill at the book-reported best price. |
| `FIXED_TICKS` | Shift the fill by `ticks * tickSize` against the taker (buys pay more, sells receive less). Set `tickSize` to the venue's minimum price increment; leaving it zero falls back to one raw price unit, which is rarely what you want for real instruments. |
| `FIXED_BPS` | Shift the fill by `bps` basis points of the fill price. `bps = 10.0` is 10 bps = 0.10%. |
| `VOLUME_IMPACT` | Shift proportional to `impactCoeff * (orderQty / levelQty)`. Larger orders relative to top-of-book size move the price more. |

## Slippage and the tick

`FIXED_BPS` and `VOLUME_IMPACT` move a price by arithmetic, and `FIXED_TICKS` with a `tickSize` that is not the symbol's tick does too, so the result is usually not a price the venue could trade at. When the simulator knows the symbol's tick (see [SimulatedExecutor](./simulated_executor.md#order-prices-and-the-tick)), a fill the model moved is snapped back onto the tick against the taker: a buy up to the next tick, a sell down to the previous one. With a 0.1 tick and 7 bps, a buy at 100.0 fills at 100.1 rather than 100.07, and a sell at 100.0 at 99.9 rather than 99.93.

Two fills are not snapped:

- A fill priced over several book levels is a volume-weighted average, not a traded price. It is reported as the average plus the model's offset.
- A fill on a symbol whose tick is not known (registered without one, or with a tick of zero) keeps the model's price.

## Applying slippage

```cpp
BacktestConfig cfg;
cfg.defaultSlippage = {SlippageModel::FIXED_BPS, 0, Price{}, 5.0, 0.0};  // 5 bps default
cfg.perSymbolSlippage.emplace_back(
    kBtcUsd, SlippageProfile{SlippageModel::VOLUME_IMPACT, 0, Price{}, 0.0, 0.01});

BacktestRunner runner(cfg);  // config is forwarded to the simulated executor
```

For direct-executor use:

```cpp
SimulatedExecutor exec(clock);
exec.setDefaultSlippage({SlippageModel::FIXED_BPS, 0, Price{}, 5.0, 0.0});
exec.setSymbolSlippage(kBtcUsd,
                       {SlippageModel::FIXED_TICKS, 3, Price::fromDouble(0.25), 0.0, 0.0});
```

## Bindings

| Language | Entry point |
|----------|-------------|
| Python | `SimulatedExecutor.set_default_slippage(model, ticks, tick_size, bps, impact_coeff)` / `set_symbol_slippage(symbol, ...)` |
| C API | `lrvx_simulated_executor_set_default_slippage(exec, model, ticks, tick_size, bps, impact)` |
| Codon | `SimulatedExecutor.set_default_slippage(model, ticks, tick_size, bps, impact_coeff)` |
| JavaScript | `executor.setDefaultSlippage("fixed_bps", 0, 0, 5.0, 0)` |

All bindings accept the same model values (`none`, `fixed_ticks`, `fixed_bps`, `volume_impact`) either as strings (Python, JS, Codon enum constants) or as the `LrvxSlippageModel` enum in the C API. Pass `tick_size = 0.0` for `NONE` / `FIXED_BPS` / `VOLUME_IMPACT`; it is only meaningful for `FIXED_TICKS`.
