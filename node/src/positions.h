// node/src/positions.h -- PositionTracker, PositionGroupTracker, OrderTracker

#pragma once
#include <napi.h>
#include "lrvx/capi/lrvx_capi.h"

namespace node_lrvx
{

class PositionTrackerWrap : public Napi::ObjectWrap<PositionTrackerWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "PositionTracker",
                       {InstanceMethod("onFill", &PositionTrackerWrap::OnFill),
                        InstanceMethod("position", &PositionTrackerWrap::Position),
                        InstanceMethod("avgEntryPrice", &PositionTrackerWrap::AvgEntry),
                        InstanceMethod("realizedPnl", &PositionTrackerWrap::Pnl),
                        InstanceMethod("totalRealizedPnl", &PositionTrackerWrap::TotalPnl)});
  }
  PositionTrackerWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<PositionTrackerWrap>(info),
                                                        _h(lrvx_position_tracker_create(info.Length() > 0 ? info[0].As<Napi::Number>().Uint32Value() : 0)) {}
  ~PositionTrackerWrap()
  {
    if (_h)
    {
      lrvx_position_tracker_destroy(_h);
    }
  }

 private:
  void OnFill(const Napi::CallbackInfo& info)
  {
    std::string side = info[1].As<Napi::String>().Utf8Value();
    lrvx_position_tracker_on_fill(_h, info[0].As<Napi::Number>().Uint32Value(),
                                  side == "buy" ? 0 : 1, info[2].As<Napi::Number>().DoubleValue(), info[3].As<Napi::Number>().DoubleValue());
  }
  Napi::Value Position(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), lrvx_position_tracker_position(_h, info[0].As<Napi::Number>().Uint32Value())); }
  Napi::Value AvgEntry(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), lrvx_position_tracker_avg_entry(_h, info[0].As<Napi::Number>().Uint32Value())); }
  Napi::Value Pnl(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), lrvx_position_tracker_realized_pnl(_h, info[0].As<Napi::Number>().Uint32Value())); }
  Napi::Value TotalPnl(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), lrvx_position_tracker_total_pnl(_h)); }
  LrvxPositionTrackerHandle _h;
};

class PositionGroupTrackerWrap : public Napi::ObjectWrap<PositionGroupTrackerWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "PositionGroupTracker",
                       {InstanceMethod("openPosition", &PositionGroupTrackerWrap::Open),
                        InstanceMethod("closePosition", &PositionGroupTrackerWrap::Close),
                        InstanceMethod("partialClose", &PositionGroupTrackerWrap::PartialClose),
                        InstanceMethod("netPosition", &PositionGroupTrackerWrap::Net),
                        InstanceMethod("realizedPnl", &PositionGroupTrackerWrap::Pnl),
                        InstanceMethod("totalRealizedPnl", &PositionGroupTrackerWrap::TotalPnl),
                        InstanceMethod("openCount", &PositionGroupTrackerWrap::OpenCount),
                        InstanceMethod("prune", &PositionGroupTrackerWrap::Prune)});
  }
  PositionGroupTrackerWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<PositionGroupTrackerWrap>(info), _h(lrvx_position_group_create()) {}
  ~PositionGroupTrackerWrap()
  {
    if (_h)
    {
      lrvx_position_group_destroy(_h);
    }
  }

 private:
  Napi::Value Open(const Napi::CallbackInfo& info)
  {
    std::string side = info[2].As<Napi::String>().Utf8Value();
    return Napi::Number::New(info.Env(), (double)lrvx_position_group_open(_h,
                                                                          info[0].As<Napi::Number>().Int64Value(), info[1].As<Napi::Number>().Uint32Value(),
                                                                          side == "buy" ? 0 : 1, info[3].As<Napi::Number>().DoubleValue(), info[4].As<Napi::Number>().DoubleValue()));
  }
  void Close(const Napi::CallbackInfo& info) { lrvx_position_group_close(_h, info[0].As<Napi::Number>().Int64Value(), info[1].As<Napi::Number>().DoubleValue()); }
  void PartialClose(const Napi::CallbackInfo& info) { lrvx_position_group_partial_close(_h, info[0].As<Napi::Number>().Int64Value(), info[1].As<Napi::Number>().DoubleValue(), info[2].As<Napi::Number>().DoubleValue()); }
  Napi::Value Net(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), lrvx_position_group_net_position(_h, info[0].As<Napi::Number>().Uint32Value())); }
  Napi::Value Pnl(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), lrvx_position_group_realized_pnl(_h, info[0].As<Napi::Number>().Uint32Value())); }
  Napi::Value TotalPnl(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), lrvx_position_group_total_pnl(_h)); }
  Napi::Value OpenCount(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), lrvx_position_group_open_count(_h, info[0].As<Napi::Number>().Uint32Value())); }
  void Prune(const Napi::CallbackInfo&) { lrvx_position_group_prune(_h); }
  LrvxPositionGroupHandle _h;
};

