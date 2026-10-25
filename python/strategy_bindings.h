// python/strategy_bindings.h

#pragma once

#include <pybind11/functional.h>
#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <limits>
#include <stdexcept>

#include "callback_guard.h"
#include "hook_bindings.h"
#include "lrvx/backtest/backtest_config.h"
#include "lrvx/backtest/backtest_result.h"
#include "lrvx/backtest/backtest_runner.h"
#include "lrvx/backtest/venue_stack.h"
#include "lrvx/book/bus/book_update_bus.h"
#include "lrvx/book/bus/trade_bus.h"
#include "lrvx/book/events/book_update_event.h"
#include "lrvx/book/events/trade_event.h"
#include "lrvx/capi/bridge_strategy.h"
#include "lrvx/capi/lrvx_capi.h"
#include "lrvx/capi/order_type_names.hpp"
#include "lrvx/engine/symbol_registry.h"
#include "lrvx/error/lrvx_error.h"
#include "lrvx/replay/abstract_event_reader.h"
#include "lrvx/replay/binary_format_v1.h"
#include "lrvx/replay/binary_log_recorder_hook.h"
#include "lrvx/run/trace_recorder.h"
#include "lrvx/util/memory/pool.h"
#include "replay_bindings.h"
#include "types_bindings.h"

#include <atomic>
#include <cctype>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace py = pybind11;

namespace
{

using namespace lrvx;

// PyTradeData lives in types_bindings.h (shared with hook_bindings.h).

struct PyBarData
{
  uint32_t symbol;
  std::string symbol_name;
  uint8_t bar_type;
  uint64_t bar_type_param;
  double open;
  double high;
  double low;
  double close;
  double volume;
  double buy_volume;
  int64_t start_time_ns;
  int64_t end_time_ns;
  uint8_t close_reason;
};

struct PySymbolCtx
{
  uint32_t symbol_id;
  std::string symbol;
  double position;
  double last_trade_price;
  // None when the side has no best level. A book may be quoted at exactly
  // zero, so 0.0 is a price here, never a stand-in for "no quote".
  std::optional<double> best_bid;
  std::optional<double> best_ask;
  std::optional<double> mid_price;
  double unrealized_pnl;

  std::optional<double> book_spread() const
  {
    if (best_bid && best_ask)
    {
      return *best_ask - *best_bid;
    }
    return std::nullopt;
  }

  bool is_long() const { return position > 0; }
  bool is_short() const { return position < 0; }
  bool is_flat() const { return position == 0; }
};

// The presence flags say whether a side has a quote; the price fields cannot,
// since a book may be quoted at exactly zero.
inline void fillQuotes(PySymbolCtx& pc, const LrvxBookSnapshot& book)
{
  if (book.has_bid)
  {
    pc.best_bid = lrvx_price_to_double(book.bid_price_raw);
  }
  if (book.has_ask)
  {
    pc.best_ask = lrvx_price_to_double(book.ask_price_raw);
  }
  if (book.has_bid && book.has_ask)
  {
    pc.mid_price = lrvx_price_to_double(book.mid_raw);
  }
}

// Order-event payload exposed to the Python `on_fill` /
// `on_order_update` hooks. Mirrors `LrvxOrderEventData` from the C
// ABI but with double prices/qtys so user code doesn't need to
// touch raw fixed-point.
struct PyOrderEventData
{
  uint64_t order_id;
  uint32_t symbol_id;
  std::string side;        // "buy" | "sell"
  std::string order_type;  // "limit" | "market" | "stop_market" | ...
  std::string status;      // "FILLED" | "PARTIALLY_FILLED" | "CANCELED" | ...
  double fill_qty;
  double fill_price;
  int64_t exchange_ts_ns;
  std::string reject_reason;  // empty unless status == "REJECTED"
  double queue_ahead{0.0};    // backtest only: volume ahead at level
  double queue_total{0.0};    // backtest only: total quantity at level
  // Per-lifecycle-stage timestamps. Zero means the stage has not
  // fired yet for this order.
  int64_t submitted_at_ns{0};
  int64_t accepted_at_ns{0};
  int64_t first_fill_at_ns{0};
  int64_t last_fill_at_ns{0};
  int64_t canceled_at_ns{0};
  int64_t rejected_at_ns{0};
  int64_t triggered_at_ns{0};
  int64_t expired_at_ns{0};
  bool is_maker{false};         // fill statuses only
  std::string fill_role;        // "maker" | "taker" | "" if not a fill
  std::string market_position;  // "best" | "behind_best" | "mid_spread" | "level_empty" | "crossed" | ""
  int32_t distance_to_best_ticks{0};
};

inline Side parseSide(const std::string& s)
{
  return (s == "buy") ? Side::BUY : Side::SELL;
}

class PyStrategyBase
{
 public:
  PyStrategyBase(std::vector<uint32_t> symbols) : _symbols(std::move(symbols)) {}

  virtual ~PyStrategyBase() = default;

  virtual void on_trade(const PySymbolCtx& ctx, const PyTradeData& trade) {}
  virtual void on_book_update(const PySymbolCtx& ctx) {}
  virtual void on_bar(const PySymbolCtx& ctx, const PyBarData& bar) {}
  virtual void on_fill(const PySymbolCtx& ctx, const PyOrderEventData& ev) {}
  virtual void on_order_update(const PySymbolCtx& ctx, const PyOrderEventData& ev) {}
  virtual void on_queue_position_change(const PySymbolCtx& ctx,
                                        const PyOrderEventData& ev) {}
  virtual void on_market_position_change(const PySymbolCtx& ctx,
                                         const PyOrderEventData& ev) {}
  virtual void on_start() {}
  virtual void on_stop() {}

  uint64_t emit_market_buy(uint32_t symbol, double qty)
  {
    if (!_bridge)
    {
      return 0;
    }
    return _bridge->publicEmitMarketBuy(symbol, Quantity::fromDouble(qty));
  }

  uint64_t emit_market_sell(uint32_t symbol, double qty)
  {
    if (!_bridge)
    {
      return 0;
    }
    return _bridge->publicEmitMarketSell(symbol, Quantity::fromDouble(qty));
  }

  uint64_t emit_limit_buy(uint32_t symbol, double price, double qty)
  {
    if (!_bridge)
    {
      return 0;
    }
    return _bridge->publicEmitLimitBuy(symbol, Price::fromDouble(price),
                                       Quantity::fromDouble(qty));
  }

  uint64_t emit_limit_sell(uint32_t symbol, double price, double qty)
  {
    if (!_bridge)
    {
      return 0;
    }
    return _bridge->publicEmitLimitSell(symbol, Price::fromDouble(price),
                                        Quantity::fromDouble(qty));
  }

  uint64_t emit_provide_liquidity(uint32_t pool, double price_lower, double price_upper,
                                  double liquidity)
  {
    if (!_bridge)
    {
      return 0;
    }
    return _bridge->publicEmitProvideLiquidity(pool, Price::fromDouble(price_lower),
                                               Price::fromDouble(price_upper),
                                               Quantity::fromDouble(liquidity));
  }

  uint64_t emit_withdraw_liquidity(uint32_t pool, double liquidity)
  {
    if (!_bridge)
    {
      return 0;
    }
    return _bridge->publicEmitWithdrawLiquidity(pool, Quantity::fromDouble(liquidity));
  }

  void emit_cancel(uint64_t order_id)
  {
    if (_bridge)
    {
      _bridge->publicEmitCancel(order_id);
    }
  }

  void emit_cancel_all(uint32_t symbol)
  {
    if (_bridge)
    {
      _bridge->publicEmitCancelAll(symbol);
    }
  }

  void emit_modify(uint64_t order_id, double new_price, double new_qty)
  {
    if (_bridge)
    {
      _bridge->publicEmitModify(order_id, Price::fromDouble(new_price),
                                Quantity::fromDouble(new_qty));
    }
  }

  uint64_t emit_stop_market(uint32_t symbol, const std::string& side, double trigger, double qty)
  {
    if (!_bridge)
    {
      return 0;
    }
    return _bridge->publicEmitStopMarket(symbol, parseSide(side), Price::fromDouble(trigger),
                                         Quantity::fromDouble(qty));
  }

  uint64_t emit_stop_limit(uint32_t symbol, const std::string& side, double trigger,
                           double limit_price, double qty)
  {
    if (!_bridge)
    {
      return 0;
    }
    return _bridge->publicEmitStopLimit(symbol, parseSide(side), Price::fromDouble(trigger),
                                        Price::fromDouble(limit_price),
                                        Quantity::fromDouble(qty));
  }

  uint64_t emit_take_profit_market(uint32_t symbol, const std::string& side, double trigger,
                                   double qty)
  {
    if (!_bridge)
    {
      return 0;
    }
    return _bridge->publicEmitTakeProfitMarket(symbol, parseSide(side),
                                               Price::fromDouble(trigger),
                                               Quantity::fromDouble(qty));
  }

  uint64_t emit_take_profit_limit(uint32_t symbol, const std::string& side, double trigger,
                                  double limit_price, double qty)
  {
    if (!_bridge)
    {
      return 0;
    }
    return _bridge->publicEmitTakeProfitLimit(symbol, parseSide(side),
                                              Price::fromDouble(trigger),
                                              Price::fromDouble(limit_price),
                                              Quantity::fromDouble(qty));
  }

  uint64_t emit_trailing_stop(uint32_t symbol, const std::string& side, double offset, double qty)
  {
    if (!_bridge)
    {
      return 0;
    }
    return _bridge->publicEmitTrailingStop(symbol, parseSide(side), Price::fromDouble(offset),
                                           Quantity::fromDouble(qty));
  }

  uint64_t emit_trailing_stop_percent(uint32_t symbol, const std::string& side,
                                      int32_t callback_bps, double qty)
  {
    if (!_bridge)
    {
      return 0;
    }
    return _bridge->publicEmitTrailingStopPercent(symbol, parseSide(side), callback_bps,
                                                  Quantity::fromDouble(qty));
  }

  uint64_t emit_limit_buy_tif(uint32_t symbol, double price, double qty, const std::string& tif)
  {
    if (!_bridge)
    {
      return 0;
    }
    TimeInForce t = TimeInForce::GTC;
    if (tif == "ioc")
    {
      t = TimeInForce::IOC;
    }
    else if (tif == "fok")
    {
      t = TimeInForce::FOK;
    }
    else if (tif == "post_only")
    {
      t = TimeInForce::POST_ONLY;
    }
    return _bridge->publicEmitLimitBuyTif(symbol, Price::fromDouble(price),
                                          Quantity::fromDouble(qty), t);
  }

  uint64_t emit_limit_sell_tif(uint32_t symbol, double price, double qty, const std::string& tif)
  {
    if (!_bridge)
    {
      return 0;
    }
    TimeInForce t = TimeInForce::GTC;
    if (tif == "ioc")
    {
      t = TimeInForce::IOC;
    }
    else if (tif == "fok")
    {
      t = TimeInForce::FOK;
    }
    else if (tif == "post_only")
    {
      t = TimeInForce::POST_ONLY;
    }
    return _bridge->publicEmitLimitSellTif(symbol, Price::fromDouble(price),
                                           Quantity::fromDouble(qty), t);
  }

  uint64_t emit_close_position(uint32_t symbol)
  {
    if (!_bridge)
    {
      return 0;
    }
    return _bridge->publicEmitClosePosition(symbol);
  }

  int32_t get_order_status(uint64_t order_id) const
  {
    if (!_bridge)
    {
      return -1;
    }
    auto status = _bridge->getOrderStatus(order_id);
    return status ? static_cast<int32_t>(*status) : -1;
  }

  double position(std::optional<uint32_t> symbol = std::nullopt) const
  {
    if (!_bridge)
    {
      return 0.0;
    }
    return _bridge->position(symbol.value_or(_symbols[0])).toDouble();
  }

  PySymbolCtx ctx(std::optional<uint32_t> symbol = std::nullopt) const
  {
    PySymbolCtx result{};
    if (!_bridge)
    {
      return result;
    }
    const auto& c = _bridge->ctx(symbol.value_or(_symbols[0]));
    result.symbol_id = c.symbolId;
    result.position = c.position.toDouble();
    result.last_trade_price = c.lastTradePrice.toDouble();
    // The touch, not book.bestBid()/bestAsk(): it also follows TOP_OF_BOOK
    // frames the depth book ignores, same as the C API snapshots.
    if (c.touchBid)
    {
      result.best_bid = c.touchBid->price.toDouble();
    }
    if (c.touchAsk)
    {
      result.best_ask = c.touchAsk->price.toDouble();
    }
    if (auto mid = c.touchMid())
    {
      result.mid_price = mid->toDouble();
    }
    // NaN when the position manager reports no entry price. This used to be
    // position times mark, i.e. the whole notional reported as profit.
    result.unrealized_pnl =
        c.unrealizedPnl().value_or(std::numeric_limits<double>::quiet_NaN());
    return result;
  }

  const std::vector<uint32_t>& symbols() const { return _symbols; }

  const std::vector<std::string>& symbol_names() const { return _symbolNames; }

  // Internal: set up bridge for backtest
  void _initBridge(BridgeStrategy* bridge)
  {
    _bridge = bridge;
    _buildSymbolMap();
  }

  // ------------------------------------------------------------------
  // Ergonomic API: string symbols, kwargs, no emit_ prefix
  // ------------------------------------------------------------------

  uint64_t market_buy(double qty, std::optional<std::string> symbol = std::nullopt)
  {
    return emit_market_buy(_resolve(symbol), qty);
  }

  uint64_t market_sell(double qty, std::optional<std::string> symbol = std::nullopt)
  {
    return emit_market_sell(_resolve(symbol), qty);
  }

  uint64_t limit_buy(double price, double qty, std::optional<std::string> symbol = std::nullopt,
                     const std::string& tif = "gtc")
  {
    return emit_limit_buy_tif(_resolve(symbol), price, qty, tif);
  }

  uint64_t limit_sell(double price, double qty, std::optional<std::string> symbol = std::nullopt,
                      const std::string& tif = "gtc")
  {
    return emit_limit_sell_tif(_resolve(symbol), price, qty, tif);
  }

  uint64_t stop_market(const std::string& side, double trigger, double qty,
                       std::optional<std::string> symbol = std::nullopt)
  {
    return emit_stop_market(_resolve(symbol), side, trigger, qty);
  }

  uint64_t stop_limit(const std::string& side, double trigger, double limit_price, double qty,
                      std::optional<std::string> symbol = std::nullopt)
  {
    return emit_stop_limit(_resolve(symbol), side, trigger, limit_price, qty);
  }

  uint64_t take_profit_market(const std::string& side, double trigger, double qty,
                              std::optional<std::string> symbol = std::nullopt)
  {
    return emit_take_profit_market(_resolve(symbol), side, trigger, qty);
  }

  uint64_t take_profit_limit(const std::string& side, double trigger, double limit_price,
                             double qty, std::optional<std::string> symbol = std::nullopt)
  {
    return emit_take_profit_limit(_resolve(symbol), side, trigger, limit_price, qty);
  }

  uint64_t trailing_stop(const std::string& side, double offset, double qty,
                         std::optional<std::string> symbol = std::nullopt)
  {
    return emit_trailing_stop(_resolve(symbol), side, offset, qty);
  }

  uint64_t trailing_stop_percent(const std::string& side, int32_t callback_bps, double qty,
                                 std::optional<std::string> symbol = std::nullopt)
  {
    return emit_trailing_stop_percent(_resolve(symbol), side, callback_bps, qty);
  }

