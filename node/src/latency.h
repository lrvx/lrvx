// node/src/latency.h -- LatencyModel wrappers for Node.js (Phase 1).
//
// Each wrap class owns a LrvxLatencyModelHandle through the C ABI and
// exposes feedDelay / orderDelay / fillDelay / sample / reset. The
// underlying RNG and validation live in lrvx::LatencyModel; the wrap
// is purely a marshalling layer.

#pragma once

#include <napi.h>

#include "error_translator.h"
#include "lrvx/capi/lrvx_capi.h"

#include <cstdint>
#include <vector>

namespace node_lrvx
{

namespace detail
{

inline Napi::Value sampleToObject(Napi::Env env, const LrvxLatencySample& s)
{
  Napi::Object out = Napi::Object::New(env);
  out.Set("feedNs", Napi::Number::New(env, static_cast<double>(s.feed_ns)));
  out.Set("orderNs", Napi::Number::New(env, static_cast<double>(s.order_ns)));
  out.Set("fillNs", Napi::Number::New(env, static_cast<double>(s.fill_ns)));
  return out;
}

inline std::vector<int64_t> readInt64Array(const Napi::Value& v, const char* what)
{
  std::vector<int64_t> out;
  if (!given(v))
  {
    return out;
  }
  Napi::Array arr = argArray(v, what);
  out.reserve(arr.Length());
  for (uint32_t i = 0; i < arr.Length(); ++i)
  {
    out.push_back(argInteger(arr.Get(i), "a latency sample"));
  }
  return out;
}

inline int64_t getInt64(const Napi::Object& o, const char* key, int64_t fallback)
{
  if (given(o.Get(key)))
  {
    return argInteger(o.Get(key), key);
  }
  return fallback;
}

inline double getDouble(const Napi::Object& o, const char* key, double fallback)
{
  if (given(o.Get(key)))
  {
    return argNumber(o.Get(key), key);
  }
  return fallback;
}

}  // namespace detail

#define LRVX_LATENCY_INSTANCE_METHODS(Cls)                                       \
  Napi::Value FeedDelay(const Napi::CallbackInfo& info)                          \
  {                                                                              \
    return Napi::Number::New(info.Env(),                                         \
                             static_cast<double>(lrvx_latency_feed_delay(_h)));  \
  }                                                                              \
  Napi::Value OrderDelay(const Napi::CallbackInfo& info)                         \
  {                                                                              \
    return Napi::Number::New(info.Env(),                                         \
                             static_cast<double>(lrvx_latency_order_delay(_h))); \
  }                                                                              \
  Napi::Value FillDelay(const Napi::CallbackInfo& info)                          \
  {                                                                              \
    return Napi::Number::New(info.Env(),                                         \
                             static_cast<double>(lrvx_latency_fill_delay(_h)));  \
  }                                                                              \
  Napi::Value Sample(const Napi::CallbackInfo& info)                             \
  {                                                                              \
    LrvxLatencySample s{};                                                       \
    lrvx_latency_sample(_h, &s);                                                 \
    return detail::sampleToObject(info.Env(), s);                                \
  }                                                                              \
  void Reset(const Napi::CallbackInfo& info)                                     \
  {                                                                              \
    uint64_t seed = 0;                                                           \
    if (given(info[0]))                                                          \
    {                                                                            \
      seed = static_cast<uint64_t>(argInteger(info[0], "reset: seed"));          \
    }                                                                            \
    lrvx_latency_reset(_h, seed);                                                \
  }                                                                              \
  LrvxLatencyModelHandle handle() const { return _h; }                           \
  /* Cached at module init so a BacktestRunner can InstanceOf-check a model */   \
  /* before unwrapping it. */                                                    \
  static Napi::FunctionReference& Ctor(Napi::Env env)                            \
  {                                                                              \
    return ctorOf<Cls>(env);                                                     \
  }                                                                              \
  static Napi::Function InitClass(Napi::Env env, const char* name)               \
  {                                                                              \
    Napi::Function fn =                                                          \
        DefineClass(env, name,                                                   \
                    {InstanceMethod("feedDelay", &Cls::FeedDelay),               \
                     InstanceMethod("orderDelay", &Cls::OrderDelay),             \
                     InstanceMethod("fillDelay", &Cls::FillDelay),               \
                     InstanceMethod("sample", &Cls::Sample),                     \
                     InstanceMethod("reset", &Cls::Reset)});                     \
    return keepCtor(Ctor(env), fn);                                              \
  }

class ConstantLatencyWrap : public Napi::ObjectWrap<ConstantLatencyWrap>
{
 public:
  ConstantLatencyWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<ConstantLatencyWrap>(info)
  {
    int64_t feed = 0, order = 0, fill = 0;
    if (given(info[0]))
    {
      auto opts = argObject(info[0], "ConstantLatency: opts");
      feed = detail::getInt64(opts, "feedNs", 0);
      order = detail::getInt64(opts, "orderNs", 0);
      fill = detail::getInt64(opts, "fillNs", 0);
    }
    _h = lrvx_latency_constant_create(feed, order, fill);
    if (!_h)
    {
      auto err = Napi::Error::New(info.Env(),
                                  "ConstantLatency: feedNs/orderNs/fillNs must be non-negative.");
      err.Value().Set("code", Napi::String::New(info.Env(), "E_VAL_002"));
      err.Value().Set("name", Napi::String::New(info.Env(), "LrvxError"));
      throw err;
    }
  }
  ~ConstantLatencyWrap()
  {
    if (_h)
    {
      lrvx_latency_destroy(_h);
    }
  }
  LRVX_LATENCY_INSTANCE_METHODS(ConstantLatencyWrap)

