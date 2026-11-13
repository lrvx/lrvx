// node/src/backtest.h -- SimulatedExecutor + BacktestResult

#pragma once
#include <napi.h>
#include "bindings_common.h"
#include "data_ops.h"
#include "lrvx/backtest/runner_fill_settings.h"
#include "lrvx/capi/lrvx_capi.h"
#include "lrvx/capi/order_type_names.hpp"
#include "rate_limit.h"
#include "venue_availability.h"

namespace node_lrvx
{

class LatencyDistributionWrap : public Napi::ObjectWrap<LatencyDistributionWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(
        env, "LatencyDistribution",
        {InstanceMethod("setConstant", &LatencyDistributionWrap::SetConstant),
         InstanceMethod("setUniform", &LatencyDistributionWrap::SetUniform),
         InstanceMethod("setLognormal", &LatencyDistributionWrap::SetLognormal),
         InstanceMethod("setEmpirical", &LatencyDistributionWrap::SetEmpirical),
         InstanceMethod("setBurstCorrelation",
                        &LatencyDistributionWrap::SetBurstCorrelation),
         InstanceMethod("medianNs", &LatencyDistributionWrap::MedianNs)});
  }

  LatencyDistributionWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<LatencyDistributionWrap>(info),
        _h(lrvx_latency_distribution_create())
  {
  }
  ~LatencyDistributionWrap()
  {
    if (_h)
    {
      lrvx_latency_distribution_destroy(_h);
    }
  }
  LrvxLatencyDistributionHandle handle() const { return _h; }

 private:
  void SetConstant(const Napi::CallbackInfo& info)
  {
    lrvx_latency_distribution_set_constant(_h, argInteger(info[0], "setConstant: argument 1"));
  }
  void SetUniform(const Napi::CallbackInfo& info)
  {
    lrvx_latency_distribution_set_uniform(_h, argInteger(info[0], "setUniform: argument 1"),
                                          argInteger(info[1], "setUniform: argument 2"));
  }
  void SetLognormal(const Napi::CallbackInfo& info)
  {
    lrvx_latency_distribution_set_lognormal(_h,
                                            argInteger(info[0], "setLognormal: argument 1"),
                                            argNumber(info[1], "setLognormal: argument 2"));
  }
  void SetEmpirical(const Napi::CallbackInfo& info)
  {
    auto arr = argArray(info[0], "setEmpirical: argument 1");
    std::vector<int64_t> samples;
    samples.reserve(arr.Length());
    for (uint32_t i = 0; i < arr.Length(); ++i)
    {
      samples.push_back(argInteger(arr.Get(i), "setEmpirical: a sample"));
    }
    lrvx_latency_distribution_set_empirical(
        _h, samples.empty() ? nullptr : samples.data(),
        static_cast<uint32_t>(samples.size()));
  }
  void SetBurstCorrelation(const Napi::CallbackInfo& info)
  {
    lrvx_latency_distribution_set_burst_correlation(
        _h, argNumber(info[0], "setBurstCorrelation: argument 1"));
  }
  Napi::Value MedianNs(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(),
                             static_cast<double>(lrvx_latency_distribution_median_ns(_h)));
  }

  LrvxLatencyDistributionHandle _h;
};

