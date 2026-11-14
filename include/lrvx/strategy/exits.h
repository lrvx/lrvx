/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2026 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

// Exit rules for bar strategies: an ATR stop, a Chandelier exit, a
// break-even trail, a percent stop, a take-profit and a time stop. Each one
// tracks one open position and answers, bar by bar, where the stop is and
// whether the bar reached it. The strategy owns
// the entry, the ATR (an indicator::ATR fed with the same bars) and the exit
// order; these classes own the arithmetic of the level.
//
// One shape for the three ATR rules:
//   arm(side, entryPrice, atr)   when the position opens, with the fill
//                                price and the ATR of that bar
//   update(high, low, atr)       once per closed bar from the fill bar on;
//                                true when the bar's range reached the stop
//   stop() / armed() / disarm()
//
// update() tests the bar against the stop that was resting before the bar
// (the level the previous update left), then moves the stop with the bar.
// A bar cannot be stopped out by a level its own high raised, and a resting
// stop order at stop() would have filled on the same bar the test reports.
// A long is stopped when low <= stop, a short when high >= stop. A hit does
// not disarm: the strategy exits and disarms, and the same hit is reported
// again until it does.
//
// PercentStop and TakeProfit take no ATR: arm(side, entryPrice) and
// update(high, low), with the level at entryPrice * (1 -/+ pct / 100); a
// hit is latched and reported until disarm() or the next arm(). TimeStop takes no prices at all: arm() and update(), true from the
// maxBars-th update on.
//
// The python package carries the same classes in lrvx.exits with the same
// arithmetic; tests on both sides pin one scenario to the same levels.

#include "lrvx/common.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <deque>
#include <limits>
#include <stdexcept>
#include <string>

namespace lrvx::exits
{

namespace detail
{

inline bool reached(Side side, double high, double low, double stop)
{
  return side == Side::BUY ? low <= stop : high >= stop;
}

inline double requirePositivePct(double pct, const char* rule)
{
  if (!(pct > 0.0))
  {
    throw std::invalid_argument(std::string(rule) + ": pct must be > 0");
  }
  return pct;
}

}  // namespace detail

// A stop a fixed number of ATRs from the entry, frozen there.
class AtrStop
{
 public:
  explicit AtrStop(double atrMultiple) noexcept : _multiple(atrMultiple) {}

  void arm(Side side, double entryPrice, double atr) noexcept
  {
    _side = side;
    _stop = side == Side::BUY ? entryPrice - _multiple * atr : entryPrice + _multiple * atr;
    _armed = true;
  }

  bool update(double high, double low, double /*atr*/) noexcept
  {
    if (!_armed)
    {
      return false;
    }
    return detail::reached(_side, high, low, _stop);
  }

  double stop() const noexcept { return _stop; }
  bool armed() const noexcept { return _armed; }
  void disarm() noexcept
  {
    _armed = false;
    _stop = std::numeric_limits<double>::quiet_NaN();
  }
  double atrMultiple() const noexcept { return _multiple; }

 private:
  double _multiple;
  Side _side{Side::BUY};
  double _stop{std::numeric_limits<double>::quiet_NaN()};
  bool _armed{false};
};

// The Chandelier exit: the stop hangs a number of ATRs below the highest
// high since the entry (above the lowest low, for a short), looking back at
// most `lookback` bars; 0 looks back to the entry. The stop only moves in
// the position's favour, and starts at the entry price minus (plus) the same
// number of ATRs, so the position is protected from the first bar.
class ChandelierExit
{
 public:
  ChandelierExit(std::size_t lookback, double atrMultiple) noexcept
      : _lookback(lookback), _multiple(atrMultiple)
  {
  }

  void arm(Side side, double entryPrice, double atr) noexcept
  {
    _side = side;
    _stop = side == Side::BUY ? entryPrice - _multiple * atr : entryPrice + _multiple * atr;
    _extremes.clear();
    _armed = true;
  }

  bool update(double high, double low, double atr)
  {
    if (!_armed)
    {
      return false;
    }
    const bool hit = detail::reached(_side, high, low, _stop);

    _extremes.push_back(_side == Side::BUY ? high : low);
    if (_lookback > 0 && _extremes.size() > _lookback)
    {
      _extremes.pop_front();
    }
    double anchor = _extremes.front();
    for (double v : _extremes)
    {
      anchor = _side == Side::BUY ? std::max(anchor, v) : std::min(anchor, v);
    }
    const double candidate = _side == Side::BUY ? anchor - _multiple * atr : anchor + _multiple * atr;
    _stop = _side == Side::BUY ? std::max(_stop, candidate) : std::min(_stop, candidate);
    return hit;
  }

  double stop() const noexcept { return _stop; }
  bool armed() const noexcept { return _armed; }
  void disarm() noexcept
  {
    _armed = false;
    _stop = std::numeric_limits<double>::quiet_NaN();
    _extremes.clear();
  }
  std::size_t lookback() const noexcept { return _lookback; }
  double atrMultiple() const noexcept { return _multiple; }

 private:
  std::size_t _lookback;
  double _multiple;
  Side _side{Side::BUY};
  double _stop{std::numeric_limits<double>::quiet_NaN()};
  std::deque<double> _extremes;
  bool _armed{false};
};

// A hard stop at `hardStopAtr` ATRs from the entry; once the position has
// moved `breakEvenAtr` ATRs (measured with the ATR at arm time) in its
// favour, the stop moves to the entry price and from then on trails the
// best price seen at `trailAtr` ATRs (measured with the current bar's ATR).
// The stop only moves in the position's favour.
class BreakEvenTrail
{
 public:
  BreakEvenTrail(double hardStopAtr, double breakEvenAtr, double trailAtr) noexcept
      : _hard(hardStopAtr), _trigger(breakEvenAtr), _trail(trailAtr)
  {
  }

