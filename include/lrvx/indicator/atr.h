#pragma once

#include "lrvx/indicator/streaming.h"

#include "lrvx/aggregator/bar.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <span>
#include <vector>

namespace lrvx::indicator
{

namespace detail
{

// Wilder ATR step. O(1) per step.
//
// TR[0] has no previous close and is NaN -- matches TA-Lib seeding from
// TR[1..period]: the first output, at index `period`, is the plain mean of
// TR[1..period] (a NaN there makes that output NaN, and the recursion stays
// NaN). After the seed a NaN TR (NaN high, low or previous close) holds the
// previous output.
class AtrKernel
{
 public:
  explicit AtrKernel(size_t period) noexcept : _period(period) {}

  LRVX_INDICATOR_STEP double step(double high, double low, double close) noexcept
  {
    const size_t i = _index++;
    double tr = std::nan("");
    if (i > 0 && !(std::isnan(high) || std::isnan(low) || std::isnan(_prevClose)))
    {
      tr = std::max({high - low, std::abs(high - _prevClose), std::abs(low - _prevClose)});
    }
    _prevClose = close;

    if (i < _period)
    {
      if (i >= 1)
      {
        _sum += tr;
      }
      return std::nan("");
    }
    if (i == _period)
    {
      if (i >= 1)
      {
        _sum += tr;
      }
      _last = _sum / static_cast<double>(_period);
      return _last;
    }

    if (std::isnan(tr))
    {
      return _last;
    }
    const double alpha = 1.0 / static_cast<double>(_period);
    _last = alpha * tr + (1.0 - alpha) * _last;
    return _last;
  }

  void reset() noexcept
  {
    _index = 0;
    _prevClose = std::nan("");
    _sum = 0.0;
    _last = std::nan("");
  }

 private:
  size_t _period;
  size_t _index = 0;
  double _prevClose = std::nan("");
  double _sum = 0.0;
  double _last = std::nan("");
};

}  // namespace detail

class ATR : public StreamingBar<ATR, detail::AtrKernel>
{
 public:
  explicit ATR(size_t period) noexcept : StreamingBar(detail::AtrKernel(period)), _period(period) {}

  std::vector<double> compute(std::span<const double> high, std::span<const double> low,
                              std::span<const double> close) const
  {
    assert(high.size() == low.size() && low.size() == close.size());
    std::vector<double> out(high.size(), std::nan(""));
    compute(high, low, close, out);
    return out;
  }

  void compute(std::span<const double> high, std::span<const double> low,
               std::span<const double> close, std::span<double> output) const
  {
    assert(output.size() >= high.size());
    assert(high.size() == low.size() && low.size() == close.size());
    fold(detail::AtrKernel(_period), high, low, close, output);
  }

  std::vector<double> compute(std::span<const Bar> bars) const
  {
    std::vector<double> out(bars.size(), std::nan(""));
    compute(bars, out);
    return out;
  }

  void compute(std::span<const Bar> bars, std::span<double> output) const
  {
    assert(output.size() >= bars.size());
    detail::AtrKernel kernel(_period);
    for (size_t i = 0; i < bars.size(); ++i)
    {
      output[i] =
          kernel.step(bars[i].high.toDouble(), bars[i].low.toDouble(), bars[i].close.toDouble());
    }
  }

  size_t period() const noexcept { return _period; }

 private:
  size_t _period;
};

}  // namespace lrvx::indicator

#include "lrvx/indicator/indicator.h"
static_assert(lrvx::indicator::BarIndicator<lrvx::indicator::ATR>);
