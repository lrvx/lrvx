/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2026 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

// Probability of backtest overfitting by combinatorially symmetric
// cross-validation (CSCV).
//
// Reference: Bailey, Borwein, Lopez de Prado, Zhu (2017). "The Probability
// of Backtest Overfitting." Journal of Computational Finance 20(4).
//
// The question it answers: when a sweep over N parameter combinations picks
// the one with the best in-sample performance, how often does that pick land
// in the worse half out of sample? A sweep whose winner is a fluke of the
// period it was fitted on answers "about half the time or more".
//
// Algorithm, with T periods of returns for each of N trials:
//   1. Cut the T periods into S blocks of consecutive periods (S even).
//   2. For every way of choosing S/2 of the blocks (C(S, S/2) of them), the
//      chosen blocks are the in-sample set J and the rest the out-of-sample
//      set J-bar. Every J also appears as somebody else's J-bar.
//   3. On each split, score every trial on J and on J-bar with the Sharpe
//      ratio of its returns there (mean over sample standard deviation; 0
//      when the deviation is 0). n* is the trial with the best in-sample
//      score.
//   4. The relative out-of-sample rank of n* is w = r / (N + 1), where r is
//      1 for the worst out-of-sample score and N for the best, so w is in
//      (0, 1). Its logit is log(w / (1 - w)).
//   5. PBO is the fraction of splits whose logit is below 0: the in-sample
//      winner performed below the out-of-sample median.
//
// returns is row-major, trials x periods, the layout whitesRealityCheck
// takes. numBlocks is S; 16 gives 12 870 splits. The result is deterministic:
// nothing is sampled.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace lrvx::stats
{

struct PboResult
{
  // Fraction of splits where the in-sample best ranked below the
  // out-of-sample median; NaN when the inputs cannot be split (fewer than 2
  // trials, S odd, S below 2 or above 32, fewer periods than blocks).
  double pbo;
  // C(S, S/2) splits evaluated, 0 when pbo is NaN.
  std::uint64_t splits;
  // Mean relative out-of-sample rank of the in-sample best, in (0, 1).
  double meanOosRank;
  // Mean logit over the splits.
  double meanLogit;
};

inline PboResult pboCscv(const double* returns, std::size_t numTrials, std::size_t numPeriods,
                         std::uint32_t numBlocks = 16)
{
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const std::size_t N = numTrials;
  const std::size_t T = numPeriods;
  const std::size_t S = numBlocks;
  if (returns == nullptr || N < 2 || S < 2 || S > 32 || (S % 2) != 0 || T < S)
  {
    return {nan, 0, nan, nan};
  }

  // Per trial, per block: count, sum and sum of squares, so a Sharpe ratio
  // over any union of blocks is a few additions.
  std::vector<double> blockSum(N * S, 0.0);
  std::vector<double> blockSumSq(N * S, 0.0);
  std::vector<std::size_t> blockCount(S, 0);
  for (std::size_t b = 0; b < S; ++b)
  {
    const std::size_t from = b * T / S;
    const std::size_t to = (b + 1) * T / S;
    blockCount[b] = to - from;
    for (std::size_t k = 0; k < N; ++k)
    {
      double s = 0.0;
      double q = 0.0;
      for (std::size_t t = from; t < to; ++t)
      {
        const double v = returns[k * T + t];
        s += v;
        q += v * v;
      }
      blockSum[k * S + b] = s;
      blockSumSq[k * S + b] = q;
    }
  }

  auto sharpeOver = [&](std::size_t k, std::uint64_t mask) -> double
  {
    double s = 0.0;
    double q = 0.0;
    std::size_t n = 0;
    for (std::size_t b = 0; b < S; ++b)
    {
      if (mask & (std::uint64_t{1} << b))
      {
        s += blockSum[k * S + b];
        q += blockSumSq[k * S + b];
        n += blockCount[b];
      }
    }
    if (n < 2)
    {
      return 0.0;
    }
    const double mean = s / static_cast<double>(n);
    const double var = (q - static_cast<double>(n) * mean * mean) / static_cast<double>(n - 1);
    if (!(var > 0.0))
    {
      return 0.0;
    }
    return mean / std::sqrt(var);
  };

  std::vector<double> isScore(N);
  std::vector<double> oosScore(N);
  const std::uint64_t all = (S == 64) ? ~std::uint64_t{0} : ((std::uint64_t{1} << S) - 1);
  std::uint64_t splits = 0;
  std::uint64_t below = 0;
  double rankSum = 0.0;
  double logitSum = 0.0;

  // Every S-bit mask with exactly S/2 bits set, in increasing order
  // (Gosper's hack).
  std::uint64_t mask = (std::uint64_t{1} << (S / 2)) - 1;
  while (mask <= all)
  {
    const std::uint64_t oos = all & ~mask;
    std::size_t best = 0;
    for (std::size_t k = 0; k < N; ++k)
    {
      isScore[k] = sharpeOver(k, mask);
      oosScore[k] = sharpeOver(k, oos);
      if (isScore[k] > isScore[best])
      {
        best = k;
      }
    }
    std::size_t rank = 0;
    for (std::size_t k = 0; k < N; ++k)
    {
      if (oosScore[k] <= oosScore[best])
      {
        ++rank;
      }
    }
    const double w = static_cast<double>(rank) / static_cast<double>(N + 1);
    const double logit = std::log(w / (1.0 - w));
    ++splits;
    if (logit < 0.0)
    {
      ++below;
    }
    rankSum += w;
    logitSum += logit;

    const std::uint64_t u = mask & (~mask + 1);
    const std::uint64_t v = mask + u;
    if (v == 0 || v > all)
    {
      break;
    }
    mask = v + (((v ^ mask) / u) >> 2);
  }

  PboResult out;
  out.splits = splits;
  out.pbo = static_cast<double>(below) / static_cast<double>(splits);
  out.meanOosRank = rankSum / static_cast<double>(splits);
  out.meanLogit = logitSum / static_cast<double>(splits);
  return out;
}

}  // namespace lrvx::stats
