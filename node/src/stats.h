// node/src/stats.h -- statistical functions

#pragma once
#include <napi.h>
#include <cstring>
#include <vector>
#include "bindings_common.h"
#include "lrvx/capi/lrvx_capi.h"

namespace node_lrvx
{

inline std::vector<double> toVec(Napi::Float64Array& a) { return {a.Data(), a.Data() + a.ElementLength()}; }

inline Napi::Value stat_correlation(const Napi::CallbackInfo& info)
{
  auto x = info[0].As<Napi::Float64Array>();
  auto y = info[1].As<Napi::Float64Array>();
  size_t n = std::min(x.ElementLength(), y.ElementLength());
  return Napi::Number::New(info.Env(), lrvx_stat_correlation(x.Data(), y.Data(), n));
}

inline Napi::Value stat_profit_factor(const Napi::CallbackInfo& info)
{
  auto a = info[0].As<Napi::Float64Array>();
  return Napi::Number::New(info.Env(), lrvx_stat_profit_factor(a.Data(), a.ElementLength()));
}

inline Napi::Value stat_win_rate(const Napi::CallbackInfo& info)
{
  auto a = info[0].As<Napi::Float64Array>();
  return Napi::Number::New(info.Env(), lrvx_stat_win_rate(a.Data(), a.ElementLength()));
}

inline Napi::Value stat_bootstrap_ci(const Napi::CallbackInfo& info)
{
  auto a = info[0].As<Napi::Float64Array>();
  double conf = info.Length() > 1 ? info[1].As<Napi::Number>().DoubleValue() : 0.95;
  uint32_t samples = info.Length() > 2 ? info[2].As<Napi::Number>().Uint32Value() : 10000;
  double lo, med, hi;
  lrvx_stat_bootstrap_ci(a.Data(), a.ElementLength(), conf, samples, &lo, &med, &hi);
  auto o = Napi::Object::New(info.Env());
  o.Set("lower", lo);
  o.Set("median", med);
  o.Set("upper", hi);
  return o;
}

inline Napi::Value stat_permutation_test(const Napi::CallbackInfo& info)
{
  auto g1 = info[0].As<Napi::Float64Array>();
  auto g2 = info[1].As<Napi::Float64Array>();
  uint32_t n = info.Length() > 2 ? info[2].As<Napi::Number>().Uint32Value() : 10000;
  return Napi::Number::New(info.Env(), lrvx_stat_permutation_test(g1.Data(), g1.ElementLength(), g2.Data(), g2.ElementLength(), n));
}

// White's reality check.
//
// Args (positional):
//   0: returns Float64Array — flat row-major K*T matrix of EXCESS returns
//      (each strategy's series concatenated; caller is responsible for
//      benchmark adjustment).
//   1: numStrategies (number)
//   2: numPeriods (number)
//   3: numBootstrap (number, optional, default 10000)
//   4: avgBlockSize (number, optional, default 0 → auto sqrt(T))
//
// Returns: { p_value: number, best_stat: number, best_index: number }.
inline Napi::Value stat_whites_reality_check(const Napi::CallbackInfo& info)
{
  auto returns = info[0].As<Napi::Float64Array>();
  size_t K = info[1].As<Napi::Number>().Uint32Value();
  size_t T = info[2].As<Napi::Number>().Uint32Value();
  uint32_t numBootstrap = info.Length() > 3 ? info[3].As<Napi::Number>().Uint32Value() : 10000u;
  double avgBlock = info.Length() > 4 ? info[4].As<Napi::Number>().DoubleValue() : 0.0;
  if (returns.ElementLength() < K * T)
  {
    Napi::TypeError::New(info.Env(),
                         "returns array shorter than K*T")
        .ThrowAsJavaScriptException();
    return info.Env().Undefined();
  }
  double pValue = 1.0;
  double bestStat = 0.0;
  int32_t bestIndex = -1;
  lrvx_stat_whites_reality_check(returns.Data(), K, T, numBootstrap, avgBlock,
                                 &pValue, &bestStat, &bestIndex);
  auto o = Napi::Object::New(info.Env());
  o.Set("p_value", pValue);
  o.Set("best_stat", bestStat);
  o.Set("best_index", bestIndex);
  return o;
}

// pboCscv(returns: Float64Array, numTrials, numPeriods, numBlocks = 16)
// Probability of backtest overfitting by CSCV; returns is trials x periods
// row-major. Returns { pbo, splits, mean_oos_rank, mean_logit }; pbo is NaN
// and splits 0 when the inputs cannot be split.
inline Napi::Value stat_pbo_cscv(const Napi::CallbackInfo& info)
{
  auto returns = info[0].As<Napi::Float64Array>();
  size_t N = info[1].As<Napi::Number>().Uint32Value();
  size_t T = info[2].As<Napi::Number>().Uint32Value();
  uint32_t numBlocks = info.Length() > 3 ? info[3].As<Napi::Number>().Uint32Value() : 16u;
  if (returns.ElementLength() < N * T)
  {
    Napi::TypeError::New(info.Env(), "returns array shorter than numTrials*numPeriods")
        .ThrowAsJavaScriptException();
    return info.Env().Undefined();
  }
  double pbo = 0.0;
  uint64_t splits = 0;
  double meanRank = 0.0;
  double meanLogit = 0.0;
  lrvx_stat_pbo_cscv(returns.Data(), N, T, numBlocks, &pbo, &splits, &meanRank, &meanLogit);
  auto o = Napi::Object::New(info.Env());
  o.Set("pbo", pbo);
  o.Set("splits", static_cast<double>(splits));
  o.Set("mean_oos_rank", meanRank);
  o.Set("mean_logit", meanLogit);
  return o;
}

// deflatedSharpe(returns: Float64Array, trialSharpes?: Float64Array)
// Returns { sharpe, skewness, kurtosis, expected_max_sharpe, dsr }.
inline Napi::Value stat_deflated_sharpe(const Napi::CallbackInfo& info)
{
  auto returns = info[0].As<Napi::Float64Array>();
  const double* trials = nullptr;
  size_t numTrials = 0;
  if (info.Length() > 1 && !info[1].IsUndefined() && !info[1].IsNull())
  {
    auto t = info[1].As<Napi::Float64Array>();
    trials = t.Data();
    numTrials = t.ElementLength();
  }
  double sharpe = 0, skewness = 0, kurtosis = 0, expectedMax = 0, dsr = 0;
  lrvx_stat_deflated_sharpe(returns.Data(), returns.ElementLength(), trials, numTrials, &sharpe,
                            &skewness, &kurtosis, &expectedMax, &dsr);
  auto o = Napi::Object::New(info.Env());
  o.Set("sharpe", sharpe);
  o.Set("skewness", skewness);
  o.Set("kurtosis", kurtosis);
  o.Set("expected_max_sharpe", expectedMax);
  o.Set("dsr", dsr);
  return o;
}

// gridNeighborhood(values: Float64Array, shape: number[], radius = 1)
// values is row-major over shape. Returns { neighbor_mean: Float64Array,
// neighbor_count: Uint32Array }, both with one entry per cell.
inline Napi::Value stat_grid_neighborhood(const Napi::CallbackInfo& info)
{
  auto values = info[0].As<Napi::Float64Array>();
  auto shapeArr = info[1].As<Napi::Array>();
  size_t radius = info.Length() > 2 ? info[2].As<Napi::Number>().Uint32Value() : 1u;
  std::vector<size_t> shape(shapeArr.Length());
  size_t total = shape.empty() ? 0 : 1;
  for (uint32_t i = 0; i < shapeArr.Length(); ++i)
  {
    shape[i] = shapeArr.Get(i).As<Napi::Number>().Uint32Value();
    total *= shape[i];
  }
  if (values.ElementLength() < total)
  {
    Napi::TypeError::New(info.Env(), "values array shorter than the product of shape")
        .ThrowAsJavaScriptException();
    return info.Env().Undefined();
  }
  auto mean = Napi::Float64Array::New(info.Env(), total);
  auto count = Napi::Uint32Array::New(info.Env(), total);
  if (total > 0)
  {
    lrvx_stat_grid_neighborhood(values.Data(), shape.data(), shape.size(), radius, mean.Data(),
                                count.Data());
  }
  auto o = Napi::Object::New(info.Env());
  o.Set("neighbor_mean", mean);
  o.Set("neighbor_count", count);
  return o;
}

// n used to come from `sl` alone, with `ss`/`lr` indexed at that same
// length -- the same missing-length-check defect as the pybind11
// bar_returns/trade_pnl bindings (see indicator_bindings.h).
inline Napi::Value stat_bar_returns(const Napi::CallbackInfo& info)
{
  auto sl = info[0].As<Napi::Int8Array>();
  auto ss = info[1].As<Napi::Int8Array>();
  auto lr = info[2].As<Napi::Float64Array>();
  size_t n = sl.ElementLength();
  if (!requireSameLength(info.Env(), "bar_returns", {n, ss.ElementLength(), lr.ElementLength()}))
  {
    return info.Env().Undefined();
  }
  auto out = Napi::Float64Array::New(info.Env(), n);
  if (n == 0)
  {
    return out;
  }
  out[0] = 0.0;
  for (size_t i = 1; i < n; ++i)
  {
    out[i] = static_cast<double>(sl[i - 1] + ss[i - 1]) * lr[i];
  }
  return out;
}

inline Napi::Value stat_trade_pnl(const Napi::CallbackInfo& info)
{
  auto sl = info[0].As<Napi::Int8Array>();
  auto ss = info[1].As<Napi::Int8Array>();
  auto lr = info[2].As<Napi::Float64Array>();
  size_t n = sl.ElementLength();
  if (!requireSameLength(info.Env(), "trade_pnl", {n, ss.ElementLength(), lr.ElementLength()}))
  {
    return info.Env().Undefined();
  }
  std::vector<double> trades;
  double pnl = 0;
  int8_t prev = 0;
  bool inTrade = false;
  for (size_t i = 1; i < n; ++i)
  {
    int8_t pos = sl[i - 1] + ss[i - 1];
    double ret = static_cast<double>(pos) * lr[i];
    if (pos != prev)
    {
      if (inTrade)
      {
        trades.push_back(pnl);
        pnl = 0;
      }
      inTrade = pos != 0;
    }
    if (pos != 0)
    {
      pnl += ret;
    }
    prev = pos;
  }
  if (inTrade)
  {
    trades.push_back(pnl);
  }
  auto out = Napi::Float64Array::New(info.Env(), trades.size());
  std::memcpy(out.Data(), trades.data(), trades.size() * sizeof(double));
  return out;
}

inline void registerStats(Napi::Env env, Napi::Object exports)
{
  exports.Set("correlation", Napi::Function::New(env, stat_correlation));
  exports.Set("profitFactor", Napi::Function::New(env, stat_profit_factor));
  exports.Set("winRate", Napi::Function::New(env, stat_win_rate));
  exports.Set("bootstrapCI", Napi::Function::New(env, stat_bootstrap_ci));
  exports.Set("permutationTest", Napi::Function::New(env, stat_permutation_test));
  exports.Set("whitesRealityCheck", Napi::Function::New(env, stat_whites_reality_check));
  exports.Set("pboCscv", Napi::Function::New(env, stat_pbo_cscv));
  exports.Set("deflatedSharpe", Napi::Function::New(env, stat_deflated_sharpe));
  exports.Set("gridNeighborhood", Napi::Function::New(env, stat_grid_neighborhood));
  exports.Set("barReturns", Napi::Function::New(env, stat_bar_returns));
  exports.Set("tradePnl", Napi::Function::New(env, stat_trade_pnl));
}

}  // namespace node_lrvx
