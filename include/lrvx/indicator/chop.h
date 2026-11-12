#pragma once

#include "lrvx/indicator/streaming.h"

#include "lrvx/aggregator/bar.h"
#include "lrvx/indicator/atr.h"

#include <cassert>
#include <cmath>
#include <span>
#include <vector>

namespace lrvx::indicator
{

namespace detail
{

// Choppiness index step:
//
//   CHOP = 100 * log10(sum of ATR(1) over the window / (highest high - lowest low))
//          / log10(period)
//
// ATR(1) is the Wilder ATR kernel with period 1, so a bar's term is its true
// range, NaN on the first bar (no previous close), and after that the previous
// term again when the bar's own true range is NaN. A window holding any NaN
// term gives NaN. The highest high and lowest low start from the window's
// oldest bar and take a later bar only when it compares greater (smaller), so
// a NaN oldest high or low makes that window NaN and a later NaN is skipped.
// A window with no range, or no true range, gives NaN.
//
// O(period) per step: the sum and the extremes are taken afresh over ring
// buffers of the last `period` terms, highs and lows, oldest first, which
// keeps the sum's rounding identical to a plain left-to-right sum over the
// window. A zero period never produces a value.
class ChopKernel
{
 public:
  explicit ChopKernel(size_t period)
      : _period(period),
        _logPeriod(std::log10(static_cast<double>(period))),
        _atr(1),
        _tr(period),
        _high(period),
        _low(period)
  {
  }

  LRVX_INDICATOR_STEP double step(double high, double low, double close)
  {
    _tr.push(_atr.step(high, low, close));
    _high.push(high);
    _low.push(low);
    if (_period == 0 || !_tr.full())
    {
      return std::nan("");
    }

    double trSum = 0.0;
    LRVX_INDICATOR_NO_UNROLL
    for (double v : _tr.window())
    {
      if (std::isnan(v))
      {
        return std::nan("");
      }
      trSum += v;
    }

    const std::span<const double> highs = _high.window();
    const std::span<const double> lows = _low.window();
    double highest = highs[0];
    double lowest = lows[0];
    for (size_t j = 1; j < _period; ++j)
    {
      if (highs[j] > highest)
      {
        highest = highs[j];
      }
      if (lows[j] < lowest)
      {
        lowest = lows[j];
      }
    }

    const double range = highest - lowest;
    if (range > 0 && trSum > 0)
    {
      return 100.0 * std::log10(trSum / range) / _logPeriod;
    }
    return std::nan("");
  }

  void reset()
  {
    _atr.reset();
    _tr.clear();
    _high.clear();
    _low.clear();
  }

 private:
  size_t _period;
  double _logPeriod;
  AtrKernel _atr;
  Ring _tr;
  Ring _high;
  Ring _low;
};

}  // namespace detail

class CHOP : public StreamingBar<CHOP, detail::ChopKernel>
{
 public:
  explicit CHOP(size_t period) : StreamingBar(detail::ChopKernel(period)), _period(period) {}

  std::vector<double> compute(std::span<const double> high, std::span<const double> low,
                              std::span<const double> close) const
  {
    assert(high.size() == low.size() && low.size() == close.size());
    std::vector<double> out(high.size(), std::nan(""));
    fold(detail::ChopKernel(_period), high, low, close, out);
    return out;
  }

  std::vector<double> compute(std::span<const Bar> bars) const
  {
    std::vector<double> out(bars.size(), std::nan(""));
    detail::ChopKernel kernel(_period);
    for (size_t i = 0; i < bars.size(); ++i)
    {
      out[i] =
          kernel.step(bars[i].high.toDouble(), bars[i].low.toDouble(), bars[i].close.toDouble());
    }
    return out;
  }

  size_t period() const noexcept { return _period; }

 private:
  size_t _period;
};

}  // namespace lrvx::indicator

#include "lrvx/indicator/indicator.h"
static_assert(lrvx::indicator::BarIndicator<lrvx::indicator::CHOP>);
