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

// Pearson correlation of x[t] with x[t - lag] over the last `window` pairs.
// O(window) per step: the sums are recomputed over the ring (the last
// window + lag inputs) each step. Zero denominator gives NaN.
class AutoCorrelationKernel
{
 public:
  AutoCorrelationKernel(size_t window, size_t lag)
      : _window(window), _lag(lag), _firstValid(window + lag - 1), _win(window + lag)
  {
  }

  LRVX_INDICATOR_STEP double step(double v)
  {
    _win.push(v);
    const size_t t = _index++;
    if (t < _firstValid)
    {
      return std::nan("");
    }
    // w[k] = x[t - window - lag + 1 + k]
    const auto w = _win.window();
    const double n = static_cast<double>(_window);

    double sx = 0, sy = 0, sxy = 0, sx2 = 0, sy2 = 0;
    LRVX_INDICATOR_NO_UNROLL
    for (size_t i = 0; i < _window; ++i)
    {
      double xi = w[_lag + i];
      double yi = w[i];
      sx += xi;
      sy += yi;
      sxy += xi * yi;
      sx2 += xi * xi;
      sy2 += yi * yi;
    }

    double num = n * sxy - sx * sy;
    double den = std::sqrt((n * sx2 - sx * sx) * (n * sy2 - sy * sy));
    if (den == 0.0)
    {
      return std::nan("");
    }
    return num / den;
  }

  void reset()
  {
    _win.clear();
    _index = 0;
  }

 private:
  size_t _window;
  size_t _lag;
  size_t _firstValid;
  Ring _win;
  size_t _index = 0;
};

}  // namespace detail

// AutoCorrelation(window, lag): Pearson correlation between x[t] and x[t-lag]
// over a trailing window of `window` paired observations. Equivalent to
// Correlation(window) applied to (x[lag..], x[..-lag]) but explicit about the
// lag and warm-up.
//
//   first valid index is (window - 1 + lag).
//
// Reuses Pearson's standard closed form so output matches Correlation when
// the same data is paired manually.
class AutoCorrelation : public StreamingSingle<AutoCorrelation, detail::AutoCorrelationKernel>
{
 public:
  AutoCorrelation(size_t window, size_t lag)
      : StreamingSingle(detail::AutoCorrelationKernel(window, lag)), _window(window), _lag(lag)
  {
    assert(window >= 2);
    assert(lag >= 1);
  }

  std::vector<double> compute(std::span<const double> x) const
  {
    std::vector<double> out(x.size(), std::nan(""));
    compute(x, out);
    return out;
  }

  void compute(std::span<const double> x, std::span<double> output) const
  {
    assert(output.size() >= x.size());
    fold(detail::AutoCorrelationKernel(_window, _lag), x, output);
  }

  size_t period() const noexcept { return _window + _lag - 1; }
  size_t window() const noexcept { return _window; }
  size_t lag() const noexcept { return _lag; }

 private:
  size_t _window;
  size_t _lag;
};

inline std::vector<double> autocorrelation(std::span<const double> x, size_t window, size_t lag)
{
  return AutoCorrelation(window, lag).compute(x);
}

}  // namespace lrvx::indicator

#include "lrvx/indicator/indicator.h"
static_assert(lrvx::indicator::HasPeriod<lrvx::indicator::AutoCorrelation>);
