/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// NAPI wrappers for the C-ABI extension hooks.
//
// API shape (idiomatic Node):
//
//   runner.setPnlTracker({ onSignal(sig) { ... } });
//   runner.setExecutor({
//     submit(order) { broker.place(order); },
//     cancel(orderId) { broker.cancel(orderId); },
//     capabilities() { return { stopMarket: true, oco: true }; },
//   });
//
// Each hook host:
//   - extracts named function references from the JS object on attach;
//   - calls straight back into JS from the C bridge;
//   - owns a lrvx<Hook>Handle via RAII; non-copyable.
//
// Every host here runs on the JS thread and only there. Each of these hooks is
// read inline by the engine while a signal is in flight -- a gate decides
// whether the signal survives, an executor is handed the order it produced --
// so there is no point at which the result could be delivered on a later tick.
// A Runner that dispatches on a background thread refuses to take any of them
// (see RunnerNode::requireSyncHook in strategy.h); that refusal is what makes
// the JS-thread invariant a property of the code rather than of the call sites
// nobody has written yet.
//
// This file used to carry a second half -- a mode enum and a
// ThreadSafeFunction path behind it, for a threaded case nothing constructed.
// Dead plumbing that looks like a safety net is worse than none: it made the
// inline calls read as one branch of a choice, when they were the only branch.

#pragma once

#include <napi.h>

#include "lrvx/capi/lrvx_capi.h"
#include "lrvx/capi/order_type_names.hpp"
#include "tsfn_util.h"

