#pragma once

#include "lrvx/indicator/streaming.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <span>
#include <vector>

namespace lrvx::indicator
{

struct StochasticResult
{
  std::vector<double> k;
  std::vector<double> d;
};

namespace detail
{

// Stochastic %K / %D step. O(kPeriod + dPeriod) per step: the highest high
// and lowest low are rescanned over the last kPeriod bars and %D sums the
// last dPeriod raw %K values, in the original batch order.
//
// TA-Lib aligns K and D output to the same start index,
// kPeriod - 1 + dPeriod - 1. A flat range reports the neutral 50.
class StochasticKernel
{
 public:
  struct Out
  {
    double k, d;
  };

  StochasticKernel(size_t kPeriod, size_t dPeriod)
      : _kPeriod(kPeriod),
        _dPeriod(dPeriod),
        _firstValid(kPeriod - 1 + dPeriod - 1),
        _high(kPeriod),
        _low(kPeriod),
        _rawK(dPeriod)
  {
    const size_t kStart = _firstValid >= dPeriod - 1 ? _firstValid - dPeriod + 1 : 0;
    _rawKStart = std::max(kStart, kPeriod - 1);
  }

  LRVX_INDICATOR_STEP Out step(double high, double low, double close)
  {
    const size_t i = _index++;
    _high.push(high);
    _low.push(low);

    double rawK = std::nan("");
    if (i >= _rawKStart)
    {
      // Window is high/low[i - kPeriod + 1 .. i]; the scan starts from bar i
      // and then visits the older bars oldest first, as the batch loop did.
      const auto h = _high.window();
      const auto l = _low.window();
      double hh = high;
      double ll = low;
      for (size_t j = 0; j + 1 < _kPeriod; ++j)
      {
        if (h[j] > hh)
        {
          hh = h[j];
        }
        if (l[j] < ll)
        {
          ll = l[j];
        }
      }
      double range = hh - ll;
      rawK = range > 0 ? 100.0 * (close - ll) / range : 50.0;
    }
    _rawK.push(rawK);

    if (i < _firstValid)
    {
      return {std::nan(""), std::nan("")};
    }
    double sum = 0.0;
    // The batch loop summed rawK[i - dPeriod + 1 .. i]; when that start
    // index wraps below zero the range is empty.
    if (i - _dPeriod + 1 <= i)
    {
      for (double v : _rawK.window())
      {
        sum += v;
      }
    }
    return {rawK, sum / static_cast<double>(_dPeriod)};
  }

  void reset()
  {
    _index = 0;
    _high.clear();
    _low.clear();
    _rawK.clear();
  }

 private:
  size_t _kPeriod;
  size_t _dPeriod;
  size_t _firstValid;
  size_t _rawKStart = 0;
  Ring _high, _low, _rawK;
  size_t _index = 0;
};

}  // namespace detail

class Stochastic
{
 public:
  Stochastic(size_t kPeriod = 14, size_t dPeriod = 3)
      : _kPeriod(kPeriod), _dPeriod(dPeriod), _kernel(kPeriod, dPeriod)
  {
  }

  // ── Streaming API ─────────────────────────────────────────────────
  void update(double high, double low, double close)
  {
    _last = _kernel.step(high, low, close);
    ++_count;
  }
  double value() const noexcept { return _last.k; }
  double kValue() const noexcept { return _last.k; }
  double dValue() const noexcept { return _last.d; }
  bool ready() const noexcept { return std::isfinite(_last.k) && std::isfinite(_last.d); }
  void reset()
  {
    _kernel.reset();
    _last = {std::nan(""), std::nan("")};
    _count = 0;
  }
  size_t count() const noexcept { return _count; }

  StochasticResult compute(std::span<const double> high, std::span<const double> low,
                           std::span<const double> close) const
  {
    const size_t n = high.size();
    StochasticResult result;
    result.k.resize(n, std::nan(""));
    result.d.resize(n, std::nan(""));
    detail::StochasticKernel kernel(_kPeriod, _dPeriod);
    for (size_t i = 0; i < n; ++i)
    {
      const auto o = kernel.step(high[i], low[i], close[i]);
      result.k[i] = o.k;
      result.d[i] = o.d;
    }
    return result;
  }

  size_t period() const noexcept { return _kPeriod; }

 private:
  size_t _kPeriod;
  size_t _dPeriod;

  // streaming state
  detail::StochasticKernel _kernel;
  detail::StochasticKernel::Out _last{std::nan(""), std::nan("")};
  size_t _count = 0;
};

}  // namespace lrvx::indicator

#include "lrvx/indicator/indicator.h"
static_assert(lrvx::indicator::HasPeriod<lrvx::indicator::Stochastic>);
