// tests/test_indicator_streaming_cost.cpp
//
// A streaming update() must cost the same on update 200000 as on update 200:
// the work and the memory of one step may depend on the indicator's window,
// never on how many values it has already seen.
//
// Two checks:
//
//   - Structural: once an indicator is constructed, update() / value() never
//     allocate. Every buffer an indicator needs is sized by its parameters
//     and reserved up front, so memory cannot grow with history. Counted by
//     replacing the global operator new for this test binary.
//   - Timing: 10N updates must take less than 25 x the time of N updates,
//     or less than a quarter of a second when that product is smaller.
//     A per-update cost that is constant gives a ratio near 10; one that
//     grows linearly with history gives a ratio near 100. The bound sits
//     between the two with a wide margin either way, and each side takes the
//     minimum over several runs so a preempted run on a shared CI runner does
//     not decide the outcome. A run that passes the bound is cut short and
//     the next one tried, so a regression fails in seconds instead of
//     running a quadratic loop to completion; the check fails when no run
//     finishes inside the bound.

#include "lrvx/indicator/atr.h"
#include "lrvx/indicator/autocorrelation.h"
#include "lrvx/indicator/bollinger.h"
#include "lrvx/indicator/cci.h"
#include "lrvx/indicator/chop.h"
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
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <random>
#include <vector>

namespace
{
std::atomic<size_t> gAllocations{0};
}  // namespace

// Replacing the global operator new does not mix with the sanitizers, which
// interpose new/delete themselves (see test_hot_path_allocation.cpp). Under a
// sanitizer build the allocation cases skip; the timing cases still run.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define LRVX_TEST_UNDER_SANITIZER 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || \
    __has_feature(memory_sanitizer)
#define LRVX_TEST_UNDER_SANITIZER 1
#endif
#endif

#ifndef LRVX_TEST_UNDER_SANITIZER
#define LRVX_TEST_UNDER_SANITIZER 0
#endif

#if LRVX_TEST_UNDER_SANITIZER
#define SKIP_UNDER_SANITIZER() \
  GTEST_SKIP() << "allocation counting replaces global operator new, which the sanitizers interpose"
#else
#define SKIP_UNDER_SANITIZER() ((void)0)

void* operator new(std::size_t size)
{
  gAllocations.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(size == 0 ? 1 : size))
  {
    return p;
  }
  throw std::bad_alloc();
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

#endif  // LRVX_TEST_UNDER_SANITIZER

using namespace lrvx::indicator;

namespace
{

struct Bar4
{
  double open, high, low, close;
};

std::vector<Bar4> randomBars(size_t n, uint32_t seed)
{
  std::mt19937 rng(seed);
  std::normal_distribution<double> step(0.0, 1.0);
  std::uniform_real_distribution<double> spread(0.1, 2.0);
  std::vector<Bar4> out(n);
  double px = 1000.0;
  for (auto& b : out)
  {
    b.open = px;
    px += step(rng);
    b.close = px;
    b.high = std::max(b.open, b.close) + spread(rng);
    b.low = std::min(b.open, b.close) - spread(rng);
  }
  return out;
}

// Each feeder drives one indicator with one bar and reads value(), the way a
// strategy does on every bar.
template <typename T>
double feed(T& ind, const Bar4& b)
{
  if constexpr (requires { ind.update(b.open, b.high, b.low, b.close); })
  {
    ind.update(b.open, b.high, b.low, b.close);
  }
  else if constexpr (requires { ind.update(b.high, b.low, b.close); })
  {
    ind.update(b.high, b.low, b.close);
  }
  else if constexpr (requires { ind.update(b.high, b.low); })
  {
    ind.update(b.high, b.low);
  }
  else
  {
    ind.update(b.close);
  }
  return ind.value();
}

volatile double gSink = 0.0;

using Clock = std::chrono::steady_clock;

// Runs `n` updates on a fresh indicator. Returns elapsed seconds, or a
// negative value when the run exceeded `budget` seconds and was cut short.
template <typename Make>
double timedRun(Make make, const std::vector<Bar4>& bars, size_t n, double budget)
{
  auto ind = make();
  double acc = 0.0;
  const auto t0 = Clock::now();
  for (size_t i = 0; i < n; ++i)
  {
    acc += feed(ind, bars[i]);
    if ((i & 255) == 255)
    {
      const double el = std::chrono::duration<double>(Clock::now() - t0).count();
      if (el > budget)
      {
        gSink = acc;
        return -el;
      }
    }
  }
  const double el = std::chrono::duration<double>(Clock::now() - t0).count();
  gSink = acc;
  return el;
}

constexpr double kMaxRatio = 25.0;

// N updates of an O(1) indicator take about a tenth of a millisecond, so
// 25 x that is a budget one scheduler pause on a shared runner can exceed.
// The budget therefore has an absolute floor. It costs nothing in power: a
// per-update cost that grows with history turns the 10N run into seconds
// (48 s for RollingZScore(240) at N = 2000 before the kernels), far past it.
constexpr double kBudgetFloorSeconds = 0.25;
constexpr int kLongRuns = 5;

template <typename Make>
void checkCostIndependentOfHistory(const char* name, Make make, size_t n)
{
  const auto bars = randomBars(10 * n, 11);

  double t1 = 1e300;
  for (int r = 0; r < 5; ++r)
  {
    t1 = std::min(t1, timedRun(make, bars, n, 1e300));
  }
  // Below the steady_clock's useful resolution the ratio is noise. Fail
  // loudly instead of passing on a meaningless number.
  ASSERT_GT(t1, 0.0) << name;

  // One slow run is the machine; every run slow is the code. A run past the
  // budget is cut short and the next one tried; the check fails only when
  // none of them finishes inside it.
  const double budget = std::max(kMaxRatio * t1, kBudgetFloorSeconds);
  double t10 = 1e300;
  double longestCut = 0.0;
  for (int r = 0; r < kLongRuns; ++r)
  {
    const double t = timedRun(make, bars, 10 * n, budget);
    if (t < 0)
    {
      longestCut = std::max(longestCut, -t);
      continue;
    }
    t10 = std::min(t10, t);
  }
  if (t10 == 1e300)
  {
    FAIL() << name << ": " << 10 * n << " updates never finished inside the budget of " << budget
           << " s (" << kMaxRatio << " x the time of " << n << " updates, " << t1
           << " s, with a floor of " << kBudgetFloorSeconds << " s) in " << kLongRuns
           << " runs; the longest was cut short after " << longestCut
           << " s; per-update cost grows with history";
  }
  std::printf("%s: t(N=%zu)=%.6f s, t(10N)=%.6f s, ratio %.2f (budget %.3f s)\n", name, n, t1,
              t10, t10 / t1, budget);
  EXPECT_LT(t10, budget) << name << ": t(N)=" << t1 << " s, t(10N)=" << t10
                         << " s, ratio=" << t10 / t1;
}

template <typename Make>
void checkNoAllocation(const char* name, Make make, size_t n = 5000)
{
  const auto bars = randomBars(n, 5);
  auto ind = make();
  double acc = 0.0;
  const size_t before = gAllocations.load();
  for (size_t i = 0; i < n; ++i)
  {
    acc += feed(ind, bars[i]);
    acc += ind.ready() ? 1.0 : 0.0;
  }
  const size_t after = gAllocations.load();
  gSink = acc;
  EXPECT_EQ(after - before, 0u) << name << ": update()/value()/ready() allocated "
                                << (after - before) << " times over " << n << " updates";
  EXPECT_EQ(ind.count(), n) << name;
}

}  // namespace