class OrderTrackerWrap : public Napi::ObjectWrap<OrderTrackerWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "OrderTracker",
                       {InstanceMethod("onSubmitted", &OrderTrackerWrap::OnSubmitted),
                        InstanceMethod("onFilled", &OrderTrackerWrap::OnFilled),
                        InstanceMethod("onCanceled", &OrderTrackerWrap::OnCanceled),
                        InstanceMethod("isActive", &OrderTrackerWrap::IsActive),
                        InstanceAccessor("activeCount", &OrderTrackerWrap::ActiveCount, nullptr),
                        InstanceAccessor("totalCount", &OrderTrackerWrap::TotalCount, nullptr),
                        InstanceMethod("prune", &OrderTrackerWrap::Prune)});
  }
  OrderTrackerWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<OrderTrackerWrap>(info), _h(lrvx_order_tracker_create()) {}
  ~OrderTrackerWrap()
  {
    if (_h)
    {
      lrvx_order_tracker_destroy(_h);
    }
  }

 private:
  Napi::Value OnSubmitted(const Napi::CallbackInfo& info)
  {
    std::string side = info[2].As<Napi::String>().Utf8Value();
    return Napi::Boolean::New(info.Env(), lrvx_order_tracker_on_submitted(_h,
                                                                          info[0].As<Napi::Number>().Int64Value(), info[1].As<Napi::Number>().Uint32Value(),
                                                                          side == "buy" ? 0 : 1, info[3].As<Napi::Number>().DoubleValue(), info[4].As<Napi::Number>().DoubleValue()));
  }
  Napi::Value OnFilled(const Napi::CallbackInfo& info) { return Napi::Boolean::New(info.Env(), lrvx_order_tracker_on_filled(_h, info[0].As<Napi::Number>().Int64Value(), info[1].As<Napi::Number>().DoubleValue())); }
  Napi::Value OnCanceled(const Napi::CallbackInfo& info) { return Napi::Boolean::New(info.Env(), lrvx_order_tracker_on_canceled(_h, info[0].As<Napi::Number>().Int64Value())); }
  Napi::Value IsActive(const Napi::CallbackInfo& info) { return Napi::Boolean::New(info.Env(), lrvx_order_tracker_is_active(_h, info[0].As<Napi::Number>().Int64Value())); }
  Napi::Value ActiveCount(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), lrvx_order_tracker_active_count(_h)); }
  Napi::Value TotalCount(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), lrvx_order_tracker_total_count(_h)); }
  void Prune(const Napi::CallbackInfo&) { lrvx_order_tracker_prune(_h); }
  LrvxOrderTrackerHandle _h;
};

