// node/src/order_group.h — Multi-leg order group state machine wrap.
//
// Thin NAPI wrap over the C ABI lrvx_order_group_* surface. The group
// is a passive state machine — bindings record submit / fill / cancel
// events and read back state + recommended actions; no I/O.

#pragma once

#include <napi.h>

#include "bindings_common.h"

#include "lrvx/capi/lrvx_capi.h"

namespace node_lrvx
{

class OrderGroupWrap : public Napi::ObjectWrap<OrderGroupWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "OrderGroup",
                       {InstanceMethod("addMarketLeg", &OrderGroupWrap::AddMarketLeg),
                        InstanceMethod("addLimitLeg", &OrderGroupWrap::AddLimitLeg),
                        InstanceMethod("legCount", &OrderGroupWrap::LegCount),
                        InstanceMethod("legState", &OrderGroupWrap::LegState),
                        InstanceMethod("legFilled", &OrderGroupWrap::LegFilled),
                        InstanceMethod("legOrderId", &OrderGroupWrap::LegOrderId),
                        InstanceMethod("recordSubmit", &OrderGroupWrap::RecordSubmit),
                        InstanceMethod("recordFill", &OrderGroupWrap::RecordFill),
                        InstanceMethod("recordCancel", &OrderGroupWrap::RecordCancel),
                        InstanceMethod("recordFailure", &OrderGroupWrap::RecordFailure),
                        InstanceMethod("recordReplaceAccepted",
                                       &OrderGroupWrap::RecordReplaceAccepted),
                        InstanceMethod("recordReplaceRejected",
                                       &OrderGroupWrap::RecordReplaceRejected),
                        InstanceMethod("findLegByOrderId",
                                       &OrderGroupWrap::FindLegByOrderId),
                        InstanceMethod("state", &OrderGroupWrap::State),
                        InstanceMethod("recommendedActions",
                                       &OrderGroupWrap::RecommendedActions),
                        InstanceMethod("markActionDispatched",
                                       &OrderGroupWrap::MarkActionDispatched),
                        InstanceMethod("autoDispatch", &OrderGroupWrap::AutoDispatch),
                        InstanceMethod("setRiskLimits", &OrderGroupWrap::SetRiskLimits),
                        InstanceMethod("precheckSubmission",
                                       &OrderGroupWrap::PrecheckSubmission),
                        InstanceMethod("setPairLatencyBudgetNs",
                                       &OrderGroupWrap::SetPairLatencyBudgetNs),
                        InstanceMethod("pairLatencyDecision",
                                       &OrderGroupWrap::PairLatencyDecision)});
  }

  OrderGroupWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<OrderGroupWrap>(info)
  {
    uint64_t parent = 0;
    uint8_t policy = 0;
    if (given(info[0]))
    {
      auto opts = argObject(info[0], "OrderGroup: argument 1");
      if (opts.Has("parentSignalId"))
      {
        parent =
            static_cast<uint64_t>(argInteger(opts.Get("parentSignalId"), "OrderGroup: parentSignalId"));
      }
      if (opts.Has("policy"))
      {
        auto p = opts.Get("policy");
        if (p.IsString())
        {
          std::string s = p.As<Napi::String>().Utf8Value();
          if (s == "BestEffort")
          {
            policy = 0;
          }
          else if (s == "AllOrNothing")
          {
            policy = 1;
          }
          else if (s == "OneSided")
          {
            policy = 2;
          }
          else
          {
            Napi::TypeError::New(info.Env(),
                                 "Unknown OrderGroup policy: '" + s +
                                     "'. Use 'BestEffort' / 'AllOrNothing' / 'OneSided'.")
                .ThrowAsJavaScriptException();
            return;
          }
        }
        else
        {
          policy = static_cast<uint8_t>(argUint32(p, "OrderGroup: policy"));
        }
      }
    }
    _h = lrvx_order_group_create(parent, policy);
  }

  ~OrderGroupWrap()
  {
    if (_h)
    {
      lrvx_order_group_destroy(_h);
      _h = nullptr;
    }
  }

 private:
  LrvxOrderGroupHandle _h{nullptr};

  Napi::Value AddMarketLeg(const Napi::CallbackInfo& info)
  {
    uint32_t sym = argUint32(info[0], "addMarketLeg: argument 1");
    uint8_t side = static_cast<uint8_t>(argUint32(info[1], "addMarketLeg: argument 2"));
    double qty = argNumber(info[2], "addMarketLeg: argument 3");
    int64_t qty_raw = lrvx_quantity_from_double(qty);
    return Napi::Number::New(info.Env(),
                             lrvx_order_group_add_market_leg(_h, sym, side, qty_raw));
  }

  Napi::Value AddLimitLeg(const Napi::CallbackInfo& info)
  {
    uint32_t sym = argUint32(info[0], "addLimitLeg: argument 1");
    uint8_t side = static_cast<uint8_t>(argUint32(info[1], "addLimitLeg: argument 2"));
    double price = argNumber(info[2], "addLimitLeg: argument 3");
    double qty = argNumber(info[3], "addLimitLeg: argument 4");
    return Napi::Number::New(info.Env(), lrvx_order_group_add_limit_leg(
                                             _h, sym, side, lrvx_price_from_double(price),
                                             lrvx_quantity_from_double(qty)));
  }

  Napi::Value LegCount(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(), lrvx_order_group_leg_count(_h));
  }

  Napi::Value LegState(const Napi::CallbackInfo& info)
  {
    uint32_t idx = argUint32(info[0], "legState: argument 1");
    static const char* const names[] = {
        "Pending", "Submitted", "PartiallyFilled", "Filled", "Cancelled", "Failed"};
    uint8_t s = lrvx_order_group_leg_state(_h, idx);
    const char* name = (s < 6) ? names[s] : "Unknown";
    return Napi::String::New(info.Env(), name);
  }

  Napi::Value LegFilled(const Napi::CallbackInfo& info)
  {
    uint32_t idx = argUint32(info[0], "legFilled: argument 1");
    return Napi::Number::New(
        info.Env(), lrvx_quantity_to_double(lrvx_order_group_leg_filled_raw(_h, idx)));
  }

  Napi::Value LegOrderId(const Napi::CallbackInfo& info)
  {
    uint32_t idx = argUint32(info[0], "legOrderId: argument 1");
    return Napi::Number::New(
        info.Env(), static_cast<double>(lrvx_order_group_leg_order_id(_h, idx)));
  }

  void RecordSubmit(const Napi::CallbackInfo& info)
  {
    uint32_t idx = argUint32(info[0], "recordSubmit: argument 1");
    uint64_t order_id =
        static_cast<uint64_t>(argInteger(info[1], "recordSubmit: argument 2"));
    lrvx_order_group_record_submit(_h, idx, order_id);
  }

  void RecordFill(const Napi::CallbackInfo& info)
  {
    uint32_t idx = argUint32(info[0], "recordFill: argument 1");
    double cum = argNumber(info[1], "recordFill: argument 2");
    lrvx_order_group_record_fill(_h, idx, lrvx_quantity_from_double(cum));
  }

  void RecordCancel(const Napi::CallbackInfo& info)
  {
    lrvx_order_group_record_cancel(_h, argUint32(info[0], "recordCancel: argument 1"));
  }

  void RecordFailure(const Napi::CallbackInfo& info)
  {
    lrvx_order_group_record_failure(_h, argUint32(info[0], "recordFailure: argument 1"));
  }

  void RecordReplaceAccepted(const Napi::CallbackInfo& info)
  {
    uint32_t idx = argUint32(info[0], "recordReplaceAccepted: argument 1");
    uint64_t new_id = static_cast<uint64_t>(argInteger(info[1], "recordReplaceAccepted: argument 2"));
    lrvx_order_group_record_replace_accepted(_h, idx, new_id);
  }

  void RecordReplaceRejected(const Napi::CallbackInfo& info)
  {
    lrvx_order_group_record_replace_rejected(_h,
                                             argUint32(info[0], "recordReplaceRejected: argument 1"));
  }

  Napi::Value FindLegByOrderId(const Napi::CallbackInfo& info)
  {
    uint64_t order_id = static_cast<uint64_t>(argInteger(info[0], "findLegByOrderId: argument 1"));
    uint32_t idx = lrvx_order_group_find_leg_by_order_id(_h, order_id);
    if (idx == UINT32_MAX)
    {
      return info.Env().Null();
    }
    return Napi::Number::New(info.Env(), idx);
  }

  Napi::Value State(const Napi::CallbackInfo& info)
  {
    static const char* const names[] = {
        "Pending", "Submitted", "PartiallyFilled", "Filled",
        "Cancelled", "Reverting", "Failed"};
    uint8_t s = lrvx_order_group_state(_h);
    const char* name = (s < 7) ? names[s] : "Unknown";
    return Napi::String::New(info.Env(), name);
  }

  Napi::Value RecommendedActions(const Napi::CallbackInfo& info)
  {
    constexpr uint32_t kMaxActions = 32;
    int64_t buf[kMaxActions * 5];
    uint32_t n = lrvx_order_group_recommended_actions(_h, buf, kMaxActions);
    Napi::Array out = Napi::Array::New(info.Env(), n);
    for (uint32_t i = 0; i < n; ++i)
    {
      const int64_t* slot = buf + i * 5;
      Napi::Object obj = Napi::Object::New(info.Env());
      obj.Set("kind", Napi::String::New(info.Env(), slot[0] == 0 ? "cancel" : "revert"));
      obj.Set("legIndex", Napi::Number::New(info.Env(), static_cast<double>(slot[1])));
      if (slot[0] == 0)
      {
        obj.Set("orderId", Napi::Number::New(info.Env(), static_cast<double>(slot[2])));
      }
      else
      {
        obj.Set("symbol", Napi::Number::New(info.Env(), static_cast<double>(slot[2])));
        obj.Set("side", Napi::Number::New(info.Env(), static_cast<double>(slot[3])));
        obj.Set("qty", Napi::Number::New(info.Env(), lrvx_quantity_to_double(slot[4])));
      }
      out.Set(i, obj);
    }
    return out;
  }

  void MarkActionDispatched(const Napi::CallbackInfo& info)
  {
    uint32_t leg = argUint32(info[0], "markActionDispatched: argument 1");
    std::string kind = argString(info[1], "markActionDispatched: argument 2");
    uint8_t k = (kind == "cancel") ? 0 : 1;
    lrvx_order_group_mark_action_dispatched(_h, leg, k);
  }

  void SetRiskLimits(const Napi::CallbackInfo& info)
  {
    auto opts = argObject(info[0], "setRiskLimits: argument 1");
    double max_gross = opts.Has("maxGrossNotional")
                           ? argNumber(opts.Get("maxGrossNotional"), "setRiskLimits: maxGrossNotional")
                           : 0.0;
    double max_conc = opts.Has("maxConcentrationPct")
                          ? argNumber(opts.Get("maxConcentrationPct"), "setRiskLimits: maxConcentrationPct")
                          : 0.0;
    double max_leg = opts.Has("maxLegQty")
                         ? argNumber(opts.Get("maxLegQty"), "setRiskLimits: maxLegQty")
                         : 0.0;
    lrvx_order_group_set_risk_limits(_h, lrvx_quantity_from_double(max_gross), max_conc,
                                     lrvx_quantity_from_double(max_leg));
  }

  Napi::Value PrecheckSubmission(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    double equity = 0.0;
    std::vector<int64_t> ref_prices;
    if (given(info[0]))
    {
      auto opts = argObject(info[0], "precheckSubmission: argument 1");
      if (opts.Has("equity"))
      {
        equity = argNumber(opts.Get("equity"), "precheckSubmission: equity");
      }
      if (opts.Has("marketRefPrices"))
      {
        auto arr = argArray(opts.Get("marketRefPrices"), "precheckSubmission: marketRefPrices");
        ref_prices.reserve(arr.Length());
        for (uint32_t i = 0; i < arr.Length(); ++i)
        {
          ref_prices.push_back(
              lrvx_price_from_double(argNumber(arr.Get(i), "a leg price")));
        }
      }
    }
    char rule[64] = {0};
    char detail[256] = {0};
    uint8_t denied = lrvx_order_group_precheck_submission(
        _h, equity, ref_prices.empty() ? nullptr : ref_prices.data(),
        static_cast<uint32_t>(ref_prices.size()), rule, sizeof(rule), detail, sizeof(detail));
    Napi::Object out = Napi::Object::New(env);
    out.Set("denied", Napi::Boolean::New(env, denied != 0));
    out.Set("rule", Napi::String::New(env, rule));
    out.Set("detail", Napi::String::New(env, detail));
    return out;
  }

  void SetPairLatencyBudgetNs(const Napi::CallbackInfo& info)
  {
    int64_t budget = argInteger(info[0], "setPairLatencyBudgetNs: argument 1");
    lrvx_order_group_set_pair_latency_budget_ns(_h, budget);
  }

  Napi::Value PairLatencyDecision(const Napi::CallbackInfo& info)
  {
    auto opts = argObject(info[0], "pairLatencyDecision: argument 1");
    int64_t submit_ts = toInt64Ns(opts.Get("leaderSubmitTsNs"), "pairLatencyDecision: leaderSubmitTsNs");
    int64_t ack_ts = toInt64Ns(opts.Get("leaderAckTsNs"), "pairLatencyDecision: leaderAckTsNs");
    bool ack_received = opts.Has("ackReceived")
                            ? argBool(opts.Get("ackReceived"), "pairLatencyDecision: ackReceived")
                            : false;
    uint8_t d = lrvx_order_group_pair_latency_decision(_h, submit_ts, ack_ts,
                                                       ack_received ? 1 : 0);
    const char* name = (d == 0) ? "wait" : (d == 1) ? "submit_follower"
                                                    : "cancel_leader";
    return Napi::String::New(info.Env(), name);
  }

  Napi::Value AutoDispatch(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    if (info.Length() == 0 || !info[0].IsObject())
    {
      Napi::TypeError::New(env, "autoDispatch requires a strategy object")
          .ThrowAsJavaScriptException();
      return Napi::Number::New(env, 0);
    }
    auto strategy = argObject(info[0], "autoDispatch: argument 1");

    constexpr uint32_t kMaxActions = 32;
    int64_t buf[kMaxActions * 5];
    uint32_t n = lrvx_order_group_recommended_actions(_h, buf, kMaxActions);
    uint32_t fired = 0;
    for (uint32_t i = 0; i < n; ++i)
    {
      const int64_t* slot = buf + i * 5;
      uint32_t leg = static_cast<uint32_t>(slot[1]);
      if (slot[0] == 0)
      {
        // CancelLeg: strategy.emitCancel(orderId).
        Napi::Function fn =
            argFunction(strategy.Get("emitCancel"), "autoDispatch: strategy.emitCancel");
        fn.Call(strategy, {Napi::Number::New(env, static_cast<double>(slot[2]))});
        lrvx_order_group_mark_action_dispatched(_h, leg, 0);
      }
      else
      {
        // RevertLeg: opposite side market order.
        const char* method = (slot[3] == 0) ? "emitMarketBuy" : "emitMarketSell";
        Napi::Function fn = argFunction(
            strategy.Get(method), (std::string("autoDispatch: strategy.") + method).c_str());
        fn.Call(strategy, {Napi::Number::New(env, static_cast<double>(slot[2])),
                           Napi::Number::New(env, lrvx_quantity_to_double(slot[4]))});
        lrvx_order_group_mark_action_dispatched(_h, leg, 1);
      }
      ++fired;
    }
    return Napi::Number::New(env, fired);
  }
};

