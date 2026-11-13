// node/src/books.h -- OrderBook, L3Book

#pragma once
#include <napi.h>
#include "data_ops.h"
#include "lrvx/capi/lrvx_capi.h"

namespace node_lrvx
{

class OrderBookWrap : public Napi::ObjectWrap<OrderBookWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "OrderBook",
                       {InstanceMethod("applySnapshot", &OrderBookWrap::ApplySnapshot),
                        InstanceMethod("applyDelta", &OrderBookWrap::ApplyDelta),
                        InstanceMethod("bestBid", &OrderBookWrap::BestBid),
                        InstanceMethod("bestAsk", &OrderBookWrap::BestAsk),
                        InstanceMethod("mid", &OrderBookWrap::Mid),
                        InstanceMethod("spread", &OrderBookWrap::Spread),
                        InstanceMethod("isCrossed", &OrderBookWrap::IsCrossed),
                        InstanceMethod("getBids", &OrderBookWrap::GetBids),
                        InstanceMethod("getAsks", &OrderBookWrap::GetAsks),
                        InstanceMethod("clear", &OrderBookWrap::Clear)});
  }
  OrderBookWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<OrderBookWrap>(info),
                                                  _h(lrvx_book_create(argNumber(info[0], "OrderBook: tickSize")))
  {
    if (!_h)
    {
      Napi::RangeError::New(info.Env(), "OrderBook: tick size must be positive")
          .ThrowAsJavaScriptException();
    }
  }
  ~OrderBookWrap()
  {
    if (_h)
    {
      lrvx_book_destroy(_h);
    }
  }

 private:
  void applyUpdate(const Napi::CallbackInfo& info, const std::string& method,
                   void (*fn)(LrvxBookHandle, const double*, const double*, size_t, const double*, const double*, size_t))
  {
    auto bp = argFloat64Array(info[0], (method + ": bidPrices").c_str());
    auto bq = argFloat64Array(info[1], (method + ": bidQtys").c_str());
    auto ap = argFloat64Array(info[2], (method + ": askPrices").c_str());
    auto aq = argFloat64Array(info[3], (method + ": askQtys").c_str());
    // The C ABI takes one length per side, so it assumes price and qty
    // arrays for that side already match. A caller passing mismatched
    // arrays used to have the qty side read out of bounds at whatever
    // length the price side happened to report.
    if (bp.ElementLength() != bq.ElementLength() || ap.ElementLength() != aq.ElementLength())
    {
      Napi::RangeError::New(info.Env(),
                            "OrderBook: bid price/qty and ask price/qty arrays must each have the same length")
          .ThrowAsJavaScriptException();
      return;
    }
    fn(_h, bp.Data(), bq.Data(), bp.ElementLength(), ap.Data(), aq.Data(), ap.ElementLength());
  }
  void ApplySnapshot(const Napi::CallbackInfo& info) { applyUpdate(info, "applySnapshot", lrvx_book_apply_snapshot); }
  void ApplyDelta(const Napi::CallbackInfo& info) { applyUpdate(info, "applyDelta", lrvx_book_apply_delta); }

  Napi::Value optDouble(const Napi::CallbackInfo& info, uint8_t (*fn)(LrvxBookHandle, double*))
  {
    double v = 0;
    if (fn(_h, &v))
    {
      return Napi::Number::New(info.Env(), v);
    }
    return info.Env().Null();
  }
  Napi::Value BestBid(const Napi::CallbackInfo& info) { return optDouble(info, lrvx_book_best_bid); }
  Napi::Value BestAsk(const Napi::CallbackInfo& info) { return optDouble(info, lrvx_book_best_ask); }
  Napi::Value Mid(const Napi::CallbackInfo& info) { return optDouble(info, lrvx_book_mid); }
  Napi::Value Spread(const Napi::CallbackInfo& info) { return optDouble(info, lrvx_book_spread); }
  Napi::Value IsCrossed(const Napi::CallbackInfo& info) { return Napi::Boolean::New(info.Env(), lrvx_book_is_crossed(_h)); }
  void Clear(const Napi::CallbackInfo&) { lrvx_book_clear(_h); }

  Napi::Value getLevels(const Napi::CallbackInfo& info, const std::string& method,
                        uint32_t (*fn)(LrvxBookHandle, double*, double*, uint32_t))
  {
    uint32_t n = argUint32(info[0], (method + ": n").c_str());
    if (n > 100)
    {
      n = 100;
    }
    double prices[100], qtys[100];
    uint32_t got = fn(_h, prices, qtys, n);
    auto arr = Napi::Array::New(info.Env(), got);
    for (uint32_t i = 0; i < got; i++)
    {
      auto lv = Napi::Array::New(info.Env(), 2);
      lv.Set((uint32_t)0, prices[i]);
      lv.Set((uint32_t)1, qtys[i]);
      arr.Set(i, lv);
    }
    return arr;
  }
  Napi::Value GetBids(const Napi::CallbackInfo& info) { return getLevels(info, "getBids", lrvx_book_get_bids); }
  Napi::Value GetAsks(const Napi::CallbackInfo& info) { return getLevels(info, "getAsks", lrvx_book_get_asks); }

  LrvxBookHandle _h;
};

