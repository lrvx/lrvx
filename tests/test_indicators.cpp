#include "lrvx/indicator/adx.h"
#include "lrvx/indicator/atr.h"
#include "lrvx/indicator/bar_fields.h"
#include "lrvx/indicator/bollinger.h"
#include "lrvx/indicator/cci.h"
#include "lrvx/indicator/chop.h"
#include "lrvx/indicator/correlation.h"
#include "lrvx/indicator/cvd.h"
#include "lrvx/indicator/dema.h"
#include "lrvx/indicator/ema.h"
#include "lrvx/indicator/indicator_pipeline.h"
#include "lrvx/indicator/kama.h"
#include "lrvx/indicator/kurtosis.h"
#include "lrvx/indicator/macd.h"
#include "lrvx/indicator/obv.h"
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
#include "lrvx/indicator/vwap.h"

#include "lrvx/error/lrvx_error.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

using namespace lrvx;
using namespace lrvx::indicator;

// The buffer overload and the allocating overload run the same inline
// arithmetic, but each call site is its own inlining context: under GCC's
// default -ffp-contract=fast with an FMA-capable -march, one copy can fuse a
// multiply-add that the other rounds twice, and the two results differ by a
// few ULP (seen on Skewness at -O3 -march=x86-64-v3 without LTO). The
// contract is the same value, not the same bits.
#define EXPECT_SAME_VALUE(a, b) EXPECT_NEAR((a), (b), 1e-12 * std::max(1.0, std::abs(b)))

static std::vector<double> prices()
{
  return {44, 44.34, 44.09, 43.61, 44.33, 44.83, 45.10, 45.42, 45.84,
          46.08, 45.89, 46.03, 45.61, 46.28, 46.28, 46.00, 46.03, 46.41,
          46.22, 45.64};
}

TEST(SMA, BasicWindow)
{
  SMA sma(5);
  auto result = sma.compute(prices());
  ASSERT_EQ(result.size(), 20u);
  for (int i = 0; i < 4; ++i)
  {
    EXPECT_TRUE(std::isnan(result[i]));
  }
  EXPECT_NEAR(result[4], (44 + 44.34 + 44.09 + 43.61 + 44.33) / 5.0, 1e-10);
}

TEST(SMA, RollingSum)
{
  SMA sma(3);
  std::vector<double> input = {1, 2, 3, 4, 5};
  auto result = sma.compute(input);
  EXPECT_NEAR(result[2], 2.0, 1e-10);
  EXPECT_NEAR(result[3], 3.0, 1e-10);
  EXPECT_NEAR(result[4], 4.0, 1e-10);
}

TEST(EMA, SmaSeed)
{
  EMA ema(10);
  auto result = ema.compute(prices());
  ASSERT_EQ(result.size(), 20u);
  for (int i = 0; i < 9; ++i)
  {
    EXPECT_TRUE(std::isnan(result[i]));
  }
  // Seed = SMA of first 10 values
  double sum = 0;
  auto p = prices();
  for (int i = 0; i < 10; ++i)
  {
    sum += p[i];
  }
  EXPECT_NEAR(result[9], sum / 10.0, 1e-10);
}

TEST(EMA, ExponentialDecay)
{
  EMA ema(10);
  auto p = prices();
  auto result = ema.compute(p);
  double alpha = 2.0 / 11.0;
  double expected = result[9];
  for (size_t i = 10; i < p.size(); ++i)
  {
    expected = alpha * p[i] + (1.0 - alpha) * expected;
    EXPECT_NEAR(result[i], expected, 1e-10);
  }
}

