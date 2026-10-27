/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2026 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "lrvx/stats/deflated_sharpe.h"
#include "lrvx/stats/grid_neighborhood.h"
#include "lrvx/stats/normal.h"
#include "lrvx/stats/pbo_cscv.h"

#include <gtest/gtest.h>
#include <cmath>
#include <random>
#include <vector>

using namespace lrvx::stats;

namespace
{

// trials x periods, row-major. Every trial is N(0, 0.01) noise; the trial at
// `signal` gets `shift` added to every period.
std::vector<double> noiseMatrix(std::size_t trials, std::size_t periods, unsigned seed,
                                std::size_t signal = static_cast<std::size_t>(-1),
                                double shift = 0.0)
{
  std::mt19937 rng(seed);
  std::normal_distribution<double> dist(0.0, 0.01);
  std::vector<double> m(trials * periods);
  for (std::size_t k = 0; k < trials; ++k)
  {
    for (std::size_t t = 0; t < periods; ++t)
    {
      m[k * periods + t] = dist(rng) + (k == signal ? shift : 0.0);
    }
  }
  return m;
}

}  // namespace

// ---------------------------------------------------------------- normal

TEST(Normal, QuantileInvertsTheCdf)
{
  for (double p : {1e-6, 1e-3, 0.02425, 0.1, 0.5, 0.9, 0.97575, 0.999, 1.0 - 1e-6})
  {
    const double x = normalQuantile(p);
    EXPECT_NEAR(normalCdf(x), p, 1e-13) << "p=" << p;
  }
  EXPECT_NEAR(normalQuantile(0.5), 0.0, 1e-15);
  EXPECT_NEAR(normalQuantile(0.975), 1.959963984540054, 1e-12);
  EXPECT_NEAR(normalQuantile(0.9), 1.2815515655446004, 1e-12);
  EXPECT_TRUE(std::isinf(normalQuantile(0.0)) && normalQuantile(0.0) < 0);
  EXPECT_TRUE(std::isinf(normalQuantile(1.0)) && normalQuantile(1.0) > 0);
  EXPECT_TRUE(std::isnan(normalQuantile(1.5)));
  EXPECT_TRUE(std::isnan(normalQuantile(-0.1)));
}

// ---------------------------------------------------------------- PBO

// Two trials, four periods, two blocks of two: the only two splits are
// (block 0 in, block 1 out) and the reverse. Trial A beats B on every
// block, so the in-sample best is always A and A also ranks best out of
// sample: rank 2 of 2, w = 2/3, logit > 0 on both splits.
TEST(PboCscv, ConsistentWinnerHasZeroPbo)
{
  const std::vector<double> m = {1.0, 2.0, 1.0, 2.0,       // A
                                 -1.0, -2.0, -1.0, -2.0};  // B
  const PboResult r = pboCscv(m.data(), 2, 4, 2);
  EXPECT_EQ(r.splits, 2u);
  EXPECT_DOUBLE_EQ(r.pbo, 0.0);
  EXPECT_NEAR(r.meanOosRank, 2.0 / 3.0, 1e-12);
  EXPECT_NEAR(r.meanLogit, std::log(2.0), 1e-12);
}

// The winner of each block loses the other one: whichever trial is best in
// sample is worst out of sample, rank 1 of 2, w = 1/3, logit < 0 on both
// splits.
TEST(PboCscv, WinnerThatFlipsHasPboOne)
{
  const std::vector<double> m = {1.0, 2.0, -1.0, -2.0,   // A
                                 -1.0, -2.0, 1.0, 2.0};  // B
  const PboResult r = pboCscv(m.data(), 2, 4, 2);
  EXPECT_EQ(r.splits, 2u);
  EXPECT_DOUBLE_EQ(r.pbo, 1.0);
  EXPECT_NEAR(r.meanOosRank, 1.0 / 3.0, 1e-12);
  EXPECT_NEAR(r.meanLogit, -std::log(2.0), 1e-12);
}

TEST(PboCscv, SplitCountIsChooseSOverHalfS)
{
  const auto m = noiseMatrix(6, 256, 11);
  EXPECT_EQ(pboCscv(m.data(), 6, 256, 8).splits, 70u);      // C(8,4)
  EXPECT_EQ(pboCscv(m.data(), 6, 256, 16).splits, 12870u);  // C(16,8)
  EXPECT_EQ(pboCscv(m.data(), 6, 256, 2).splits, 2u);
}

TEST(PboCscv, RealSignalIsRarelyOverfit)
{
  // A +0.5 sigma per-period edge on one of ten trials is a Sharpe near 0.5
  // per period on 128 out-of-sample periods, so that trial wins in sample
  // and stays above the median out of sample on almost every split.
  const auto m = noiseMatrix(10, 256, 7, /*signal=*/3, /*shift=*/0.005);
  const PboResult r = pboCscv(m.data(), 10, 256, 8);
  EXPECT_LT(r.pbo, 0.1) << "pbo=" << r.pbo;
  EXPECT_GT(r.meanLogit, 0.0);
}

