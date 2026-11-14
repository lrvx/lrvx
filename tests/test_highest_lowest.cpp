/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2026 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Highest(n) / Lowest(n): the max / min of the last n inputs, the current
// one included. Same streaming contract as SMA: NaN until n inputs are in,
// a NaN in the window makes the value NaN until it leaves the window, and
// compute() over a series equals update() step by step, bit for bit.

#include "lrvx/indicator/highest.h"
#include "lrvx/indicator/indicator.h"
#include "lrvx/indicator/lowest.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <vector>

using namespace lrvx::indicator;

static_assert(SingleIndicator<Highest>);
static_assert(SingleIndicator<Lowest>);

namespace
{

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// The window of 3 sees the max leave (9 drops out at step 5), equal values
// (7, 7), a NaN that poisons three windows, and a min of 0.5.
const std::vector<double> kInput = {5, 9, 4, 3, 7, 7, 2, 8, kNaN, 1, 6, 0.5, 6};
const std::vector<double> kHighest = {kNaN, kNaN, 9, 9, 7, 7, 7, 8, kNaN, kNaN, kNaN, 6, 6};
const std::vector<double> kLowest = {kNaN, kNaN, 4, 3, 3, 3, 2, 2, kNaN, kNaN, kNaN, 0.5, 0.5};

bool same(double a, double b) { return (std::isnan(a) && std::isnan(b)) || a == b; }

uint64_t bitsOf(double v)
{
  uint64_t b = 0;
  std::memcpy(&b, &v, sizeof b);
  return b;
}

template <typename T>
void expectStream(const std::vector<double>& want, const char* name)
{
  T ind(3);
  EXPECT_EQ(ind.count(), 0u);
  EXPECT_FALSE(ind.ready());
  EXPECT_TRUE(std::isnan(ind.value()));
  for (size_t i = 0; i < kInput.size(); ++i)
  {
    ind.update(kInput[i]);
    EXPECT_TRUE(same(ind.value(), want[i]))
        << name << " step " << i << ": got " << ind.value() << ", want " << want[i];
    EXPECT_EQ(ind.ready(), !std::isnan(want[i])) << name << " step " << i;
    EXPECT_EQ(ind.count(), i + 1) << name << " counts every update, NaN included";
  }

  const auto batch = ind.compute(kInput);
  ASSERT_EQ(batch.size(), kInput.size());
  for (size_t i = 0; i < kInput.size(); ++i)
  {
    EXPECT_TRUE(same(batch[i], want[i])) << name << " compute()[" << i << "]";
  }

  ind.reset();
  EXPECT_EQ(ind.count(), 0u);
  EXPECT_FALSE(ind.ready());
  EXPECT_TRUE(std::isnan(ind.value()));
  ind.update(1.0);
  ind.update(2.0);
  EXPECT_FALSE(ind.ready()) << name << ": reset() forgets the old window";
  ind.update(3.0);
  EXPECT_TRUE(ind.ready());
}

std::vector<double> walk(size_t n, uint32_t seed, bool nans)
{
  std::mt19937 rng(seed);
  std::normal_distribution<double> step(0.0, 1.0);
  std::vector<double> out(n);
  double px = 100.0;
  for (size_t i = 0; i < n; ++i)
  {
    px += step(rng);
    // Repeat values now and then so ties are exercised.
    out[i] = (i % 17 == 5 && i > 0) ? out[i - 1] : px;
  }
  if (nans)
  {
    for (size_t i : {40u, 41u, 300u, 777u})
    {
      out[i] = kNaN;
    }
  }
  return out;
}

template <typename T, typename Pick>
void expectParityAndReference(size_t period, Pick pick, const char* name)
{
  for (bool nans : {false, true})
  {
    const auto input = walk(1000, 7, nans);
    T stream(period);
    const auto batch = T(period).compute(input);
    ASSERT_EQ(batch.size(), input.size());
    for (size_t i = 0; i < input.size(); ++i)
    {
      stream.update(input[i]);
      EXPECT_EQ(bitsOf(stream.value()), bitsOf(batch[i]))
          << name << "(" << period << ") step " << i << " streaming vs compute";

      // Naive reference over the window.
      double ref = kNaN;
      if (i + 1 >= period)
      {
        ref = input[i + 1 - period];
        bool poisoned = false;
        for (size_t j = i + 1 - period; j <= i; ++j)
        {
          poisoned = poisoned || std::isnan(input[j]);
          ref = pick(ref, input[j]);
        }
        if (poisoned)
        {
          ref = kNaN;
        }
      }
      EXPECT_TRUE(same(batch[i], ref))
          << name << "(" << period << ") step " << i << ": got " << batch[i] << ", want " << ref;
    }
  }
}

}  // namespace