TEST(EMA, Composition)
{
  // EMA of EMA -- verify compute(span<double>) works
  EMA ema1(5);
  EMA ema2(3);
  auto p = prices();
  auto first = ema1.compute(p);
  auto second = ema2.compute(std::span<const double>(first));
  // second should have NaN where first has NaN + 2 more warmup bars
  EXPECT_TRUE(std::isnan(second[0]));
  bool found = false;
  for (size_t i = 0; i < second.size(); ++i)
  {
    if (!std::isnan(second[i]))
    {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found);
}

TEST(EMA, WriteToBuffer)
{
  EMA ema(5);
  auto p = prices();
  std::vector<double> buf(p.size());
  ema.compute(p, buf);
  auto alloced = ema.compute(p);
  for (size_t i = 0; i < p.size(); ++i)
  {
    if (std::isnan(alloced[i]))
    {
      EXPECT_TRUE(std::isnan(buf[i]));
    }
    else
    {
      EXPECT_SAME_VALUE(buf[i], alloced[i]);
    }
  }
}

TEST(ATR, WilderSmoothing)
{
  // Construct bars with known H/L/C
  std::vector<double> h = {48.70, 48.72, 48.90, 48.87, 48.82, 49.05, 49.20, 49.35, 49.92, 50.19,
                           50.12, 49.66, 49.88, 50.19, 50.36, 50.57, 50.65, 50.43, 49.63, 50.33};
  std::vector<double> l = {47.79, 48.14, 48.39, 48.37, 48.24, 48.64, 48.94, 48.86, 49.50, 49.87,
                           49.20, 48.90, 49.43, 49.73, 49.26, 50.09, 50.30, 49.21, 48.98, 49.61};
  std::vector<double> c = {48.16, 48.61, 48.75, 48.63, 48.74, 49.03, 49.07, 49.32, 49.91, 50.13,
                           49.53, 49.50, 49.75, 50.03, 49.99, 50.23, 50.33, 49.24, 49.00, 50.29};

  ATR atr(14);
  auto result = atr.compute(h, l, c);
  ASSERT_EQ(result.size(), 20u);

  // TA-Lib: TR[0] = NaN (no prev close), ATR first valid at index period (14)
  for (int i = 0; i < 14; ++i)
  {
    EXPECT_TRUE(std::isnan(result[i]));
  }
  EXPECT_FALSE(std::isnan(result[14]));

  for (size_t i = 14; i < result.size(); ++i)
  {
    EXPECT_FALSE(std::isnan(result[i]));
    EXPECT_GT(result[i], 0.0);
  }
}

TEST(ATR, BarOverload)
{
  // Create bars and verify Bar overload matches raw span overload
  std::vector<Bar> bars(10);
  for (size_t i = 0; i < 10; ++i)
  {
    bars[i].open = Price::fromDouble(100.0 + i);
    bars[i].high = Price::fromDouble(101.0 + i);
    bars[i].low = Price::fromDouble(99.0 + i);
    bars[i].close = Price::fromDouble(100.5 + i);
  }

  ATR atr(5);
  auto fromBars = atr.compute(std::span<const Bar>(bars));

  auto h = indicator::high(std::span<const Bar>(bars));
  auto l = indicator::low(std::span<const Bar>(bars));
  auto c = indicator::close(std::span<const Bar>(bars));
  auto fromSpans = atr.compute(h, l, c);

  ASSERT_EQ(fromBars.size(), fromSpans.size());
  for (size_t i = 0; i < fromBars.size(); ++i)
  {
    if (std::isnan(fromBars[i]))
    {
      EXPECT_TRUE(std::isnan(fromSpans[i]));
    }
    else
    {
      EXPECT_DOUBLE_EQ(fromBars[i], fromSpans[i]);
    }
  }
}

TEST(RSI, BoundaryValues)
{
  // All gains -> RSI should be 100
  std::vector<double> allUp = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
  RSI rsi(14);
  auto result = rsi.compute(allUp);
  for (size_t i = 15; i < result.size(); ++i)
  {
    EXPECT_NEAR(result[i], 100.0, 1e-10);
  }

  // All losses -> RSI should be 0
  std::vector<double> allDown = {16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1};
  auto result2 = rsi.compute(allDown);
  for (size_t i = 15; i < result2.size(); ++i)
  {
    EXPECT_NEAR(result2[i], 0.0, 1e-10);
  }
}

// A fully flat window (zero average gain AND zero average loss -- no movement
// at all in the seed) used to report 100, the same value as a pure uptrend.
// Read as "overbought", that is backwards: nothing happened. The decided
// convention is the neutral midpoint 50, matching Stochastic's flat-range
// convention below. A pure uptrend (avgLoss == 0, avgGain > 0, covered by
// BoundaryValues above) is a different case and still correctly reports 100.
TEST(RSI, FlatSeedWindowIsNeutralNotOverbought)
{
  RSI rsi(14);
  std::vector<double> flat(20, 100.0);
  auto result = rsi.compute(flat);
  for (size_t i = 14; i < result.size(); ++i)
  {
    EXPECT_NEAR(result[i], 50.0, 1e-10) << "index " << i;
  }
}

// A decline followed by a flat plateau must NOT hit the same branch: avgLoss
// decays geometrically toward zero but never becomes exactly zero, so this
// stays correctly at 0 (maximally oversold), unlike a flat *seed* window.
TEST(RSI, PlateauAfterDeclineStaysZeroNotNeutral)
{
  RSI rsi(14);
  std::vector<double> series;
  for (int i = 20; i >= 1; --i)
  {
    series.push_back(static_cast<double>(i));  // decline
  }
  series.insert(series.end(), 40, 1.0);  // flat plateau at the bottom
  auto result = rsi.compute(series);
  for (size_t i = series.size() - 10; i < series.size(); ++i)
  {
    EXPECT_NEAR(result[i], 0.0, 1e-9) << "index " << i;
  }
}

TEST(RSI, MiddleRange)
{
  RSI rsi(14);
  auto result = rsi.compute(prices());
  for (size_t i = 0; i < 14; ++i)
  {
    EXPECT_TRUE(std::isnan(result[i]));
  }
  EXPECT_FALSE(std::isnan(result[14]));
  for (size_t i = 14; i < result.size(); ++i)
  {
    EXPECT_GE(result[i], 0.0);
    EXPECT_LE(result[i], 100.0);
  }
}

TEST(MACD, ThreeOutputs)
{
  MACD macd(12, 26, 9);
  auto result = macd.compute(prices());
  ASSERT_EQ(result.line.size(), 20u);
  ASSERT_EQ(result.signal.size(), 20u);
  ASSERT_EQ(result.histogram.size(), 20u);
}

TEST(MACD, Composition)
{
  // MACD line values should be finite where signal is valid
  MACD macd(3, 5, 3);
  auto p = prices();
  auto result = macd.compute(p);

  bool hasValid = false;
  for (size_t i = 0; i < p.size(); ++i)
  {
    if (!std::isnan(result.line[i]))
    {
      hasValid = true;
      EXPECT_FALSE(std::isnan(result.signal[i]));
      EXPECT_FALSE(std::isnan(result.histogram[i]));
    }
  }
  EXPECT_TRUE(hasValid);
}

TEST(MACD, HistogramIsLineMinusSignal)
{
  MACD macd(3, 5, 3);
  auto result = macd.compute(prices());
  for (size_t i = 0; i < result.histogram.size(); ++i)
  {
    if (!std::isnan(result.histogram[i]))
    {
      EXPECT_NEAR(result.histogram[i], result.line[i] - result.signal[i], 1e-10);
    }
  }
}

TEST(MACD, WriteToBuffer)
{
  MACD macd(3, 5, 3);
  auto p = prices();
  std::vector<double> line(p.size()), signal(p.size()), hist(p.size());
  macd.compute(p, line, signal, hist);
  auto result = macd.compute(p);
  for (size_t i = 0; i < p.size(); ++i)
  {
    if (std::isnan(result.line[i]))
    {
      EXPECT_TRUE(std::isnan(line[i]));
    }
    else
    {
      EXPECT_DOUBLE_EQ(line[i], result.line[i]);
    }
  }
}

// Edge cases

TEST(SMA, EmptyInput)
{
  SMA sma(5);
  auto result = sma.compute(std::span<const double>{});
  EXPECT_TRUE(result.empty());
}

TEST(EMA, EmptyInput)
{
  EMA ema(5);
  auto result = ema.compute(std::span<const double>{});
  EXPECT_TRUE(result.empty());
}

TEST(ATR, EmptyInput)
{
  ATR atr(5);
  auto result = atr.compute(
      std::span<const double>{}, std::span<const double>{}, std::span<const double>{});
  EXPECT_TRUE(result.empty());
}

TEST(RSI, EmptyInput)
{
  RSI rsi(14);
  auto result = rsi.compute(std::span<const double>{});
  EXPECT_TRUE(result.empty());
}

TEST(MACD, EmptyInput)
{
  MACD macd(3, 5, 3);
  auto result = macd.compute(std::span<const double>{});
  EXPECT_TRUE(result.line.empty());
  EXPECT_TRUE(result.signal.empty());
  EXPECT_TRUE(result.histogram.empty());
}

TEST(SMA, InputShorterThanPeriod)
{
  SMA sma(10);
  std::vector<double> input = {1, 2, 3};
  auto result = sma.compute(input);
  ASSERT_EQ(result.size(), 3u);
  for (auto v : result)
  {
    EXPECT_TRUE(std::isnan(v));
  }
}

TEST(EMA, NaNInputSkipped)
{
  EMA ema(3);
  std::vector<double> input = {std::nan(""), std::nan(""), 1, 2, 3, 4, 5};
  auto result = ema.compute(input);
  // First 2 NaN in input, then EMA seeds at index 4 (3 valid values: 1,2,3)
  EXPECT_TRUE(std::isnan(result[0]));
  EXPECT_TRUE(std::isnan(result[1]));
  EXPECT_TRUE(std::isnan(result[2]));
  EXPECT_TRUE(std::isnan(result[3]));
  EXPECT_FALSE(std::isnan(result[4]));
  EXPECT_NEAR(result[4], 2.0, 1e-10);  // SMA(1,2,3) = 2.0
}

TEST(RSI, NaNInputHandled)
{
  RSI rsi(3);
  std::vector<double> input = {std::nan(""), 1, 2, 3, 4, 5, 4, 3, 2, 1};
  auto result = rsi.compute(input);
  // Should not crash, some values should be valid
  bool hasValid = false;
  for (auto v : result)
  {
    if (!std::isnan(v))
    {
      hasValid = true;
      EXPECT_GE(v, 0.0);
      EXPECT_LE(v, 100.0);
    }
  }
  EXPECT_TRUE(hasValid);
}

// The rolling-sum family (SMA/RMA/Bollinger/VWAP/CCI) used to have no NaN
// handling at all: one bad candle poisoned _period outputs and, worse, the
// underflowed `_period - 1` index on a zero period was a heap-buffer-overflow
// waiting to happen. These five tests are the direct analogs of
// EMA.NaNInputSkipped above, same input, same shape of assertion -- proving
// the rolling-sum family now has the same "poison the window, recover once
// the NaN slides out" contract EMA and RSI already had.

TEST(SMA, NaNInputSkipped)
{
  SMA sma(3);
  std::vector<double> input = {std::nan(""), std::nan(""), 1, 2, 3, 4, 5};
  auto result = sma.compute(input);
  // First 2 NaN in input poison every 3-wide window that overlaps them
  // (indices 0..3); the window [2,3,4] is the first fully clean one.
  EXPECT_TRUE(std::isnan(result[0]));
  EXPECT_TRUE(std::isnan(result[1]));
  EXPECT_TRUE(std::isnan(result[2]));
  EXPECT_TRUE(std::isnan(result[3]));
  EXPECT_FALSE(std::isnan(result[4]));
  EXPECT_NEAR(result[4], 2.0, 1e-10);  // mean(1,2,3)
  EXPECT_NEAR(result[5], 3.0, 1e-10);  // mean(2,3,4)
  EXPECT_NEAR(result[6], 4.0, 1e-10);  // mean(3,4,5)
}

TEST(SMA, NaNInMiddleRecovers)
{
  // A single bad candle mid-series must poison only the windows that overlap
  // it, then recover -- not poison every output for the rest of the series.
  SMA sma(3);
  std::vector<double> input = {1, 2, 3, 4, std::nan(""), 6, 7, 8, 9, 10};
  auto result = sma.compute(input);
  EXPECT_FALSE(std::isnan(result[3]));  // window [1,2,3] is clean
  EXPECT_TRUE(std::isnan(result[4]));   // window [2,3,nan]
  EXPECT_TRUE(std::isnan(result[5]));   // window [3,nan,6]
  EXPECT_TRUE(std::isnan(result[6]));   // window [nan,6,7]
  EXPECT_FALSE(std::isnan(result[7]));  // window [6,7,8] is clean again
  EXPECT_NEAR(result[7], 7.0, 1e-10);
  EXPECT_NEAR(result[9], 9.0, 1e-10);  // window [8,9,10]
}

TEST(SMA, ZeroPeriodIsSafeNotOutOfBounds)
{
  // period=0 has no well-defined window. Before the fix, `_period - 1`
  // underflowed to SIZE_MAX and wrote out of bounds (confirmed by ASan); it
  // must now behave like "insufficient data" instead: never NaN, never a
  // crash, no assumption about output size beyond what compute() promises.
  SMA sma(0);
  std::vector<double> input = {1, 2, 3, 4, 5};
  auto result = sma.compute(input);
  ASSERT_EQ(result.size(), input.size());
  for (double v : result)
  {
    EXPECT_TRUE(std::isnan(v));
  }
}

TEST(RMA, NaNInputSkipped)
{
  RMA rma(3);
  std::vector<double> input = {std::nan(""), std::nan(""), 1, 2, 3, 4, 5};
  auto result = rma.compute(input);
  EXPECT_TRUE(std::isnan(result[0]));
  EXPECT_TRUE(std::isnan(result[1]));
  EXPECT_TRUE(std::isnan(result[2]));
  EXPECT_TRUE(std::isnan(result[3]));
  EXPECT_FALSE(std::isnan(result[4]));
  EXPECT_NEAR(result[4], 2.0, 1e-10);  // seeded as SMA(1,2,3)
  EXPECT_FALSE(std::isnan(result[5]));
  EXPECT_FALSE(std::isnan(result[6]));
}

TEST(RMA, ZeroPeriodIsSafeNotOutOfBounds)
{
  RMA rma(0);
  std::vector<double> input = {1, 2, 3, 4, 5};
  auto result = rma.compute(input);
  ASSERT_EQ(result.size(), input.size());
  for (double v : result)
  {
    EXPECT_TRUE(std::isnan(v));
  }
}

TEST(Bollinger, NaNInputSkipped)
{
  Bollinger bb(3, 2.0);
  std::vector<double> input = {std::nan(""), std::nan(""), 1, 2, 3, 4, 5};
  auto result = bb.compute(input);
  EXPECT_TRUE(std::isnan(result.middle[3]));
  EXPECT_TRUE(std::isnan(result.upper[3]));
  EXPECT_TRUE(std::isnan(result.lower[3]));
  ASSERT_FALSE(std::isnan(result.middle[4]));
  ASSERT_FALSE(std::isnan(result.upper[4]));
  ASSERT_FALSE(std::isnan(result.lower[4]));
  EXPECT_NEAR(result.middle[4], 2.0, 1e-10);
  EXPECT_GT(result.upper[4], result.middle[4]);
  EXPECT_LT(result.lower[4], result.middle[4]);
  EXPECT_FALSE(std::isnan(result.middle[6]));
}

TEST(Bollinger, ZeroPeriodIsSafeNotOutOfBounds)
{
  Bollinger bb(0, 2.0);
  std::vector<double> input = {1, 2, 3, 4, 5};
  auto result = bb.compute(input);
  ASSERT_EQ(result.middle.size(), input.size());
  for (double v : result.middle)
  {
    EXPECT_TRUE(std::isnan(v));
  }
}

TEST(VWAP, NaNInputSkipped)
{
  VWAP vwap(3);
  std::vector<double> close = {std::nan(""), std::nan(""), 10, 20, 30, 40, 50};
  std::vector<double> volume = {1, 1, 1, 1, 1, 1, 1};
  auto result = vwap.compute(close, volume);
  EXPECT_TRUE(std::isnan(result[2]));
  EXPECT_TRUE(std::isnan(result[3]));
  ASSERT_FALSE(std::isnan(result[4]));
  EXPECT_NEAR(result[4], 20.0, 1e-10);  // vwap(10,20,30) equal volumes
  EXPECT_NEAR(result[5], 30.0, 1e-10);
  EXPECT_NEAR(result[6], 40.0, 1e-10);
}

TEST(VWAP, ZeroWindowIsSafeNotOutOfBounds)
{
  VWAP vwap(0);
  std::vector<double> close = {1, 2, 3, 4, 5};
  std::vector<double> volume = {1, 1, 1, 1, 1};
  auto result = vwap.compute(close, volume);
  ASSERT_EQ(result.size(), close.size());
  for (double v : result)
  {
    EXPECT_TRUE(std::isnan(v));
  }
}

TEST(CCI, NaNInputSkipped)
{
  // CCI has always had its own NaN check on the SMA it builds internally
  // (`if (std::isnan(tpSma[i])) continue;`) -- but that check was useless
  // while SMA itself never recovered from a NaN. Once SMA recovers, CCI does
  // too, with no CCI-specific change needed.
  CCI cci(3);
  std::vector<double> hlc = {std::nan(""), std::nan(""), 10, 20, 30, 40, 50};
  auto result = cci.compute(hlc, hlc, hlc);
  EXPECT_TRUE(std::isnan(result[3]));
  ASSERT_FALSE(std::isnan(result[4]));
  EXPECT_FALSE(std::isnan(result[6]));
}

TEST(CCI, ZeroPeriodIsSafeNotOutOfBounds)
{
  CCI cci(0);
  std::vector<double> hlc = {1, 2, 3, 4, 5};
  auto result = cci.compute(hlc, hlc, hlc);
  ASSERT_EQ(result.size(), hlc.size());
  for (double v : result)
  {
    EXPECT_TRUE(std::isnan(v));
  }
}

// The documented composition pattern (indicator_pipeline.h's own doc comment):
// stack one indicator on top of another node's output, where the upstream
// node's warmup prefix is a normal, expected run of NaN. Before the fix this
// lost 100% of the downstream output (60/60 bars in the audit's repro) because
// the rolling-sum family never recovered from the first NaN it ever saw.
TEST(SMA, ComposedOverWarmupPrefixRecovers)
{
  EMA ema(10);
  auto emaOut = ema.compute(prices());  // 20 bars, 9-bar NaN warmup prefix
  SMA sma(5);
  auto smaOut = sma.compute(emaOut);
  ASSERT_EQ(smaOut.size(), 20u);
  // sma's 5-wide window first fully clears the EMA's NaN prefix (length 9,
  // valid from index 9) at index 9 + 5 - 1 = 13.
  for (size_t i = 0; i < 13; ++i)
  {
    EXPECT_TRUE(std::isnan(smaOut[i])) << "index " << i;
  }
  for (size_t i = 13; i < 20; ++i)
  {
    EXPECT_FALSE(std::isnan(smaOut[i])) << "index " << i;
  }
}

TEST(BarFields, ExtractClose)
{
  std::vector<Bar> bars(5);
  for (size_t i = 0; i < 5; ++i)
  {
    bars[i].close = Price::fromDouble(100.0 + i);
  }
  auto closes = indicator::close(std::span<const Bar>(bars));
  ASSERT_EQ(closes.size(), 5u);
  for (size_t i = 0; i < 5; ++i)
  {
    EXPECT_DOUBLE_EQ(closes[i], 100.0 + i);
  }
}

TEST(BarFields, InPlaceExtract)
{
  std::vector<Bar> bars(3);
  bars[0].high = Price::fromDouble(10.0);
  bars[1].high = Price::fromDouble(20.0);
  bars[2].high = Price::fromDouble(30.0);
  std::vector<double> buf(3);
  indicator::high(std::span<const Bar>(bars), buf);
  EXPECT_DOUBLE_EQ(buf[0], 10.0);
  EXPECT_DOUBLE_EQ(buf[1], 20.0);
  EXPECT_DOUBLE_EQ(buf[2], 30.0);
}

// Slope

TEST(Slope, Basic)
{
  Slope slope(1);
  std::vector<double> input = {10, 12, 11, 15, 13};
  auto result = slope.compute(input);
  EXPECT_TRUE(std::isnan(result[0]));
  EXPECT_NEAR(result[1], 2.0, 1e-10);
  EXPECT_NEAR(result[2], -1.0, 1e-10);
  EXPECT_NEAR(result[3], 4.0, 1e-10);
  EXPECT_NEAR(result[4], -2.0, 1e-10);
}

TEST(Slope, LongerPeriod)
{
  Slope slope(3);
  std::vector<double> input = {10, 12, 11, 15, 13};
  auto result = slope.compute(input);
  EXPECT_TRUE(std::isnan(result[0]));
  EXPECT_TRUE(std::isnan(result[1]));
  EXPECT_TRUE(std::isnan(result[2]));
  EXPECT_NEAR(result[3], (15 - 10) / 3.0, 1e-10);
  EXPECT_NEAR(result[4], (13 - 12) / 3.0, 1e-10);
}

// KAMA

TEST(KAMA, SmaSeed)
{
  KAMA kama(10);
  auto p = prices();
  auto result = kama.compute(p);
  ASSERT_EQ(result.size(), p.size());
  // TA-Lib: KAMA first valid at period (10), seeded from close[period-1]
  for (int i = 0; i < 10; ++i)
  {
    EXPECT_TRUE(std::isnan(result[i]));
  }
  EXPECT_FALSE(std::isnan(result[10]));
  for (size_t i = 10; i < p.size(); ++i)
  {
    EXPECT_FALSE(std::isnan(result[i]));
  }
}

// ADX

TEST(ADX, ThreeOutputs)
{
  std::vector<double> h = {48.70, 48.72, 48.90, 48.87, 48.82, 49.05, 49.20, 49.35, 49.92, 50.19,
                           50.12, 49.66, 49.88, 50.19, 50.36, 50.57, 50.65, 50.43, 49.63, 50.33};
  std::vector<double> l = {47.79, 48.14, 48.39, 48.37, 48.24, 48.64, 48.94, 48.86, 49.50, 49.87,
                           49.20, 48.90, 49.43, 49.73, 49.26, 50.09, 50.30, 49.21, 48.98, 49.61};
  std::vector<double> c = {48.16, 48.61, 48.75, 48.63, 48.74, 49.03, 49.07, 49.32, 49.91, 50.13,
                           49.53, 49.50, 49.75, 50.03, 49.99, 50.23, 50.33, 49.24, 49.00, 50.29};

  // ADX(14) needs 2*14+1 = 29 bars minimum. We have 20 -- not enough for ADX.
  // Use a larger dataset.
  std::vector<double> h2, l2, c2;
  for (int rep = 0; rep < 3; ++rep)
  {
    h2.insert(h2.end(), h.begin(), h.end());
    l2.insert(l2.end(), l.begin(), l.end());
    c2.insert(c2.end(), c.begin(), c.end());
  }

  ADX adx(14);
  auto result = adx.compute(h2, l2, c2);
  ASSERT_EQ(result.adx.size(), 60u);
  ASSERT_EQ(result.plus_di.size(), 60u);

  // First valid ADX at index 2*14-1 = 27
  for (size_t i = 0; i < 27; ++i)
  {
    EXPECT_TRUE(std::isnan(result.adx[i]));
  }
  EXPECT_FALSE(std::isnan(result.adx[27]));
  EXPECT_GE(result.adx[27], 0.0);
}

// adx.h claims exact TA-Lib parity. TA-Lib's lookback is `period` bars for
// PLUS_DI/MINUS_DI and `2*period - 1` for ADX, so on n == 2*period bars it
// already has one full ADX value (at index 2*period-1) and `period` DI values
// (from index `period`). The old guard (`n < 2*period + 1`) demanded one bar
// more than that and returned nothing at all on exactly n == 2*period.
TEST(ADX, MatchesTaLibLookbackOnMinimalBarCount)
{
  std::vector<double> h = {48.70, 48.72, 48.90, 48.87, 48.82, 49.05, 49.20, 49.35, 49.92, 50.19};
  std::vector<double> l = {47.79, 48.14, 48.39, 48.37, 48.24, 48.64, 48.94, 48.86, 49.50, 49.87};
  std::vector<double> c = {48.16, 48.61, 48.75, 48.63, 48.74, 49.03, 49.07, 49.32, 49.91, 50.13};
  const size_t period = 5;
  ASSERT_EQ(h.size(), 2 * period);  // exactly the minimal TA-Lib bar count

  ADX adx(period);
  auto result = adx.compute(h, l, c);
  ASSERT_EQ(result.adx.size(), 2 * period);

  for (size_t i = 0; i < period; ++i)
  {
    EXPECT_TRUE(std::isnan(result.plus_di[i])) << "index " << i;
    EXPECT_TRUE(std::isnan(result.minus_di[i])) << "index " << i;
  }
  for (size_t i = period; i < 2 * period; ++i)
  {
    EXPECT_FALSE(std::isnan(result.plus_di[i])) << "index " << i;
    EXPECT_FALSE(std::isnan(result.minus_di[i])) << "index " << i;
  }
  for (size_t i = 0; i + 1 < 2 * period - 1; ++i)
  {
    EXPECT_TRUE(std::isnan(result.adx[i])) << "index " << i;
  }
  EXPECT_FALSE(std::isnan(result.adx[2 * period - 1]));
}

// A perfectly flat instrument has zero true range: TA-Lib writes 0 for
// +DI/-DI/ADX there, not NaN (adx.h's comment claims exact TA-Lib parity, so
// this boundary has to match too, not just the happy path with real movement).
TEST(ADX, FlatTrueRangeWritesZeroNotNaN)
{
  std::vector<double> flat(20, 100.0);
  ADX adx(5);
  auto result = adx.compute(flat, flat, flat);
  for (size_t i = 5; i < 10; ++i)
  {
    EXPECT_NEAR(result.plus_di[i], 0.0, 1e-12) << "index " << i;
    EXPECT_NEAR(result.minus_di[i], 0.0, 1e-12) << "index " << i;
  }
  ASSERT_FALSE(std::isnan(result.adx[9]));
  EXPECT_NEAR(result.adx[9], 0.0, 1e-12);
}

// CHOP

TEST(CHOP, Range)
{
  std::vector<double> h = {48.70, 48.72, 48.90, 48.87, 48.82, 49.05, 49.20, 49.35, 49.92, 50.19,
                           50.12, 49.66, 49.88, 50.19, 50.36, 50.57, 50.65, 50.43, 49.63, 50.33};
  std::vector<double> l = {47.79, 48.14, 48.39, 48.37, 48.24, 48.64, 48.94, 48.86, 49.50, 49.87,
                           49.20, 48.90, 49.43, 49.73, 49.26, 50.09, 50.30, 49.21, 48.98, 49.61};
  std::vector<double> c = {48.16, 48.61, 48.75, 48.63, 48.74, 49.03, 49.07, 49.32, 49.91, 50.13,
                           49.53, 49.50, 49.75, 50.03, 49.99, 50.23, 50.33, 49.24, 49.00, 50.29};

  CHOP chop(14);
  auto result = chop.compute(h, l, c);
  ASSERT_EQ(result.size(), 20u);

  for (size_t i = 0; i < result.size(); ++i)
  {
    if (!std::isnan(result[i]))
    {
      // CHOP should be between 0 and 100
      EXPECT_GE(result[i], 0.0);
      EXPECT_LE(result[i], 100.0);
    }
  }
}

// OBV

TEST(OBV, CumulativeVolume)
{
  std::vector<double> close = {10, 11, 10.5, 12, 11.5};
  std::vector<double> vol = {100, 200, 150, 300, 250};

  OBV obv;
  auto result = obv.compute(close, vol);
  ASSERT_EQ(result.size(), 5u);
  // TA-Lib: OBV[0] = volume[0]
  EXPECT_DOUBLE_EQ(result[0], 100.0);        // volume[0]
  EXPECT_DOUBLE_EQ(result[1], 100.0 + 200);  // up: +200
  EXPECT_DOUBLE_EQ(result[2], 300.0 - 150);  // down: -150
  EXPECT_DOUBLE_EQ(result[3], 150.0 + 300);  // up: +300
  EXPECT_DOUBLE_EQ(result[4], 450.0 - 250);  // down: -250
}

TEST(OBV, EmptyInput)
{
  OBV obv;
  auto result = obv.compute(std::span<const double>{}, std::span<const double>{});
  EXPECT_TRUE(result.empty());
}

// Bollinger

TEST(Bollinger, ThreeBands)
{
  Bollinger bb(5, 2.0);
  auto p = prices();
  auto result = bb.compute(p);
  ASSERT_EQ(result.upper.size(), p.size());
  ASSERT_EQ(result.middle.size(), p.size());
  ASSERT_EQ(result.lower.size(), p.size());

  for (size_t i = 4; i < p.size(); ++i)
  {
    EXPECT_FALSE(std::isnan(result.middle[i]));
    EXPECT_FALSE(std::isnan(result.upper[i]));
    EXPECT_FALSE(std::isnan(result.lower[i]));
    EXPECT_GT(result.upper[i], result.middle[i]);
    EXPECT_LT(result.lower[i], result.middle[i]);
  }
}

TEST(Bollinger, MiddleIsSma)
{
  Bollinger bb(5, 2.0);
  SMA sma(5);
  auto p = prices();
  auto bbResult = bb.compute(p);
  auto smaResult = sma.compute(p);

  for (size_t i = 0; i < p.size(); ++i)
  {
    if (!std::isnan(bbResult.middle[i]))
    {
      EXPECT_NEAR(bbResult.middle[i], smaResult[i], 1e-10);
    }
  }
}

TEST(RMA, WilderSmoothing)
{
  RMA rma(5);
  auto p = prices();
  auto result = rma.compute(p);
  ASSERT_EQ(result.size(), p.size());
  for (int i = 0; i < 4; ++i)
  {
    EXPECT_TRUE(std::isnan(result[i]));
  }
  EXPECT_FALSE(std::isnan(result[4]));
  double sum = 0;
  for (int i = 0; i < 5; ++i)
  {
    sum += p[i];
  }
  EXPECT_NEAR(result[4], sum / 5.0, 1e-10);
}

TEST(DEMA, HasOutput)
{
  DEMA dema(5);
  auto result = dema.compute(prices());
  ASSERT_EQ(result.size(), 20u);
  bool found = false;
  for (auto v : result)
  {
    if (!std::isnan(v))
    {
      found = true;
    }
  }
  EXPECT_TRUE(found);
}

TEST(TEMA, HasOutput)
{
  TEMA tema(5);
  auto result = tema.compute(prices());
  ASSERT_EQ(result.size(), 20u);
  bool found = false;
  for (auto v : result)
  {
    if (!std::isnan(v))
    {
      found = true;
    }
  }
  EXPECT_TRUE(found);
}

TEST(Stochastic, KAndD)
{
  std::vector<double> h = {48.70, 48.72, 48.90, 48.87, 48.82, 49.05, 49.20, 49.35, 49.92, 50.19,
                           50.12, 49.66, 49.88, 50.19, 50.36, 50.57, 50.65, 50.43, 49.63, 50.33};
  std::vector<double> l = {47.79, 48.14, 48.39, 48.37, 48.24, 48.64, 48.94, 48.86, 49.50, 49.87,
                           49.20, 48.90, 49.43, 49.73, 49.26, 50.09, 50.30, 49.21, 48.98, 49.61};
  std::vector<double> c = {48.16, 48.61, 48.75, 48.63, 48.74, 49.03, 49.07, 49.32, 49.91, 50.13,
                           49.53, 49.50, 49.75, 50.03, 49.99, 50.23, 50.33, 49.24, 49.00, 50.29};
  Stochastic stoch(5, 3);
  auto result = stoch.compute(h, l, c);
  ASSERT_EQ(result.k.size(), 20u);
  for (size_t i = 6; i < 20; ++i)
  {
    EXPECT_FALSE(std::isnan(result.k[i]));
    EXPECT_GE(result.k[i], 0.0);
    EXPECT_LE(result.k[i], 100.0);
  }
}

TEST(CCI, Range)
{
  std::vector<double> h = {48.70, 48.72, 48.90, 48.87, 48.82, 49.05, 49.20, 49.35, 49.92, 50.19,
                           50.12, 49.66, 49.88, 50.19, 50.36, 50.57, 50.65, 50.43, 49.63, 50.33};
  std::vector<double> l = {47.79, 48.14, 48.39, 48.37, 48.24, 48.64, 48.94, 48.86, 49.50, 49.87,
                           49.20, 48.90, 49.43, 49.73, 49.26, 50.09, 50.30, 49.21, 48.98, 49.61};
  std::vector<double> c = {48.16, 48.61, 48.75, 48.63, 48.74, 49.03, 49.07, 49.32, 49.91, 50.13,
                           49.53, 49.50, 49.75, 50.03, 49.99, 50.23, 50.33, 49.24, 49.00, 50.29};
  CCI cci(14);
  auto result = cci.compute(h, l, c);
  ASSERT_EQ(result.size(), 20u);
  for (size_t i = 13; i < 20; ++i)
  {
    EXPECT_FALSE(std::isnan(result[i]));
  }
}

TEST(VWAP, Rolling)
{
  std::vector<double> close = {100, 101, 102, 103, 104};
  std::vector<double> volume = {1000, 2000, 1500, 3000, 2500};
  VWAP vwap(3);
  auto result = vwap.compute(close, volume);
  ASSERT_EQ(result.size(), 5u);
  EXPECT_TRUE(std::isnan(result[0]));
  EXPECT_FALSE(std::isnan(result[2]));
  double expected = (100 * 1000.0 + 101 * 2000.0 + 102 * 1500.0) / (1000.0 + 2000.0 + 1500.0);
  EXPECT_NEAR(result[2], expected, 1e-10);
}

TEST(CVD, Cumulative)
{
  std::vector<double> o = {100, 100, 100, 100, 100};
  std::vector<double> h = {102, 102, 102, 102, 102};
  std::vector<double> l = {98, 98, 98, 98, 98};
  std::vector<double> c = {101, 99, 101, 99, 101};
  std::vector<double> v = {100, 100, 100, 100, 100};
  CVD cvd;
  auto result = cvd.compute(o, h, l, c, v);
  ASSERT_EQ(result.size(), 5u);
  EXPECT_GT(result[0], 0);
  EXPECT_LT(result[1], result[0]);
}

TEST(IndicatorGraph, DependencyResolution)
{
  IndicatorGraph g;
  std::vector<Bar> bars(20);
  for (size_t i = 0; i < 20; ++i)
  {
    bars[i].close = Price::fromDouble(100.0 + i);
    bars[i].high = Price::fromDouble(101.0 + i);
    bars[i].low = Price::fromDouble(99.0 + i);
  }
  g.setBars(0, bars);
  g.addNode("ema5", {}, [](IndicatorGraph& g, SymbolId sym)
            { return EMA(5).compute(g.close(sym)); });
  g.addNode("slope1", {"ema5"}, [](IndicatorGraph& g, SymbolId sym)
            { return Slope(1).compute(*g.get(sym, "ema5")); });
  auto& slope = g.require(0, "slope1");
  ASSERT_EQ(slope.size(), 20u);
  auto* ema = g.get(0, "ema5");
  ASSERT_NE(ema, nullptr);
}

TEST(IndicatorGraph, CircularDependencyThrows)
{
  IndicatorGraph g;
  std::vector<Bar> bars(10);
  for (auto& b : bars)
  {
    b.close = Price::fromDouble(100.0);
  }
  g.setBars(0, bars);
  g.addNode("a", {"b"}, [](IndicatorGraph&, SymbolId)
            { return std::vector<double>{}; });
  g.addNode("b", {"a"}, [](IndicatorGraph&, SymbolId)
            { return std::vector<double>{}; });
  EXPECT_THROW(g.require(0, "a"), lrvx::LrvxError);
}

// A node whose compute function throws once used to poison _computing
// permanently: nothing erased the "in progress" marker on the exception path,
// so every later require() for that node reported a phantom circular
// dependency instead of retrying. Neither invalidate() nor a full reset()
// used to clear it either. A scope guard (RAII) around _computing now erases
// the marker on every exit path, so the node recovers as soon as its compute
// function stops throwing.
TEST(IndicatorGraph, RecoversAfterComputeThrows)
{
  IndicatorGraph g;
  std::vector<Bar> bars(1);
  bars[0].close = Price::fromDouble(100.0);
  g.setBars(0, bars);

  bool shouldThrow = true;
  int calls = 0;
  g.addNode("flaky", {},
            [&](IndicatorGraph&, SymbolId) -> std::vector<double>
            {
              ++calls;
              if (shouldThrow)
              {
                throw std::runtime_error("transient");
              }
              return std::vector<double>{42.0};
            });

  EXPECT_THROW(g.require(0, "flaky"), std::runtime_error);
  EXPECT_EQ(calls, 1);

  // The cause is gone, but nothing has invalidated the cache -- require()
  // must still be able to retry rather than reporting a circular dependency
  // left over from the failed attempt.
  shouldThrow = false;
  auto& result = g.require(0, "flaky");
  ASSERT_EQ(result.size(), 1u);
  EXPECT_DOUBLE_EQ(result[0], 42.0);
  EXPECT_EQ(calls, 2);
}

// Same recovery, exercised through current() (which already swallows the
// exception into NaN by contract) and through the explicit invalidation
// paths, so a caller who invalidates instead of relying on the next
// require() also gets a working node back.
TEST(IndicatorGraph, CurrentRecoversAfterComputeThrowsAndInvalidate)
{
  IndicatorGraph g;
  std::vector<Bar> bars(1);
  bars[0].close = Price::fromDouble(100.0);
  g.setBars(0, bars);

  bool shouldThrow = true;
  g.addNode("flaky", {},
            [&](IndicatorGraph&, SymbolId) -> std::vector<double>
            {
              if (shouldThrow)
              {
                throw std::runtime_error("transient");
              }
              return std::vector<double>{7.0};
            });

  EXPECT_TRUE(std::isnan(g.current(0, "flaky")));

  shouldThrow = false;
  g.invalidateAll();
  EXPECT_DOUBLE_EQ(g.current(0, "flaky"), 7.0);
}

TEST(IndicatorGraph, CacheInvalidation)
{
  IndicatorGraph g;
  std::vector<Bar> bars(10);
  for (size_t i = 0; i < 10; ++i)
  {
    bars[i].close = Price::fromDouble(100.0 + i);
  }
  g.setBars(0, bars);
  g.addNode("ema3", {}, [](IndicatorGraph& g, SymbolId sym)
            { return EMA(3).compute(g.close(sym)); });
  auto& r1 = g.require(0, "ema3");
  double first = r1[9];
  bars[9].close = Price::fromDouble(200.0);
  g.setBars(0, bars);
  auto& r2 = g.require(0, "ema3");
  EXPECT_NE(r2[9], first);
}

// Skewness

TEST(Skewness, BasicComputation)
{
  Skewness skew(5);
  std::vector<double> input = {1, 2, 3, 4, 10};
  auto result = skew.compute(input);
  ASSERT_EQ(result.size(), 5u);
  for (int i = 0; i < 4; ++i)
  {
    EXPECT_TRUE(std::isnan(result[i]));
  }
  EXPECT_FALSE(std::isnan(result[4]));
  EXPECT_GT(result[4], 0.0);
}

TEST(Skewness, SymmetricDistribution)
{
  Skewness skew(5);
  std::vector<double> input = {1, 2, 3, 4, 5};
  auto result = skew.compute(input);
  EXPECT_NEAR(result[4], 0.0, 1e-10);
}

TEST(Skewness, ZeroStd)
{
  Skewness skew(3);
  std::vector<double> input = {5, 5, 5};
  auto result = skew.compute(input);
  EXPECT_TRUE(std::isnan(result[2]));
}

TEST(Skewness, EmptyInput)
{
  Skewness skew(3);
  auto result = skew.compute(std::span<const double>{});
  EXPECT_TRUE(result.empty());
}

TEST(Skewness, WriteToBuffer)
{
  Skewness skew(5);
  auto p = prices();
  std::vector<double> buf(p.size());
  skew.compute(p, buf);
  auto alloced = skew.compute(p);
  for (size_t i = 0; i < p.size(); ++i)
  {
    if (std::isnan(alloced[i]))
    {
      EXPECT_TRUE(std::isnan(buf[i]));
    }
    else
    {
      EXPECT_SAME_VALUE(buf[i], alloced[i]);
    }
  }
}

// Kurtosis

TEST(Kurtosis, BasicComputation)
{
  Kurtosis kurt(5);
  std::vector<double> input = {1, 2, 3, 4, 100};
  auto result = kurt.compute(input);
  ASSERT_EQ(result.size(), 5u);
  for (int i = 0; i < 4; ++i)
  {
    EXPECT_TRUE(std::isnan(result[i]));
  }
  EXPECT_FALSE(std::isnan(result[4]));
  EXPECT_GT(result[4], 0.0);
}

TEST(Kurtosis, ZeroStd)
{
  Kurtosis kurt(4);
  std::vector<double> input = {5, 5, 5, 5};
  auto result = kurt.compute(input);
  EXPECT_TRUE(std::isnan(result[3]));
}

TEST(Kurtosis, EmptyInput)
{
  Kurtosis kurt(4);
  auto result = kurt.compute(std::span<const double>{});
  EXPECT_TRUE(result.empty());
}

TEST(Kurtosis, WriteToBuffer)
{
  Kurtosis kurt(5);
  auto p = prices();
  std::vector<double> buf(p.size());
  kurt.compute(p, buf);
  auto alloced = kurt.compute(p);
  for (size_t i = 0; i < p.size(); ++i)
  {
    if (std::isnan(alloced[i]))
    {
      EXPECT_TRUE(std::isnan(buf[i]));
    }
    else
    {
      EXPECT_SAME_VALUE(buf[i], alloced[i]);
    }
  }
}

// RollingZScore

TEST(RollingZScore, BasicComputation)
{
  RollingZScore zscore(5);
  std::vector<double> input = {10, 10, 10, 10, 10};
  auto result = zscore.compute(input);
  for (int i = 0; i < 4; ++i)
  {
    EXPECT_TRUE(std::isnan(result[i]));
  }
  EXPECT_TRUE(std::isnan(result[4]));
}

TEST(RollingZScore, MeanIsZero)
{
  RollingZScore zscore(3);
  std::vector<double> input = {1, 2, 3, 2, 1};
  auto result = zscore.compute(input);
  EXPECT_FALSE(std::isnan(result[2]));
}

TEST(RollingZScore, EmptyInput)
{
  RollingZScore zscore(3);
  auto result = zscore.compute(std::span<const double>{});
  EXPECT_TRUE(result.empty());
}

TEST(RollingZScore, WriteToBuffer)
{
  RollingZScore zscore(5);
  auto p = prices();
  std::vector<double> buf(p.size());
  zscore.compute(p, buf);
  auto alloced = zscore.compute(p);
  for (size_t i = 0; i < p.size(); ++i)
  {
    if (std::isnan(alloced[i]))
    {
      EXPECT_TRUE(std::isnan(buf[i]));
    }
    else
    {
      EXPECT_SAME_VALUE(buf[i], alloced[i]);
    }
  }
}

// ShannonEntropy

TEST(ShannonEntropy, ZeroEntropy)
{
  ShannonEntropy ent(5, 10);
  std::vector<double> input = {5, 5, 5, 5, 5};
  auto result = ent.compute(input);
  EXPECT_DOUBLE_EQ(result[4], 0.0);
}

TEST(ShannonEntropy, MaxEntropy)
{
  ShannonEntropy ent(10, 10);
  std::vector<double> input = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
  auto result = ent.compute(input);
  EXPECT_GT(result[9], 0.8);
  EXPECT_LE(result[9], 1.0);
}

TEST(ShannonEntropy, EmptyInput)
{
  ShannonEntropy ent(5);
  auto result = ent.compute(std::span<const double>{});
  EXPECT_TRUE(result.empty());
}

TEST(ShannonEntropy, WarmupPeriod)
{
  ShannonEntropy ent(5, 10);
  auto result = ent.compute(prices());
  for (int i = 0; i < 4; ++i)
  {
    EXPECT_TRUE(std::isnan(result[i]));
  }
  EXPECT_FALSE(std::isnan(result[4]));
}

// ParkinsonVol

TEST(ParkinsonVol, BasicComputation)
{
  std::vector<double> h = {48.70, 48.72, 48.90, 48.87, 48.82, 49.05, 49.20, 49.35, 49.92, 50.19};
  std::vector<double> l = {47.79, 48.14, 48.39, 48.37, 48.24, 48.64, 48.94, 48.86, 49.50, 49.87};

  ParkinsonVol pv(5);
  auto result = pv.compute(h, l);
  ASSERT_EQ(result.size(), 10u);
  for (int i = 0; i < 4; ++i)
  {
    EXPECT_TRUE(std::isnan(result[i]));
  }
  EXPECT_FALSE(std::isnan(result[4]));
  EXPECT_GT(result[4], 0.0);
}

TEST(ParkinsonVol, EqualHighLow)
{
  std::vector<double> h = {10, 10, 10, 10, 10};
  std::vector<double> l = {10, 10, 10, 10, 10};
  ParkinsonVol pv(5);
  auto result = pv.compute(h, l);
  EXPECT_DOUBLE_EQ(result[4], 0.0);
}

TEST(ParkinsonVol, EmptyInput)
{
  ParkinsonVol pv(5);
  auto result = pv.compute(std::span<const double>{}, std::span<const double>{});
  EXPECT_TRUE(result.empty());
}

TEST(ParkinsonVol, BarOverload)
{
  std::vector<Bar> bars(10);
  for (size_t i = 0; i < 10; ++i)
  {
    bars[i].high = Price::fromDouble(101.0 + i);
    bars[i].low = Price::fromDouble(99.0 + i);
  }
  ParkinsonVol pv(5);
  auto fromBars = pv.compute(std::span<const Bar>(bars));

  std::vector<double> h(10), l(10);
  for (size_t i = 0; i < 10; ++i)
  {
    h[i] = bars[i].high.toDouble();
    l[i] = bars[i].low.toDouble();
  }
  auto fromSpans = pv.compute(h, l);

  for (size_t i = 0; i < 10; ++i)
  {
    if (std::isnan(fromBars[i]))
    {
      EXPECT_TRUE(std::isnan(fromSpans[i]));
    }
    else
    {
      EXPECT_NEAR(fromBars[i], fromSpans[i], 1e-10);
    }
  }
}

// RogersSatchellVol

TEST(RogersSatchellVol, BasicComputation)
{
  std::vector<double> o = {100, 101, 102, 103, 104};
  std::vector<double> h = {102, 103, 104, 105, 106};
  std::vector<double> l = {98, 99, 100, 101, 102};
  std::vector<double> c = {101, 102, 103, 104, 105};

  RogersSatchellVol rsv(3);
  auto result = rsv.compute(o, h, l, c);
  ASSERT_EQ(result.size(), 5u);
  for (int i = 0; i < 2; ++i)
  {
    EXPECT_TRUE(std::isnan(result[i]));
  }
  EXPECT_FALSE(std::isnan(result[2]));
  EXPECT_GT(result[2], 0.0);
}

TEST(RogersSatchellVol, FlatBars)
{
  std::vector<double> v = {100, 100, 100, 100, 100};
  RogersSatchellVol rsv(3);
  auto result = rsv.compute(v, v, v, v);
  EXPECT_DOUBLE_EQ(result[2], 0.0);
}

TEST(RogersSatchellVol, EmptyInput)
{
  RogersSatchellVol rsv(3);
  auto result = rsv.compute(std::span<const double>{}, std::span<const double>{},
                            std::span<const double>{}, std::span<const double>{});
  EXPECT_TRUE(result.empty());
}

TEST(RogersSatchellVol, BarOverload)
{
  std::vector<Bar> bars(10);
  for (size_t i = 0; i < 10; ++i)
  {
    bars[i].open = Price::fromDouble(100.0 + i);
    bars[i].high = Price::fromDouble(102.0 + i);
    bars[i].low = Price::fromDouble(98.0 + i);
    bars[i].close = Price::fromDouble(101.0 + i);
  }
  RogersSatchellVol rsv(5);
  auto fromBars = rsv.compute(std::span<const Bar>(bars));

  std::vector<double> o(10), h(10), l(10), c(10);
  for (size_t i = 0; i < 10; ++i)
  {
    o[i] = bars[i].open.toDouble();
    h[i] = bars[i].high.toDouble();
    l[i] = bars[i].low.toDouble();
    c[i] = bars[i].close.toDouble();
  }
  auto fromSpans = rsv.compute(o, h, l, c);

  for (size_t i = 0; i < 10; ++i)
  {
    if (std::isnan(fromBars[i]))
    {
      EXPECT_TRUE(std::isnan(fromSpans[i]));
    }
    else
    {
      EXPECT_NEAR(fromBars[i], fromSpans[i], 1e-10);
    }
  }
}

// Correlation

TEST(Correlation, PerfectPositive)
{
  Correlation corr(5);
  std::vector<double> x = {1, 2, 3, 4, 5};
  std::vector<double> y = {2, 4, 6, 8, 10};
  auto result = corr.compute(x, y);
  EXPECT_NEAR(result[4], 1.0, 1e-10);
}

TEST(Correlation, PerfectNegative)
{
  Correlation corr(5);
  std::vector<double> x = {1, 2, 3, 4, 5};
  std::vector<double> y = {10, 8, 6, 4, 2};
  auto result = corr.compute(x, y);
  EXPECT_NEAR(result[4], -1.0, 1e-10);
}

TEST(Correlation, ConstantSeries)
{
  Correlation corr(5);
  std::vector<double> x = {1, 2, 3, 4, 5};
  std::vector<double> y = {5, 5, 5, 5, 5};
  auto result = corr.compute(x, y);
  EXPECT_TRUE(std::isnan(result[4]));
}

TEST(Correlation, WarmupPeriod)
{
  Correlation corr(5);
  auto p = prices();
  std::vector<double> p2(p.rbegin(), p.rend());
  auto result = corr.compute(p, p2);
  for (int i = 0; i < 4; ++i)
  {
    EXPECT_TRUE(std::isnan(result[i]));
  }
  EXPECT_FALSE(std::isnan(result[4]));
}

TEST(Correlation, EmptyInput)
{
  Correlation corr(3);
  auto result = corr.compute(std::span<const double>{}, std::span<const double>{});
  EXPECT_TRUE(result.empty());
}

// Contract tests: warmup NaN for all single-input indicators

TEST(Contract, WarmupAllSingleIndicators)
{
  auto p = prices();
  auto check = [&](const std::string& name, const std::vector<double>& result, size_t warmup)
  {
    for (size_t i = 0; i < warmup && i < result.size(); ++i)
    {
      EXPECT_TRUE(std::isnan(result[i])) << name << " should be NaN at index " << i;
    }
    if (result.size() > warmup)
    {
      EXPECT_FALSE(std::isnan(result[warmup])) << name << " should be valid at index " << warmup;
    }
  };

  check("SMA(5)", SMA(5).compute(p), 4);
  check("EMA(5)", EMA(5).compute(p), 4);
  check("RMA(5)", RMA(5).compute(p), 4);
  check("RSI(5)", RSI(5).compute(p), 5);
  check("Slope(3)", Slope(3).compute(p), 3);
  check("KAMA(5)", KAMA(5).compute(p), 5);
  check("Skewness(5)", Skewness(5).compute(p), 4);
  check("Kurtosis(5)", Kurtosis(5).compute(p), 4);
  check("RollingZScore(5)", RollingZScore(5).compute(p), 4);
  check("ShannonEntropy(5)", ShannonEntropy(5).compute(p), 4);
}

TEST(Contract, EmptyAllSingleIndicators)
{
  std::span<const double> empty{};
  EXPECT_TRUE(SMA(5).compute(empty).empty());
  EXPECT_TRUE(EMA(5).compute(empty).empty());
  EXPECT_TRUE(RMA(5).compute(empty).empty());
  EXPECT_TRUE(RSI(5).compute(empty).empty());
  EXPECT_TRUE(Slope(3).compute(empty).empty());
  EXPECT_TRUE(KAMA(5).compute(empty).empty());
  EXPECT_TRUE(Skewness(5).compute(empty).empty());
  EXPECT_TRUE(Kurtosis(5).compute(empty).empty());
  EXPECT_TRUE(RollingZScore(5).compute(empty).empty());
  EXPECT_TRUE(ShannonEntropy(5).compute(empty).empty());
}
