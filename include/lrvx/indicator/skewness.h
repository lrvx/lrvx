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

// Sample skewness of the last `period` inputs. O(period) per step: an exact
// two-pass computation over the ring (mean, then central moments), not a
// running-moment update, so values do not drift over long series. A NaN in
// the window gives NaN for that window; a zero-variance window gives NaN.
class SkewnessKernel
{
 public:
  explicit SkewnessKernel(size_t period) : _period(period), _win(period) {}

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

    double m2 = 0, m3 = 0;
    LRVX_INDICATOR_NO_UNROLL
    for (double v : w)
    {
      double d = v - mean;
      double d2 = d * d;
      m2 += d2;
      m3 += d2 * d;
    }

    double variance = m2 / (p - 1.0);
    if (variance == 0.0)
    {
      return std::nan("");
    }

    double s = std::sqrt(variance);
    return (p / ((p - 1.0) * (p - 2.0))) * (m3 / (s * s * s));
  }

  void reset() { _win.clear(); }

 private:
  size_t _period;
  Ring _win;
};

}  // namespace detail

class Skewness : public StreamingSingle<Skewness, detail::SkewnessKernel>
{
 public:
  explicit Skewness(size_t period) : StreamingSingle(detail::SkewnessKernel(period)), _period(period)
  {
    assert(period >= 3);
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
    fold(detail::SkewnessKernel(_period), input, output);
  }

  size_t period() const noexcept { return _period; }

 private:
  size_t _period;
};

}  // namespace lrvx::indicator

#include "lrvx/indicator/indicator.h"
static_assert(lrvx::indicator::SingleIndicator<lrvx::indicator::Skewness>);
