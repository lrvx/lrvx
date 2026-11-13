// node/src/venue_stack.h — single-call venue-realistic backtest wrap.
//
// Exposes a flat proxy surface over the C ABI VenueStack. JS callers
// drive the stack through proxy methods instead of obtaining
// non-owning subsystem wraps; this keeps the existing wrap classes
// (SimulatedExecutor, Account, etc.) ownership-only without a
// repository-wide refactor.

#pragma once

#include <napi.h>

#include "bindings_common.h"

#include <string>

#include "lrvx/capi/lrvx_capi.h"

namespace node_lrvx
{

class VenueStackWrap : public Napi::ObjectWrap<VenueStackWrap>
{
 public:
  // Cached at module init. The static factories need the actual constructor,
  // and inside a StaticMethod there is no way to reach it from the callback
  // info: `info.This()` IS the constructor function, so `.Get("constructor")`
  // returns the global `Function`, and `Function(0, 1, 100000)` parses its
  // last argument as a function body -- every factory threw
  // "SyntaxError: Unexpected number".
  static Napi::FunctionReference& Ctor()
  {
    static Napi::FunctionReference ref;
    return ref;
  }

  static Napi::Function Init(Napi::Env env)
  {
    Napi::Function fn = DefineClass(
        env, "VenueStack",
        {InstanceMethod("venueName", &VenueStackWrap::VenueName),
         // Account proxies.
         InstanceMethod("accountId", &VenueStackWrap::AccountId),
         InstanceMethod("accountEquity", &VenueStackWrap::AccountEquity),
         InstanceMethod("accountSetEquity", &VenueStackWrap::AccountSetEquity),
         InstanceMethod("accountAddEquity", &VenueStackWrap::AccountAddEquity),
         InstanceMethod("accountOpenPosition",
                        &VenueStackWrap::AccountOpenPosition),
         InstanceMethod("accountClosePosition",
                        &VenueStackWrap::AccountClosePosition),
         InstanceMethod("accountPositionCount",
                        &VenueStackWrap::AccountPositionCount),
         InstanceMethod("accountSetMark", &VenueStackWrap::AccountSetMark),
         InstanceMethod("accountTotalNotional",
                        &VenueStackWrap::AccountTotalNotional),
         InstanceMethod("accountTotalUnrealisedPnl",
                        &VenueStackWrap::AccountTotalUnrealisedPnl),
         InstanceMethod("accountRollingNotional30d",
                        &VenueStackWrap::AccountRollingNotional30d),
         // Liquidation proxies.
         InstanceMethod("liquidationOnMark", &VenueStackWrap::LiquidationOnMark),
         InstanceMethod("liquidationsCount",
                        &VenueStackWrap::LiquidationsCount),
         InstanceMethod("insuranceFundBalance",
                        &VenueStackWrap::InsuranceFundBalance),
         // Fees proxies.
         InstanceMethod("feesRecordFill", &VenueStackWrap::FeesRecordFill),
         InstanceMethod("feesCurrentTierIndex",
                        &VenueStackWrap::FeesCurrentTierIndex),
         InstanceMethod("feesFor", &VenueStackWrap::FeesFor),
         // Static factories.
         StaticMethod("binanceUmFutures", &VenueStackWrap::BinanceUmFutures),
         StaticMethod("bybitLinear", &VenueStackWrap::BybitLinear),
         StaticMethod("okxSwap", &VenueStackWrap::OkxSwap),
         StaticMethod("deribit", &VenueStackWrap::Deribit),
         StaticMethod("fromVenue", &VenueStackWrap::FromVenue)});
    Ctor() = Napi::Persistent(fn);
    Ctor().SuppressDestruct();
    return fn;
  }

  VenueStackWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<VenueStackWrap>(info)
  {
    const uint8_t venueCode =
        info.Length() > 0 ? info[0].As<Napi::Number>().Uint32Value() : 0;
    const uint64_t accountId =
        info.Length() > 1 ? info[1].As<Napi::Number>().Int64Value() : 0;
    const double equity =
        info.Length() > 2 ? info[2].As<Napi::Number>().DoubleValue() : 0.0;
    _h = lrvx_venue_stack_create(venueCode, accountId, equity);
  }
  ~VenueStackWrap()
  {
    if (_h)
    {
      lrvx_venue_stack_destroy(_h);
    }
  }

