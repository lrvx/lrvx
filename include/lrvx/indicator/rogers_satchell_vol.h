#pragma once

#include "lrvx/indicator/streaming.h"

#include <cassert>
#include <cmath>
#include <span>
#include <vector>

#include "lrvx/aggregator/bar.h"

namespace lrvx::indicator
{

namespace detail
{

// Rogers-Satchell OHLC volatility over the last `period` bars. O(period) per
// step: the log-ratio sum is recomputed over the rings. A window holding a
// bar with open <= 0 or close <= 0 gives NaN; a negative mean clamps to 0.
class RogersSatchellVolKernel
{
 public:
  explicit RogersSatchellVolKernel(size_t period)
      : _period(period), _open(period), _high(period), _low(period), _close(period)
  {
  }

  LRVX_INDICATOR_STEP double step(double open, double high, double low, double close)
  {
    _open.push(open);
    _high.push(high);
    _low.push(low);
    _close.push(close);
    if (_period == 0 || !_open.full())
    {
      return std::nan("");
    }
    const auto o = _open.window();
    const auto h = _high.window();
    const auto l = _low.window();
    const auto c = _close.window();

    double sum = 0;
    LRVX_INDICATOR_NO_UNROLL
    for (size_t j = 0; j < _period; ++j)
    {
      if (o[j] <= 0.0 || c[j] <= 0.0)
      {
        return std::nan("");
      }
      double lnHC = std::log(h[j] / c[j]);
      double lnHO = std::log(h[j] / o[j]);
      double lnLC = std::log(l[j] / c[j]);
      double lnLO = std::log(l[j] / o[j]);
      sum += lnHC * lnHO + lnLC * lnLO;
    }

    double avg = sum / static_cast<double>(_period);
    return avg >= 0.0 ? std::sqrt(avg) : 0.0;
  }

  void reset()
  {
    _open.clear();
    _high.clear();
    _low.clear();
    _close.clear();
  }

 private:
  size_t _period;
  Ring _open, _high, _low, _close;
};

}  // namespace detail

class RogersSatchellVol : public StreamingOhlc<RogersSatchellVol, detail::RogersSatchellVolKernel>
{
 public:
  explicit RogersSatchellVol(size_t period)
      : StreamingOhlc(detail::RogersSatchellVolKernel(period)), _period(period)
  {
    assert(period >= 1);
  }

  std::vector<double> compute(std::span<const double> open, std::span<const double> high,
                              std::span<const double> low, std::span<const double> close) const
  {
    assert(open.size() == high.size() && high.size() == low.size() && low.size() == close.size());
    std::vector<double> out(open.size(), std::nan(""));
    compute(open, high, low, close, out);
    return out;
  }

  void compute(std::span<const double> open, std::span<const double> high,
               std::span<const double> low, std::span<const double> close,
               std::span<double> output) const
  {
    assert(open.size() == high.size() && high.size() == low.size() && low.size() == close.size());
    assert(output.size() >= open.size());
    fold(detail::RogersSatchellVolKernel(_period), open, high, low, close, output);
  }

  std::vector<double> compute(std::span<const Bar> bars) const
  {
    const size_t n = bars.size();
    std::vector<double> o(n), h(n), l(n), c(n);
    for (size_t i = 0; i < n; ++i)
    {
      o[i] = bars[i].open.toDouble();
      h[i] = bars[i].high.toDouble();
      l[i] = bars[i].low.toDouble();
      c[i] = bars[i].close.toDouble();
    }
    return compute(std::span<const double>(o), std::span<const double>(h),
                   std::span<const double>(l), std::span<const double>(c));
  }

  size_t period() const noexcept { return _period; }

 private:
  size_t _period;
};

}  // namespace lrvx::indicator

#include "lrvx/indicator/indicator.h"
static_assert(lrvx::indicator::BarIndicator<lrvx::indicator::RogersSatchellVol>);