class L3BookWrap : public Napi::ObjectWrap<L3BookWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "L3Book",
                       {InstanceMethod("addOrder", &L3BookWrap::AddOrder),
                        InstanceMethod("removeOrder", &L3BookWrap::RemoveOrder),
                        InstanceMethod("modifyOrder", &L3BookWrap::ModifyOrder),
                        InstanceMethod("bestBid", &L3BookWrap::BestBid),
                        InstanceMethod("bestAsk", &L3BookWrap::BestAsk),
                        InstanceMethod("bidAtPrice", &L3BookWrap::BidAtPrice),
                        InstanceMethod("askAtPrice", &L3BookWrap::AskAtPrice)});
  }
  L3BookWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<L3BookWrap>(info), _h(lrvx_l3_book_create()) {}
  ~L3BookWrap()
  {
    if (_h)
    {
      lrvx_l3_book_destroy(_h);
    }
  }

 private:
  Napi::Value AddOrder(const Napi::CallbackInfo& info)
  {
    std::string side = argString(info[3], "addOrder: side");
    return Napi::Number::New(info.Env(), lrvx_l3_book_add_order(_h,
                                                                argInteger(info[0], "addOrder: orderId"), argNumber(info[1], "addOrder: price"),
                                                                argNumber(info[2], "addOrder: qty"), side == "buy" ? 0 : 1));
  }
  Napi::Value RemoveOrder(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), lrvx_l3_book_remove_order(_h, argInteger(info[0], "removeOrder: orderId"))); }
  Napi::Value ModifyOrder(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), lrvx_l3_book_modify_order(_h, argInteger(info[0], "modifyOrder: orderId"), argNumber(info[1], "modifyOrder: newQty"))); }
  Napi::Value BestBid(const Napi::CallbackInfo& info)
  {
    double v;
    if (lrvx_l3_book_best_bid(_h, &v))
    {
      return Napi::Number::New(info.Env(), v);
    }
    return info.Env().Null();
  }
  Napi::Value BestAsk(const Napi::CallbackInfo& info)
  {
    double v;
    if (lrvx_l3_book_best_ask(_h, &v))
    {
      return Napi::Number::New(info.Env(), v);
    }
    return info.Env().Null();
  }
  Napi::Value BidAtPrice(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), lrvx_l3_book_bid_at_price(_h, argNumber(info[0], "bidAtPrice: price"))); }
  Napi::Value AskAtPrice(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), lrvx_l3_book_ask_at_price(_h, argNumber(info[0], "askAtPrice: price"))); }
  LrvxL3BookHandle _h;
};

