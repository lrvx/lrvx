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
// RSI value given the average gain/loss over the window. avgLoss == 0 with
// avgGain > 0 is a genuine "always up" window and reports 100. avgLoss == 0
// with avgGain == 0 too -- a fully flat window -- is the neutral midpoint
// 50, not 100: see the convention note in RsiKernel::step.
inline double flatRsi(double avgGain, double avgLoss) noexcept
{
  if (avgGain == 0.0 && avgLoss == 0.0)
  {
    return 50.0;
  }
  return avgLoss > 0 ? 100.0 - 100.0 / (1.0 + avgGain / avgLoss) : 100.0;
}

// Wilder RSI step. O(1) per step.
//
// The seed averages the gains and losses of the first `period` differences
// (inputs 1..period against their predecessors), skipping any difference
// with a NaN on either side. After the seed, a NaN on either side of a
// difference holds the previous output.
class RsiKernel
{
 public:
  explicit RsiKernel(size_t period) noexcept : _period(period) {}

  LRVX_INDICATOR_STEP double step(double x) noexcept
  {
    const size_t i = _index++;
    const double prev = _prev;
    _prev = x;

    if (i < _period)
    {
      if (i >= 1)
      {
        accumulateSeed(prev, x);
      }
      return std::nan("");
    }

    if (i == _period)
    {
      if (i >= 1)
      {
        accumulateSeed(prev, x);
      }
      _avgGain /= static_cast<double>(_period);
      _avgLoss /= static_cast<double>(_period);
      // Convention: a fully flat window (avgGain == avgLoss == 0, i.e. the
      // seed has no movement at all) reports the neutral midpoint 50,
      // matching Stochastic's flat-range convention (stochastic.h) instead of
      // the mathematical limit 100 -- which would read as "maximally
      // overbought" on an instrument that has not moved. A pure uptrend
      // (avgLoss == 0, avgGain > 0) is unaffected and still correctly
      // reports 100.
      _last = flatRsi(_avgGain, _avgLoss);
      return _last;
    }

    if (std::isnan(x) || std::isnan(prev))
    {
      return _last;
    }
    const double pm1 = static_cast<double>(_period - 1);
    const double pd = static_cast<double>(_period);
    double d = x - prev;
    double gain = d > 0 ? d : 0.0;
    double loss = d < 0 ? -d : 0.0;
    _avgGain = (_avgGain * pm1 + gain) / pd;
    _avgLoss = (_avgLoss * pm1 + loss) / pd;
    _last = flatRsi(_avgGain, _avgLoss);
    return _last;
  }

  void reset() noexcept
  {
    _index = 0;
    _prev = std::nan("");
    _avgGain = 0.0;
    _avgLoss = 0.0;
    _last = std::nan("");
  }

 private:
  void accumulateSeed(double prev, double x) noexcept
  {
    if (std::isnan(x) || std::isnan(prev))
    {
      return;
    }
    double d = x - prev;
    if (d > 0)
    {
      _avgGain += d;
    }
    else
    {
      _avgLoss -= d;
    }
  }

  size_t _period;
  size_t _index = 0;
  double _prev = std::nan("");
  double _avgGain = 0.0;
  double _avgLoss = 0.0;
  double _last = std::nan("");
};
}  // namespace detail

class RSI : public StreamingSingle<RSI, detail::RsiKernel>
{
 public:
  explicit RSI(size_t period) noexcept : StreamingSingle(detail::RsiKernel(period)), _period(period)
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
    fold(detail::RsiKernel(_period), input, output);
  }

  size_t period() const noexcept { return _period; }

 private:
  size_t _period;
};

}  // namespace lrvx::indicator

#include "lrvx/indicator/indicator.h"
static_assert(lrvx::indicator::SingleIndicator<lrvx::indicator::RSI>);