#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace lrvx_node
{

// ── Helpers ─────────────────────────────────────────────────────────────

inline Napi::FunctionReference takeFn(Napi::Object obj, const char* name)
{
  auto val = obj.Get(name);
  if (val.IsFunction())
  {
    return Napi::Persistent(val.As<Napi::Function>());
  }
  return {};
}

// Build a JS object mirroring LrvxSignal for the user callback.
inline Napi::Object signalToJs(Napi::Env env, const LrvxSignal* s)
{
  auto obj = Napi::Object::New(env);
  obj.Set("orderId", Napi::Number::New(env, static_cast<double>(s->order_id)));
  obj.Set("symbol", Napi::Number::New(env, s->symbol));
  obj.Set("side", Napi::String::New(env, s->side == 0 ? "buy" : "sell"));
  obj.Set("orderType", Napi::String::New(env, lrvx::capi::signalTypeName(s->order_type)));
  obj.Set("price", Napi::Number::New(env, s->price));
  obj.Set("quantity", Napi::Number::New(env, s->quantity));
  obj.Set("triggerPrice", Napi::Number::New(env, s->trigger_price));
  obj.Set("trailingOffset", Napi::Number::New(env, s->trailing_offset));
  obj.Set("trailingBps", Napi::Number::New(env, s->trailing_bps));
  obj.Set("newPrice", Napi::Number::New(env, s->new_price));
  obj.Set("newQuantity", Napi::Number::New(env, s->new_quantity));
  obj.Set("rangeLower", Napi::Number::New(env, s->range_lower));
  obj.Set("rangeUpper", Napi::Number::New(env, s->range_upper));
  obj.Set("liquidity", Napi::Number::New(env, s->liquidity));
  return obj;
}

inline Napi::Object orderToJs(Napi::Env env, const LrvxOrder* o)
{
  static constexpr const char* kTif[] = {"gtc", "ioc", "fok", "gtd", "post_only"};
  auto obj = Napi::Object::New(env);
  obj.Set("id", Napi::Number::New(env, static_cast<double>(o->id)));
  obj.Set("clientOrderId", Napi::Number::New(env, static_cast<double>(o->client_order_id)));
  obj.Set("symbol", Napi::Number::New(env, o->symbol));
  obj.Set("strategyId", Napi::Number::New(env, o->strategy_id));
  obj.Set("orderTag", Napi::Number::New(env, o->order_tag));
  obj.Set("side", Napi::String::New(env, o->side == 0 ? "buy" : "sell"));
  obj.Set("orderType", Napi::String::New(env, lrvx::capi::orderTypeNameLower(o->type)));
  obj.Set("timeInForce",
          Napi::String::New(env, o->time_in_force < 5 ? kTif[o->time_in_force] : "unknown"));
  obj.Set("reduceOnly", Napi::Boolean::New(env, (o->flags & 0x01) != 0));
  obj.Set("closePosition", Napi::Boolean::New(env, (o->flags & 0x02) != 0));
  obj.Set("postOnly", Napi::Boolean::New(env, (o->flags & 0x04) != 0));
  obj.Set("price", Napi::Number::New(env, o->price_raw / 1e8));
  obj.Set("quantity", Napi::Number::New(env, o->quantity_raw / 1e8));
  obj.Set("filledQuantity", Napi::Number::New(env, o->filled_quantity_raw / 1e8));
  obj.Set("triggerPrice", Napi::Number::New(env, o->trigger_price_raw / 1e8));
  obj.Set("trailingOffset", Napi::Number::New(env, o->trailing_offset_raw / 1e8));
  // Clock readings, so BigInt: a double steps 256 ns at present-day
  // magnitudes, and these are the fields a listener orders events by. The
  // prices and sizes above are not clock readings and stay Numbers.
  obj.Set("createdAtNs", Napi::BigInt::New(env, static_cast<int64_t>(o->created_at_ns)));
  obj.Set("exchangeTsNs", Napi::BigInt::New(env, static_cast<int64_t>(o->exchange_ts_ns)));
  return obj;
}

// The trade a MarketDataRecorderHook is handed. index.d.ts types that
// argument TradeData, and this used to deliver something else: `quantity`
// where TradeData says `qty`, `exchangeTsNs` where it says `timestampNs`,
// and no `side` at all -- so a hook written against the declared type read
// undefined from two of its five fields, while the same TradeData coming out
// of a strategy's onTrade (built in node/src/strategy.h) had the declared
// spelling all along.
//
// The declared shape is what goes out now. `quantity` and `exchangeTsNs`
// stay beside it for one release so hooks written against what was actually
// delivered keep working; both are deprecated in index.d.ts and go in the
// release after. They are aliases, not copies of the old behaviour:
// exchangeTsNs carries the same BigInt as timestampNs, because a double
// could not hold the reading in the first place.
inline Napi::Object tradeToJs(Napi::Env env, const LrvxTradeData* t)
{
  auto obj = Napi::Object::New(env);
  auto qty = Napi::Number::New(env, t->quantity_raw / 1e8);
  auto ts = Napi::BigInt::New(env, static_cast<int64_t>(t->exchange_ts_ns));
  obj.Set("symbol", Napi::Number::New(env, t->symbol));
  obj.Set("price", Napi::Number::New(env, t->price_raw / 1e8));
  obj.Set("qty", qty);
  obj.Set("isBuy", Napi::Boolean::New(env, t->is_buy != 0));
  obj.Set("side", Napi::String::New(env, t->is_buy ? "buy" : "sell"));
  obj.Set("timestampNs", ts);
  obj.Set("quantity", qty);
  obj.Set("exchangeTsNs", ts);
  return obj;
}

inline Napi::Array bookLevelsToJs(Napi::Env env, const LrvxBookLevel* lvls, uint32_t n)
{
  auto arr = Napi::Array::New(env, n);
  for (uint32_t i = 0; i < n; ++i)
  {
    auto pair = Napi::Array::New(env, 2);
    pair.Set(uint32_t{0}, Napi::Number::New(env, lvls[i].price_raw / 1e8));
    pair.Set(uint32_t{1}, Napi::Number::New(env, lvls[i].quantity_raw / 1e8));
    arr.Set(i, pair);
  }
  return arr;
}

// Flat BigInt64Array view of LrvxBookLevel[]: [price_raw, qty_raw, ...].
// LrvxBookLevel layout is two int64s, so we copy via a backing ArrayBuffer
// to keep ownership simple — the JS caller can read raw int64 ticks without
// the precision loss of /1e8 doubles.
inline Napi::BigInt64Array bookLevelsToBigInt64(Napi::Env env, const LrvxBookLevel* lvls,
                                                uint32_t n)
{
  size_t elems = static_cast<size_t>(n) * 2;
  auto buf = Napi::ArrayBuffer::New(env, elems * sizeof(int64_t));
  if (n > 0)
  {
    std::memcpy(buf.Data(), lvls, n * sizeof(LrvxBookLevel));
  }
  return Napi::BigInt64Array::New(env, elems, buf, 0);
}

// ── PnLTracker ──────────────────────────────────────────────────────────

struct PnLTrackerHost
{
  Napi::FunctionReference on_signal_fn;
  Napi::Env env;
  LrvxPnLTrackerHandle handle{nullptr};

  PnLTrackerHost(Napi::Env env_, Napi::Object obj)
      : on_signal_fn(takeFn(obj, "onSignal")), env(env_)
  {
    LrvxPnLTrackerCallbacks cb{};
    cb.on_signal = &PnLTrackerHost::onSignalBridge;
    cb.user_data = this;
    handle = lrvx_pnl_tracker_create(cb);
  }
  ~PnLTrackerHost()
  {
    if (handle)
    {
      lrvx_pnl_tracker_destroy(handle);
    }
  }
  PnLTrackerHost(const PnLTrackerHost&) = delete;
  PnLTrackerHost& operator=(const PnLTrackerHost&) = delete;

  static void onSignalBridge(void* ud, const LrvxSignal* sig)
  {
    auto* self = static_cast<PnLTrackerHost*>(ud);
    if (self->on_signal_fn.IsEmpty())
    {
      return;
    }
    self->on_signal_fn.Call({signalToJs(self->env, sig)});
  }
};

// ── StorageSink ─────────────────────────────────────────────────────────

struct StorageSinkHost
{
  Napi::FunctionReference store_fn;
  Napi::Env env;
  LrvxStorageSinkHandle handle{nullptr};

  StorageSinkHost(Napi::Env env_, Napi::Object obj) : store_fn(takeFn(obj, "store")), env(env_)
  {
    LrvxStorageSinkCallbacks cb{};
    cb.store = &StorageSinkHost::storeBridge;
    cb.user_data = this;
    handle = lrvx_storage_sink_create(cb);
  }
  ~StorageSinkHost()
  {
    if (handle)
    {
      lrvx_storage_sink_destroy(handle);
    }
  }
  StorageSinkHost(const StorageSinkHost&) = delete;
  StorageSinkHost& operator=(const StorageSinkHost&) = delete;

  static void storeBridge(void* ud, const LrvxSignal* sig)
  {
    auto* self = static_cast<StorageSinkHost*>(ud);
    if (self->store_fn.IsEmpty())
    {
      return;
    }
    self->store_fn.Call({signalToJs(self->env, sig)});
  }
};

// ── Hook failure reporting ──────────────────────────────────────────────
//
// A gate runs inline while a signal is in flight, so it has no caller to
// throw back at: the frame underneath it is a C function pointer the engine
// called, and the frame under that is whatever JS happened to emit the
// signal. It leaves a record instead, which the owning Runner hands back
// through hookErrors().

struct HookErrorRecord
{
  std::string hook;
  std::string method;
  std::string message;
};

struct HookErrorSink
{
  std::vector<HookErrorRecord> records;

  void record(const char* hook, const char* method, std::string message)
  {
    records.push_back(HookErrorRecord{hook, method, std::move(message)});
  }
};

inline void reportHookError(HookErrorSink* sink, const char* hook, const char* method,
                            std::string message)
{
  if (sink)
  {
    sink->record(hook, method, std::move(message));
  }
}

// ── RiskManager / KillSwitch / OrderValidator (gate hooks) ──────────────
//
// These are pre-trade gates: return false → drop the signal. We must
// block on the JS callback for the result, which is incompatible with
// LiveEngine's async consumer thread (would deadlock). Document this
// limitation: gate hooks only work with the synchronous Runner.
//
// The policy, one for every binding: a host-language throw and a non-boolean
// return both DENY the order, and both are reported. Neither ever lets the
// order through, and neither escapes through the C boundary. A gate that
// plainly returns false is a decision, not a failure, and is not reported.
//
// The throw half matters here in particular: under
// NODE_ADDON_API_CPP_EXCEPTIONS_ALL (node/CMakeLists.txt)
// Napi::FunctionReference::Call turns a pending JS exception into a C++ one,
// which then unwinds out of a bridge declared to C, through the engine frames
// that were mid-signal, and surfaces at whatever JS frame was on the stack --
// the strategy's own emit.marketBuy() call. It is caught here instead.

inline uint8_t callGate(Napi::FunctionReference& fn, Napi::Env env, const LrvxSignal* sig,
                        HookErrorSink* sink, const char* hook, const char* method)
{
  if (fn.IsEmpty())
  {
    return 1;
  }
  Napi::Value result;
  try
  {
    result = fn.Call({signalToJs(env, sig)});
  }
  catch (const Napi::Error& e)
  {
    // node-addon-api cleared the pending JS exception when it built this.
    reportHookError(sink, hook, method, e.Message());
    return 0;
  }
  catch (const std::exception& e)
  {
    reportHookError(sink, hook, method, e.what());
    return 0;
  }
  if (!result.IsBoolean())
  {
    reportHookError(sink, hook, method,
                    std::string("returned a non-boolean (") + result.ToString().Utf8Value() +
                        "); a gate must return a boolean");
    return 0;
  }
  return result.As<Napi::Boolean>().Value() ? 1u : 0u;
}

struct RiskManagerHost
{
  Napi::FunctionReference allow_fn;
  Napi::Env env;
  LrvxRiskManagerHandle handle{nullptr};

  HookErrorSink* errors{nullptr};

  RiskManagerHost(Napi::Env env_, Napi::Object obj, HookErrorSink* sink = nullptr)
      : allow_fn(takeFn(obj, "allow")), env(env_), errors(sink)
  {
    LrvxRiskManagerCallbacks cb{};
    cb.allow = &RiskManagerHost::allowBridge;
    cb.user_data = this;
    handle = lrvx_risk_manager_create(cb);
  }
  ~RiskManagerHost()
  {
    if (handle)
    {
      lrvx_risk_manager_destroy(handle);
    }
  }
  RiskManagerHost(const RiskManagerHost&) = delete;
  RiskManagerHost& operator=(const RiskManagerHost&) = delete;

  static uint8_t allowBridge(void* ud, const LrvxSignal* sig)
  {
    auto* self = static_cast<RiskManagerHost*>(ud);
    // Synchronous — only safe to call from the JS thread (sync Runner).
    return callGate(self->allow_fn, self->env, sig, self->errors, "riskManager", "allow");
  }
};

struct KillSwitchHost
{
  Napi::FunctionReference check_fn;
  Napi::Env env;
  LrvxKillSwitchHandle handle{nullptr};

  HookErrorSink* errors{nullptr};

  KillSwitchHost(Napi::Env env_, Napi::Object obj, HookErrorSink* sink = nullptr)
      : check_fn(takeFn(obj, "check")), env(env_), errors(sink)
  {
    LrvxKillSwitchCallbacks cb{};
    cb.check = &KillSwitchHost::checkBridge;
    cb.user_data = this;
    handle = lrvx_kill_switch_create(cb);
  }
  ~KillSwitchHost()
  {
    if (handle)
    {
      lrvx_kill_switch_destroy(handle);
    }
  }
  KillSwitchHost(const KillSwitchHost&) = delete;
  KillSwitchHost& operator=(const KillSwitchHost&) = delete;

  static uint8_t checkBridge(void* ud, const LrvxSignal* sig)
  {
    auto* self = static_cast<KillSwitchHost*>(ud);
    return callGate(self->check_fn, self->env, sig, self->errors, "killSwitch", "check");
  }
};

struct OrderValidatorHost
{
  Napi::FunctionReference validate_fn;
  Napi::Env env;
  LrvxOrderValidatorHandle handle{nullptr};

  HookErrorSink* errors{nullptr};

  OrderValidatorHost(Napi::Env env_, Napi::Object obj, HookErrorSink* sink = nullptr)
      : validate_fn(takeFn(obj, "validate")), env(env_), errors(sink)
  {
    LrvxOrderValidatorCallbacks cb{};
    cb.validate = &OrderValidatorHost::validateBridge;
    cb.user_data = this;
    handle = lrvx_order_validator_create(cb);
  }
  ~OrderValidatorHost()
  {
    if (handle)
    {
      lrvx_order_validator_destroy(handle);
    }
  }
  OrderValidatorHost(const OrderValidatorHost&) = delete;
  OrderValidatorHost& operator=(const OrderValidatorHost&) = delete;

  static uint8_t validateBridge(void* ud, const LrvxSignal* sig)
  {
    auto* self = static_cast<OrderValidatorHost*>(ud);
    return callGate(self->validate_fn, self->env, sig, self->errors, "orderValidator",
                    "validate");
  }
};

// ── MarketDataRecorderHook ──────────────────────────────────────────────

struct MarketDataRecorderHookHost
{
  Napi::FunctionReference on_trade_fn;
  Napi::FunctionReference on_book_fn;
  Napi::FunctionReference on_start_fn;
  Napi::FunctionReference on_stop_fn;
  LrvxMarketDataRecorderHandle handle{nullptr};
  Napi::Env env;

  MarketDataRecorderHookHost(Napi::Env env_, Napi::Object obj)
      : on_trade_fn(takeFn(obj, "onTrade")),
        on_book_fn(takeFn(obj, "onBookUpdate")),
        on_start_fn(takeFn(obj, "onStart")),
        on_stop_fn(takeFn(obj, "onStop")),
        env(env_)
  {
    LrvxMarketDataRecorderCallbacks cb{};
    cb.on_trade = &MarketDataRecorderHookHost::onTradeBridge;
    cb.on_book_update = &MarketDataRecorderHookHost::onBookBridge;
    cb.on_start = &MarketDataRecorderHookHost::onStartBridge;
    cb.on_stop = &MarketDataRecorderHookHost::onStopBridge;
    cb.user_data = this;
    handle = lrvx_market_data_recorder_create(cb);
  }
  ~MarketDataRecorderHookHost()
  {
    if (handle)
    {
      lrvx_market_data_recorder_destroy(handle);
    }
  }
  MarketDataRecorderHookHost(const MarketDataRecorderHookHost&) = delete;
  MarketDataRecorderHookHost& operator=(const MarketDataRecorderHookHost&) = delete;

  static void onTradeBridge(void* ud, const LrvxTradeData* t)
  {
    auto* self = static_cast<MarketDataRecorderHookHost*>(ud);
    if (self->on_trade_fn.IsEmpty())
    {
      return;
    }
    self->on_trade_fn.Call({tradeToJs(self->env, t)});
  }
  static void onBookBridge(void* ud, uint32_t symbol, uint8_t is_snap,
                           const LrvxBookLevel* bids, uint32_t n_bids,
                           const LrvxBookLevel* asks, uint32_t n_asks,
                           int64_t ts)
  {
    auto* self = static_cast<MarketDataRecorderHookHost*>(ud);
    if (self->on_book_fn.IsEmpty())
    {
      return;
    }
    self->on_book_fn.Call({
        Napi::Number::New(self->env, symbol),
        Napi::Boolean::New(self->env, is_snap != 0),
        bookLevelsToBigInt64(self->env, bids, n_bids),
        bookLevelsToBigInt64(self->env, asks, n_asks),
        Napi::BigInt::New(self->env, ts),
    });
  }
  static void onStartBridge(void* ud)
  {
    auto* self = static_cast<MarketDataRecorderHookHost*>(ud);
    if (self->on_start_fn.IsEmpty())
    {
      return;
    }
    self->on_start_fn.Call({});
  }
  static void onStopBridge(void* ud)
  {
    auto* self = static_cast<MarketDataRecorderHookHost*>(ud);
    if (self->on_stop_fn.IsEmpty())
    {
      return;
    }
    self->on_stop_fn.Call({});
  }
};

// ── Executor ────────────────────────────────────────────────────────────

struct ExecutorHost
{
  Napi::FunctionReference submit_fn;
  Napi::FunctionReference cancel_fn;
  Napi::FunctionReference cancel_all_fn;
  Napi::FunctionReference replace_fn;
  Napi::FunctionReference submit_oco_fn;
  Napi::FunctionReference capabilities_fn;
  Napi::FunctionReference on_start_fn;
  Napi::FunctionReference on_stop_fn;
  Napi::Env env;
  LrvxExecutorHandle handle{nullptr};
  HookErrorSink* errors{nullptr};

  ExecutorHost(Napi::Env env_, Napi::Object obj, HookErrorSink* sink = nullptr)
      : submit_fn(takeFn(obj, "submit")),
        cancel_fn(takeFn(obj, "cancel")),
        cancel_all_fn(takeFn(obj, "cancelAll")),
        replace_fn(takeFn(obj, "replace")),
        submit_oco_fn(takeFn(obj, "submitOco")),
        capabilities_fn(takeFn(obj, "capabilities")),
        on_start_fn(takeFn(obj, "onStart")),
        on_stop_fn(takeFn(obj, "onStop")),
        env(env_),
        errors(sink)
  {
    LrvxExecutorCallbacks cb{};
    cb.submit = &ExecutorHost::submitBridge;
    cb.cancel = &ExecutorHost::cancelBridge;
    cb.cancel_all = &ExecutorHost::cancelAllBridge;
    cb.replace = &ExecutorHost::replaceBridge;
    cb.submit_oco = &ExecutorHost::submitOcoBridge;
    cb.capabilities = &ExecutorHost::capabilitiesBridge;
    cb.on_start = &ExecutorHost::onStartBridge;
    cb.on_stop = &ExecutorHost::onStopBridge;
    cb.user_data = this;
    handle = lrvx_executor_create(cb);
  }
  ~ExecutorHost()
  {
    if (handle)
    {
      lrvx_executor_destroy(handle);
    }
  }
  ExecutorHost(const ExecutorHost&) = delete;
  ExecutorHost& operator=(const ExecutorHost&) = delete;

  static void submitBridge(void* ud, const LrvxOrder* o)
  {
    auto* self = static_cast<ExecutorHost*>(ud);
    if (self->submit_fn.IsEmpty())
    {
      return;
    }
    self->submit_fn.Call({orderToJs(self->env, o)});
  }
  static void cancelBridge(void* ud, uint64_t id)
  {
    auto* self = static_cast<ExecutorHost*>(ud);
    if (self->cancel_fn.IsEmpty())
    {
      return;
    }
    self->cancel_fn.Call({Napi::Number::New(self->env, static_cast<double>(id))});
  }
  static void cancelAllBridge(void* ud, uint32_t s)
  {
    auto* self = static_cast<ExecutorHost*>(ud);
    if (self->cancel_all_fn.IsEmpty())
    {
      return;
    }
    self->cancel_all_fn.Call({Napi::Number::New(self->env, s)});
  }
  static void replaceBridge(void* ud, uint64_t old_id, const LrvxOrder* o)
  {
    auto* self = static_cast<ExecutorHost*>(ud);
    if (self->replace_fn.IsEmpty())
    {
      return;
    }
    self->replace_fn.Call({
        Napi::Number::New(self->env, static_cast<double>(old_id)),
        orderToJs(self->env, o),
    });
  }
  static void submitOcoBridge(void* ud, const LrvxOrder* a, const LrvxOrder* b)
  {
    auto* self = static_cast<ExecutorHost*>(ud);
    if (self->submit_oco_fn.IsEmpty())
    {
      return;
    }
    self->submit_oco_fn.Call({orderToJs(self->env, a), orderToJs(self->env, b)});
  }
  // Capabilities is synchronous — the engine queries it inline. It can only
  // be reached from the JS thread, and by construction rather than by
  // convention: a threaded Runner refuses setExecutor outright, so the only
  // runners that hold an executor are the ones whose callbacks run inline.
  static void capabilitiesBridge(void* ud, LrvxExchangeCapabilities* out)
  {
    auto* self = static_cast<ExecutorHost*>(ud);
    *out = LrvxExchangeCapabilities{};
    if (self->capabilities_fn.IsEmpty())
    {
      return;
    }
    // Same boundary as the gates: this is called through a C function
    // pointer, so a throw out of it is reported rather than unwound. An
    // executor that cannot say what it supports supports nothing.
    Napi::Value result;
    try
    {
      result = self->capabilities_fn.Call({});
    }
    catch (const Napi::Error& e)
    {
      reportHookError(self->errors, "executor", "capabilities", e.Message());
      return;
    }
    catch (const std::exception& e)
    {
      reportHookError(self->errors, "executor", "capabilities", e.what());
      return;
    }
    if (!result.IsObject())
    {
      return;
    }
    auto obj = result.As<Napi::Object>();
    auto getBool = [&](const char* k) -> uint8_t
    {
      auto v = obj.Get(k);
      return (v.IsBoolean() && v.As<Napi::Boolean>().Value()) ? 1u : 0u;
    };
    out->supports_stop_market = getBool("stopMarket");
    out->supports_stop_limit = getBool("stopLimit");
    out->supports_take_profit_market = getBool("takeProfitMarket");
    out->supports_take_profit_limit = getBool("takeProfitLimit");
    out->supports_trailing_stop = getBool("trailingStop");
    out->supports_iceberg = getBool("iceberg");
    out->supports_oco = getBool("oco");
    out->supports_gtc = getBool("gtc");
    out->supports_ioc = getBool("ioc");
    out->supports_fok = getBool("fok");
    out->supports_gtd = getBool("gtd");
    out->supports_post_only = getBool("postOnly");
    out->supports_reduce_only = getBool("reduceOnly");
    out->supports_close_position = getBool("closePosition");
    out->supports_batch_submit = getBool("batchSubmit");
  }
  static void onStartBridge(void* ud)
  {
    auto* self = static_cast<ExecutorHost*>(ud);
    if (self->on_start_fn.IsEmpty())
    {
      return;
    }
    self->on_start_fn.Call({});
  }
  static void onStopBridge(void* ud)
  {
    auto* self = static_cast<ExecutorHost*>(ud);
    if (self->on_stop_fn.IsEmpty())
    {
      return;
    }
    self->on_stop_fn.Call({});
  }
};

// ── ExecutionListener ───────────────────────────────────────────────────

struct ExecutionListenerHost
{
  Napi::FunctionReference on_submitted_fn;
  Napi::FunctionReference on_accepted_fn;
  Napi::FunctionReference on_partial_fn;
  Napi::FunctionReference on_filled_fn;
  Napi::FunctionReference on_pending_cancel_fn;
  Napi::FunctionReference on_canceled_fn;
  Napi::FunctionReference on_expired_fn;
  Napi::FunctionReference on_rejected_fn;
  Napi::FunctionReference on_replaced_fn;
  Napi::FunctionReference on_pending_trigger_fn;
  Napi::FunctionReference on_triggered_fn;
  Napi::FunctionReference on_trailing_update_fn;
  LrvxExecutionListenerHandle handle{nullptr};
  Napi::Env env;

  ExecutionListenerHost(Napi::Env env_, Napi::Object obj)
      : on_submitted_fn(takeFn(obj, "onSubmitted")),
        on_accepted_fn(takeFn(obj, "onAccepted")),
        on_partial_fn(takeFn(obj, "onPartiallyFilled")),
        on_filled_fn(takeFn(obj, "onFilled")),
        on_pending_cancel_fn(takeFn(obj, "onPendingCancel")),
        on_canceled_fn(takeFn(obj, "onCanceled")),
        on_expired_fn(takeFn(obj, "onExpired")),
        on_rejected_fn(takeFn(obj, "onRejected")),
        on_replaced_fn(takeFn(obj, "onReplaced")),
        on_pending_trigger_fn(takeFn(obj, "onPendingTrigger")),
        on_triggered_fn(takeFn(obj, "onTriggered")),
        on_trailing_update_fn(takeFn(obj, "onTrailingStopUpdated")),
        env(env_)
  {
    LrvxExecutionListenerCallbacks cb{};
    cb.on_submitted = &ExecutionListenerHost::onSubmittedBridge;
    cb.on_accepted = &ExecutionListenerHost::onAcceptedBridge;
    cb.on_partially_filled = &ExecutionListenerHost::onPartialBridge;
    cb.on_filled = &ExecutionListenerHost::onFilledBridge;
    cb.on_pending_cancel = &ExecutionListenerHost::onPendingCancelBridge;
    cb.on_canceled = &ExecutionListenerHost::onCanceledBridge;
    cb.on_expired = &ExecutionListenerHost::onExpiredBridge;
    cb.on_rejected = &ExecutionListenerHost::onRejectedBridge;
    cb.on_replaced = &ExecutionListenerHost::onReplacedBridge;
    cb.on_pending_trigger = &ExecutionListenerHost::onPendingTriggerBridge;
    cb.on_triggered = &ExecutionListenerHost::onTriggeredBridge;
    cb.on_trailing_stop_updated = &ExecutionListenerHost::onTrailingUpdateBridge;
    cb.user_data = this;
    handle = lrvx_execution_listener_create(cb);
  }
  ~ExecutionListenerHost()
  {
    if (handle)
    {
      lrvx_execution_listener_destroy(handle);
    }
  }
  ExecutionListenerHost(const ExecutionListenerHost&) = delete;
  ExecutionListenerHost& operator=(const ExecutionListenerHost&) = delete;

  static void singleOrderBridge(ExecutionListenerHost* self, Napi::FunctionReference& fn,
                                const LrvxOrder* o)
  {
    if (fn.IsEmpty())
    {
      return;
    }
    fn.Call({orderToJs(self->env, o)});
  }

  static void onSubmittedBridge(void* ud, const LrvxOrder* o)
  {
    auto* self = static_cast<ExecutionListenerHost*>(ud);
    singleOrderBridge(self, self->on_submitted_fn, o);
  }
  static void onAcceptedBridge(void* ud, const LrvxOrder* o)
  {
    auto* self = static_cast<ExecutionListenerHost*>(ud);
    singleOrderBridge(self, self->on_accepted_fn, o);
  }
  static void onFilledBridge(void* ud, const LrvxOrder* o)
  {
    auto* self = static_cast<ExecutionListenerHost*>(ud);
    singleOrderBridge(self, self->on_filled_fn, o);
  }
  static void onPendingCancelBridge(void* ud, const LrvxOrder* o)
  {
    auto* self = static_cast<ExecutionListenerHost*>(ud);
    singleOrderBridge(self, self->on_pending_cancel_fn, o);
  }
  static void onCanceledBridge(void* ud, const LrvxOrder* o)
  {
    auto* self = static_cast<ExecutionListenerHost*>(ud);
    singleOrderBridge(self, self->on_canceled_fn, o);
  }
  static void onExpiredBridge(void* ud, const LrvxOrder* o)
  {
    auto* self = static_cast<ExecutionListenerHost*>(ud);
    singleOrderBridge(self, self->on_expired_fn, o);
  }
  static void onPendingTriggerBridge(void* ud, const LrvxOrder* o)
  {
    auto* self = static_cast<ExecutionListenerHost*>(ud);
    singleOrderBridge(self, self->on_pending_trigger_fn, o);
  }
  static void onTriggeredBridge(void* ud, const LrvxOrder* o)
  {
    auto* self = static_cast<ExecutionListenerHost*>(ud);
    singleOrderBridge(self, self->on_triggered_fn, o);
  }
  static void onPartialBridge(void* ud, const LrvxOrder* o, int64_t fill_qty)
  {
    auto* self = static_cast<ExecutionListenerHost*>(ud);
    if (self->on_partial_fn.IsEmpty())
    {
      return;
    }
    self->on_partial_fn.Call({
        orderToJs(self->env, o),
        Napi::Number::New(self->env, fill_qty / 1e8),
    });
  }
  static void onRejectedBridge(void* ud, const LrvxOrder* o, const char* reason)
  {
    auto* self = static_cast<ExecutionListenerHost*>(ud);
    if (self->on_rejected_fn.IsEmpty())
    {
      return;
    }
    self->on_rejected_fn.Call({
        orderToJs(self->env, o),
        Napi::String::New(self->env, reason ? reason : ""),
    });
  }
  static void onReplacedBridge(void* ud, const LrvxOrder* a, const LrvxOrder* b)
  {
    auto* self = static_cast<ExecutionListenerHost*>(ud);
    if (self->on_replaced_fn.IsEmpty())
    {
      return;
    }
    self->on_replaced_fn.Call({orderToJs(self->env, a), orderToJs(self->env, b)});
  }
  static void onTrailingUpdateBridge(void* ud, const LrvxOrder* o, int64_t new_trigger_raw)
  {
    auto* self = static_cast<ExecutionListenerHost*>(ud);
    if (self->on_trailing_update_fn.IsEmpty())
    {
      return;
    }
    self->on_trailing_update_fn.Call({
        orderToJs(self->env, o),
        Napi::Number::New(self->env, new_trigger_raw / 1e8),
    });
  }
};

// ── Logger callback ─────────────────────────────────────────────────────

// The one owner whose channel is not tied to a runner: the C ABI logger is
// process-scoped, so install/uninstall are its open/close, and a JS log
// callback deliberately keeps the event loop alive while it is installed.
// The instance is a function-local static, so `self` in the queued callback
// below can never dangle and needs no liveness flag.
struct LoggerCallback
{
  Napi::FunctionReference fn;
  std::unique_ptr<TsfnChannel> chan;
  bool active{false};

  void install(Napi::Env env, Napi::Function f)
  {
    if (active)
    {
      uninstall();
    }
    fn = Napi::Persistent(f);
    auto noop = Napi::Function::New(env, [](const Napi::CallbackInfo&) {});
    chan = std::make_unique<TsfnChannel>(env, noop, "lrvx_logger_cb");
    lrvx_set_log_callback(&LoggerCallback::bridge, this);
    active = true;
  }
  void uninstall()
  {
    if (!active)
    {
      return;
    }
    lrvx_set_log_callback(nullptr, nullptr);
    chan->close();
    chan.reset();
    fn.Reset();
    active = false;
  }
  static void bridge(void* ud, int32_t level, const char* msg)
  {
    auto* self = static_cast<LoggerCallback*>(ud);
    if (self->fn.IsEmpty())
    {
      return;
    }
    struct LogPayload
    {
      int32_t level;
      std::string msg;
    };
    auto* p = new LogPayload{level, msg ? std::string(msg) : std::string{}};
    if (!self->chan)
    {
      delete p;
      return;
    }
    self->chan->call(p,
                     [self](Napi::Env env, Napi::Function, LogPayload* lp)
                     {
                       std::unique_ptr<LogPayload> guard(lp);
                       self->fn.Call({
                           Napi::Number::New(env, lp->level),
                           Napi::String::New(env, lp->msg),
                       });
                     });
  }
};

// Global, single instance. Logger is process-scoped in the C ABI.
inline LoggerCallback& globalLogger()
{
  static LoggerCallback inst;
  return inst;
}

inline Napi::Value setLogCallback(const Napi::CallbackInfo& info)
{
  auto env = info.Env();
  if (info.Length() == 0 || info[0].IsNull() || info[0].IsUndefined())
  {
    globalLogger().uninstall();
    return env.Undefined();
  }
  if (!info[0].IsFunction())
  {
    Napi::TypeError::New(env, "expected function or null").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  globalLogger().install(env, info[0].As<Napi::Function>());
  return env.Undefined();
}

// Register setLogCallback on the module exports object. The d.ts gate
// (scripts/check_dts_exports.py) scans *.h files for exports.Set(...)
// patterns, so the registration call lives here rather than in the .cpp.
inline void registerHooks(Napi::Env env, Napi::Object exports)
{
  exports.Set("setLogCallback", Napi::Function::New(env, &setLogCallback));
}

}  // namespace lrvx_node
