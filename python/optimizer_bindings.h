// python/optimizer_bindings.h

#pragma once

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>

#include "lrvx/error/lrvx_error.h"
#include "lrvx/stats/deflated_sharpe.h"
#include "lrvx/stats/grid_neighborhood.h"
#include "lrvx/stats/pbo_cscv.h"
#include "lrvx/stats/whites_reality_check.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <optional>
#include <random>
#include <vector>

namespace py = pybind11;

namespace
{

// Permutation test: tests whether two groups have the same mean
// Returns p-value (probability of observing the actual difference by chance)
double doPermutationTest(const double* g1, size_t n1, const double* g2, size_t n2,
                         size_t numPermutations)
{
  double sum1 = 0, sum2 = 0;
  for (size_t i = 0; i < n1; ++i)
  {
    sum1 += g1[i];
  }
  for (size_t i = 0; i < n2; ++i)
  {
    sum2 += g2[i];
  }
  double observedDiff = std::abs(sum1 / n1 - sum2 / n2);

  // Pool all values
  std::vector<double> pool(n1 + n2);
  std::copy(g1, g1 + n1, pool.begin());
  std::copy(g2, g2 + n2, pool.begin() + n1);

  std::mt19937 rng(42);
  size_t extremeCount = 0;

  for (size_t p = 0; p < numPermutations; ++p)
  {
    std::shuffle(pool.begin(), pool.end(), rng);
    double s1 = 0, s2 = 0;
    for (size_t i = 0; i < n1; ++i)
    {
      s1 += pool[i];
    }
    for (size_t i = n1; i < n1 + n2; ++i)
    {
      s2 += pool[i];
    }
    double diff = std::abs(s1 / n1 - s2 / n2);
    if (diff >= observedDiff)
    {
      ++extremeCount;
    }
  }

  return static_cast<double>(extremeCount) / numPermutations;
}

// Pearson correlation coefficient
double doCorrelation(const double* x, const double* y, size_t n)
{
  double sumX = 0, sumY = 0, sumXY = 0, sumX2 = 0, sumY2 = 0;
  for (size_t i = 0; i < n; ++i)
  {
    sumX += x[i];
    sumY += y[i];
    sumXY += x[i] * y[i];
    sumX2 += x[i] * x[i];
    sumY2 += y[i] * y[i];
  }

  double nd = static_cast<double>(n);
  double num = nd * sumXY - sumX * sumY;
  double den = std::sqrt((nd * sumX2 - sumX * sumX) * (nd * sumY2 - sumY * sumY));

  if (den == 0.0)
  {
    return 0.0;
  }
  return num / den;
}

// Bootstrap confidence interval
// Returns (lower, median, upper)
std::tuple<double, double, double> doBootstrapCI(const double* data, size_t n,
                                                 double confidence, size_t numSamples)
{
  std::mt19937 rng(42);
  std::uniform_int_distribution<size_t> dist(0, n - 1);

  std::vector<double> means(numSamples);
  for (size_t s = 0; s < numSamples; ++s)
  {
    double sum = 0;
    for (size_t i = 0; i < n; ++i)
    {
      sum += data[dist(rng)];
    }
    means[s] = sum / n;
  }

  std::sort(means.begin(), means.end());

  double alpha = (1.0 - confidence) / 2.0;
  size_t lowerIdx = static_cast<size_t>(alpha * numSamples);
  size_t upperIdx = static_cast<size_t>((1.0 - alpha) * numSamples);
  size_t medianIdx = numSamples / 2;

  if (lowerIdx >= numSamples)
  {
    lowerIdx = 0;
  }
  if (upperIdx >= numSamples)
  {
    upperIdx = numSamples - 1;
  }

  return {means[lowerIdx], means[medianIdx], means[upperIdx]};
}

}  // namespace

