/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2026 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "lrvx/indicator/streaming.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

namespace lrvx::indicator
{

namespace detail
{

// The max (or min) of the last `period` inputs, the current one included,
// over a monotonic deque: the deque holds the inputs that can still become
// the extreme, best first, so an update pops what the new input dominates
// and what has left the window, each input entering and leaving once. O(1)
// amortized per step; the deque is a ring of `period` slots allocated in
// the constructor, so a step never allocates.
//
// A NaN is not an input to the deque. Like SMA, it withholds the output for
// every window it sits in and the window recovers once it slides out.
// Before `period` inputs have arrived the output is NaN; a zero period
// never produces one.
template <bool Max>
class RollingExtremeKernel
{
 public:
  explicit RollingExtremeKernel(size_t period) : _period(period), _slots(period) {}

  LRVX_INDICATOR_STEP double step(double x)
  {
    if (_period == 0)
    {
      return std::nan("");
    }
    const uint64_t i = _seen++;
    // Expire first, so the ring never holds more than `period` slots.
    while (_size > 0 && _slots[_head].index + _period <= i)
    {
      _head = (_head + 1) % _period;
      --_size;
    }
    if (std::isnan(x))
    {
      _lastNan = i;
      _hasNan = true;
    }
    else
    {
      while (_size > 0 && !beats(back().value, x))
      {
        --_size;
      }
      _slots[(_head + _size) % _period] = Slot{i, x};
      ++_size;
    }
    if (_seen < _period || (_hasNan && _lastNan + _period > i))
    {
      return std::nan("");
    }
    return _slots[_head].value;
  }

  void reset()
  {
    _head = 0;
    _size = 0;
    _seen = 0;
    _lastNan = 0;
    _hasNan = false;
  }

 private:
  struct Slot
  {
    uint64_t index{0};
    double value{0.0};
  };

  // Whether a deque entry holding `kept` survives the arrival of `x`: only
  // when it is strictly better. An equal older entry is dropped, since the
  // newer one carries the same value and stays in the window longer.
  static bool beats(double kept, double x) noexcept { return Max ? kept > x : kept < x; }

  const Slot& back() const noexcept { return _slots[(_head + _size - 1) % _period]; }

  size_t _period;
  std::vector<Slot> _slots;
  size_t _head{0};
  size_t _size{0};
  uint64_t _seen{0};
  uint64_t _lastNan{0};
  bool _hasNan{false};
};

}  // namespace detail

// The highest of the last `period` inputs, the current one included. NaN
// until `period` inputs are in and while a NaN is in the window.
class Highest : public StreamingSingle<Highest, detail::RollingExtremeKernel<true>>
{
 public:
  explicit Highest(size_t period)
      : StreamingSingle(detail::RollingExtremeKernel<true>(period)), _period(period)
  {
  }

  std::vector<double> compute(std::span<const double> input) const
  {
    std::vector<double> out(input.size(), std::nan(""));
    compute(input, out);
    return out;
  }

  void compute(std::span<const double> input, std::span<double> output) const
  {
    assert(output.size() >= input.size());
    fold(detail::RollingExtremeKernel<true>(_period), input, output);
  }

  size_t period() const noexcept { return _period; }

 private:
  size_t _period;
};

}  // namespace lrvx::indicator

#include "lrvx/indicator/indicator.h"
static_assert(lrvx::indicator::SingleIndicator<lrvx::indicator::Highest>);