TEST(PboCscv, PureNoiseIsOverfitAboutHalfTheTime)
{
  // With no skill anywhere, the in-sample best is a coin toss out of
  // sample. Averaged over a few seeds the PBO sits near one half.
  double sum = 0.0;
  for (unsigned seed = 1; seed <= 6; ++seed)
  {
    const auto m = noiseMatrix(12, 256, seed);
    sum += pboCscv(m.data(), 12, 256, 8).pbo;
  }
  const double mean = sum / 6.0;
  EXPECT_GT(mean, 0.3) << mean;
  EXPECT_LT(mean, 0.7) << mean;
}

TEST(PboCscv, InputsThatCannotBeSplitAnswerNaN)
{
  const auto m = noiseMatrix(4, 32, 3);
  EXPECT_TRUE(std::isnan(pboCscv(m.data(), 1, 32, 8).pbo)) << "one trial";
  EXPECT_TRUE(std::isnan(pboCscv(m.data(), 4, 32, 7).pbo)) << "odd S";
  EXPECT_TRUE(std::isnan(pboCscv(m.data(), 4, 32, 64).pbo)) << "S above 32";
  EXPECT_TRUE(std::isnan(pboCscv(m.data(), 4, 4, 8).pbo)) << "fewer periods than blocks";
  EXPECT_TRUE(std::isnan(pboCscv(nullptr, 4, 32, 8).pbo));
  EXPECT_EQ(pboCscv(m.data(), 4, 32, 7).splits, 0u);
}

TEST(PboCscv, IsDeterministic)
{
  const auto m = noiseMatrix(8, 128, 5, 2, 0.003);
  const PboResult a = pboCscv(m.data(), 8, 128, 8);
  const PboResult b = pboCscv(m.data(), 8, 128, 8);
  EXPECT_EQ(a.pbo, b.pbo);
  EXPECT_EQ(a.meanLogit, b.meanLogit);
}

// ---------------------------------------------------------------- DSR

namespace
{

struct Moments
{
  double sharpe, skew, kurt;
};

Moments momentsOf(const std::vector<double>& r)
{
  const double T = static_cast<double>(r.size());
  double mean = 0.0;
  for (double v : r)
  {
    mean += v;
  }
  mean /= T;
  double m2 = 0, m3 = 0, m4 = 0, sv = 0;
  for (double v : r)
  {
    const double d = v - mean;
    m2 += d * d;
    m3 += d * d * d;
    m4 += d * d * d * d;
  }
  sv = m2 / (T - 1.0);
  m2 /= T;
  m3 /= T;
  m4 /= T;
  return {mean / std::sqrt(sv), m3 / std::pow(m2, 1.5), m4 / (m2 * m2)};
}

std::vector<double> series(std::size_t n, unsigned seed, double shift)
{
  std::mt19937 rng(seed);
  std::normal_distribution<double> dist(0.0, 0.01);
  std::vector<double> r(n);
  for (auto& v : r)
  {
    v = dist(rng) + shift;
  }
  return r;
}

}  // namespace

TEST(DeflatedSharpe, OneTrialIsTheProbabilisticSharpeRatioAgainstZero)
{
  const auto r = series(500, 21, 0.002);
  const double trial = 0.0;
  const DeflatedSharpeResult d = deflatedSharpeRatio(r.data(), r.size(), &trial, 1);
  const Moments m = momentsOf(r);
  EXPECT_NEAR(d.sharpe, m.sharpe, 1e-12);
  EXPECT_NEAR(d.skewness, m.skew, 1e-12);
  EXPECT_NEAR(d.kurtosis, m.kurt, 1e-12);
  EXPECT_DOUBLE_EQ(d.expectedMaxSharpe, 0.0);
  const double z = m.sharpe * std::sqrt(499.0) /
                   std::sqrt(1.0 - m.skew * m.sharpe + (m.kurt - 1.0) / 4.0 * m.sharpe * m.sharpe);
  EXPECT_NEAR(d.dsr, normalCdf(z), 1e-12);
  EXPECT_GT(d.dsr, 0.99) << "a 0.2 sigma edge over 500 periods is significant";
}

TEST(DeflatedSharpe, ExpectedMaximumFollowsTheFormulaAndGrowsWithTrials)
{
  const auto r = series(300, 4, 0.001);
  // Ten trial Sharpe ratios scaled to a sample variance of exactly 0.01.
  auto scaledToVariance = [](std::size_t n, double variance)
  {
    std::vector<double> v(n);
    double ss = 0.0;
    for (std::size_t i = 0; i < n; ++i)
    {
      v[i] = static_cast<double>(i) - (static_cast<double>(n) - 1.0) / 2.0;
      ss += v[i] * v[i];
    }
    const double scale = std::sqrt(variance / (ss / (static_cast<double>(n) - 1.0)));
    for (auto& x : v)
    {
      x *= scale;
    }
    return v;
  };
  const std::vector<double> ten = scaledToVariance(10, 0.01);
  const DeflatedSharpeResult d = deflatedSharpeRatio(r.data(), r.size(), ten.data(), ten.size());
  constexpr double euler = 0.57721566490153286;
  const double expected = 0.1 * ((1.0 - euler) * normalQuantile(0.9) +
                                 euler * normalQuantile(1.0 - 1.0 / (10.0 * std::exp(1.0))));
  EXPECT_NEAR(d.expectedMaxSharpe, expected, 1e-12);
  // Q(0.9) = 1.28155 and Q(1 - 1/(10 e)) = Q(0.96321) = 1.78916, so SR0 is
  // 0.1 * (0.42278 * 1.28155 + 0.57722 * 1.78916) = 0.15746.
  EXPECT_NEAR(d.expectedMaxSharpe, 0.15746, 1e-4);

  const std::vector<double> thousand = scaledToVariance(1000, 0.01);
  const DeflatedSharpeResult more =
      deflatedSharpeRatio(r.data(), r.size(), thousand.data(), thousand.size());
  EXPECT_GT(more.expectedMaxSharpe, d.expectedMaxSharpe);
  EXPECT_LT(more.dsr, d.dsr) << "the same Sharpe is less convincing after more trials";
}