class SimulatedExecutorWrap : public Napi::ObjectWrap<SimulatedExecutorWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "SimulatedExecutor",
                       {InstanceMethod("submitOrder", &SimulatedExecutorWrap::SubmitOrder),
                        InstanceMethod("cancelOrder", &SimulatedExecutorWrap::CancelOrder),
                        InstanceMethod("cancelAll", &SimulatedExecutorWrap::CancelAll),
                        InstanceMethod("onBar", &SimulatedExecutorWrap::OnBar),
                        InstanceMethod("onBarOhlc", &SimulatedExecutorWrap::OnBarOhlc),
                        InstanceMethod("beginBarCallbackWindow",
                                       &SimulatedExecutorWrap::BeginBarCallbackWindow),
                        InstanceMethod("endBarCallbackWindow",
                                       &SimulatedExecutorWrap::EndBarCallbackWindow),
                        InstanceMethod("reset", &SimulatedExecutorWrap::Reset),
                        InstanceMethod("onTrade", &SimulatedExecutorWrap::OnTrade),
                        InstanceMethod("onTopOfBook", &SimulatedExecutorWrap::OnTopOfBook),
                        InstanceMethod("advanceClock", &SimulatedExecutorWrap::AdvanceClock),
                        InstanceMethod("setDefaultSlippage", &SimulatedExecutorWrap::SetDefaultSlippage),
                        InstanceMethod("setQueueModel", &SimulatedExecutorWrap::SetQueueModel),
                        InstanceMethod("setQueueFifoTopN", &SimulatedExecutorWrap::SetQueueFifoTopN),
                        InstanceMethod("setTopPriorityShare", &SimulatedExecutorWrap::SetTopPriorityShare),
                        InstanceMethod("setLmmOrders", &SimulatedExecutorWrap::SetLmmOrders),
                        InstanceMethod("setLmmBonusMultiplier", &SimulatedExecutorWrap::SetLmmBonusMultiplier),
                        InstanceMethod("setOrderPriorityMultiplier", &SimulatedExecutorWrap::SetOrderPriorityMultiplier),
                        InstanceMethod("setSubmitAckLatency", &SimulatedExecutorWrap::SetSubmitAck),
                        InstanceMethod("setCancelAckLatency", &SimulatedExecutorWrap::SetCancelAck),
                        InstanceMethod("setReplaceAckLatency", &SimulatedExecutorWrap::SetReplaceAck),
                        InstanceMethod("setSubmitAckLatencyDistribution",
                                       &SimulatedExecutorWrap::SetSubmitAckDist),
                        InstanceMethod("setCancelAckLatencyDistribution",
                                       &SimulatedExecutorWrap::SetCancelAckDist),
                        InstanceMethod("setReplaceAckLatencyDistribution",
                                       &SimulatedExecutorWrap::SetReplaceAckDist),
                        InstanceMethod("applyLatencyProfile", &SimulatedExecutorWrap::ApplyLatencyProfile),
                        InstanceMethod("setRateLimitPolicy",
                                       &SimulatedExecutorWrap::SetRateLimitPolicy),
                        InstanceMethod("clearRateLimitPolicy",
                                       &SimulatedExecutorWrap::ClearRateLimitPolicy),
                        InstanceMethod("setSTPMode", &SimulatedExecutorWrap::SetSTPMode),
                        InstanceMethod("setSTPGroupMembership", &SimulatedExecutorWrap::SetStpGroupMembership),
                        InstanceMethod("stpGroupFor", &SimulatedExecutorWrap::StpGroupFor),
                        InstanceMethod("setFokMode", &SimulatedExecutorWrap::SetFokMode),
                        InstanceMethod("fokMode", &SimulatedExecutorWrap::FokMode),
                        InstanceMethod("setVenueAvailability",
                                       &SimulatedExecutorWrap::SetVenueAvailability),
                        InstanceMethod("submitBracket",
                                       &SimulatedExecutorWrap::SubmitBracket),
                        InstanceMethod("cancelBracket",
                                       &SimulatedExecutorWrap::CancelBracket),
                        InstanceMethod("bracketState",
                                       &SimulatedExecutorWrap::BracketState),
                        InstanceMethod("setBracketChildArmMode",
                                       &SimulatedExecutorWrap::SetBracketChildArmMode),
                        InstanceMethod("submitIceberg", &SimulatedExecutorWrap::SubmitIceberg),
                        InstanceMethod("setIcebergRefreshLatency",
                                       &SimulatedExecutorWrap::SetIcebergRefreshLatency),
                        InstanceMethod("icebergHiddenRemainingRaw",
                                       &SimulatedExecutorWrap::IcebergHiddenRemainingRaw),
                        InstanceMethod("setIcebergSizeRandomisationPct",
                                       &SimulatedExecutorWrap::SetIcebergSizeRandomisationPct),
                        InstanceMethod("setIcebergPriorityMode",
                                       &SimulatedExecutorWrap::SetIcebergPriorityMode),
                        InstanceMethod("setIcebergJitterSeed",
                                       &SimulatedExecutorWrap::SetIcebergJitterSeed),
                        InstanceAccessor("fillCount", &SimulatedExecutorWrap::FillCount, nullptr)});
  }

  SimulatedExecutorWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<SimulatedExecutorWrap>(info), _h(lrvx_simulated_executor_create()) {}
  ~SimulatedExecutorWrap()
  {
    if (_h)
    {
      lrvx_simulated_executor_destroy(_h);
    }
  }
  LrvxSimulatedExecutorHandle handle() const { return _h; }

 private:
  void SubmitOrder(const Napi::CallbackInfo& info)
  {
    uint64_t id = argInteger(info[0], "submitOrder: argument 1");
    std::string side = argString(info[1], "submitOrder: argument 2");
    double price = argNumber(info[2], "submitOrder: argument 3");
    double qty = argNumber(info[3], "submitOrder: argument 4");
    std::string type = info.Length() > 4 && info[4].IsString() ? argString(info[4], "submitOrder: argument 5") : "market";
    uint32_t sym = info.Length() > 5 ? argUint32(info[5], "submitOrder: argument 6") : 1;
    uint8_t s = side == "buy" ? 0 : 1;
    // lrvx_simulated_executor_submit_order takes an order_type in the
    // lrvx::OrderType space (LIMIT=0, MARKET=1, ...), NOT the
    // LRVX_SIGNAL_TYPE_* space. See include/lrvx/capi/order_type_names.hpp.
    uint8_t t = 0;
    if (!lrvx::capi::orderTypeCodeFromName(type, &t))
    {
      Napi::TypeError::New(info.Env(),
                           "submitOrder: " + lrvx::capi::unknownOrderTypeMessage(type))
          .ThrowAsJavaScriptException();
      return;
    }
    // Optional opts object as the 7th argument: { tif, reduceOnly, expiresAtNs }.
    if (info.Length() > 6 && info[6].IsObject())
    {
      auto opts = argObject(info[6], "submitOrder: argument 7");
      uint8_t tif = 0;
      bool reduceOnly = false;
      int64_t expiresAtNs = 0;
      if (opts.Has("tif"))
      {
        std::string tifStr = argString(opts.Get("tif"), "submitOrder: tif");
        if (tifStr == "ioc")
        {
          tif = 1;
        }
        else if (tifStr == "fok")
        {
          tif = 2;
        }
        else if (tifStr == "gtd")
        {
          tif = 3;
        }
        else if (tifStr == "post_only")
        {
          tif = 4;
        }
      }
      if (opts.Has("reduceOnly"))
      {
        reduceOnly = argBool(opts.Get("reduceOnly"), "submitOrder: reduceOnly");
      }
      if (opts.Has("expiresAtNs"))
      {
        expiresAtNs = toInt64Ns(opts.Get("expiresAtNs"), "submitOrder: expiresAtNs");
      }
      lrvx_simulated_executor_submit_order_ex(_h, id, s, price, qty, t, sym, tif,
                                              reduceOnly ? 1 : 0, expiresAtNs);
      return;
    }
    lrvx_simulated_executor_submit_order(_h, id, s, price, qty, t, sym);
  }
  void CancelOrder(const Napi::CallbackInfo& info) { lrvx_simulated_executor_cancel_order(_h, argInteger(info[0], "cancelOrder: argument 1")); }
  void CancelAll(const Napi::CallbackInfo& info) { lrvx_simulated_executor_cancel_all(_h, argUint32(info[0], "cancelAll: argument 1")); }
  void OnBar(const Napi::CallbackInfo& info) { lrvx_simulated_executor_on_bar(_h, argUint32(info[0], "onBar: argument 1"), argNumber(info[1], "onBar: argument 2")); }
  // Open-aware form: moves the market to the open (releasing any order held
  // from the previous bar's callback there), then walks low -> high -> close.
  // Needed to drive SimulatedExecutor by hand to the same fills
  // BacktestRunner.runBars produces, which uses this path internally.
  void OnBarOhlc(const Napi::CallbackInfo& info)
  {
    lrvx_simulated_executor_on_bar_ohlc(_h, argUint32(info[0], "onBarOhlc: argument 1"),
                                        argNumber(info[1], "onBarOhlc: argument 2"),
                                        argNumber(info[2], "onBarOhlc: argument 3"),
                                        argNumber(info[3], "onBarOhlc: argument 4"),
                                        argNumber(info[4], "onBarOhlc: argument 5"));
  }
  void BeginBarCallbackWindow(const Napi::CallbackInfo&)
  {
    lrvx_simulated_executor_begin_bar_callback_window(_h);
  }
  void EndBarCallbackWindow(const Napi::CallbackInfo&)
  {
    lrvx_simulated_executor_end_bar_callback_window(_h);
  }
  // Drops fills and run-scoped state while keeping installed configuration
  // (slippage, queue model, latency, ...), so a second hand-driven run
  // reports that run and not the sum of every run.
  void Reset(const Napi::CallbackInfo&) { lrvx_simulated_executor_reset(_h); }
  void OnTrade(const Napi::CallbackInfo& info) { lrvx_simulated_executor_on_trade(_h, argUint32(info[0], "onTrade: argument 1"), argNumber(info[1], "onTrade: argument 2"), argBool(info[2], "onTrade: argument 3") ? 1 : 0); }

  // Touch-only frame. A side is `{ price, qty }` or null; null means that
  // side of the book is empty.
  // A side is `{ price, qty }` or null; null (or undefined) means that
  // side of the book is empty. Anything else throws — silently feeding an
  // empty side for a malformed argument would erase the executor's touch.
  void OnTopOfBook(const Napi::CallbackInfo& info)
  {
    auto side = [&info](Napi::Value v, uint8_t& has, double& price, double& qty) -> bool
    {
      if (v.IsNull() || v.IsUndefined())
      {
        return true;
      }
      if (!v.IsObject())
      {
        Napi::TypeError::New(info.Env(), "onTopOfBook: a side is { price, qty } or null")
            .ThrowAsJavaScriptException();
        return false;
      }
      auto o = v.As<Napi::Object>();
      has = 1;
      price = argNumber(o.Get("price"), "onTopOfBook: price");
      qty = argNumber(o.Get("qty"), "onTopOfBook: qty");
      return true;
    };
    uint8_t hasBid = 0;
    uint8_t hasAsk = 0;
    double bidPrice = 0, bidQty = 0, askPrice = 0, askQty = 0;
    if (!side(info[1], hasBid, bidPrice, bidQty) || !side(info[2], hasAsk, askPrice, askQty))
    {
      return;
    }
    lrvx_simulated_executor_on_top_of_book(_h, argUint32(info[0], "onTopOfBook: argument 1"),
                                           hasBid, bidPrice, bidQty, hasAsk, askPrice, askQty);
  }
  void AdvanceClock(const Napi::CallbackInfo& info) { lrvx_simulated_executor_advance_clock(_h, toInt64Ns(info[0], "advanceClock: argument 1")); }
  void SetDefaultSlippage(const Napi::CallbackInfo& info)
  {
    // Accept either the string name or the exported SLIPPAGE_* constant.
    // The constants are part of the public surface, so rejecting them
    // made documented code throw at runtime. An unknown name or code throws
    // and leaves the previous profile in place.
    int32_t m = 0;
    if (info.Length() > 0 && info[0].IsNumber())
    {
      m = argInt32(info[0], "setDefaultSlippage: argument 1");
    }
    else
    {
      const std::string model =
          info.Length() > 0 ? info[0].ToString().Utf8Value() : std::string("undefined");
      const auto parsed = lrvx::slippageModelFromName(model);
      if (!parsed)
      {
        Napi::Error::New(info.Env(), "setDefaultSlippage: model must be one of " +
                                         lrvx::slippageModelNameList() + "; got '" + model +
                                         "'")
            .ThrowAsJavaScriptException();
        return;
      }
      m = static_cast<int32_t>(*parsed);
    }
    int32_t ticks = info.Length() > 1 ? argInt32(info[1], "setDefaultSlippage: argument 2") : 0;
    double tickSize = info.Length() > 2 ? argNumber(info[2], "setDefaultSlippage: argument 3") : 0.0;
    double bps = info.Length() > 3 ? argNumber(info[3], "setDefaultSlippage: argument 4") : 0.0;
    double impact = info.Length() > 4 ? argNumber(info[4], "setDefaultSlippage: argument 5") : 0.0;
    lrvx_clear_last_error();
    lrvx_simulated_executor_set_default_slippage(_h, m, ticks, tickSize, bps, impact);
    if (lrvx_last_error_code() != 0)
    {
      Napi::Error::New(info.Env(),
                       std::string("setDefaultSlippage: ") + lrvx_last_error_message())
          .ThrowAsJavaScriptException();
    }
  }
  void SetQueueModel(const Napi::CallbackInfo& info)
  {
    // Accept either the string name or the exported QUEUE_* constant. An
    // unknown one of either throws and leaves the model installed before.
    int32_t m = 0;
    if (info.Length() > 0 && info[0].IsNumber())
    {
      m = argInt32(info[0], "setQueueModel: argument 1");
    }
    else
    {
      const std::string model =
          info.Length() > 0 ? info[0].ToString().Utf8Value() : std::string("undefined");
      const auto parsed = lrvx::queueModelFromName(model);
      if (!parsed)
      {
        Napi::Error::New(info.Env(), "setQueueModel: model must be one of " +
                                         lrvx::queueModelNameList() + "; got '" + model + "'")
            .ThrowAsJavaScriptException();
        return;
      }
      m = static_cast<int32_t>(*parsed);
    }
    uint32_t depth = info.Length() > 1 ? argUint32(info[1], "setQueueModel: argument 2") : 1;
    lrvx_clear_last_error();
    lrvx_simulated_executor_set_queue_model(_h, m, depth);
    if (lrvx_last_error_code() != 0)
    {
      Napi::Error::New(info.Env(), std::string("setQueueModel: ") + lrvx_last_error_message())
          .ThrowAsJavaScriptException();
    }
  }
  void SetQueueFifoTopN(const Napi::CallbackInfo& info)
  {
    uint32_t topN = argUint32(info[0], "setQueueFifoTopN: argument 1");
    lrvx_simulated_executor_set_queue_fifo_top_n(_h, topN);
  }
  void SetTopPriorityShare(const Napi::CallbackInfo& info)
  {
    lrvx_simulated_executor_set_top_priority_share(
        _h, argNumber(info[0], "setTopPriorityShare: argument 1"));
  }
  void SetLmmOrders(const Napi::CallbackInfo& info)
  {
    auto arr = argArray(info[0], "setLmmOrders: argument 1");
    std::vector<uint64_t> ids(arr.Length());
    for (uint32_t i = 0; i < arr.Length(); ++i)
    {
      ids[i] = argUint64(arr.Get(i), "setLmmOrders: an order id");
    }
    lrvx_simulated_executor_set_lmm_orders(_h, ids.data(),
                                           static_cast<uint32_t>(ids.size()));
  }
  void SetLmmBonusMultiplier(const Napi::CallbackInfo& info)
  {
    lrvx_simulated_executor_set_lmm_bonus_multiplier(
        _h, argNumber(info[0], "setLmmBonusMultiplier: argument 1"));
  }
  void SetOrderPriorityMultiplier(const Napi::CallbackInfo& info)
  {
    lrvx_simulated_executor_set_order_priority_multiplier(
        _h, argInteger(info[0], "setOrderPriorityMultiplier: argument 1"),
        argNumber(info[1], "setOrderPriorityMultiplier: argument 2"));
  }
  void SetSubmitAck(const Napi::CallbackInfo& info)
  {
    int64_t latency = argInteger(info[0], "setSubmitAck: argument 1");
    int64_t jitter = info.Length() > 1 ? argInteger(info[1], "setSubmitAck: argument 2") : 0;
    lrvx_simulated_executor_set_submit_ack_latency(_h, latency, jitter);
  }
  void SetCancelAck(const Napi::CallbackInfo& info)
  {
    int64_t latency = argInteger(info[0], "setCancelAck: argument 1");
    int64_t jitter = info.Length() > 1 ? argInteger(info[1], "setCancelAck: argument 2") : 0;
    lrvx_simulated_executor_set_cancel_ack_latency(_h, latency, jitter);
  }
  void SetReplaceAck(const Napi::CallbackInfo& info)
  {
    int64_t latency = argInteger(info[0], "setReplaceAck: argument 1");
    int64_t jitter = info.Length() > 1 ? argInteger(info[1], "setReplaceAck: argument 2") : 0;
    lrvx_simulated_executor_set_replace_ack_latency(_h, latency, jitter);
  }
  void ApplyLatencyProfile(const Napi::CallbackInfo& info)
  {
    std::string name = argString(info[0], "applyLatencyProfile: argument 1");
    // A typo used to be a silent no-op, leaving default latency profile
    // values in place -- a zero-fee or unthrottled backtest that
    // looks perfectly healthy.
    if (lrvx_simulated_executor_apply_latency_profile(_h, name.c_str()) == 0)
    {
      Napi::Error::New(info.Env(), "unknown latency profile profile: " + name +
                                       ". known: binance_um_futures, bybit_linear, okx_swap, deribit, idealized, adversarial")
          .ThrowAsJavaScriptException();
    }
  }
  void SetSubmitAckDist(const Napi::CallbackInfo& info)
  {
    auto* w = Napi::ObjectWrap<LatencyDistributionWrap>::Unwrap(
        argObject(info[0], "setSubmitAckDist: argument 1"));
    lrvx_simulated_executor_set_submit_ack_latency_distribution(_h, w->handle());
  }
  void SetCancelAckDist(const Napi::CallbackInfo& info)
  {
    auto* w = Napi::ObjectWrap<LatencyDistributionWrap>::Unwrap(
        argObject(info[0], "setCancelAckDist: argument 1"));
    lrvx_simulated_executor_set_cancel_ack_latency_distribution(_h, w->handle());
  }
  void SetReplaceAckDist(const Napi::CallbackInfo& info)
  {
    auto* w = Napi::ObjectWrap<LatencyDistributionWrap>::Unwrap(
        argObject(info[0], "setReplaceAckDist: argument 1"));
    lrvx_simulated_executor_set_replace_ack_latency_distribution(_h, w->handle());
  }
  void SetRateLimitPolicy(const Napi::CallbackInfo& info)
  {
    auto* w = Napi::ObjectWrap<RateLimitPolicyWrap>::Unwrap(
        argObject(info[0], "setRateLimitPolicy: argument 1"));
    lrvx_simulated_executor_set_rate_limit_policy(_h, w->handle());
  }
  void ClearRateLimitPolicy(const Napi::CallbackInfo&)
  {
    lrvx_simulated_executor_clear_rate_limit_policy(_h);
  }
  void SetSTPMode(const Napi::CallbackInfo& info)
  {
    std::string mode = argString(info[0], "setSTPMode: argument 1");
    uint8_t m = 0;
    if (mode == "cancel_newest")
    {
      m = 1;
    }
    else if (mode == "cancel_oldest")
    {
      m = 2;
    }
    else if (mode == "cancel_both")
    {
      m = 3;
    }
    else if (mode == "decrement")
    {
      m = 4;
    }
    lrvx_simulated_executor_set_stp_mode(_h, m);
  }
  void SetStpGroupMembership(const Napi::CallbackInfo& info)
  {
    lrvx_simulated_executor_set_stp_group_membership(
        _h, argInteger(info[0], "setStpGroupMembership: argument 1"),
        argInteger(info[1], "setStpGroupMembership: argument 2"));
  }
  Napi::Value StpGroupFor(const Napi::CallbackInfo& info)
  {
    uint64_t v = lrvx_simulated_executor_stp_group_for(
        _h, argInteger(info[0], "stpGroupFor: argument 1"));
    return Napi::Number::New(info.Env(), static_cast<double>(v));
  }
  void SetFokMode(const Napi::CallbackInfo& info)
  {
    std::string mode = argString(info[0], "setFokMode: argument 1");
    uint8_t m = 0;
    if (mode == "single_price")
    {
      m = 1;
    }
    lrvx_simulated_executor_set_fok_mode(_h, m);
  }
  Napi::Value FokMode(const Napi::CallbackInfo& info)
  {
    const uint8_t code = lrvx_simulated_executor_fok_mode(_h);
    return Napi::String::New(info.Env(),
                             code == 1 ? "single_price" : "any_price");
  }
  void SetVenueAvailability(const Napi::CallbackInfo& info)
  {
    if (info.Length() == 0 || info[0].IsNull() || info[0].IsUndefined())
    {
      lrvx_simulated_executor_set_venue_availability(_h, nullptr);
      return;
    }
    auto* w = Napi::ObjectWrap<VenueAvailabilityWrap>::Unwrap(argObject(info[0], "setVenueAvailability: argument 1"));
    lrvx_simulated_executor_set_venue_availability(_h, w->handle());
  }
  void SubmitBracket(const Napi::CallbackInfo& info)
  {
    auto opts = argObject(info[0], "submitBracket: argument 1");
    uint64_t bracketId = argInteger(opts.Get("bracketId"), "submitBracket: bracketId");
    uint32_t sym = opts.Has("symbol") ? argUint32(opts.Get("symbol"), "submitBracket: symbol") : 1;
    auto sideOf = [&](const std::string& key)
    {
      const std::string s = argString(opts.Get(key), ("submitBracket: " + key).c_str());
      if (s != "buy" && s != "sell")
      {
        throw Napi::TypeError::New(info.Env(), "submitBracket: " + key +
                                                   " must be 'buy' or 'sell', got '" + s + "'");
      }
      return s == "buy" ? 0 : 1;
    };
    auto typeOf = [&](const std::string& key)
    {
      const std::string s = argString(opts.Get(key), ("submitBracket: " + key).c_str());
      if (s == "market")
      {
        return 1;
      }
      if (s == "stop_market")
      {
        return 2;
      }
      if (s == "stop_limit")
      {
        return 3;
      }
      return 0;
    };
    double entryPrice = argNumber(opts.Get("entryPrice"), "submitBracket: entryPrice");
    double qty = argNumber(opts.Get("quantity"), "submitBracket: quantity");
    double tpPrice = argNumber(opts.Get("tpPrice"), "submitBracket: tpPrice");
    double stopTriggerPrice = argNumber(opts.Get("stopTriggerPrice"), "submitBracket: stopTriggerPrice");
    lrvx_simulated_executor_submit_bracket(
        _h, bracketId, sym,
        static_cast<uint8_t>(sideOf("entrySide")),
        static_cast<uint8_t>(typeOf("entryType")),
        entryPrice, qty,
        static_cast<uint8_t>(sideOf("tpSide")),
        static_cast<uint8_t>(typeOf("tpType")),
        tpPrice,
        static_cast<uint8_t>(sideOf("stopSide")),
        static_cast<uint8_t>(typeOf("stopType")),
        stopTriggerPrice);
  }
  void CancelBracket(const Napi::CallbackInfo& info)
  {
    lrvx_simulated_executor_cancel_bracket(_h, argInteger(info[0], "cancelBracket: argument 1"));
  }
  Napi::Value BracketState(const Napi::CallbackInfo& info)
  {
    const uint8_t s = lrvx_simulated_executor_bracket_state(
        _h, argInteger(info[0], "bracketState: argument 1"));
    const char* names[] = {"pending_entry", "entry_filled", "tp_filled",
                           "stop_filled", "canceled"};
    return Napi::String::New(info.Env(), s < 5 ? names[s] : "pending_entry");
  }
  void SetBracketChildArmMode(const Napi::CallbackInfo& info)
  {
    const std::string mode = argString(info[0], "setBracketChildArmMode: argument 1");
    lrvx_simulated_executor_set_bracket_child_arm_mode(
        _h, mode == "on_partial_fill" ? 1 : 0);
  }
  void SubmitIceberg(const Napi::CallbackInfo& info)
  {
    uint64_t id = argInteger(info[0], "submitIceberg: argument 1");
    std::string side = argString(info[1], "submitIceberg: argument 2");
    double price = argNumber(info[2], "submitIceberg: argument 3");
    double total = argNumber(info[3], "submitIceberg: argument 4");
    double visible = argNumber(info[4], "submitIceberg: argument 5");
    uint32_t sym = info.Length() > 5 ? argUint32(info[5], "submitIceberg: argument 6") : 1;
    uint8_t s = side == "buy" ? 0 : 1;
    lrvx_simulated_executor_submit_iceberg(_h, id, s, price, total, visible, sym);
  }
  void SetIcebergRefreshLatency(const Napi::CallbackInfo& info)
  {
    lrvx_simulated_executor_set_iceberg_refresh_latency(
        _h, argInteger(info[0], "setIcebergRefreshLatency: argument 1"));
  }
  Napi::Value IcebergHiddenRemainingRaw(const Napi::CallbackInfo& info)
  {
    int64_t id = argInteger(info[0], "icebergHiddenRemainingRaw: argument 1");
    return Napi::Number::New(info.Env(),
                             static_cast<double>(
                                 lrvx_simulated_executor_iceberg_hidden_remaining_raw(_h, id)));
  }
  void SetIcebergSizeRandomisationPct(const Napi::CallbackInfo& info)
  {
    lrvx_simulated_executor_set_iceberg_size_randomisation_pct(
        _h, argNumber(info[0], "setIcebergSizeRandomisationPct: argument 1"));
  }
  void SetIcebergPriorityMode(const Napi::CallbackInfo& info)
  {
    const std::string mode = argString(info[0], "setIcebergPriorityMode: argument 1");
    lrvx_simulated_executor_set_iceberg_priority_mode(_h, mode == "retain" ? 1 : 0);
  }
  void SetIcebergJitterSeed(const Napi::CallbackInfo& info)
  {
    lrvx_simulated_executor_set_iceberg_jitter_seed(
        _h, static_cast<uint64_t>(argInteger(info[0], "setIcebergJitterSeed: argument 1")));
  }
  Napi::Value FillCount(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), lrvx_simulated_executor_fill_count(_h)); }
  LrvxSimulatedExecutorHandle _h;
};

