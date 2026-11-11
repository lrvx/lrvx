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

// Parkinson high/low volatility over the last `period` bars. O(period) per
// step: the sum of squared log ranges is recomputed over the rings. A window
// holding a bar with low <= 0 or high < low gives NaN.
class ParkinsonVolKernel
{
 public:
  explicit ParkinsonVolKernel(size_t period) : _period(period), _high(period), _low(period) {}

  LRVX_INDICATOR_STEP double step(double high, double low)
  {
    _high.push(high);
    _low.push(low);
    if (_period == 0 || !_high.full())
    {
      return std::nan("");
    }
    const auto h = _high.window();
    const auto l = _low.window();
    const double inv4ln2 = 1.0 / (4.0 * std::log(2.0));

    double sum = 0;
    LRVX_INDICATOR_NO_UNROLL
    for (size_t j = 0; j < _period; ++j)
    {
      if (l[j] <= 0.0 || h[j] < l[j])
      {
        return std::nan("");
      }
      double lnhl = std::log(h[j] / l[j]);
      sum += lnhl * lnhl;
    }
    return std::sqrt(sum / static_cast<double>(_period) * inv4ln2);
  }

  void reset()
  {
    _high.clear();
    _low.clear();
  }

 private:
  size_t _period;
  Ring _high, _low;
};

}  // namespace detail

class ParkinsonVol : public StreamingHighLow<ParkinsonVol, detail::ParkinsonVolKernel>
{
 public:
  explicit ParkinsonVol(size_t period)
      : StreamingHighLow(detail::ParkinsonVolKernel(period)), _period(period)
  {
    assert(period >= 1);
  }

  std::vector<double> compute(std::span<const double> high, std::span<const double> low) const
  {
    assert(high.size() == low.size());
    std::vector<double> out(high.size(), std::nan(""));
    compute(high, low, out);
    return out;
  }

  void compute(std::span<const double> high, std::span<const double> low,
               std::span<double> output) const
  {
    assert(high.size() == low.size());
    assert(output.size() >= high.size());
    fold(detail::ParkinsonVolKernel(_period), high, low, output);
  }

  std::vector<double> compute(std::span<const Bar> bars) const
  {
    const size_t n = bars.size();
    std::vector<double> h(n), l(n);
    for (size_t i = 0; i < n; ++i)
    {
      h[i] = bars[i].high.toDouble();
      l[i] = bars[i].low.toDouble();
    }
    return compute(std::span<const double>(h), std::span<const double>(l));
  }

  size_t period() const noexcept { return _period; }

 private:
  size_t _period;
};

}  // namespace lrvx::indicator

#include "lrvx/indicator/indicator.h"
static_assert(lrvx::indicator::HasPeriod<lrvx::indicator::ParkinsonVol>);
