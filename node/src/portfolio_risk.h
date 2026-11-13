// node/src/portfolio_risk.h -- Portfolio risk aggregator wrap.
//
// Wraps a LrvxPortfolioRiskHandle through the C ABI. The constructor
// takes a rules object plus optional initialEquity. update() takes
// (name, fields) where fields is a partial object — present keys
// drive the C ABI field_mask. Returns a JS object matching the
// Python snapshot dict shape.

#pragma once

#include <napi.h>

#include "error_translator.h"
#include "lrvx/capi/lrvx_capi.h"

#include <cstdint>
#include <string>
#include <vector>

namespace node_lrvx
{

namespace detail_pr
{

inline void readOptDouble(const Napi::Object& opts, const char* key,
                          uint8_t& has, double& val)
{
  if (opts.Has(key) && !opts.Get(key).IsNull() && !opts.Get(key).IsUndefined())
  {
    has = 1;
    val = argNumber(opts.Get(key), key);
  }
  else
  {
    has = 0;
    val = 0.0;
  }
}

inline Napi::Object breachToJs(Napi::Env env, const LrvxBreach& b)
{
  Napi::Object o = Napi::Object::New(env);
  o.Set("rule", Napi::String::New(env, b.rule ? b.rule : ""));
  o.Set("value", Napi::Number::New(env, b.value));
  o.Set("limit", Napi::Number::New(env, b.limit));
  o.Set("detail", Napi::String::New(env, b.detail ? b.detail : ""));
  return o;
}

}  // namespace detail_pr

class PortfolioRiskWrap : public Napi::ObjectWrap<PortfolioRiskWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "PortfolioRiskAggregator",
                       {InstanceMethod("update", &PortfolioRiskWrap::Update),
                        InstanceMethod("remove", &PortfolioRiskWrap::Remove),
                        InstanceMethod("resetKillSwitch", &PortfolioRiskWrap::ResetKillSwitch),
                        InstanceMethod("checkOrder", &PortfolioRiskWrap::CheckOrder),
                        InstanceMethod("snapshot", &PortfolioRiskWrap::Snapshot),
                        InstanceMethod("totalDailyPnl", &PortfolioRiskWrap::TotalDailyPnl),
                        InstanceMethod("totalGrossExposure", &PortfolioRiskWrap::TotalGrossExposure),
                        InstanceMethod("currentEquity", &PortfolioRiskWrap::CurrentEquity),
                        InstanceMethod("drawdownPct", &PortfolioRiskWrap::DrawdownPct),
                        InstanceMethod("killSwitchActive", &PortfolioRiskWrap::KillSwitchActive)});
  }

  PortfolioRiskWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<PortfolioRiskWrap>(info)
  {
    LrvxPortfolioRiskRules rules{};
    double initial_equity = 0.0;
    if (given(info[0]))
    {
      auto opts = argObject(info[0], "PortfolioRiskAggregator: opts");
      if (given(opts.Get("rules")))
      {
        auto r = argObject(opts.Get("rules"), "PortfolioRisk: rules");
        detail_pr::readOptDouble(r, "maxDrawdownPct",
                                 rules.has_max_drawdown_pct, rules.max_drawdown_pct);
        detail_pr::readOptDouble(r, "maxDailyLoss",
                                 rules.has_max_daily_loss, rules.max_daily_loss);
        detail_pr::readOptDouble(r, "maxGrossExposure",
                                 rules.has_max_gross_exposure, rules.max_gross_exposure);
        detail_pr::readOptDouble(r, "maxConcentrationPct",
                                 rules.has_max_concentration_pct, rules.max_concentration_pct);
      }
      if (given(opts.Get("initialEquity")))
      {
        initial_equity = argNumber(opts.Get("initialEquity"), "PortfolioRisk: initialEquity");
      }
    }
    _h = lrvx_portfolio_risk_create(&rules, initial_equity);
    if (!_h)
    {
      auto err = Napi::Error::New(
          info.Env(),
          "PortfolioRiskAggregator: failed to construct. A maxDrawdownPct rule "
          "needs a positive initialEquity to measure against.");
      err.Value().Set("code", Napi::String::New(info.Env(), "E_VAL_002"));
      err.Value().Set("name", Napi::String::New(info.Env(), "LrvxError"));
      throw err;
    }
  }
  ~PortfolioRiskWrap()
  {
    if (_h)
    {
      lrvx_portfolio_risk_destroy(_h);
    }
  }

 private:
  void Update(const Napi::CallbackInfo& info)
  {
    if (info.Length() < 2 || !info[0].IsString() || !info[1].IsObject())
    {
      throw Napi::TypeError::New(info.Env(), "update(name, fields) expected");
    }
    std::string name = argString(info[0], "update: name");
    auto fields = argObject(info[1], "update: fields");
    LrvxStrategyAccountFields f{};
    uint8_t mask = 0;
    auto putDouble = [&](const char* js_key, uint8_t bit, double& out)
    {
      if (given(fields.Get(js_key)))
      {
        out = argNumber(fields.Get(js_key), js_key);
        mask |= bit;
      }
    };
    auto putUint64 = [&](const char* js_key, uint8_t bit, uint64_t& out)
    {
      if (given(fields.Get(js_key)))
      {
        out = argUint64(fields.Get(js_key), js_key);
        mask |= bit;
      }
    };
    putDouble("realizedPnl", 1u << 0, f.realized_pnl);
    putDouble("unrealizedPnl", 1u << 1, f.unrealized_pnl);
    putDouble("fees", 1u << 2, f.fees);
    putDouble("grossExposure", 1u << 3, f.gross_exposure);
    putDouble("netExposure", 1u << 4, f.net_exposure);
    putUint64("tradeCount", 1u << 5, f.trade_count);
    lrvx_portfolio_risk_update(_h, name.c_str(), &f, mask);
  }

  void Remove(const Napi::CallbackInfo& info)
  {
    std::string name = argString(info[0], "remove: name");
    lrvx_portfolio_risk_remove(_h, name.c_str());
  }

  void ResetKillSwitch(const Napi::CallbackInfo&)
  {
    lrvx_portfolio_risk_reset_kill_switch(_h);
  }

  Napi::Value CheckOrder(const Napi::CallbackInfo& info)
  {
    std::string strategy = argString(info[0], "checkOrder: strategy");
    double notional = argNumber(info[1], "checkOrder: notional");
    std::string side = argString(info[2], "checkOrder: side");
    LrvxBreach b{};
    if (!lrvx_portfolio_risk_check_order(_h, strategy.c_str(), notional,
                                         side.c_str(), &b))
    {
      return info.Env().Null();
    }
    return detail_pr::breachToJs(info.Env(), b);
  }

  Napi::Value Snapshot(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    Napi::Object out = Napi::Object::New(env);
    out.Set("totalDailyPnl", Napi::Number::New(env, lrvx_portfolio_risk_total_daily_pnl(_h)));
    out.Set("totalGrossExposure",
            Napi::Number::New(env, lrvx_portfolio_risk_total_gross_exposure(_h)));
    out.Set("currentEquity",
            Napi::Number::New(env, lrvx_portfolio_risk_current_equity(_h)));
    out.Set("drawdownPct",
            Napi::Number::New(env, lrvx_portfolio_risk_drawdown_pct(_h)));
    out.Set("killSwitchActive",
            Napi::Boolean::New(env, lrvx_portfolio_risk_kill_switch_active(_h) != 0));

    const uint64_t bn = lrvx_portfolio_risk_breach_count(_h);
    Napi::Array breaches = Napi::Array::New(env, static_cast<size_t>(bn));
    for (uint64_t i = 0; i < bn; ++i)
    {
      LrvxBreach b{};
      if (lrvx_portfolio_risk_breach_at(_h, i, &b))
      {
        breaches.Set(static_cast<uint32_t>(i), detail_pr::breachToJs(env, b));
      }
    }
    out.Set("breaches", breaches);
    out.Set("accountCount",
            Napi::Number::New(env, static_cast<double>(lrvx_portfolio_risk_account_count(_h))));
    return out;
  }

  Napi::Value TotalDailyPnl(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(), lrvx_portfolio_risk_total_daily_pnl(_h));
  }
  Napi::Value TotalGrossExposure(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(), lrvx_portfolio_risk_total_gross_exposure(_h));
  }
  Napi::Value CurrentEquity(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(), lrvx_portfolio_risk_current_equity(_h));
  }
  Napi::Value DrawdownPct(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(), lrvx_portfolio_risk_drawdown_pct(_h));
  }
  Napi::Value KillSwitchActive(const Napi::CallbackInfo& info)
  {
    return Napi::Boolean::New(info.Env(),
                              lrvx_portfolio_risk_kill_switch_active(_h) != 0);
  }

  LrvxPortfolioRiskHandle _h{nullptr};
};

inline void registerPortfolioRisk(Napi::Env env, Napi::Object exports)
{
  exports.Set("PortfolioRiskAggregator", PortfolioRiskWrap::Init(env));
}

}  // namespace node_lrvx