TEST(DeflatedSharpe, DegenerateInputsAnswerNaN)
{
  const double one[1] = {0.01};
  EXPECT_TRUE(std::isnan(deflatedSharpeRatio(one, 1, nullptr, 0).sharpe));
  const double flat[4] = {0.01, 0.01, 0.01, 0.01};
  EXPECT_TRUE(std::isnan(deflatedSharpeRatio(flat, 4, nullptr, 0).sharpe));
  EXPECT_TRUE(std::isnan(deflatedSharpeRatio(nullptr, 4, nullptr, 0).dsr));
}

// ---------------------------------------------------------------- grid

TEST(GridNeighborhood, TwoDimensionalRadiusOne)
{
  const std::vector<double> v = {1, 2, 3, 4, 5, 6, 7, 8, 9};
  const std::size_t shape[2] = {3, 3};
  std::vector<double> mean(9);
  std::vector<std::uint32_t> count(9);
  gridNeighborhood(v.data(), shape, 2, 1, mean.data(), count.data());
  EXPECT_EQ(count[4], 8u);
  EXPECT_DOUBLE_EQ(mean[4], 5.0);  // (45 - 5) / 8
  EXPECT_EQ(count[0], 3u);
  EXPECT_NEAR(mean[0], (2.0 + 4.0 + 5.0) / 3.0, 1e-12);
  EXPECT_EQ(count[1], 5u);
  EXPECT_NEAR(mean[1], (1.0 + 3.0 + 4.0 + 5.0 + 6.0) / 5.0, 1e-12);
  EXPECT_EQ(count[8], 3u);
  EXPECT_NEAR(mean[8], (5.0 + 6.0 + 8.0) / 3.0, 1e-12);
}

TEST(GridNeighborhood, OneDimensionAndRadiusTwo)
{
  const std::vector<double> v = {1, 2, 3, 4};
  const std::size_t shape[1] = {4};
  std::vector<double> mean(4);
  std::vector<std::uint32_t> count(4);
  gridNeighborhood(v.data(), shape, 1, 1, mean.data(), count.data());
  EXPECT_EQ(count[0], 1u);
  EXPECT_DOUBLE_EQ(mean[0], 2.0);
  EXPECT_EQ(count[1], 2u);
  EXPECT_DOUBLE_EQ(mean[1], 2.0);
  gridNeighborhood(v.data(), shape, 1, 2, mean.data(), count.data());
  EXPECT_EQ(count[0], 2u);
  EXPECT_DOUBLE_EQ(mean[0], 2.5);
  EXPECT_EQ(count[1], 3u);
  EXPECT_NEAR(mean[1], (1.0 + 3.0 + 4.0) / 3.0, 1e-12);
}

TEST(GridNeighborhood, NaNValuesAreSkippedAndEmptyNeighbourhoodsAreNaN)
{
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const std::vector<double> v = {1, nan, 3};
  const std::size_t shape[1] = {3};
  std::vector<double> mean(3);
  std::vector<std::uint32_t> count(3);
  gridNeighborhood(v.data(), shape, 1, 1, mean.data(), count.data());
  EXPECT_EQ(count[0], 0u);
  EXPECT_TRUE(std::isnan(mean[0]));
  EXPECT_EQ(count[1], 2u);
  EXPECT_DOUBLE_EQ(mean[1], 2.0);
  gridNeighborhood(v.data(), shape, 1, 0, mean.data(), count.data());
  EXPECT_EQ(count[2], 0u);
  EXPECT_TRUE(std::isnan(mean[2]));
}

TEST(GridNeighborhood, ThreeDimensionalInteriorHasTwentySixNeighbours)
{
  std::vector<double> v(27, 1.0);
  v[13] = 100.0;  // the centre
  const std::size_t shape[3] = {3, 3, 3};
  std::vector<double> mean(27);
  std::vector<std::uint32_t> count(27);
  gridNeighborhood(v.data(), shape, 3, 1, mean.data(), count.data());
  EXPECT_EQ(count[13], 26u);
  EXPECT_DOUBLE_EQ(mean[13], 1.0);
  EXPECT_EQ(count[0], 7u);
  EXPECT_NEAR(mean[0], (6.0 + 100.0) / 7.0, 1e-12);
}