TEST(Highest, WarmUpDropOutEqualValuesNaNAndReset) { expectStream<Highest>(kHighest, "Highest"); }

TEST(Lowest, WarmUpDropOutEqualValuesNaNAndReset) { expectStream<Lowest>(kLowest, "Lowest"); }

TEST(Highest, PeriodOneIsTheInput)
{
  Highest h(1);
  for (double v : {3.0, 1.0, 2.0})
  {
    h.update(v);
    EXPECT_EQ(h.value(), v);
  }
  EXPECT_EQ(h.period(), 1u);
}

TEST(Highest, MatchesANaiveWindowAndStreamsLikeCompute)
{
  auto pick = [](double a, double b)
  { return std::max(a, b); };
  for (size_t p : {1u, 2u, 5u, 20u, 64u})
  {
    expectParityAndReference<Highest>(p, pick, "Highest");
  }
}

TEST(Lowest, MatchesANaiveWindowAndStreamsLikeCompute)
{
  auto pick = [](double a, double b)
  { return std::min(a, b); };
  for (size_t p : {1u, 2u, 5u, 20u, 64u})
  {
    expectParityAndReference<Lowest>(p, pick, "Lowest");
  }
}

// A monotone series is the worst case for a naive rescan of the window and
// the best case for a monotonic deque: either way an update stays O(1)
// amortized, so a long window costs about what a short one does.
TEST(Highest, UpdateCostDoesNotGrowWithThePeriod)
{
  auto timeIt = [](size_t period)
  {
    Highest h(period);
    Lowest l(period);
    const auto t0 = std::chrono::steady_clock::now();
    double sink = 0;
    for (size_t i = 0; i < 400000; ++i)
    {
      // Falling for Highest (each new max scan would cover the whole window),
      // rising for Lowest.
      h.update(1e6 - static_cast<double>(i));
      l.update(static_cast<double>(i));
      sink += h.value() + l.value();
    }
    const auto t1 = std::chrono::steady_clock::now();
    EXPECT_FALSE(std::isnan(sink));
    return std::chrono::duration<double>(t1 - t0).count();
  };
  timeIt(8);  // warm-up
  const double small = timeIt(8);
  const double large = timeIt(4096);
  EXPECT_LT(large, small * 8.0 + 0.05)
      << "period 4096 took " << large << " s against " << small << " s for period 8";
}

TEST(HighestLowest, AreRegistryRowsWithAFloorOfOne)
{
  bool highest = false;
  bool lowest = false;
#define LRVX_INDICATOR(Cls, snake, Kind, Args)     \
  if (std::string(#snake) == "highest")            \
  {                                                \
    highest = std::string(#Kind) == "SingleInput"; \
  }                                                \
  if (std::string(#snake) == "lowest")             \
  {                                                \
    lowest = std::string(#Kind) == "SingleInput";  \
  }
#include "lrvx/indicator/registry.def"
#undef LRVX_INDICATOR
  EXPECT_TRUE(highest) << "registry.def has a SingleInput row named highest";
  EXPECT_TRUE(lowest) << "registry.def has a SingleInput row named lowest";
}