class CompositeBookMatrixWrap : public Napi::ObjectWrap<CompositeBookMatrixWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "CompositeBookMatrix",
                       {InstanceMethod("applySnapshot", &CompositeBookMatrixWrap::ApplySnapshot),
                        InstanceMethod("applyDelta", &CompositeBookMatrixWrap::ApplyDelta),
                        InstanceMethod("bestBid", &CompositeBookMatrixWrap::BestBid),
                        InstanceMethod("bestAsk", &CompositeBookMatrixWrap::BestAsk),
                        InstanceMethod("hasArbitrage", &CompositeBookMatrixWrap::HasArb),
                        InstanceMethod("markStale", &CompositeBookMatrixWrap::MarkStale),
                        InstanceMethod("checkStaleness", &CompositeBookMatrixWrap::CheckStaleness)});
  }
  CompositeBookMatrixWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<CompositeBookMatrixWrap>(info), _h(lrvx_composite_book_create()) {}
  ~CompositeBookMatrixWrap()
  {
    if (_h)
    {
      lrvx_composite_book_destroy(_h);
    }
  }

 private:
  // exchange, symbol, bidPx, bidQty, askPx, askQty, recvNs.
  void applyUpdate(const Napi::CallbackInfo& info, const std::string& method,
                   void (*fn)(LrvxCompositeBookHandle, uint32_t, uint32_t, const double*, const double*,
                              size_t, const double*, const double*, size_t, int64_t))
  {
    uint32_t exchange = argUint32(info[0], (method + ": exchange").c_str());
    uint32_t symbol = argUint32(info[1], (method + ": symbol").c_str());
    auto bp = argFloat64Array(info[2], (method + ": bidPrices").c_str());
    auto bq = argFloat64Array(info[3], (method + ": bidQtys").c_str());
    auto ap = argFloat64Array(info[4], (method + ": askPrices").c_str());
    auto aq = argFloat64Array(info[5], (method + ": askQtys").c_str());
    // Same guard as OrderBookWrap::applyUpdate: the C ABI takes one length
    // per side and trusts price/qty to already match.
    if (bp.ElementLength() != bq.ElementLength() || ap.ElementLength() != aq.ElementLength())
    {
      Napi::RangeError::New(info.Env(),
                            "CompositeBookMatrix: bid price/qty and ask price/qty arrays must each have the same length")
          .ThrowAsJavaScriptException();
      return;
    }
    int64_t recvNs = info.Length() > 6 ? toInt64Ns(info[6], (method + ": recvNs").c_str()) : 0;
    fn(_h, exchange, symbol, bp.Data(), bq.Data(), bp.ElementLength(), ap.Data(), aq.Data(),
       ap.ElementLength(), recvNs);
  }
  void ApplySnapshot(const Napi::CallbackInfo& info) { applyUpdate(info, "applySnapshot", lrvx_composite_book_apply_snapshot); }
  void ApplyDelta(const Napi::CallbackInfo& info) { applyUpdate(info, "applyDelta", lrvx_composite_book_apply_delta); }

  Napi::Value BestBid(const Napi::CallbackInfo& info)
  {
    double p = 0, q = 0;
    if (lrvx_composite_book_best_bid(_h, argUint32(info[0], "bestBid: symbol"), &p, &q))
    {
      auto o = Napi::Object::New(info.Env());
      o.Set("price", p);
      o.Set("qty", q);
      return o;
    }
    return info.Env().Null();
  }
  Napi::Value BestAsk(const Napi::CallbackInfo& info)
  {
    double p = 0, q = 0;
    if (lrvx_composite_book_best_ask(_h, argUint32(info[0], "bestAsk: symbol"), &p, &q))
    {
      auto o = Napi::Object::New(info.Env());
      o.Set("price", p);
      o.Set("qty", q);
      return o;
    }
    return info.Env().Null();
  }
  Napi::Value HasArb(const Napi::CallbackInfo& info) { return Napi::Boolean::New(info.Env(), lrvx_composite_book_has_arb(_h, argUint32(info[0], "hasArbitrage: symbol"))); }
  void MarkStale(const Napi::CallbackInfo& info) { lrvx_composite_book_mark_stale(_h, argUint32(info[0], "markStale: exchange"), argUint32(info[1], "markStale: symbol")); }
  void CheckStaleness(const Napi::CallbackInfo& info) { lrvx_composite_book_check_staleness(_h, toInt64Ns(info[0], "checkStaleness: nowNs"), toInt64Ns(info[1], "checkStaleness: thresholdNs")); }
  LrvxCompositeBookHandle _h;
};

inline void registerBooks(Napi::Env env, Napi::Object exports)
{
  exports.Set("OrderBook", OrderBookWrap::Init(env));
  exports.Set("L3Book", L3BookWrap::Init(env));
  exports.Set("CompositeBookMatrix", CompositeBookMatrixWrap::Init(env));
}

}  // namespace node_lrvx
