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

// Running-sum SMA step. O(1) per step; the ring holds the last `period`
// inputs so the outgoing one can be subtracted.
//
// NaN in the window poisons only that window, not the rest of the series:
// the kernel counts how many of the `period` most recent inputs are NaN and
// withholds the output while that count is nonzero, excluding NaN entries
// from the running sum so it stays finite. The window recovers on its own
// once the NaN slides out, matching the NaN-tolerant contract EMA/RSI have
// (see NaNInputSkipped / NaNInputHandled). A zero period has no well-defined
// window and never produces a value.
class SmaKernel
{
 public:
  explicit SmaKernel(size_t period) : _period(period), _window(period) {}

  LRVX_INDICATOR_STEP double step(double incoming)
  {
    if (_period == 0)
    {
      return std::nan("");
    }
    if (!_window.full())
    {
      if (std::isnan(incoming))
      {
        ++_nanCount;
      }
      else
      {
        _sum += incoming;
      }
      _window.push(incoming);
      if (!_window.full())
      {
        return std::nan("");
      }
      return _nanCount == 0 ? _sum / static_cast<double>(_period) : std::nan("");
    }

    const double outgoing = _window.oldest();
    if (std::isnan(incoming))
    {
      ++_nanCount;
    }
    else
    {
      _sum += incoming;
    }
    if (std::isnan(outgoing))
    {
      --_nanCount;
    }
    else
    {
      _sum -= outgoing;
    }
    _window.push(incoming);
    return _nanCount == 0 ? _sum / static_cast<double>(_period) : std::nan("");
  }

  // The last `period` inputs, oldest first; complete once the first output
  // has been produced. CCI and Bollinger reuse it for their window loops.
  std::span<const double> window() const noexcept { return _window.window(); }

  void reset()
  {
    _window.clear();
    _sum = 0.0;
    _nanCount = 0;
  }

 private:
  size_t _period;
  Ring _window;
  double _sum = 0.0;
  size_t _nanCount = 0;
};

}  // namespace detail

class SMA : public StreamingSingle<SMA, detail::SmaKernel>
{
 public:
  explicit SMA(size_t period) : StreamingSingle(detail::SmaKernel(period)), _period(period) {}

  std::vector<double> compute(std::span<const double> input) const
  {
    std::vector<double> out(input.size(), std::nan(""));
    compute(input, out);
    return out;
  }

  void compute(std::span<const double> input, std::span<double> output) const
  {
    assert(output.size() >= input.size());
    fold(detail::SmaKernel(_period), input, output);
  }

  size_t period() const noexcept { return _period; }

 private:
  size_t _period;
};

}  // namespace lrvx::indicator

#include "lrvx/indicator/indicator.h"
static_assert(lrvx::indicator::SingleIndicator<lrvx::indicator::SMA>);
