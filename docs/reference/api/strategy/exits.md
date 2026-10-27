# Exit rules

`lrvx/strategy/exits.h`, namespace `lrvx::exits`. Three exit rules for a position opened by a bar strategy, with one shape: `arm` when the position opens, `update` once per closed bar, `stop()`, `armed()`, `disarm()`. The Python package carries the same classes in `lrvx.exits`. The guide is [Exits for bar strategies](../../../how-to/bar-exits.md).

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