class BacktestResultWrap : public Napi::ObjectWrap<BacktestResultWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "BacktestResult",
                       {InstanceMethod("recordFill", &BacktestResultWrap::RecordFill),
                        InstanceMethod("ingestExecutor", &BacktestResultWrap::IngestExecutor),
                        InstanceMethod("stats", &BacktestResultWrap::Stats)});
  }
  BacktestResultWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<BacktestResultWrap>(info)
  {
    double cap = info.Length() > 0 ? argNumber(info[0], "BacktestResult: argument 1") : 100000.0;
    double fee = info.Length() > 1 ? argNumber(info[1], "BacktestResult: argument 2") : 0.0001;
    _h = lrvx_backtest_result_create(cap, fee, 1, 0.0, 0.0, 252.0);
  }
  ~BacktestResultWrap()
  {
    if (_h)
    {
      lrvx_backtest_result_destroy(_h);
    }
  }

 private:
  void RecordFill(const Napi::CallbackInfo& info)
  {
    lrvx_backtest_result_record_fill(_h,
                                     argInteger(info[0], "recordFill: argument 1"),
                                     argUint32(info[1], "recordFill: argument 2"),
                                     argString(info[2], "recordFill: argument 3") == "buy" ? 0 : 1,
                                     argNumber(info[3], "recordFill: argument 4"),
                                     argNumber(info[4], "recordFill: argument 5"),
                                     toInt64Ns(info[5], "recordFill: argument 6"));
  }
  void IngestExecutor(const Napi::CallbackInfo& info)
  {
    auto* exec = Napi::ObjectWrap<SimulatedExecutorWrap>::Unwrap(argObject(info[0], "ingestExecutor: argument 1"));
    lrvx_backtest_result_ingest_executor(_h, exec->handle());
  }
  Napi::Value Stats(const Napi::CallbackInfo& info)
  {
    LrvxBacktestStats s{};
    lrvx_backtest_result_stats(_h, &s);
    auto o = Napi::Object::New(info.Env());
    o.Set("totalTrades", (double)s.totalTrades);
    o.Set("winningTrades", (double)s.winningTrades);
    o.Set("losingTrades", (double)s.losingTrades);
    o.Set("initialCapital", s.initialCapital);
    o.Set("finalCapital", s.finalCapital);
    o.Set("netPnl", s.netPnl);
    o.Set("totalPnl", s.totalPnl);
    o.Set("totalFees", s.totalFees);
    o.Set("grossProfit", s.grossProfit);
    o.Set("grossLoss", s.grossLoss);
    o.Set("maxDrawdown", s.maxDrawdown);
    o.Set("maxDrawdownPct", s.maxDrawdownPct);
    o.Set("winRate", s.winRate);
    o.Set("profitFactor", s.profitFactor);
    o.Set("avgWin", s.avgWin);
    o.Set("avgLoss", s.avgLoss);
    o.Set("sharpeRatio", s.sharpeRatio);
    o.Set("sortinoRatio", s.sortinoRatio);
    o.Set("calmarRatio", s.calmarRatio);
    o.Set("returnPct", s.returnPct);
    setTradeTimeStats(info.Env(), o, s);
    return o;
  }
  LrvxBacktestResultHandle _h;
};

