// node/src/strategy.h — StrategyRunner and LiveEngine for Node.js

#pragma once

#include <napi.h>

#include "bindings_common.h"
#include "data_ops.h"
#include "error_translator.h"
#include "hooks.h"
#include "latency.h"
#include "lrvx/backtest/runner_fill_settings.h"
#include "lrvx/capi/bridge_strategy.h"
#include "lrvx/capi/lrvx_capi.h"
#include "lrvx/capi/order_status_names.hpp"
#include "lrvx/capi/order_type_names.hpp"
#include "lrvx/engine/symbol_registry.h"
#include "lrvx/execution/order_price_rule.h"
#include "positions.h"
#include "run_trace.h"
#include "tsfn_util.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace node_lrvx
{

using namespace lrvx;

// ──────────────────────────────────────────────────────────────
// SymbolRegistryNode — owns a SymbolRegistry, exposes addSymbol
// ──────────────────────────────────────────────────────────────

class SymbolRegistryNode : public Napi::ObjectWrap<SymbolRegistryNode>
{
 public:
  static Napi::FunctionReference& Ctor()
  {
    static Napi::FunctionReference ref;
    return ref;
  }

  static Napi::Function Init(Napi::Env env)
  {
    Napi::Function fn = DefineClass(env, "SymbolRegistry",
                                    {
                                        InstanceMethod("addSymbol", &SymbolRegistryNode::addSymbol),
                                        InstanceMethod("symbolCount", &SymbolRegistryNode::symbolCount),
                                        InstanceMethod("tickSize", &SymbolRegistryNode::tickSize),
                                        InstanceMethod("tickSizeDeclared", &SymbolRegistryNode::tickSizeDeclared),
                                        InstanceMethod("lotSize", &SymbolRegistryNode::lotSize),
                                    });
    Ctor() = Napi::Persistent(fn);
    Ctor().SuppressDestruct();
    return fn;
  }

  // The registry behind `v`. Unwrap() alone takes any wrapped object: a plain
  // one threw a bare "Invalid argument" and another class's instance was read
  // as a registry.
  static SymbolRegistry* of(const Napi::Value& v, const char* what)
  {
    if (!v.IsObject() || Ctor().IsEmpty() || !v.As<Napi::Object>().InstanceOf(Ctor().Value()))
    {
      throw Napi::TypeError::New(v.Env(), std::string(what) + " must be a SymbolRegistry");
    }
    return Unwrap(v.As<Napi::Object>())->get();
  }

  SymbolRegistryNode(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<SymbolRegistryNode>(info)
  {
    _reg = std::make_unique<SymbolRegistry>();
  }

  SymbolRegistry* get() { return _reg.get(); }

 private:
  std::unique_ptr<SymbolRegistry> _reg;

  Napi::Value addSymbol(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    std::string ex = argString(info[0], "addSymbol: exchange");
    std::string sym = argString(info[1], "addSymbol: symbol");
    // A tick passed here is the venue's, and a backtest refuses orders priced
    // off it. Left out, the book uses 0.01 and order prices are not checked.
    const bool hasTick = info.Length() > 2 && !info[2].IsUndefined();
    double tickSize = hasTick ? argNumber(info[2], "addSymbol: tickSize") : 0.01;
    lrvx::SymbolInfo si;
    si.exchange = ex;
    si.symbol = sym;
    si.tickSize = lrvx::Price::fromDouble(tickSize);
    si.tickSizeKnown = hasTick;
    // A lot passed here is the venue's quantity step, and a backtest refuses
    // an order whose quantity is not a whole number of it. Left out,
    // quantities are not checked.
    const bool hasLot = info.Length() > 3 && !info[3].IsUndefined();
    if (hasLot)
    {
      const double lotSize = argNumber(info[3], "addSymbol: lotSize");
      si.lotSize = lrvx::Quantity::fromDouble(lotSize);
      if (si.lotSize.raw() <= 0)
      {
        Napi::RangeError::New(env, "addSymbol: lotSize must be a positive quantity")
            .ThrowAsJavaScriptException();
        return env.Undefined();
      }
    }
    uint32_t id = static_cast<uint32_t>(_reg->registerSymbol(si));

    // Return a Symbol object: { id, exchange, name, tickSize, valueOf() → id }
    // valueOf() makes it transparently usable as a number everywhere.
    auto obj = Napi::Object::New(env);
    obj.Set("id", Napi::Number::New(env, id));
    obj.Set("exchange", Napi::String::New(env, ex));
    obj.Set("name", Napi::String::New(env, sym));
    obj.Set("tickSize", Napi::Number::New(env, tickSize));
    if (hasLot)
    {
      obj.Set("lotSize", Napi::Number::New(env, si.lotSize.toDouble()));
    }
    else
    {
      obj.Set("lotSize", env.Null());
    }
    obj.Set("valueOf", Napi::Function::New(env,
                                           [](const Napi::CallbackInfo& i) -> Napi::Value
                                           {
                                             return i.This().As<Napi::Object>().Get("id");
                                           }));
    obj.Set("toString", Napi::Function::New(env,
                                            [ex, sym, id](const Napi::CallbackInfo& i) -> Napi::Value
                                            {
                                              return Napi::String::New(i.Env(),
                                                                       "Symbol(" + ex + ":" + sym + ", id=" + std::to_string(id) + ")");
                                            }));
    return obj;
  }

  Napi::Value symbolCount(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(), static_cast<double>(_reg->size()));
  }

  // The entry for a Symbol object or a numeric id; throws a RangeError and
  // returns nullopt when the id is not registered.
  std::optional<lrvx::SymbolInfo> entryFor(const Napi::CallbackInfo& info, const char* method)
  {
    auto env = info.Env();
    uint32_t id = 0;
    if (info.Length() > 0 && info[0].IsObject())
    {
      id = argObject(info[0], "symbolCount: argument 1").Get("id").ToNumber().Uint32Value();
    }
    else if (info.Length() > 0 && info[0].IsNumber())
    {
      id = argUint32(info[0], "symbolCount: argument 1");
    }
    else
    {
      Napi::TypeError::New(env, std::string(method) + ": expected a Symbol or a symbol id")
          .ThrowAsJavaScriptException();
      return std::nullopt;
    }
    auto entry = _reg->getSymbolInfo(id);
    if (!entry)
    {
      Napi::RangeError::New(env, std::string(method) + ": symbol id " + std::to_string(id) +
                                     " is not registered")
          .ThrowAsJavaScriptException();
    }
    return entry;
  }

  Napi::Value tickSize(const Napi::CallbackInfo& info)
  {
    const auto entry = entryFor(info, "tickSize");
    if (!entry)
    {
      return info.Env().Undefined();
    }
    return Napi::Number::New(info.Env(), entry->tickSize.toDouble());
  }

  Napi::Value tickSizeDeclared(const Napi::CallbackInfo& info)
  {
    const auto entry = entryFor(info, "tickSizeDeclared");
    if (!entry)
    {
      return info.Env().Undefined();
    }
    return Napi::Boolean::New(info.Env(), lrvx::orderTickRaw(*entry) != 0);
  }

  Napi::Value lotSize(const Napi::CallbackInfo& info)
  {
    const auto entry = entryFor(info, "lotSize");
    if (!entry)
    {
      return info.Env().Undefined();
    }
    const int64_t lot = lrvx::orderLotRaw(*entry);
    if (lot == 0)
    {
      return info.Env().Null();
    }
    return Napi::Number::New(info.Env(), lrvx::Quantity::fromRaw(lot).toDouble());
  }
};

// ──────────────────────────────────────────────────────────────
// Helpers
// ──────────────────────────────────────────────────────────────

// Accept Symbol object or plain number → uint32_t id. Anything else (a
// missing argument, a string, an object with no numeric id, a fraction, a
// negative) is a TypeError; it used to be read as whatever the conversion
// made of it, or to surface as a bare "A number was expected".
inline uint32_t symId(const Napi::Value& v)
{
  if (v.IsObject())
  {
    return argUint32(v.As<Napi::Object>().Get("id"), "symbol: the id of a Symbol");
  }
  return argUint32(v, "symbol: a Symbol or a symbol id");
}

// The arguments of an emit.* call. A missing argument or one of the wrong
// type is a TypeError that names the call, thrown before anything is
// emitted; reading it unchecked either threw a bare "A number was expected"
// or, for an order id that is NaN, negative or fractional, emitted with
// whatever the conversion produced.
[[noreturn]] inline void throwEmitUsage(const Napi::CallbackInfo& i, const char* usage,
                                        const char* what)
{
  throw Napi::TypeError::New(i.Env(), std::string(usage) + " expected: " + what);
}

inline double emitNumber(const Napi::CallbackInfo& i, size_t at, const char* usage,
                         const char* name)
{
  if (i.Length() <= at || !i[at].IsNumber())
  {
    throwEmitUsage(i, usage, (std::string(name) + " is a number").c_str());
  }
  return i[at].As<Napi::Number>().DoubleValue();
}

// An order id is the Number the emit that placed the order returned and
// order events carry; a BigInt is taken too, as Runner.orderClosed takes one.
inline uint64_t emitOrderId(const Napi::CallbackInfo& i, size_t at, const char* usage)
{
  if (i.Length() > at && i[at].IsBigInt())
  {
    bool lossless = false;
    const uint64_t id = i[at].As<Napi::BigInt>().Uint64Value(&lossless);
    if (lossless)
    {
      return id;
    }
  }
  else if (i.Length() > at && i[at].IsNumber())
  {
    const double v = i[at].As<Napi::Number>().DoubleValue();
    if (v >= 0.0 && v <= 9007199254740992.0 && v == std::floor(v))
    {
      return static_cast<uint64_t>(v);
    }
  }
  throwEmitUsage(i, usage, "orderId is a non-negative integer Number or a BigInt");
}

// The optional symbol argument: a Symbol, a symbol id, or left out (the
// strategy's first symbol).
inline uint32_t emitSymbol(const Napi::CallbackInfo& i, size_t at, const std::vector<uint32_t>& syms,
                           const char* usage)
{
  if (i.Length() <= at || i[at].IsUndefined())
  {
    return syms.empty() ? 0u : syms[0];
  }
  const Napi::Value v = i[at];
  if (v.IsNumber() || (v.IsObject() && v.As<Napi::Object>().Get("id").IsNumber()))
  {
    return symId(v);
  }
  throwEmitUsage(i, usage, "symbol is a Symbol or a symbol id");
}

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

// ──────────────────────────────────────────────────────────────
// NodeStrategy — wraps a JS object with on_trade / on_book_update
// callbacks, owns a BridgeStrategy.
// ──────────────────────────────────────────────────────────────

struct NodeStrategyHost : TsfnHost
{
  Napi::Env env;
  Napi::FunctionReference on_trade_fn;
  Napi::FunctionReference on_book_fn;
  Napi::FunctionReference on_touch_fn;
  Napi::FunctionReference on_bar_fn;
  Napi::FunctionReference on_start_fn;
  Napi::FunctionReference on_stop_fn;
  Napi::FunctionReference on_fill_fn;
  Napi::FunctionReference on_order_update_fn;
  Napi::FunctionReference on_queue_position_change_fn;
  Napi::FunctionReference on_market_position_change_fn;
  Napi::ObjectReference emitter;
  std::vector<uint32_t> syms;
  std::unique_ptr<BridgeStrategy> bridge;

  // What every emit.* calls through. The last error is cleared first, so
  // lastError() read right after an emit is '' unless that emit left a
  // reason: a live runner refused a modify, or a pre-trade gate dropped the
  // signal.
  BridgeStrategy* emitting() const
  {
    lrvx_clear_last_error();
    return bridge.get();
  }

  // Set by enableThreaded() when host is used with LiveEngine.
  // onTrade / onBook / onStart / onStop queue via the channel instead of
  // calling V8 directly (which is illegal from a C++ consumer thread).
  //
  // Read from the bus thread, written from the JS thread before the host can
  // be reached by one, so it is atomic for the same reason the handler mask
  // below is.
  std::atomic<bool> _threaded{false};

  // Which callbacks the current strategy object provides.
  //
  // The bus thread used to answer that question by calling IsEmpty() on the
  // FunctionReference itself, while replaceStrategy move-assigned the same
  // references from the JS thread. Move-assignment deletes the old
  // reference, so the bus thread was reading a handle being freed underneath
  // it. A bitmask keeps the question answerable from the bus thread and
  // leaves every FunctionReference to the JS thread, which is the only thread
  // allowed to touch a V8 handle anyway.
  //
  // A replacement is not instantaneous: an event can be queued against the
  // outgoing mask and run against the incoming callbacks. That is what
  // swapping a strategy on a live engine means, and it is harmless -- the
  // consumer re-reads the reference on the JS thread and re-checks IsEmpty.
  enum HandlerBit : uint16_t
  {
    kHandlerTrade = 1u << 0,
    kHandlerBook = 1u << 1,
    kHandlerBar = 1u << 2,
    kHandlerStart = 1u << 3,
    kHandlerStop = 1u << 4,
    kHandlerFill = 1u << 5,
    kHandlerOrderUpdate = 1u << 6,
    kHandlerQueuePosition = 1u << 7,
    kHandlerMarketPosition = 1u << 8,
    kHandlerTouch = 1u << 9,
  };
  std::atomic<uint16_t> _handlers{0};

  bool hasHandler(uint16_t bit) const
  {
    return (_handlers.load(std::memory_order_acquire) & bit) != 0;
  }

  struct TradeCallData
  {
    LrvxSymbolContext ctx;
    LrvxTradeData trade;
    NodeStrategyHost* host;
    TsfnOwnerPtr owner;
  };
  struct BookCallData
  {
    LrvxSymbolContext ctx;
    NodeStrategyHost* host;
    TsfnOwnerPtr owner;
  };
  struct BarCallData
  {
    LrvxSymbolContext ctx;
    LrvxBarData bar;
    NodeStrategyHost* host;
    TsfnOwnerPtr owner;
  };
  enum OrderEventKind : uint8_t
  {
    kOrderEventKindFill = 0,
    kOrderEventKindUpdate = 1,
    kOrderEventKindQueuePosition = 2,
    kOrderEventKindMarketPosition = 3,
  };

  struct OrderEventCallData
  {
    LrvxSymbolContext ctx;
    LrvxOrderEventData ev;
    // reject_reason is a borrowed pointer in the C ABI struct; copy
    // it onto the heap so the TSFN consumer can outlive the caller.
    std::string reject_reason_owned;
    NodeStrategyHost* host;
    OrderEventKind kind;
    TsfnOwnerPtr owner;
  };

  void enableThreaded() { _threaded.store(true, std::memory_order_release); }

  // JS thread, before this host can be reached from a bus thread.
  void openChannel()
  {
    if (_threaded.load(std::memory_order_acquire))
    {
      TsfnHost::openChannel("lrvx_strategy_cb");
    }
  }

  // Recompute the handler bitmask from the references currently held.
  // JS thread only; the store publishes the whole set to the bus thread.
  void publishHandlerMask()
  {
    uint16_t mask = 0;
    auto bit = [&](const Napi::FunctionReference& fn, uint16_t b)
    {
      if (!fn.IsEmpty())
      {
        mask |= b;
      }
    };
    bit(on_trade_fn, kHandlerTrade);
    bit(on_book_fn, kHandlerBook);
    bit(on_bar_fn, kHandlerBar);
    bit(on_start_fn, kHandlerStart);
    bit(on_stop_fn, kHandlerStop);
    bit(on_fill_fn, kHandlerFill);
    bit(on_order_update_fn, kHandlerOrderUpdate);
    bit(on_queue_position_change_fn, kHandlerQueuePosition);
    bit(on_market_position_change_fn, kHandlerMarketPosition);
    bit(on_touch_fn, kHandlerTouch);
    _handlers.store(mask, std::memory_order_release);
  }

  // Replace the strategy's callbacks with those from a new strategy
  // object. Bridge subscriptions, in-flight orders, and connector
  // sessions stay intact.
  //
  // Must run on the V8 thread, which is where every read of these
  // references also happens: the bus thread decides whether a callback exists
  // from the handler bitmask instead of touching the references themselves.
  void replaceStrategy(Napi::Object new_strategy_obj)
  {
    auto get = [&](const char* name) -> Napi::FunctionReference
    {
      auto val = new_strategy_obj.Get(name);
      if (val.IsFunction())
      {
        return Napi::Persistent(val.As<Napi::Function>());
      }
      return {};
    };

    if (on_stop_fn)
    {
      on_stop_fn.Call({});
    }
    on_trade_fn = get("onTrade");
    on_book_fn = get("onBookUpdate");
    on_bar_fn = get("onBar");
    on_start_fn = get("onStart");
    on_stop_fn = get("onStop");
    on_fill_fn = get("onFill");
    on_order_update_fn = get("onOrderUpdate");
    on_queue_position_change_fn = get("onQueuePositionChange");
    on_market_position_change_fn = get("onMarketPositionChange");
    on_touch_fn = get("onTouch");
    publishHandlerMask();
    if (on_start_fn)
    {
      on_start_fn.Call({});
    }
  }

  NodeStrategyHost(Napi::Env env_, Napi::Object strategy_obj,
                   SymbolRegistry* reg, uint32_t id,
                   const std::vector<uint32_t>& syms_)
      : TsfnHost(env_), env(env_), syms(syms_)
  {
    auto get = [&](const char* name) -> Napi::FunctionReference
    {
      auto val = strategy_obj.Get(name);
      if (val.IsFunction())
      {
        return Napi::Persistent(val.As<Napi::Function>());
      }
      return {};
    };
    on_trade_fn = get("onTrade");
    on_book_fn = get("onBookUpdate");
    on_bar_fn = get("onBar");
    on_start_fn = get("onStart");
    on_stop_fn = get("onStop");
    on_fill_fn = get("onFill");
    on_order_update_fn = get("onOrderUpdate");
    on_queue_position_change_fn = get("onQueuePositionChange");
    on_market_position_change_fn = get("onMarketPositionChange");
    on_touch_fn = get("onTouch");
    publishHandlerMask();

    LrvxStrategyCallbacks cbs{};
    cbs.user_data = this;
    cbs.on_trade = &NodeStrategyHost::onTrade;
    cbs.on_book = &NodeStrategyHost::onBook;
    cbs.on_bar = &NodeStrategyHost::onBar;
    cbs.on_start = &NodeStrategyHost::onStart;
    cbs.on_stop = &NodeStrategyHost::onStop;
    cbs.on_fill = &NodeStrategyHost::onFill;
    cbs.on_order_update = &NodeStrategyHost::onOrderUpdate;
    cbs.on_queue_position_change = &NodeStrategyHost::onQueuePositionChange;
    cbs.on_market_position_change = &NodeStrategyHost::onMarketPositionChange;
    cbs.on_touch = &NodeStrategyHost::onTouch;

    bridge = std::make_unique<BridgeStrategy>(
        static_cast<SubscriberId>(id), syms, *reg, cbs);

    // Build emitter: order emission functions bound to this bridge.
    // Created once per host; passed to every onTrade / onBook callback.
    auto em = Napi::Object::New(env_);
    uint32_t defaultSym = syms.empty() ? 0 : syms[0];

    em.Set("marketBuy", Napi::Function::New(env_, [](const Napi::CallbackInfo& i) -> Napi::Value
                                            {
        static const char* const usage = "emit.marketBuy(qty, symbol?)";
        auto* h   = static_cast<NodeStrategyHost*>(i.Data());
        double qty = emitNumber(i, 0, usage, "qty");
        uint32_t sym = emitSymbol(i, 1, h->syms, usage);
        return orderIdToJs(i.Env(), h->emitting()->publicEmitMarketBuy(sym, Quantity::fromDouble(qty))); }, "marketBuy", this));

    em.Set("marketSell", Napi::Function::New(env_, [](const Napi::CallbackInfo& i) -> Napi::Value
                                             {
        static const char* const usage = "emit.marketSell(qty, symbol?)";
        auto* h   = static_cast<NodeStrategyHost*>(i.Data());
        double qty = emitNumber(i, 0, usage, "qty");
        uint32_t sym = emitSymbol(i, 1, h->syms, usage);
        return orderIdToJs(i.Env(), h->emitting()->publicEmitMarketSell(sym, Quantity::fromDouble(qty))); }, "marketSell", this));

    em.Set("limitBuy", Napi::Function::New(env_, [](const Napi::CallbackInfo& i) -> Napi::Value
                                           {
        static const char* const usage = "emit.limitBuy(price, qty, symbol?)";
        auto* h    = static_cast<NodeStrategyHost*>(i.Data());
        double price = emitNumber(i, 0, usage, "price");
        double qty   = emitNumber(i, 1, usage, "qty");
        uint32_t sym = emitSymbol(i, 2, h->syms, usage);
        return orderIdToJs(i.Env(), h->emitting()->publicEmitLimitBuy(sym, Price::fromDouble(price), Quantity::fromDouble(qty))); }, "limitBuy", this));

    em.Set("limitSell", Napi::Function::New(env_, [](const Napi::CallbackInfo& i) -> Napi::Value
                                            {
        static const char* const usage = "emit.limitSell(price, qty, symbol?)";
        auto* h    = static_cast<NodeStrategyHost*>(i.Data());
        double price = emitNumber(i, 0, usage, "price");
        double qty   = emitNumber(i, 1, usage, "qty");
        uint32_t sym = emitSymbol(i, 2, h->syms, usage);
        return orderIdToJs(i.Env(), h->emitting()->publicEmitLimitSell(sym, Price::fromDouble(price), Quantity::fromDouble(qty))); }, "limitSell", this));

    em.Set("provideLiquidity", Napi::Function::New(env_, [](const Napi::CallbackInfo& i) -> Napi::Value
                                                   {
        static const char* const usage =
            "emit.provideLiquidity(priceLower, priceUpper, liquidity, symbol?)";
        auto* h    = static_cast<NodeStrategyHost*>(i.Data());
        double lo  = emitNumber(i, 0, usage, "priceLower");
        double hi  = emitNumber(i, 1, usage, "priceUpper");
        double liq = emitNumber(i, 2, usage, "liquidity");
        uint32_t sym = emitSymbol(i, 3, h->syms, usage);
        return orderIdToJs(i.Env(), h->emitting()->publicEmitProvideLiquidity(sym, Price::fromDouble(lo), Price::fromDouble(hi),
                                              Quantity::fromDouble(liq))); }, "provideLiquidity", this));

    em.Set("withdrawLiquidity", Napi::Function::New(env_, [](const Napi::CallbackInfo& i) -> Napi::Value
                                                    {
        static const char* const usage = "emit.withdrawLiquidity(liquidity, symbol?)";
        auto* h    = static_cast<NodeStrategyHost*>(i.Data());
        double liq = emitNumber(i, 0, usage, "liquidity");
        uint32_t sym = emitSymbol(i, 1, h->syms, usage);
        return orderIdToJs(i.Env(), h->emitting()->publicEmitWithdrawLiquidity(sym, Quantity::fromDouble(liq))); }, "withdrawLiquidity", this));

    em.Set("cancel", Napi::Function::New(env_, [](const Napi::CallbackInfo& i) -> Napi::Value
                                         {
        auto* h = static_cast<NodeStrategyHost*>(i.Data());
        const uint64_t orderId = emitOrderId(i, 0, "emit.cancel(orderId)");
        h->emitting()->publicEmitCancel(orderId);
        return i.Env().Undefined(); }, "cancel", this));

    em.Set("modify", Napi::Function::New(env_, [](const Napi::CallbackInfo& i) -> Napi::Value
                                         {
        static const char* const usage = "emit.modify(orderId, newPrice, newQty)";
        auto* h = static_cast<NodeStrategyHost*>(i.Data());
        const uint64_t orderId = emitOrderId(i, 0, usage);
        const double newPrice = emitNumber(i, 1, usage, "newPrice");
        const double newQty = emitNumber(i, 2, usage, "newQty");
        // lastError() after the call is '' unless a live runner refused the
        // modify (an id its executor record does not hold) or a gate dropped
        // it: emitting() clears the last error first.
        lrvx_emit_modify(h->emitting(), orderId, lrvx_price_from_double(newPrice),
                         lrvx_quantity_from_double(newQty));
        return i.Env().Undefined(); }, "modify", this));

    em.Set("closePosition", Napi::Function::New(env_, [](const Napi::CallbackInfo& i) -> Napi::Value
                                                {
        auto* h  = static_cast<NodeStrategyHost*>(i.Data());
        uint32_t sym = emitSymbol(i, 0, h->syms, "emit.closePosition(symbol?)");
        return orderIdToJs(i.Env(), h->emitting()->publicEmitClosePosition(sym)); }, "closePosition", this));

    // Multi-TF alignment helpers: read the per-(symbol, timeframe)
    // ring of the most recent closed bars without bookkeeping in
    // user code. `lastClosedBar(symbolId, barType, param)` returns
    // the most recent or `null`; `lastNClosedBars(...)` returns up
    // to `n` bars oldest first.
    auto buildBar = [](Napi::Env env, const Bar& b)
    {
      auto o = Napi::Object::New(env);
      o.Set("open", Napi::Number::New(env, b.open.toDouble()));
      o.Set("high", Napi::Number::New(env, b.high.toDouble()));
      o.Set("low", Napi::Number::New(env, b.low.toDouble()));
      o.Set("close", Napi::Number::New(env, b.close.toDouble()));
      o.Set("volume", Napi::Number::New(env, b.volume.toDouble()));
      o.Set("startNs",
            Napi::BigInt::New(env, static_cast<int64_t>(b.startTime.time_since_epoch().count())));
      o.Set("endNs",
            Napi::BigInt::New(env, static_cast<int64_t>(b.endTime.time_since_epoch().count())));
      return o;
    };
    em.Set("lastClosedBar", Napi::Function::New(env_, [buildBar](const Napi::CallbackInfo& i) -> Napi::Value
                                                {
        auto* h = static_cast<NodeStrategyHost*>(i.Data());
        uint32_t sym = symId(i[0]);
        uint8_t bt = static_cast<uint8_t>(argUint32(i[1], "lastClosedBar: barType"));
        uint64_t param = argUint64(i[2], "lastClosedBar: barTypeParam");
        auto bar = h->bridge->lastClosedBar(sym, static_cast<BarType>(bt), param);
        if (!bar){ return i.Env().Null();
}
        return buildBar(i.Env(), *bar); }, "lastClosedBar", this));
    em.Set("lastNClosedBars", Napi::Function::New(env_, [buildBar](const Napi::CallbackInfo& i) -> Napi::Value
                                                  {
        auto* h = static_cast<NodeStrategyHost*>(i.Data());
        uint32_t sym = symId(i[0]);
        uint8_t bt = static_cast<uint8_t>(argUint32(i[1], "lastNClosedBars: barType"));
        uint64_t param = argUint64(i[2], "lastNClosedBars: barTypeParam");
        size_t n = argUint32(i[3], "lastNClosedBars: n");
        auto bars = h->bridge->lastNClosedBars(sym, static_cast<BarType>(bt), param, n);
        Napi::Array out = Napi::Array::New(i.Env(), bars.size());
        for (size_t k = 0; k < bars.size(); ++k){ out.Set(static_cast<uint32_t>(k), buildBar(i.Env(), bars[k]));
}
        return out; }, "lastNClosedBars", this));
    em.Set("setBarRingCapacity", Napi::Function::New(env_, [](const Napi::CallbackInfo& i) -> Napi::Value
                                                     {
        auto* h = static_cast<NodeStrategyHost*>(i.Data());
        size_t n = argUint32(i[0], "setBarRingCapacity: capacity");
        h->bridge->setBarRingCapacity(n);
        return i.Env().Undefined(); }, "setBarRingCapacity", this));

    (void)defaultSym;
    emitter = Napi::Persistent(em);
    emitter.SuppressDestruct();
  }

  static void buildCtxObj(Napi::Env env, Napi::Object& o, const LrvxSymbolContext* ctx)
  {
    o.Set("symbolId", Napi::Number::New(env, ctx->symbol_id));
    o.Set("position", Napi::Number::New(env, lrvx_quantity_to_double(ctx->position_raw)));
    o.Set("lastTradePrice", Napi::Number::New(env, lrvx_price_to_double(ctx->last_trade_price_raw)));
    // null when the side has no best level. A book may be quoted at exactly
    // zero, so 0 is a price here, never a stand-in for "no quote"; the
    // presence flags on the snapshot are what say.
    const bool both = ctx->book.has_bid && ctx->book.has_ask;
    o.Set("bestBid", ctx->book.has_bid
                         ? Napi::Value(Napi::Number::New(env, lrvx_price_to_double(ctx->book.bid_price_raw)))
                         : Napi::Value(env.Null()));
    o.Set("bestAsk", ctx->book.has_ask
                         ? Napi::Value(Napi::Number::New(env, lrvx_price_to_double(ctx->book.ask_price_raw)))
                         : Napi::Value(env.Null()));
    o.Set("midPrice", both ? Napi::Value(Napi::Number::New(env, lrvx_price_to_double(ctx->book.mid_raw)))
                           : Napi::Value(env.Null()));
    // Absolute nanosecond clock readings cross as BigInt. lastTouchNs can be
    // the fresher of the two: a TOP_OF_BOOK frame writes it and leaves
    // lastUpdateNs alone.
    o.Set("lastUpdateNs", Napi::BigInt::New(env, static_cast<int64_t>(ctx->last_update_ns)));
    o.Set("lastTouchNs", Napi::BigInt::New(env, static_cast<int64_t>(ctx->last_touch_ns)));
  }

  static void callOnTrade(Napi::Env env, Napi::Function, TradeCallData* d)
  {
    // Guards against the leak on exception: previously a bare `delete d`
    // at the end of the function never ran if on_trade_fn.Call() threw
    // (NODE_ADDON_API_CPP_EXCEPTIONS_ALL turns a JS throw into a C++
    // exception here), so every strategy exception leaked its CallData.
    std::unique_ptr<TradeCallData> guard(d);
    // The host is gone and the queue was not emptied by its release: free
    // the payload and stop, rather than reading through a dead pointer.
    if (d->owner->retired())
    {
      return;
    }
    auto* self = d->host;
    if (!self->on_trade_fn.IsEmpty())
    {
      auto ctxObj = Napi::Object::New(env);
      buildCtxObj(env, ctxObj, &d->ctx);
      auto tradeObj = Napi::Object::New(env);
      tradeObj.Set("symbol", Napi::Number::New(env, d->trade.symbol));
      tradeObj.Set("price", Napi::Number::New(env, lrvx_price_to_double(d->trade.price_raw)));
      tradeObj.Set("qty", Napi::Number::New(env, lrvx_quantity_to_double(d->trade.quantity_raw)));
      tradeObj.Set("isBuy", Napi::Boolean::New(env, d->trade.is_buy != 0));
      tradeObj.Set("side", Napi::String::New(env, d->trade.is_buy ? "buy" : "sell"));
      // TradeData.timestampNs is declared bigint in index.d.ts: a real ns
      // timestamp does not survive a double (see toInt64Ns in data_ops.h).
      tradeObj.Set("timestampNs", Napi::BigInt::New(env, static_cast<int64_t>(d->trade.exchange_ts_ns)));
      self->on_trade_fn.Call({ctxObj, tradeObj, self->emitter.Value()});
    }
  }

  static void callOnBook(Napi::Env env, Napi::Function, BookCallData* d)
  {
    std::unique_ptr<BookCallData> guard(d);
    if (d->owner->retired())
    {
      return;
    }
    auto* self = d->host;
    if (!self->on_book_fn.IsEmpty())
    {
      auto ctxObj = Napi::Object::New(env);
      buildCtxObj(env, ctxObj, &d->ctx);
      self->on_book_fn.Call({ctxObj, self->emitter.Value()});
    }
  }

  static void callOnTouch(Napi::Env env, Napi::Function, BookCallData* d)
  {
    std::unique_ptr<BookCallData> guard(d);
    if (d->owner->retired())
    {
      return;
    }
    auto* self = d->host;
    if (!self->on_touch_fn.IsEmpty())
    {
      auto ctxObj = Napi::Object::New(env);
      buildCtxObj(env, ctxObj, &d->ctx);
      self->on_touch_fn.Call({ctxObj, self->emitter.Value()});
    }
  }

  static void buildBarObj(Napi::Env env, Napi::Object& o, const LrvxBarData* bar)
  {
    o.Set("symbol", Napi::Number::New(env, bar->symbol));
    o.Set("barType", Napi::Number::New(env, bar->bar_type));
    o.Set("barTypeParam", Napi::Number::New(env, static_cast<double>(bar->bar_type_param)));
    o.Set("open", Napi::Number::New(env, lrvx_price_to_double(bar->open_raw)));
    o.Set("high", Napi::Number::New(env, lrvx_price_to_double(bar->high_raw)));
    o.Set("low", Napi::Number::New(env, lrvx_price_to_double(bar->low_raw)));
    o.Set("close", Napi::Number::New(env, lrvx_price_to_double(bar->close_raw)));
    o.Set("volume", Napi::Number::New(env, lrvx_quantity_to_double(bar->volume_raw)));
    o.Set("buyVolume", Napi::Number::New(env, lrvx_quantity_to_double(bar->buy_volume_raw)));
    // BigInt for the same reason as TradeData.timestampNs above: a real ns
    // reading does not survive a double.
    o.Set("startTimeNs", Napi::BigInt::New(env, static_cast<int64_t>(bar->start_time_ns)));
    o.Set("endTimeNs", Napi::BigInt::New(env, static_cast<int64_t>(bar->end_time_ns)));
    o.Set("closeReason", Napi::Number::New(env, bar->close_reason));
  }

  static void callOnBar(Napi::Env env, Napi::Function, BarCallData* d)
  {
    std::unique_ptr<BarCallData> guard(d);
    if (d->owner->retired())
    {
      return;
    }
    auto* self = d->host;
    if (!self->on_bar_fn.IsEmpty())
    {
      auto ctxObj = Napi::Object::New(env);
      buildCtxObj(env, ctxObj, &d->ctx);
      auto barObj = Napi::Object::New(env);
      buildBarObj(env, barObj, &d->bar);
      self->on_bar_fn.Call({ctxObj, barObj, self->emitter.Value()});
    }
  }

  static void onTrade(void* ud, const LrvxSymbolContext* ctx,
                      const LrvxTradeData* trade)
  {
    auto* self = static_cast<NodeStrategyHost*>(ud);
    if (!self->hasHandler(kHandlerTrade))
    {
      return;
    }

    if (self->_threaded.load(std::memory_order_acquire))
    {
      auto* d = new TradeCallData{*ctx, *trade, self, self->owner()};
      self->post(d, &NodeStrategyHost::callOnTrade);
    }
    else
    {
      auto env = self->env;
      auto ctxObj = Napi::Object::New(env);
      buildCtxObj(env, ctxObj, ctx);
      auto tradeObj = Napi::Object::New(env);
      tradeObj.Set("symbol", Napi::Number::New(env, trade->symbol));
      tradeObj.Set("price", Napi::Number::New(env, lrvx_price_to_double(trade->price_raw)));
      tradeObj.Set("qty", Napi::Number::New(env, lrvx_quantity_to_double(trade->quantity_raw)));
      tradeObj.Set("isBuy", Napi::Boolean::New(env, trade->is_buy != 0));
      tradeObj.Set("side", Napi::String::New(env, trade->is_buy ? "buy" : "sell"));
      // See callOnTrade above: timestampNs is declared bigint.
      tradeObj.Set("timestampNs", Napi::BigInt::New(env, static_cast<int64_t>(trade->exchange_ts_ns)));
      self->on_trade_fn.Call({ctxObj, tradeObj, self->emitter.Value()});
    }
  }

  static void onBook(void* ud, const LrvxSymbolContext* ctx,
                     const LrvxBookData* /*book*/)
  {
    auto* self = static_cast<NodeStrategyHost*>(ud);
    if (!self->hasHandler(kHandlerBook))
    {
      return;
    }

    if (self->_threaded.load(std::memory_order_acquire))
    {
      auto* d = new BookCallData{*ctx, self, self->owner()};
      self->post(d, &NodeStrategyHost::callOnBook);
    }
    else
    {
      auto env = self->env;
      auto ctxObj = Napi::Object::New(env);
      buildCtxObj(env, ctxObj, ctx);
      self->on_book_fn.Call({ctxObj, self->emitter.Value()});
    }
  }

  static void onTouch(void* ud, const LrvxSymbolContext* ctx,
                      const LrvxBookData* /*book*/)
  {
    auto* self = static_cast<NodeStrategyHost*>(ud);
    if (!self->hasHandler(kHandlerTouch))
    {
      return;
    }

    if (self->_threaded.load(std::memory_order_acquire))
    {
      auto* d = new BookCallData{*ctx, self, self->owner()};
      self->post(d, &NodeStrategyHost::callOnTouch);
    }
    else
    {
      auto env = self->env;
      auto ctxObj = Napi::Object::New(env);
      buildCtxObj(env, ctxObj, ctx);
      self->on_touch_fn.Call({ctxObj, self->emitter.Value()});
    }
  }

  static void onBar(void* ud, const LrvxSymbolContext* ctx,
                    const LrvxBarData* bar)
  {
    auto* self = static_cast<NodeStrategyHost*>(ud);
    if (!self->hasHandler(kHandlerBar))
    {
      return;
    }

    if (self->_threaded.load(std::memory_order_acquire))
    {
      auto* d = new BarCallData{*ctx, *bar, self, self->owner()};
      self->post(d, &NodeStrategyHost::callOnBar);
    }
    else
    {
      auto env = self->env;
      auto ctxObj = Napi::Object::New(env);
      buildCtxObj(env, ctxObj, ctx);
      auto barObj = Napi::Object::New(env);
      buildBarObj(env, barObj, bar);
      self->on_bar_fn.Call({ctxObj, barObj, self->emitter.Value()});
    }
  }

  // ── Order-event hooks ─────────────────────────────────────────────
  //
  // Mirror the pybind11 surface: status names match
  // `LrvxOrderEventStatus` (NEW, PARTIALLY_FILLED, FILLED, CANCELED,
  // REJECTED, ...). `onFill` is called only on terminal fill events
  // (status == PARTIALLY_FILLED or FILLED); `onOrderUpdate` fires on
  // every status change including fills, so a strategy that only
  // wants the lifecycle and is happy to inspect status itself can
  // implement the latter.

  static const char* orderEventStatusName(uint8_t s)
  {
    return lrvx::capi::orderEventStatusName(s);
  }

  static const char* marketPositionName(uint8_t p)
  {
    switch (p)
    {
      case 1:
        return "best";
      case 2:
        return "behind_best";
      case 3:
        return "mid_spread";
      case 4:
        return "level_empty";
      case 5:
        return "crossed";
      default:
        return "";
    }
  }

  static const char* orderTypeName(uint8_t t)
  {
    return lrvx::capi::orderTypeNameUpper(t);
  }

  static void buildOrderEventObj(Napi::Env env, Napi::Object& o,
                                 const LrvxOrderEventData* ev,
                                 const std::string* reject_owned)
  {
    o.Set("orderId", Napi::Number::New(env, static_cast<double>(ev->order_id)));
    o.Set("symbolId", Napi::Number::New(env, ev->symbol_id));
    o.Set("side", Napi::String::New(env, ev->side == 0 ? "buy" : "sell"));
    o.Set("orderType", Napi::String::New(env, orderTypeName(ev->order_type)));
    o.Set("status", Napi::String::New(env, orderEventStatusName(ev->status)));
    o.Set("fillQty", Napi::Number::New(env, lrvx_quantity_to_double(ev->fill_qty_raw)));
    o.Set("fillPrice", Napi::Number::New(env, lrvx_price_to_double(ev->fill_price_raw)));
    // Clock readings cross as BigInt: a double steps 256 ns at today's values.
    o.Set("exchangeTsNs", Napi::BigInt::New(env, ev->exchange_ts_ns));
    o.Set("queueAhead", Napi::Number::New(env, lrvx_quantity_to_double(ev->queue_ahead_raw)));
    o.Set("queueTotal", Napi::Number::New(env, lrvx_quantity_to_double(ev->queue_total_raw)));
    const char* mpName = marketPositionName(ev->market_position);
    if (mpName[0] != '\0')
    {
      o.Set("marketPosition", Napi::String::New(env, mpName));
    }
    else
    {
      o.Set("marketPosition", env.Null());
    }
    o.Set("distanceToBestTicks",
          Napi::Number::New(env, static_cast<double>(ev->distance_to_best_ticks)));
    o.Set("submittedAtNs", Napi::BigInt::New(env, ev->submitted_at_ns));
    o.Set("acceptedAtNs", Napi::BigInt::New(env, ev->accepted_at_ns));
    o.Set("firstFillAtNs", Napi::BigInt::New(env, ev->first_fill_at_ns));
    o.Set("lastFillAtNs", Napi::BigInt::New(env, ev->last_fill_at_ns));
    o.Set("canceledAtNs", Napi::BigInt::New(env, ev->canceled_at_ns));
    o.Set("rejectedAtNs", Napi::BigInt::New(env, ev->rejected_at_ns));
    o.Set("triggeredAtNs", Napi::BigInt::New(env, ev->triggered_at_ns));
    o.Set("expiredAtNs", Napi::BigInt::New(env, ev->expired_at_ns));
    const bool isMaker = ev->is_maker != 0;
    o.Set("isMaker", Napi::Boolean::New(env, isMaker));
    const bool isFill = (ev->status == 3 /* PARTIALLY_FILLED */ ||
                         ev->status == 4 /* FILLED */);
    o.Set("fillRole",
          isFill ? Napi::String::New(env, isMaker ? "maker" : "taker").As<Napi::Value>()
                 : env.Null().As<Napi::Value>());
    if (reject_owned && !reject_owned->empty())
    {
      o.Set("rejectReason", Napi::String::New(env, *reject_owned));
    }
    else if (ev->reject_reason)
    {
      o.Set("rejectReason", Napi::String::New(env, ev->reject_reason));
    }
    else
    {
      o.Set("rejectReason", env.Null());
    }
  }

  // Bus-thread-safe counterpart of pickFnRef: answers "does this kind have a
  // handler" without touching a FunctionReference.
  static uint16_t handlerBit(OrderEventKind kind)
  {
    switch (kind)
    {
      case kOrderEventKindFill:
        return kHandlerFill;
      case kOrderEventKindQueuePosition:
        return kHandlerQueuePosition;
      case kOrderEventKindMarketPosition:
        return kHandlerMarketPosition;
      case kOrderEventKindUpdate:
      default:
        return kHandlerOrderUpdate;
    }
  }

  // JS thread only.
  static Napi::FunctionReference& pickFnRef(NodeStrategyHost* self, OrderEventKind kind)
  {
    switch (kind)
    {
      case kOrderEventKindFill:
        return self->on_fill_fn;
      case kOrderEventKindQueuePosition:
        return self->on_queue_position_change_fn;
      case kOrderEventKindMarketPosition:
        return self->on_market_position_change_fn;
      case kOrderEventKindUpdate:
      default:
        return self->on_order_update_fn;
    }
  }

  static void callOnOrderEvent(Napi::Env env, Napi::Function, OrderEventCallData* d)
  {
    std::unique_ptr<OrderEventCallData> guard(d);
    if (d->owner->retired())
    {
      return;
    }
    auto* self = d->host;
    auto& fn = pickFnRef(self, d->kind);
    if (!fn.IsEmpty())
    {
      auto ctxObj = Napi::Object::New(env);
      buildCtxObj(env, ctxObj, &d->ctx);
      auto evObj = Napi::Object::New(env);
      buildOrderEventObj(env, evObj, &d->ev, &d->reject_reason_owned);
      fn.Call({ctxObj, evObj, self->emitter.Value()});
    }
  }

  static void dispatchOrderEvent(NodeStrategyHost* self,
                                 const LrvxSymbolContext* ctx,
                                 const LrvxOrderEventData* ev,
                                 OrderEventKind kind)
  {
    if (!self->hasHandler(handlerBit(kind)))
    {
      return;
    }
    if (self->_threaded.load(std::memory_order_acquire))
    {
      auto* d = new OrderEventCallData{
          *ctx, *ev,
          ev->reject_reason ? std::string(ev->reject_reason) : std::string{},
          self, kind, self->owner()};
      self->post(d, &NodeStrategyHost::callOnOrderEvent);
    }
    else
    {
      auto& fn = pickFnRef(self, kind);
      auto env = self->env;
      auto ctxObj = Napi::Object::New(env);
      buildCtxObj(env, ctxObj, ctx);
      auto evObj = Napi::Object::New(env);
      buildOrderEventObj(env, evObj, ev, nullptr);
      fn.Call({ctxObj, evObj, self->emitter.Value()});
    }
  }

  static void onFill(void* ud, const LrvxSymbolContext* ctx,
                     const LrvxOrderEventData* ev)
  {
    dispatchOrderEvent(static_cast<NodeStrategyHost*>(ud), ctx, ev, kOrderEventKindFill);
  }

  static void onOrderUpdate(void* ud, const LrvxSymbolContext* ctx,
                            const LrvxOrderEventData* ev)
  {
    dispatchOrderEvent(static_cast<NodeStrategyHost*>(ud), ctx, ev, kOrderEventKindUpdate);
  }

  static void onQueuePositionChange(void* ud, const LrvxSymbolContext* ctx,
                                    const LrvxOrderEventData* ev)
  {
    dispatchOrderEvent(static_cast<NodeStrategyHost*>(ud), ctx, ev,
                       kOrderEventKindQueuePosition);
  }

  static void onMarketPositionChange(void* ud, const LrvxSymbolContext* ctx,
                                     const LrvxOrderEventData* ev)
  {
    dispatchOrderEvent(static_cast<NodeStrategyHost*>(ud), ctx, ev,
                       kOrderEventKindMarketPosition);
  }

  struct LifecycleCallData
  {
    NodeStrategyHost* host;
    bool is_start;
    TsfnOwnerPtr owner;
  };

  static void callLifecycle(Napi::Env, Napi::Function, LifecycleCallData* d)
  {
    std::unique_ptr<LifecycleCallData> guard(d);
    if (d->owner->retired())
    {
      return;
    }
    auto& fn = d->is_start ? d->host->on_start_fn : d->host->on_stop_fn;
    if (!fn.IsEmpty())
    {
      fn.Call({});
    }
  }

  static void onStart(void* ud)
  {
    auto* self = static_cast<NodeStrategyHost*>(ud);
    if (!self->hasHandler(kHandlerStart))
    {
      return;
    }
    if (self->_threaded.load(std::memory_order_acquire))
    {
      auto* d = new LifecycleCallData{self, true, self->owner()};
      self->post(d, &NodeStrategyHost::callLifecycle);
    }
    else
    {
      self->on_start_fn.Call({});
    }
  }

  static void onStop(void* ud)
  {
    auto* self = static_cast<NodeStrategyHost*>(ud);
    if (!self->hasHandler(kHandlerStop))
    {
      return;
    }
    if (self->_threaded.load(std::memory_order_acquire))
    {
      auto* d = new LifecycleCallData{self, false, self->owner()};
      self->post(d, &NodeStrategyHost::callLifecycle);
    }
    else
    {
      self->on_stop_fn.Call({});
    }
  }
};

// ──────────────────────────────────────────────────────────────
// BacktestRunnerNode
// ──────────────────────────────────────────────────────────────

class BacktestRunnerNode : public Napi::ObjectWrap<BacktestRunnerNode>
{
 public:
  static Napi::Object Init(Napi::Env env)
  {
    return DefineClass(
        env, "BacktestRunner",
        {
            InstanceMethod("setStrategy", &BacktestRunnerNode::setStrategy),
            InstanceMethod("runCsv", &BacktestRunnerNode::runCsv),
            InstanceMethod("runOhlcv", &BacktestRunnerNode::runOhlcv),
            InstanceMethod("runBars", &BacktestRunnerNode::runBars),
            InstanceMethod("runTape", &BacktestRunnerNode::runTape),
            InstanceMethod("runTapes", &BacktestRunnerNode::runTapes),
            InstanceMethod("runTapeBars", &BacktestRunnerNode::runTapeBars),
            InstanceMethod("runTapeBarsOnTicks", &BacktestRunnerNode::runTapeBarsOnTicks),
            InstanceMethod("setExecutor", &BacktestRunnerNode::setExecutor),
            InstanceMethod("addExecutionListener", &BacktestRunnerNode::addExecutionListener),
            InstanceMethod("addJourneyTracer", &BacktestRunnerNode::addJourneyTracer),
            InstanceMethod("equityCurve", &BacktestRunnerNode::equityCurve),
            InstanceMethod("trades", &BacktestRunnerNode::trades),
        });
  }

  // new BacktestRunner(registry, feeRate, initialCapital, options?)
  //
  // options carries the built-in executor's slippage and latency, the same
  // BacktestConfig fields Python takes as keywords, camelCased. Without it
  // (or with every key at its default) the runner is the plain one.
  BacktestRunnerNode(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<BacktestRunnerNode>(info)
  {
    auto env = info.Env();
    _reg = SymbolRegistryNode::of(info[0], "BacktestRunner: registry");
    double fee = argNumber(info[1], "BacktestRunner: feeRate");
    double cap = argNumber(info[2], "BacktestRunner: initialCapital");
    if (info.Length() < 4 || info[3].IsUndefined())
    {
      _handle = lrvx_backtest_runner_create(static_cast<LrvxRegistryHandle>(_reg), fee, cap);
      return;
    }
    if (!info[3].IsObject())
    {
      throw Napi::TypeError::New(env, "BacktestRunner: options must be an object");
    }
    auto opts = argObject(info[3], "BacktestRunner: options");
    static const char* const kKeys[] = {
        "slippageModel", "slippageTicks", "slippageTickSize", "slippageBps",
        "slippageImpactCoeff", "submitAckLatencyNs", "submitAckJitterNs",
        "cancelAckLatencyNs", "cancelAckJitterNs", "cancelAckSeed",
        "replaceAckLatencyNs", "replaceAckJitterNs", "latency"};
    auto names = opts.GetPropertyNames();
    for (uint32_t i = 0; i < names.Length(); ++i)
    {
      const std::string key = names.Get(i).As<Napi::String>().Utf8Value();
      bool known = false;
      for (const char* k : kKeys)
      {
        known = known || key == k;
      }
      if (!known)
      {
        throw lrvxError(env, "BacktestRunner: unknown option '" + key + "'");
      }
    }
    const auto num = [&](const char* key, double fallback)
    {
      auto v = opts.Get(key);
      if (v.IsUndefined())
      {
        return fallback;
      }
      if (!v.IsNumber())
      {
        throw Napi::TypeError::New(env, std::string("BacktestRunner: ") + key +
                                            " must be a number");
      }
      return v.As<Napi::Number>().DoubleValue();
    };
    const auto i64 = [&](const char* key, double fallback)
    { return static_cast<int64_t>(num(key, fallback)); };

    int32_t model = 0;
    auto modelV = opts.Get("slippageModel");
    if (!modelV.IsUndefined())
    {
      const std::string m = modelV.ToString().Utf8Value();
      const auto parsed = lrvx::slippageModelFromName(m);
      if (!parsed)
      {
        throw lrvxError(env, "BacktestRunner: slippageModel must be one of " +
                                 lrvx::slippageModelNameList() + "; got '" + m + "'");
      }
      model = static_cast<int32_t>(*parsed);
    }
    LrvxLatencyModelHandle latency = nullptr;
    auto latencyV = opts.Get("latency");
    if (!latencyV.IsUndefined() && !latencyV.IsNull())
    {
      latency = latencyHandleOf(latencyV);
      if (latency == nullptr)
      {
        throw Napi::TypeError::New(
            env,
            "BacktestRunner: latency must be a ConstantLatency, GaussianLatency, "
            "ExponentialLatency or EmpiricalLatency");
      }
      // The runner borrows the model; keep the JS object (and its handle) alive.
      _latencyRef = Napi::Persistent(latencyV.As<Napi::Object>());
    }
    _handle = lrvx_backtest_runner_create_with_fill_settings(
        static_cast<LrvxRegistryHandle>(_reg), fee, cap, model,
        static_cast<int32_t>(num("slippageTicks", 0)), num("slippageTickSize", 0.0),
        num("slippageBps", 0.0), num("slippageImpactCoeff", 0.0), i64("submitAckLatencyNs", 0),
        i64("submitAckJitterNs", 0), i64("cancelAckLatencyNs", 0), i64("cancelAckJitterNs", 0),
        static_cast<uint64_t>(num("cancelAckSeed", 42)), i64("replaceAckLatencyNs", 0),
        i64("replaceAckJitterNs", 0), latency);
    if (_handle == nullptr)
    {
      throw lrvxError(env, "BacktestRunner: " + camelCaseNames(lrvx_last_error_message()));
    }
  }

  ~BacktestRunnerNode()
  {
    if (_handle)
    {
      lrvx_backtest_runner_destroy(_handle);
    }
  }

 private:
  LrvxBacktestRunnerHandle _handle{nullptr};
  SymbolRegistry* _reg{nullptr};
  std::unique_ptr<NodeStrategyHost> _host;
  Napi::ObjectReference _latencyRef;

  static Napi::Error lrvxError(Napi::Env env, const std::string& msg)
  {
    auto err = Napi::Error::New(env, msg);
    err.Value().Set("code", Napi::String::New(env, "E_VAL_002"));
    err.Value().Set("name", Napi::String::New(env, "LrvxError"));
    return err;
  }

  // The C ABI names arguments the way Python spells them; this binding's
  // options are camelCase, so the message is translated before it is shown.
  static std::string camelCaseNames(std::string msg)
  {
    static const std::pair<const char*, const char*> kNames[] = {
        {"slippage_impact_coeff", "slippageImpactCoeff"},
        {"slippage_tick_size", "slippageTickSize"},
        {"slippage_ticks", "slippageTicks"},
        {"slippage_bps", "slippageBps"},
        {"slippage_model", "slippageModel"},
        {"submit_ack_latency_ns", "submitAckLatencyNs"},
        {"submit_ack_jitter_ns", "submitAckJitterNs"},
        {"cancel_ack_latency_ns", "cancelAckLatencyNs"},
        {"cancel_ack_jitter_ns", "cancelAckJitterNs"},
        {"replace_ack_latency_ns", "replaceAckLatencyNs"},
        {"replace_ack_jitter_ns", "replaceAckJitterNs"},
        {"interval_ns", "intervalSeconds"},
        {"lrvx_backtest_runner_create_with_fill_settings: ", ""},
        {"lrvx_backtest_runner_run_tape_bars_on_ticks: ", ""},
        {"lrvx_backtest_runner_run_tape_bars: ", ""},
        {"lrvx_backtest_runner_run_tapes: ", ""},
        {"lrvx_backtest_runner_run_tape: ", ""},
        {"lrvx_backtest_runner_run_csv: ", ""},
        {"lrvx_backtest_runner_run_ohlcv: ", ""},
        {"lrvx_backtest_runner_run_bars: ", ""},
    };
    for (const auto& [from, to] : kNames)
    {
      for (auto pos = msg.find(from); pos != std::string::npos; pos = msg.find(from, pos))
      {
        msg.replace(pos, std::strlen(from), to);
        pos += std::strlen(to);
      }
    }
    return msg;
  }

  // runner.runTapeBars(path, intervalSeconds, symbol?) -> stats. The tape
  // replayed as time bars with runBars' fill rules; throws on error.
  Napi::Value runTapeBars(const Napi::CallbackInfo& info)
  {
    return runTapeAsBars(info, "runTapeBars", lrvx_backtest_runner_run_tape_bars);
  }

  // runner.runTapeBarsOnTicks(path, intervalSeconds, symbol?) -> stats. The
  // same bars reach onBar, and the executor matches on the tape's records
  // with runTape's rules; throws on error.
  Napi::Value runTapeBarsOnTicks(const Napi::CallbackInfo& info)
  {
    return runTapeAsBars(info, "runTapeBarsOnTicks",
                         lrvx_backtest_runner_run_tape_bars_on_ticks);
  }

  using TapeBarsRun = int (*)(LrvxBacktestRunnerHandle, const char*, int64_t, const char*,
                              LrvxBacktestStats*);

  Napi::Value runTapeAsBars(const Napi::CallbackInfo& info, const std::string& method,
                            TapeBarsRun runFn)
  {
    auto env = info.Env();
    if (info.Length() < 2 || !info[0].IsString() || !info[1].IsNumber())
    {
      Napi::TypeError::New(env, "BacktestRunner." + method +
                                    "(path, intervalSeconds, symbol?): expected "
                                    "a string and a number")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    const std::string path = argString(info[0], "runTapeBarsOnTicks: argument 1");
    const double seconds = argNumber(info[1], "runTapeBarsOnTicks: argument 2");
    // Converted the way aggregateTimeBars converts its interval.
    const auto ns = std::isfinite(seconds)
                        ? std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::duration<double>(seconds))
                              .count()
                        : 0;
    if (ns <= 0)
    {
      lrvxError(env,
                "BacktestRunner." + method +
                    ": intervalSeconds must be a finite number of "
                    "seconds of at least one nanosecond, got " +
                    std::to_string(seconds))
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    std::string symbol;
    if (info.Length() > 2 && info[2].IsString())
    {
      symbol = argString(info[2], "runTapeBarsOnTicks: argument 3");
    }
    LrvxBacktestStats s{};
    if (!runFn(_handle, path.c_str(), ns, symbol.empty() ? nullptr : symbol.c_str(), &s))
    {
      lrvxError(env, camelCaseNames(lrvx_last_error_message())).ThrowAsJavaScriptException();
      return env.Undefined();
    }
    return statsToJs(env, s);
  }

  // runner.setStrategy(strategyObj) — plain JS object with callbacks
  Napi::Value setStrategy(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    auto obj = argObject(info[0], "setStrategy: argument 1");
    std::vector<uint32_t> syms;
    auto symsV = obj.Get("symbols");
    if (symsV.IsArray())
    {
      auto arr = symsV.As<Napi::Array>();
      for (uint32_t i = 0; i < arr.Length(); ++i)
      {
        syms.push_back(symId(arr.Get(i)));
      }
    }
    _host = std::make_unique<NodeStrategyHost>(env, obj, _reg, 1, syms);
    lrvx_backtest_runner_set_strategy(_handle,
                                      static_cast<LrvxStrategyHandle>(_host->bridge.get()));
    return env.Undefined();
  }

  // runner.runTape(path) → stats object. `path` is a `.lrvx`
  // directory written by `lrvx tape record` or `scripts/backfill_to_tape.py`.
  Napi::Value runTape(const Napi::CallbackInfo& info)
  {
    return tryLrvx(info.Env(),
                   [&]() -> Napi::Value
                   {
                     std::string path = argString(info[0], "runTape: argument 1");
                     LrvxBacktestStats s{};
                     int ok = lrvx_backtest_runner_run_tape(_handle, path.c_str(), &s);
                     if (!ok)
                     {
                       lrvxError(info.Env(), camelCaseNames(lrvx_last_error_message()))
                           .ThrowAsJavaScriptException();
                       return info.Env().Undefined();
                     }
                     return statsToJs(info.Env(), s);
                   });
  }

  // runner.runTapes(paths) → stats object. Merges N `.lrvx` directories
  // on read via MergedTapeReader and feeds the merged event stream through
  // the same engine pipeline as runTape. Symbols are rekeyed by
  // (metadata.exchange, name) so two venues remain distinct.
  Napi::Value runTapes(const Napi::CallbackInfo& info)
  {
    return tryLrvx(info.Env(),
                   [&]() -> Napi::Value
                   {
                     auto env = info.Env();
                     if (info.Length() == 0 || !info[0].IsArray())
                     {
                       Napi::TypeError::New(
                           env, "BacktestRunner.runTapes(paths): expected string[]")
                           .ThrowAsJavaScriptException();
                       return env.Undefined();
                     }
                     auto arr = argArray(info[0], "runTapes: argument 1");
                     uint32_t n = arr.Length();
                     std::vector<std::string> owned;
                     owned.reserve(n);
                     for (uint32_t i = 0; i < n; ++i)
                     {
                       owned.emplace_back(argString(arr.Get(i), "runTapes: a path"));
                     }
                     std::vector<const char*> cstrs;
                     cstrs.reserve(n);
                     for (const auto& s : owned)
                     {
                       cstrs.push_back(s.c_str());
                     }
                     LrvxBacktestStats s{};
                     int ok = lrvx_backtest_runner_run_tapes(
                         _handle, cstrs.data(), n, &s);
                     if (!ok)
                     {
                       lrvxError(env, camelCaseNames(lrvx_last_error_message()))
                           .ThrowAsJavaScriptException();
                       return env.Undefined();
                     }
                     return statsToJs(env, s);
                   });
  }

  // runner.runCsv(path, symbol) → stats object; throws on error
  Napi::Value runCsv(const Napi::CallbackInfo& info)
  {
    return tryLrvx(info.Env(), [&]() -> Napi::Value
                   {
      std::string path = argString(info[0], "runCsv: argument 1");
      std::string symbol = argString(info[1], "runCsv: argument 2");
      LrvxBacktestStats s{};
      int ok = lrvx_backtest_runner_run_csv(_handle, path.c_str(), symbol.c_str(), &s);
      if (!ok)
      {
        lrvxError(info.Env(), camelCaseNames(lrvx_last_error_message()))
            .ThrowAsJavaScriptException();
        return info.Env().Undefined();
      }
      return statsToJs(info.Env(), s); });
  }

  // runner.runOhlcv(tsArray, closeArray, symbol) → stats object
  Napi::Value runOhlcv(const Napi::CallbackInfo& info)
  {
    return tryLrvx(info.Env(), [&]() -> Napi::Value
                   {
      auto tsArr = argBigInt64Array(info[0], "runOhlcv: argument 1");
      auto closeArr = argFloat64Array(info[1], "runOhlcv: argument 2");
      if (!requireSameLength(info.Env(), "runOhlcv",
                             {tsArr.ElementLength(), closeArr.ElementLength()}))
      {
        return info.Env().Undefined();
      }
      std::string symbol = argString(info[2], "runOhlcv: argument 3");
      uint32_t n = static_cast<uint32_t>(tsArr.ElementLength());
      LrvxBacktestStats s{};
      int ok = lrvx_backtest_runner_run_ohlcv(_handle,
                                              reinterpret_cast<const int64_t*>(tsArr.Data()),
                                              closeArr.Data(),
                                              n, symbol.c_str(), &s);
      if (!ok)
      {
        lrvxError(info.Env(), camelCaseNames(lrvx_last_error_message()))
            .ThrowAsJavaScriptException();
        return info.Env().Undefined();
      }
      return statsToJs(info.Env(), s); });
  }

  // runner.runBars(startNs, endNs, open, high, low, close, volume,
  //                symbol, barType?, barTypeParam?) → stats
  // All arrays are typed (BigInt64Array for ts, Float64Array for OHLCV).
  Napi::Value runBars(const Napi::CallbackInfo& info)
  {
    return tryLrvx(info.Env(), [&]() -> Napi::Value
                   {
      auto startNs = argBigInt64Array(info[0], "runBars: argument 1");
      auto endNs = argBigInt64Array(info[1], "runBars: argument 2");
      auto openA = argFloat64Array(info[2], "runBars: argument 3");
      auto highA = argFloat64Array(info[3], "runBars: argument 4");
      auto lowA = argFloat64Array(info[4], "runBars: argument 5");
      auto closeA = argFloat64Array(info[5], "runBars: argument 6");
      auto volA = argFloat64Array(info[6], "runBars: argument 7");
      // The C entry point takes one element count for all eight columns and
      // indexes every one of them over [0, n). n came from startNs alone, so
      // a `high` one element short was an out-of-bounds read of the other
      // seven and the run reported stats as if nothing had happened. Reject
      // before the call, the way indicators.h and aggregators.h already do,
      // so a mismatch delivers no bars at all rather than a partial run over
      // whatever followed the short array.
      if (!requireSameLength(info.Env(), "runBars",
                             {startNs.ElementLength(), endNs.ElementLength(),
                              openA.ElementLength(), highA.ElementLength(),
                              lowA.ElementLength(), closeA.ElementLength(),
                              volA.ElementLength()}))
      {
        return info.Env().Undefined();
      }
      std::string symbol = argString(info[7], "runBars: argument 8");
      uint8_t barType = info.Length() > 8 && !info[8].IsUndefined()
                            ? static_cast<uint8_t>(argUint32(info[8], "runBars: barType"))
                            : 0;
      uint64_t barTypeParam = info.Length() > 9 && !info[9].IsUndefined()
                                  ? argUint64(info[9], "runBars: barTypeParam")
                                  : 0;
      uint32_t n = static_cast<uint32_t>(startNs.ElementLength());
      LrvxBacktestStats s{};
      int ok = lrvx_backtest_runner_run_bars(
          _handle,
          reinterpret_cast<const int64_t*>(startNs.Data()),
          reinterpret_cast<const int64_t*>(endNs.Data()),
          openA.Data(), highA.Data(), lowA.Data(), closeA.Data(), volA.Data(),
          n, symbol.c_str(), barType, barTypeParam, &s);
      if (!ok)
      {
        lrvxError(info.Env(), camelCaseNames(lrvx_last_error_message()))
            .ThrowAsJavaScriptException();
        return info.Env().Undefined();
      }
      return statsToJs(info.Env(), s); });
  }

  // runner.equityCurve() → { timestampNs: BigInt64Array,
  //                           equity: Float64Array,
  //                           drawdownPct: Float64Array }
  Napi::Value equityCurve(const Napi::CallbackInfo& info)
  {
    return tryLrvx(info.Env(), [&]() -> Napi::Value
                   {
      auto env = info.Env();
      LrvxBacktestResultHandle res = lrvx_backtest_runner_take_result(_handle);
      if (!res) {
        throw lrvx::LrvxError(
            "E_RUN_002",
            "BacktestRunner.equityCurve() called before any run* completed.");
      }
      const uint32_t total = lrvx_backtest_result_equity_curve(res, nullptr, 0);
      std::vector<LrvxEquityPoint> buf(total);
      if (total > 0) {
        lrvx_backtest_result_equity_curve(res, buf.data(), total);
      }
      auto tsArr = Napi::BigInt64Array::New(env, total);
      auto eqArr = Napi::Float64Array::New(env, total);
      auto ddArr = Napi::Float64Array::New(env, total);
      for (uint32_t i = 0; i < total; ++i) {
        tsArr.Data()[i] = buf[i].timestamp_ns;
        eqArr.Data()[i] = buf[i].equity;
        ddArr.Data()[i] = buf[i].drawdown_pct;
      }
      lrvx_backtest_result_destroy(res);
      auto obj = Napi::Object::New(env);
      obj.Set("timestampNs", tsArr);
      obj.Set("equity", eqArr);
      obj.Set("drawdownPct", ddArr);
      return obj; });
  }

  // runner.trades() → { symbol: Uint32Array, side: Uint8Array,
  //                     entryPrice/exitPrice/quantity/pnl/fee: Float64Array,
  //                     entryTimeNs/exitTimeNs: BigInt64Array }
  Napi::Value trades(const Napi::CallbackInfo& info)
  {
    return tryLrvx(info.Env(), [&]() -> Napi::Value
                   {
      auto env = info.Env();
      LrvxBacktestResultHandle res = lrvx_backtest_runner_take_result(_handle);
      if (!res) {
        throw lrvx::LrvxError(
            "E_RUN_002",
            "BacktestRunner.trades() called before any run* completed.");
      }
      const uint32_t total = lrvx_backtest_result_trades(res, nullptr, 0);
      std::vector<LrvxBacktestTrade> buf(total);
      if (total > 0) {
        lrvx_backtest_result_trades(res, buf.data(), total);
      }
      auto symArr = Napi::Uint32Array::New(env, total);
      auto sideArr = Napi::Uint8Array::New(env, total);
      auto epArr = Napi::Float64Array::New(env, total);
      auto xpArr = Napi::Float64Array::New(env, total);
      auto qtyArr = Napi::Float64Array::New(env, total);
      auto pnlArr = Napi::Float64Array::New(env, total);
      auto feeArr = Napi::Float64Array::New(env, total);
      auto etsArr = Napi::BigInt64Array::New(env, total);
      auto xtsArr = Napi::BigInt64Array::New(env, total);
      for (uint32_t i = 0; i < total; ++i) {
        symArr.Data()[i] = buf[i].symbol;
        sideArr.Data()[i] = buf[i].side;
        epArr.Data()[i] = buf[i].entry_price;
        xpArr.Data()[i] = buf[i].exit_price;
        qtyArr.Data()[i] = buf[i].quantity;
        pnlArr.Data()[i] = buf[i].pnl;
        feeArr.Data()[i] = buf[i].fee;
        etsArr.Data()[i] = buf[i].entry_time_ns;
        xtsArr.Data()[i] = buf[i].exit_time_ns;
      }
      lrvx_backtest_result_destroy(res);
      auto obj = Napi::Object::New(env);
      obj.Set("symbol", symArr);
      obj.Set("side", sideArr);
      obj.Set("entryPrice", epArr);
      obj.Set("exitPrice", xpArr);
      obj.Set("quantity", qtyArr);
      obj.Set("pnl", pnlArr);
      obj.Set("fee", feeArr);
      obj.Set("entryTimeNs", etsArr);
      obj.Set("exitTimeNs", xtsArr);
      return obj; });
  }

  static Napi::Object statsToJs(Napi::Env env, const LrvxBacktestStats& s)
  {
    auto obj = Napi::Object::New(env);
    obj.Set("totalTrades", Napi::Number::New(env, static_cast<double>(s.totalTrades)));
    obj.Set("winningTrades", Napi::Number::New(env, static_cast<double>(s.winningTrades)));
    obj.Set("losingTrades", Napi::Number::New(env, static_cast<double>(s.losingTrades)));
    obj.Set("initialCapital", Napi::Number::New(env, s.initialCapital));
    obj.Set("finalCapital", Napi::Number::New(env, s.finalCapital));
    obj.Set("netPnl", Napi::Number::New(env, s.netPnl));
    obj.Set("totalFees", Napi::Number::New(env, s.totalFees));
    obj.Set("returnPct", Napi::Number::New(env, s.returnPct));
    obj.Set("maxDrawdown", Napi::Number::New(env, s.maxDrawdown));
    obj.Set("maxDrawdownPct", Napi::Number::New(env, s.maxDrawdownPct));
    obj.Set("sharpeRatio", Napi::Number::New(env, s.sharpeRatio));
    obj.Set("sortinoRatio", Napi::Number::New(env, s.sortinoRatio));
    obj.Set("calmarRatio", Napi::Number::New(env, s.calmarRatio));
    obj.Set("winRate", Napi::Number::New(env, s.winRate));
    obj.Set("avgWin", Napi::Number::New(env, s.avgWin));
    obj.Set("avgLoss", Napi::Number::New(env, s.avgLoss));
    obj.Set("profitFactor", Napi::Number::New(env, s.profitFactor));
    setTradeTimeStats(env, obj, s);
    // Option-quote and pool-state records a tape run read and delivered to
    // nobody; 0 for a run that read no tape.
    obj.Set("skippedOptionQuotes",
            Napi::Number::New(env, static_cast<double>(s.optionQuoteCount)));
    obj.Set("skippedPoolStates", Napi::Number::New(env, static_cast<double>(s.poolStateCount)));
    return obj;
  }

  // ── Hook setters ────────────────────────────────────────────────────

  std::unique_ptr<lrvx_node::ExecutorHost> _executor_host;
  std::vector<std::unique_ptr<lrvx_node::ExecutionListenerHost>> _listener_hosts;

  Napi::Value setExecutor(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    if (info.Length() == 0 || info[0].IsNull() || info[0].IsUndefined())
    {
      lrvx_backtest_runner_set_executor(_handle, nullptr);
      _executor_host.reset();
      return env.Undefined();
    }
    auto host = std::make_unique<lrvx_node::ExecutorHost>(env, argObject(info[0], "setExecutor: argument 1"));
    lrvx_clear_last_error();
    lrvx_backtest_runner_set_executor(_handle, host->handle);
    if (lrvx_last_error_code() != 0)
    {
      // Refused: the runner was built with slippage / latency options.
      lrvxError(env, "BacktestRunner.setExecutor: " + camelCaseNames(lrvx_last_error_message()))
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    _executor_host = std::move(host);
    return env.Undefined();
  }

  Napi::Value addExecutionListener(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    if (info.Length() == 0 || !info[0].IsObject())
    {
      return env.Undefined();
    }
    auto host =
        std::make_unique<lrvx_node::ExecutionListenerHost>(env, argObject(info[0], "addExecutionListener: argument 1"));
    lrvx_backtest_runner_add_execution_listener(_handle, host->handle);
    _listener_hosts.push_back(std::move(host));
    return env.Undefined();
  }

  // runner.addJourneyTracer(tracer): the tracer records every order event of
  // the runs that follow. The runner keeps the tracer object alive.
  Napi::Value addJourneyTracer(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    if (info.Length() == 0 || !info[0].IsObject())
    {
      Napi::TypeError::New(env,
                           "BacktestRunner.addJourneyTracer(tracer): expected an "
                           "OrderJourneyTracer")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    auto obj = argObject(info[0], "addJourneyTracer: argument 1");
    auto* tracer = Napi::ObjectWrap<OrderJourneyTracerWrap>::Unwrap(obj);
    lrvx_backtest_runner_add_journey_tracer(_handle, tracer->handle());
    _tracers.push_back(Napi::Persistent(obj));
    return env.Undefined();
  }

  std::vector<Napi::ObjectReference> _tracers;
};

// ──────────────────────────────────────────────────────────────
// RunnerNode — unified live runner.
//
//   new Runner(registry, onSignal)                — sync
//   new Runner(registry, onSignal, true)          — Disruptor (threaded)
// ──────────────────────────────────────────────────────────────

class RunnerNode : public Napi::ObjectWrap<RunnerNode>, public TsfnHost
{
 public:
  static Napi::Object Init(Napi::Env env)
  {
    return DefineClass(
        env, "Runner",
        {
            InstanceMethod("addStrategy", &RunnerNode::addStrategy),
            InstanceMethod("replaceStrategy", &RunnerNode::replaceStrategy),
            InstanceMethod("start", &RunnerNode::start),
            InstanceMethod("stop", &RunnerNode::stop),
            InstanceMethod("onTrade", &RunnerNode::onTrade),
            InstanceMethod("onBookSnapshot", &RunnerNode::onBookSnapshot),
            InstanceMethod("onBookTop", &RunnerNode::onBookTop),
            InstanceMethod("onBar", &RunnerNode::onBar),
            InstanceMethod("setPnlTracker", &RunnerNode::setPnlTracker),
            InstanceMethod("setStorageSink", &RunnerNode::setStorageSink),
            InstanceMethod("setRiskManager", &RunnerNode::setRiskManager),
            InstanceMethod("setKillSwitch", &RunnerNode::setKillSwitch),
            InstanceMethod("setOrderValidator", &RunnerNode::setOrderValidator),
            InstanceMethod("setMarketDataRecorder", &RunnerNode::setMarketDataRecorder),
            InstanceMethod("setExecutor", &RunnerNode::setExecutor),
            InstanceMethod("orderClosed", &RunnerNode::orderClosed),
            InstanceMethod("onOrderEvent", &RunnerNode::onOrderEvent),
            InstanceMethod("seedPosition", &RunnerNode::seedPosition),
            InstanceMethod("hookErrors", &RunnerNode::hookErrors),
            InstanceMethod("attachTraceRecorder", &RunnerNode::attachTraceRecorder),
            InstanceMethod("setTraceFeedTsNs", &RunnerNode::setTraceFeedTsNs),
            InstanceMethod("traceOrderEvent", &RunnerNode::traceOrderEvent),
            InstanceMethod("traceFill", &RunnerNode::traceFill),
        });
  }

  RunnerNode(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<RunnerNode>(info), TsfnHost(info.Env())
  {
    auto env = info.Env();
    auto* reg = SymbolRegistryNode::of(info[0], "Runner: registry");
    auto onSig = argFunction(info[1], "Runner: argument 2");
    bool threaded = info.Length() > 2 && argBool(info[2], "Runner: argument 3");

    if (threaded)
    {
      _reg = reg;
      _engine = lrvx_live_engine_create(static_cast<LrvxRegistryHandle>(reg));
      // The signal channel opens in start(), not here. A channel holds Node's
      // event loop open, and nothing should hold it open while no engine
      // thread is running -- constructing a threaded runner and doing nothing
      // else used to be enough to keep the process alive for good.
      _on_signal = Napi::Persistent(onSig);
      _mode = Mode::Threaded;
    }
    else
    {
      _reg = reg;
      _runner = lrvx_runner_create(static_cast<LrvxRegistryHandle>(reg),
                                   &RunnerNode::signalCbSync, this);
      _on_signal = Napi::Persistent(onSig);
      _mode = Mode::Sync;
    }
  }

  ~RunnerNode()
  {
    if (_mode == Mode::Sync && _runner)
    {
      lrvx_runner_destroy(_runner);
    }
    if (_mode == Mode::Threaded && _engine)
    {
      // Destroying the engine joins its consumer threads, so no producer can
      // reach a channel past this point. Each host retires and closes its own
      // channel in its destructor; the signal channel is closed by ~TsfnHost,
      // which runs after this body.
      lrvx_live_engine_destroy(_engine);
    }
  }

 private:
  enum class Mode
  {
    Sync,
    Threaded
  };
  Mode _mode{Mode::Sync};
  SymbolRegistry* _reg{nullptr};
  // Sync
  LrvxRunnerHandle _runner{nullptr};
  Napi::FunctionReference _on_signal;
  // Threaded
  LrvxLiveEngineHandle _engine{nullptr};
  bool _running{false};

  std::vector<std::unique_ptr<NodeStrategyHost>> _hosts;

  std::vector<uint32_t> extractSyms(Napi::Object obj)
  {
    std::vector<uint32_t> syms;
    auto symsV = obj.Get("symbols");
    if (symsV.IsArray())
    {
      auto arr = symsV.As<Napi::Array>();
      for (uint32_t i = 0; i < arr.Length(); ++i)
      {
        syms.push_back(symId(arr.Get(i)));
      }
    }
    return syms;
  }

  Napi::Value addStrategy(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    auto obj = argObject(info[0], "addStrategy: argument 1");
    uint32_t id = static_cast<uint32_t>(_hosts.size()) + 1;
    auto host = std::make_unique<NodeStrategyHost>(env, obj, _reg, id, extractSyms(obj));
    if (_mode == Mode::Threaded)
    {
      host->enableThreaded();
      if (_running)
      {
        // Added to an engine that is already running: the channel has to exist
        // before the bridge is subscribed, or the first event finds none.
        // Nothing reads this host yet, so the write is unshared.
        host->openChannel();
      }
      lrvx_clear_last_error();
      lrvx_live_engine_add_strategy(_engine,
                                    static_cast<LrvxStrategyHandle>(host->bridge.get()),
                                    &RunnerNode::signalCbThreaded, this);
    }
    else
    {
      lrvx_clear_last_error();
      lrvx_runner_add_strategy(_runner,
                               static_cast<LrvxStrategyHandle>(host->bridge.get()));
    }
    if (lrvx_last_error_code() != 0)
    {
      // Not attached: the host is dropped with this scope.
      Napi::Error::New(env, "Runner.addStrategy: " + capiReason()).ThrowAsJavaScriptException();
      return env.Undefined();
    }
    _hosts.push_back(std::move(host));
    return env.Undefined();
  }

  Napi::Value replaceStrategy(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    if (info.Length() < 2 || !info[0].IsNumber() || !info[1].IsObject())
    {
      Napi::TypeError::New(env, "replaceStrategy(index, newStrategy) expected")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    uint32_t idx = argUint32(info[0], "replaceStrategy: argument 1");
    if (idx >= _hosts.size())
    {
      auto err = Napi::Error::New(env, "replaceStrategy: index out of range");
      err.Value().Set("code", Napi::String::New(env, "E_VAL_002"));
      err.Value().Set("name", Napi::String::New(env, "LrvxError"));
      err.ThrowAsJavaScriptException();
      return env.Undefined();
    }
    _hosts[idx]->replaceStrategy(argObject(info[1], "replaceStrategy: argument 2"));
    return env.Undefined();
  }

  Napi::Value start(const Napi::CallbackInfo& info)
  {
    if (_mode == Mode::Sync)
    {
      lrvx_runner_start(_runner);
      return info.Env().Undefined();
    }
    // Channels open before the engine does, so every write to them happens
    // before the consumer threads that read them exist.
    TsfnHost::openChannel(_on_signal.Value(), "lrvx_runner_signal");
    for (auto& h : _hosts)
    {
      h->openChannel();
    }
    _running = true;
    lrvx_live_engine_start(_engine);
    return info.Env().Undefined();
  }

  Napi::Value stop(const Napi::CallbackInfo& info)
  {
    if (_mode == Mode::Sync)
    {
      lrvx_runner_stop(_runner);
      return info.Env().Undefined();
    }
    // Stopping the engine dispatches each strategy's onStop and then joins
    // the bus threads, so by the time it returns nothing can queue more work.
    // Closing after that hands back the event loop reference while leaving
    // the queued onStop to run on a later tick.
    lrvx_live_engine_stop(_engine);
    _running = false;
    for (auto& h : _hosts)
    {
      h->closeChannel();
    }
    TsfnHost::closeChannel();
    return info.Env().Undefined();
  }

  Napi::Value onTrade(const Napi::CallbackInfo& info)
  {
    uint32_t sym = symId(info[0]);
    double price = argNumber(info[1], "onTrade: price");
    double qty = argNumber(info[2], "onTrade: qty");
    if (!info[3].IsBoolean())
    {
      throw Napi::TypeError::New(info.Env(), "onTrade: isBuy must be a boolean");
    }
    bool isBuy = argBool(info[3], "onTrade: argument 4");
    int64_t ts = info.Length() > 4 ? toInt64Ns(info[4], "onTrade: argument 5") : 0;
    if (_mode == Mode::Sync)
    {
      lrvx_runner_on_trade(_runner, sym, price, qty, isBuy ? 1 : 0, ts);
    }
    else
    {
      lrvx_live_engine_publish_trade(_engine, sym, price, qty, isBuy ? 1 : 0, ts);
    }
    return info.Env().Undefined();
  }

  Napi::Value onBookSnapshot(const Napi::CallbackInfo& info)
  {
    uint32_t sym = symId(info[0]);
    // index.d.ts declares these as Float64Array; reading them as a plain
    // Napi::Array made every call written to the declared contract throw
    // "An array was expected".
    auto bidP = argFloat64Array(info[1], "onBookSnapshot: argument 2");
    auto bidQ = argFloat64Array(info[2], "onBookSnapshot: argument 3");
    auto askP = argFloat64Array(info[3], "onBookSnapshot: argument 4");
    auto askQ = argFloat64Array(info[4], "onBookSnapshot: argument 5");
    int64_t ts = info.Length() > 5 ? toInt64Ns(info[5], "onBookSnapshot: argument 6") : 0;
    // The C ABI takes one length per side (bp/bq share bpLen, ap/aq share
    // apLen), so mismatched price/qty arrays on one side used to make the
    // qty side get read at the price side's length.
    if (bidP.ElementLength() != bidQ.ElementLength() || askP.ElementLength() != askQ.ElementLength())
    {
      Napi::RangeError::New(info.Env(),
                            "onBookSnapshot: bid price/qty and ask price/qty arrays must each have the same length")
          .ThrowAsJavaScriptException();
      return info.Env().Undefined();
    }
    if (_mode == Mode::Sync)
    {
      lrvx_runner_on_book_snapshot(_runner, sym,
                                   bidP.Data(), bidQ.Data(), static_cast<uint32_t>(bidP.ElementLength()),
                                   askP.Data(), askQ.Data(), static_cast<uint32_t>(askP.ElementLength()), ts);
    }
    else
    {
      lrvx_live_engine_publish_book_snapshot(_engine, sym,
                                             bidP.Data(), bidQ.Data(), static_cast<uint32_t>(bidP.ElementLength()),
                                             askP.Data(), askQ.Data(), static_cast<uint32_t>(askP.ElementLength()), ts);
    }
    return info.Env().Undefined();
  }

  Napi::Value onBookTop(const Napi::CallbackInfo& info)
  {
    uint32_t sym = symId(info[0]);
    // A side is `{ price, qty }` or null; null (or undefined) means that
    // side of the book is empty, matching the quote reads where "no quote"
    // is null. Anything else throws — silently publishing an empty side
    // for a malformed argument would erase the live touch.
    auto side = [&info](Napi::Value v, uint8_t& has, double& price, double& qty) -> bool
    {
      if (v.IsNull() || v.IsUndefined())
      {
        return true;
      }
      if (!v.IsObject())
      {
        Napi::TypeError::New(info.Env(), "onBookTop: a side is { price, qty } or null")
            .ThrowAsJavaScriptException();
        return false;
      }
      auto o = v.As<Napi::Object>();
      has = 1;
      price = argNumber(o.Get("price"), "onBookTop: price");
      qty = argNumber(o.Get("qty"), "onBookTop: qty");
      return true;
    };
    uint8_t hasBid = 0;
    uint8_t hasAsk = 0;
    double bidPrice = 0, bidQty = 0, askPrice = 0, askQty = 0;
    if (!side(info[1], hasBid, bidPrice, bidQty) || !side(info[2], hasAsk, askPrice, askQty))
    {
      return info.Env().Undefined();
    }
    int64_t ts = info.Length() > 3 ? toInt64Ns(info[3], "onBookTop: argument 4") : 0;
    if (_mode == Mode::Sync)
    {
      lrvx_runner_on_book_top(_runner, sym, hasBid, bidPrice, bidQty, hasAsk, askPrice,
                              askQty, ts);
    }
    else
    {
      lrvx_live_engine_publish_book_top(_engine, sym, hasBid, bidPrice, bidQty, hasAsk,
                                        askPrice, askQty, ts);
    }
    return info.Env().Undefined();
  }

  Napi::Value onBar(const Napi::CallbackInfo& info)
  {
    uint32_t sym = symId(info[0]);
    auto opts = argObject(info[1], "onBar: argument 2");
    auto getNum = [&](const char* k, double dflt) -> double
    {
      auto v = opts.Get(k);
      return v.IsNumber() ? v.As<Napi::Number>().DoubleValue() : dflt;
    };
    // A BigInt is the type onBar hands back, so it has to be the type onBar
    // takes: the old `IsNumber()` guard silently substituted the default for
    // a BigInt startTimeNs, and a bar taken out of the addon and fed straight
    // back in arrived stamped 0.
    auto getInt = [&](const char* k, int64_t dflt) -> int64_t
    {
      auto v = opts.Get(k);
      if (v.IsBigInt() || v.IsNumber())
      {
        return toInt64Ns(v, "onBar: timestamp");
      }
      return dflt;
    };
    auto getU8 = [&](const char* k, uint8_t dflt) -> uint8_t
    {
      auto v = opts.Get(k);
      return v.IsNumber() ? static_cast<uint8_t>(v.As<Napi::Number>().Uint32Value()) : dflt;
    };
    uint8_t bt = getU8("barType", 0);
    uint64_t btp = static_cast<uint64_t>(getInt("barTypeParam", 0));
    double o = getNum("open", 0.0);
    double h = getNum("high", 0.0);
    double l = getNum("low", 0.0);
    double c = getNum("close", 0.0);
    double v = getNum("volume", 0.0);
    double bv = getNum("buyVolume", 0.0);
    int64_t sNs = getInt("startTimeNs", 0);
    int64_t eNs = getInt("endTimeNs", 0);
    uint8_t cr = getU8("closeReason", 0);
    if (_mode == Mode::Sync)
    {
      lrvx_runner_on_bar(_runner, sym, bt, btp, o, h, l, c, v, bv, sNs, eNs, cr);
    }
    else
    {
      lrvx_live_engine_publish_bar(_engine, sym, bt, btp, o, h, l, c, v, bv, sNs, eNs, cr);
    }
    return info.Env().Undefined();
  }

  static void signalCbSync(void* ud, const LrvxSignal* sig)
  {
    auto* self = static_cast<RunnerNode*>(ud);
    self->_on_signal.Call({signalToJs(self->_on_signal.Env(), sig)});
  }

  static void signalCbThreaded(void* ud, const LrvxSignal* sig)
  {
    auto* self = static_cast<RunnerNode*>(ud);
    auto* copy = new LrvxSignal(*sig);
    // No liveness guard here, unlike the strategy payloads: this callback
    // reads nothing but the channel's own bound function, which the channel
    // keeps alive for as long as it has anything to dispatch.
    self->post(copy, [](Napi::Env env, Napi::Function fn, LrvxSignal* s)
               {
      std::unique_ptr<LrvxSignal> guard(s);
      fn.Call({signalToJs(env, s)}); });
  }

  // ── Hook setters ────────────────────────────────────────────────────
  //
  // Each setX accepts either a plain JS object with named callbacks or
  // null/undefined to detach. The host is owned here and torn down on
  // detach / runner destruction so the C ABI handle is always valid
  // while the engine holds a reference.

  // Declared ahead of every host that points at it: members are destroyed in
  // reverse declaration order, so the sink outlives the hosts writing to it.
  lrvx_node::HookErrorSink _hook_errors;

  std::unique_ptr<lrvx_node::PnLTrackerHost> _pnl_host;
  std::unique_ptr<lrvx_node::StorageSinkHost> _storage_host;
  std::unique_ptr<lrvx_node::RiskManagerHost> _risk_host;
  std::unique_ptr<lrvx_node::KillSwitchHost> _kill_host;
  std::unique_ptr<lrvx_node::OrderValidatorHost> _validator_host;
  std::unique_ptr<lrvx_node::MarketDataRecorderHookHost> _recorder_host;
  // Keep the JS BinaryLogRecorderHook alive while attached so its C++
  // handle (borrowed via lrvx_binary_log_recorder_hook_as_recorder) stays valid.
  Napi::ObjectReference _recorder_binlog_ref;
  std::unique_ptr<lrvx_node::ExecutorHost> _executor_host;

  // Every hook host in hooks.h calls straight into JS from the C bridge the
  // engine invokes it by, so a threaded Runner -- whose engine calls those
  // bridges from a C++ consumer thread -- would be reaching into V8 off the
  // JS thread. index.d.ts has documented the four inline hooks as sync only
  // since they were added; nothing enforced it, and all seven were accepted
  // and wired into the LiveEngine. Detaching (a null argument) is always
  // allowed: it touches no JS.
  bool requireSyncHook(Napi::Env env, const char* setter)
  {
    if (_mode == Mode::Sync)
    {
      return true;
    }
    Napi::Error::New(env, std::string(setter) +
                              " requires a sync Runner: the hook runs inline on the JS "
                              "thread, which a threaded Runner does not dispatch from")
        .ThrowAsJavaScriptException();
    return false;
  }

  Napi::Value setPnlTracker(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    if (info.Length() == 0 || info[0].IsNull() || info[0].IsUndefined())
    {
      _pnl_host.reset();
      if (_mode == Mode::Sync)
      {
        lrvx_runner_set_pnl_tracker(_runner, nullptr);
      }
      else
      {
        lrvx_live_engine_set_pnl_tracker(_engine, nullptr);
      }
      return env.Undefined();
    }
    if (!requireSyncHook(env, "setPnlTracker"))
    {
      return env.Undefined();
    }
    _pnl_host = std::make_unique<lrvx_node::PnLTrackerHost>(env, argObject(info[0], "setPnlTracker: argument 1"));
    if (_mode == Mode::Sync)
    {
      lrvx_runner_set_pnl_tracker(_runner, _pnl_host->handle);
    }
    else
    {
      lrvx_live_engine_set_pnl_tracker(_engine, _pnl_host->handle);
    }
    return env.Undefined();
  }

  Napi::Value setStorageSink(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    if (info.Length() == 0 || info[0].IsNull() || info[0].IsUndefined())
    {
      _storage_host.reset();
      if (_mode == Mode::Sync)
      {
        lrvx_runner_set_storage_sink(_runner, nullptr);
      }
      else
      {
        lrvx_live_engine_set_storage_sink(_engine, nullptr);
      }
      return env.Undefined();
    }
    if (!requireSyncHook(env, "setStorageSink"))
    {
      return env.Undefined();
    }
    _storage_host = std::make_unique<lrvx_node::StorageSinkHost>(env, argObject(info[0], "setStorageSink: argument 1"));
    if (_mode == Mode::Sync)
    {
      lrvx_runner_set_storage_sink(_runner, _storage_host->handle);
    }
    else
    {
      lrvx_live_engine_set_storage_sink(_engine, _storage_host->handle);
    }
    return env.Undefined();
  }

  Napi::Value setRiskManager(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    if (info.Length() == 0 || info[0].IsNull() || info[0].IsUndefined())
    {
      _risk_host.reset();
      if (_mode == Mode::Sync)
      {
        lrvx_runner_set_risk_manager(_runner, nullptr);
      }
      else
      {
        lrvx_live_engine_set_risk_manager(_engine, nullptr);
      }
      return env.Undefined();
    }
    if (!requireSyncHook(env, "setRiskManager"))
    {
      return env.Undefined();
    }
    _risk_host = std::make_unique<lrvx_node::RiskManagerHost>(env, argObject(info[0], "setRiskManager: argument 1"), &_hook_errors);
    if (_mode == Mode::Sync)
    {
      lrvx_runner_set_risk_manager(_runner, _risk_host->handle);
    }
    else
    {
      lrvx_live_engine_set_risk_manager(_engine, _risk_host->handle);
    }
    return env.Undefined();
  }

  Napi::Value setKillSwitch(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    if (info.Length() == 0 || info[0].IsNull() || info[0].IsUndefined())
    {
      _kill_host.reset();
      if (_mode == Mode::Sync)
      {
        lrvx_runner_set_kill_switch(_runner, nullptr);
      }
      else
      {
        lrvx_live_engine_set_kill_switch(_engine, nullptr);
      }
      return env.Undefined();
    }
    if (!requireSyncHook(env, "setKillSwitch"))
    {
      return env.Undefined();
    }
    _kill_host = std::make_unique<lrvx_node::KillSwitchHost>(env, argObject(info[0], "setKillSwitch: argument 1"), &_hook_errors);
    if (_mode == Mode::Sync)
    {
      lrvx_runner_set_kill_switch(_runner, _kill_host->handle);
    }
    else
    {
      lrvx_live_engine_set_kill_switch(_engine, _kill_host->handle);
    }
    return env.Undefined();
  }

  Napi::Value setOrderValidator(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    if (info.Length() == 0 || info[0].IsNull() || info[0].IsUndefined())
    {
      _validator_host.reset();
      if (_mode == Mode::Sync)
      {
        lrvx_runner_set_order_validator(_runner, nullptr);
      }
      else
      {
        lrvx_live_engine_set_order_validator(_engine, nullptr);
      }
      return env.Undefined();
    }
    if (!requireSyncHook(env, "setOrderValidator"))
    {
      return env.Undefined();
    }
    _validator_host =
        std::make_unique<lrvx_node::OrderValidatorHost>(env, argObject(info[0], "setOrderValidator: argument 1"), &_hook_errors);
    if (_mode == Mode::Sync)
    {
      lrvx_runner_set_order_validator(_runner, _validator_host->handle);
    }
    else
    {
      lrvx_live_engine_set_order_validator(_engine, _validator_host->handle);
    }
    return env.Undefined();
  }

  Napi::Value setMarketDataRecorder(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    if (info.Length() == 0 || info[0].IsNull() || info[0].IsUndefined())
    {
      _recorder_host.reset();
      _recorder_binlog_ref.Reset();
      if (_mode == Mode::Sync)
      {
        lrvx_runner_set_market_data_recorder(_runner, nullptr);
      }
      else
      {
        lrvx_live_engine_set_market_data_recorder(_engine, nullptr);
      }
      return env.Undefined();
    }

    if (!requireSyncHook(env, "setMarketDataRecorder"))
    {
      return env.Undefined();
    }

    auto obj = argObject(info[0], "setMarketDataRecorder: argument 1");

    // Detect the built-in BinaryLogRecorderHook wrapper via the cached
    // constructor stored at module init.
    LrvxMarketDataRecorderHandle binlog_handle = nullptr;
    auto& ctor = node_lrvx::BinaryLogRecorderHookWrap::Ctor();
    if (!ctor.IsEmpty() && obj.InstanceOf(ctor.Value()))
    {
      auto* hook = Napi::ObjectWrap<node_lrvx::BinaryLogRecorderHookWrap>::Unwrap(obj);
      binlog_handle = hook->asRecorder();
    }

    if (binlog_handle)
    {
      _recorder_host.reset();
      _recorder_binlog_ref = Napi::Persistent(obj);
      if (_mode == Mode::Sync)
      {
        lrvx_runner_set_market_data_recorder(_runner, binlog_handle);
      }
      else
      {
        lrvx_live_engine_set_market_data_recorder(_engine, binlog_handle);
      }
      return env.Undefined();
    }

    _recorder_binlog_ref.Reset();
    _recorder_host = std::make_unique<lrvx_node::MarketDataRecorderHookHost>(env, obj);
    if (_mode == Mode::Sync)
    {
      lrvx_runner_set_market_data_recorder(_runner, _recorder_host->handle);
    }
    else
    {
      lrvx_live_engine_set_market_data_recorder(_engine, _recorder_host->handle);
    }
    return env.Undefined();
  }

  Napi::Value setExecutor(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    if (info.Length() == 0 || info[0].IsNull() || info[0].IsUndefined())
    {
      _executor_host.reset();
      if (_mode == Mode::Sync)
      {
        lrvx_runner_set_executor(_runner, nullptr);
      }
      else
      {
        lrvx_live_engine_set_executor(_engine, nullptr);
      }
      return env.Undefined();
    }
    if (!requireSyncHook(env, "setExecutor"))
    {
      return env.Undefined();
    }
    _executor_host =
        std::make_unique<lrvx_node::ExecutorHost>(env, argObject(info[0], "setExecutor: argument 1"), &_hook_errors);
    if (_mode == Mode::Sync)
    {
      lrvx_runner_set_executor(_runner, _executor_host->handle);
    }
    else
    {
      lrvx_live_engine_set_executor(_engine, _executor_host->handle);
    }
    return env.Undefined();
  }

  // The host's word that an order sent through the executor is finished, so
  // the runner drops it from the record a Modify is built from.
  Napi::Value orderClosed(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    if (info.Length() < 1)
    {
      throw Napi::TypeError::New(env, "orderClosed(orderId) expected");
    }
    const uint64_t orderId = argUint64(info[0], "orderClosed: orderId");
    if (_mode == Mode::Sync)
    {
      lrvx_runner_order_closed(_runner, orderId);
    }
    else
    {
      lrvx_live_engine_order_closed(_engine, orderId);
    }
    return env.Undefined();
  }

  // What the strategy at `index` holds before any fill is reported.
  Napi::Value seedPosition(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    if (info.Length() < 3)
    {
      throw Napi::TypeError::New(
          env, "seedPosition(strategyIndex, symbol, position, avgEntryPrice?) expected");
    }
    const uint32_t idx = argUint32(info[0], "seedPosition: strategyIndex");
    if (idx >= _hosts.size())
    {
      throw Napi::RangeError::New(env, "seedPosition: strategyIndex " + std::to_string(idx) +
                                           " is not a strategy of this runner (it has " +
                                           std::to_string(_hosts.size()) + ")");
    }
    const uint32_t sym = symId(info[1]);
    const double position = argNumber(info[2], "seedPosition: position");
    const double avg = info.Length() > 3 && !info[3].IsUndefined()
                           ? argNumber(info[3], "seedPosition: avgEntryPrice")
                           : 0.0;
    const auto strategy = static_cast<LrvxStrategyHandle>(_hosts[idx]->bridge.get());
    lrvx_clear_last_error();
    const uint8_t ok =
        _mode == Mode::Sync
            ? lrvx_runner_seed_position(_runner, strategy, sym,
                                        lrvx_quantity_from_double(position),
                                        lrvx_price_from_double(avg))
            : lrvx_live_engine_seed_position(_engine, strategy, sym,
                                             lrvx_quantity_from_double(position),
                                             lrvx_price_from_double(avg));
    if (ok == 0)
    {
      throw Napi::Error::New(env, "seedPosition: " + capiReason());
    }
    return env.Undefined();
  }

  // One order event a venue reported, in the words a strategy's onFill and
  // onOrderUpdate read them back in.
  Napi::Value onOrderEvent(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    auto opts = argOptions(info, "onOrderEvent");
    const auto text = [&](const char* key, const char* fallback) -> std::string
    {
      if (!opts.Has(key) || opts.Get(key).IsUndefined())
      {
        if (fallback != nullptr)
        {
          return fallback;
        }
        throw Napi::TypeError::New(env, std::string("onOrderEvent: ") + key + " is required");
      }
      if (!opts.Get(key).IsString())
      {
        throw Napi::TypeError::New(env, std::string("onOrderEvent: ") + key + " must be a string");
      }
      return opts.Get(key).As<Napi::String>().Utf8Value();
    };
    if (!opts.Has("orderId") || !opts.Has("symbol"))
    {
      throw Napi::TypeError::New(env, "onOrderEvent: orderId and symbol are required");
    }

    LrvxOrderEventData ev{};
    ev.order_id = argUint64(opts.Get("orderId"), "onOrderEvent: orderId");
    ev.symbol_id = symId(opts.Get("symbol"));
    const std::string side = text("side", nullptr);
    if (side != "buy" && side != "sell")
    {
      throw Napi::TypeError::New(env, "onOrderEvent: side must be 'buy' or 'sell', got '" + side +
                                          "'");
    }
    ev.side = side == "buy" ? 0 : 1;
    const std::string orderType = text("orderType", "limit");
    if (!lrvx::capi::orderTypeCodeFromName(orderType, &ev.order_type))
    {
      throw Napi::TypeError::New(env, "onOrderEvent: " +
                                          lrvx::capi::unknownOrderTypeMessage(orderType));
    }
    const std::string status = text("status", nullptr);
    if (!lrvx::capi::orderEventStatusCodeFromName(status, &ev.status))
    {
      throw Napi::TypeError::New(env, "onOrderEvent: " +
                                          lrvx::capi::unknownOrderEventStatusMessage(status));
    }
    const auto number = [&](const char* key, const char* what)
    { return opts.Has(key) ? argNumber(opts.Get(key), what) : 0.0; };
    ev.fill_qty_raw = lrvx_quantity_from_double(number("fillQty", "onOrderEvent: fillQty"));
    ev.fill_price_raw = lrvx_price_from_double(number("fillPrice", "onOrderEvent: fillPrice"));
    ev.exchange_ts_ns = opts.Has("tsNs") ? toInt64Ns(opts.Get("tsNs"), "onOrderEvent: tsNs") : 0;
    const std::string rejectReason = text("rejectReason", "");
    ev.reject_reason = rejectReason.empty() ? nullptr : rejectReason.c_str();
    if (opts.Has("isMaker"))
    {
      if (!opts.Get("isMaker").IsBoolean())
      {
        throw Napi::TypeError::New(env, "onOrderEvent: isMaker must be a boolean");
      }
      ev.is_maker = argBool(opts.Get("isMaker"), "onOrderEvent: isMaker") ? 1 : 0;
    }
    if (ev.status == 3 || ev.status == 4)
    {
      ev.last_fill_at_ns = ev.exchange_ts_ns;
    }

    lrvx_clear_last_error();
    if (_mode == Mode::Sync)
    {
      lrvx_runner_on_order_event(_runner, &ev);
    }
    else
    {
      // A threaded runner's callbacks cross to this thread through channels
      // that only exist between start() and stop().
      if (!_running)
      {
        throw Napi::Error::New(env, "onOrderEvent: the threaded runner is not started");
      }
      lrvx_live_engine_on_order_event(_engine, &ev);
    }
    if (lrvx_last_error_code() != 0)
    {
      throw Napi::Error::New(env, "onOrderEvent: " + capiReason());
    }
    return env.Undefined();
  }

  // Every hook failure this runner has seen, oldest first. A gate runs inline
  // while a signal is in flight and has no caller to throw back at, and the
  // process-wide log callback is asynchronous and shared, so it cannot say
  // which runner denied what. Records accumulate for the life of the runner.
  Napi::Value hookErrors(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    const auto& records = _hook_errors.records;
    auto arr = Napi::Array::New(env, records.size());
    for (size_t i = 0; i < records.size(); ++i)
    {
      auto obj = Napi::Object::New(env);
      obj.Set("hook", Napi::String::New(env, records[i].hook));
      obj.Set("method", Napi::String::New(env, records[i].method));
      obj.Set("message", Napi::String::New(env, records[i].message));
      arr.Set(static_cast<uint32_t>(i), obj);
    }
    return arr;
  }

  Napi::Value attachTraceRecorder(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    if (_mode != Mode::Sync)
    {
      Napi::Error::New(env, "attachTraceRecorder is only supported in sync mode")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    if (info.Length() == 0 || info[0].IsNull() || info[0].IsUndefined())
    {
      lrvx_runner_attach_trace_recorder(_runner, nullptr);
      return env.Undefined();
    }
    auto* tr = Napi::ObjectWrap<TraceRecorderWrap>::Unwrap(argObject(info[0], "attachTraceRecorder: argument 1"));
    lrvx_runner_attach_trace_recorder(_runner, tr->handle());
    return env.Undefined();
  }

  Napi::Value setTraceFeedTsNs(const Napi::CallbackInfo& info)
  {
    if (_mode != Mode::Sync)
    {
      Napi::Error::New(info.Env(), "setTraceFeedTsNs is only supported in sync mode")
          .ThrowAsJavaScriptException();
      return info.Env().Undefined();
    }
    int64_t ts = toInt64Ns(info[0], "setTraceFeedTsNs: argument 1");
    lrvx_runner_set_trace_feed_ts_ns(_runner, ts);
    return info.Env().Undefined();
  }

  Napi::Value traceOrderEvent(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    if (_mode != Mode::Sync)
    {
      Napi::Error::New(env, "traceOrderEvent is only supported in sync mode")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    auto opts = argOptions(info, "traceOrderEvent");
    const auto u8 = [&opts](const char* key, const char* what)
    { return static_cast<uint8_t>(argUint32(opts.Get(key), what)); };
    uint64_t order_id = argUint64(opts.Get("orderId"), "traceOrderEvent: orderId");
    uint64_t parent = opts.Has("parentSignalId")
                          ? argUint64(opts.Get("parentSignalId"), "traceOrderEvent: parentSignalId")
                          : 0;
    uint32_t sym = argUint32(opts.Get("symbolId"), "traceOrderEvent: symbolId");
    uint8_t kind = u8("eventKind", "traceOrderEvent: eventKind");
    uint8_t side = u8("side", "traceOrderEvent: side");
    uint8_t order_type = opts.Has("orderType") ? u8("orderType", "traceOrderEvent: orderType") : 0;
    double price = opts.Has("price") ? argNumber(opts.Get("price"), "traceOrderEvent: price") : 0.0;
    double qty = opts.Has("qty") ? argNumber(opts.Get("qty"), "traceOrderEvent: qty") : 0.0;
    uint32_t flags = opts.Has("flags") ? argUint32(opts.Get("flags"), "traceOrderEvent: flags") : 0;
    lrvx_runner_trace_order_event(_runner, order_id, parent, sym, kind, side, order_type,
                                  lrvx_price_from_double(price),
                                  lrvx_quantity_from_double(qty), flags);
    return env.Undefined();
  }

  Napi::Value traceFill(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    if (_mode != Mode::Sync)
    {
      Napi::Error::New(env, "traceFill is only supported in sync mode")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    auto opts = argOptions(info, "traceFill");
    uint64_t order_id = argUint64(opts.Get("orderId"), "traceFill: orderId");
    uint64_t fill_id = opts.Has("fillId") ? argUint64(opts.Get("fillId"), "traceFill: fillId") : 0;
    double price = argNumber(opts.Get("price"), "traceFill: price");
    double qty = argNumber(opts.Get("qty"), "traceFill: qty");
    double fee = opts.Has("fee") ? argNumber(opts.Get("fee"), "traceFill: fee") : 0.0;
    uint32_t sym = argUint32(opts.Get("symbolId"), "traceFill: symbolId");
    uint8_t side = static_cast<uint8_t>(argUint32(opts.Get("side"), "traceFill: side"));
    uint8_t liq = opts.Has("liquidity")
                      ? static_cast<uint8_t>(argUint32(opts.Get("liquidity"), "traceFill: liquidity"))
                      : 0;
    lrvx_runner_trace_fill(_runner, order_id, fill_id, lrvx_price_from_double(price),
                           lrvx_quantity_from_double(qty), lrvx_quantity_from_double(fee),
                           sym, side, liq);
    return env.Undefined();
  }
};

inline void registerStrategy(Napi::Env env, Napi::Object exports);

}  // namespace node_lrvx

#include "grid_search.h"
#include "walk_forward.h"

namespace node_lrvx
{

inline void registerStrategy(Napi::Env env, Napi::Object exports)
{
  exports.Set("SymbolRegistry", SymbolRegistryNode::Init(env));
  exports.Set("Runner", RunnerNode::Init(env));
  exports.Set("BacktestRunner", BacktestRunnerNode::Init(env));
  exports.Set("WalkForwardRunner", WalkForwardRunnerNode::Init(env));
  exports.Set("GridSearch", GridSearchNode::Init(env));
}

}  // namespace node_lrvx
