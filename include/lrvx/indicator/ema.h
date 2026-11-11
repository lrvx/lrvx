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

// Exponential smoothing step shared by EMA (alpha = 2 / (period + 1)) and RMA
// (alpha = 1 / period). O(1) per step.
//
// Seeds with the mean of the first `period` consecutive non-NaN inputs; a NaN
// inside the seed window restarts the seed. After the seed a NaN input holds
// the previous output instead of poisoning the recursion. A zero period never
// seeds and never produces a value.
class ExpSmoothingKernel
{
 public:
  ExpSmoothingKernel(size_t period, double alpha) noexcept : _period(period), _alpha(alpha) {}

  LRVX_INDICATOR_STEP double step(double x) noexcept
  {
    if (!_seeded)
    {
      if (std::isnan(x))
      {
        _validCount = 0;
        _sum = 0.0;
        return std::nan("");
      }
      _sum += x;
      ++_validCount;
      if (_validCount != _period)
      {
        return std::nan("");
      }
      _seeded = true;
      _last = _sum / static_cast<double>(_period);
      return _last;
    }
    if (std::isnan(x))
    {
      return _last;
    }
    _last = _alpha * x + (1.0 - _alpha) * _last;
    return _last;
  }

  void reset() noexcept
  {
    _validCount = 0;
    _sum = 0.0;
    _seeded = false;
    _last = std::nan("");
  }

 private:
  size_t _period;
  double _alpha;
  size_t _validCount = 0;
  double _sum = 0.0;
  bool _seeded = false;
  double _last = std::nan("");
};

}  // namespace detail

class EMA : public StreamingSingle<EMA, detail::ExpSmoothingKernel>
{
 public:
  explicit EMA(size_t period) noexcept
      : StreamingSingle(detail::ExpSmoothingKernel(period, alphaFor(period))),
        _period(period),
        _alpha(alphaFor(period))
  {
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
    fold(kernel(), input, output);
  }

  // A fresh step kernel with this EMA's parameters (DEMA, TEMA and MACD chain
  // these).
  detail::ExpSmoothingKernel kernel() const noexcept
  {
    return detail::ExpSmoothingKernel(_period, _alpha);
  }

  size_t period() const noexcept { return _period; }

 private:
  static double alphaFor(size_t period) noexcept { return 2.0 / (period + 1.0); }

  size_t _period;
  double _alpha;
};

}  // namespace lrvx::indicator

#include "lrvx/indicator/indicator.h"
static_assert(lrvx::indicator::SingleIndicator<lrvx::indicator::EMA>);