inline void registerBacktest(Napi::Env env, Napi::Object exports)
{
  exports.Set("LatencyDistribution", LatencyDistributionWrap::Init(env));
  exports.Set("SimulatedExecutor", SimulatedExecutorWrap::Init(env));
  exports.Set("BacktestResult", BacktestResultWrap::Init(env));

  exports.Set("SLIPPAGE_NONE", Napi::Number::New(env, LRVX_SLIPPAGE_NONE));
  exports.Set("SLIPPAGE_FIXED_TICKS", Napi::Number::New(env, LRVX_SLIPPAGE_FIXED_TICKS));
  exports.Set("SLIPPAGE_FIXED_BPS", Napi::Number::New(env, LRVX_SLIPPAGE_FIXED_BPS));
  exports.Set("SLIPPAGE_VOLUME_IMPACT", Napi::Number::New(env, LRVX_SLIPPAGE_VOLUME_IMPACT));

  exports.Set("QUEUE_NONE", Napi::Number::New(env, LRVX_QUEUE_NONE));
  exports.Set("QUEUE_TOB", Napi::Number::New(env, LRVX_QUEUE_TOB));
  exports.Set("QUEUE_FULL", Napi::Number::New(env, LRVX_QUEUE_FULL));
}

}  // namespace node_lrvx
