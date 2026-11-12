#pragma once

// lrvx/indicator/streaming.h
//
// CRTP mixins that give an indicator a streaming API (update / value / ready /
// reset / count) on the same class that provides batch compute().
//
// Contract:
//
//   - The step is the primitive. Each indicator defines a small state object,
//     its kernel, whose step() takes one input (or one bar's inputs) and
//     returns the indicator's output at that position, NaN while not ready.
//   - update() advances the instance's kernel by exactly one step and stores
//     the result. value() and ready() read that stored result. No input
//     history is kept: the kernel holds only what its window needs (at most a
//     ring buffer of the last `period` inputs), all allocated when the
//     indicator is constructed.
//   - compute(span...) builds a fresh kernel and folds it over the input.
//     Batch and streaming run the same step, so value() after k updates
//     equals compute(input)[k - 1] bit for bit.
//
// Cost per update is O(1) for the recursive indicators (SMA, EMA, RMA, RSI,
// DEMA, TEMA, Slope, ATR, MACD) and O(period) for the ones that recompute
// their window on every step to keep the exact numerics of a two-pass
// computation (the rolling moments, correlation, entropy, range, choppiness
// and volatility estimators). It never depends on how many values the indicator
// has already seen.

#include <cmath>
#include <cstddef>
#include <span>
#include <utility>
#include <vector>

// Kernel step() functions are kept out of line. compute() and update() then
// run the same machine code for a step, so batch and streaming agree bit for
// bit in every build, not just at the source level: the compiler may fuse a
// multiply-add (a * b + c) into one FMA or not depending on the surrounding
// code, and an inlined step could round differently in the batch loop than in
// a binding's update() call.
#if defined(_MSC_VER) && !defined(__clang__)
#define LRVX_INDICATOR_STEP __declspec(noinline)
#else
#define LRVX_INDICATOR_STEP __attribute__((noinline))
#endif

// Marks a window loop that must not be unrolled. Unrolling lets clang split
// some of the loop's contracted multiply-adds back into a multiply and an add,
// which changes the last bits of the result relative to the batch loops these
// kernels replaced.
#if defined(__clang__)
#define LRVX_INDICATOR_NO_UNROLL _Pragma("clang loop unroll(disable)")
#else
#define LRVX_INDICATOR_NO_UNROLL
#endif

namespace lrvx::indicator
{

namespace detail
{

// Fixed-capacity ring of the last `capacity` values. Every value is written
// twice, at `head` and `head + capacity`, so the last `capacity` values are
// always one contiguous oldest-to-newest span and window loops need no index
// wrapping. window() is meaningful once full(); before that its leading slots
// hold zeros. Memory is allocated once, in the constructor.
class Ring
{
 public:
  explicit Ring(size_t capacity) : _cap(capacity), _buf(2 * capacity, 0.0) {}

  void push(double v) noexcept
  {
    if (_cap == 0)
    {
      return;
    }
    _buf[_head] = v;
    _buf[_head + _cap] = v;
    _head = (_head + 1 == _cap) ? 0 : _head + 1;
    if (_size < _cap)
    {
      ++_size;
    }
  }

  // Oldest value still held (the one the next push() evicts once full).
  double oldest() const noexcept { return _buf[_head]; }

  // The last capacity() values, oldest first.
  std::span<const double> window() const noexcept
  {
    return std::span<const double>(_buf.data() + _head, _cap);
  }

  size_t size() const noexcept { return _size; }
  size_t capacity() const noexcept { return _cap; }
  bool full() const noexcept { return _size == _cap; }

  void clear() noexcept
  {
    _head = 0;
    _size = 0;
  }

 private:
  size_t _cap;
  std::vector<double> _buf;
  size_t _head = 0;
  size_t _size = 0;
};

// Shared state of every single-value streaming mixin.
template <typename Kernel>
class StreamingCore
{
 public:
  double value() const noexcept { return _last; }
  bool ready() const noexcept { return std::isfinite(_last); }

  void reset()
  {
    _kernel.reset();
    _last = std::nan("");
    _count = 0;
  }

  size_t count() const noexcept { return _count; }

 protected:
  explicit StreamingCore(Kernel kernel) : _kernel(std::move(kernel)) {}

