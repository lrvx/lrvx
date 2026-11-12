// node/src/execution_algos.h -- Execution algorithm wrappers.
//
// Each class wraps a LrvxExecAlgoHandle through the C ABI. step()
// drives the engine state machine; pending() reads the buffered
// child orders the algo emitted; clearPending() flushes them so the
// next step's pending list contains only fresh entries.

#pragma once

#include <napi.h>

#include "bindings_common.h"
#include "error_translator.h"
#include "lrvx/capi/lrvx_capi.h"

#include <cstdint>
#include <string>
#include <vector>

namespace node_lrvx
{

namespace detail_exec
{

inline Napi::Object childToJs(Napi::Env env, const LrvxExecChildOrder& c)
{
  Napi::Object o = Napi::Object::New(env);
  o.Set("orderId", Napi::Number::New(env, static_cast<double>(c.order_id)));
  o.Set("timestampNs", Napi::BigInt::New(env, c.timestamp_ns));
  o.Set("qty", Napi::Number::New(env, c.qty));
  o.Set("price", Napi::Number::New(env, c.price));
  o.Set("type", Napi::String::New(env, c.type == 1 ? "limit" : "market"));
  return o;
}

inline uint8_t parseSide(const std::string& s)
{
  return s == "sell" ? 1 : 0;
}

inline uint8_t parseType(const std::string& s)
{
  return s == "limit" ? 1 : 0;
}

}  // namespace detail_exec

#define LRVX_EXEC_INSTANCE_METHODS(Cls)                                       \
  Napi::Value Step(const Napi::CallbackInfo& info)                            \
  {                                                                           \
    lrvx_exec_step(_h, toInt64Ns(info[0]));                                   \
    auto env = info.Env();                                                    \
    const size_t n = lrvx_exec_pending_count(_h);                             \
    Napi::Array out = Napi::Array::New(env, n);                               \
    for (size_t i = 0; i < n; ++i)                                            \
    {                                                                         \
      LrvxExecChildOrder c{};                                                 \
      if (lrvx_exec_pending_at(_h, i, &c))                                    \
      {                                                                       \
        out.Set(static_cast<uint32_t>(i), detail_exec::childToJs(env, c));    \
      }                                                                       \
    }                                                                         \
    lrvx_exec_clear_pending(_h);                                              \
    return out;                                                               \
  }                                                                           \
  void ReportFill(const Napi::CallbackInfo& info)                             \
  {                                                                           \
    lrvx_exec_report_fill(_h, info[0].As<Napi::Number>().DoubleValue());      \
  }                                                                           \
  void ObserveVolume(const Napi::CallbackInfo& info)                          \
  {                                                                           \
    lrvx_exec_observe_volume(_h, info[0].As<Napi::Number>().DoubleValue());   \
  }                                                                           \
  Napi::Value SubmittedQty(const Napi::CallbackInfo& info)                    \
  {                                                                           \
    return Napi::Number::New(info.Env(), lrvx_exec_submitted_qty(_h));        \
  }                                                                           \
  Napi::Value FilledQty(const Napi::CallbackInfo& info)                       \
  {                                                                           \
    return Napi::Number::New(info.Env(), lrvx_exec_filled_qty(_h));           \
  }                                                                           \
  Napi::Value RemainingQty(const Napi::CallbackInfo& info)                    \
  {                                                                           \
    return Napi::Number::New(info.Env(), lrvx_exec_remaining_qty(_h));        \
  }                                                                           \
  Napi::Value IsDone(const Napi::CallbackInfo& info)                          \
  {                                                                           \
    return Napi::Boolean::New(info.Env(), lrvx_exec_is_done(_h) != 0);        \
  }                                                                           \
  static Napi::Function InitClass(Napi::Env env, const char* name)            \
  {                                                                           \
    return DefineClass(env, name,                                             \
                       {InstanceMethod("step", &Cls::Step),                   \
                        InstanceMethod("reportFill", &Cls::ReportFill),       \
                        InstanceMethod("observeVolume", &Cls::ObserveVolume), \
                        InstanceMethod("submittedQty", &Cls::SubmittedQty),   \
                        InstanceMethod("filledQty", &Cls::FilledQty),         \
                        InstanceMethod("remainingQty", &Cls::RemainingQty),   \
                        InstanceMethod("isDone", &Cls::IsDone)});             \
  }

#define LRVX_EXEC_THROW_BAD_ARGS(name)                                   \
  do                                                                     \
  {                                                                      \
    auto err = Napi::Error::New(info.Env(), name ": invalid arguments"); \
    err.Value().Set("code", Napi::String::New(info.Env(), "E_VAL_002")); \
    err.Value().Set("name", Napi::String::New(info.Env(), "LrvxError")); \
    throw err;                                                           \
  } while (0)

class TWAPWrap : public Napi::ObjectWrap<TWAPWrap>
{
 public:
  TWAPWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<TWAPWrap>(info)
  {
    auto opts = info[0].As<Napi::Object>();
    double target_qty = opts.Get("targetQty").As<Napi::Number>().DoubleValue();
    uint8_t side = detail_exec::parseSide(opts.Get("side").As<Napi::String>().Utf8Value());
    uint32_t symbol =
        opts.Has("symbol") ? opts.Get("symbol").As<Napi::Number>().Uint32Value() : 0;
    uint8_t type = opts.Has("type")
                       ? detail_exec::parseType(opts.Get("type").As<Napi::String>().Utf8Value())
                       : 0;
    double limit_price =
        opts.Has("limitPrice") ? opts.Get("limitPrice").As<Napi::Number>().DoubleValue() : 0.0;
    int64_t duration_ns = opts.Get("durationNs").As<Napi::Number>().Int64Value();
    uint32_t slice_count = opts.Get("sliceCount").As<Napi::Number>().Uint32Value();
    // A clock reading, not a duration: read it the way every other ns
    // argument in the addon is read.
    int64_t start_time_ns = toInt64Ns(opts.Get("startTimeNs"));
    _h = lrvx_exec_twap_create(target_qty, side, symbol, type, limit_price,
                               duration_ns, slice_count, start_time_ns);
    if (!_h)
    {
      LRVX_EXEC_THROW_BAD_ARGS("TWAPExecutor");
    }
  }
  ~TWAPWrap()
  {
    if (_h)
    {
      lrvx_exec_destroy(_h);
    }
  }
  LRVX_EXEC_INSTANCE_METHODS(TWAPWrap)

