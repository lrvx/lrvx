# Add a new indicator

!!! info "C++ extension point"
    Indicators are defined once in C++ and surface automatically in every binding. Most users don't need this guide — the [built-in indicators](../reference/python/indicators.md) cover the standard set.

Adding a new indicator takes **one C++ class plus one line in a
registry**. After those two changes it appears in C++,
Python, every binding, in `lrvx.list_indicators()` discovery, in the parity
tests, and in the docs site, all automatically.

## 1. Write the C++ class

Create `include/lrvx/indicator/my_indicator.h`. The math lives in one place:
a small state object, the kernel, whose `step()` takes one input and returns
the indicator's value at that position (NaN until it is ready). Inherit from
the matching streaming mixin with that kernel to get
`update()`/`value()`/`ready()`/`reset()`/`count()` on the same class, and
implement `compute()` by folding a fresh kernel over the input:

```cpp
#pragma once

#include "lrvx/indicator/streaming.h"

#include <cmath>
#include <span>
#include <vector>

namespace lrvx::indicator
{

namespace detail
{

// Mean of the last `period` inputs, recomputed over a ring buffer.
class MyIndicatorKernel
{
 public:
  explicit MyIndicatorKernel(size_t period) : _period(period), _window(period) {}

  LRVX_INDICATOR_STEP double step(double x)
  {
    _window.push(x);
    if (_period == 0 || !_window.full())
    {
      return std::nan("");
    }
    double sum = 0.0;
    for (double v : _window.window())
    {
      sum += v;
    }
    return sum / static_cast<double>(_period);
  }

  void reset() { _window.clear(); }

 private:
  size_t _period;
  Ring _window;
};

}  // namespace detail

class MyIndicator : public StreamingSingle<MyIndicator, detail::MyIndicatorKernel>
{
 public:
  explicit MyIndicator(size_t period)
      : StreamingSingle(detail::MyIndicatorKernel(period)), _period(period)
  {
  }

  std::vector<double> compute(std::span<const double> input) const
  {
    std::vector<double> out(input.size(), std::nan(""));
    fold(detail::MyIndicatorKernel(_period), input, out);
    return out;
  }

  size_t period() const noexcept { return _period; }

 private:
  size_t _period;
};

}  // namespace lrvx::indicator

#include "lrvx/indicator/indicator.h"
static_assert(lrvx::indicator::SingleIndicator<lrvx::indicator::MyIndicator>);
```

Keep everything the kernel needs inside it and size it in the constructor:
`update()` must not allocate, and its cost must depend on the window, never on
how many values the indicator has seen. `tests/test_indicator_streaming_cost.cpp`
checks both for the built-in indicators.

The streaming mixin you choose depends on the input shape; the kernel's
`step()` takes the same arguments as `update()`:

| Input | Mixin |
|---|---|
| Single series (`compute(span)`) | `StreamingSingle<T, Kernel>` |
| `compute(high, low, close)` | `StreamingBar<T, Kernel>` |
| `compute(high, low)` | `StreamingHighLow<T, Kernel>` |
| `compute(open, high, low, close)` | `StreamingOhlc<T, Kernel>` |
| `compute(x, y)` | `StreamingPair<T, Kernel>` |
| Multi-output result struct | the class holds its kernel and one getter per output (see `MACD`, `Bollinger`, `Stochastic`) |

## 2. Register it

Add **one line** to `include/lrvx/indicator/registry.def`:

```c
LRVX_INDICATOR(MyIndicator, my_indicator, SingleInput, (size_t period))
```

The `Kind` matches the mixin you used (`SingleInput`, `BarInput`,
`HighLowInput`, `OhlcInput`, `PairInput`, or `MultiOutput`).

## 3. Build, regenerate docs, run tests

```sh
cmake --build build
python3 scripts/gen_indicator_docs.py
ctest --test-dir build
```

You now have:

- `lrvx::indicator::MyIndicator` in C++ with `compute()` (batch) and
  `update()`/`value()`/`ready()`/`reset()` (streaming) on the same instance.
- `lrvx.MyIndicator(...)` in Python with `compute(arr)` and
  `update(v)` / `value` / `ready` / `reset()` on the same instance.
- `lrvx.MyIndicator(...)` in **Node.js** — same surface, same semantics
  (thin N-API wrapper around the same C++ class).
- A C API handle: `lrvx_streaming_indicator_create("my_indicator", args, n)`
  creates one from the registry row, with `_update*` / `_value` / `_ready` /
  `_count` / `_reset` / `_destroy` forwarding to the C++ class (include the
  new header in `src/capi/lrvx_capi.cpp`). A `MultiOutput` row also needs a
  `StreamingIndicatorOutputs` specialisation there, or it does not compile.
  Add the row's constructor arguments to `kCases` in
  `tests/support/streaming_indicator_cases.h`; the C API and QuickJS tests
  fail until you do, and then hold the handle against the class bit for bit.
- `MyIndicator` in **QuickJS** (`quickjs/lrvx/indicators.js`) — a 3-line
  class on one of the `_Stream*` bases, which hold the C handle; same
  `update`/`value`/`ready`/`count`/`reset`, one kernel step per update.
- `MyIndicator` in **Codon** (`codon/lrvx/indicators.codon`) — a class
  around `_Stream`, the same handle; add it to
  `codon/examples/indicator_streaming_smoke.codon`.
- The streaming-vs-batch parity test
  (`tests/test_streaming_parity.cpp`) automatically covers it once you add
  one line to the registry-driven test list.
- `lrvx.list_indicators()` returns it (Python and Node).
- The docs site lists it on every per-binding page (run
  `python3 scripts/gen_indicator_docs.py`).

The parity is **by construction**: `update()` runs one `step()` of the
instance's kernel and `compute()` runs the same `step()` over the whole input
on a fresh kernel, so there is no separate streaming logic to drift. The
parity test compares every step bit for bit.

## 4. Reading the existing 21 indicators

Every indicator listed in `registry.def` (EMA, SMA, RSI, ATR, MACD, ...)
follows exactly this pattern. Look at any `include/lrvx/indicator/<name>.h`
for a working example.

## Cost of a streaming update

An `update()` is one kernel step. The mixin keeps no input history, so the
cost of a step depends only on the indicator's parameters. Recursive
indicators (SMA's running sum, EMA, RMA, RSI, DEMA, TEMA, Slope, ATR, MACD)
step in O(1). Indicators that recompute their window each step to keep the
exact numerics of a two-pass computation (Skewness, Kurtosis, RollingZScore,
Correlation, AutoCorrelation, ShannonEntropy, KAMA, CCI, Bollinger,
Stochastic, ParkinsonVol, RogersSatchellVol) step in O(period). Memory is a
ring buffer of at most the last `period` inputs, allocated when the indicator
is constructed.
