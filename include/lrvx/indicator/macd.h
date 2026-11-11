#pragma once

#include "lrvx/indicator/ema.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <span>
#include <vector>

namespace lrvx::indicator
{

struct MacdResult
{
  std::vector<double> line;
  std::vector<double> signal;
  std::vector<double> histogram;
};

namespace detail
{

// MACD step. O(1) per step.
//
// TA-Lib MACD seeds the fast EMA at the slow EMA's start point, not
// independently: fast seed = mean(input[slow - fast .. slow - 1]), first
// value at index slow - 1, then the plain EMA recursion (no NaN skipping,
// so a NaN poisons the fast line). The slow EMA and the signal EMA follow
// EMA's own step and NaN contract. TA-Lib convention: line, signal and
// histogram all start where the signal is first valid and are NaN before.
class MacdKernel
{
 public:
  struct Out
  {
    double line, signal, histogram;
  };

  // Ensures slow >= fast (swaps if needed, matching TA-Lib behavior).
  MacdKernel(size_t fastPeriod, size_t slowPeriod, size_t signalPeriod) noexcept
      : _fastP(std::min(fastPeriod, slowPeriod)),
        _slowP(std::max(fastPeriod, slowPeriod)),
        _fastAlpha(2.0 / (static_cast<double>(_fastP) + 1.0)),
        _slow(EMA(_slowP).kernel()),
        _signal(EMA(signalPeriod).kernel())
  {
  }

  LRVX_INDICATOR_STEP Out step(double x) noexcept
  {
    const size_t i = _index++;
    const double slow = _slow.step(x);

    double fast = std::nan("");
    if (_slowP > 0)
    {
      if (i + 1 < _slowP)
      {
        if (i >= _slowP - _fastP)
        {
          _fastSum += x;
        }
      }
      else if (i + 1 == _slowP)
      {
        if (i >= _slowP - _fastP)
        {
          _fastSum += x;
        }
        _fast = _fastSum / static_cast<double>(_fastP);
        fast = _fast;
      }
      else
      {
        _fast = _fastAlpha * x + (1.0 - _fastAlpha) * _fast;
        fast = _fast;
      }
    }

    double line = std::nan("");
    if (!std::isnan(fast) && !std::isnan(slow))
    {
      line = fast - slow;
    }
    const double sig = _signal.step(line);
    if (std::isnan(sig))
    {
      return {std::nan(""), std::nan(""), std::nan("")};
    }
    return {line, sig, line - sig};
  }

  void reset() noexcept
  {
    _index = 0;
    _slow.reset();
    _signal.reset();
    _fastSum = 0.0;
    _fast = std::nan("");
  }

 private:
  size_t _fastP;
  size_t _slowP;
  double _fastAlpha;
  ExpSmoothingKernel _slow;
  ExpSmoothingKernel _signal;
  size_t _index = 0;
  double _fastSum = 0.0;
  double _fast = std::nan("");
};

}  // namespace detail

class MACD
{
 public:
  MACD(size_t fastPeriod = 12, size_t slowPeriod = 26, size_t signalPeriod = 9) noexcept
      : _fastPeriod(fastPeriod),
        _slowPeriod(slowPeriod),
        _signalPeriod(signalPeriod),
        _kernel(fastPeriod, slowPeriod, signalPeriod)
  {
  }

  // ── Streaming API ─────────────────────────────────────────────────
  void update(double v)
  {
    _last = _kernel.step(v);
    ++_count;
  }
  double value() const noexcept { return _last.line; }
  double signalValue() const noexcept { return _last.signal; }
  double histogramValue() const noexcept { return _last.histogram; }
  bool ready() const noexcept { return std::isfinite(_last.line); }
  void reset() noexcept
  {
    _kernel.reset();
    _last = nanOut();
    _count = 0;
  }
  size_t count() const noexcept { return _count; }

  MacdResult compute(std::span<const double> input) const
  {
    MacdResult result;
    result.line.resize(input.size(), std::nan(""));
    result.signal.resize(input.size(), std::nan(""));
    result.histogram.resize(input.size(), std::nan(""));
    compute(input, result.line, result.signal, result.histogram);
    return result;
  }

  void compute(std::span<const double> input, std::span<double> line, std::span<double> signal,
               std::span<double> histogram) const
  {
    const size_t n = input.size();
    assert(line.size() >= n && signal.size() >= n && histogram.size() >= n);
    detail::MacdKernel kernel(_fastPeriod, _slowPeriod, _signalPeriod);
    for (size_t i = 0; i < n; ++i)
    {
      const auto o = kernel.step(input[i]);
      line[i] = o.line;
      signal[i] = o.signal;
      histogram[i] = o.histogram;
    }
  }

  size_t period() const noexcept { return _slowPeriod + _signalPeriod - 1; }

 private:
  static detail::MacdKernel::Out nanOut() noexcept
  {
    return {std::nan(""), std::nan(""), std::nan("")};
  }

  size_t _fastPeriod;
  size_t _slowPeriod;
  size_t _signalPeriod;

  // streaming state
  detail::MacdKernel _kernel;
  detail::MacdKernel::Out _last = nanOut();
  size_t _count = 0;
};

}  // namespace lrvx::indicator

#include "lrvx/indicator/indicator.h"
static_assert(lrvx::indicator::MultiOutputIndicator<lrvx::indicator::MACD>);
