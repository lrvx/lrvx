// tests/test_streaming_parity.cpp
//
// Every indicator in registry.def exposes both compute() and
// update()/value(). Feeding the same input through both paths must give the
// same output at every step, bit for bit: value() after update k equals
// compute(input)[k] exactly, with NaN matching NaN.
//
// Both paths run the same per-indicator step kernel (compute() folds a fresh
// kernel over the input, update() advances the instance's kernel by one
// step), so any difference means the mixin or a hand-written streaming path
// diverged from the batch one. A broken compute() itself is out of scope here
// (test_indicators.cpp owns that).
//
// Each case runs twice: on a clean random series and on the same series with
// NaN sprinkled in, so the NaN contracts (SMA's poisoned window, EMA/RMA/RSI/
// ATR holding through NaN, ...) are held to the same exactness.

#include "lrvx/indicator/atr.h"
#include "lrvx/indicator/autocorrelation.h"
#include "lrvx/indicator/bollinger.h"
#include "lrvx/indicator/cci.h"
#include "lrvx/indicator/correlation.h"
#include "lrvx/indicator/dema.h"
#include "lrvx/indicator/ema.h"
#include "lrvx/indicator/kama.h"
#include "lrvx/indicator/kurtosis.h"
#include "lrvx/indicator/macd.h"
#include "lrvx/indicator/parkinson_vol.h"
#include "lrvx/indicator/rma.h"
#include "lrvx/indicator/rogers_satchell_vol.h"
#include "lrvx/indicator/rolling_zscore.h"
#include "lrvx/indicator/rsi.h"
#include "lrvx/indicator/shannon_entropy.h"
#include "lrvx/indicator/skewness.h"
#include "lrvx/indicator/slope.h"
#include "lrvx/indicator/sma.h"
#include "lrvx/indicator/stochastic.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace
{

std::vector<double> randomSeries(size_t n, uint32_t seed, double lo = 100.0, double hi = 200.0)
{
  std::mt19937 rng(seed);
  std::uniform_real_distribution<double> dist(lo, hi);
  std::vector<double> v(n);
  for (size_t i = 0; i < n; ++i)
  {
    v[i] = dist(rng);
  }
  return v;
}

struct Hlc
{
  std::vector<double> high, low, close;
};

Hlc randomHlc(size_t n, uint32_t seed)
{
  std::mt19937 rng(seed);
  std::uniform_real_distribution<double> mid(100.0, 200.0);
  std::uniform_real_distribution<double> spread(0.5, 5.0);
  Hlc out;
  out.high.resize(n);
  out.low.resize(n);
  out.close.resize(n);
  for (size_t i = 0; i < n; ++i)
  {
    double m = mid(rng), s = spread(rng);
    out.high[i] = m + s;
    out.low[i] = m - s;
    std::uniform_real_distribution<double> cd(out.low[i], out.high[i]);
    out.close[i] = cd(rng);
  }
  return out;
}

std::vector<double> withNaNs(std::vector<double> v, uint32_t seed)
{
  std::mt19937 rng(seed);
  std::uniform_int_distribution<int> pick(0, 9);
  for (auto& x : v)
  {
    if (pick(rng) == 0)
    {
      x = std::nan("");
    }
  }
  return v;
}

Hlc withNaNs(Hlc h, uint32_t seed)
{
  h.high = withNaNs(std::move(h.high), seed);
  h.low = withNaNs(std::move(h.low), seed + 1);
  h.close = withNaNs(std::move(h.close), seed + 2);
  return h;
}

uint64_t bits(double v)
{
  uint64_t b;
  std::memcpy(&b, &v, sizeof b);
  return b;
}

// Exact equality on the bit pattern; any NaN equals any NaN.
void expectSame(double stream, double batch, const char* name, size_t step)
{
  if (std::isnan(stream) && std::isnan(batch))
  {
    return;
  }
  EXPECT_EQ(bits(stream), bits(batch))
      << name << " step " << step << ": stream " << stream << " vs batch " << batch;
}

constexpr size_t kLen = 400;

}  // namespace

using namespace lrvx::indicator;

// ---- Single input ---------------------------------------------------------

template <typename T, typename... Args>
void checkSingleParityOn(const char* name, const std::vector<double>& input, Args... args)
{
  T batch(args...);
  auto out = batch.compute(input);
  ASSERT_EQ(out.size(), input.size());

  T stream(args...);
  for (size_t i = 0; i < input.size(); ++i)
  {
    stream.update(input[i]);
    expectSame(stream.value(), out[i], name, i);
    EXPECT_EQ(stream.ready(), std::isfinite(out[i])) << name << " step " << i;
  }
  EXPECT_EQ(stream.count(), input.size());

  // reset() returns to the just-constructed state.
  stream.reset();
  EXPECT_EQ(stream.count(), 0u);
  EXPECT_TRUE(std::isnan(stream.value()));
  for (size_t i = 0; i < input.size(); ++i)
  {
    stream.update(input[i]);
    expectSame(stream.value(), out[i], name, i);
  }
}

