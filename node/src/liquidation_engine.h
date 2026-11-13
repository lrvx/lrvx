// node/src/liquidation_engine.h — Liquidation + insurance + ADL wrap.

#pragma once

#include <napi.h>

#include "bindings_common.h"

#include <string>

#include "account.h"
#include "backtest.h"
#include "lrvx/capi/lrvx_capi.h"

namespace node_lrvx
{

class LiquidationEngineWrap : public Napi::ObjectWrap<LiquidationEngineWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(
        env, "LiquidationEngine",
        {InstanceMethod("addTier", &LiquidationEngineWrap::AddTier),
         InstanceMethod("setInsuranceFundCapital",
                        &LiquidationEngineWrap::SetInsuranceFundCapital),
         InstanceMethod("insuranceFundBalance",
                        &LiquidationEngineWrap::InsuranceFundBalance),
         InstanceMethod("setAdlEnabled", &LiquidationEngineWrap::SetAdlEnabled),
         InstanceMethod("setAdlRanking", &LiquidationEngineWrap::SetAdlRanking),
         InstanceMethod("adlRanking", &LiquidationEngineWrap::AdlRanking),
         InstanceMethod("setLiquidationSlippageBps",
                        &LiquidationEngineWrap::SetLiquidationSlippageBps),
         InstanceMethod("openPosition", &LiquidationEngineWrap::OpenPosition),
         InstanceMethod("closePosition", &LiquidationEngineWrap::ClosePosition),
         InstanceMethod("onMark", &LiquidationEngineWrap::OnMark),
         InstanceMethod("onMarks", &LiquidationEngineWrap::OnMarks),
         InstanceMethod("liquidationsCount",
                        &LiquidationEngineWrap::LiquidationsCount),
         InstanceMethod("insurancePaymentsCount",
                        &LiquidationEngineWrap::InsurancePaymentsCount),
         InstanceMethod("adlCloseoutsCount",
                        &LiquidationEngineWrap::AdlCloseoutsCount),
         InstanceMethod("loadProfile", &LiquidationEngineWrap::LoadProfile),
         InstanceMethod("setExecutor", &LiquidationEngineWrap::SetExecutor),
         InstanceMethod("deficitsPaidByFund",
                        &LiquidationEngineWrap::DeficitsPaidByFund),
         InstanceMethod("deficitsPaidByAdl",
                        &LiquidationEngineWrap::DeficitsPaidByAdl),
         InstanceMethod("cascadeSizesPerTick",
                        &LiquidationEngineWrap::CascadeSizesPerTick),
         InstanceMethod("fundBalanceHistory",
                        &LiquidationEngineWrap::FundBalanceHistory),
         InstanceMethod("ticksToFirstAdl",
                        &LiquidationEngineWrap::TicksToFirstAdl),
         InstanceMethod("resetStats", &LiquidationEngineWrap::ResetStats),
         InstanceMethod("setMarkImpactModel",
                        &LiquidationEngineWrap::SetMarkImpactModel),
         InstanceMethod("markImpactModel",
                        &LiquidationEngineWrap::MarkImpactModel),
         InstanceMethod("markImpactWeight",
                        &LiquidationEngineWrap::MarkImpactWeight),
         InstanceMethod("setMaxCascadeDepth",
                        &LiquidationEngineWrap::SetMaxCascadeDepth),
         InstanceMethod("maxCascadeDepth",
                        &LiquidationEngineWrap::MaxCascadeDepth),
         InstanceMethod("attachAccount",
                        &LiquidationEngineWrap::AttachAccount),
         InstanceMethod("detachAccount",
                        &LiquidationEngineWrap::DetachAccount)});
  }

  LiquidationEngineWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<LiquidationEngineWrap>(info),
        _h(lrvx_liquidation_engine_create())
  {
  }
  ~LiquidationEngineWrap()
  {
    if (_h)
    {
      lrvx_liquidation_engine_destroy(_h);
    }
  }
  LrvxLiquidationEngineHandle handle() const { return _h; }

 private:
  void AddTier(const Napi::CallbackInfo& info)
  {
    lrvx_liquidation_engine_add_tier(_h, argNumber(info[0], "addTier: minNotional"),
                                     argNumber(info[1], "addTier: mmFraction"));
  }
  void SetInsuranceFundCapital(const Napi::CallbackInfo& info)
  {
    lrvx_liquidation_engine_set_insurance_fund_capital(
        _h, argNumber(info[0], "setInsuranceFundCapital: capital"));
  }
  Napi::Value InsuranceFundBalance(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(), lrvx_liquidation_engine_insurance_fund_balance(_h));
  }
  void SetAdlEnabled(const Napi::CallbackInfo& info)
  {
    lrvx_liquidation_engine_set_adl_enabled(
        _h, argBool(info[0], "setAdlEnabled: enabled") ? 1 : 0);
  }
  void SetAdlRanking(const Napi::CallbackInfo& info)
  {
    const uint8_t code = static_cast<uint8_t>(argNamedCode(
        info[0], "setAdlRanking: ranking", {"pnl_ratio", "binance", "bybit", "position_size"}));
    lrvx_liquidation_engine_set_adl_ranking(_h, code);
  }
  Napi::Value AdlRanking(const Napi::CallbackInfo& info)
  {
    const uint8_t code = lrvx_liquidation_engine_adl_ranking(_h);
    const char* name = "pnl_ratio";
    switch (code)
    {
      case 1:
        name = "binance";
        break;
      case 2:
        name = "bybit";
        break;
      case 3:
        name = "position_size";
        break;
      default:
        name = "pnl_ratio";
        break;
    }
    return Napi::String::New(info.Env(), name);
  }
  void SetLiquidationSlippageBps(const Napi::CallbackInfo& info)
  {
    lrvx_liquidation_engine_set_liquidation_slippage_bps(
        _h, argNumber(info[0], "setLiquidationSlippageBps: bps"));
  }
  void OpenPosition(const Napi::CallbackInfo& info)
  {
    lrvx_liquidation_engine_open_position(
        _h, argInteger(info[0], "openPosition: accountId"),
        argUint32(info[1], "openPosition: symbol"),
        argNumber(info[2], "openPosition: quantity"),
        argNumber(info[3], "openPosition: entryPrice"),
        argNumber(info[4], "openPosition: equity"));
  }
  void ClosePosition(const Napi::CallbackInfo& info)
  {
    lrvx_liquidation_engine_close_position(
        _h, argInteger(info[0], "closePosition: accountId"),
        argUint32(info[1], "closePosition: symbol"));
  }
  Napi::Value OnMark(const Napi::CallbackInfo& info)
  {
    const uint32_t n = lrvx_liquidation_engine_on_mark(
        _h, argUint32(info[0], "onMark: symbol"),
        argNumber(info[1], "onMark: markPrice"));
    return Napi::Number::New(info.Env(), n);
  }
  Napi::Value OnMarks(const Napi::CallbackInfo& info)
  {
    // Expects (marks: Array<[symbol, price]>, tsNs?: number).
    Napi::Array arr = argArray(info[0], "onMarks: marks");
    const uint32_t n = arr.Length();
    std::vector<uint32_t> symbols(n);
    std::vector<double> prices(n);
    for (uint32_t i = 0; i < n; ++i)
    {
      Napi::Array pair = argArray(arr.Get(i), "a [symbol, price] pair");
      symbols[i] = argUint32(pair.Get(uint32_t{0}), "a [symbol, price] pair's symbol");
      prices[i] = argNumber(pair.Get(uint32_t{1}), "a [symbol, price] pair's price");
    }
    const int64_t tsNs = given(info[1])
                             ? toInt64Ns(info[1], "onMarks: tsNs")
                             : 0;
    const uint32_t total = lrvx_liquidation_engine_on_marks(
        _h, n, symbols.data(), prices.data(), tsNs);
    return Napi::Number::New(info.Env(), total);
  }
  Napi::Value LiquidationsCount(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(),
                             static_cast<double>(lrvx_liquidation_engine_liquidations_count(_h)));
  }
  Napi::Value InsurancePaymentsCount(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(),
                             static_cast<double>(lrvx_liquidation_engine_insurance_payments_count(_h)));
  }
  Napi::Value AdlCloseoutsCount(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(),
                             static_cast<double>(lrvx_liquidation_engine_adl_closeouts_count(_h)));
  }
  void LoadProfile(const Napi::CallbackInfo& info)
  {
    const std::string name = argString(info[0], "loadProfile: name");
    uint8_t p = 0;
    if (name == "bybit_linear")
    {
      p = 1;
    }
    else if (name == "okx_swap")
    {
      p = 2;
    }
    lrvx_liquidation_engine_load_profile(_h, p);
  }
  void SetExecutor(const Napi::CallbackInfo& info)
  {
    if (info.Length() == 0 || info[0].IsNull() || info[0].IsUndefined())
    {
      lrvx_liquidation_engine_set_executor(_h, nullptr);
      return;
    }
    auto* w = argInstance<SimulatedExecutorWrap>(info[0], "setExecutor: executor");
    lrvx_liquidation_engine_set_executor(_h, w->handle());
  }
  Napi::Value DeficitsPaidByFund(const Napi::CallbackInfo& info)
  {
    const uint32_t n = lrvx_liquidation_engine_deficits_paid_by_fund_size(_h);
    std::vector<double> buf(n);
    lrvx_liquidation_engine_deficits_paid_by_fund_copy(_h, buf.data(), n);
    Napi::Array arr = Napi::Array::New(info.Env(), n);
    for (uint32_t i = 0; i < n; ++i)
    {
      arr.Set(i, Napi::Number::New(info.Env(), buf[i]));
    }
    return arr;
  }
  Napi::Value DeficitsPaidByAdl(const Napi::CallbackInfo& info)
  {
    const uint32_t n = lrvx_liquidation_engine_deficits_paid_by_adl_size(_h);
    std::vector<double> buf(n);
    lrvx_liquidation_engine_deficits_paid_by_adl_copy(_h, buf.data(), n);
    Napi::Array arr = Napi::Array::New(info.Env(), n);
    for (uint32_t i = 0; i < n; ++i)
    {
      arr.Set(i, Napi::Number::New(info.Env(), buf[i]));
    }
    return arr;
  }
  Napi::Value CascadeSizesPerTick(const Napi::CallbackInfo& info)
  {
    const uint32_t n = lrvx_liquidation_engine_cascade_sizes_size(_h);
    std::vector<uint32_t> buf(n);
    lrvx_liquidation_engine_cascade_sizes_copy(_h, buf.data(), n);
    Napi::Array arr = Napi::Array::New(info.Env(), n);
    for (uint32_t i = 0; i < n; ++i)
    {
      arr.Set(i, Napi::Number::New(info.Env(), buf[i]));
    }
    return arr;
  }
  Napi::Value FundBalanceHistory(const Napi::CallbackInfo& info)
  {
    const uint32_t n = lrvx_liquidation_engine_fund_balance_history_size(_h);
    std::vector<double> buf(n);
    lrvx_liquidation_engine_fund_balance_history_copy(_h, buf.data(), n);
    Napi::Array arr = Napi::Array::New(info.Env(), n);
    for (uint32_t i = 0; i < n; ++i)
    {
      arr.Set(i, Napi::Number::New(info.Env(), buf[i]));
    }
    return arr;
  }
  Napi::Value TicksToFirstAdl(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(
        info.Env(),
        static_cast<double>(lrvx_liquidation_engine_ticks_to_first_adl(_h)));
  }
  void ResetStats(const Napi::CallbackInfo& info)
  {
    lrvx_liquidation_engine_reset_stats(_h);
  }
  void SetMarkImpactModel(const Napi::CallbackInfo& info)
  {
    const uint8_t code = static_cast<uint8_t>(argNamedCode(
        info[0], "setMarkImpactModel: model", {"none", "book_anchored", "book_only"}));
    double weight = 0.3;
    if (given(info[1]))
    {
      weight = argNumber(info[1], "setMarkImpactModel: weight");
    }
    lrvx_liquidation_engine_set_mark_impact_model(_h, code, weight);
  }
  Napi::Value MarkImpactModel(const Napi::CallbackInfo& info)
  {
    const uint8_t code = lrvx_liquidation_engine_mark_impact_model(_h);
    const char* name = "none";
    switch (code)
    {
      case 1:
        name = "book_anchored";
        break;
      case 2:
        name = "book_only";
        break;
      default:
        name = "none";
        break;
    }
    return Napi::String::New(info.Env(), name);
  }
  Napi::Value MarkImpactWeight(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(),
                             lrvx_liquidation_engine_mark_impact_weight(_h));
  }
  void SetMaxCascadeDepth(const Napi::CallbackInfo& info)
  {
    lrvx_liquidation_engine_set_max_cascade_depth(
        _h, argUint32(info[0], "setMaxCascadeDepth: depth"));
  }
  Napi::Value MaxCascadeDepth(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(),
                             lrvx_liquidation_engine_max_cascade_depth(_h));
  }
  void AttachAccount(const Napi::CallbackInfo& info)
  {
    if (info.Length() == 0 || info[0].IsNull() || info[0].IsUndefined())
    {
      return;
    }
    auto* w = argInstance<AccountWrap>(info[0], "attachAccount: account");
    lrvx_liquidation_engine_attach_account(_h, w->handle());
  }
  void DetachAccount(const Napi::CallbackInfo& info)
  {
    lrvx_liquidation_engine_detach_account(
        _h, argInteger(info[0], "detachAccount: accountId"));
  }

  LrvxLiquidationEngineHandle _h;
};

inline void registerLiquidationEngine(Napi::Env env, Napi::Object exports)
{
  exports.Set("LiquidationEngine", LiquidationEngineWrap::Init(env));
}

}  // namespace node_lrvx
