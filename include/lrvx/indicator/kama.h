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

// KAMA step. O(period) per step: the volatility sum over the last `period`
// differences is recomputed each step, in the same order as the original
// batch loop, so outputs keep their exact values.
//
// TA-Lib seeds KAMA with input[period - 1]; the first output is at index
// `period`. A NaN input poisons the recursion from that point on. A zero
// period never produces a value.
class KamaKernel
{
 public:
  KamaKernel(size_t period, double fr, double sr) : _period(period), _fr(fr), _sr(sr), _win(period + 1)
  {
  }

  LRVX_INDICATOR_STEP double step(double x)
  {
    _win.push(x);
    if (_period == 0 || !_win.full())
    {
      return std::nan("");
    }
    const auto w = _win.window();  // input[i - period .. i]
    if (!_seeded)
    {
      _prev = w[_period - 1];
      _seeded = true;
    }

    double direction = std::abs(w[_period] - w[0]);
    double volatility = 0.0;
    for (size_t j = 1; j <= _period; ++j)
    {
      volatility += std::abs(w[j] - w[j - 1]);
    }
    double er = volatility > 0 ? direction / volatility : 0.0;
    double sc = (er * (_fr - _sr) + _sr);
    sc *= sc;
    _prev = sc * x + (1.0 - sc) * _prev;
    return _prev;
  }

  void reset()
  {
    _win.clear();
    _seeded = false;
    _prev = 0.0;
  }

 private:
  size_t _period;
  double _fr;
  double _sr;
  Ring _win;
  bool _seeded = false;
  double _prev = 0.0;
};

}  // namespace detail

class KAMA : public StreamingSingle<KAMA, detail::KamaKernel>
{
 public:
  explicit KAMA(size_t period, size_t fast = 2, size_t slow = 30)
      : StreamingSingle(detail::KamaKernel(period, 2.0 / (fast + 1), 2.0 / (slow + 1))),
        _period(period),
        _fr(2.0 / (fast + 1)),
        _sr(2.0 / (slow + 1))
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
    fold(detail::KamaKernel(_period, _fr, _sr), input, output);
  }

  size_t period() const noexcept { return _period; }

 private:
  size_t _period;
  double _fr;
  double _sr;
};

}  // namespace lrvx::indicator

#include "lrvx/indicator/indicator.h"
static_assert(lrvx::indicator::SingleIndicator<lrvx::indicator::KAMA>);
