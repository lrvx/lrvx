/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2026 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

// The deflated Sharpe ratio: the probability that a strategy's true Sharpe
// ratio is above the Sharpe ratio one expects from the best of N unskilled
// trials, given the length of the sample and the shape of the returns.
//
// References: Bailey, Lopez de Prado (2014). "The Deflated Sharpe Ratio:
// Correcting for Selection Bias, Backtest Overfitting and Non-Normality."
// Journal of Portfolio Management 40(5). The probabilistic Sharpe ratio it
// builds on: Bailey, Lopez de Prado (2012). "The Sharpe Ratio Efficient
// Frontier." Journal of Risk 15(2).
//
// With SR the per-period Sharpe ratio of the selected strategy's returns
// (mean over sample standard deviation), T periods, g3 the skewness and g4
// the kurtosis (3 for normal returns) of those returns, N the number of
// trials the selection ran over and V the variance of their Sharpe ratios:
//
//   SR0 = sqrt(V) * ((1 - e) * Q(1 - 1/N) + e * Q(1 - 1/(N exp(1))))
//   DSR = Phi( (SR - SR0) * sqrt(T - 1) / sqrt(1 - g3 SR + (g4 - 1)/4 SR^2) )
//
// where e is the Euler-Mascheroni constant, Q the standard normal quantile
// and Phi its CDF. SR0 is the expected maximum Sharpe ratio of N trials with
// no skill; with one trial there is no selection and SR0 is 0, which makes
// DSR the probabilistic Sharpe ratio against zero.

#include "lrvx/stats/normal.h"

#include <cmath>
#include <cstddef>
#include <limits>

namespace lrvx::stats
{

struct DeflatedSharpeResult
{
  double sharpe;             // per-period, mean over sample standard deviation
  double skewness;           // m3 / m2^1.5 over the returns
  double kurtosis;           // m4 / m2^2 over the returns, 3 for normal
  double expectedMaxSharpe;  // SR0 above
  double dsr;                // the probability; NaN when the radicand is not positive
};

// returns: the selected strategy's per-period returns, numPeriods of them.
// trialSharpes: the per-period Sharpe ratio of every trial the selection
// ran over, numTrials of them (the selected one included). Every field is
// NaN with fewer than 2 periods or a zero return variance.
inline DeflatedSharpeResult deflatedSharpeRatio(const double* returns, std::size_t numPeriods,
                                                const double* trialSharpes,
                                                std::size_t numTrials)
{
  const double nan = std::numeric_limits<double>::quiet_NaN();
  DeflatedSharpeResult out{nan, nan, nan, nan, nan};
  if (returns == nullptr || numPeriods < 2)
  {
    return out;
  }
  const double T = static_cast<double>(numPeriods);

  double mean = 0.0;
  for (std::size_t t = 0; t < numPeriods; ++t)
  {
    mean += returns[t];
  }
  mean /= T;
  double m2 = 0.0;
  double m3 = 0.0;
  double m4 = 0.0;
  for (std::size_t t = 0; t < numPeriods; ++t)
  {
    const double d = returns[t] - mean;
    const double d2 = d * d;
    m2 += d2;
    m3 += d2 * d;
    m4 += d2 * d2;
  }
  const double sampleVar = m2 / (T - 1.0);
  m2 /= T;
  m3 /= T;
  m4 /= T;
  if (!(sampleVar > 0.0) || !(m2 > 0.0))
  {
    return out;
  }
  out.sharpe = mean / std::sqrt(sampleVar);
  out.skewness = m3 / std::pow(m2, 1.5);
  out.kurtosis = m4 / (m2 * m2);

  double sr0 = 0.0;
  if (trialSharpes != nullptr && numTrials >= 2)
  {
    const double N = static_cast<double>(numTrials);
    double tm = 0.0;
    for (std::size_t k = 0; k < numTrials; ++k)
    {
      tm += trialSharpes[k];
    }
    tm /= N;
    double tv = 0.0;
    for (std::size_t k = 0; k < numTrials; ++k)
    {
      const double d = trialSharpes[k] - tm;
      tv += d * d;
    }
    tv /= (N - 1.0);
    if (tv > 0.0)
    {
      constexpr double euler = 0.57721566490153286;
      sr0 = std::sqrt(tv) * ((1.0 - euler) * normalQuantile(1.0 - 1.0 / N) +
                             euler * normalQuantile(1.0 - 1.0 / (N * std::exp(1.0))));
    }
  }
  out.expectedMaxSharpe = sr0;

  const double sr = out.sharpe;
  const double radicand = 1.0 - out.skewness * sr + (out.kurtosis - 1.0) / 4.0 * sr * sr;
  if (!(radicand > 0.0))
  {
    return out;
  }
  out.dsr = normalCdf((sr - sr0) * std::sqrt(T - 1.0) / std::sqrt(radicand));
  return out;
}

}  // namespace lrvx::stats
