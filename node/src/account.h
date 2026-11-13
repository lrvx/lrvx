// node/src/account.h — Cross-margin Account state shared across the venue-stack subsystems.

#pragma once

#include <napi.h>

#include "bindings_common.h"

#include <string>

#include "lrvx/capi/lrvx_capi.h"

namespace node_lrvx
{

class AccountWrap : public Napi::ObjectWrap<AccountWrap>
{
 public:
  static Napi::FunctionReference& Ctor()
  {
    static Napi::FunctionReference ref;
    return ref;
  }

  static Napi::Function Init(Napi::Env env)
  {
    return keepCtor(Ctor(), DefineClass(
                                env, "Account",
                                {InstanceMethod("accountId", &AccountWrap::AccountId),
                                 InstanceMethod("equity", &AccountWrap::Equity),
                                 InstanceMethod("setEquity", &AccountWrap::SetEquity),
                                 InstanceMethod("addEquity", &AccountWrap::AddEquity),
                                 InstanceMethod("marginMode", &AccountWrap::MarginMode),
                                 InstanceMethod("setMarginMode", &AccountWrap::SetMarginMode),
                                 InstanceMethod("openPosition", &AccountWrap::OpenPosition),
                                 InstanceMethod("closePosition", &AccountWrap::ClosePosition),
                                 InstanceMethod("positionCount", &AccountWrap::PositionCount),
                                 InstanceMethod("setMark", &AccountWrap::SetMark),
                                 InstanceMethod("markTsFor", &AccountWrap::MarkTsFor),
                                 InstanceMethod("hasStaleMarks", &AccountWrap::HasStaleMarks),
                                 InstanceMethod("totalNotional", &AccountWrap::TotalNotional),
                                 InstanceMethod("totalUnrealisedPnl", &AccountWrap::TotalUpnl),
                                 InstanceMethod("recordFill", &AccountWrap::RecordFill),
                                 InstanceMethod("rollingNotionalBySymbol30d",
                                                &AccountWrap::RollingNotionalBySymbol),
                                 InstanceMethod("rollingNotional30d", &AccountWrap::RollingNotional),
                                 InstanceMethod("resetRolling", &AccountWrap::ResetRolling)}));
  }

  AccountWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<AccountWrap>(info)
  {
    const uint64_t id = info.Length() > 0 && info[0].IsNumber()
                            ? argInteger(info[0], "Account: argument 1")
                            : 0;
    const double eq = info.Length() > 1 && info[1].IsNumber()
                          ? argNumber(info[1], "Account: argument 2")
                          : 0.0;
    _h = lrvx_account_create(id, eq);
  }
  ~AccountWrap()
  {
    if (_h)
    {
      lrvx_account_destroy(_h);
    }
  }
  LrvxAccountHandle handle() const { return _h; }

