#pragma once

#include "lrvx/indicator/ema.h"
#include "lrvx/indicator/streaming.h"

#include <cassert>
#include <cmath>
#include <span>
#include <vector>

namespace lrvx::indicator
{

namespace detail
{

// DEMA = 2*EMA(x) - EMA(EMA(x)): two chained EMA steps, O(1).
class DemaKernel
{
 public:
  explicit DemaKernel(const EMA& ema) noexcept : _e1(ema.kernel()), _e2(ema.kernel()) {}

  LRVX_INDICATOR_STEP double step(double x) noexcept
  {
    const double a = _e1.step(x);
    const double b = _e2.step(a);
    if (!std::isnan(a) && !std::isnan(b))
    {
      return 2.0 * a - b;
    }
    return std::nan("");
  }

  void reset() noexcept
  {
    _e1.reset();
    _e2.reset();
  }

 private:
  ExpSmoothingKernel _e1, _e2;
};

// TEMA = 3*EMA - 3*EMA(EMA) + EMA(EMA(EMA)): three chained EMA steps, O(1).
class TemaKernel
{
 public:
  explicit TemaKernel(const EMA& ema) noexcept
      : _e1(ema.kernel()), _e2(ema.kernel()), _e3(ema.kernel())
  {
  }

  LRVX_INDICATOR_STEP double step(double x) noexcept
  {
    const double a = _e1.step(x);
    const double b = _e2.step(a);
    const double c = _e3.step(b);
    if (!std::isnan(a) && !std::isnan(b) && !std::isnan(c))
    {
      return 3.0 * a - 3.0 * b + c;
    }
    return std::nan("");
  }

  void reset() noexcept
  {
    _e1.reset();
    _e2.reset();
    _e3.reset();
  }

 private:
  ExpSmoothingKernel _e1, _e2, _e3;
};

}  // namespace detail

class DEMA : public StreamingSingle<DEMA, detail::DemaKernel>
{
 public:
  explicit DEMA(size_t period) noexcept
      : StreamingSingle(detail::DemaKernel(EMA(period))), _ema(period), _period(period)
  {
  }

  std::vector<double> compute(std::span<const double> input) const
  {
    std::vector<double> out(input.size(), std::nan(""));
    fold(detail::DemaKernel(_ema), input, out);
    return out;
  }

  size_t period() const noexcept { return _period; }

 private:
  EMA _ema;
  size_t _period;
};

class TEMA : public StreamingSingle<TEMA, detail::TemaKernel>
{
 public:
  explicit TEMA(size_t period) noexcept
      : StreamingSingle(detail::TemaKernel(EMA(period))), _ema(period), _period(period)
  {
  }

  std::vector<double> compute(std::span<const double> input) const
  {
    std::vector<double> out(input.size(), std::nan(""));
    fold(detail::TemaKernel(_ema), input, out);
    return out;
  }

  size_t period() const noexcept { return _period; }

 private:
  EMA _ema;
  size_t _period;
};

}  // namespace lrvx::indicator

#include "lrvx/indicator/indicator.h"
static_assert(lrvx::indicator::SingleIndicator<lrvx::indicator::DEMA>);
static_assert(lrvx::indicator::SingleIndicator<lrvx::indicator::TEMA>);