 private:
  LrvxExecAlgoHandle _h{nullptr};
};

class IcebergWrap : public Napi::ObjectWrap<IcebergWrap>
{
 public:
  IcebergWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<IcebergWrap>(info)
  {
    auto opts = info[0].As<Napi::Object>();
    double target_qty = opts.Get("targetQty").As<Napi::Number>().DoubleValue();
    uint8_t side = detail_exec::parseSide(opts.Get("side").As<Napi::String>().Utf8Value());
    uint32_t symbol =
        opts.Has("symbol") ? opts.Get("symbol").As<Napi::Number>().Uint32Value() : 0;
    uint8_t type = opts.Has("type")
                       ? detail_exec::parseType(opts.Get("type").As<Napi::String>().Utf8Value())
                       : 0;
    double limit_price =
        opts.Has("limitPrice") ? opts.Get("limitPrice").As<Napi::Number>().DoubleValue() : 0.0;
    double visible_qty = opts.Get("visibleQty").As<Napi::Number>().DoubleValue();
    _h = lrvx_exec_iceberg_create(target_qty, side, symbol, type, limit_price, visible_qty);
    if (!_h)
    {
      LRVX_EXEC_THROW_BAD_ARGS("IcebergExecutor");
    }
  }
  ~IcebergWrap()
  {
    if (_h)
    {
      lrvx_exec_destroy(_h);
    }
  }
  LRVX_EXEC_INSTANCE_METHODS(IcebergWrap)