  uint64_t close_position(std::optional<std::string> symbol = std::nullopt)
  {
    return emit_close_position(_resolve(symbol));
  }

  void cancel_order(uint64_t order_id) { emit_cancel(order_id); }

  void cancel_all_orders(std::optional<std::string> symbol = std::nullopt)
  {
    emit_cancel_all(_resolve(symbol));
  }

  void modify_order(uint64_t order_id, double new_price, double new_qty)
  {
    emit_modify(order_id, new_price, new_qty);
  }

  double pos(std::optional<std::string> symbol = std::nullopt) const
  {
    return position(_resolve(symbol));
  }

  double last_price(std::optional<std::string> symbol = std::nullopt) const
  {
    if (!_bridge)
    {
      return 0.0;
    }
    return _bridge->ctx(_resolve(symbol)).lastTradePrice.toDouble();
  }

  // None when there is no quote (no book yet, or an empty side); 0.0 is a price.
  std::optional<double> best_bid(std::optional<std::string> symbol = std::nullopt) const
  {
    if (!_bridge)
    {
      return std::nullopt;
    }
    const auto& bid = _bridge->ctx(_resolve(symbol)).touchBid;
    return bid ? std::optional<double>(bid->price.toDouble()) : std::nullopt;
  }

  std::optional<double> best_ask(std::optional<std::string> symbol = std::nullopt) const
  {
    if (!_bridge)
    {
      return std::nullopt;
    }
    const auto& ask = _bridge->ctx(_resolve(symbol)).touchAsk;
    return ask ? std::optional<double>(ask->price.toDouble()) : std::nullopt;
  }

  std::optional<double> mid_price(std::optional<std::string> symbol = std::nullopt) const
  {
    if (!_bridge)
    {
      return std::nullopt;
    }
    auto mid = _bridge->ctx(_resolve(symbol)).touchMid();
    return mid ? std::optional<double>(mid->toDouble()) : std::nullopt;
  }

  int32_t order_status(uint64_t order_id) const { return get_order_status(order_id); }

  std::string primary_symbol_name() const
  {
    return _symbolNames.empty() ? "" : _symbolNames[0];
  }

  // Multi-TF alignment helpers. Bars get pushed into a per-(symbol,
  // timeframe) ring as the engine dispatches them; these accessors
  // pull the most recent N back without bookkeeping in user code.
  std::optional<py::dict> last_closed_bar(uint32_t symbol_id, uint8_t bar_type, uint64_t param) const
  {
    if (!_bridge)
    {
      return std::nullopt;
    }
    auto bar = _bridge->lastClosedBar(symbol_id, static_cast<lrvx::BarType>(bar_type), param);
    if (!bar)
    {
      return std::nullopt;
    }
    py::dict d;
    d["open"] = bar->open.toDouble();
    d["high"] = bar->high.toDouble();
    d["low"] = bar->low.toDouble();
    d["close"] = bar->close.toDouble();
    d["volume"] = bar->volume.toDouble();
    d["start_ns"] = bar->startTime.time_since_epoch().count();
    d["end_ns"] = bar->endTime.time_since_epoch().count();
    return d;
  }

  py::list last_n_closed_bars(uint32_t symbol_id, uint8_t bar_type, uint64_t param, size_t n) const
  {
    py::list out;
    if (!_bridge)
    {
      return out;
    }
    auto bars = _bridge->lastNClosedBars(symbol_id, static_cast<lrvx::BarType>(bar_type), param, n);
    for (const auto& bar : bars)
    {
      py::dict d;
      d["open"] = bar.open.toDouble();
      d["high"] = bar.high.toDouble();
      d["low"] = bar.low.toDouble();
      d["close"] = bar.close.toDouble();
      d["volume"] = bar.volume.toDouble();
      d["start_ns"] = bar.startTime.time_since_epoch().count();
      d["end_ns"] = bar.endTime.time_since_epoch().count();
      out.append(d);
    }
    return out;
  }

  size_t bar_ring_capacity() const
  {
    return _bridge ? _bridge->barRingCapacity() : 0;
  }

  void set_bar_ring_capacity(size_t n)
  {
    if (_bridge)
    {
      _bridge->setBarRingCapacity(n);
    }
  }

 private:
  std::vector<uint32_t> _symbols;
  std::vector<std::string> _symbolNames;
  std::unordered_map<std::string, uint32_t> _symbolMap;
  std::unordered_map<uint32_t, std::string> _reverseMap;
  BridgeStrategy* _bridge = nullptr;

  void _buildSymbolMap()
  {
    if (!_bridge)
    {
      return;
    }
    _symbolNames.clear();
    _symbolMap.clear();
    _reverseMap.clear();
    const auto& reg = _bridge->registry();
    for (uint32_t id : _symbols)
    {
      auto [exchange, name] = reg.getSymbolName(id);
      _symbolNames.push_back(name);
      _symbolMap[name] = id;
      _reverseMap[id] = name;
    }
  }

  uint32_t _resolve(std::optional<std::string> symbol) const
  {
    if (!symbol.has_value())
    {
      return _symbols.empty() ? 0 : _symbols[0];
    }
    auto it = _symbolMap.find(symbol.value());
    if (it != _symbolMap.end())
    {
      return it->second;
    }
    throw lrvx::LrvxError(
        "E_SYM_001",
        "Symbol '" + symbol.value() +
            "' is not registered. Add it via Engine.add_symbol() "
            "before referencing it.");
  }

  uint32_t _resolve(std::optional<uint32_t> symbol) const
  {
    return symbol.value_or(_symbols.empty() ? 0 : _symbols[0]);
  }

  std::string _reverseLookup(uint32_t id) const
  {
    auto it = _reverseMap.find(id);
    return it != _reverseMap.end() ? it->second : "";
  }
};

class PyStrategyTrampoline : public PyStrategyBase
{
 public:
  using PyStrategyBase::PyStrategyBase;

  void on_trade(const PySymbolCtx& ctx, const PyTradeData& trade) override
  {
    PYBIND11_OVERRIDE(void, PyStrategyBase, on_trade, ctx, trade);
  }

  void on_book_update(const PySymbolCtx& ctx) override
  {
    PYBIND11_OVERRIDE(void, PyStrategyBase, on_book_update, ctx);
  }

  void on_bar(const PySymbolCtx& ctx, const PyBarData& bar) override
  {
    PYBIND11_OVERRIDE(void, PyStrategyBase, on_bar, ctx, bar);
  }

  void on_fill(const PySymbolCtx& ctx, const PyOrderEventData& ev) override
  {
    PYBIND11_OVERRIDE(void, PyStrategyBase, on_fill, ctx, ev);
  }

  void on_order_update(const PySymbolCtx& ctx, const PyOrderEventData& ev) override
  {
    PYBIND11_OVERRIDE(void, PyStrategyBase, on_order_update, ctx, ev);
  }

  void on_queue_position_change(const PySymbolCtx& ctx,
                                const PyOrderEventData& ev) override
  {
    PYBIND11_OVERRIDE(void, PyStrategyBase, on_queue_position_change, ctx, ev);
  }

  void on_market_position_change(const PySymbolCtx& ctx,
                                 const PyOrderEventData& ev) override
  {
    PYBIND11_OVERRIDE(void, PyStrategyBase, on_market_position_change, ctx, ev);
  }

  void on_start() override { PYBIND11_OVERRIDE(void, PyStrategyBase, on_start); }

  void on_stop() override { PYBIND11_OVERRIDE(void, PyStrategyBase, on_stop); }
};

// PySignal + pySignalFromC live in types_bindings.h (shared with hook_bindings.h).

// ──────────────────────────────────────────────────────────────────────
// PyStrategyHost — owns a BridgeStrategy, wires Python callbacks.
// with_gil: true when callbacks fire from a non-Python thread
//           (i.e., LiveEngine consumer threads).
// ──────────────────────────────────────────────────────────────────────

struct PyStrategyHost
{
  std::atomic<PyStrategyBase*> strategy;
  std::unique_ptr<BridgeStrategy> bridge;
  bool with_gil;

  // Every one of the nine bridge callbacks goes through here. The
  // engine is C code in the middle of delivering an event, so a Python
  // exception must not unwind past this frame: on the live path it
  // would kill the bus consumer and the strategy would stop receiving
  // events, and out of a frame with C linkage it is undefined
  // behaviour either way. The exception is contained and its text is
  // reported through the engine log; callback_guard.h has the policy.
  template <typename Fn>
  static void dispatch(const PyStrategyHost* self, const char* source, Fn&& fn)
  {
    if (self->with_gil)
    {
      py::gil_scoped_acquire gil;
      lrvx::guardCallback(fn, source);
    }
    else
    {
      lrvx::guardCallback(fn, source);
    }
  }

  // Atomically swap the user-facing strategy. Old strategy.on_stop
  // fires before the swap, new strategy.on_start fires after. Bus
  // subscriptions, in-flight orders, and the bridge's internal
  // state survive untouched.
  void replace_strategy(PyStrategyBase* new_strat)
  {
    PyStrategyBase* old = strategy.load(std::memory_order_acquire);
    auto fire_lifecycle = [this](PyStrategyBase* s, bool start)
    {
      if (!s)
      {
        return;
      }
      dispatch(this, start ? "Strategy.on_start" : "Strategy.on_stop",
               [s, start]
               {
                 if (start)
                 {
                   s->on_start();
                 }
                 else
                 {
                   s->on_stop();
                 }
               });
    };
    fire_lifecycle(old, false);
    new_strat->_initBridge(bridge.get());
    strategy.store(new_strat, std::memory_order_release);
    fire_lifecycle(new_strat, true);
  }

  PyStrategyHost(PyStrategyBase* strat, SymbolRegistry* reg,
                 uint32_t id, bool with_gil_)
      : strategy(strat), with_gil(with_gil_)
  {
    LrvxStrategyCallbacks cbs{};
    cbs.user_data = this;
    cbs.on_trade = &PyStrategyHost::onTrade;
    cbs.on_book = &PyStrategyHost::onBook;
    cbs.on_bar = &PyStrategyHost::onBar;
    cbs.on_start = &PyStrategyHost::onStart;
    cbs.on_stop = &PyStrategyHost::onStop;
    cbs.on_fill = &PyStrategyHost::onFill;
    cbs.on_order_update = &PyStrategyHost::onOrderUpdate;
    cbs.on_queue_position_change = &PyStrategyHost::onQueuePositionChange;
    cbs.on_market_position_change = &PyStrategyHost::onMarketPositionChange;

    const auto& syms = strat->symbols();
    bridge = std::make_unique<BridgeStrategy>(
        static_cast<SubscriberId>(id), syms, *reg, cbs);
    strat->_initBridge(bridge.get());
  }

  // Callbacks are C function pointers — no captures allowed.
  static void onTrade(void* ud, const LrvxSymbolContext* ctx,
                      const LrvxTradeData* trade)
  {
    auto* self = static_cast<PyStrategyHost*>(ud);
    auto call = [self, ctx, trade]()
    {
      PySymbolCtx pc{};
      pc.symbol_id = ctx->symbol_id;
      pc.position = lrvx_quantity_to_double(ctx->position_raw);
      pc.last_trade_price = lrvx_price_to_double(ctx->last_trade_price_raw);
      fillQuotes(pc, ctx->book);

      PyTradeData pt{};
      pt.symbol = trade->symbol;
      pt.price = lrvx_price_to_double(trade->price_raw);
      pt.quantity = lrvx_quantity_to_double(trade->quantity_raw);
      pt.is_buy = trade->is_buy != 0;
      pt.side = pt.is_buy ? "buy" : "sell";
      pt.timestamp_ns = trade->exchange_ts_ns;
      pt.exchange_ts_ns = trade->exchange_ts_ns;

      self->strategy.load(std::memory_order_acquire)->on_trade(pc, pt);
    };
    dispatch(self, "Strategy.on_trade", call);
  }

  static void onBook(void* ud, const LrvxSymbolContext* ctx,
                     const LrvxBookData* /*book*/)
  {
    auto* self = static_cast<PyStrategyHost*>(ud);
    auto call = [self, ctx]()
    {
      PySymbolCtx pc{};
      pc.symbol_id = ctx->symbol_id;
      pc.position = lrvx_quantity_to_double(ctx->position_raw);
      pc.last_trade_price = lrvx_price_to_double(ctx->last_trade_price_raw);
      fillQuotes(pc, ctx->book);
      self->strategy.load(std::memory_order_acquire)->on_book_update(pc);
    };
    dispatch(self, "Strategy.on_book_update", call);
  }

  static void onBar(void* ud, const LrvxSymbolContext* ctx,
                    const LrvxBarData* bar)
  {
    auto* self = static_cast<PyStrategyHost*>(ud);
    auto call = [self, ctx, bar]()
    {
      PySymbolCtx pc{};
      pc.symbol_id = ctx->symbol_id;
      pc.position = lrvx_quantity_to_double(ctx->position_raw);
      pc.last_trade_price = lrvx_price_to_double(ctx->last_trade_price_raw);
      fillQuotes(pc, ctx->book);

      PyBarData pb{};
      pb.symbol = bar->symbol;
      pb.bar_type = bar->bar_type;
      pb.bar_type_param = bar->bar_type_param;
      pb.open = lrvx_price_to_double(bar->open_raw);
      pb.high = lrvx_price_to_double(bar->high_raw);
      pb.low = lrvx_price_to_double(bar->low_raw);
      pb.close = lrvx_price_to_double(bar->close_raw);
      pb.volume = lrvx_quantity_to_double(bar->volume_raw);
      pb.buy_volume = lrvx_quantity_to_double(bar->buy_volume_raw);
      pb.start_time_ns = bar->start_time_ns;
      pb.end_time_ns = bar->end_time_ns;
      pb.close_reason = bar->close_reason;

      self->strategy.load(std::memory_order_acquire)->on_bar(pc, pb);
    };
    dispatch(self, "Strategy.on_bar", call);
  }

  static void onStart(void* ud)
  {
    auto* self = static_cast<PyStrategyHost*>(ud);
    dispatch(self, "Strategy.on_start",
             [self]
             { self->strategy.load(std::memory_order_acquire)->on_start(); });
  }

  static PyOrderEventData toPyOrderEvent(const LrvxOrderEventData* ev)
  {
    PyOrderEventData pe{};
    pe.order_id = ev->order_id;
    pe.symbol_id = ev->symbol_id;
    pe.side = (ev->side == 0) ? "buy" : "sell";
    pe.order_type = orderTypeName(ev->order_type);
    pe.status = orderStatusName(ev->status);
    pe.fill_qty = lrvx_quantity_to_double(ev->fill_qty_raw);
    pe.fill_price = lrvx_price_to_double(ev->fill_price_raw);
    pe.exchange_ts_ns = ev->exchange_ts_ns;
    pe.reject_reason = ev->reject_reason ? std::string(ev->reject_reason) : "";
    pe.queue_ahead = lrvx_quantity_to_double(ev->queue_ahead_raw);
    pe.queue_total = lrvx_quantity_to_double(ev->queue_total_raw);
    pe.submitted_at_ns = ev->submitted_at_ns;
    pe.accepted_at_ns = ev->accepted_at_ns;
    pe.first_fill_at_ns = ev->first_fill_at_ns;
    pe.last_fill_at_ns = ev->last_fill_at_ns;
    pe.canceled_at_ns = ev->canceled_at_ns;
    pe.rejected_at_ns = ev->rejected_at_ns;
    pe.triggered_at_ns = ev->triggered_at_ns;
    pe.expired_at_ns = ev->expired_at_ns;
    pe.is_maker = (ev->is_maker != 0);
    if (ev->status == 3 /* PARTIALLY_FILLED */ || ev->status == 4 /* FILLED */)
    {
      pe.fill_role = pe.is_maker ? "maker" : "taker";
    }
    pe.market_position = marketPositionName(ev->market_position);
    pe.distance_to_best_ticks = ev->distance_to_best_ticks;
    return pe;
  }