template <typename T, typename... Args>
void checkSingleParity(const char* name, Args... args)
{
  auto clean = randomSeries(kLen, 42);
  checkSingleParityOn<T>(name, clean, args...);
  checkSingleParityOn<T>(name, withNaNs(clean, 9), args...);
}

TEST(StreamingParity, EMA) { checkSingleParity<EMA>("EMA", size_t{10}); }
TEST(StreamingParity, SMA) { checkSingleParity<SMA>("SMA", size_t{10}); }
TEST(StreamingParity, RMA) { checkSingleParity<RMA>("RMA", size_t{10}); }
TEST(StreamingParity, RSI) { checkSingleParity<RSI>("RSI", size_t{14}); }
TEST(StreamingParity, DEMA) { checkSingleParity<DEMA>("DEMA", size_t{10}); }
TEST(StreamingParity, TEMA) { checkSingleParity<TEMA>("TEMA", size_t{10}); }
TEST(StreamingParity, KAMA) { checkSingleParity<KAMA>("KAMA", size_t{10}, size_t{2}, size_t{30}); }
TEST(StreamingParity, Slope) { checkSingleParity<Slope>("Slope", size_t{10}); }
TEST(StreamingParity, Skewness) { checkSingleParity<Skewness>("Skewness", size_t{10}); }
TEST(StreamingParity, Kurtosis) { checkSingleParity<Kurtosis>("Kurtosis", size_t{10}); }
TEST(StreamingParity, RollingZScore) { checkSingleParity<RollingZScore>("RollingZScore", size_t{10}); }
TEST(StreamingParity, ShannonEntropy)
{
  checkSingleParity<ShannonEntropy>("ShannonEntropy", size_t{10}, size_t{5});
}
TEST(StreamingParity, AutoCorrelation)
{
  checkSingleParity<AutoCorrelation>("AutoCorrelation", size_t{10}, size_t{1});
}

// ---- Bar input -----------------------------------------------------------

template <typename T, typename... Args>
void checkBarParityOn(const char* name, const Hlc& bars, Args... args)
{
  T batch(args...);
  auto out = batch.compute(std::span<const double>(bars.high), std::span<const double>(bars.low),
                           std::span<const double>(bars.close));
  ASSERT_EQ(out.size(), bars.high.size());

  T stream(args...);
  for (size_t i = 0; i < bars.high.size(); ++i)
  {
    stream.update(bars.high[i], bars.low[i], bars.close[i]);
    expectSame(stream.value(), out[i], name, i);
    EXPECT_EQ(stream.ready(), std::isfinite(out[i])) << name << " step " << i;
  }
  EXPECT_EQ(stream.count(), bars.high.size());
}

template <typename T, typename... Args>
void checkBarParity(const char* name, Args... args)
{
  auto clean = randomHlc(kLen, 42);
  checkBarParityOn<T>(name, clean, args...);
  checkBarParityOn<T>(name, withNaNs(clean, 9), args...);
}

TEST(StreamingParity, ATR) { checkBarParity<ATR>("ATR", size_t{14}); }
TEST(StreamingParity, CCI) { checkBarParity<CCI>("CCI", size_t{20}); }

// Stochastic (multi-output bar)
TEST(StreamingParity, Stochastic)
{
  for (const auto& bars : {randomHlc(kLen, 42), withNaNs(randomHlc(kLen, 42), 9)})
  {
    Stochastic s(14, 3);
    auto r = s.compute(std::span<const double>(bars.high), std::span<const double>(bars.low),
                       std::span<const double>(bars.close));
    Stochastic stream(14, 3);
    for (size_t i = 0; i < bars.high.size(); ++i)
    {
      stream.update(bars.high[i], bars.low[i], bars.close[i]);
      expectSame(stream.value(), r.k[i], "Stochastic.value", i);
      expectSame(stream.kValue(), r.k[i], "Stochastic.k", i);
      expectSame(stream.dValue(), r.d[i], "Stochastic.d", i);
      EXPECT_EQ(stream.ready(), std::isfinite(r.k[i]) && std::isfinite(r.d[i]));
    }
    EXPECT_EQ(stream.count(), bars.high.size());
  }
}

// ---- High/Low only -------------------------------------------------------

TEST(StreamingParity, ParkinsonVol)
{
  for (const auto& bars : {randomHlc(kLen, 42), withNaNs(randomHlc(kLen, 42), 9)})
  {
    ParkinsonVol p(14);
    auto out = p.compute(std::span<const double>(bars.high), std::span<const double>(bars.low));
    ParkinsonVol stream(14);
    for (size_t i = 0; i < bars.high.size(); ++i)
    {
      stream.update(bars.high[i], bars.low[i]);
      expectSame(stream.value(), out[i], "ParkinsonVol", i);
    }
    EXPECT_EQ(stream.count(), bars.high.size());
  }
}

