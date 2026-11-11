#pragma once

#include "lrvx/indicator/streaming.h"

#include "lrvx/indicator/sma.h"

#include <cassert>
#include <cmath>
#include <span>
#include <vector>

namespace lrvx::indicator
{

namespace detail
{

// CCI step: typical price tp = (h + l + c) / 3, its SMA, and the mean
// absolute deviation of tp from that SMA over the window. O(period) per step
// (the deviation depends on the current SMA, so it is summed afresh over the
// window). A NaN anywhere in the window gives NaN for that window, through
// the SMA's NaN contract. A zero period has no well-defined window and never
// produces a value.
class CciKernel
{
 public:
  explicit CciKernel(size_t period) : _period(period), _sma(period) {}

  LRVX_INDICATOR_STEP double step(double high, double low, double close)
  {
    const double tp = (high + low + close) / 3.0;
    const double mean = _sma.step(tp);
    if (_period == 0 || std::isnan(mean))
    {
      return std::nan("");
    }
    double md = 0.0;
    for (double v : _sma.window())
    {
      md += std::abs(v - mean);
    }
    md /= static_cast<double>(_period);
    return md > 0 ? (tp - mean) / (0.015 * md) : 0.0;
  }

  void reset() { _sma.reset(); }

 private:
  size_t _period;
  SmaKernel _sma;
};

}  // namespace detail

class CCI : public StreamingBar<CCI, detail::CciKernel>
{
 public:
  explicit CCI(size_t period = 20) : StreamingBar(detail::CciKernel(period)), _period(period) {}

  std::vector<double> compute(std::span<const double> high, std::span<const double> low,
                              std::span<const double> close) const
  {
    std::vector<double> out(high.size(), std::nan(""));
    fold(detail::CciKernel(_period), high, low, close, out);
    return out;
  }

  size_t period() const noexcept { return _period; }

 private:
  size_t _period;
};

}  // namespace lrvx::indicator

#include "lrvx/indicator/indicator.h"
static_assert(lrvx::indicator::HasPeriod<lrvx::indicator::CCI>);
