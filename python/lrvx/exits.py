"""Exit rules for bar strategies: an ATR stop, a Chandelier exit, a
break-even trail, a percent stop, a take-profit and a time stop.

Each one tracks one open position and answers, bar by bar, where the stop is
and whether the bar reached it. The strategy owns the entry, the ATR (an
``lrvx.ATR`` fed with the same bars) and the exit order; these classes own the
arithmetic of the level. The C++ header ``lrvx/strategy/exits.h`` carries the
same classes with the same arithmetic, and tests on both sides pin one
scenario to the same levels.

One shape for the three ATR rules::

    rule.arm(side, entry_price, atr)   # when the position opens
    hit = rule.update(high, low, atr)  # once per closed bar from the fill bar on
    rule.stop, rule.armed, rule.disarm()

``update`` tests the bar against the stop that was resting before the bar
(the level the previous update left), then moves the stop with the bar: a bar
cannot be stopped out by a level its own high raised. A long (``side ==
"long"``) is stopped when ``low <= stop``, a short when ``high >= stop``. A
hit does not disarm; the strategy exits and disarms.

``PercentStop`` and ``TakeProfit`` take no ATR: ``arm(side, entry_price)``
and ``update(high, low)``, with the level at ``entry_price * (1 -/+ pct /
100)``; like the ATR rules, ``update`` reports whether this bar reached the
level. ``TimeStop`` takes no prices: ``arm()`` and ``update()``, true
from the ``max_bars``-th update on.
"""

from __future__ import annotations

import math
from collections import deque
from typing import Deque

_SIDES = ("long", "short")


def _side(side: str) -> str:
    s = str(side).lower()
    if s not in _SIDES:
        raise ValueError(f"side must be 'long' or 'short'; got {side!r}")
    return s


def _reached(side: str, high: float, low: float, stop: float) -> bool:
    return low <= stop if side == "long" else high >= stop


class AtrStop:
    """A stop a fixed number of ATRs from the entry, frozen there."""

    def __init__(self, atr_multiple: float) -> None:
        self.atr_multiple = float(atr_multiple)
        self._side = "long"
        self.stop = math.nan
        self.armed = False

    def arm(self, side: str, entry_price: float, atr: float) -> None:
        self._side = _side(side)
        d = self.atr_multiple * atr
        self.stop = entry_price - d if self._side == "long" else entry_price + d
        self.armed = True

    def update(self, high: float, low: float, atr: float) -> bool:
        if not self.armed:
            return False
        return _reached(self._side, high, low, self.stop)

    def disarm(self) -> None:
        self.armed = False
        self.stop = math.nan


class ChandelierExit:
    """The stop hangs ``atr_multiple`` ATRs below the highest high since the
    entry (above the lowest low for a short), looking back at most
    ``lookback`` bars; 0 looks back to the entry. The stop only moves in the
    position's favour and starts ``atr_multiple`` ATRs from the entry."""

    def __init__(self, lookback: int, atr_multiple: float) -> None:
        self.lookback = int(lookback)
        self.atr_multiple = float(atr_multiple)
        self._side = "long"
        self._extremes: Deque[float] = deque()
        self.stop = math.nan
        self.armed = False

    def arm(self, side: str, entry_price: float, atr: float) -> None:
        self._side = _side(side)
        d = self.atr_multiple * atr
        self.stop = entry_price - d if self._side == "long" else entry_price + d
        self._extremes.clear()
        self.armed = True

    def update(self, high: float, low: float, atr: float) -> bool:
        if not self.armed:
            return False
        hit = _reached(self._side, high, low, self.stop)
        self._extremes.append(high if self._side == "long" else low)
        if self.lookback > 0 and len(self._extremes) > self.lookback:
            self._extremes.popleft()
        if self._side == "long":
            candidate = max(self._extremes) - self.atr_multiple * atr
            self.stop = max(self.stop, candidate)
        else:
            candidate = min(self._extremes) + self.atr_multiple * atr
            self.stop = min(self.stop, candidate)
        return hit

    def disarm(self) -> None:
        self.armed = False
        self.stop = math.nan
        self._extremes.clear()


