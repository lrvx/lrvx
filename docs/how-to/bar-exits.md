# Exits for bar strategies

Three exit rules for a position opened by a bar strategy: a stop a fixed distance from the entry, a Chandelier exit that follows the best price, and a break-even trail. They live in `lrvx.exits` in Python and in `lrvx/strategy/exits.h` in C++, with the same arithmetic on both sides, and they carry no dependency on the engine: a rule takes the entry and the bars, and answers where the stop is and whether a bar reached it. The strategy keeps the entry logic, the ATR and the exit order.

## One shape

Every rule has the same four moves.

- `arm(side, entry_price, atr)` when the position opens, with the fill price and the ATR of that bar. `side` is `"long"` or `"short"` in Python and `Side::BUY` or `Side::SELL` in C++.
- `update(high, low, atr)` once per closed bar from the fill bar on. It returns `True` when the bar's range reached the stop: `low <= stop` for a long, `high >= stop` for a short.
- `stop` is the current level, `armed` whether a position is being watched.
- `disarm()` forgets the position. A hit does not disarm; the strategy exits and disarms, and the same hit is reported again until it does.

`update` tests the bar against the stop that was resting before the bar, the level the previous update left, and only then moves the stop with the bar. A bar cannot be stopped out by a level its own high raised, and a stop order resting at `stop` would have filled on the bar the test reports.

## The rules

| Rule | Stop | Moves |
|---|---|---|
| `AtrStop(atr_multiple)` | `atr_multiple` ATRs from the entry | Never |
| `ChandelierExit(lookback, atr_multiple)` | `atr_multiple` ATRs below the highest high since the entry (above the lowest low for a short), over the last `lookback` bars, 0 for all of them; starts the same distance from the entry | Only in the position's favour |
| `BreakEvenTrail(hard_stop_atr, break_even_atr, trail_atr)` | `hard_stop_atr` ATRs from the entry until the position has moved `break_even_atr` ATRs (the ATR at arm time) in its favour; then the entry price, or the best price seen minus `trail_atr` ATRs (the current ATR) when that is better | Only in the position's favour |

The ATR is the strategy's: feed `lrvx.ATR` (`indicator::ATR`) the same bars and pass its value in. Nothing in a rule is annualised or scaled; a rule works in the price units it is given.

## Where the entry price comes from

Under [the execution model](../explanation/reference-backtest.md) an order from the bar callback fills at the next bar's open. A strategy that buys from `on_bar` sees the position open on the following bar, and that bar's `open` is the entry: arm the rule there, then update it with the same bar's range, since the stop has been resting from the open.

## Example

The reference backtest's entry with and without a Chandelier exit, on the one-minute file that ships with the docs. The guarded run closes a position on the bar whose range reaches the stop and lets the cross close the rest.

```python
--8<-- "examples/python_bar_exits.py"
```

The same in C++, with the rule as a member next to the indicators:

```cpp
#include "lrvx/indicator/atr.h"
#include "lrvx/indicator/sma.h"
#include "lrvx/strategy/exits.h"
#include "lrvx/strategy/strategy.h"

class SmaCrossWithChandelier : public lrvx::Strategy
{
 public:
  using Strategy::Strategy;

 protected:
  void onSymbolBar(SymbolContext& ctx, const BarEvent& ev) override
  {
    const double high = ev.bar.high.toDouble();
    const double low = ev.bar.low.toDouble();
    const double close = ev.bar.close.toDouble();
    _fast.update(close);
    _slow.update(close);
    _atr.update(high, low, close);
    if (!_fast.ready() || !_slow.ready() || !_atr.ready())
    {
      return;
    }
    const double atr = _atr.value();

    if (ctx.position.raw() > 0)
    {
      if (!_exit.armed())
      {
        _exit.arm(lrvx::Side::BUY, ev.bar.open.toDouble(), atr);
      }
      if (_exit.update(high, low, atr))
      {
        emitClosePosition(ev.symbol);
        _exit.disarm();
        return;
      }
    }

    if (_fast.value() > _slow.value() && ctx.position.raw() == 0)
    {
      emitMarketBuy(ev.symbol, Quantity::fromDouble(0.01));
    }
    else if (_fast.value() < _slow.value() && ctx.position.raw() > 0)
    {
      emitClosePosition(ev.symbol);
      _exit.disarm();
    }
  }

 private:
  lrvx::indicator::SMA _fast{10};
  lrvx::indicator::SMA _slow{40};
  lrvx::indicator::ATR _atr{14};
  lrvx::exits::ChandelierExit _exit{0, 3.0};
};
```

## Sweeping the rule

A rule's parameters are numbers, so they go on a grid axis like any other: a `ChandelierExit(0, m)` per combination with `m` from the axis, inside the factory of [a sweep over bars](sweep-over-bars.md). The [overfitting checks](robustness-statistics.md) then apply to the exit parameters the same way they apply to the entry's.

## See also

- [Sweep parameters over bars](sweep-over-bars.md)
- [The reference backtest](../explanation/reference-backtest.md)
- [Advanced orders](advanced-orders.md) for the venue-side stop, take-profit and trailing-stop order types