  template <typename... In>
  void advance(In... in)
  {
    _last = _kernel.step(in...);
    ++_count;
  }

 private:
  Kernel _kernel;
  double _last = std::nan("");
  size_t _count = 0;
};

}  // namespace detail

// -----------------------------------------------------------------------------
// SingleInput: compute(span) -> vector<double>; Kernel::step(double)
// -----------------------------------------------------------------------------
template <typename Derived, typename Kernel>
class StreamingSingle : public detail::StreamingCore<Kernel>
{
 public:
  void update(double v) { this->advance(v); }

 protected:
  explicit StreamingSingle(Kernel kernel) : detail::StreamingCore<Kernel>(std::move(kernel)) {}

  // Runs `kernel` (fresh) over `input`, writing one output per input.
  static void fold(Kernel kernel, std::span<const double> input, std::span<double> output)
  {
    for (size_t i = 0; i < input.size(); ++i)
    {
      output[i] = kernel.step(input[i]);
    }
  }
};

// -----------------------------------------------------------------------------
// BarInput: compute(high, low, close) -> vector<double>; Kernel::step(h, l, c)
// -----------------------------------------------------------------------------
template <typename Derived, typename Kernel>
class StreamingBar : public detail::StreamingCore<Kernel>
{
 public:
  void update(double high, double low, double close) { this->advance(high, low, close); }

 protected:
  explicit StreamingBar(Kernel kernel) : detail::StreamingCore<Kernel>(std::move(kernel)) {}

  static void fold(Kernel kernel, std::span<const double> high, std::span<const double> low,
                   std::span<const double> close, std::span<double> output)
  {
    for (size_t i = 0; i < high.size(); ++i)
    {
      output[i] = kernel.step(high[i], low[i], close[i]);
    }
  }
};

// -----------------------------------------------------------------------------
// HighLowInput: compute(high, low) -> vector<double>; Kernel::step(h, l)
// -----------------------------------------------------------------------------
template <typename Derived, typename Kernel>
class StreamingHighLow : public detail::StreamingCore<Kernel>
{
 public:
  void update(double high, double low) { this->advance(high, low); }

 protected:
  explicit StreamingHighLow(Kernel kernel) : detail::StreamingCore<Kernel>(std::move(kernel)) {}

  static void fold(Kernel kernel, std::span<const double> high, std::span<const double> low,
                   std::span<double> output)
  {
    for (size_t i = 0; i < high.size(); ++i)
    {
      output[i] = kernel.step(high[i], low[i]);
    }
  }
};

// -----------------------------------------------------------------------------
// OhlcInput: compute(open, high, low, close) -> vector<double>;
// Kernel::step(o, h, l, c)
// -----------------------------------------------------------------------------
template <typename Derived, typename Kernel>
class StreamingOhlc : public detail::StreamingCore<Kernel>
{
 public:
  void update(double open, double high, double low, double close)
  {
    this->advance(open, high, low, close);
  }

 protected:
  explicit StreamingOhlc(Kernel kernel) : detail::StreamingCore<Kernel>(std::move(kernel)) {}

  static void fold(Kernel kernel, std::span<const double> open, std::span<const double> high,
                   std::span<const double> low, std::span<const double> close,
                   std::span<double> output)
  {
    for (size_t i = 0; i < open.size(); ++i)
    {
      output[i] = kernel.step(open[i], high[i], low[i], close[i]);
    }
  }
};

// -----------------------------------------------------------------------------
// PairInput: compute(x, y) -> vector<double>; Kernel::step(x, y)
// -----------------------------------------------------------------------------
template <typename Derived, typename Kernel>
class StreamingPair : public detail::StreamingCore<Kernel>
{
 public:
  void update(double x, double y) { this->advance(x, y); }

 protected:
  explicit StreamingPair(Kernel kernel) : detail::StreamingCore<Kernel>(std::move(kernel)) {}

  static void fold(Kernel kernel, std::span<const double> x, std::span<const double> y,
                   std::span<double> output)
  {
    for (size_t i = 0; i < x.size(); ++i)
    {
      output[i] = kernel.step(x[i], y[i]);
    }
  }
};

// Multi-output indicators (MACD, Bollinger, Stochastic) hold their kernel
// directly and expose one getter per output; see those headers.

}  // namespace lrvx::indicator
