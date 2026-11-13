// node/src/bar_dispatch.h — bar-close dispatch recorder NAPI wrap.
//
// Cross-binding parity test fixture for the documented bar-close
// ordering rule: on tied closes, MultiTimeframeAggregator dispatches
// bars in the order their timeframes were registered. The wrap is a
// thin shim over the C ABI lrvx_bar_dispatch_recorder_* surface.

#pragma once

#include <napi.h>

#include "bindings_common.h"

#include "lrvx/capi/lrvx_capi.h"

namespace node_lrvx
{

class BarDispatchRecorderWrap : public Napi::ObjectWrap<BarDispatchRecorderWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(
        env, "BarDispatchRecorder",
        {InstanceMethod("addTimeIntervalSeconds",
                        &BarDispatchRecorderWrap::AddTimeIntervalSeconds),
         InstanceMethod("onTrade", &BarDispatchRecorderWrap::OnTrade),
         InstanceMethod("finalize", &BarDispatchRecorderWrap::Finalize),
         InstanceMethod("count", &BarDispatchRecorderWrap::Count),
         InstanceMethod("typeAt", &BarDispatchRecorderWrap::TypeAt),
         InstanceMethod("paramAt", &BarDispatchRecorderWrap::ParamAt)});
  }

  BarDispatchRecorderWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<BarDispatchRecorderWrap>(info)
  {
    _h = lrvx_bar_dispatch_recorder_create();
  }

  ~BarDispatchRecorderWrap()
  {
    if (_h)
    {
      lrvx_bar_dispatch_recorder_destroy(_h);
      _h = nullptr;
    }
  }

 private:
  Napi::Value AddTimeIntervalSeconds(const Napi::CallbackInfo& info)
  {
    uint32_t seconds = argUint32(info[0], "addTimeIntervalSeconds: seconds");
    return Napi::Number::New(info.Env(),
                             lrvx_bar_dispatch_recorder_add_time_seconds(_h, seconds));
  }

  void OnTrade(const Napi::CallbackInfo& info)
  {
    uint32_t symbol = argUint32(info[0], "onTrade: symbol");
    double price = argNumber(info[1], "onTrade: price");
    double qty = argNumber(info[2], "onTrade: qty");
    int64_t ts_ns = toInt64Ns(info[3], "onTrade: tsNs");
    lrvx_bar_dispatch_recorder_on_trade(_h, symbol, price, qty, ts_ns);
  }

  void Finalize(const Napi::CallbackInfo&) { lrvx_bar_dispatch_recorder_finalize(_h); }

  Napi::Value Count(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(), lrvx_bar_dispatch_recorder_count(_h));
  }

  Napi::Value TypeAt(const Napi::CallbackInfo& info)
  {
    uint32_t i = argUint32(info[0], "typeAt: index");
    return Napi::Number::New(info.Env(), lrvx_bar_dispatch_recorder_type_at(_h, i));
  }

  Napi::Value ParamAt(const Napi::CallbackInfo& info)
  {
    uint32_t i = argUint32(info[0], "paramAt: index");
    return Napi::Number::New(info.Env(),
                             static_cast<double>(lrvx_bar_dispatch_recorder_param_at(_h, i)));
  }

  LrvxBarDispatchRecorderHandle _h = nullptr;
};

inline void registerBarDispatchRecorder(Napi::Env env, Napi::Object exports)
{
  exports.Set("BarDispatchRecorder", BarDispatchRecorderWrap::Init(env));
}

}  // namespace node_lrvx