class OrderJourneyTracerWrap : public Napi::ObjectWrap<OrderJourneyTracerWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(
        env, "OrderJourneyTracer",
        {InstanceMethod("orderCount", &OrderJourneyTracerWrap::OrderCount),
         InstanceMethod("recordCount", &OrderJourneyTracerWrap::RecordCount),
         InstanceMethod("medianAckLatencyNs",
                        &OrderJourneyTracerWrap::MedianAckLatencyNs),
         InstanceMethod("medianTimeToFirstFillNs",
                        &OrderJourneyTracerWrap::MedianTimeToFirstFillNs),
         InstanceMethod("makerFillRatio",
                        &OrderJourneyTracerWrap::MakerFillRatio),
         InstanceMethod("cancelRaceLossRate",
                        &OrderJourneyTracerWrap::CancelRaceLossRate),
         InstanceMethod("result", &OrderJourneyTracerWrap::Result),
         InstanceMethod("journey", &OrderJourneyTracerWrap::Journey),
         InstanceMethod("clear", &OrderJourneyTracerWrap::Clear)});
  }

  OrderJourneyTracerWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<OrderJourneyTracerWrap>(info)
  {
    uint64_t maxOrders = 1'000'000;
    uint64_t maxRecordsPerOrder = 64;
    double sampleRate = 1.0;
    uint64_t sampleSalt = 0x9E3779B97F4A7C15ULL;
    if (info.Length() > 0 && info[0].IsObject())
    {
      Napi::Object cfg = info[0].As<Napi::Object>();
      if (cfg.Has("maxOrders"))
      {
        maxOrders = cfg.Get("maxOrders").As<Napi::Number>().Int64Value();
      }
      if (cfg.Has("maxRecordsPerOrder"))
      {
        maxRecordsPerOrder =
            cfg.Get("maxRecordsPerOrder").As<Napi::Number>().Int64Value();
      }
      if (cfg.Has("sampleRate"))
      {
        sampleRate = cfg.Get("sampleRate").As<Napi::Number>().DoubleValue();
      }
      if (cfg.Has("sampleSalt"))
      {
        sampleSalt = cfg.Get("sampleSalt").As<Napi::Number>().Int64Value();
      }
    }
    _h = lrvx_order_journey_tracer_create(maxOrders, maxRecordsPerOrder,
                                          sampleRate, sampleSalt);
  }
  ~OrderJourneyTracerWrap()
  {
    if (_h)
    {
      lrvx_order_journey_tracer_destroy(_h);
    }
  }

  LrvxOrderJourneyTracerHandle handle() const { return _h; }

 private:
  Napi::Value OrderCount(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(), lrvx_order_journey_tracer_order_count(_h));
  }
  Napi::Value RecordCount(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(), lrvx_order_journey_tracer_record_count(_h));
  }
  Napi::Value MedianAckLatencyNs(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(),
                             lrvx_order_journey_tracer_median_ack_latency_ns(_h));
  }
  Napi::Value MedianTimeToFirstFillNs(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(
        info.Env(), lrvx_order_journey_tracer_median_time_to_first_fill_ns(_h));
  }
  Napi::Value MakerFillRatio(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(),
                             lrvx_order_journey_tracer_maker_fill_ratio(_h));
  }
  Napi::Value CancelRaceLossRate(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(),
                             lrvx_order_journey_tracer_cancel_race_loss_rate(_h));
  }
  static Napi::Value rowToObj(Napi::Env env, const LrvxOrderTraceRow& r)
  {
    auto o = Napi::Object::New(env);
    o.Set("orderId", Napi::Number::New(env, static_cast<double>(r.order_id)));
    o.Set("seq", Napi::Number::New(env, r.seq));
    o.Set("status", Napi::Number::New(env, r.status));
    o.Set("isMaker", Napi::Boolean::New(env, r.is_maker != 0));
    // Clock readings cross as BigInt: a double steps 256 ns at today's values.
    o.Set("tsNs", Napi::BigInt::New(env, r.ts_ns));
    o.Set("fillQty",
          Napi::Number::New(env, static_cast<double>(r.fill_qty_raw) / 1e8));
    o.Set("fillPrice",
          Napi::Number::New(env, static_cast<double>(r.fill_price_raw) / 1e8));
    o.Set("queueAhead",
          Napi::Number::New(env, static_cast<double>(r.queue_ahead_raw) / 1e8));
    o.Set("queueTotal",
          Napi::Number::New(env, static_cast<double>(r.queue_total_raw) / 1e8));
    o.Set("submittedAtNs", Napi::BigInt::New(env, r.submitted_at_ns));
    o.Set("acceptedAtNs", Napi::BigInt::New(env, r.accepted_at_ns));
    o.Set("firstFillAtNs", Napi::BigInt::New(env, r.first_fill_at_ns));
    o.Set("lastFillAtNs", Napi::BigInt::New(env, r.last_fill_at_ns));
    o.Set("canceledAtNs", Napi::BigInt::New(env, r.canceled_at_ns));
    o.Set("rejectedAtNs", Napi::BigInt::New(env, r.rejected_at_ns));
    o.Set("triggeredAtNs", Napi::BigInt::New(env, r.triggered_at_ns));
    o.Set("expiredAtNs", Napi::BigInt::New(env, r.expired_at_ns));
    return o;
  }
  Napi::Value Result(const Napi::CallbackInfo& info)
  {
    const uint64_t n = lrvx_order_journey_tracer_result(_h, nullptr, 0);
    std::vector<LrvxOrderTraceRow> buf(n);
    if (n > 0)
    {
      lrvx_order_journey_tracer_result(_h, buf.data(), n);
    }
    auto arr = Napi::Array::New(info.Env(), n);
    for (uint64_t i = 0; i < n; ++i)
    {
      arr.Set(i, rowToObj(info.Env(), buf[i]));
    }
    return arr;
  }
  Napi::Value Journey(const Napi::CallbackInfo& info)
  {
    uint64_t orderId = info[0].As<Napi::Number>().Int64Value();
    const uint64_t n =
        lrvx_order_journey_tracer_journey(_h, orderId, nullptr, 0);
    std::vector<LrvxOrderTraceRow> buf(n);
    if (n > 0)
    {
      lrvx_order_journey_tracer_journey(_h, orderId, buf.data(), n);
    }
    auto arr = Napi::Array::New(info.Env(), n);
    for (uint64_t i = 0; i < n; ++i)
    {
      arr.Set(i, rowToObj(info.Env(), buf[i]));
    }
    return arr;
  }
  void Clear(const Napi::CallbackInfo&) { lrvx_order_journey_tracer_clear(_h); }

  LrvxOrderJourneyTracerHandle _h;
};

inline void registerPositions(Napi::Env env, Napi::Object exports)
{
  exports.Set("PositionTracker", PositionTrackerWrap::Init(env));
  exports.Set("PositionGroupTracker", PositionGroupTrackerWrap::Init(env));
  exports.Set("OrderTracker", OrderTrackerWrap::Init(env));
  exports.Set("OrderJourneyTracer", OrderJourneyTracerWrap::Init(env));
  exports.Set("POSITION_FIFO", Napi::Number::New(env, 0));
  exports.Set("POSITION_AVG_COST", Napi::Number::New(env, 1));
}

}  // namespace node_lrvx
