#pragma once

#include "lrvx/indicator/streaming.h"

#include <cassert>
#include <cmath>
#include <span>
#include <vector>

namespace lrvx::indicator
{

namespace detail
{

// (x - mean) / stdev over the last `period` inputs (sample stdev). O(period)
// per step: mean and stdev are recomputed exactly over the ring each step
// rather than kept as running sums, which would lose precision on long,
// trending series. A NaN in the window gives NaN for that window; a
// zero-stdev window gives NaN.
class RollingZScoreKernel
{
 public:
  explicit RollingZScoreKernel(size_t period) : _period(period), _win(period) {}

  LRVX_INDICATOR_STEP double step(double x)
  {
    _win.push(x);
    if (_period == 0 || !_win.full())
    {
      return std::nan("");
    }
    const auto w = _win.window();
    const double p = static_cast<double>(_period);

    double mean = 0;
    for (double v : w)
    {
      mean += v;
    }
    mean /= p;

    double sumSq = 0;
    LRVX_INDICATOR_NO_UNROLL
    for (double v : w)
    {
      double d = v - mean;
      sumSq += d * d;
    }

    double std = std::sqrt(sumSq / (p - 1.0));
    if (std == 0.0)
    {
      return std::nan("");
    }
    return (x - mean) / std;
  }

  void reset() { _win.clear(); }

 private:
  size_t _period;
  Ring _win;
};

}  // namespace detail

class RollingZScore : public StreamingSingle<RollingZScore, detail::RollingZScoreKernel>
{
 public:
  explicit RollingZScore(size_t period)
      : StreamingSingle(detail::RollingZScoreKernel(period)), _period(period)
  {
    assert(period >= 2);
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
    fold(detail::RollingZScoreKernel(_period), input, output);
  }

  size_t period() const noexcept { return _period; }

 private:
  size_t _period;
};

}  // namespace lrvx::indicator

#include "lrvx/indicator/indicator.h"
static_assert(lrvx::indicator::SingleIndicator<lrvx::indicator::RollingZScore>);
