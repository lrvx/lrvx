// node/src/strategy.h — StrategyRunner and LiveEngine for Node.js

#pragma once

#include <napi.h>

#include "bindings_common.h"
#include "data_ops.h"
#include "error_translator.h"
#include "hooks.h"
#include "lrvx/capi/bridge_strategy.h"
#include "lrvx/capi/lrvx_capi.h"
#include "lrvx/capi/order_type_names.hpp"
#include "lrvx/engine/symbol_registry.h"
#include "run_trace.h"
#include "tsfn_util.h"

#include <memory>
#include <string>
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
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "SymbolRegistry",
                       {
                           InstanceMethod("addSymbol", &SymbolRegistryNode::addSymbol),
                           InstanceMethod("symbolCount", &SymbolRegistryNode::symbolCount),
                       });
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
    std::string ex = info[0].As<Napi::String>().Utf8Value();
    std::string sym = info[1].As<Napi::String>().Utf8Value();
    // A tick passed here is the venue's, and a backtest refuses orders priced
    // off it. Left out, the book uses 0.01 and order prices are not checked.
    const bool hasTick = info.Length() > 2 && info[2].IsNumber();
    double tickSize = hasTick ? info[2].As<Napi::Number>().DoubleValue() : 0.01;
    lrvx::SymbolInfo si;
    si.exchange = ex;
    si.symbol = sym;
    si.tickSize = lrvx::Price::fromDouble(tickSize);
    si.tickSizeKnown = hasTick;
    uint32_t id = static_cast<uint32_t>(_reg->registerSymbol(si));

    // Return a Symbol object: { id, exchange, name, tickSize, valueOf() → id }
    // valueOf() makes it transparently usable as a number everywhere.
    auto obj = Napi::Object::New(env);
    obj.Set("id", Napi::Number::New(env, id));
    obj.Set("exchange", Napi::String::New(env, ex));
    obj.Set("name", Napi::String::New(env, sym));
    obj.Set("tickSize", Napi::Number::New(env, tickSize));
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
};

// ──────────────────────────────────────────────────────────────
// Helpers
// ──────────────────────────────────────────────────────────────