 private:
  LrvxExecAlgoHandle _h{nullptr};
};

class POVWrap : public Napi::ObjectWrap<POVWrap>
{
 public:
  POVWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<POVWrap>(info)
  {
    auto opts = info[0].As<Napi::Object>();
    double target_qty = opts.Get("targetQty").As<Napi::Number>().DoubleValue();
    uint8_t side = detail_exec::parseSide(opts.Get("side").As<Napi::String>().Utf8Value());
    uint32_t symbol =
        opts.Has("symbol") ? opts.Get("symbol").As<Napi::Number>().Uint32Value() : 0;
    uint8_t type = opts.Has("type")
                       ? detail_exec::parseType(opts.Get("type").As<Napi::String>().Utf8Value())
                       : 0;
    double limit_price =
        opts.Has("limitPrice") ? opts.Get("limitPrice").As<Napi::Number>().DoubleValue() : 0.0;
    double rate = opts.Get("participationRate").As<Napi::Number>().DoubleValue();
    double min_slice =
        opts.Has("minSliceQty") ? opts.Get("minSliceQty").As<Napi::Number>().DoubleValue() : 0.0;
    _h = lrvx_exec_pov_create(target_qty, side, symbol, type, limit_price, rate, min_slice);
    if (!_h)
    {
      LRVX_EXEC_THROW_BAD_ARGS("POVExecutor");
    }
  }
  ~POVWrap()
  {
    if (_h)
    {
      lrvx_exec_destroy(_h);
    }
  }
  LRVX_EXEC_INSTANCE_METHODS(POVWrap)

 private:
  LrvxExecAlgoHandle _h{nullptr};
};

class VWAPWrap : public Napi::ObjectWrap<VWAPWrap>
{
 public:
  VWAPWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<VWAPWrap>(info)
  {
    auto opts = info[0].As<Napi::Object>();
    double target_qty = opts.Get("targetQty").As<Napi::Number>().DoubleValue();
    uint8_t side = detail_exec::parseSide(opts.Get("side").As<Napi::String>().Utf8Value());
    uint32_t symbol =
        opts.Has("symbol") ? opts.Get("symbol").As<Napi::Number>().Uint32Value() : 0;
    uint8_t type = opts.Has("type")
                       ? detail_exec::parseType(opts.Get("type").As<Napi::String>().Utf8Value())
                       : 0;
    double limit_price =
        opts.Has("limitPrice") ? opts.Get("limitPrice").As<Napi::Number>().DoubleValue() : 0.0;

    Napi::Array curve = opts.Get("volumeCurve").As<Napi::Array>();
    std::vector<int64_t> ts;
    std::vector<double> vol;
    for (uint32_t i = 0; i < curve.Length(); ++i)
    {
      auto entry = curve.Get(i).As<Napi::Array>();
      ts.push_back(toInt64Ns(entry.Get(uint32_t(0))));
      vol.push_back(entry.Get(uint32_t(1)).As<Napi::Number>().DoubleValue());
    }
    _h = lrvx_exec_vwap_create(target_qty, side, symbol, type, limit_price,
                               ts.empty() ? nullptr : ts.data(),
                               vol.empty() ? nullptr : vol.data(),
                               ts.size());
    if (!_h)
    {
      LRVX_EXEC_THROW_BAD_ARGS("VWAPExecutor");
    }
  }
  ~VWAPWrap()
  {
    if (_h)
    {
      lrvx_exec_destroy(_h);
    }
  }
  LRVX_EXEC_INSTANCE_METHODS(VWAPWrap)

 private:
  LrvxExecAlgoHandle _h{nullptr};
};

#undef LRVX_EXEC_INSTANCE_METHODS
#undef LRVX_EXEC_THROW_BAD_ARGS

inline void registerExecutionAlgos(Napi::Env env, Napi::Object exports)
{
  exports.Set("TWAPExecutor", TWAPWrap::InitClass(env, "TWAPExecutor"));
  exports.Set("VWAPExecutor", VWAPWrap::InitClass(env, "VWAPExecutor"));
  exports.Set("IcebergExecutor", IcebergWrap::InitClass(env, "IcebergExecutor"));
  exports.Set("POVExecutor", POVWrap::InitClass(env, "POVExecutor"));
}

}  // namespace node_lrvx