  void arm(Side side, double entryPrice, double atr) noexcept
  {
    _side = side;
    _entry = entryPrice;
    _triggerDistance = _trigger * atr;
    _stop = side == Side::BUY ? entryPrice - _hard * atr : entryPrice + _hard * atr;
    _best = entryPrice;
    _breakEven = false;
    _armed = true;
  }

  bool update(double high, double low, double atr) noexcept
  {
    if (!_armed)
    {
      return false;
    }
    const bool hit = detail::reached(_side, high, low, _stop);

    if (_side == Side::BUY)
    {
      _best = std::max(_best, high);
      if (!_breakEven && _best - _entry >= _triggerDistance)
      {
        _breakEven = true;
      }
      if (_breakEven)
      {
        _stop = std::max(_stop, std::max(_entry, _best - _trail * atr));
      }
    }
    else
    {
      _best = std::min(_best, low);
      if (!_breakEven && _entry - _best >= _triggerDistance)
      {
        _breakEven = true;
      }
      if (_breakEven)
      {
        _stop = std::min(_stop, std::min(_entry, _best + _trail * atr));
      }
    }
    return hit;
  }

  double stop() const noexcept { return _stop; }
  bool armed() const noexcept { return _armed; }
  // True once the stop has reached the entry price.
  bool atBreakEven() const noexcept { return _breakEven; }
  void disarm() noexcept
  {
    _armed = false;
    _breakEven = false;
    _stop = std::numeric_limits<double>::quiet_NaN();
  }

 private:
  double _hard;
  double _trigger;
  double _trail;
  Side _side{Side::BUY};
  double _entry{0.0};
  double _triggerDistance{0.0};
  double _stop{std::numeric_limits<double>::quiet_NaN()};
  double _best{0.0};
  bool _breakEven{false};
  bool _armed{false};
};

// A stop `pct` percent from the entry, frozen there: entry * (1 - pct / 100)
// for a long, entry * (1 + pct / 100) for a short. Throws
// std::invalid_argument unless pct > 0.
class PercentStop
{
 public:
  explicit PercentStop(double pct) : _pct(detail::requirePositivePct(pct, "PercentStop")) {}

  void arm(Side side, double entryPrice) noexcept
  {
    _side = side;
    _stop = side == Side::BUY ? entryPrice * (1.0 - _pct / 100.0)
                              : entryPrice * (1.0 + _pct / 100.0);
    _armed = true;
    _hit = false;
  }

  // Once the stop is reached the hit is latched: every update reports it
  // until disarm() or the next arm().
  bool update(double high, double low) noexcept
  {
    if (!_armed)
    {
      return false;
    }
    _hit = _hit || detail::reached(_side, high, low, _stop);
    return _hit;
  }

  double stop() const noexcept { return _stop; }
  bool armed() const noexcept { return _armed; }
  void disarm() noexcept
  {
    _armed = false;
    _hit = false;
    _stop = std::numeric_limits<double>::quiet_NaN();
  }
  double pct() const noexcept { return _pct; }

 private:
  double _pct;
  Side _side{Side::BUY};
  double _stop{std::numeric_limits<double>::quiet_NaN()};
  bool _armed{false};
  bool _hit{false};
};

// A profit target `pct` percent from the entry: entry * (1 + pct / 100) for a
// long, reached when high >= target; entry * (1 - pct / 100) for a short,
// reached when low <= target. Throws std::invalid_argument unless pct > 0.
class TakeProfit
{
 public:
  explicit TakeProfit(double pct) : _pct(detail::requirePositivePct(pct, "TakeProfit")) {}

  void arm(Side side, double entryPrice) noexcept
  {
    _side = side;
    _target = side == Side::BUY ? entryPrice * (1.0 + _pct / 100.0)
                                : entryPrice * (1.0 - _pct / 100.0);
    _armed = true;
    _hit = false;
  }

  // Latched like PercentStop: reported until disarm() or the next arm().
  bool update(double high, double low) noexcept
  {
    if (!_armed)
    {
      return false;
    }
    _hit = _hit || (_side == Side::BUY ? high >= _target : low <= _target);
    return _hit;
  }

  double target() const noexcept { return _target; }
  bool armed() const noexcept { return _armed; }
  void disarm() noexcept
  {
    _armed = false;
    _hit = false;
    _target = std::numeric_limits<double>::quiet_NaN();
  }
  double pct() const noexcept { return _pct; }

 private:
  double _pct;
  Side _side{Side::BUY};
  double _target{std::numeric_limits<double>::quiet_NaN()};
  bool _armed{false};
  bool _hit{false};
};

// Exit after a number of bars: update() counts the bars since arm() and is
// true from the maxBars-th one on. Throws std::invalid_argument for 0 bars.
class TimeStop
{
 public:
  explicit TimeStop(std::size_t maxBars) : _maxBars(maxBars)
  {
    if (maxBars < 1)
    {
      throw std::invalid_argument("TimeStop: maxBars must be >= 1");
    }
  }

  void arm() noexcept
  {
    _bars = 0;
    _armed = true;
  }

  bool update() noexcept
  {
    if (!_armed)
    {
      return false;
    }
    ++_bars;
    return _bars >= _maxBars;
  }

  std::size_t bars() const noexcept { return _bars; }
  std::size_t maxBars() const noexcept { return _maxBars; }
  bool armed() const noexcept { return _armed; }
  void disarm() noexcept
  {
    _armed = false;
    _bars = 0;
  }

 private:
  std::size_t _maxBars;
  std::size_t _bars{0};
  bool _armed{false};
};

}  // namespace lrvx::exits
