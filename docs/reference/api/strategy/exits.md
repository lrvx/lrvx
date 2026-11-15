# Exit rules

`lrvx/strategy/exits.h`, namespace `lrvx::exits`. Six exit rules for a position opened by a bar strategy, with one shape: `arm` when the position opens, `update` once per closed bar, the level, `armed()`, `disarm()`. The three ATR rules take an ATR; `PercentStop` and `TakeProfit` take only prices; `TimeStop` takes neither. The guide is [Exits for bar strategies](../../../how-to/bar-exits.md).

Every binding carries the six classes with this arithmetic: Python in `lrvx.exits` (pure Python, same expressions), Node and QuickJS as `AtrStop`, `ChandelierExit`, `BreakEvenTrail`, `PercentStop`, `TakeProfit`, `TimeStop`, Codon in `lrvx.exits`, and the C API as `LrvxExitRuleHandle` (`lrvx_exit_*`). The side is `"long"` / `"short"` in the bindings, `Side::BUY` / `Side::SELL` here.

## Common contract

```cpp
void arm(Side side, double entryPrice, double atr);
bool update(double high, double low, double atr);  // true when the bar reached the stop
double stop() const;
bool armed() const;
void disarm();
```

`update` tests the bar against the stop that was resting before the bar, then moves the stop with the bar. A long is stopped when `low <= stop()`, a short when `high >= stop()`. A hit does not disarm. `stop()` is NaN while disarmed.

## AtrStop

```cpp
explicit AtrStop(double atrMultiple);
double atrMultiple() const;
```

The stop is `entryPrice - atrMultiple * atr` for a long and `entryPrice + atrMultiple * atr` for a short, fixed at `arm`.

## ChandelierExit

```cpp
ChandelierExit(std::size_t lookback, double atrMultiple);
std::size_t lookback() const;
double atrMultiple() const;
```

The stop starts `atrMultiple` ATRs from the entry. Each `update` records the bar's high (low, for a short), keeps the last `lookback` of them (all of them when `lookback` is 0), and raises the stop to the highest of them minus `atrMultiple` times the bar's ATR when that is higher (lowers it to the lowest plus the same for a short). The stop never moves against the position.

## BreakEvenTrail

```cpp
BreakEvenTrail(double hardStopAtr, double breakEvenAtr, double trailAtr);
bool atBreakEven() const;
```

The stop starts `hardStopAtr` ATRs from the entry. Once the best price seen has moved `breakEvenAtr * atr` (the ATR passed to `arm`) in the position's favour, `atBreakEven()` turns true and the stop becomes the better of the entry price and the best price minus `trailAtr` times the current bar's ATR (plus, for a short), never moving against the position afterwards.

## PercentStop

```cpp
explicit PercentStop(double pct);  // std::invalid_argument unless pct > 0
void arm(Side side, double entryPrice);
bool update(double high, double low);
double stop() const;
bool armed() const;
void disarm();
double pct() const;
```

The stop is `entryPrice * (1 - pct / 100)` for a long and `entryPrice * (1 + pct / 100)` for a short, fixed at `arm`. A long is stopped when `low <= stop()`, a short when `high >= stop()`. As with the ATR rules, `update` reports whether this bar reached the stop; a hit leaves the rule armed and the stop where it was. `PercentStop(2.5)` armed short at 100 gives exactly `100 * (1 + 2.5 / 100)`, which is `102.49999999999999`; every binding computes the same expression and gets the same bits.

## TakeProfit

```cpp
explicit TakeProfit(double pct);  // std::invalid_argument unless pct > 0
void arm(Side side, double entryPrice);
bool update(double high, double low);
double target() const;
bool armed() const;
void disarm();
double pct() const;
```

The target is `entryPrice * (1 + pct / 100)` for a long, reached when `high >= target()`, and `entryPrice * (1 - pct / 100)` for a short, reached when `low <= target()`. Reported per bar like `PercentStop`. `target()` is NaN while disarmed.

## TimeStop

```cpp
explicit TimeStop(std::size_t maxBars);  // std::invalid_argument for 0
void arm();
bool update();
std::size_t bars() const;
std::size_t maxBars() const;
bool armed() const;
void disarm();
```

`update` counts the bars since `arm` and returns true from the `maxBars`-th one on: `TimeStop(3)` answers false, false, true, true. `arm` while armed restarts the count; `disarm` sets `bars()` back to 0. Before `arm`, `update` returns false and counts nothing.

```cpp
lrvx::exits::PercentStop stop(2.0);
lrvx::exits::TimeStop timeout(20);
// in onSymbolBar, while holding a position:
if (ctx.barsInPosition == 1)
{
  stop.arm(ctx.isLong() ? Side::BUY : Side::SELL, ctx.entryPrice->toDouble());
  timeout.arm();
}
const bool out = stop.update(ev.bar.high.toDouble(), ev.bar.low.toDouble()) | timeout.update();
```
