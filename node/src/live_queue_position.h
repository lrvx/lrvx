// node/src/live_queue_position.h — Live queue position estimator.
//
// Thin NAPI wrap over the C ABI lrvx_live_queue_position_* surface.
// Client-side heuristic: feed it order placements + trades + book
// updates + our fills, read back estimated queue-ahead per resting
// order with a confidence score.

#pragma once

#include <napi.h>

#include <cstring>

#include "bindings_common.h"
#include "lrvx/capi/lrvx_capi.h"

namespace node_lrvx
{

class LiveQueuePositionEstimatorWrap
    : public Napi::ObjectWrap<LiveQueuePositionEstimatorWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(
        env, "LiveQueuePositionEstimator",
        {InstanceMethod("setConfidenceHalfLifeNs",
                        &LiveQueuePositionEstimatorWrap::SetConfidenceHalfLifeNs),
         InstanceMethod("setShrinkAttributionFactor",
                        &LiveQueuePositionEstimatorWrap::SetShrinkAttributionFactor),
         InstanceMethod("onOrderPlaced",
                        &LiveQueuePositionEstimatorWrap::OnOrderPlaced),
         InstanceMethod("onOrderCancelled",
                        &LiveQueuePositionEstimatorWrap::OnOrderCancelled),
         InstanceMethod("onOrderFilled",
                        &LiveQueuePositionEstimatorWrap::OnOrderFilled),
         InstanceMethod("onTrade", &LiveQueuePositionEstimatorWrap::OnTrade),
         InstanceMethod("onTradeWithFlag",
                        &LiveQueuePositionEstimatorWrap::OnTradeWithFlag),
         InstanceMethod("setHiddenOrderPolicy",
                        &LiveQueuePositionEstimatorWrap::SetHiddenOrderPolicy),
         InstanceMethod("onLevelUpdate",
                        &LiveQueuePositionEstimatorWrap::OnLevelUpdate),
         InstanceMethod("snapshot", &LiveQueuePositionEstimatorWrap::Snapshot),
         InstanceMethod("trackedOrderCount",
                        &LiveQueuePositionEstimatorWrap::TrackedOrderCount)});
  }

  LiveQueuePositionEstimatorWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<LiveQueuePositionEstimatorWrap>(info)
  {
    _h = lrvx_live_queue_position_create();
  }

  ~LiveQueuePositionEstimatorWrap()
  {
    if (_h)
    {
      lrvx_live_queue_position_destroy(_h);
      _h = nullptr;
    }
  }

 private:
  LrvxLiveQueuePositionHandle _h{nullptr};

  void SetConfidenceHalfLifeNs(const Napi::CallbackInfo& info)
  {
    lrvx_live_queue_position_set_confidence_half_life_ns(
        _h, argInteger(info[0], "setConfidenceHalfLifeNs: argument 1"));
  }

  void SetShrinkAttributionFactor(const Napi::CallbackInfo& info)
  {
    lrvx_live_queue_position_set_shrink_factor(
        _h, argNumber(info[0], "setShrinkAttributionFactor: argument 1"));
  }

  void OnOrderPlaced(const Napi::CallbackInfo& info)
  {
    uint32_t symbol = argUint32(info[0], "onOrderPlaced: argument 1");
    uint8_t side = static_cast<uint8_t>(argUint32(info[1], "onOrderPlaced: argument 2"));
    double price = argNumber(info[2], "onOrderPlaced: argument 3");
    uint64_t order_id =
        static_cast<uint64_t>(argInteger(info[3], "onOrderPlaced: argument 4"));
    double order_qty = argNumber(info[4], "onOrderPlaced: argument 5");
    double level_qty = argNumber(info[5], "onOrderPlaced: argument 6");
    int64_t ts_ns = info.Length() > 6 ? toInt64Ns(info[6], "onOrderPlaced: argument 7") : 0;
    lrvx_live_queue_position_on_order_placed(
        _h, symbol, side, lrvx_price_from_double(price), order_id,
        lrvx_quantity_from_double(order_qty), lrvx_quantity_from_double(level_qty),
        ts_ns);
  }

  void OnOrderCancelled(const Napi::CallbackInfo& info)
  {
    uint64_t order_id =
        static_cast<uint64_t>(argInteger(info[0], "onOrderCancelled: argument 1"));
    int64_t ts_ns = info.Length() > 1 ? toInt64Ns(info[1], "onOrderCancelled: argument 2") : 0;
    lrvx_live_queue_position_on_order_cancelled(_h, order_id, ts_ns);
  }

  void OnOrderFilled(const Napi::CallbackInfo& info)
  {
    uint64_t order_id =
        static_cast<uint64_t>(argInteger(info[0], "onOrderFilled: argument 1"));
    double cum = argNumber(info[1], "onOrderFilled: argument 2");
    int64_t ts_ns = info.Length() > 2 ? toInt64Ns(info[2], "onOrderFilled: argument 3") : 0;
    lrvx_live_queue_position_on_order_filled(_h, order_id,
                                             lrvx_quantity_from_double(cum), ts_ns);
  }

  void OnTrade(const Napi::CallbackInfo& info)
  {
    uint32_t symbol = argUint32(info[0], "onTrade: argument 1");
    double price = argNumber(info[1], "onTrade: argument 2");
    double qty = argNumber(info[2], "onTrade: argument 3");
    int64_t ts_ns = info.Length() > 3 ? toInt64Ns(info[3], "onTrade: argument 4") : 0;
    lrvx_live_queue_position_on_trade(_h, symbol, lrvx_price_from_double(price),
                                      lrvx_quantity_from_double(qty), ts_ns);
  }

  void OnTradeWithFlag(const Napi::CallbackInfo& info)
  {
    uint32_t symbol = argUint32(info[0], "onTradeWithFlag: argument 1");
    double price = argNumber(info[1], "onTradeWithFlag: argument 2");
    double qty = argNumber(info[2], "onTradeWithFlag: argument 3");
    int64_t ts_ns = toInt64Ns(info[3], "onTradeWithFlag: argument 4");
    bool is_hidden = argBool(info[4], "onTradeWithFlag: argument 5");
    lrvx_live_queue_position_on_trade_with_flag(
        _h, symbol, lrvx_price_from_double(price), lrvx_quantity_from_double(qty),
        ts_ns, is_hidden ? 1 : 0);
  }

  void SetHiddenOrderPolicy(const Napi::CallbackInfo& info)
  {
    std::string s = argString(info[0], "setHiddenOrderPolicy: argument 1");
    uint8_t p = 0;
    if (s == "trust_trade_flag")
    {
      p = 1;
    }
    else if (s == "infer_if_trade_exceeds_visible")
    {
      p = 2;
    }
    lrvx_live_queue_position_set_hidden_order_policy(_h, p);
  }

  void OnLevelUpdate(const Napi::CallbackInfo& info)
  {
    uint32_t symbol = argUint32(info[0], "onLevelUpdate: argument 1");
    uint8_t side = static_cast<uint8_t>(argUint32(info[1], "onLevelUpdate: argument 2"));
    double price = argNumber(info[2], "onLevelUpdate: argument 3");
    double new_qty = argNumber(info[3], "onLevelUpdate: argument 4");
    int64_t ts_ns = info.Length() > 4 ? toInt64Ns(info[4], "onLevelUpdate: argument 5") : 0;
    lrvx_live_queue_position_on_level_update(_h, symbol, side,
                                             lrvx_price_from_double(price),
                                             lrvx_quantity_from_double(new_qty), ts_ns);
  }

  Napi::Value Snapshot(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    uint64_t order_id =
        static_cast<uint64_t>(argInteger(info[0], "snapshot: argument 1"));
    int64_t now_ns = info.Length() > 1 ? toInt64Ns(info[1], "snapshot: argument 2") : 0;
    int64_t slots[6] = {0};
    uint8_t ok = lrvx_live_queue_position_snapshot(_h, order_id, now_ns, slots);
    if (!ok)
    {
      return env.Null();
    }
    double conf = 0.0;
    std::memcpy(&conf, &slots[4], sizeof(double));
    Napi::Object out = Napi::Object::New(env);
    out.Set("orderId", Napi::Number::New(env, static_cast<double>(slots[0])));
    out.Set("queueAheadEst",
            Napi::Number::New(env, lrvx_quantity_to_double(slots[1])));
    out.Set("total", Napi::Number::New(env, lrvx_quantity_to_double(slots[2])));
    // The reading the estimator was handed, given back at full width: a
    // Number steps 256 ns at present-day magnitudes, in a class whose whole
    // job is to age an estimate by a confidence half-life.
    out.Set("lastUpdateNs", Napi::BigInt::New(env, slots[3]));
    out.Set("confidence", Napi::Number::New(env, conf));
    out.Set("hiddenVolumeSeen",
            Napi::Number::New(env, lrvx_quantity_to_double(slots[5])));
    return out;
  }

  Napi::Value TrackedOrderCount(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(), lrvx_live_queue_position_tracked_count(_h));
  }
};

inline void registerLiveQueuePositionEstimator(Napi::Env env, Napi::Object exports)
{
  exports.Set("LiveQueuePositionEstimator",
              LiveQueuePositionEstimatorWrap::Init(env));
}

}  // namespace node_lrvx