 private:
  static Napi::Value buildFromVenue(const Napi::CallbackInfo& info,
                                    uint8_t venue)
  {
    Napi::Env env = info.Env();
    if (Ctor().IsEmpty())
    {
      Napi::Error::New(env, "VenueStack constructor unavailable")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    return Ctor().New({Napi::Number::New(env, venue), info[0], info[1]});
  }
  static Napi::Value BinanceUmFutures(const Napi::CallbackInfo& info)
  {
    return buildFromVenue(info, 0);
  }
  static Napi::Value BybitLinear(const Napi::CallbackInfo& info)
  {
    return buildFromVenue(info, 1);
  }
  static Napi::Value OkxSwap(const Napi::CallbackInfo& info)
  {
    return buildFromVenue(info, 2);
  }
  static Napi::Value Deribit(const Napi::CallbackInfo& info)
  {
    return buildFromVenue(info, 3);
  }
  static Napi::Value FromVenue(const Napi::CallbackInfo& info)
  {
    const std::string name =
        info[0].IsString() ? info[0].As<Napi::String>().Utf8Value() : "";
    uint8_t code = 0;
    if (name == "bybit_linear" || name == "bybit")
    {
      code = 1;
    }
    else if (name == "okx_swap" || name == "okx")
    {
      code = 2;
    }
    else if (name == "deribit")
    {
      code = 3;
    }
    else if (name == "binance_um_futures" || name == "binance")
    {
      code = 0;
    }
    else
    {
      Napi::Error::New(info.Env(), "unknown venue: " + name)
          .ThrowAsJavaScriptException();
      return info.Env().Undefined();
    }
    if (Ctor().IsEmpty())
    {
      Napi::Error::New(info.Env(), "VenueStack constructor unavailable")
          .ThrowAsJavaScriptException();
      return info.Env().Undefined();
    }
    return Ctor().New({Napi::Number::New(info.Env(), code), info[1], info[2]});
  }

  Napi::Value VenueName(const Napi::CallbackInfo& info)
  {
    const char* name = lrvx_venue_stack_venue_name(_h);
    return Napi::String::New(info.Env(), name ? name : "");
  }
  LrvxAccountHandle acct() { return lrvx_venue_stack_account(_h); }
  LrvxLiquidationEngineHandle liq() { return lrvx_venue_stack_liquidation(_h); }
  LrvxFeeScheduleHandle fees() { return lrvx_venue_stack_fees(_h); }

  Napi::Value AccountId(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(),
                             static_cast<double>(lrvx_account_id(acct())));
  }
  Napi::Value AccountEquity(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(), lrvx_account_equity(acct()));
  }
  void AccountSetEquity(const Napi::CallbackInfo& info)
  {
    lrvx_account_set_equity(acct(), info[0].As<Napi::Number>().DoubleValue());
  }
  void AccountAddEquity(const Napi::CallbackInfo& info)
  {
    lrvx_account_add_equity(acct(), info[0].As<Napi::Number>().DoubleValue());
  }
  void AccountOpenPosition(const Napi::CallbackInfo& info)
  {
    lrvx_account_open_position(acct(), info[0].As<Napi::Number>().Uint32Value(),
                               info[1].As<Napi::Number>().DoubleValue(),
                               info[2].As<Napi::Number>().DoubleValue());
  }
  void AccountClosePosition(const Napi::CallbackInfo& info)
  {
    lrvx_account_close_position(acct(),
                                info[0].As<Napi::Number>().Uint32Value());
  }
  Napi::Value AccountPositionCount(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(), lrvx_account_position_count(acct()));
  }
  void AccountSetMark(const Napi::CallbackInfo& info)
  {
    lrvx_account_set_mark(acct(), info[0].As<Napi::Number>().Uint32Value(),
                          info[1].As<Napi::Number>().DoubleValue());
  }
  Napi::Value AccountTotalNotional(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(), lrvx_account_total_notional(acct()));
  }
  Napi::Value AccountTotalUnrealisedPnl(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(),
                             lrvx_account_total_unrealised_pnl(acct()));
  }
  Napi::Value AccountRollingNotional30d(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(),
                             lrvx_account_rolling_notional_30d(acct()));
  }
  Napi::Value LiquidationOnMark(const Napi::CallbackInfo& info)
  {
    const uint32_t n = lrvx_liquidation_engine_on_mark(
        liq(), info[0].As<Napi::Number>().Uint32Value(),
        info[1].As<Napi::Number>().DoubleValue());
    return Napi::Number::New(info.Env(), n);
  }
  Napi::Value LiquidationsCount(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(
        info.Env(),
        static_cast<double>(lrvx_liquidation_engine_liquidations_count(liq())));
  }
  Napi::Value InsuranceFundBalance(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(
        info.Env(), lrvx_liquidation_engine_insurance_fund_balance(liq()));
  }
  void FeesRecordFill(const Napi::CallbackInfo& info)
  {
    lrvx_fee_schedule_record_fill(fees(), toInt64Ns(info[0]),
                                  info[1].As<Napi::Number>().DoubleValue());
  }
  Napi::Value FeesCurrentTierIndex(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(), lrvx_fee_schedule_current_tier(fees()));
  }
  Napi::Value FeesFor(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(
        info.Env(),
        lrvx_fee_schedule_fee_for(fees(),
                                  toInt64Ns(info[0]),
                                  info[1].As<Napi::Number>().DoubleValue(),
                                  info[2].As<Napi::Boolean>().Value() ? 1 : 0));
  }

  LrvxVenueStackHandle _h{nullptr};
};

inline void registerVenueStack(Napi::Env env, Napi::Object exports)
{
  exports.Set("VenueStack", VenueStackWrap::Init(env));
}

}  // namespace node_lrvx
