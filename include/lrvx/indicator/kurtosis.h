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

// Sample excess kurtosis of the last `period` inputs. O(period) per step: an
// exact two-pass computation over the ring, not a running-moment update. A
// NaN in the window gives NaN for that window; a zero-variance window gives
// NaN.
class KurtosisKernel
{
 public:
  explicit KurtosisKernel(size_t period) : _period(period), _win(period) {}

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

    double m2 = 0, m4 = 0;
    LRVX_INDICATOR_NO_UNROLL
    for (double v : w)
    {
      double d = v - mean;
      double d2 = d * d;
      m2 += d2;
      m4 += d2 * d2;
    }

    double variance = m2 / (p - 1.0);
    if (variance == 0.0)
    {
      return std::nan("");
    }

    double s2 = variance;
    double term1 = (p * (p + 1.0)) / ((p - 1.0) * (p - 2.0) * (p - 3.0));
    double term2 = m4 / (s2 * s2);
    double term3 = (3.0 * (p - 1.0) * (p - 1.0)) / ((p - 2.0) * (p - 3.0));
    return term1 * term2 - term3;
  }

  void reset() { _win.clear(); }

 private:
  size_t _period;
  Ring _win;
};

}  // namespace detail

class Kurtosis : public StreamingSingle<Kurtosis, detail::KurtosisKernel>
{
 public:
  explicit Kurtosis(size_t period) : StreamingSingle(detail::KurtosisKernel(period)), _period(period)
  {
    assert(period >= 4);
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
    fold(detail::KurtosisKernel(_period), input, output);
  }

  size_t period() const noexcept { return _period; }

 private:
  size_t _period;
};

}  // namespace lrvx::indicator

#include "lrvx/indicator/indicator.h"
static_assert(lrvx::indicator::SingleIndicator<lrvx::indicator::Kurtosis>);
