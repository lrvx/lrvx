// node/src/targets.h -- forward-looking labels (research-only).
//
// Targets are intentionally batch-only and live under the `lrvx.targets.*`
// namespace so look-ahead is visible at the call site.

#pragma once
#include <napi.h>

#include <cstring>
#include <span>
#include <vector>

#include "lrvx/target/future_ctc_volatility.h"
#include "lrvx/target/future_linear_slope.h"
#include "lrvx/target/future_return.h"

namespace node_lrvx
{

namespace
{

inline Napi::Float64Array vecToFloat64Array(Napi::Env env, const std::vector<double>& v)
{
  auto a = Napi::Float64Array::New(env, v.size());
  std::memcpy(a.Data(), v.data(), v.size() * sizeof(double));
  return a;
}

template <typename Target>
Napi::Value computeTarget(const Napi::CallbackInfo& info, Target t)
{
  auto in = argFloat64Array(info[0], "argument 1");
  size_t n = in.ElementLength();
  std::vector<double> out(n);
  t.compute(std::span<const double>(in.Data(), n), std::span<double>(out.data(), n));
  return vecToFloat64Array(info.Env(), out);
}

inline Napi::Value batch_future_return(const Napi::CallbackInfo& info)
{
  size_t h = argUint32(info[1], "batch_future_return: argument 2");
  return computeTarget(info, lrvx::target::FutureReturn(h));
}

inline Napi::Value batch_future_ctc_volatility(const Napi::CallbackInfo& info)
{
  size_t h = argUint32(info[1], "batch_future_ctc_volatility: argument 2");
  return computeTarget(info, lrvx::target::FutureCTCVolatility(h));
}

inline Napi::Value batch_future_linear_slope(const Napi::CallbackInfo& info)
{
  size_t h = argUint32(info[1], "batch_future_linear_slope: argument 2");
  return computeTarget(info, lrvx::target::FutureLinearSlope(h));
}

}  // namespace

inline void registerTargets(Napi::Env env, Napi::Object exports)
{
  auto targets = Napi::Object::New(env);
  targets.Set("future_return", Napi::Function::New(env, batch_future_return));
  targets.Set("future_ctc_volatility", Napi::Function::New(env, batch_future_ctc_volatility));
  targets.Set("future_linear_slope", Napi::Function::New(env, batch_future_linear_slope));
  exports.Set("targets", targets);
}

}  // namespace node_lrvx
