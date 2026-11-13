// node/src/fee_schedule.h — Tiered maker/taker fee ladder.

#pragma once

#include <napi.h>

#include <vector>

#include "account.h"
#include "bindings_common.h"
#include "lrvx/capi/lrvx_capi.h"

namespace node_lrvx
{

class FeeScheduleWrap : public Napi::ObjectWrap<FeeScheduleWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(
        env, "FeeSchedule",
        {InstanceMethod("addTier", &FeeScheduleWrap::AddTier),
         InstanceMethod("loadProfile", &FeeScheduleWrap::LoadProfile),
         InstanceMethod("recordFill", &FeeScheduleWrap::RecordFill),
         InstanceMethod("feeFor", &FeeScheduleWrap::FeeFor),
         InstanceMethod("currentTierIndex", &FeeScheduleWrap::CurrentTier),
         InstanceMethod("rollingNotional30d", &FeeScheduleWrap::RollingNotional),
         InstanceMethod("tierTransitions", &FeeScheduleWrap::TierTransitions),
         InstanceMethod("resetRolling", &FeeScheduleWrap::ResetRolling),
         InstanceMethod("bindAccount", &FeeScheduleWrap::BindAccount),
         InstanceMethod("clearAccountBinding", &FeeScheduleWrap::ClearAccountBinding)});
  }

  FeeScheduleWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<FeeScheduleWrap>(info), _h(lrvx_fee_schedule_create())
  {
  }
  ~FeeScheduleWrap()
  {
    if (_h)
    {
      lrvx_fee_schedule_destroy(_h);
    }
  }
  LrvxFeeScheduleHandle handle() const { return _h; }

 private:
  void AddTier(const Napi::CallbackInfo& info)
  {
    lrvx_fee_schedule_add_tier(_h, argNumber(info[0], "addTier: minNotional30d"),
                               argNumber(info[1], "addTier: makerBps"),
                               argNumber(info[2], "addTier: takerBps"));
  }
  void LoadProfile(const Napi::CallbackInfo& info)
  {
    std::string name = argString(info[0], "loadProfile: name");
    // A typo used to be a silent no-op, leaving default fee schedule
    // values in place -- a zero-fee or unthrottled backtest that
    // looks perfectly healthy.
    if (lrvx_fee_schedule_load_profile(_h, name.c_str()) == 0)
    {
      Napi::Error::New(info.Env(), "unknown fee schedule profile: " + name +
                                       ". known: binance_um_futures, bybit_linear, okx_swap, deribit")
          .ThrowAsJavaScriptException();
    }
  }
  void RecordFill(const Napi::CallbackInfo& info)
  {
    lrvx_fee_schedule_record_fill(_h, toInt64Ns(info[0], "recordFill: tsNs"),
                                  argNumber(info[1], "recordFill: notional"));
  }
  Napi::Value FeeFor(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(
        info.Env(), lrvx_fee_schedule_fee_for(
                        _h, toInt64Ns(info[0], "feeFor: tsNs"),
                        argNumber(info[1], "feeFor: notional"),
                        argBool(info[2], "feeFor: isMaker") ? 1 : 0));
  }
  Napi::Value CurrentTier(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(), lrvx_fee_schedule_current_tier(_h));
  }
  Napi::Value RollingNotional(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(), lrvx_fee_schedule_rolling_notional(_h));
  }
  Napi::Value TierTransitions(const Napi::CallbackInfo& info)
  {
    uint32_t n = lrvx_fee_schedule_tier_transitions(_h, nullptr, 0);
    std::vector<int64_t> buf(n, 0);
    lrvx_fee_schedule_tier_transitions(_h, buf.data(), n);
    Napi::Array arr = Napi::Array::New(info.Env(), n);
    for (uint32_t i = 0; i < n; ++i)
    {
      arr.Set(i, Napi::BigInt::New(info.Env(), buf[i]));
    }
    return arr;
  }
  void ResetRolling(const Napi::CallbackInfo&) { lrvx_fee_schedule_reset_rolling(_h); }
  void BindAccount(const Napi::CallbackInfo& info)
  {
    if (info.Length() == 0 || info[0].IsNull() || info[0].IsUndefined())
    {
      lrvx_fee_schedule_bind_account(_h, nullptr);
      return;
    }
    auto* w = argInstance<AccountWrap>(info[0], "bindAccount: account");
    lrvx_fee_schedule_bind_account(_h, w->handle());
  }
  void ClearAccountBinding(const Napi::CallbackInfo&)
  {
    lrvx_fee_schedule_clear_account_binding(_h);
  }

  LrvxFeeScheduleHandle _h;
};

inline void registerFeeSchedule(Napi::Env env, Napi::Object exports)
{
  exports.Set("FeeSchedule", FeeScheduleWrap::Init(env));
}

}  // namespace node_lrvx