  // The canonical table, not a copy of it. The copy that used to sit
  // here spelled codes 4 and 5 "take_profit_market"/"take_profit_limit"
  // and had no case 7, so an iceberg fill read back "unknown".
  static const char* orderTypeName(uint8_t t) { return lrvx::capi::orderTypeNameLower(t); }

  static const char* orderStatusName(uint8_t s)
  {
    switch (s)
    {
      case 0:
        return "NEW";
      case 1:
        return "SUBMITTED";
      case 2:
        return "ACCEPTED";
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

  static void onFill(void* ud, const LrvxSymbolContext* ctx,
                     const LrvxOrderEventData* ev)
  {
    auto* self = static_cast<PyStrategyHost*>(ud);
    auto call = [self, ctx, ev]()
    {
      PySymbolCtx pc{};
      pc.symbol_id = ctx->symbol_id;
      pc.position = lrvx_quantity_to_double(ctx->position_raw);
      pc.last_trade_price = lrvx_price_to_double(ctx->last_trade_price_raw);
      fillQuotes(pc, ctx->book);
      PyOrderEventData pe = toPyOrderEvent(ev);
      self->strategy.load(std::memory_order_acquire)->on_fill(pc, pe);
    };
    dispatch(self, "Strategy.on_fill", call);
  }

  static void onOrderUpdate(void* ud, const LrvxSymbolContext* ctx,
                            const LrvxOrderEventData* ev)
  {
    auto* self = static_cast<PyStrategyHost*>(ud);
    auto call = [self, ctx, ev]()
    {
      PySymbolCtx pc{};
      pc.symbol_id = ctx->symbol_id;
      pc.position = lrvx_quantity_to_double(ctx->position_raw);
      pc.last_trade_price = lrvx_price_to_double(ctx->last_trade_price_raw);
      fillQuotes(pc, ctx->book);
      PyOrderEventData pe = toPyOrderEvent(ev);
      self->strategy.load(std::memory_order_acquire)->on_order_update(pc, pe);
    };
    dispatch(self, "Strategy.on_order_update", call);
  }

  static void onQueuePositionChange(void* ud, const LrvxSymbolContext* ctx,
                                    const LrvxOrderEventData* ev)
  {
    auto* self = static_cast<PyStrategyHost*>(ud);
    auto call = [self, ctx, ev]()
    {
      PySymbolCtx pc{};
      pc.symbol_id = ctx->symbol_id;
      pc.position = lrvx_quantity_to_double(ctx->position_raw);
      pc.last_trade_price = lrvx_price_to_double(ctx->last_trade_price_raw);
      fillQuotes(pc, ctx->book);
      PyOrderEventData pe = toPyOrderEvent(ev);
      self->strategy.load(std::memory_order_acquire)->on_queue_position_change(pc, pe);
    };
    dispatch(self, "Strategy.on_queue_position_change", call);
  }

  static void onMarketPositionChange(void* ud, const LrvxSymbolContext* ctx,
                                     const LrvxOrderEventData* ev)
  {
    auto* self = static_cast<PyStrategyHost*>(ud);
    auto call = [self, ctx, ev]()
    {
      PySymbolCtx pc{};
      pc.symbol_id = ctx->symbol_id;
      pc.position = lrvx_quantity_to_double(ctx->position_raw);
      pc.last_trade_price = lrvx_price_to_double(ctx->last_trade_price_raw);
      fillQuotes(pc, ctx->book);
      PyOrderEventData pe = toPyOrderEvent(ev);
      self->strategy.load(std::memory_order_acquire)
          ->on_market_position_change(pc, pe);
    };
    dispatch(self, "Strategy.on_market_position_change", call);
  }

  static void onStop(void* ud)
  {
    auto* self = static_cast<PyStrategyHost*>(ud);
    dispatch(self, "Strategy.on_stop",
             [self]
             { self->strategy.load(std::memory_order_acquire)->on_stop(); });
  }
};

// ──────────────────────────────────────────────────────────────────────
// PyStrategyRunner — synchronous strategy host.
//
// Market data is pushed from the Python thread (e.g. asyncio callback,
// WebSocket handler). Strategy on_trade / on_book callbacks fire
// synchronously before the push call returns. Orders emitted by the
// strategy are forwarded to the on_signal Python callable.
// ──────────────────────────────────────────────────────────────────────

class PyStrategyRunner
{
 public:
  PyStrategyRunner(SymbolRegistry* reg, py::object on_signal)
      : _reg(reg), _on_signal(std::move(on_signal))
  {
    _runner = lrvx_runner_create(static_cast<LrvxRegistryHandle>(reg),
                                 &PyStrategyRunner::signalCallback, this);
  }

  ~PyStrategyRunner()
  {
    if (_binlog_recorder_handle)
    {
      lrvx_market_data_recorder_destroy(_binlog_recorder_handle);
      _binlog_recorder_handle = nullptr;
    }
    if (_runner)
    {
      lrvx_runner_destroy(_runner);
    }
  }

  void add_strategy(PyStrategyBase* strat)
  {
    uint32_t id = static_cast<uint32_t>(_hosts.size()) + 1;
    auto host = std::make_unique<PyStrategyHost>(strat, _reg, id, false);
    lrvx_runner_add_strategy(_runner,
                             static_cast<LrvxStrategyHandle>(host->bridge.get()));
    _hosts.push_back(std::move(host));
  }

  void replace_strategy(uint32_t index, PyStrategyBase* new_strat)
  {
    if (index >= _hosts.size())
    {
      throw lrvx::LrvxError("E_VAL_002",
                            "replace_strategy: index out of range");
    }
    _hosts[index]->replace_strategy(new_strat);
  }

  void start() { lrvx_runner_start(_runner); }
  void stop() { lrvx_runner_stop(_runner); }

  LrvxRunnerHandle handle() const noexcept { return _runner; }

  void set_pnl_tracker(std::shared_ptr<lrvx::PyPnLTracker> tracker)
  {
    _pnl_owner.reset();
    if (tracker)
    {
      _pnl_owner = std::make_unique<lrvx::PyPnLTrackerOwner>(std::move(tracker));
      lrvx_runner_set_pnl_tracker(_runner, _pnl_owner->handle());
    }
    else
    {
      lrvx_runner_set_pnl_tracker(_runner, nullptr);
    }
  }

  void set_storage_sink(std::shared_ptr<lrvx::PyStorageSink> sink)
  {
    _storage_owner.reset();
    if (sink)
    {
      _storage_owner = std::make_unique<lrvx::PyStorageSinkOwner>(std::move(sink));
      lrvx_runner_set_storage_sink(_runner, _storage_owner->handle());
    }
    else
    {
      lrvx_runner_set_storage_sink(_runner, nullptr);
    }
  }

  void set_risk_manager(std::shared_ptr<lrvx::PyRiskManager> rm)
  {
    _risk_owner.reset();
    if (rm)
    {
      _risk_owner = std::make_unique<lrvx::PyRiskManagerOwner>(std::move(rm));
      lrvx_runner_set_risk_manager(_runner, _risk_owner->handle());
    }
    else
    {
      lrvx_runner_set_risk_manager(_runner, nullptr);
    }
  }

  void set_kill_switch(std::shared_ptr<lrvx::PyKillSwitch> ks)
  {
    _kill_owner.reset();
    if (ks)
    {
      _kill_owner = std::make_unique<lrvx::PyKillSwitchOwner>(std::move(ks));
      lrvx_runner_set_kill_switch(_runner, _kill_owner->handle());
    }
    else
    {
      lrvx_runner_set_kill_switch(_runner, nullptr);
    }
  }

  void set_order_validator(std::shared_ptr<lrvx::PyOrderValidator> ov)
  {
    _validator_owner.reset();
    if (ov)
    {
      _validator_owner = std::make_unique<lrvx::PyOrderValidatorOwner>(std::move(ov));
      lrvx_runner_set_order_validator(_runner, _validator_owner->handle());
    }
    else
    {
      lrvx_runner_set_order_validator(_runner, nullptr);
    }
  }

  void set_market_data_recorder(std::shared_ptr<lrvx::PyMarketDataRecorderHook> rec)
  {
    _recorder_owner.reset();
    _binlog_recorder.reset();
    if (rec)
    {
      _recorder_owner = std::make_unique<lrvx::PyMarketDataRecorderHookOwner>(std::move(rec));
      lrvx_runner_set_market_data_recorder(_runner, _recorder_owner->handle());
    }
    else
    {
      lrvx_runner_set_market_data_recorder(_runner, nullptr);
    }
  }

  // Built-in `.lrvx` sink overload. Same single attach slot as the
  // user-callback flavour — setting one clears the other.
  void set_market_data_recorder(std::shared_ptr<lrvx::PyBinaryLogRecorderHook> rec)
  {
    _recorder_owner.reset();
    _binlog_recorder.reset();
    if (rec)
    {
      auto* raw = rec->raw();
      // Bridge C++ hook through the same callback handle the C-API uses
      // for user-callback recorders. The C-API `binary_log_recorder_hook_create`
      // path would yield a self-owned handle; here we already own the
      // C++ object, so wire the callbacks manually onto a transient handle.
      LrvxMarketDataRecorderCallbacks cb{};
      cb.on_trade = [](void* ud, const LrvxTradeData* t)
      {
        if (!t)
        {
          return;
        }
        static_cast<lrvx::replay::BinaryLogRecorderHook*>(ud)->onTrade(
            t->symbol, t->price_raw, t->quantity_raw,
            t->is_buy != 0, t->exchange_ts_ns, 0);
      };
      cb.on_book_update = [](void* ud, uint32_t symbol, uint8_t is_snap,
                             const LrvxBookLevel* bids, uint32_t n_bids,
                             const LrvxBookLevel* asks, uint32_t n_asks,
                             int64_t ts)
      {
        auto* hk = static_cast<lrvx::replay::BinaryLogRecorderHook*>(ud);
        hk->onBookUpdate(symbol, is_snap != 0,
                         reinterpret_cast<const lrvx::replay::BookLevel*>(bids), n_bids,
                         reinterpret_cast<const lrvx::replay::BookLevel*>(asks), n_asks,
                         ts, 0);
      };
      cb.on_start = [](void* ud)
      {
        static_cast<lrvx::replay::BinaryLogRecorderHook*>(ud)->start();
      };
      cb.on_stop = [](void* ud)
      {
        static_cast<lrvx::replay::BinaryLogRecorderHook*>(ud)->stop();
      };
      cb.user_data = raw;
      _binlog_recorder_handle = lrvx_market_data_recorder_create(cb);
      _binlog_recorder = std::move(rec);
      lrvx_runner_set_market_data_recorder(_runner, _binlog_recorder_handle);
    }
    else
    {
      if (_binlog_recorder_handle)
      {
        lrvx_runner_set_market_data_recorder(_runner, nullptr);
        lrvx_market_data_recorder_destroy(_binlog_recorder_handle);
        _binlog_recorder_handle = nullptr;
      }
    }
  }

  void set_executor(std::shared_ptr<lrvx::PyExecutor> exec)
  {
    _executor_owner.reset();
    if (exec)
    {
      _executor_owner = std::make_unique<lrvx::PyExecutorOwner>(std::move(exec));
      lrvx_runner_set_executor(_runner, _executor_owner->handle());
    }
    else
    {
      lrvx_runner_set_executor(_runner, nullptr);
    }
  }

  void on_trade(uint32_t symbol, double price, double qty,
                bool is_buy, int64_t ts_ns)
  {
    lrvx_runner_on_trade(_runner, symbol, price, qty,
                         static_cast<uint8_t>(is_buy), ts_ns);
  }

  void on_book_snapshot(uint32_t symbol,
                        const std::vector<double>& bid_prices,
                        const std::vector<double>& bid_qtys,
                        const std::vector<double>& ask_prices,
                        const std::vector<double>& ask_qtys,
                        int64_t ts_ns)
  {
    uint32_t nb = static_cast<uint32_t>(bid_prices.size());
    uint32_t na = static_cast<uint32_t>(ask_prices.size());
    lrvx_runner_on_book_snapshot(_runner, symbol,
                                 bid_prices.data(), bid_qtys.data(), nb,
                                 ask_prices.data(), ask_qtys.data(), na,
                                 ts_ns);
  }

  void on_bar(uint32_t symbol,
              double open, double high, double low, double close,
              double volume, double buy_volume,
              int64_t start_time_ns, int64_t end_time_ns,
              uint8_t bar_type = 0, uint64_t bar_type_param = 0,
              uint8_t close_reason = 0)
  {
    lrvx_runner_on_bar(_runner, symbol, bar_type, bar_type_param,
                       open, high, low, close, volume, buy_volume,
                       start_time_ns, end_time_ns, close_reason);
  }

 private:
  SymbolRegistry* _reg;
  LrvxRunnerHandle _runner{nullptr};
  py::object _on_signal;
  std::vector<std::unique_ptr<PyStrategyHost>> _hosts;

  // Hook owners — keep the Owner alive while the runner holds a non-owning
  // LrvxXxxHandle. Reset on detach (set_xxx(None)) or on destruction.
  std::unique_ptr<lrvx::PyPnLTrackerOwner> _pnl_owner;
  std::unique_ptr<lrvx::PyStorageSinkOwner> _storage_owner;
  std::unique_ptr<lrvx::PyRiskManagerOwner> _risk_owner;
  std::unique_ptr<lrvx::PyKillSwitchOwner> _kill_owner;
  std::unique_ptr<lrvx::PyOrderValidatorOwner> _validator_owner;
  std::unique_ptr<lrvx::PyMarketDataRecorderHookOwner> _recorder_owner;
  // Built-in `.lrvx` sink overload — keeps the Python hook alive
  // for the duration of the attach, plus the transient callback bundle
  // we hand to lrvx_market_data_recorder_create.
  std::shared_ptr<lrvx::PyBinaryLogRecorderHook> _binlog_recorder;
  LrvxMarketDataRecorderHandle _binlog_recorder_handle{nullptr};
  std::unique_ptr<lrvx::PyExecutorOwner> _executor_owner;

  static void signalCallback(void* ud, const LrvxSignal* sig)
  {
    // Called synchronously from Python thread — GIL already held.
    auto* self = static_cast<PyStrategyRunner*>(ud);
    self->_on_signal(pySignalFromC(sig));
  }
};

// ──────────────────────────────────────────────────────────────────────
// PyLiveEngine — Disruptor-based live engine.
//
// Each strategy runs in its own bus consumer thread. publish_trade /
// publish_book_snapshot are lock-free and return immediately.
// Strategy callbacks and on_signal fire from C++ consumer threads;
// the GIL is acquired automatically.
// ──────────────────────────────────────────────────────────────────────

class PyLiveEngine
{
 public:
  explicit PyLiveEngine(SymbolRegistry* reg, py::object on_signal)
      : _reg(reg), _on_signal(std::move(on_signal))
  {
    _engine = lrvx_live_engine_create(static_cast<LrvxRegistryHandle>(reg));
  }

  ~PyLiveEngine()
  {
    if (_binlog_recorder_handle)
    {
      lrvx_market_data_recorder_destroy(_binlog_recorder_handle);
      _binlog_recorder_handle = nullptr;
    }
    if (_engine)
    {
      lrvx_live_engine_destroy(_engine);
    }
  }

  void add_strategy(PyStrategyBase* strat)
  {
    uint32_t id = static_cast<uint32_t>(_hosts.size()) + 1;
    auto host = std::make_unique<PyStrategyHost>(strat, _reg, id, true);
    lrvx_live_engine_add_strategy(_engine,
                                  static_cast<LrvxStrategyHandle>(host->bridge.get()),
                                  &PyLiveEngine::signalCallback, this);
    _hosts.push_back(std::move(host));
  }

  void replace_strategy(uint32_t index, PyStrategyBase* new_strat)
  {
    if (index >= _hosts.size())
    {
      throw lrvx::LrvxError("E_VAL_002",
                            "replace_strategy: index out of range");
    }
    _hosts[index]->replace_strategy(new_strat);
  }

  void start() { lrvx_live_engine_start(_engine); }
  void stop()
  {
    py::gil_scoped_release release;
    lrvx_live_engine_stop(_engine);
  }

  // The three publish_* methods release the GIL for the duration of the
  // call, the way stop() does. Publishing gates on the consumer once the
  // ring is full (event_bus.h), and the consumer thread needs the GIL to
  // finish the Python callback that would drain it — holding the GIL
  // here wedges the two threads against each other. Nothing below
  // touches a Python object: the vectors were already converted.
  void publish_trade(uint32_t symbol, double price, double qty,
                     bool is_buy, int64_t ts_ns)
  {
    py::gil_scoped_release release;
    lrvx_live_engine_publish_trade(_engine, symbol, price, qty,
                                   static_cast<uint8_t>(is_buy), ts_ns);
  }

  void publish_book_snapshot(uint32_t symbol,
                             const std::vector<double>& bid_prices,
                             const std::vector<double>& bid_qtys,
                             const std::vector<double>& ask_prices,
                             const std::vector<double>& ask_qtys,
                             int64_t ts_ns)
  {
    uint32_t nb = static_cast<uint32_t>(bid_prices.size());
    uint32_t na = static_cast<uint32_t>(ask_prices.size());
    py::gil_scoped_release release;
    lrvx_live_engine_publish_book_snapshot(_engine, symbol,
                                           bid_prices.data(), bid_qtys.data(), nb,
                                           ask_prices.data(), ask_qtys.data(), na,
                                           ts_ns);
  }

  void publish_bar(uint32_t symbol, uint8_t bar_type, uint64_t bar_type_param,
                   double open, double high, double low, double close,
                   double volume, double buy_volume,
                   int64_t start_time_ns, int64_t end_time_ns,
                   uint8_t close_reason)
  {
    py::gil_scoped_release release;
    lrvx_live_engine_publish_bar(_engine, symbol, bar_type, bar_type_param,
                                 open, high, low, close, volume, buy_volume,
                                 start_time_ns, end_time_ns, close_reason);
  }

  void set_pnl_tracker(std::shared_ptr<lrvx::PyPnLTracker> tracker)
  {
    _pnl_owner.reset();
    if (tracker)
    {
      _pnl_owner = std::make_unique<lrvx::PyPnLTrackerOwner>(std::move(tracker));
      lrvx_live_engine_set_pnl_tracker(_engine, _pnl_owner->handle());
    }
    else
    {
      lrvx_live_engine_set_pnl_tracker(_engine, nullptr);
    }
  }

  void set_storage_sink(std::shared_ptr<lrvx::PyStorageSink> sink)
  {
    _storage_owner.reset();
    if (sink)
    {
      _storage_owner = std::make_unique<lrvx::PyStorageSinkOwner>(std::move(sink));
      lrvx_live_engine_set_storage_sink(_engine, _storage_owner->handle());
    }
    else
    {
      lrvx_live_engine_set_storage_sink(_engine, nullptr);
    }
  }

  void set_risk_manager(std::shared_ptr<lrvx::PyRiskManager> rm)
  {
    _risk_owner.reset();
    if (rm)
    {
      _risk_owner = std::make_unique<lrvx::PyRiskManagerOwner>(std::move(rm));
      lrvx_live_engine_set_risk_manager(_engine, _risk_owner->handle());
    }
    else
    {
      lrvx_live_engine_set_risk_manager(_engine, nullptr);
    }
  }

  void set_kill_switch(std::shared_ptr<lrvx::PyKillSwitch> ks)
  {
    _kill_owner.reset();
    if (ks)
    {
      _kill_owner = std::make_unique<lrvx::PyKillSwitchOwner>(std::move(ks));
      lrvx_live_engine_set_kill_switch(_engine, _kill_owner->handle());
    }
    else
    {
      lrvx_live_engine_set_kill_switch(_engine, nullptr);
    }
  }

  void set_order_validator(std::shared_ptr<lrvx::PyOrderValidator> ov)
  {
    _validator_owner.reset();
    if (ov)
    {
      _validator_owner = std::make_unique<lrvx::PyOrderValidatorOwner>(std::move(ov));
      lrvx_live_engine_set_order_validator(_engine, _validator_owner->handle());
    }
    else
    {
      lrvx_live_engine_set_order_validator(_engine, nullptr);
    }
  }

  void set_market_data_recorder(std::shared_ptr<lrvx::PyMarketDataRecorderHook> rec)
  {
    _recorder_owner.reset();
    _binlog_recorder.reset();
    if (rec)
    {
      _recorder_owner = std::make_unique<lrvx::PyMarketDataRecorderHookOwner>(std::move(rec));
      lrvx_live_engine_set_market_data_recorder(_engine, _recorder_owner->handle());
    }
    else
    {
      lrvx_live_engine_set_market_data_recorder(_engine, nullptr);
    }
  }

  void set_market_data_recorder(std::shared_ptr<lrvx::PyBinaryLogRecorderHook> rec)
  {
    _recorder_owner.reset();
    _binlog_recorder.reset();
    if (_binlog_recorder_handle)
    {
      lrvx_live_engine_set_market_data_recorder(_engine, nullptr);
      lrvx_market_data_recorder_destroy(_binlog_recorder_handle);
      _binlog_recorder_handle = nullptr;
    }
    if (rec)
    {
      auto* raw = rec->raw();
      LrvxMarketDataRecorderCallbacks cb{};
      cb.on_trade = [](void* ud, const LrvxTradeData* t)
      {
        if (!t)
        {
          return;
        }
        static_cast<lrvx::replay::BinaryLogRecorderHook*>(ud)->onTrade(
            t->symbol, t->price_raw, t->quantity_raw,
            t->is_buy != 0, t->exchange_ts_ns, 0);
      };
      cb.on_book_update = [](void* ud, uint32_t symbol, uint8_t is_snap,
                             const LrvxBookLevel* bids, uint32_t n_bids,
                             const LrvxBookLevel* asks, uint32_t n_asks,
                             int64_t ts)
      {
        auto* hk = static_cast<lrvx::replay::BinaryLogRecorderHook*>(ud);
        hk->onBookUpdate(symbol, is_snap != 0,
                         reinterpret_cast<const lrvx::replay::BookLevel*>(bids), n_bids,
                         reinterpret_cast<const lrvx::replay::BookLevel*>(asks), n_asks,
                         ts, 0);
      };
      cb.on_start = [](void* ud)
      {
        static_cast<lrvx::replay::BinaryLogRecorderHook*>(ud)->start();
      };
      cb.on_stop = [](void* ud)
      {
        static_cast<lrvx::replay::BinaryLogRecorderHook*>(ud)->stop();
      };
      cb.user_data = raw;
      _binlog_recorder_handle = lrvx_market_data_recorder_create(cb);
      _binlog_recorder = std::move(rec);
      lrvx_live_engine_set_market_data_recorder(_engine, _binlog_recorder_handle);
    }
  }

  void set_executor(std::shared_ptr<lrvx::PyExecutor> exec)
  {
    _executor_owner.reset();
    if (exec)
    {
      _executor_owner = std::make_unique<lrvx::PyExecutorOwner>(std::move(exec));
      lrvx_live_engine_set_executor(_engine, _executor_owner->handle());
    }
    else
    {
      lrvx_live_engine_set_executor(_engine, nullptr);
    }
  }

 private:
  SymbolRegistry* _reg;
  LrvxLiveEngineHandle _engine{nullptr};
  py::object _on_signal;
  std::vector<std::unique_ptr<PyStrategyHost>> _hosts;
  std::unique_ptr<lrvx::PyPnLTrackerOwner> _pnl_owner;
  std::unique_ptr<lrvx::PyStorageSinkOwner> _storage_owner;
  std::unique_ptr<lrvx::PyRiskManagerOwner> _risk_owner;
  std::unique_ptr<lrvx::PyKillSwitchOwner> _kill_owner;
  std::unique_ptr<lrvx::PyOrderValidatorOwner> _validator_owner;
  std::unique_ptr<lrvx::PyMarketDataRecorderHookOwner> _recorder_owner;
  std::shared_ptr<lrvx::PyBinaryLogRecorderHook> _binlog_recorder;
  LrvxMarketDataRecorderHandle _binlog_recorder_handle{nullptr};
  std::unique_ptr<lrvx::PyExecutorOwner> _executor_owner;

  static void signalCallback(void* ud, const LrvxSignal* sig)
  {
    // Called from C++ consumer thread — GIL must be acquired.
    auto* self = static_cast<PyLiveEngine*>(ud);
    py::gil_scoped_acquire gil;
    self->_on_signal(pySignalFromC(sig));
  }
};

// ──────────────────────────────────────────────────────────────────────
// PyRunner — unified live runner.
//
//   Runner(registry, on_signal)                — sync (StrategyRunner)
//   Runner(registry, on_signal, threaded=True) — Disruptor (LiveEngine)
//
// The same Strategy class and the same on_trade() / on_book_snapshot()
// calls work in both modes. threaded=True uses lock-free publish and
// runs strategy callbacks in a background C++ thread.
// ──────────────────────────────────────────────────────────────────────

class PyRunner
{
 public:
  PyRunner(SymbolRegistry* reg, py::object on_signal, bool threaded)
  {
    if (threaded)
    {
      _live = std::make_unique<PyLiveEngine>(reg, std::move(on_signal));
    }
    else
    {
      _sync = std::make_unique<PyStrategyRunner>(reg, std::move(on_signal));
    }
  }

  void add_strategy(PyStrategyBase* strat)
  {
    if (_live)
    {
      _live->add_strategy(strat);
    }
    else
    {
      _sync->add_strategy(strat);
    }
  }

  void replace_strategy(uint32_t index, PyStrategyBase* new_strat)
  {
    if (_live)
    {
      _live->replace_strategy(index, new_strat);
    }
    else
    {
      _sync->replace_strategy(index, new_strat);
    }
  }

  void start()
  {
    if (_live)
    {
      _live->start();
    }
    else
    {
      _sync->start();
    }
  }

  void stop()
  {
    if (_live)
    {
      _live->stop();
    }
    else
    {
      _sync->stop();
    }
  }

#define _PYRUNNER_SET_HOOK(NAME, TYPE)              \
  void set_##NAME(std::shared_ptr<lrvx::TYPE> hook) \
  {                                                 \
    if (_live)                                      \
    {                                               \
      _live->set_##NAME(std::move(hook));           \
    }                                               \
    else                                            \
    {                                               \
      _sync->set_##NAME(std::move(hook));           \
    }                                               \
  }
  _PYRUNNER_SET_HOOK(pnl_tracker, PyPnLTracker)
  _PYRUNNER_SET_HOOK(storage_sink, PyStorageSink)
  _PYRUNNER_SET_HOOK(risk_manager, PyRiskManager)
  _PYRUNNER_SET_HOOK(kill_switch, PyKillSwitch)
  _PYRUNNER_SET_HOOK(order_validator, PyOrderValidator)
  _PYRUNNER_SET_HOOK(market_data_recorder, PyMarketDataRecorderHook)
  _PYRUNNER_SET_HOOK(executor, PyExecutor)
#undef _PYRUNNER_SET_HOOK

  // Built-in `.lrvx` recorder overload — picks up the same attach
  // slot as the user-callback flavour on the underlying runner / engine.
  void set_market_data_recorder(std::shared_ptr<lrvx::PyBinaryLogRecorderHook> hook)
  {
    if (_live)
    {
      _live->set_market_data_recorder(std::move(hook));
    }
    else
    {
      _sync->set_market_data_recorder(std::move(hook));
    }
  }

  void on_trade(uint32_t symbol, double price, double qty,
                bool is_buy, int64_t ts_ns)
  {
    if (_live)
    {
      _live->publish_trade(symbol, price, qty, is_buy, ts_ns);
    }
    else
    {
      _sync->on_trade(symbol, price, qty, is_buy, ts_ns);
    }
  }

  void on_book_snapshot(uint32_t symbol,
                        const std::vector<double>& bid_prices,
                        const std::vector<double>& bid_qtys,
                        const std::vector<double>& ask_prices,
                        const std::vector<double>& ask_qtys,
                        int64_t ts_ns)
  {
    if (_live)
    {
      _live->publish_book_snapshot(symbol, bid_prices, bid_qtys,
                                   ask_prices, ask_qtys, ts_ns);
    }
    else
    {
      _sync->on_book_snapshot(symbol, bid_prices, bid_qtys,
                              ask_prices, ask_qtys, ts_ns);
    }
  }

  void on_bar(uint32_t symbol,
              double open, double high, double low, double close,
              double volume, double buy_volume,
              int64_t start_time_ns, int64_t end_time_ns,
              uint8_t bar_type = 0, uint64_t bar_type_param = 0,
              uint8_t close_reason = 0)
  {
    if (_live)
    {
      _live->publish_bar(symbol, bar_type, bar_type_param,
                         open, high, low, close, volume, buy_volume,
                         start_time_ns, end_time_ns, close_reason);
    }
    else
    {
      _sync->on_bar(symbol, open, high, low, close, volume, buy_volume,
                    start_time_ns, end_time_ns, bar_type, bar_type_param,
                    close_reason);
    }
  }

 public:
  LrvxRunnerHandle _runner_handle() const noexcept
  {
    return _sync ? _sync->handle() : nullptr;
  }

 private:
  std::unique_ptr<PyStrategyRunner> _sync;
  std::unique_ptr<PyLiveEngine> _live;
};

// ──────────────────────────────────────────────────────────────────────
// OhlcvBacktestReader — feeds OHLCV bars into BacktestRunner as trades.
// ──────────────────────────────────────────────────────────────────────

class OhlcvBacktestReader : public replay::IMultiSegmentReader
{
 public:
  struct Bar
  {
    int64_t ts_ns;
    int64_t price_raw;
    uint32_t symbol_id;
  };

  explicit OhlcvBacktestReader(std::vector<Bar> bars) : _bars(std::move(bars)) {}

  uint64_t forEach(EventCallback cb) override
  {
    uint64_t n = 0;
    for (const auto& bar : _bars)
    {
      if (!cb(makeEvent(bar)))
      {
        break;
      }
      ++n;
    }
    return n;
  }

  uint64_t forEachFrom(int64_t start_ns, EventCallback cb) override
  {
    uint64_t n = 0;
    for (const auto& bar : _bars)
    {
      if (bar.ts_ns < start_ns)
      {
        continue;
      }
      if (!cb(makeEvent(bar)))
      {
        break;
      }
      ++n;
    }
    return n;
  }

  const std::vector<replay::SegmentInfo>& segments() const override { return _segs; }
  uint64_t totalEvents() const override { return _bars.size(); }

 private:
  static replay::ReplayEvent makeEvent(const Bar& bar)
  {
    replay::ReplayEvent ev{};
    ev.type = replay::EventType::Trade;
    ev.timestamp_ns = bar.ts_ns;
    ev.trade.exchange_ts_ns = bar.ts_ns;
    ev.trade.price_raw = bar.price_raw;
    ev.trade.qty_raw = Quantity::fromDouble(1.0).raw();  // synthetic qty
    ev.trade.symbol_id = bar.symbol_id;
    ev.trade.side = 0;  // buy, in the tape's encoding
    return ev;
  }

  std::vector<Bar> _bars;
  std::vector<replay::SegmentInfo> _segs;
};

// ──────────────────────────────────────────────────────────────────────
// PyBacktestRunner — same Strategy class as StrategyRunner / LiveEngine,
// but replays historical OHLCV data through it and returns stats.
// ──────────────────────────────────────────────────────────────────────

class PyBacktestRunner
{
 public:
  PyBacktestRunner(SymbolRegistry* reg, double fee_rate, double initial_capital)
      : _reg(reg)
  {
    BacktestConfig cfg{};
    cfg.feeRate = fee_rate;
    cfg.initialCapital = initial_capital;
    cfg.usePercentageFee = true;
    _runner = std::make_unique<BacktestRunner>(cfg);
  }

  void set_strategy(PyStrategyBase* strat)
  {
    uint32_t id = 1;
    // with_gil = false: backtest runs on caller's thread, GIL already held
    _host = std::make_unique<PyStrategyHost>(strat, _reg, id, false);
    // BacktestRunner sets itself as ISignalHandler on the bridge —
    // emitted orders go straight to SimulatedExecutor.
    _runner->setStrategy(_host->bridge.get());
  }

  void set_executor(std::shared_ptr<lrvx::PyExecutor> exec)
  {
    if (exec)
    {
      _executor_adapter =
          std::make_unique<lrvx::cxx_adapters::PyExecutorCxxAdapter>(std::move(exec));
      _runner->setExecutor(_executor_adapter.get());
    }
    else
    {
      _runner->setExecutor(nullptr);
      _executor_adapter.reset();
    }
  }

  // A VenueStack executor cannot drive a BacktestRunner today, and
  // silently accepting it is worse than refusing: setExecutor() routes
  // signals to the custom executor but keeps feeding market data only to
  // the built-in simulator, and the contract puts fill reporting on the
  // custom executor. The run completes with zero trades and a flat
  // equity curve, which reads like a broken strategy. Refuse loudly and
  // name the supported pattern instead.
  // Run against a venue stack: the runner feeds the stack's executor
  // market data and harvests its fills, so the run uses that venue's
  // FILL MECHANICS -- queue model and depth, iceberg refresh latency,
  // venue availability and rate limits -- and that venue's FEE LADDER:
  // the stack's FeeSchedule prices every fill at the tier its 30-day
  // notional resolves to, with Fill::isMaker picking the side. Funding
  // and liquidation are still driven by explicit calls, not the replay
  // loop.
  // Takes the whole stack because the executor alone does not expose the
  // clock, and without advancing it ack latency and queue timing would
  // never progress. Pass None to revert to the built-in executor.
  void set_venue_stack(lrvx::VenueStack* stack)
  {
    if (stack == nullptr)
    {
      _runner->setSimulatedExecutor(nullptr, nullptr);
      return;
    }
    _runner->setSimulatedExecutor(&stack->executor(), &stack->clock());
  }

  // A bare VenueExecutor cannot be wired correctly: SimulatedExecutor
  // does not expose the clock it was built with, so the runner could not
  // advance it. Point the caller at set_venue_stack instead of accepting
  // a half-wired run.
  [[noreturn]] void set_venue_executor(lrvx::SimulatedExecutor*)
  {
    throw std::invalid_argument(
        "BacktestRunner.set_executor() does not take a VenueStack executor "
        "(lrvx.VenueExecutor) — the executor alone does not carry the clock "
        "the runner has to advance. Pass the stack itself: "
        "runner.set_venue_stack(stack). See docs/how-to/realistic-backtest.md.");
  }

  void add_execution_listener(std::shared_ptr<lrvx::PyExecutionListener> listener)
  {
    if (!listener)
    {
      return;
    }
    auto id = static_cast<lrvx::SubscriberId>(_listener_adapters.size() + 1);
    auto adapter = std::make_unique<lrvx::cxx_adapters::PyExecutionListenerCxxAdapter>(
        id, std::move(listener));
    _runner->addExecutionListener(adapter.get());
    _listener_adapters.push_back(std::move(adapter));
  }

  // Pre-trade gate parity with the live Runner. None on entry; pass an
  // instance of PyRiskManager / PyKillSwitch / PyOrderValidator /
  // PyPnLTracker to attach, None to detach. Reduce-only orders bypass
  // the gate by design (see lookup_symbol gotcha) so tightening caps
  // does not strand a strategy in a position.
  void set_risk_manager(std::shared_ptr<lrvx::PyRiskManager> rm)
  {
    if (rm)
    {
      _risk_adapter = std::make_unique<lrvx::cxx_adapters::PyRiskManagerCxxAdapter>(
          std::move(rm));
      _runner->setRiskManager(_risk_adapter.get());
    }
    else
    {
      _runner->setRiskManager(nullptr);
      _risk_adapter.reset();
    }
  }

  void set_kill_switch(std::shared_ptr<lrvx::PyKillSwitch> ks)
  {
    if (ks)
    {
      _kill_adapter = std::make_unique<lrvx::cxx_adapters::PyKillSwitchCxxAdapter>(
          std::move(ks));
      _runner->setKillSwitch(_kill_adapter.get());
    }
    else
    {
      _runner->setKillSwitch(nullptr);
      _kill_adapter.reset();
    }
  }

  void set_order_validator(std::shared_ptr<lrvx::PyOrderValidator> ov)
  {
    if (ov)
    {
      _validator_adapter =
          std::make_unique<lrvx::cxx_adapters::PyOrderValidatorCxxAdapter>(
              std::move(ov));
      _runner->setOrderValidator(_validator_adapter.get());
    }
    else
    {
      _runner->setOrderValidator(nullptr);
      _validator_adapter.reset();
    }
  }

  void set_pnl_tracker(std::shared_ptr<lrvx::PyPnLTracker> tracker)
  {
    if (tracker)
    {
      _pnl_adapter = std::make_unique<lrvx::cxx_adapters::PyPnLTrackerCxxAdapter>(
          std::move(tracker));
      _runner->setPnLTracker(_pnl_adapter.get());
    }
    else
    {
      _runner->setPnLTracker(nullptr);
      _pnl_adapter.reset();
    }
  }

  py::object run_csv(const std::string& path, const std::string& symbol = "")
  {
    std::string sym = symbol.empty() ? inferSymbol(path) : symbol;
    auto id = resolveSymbol(sym);
    auto bars = loadCsv(path, id);
    return runBars(std::move(bars));
  }

  py::object run_tape(const std::string& path)
  {
    if (!_host)
    {
      throw lrvx::LrvxError(
          "E_RUN_001",
          "BacktestRunner.run_tape() called before set_strategy(). "
          "Attach a strategy with set_strategy(MyStrategy()) first.");
    }
    BacktestResult result = _runner->runTape(path);
    return statsToDict(std::move(result));
  }

  py::object run_tapes(const std::vector<std::string>& paths)
  {
    if (!_host)
    {
      throw lrvx::LrvxError(
          "E_RUN_001",
          "BacktestRunner.run_tapes() called before set_strategy(). "
          "Attach a strategy with set_strategy(MyStrategy()) first.");
    }
    std::vector<std::filesystem::path> ps;
    ps.reserve(paths.size());
    for (const auto& p : paths)
    {
      ps.emplace_back(p);
    }
    BacktestResult result = _runner->runTapes(ps);
    return statsToDict(std::move(result));
  }

 private:
  py::object statsToDict(BacktestResult result)
  {
    BacktestStats stats = result.computeStats();
    py::dict d;
    d["total_trades"] = stats.totalTrades;
    d["winning_trades"] = stats.winningTrades;
    d["losing_trades"] = stats.losingTrades;
    d["initial_capital"] = stats.initialCapital;
    d["final_capital"] = stats.finalCapital;
    d["total_pnl"] = stats.totalPnl;
    d["total_fees"] = stats.totalFees;
    d["net_pnl"] = stats.netPnl;
    d["gross_profit"] = stats.grossProfit;
    d["gross_loss"] = stats.grossLoss;
    d["max_drawdown"] = stats.maxDrawdown;
    d["max_drawdown_pct"] = stats.maxDrawdownPct;
    d["win_rate"] = stats.winRate;
    d["profit_factor"] = stats.profitFactor;
    d["sharpe_ratio"] = stats.sharpeRatio;
    d["sortino_ratio"] = stats.sortinoRatio;
    d["calmar_ratio"] = stats.calmarRatio;
    d["return_pct"] = stats.returnPct;
    _lastResult = std::move(result);
    return d;
  }

 public:
  // ts is nanoseconds by contract, not a magnitude-guessed unit: the same
  // contract as lrvx_backtest_runner_run_ohlcv's timestamps_ns parameter
  // (src/capi/lrvx_capi.cpp), Node's runOhlcv (BigInt64Array passed straight
  // through to that C ABI call), and Codon's run_ohlcv ("timestamps in
  // nanoseconds" per its docstring, same passthrough). This binding used to
  // be the odd one out, running every value through normalizeTs()'s
  // seconds/ms/us/ns magnitude guess -- see run_bars's identical fix a few
  // lines below for the exact hazard (a legitimate sub-1e12ns value read as
  // seconds and rescaled by another 1e9, overflowing int64_t).
  py::object run_ohlcv(py::array_t<int64_t, py::array::c_style | py::array::forcecast> ts,
                       py::array_t<double, py::array::c_style | py::array::forcecast> close,
                       const std::string& symbol = "")
  {
    std::string sym = symbol.empty() ? "default" : symbol;
    auto id = resolveSymbol(sym);
    auto n = ts.size();
    std::vector<OhlcvBacktestReader::Bar> bars;
    bars.reserve(n);
    auto pts = ts.unchecked<1>();
    auto pc = close.unchecked<1>();
    for (py::ssize_t i = 0; i < n; ++i)
    {
      bars.push_back({pts(i), Price::fromDouble(pc(i)).raw(), id});
    }
    return runBars(std::move(bars));
  }

  // Replay full OHLCV bars. Strategy.on_bar fires; on_trade does NOT.
  // bar_type: 0=Time (default), 1=Tick, 2=Volume, 3=Renko, 4=Range, 5=HeikinAshi.
  py::object run_bars(
      py::array_t<int64_t, py::array::c_style | py::array::forcecast> start_ns,
      py::array_t<int64_t, py::array::c_style | py::array::forcecast> end_ns,
      py::array_t<double, py::array::c_style | py::array::forcecast> open,
      py::array_t<double, py::array::c_style | py::array::forcecast> high,
      py::array_t<double, py::array::c_style | py::array::forcecast> low,
      py::array_t<double, py::array::c_style | py::array::forcecast> close,
      py::array_t<double, py::array::c_style | py::array::forcecast> volume,
      const std::string& symbol = "",
      uint8_t bar_type = 0,
      uint64_t bar_type_param = 0)
  {
    if (!_host)
    {
      throw lrvx::LrvxError(
          "E_RUN_001",
          "BacktestRunner.run_bars() called before set_strategy(). "
          "Attach a strategy with set_strategy(MyStrategy()) first.");
    }
    std::string sym = symbol.empty() ? "default" : symbol;
    auto id = resolveSymbol(sym);
    const auto n = start_ns.size();
    if (end_ns.size() != n || open.size() != n || high.size() != n || low.size() != n || close.size() != n || volume.size() != n)
    {
      throw lrvx::LrvxError(
          "E_LEN_001",
          "run_bars: all arrays (start_time_ns, end_time_ns, open, high, "
          "low, close, volume) must have the same length.");
    }
    auto ps = start_ns.unchecked<1>();
    auto pe = end_ns.unchecked<1>();
    auto po = open.unchecked<1>();
    auto ph = high.unchecked<1>();
    auto pl = low.unchecked<1>();
    auto pc = close.unchecked<1>();
    auto pv = volume.unchecked<1>();
    std::vector<BarEvent> events;
    events.reserve(n);
    for (py::ssize_t i = 0; i < n; ++i)
    {
      BarEvent ev{};
      ev.symbol = id;
      ev.barType = static_cast<BarType>(bar_type);
      ev.barTypeParam = bar_type_param;
      ev.bar.open = Price::fromDouble(po(i));
      ev.bar.high = Price::fromDouble(ph(i));
      ev.bar.low = Price::fromDouble(pl(i));
      ev.bar.close = Price::fromDouble(pc(i));
      ev.bar.volume = Volume::fromDouble(pv(i));
      // start_time_ns / end_time_ns are already nanoseconds by contract
      // (the Python-facing kwarg names say so, and the docs example builds
      // them from a prior run's own nanosecond output). Passing them through
      // normalizeTs() re-guessed the unit from magnitude and silently
      // rescaled legitimate small values -- e.g. bar 2 of a 1-minute series
      // starts at 60_000_000_000ns, which normalizeTs mistook for seconds
      // and multiplied by 1e9 again, overflowing int64. Use them as-is.
      ev.bar.startTime = TimePoint{std::chrono::nanoseconds{ps(i)}};
      ev.bar.endTime = TimePoint{std::chrono::nanoseconds{pe(i)}};
      ev.bar.reason = BarCloseReason::Threshold;
      events.push_back(ev);
    }
    BacktestResult result = _runner->runBars(events);
    BacktestStats stats = result.computeStats();
    py::dict d;
    d["total_trades"] = stats.totalTrades;
    d["winning_trades"] = stats.winningTrades;
    d["losing_trades"] = stats.losingTrades;
    d["initial_capital"] = stats.initialCapital;
    d["final_capital"] = stats.finalCapital;
    d["total_pnl"] = stats.totalPnl;
    d["total_fees"] = stats.totalFees;
    d["net_pnl"] = stats.netPnl;
    d["gross_profit"] = stats.grossProfit;
    d["gross_loss"] = stats.grossLoss;
    d["max_drawdown"] = stats.maxDrawdown;
    d["max_drawdown_pct"] = stats.maxDrawdownPct;
    d["win_rate"] = stats.winRate;
    d["profit_factor"] = stats.profitFactor;
    d["sharpe_ratio"] = stats.sharpeRatio;
    d["sortino_ratio"] = stats.sortinoRatio;
    d["calmar_ratio"] = stats.calmarRatio;
    d["return_pct"] = stats.returnPct;
    _lastResult = std::move(result);
    return d;
  }

 public:
  // Most recent BacktestResult, kept after each run_* call so equity
  // curve / trades stay accessible without re-running. Replaced on
  // every run; cleared if no run has happened.
  py::object equity_curve()
  {
    if (!_lastResult.has_value())
    {
      throw lrvx::LrvxError(
          "E_RUN_002",
          "BacktestRunner.equity_curve() called before any run_* completed.");
    }
    const auto& curve = _lastResult->equityCurve();
    py::array_t<int64_t> ts(curve.size());
    py::array_t<double> eq(curve.size());
    py::array_t<double> dd(curve.size());
    auto pts = ts.mutable_unchecked<1>();
    auto peq = eq.mutable_unchecked<1>();
    auto pdd = dd.mutable_unchecked<1>();
    for (size_t i = 0; i < curve.size(); ++i)
    {
      pts(i) = curve[i].timestampNs.raw();
      peq(i) = curve[i].equity;
      pdd(i) = curve[i].drawdownPct;
    }
    py::dict d;
    d["timestamp_ns"] = ts;
    d["equity"] = eq;
    d["drawdown_pct"] = dd;
    return d;
  }

  py::object trades()
  {
    if (!_lastResult.has_value())
    {
      throw lrvx::LrvxError(
          "E_RUN_002",
          "BacktestRunner.trades() called before any run_* completed.");
    }
    const auto& tr = _lastResult->trades();
    const size_t n = tr.size();
    py::array_t<uint32_t> sym(n);
    py::array_t<uint8_t> side(n);
    py::array_t<double> ep(n), xp(n), qty(n), pnl(n), fee(n);
    py::array_t<int64_t> ets(n), xts(n);
    auto a_sym = sym.mutable_unchecked<1>();
    auto a_side = side.mutable_unchecked<1>();
    auto a_ep = ep.mutable_unchecked<1>();
    auto a_xp = xp.mutable_unchecked<1>();
    auto a_qty = qty.mutable_unchecked<1>();
    auto a_pnl = pnl.mutable_unchecked<1>();
    auto a_fee = fee.mutable_unchecked<1>();
    auto a_ets = ets.mutable_unchecked<1>();
    auto a_xts = xts.mutable_unchecked<1>();
    for (size_t i = 0; i < n; ++i)
    {
      a_sym(i) = static_cast<uint32_t>(tr[i].symbol);
      a_side(i) = static_cast<uint8_t>(tr[i].side);
      a_ep(i) = tr[i].entryPrice.toDouble();
      a_xp(i) = tr[i].exitPrice.toDouble();
      a_qty(i) = tr[i].quantity.toDouble();
      a_pnl(i) = tr[i].pnl.toDouble();
      a_fee(i) = tr[i].fee.toDouble();
      a_ets(i) = tr[i].entryTimeNs.raw();
      a_xts(i) = tr[i].exitTimeNs.raw();
    }
    py::dict d;
    d["symbol"] = sym;
    d["side"] = side;
    d["entry_price"] = ep;
    d["exit_price"] = xp;
    d["quantity"] = qty;
    d["pnl"] = pnl;
    d["fee"] = fee;
    d["entry_time_ns"] = ets;
    d["exit_time_ns"] = xts;
    return d;
  }

 private:
  SymbolRegistry* _reg;
  std::unique_ptr<BacktestRunner> _runner;
  std::unique_ptr<PyStrategyHost> _host;
  std::optional<BacktestResult> _lastResult;
  // C++ adapters bridging Python hooks → lrvx::IOrderExecutor /
  // IOrderExecutionListener used by BacktestRunner directly (without
  // going through the C ABI).
  std::unique_ptr<lrvx::cxx_adapters::PyExecutorCxxAdapter> _executor_adapter;
  std::vector<std::unique_ptr<lrvx::cxx_adapters::PyExecutionListenerCxxAdapter>>
      _listener_adapters;
  // Pre-trade gate adapters. Owned so they outlive the
  // runner's non-owning IRiskManager / IKillSwitch / IOrderValidator
  // / IPnLTracker pointers. Replaced on every set_* call.
  std::unique_ptr<lrvx::cxx_adapters::PyRiskManagerCxxAdapter> _risk_adapter;
  std::unique_ptr<lrvx::cxx_adapters::PyKillSwitchCxxAdapter> _kill_adapter;
  std::unique_ptr<lrvx::cxx_adapters::PyOrderValidatorCxxAdapter> _validator_adapter;
  std::unique_ptr<lrvx::cxx_adapters::PyPnLTrackerCxxAdapter> _pnl_adapter;

  uint32_t resolveSymbol(const std::string& sym)
  {
    auto opt = _reg->getSymbolId("", sym);
    if (!opt)
    {
      // try any exchange
      auto all = _reg->getAllSymbols();
      for (const auto& info : all)
      {
        if (info.symbol == sym)
        {
          return info.id;
        }
      }
      throw lrvx::LrvxError(
          "E_SYM_001",
          "Symbol '" + sym +
              "' is not registered. Register it via "
              "BacktestRunner.set_strategy(...) or by calling "
              "Engine.add_symbol() before running.");
    }
    return *opt;
  }

  py::object runBars(std::vector<OhlcvBacktestReader::Bar> bars)
  {
    if (!_host)
    {
      throw lrvx::LrvxError(
          "E_RUN_001",
          "BacktestRunner.run_*() called before set_strategy(). "
          "Attach a strategy with set_strategy(MyStrategy()) first.");
    }
    OhlcvBacktestReader reader(std::move(bars));
    BacktestResult result = _runner->run(reader);
    BacktestStats stats = result.computeStats();
    // Return a dict — same keys as PyStats.to_dict()
    py::dict d;
    d["total_trades"] = stats.totalTrades;
    d["winning_trades"] = stats.winningTrades;
    d["losing_trades"] = stats.losingTrades;
    d["initial_capital"] = stats.initialCapital;
    d["final_capital"] = stats.finalCapital;
    d["total_pnl"] = stats.totalPnl;
    d["total_fees"] = stats.totalFees;
    d["net_pnl"] = stats.netPnl;
    d["gross_profit"] = stats.grossProfit;
    d["gross_loss"] = stats.grossLoss;
    d["max_drawdown"] = stats.maxDrawdown;
    d["max_drawdown_pct"] = stats.maxDrawdownPct;
    d["win_rate"] = stats.winRate;
    d["profit_factor"] = stats.profitFactor;
    d["sharpe_ratio"] = stats.sharpeRatio;
    d["sortino_ratio"] = stats.sortinoRatio;
    d["calmar_ratio"] = stats.calmarRatio;
    d["return_pct"] = stats.returnPct;
    _lastResult = std::move(result);
    return d;
  }

  static std::vector<OhlcvBacktestReader::Bar> loadCsv(const std::string& path, uint32_t id)
  {
    std::ifstream f(path);
    if (!f.is_open())
    {
      throw lrvx::LrvxError(
          "E_IO_001",
          "Cannot open file: '" + path +
              "'. Check the path is correct and the file is readable.");
    }
    std::vector<OhlcvBacktestReader::Bar> bars;
    std::string line;
    std::getline(f, line);  // skip header
    while (std::getline(f, line))
    {
      if (line.empty())
      {
        continue;
      }
      std::istringstream ss(line);
      std::string tok;
      std::getline(ss, tok, ',');
      int64_t ts = normalizeTs(std::stoll(tok));
      std::getline(ss, tok, ',');  // open
      std::getline(ss, tok, ',');  // high
      std::getline(ss, tok, ',');  // low
      std::getline(ss, tok, ',');
      double c = std::stod(tok);
      bars.push_back({ts, Price::fromDouble(c).raw(), id});
    }
    return bars;
  }

  // Best-effort unit guess for genuinely unit-less input: a raw CSV column
  // (loadCsv, run_csv) that the caller may have populated with seconds,
  // milliseconds, microseconds, or nanoseconds -- lrvx has no way to know,
  // and docs/bindings/python.md documents this exact threshold behavior for
  // run_csv. Do NOT call this on a value whose unit is already part of the
  // contract (run_bars's start_time_ns / end_time_ns, or run_ohlcv's ts --
  // both nanoseconds by contract, matching the C ABI, Node, and Codon) --
  // that was the bug fixed above (run_bars) and in run_ohlcv itself: guessing
  // a unit for data that already has one silently corrupts legitimate small
  // values. Do not retune these thresholds either; second/millisecond/
  // microsecond/nanosecond ranges overlap on real dates, so any cutoff is a
  // bet on which timestamps show up.
  static int64_t normalizeTs(int64_t t)
  {
    if (t < static_cast<int64_t>(1e12))
    {
      return t * 1'000'000'000LL;
    }
    if (t < static_cast<int64_t>(1e15))
    {
      return t * 1'000'000LL;
    }
    if (t < static_cast<int64_t>(1e18))
    {
      return t * 1'000LL;
    }
    return t;
  }

  static std::string inferSymbol(const std::string& path)
  {
    auto pos = path.find_last_of('/');
    std::string name = pos != std::string::npos ? path.substr(pos + 1) : path;
    auto dot = name.find('.');
    if (dot != std::string::npos)
    {
      name = name.substr(0, dot);
    }
    for (const char* suf : {"_1m", "_5m", "_15m", "_1h", "_4h", "_1d"})
    {
      size_t sl = std::strlen(suf);
      if (name.size() > sl && name.substr(name.size() - sl) == suf)
      {
        name = name.substr(0, name.size() - sl);
        break;
      }
    }
    for (auto& c : name)
    {
      c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return name;
  }
};

}  // namespace

// Convert a Python value (Symbol or int) to a symbol id.
inline uint32_t symId(const py::object& o)
{
  if (py::hasattr(o, "id"))
  {
    return o.attr("id").cast<uint32_t>();
  }
  return o.cast<uint32_t>();
}

// Convert a list of Python values (Symbol or int) to symbol ids.
inline std::vector<uint32_t> symIds(const py::list& lst)
{
  std::vector<uint32_t> ids;
  ids.reserve(lst.size());
  for (auto item : lst)
  {
    ids.push_back(symId(item.cast<py::object>()));
  }
  return ids;
}

struct PySymbol
{
  uint32_t id;
  std::string exchange;
  std::string name;
  double tick_size;

  std::string repr() const
  {
    return "Symbol(" + exchange + ":" + name +
           ", id=" + std::to_string(id) + ")";
  }
};

inline void bindStrategy(py::module_& m)
{
  py::class_<PySymbol>(m, "Symbol")
      .def_readonly("id", &PySymbol::id)
      .def_readonly("exchange", &PySymbol::exchange)
      .def_readonly("name", &PySymbol::name)
      .def_readonly("tick_size", &PySymbol::tick_size)
      .def("__repr__", &PySymbol::repr)
      .def("__str__", &PySymbol::repr)
      .def("__int__", [](const PySymbol& s)
           { return static_cast<int>(s.id); })
      .def("__index__", [](const PySymbol& s)
           { return static_cast<int>(s.id); })
      .def("__eq__", [](const PySymbol& a, const PySymbol& b)
           { return a.id == b.id; })
      .def("__hash__", [](const PySymbol& s)
           { return std::hash<uint32_t>{}(s.id); });

  py::class_<SymbolRegistry>(m, "SymbolRegistry")
      .def(py::init<>())
      .def("add_symbol", [](SymbolRegistry& reg, const std::string& exchange, const std::string& symbol, double tick_size) -> PySymbol
           {
             lrvx::SymbolInfo info;
             info.exchange  = exchange;
             info.symbol    = symbol;
             info.tickSize  = Price::fromDouble(tick_size);
             uint32_t id = static_cast<uint32_t>(reg.registerSymbol(info));
             return PySymbol{id, exchange, symbol, tick_size}; }, py::arg("exchange"), py::arg("symbol"), py::arg("tick_size") = 0.01)
      .def("symbol_count", &SymbolRegistry::size);

  py::class_<PyTradeData>(m, "TradeData")
      .def(py::init<>())
      .def_readwrite("symbol", &PyTradeData::symbol)
      .def_readwrite("symbol_name", &PyTradeData::symbol_name)
      .def_readwrite("price", &PyTradeData::price)
      .def_readwrite("quantity", &PyTradeData::quantity)
      .def_readwrite("is_buy", &PyTradeData::is_buy)
      .def_readwrite("side", &PyTradeData::side)
      .def_readwrite("timestamp_ns", &PyTradeData::timestamp_ns)
      .def_readwrite("exchange_ts_ns", &PyTradeData::exchange_ts_ns);

  py::class_<PyBarData>(m, "BarData")
      .def(py::init<>())
      .def_readwrite("symbol", &PyBarData::symbol)
      .def_readwrite("symbol_name", &PyBarData::symbol_name)
      .def_readwrite("bar_type", &PyBarData::bar_type)
      .def_readwrite("bar_type_param", &PyBarData::bar_type_param)
      .def_readwrite("open", &PyBarData::open)
      .def_readwrite("high", &PyBarData::high)
      .def_readwrite("low", &PyBarData::low)
      .def_readwrite("close", &PyBarData::close)
      .def_readwrite("volume", &PyBarData::volume)
      .def_readwrite("buy_volume", &PyBarData::buy_volume)
      .def_readwrite("start_time_ns", &PyBarData::start_time_ns)
      .def_readwrite("end_time_ns", &PyBarData::end_time_ns)
      .def_readwrite("close_reason", &PyBarData::close_reason);

  py::class_<PySymbolCtx>(m, "SymbolContext")
      .def(py::init<>())
      .def_readwrite("symbol_id", &PySymbolCtx::symbol_id)
      .def_readwrite("symbol", &PySymbolCtx::symbol)
      .def_readwrite("position", &PySymbolCtx::position)
      .def_readwrite("last_trade_price", &PySymbolCtx::last_trade_price)
      .def_readwrite("best_bid", &PySymbolCtx::best_bid)
      .def_readwrite("best_ask", &PySymbolCtx::best_ask)
      .def_readwrite("mid_price", &PySymbolCtx::mid_price)
      .def_readwrite("unrealized_pnl", &PySymbolCtx::unrealized_pnl)
      .def("book_spread", &PySymbolCtx::book_spread)
      .def("is_long", &PySymbolCtx::is_long)
      .def("is_short", &PySymbolCtx::is_short)
      .def("is_flat", &PySymbolCtx::is_flat);

  py::class_<PyOrderEventData>(m, "OrderEventData")
      .def_readwrite("order_id", &PyOrderEventData::order_id)
      .def_readwrite("symbol_id", &PyOrderEventData::symbol_id)
      .def_readwrite("side", &PyOrderEventData::side)
      .def_readwrite("order_type", &PyOrderEventData::order_type)
      .def_readwrite("status", &PyOrderEventData::status)
      .def_readwrite("fill_qty", &PyOrderEventData::fill_qty)
      .def_readwrite("fill_price", &PyOrderEventData::fill_price)
      .def_readwrite("exchange_ts_ns", &PyOrderEventData::exchange_ts_ns)
      .def_readwrite("reject_reason", &PyOrderEventData::reject_reason)
      .def_readwrite("queue_ahead", &PyOrderEventData::queue_ahead)
      .def_readwrite("queue_total", &PyOrderEventData::queue_total)
      .def_readwrite("submitted_at_ns", &PyOrderEventData::submitted_at_ns)
      .def_readwrite("accepted_at_ns", &PyOrderEventData::accepted_at_ns)
      .def_readwrite("first_fill_at_ns", &PyOrderEventData::first_fill_at_ns)
      .def_readwrite("last_fill_at_ns", &PyOrderEventData::last_fill_at_ns)
      .def_readwrite("canceled_at_ns", &PyOrderEventData::canceled_at_ns)
      .def_readwrite("rejected_at_ns", &PyOrderEventData::rejected_at_ns)
      .def_readwrite("triggered_at_ns", &PyOrderEventData::triggered_at_ns)
      .def_readwrite("expired_at_ns", &PyOrderEventData::expired_at_ns)
      .def_readwrite("is_maker", &PyOrderEventData::is_maker)
      .def_readwrite("fill_role", &PyOrderEventData::fill_role)
      .def_readwrite("market_position", &PyOrderEventData::market_position)
      .def_readwrite("distance_to_best_ticks", &PyOrderEventData::distance_to_best_ticks);

  py::class_<PyStrategyBase, PyStrategyTrampoline>(m, "Strategy")
      .def(py::init([](py::list symbols)
                    { return std::make_unique<PyStrategyTrampoline>(symIds(symbols)); }),
           py::arg("symbols"))
      .def("on_trade", &PyStrategyBase::on_trade, py::arg("ctx"), py::arg("trade"))
      .def("on_book_update", &PyStrategyBase::on_book_update, py::arg("ctx"))
      .def("on_bar", &PyStrategyBase::on_bar, py::arg("ctx"), py::arg("bar"))
      .def("on_fill", &PyStrategyBase::on_fill, py::arg("ctx"), py::arg("event"))
      .def("on_order_update", &PyStrategyBase::on_order_update, py::arg("ctx"), py::arg("event"))
      .def("on_queue_position_change", &PyStrategyBase::on_queue_position_change,
           py::arg("ctx"), py::arg("event"))
      .def("on_market_position_change",
           &PyStrategyBase::on_market_position_change,
           py::arg("ctx"), py::arg("event"))
      .def("on_start", &PyStrategyBase::on_start)
      .def("on_stop", &PyStrategyBase::on_stop)
      .def("emit_market_buy", &PyStrategyBase::emit_market_buy, py::arg("symbol"),
           py::arg("quantity"))
      .def("emit_market_sell", &PyStrategyBase::emit_market_sell, py::arg("symbol"),
           py::arg("quantity"))
      .def("emit_limit_buy", &PyStrategyBase::emit_limit_buy, py::arg("symbol"), py::arg("price"),
           py::arg("quantity"))
      .def("emit_limit_sell", &PyStrategyBase::emit_limit_sell, py::arg("symbol"),
           py::arg("price"), py::arg("quantity"))
      .def("emit_cancel", &PyStrategyBase::emit_cancel, py::arg("order_id"))
      .def("emit_cancel_all", &PyStrategyBase::emit_cancel_all, py::arg("symbol"))
      .def("emit_modify", &PyStrategyBase::emit_modify, py::arg("order_id"),
           py::arg("new_price"), py::arg("new_quantity"))
      .def("emit_stop_market", &PyStrategyBase::emit_stop_market, py::arg("symbol"),
           py::arg("side"), py::arg("trigger"), py::arg("quantity"))
      .def("emit_stop_limit", &PyStrategyBase::emit_stop_limit, py::arg("symbol"),
           py::arg("side"), py::arg("trigger"), py::arg("limit_price"), py::arg("quantity"))
      .def("emit_take_profit_market", &PyStrategyBase::emit_take_profit_market,
           py::arg("symbol"), py::arg("side"), py::arg("trigger"), py::arg("quantity"))
      .def("emit_take_profit_limit", &PyStrategyBase::emit_take_profit_limit, py::arg("symbol"),
           py::arg("side"), py::arg("trigger"), py::arg("limit_price"), py::arg("quantity"))
      .def("emit_trailing_stop", &PyStrategyBase::emit_trailing_stop, py::arg("symbol"),
           py::arg("side"), py::arg("offset"), py::arg("quantity"))
      .def("emit_trailing_stop_percent", &PyStrategyBase::emit_trailing_stop_percent,
           py::arg("symbol"), py::arg("side"), py::arg("callback_bps"), py::arg("quantity"))
      .def("emit_provide_liquidity", &PyStrategyBase::emit_provide_liquidity, py::arg("pool"),
           py::arg("price_lower"), py::arg("price_upper"), py::arg("liquidity"))
      .def("emit_withdraw_liquidity", &PyStrategyBase::emit_withdraw_liquidity, py::arg("pool"),
           py::arg("liquidity"))
      .def("emit_limit_buy_tif", &PyStrategyBase::emit_limit_buy_tif, py::arg("symbol"),
           py::arg("price"), py::arg("quantity"), py::arg("tif") = "gtc")
      .def("emit_limit_sell_tif", &PyStrategyBase::emit_limit_sell_tif, py::arg("symbol"),
           py::arg("price"), py::arg("quantity"), py::arg("tif") = "gtc")
      .def("emit_close_position", &PyStrategyBase::emit_close_position, py::arg("symbol"))
      .def("get_order_status", &PyStrategyBase::get_order_status, py::arg("order_id"))
      .def("position", &PyStrategyBase::position, py::arg("symbol") = py::none())
      .def("ctx", &PyStrategyBase::ctx, py::arg("symbol") = py::none())
      .def_property_readonly("symbols", &PyStrategyBase::symbols)
      // Ergonomic API: string symbols, kwargs, no emit_ prefix
      .def("market_buy", &PyStrategyBase::market_buy, py::arg("qty"),
           py::arg("symbol") = py::none())
      .def("market_sell", &PyStrategyBase::market_sell, py::arg("qty"),
           py::arg("symbol") = py::none())
      .def("limit_buy", &PyStrategyBase::limit_buy, py::arg("price"), py::arg("qty"),
           py::arg("symbol") = py::none(), py::arg("tif") = "gtc")
      .def("limit_sell", &PyStrategyBase::limit_sell, py::arg("price"), py::arg("qty"),
           py::arg("symbol") = py::none(), py::arg("tif") = "gtc")
      .def("stop_market", &PyStrategyBase::stop_market, py::arg("side"), py::arg("trigger"),
           py::arg("qty"), py::arg("symbol") = py::none())
      .def("stop_limit", &PyStrategyBase::stop_limit, py::arg("side"), py::arg("trigger"),
           py::arg("limit_price"), py::arg("qty"), py::arg("symbol") = py::none())
      .def("take_profit_market", &PyStrategyBase::take_profit_market, py::arg("side"),
           py::arg("trigger"), py::arg("qty"), py::arg("symbol") = py::none())
      .def("take_profit_limit", &PyStrategyBase::take_profit_limit, py::arg("side"),
           py::arg("trigger"), py::arg("limit_price"), py::arg("qty"),
           py::arg("symbol") = py::none())
      .def("trailing_stop", &PyStrategyBase::trailing_stop, py::arg("side"), py::arg("offset"),
           py::arg("qty"), py::arg("symbol") = py::none())
      .def("trailing_stop_percent", &PyStrategyBase::trailing_stop_percent, py::arg("side"),
           py::arg("callback_bps"), py::arg("qty"), py::arg("symbol") = py::none())
      .def("close_position", &PyStrategyBase::close_position, py::arg("symbol") = py::none())
      .def("cancel_order", &PyStrategyBase::cancel_order, py::arg("order_id"))
      .def("cancel_all_orders", &PyStrategyBase::cancel_all_orders,
           py::arg("symbol") = py::none())
      .def("modify_order", &PyStrategyBase::modify_order, py::arg("order_id"),
           py::arg("new_price"), py::arg("new_qty"))
      .def("pos", &PyStrategyBase::pos, py::arg("symbol") = py::none())
      .def("last_price", &PyStrategyBase::last_price, py::arg("symbol") = py::none())
      .def("best_bid", &PyStrategyBase::best_bid, py::arg("symbol") = py::none())
      .def("best_ask", &PyStrategyBase::best_ask, py::arg("symbol") = py::none())
      .def("mid_price", &PyStrategyBase::mid_price, py::arg("symbol") = py::none())
      .def("order_status", &PyStrategyBase::order_status, py::arg("order_id"))
      .def("last_closed_bar", &PyStrategyBase::last_closed_bar,
           py::arg("symbol_id"), py::arg("bar_type") = uint8_t{0}, py::arg("param") = uint64_t{0},
           "Return the last closed bar for (symbol, bar_type, param) or "
           "None if no bar of that timeframe has been emitted yet. "
           "bar_type: 0=Time, 1=Tick, 2=Volume, 3=Renko, 4=Range, "
           "5=HeikinAshi, 6=BpsRange. param: time bar interval in "
           "nanoseconds, or the tick / volume / range threshold the "
           "aggregator was configured with.")
      .def("last_n_closed_bars", &PyStrategyBase::last_n_closed_bars,
           py::arg("symbol_id"), py::arg("bar_type"), py::arg("param"), py::arg("n"),
           "Return the most recent up to n closed bars for the given "
           "(symbol, timeframe), oldest first. Returns an empty list "
           "until the aggregator has emitted at least one bar.")
      .def("bar_ring_capacity", &PyStrategyBase::bar_ring_capacity)
      .def("set_bar_ring_capacity", &PyStrategyBase::set_bar_ring_capacity, py::arg("n"))
      .def_property_readonly("symbol_names", &PyStrategyBase::symbol_names)
      .def_property_readonly("primary_symbol_name", &PyStrategyBase::primary_symbol_name);

  py::class_<PySignal>(m, "Signal")
      .def(py::init<>())
      .def_readwrite("order_id", &PySignal::order_id)
      .def_readwrite("symbol", &PySignal::symbol)
      .def_readwrite("side", &PySignal::side)
      .def_readwrite("order_type", &PySignal::order_type)
      .def_readwrite("price", &PySignal::price)
      .def_readwrite("quantity", &PySignal::quantity)
      .def_readwrite("trigger_price", &PySignal::trigger_price)
      .def_readwrite("trailing_offset", &PySignal::trailing_offset)
      .def_readwrite("trailing_bps", &PySignal::trailing_bps)
      .def_readwrite("new_price", &PySignal::new_price)
      .def_readwrite("new_quantity", &PySignal::new_quantity)
      .def_readwrite("range_lower", &PySignal::range_lower)
      .def_readwrite("range_upper", &PySignal::range_upper)
      .def_readwrite("liquidity", &PySignal::liquidity);

  // ── Extension-hook base classes (users subclass these) ──────────────

  py::class_<lrvx::PyPnLTracker, lrvx::PyPnLTrackerTrampoline,
             std::shared_ptr<lrvx::PyPnLTracker>>(m, "PnLTracker")
      .def(py::init<>())
      .def("on_signal", &lrvx::PyPnLTracker::on_signal, py::arg("signal"));

  py::class_<lrvx::PyStorageSink, lrvx::PyStorageSinkTrampoline,
             std::shared_ptr<lrvx::PyStorageSink>>(m, "StorageSink")
      .def(py::init<>())
      .def("store", &lrvx::PyStorageSink::store, py::arg("signal"));

  py::class_<lrvx::PyRiskManager, lrvx::PyRiskManagerTrampoline,
             std::shared_ptr<lrvx::PyRiskManager>>(m, "RiskManager")
      .def(py::init<>())
      .def("allow", &lrvx::PyRiskManager::allow, py::arg("signal"));

  py::class_<lrvx::PyKillSwitch, lrvx::PyKillSwitchTrampoline,
             std::shared_ptr<lrvx::PyKillSwitch>>(m, "KillSwitch")
      .def(py::init<>())
      .def("check", &lrvx::PyKillSwitch::check, py::arg("signal"));

  py::class_<lrvx::PyOrderValidator, lrvx::PyOrderValidatorTrampoline,
             std::shared_ptr<lrvx::PyOrderValidator>>(m, "OrderValidator")
      .def(py::init<>())
      .def("validate", &lrvx::PyOrderValidator::validate, py::arg("signal"));

  py::class_<lrvx::PyMarketDataRecorderHook,
             lrvx::PyMarketDataRecorderHookTrampoline,
             std::shared_ptr<lrvx::PyMarketDataRecorderHook>>(
      m, "MarketDataRecorderHook")
      .def(py::init<>())
      .def("on_trade", &lrvx::PyMarketDataRecorderHook::on_trade)
      .def("on_book_update", &lrvx::PyMarketDataRecorderHook::on_book_update,
           py::arg("symbol"), py::arg("is_snapshot"), py::arg("bids"),
           py::arg("asks"), py::arg("ts_ns"))
      .def("on_start", &lrvx::PyMarketDataRecorderHook::on_start)
      .def("on_stop", &lrvx::PyMarketDataRecorderHook::on_stop);

  py::class_<lrvx::PyReplayEvent>(m, "ReplayEvent")
      .def(py::init<>())
      .def_readwrite("type", &lrvx::PyReplayEvent::type)
      .def_readwrite("timestamp_ns", &lrvx::PyReplayEvent::timestamp_ns)
      .def_readwrite("trade_symbol", &lrvx::PyReplayEvent::trade_symbol)
      .def_readwrite("trade_is_buy", &lrvx::PyReplayEvent::trade_is_buy)
      .def_readwrite("trade_price", &lrvx::PyReplayEvent::trade_price)
      .def_readwrite("trade_quantity", &lrvx::PyReplayEvent::trade_quantity)
      .def_readwrite("book_symbol", &lrvx::PyReplayEvent::book_symbol)
      .def_readwrite("bids", &lrvx::PyReplayEvent::bids)
      .def_readwrite("asks", &lrvx::PyReplayEvent::asks);

  py::class_<lrvx::PyReplaySource, lrvx::PyReplaySourceTrampoline,
             std::shared_ptr<lrvx::PyReplaySource>>(m, "ReplaySource")
      .def(py::init<>())
      .def("on_start", &lrvx::PyReplaySource::on_start)
      .def("on_stop", &lrvx::PyReplaySource::on_stop)
      .def("seek_to", &lrvx::PyReplaySource::seek_to, py::arg("ts_ns"))
      .def("next", &lrvx::PyReplaySource::next);

  py::class_<lrvx::PyOrder>(m, "Order")
      .def(py::init<>())
      .def_readwrite("id", &lrvx::PyOrder::id)
      .def_readwrite("client_order_id", &lrvx::PyOrder::client_order_id)
      .def_readwrite("symbol", &lrvx::PyOrder::symbol)
      .def_readwrite("strategy_id", &lrvx::PyOrder::strategy_id)
      .def_readwrite("order_tag", &lrvx::PyOrder::order_tag)
      .def_readwrite("side", &lrvx::PyOrder::side)
      .def_readwrite("order_type", &lrvx::PyOrder::order_type)
      .def_readwrite("time_in_force", &lrvx::PyOrder::time_in_force)
      .def_readwrite("reduce_only", &lrvx::PyOrder::reduce_only)
      .def_readwrite("post_only", &lrvx::PyOrder::post_only)
      .def_readwrite("close_position", &lrvx::PyOrder::close_position)
      .def_readwrite("price", &lrvx::PyOrder::price)
      .def_readwrite("quantity", &lrvx::PyOrder::quantity)
      .def_readwrite("filled_quantity", &lrvx::PyOrder::filled_quantity)
      .def_readwrite("trigger_price", &lrvx::PyOrder::trigger_price)
      .def_readwrite("trailing_offset", &lrvx::PyOrder::trailing_offset)
      .def_readwrite("created_at_ns", &lrvx::PyOrder::created_at_ns)
      .def_readwrite("exchange_ts_ns", &lrvx::PyOrder::exchange_ts_ns);

  py::class_<lrvx::PyExchangeCapabilities>(m, "ExchangeCapabilities")
      .def(py::init<>())
      .def_readwrite("stop_market", &lrvx::PyExchangeCapabilities::stop_market)
      .def_readwrite("stop_limit", &lrvx::PyExchangeCapabilities::stop_limit)
      .def_readwrite("take_profit_market",
                     &lrvx::PyExchangeCapabilities::take_profit_market)
      .def_readwrite("take_profit_limit",
                     &lrvx::PyExchangeCapabilities::take_profit_limit)
      .def_readwrite("trailing_stop", &lrvx::PyExchangeCapabilities::trailing_stop)
      .def_readwrite("iceberg", &lrvx::PyExchangeCapabilities::iceberg)
      .def_readwrite("oco", &lrvx::PyExchangeCapabilities::oco)
      .def_readwrite("gtc", &lrvx::PyExchangeCapabilities::gtc)
      .def_readwrite("ioc", &lrvx::PyExchangeCapabilities::ioc)
      .def_readwrite("fok", &lrvx::PyExchangeCapabilities::fok)
      .def_readwrite("gtd", &lrvx::PyExchangeCapabilities::gtd)
      .def_readwrite("post_only", &lrvx::PyExchangeCapabilities::post_only)
      .def_readwrite("reduce_only", &lrvx::PyExchangeCapabilities::reduce_only)
      .def_readwrite("close_position", &lrvx::PyExchangeCapabilities::close_position)
      .def_readwrite("batch_submit", &lrvx::PyExchangeCapabilities::batch_submit);

  py::class_<lrvx::PyExecutor, lrvx::PyExecutorTrampoline,
             std::shared_ptr<lrvx::PyExecutor>>(m, "Executor")
      .def(py::init<>())
      .def("submit", &lrvx::PyExecutor::submit, py::arg("order"))
      .def("cancel", &lrvx::PyExecutor::cancel, py::arg("order_id"))
      .def("cancel_all", &lrvx::PyExecutor::cancel_all, py::arg("symbol"))
      .def("replace", &lrvx::PyExecutor::replace, py::arg("old_order_id"),
           py::arg("new_order"))
      .def("submit_oco", &lrvx::PyExecutor::submit_oco, py::arg("order1"),
           py::arg("order2"))
      .def("capabilities", &lrvx::PyExecutor::capabilities)
      .def("on_start", &lrvx::PyExecutor::on_start)
      .def("on_stop", &lrvx::PyExecutor::on_stop);

  py::class_<lrvx::PyExecutionListener, lrvx::PyExecutionListenerTrampoline,
             std::shared_ptr<lrvx::PyExecutionListener>>(m, "ExecutionListener")
      .def(py::init<>())
      .def("on_submitted", &lrvx::PyExecutionListener::on_submitted)
      .def("on_accepted", &lrvx::PyExecutionListener::on_accepted)
      .def("on_partially_filled", &lrvx::PyExecutionListener::on_partially_filled,
           py::arg("order"), py::arg("fill_qty"))
      .def("on_filled", &lrvx::PyExecutionListener::on_filled)
      .def("on_pending_cancel", &lrvx::PyExecutionListener::on_pending_cancel)
      .def("on_canceled", &lrvx::PyExecutionListener::on_canceled)
      .def("on_expired", &lrvx::PyExecutionListener::on_expired)
      .def("on_rejected", &lrvx::PyExecutionListener::on_rejected,
           py::arg("order"), py::arg("reason"))
      .def("on_replaced", &lrvx::PyExecutionListener::on_replaced,
           py::arg("old_order"), py::arg("new_order"))
      .def("on_pending_trigger", &lrvx::PyExecutionListener::on_pending_trigger)
      .def("on_triggered", &lrvx::PyExecutionListener::on_triggered)
      .def("on_trailing_stop_updated",
           &lrvx::PyExecutionListener::on_trailing_stop_updated,
           py::arg("order"), py::arg("new_trigger"));

  m.def("set_log_callback", &lrvx::setPythonLogCallback, py::arg("callback"),
        "Install a Python callable as the global log sink. Pass None to "
        "detach. Callable receives (level: int, msg: str); level: 0=info, "
        "1=warn, 2=error. The binding detaches it automatically at "
        "interpreter shutdown.");

  // The sink is a Python callable held by the C API for the whole
  // process. Detach it while the interpreter is still up, or shutdown
  // releases it afterwards and the process crashes on exit.
  py::module_::import("atexit").attr("register")(
      py::cpp_function(&lrvx::logCallbackTeardown));

  py::class_<PyRunner>(m, "Runner")
      .def(py::init([](SymbolRegistry* reg, py::object on_signal, bool threaded)
                    { return std::make_unique<PyRunner>(reg, std::move(on_signal), threaded); }),
           py::arg("registry"), py::arg("on_signal"), py::arg("threaded") = false,
           py::keep_alive<1, 2>())
      .def("add_strategy", &PyRunner::add_strategy, py::arg("strategy"),
           py::keep_alive<1, 2>())
      .def("replace_strategy", &PyRunner::replace_strategy,
           py::arg("index"), py::arg("strategy"), py::keep_alive<1, 3>(),
           "Atomically swap the strategy instance at `index` for a new "
           "one. The old strategy's on_stop fires, the bridge's "
           "internal state survives, the new strategy's on_start fires "
           "afterwards. WebSocket / gRPC connections are unaffected.")
      .def("start", &PyRunner::start)
      .def("stop", &PyRunner::stop)
      .def("set_pnl_tracker", &PyRunner::set_pnl_tracker, py::arg("tracker"),
           py::keep_alive<1, 2>())
      .def("set_storage_sink", &PyRunner::set_storage_sink, py::arg("sink"),
           py::keep_alive<1, 2>())
      .def("set_risk_manager", &PyRunner::set_risk_manager, py::arg("rm"),
           py::keep_alive<1, 2>())
      .def("set_kill_switch", &PyRunner::set_kill_switch, py::arg("ks"),
           py::keep_alive<1, 2>())
      .def("set_order_validator", &PyRunner::set_order_validator, py::arg("ov"),
           py::keep_alive<1, 2>())
      .def("set_market_data_recorder",
           py::overload_cast<std::shared_ptr<lrvx::PyMarketDataRecorderHook>>(
               &PyRunner::set_market_data_recorder),
           py::arg("recorder"), py::keep_alive<1, 2>())
      .def("set_market_data_recorder",
           py::overload_cast<std::shared_ptr<lrvx::PyBinaryLogRecorderHook>>(
               &PyRunner::set_market_data_recorder),
           py::arg("recorder"), py::keep_alive<1, 2>())
      .def("set_executor", &PyRunner::set_executor, py::arg("executor"),
           py::keep_alive<1, 2>())
      .def("attach_trace_recorder", [](PyRunner& r, py::object recorder)
           {
             // Accepts a lrvx.TraceRecorder or None. The TraceRecorder
             // wraps a `lrvx::run::TraceRecorder*` reachable through its
             // `_handle()` accessor; we pass the raw pointer through the
             // C ABI so every binding shares one auto-capture path.
             if (recorder.is_none())
             {
               lrvx_runner_attach_trace_recorder(r._runner_handle(), nullptr);
               return;
             }
             // The pybind11 binding constructs a TraceRecorder by value
             // (defined in run_trace_bindings.h) so we cast to that exact
             // type and pass its address.
             auto* trec = recorder.cast<lrvx::run::TraceRecorder*>();
             lrvx_runner_attach_trace_recorder(r._runner_handle(),
                                                static_cast<void*>(trec)); }, py::arg("recorder"), py::keep_alive<1, 2>())
      .def("set_trace_feed_ts_ns", [](PyRunner& r, int64_t ts)
           { lrvx_runner_set_trace_feed_ts_ns(r._runner_handle(), ts); }, py::arg("feed_ts_ns"))
      .def("trace_order_event", [](PyRunner& r, uint64_t order_id, uint64_t parent_signal_id, uint32_t symbol_id, uint8_t event_kind, uint8_t side, uint8_t order_type, double price, double qty, uint32_t flags)
           { lrvx_runner_trace_order_event(
                 r._runner_handle(), order_id, parent_signal_id, symbol_id, event_kind, side,
                 order_type, lrvx::Price::fromDouble(price).raw(),
                 lrvx::Quantity::fromDouble(qty).raw(), flags); }, py::arg("order_id"), py::arg("parent_signal_id"), py::arg("symbol_id"), py::arg("event_kind"), py::arg("side"), py::arg("order_type"), py::arg("price"), py::arg("qty"), py::arg("flags") = 0u)
      .def("trace_fill", [](PyRunner& r, uint64_t order_id, uint64_t fill_id, double price, double qty, double fee, uint32_t symbol_id, uint8_t side, uint8_t liquidity)
           { lrvx_runner_trace_fill(r._runner_handle(), order_id, fill_id,
                                    lrvx::Price::fromDouble(price).raw(),
                                    lrvx::Quantity::fromDouble(qty).raw(),
                                    lrvx::Quantity::fromDouble(fee).raw(), symbol_id, side,
                                    liquidity); }, py::arg("order_id"), py::arg("fill_id"), py::arg("price"), py::arg("qty"), py::arg("fee"), py::arg("symbol_id"), py::arg("side"), py::arg("liquidity") = 0u)
      .def("on_trade", [](PyRunner& r, py::object sym, double price, double qty, bool is_buy, int64_t ts_ns)
           { r.on_trade(symId(sym), price, qty, is_buy, ts_ns); }, py::arg("symbol"), py::arg("price"), py::arg("qty"), py::arg("is_buy"), py::arg("ts_ns") = 0)
      .def("on_book_snapshot", [](PyRunner& r, py::object sym, const std::vector<double>& bp, const std::vector<double>& bq, const std::vector<double>& ap, const std::vector<double>& aq, int64_t ts_ns)
           { r.on_book_snapshot(symId(sym), bp, bq, ap, aq, ts_ns); }, py::arg("symbol"), py::arg("bid_prices"), py::arg("bid_qtys"), py::arg("ask_prices"), py::arg("ask_qtys"), py::arg("ts_ns") = 0)
      .def("on_bar", [](PyRunner& r, py::object sym, double o, double h, double l, double c, double v, double bv, int64_t start_ns, int64_t end_ns, uint8_t bar_type, uint64_t bar_type_param, uint8_t close_reason)
           { r.on_bar(symId(sym), o, h, l, c, v, bv, start_ns, end_ns,
                      bar_type, bar_type_param, close_reason); }, py::arg("symbol"), py::arg("open"), py::arg("high"), py::arg("low"), py::arg("close"), py::arg("volume") = 0.0, py::arg("buy_volume") = 0.0, py::arg("start_time_ns") = 0, py::arg("end_time_ns") = 0, py::arg("bar_type") = 0, py::arg("bar_type_param") = 0, py::arg("close_reason") = 0);

  py::class_<PyBacktestRunner>(m, "BacktestRunner")
      .def(py::init([](SymbolRegistry* reg, double fee_rate, double initial_capital)
                    { return std::make_unique<PyBacktestRunner>(reg, fee_rate, initial_capital); }),
           py::arg("registry"),
           py::arg("fee_rate") = 0.0004,
           py::arg("initial_capital") = 100000.0,
           py::keep_alive<1, 2>())
      .def("set_strategy", &PyBacktestRunner::set_strategy, py::arg("strategy"),
           py::keep_alive<1, 2>())
      .def("set_executor", &PyBacktestRunner::set_executor, py::arg("executor"),
           py::keep_alive<1, 2>())
      // Overload that exists only to fail with an explanation: passing a
      // VenueStack executor here would run to completion with zero trades.
      .def("set_executor", &PyBacktestRunner::set_venue_executor,
           py::arg("executor"),
           "Refused: pass the stack via set_venue_stack(stack) instead. The "
           "executor alone does not carry the clock the runner must advance.")
      .def("set_venue_stack", &PyBacktestRunner::set_venue_stack,
           py::arg("stack").none(true), py::keep_alive<1, 2>(),
           "Run against a VenueStack: the runner feeds the stack executor "
           "market data and harvests its fills, so the run uses the venue's "
           "fill mechanics (queue model, iceberg latency, venue "
           "availability, rate limits) and its fee ladder -- each fill is "
           "billed at the tier the stack's 30-day notional resolves to "
           "instead of the flat fee_rate. Funding and liquidation are still "
           "driven by explicit calls rather than the replay loop. Pass None "
           "to revert to the built-in executor.")
      .def("add_execution_listener", &PyBacktestRunner::add_execution_listener,
           py::arg("listener"), py::keep_alive<1, 2>())
      .def("set_risk_manager", &PyBacktestRunner::set_risk_manager,
           py::arg("rm").none(true), py::keep_alive<1, 2>(),
           "Attach (or detach with None) a pre-trade risk manager. "
           "Reduce-only orders bypass the gate by design.")
      .def("set_kill_switch", &PyBacktestRunner::set_kill_switch,
           py::arg("ks").none(true), py::keep_alive<1, 2>(),
           "Attach (or detach with None) a kill switch. Reduce-only "
           "orders bypass so tightening caps does not strand a position.")
      .def("set_order_validator", &PyBacktestRunner::set_order_validator,
           py::arg("ov").none(true), py::keep_alive<1, 2>(),
           "Attach (or detach with None) an order validator. Reduce-only "
           "orders bypass.")
      .def("set_pnl_tracker", &PyBacktestRunner::set_pnl_tracker,
           py::arg("tracker").none(true), py::keep_alive<1, 2>(),
           "Attach (or detach with None) a PnL tracker. Fires "
           "`on_signal(signal)` for every fill the simulator dispatches.")
      .def("run_csv", &PyBacktestRunner::run_csv,
           py::arg("path"), py::arg("symbol") = "")
      .def("run_tape", &PyBacktestRunner::run_tape, py::arg("path"),
           "Run a backtest off a `.lrvx` tape directory. The tape is "
           "the canonical recorded artifact (`lrvx tape record` writes it). "
           "Returns the same stats dict shape as run_csv / run_bars.")
      .def("run_tapes", &PyBacktestRunner::run_tapes, py::arg("paths"),
           "Run a backtest off N `.lrvx` tapes merged on read. "
           "Symbols are rekeyed by (metadata.exchange, name) so two "
           "captures of the same venue/symbol collapse, and two "
           "different venues stay distinct. `run_tapes([t])` is "
           "equivalent to `run_tape(t)`. Stats shape mirrors run_tape.")
      .def("run_ohlcv", &PyBacktestRunner::run_ohlcv,
           py::arg("ts"), py::arg("close"), py::arg("symbol") = "")
      .def("run_bars", &PyBacktestRunner::run_bars,
           py::arg("start_time_ns"), py::arg("end_time_ns"),
           py::arg("open"), py::arg("high"), py::arg("low"), py::arg("close"),
           py::arg("volume"),
           py::arg("symbol") = "",
           py::arg("bar_type") = 0,
           py::arg("bar_type_param") = 0)
      .def("equity_curve", &PyBacktestRunner::equity_curve,
           "Return the equity curve from the most recent run as a dict of "
           "numpy arrays (timestamp_ns, equity, drawdown_pct).")
      .def("trades", &PyBacktestRunner::trades,
           "Return closed trades from the most recent run as a dict of numpy "
           "arrays (symbol, side, entry_price, exit_price, quantity, pnl, "
           "fee, entry_time_ns, exit_time_ns).");
}