inline void registerOrderGroup(Napi::Env env, Napi::Object exports)
{
  exports.Set("OrderGroup", OrderGroupWrap::Init(env));

  // Named constants so users can write
  // `new OrderGroup({ policy: lrvx.OrderGroupPolicy.AllOrNothing })`.
  Napi::Object policy = Napi::Object::New(env);
  policy.Set("BestEffort", Napi::String::New(env, "BestEffort"));
  policy.Set("AllOrNothing", Napi::String::New(env, "AllOrNothing"));
  policy.Set("OneSided", Napi::String::New(env, "OneSided"));
  policy.Freeze();
  exports.Set("OrderGroupPolicy", policy);

  Napi::Object groupState = Napi::Object::New(env);
  groupState.Set("Pending", Napi::String::New(env, "Pending"));
  groupState.Set("Submitted", Napi::String::New(env, "Submitted"));
  groupState.Set("PartiallyFilled", Napi::String::New(env, "PartiallyFilled"));
  groupState.Set("Filled", Napi::String::New(env, "Filled"));
  groupState.Set("Cancelled", Napi::String::New(env, "Cancelled"));
  groupState.Set("Reverting", Napi::String::New(env, "Reverting"));
  groupState.Set("Failed", Napi::String::New(env, "Failed"));
  groupState.Freeze();
  exports.Set("OrderGroupState", groupState);
}

}  // namespace node_lrvx