 private:
  LrvxLatencyModelHandle _h{nullptr};
};

class GaussianLatencyWrap : public Napi::ObjectWrap<GaussianLatencyWrap>
{
 public:
  GaussianLatencyWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<GaussianLatencyWrap>(info)
  {
    double fm = 0, fs = 0, om = 0, os = 0, lm = 0, ls = 0;
    uint64_t seed = 0;
    if (given(info[0]))
    {
      auto opts = argObject(info[0], "GaussianLatency: opts");
      fm = detail::getDouble(opts, "feedMeanNs", 0.0);
      fs = detail::getDouble(opts, "feedStddevNs", 0.0);
      om = detail::getDouble(opts, "orderMeanNs", 0.0);
      os = detail::getDouble(opts, "orderStddevNs", 0.0);
      lm = detail::getDouble(opts, "fillMeanNs", 0.0);
      ls = detail::getDouble(opts, "fillStddevNs", 0.0);
      seed = static_cast<uint64_t>(detail::getInt64(opts, "seed", 0));
    }
    _h = lrvx_latency_gaussian_create(fm, fs, om, os, lm, ls, seed);
    if (!_h)
    {
      auto err = Napi::Error::New(info.Env(),
                                  "GaussianLatency: means and stddevs must be non-negative.");
      err.Value().Set("code", Napi::String::New(info.Env(), "E_VAL_002"));
      err.Value().Set("name", Napi::String::New(info.Env(), "LrvxError"));
      throw err;
    }
  }
  ~GaussianLatencyWrap()
  {
    if (_h)
    {
      lrvx_latency_destroy(_h);
    }
  }
  LRVX_LATENCY_INSTANCE_METHODS(GaussianLatencyWrap)

 private:
  LrvxLatencyModelHandle _h{nullptr};
};

class ExponentialLatencyWrap : public Napi::ObjectWrap<ExponentialLatencyWrap>
{
 public:
  ExponentialLatencyWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<ExponentialLatencyWrap>(info)
  {
    double fm = 0, om = 0, lm = 0;
    uint64_t seed = 0;
    if (given(info[0]))
    {
      auto opts = argObject(info[0], "ExponentialLatency: opts");
      fm = detail::getDouble(opts, "feedMeanNs", 0.0);
      om = detail::getDouble(opts, "orderMeanNs", 0.0);
      lm = detail::getDouble(opts, "fillMeanNs", 0.0);
      seed = static_cast<uint64_t>(detail::getInt64(opts, "seed", 0));
    }
    _h = lrvx_latency_exponential_create(fm, om, lm, seed);
    if (!_h)
    {
      auto err = Napi::Error::New(info.Env(),
                                  "ExponentialLatency: means must be non-negative.");
      err.Value().Set("code", Napi::String::New(info.Env(), "E_VAL_002"));
      err.Value().Set("name", Napi::String::New(info.Env(), "LrvxError"));
      throw err;
    }
  }
  ~ExponentialLatencyWrap()
  {
    if (_h)
    {
      lrvx_latency_destroy(_h);
    }
  }
  LRVX_LATENCY_INSTANCE_METHODS(ExponentialLatencyWrap)

