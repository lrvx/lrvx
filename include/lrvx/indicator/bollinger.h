#pragma once

#include "lrvx/indicator/sma.h"

#include <cassert>
#include <cmath>
#include <span>
#include <vector>

namespace lrvx::indicator
{

struct BollingerResult
{
  std::vector<double> upper;
  std::vector<double> middle;
  std::vector<double> lower;
};

namespace detail
{

// Bollinger step: SMA middle band, population stdev of the window around it.
// O(period) per step (the deviation sum depends on the current mean, so it
// is summed afresh over the SMA's window). While the SMA's window holds a NaN
// candle the middle band is NaN and so are both outer bands. A zero period
// has no well-defined window and never produces a value.
class BollingerKernel
{
 public:
  struct Out
  {
    double upper, middle, lower;
  };

  BollingerKernel(size_t period, double stddev) : _period(period), _stddev(stddev), _sma(period) {}

  LRVX_INDICATOR_STEP Out step(double x)
  {
    const double mean = _sma.step(x);
    if (_period == 0 || std::isnan(mean))
    {
      return {std::nan(""), mean, std::nan("")};
    }
    double sumSq = 0.0;
    LRVX_INDICATOR_NO_UNROLL
    for (double v : _sma.window())
    {
      double diff = v - mean;
      sumSq += diff * diff;
    }
    double sd = std::sqrt(sumSq / static_cast<double>(_period));
    return {mean + _stddev * sd, mean, mean - _stddev * sd};
  }

  void reset() { _sma.reset(); }

 private:
  size_t _period;
  double _stddev;
  SmaKernel _sma;
};

}  // namespace detail

class Bollinger
{
 public:
  explicit Bollinger(size_t period = 20, double stddev = 2.0)
      : _period(period), _stddev(stddev), _kernel(period, stddev)
  {
  }

  // ── Streaming API ─────────────────────────────────────────────────
  void update(double v)
  {
    _last = _kernel.step(v);
    ++_count;
  }
  double value() const noexcept { return _last.middle; }
  double upperValue() const noexcept { return _last.upper; }
  double middleValue() const noexcept { return _last.middle; }
  double lowerValue() const noexcept { return _last.lower; }
  bool ready() const noexcept { return std::isfinite(_last.middle); }
  void reset()
  {
    _kernel.reset();
    _last = nanOut();
    _count = 0;
  }
  size_t count() const noexcept { return _count; }

  BollingerResult compute(std::span<const double> input) const
  {
    const size_t n = input.size();
    BollingerResult result;
    result.upper.resize(n, std::nan(""));
    result.middle.resize(n, std::nan(""));
    result.lower.resize(n, std::nan(""));
    compute(input, result.upper, result.middle, result.lower);
    return result;
  }

  void compute(std::span<const double> input, std::span<double> upper, std::span<double> middle,
               std::span<double> lower) const
  {
    const size_t n = input.size();
    assert(upper.size() >= n && middle.size() >= n && lower.size() >= n);
    detail::BollingerKernel kernel(_period, _stddev);
    for (size_t i = 0; i < n; ++i)
    {
      const auto o = kernel.step(input[i]);
      upper[i] = o.upper;
      middle[i] = o.middle;
      lower[i] = o.lower;
    }
  }

  size_t period() const noexcept { return _period; }

 private:
  static detail::BollingerKernel::Out nanOut() noexcept
  {
    return {std::nan(""), std::nan(""), std::nan("")};
  }

  size_t _period;
  double _stddev;

  // streaming state
  detail::BollingerKernel _kernel;
  detail::BollingerKernel::Out _last = nanOut();
  size_t _count = 0;
};

}  // namespace lrvx::indicator

#include "lrvx/indicator/indicator.h"
static_assert(lrvx::indicator::MultiOutputIndicator<lrvx::indicator::Bollinger>);