// ---- Timing: N vs 10N ------------------------------------------------------

TEST(IndicatorStreamingCost, RollingZScore240)
{
  checkCostIndependentOfHistory("RollingZScore(240)", []
                                { return RollingZScore(240); }, 2000);
}

TEST(IndicatorStreamingCost, SMA240)
{
  checkCostIndependentOfHistory("SMA(240)", []
                                { return SMA(240); }, 20000);
}

TEST(IndicatorStreamingCost, EMA240)
{
  checkCostIndependentOfHistory("EMA(240)", []
                                { return EMA(240); }, 20000);
}

TEST(IndicatorStreamingCost, RSI14)
{
  checkCostIndependentOfHistory("RSI(14)", []
                                { return RSI(14); }, 20000);
}

TEST(IndicatorStreamingCost, ATR14)
{
  checkCostIndependentOfHistory("ATR(14)", []
                                { return ATR(14); }, 20000);
}

TEST(IndicatorStreamingCost, CHOP240)
{
  checkCostIndependentOfHistory("CHOP(240)", []
                                { return CHOP(240); }, 2000);
}

// ---- Structural: no allocation after construction -------------------------

TEST(IndicatorStreamingCost, NoAllocationPerUpdate)
{
  SKIP_UNDER_SANITIZER();
  checkNoAllocation("EMA", []
                    { return EMA(20); });
  checkNoAllocation("SMA", []
                    { return SMA(20); });
  checkNoAllocation("RMA", []
                    { return RMA(20); });
  checkNoAllocation("RSI", []
                    { return RSI(14); });
  checkNoAllocation("KAMA", []
                    { return KAMA(10, 2, 30); });
  checkNoAllocation("DEMA", []
                    { return DEMA(20); });
  checkNoAllocation("TEMA", []
                    { return TEMA(20); });
  checkNoAllocation("Slope", []
                    { return Slope(20); });
  checkNoAllocation("Skewness", []
                    { return Skewness(20); });
  checkNoAllocation("Kurtosis", []
                    { return Kurtosis(20); });
  checkNoAllocation("RollingZScore", []
                    { return RollingZScore(20); });
  checkNoAllocation("ShannonEntropy", []
                    { return ShannonEntropy(20, 10); });
  checkNoAllocation("AutoCorrelation", []
                    { return AutoCorrelation(20, 3); });
  checkNoAllocation("ATR", []
                    { return ATR(14); });
  checkNoAllocation("CCI", []
                    { return CCI(20); });
  checkNoAllocation("CHOP", []
                    { return CHOP(14); });
  checkNoAllocation("Stochastic", []
                    { return Stochastic(14, 3); });
  checkNoAllocation("ParkinsonVol", []
                    { return ParkinsonVol(20); });
  checkNoAllocation("RogersSatchellVol", []
                    { return RogersSatchellVol(20); });
  checkNoAllocation("MACD", []
                    { return MACD(12, 26, 9); });
  checkNoAllocation("Bollinger", []
                    { return Bollinger(20, 2.0); });
  checkNoAllocation("Correlation", []
                    { return Correlation(20); });
}

// reset() returns an indicator to its just-constructed state without giving
// up the buffers it already owns.
TEST(IndicatorStreamingCost, ResetDoesNotAllocate)
{
  SKIP_UNDER_SANITIZER();
  const auto bars = randomBars(1000, 3);
  RollingZScore z(50);
  for (const auto& b : bars)
  {
    feed(z, b);
  }
  const size_t before = gAllocations.load();
  z.reset();
  double acc = 0.0;
  for (const auto& b : bars)
  {
    acc += feed(z, b);
  }
  const size_t after = gAllocations.load();
  gSink = acc;
  EXPECT_EQ(after - before, 0u);
  EXPECT_EQ(z.count(), bars.size());
}