inline void bindOptimizer(py::module_& m)
{
  m.def(
      "permutation_test",
      [](py::array_t<double, py::array::c_style | py::array::forcecast> group1,
         py::array_t<double, py::array::c_style | py::array::forcecast> group2, size_t numPermutations)
      {
        auto* g1 = group1.data();
        auto* g2 = group2.data();
        size_t n1 = group1.size();
        size_t n2 = group2.size();

        double result;
        {
          py::gil_scoped_release release;
          result = doPermutationTest(g1, n1, g2, n2, numPermutations);
        }
        return result;
      },
      "Two-sample permutation test, returns p-value",
      py::arg("group1"), py::arg("group2"), py::arg("num_permutations") = 10000);

  m.def(
      "correlation",
      [](py::array_t<double, py::array::c_style | py::array::forcecast> x,
         py::array_t<double, py::array::c_style | py::array::forcecast> y)
      {
        if (x.size() != y.size())
        {
          throw lrvx::LrvxError(
              "E_LEN_001",
              "x and y arrays must have the same length.");
        }
        auto* xp = x.data();
        auto* yp = y.data();
        size_t n = x.size();

        double result;
        {
          py::gil_scoped_release release;
          result = doCorrelation(xp, yp, n);
        }
        return result;
      },
      "Pearson correlation coefficient",
      py::arg("x"), py::arg("y"));

  m.def(
      "bootstrap_ci",
      [](py::array_t<double, py::array::c_style | py::array::forcecast> data, double confidence, size_t numSamples)
      {
        auto* dp = data.data();
        size_t n = data.size();
        if (n == 0)
        {
          throw lrvx::LrvxError(
              "E_LEN_002",
              "Input array must not be empty.");
        }

        std::tuple<double, double, double> result;
        {
          py::gil_scoped_release release;
          result = doBootstrapCI(dp, n, confidence, numSamples);
        }
        return py::make_tuple(std::get<0>(result), std::get<1>(result), std::get<2>(result));
      },
      "Bootstrap confidence interval, returns (lower, median, upper)",
      py::arg("data"), py::arg("confidence") = 0.95, py::arg("num_samples") = 10000);

  m.def(
      "whites_reality_check",
      [](py::array_t<double, py::array::c_style | py::array::forcecast> returns,
         uint32_t numBootstrap, double avgBlockSize, uint64_t seed)
      {
        if (returns.ndim() != 2)
        {
          throw lrvx::LrvxError(
              "E_LEN_003",
              "returns must be a 2D array (strategies x periods).");
        }
        const auto K = static_cast<size_t>(returns.shape(0));
        const auto T = static_cast<size_t>(returns.shape(1));
        if (K == 0 || T == 0)
        {
          throw lrvx::LrvxError(
              "E_LEN_002",
              "returns matrix must not be empty.");
        }
        const double* data = returns.data();

        lrvx::stats::WhitesRealityCheckResult res{};
        {
          py::gil_scoped_release release;
          res = lrvx::stats::whitesRealityCheck(
              data, K, T, numBootstrap, avgBlockSize, seed);
        }
        py::dict out;
        out["p_value"] = res.pValue;
        out["best_stat"] = res.bestStat;
        out["best_index"] = res.bestIndex;
        return out;
      },
      "White's reality check (Stationary Bootstrap). Tests whether the\n"
      "best-performing strategy among K candidates is significantly better\n"
      "than zero after multiple-comparison correction. The caller passes\n"
      "EXCESS returns (relative to a benchmark) shaped (K, T). Returns\n"
      "{p_value, best_stat, best_index}.",
      py::arg("returns"), py::arg("num_bootstrap") = 10000u,
      py::arg("avg_block_size") = 0.0, py::arg("seed") = 42u);

  m.def(
      "pbo_cscv",
      [](py::array_t<double, py::array::c_style | py::array::forcecast> returns,
         uint32_t numBlocks)
      {
        if (returns.ndim() != 2)
        {
          throw lrvx::LrvxError("E_LEN_003",
                                "returns must be a 2D array (trials x periods).");
        }
        const auto N = static_cast<size_t>(returns.shape(0));
        const auto T = static_cast<size_t>(returns.shape(1));
        if (N == 0 || T == 0)
        {
          throw lrvx::LrvxError("E_LEN_002", "returns matrix must not be empty.");
        }
        lrvx::stats::PboResult res{};
        {
          py::gil_scoped_release release;
          res = lrvx::stats::pboCscv(returns.data(), N, T, numBlocks);
        }
        py::dict out;
        out["pbo"] = res.pbo;
        out["splits"] = res.splits;
        out["mean_oos_rank"] = res.meanOosRank;
        out["mean_logit"] = res.meanLogit;
        return out;
      },
      "Probability of backtest overfitting by combinatorially symmetric "
      "cross-validation. `returns` is a (trials, periods) array of per-period "
      "returns, one row per parameter combination; `num_blocks` is the even "
      "number of period blocks the C(S, S/2) splits are drawn from. Returns "
      "pbo (the fraction of splits whose in-sample best ranks below the "
      "out-of-sample median; NaN when the inputs cannot be split), splits, "
      "mean_oos_rank and mean_logit. Deterministic.",
      py::arg("returns"), py::arg("num_blocks") = 16);

  m.def(
      "deflated_sharpe",
      [](py::array_t<double, py::array::c_style | py::array::forcecast> returns,
         std::optional<py::array_t<double, py::array::c_style | py::array::forcecast>>
             trialSharpes)
      {
        if (returns.ndim() != 1)
        {
          throw lrvx::LrvxError("E_LEN_003",
                                "returns must be a 1D array of per-period returns.");
        }
        const auto T = static_cast<size_t>(returns.shape(0));
        if (T == 0)
        {
          throw lrvx::LrvxError("E_LEN_002", "returns must not be empty.");
        }
        const double* trials = nullptr;
        size_t numTrials = 0;
        if (trialSharpes)
        {
          if (trialSharpes->ndim() != 1)
          {
            throw lrvx::LrvxError("E_LEN_003",
                                  "trial_sharpes must be a 1D array, one Sharpe ratio per trial.");
          }
          trials = trialSharpes->data();
          numTrials = static_cast<size_t>(trialSharpes->shape(0));
        }
        lrvx::stats::DeflatedSharpeResult res{};
        {
          py::gil_scoped_release release;
          res = lrvx::stats::deflatedSharpeRatio(returns.data(), T, trials, numTrials);
        }
        py::dict out;
        out["sharpe"] = res.sharpe;
        out["skewness"] = res.skewness;
        out["kurtosis"] = res.kurtosis;
        out["expected_max_sharpe"] = res.expectedMaxSharpe;
        out["dsr"] = res.dsr;
        return out;
      },
      "Deflated Sharpe ratio. `returns` is the selected strategy's per-period "
      "return series; `trial_sharpes` holds the per-period Sharpe ratio of "
      "every trial the selection ran over (None or fewer than two means no "
      "selection, so the expected maximum is 0 and dsr is the probabilistic "
      "Sharpe ratio against zero). Returns sharpe, skewness, kurtosis (3 for "
      "normal returns), expected_max_sharpe and dsr, the probability that the "
      "true Sharpe ratio exceeds the expected maximum of that many unskilled "
      "trials. NaN with fewer than two periods or a zero return variance.",
      py::arg("returns"), py::arg("trial_sharpes") = py::none());

  m.def(
      "grid_neighborhood",
      [](py::array_t<double, py::array::c_style | py::array::forcecast> values, size_t radius)
      {
        const auto ndim = static_cast<size_t>(values.ndim());
        if (ndim == 0)
        {
          throw lrvx::LrvxError("E_LEN_003",
                                "values must be an array with at least one axis.");
        }
        std::vector<size_t> shape(ndim);
        std::vector<py::ssize_t> pyShape(ndim);
        size_t total = 1;
        for (size_t i = 0; i < ndim; ++i)
        {
          shape[i] = static_cast<size_t>(values.shape(static_cast<py::ssize_t>(i)));
          pyShape[i] = values.shape(static_cast<py::ssize_t>(i));
          total *= shape[i];
        }
        py::array_t<double> mean(pyShape);
        py::array_t<uint32_t> count(pyShape);
        if (total > 0)
        {
          py::gil_scoped_release release;
          lrvx::stats::gridNeighborhood(values.data(), shape.data(), ndim, radius,
                                        mean.mutable_data(), count.mutable_data());
        }
        py::dict out;
        out["neighbor_mean"] = mean;
        out["neighbor_count"] = count;
        return out;
      },
      "The neighbourhood of every point of a parameter grid. `values` is an "
      "n-dimensional array of one metric per grid point; a cell's neighbours "
      "are the cells whose index differs by at most `radius` on every axis, "
      "the cell itself excluded, NaN values skipped. Returns neighbor_mean "
      "(the mean of the neighbours' values, NaN when there are none) and "
      "neighbor_count, both shaped like `values`.",
      py::arg("values"), py::arg("radius") = 1);
}
