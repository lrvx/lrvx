#pragma once

#include "lrvx/indicator/ema.h"
#include "lrvx/indicator/streaming.h"

#include <cassert>
#include <cmath>
#include <span>
#include <vector>

namespace lrvx::indicator
{

// Wilder's Moving Average. alpha = 1/period (not 2/(period+1) like EMA).
// Used internally by ATR, RSI, ADX. Exposed for direct use.
//
// Same step and NaN contract as EMA (RMA is Wilder's exponential average, so
// it has the same recursive-state problem): seed on the first `period`
// consecutive non-NaN inputs, resetting the running sum whenever a NaN
// interrupts the seed window, then hold the last output through any later NaN
// instead of poisoning the recursion permanently. A zero period has no
// well-defined window and never produces a value.
class RMA : public StreamingSingle<RMA, detail::ExpSmoothingKernel>
{
 public:
  explicit RMA(size_t period) noexcept : StreamingSingle(kernelFor(period)), _period(period) {}

  std::vector<double> compute(std::span<const double> input) const
  {
    std::vector<double> out(input.size(), std::nan(""));
    compute(input, out);
    return out;
  }

  void compute(std::span<const double> input, std::span<double> output) const
  {
    assert(output.size() >= input.size());
    fold(kernelFor(_period), input, output);
  }

  size_t period() const noexcept { return _period; }

 private:
  static detail::ExpSmoothingKernel kernelFor(size_t period) noexcept
  {
    return detail::ExpSmoothingKernel(period, 1.0 / static_cast<double>(period));
  }

  size_t _period;
};

}  // namespace lrvx::indicator

#include "lrvx/indicator/indicator.h"
static_assert(lrvx::indicator::SingleIndicator<lrvx::indicator::RMA>);
