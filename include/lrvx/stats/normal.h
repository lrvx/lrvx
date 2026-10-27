/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2026 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

// The standard normal distribution: its CDF and its quantile, for the
// statistics that convert a z-score into a probability (the deflated Sharpe
// ratio) or a tail probability into a z-score (the expected maximum of N
// Sharpe ratios). Header-only, no dependency beyond <cmath>.

#include <cmath>
#include <limits>

namespace lrvx::stats
{

// P(Z <= x) for Z standard normal.
inline double normalCdf(double x)
{
  return 0.5 * std::erfc(-x / std::sqrt(2.0));
}

// The x with P(Z <= x) == p. Acklam's rational approximation (relative
// error below 1.2e-9 over (0, 1)) followed by one Halley step against erfc,
// which brings it to the precision of erfc itself. NaN outside [0, 1],
// -inf at 0 and +inf at 1.
inline double normalQuantile(double p)
{
  if (std::isnan(p) || p < 0.0 || p > 1.0)
  {
    return std::numeric_limits<double>::quiet_NaN();
  }
  if (p == 0.0)
  {
    return -std::numeric_limits<double>::infinity();
  }
  if (p == 1.0)
  {
    return std::numeric_limits<double>::infinity();
  }

  static constexpr double a[] = {-3.969683028665376e+01, 2.209460984245205e+02,
                                 -2.759285104469687e+02, 1.383577518672690e+02,
                                 -3.066479806614716e+01, 2.506628277459239e+00};
  static constexpr double b[] = {-5.447609879822406e+01, 1.615858368580409e+02,
                                 -1.556989798598866e+02, 6.680131188771972e+01,
                                 -1.328068155288572e+01};
  static constexpr double c[] = {-7.784894002430293e-03, -3.223964580411365e-01,
                                 -2.400758277161838e+00, -2.549732539343734e+00,
                                 4.374664141464968e+00, 2.938163982698783e+00};
  static constexpr double d[] = {7.784695709041462e-03, 3.224671290700398e-01,
                                 2.445134137142996e+00, 3.754408661907416e+00};
  constexpr double pLow = 0.02425;
  constexpr double pHigh = 1.0 - pLow;

  double x = 0.0;
  if (p < pLow)
  {
    const double q = std::sqrt(-2.0 * std::log(p));
    x = (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
        ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
  }
  else if (p <= pHigh)
  {
    const double q = p - 0.5;
    const double r = q * q;
    x = (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q /
        (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1.0);
  }
  else
  {
    const double q = std::sqrt(-2.0 * std::log(1.0 - p));
    x = -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
        ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
  }

  // One Halley step on f(x) = Phi(x) - p.
  const double e = normalCdf(x) - p;
  const double u = e * std::sqrt(2.0 * M_PI) * std::exp(x * x / 2.0);
  x = x - u / (1.0 + x * u / 2.0);
  return x;
}

}  // namespace lrvx::stats
