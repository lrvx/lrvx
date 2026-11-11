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

// Pearson correlation of the last `period` (x, y) pairs. O(period) per step:
// the sums are recomputed over the two rings each step. Zero denominator
// gives NaN.
class CorrelationKernel
{
 public:
  explicit CorrelationKernel(size_t period) : _period(period), _x(period), _y(period) {}

  LRVX_INDICATOR_STEP double step(double xv, double yv)
  {
    _x.push(xv);
    _y.push(yv);
    if (_period == 0 || !_x.full())
    {
      return std::nan("");
    }
    const auto x = _x.window();
    const auto y = _y.window();
    const double p = static_cast<double>(_period);

    double sx = 0, sy = 0, sxy = 0, sx2 = 0, sy2 = 0;
    LRVX_INDICATOR_NO_UNROLL
    for (size_t j = 0; j < _period; ++j)
    {
      sx += x[j];
      sy += y[j];
      sxy += x[j] * y[j];
      sx2 += x[j] * x[j];
      sy2 += y[j] * y[j];
    }

    double num = p * sxy - sx * sy;
    double den = std::sqrt((p * sx2 - sx * sx) * (p * sy2 - sy * sy));

    if (den == 0.0)
    {
      return std::nan("");
    }
    return num / den;
  }

  void reset()
  {
    _x.clear();
    _y.clear();
  }

 private:
  size_t _period;
  Ring _x, _y;
};

}  // namespace detail

class Correlation : public StreamingPair<Correlation, detail::CorrelationKernel>
{
 public:
  explicit Correlation(size_t period) : StreamingPair(detail::CorrelationKernel(period)), _period(period)
  {
    assert(period >= 2);
  }

  std::vector<double> compute(std::span<const double> x, std::span<const double> y) const
  {
    assert(x.size() == y.size());
    std::vector<double> out(x.size(), std::nan(""));
    compute(x, y, out);
    return out;
  }

  void compute(std::span<const double> x, std::span<const double> y,
               std::span<double> output) const
  {
    assert(x.size() == y.size());
    assert(output.size() >= x.size());
    fold(detail::CorrelationKernel(_period), x, y, output);
  }

  size_t period() const noexcept { return _period; }

 private:
  size_t _period;
};

}  // namespace lrvx::indicator

#include "lrvx/indicator/indicator.h"
static_assert(lrvx::indicator::HasPeriod<lrvx::indicator::Correlation>);