class BreakEvenTrail:
    """A hard stop at ``hard_stop_atr`` ATRs from the entry; once the
    position has moved ``break_even_atr`` ATRs (measured with the ATR at arm
    time) in its favour, the stop moves to the entry price and from then on
    trails the best price seen at ``trail_atr`` ATRs (measured with the
    current bar's ATR). The stop only moves in the position's favour."""

    def __init__(self, hard_stop_atr: float, break_even_atr: float, trail_atr: float) -> None:
        self.hard_stop_atr = float(hard_stop_atr)
        self.break_even_atr = float(break_even_atr)
        self.trail_atr = float(trail_atr)
        self._side = "long"
        self._entry = 0.0
        self._trigger_distance = 0.0
        self._best = 0.0
        self.at_break_even = False
        self.stop = math.nan
        self.armed = False

    def arm(self, side: str, entry_price: float, atr: float) -> None:
        self._side = _side(side)
        self._entry = float(entry_price)
        self._trigger_distance = self.break_even_atr * atr
        d = self.hard_stop_atr * atr
        self.stop = entry_price - d if self._side == "long" else entry_price + d
        self._best = float(entry_price)
        self.at_break_even = False
        self.armed = True

    def update(self, high: float, low: float, atr: float) -> bool:
        if not self.armed:
            return False
        hit = _reached(self._side, high, low, self.stop)
        if self._side == "long":
            self._best = max(self._best, high)
            if not self.at_break_even and self._best - self._entry >= self._trigger_distance:
                self.at_break_even = True
            if self.at_break_even:
                self.stop = max(self.stop, max(self._entry, self._best - self.trail_atr * atr))
        else:
            self._best = min(self._best, low)
            if not self.at_break_even and self._entry - self._best >= self._trigger_distance:
                self.at_break_even = True
            if self.at_break_even:
                self.stop = min(self.stop, min(self._entry, self._best + self.trail_atr * atr))
        return hit

    def disarm(self) -> None:
        self.armed = False
        self.at_break_even = False
        self.stop = math.nan


def _positive_pct(rule: str, pct: float) -> float:
    pct = float(pct)
    if not pct > 0.0:
        raise ValueError(f"{rule}: pct must be > 0; got {pct!r}")
    return pct


class PercentStop:
    """A stop ``pct`` percent from the entry, frozen there:
    ``entry_price * (1 - pct / 100)`` for a long, ``entry_price * (1 + pct /
    100)`` for a short. Raises ValueError unless ``pct > 0``."""

    def __init__(self, pct: float) -> None:
        self.pct = _positive_pct("PercentStop", pct)
        self._side = "long"
        self.stop = math.nan
        self.armed = False

    def arm(self, side: str, entry_price: float) -> None:
        self._side = _side(side)
        if self._side == "long":
            self.stop = entry_price * (1.0 - self.pct / 100.0)
        else:
            self.stop = entry_price * (1.0 + self.pct / 100.0)
        self.armed = True

    def update(self, high: float, low: float) -> bool:
        if not self.armed:
            return False
        return _reached(self._side, high, low, self.stop)

    def disarm(self) -> None:
        self.armed = False
        self.stop = math.nan


class TakeProfit:
    """A profit target ``pct`` percent from the entry: ``entry_price * (1 +
    pct / 100)`` for a long, reached when ``high >= target``; ``entry_price *
    (1 - pct / 100)`` for a short, reached when ``low <= target``. Raises
    ValueError unless ``pct > 0``."""

    def __init__(self, pct: float) -> None:
        self.pct = _positive_pct("TakeProfit", pct)
        self._side = "long"
        self.target = math.nan
        self.armed = False

    def arm(self, side: str, entry_price: float) -> None:
        self._side = _side(side)
        if self._side == "long":
            self.target = entry_price * (1.0 + self.pct / 100.0)
        else:
            self.target = entry_price * (1.0 - self.pct / 100.0)
        self.armed = True

    def update(self, high: float, low: float) -> bool:
        if not self.armed:
            return False
        if self._side == "long":
            return high >= self.target
        return low <= self.target

    def disarm(self) -> None:
        self.armed = False
        self.target = math.nan


class TimeStop:
    """Exit after a number of bars: ``update()`` counts the bars since
    ``arm()`` and is true from the ``max_bars``-th one on. Raises ValueError
    for fewer than one bar."""

    def __init__(self, max_bars: int) -> None:
        max_bars = int(max_bars)
        if max_bars < 1:
            raise ValueError(f"TimeStop: max_bars must be >= 1; got {max_bars}")
        self.max_bars = max_bars
        self.bars = 0
        self.armed = False

    def arm(self) -> None:
        self.bars = 0
        self.armed = True

    def update(self) -> bool:
        if not self.armed:
            return False
        self.bars += 1
        return self.bars >= self.max_bars

    def disarm(self) -> None:
        self.armed = False
        self.bars = 0


__all__ = ["AtrStop", "ChandelierExit", "BreakEvenTrail", "PercentStop", "TakeProfit", "TimeStop"]