 private:
  Napi::Value AccountId(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(),
                             static_cast<double>(lrvx_account_id(_h)));
  }
  Napi::Value Equity(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(), lrvx_account_equity(_h));
  }
  void SetEquity(const Napi::CallbackInfo& info)
  {
    lrvx_account_set_equity(_h, argNumber(info[0], "setEquity: argument 1"));
  }
  void AddEquity(const Napi::CallbackInfo& info)
  {
    lrvx_account_add_equity(_h, argNumber(info[0], "addEquity: argument 1"));
  }
  Napi::Value MarginMode(const Napi::CallbackInfo& info)
  {
    const uint8_t m = lrvx_account_margin_mode(_h);
    return Napi::String::New(info.Env(), m == 1 ? "isolated" : "cross");
  }
  void SetMarginMode(const Napi::CallbackInfo& info)
  {
    uint8_t code = 0;
    if (info.Length() >= 1 && info[0].IsString())
    {
      const std::string name = argString(info[0], "setMarginMode: argument 1");
      if (name == "isolated")
      {
        code = 1;
      }
    }
    else if (info.Length() >= 1 && info[0].IsNumber())
    {
      code = static_cast<uint8_t>(argUint32(info[0], "setMarginMode: argument 1"));
    }
    lrvx_account_set_margin_mode(_h, code);
  }
  void OpenPosition(const Napi::CallbackInfo& info)
  {
    const uint32_t sym = argUint32(info[0], "openPosition: argument 1");
    const double qty = argNumber(info[1], "openPosition: argument 2");
    const double entry = argNumber(info[2], "openPosition: argument 3");
    if (info.Length() >= 4 && info[3].IsNumber())
    {
      lrvx_account_open_position_isolated(
          _h, sym, qty, entry, argNumber(info[3], "openPosition: argument 4"));
    }
    else
    {
      lrvx_account_open_position(_h, sym, qty, entry);
    }
  }
  void ClosePosition(const Napi::CallbackInfo& info)
  {
    lrvx_account_close_position(_h, argUint32(info[0], "closePosition: argument 1"));
  }
  Napi::Value PositionCount(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(), lrvx_account_position_count(_h));
  }
  void SetMark(const Napi::CallbackInfo& info)
  {
    const uint32_t sym = argUint32(info[0], "setMark: argument 1");
    const double price = argNumber(info[1], "setMark: argument 2");
    if (info.Length() >= 3 && (info[2].IsNumber() || info[2].IsBigInt()))
    {
      lrvx_account_set_mark_at(_h, sym, price, toInt64Ns(info[2], "setMark: argument 3"));
    }
    else
    {
      lrvx_account_set_mark(_h, sym, price);
    }
  }
  Napi::Value MarkTsFor(const Napi::CallbackInfo& info)
  {
    return Napi::BigInt::New(
        info.Env(), lrvx_account_mark_ts(_h, argUint32(info[0], "markTsFor: argument 1")));
  }
  Napi::Value HasStaleMarks(const Napi::CallbackInfo& info)
  {
    return Napi::Boolean::New(
        info.Env(),
        lrvx_account_has_stale_marks(_h, toInt64Ns(info[0], "hasStaleMarks: argument 1"),
                                     argInteger(info[1], "hasStaleMarks: argument 2")) != 0);
  }
  Napi::Value TotalNotional(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(), lrvx_account_total_notional(_h));
  }
  Napi::Value TotalUpnl(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(), lrvx_account_total_unrealised_pnl(_h));
  }
  void RecordFill(const Napi::CallbackInfo& info)
  {
    const int64_t ts = toInt64Ns(info[0], "recordFill: argument 1");
    const double notional = argNumber(info[1], "recordFill: argument 2");
    if (info.Length() >= 3 && info[2].IsNumber())
    {
      lrvx_account_record_fill_ex(_h, ts, notional,
                                  argUint32(info[2], "recordFill: argument 3"));
    }
    else
    {
      lrvx_account_record_fill(_h, ts, notional);
    }
  }
  Napi::Value RollingNotionalBySymbol(const Napi::CallbackInfo& info)
  {
    const uint32_t n = lrvx_account_rolling_notional_by_symbol_size(_h);
    std::vector<uint32_t> syms(n);
    std::vector<double> totals(n);
    lrvx_account_rolling_notional_by_symbol_copy(_h, syms.data(),
                                                 totals.data(), n);
    Napi::Array arr = Napi::Array::New(info.Env(), n);
    for (uint32_t i = 0; i < n; ++i)
    {
      Napi::Object pair = Napi::Object::New(info.Env());
      pair.Set("symbol", Napi::Number::New(info.Env(), syms[i]));
      pair.Set("notional", Napi::Number::New(info.Env(), totals[i]));
      arr.Set(i, pair);
    }
    return arr;
  }
  Napi::Value RollingNotional(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(),
                             lrvx_account_rolling_notional_30d(_h));
  }
  void ResetRolling(const Napi::CallbackInfo& info)
  {
    lrvx_account_reset_rolling(_h);
  }

  LrvxAccountHandle _h{nullptr};
};

inline void registerAccount(Napi::Env env, Napi::Object exports)
{
  exports.Set("Account", AccountWrap::Init(env));
}

}  // namespace node_lrvx