 private:
  LrvxLatencyModelHandle _h{nullptr};
};

class EmpiricalLatencyWrap : public Napi::ObjectWrap<EmpiricalLatencyWrap>
{
 public:
  EmpiricalLatencyWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<EmpiricalLatencyWrap>(info)
  {
    std::vector<int64_t> feed, order, fill;
    uint64_t seed = 0;
    if (given(info[0]))
    {
      auto opts = argObject(info[0], "EmpiricalLatency: opts");
      if (given(opts.Get("feedSamples")))
      {
        feed = detail::readInt64Array(opts.Get("feedSamples"), "EmpiricalLatency: feedSamples");
      }
      if (given(opts.Get("orderSamples")))
      {
        order = detail::readInt64Array(opts.Get("orderSamples"), "EmpiricalLatency: orderSamples");
      }
      if (given(opts.Get("fillSamples")))
      {
        fill = detail::readInt64Array(opts.Get("fillSamples"), "EmpiricalLatency: fillSamples");
      }
      seed = static_cast<uint64_t>(detail::getInt64(opts, "seed", 0));
    }
    _h = lrvx_latency_empirical_create(
        feed.empty() ? nullptr : feed.data(), feed.size(),
        order.empty() ? nullptr : order.data(), order.size(),
        fill.empty() ? nullptr : fill.data(), fill.size(),
        seed);
    if (!_h)
    {
      auto err = Napi::Error::New(info.Env(),
                                  "EmpiricalLatency: provide at least one non-empty samples array; "
                                  "all values must be non-negative.");
      err.Value().Set("code", Napi::String::New(info.Env(), "E_VAL_002"));
      err.Value().Set("name", Napi::String::New(info.Env(), "LrvxError"));
      throw err;
    }
  }
  ~EmpiricalLatencyWrap()
  {
    if (_h)
    {
      lrvx_latency_destroy(_h);
    }
  }
  LRVX_LATENCY_INSTANCE_METHODS(EmpiricalLatencyWrap)

 private:
  LrvxLatencyModelHandle _h{nullptr};
};

#undef LRVX_LATENCY_INSTANCE_METHODS

// The C handle behind one of the latency model classes above, or nullptr when
// `v` is not one of them.
inline LrvxLatencyModelHandle latencyHandleOf(const Napi::Value& v)
{
  if (!v.IsObject())
  {
    return nullptr;
  }
  auto obj = v.As<Napi::Object>();
  if (obj.InstanceOf(ConstantLatencyWrap::Ctor(v.Env()).Value()))
  {
    return Napi::ObjectWrap<ConstantLatencyWrap>::Unwrap(obj)->handle();
  }
  if (obj.InstanceOf(GaussianLatencyWrap::Ctor(v.Env()).Value()))
  {
    return Napi::ObjectWrap<GaussianLatencyWrap>::Unwrap(obj)->handle();
  }
  if (obj.InstanceOf(ExponentialLatencyWrap::Ctor(v.Env()).Value()))
  {
    return Napi::ObjectWrap<ExponentialLatencyWrap>::Unwrap(obj)->handle();
  }
  if (obj.InstanceOf(EmpiricalLatencyWrap::Ctor(v.Env()).Value()))
  {
    return Napi::ObjectWrap<EmpiricalLatencyWrap>::Unwrap(obj)->handle();
  }
  return nullptr;
}

inline void registerLatency(Napi::Env env, Napi::Object exports)
{
  exports.Set("ConstantLatency", ConstantLatencyWrap::InitClass(env, "ConstantLatency"));
  exports.Set("GaussianLatency", GaussianLatencyWrap::InitClass(env, "GaussianLatency"));
  exports.Set("ExponentialLatency", ExponentialLatencyWrap::InitClass(env, "ExponentialLatency"));
  exports.Set("EmpiricalLatency", EmpiricalLatencyWrap::InitClass(env, "EmpiricalLatency"));
}

}  // namespace node_lrvx
