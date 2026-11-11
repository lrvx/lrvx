#pragma once

#include "lrvx/indicator/streaming.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <span>
#include <vector>

namespace lrvx::indicator
{

namespace detail
{

// Normalised Shannon entropy of the last `period` inputs binned into `bins`
// equal-width bins between the window's min and max. O(period + bins) per
// step: the bins depend on the window's range, so the histogram is rebuilt
// each step. A constant window has entropy 0.
class ShannonEntropyKernel
{
 public:
  ShannonEntropyKernel(size_t period, size_t bins) : _period(period), _bins(bins), _win(period), _counts(bins)
  {
  }

  LRVX_INDICATOR_STEP double step(double x)
  {
    _win.push(x);
    if (_period == 0 || !_win.full())
    {
      return std::nan("");
    }
    const auto w = _win.window();

    double lo = w[0], hi = w[0];
    for (size_t j = 1; j < _period; ++j)
    {
      if (w[j] < lo)
      {
        lo = w[j];
      }
      if (w[j] > hi)
      {
        hi = w[j];
      }
    }

    if (lo == hi)
    {
      return 0.0;
    }

    std::fill(_counts.begin(), _counts.end(), 0);
    double range = hi - lo;

    for (double v : w)
    {
      size_t bin = static_cast<size_t>(((v - lo) / range) * _bins);
      if (bin >= _bins)
      {
        bin = _bins - 1;
      }
      _counts[bin]++;
    }

    double entropy = 0;
    double p_denom = static_cast<double>(_period);
    for (size_t b = 0; b < _bins; ++b)
    {
      if (_counts[b] > 0)
      {
        double p = static_cast<double>(_counts[b]) / p_denom;
        entropy -= p * std::log(p);
      }
    }

    return entropy / std::log(static_cast<double>(_bins));
  }

  void reset() { _win.clear(); }

 private:
  size_t _period;
  size_t _bins;
  Ring _win;
  std::vector<size_t> _counts;
};

}  // namespace detail

class ShannonEntropy : public StreamingSingle<ShannonEntropy, detail::ShannonEntropyKernel>
{
 public:
  explicit ShannonEntropy(size_t period, size_t bins = 10)
      : StreamingSingle(detail::ShannonEntropyKernel(period, bins)), _period(period), _bins(bins)
  {
    assert(period >= 2);
    assert(bins >= 2);
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
    fold(detail::ShannonEntropyKernel(_period, _bins), input, output);
  }

  size_t period() const noexcept { return _period; }

 private:
  size_t _period;
  size_t _bins;
};

}  // namespace lrvx::indicator

#include "lrvx/indicator/indicator.h"
static_assert(lrvx::indicator::SingleIndicator<lrvx::indicator::ShannonEntropy>);