// Accept Symbol object or plain number → uint32_t id.
inline uint32_t symId(const Napi::Value& v)
{
  if (v.IsObject())
  {
    return v.As<Napi::Object>().Get("id").As<Napi::Number>().Uint32Value();
  }
  return v.As<Napi::Number>().Uint32Value();
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
        auto* h   = static_cast<NodeStrategyHost*>(i.Data());
        double qty = i[0].As<Napi::Number>().DoubleValue();
        uint32_t sym = i.Length() > 1 ? symId(i[1])
                                       : (h->syms.empty() ? 0u : h->syms[0]);
        h->bridge->publicEmitMarketBuy(sym, Quantity::fromDouble(qty));
        return i.Env().Undefined(); }, "marketBuy", this));

    em.Set("marketSell", Napi::Function::New(env_, [](const Napi::CallbackInfo& i) -> Napi::Value
                                             {
        auto* h   = static_cast<NodeStrategyHost*>(i.Data());
        double qty = i[0].As<Napi::Number>().DoubleValue();
        uint32_t sym = i.Length() > 1 ? symId(i[1])
                                       : (h->syms.empty() ? 0u : h->syms[0]);
        h->bridge->publicEmitMarketSell(sym, Quantity::fromDouble(qty));
        return i.Env().Undefined(); }, "marketSell", this));

    em.Set("limitBuy", Napi::Function::New(env_, [](const Napi::CallbackInfo& i) -> Napi::Value
                                           {
        auto* h    = static_cast<NodeStrategyHost*>(i.Data());
        double price = i[0].As<Napi::Number>().DoubleValue();
        double qty   = i[1].As<Napi::Number>().DoubleValue();
        uint32_t sym = i.Length() > 2 ? symId(i[2])
                                       : (h->syms.empty() ? 0u : h->syms[0]);
        h->bridge->publicEmitLimitBuy(sym, Price::fromDouble(price), Quantity::fromDouble(qty));
        return i.Env().Undefined(); }, "limitBuy", this));

    em.Set("limitSell", Napi::Function::New(env_, [](const Napi::CallbackInfo& i) -> Napi::Value
                                            {
        auto* h    = static_cast<NodeStrategyHost*>(i.Data());
        double price = i[0].As<Napi::Number>().DoubleValue();
        double qty   = i[1].As<Napi::Number>().DoubleValue();
        uint32_t sym = i.Length() > 2 ? symId(i[2])
                                       : (h->syms.empty() ? 0u : h->syms[0]);
        h->bridge->publicEmitLimitSell(sym, Price::fromDouble(price), Quantity::fromDouble(qty));
        return i.Env().Undefined(); }, "limitSell", this));

    em.Set("provideLiquidity", Napi::Function::New(env_, [](const Napi::CallbackInfo& i) -> Napi::Value
                                                   {
        auto* h    = static_cast<NodeStrategyHost*>(i.Data());
        double lo  = i[0].As<Napi::Number>().DoubleValue();
        double hi  = i[1].As<Napi::Number>().DoubleValue();
        double liq = i[2].As<Napi::Number>().DoubleValue();
        uint32_t sym = i.Length() > 3 ? symId(i[3])
                                       : (h->syms.empty() ? 0u : h->syms[0]);
        h->bridge->publicEmitProvideLiquidity(sym, Price::fromDouble(lo), Price::fromDouble(hi),
                                              Quantity::fromDouble(liq));
        return i.Env().Undefined(); }, "provideLiquidity", this));

    em.Set("withdrawLiquidity", Napi::Function::New(env_, [](const Napi::CallbackInfo& i) -> Napi::Value
                                                    {
        auto* h    = static_cast<NodeStrategyHost*>(i.Data());
        double liq = i[0].As<Napi::Number>().DoubleValue();
        uint32_t sym = i.Length() > 1 ? symId(i[1])
                                       : (h->syms.empty() ? 0u : h->syms[0]);
        h->bridge->publicEmitWithdrawLiquidity(sym, Quantity::fromDouble(liq));
        return i.Env().Undefined(); }, "withdrawLiquidity", this));

    em.Set("cancel", Napi::Function::New(env_, [](const Napi::CallbackInfo& i) -> Napi::Value
                                         {
        auto* h = static_cast<NodeStrategyHost*>(i.Data());
        uint64_t orderId = static_cast<uint64_t>(i[0].As<Napi::Number>().Int64Value());
        h->bridge->publicEmitCancel(orderId);
        return i.Env().Undefined(); }, "cancel", this));

    em.Set("closePosition", Napi::Function::New(env_, [](const Napi::CallbackInfo& i) -> Napi::Value
                                                {
        auto* h  = static_cast<NodeStrategyHost*>(i.Data());
        uint32_t sym = i.Length() > 0 ? symId(i[0])
                                       : (h->syms.empty() ? 0u : h->syms[0]);
        h->bridge->publicEmitClosePosition(sym);
        return i.Env().Undefined(); }, "closePosition", this));

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
        uint8_t bt = i[1].As<Napi::Number>().Uint32Value();
        uint64_t param = static_cast<uint64_t>(i[2].As<Napi::Number>().Int64Value());
        auto bar = h->bridge->lastClosedBar(sym, static_cast<BarType>(bt), param);
        if (!bar){ return i.Env().Null();
}
        return buildBar(i.Env(), *bar); }, "lastClosedBar", this));
    em.Set("lastNClosedBars", Napi::Function::New(env_, [buildBar](const Napi::CallbackInfo& i) -> Napi::Value
                                                  {
        auto* h = static_cast<NodeStrategyHost*>(i.Data());
        uint32_t sym = symId(i[0]);
        uint8_t bt = i[1].As<Napi::Number>().Uint32Value();
        uint64_t param = static_cast<uint64_t>(i[2].As<Napi::Number>().Int64Value());
        size_t n = static_cast<size_t>(i[3].As<Napi::Number>().Uint32Value());
        auto bars = h->bridge->lastNClosedBars(sym, static_cast<BarType>(bt), param, n);
        Napi::Array out = Napi::Array::New(i.Env(), bars.size());
        for (size_t k = 0; k < bars.size(); ++k){ out.Set(static_cast<uint32_t>(k), buildBar(i.Env(), bars[k]));
}
        return out; }, "lastNClosedBars", this));
    em.Set("setBarRingCapacity", Napi::Function::New(env_, [](const Napi::CallbackInfo& i) -> Napi::Value
                                                     {
        auto* h = static_cast<NodeStrategyHost*>(i.Data());
        size_t n = static_cast<size_t>(i[0].As<Napi::Number>().Uint32Value());
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
    switch (s)
    {
      case 0:
        return "NEW";
      case 1:
        return "ACCEPTED";
      case 2:
        return "PENDING_NEW";
      case 3:
        return "PARTIALLY_FILLED";
      case 4:
        return "FILLED";
      case 5:
        return "PENDING_CANCEL";
      case 6:
        return "CANCELED";
      case 7:
        return "EXPIRED";
      case 8:
        return "REJECTED";
      case 9:
        return "REPLACED";
      case 10:
        return "PENDING_TRIGGER";
      case 11:
        return "TRIGGERED";
      case 12:
        return "TRAILING_UPDATED";
      case 13:
        return "QUEUE_POSITION_UPDATED";
      case 14:
        return "MARKET_POSITION_CHANGED";
      case 15:
        return "REPLACE_SUBMITTED";
      case 16:
        return "REPLACE_ACCEPTED";
      case 17:
        return "REPLACE_REJECTED";
      case 19:
        return "PENDING_ONCHAIN";
      case 20:
        return "REVERTED";
      case 21:
        return "REPLACED_GAS";
      default:
        return "UNKNOWN";
    }
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
    o.Set("exchangeTsNs", Napi::Number::New(env, static_cast<double>(ev->exchange_ts_ns)));
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
    o.Set("submittedAtNs", Napi::Number::New(env, static_cast<double>(ev->submitted_at_ns)));
    o.Set("acceptedAtNs", Napi::Number::New(env, static_cast<double>(ev->accepted_at_ns)));
    o.Set("firstFillAtNs", Napi::Number::New(env, static_cast<double>(ev->first_fill_at_ns)));
    o.Set("lastFillAtNs", Napi::Number::New(env, static_cast<double>(ev->last_fill_at_ns)));
    o.Set("canceledAtNs", Napi::Number::New(env, static_cast<double>(ev->canceled_at_ns)));
    o.Set("rejectedAtNs", Napi::Number::New(env, static_cast<double>(ev->rejected_at_ns)));
    o.Set("triggeredAtNs", Napi::Number::New(env, static_cast<double>(ev->triggered_at_ns)));
    o.Set("expiredAtNs", Napi::Number::New(env, static_cast<double>(ev->expired_at_ns)));
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
            InstanceMethod("setExecutor", &BacktestRunnerNode::setExecutor),
            InstanceMethod("addExecutionListener", &BacktestRunnerNode::addExecutionListener),
            InstanceMethod("equityCurve", &BacktestRunnerNode::equityCurve),
            InstanceMethod("trades", &BacktestRunnerNode::trades),
        });
  }

  // new BacktestRunner(registry, feeRate, initialCapital)
  BacktestRunnerNode(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<BacktestRunnerNode>(info)
  {
    _reg = Napi::ObjectWrap<SymbolRegistryNode>::Unwrap(info[0].As<Napi::Object>())->get();
    double fee = info[1].As<Napi::Number>().DoubleValue();
    double cap = info[2].As<Napi::Number>().DoubleValue();
    _handle = lrvx_backtest_runner_create(static_cast<LrvxRegistryHandle>(_reg), fee, cap);
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

  // runner.setStrategy(strategyObj) — plain JS object with callbacks
  Napi::Value setStrategy(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    auto obj = info[0].As<Napi::Object>();
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
                     std::string path = info[0].As<Napi::String>().Utf8Value();
                     LrvxBacktestStats s{};
                     int ok = lrvx_backtest_runner_run_tape(_handle, path.c_str(), &s);
                     if (!ok)
                     {
                       return info.Env().Null();
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
                     auto arr = info[0].As<Napi::Array>();
                     uint32_t n = arr.Length();
                     std::vector<std::string> owned;
                     owned.reserve(n);
                     for (uint32_t i = 0; i < n; ++i)
                     {
                       owned.emplace_back(arr.Get(i).As<Napi::String>().Utf8Value());
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
                       Napi::Error::New(
                           env,
                           "BacktestRunner.runTapes failed (invalid paths or "
                           "overlapping book streams)")
                           .ThrowAsJavaScriptException();
                       return env.Undefined();
                     }
                     return statsToJs(env, s);
                   });
  }

  // runner.runCsv(path, symbol) → stats object
  Napi::Value runCsv(const Napi::CallbackInfo& info)
  {
    return tryLrvx(info.Env(), [&]() -> Napi::Value
                   {
      std::string path = info[0].As<Napi::String>().Utf8Value();
      std::string symbol = info[1].As<Napi::String>().Utf8Value();
      LrvxBacktestStats s{};
      int ok = lrvx_backtest_runner_run_csv(_handle, path.c_str(), symbol.c_str(), &s);
      if (!ok)
      {
        return info.Env().Null();
      }
      return statsToJs(info.Env(), s); });
  }

  // runner.runOhlcv(tsArray, closeArray, symbol) → stats object
  Napi::Value runOhlcv(const Napi::CallbackInfo& info)
  {
    return tryLrvx(info.Env(), [&]() -> Napi::Value
                   {
      auto tsArr = info[0].As<Napi::BigInt64Array>();
      auto closeArr = info[1].As<Napi::Float64Array>();
      if (!requireSameLength(info.Env(), "runOhlcv",
                             {tsArr.ElementLength(), closeArr.ElementLength()}))
      {
        return info.Env().Undefined();
      }
      std::string symbol = info[2].As<Napi::String>().Utf8Value();
      uint32_t n = static_cast<uint32_t>(tsArr.ElementLength());
      LrvxBacktestStats s{};
      int ok = lrvx_backtest_runner_run_ohlcv(_handle,
                                              reinterpret_cast<const int64_t*>(tsArr.Data()),
                                              closeArr.Data(),
                                              n, symbol.c_str(), &s);
      if (!ok)
      {
        return info.Env().Null();
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
      auto startNs = info[0].As<Napi::BigInt64Array>();
      auto endNs = info[1].As<Napi::BigInt64Array>();
      auto openA = info[2].As<Napi::Float64Array>();
      auto highA = info[3].As<Napi::Float64Array>();
      auto lowA = info[4].As<Napi::Float64Array>();
      auto closeA = info[5].As<Napi::Float64Array>();
      auto volA = info[6].As<Napi::Float64Array>();
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
      std::string symbol = info[7].As<Napi::String>().Utf8Value();
      uint8_t barType = info.Length() > 8 ? static_cast<uint8_t>(info[8].As<Napi::Number>().Uint32Value()) : 0;
      uint64_t barTypeParam = info.Length() > 9
                                  ? static_cast<uint64_t>(info[9].As<Napi::Number>().Int64Value())
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
        return info.Env().Null();
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
    _executor_host = std::make_unique<lrvx_node::ExecutorHost>(env, info[0].As<Napi::Object>());
    lrvx_backtest_runner_set_executor(_handle, _executor_host->handle);
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
        std::make_unique<lrvx_node::ExecutionListenerHost>(env, info[0].As<Napi::Object>());
    lrvx_backtest_runner_add_execution_listener(_handle, host->handle);
    _listener_hosts.push_back(std::move(host));
    return env.Undefined();
  }
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
    auto* reg = Napi::ObjectWrap<SymbolRegistryNode>::Unwrap(info[0].As<Napi::Object>())->get();
    auto onSig = info[1].As<Napi::Function>();
    bool threaded = info.Length() > 2 && info[2].As<Napi::Boolean>().Value();

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
    auto obj = info[0].As<Napi::Object>();
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
      lrvx_live_engine_add_strategy(_engine,
                                    static_cast<LrvxStrategyHandle>(host->bridge.get()),
                                    &RunnerNode::signalCbThreaded, this);
    }
    else
    {
      lrvx_runner_add_strategy(_runner,
                               static_cast<LrvxStrategyHandle>(host->bridge.get()));
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
    uint32_t idx = info[0].As<Napi::Number>().Uint32Value();
    if (idx >= _hosts.size())
    {
      auto err = Napi::Error::New(env, "replaceStrategy: index out of range");
      err.Value().Set("code", Napi::String::New(env, "E_VAL_002"));
      err.Value().Set("name", Napi::String::New(env, "LrvxError"));
      err.ThrowAsJavaScriptException();
      return env.Undefined();
    }
    _hosts[idx]->replaceStrategy(info[1].As<Napi::Object>());
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
    double price = info[1].As<Napi::Number>().DoubleValue();
    double qty = info[2].As<Napi::Number>().DoubleValue();
    bool isBuy = info[3].As<Napi::Boolean>().Value();
    int64_t ts = info.Length() > 4 ? toInt64Ns(info[4]) : 0;
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
    auto bidP = info[1].As<Napi::Float64Array>();
    auto bidQ = info[2].As<Napi::Float64Array>();
    auto askP = info[3].As<Napi::Float64Array>();
    auto askQ = info[4].As<Napi::Float64Array>();
    int64_t ts = info.Length() > 5 ? toInt64Ns(info[5]) : 0;
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
      price = o.Get("price").As<Napi::Number>().DoubleValue();
      qty = o.Get("qty").As<Napi::Number>().DoubleValue();
      return true;
    };
    uint8_t hasBid = 0;
    uint8_t hasAsk = 0;
    double bidPrice = 0, bidQty = 0, askPrice = 0, askQty = 0;
    if (!side(info[1], hasBid, bidPrice, bidQty) || !side(info[2], hasAsk, askPrice, askQty))
    {
      return info.Env().Undefined();
    }
    int64_t ts = info.Length() > 3 ? toInt64Ns(info[3]) : 0;
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
    auto opts = info[1].As<Napi::Object>();
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
        return toInt64Ns(v);
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
    _pnl_host = std::make_unique<lrvx_node::PnLTrackerHost>(env, info[0].As<Napi::Object>());
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
    _storage_host = std::make_unique<lrvx_node::StorageSinkHost>(env, info[0].As<Napi::Object>());
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
    _risk_host = std::make_unique<lrvx_node::RiskManagerHost>(env, info[0].As<Napi::Object>(), &_hook_errors);
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
    _kill_host = std::make_unique<lrvx_node::KillSwitchHost>(env, info[0].As<Napi::Object>(), &_hook_errors);
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
        std::make_unique<lrvx_node::OrderValidatorHost>(env, info[0].As<Napi::Object>(), &_hook_errors);
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

    auto obj = info[0].As<Napi::Object>();

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
        std::make_unique<lrvx_node::ExecutorHost>(env, info[0].As<Napi::Object>(), &_hook_errors);
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
    auto* tr = Napi::ObjectWrap<TraceRecorderWrap>::Unwrap(info[0].As<Napi::Object>());
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
    int64_t ts = info[0].As<Napi::Number>().Int64Value();
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
    auto opts = info[0].As<Napi::Object>();
    uint64_t order_id = opts.Get("orderId").As<Napi::Number>().Int64Value();
    uint64_t parent = opts.Has("parentSignalId")
                          ? opts.Get("parentSignalId").As<Napi::Number>().Int64Value()
                          : 0;
    uint32_t sym = opts.Get("symbolId").As<Napi::Number>().Uint32Value();
    uint8_t kind = opts.Get("eventKind").As<Napi::Number>().Uint32Value();
    uint8_t side = opts.Get("side").As<Napi::Number>().Uint32Value();
    uint8_t order_type = opts.Has("orderType")
                             ? opts.Get("orderType").As<Napi::Number>().Uint32Value()
                             : 0;
    double price = opts.Has("price") ? opts.Get("price").As<Napi::Number>().DoubleValue() : 0.0;
    double qty = opts.Has("qty") ? opts.Get("qty").As<Napi::Number>().DoubleValue() : 0.0;
    uint32_t flags = opts.Has("flags") ? opts.Get("flags").As<Napi::Number>().Uint32Value() : 0;
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
    auto opts = info[0].As<Napi::Object>();
    uint64_t order_id = opts.Get("orderId").As<Napi::Number>().Int64Value();
    uint64_t fill_id = opts.Has("fillId")
                           ? opts.Get("fillId").As<Napi::Number>().Int64Value()
                           : 0;
    double price = opts.Get("price").As<Napi::Number>().DoubleValue();
    double qty = opts.Get("qty").As<Napi::Number>().DoubleValue();
    double fee = opts.Has("fee") ? opts.Get("fee").As<Napi::Number>().DoubleValue() : 0.0;
    uint32_t sym = opts.Get("symbolId").As<Napi::Number>().Uint32Value();
    uint8_t side = opts.Get("side").As<Napi::Number>().Uint32Value();
    uint8_t liq = opts.Has("liquidity")
                      ? opts.Get("liquidity").As<Napi::Number>().Uint32Value()
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
