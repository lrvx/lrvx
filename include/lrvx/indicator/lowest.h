/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2026 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "lrvx/indicator/highest.h"

#include <cassert>
#include <cmath>
#include <span>
#include <vector>

namespace lrvx::indicator
{

// The lowest of the last `period` inputs, the current one included. NaN
// until `period` inputs are in and while a NaN is in the window. The kernel
// is Highest's monotonic deque with the comparison turned round.
class Lowest : public StreamingSingle<Lowest, detail::RollingExtremeKernel<false>>
{
 public:
  explicit Lowest(size_t period)
      : StreamingSingle(detail::RollingExtremeKernel<false>(period)), _period(period)
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
    fold(detail::RollingExtremeKernel<false>(_period), input, output);
  }

  size_t period() const noexcept { return _period; }

 private:
  size_t _period;
};

}  // namespace lrvx::indicator

#include "lrvx/indicator/indicator.h"
static_assert(lrvx::indicator::SingleIndicator<lrvx::indicator::Lowest>);