// ---- OHLC ----------------------------------------------------------------

TEST(StreamingParity, RogersSatchellVol)
{
  for (const auto& bars : {randomHlc(kLen, 42), withNaNs(randomHlc(kLen, 42), 9)})
  {
    std::vector<double> open(bars.high.size());
    for (size_t i = 0; i < open.size(); ++i)
    {
      open[i] = (bars.high[i] + bars.low[i]) * 0.5;  // synthetic
    }
    RogersSatchellVol p(14);
    auto out = p.compute(std::span<const double>(open), std::span<const double>(bars.high),
                         std::span<const double>(bars.low), std::span<const double>(bars.close));
    RogersSatchellVol stream(14);
    for (size_t i = 0; i < bars.high.size(); ++i)
    {
      stream.update(open[i], bars.high[i], bars.low[i], bars.close[i]);
      expectSame(stream.value(), out[i], "RogersSatchellVol", i);
    }
    EXPECT_EQ(stream.count(), bars.high.size());
  }
}

// ---- Pair ----------------------------------------------------------------

TEST(StreamingParity, Correlation)
{
  for (bool nans : {false, true})
  {
    auto x = randomSeries(kLen, 42);
    auto y = randomSeries(kLen, 7);
    if (nans)
    {
      x = withNaNs(x, 9);
      y = withNaNs(y, 10);
    }
    Correlation c(10);
    auto out = c.compute(std::span<const double>(x), std::span<const double>(y));
    Correlation stream(10);
    for (size_t i = 0; i < x.size(); ++i)
    {
      stream.update(x[i], y[i]);
      expectSame(stream.value(), out[i], "Correlation", i);
    }
    EXPECT_EQ(stream.count(), x.size());
  }
}

// ---- Multi-output single input -------------------------------------------

TEST(StreamingParity, MACD)
{
  for (const auto& input : {randomSeries(kLen, 42), withNaNs(randomSeries(kLen, 42), 9)})
  {
    MACD m(5, 10, 4);
    auto r = m.compute(std::span<const double>(input));
    MACD stream(5, 10, 4);
    for (size_t i = 0; i < input.size(); ++i)
    {
      stream.update(input[i]);
      expectSame(stream.value(), r.line[i], "MACD.line", i);
      expectSame(stream.signalValue(), r.signal[i], "MACD.signal", i);
      expectSame(stream.histogramValue(), r.histogram[i], "MACD.hist", i);
      EXPECT_EQ(stream.ready(), std::isfinite(r.line[i]));
    }
    EXPECT_EQ(stream.count(), input.size());
  }
}

TEST(StreamingParity, Bollinger)
{
  for (const auto& input : {randomSeries(kLen, 42), withNaNs(randomSeries(kLen, 42), 9)})
  {
    Bollinger b(10, 2.0);
    auto r = b.compute(std::span<const double>(input));
    Bollinger stream(10, 2.0);
    for (size_t i = 0; i < input.size(); ++i)
    {
      stream.update(input[i]);
      expectSame(stream.value(), r.middle[i], "Bollinger.value", i);
      expectSame(stream.middleValue(), r.middle[i], "Bollinger.middle", i);
      expectSame(stream.upperValue(), r.upper[i], "Bollinger.upper", i);
      expectSame(stream.lowerValue(), r.lower[i], "Bollinger.lower", i);
      EXPECT_EQ(stream.ready(), std::isfinite(r.middle[i]));
    }
    EXPECT_EQ(stream.count(), input.size());
  }
}

// ---- Registry coverage -----------------------------------------------------

// Every registry row must have a case above; a new row without one fails here.
TEST(StreamingParity, CoversEveryRegistryRow)
{
  const std::vector<std::string> covered = {
      "EMA", "SMA", "RMA", "RSI", "KAMA",
      "DEMA", "TEMA", "Slope", "Skewness", "Kurtosis",
      "RollingZScore", "ShannonEntropy", "AutoCorrelation",
      "ATR", "CCI", "Stochastic", "ParkinsonVol", "RogersSatchellVol",
      "Correlation", "MACD", "Bollinger"};
  std::vector<std::string> registry;
#define LRVX_INDICATOR(Cls, name, Kind, Args) registry.emplace_back(#Cls);
#include "lrvx/indicator/registry.def"
#undef LRVX_INDICATOR
  for (const auto& r : registry)
  {
    EXPECT_NE(std::find(covered.begin(), covered.end(), r), covered.end())
        << r << " is in registry.def but has no parity case";
  }
  EXPECT_EQ(registry.size(), covered.size());
}
