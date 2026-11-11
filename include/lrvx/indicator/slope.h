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

// (input[i] - input[i - length]) / length. O(1) per step; the ring holds the
// last length + 1 inputs. NaN at either end gives NaN for that step only.
class SlopeKernel
{
 public:
  explicit SlopeKernel(size_t length) : _length(length), _win(length + 1) {}

  LRVX_INDICATOR_STEP double step(double x)
  {
    _win.push(x);
    if (!_win.full())
    {
      return std::nan("");
    }
    const double back = _win.window()[0];
    if (std::isnan(x) || std::isnan(back))
    {
      return std::nan("");
    }
    return (x - back) / static_cast<double>(_length);
  }

  void reset() { _win.clear(); }

 private:
  size_t _length;
  Ring _win;
};

}  // namespace detail

class Slope : public StreamingSingle<Slope, detail::SlopeKernel>
{
 public:
  explicit Slope(size_t length) : StreamingSingle(detail::SlopeKernel(length)), _length(length) {}

  std::vector<double> compute(std::span<const double> input) const
  {
    std::vector<double> out(input.size(), std::nan(""));
    compute(input, out);
    return out;
  }

  void compute(std::span<const double> input, std::span<double> output) const
  {
    assert(output.size() >= input.size());
    fold(detail::SlopeKernel(_length), input, output);
  }

  size_t period() const noexcept { return _length; }

 private:
  size_t _length;
};

}  // namespace lrvx::indicator

#include "lrvx/indicator/indicator.h"
static_assert(lrvx::indicator::SingleIndicator<lrvx::indicator::Slope>);
