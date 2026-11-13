// python/strategy_bindings.h

#pragma once

#include <pybind11/functional.h>
#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <limits>
#include <stdexcept>

#include "backtest_bindings.h"
#include "callback_guard.h"
#include "drop_reason.h"
#include "hook_bindings.h"
#include "lrvx/backtest/backtest_config.h"
#include "lrvx/backtest/backtest_result.h"
#include "lrvx/backtest/backtest_runner.h"
#include "lrvx/backtest/latency_model.h"
#include "lrvx/backtest/runner_fill_settings.h"
#include "lrvx/backtest/venue_stack.h"
#include "lrvx/book/bus/book_update_bus.h"
#include "lrvx/book/bus/trade_bus.h"
#include "lrvx/book/events/book_update_event.h"
#include "lrvx/book/events/trade_event.h"
#include "lrvx/capi/bridge_strategy.h"
#include "lrvx/capi/lrvx_capi.h"
#include "lrvx/capi/order_status_names.hpp"
#include "lrvx/capi/order_type_names.hpp"
#include "lrvx/engine/symbol_registry.h"
#include "lrvx/error/lrvx_error.h"
#include "lrvx/execution/order_price_rule.h"
#include "lrvx/replay/abstract_event_reader.h"
#include "lrvx/replay/binary_format_v1.h"
#include "lrvx/replay/binary_log_recorder_hook.h"
#include "lrvx/run/trace_recorder.h"
#include "lrvx/util/memory/pool.h"
#include "replay_bindings.h"
#include "types_bindings.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
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
  // Exchange timestamps in nanoseconds. last_update_ns stamps the last
  // trade, depth frame or bar; last_touch_ns stamps the frame that last
  // wrote the touch, which a TOP_OF_BOOK frame does without touching
  // last_update_ns, so it can be the fresher of the two.
  int64_t last_update_ns;
  int64_t last_touch_ns;

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

inline void fillStamps(PySymbolCtx& pc, const LrvxSymbolContext& ctx)
{
  pc.last_update_ns = ctx.last_update_ns;
  pc.last_touch_ns = ctx.last_touch_ns;
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

// Anything but "buy" used to be a sell, and anything but "ioc", "fok" or
// "post_only" a GTC order: a typo placed an order nobody asked for.
inline Side parseSide(const std::string& s)
{
  if (s == "buy")
  {
    return Side::BUY;
  }
  if (s == "sell")
  {
    return Side::SELL;
  }
  throw py::value_error("unknown side '" + s + "'; expected 'buy' or 'sell'");
}

inline TimeInForce parseTimeInForce(const std::string& tif)
{
  if (tif == "gtc")
  {
    return TimeInForce::GTC;
  }
  if (tif == "ioc")
  {
    return TimeInForce::IOC;
  }
  if (tif == "fok")
  {
    return TimeInForce::FOK;
  }
  if (tif == "post_only")
  {
    return TimeInForce::POST_ONLY;
  }
  if (tif == "gtd")
  {
    throw py::value_error(
        "tif 'gtd' needs an expiry time, which this method does not take; "
        "expected 'gtc', 'ioc', 'fok' or 'post_only'");
  }
  throw py::value_error("unknown tif '" + tif +
                        "'; expected 'gtc', 'ioc', 'fok' or 'post_only'");
}

class PyStrategyBase
{
 public:
  PyStrategyBase(std::vector<uint32_t> symbols) : _symbols(std::move(symbols)) {}

  virtual ~PyStrategyBase() = default;

  virtual void on_trade(const PySymbolCtx& ctx, const PyTradeData& trade) {}
  virtual void on_book_update(const PySymbolCtx& ctx) {}
  virtual void on_touch(const PySymbolCtx& ctx) {}
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
    return emitting()->publicEmitMarketBuy(symbol, Quantity::fromDouble(qty));
  }

  uint64_t emit_market_sell(uint32_t symbol, double qty)
  {
    if (!_bridge)
    {
      return 0;
    }
    return emitting()->publicEmitMarketSell(symbol, Quantity::fromDouble(qty));
  }

  uint64_t emit_limit_buy(uint32_t symbol, double price, double qty)
  {
    if (!_bridge)
    {
      return 0;
    }
    return emitting()->publicEmitLimitBuy(symbol, Price::fromDouble(price),
                                          Quantity::fromDouble(qty));
  }

  uint64_t emit_limit_sell(uint32_t symbol, double price, double qty)
  {
    if (!_bridge)
    {
      return 0;
    }
    return emitting()->publicEmitLimitSell(symbol, Price::fromDouble(price),
                                           Quantity::fromDouble(qty));
  }

  uint64_t emit_provide_liquidity(uint32_t pool, double price_lower, double price_upper,
                                  double liquidity)
  {
    if (!_bridge)
    {
      return 0;
    }
    return emitting()->publicEmitProvideLiquidity(pool, Price::fromDouble(price_lower),
                                                  Price::fromDouble(price_upper),
                                                  Quantity::fromDouble(liquidity));
  }

  uint64_t emit_withdraw_liquidity(uint32_t pool, double liquidity)
  {
    if (!_bridge)
    {
      return 0;
    }
    return emitting()->publicEmitWithdrawLiquidity(pool, Quantity::fromDouble(liquidity));
  }

  void emit_cancel(uint64_t order_id)
  {
    if (_bridge)
    {
      emitting()->publicEmitCancel(order_id);
    }
  }

  void emit_cancel_all(uint32_t symbol)
  {
    if (_bridge)
    {
      emitting()->publicEmitCancelAll(symbol);
    }
  }

  void emit_modify(uint64_t order_id, double new_price, double new_qty)
  {
    if (_bridge)
    {
      emitting()->publicEmitModify(order_id, Price::fromDouble(new_price),
                                   Quantity::fromDouble(new_qty));
    }
  }

  uint64_t emit_stop_market(uint32_t symbol, const std::string& side, double trigger, double qty)
  {
    if (!_bridge)
    {
      return 0;
    }
    return emitting()->publicEmitStopMarket(symbol, parseSide(side), Price::fromDouble(trigger),
                                            Quantity::fromDouble(qty));
  }

  uint64_t emit_stop_limit(uint32_t symbol, const std::string& side, double trigger,
                           double limit_price, double qty)
  {
    if (!_bridge)
    {
      return 0;
    }
    return emitting()->publicEmitStopLimit(symbol, parseSide(side), Price::fromDouble(trigger),
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
    return emitting()->publicEmitTakeProfitMarket(symbol, parseSide(side),
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
    return emitting()->publicEmitTakeProfitLimit(symbol, parseSide(side),
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
    return emitting()->publicEmitTrailingStop(symbol, parseSide(side), Price::fromDouble(offset),
                                              Quantity::fromDouble(qty));
  }

  uint64_t emit_trailing_stop_percent(uint32_t symbol, const std::string& side,
                                      int32_t callback_bps, double qty)
  {
    if (!_bridge)
    {
      return 0;
    }
    return emitting()->publicEmitTrailingStopPercent(symbol, parseSide(side), callback_bps,
                                                     Quantity::fromDouble(qty));
  }

  uint64_t emit_limit_buy_tif(uint32_t symbol, double price, double qty, const std::string& tif)
  {
    if (!_bridge)
    {
      return 0;
    }
    const TimeInForce t = parseTimeInForce(tif);
    return emitting()->publicEmitLimitBuyTif(symbol, Price::fromDouble(price),
                                             Quantity::fromDouble(qty), t);
  }

  uint64_t emit_limit_sell_tif(uint32_t symbol, double price, double qty, const std::string& tif)
  {
    if (!_bridge)
    {
      return 0;
    }
    const TimeInForce t = parseTimeInForce(tif);
    return emitting()->publicEmitLimitSellTif(symbol, Price::fromDouble(price),
                                              Quantity::fromDouble(qty), t);
  }

  uint64_t emit_close_position(uint32_t symbol)
  {
    if (!_bridge)
    {
      return 0;
    }
    return emitting()->publicEmitClosePosition(symbol);
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
    result.last_update_ns = c.lastUpdateNs;
    result.last_touch_ns = c.lastTouchNs;
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

  // lrvx.last_error() after the call is '' unless a live runner refused the
  // modify (an id its executor record does not hold) or a gate dropped it:
  // every emit clears the last error first (see emitting()).
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

  // What every emit calls through. The last error is cleared first, so
  // lrvx.last_error() read right after an emit is '' unless that emit left a
  // reason: a live runner refused a modify, or a pre-trade gate dropped the
  // signal.
  BridgeStrategy* emitting() const
  {
    lrvx_clear_last_error();
    return _bridge;
  }

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

  void on_touch(const PySymbolCtx& ctx) override
  {
    PYBIND11_OVERRIDE(void, PyStrategyBase, on_touch, ctx);
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
  // Whether the strategy overrides on_touch. Most depth deltas change the
  // size at the touch, so without this a strategy that never asked for
  // on_touch would cross into Python twice per book frame, and on a live
  // host take the GIL on the bus thread for each top. Set where a strategy
  // is attached, which holds the GIL.
  std::atomic<bool> wants_touch{false};

  static bool overridesTouch(const PyStrategyBase* s)
  {
    return static_cast<bool>(py::get_override(s, "on_touch"));
  }

  // Every one of the ten bridge callbacks goes through here. The
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
    wants_touch.store(overridesTouch(new_strat), std::memory_order_release);
    fire_lifecycle(new_strat, true);
  }

  PyStrategyHost(PyStrategyBase* strat, SymbolRegistry* reg,
                 uint32_t id, bool with_gil_)
      : strategy(strat), with_gil(with_gil_), wants_touch(overridesTouch(strat))
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
    cbs.on_touch = &PyStrategyHost::onTouch;

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
      fillStamps(pc, *ctx);

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
      fillStamps(pc, *ctx);
      self->strategy.load(std::memory_order_acquire)->on_book_update(pc);
    };
    dispatch(self, "Strategy.on_book_update", call);
  }

  static void onTouch(void* ud, const LrvxSymbolContext* ctx,
                      const LrvxBookData* /*book*/)
  {
    auto* self = static_cast<PyStrategyHost*>(ud);
    if (!self->wants_touch.load(std::memory_order_acquire))
    {
      return;
    }
    auto call = [self, ctx]()
    {
      PySymbolCtx pc{};
      pc.symbol_id = ctx->symbol_id;
      pc.position = lrvx_quantity_to_double(ctx->position_raw);
      pc.last_trade_price = lrvx_price_to_double(ctx->last_trade_price_raw);
      fillQuotes(pc, ctx->book);
      fillStamps(pc, *ctx);
      self->strategy.load(std::memory_order_acquire)->on_touch(pc);
    };
    dispatch(self, "Strategy.on_touch", call);
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
      fillStamps(pc, *ctx);

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
      fillStamps(pc, *ctx);
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
      fillStamps(pc, *ctx);
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
      fillStamps(pc, *ctx);
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
      fillStamps(pc, *ctx);
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
    lrvx_clear_last_error();
    lrvx_runner_add_strategy(_runner,
                             static_cast<LrvxStrategyHandle>(host->bridge.get()));
    if (lrvx_last_error_code() != 0)
    {
      throw lrvx::LrvxError("E_VAL_002",
                            std::string("Runner.add_strategy: ") + lrvx_last_error_message());
    }
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
  const std::vector<std::unique_ptr<PyStrategyHost>>& hosts() const noexcept { return _hosts; }

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
        hk->onBookUpdate(symbol, is_snap,
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

  void order_closed(uint64_t order_id) { lrvx_runner_order_closed(_runner, order_id); }
  void on_order_event(const LrvxOrderEventData& ev) { lrvx_runner_on_order_event(_runner, &ev); }

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

  void on_book_top(uint32_t symbol, const std::optional<std::pair<double, double>>& bid,
                   const std::optional<std::pair<double, double>>& ask, int64_t ts_ns)
  {
    lrvx_runner_on_book_top(_runner, symbol,
                            bid ? 1 : 0, bid ? bid->first : 0.0, bid ? bid->second : 0.0,
                            ask ? 1 : 0, ask ? ask->first : 0.0, ask ? ask->second : 0.0,
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
// Each strategy runs in its own bus consumer thread. The publish_*
// methods are lock-free and return immediately.
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
    lrvx_clear_last_error();
    lrvx_live_engine_add_strategy(_engine,
                                  static_cast<LrvxStrategyHandle>(host->bridge.get()),
                                  &PyLiveEngine::signalCallback, this);
    if (lrvx_last_error_code() != 0)
    {
      throw lrvx::LrvxError("E_VAL_002",
                            std::string("LiveEngine.add_strategy: ") + lrvx_last_error_message());
    }
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

  // The publish_* methods release the GIL for the duration of the
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

  void publish_book_top(uint32_t symbol, const std::optional<std::pair<double, double>>& bid,
                        const std::optional<std::pair<double, double>>& ask, int64_t ts_ns)
  {
    const uint8_t has_bid = bid ? 1 : 0;
    const double bid_price = bid ? bid->first : 0.0;
    const double bid_qty = bid ? bid->second : 0.0;
    const uint8_t has_ask = ask ? 1 : 0;
    const double ask_price = ask ? ask->first : 0.0;
    const double ask_qty = ask ? ask->second : 0.0;
    py::gil_scoped_release release;
    lrvx_live_engine_publish_book_top(_engine, symbol, has_bid, bid_price, bid_qty,
                                      has_ask, ask_price, ask_qty, ts_ns);
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
        hk->onBookUpdate(symbol, is_snap,
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

  LrvxLiveEngineHandle handle() const noexcept { return _engine; }
  const std::vector<std::unique_ptr<PyStrategyHost>>& hosts() const noexcept { return _hosts; }

  void order_closed(uint64_t order_id) { lrvx_live_engine_order_closed(_engine, order_id); }
  // Released like the publish_* methods: the event is queued behind a ring a
  // consumer drains, and on an engine that is not running the callbacks run
  // here and take the GIL themselves.
  void on_order_event(const LrvxOrderEventData& ev)
  {
    std::string why;
    {
      py::gil_scoped_release release;
      lrvx_clear_last_error();
      lrvx_live_engine_on_order_event(_engine, &ev);
      if (lrvx_last_error_code() != 0)
      {
        why = lrvx_last_error_message();
      }
    }
    if (!why.empty())
    {
      throw std::runtime_error("Runner.on_order_event: " + why);
    }
  }
  void drain()
  {
    py::gil_scoped_release release;
    lrvx_live_engine_drain(_engine);
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

  void order_closed(uint64_t order_id)
  {
    if (_live)
    {
      _live->order_closed(order_id);
    }
    else
    {
      _sync->order_closed(order_id);
    }
  }

  // The position `strat` holds before any fill is reported to this runner.
  void seed_position(PyStrategyBase* strat, uint32_t symbol, double position,
                     double avg_entry_price)
  {
    const auto bridgeOf = [strat](const auto& hosts) -> BridgeStrategy*
    {
      for (const auto& host : hosts)
      {
        if (host->strategy.load(std::memory_order_acquire) == strat)
        {
          return host->bridge.get();
        }
      }
      return nullptr;
    };
    BridgeStrategy* bridge = _live ? bridgeOf(_live->hosts()) : bridgeOf(_sync->hosts());
    if (bridge == nullptr)
    {
      throw py::value_error("Runner.seed_position: the strategy is not attached to this runner");
    }
    const int64_t positionRaw = Quantity::fromDouble(position).raw();
    const int64_t avgRaw = Price::fromDouble(avg_entry_price).raw();
    lrvx_clear_last_error();
    const uint8_t ok =
        _live ? lrvx_live_engine_seed_position(_live->handle(),
                                               static_cast<LrvxStrategyHandle>(bridge), symbol,
                                               positionRaw, avgRaw)
              : lrvx_runner_seed_position(_sync->handle(),
                                          static_cast<LrvxStrategyHandle>(bridge), symbol,
                                          positionRaw, avgRaw);
    if (ok == 0)
    {
      throw py::value_error(std::string("Runner.seed_position: ") + lrvx_last_error_message());
    }
  }

  // A synchronous runner has delivered everything by the time a call returns.
  void drain()
  {
    if (_live)
    {
      _live->drain();
    }
  }

  // Replays a `.lrvx` tape into this runner with `sim` standing in for the
  // venue. The loop is BacktestRunner's own tape loop (run / processEvent),
  // pointed at `sim` in place of its built-in executor: each trade, book
  // snapshot, book delta and top-of-book frame moves the simulator's clock
  // and reaches the simulator first, then this runner's strategies, in the
  // tape's time order. So an order resting in `sim` fills from the books as
  // it would in a backtest of the same tape, not from trades alone.
  py::dict replay_tape(const std::string& path, PySimulatedExecutor& sim,
                       std::optional<uint32_t> tape_symbol, std::optional<uint32_t> symbol,
                       py::object lock, py::object after_event)
  {
    namespace fs = std::filesystem;
    if (!fs::is_directory(fs::path(path)))
    {
      throw py::value_error("Runner.replay_tape: not a directory: " + path);
    }
    if (symbol.has_value() != tape_symbol.has_value())
    {
      throw py::value_error(
          "Runner.replay_tape: tape_symbol_id and symbol go together: the tape's id and the "
          "registered id its events are delivered under");
    }
    replay::ReaderFilter filter;
    if (tape_symbol)
    {
      filter.symbols.insert(*tape_symbol);
    }
    auto inner = replay::createMultiSegmentReader(fs::path(path), filter);
    if (!inner)
    {
      throw std::runtime_error("Runner.replay_tape: cannot open `.lrvx` directory: " + path);
    }

    // Holds `lock` (anything with acquire / release) while the simulator is
    // shown an event, so that whoever else submits to it under the same lock
    // never does so mid-event.
    struct Held
    {
      py::object lock;
      bool held{false};
      void acquire()
      {
        if (!lock.is_none() && !held)
        {
          lock.attr("acquire")();
          held = true;
        }
      }
      void release()
      {
        if (held)
        {
          held = false;
          lock.attr("release")();
        }
      }
    } held{std::move(lock)};

    // The tape's events with the chosen symbol's id rewritten to the
    // registered one.
    struct Remapped final : replay::IMultiSegmentReader
    {
      replay::IMultiSegmentReader& inner;
      std::optional<uint32_t> to;
      Held& held;
      py::object& afterEvent;

      Remapped(replay::IMultiSegmentReader& r, std::optional<uint32_t> id, Held& h,
               py::object& after)
          : inner(r), to(id), held(h), afterEvent(after)
      {
      }

      bool one(const replay::ReplayEvent& event, const EventCallback& callback)
      {
        held.acquire();
        bool more = true;
        try
        {
          if (to)
          {
            replay::ReplayEvent copy = event;
            copy.trade.symbol_id = *to;
            copy.book_header.symbol_id = *to;
            copy.option_quote.symbol_id = *to;
            copy.pool_state_header.symbol_id = *to;
            more = callback(copy);
          }
          else
          {
            more = callback(event);
          }
        }
        catch (...)
        {
          held.release();
          throw;
        }
        held.release();
        if (!afterEvent.is_none())
        {
          afterEvent();
        }
        return more;
      }
      uint64_t forEach(EventCallback callback) override
      {
        return inner.forEach([&](const replay::ReplayEvent& e)
                             { return one(e, callback); });
      }
      uint64_t forEachFrom(int64_t start, EventCallback callback) override
      {
        return inner.forEachFrom(start, [&](const replay::ReplayEvent& e)
                                 { return one(e, callback); });
      }
      const std::vector<replay::SegmentInfo>& segments() const override
      {
        return inner.segments();
      }
      uint64_t totalEvents() const override { return inner.totalEvents(); }
    } reader{*inner, symbol, held, after_event};

    // The runner's side of the loop: what a backtest hands its strategy goes
    // to this runner instead. The simulator has seen the event by now, so
    // the lock is given back before a strategy can submit under it.
    struct Feed final : IMarketDataSubscriber
    {
      PyRunner& runner;
      Held& held;
      uint64_t trades{0};
      uint64_t books{0};
      uint64_t deltas{0};

      Feed(PyRunner& r, Held& h) : runner(r), held(h) {}
      SubscriberId id() const override { return 0xFEFEFEFDu; }

      void onTrade(const TradeEvent& ev) override
      {
        held.release();
        ++trades;
        runner.on_trade(ev.trade.symbol, ev.trade.price.toDouble(), ev.trade.quantity.toDouble(),
                        ev.trade.isBuy, ev.trade.exchangeTsNs.raw());
      }
      void onBookUpdate(const BookUpdateEvent& ev) override
      {
        held.release();
        const int64_t ts = ev.update.exchangeTsNs.raw();
        if (ev.update.type == BookUpdateType::TOP_OF_BOOK)
        {
          const auto side = [](const auto& levels) -> std::optional<std::pair<double, double>>
          {
            if (levels.empty())
            {
              return std::nullopt;
            }
            return std::make_pair(levels.front().price.toDouble(),
                                  levels.front().quantity.toDouble());
          };
          ++books;
          runner.on_book_top(ev.update.symbol, side(ev.update.bids), side(ev.update.asks), ts);
          return;
        }
        if (ev.update.type != BookUpdateType::SNAPSHOT)
        {
          // A live runner takes whole books and tops; the simulator has
          // applied the delta, the strategies do not see it.
          ++deltas;
          return;
        }
        std::vector<double> bp, bq, ap, aq;
        for (const auto& l : ev.update.bids)
        {
          bp.push_back(l.price.toDouble());
          bq.push_back(l.quantity.toDouble());
        }
        for (const auto& l : ev.update.asks)
        {
          ap.push_back(l.price.toDouble());
          aq.push_back(l.quantity.toDouble());
        }
        ++books;
        runner.on_book_snapshot(ev.update.symbol, bp, bq, ap, aq, ts);
      }
    } feed{*this, held};

    SimulatedExecutor& executor = sim.executor();
    BacktestRunner driver{BacktestConfig{}};
    driver.setSymbolRegistry(executor.symbolRegistry());
    driver.setSimulatedExecutor(&executor, &sim.clock());
    driver.addMarketDataSubscriber(&feed);
    // The driver's order-event callback sits in the simulator until here.
    struct Detach
    {
      SimulatedExecutor& executor;
      Held& held;
      ~Detach()
      {
        executor.setOrderEventCallback({});
        try
        {
          held.release();
        }
        catch (...)
        {
        }
      }
    } detach{executor, held};

    driver.run(reader);

    const BacktestState state = driver.state();
    py::dict out;
    out["events"] = state.eventCount;
    out["trades"] = feed.trades;
    out["books"] = feed.books;
    out["book_deltas_not_delivered"] = feed.deltas;
    out["skipped_option_quotes"] = state.optionQuoteCount;
    out["skipped_pool_states"] = state.poolStateCount;
    return out;
  }

  // One order event a venue reported, in the words a strategy's on_fill and
  // on_order_update read them back in.
  void on_order_event(uint64_t order_id, uint32_t symbol, const std::string& side,
                      const std::string& status, const std::string& order_type, double fill_qty,
                      double fill_price, int64_t ts_ns, const std::string& reject_reason,
                      bool is_maker)
  {
    LrvxOrderEventData ev{};
    ev.order_id = order_id;
    ev.symbol_id = symbol;
    ev.side = parseSide(side) == Side::BUY ? 0 : 1;
    if (!lrvx::capi::orderTypeCodeFromName(order_type, &ev.order_type))
    {
      throw py::value_error("Runner.on_order_event: " +
                            lrvx::capi::unknownOrderTypeMessage(order_type));
    }
    if (!lrvx::capi::orderEventStatusCodeFromName(status, &ev.status))
    {
      throw py::value_error("Runner.on_order_event: " +
                            lrvx::capi::unknownOrderEventStatusMessage(status));
    }
    ev.fill_qty_raw = Quantity::fromDouble(fill_qty).raw();
    ev.fill_price_raw = Price::fromDouble(fill_price).raw();
    ev.exchange_ts_ns = ts_ns;
    ev.reject_reason = reject_reason.empty() ? nullptr : reject_reason.c_str();
    ev.is_maker = is_maker ? 1 : 0;
    const bool fill = status == "FILLED" || status == "PARTIALLY_FILLED";
    if (fill)
    {
      ev.last_fill_at_ns = ts_ns;
    }
    if (_live)
    {
      _live->on_order_event(ev);
      return;
    }
    lrvx_clear_last_error();
    _sync->on_order_event(ev);
    if (lrvx_last_error_code() != 0)
    {
      throw std::runtime_error(std::string("Runner.on_order_event: ") + lrvx_last_error_message());
    }
  }

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

  void on_book_top(uint32_t symbol, const std::optional<std::pair<double, double>>& bid,
                   const std::optional<std::pair<double, double>>& ask, int64_t ts_ns)
  {
    if (_live)
    {
      _live->publish_book_top(symbol, bid, ask, ts_ns);
    }
    else
    {
      _sync->on_book_top(symbol, bid, ask, ts_ns);
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

// slippage_model as the Python keyword spells it.
inline lrvx::SlippageModel pySlippageModel(const std::string& name)
{
  if (const auto m = lrvx::slippageModelFromName(name))
  {
    return *m;
  }
  throw lrvx::LrvxError("E_VAL_002", "BacktestRunner: slippage_model must be one of " +
                                         lrvx::slippageModelNameList() + "; got '" + name +
                                         "'");
}

class PyBacktestRunner
{
 public:
  PyBacktestRunner(SymbolRegistry* reg, double fee_rate, double initial_capital,
                   lrvx::RunnerFillSettings fill = {}, py::object latencyOwner = py::none())
      : _reg(reg), _fill(std::move(fill)), _latencyOwner(std::move(latencyOwner))
  {
    BacktestConfig cfg{};
    cfg.feeRate = fee_rate;
    cfg.initialCapital = initial_capital;
    cfg.usePercentageFee = true;
    try
    {
      _fill.applyTo(cfg);
    }
    catch (const std::invalid_argument& e)
    {
      throw lrvx::LrvxError("E_VAL_002", std::string("BacktestRunner: ") + e.what());
    }
    _runner = std::make_unique<BacktestRunner>(cfg);
    _runner->setSymbolRegistry(reg);
    // The reason for a drop goes where a live Runner leaves it.
    _runner->setSignalDropHandler(&lrvxCapiNoteBacktestDrop);
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
      refuseExternalExecutor("set_executor");
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
    refuseExternalExecutor("set_venue_stack");
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
    refuseWithoutDepth("run_csv");
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
    return withSkippedRecords(statsToDict(std::move(result)));
  }

  // The tape aggregated into time bars inside the call and replayed with
  // run_bars' rules. The trades stay on the C++ side: the reader streams them
  // into the aggregator, and only closed bars reach the strategy.
  py::object run_tape_bars(const std::string& path, double interval_seconds,
                           const std::string& symbol = "")
  {
    if (!_host)
    {
      throw lrvx::LrvxError(
          "E_RUN_001",
          "BacktestRunner.run_tape_bars() called before set_strategy(). "
          "Attach a strategy with set_strategy(MyStrategy()) first.");
    }
    // Converted the way aggregate_time_bars converts it, so the same
    // interval_seconds lands on the same buckets.
    const bool finite = std::isfinite(interval_seconds);
    const auto interval = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::duration<double>(finite ? interval_seconds : 0.0));
    if (!finite || interval.count() <= 0)
    {
      throw lrvx::LrvxError(
          "E_VAL_002",
          "BacktestRunner.run_tape_bars: interval_seconds must be a finite number of seconds "
          "of at least one nanosecond, got " +
              std::to_string(interval_seconds));
    }
    refuseWithoutDepth("run_tape_bars");
    std::optional<SymbolId> sid;
    if (!symbol.empty())
    {
      sid = resolveSymbol(symbol);
    }
    BacktestResult result;
    try
    {
      result = _runner->runTapeBars(path, interval, sid);
    }
    catch (const std::invalid_argument& e)
    {
      if (sid)
      {
        throw lrvx::LrvxError("E_VAL_002",
                              "BacktestRunner.run_tape_bars: the tape has no trade for symbol '" +
                                  symbol + "' (id " + std::to_string(*sid) +
                                  "); the registry id has to be the tape's symbol id");
      }
      throw lrvx::LrvxError("E_VAL_002", std::string(e.what()) + " as symbol=<name>");
    }
    return statsToDict(std::move(result));
  }

  // Bars for the strategy, the tape's records for the executor: on_bar sees
  // the bars run_tape_bars builds, and the orders it sends are matched by
  // run_tape's rules from the first trade at or after the bar's end.
  py::object run_tape_bars_on_ticks(const std::string& path, double interval_seconds,
                                    const std::string& symbol = "")
  {
    if (!_host)
    {
      throw lrvx::LrvxError(
          "E_RUN_001",
          "BacktestRunner.run_tape_bars_on_ticks() called before set_strategy(). "
          "Attach a strategy with set_strategy(MyStrategy()) first.");
    }
    const bool finite = std::isfinite(interval_seconds);
    const auto interval = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::duration<double>(finite ? interval_seconds : 0.0));
    if (!finite || interval.count() <= 0)
    {
      throw lrvx::LrvxError(
          "E_VAL_002",
          "BacktestRunner.run_tape_bars_on_ticks: interval_seconds must be a finite number of "
          "seconds of at least one nanosecond, got " +
              std::to_string(interval_seconds));
    }
    std::optional<SymbolId> sid;
    if (!symbol.empty())
    {
      sid = resolveSymbol(symbol);
    }
    BacktestResult result;
    try
    {
      result = _runner->runTapeBarsOnTicks(path, interval, sid);
    }
    catch (const std::invalid_argument& e)
    {
      if (sid)
      {
        throw lrvx::LrvxError("E_VAL_002",
                              "BacktestRunner.run_tape_bars_on_ticks: the tape has no trade for "
                              "symbol '" +
                                  symbol + "' (id " + std::to_string(*sid) +
                                  "); the registry id has to be the tape's symbol id");
      }
      throw lrvx::LrvxError("E_VAL_002", std::string(e.what()) + " as symbol=<name>");
    }
    return withSkippedRecords(statsToDict(std::move(result)));
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
    return withSkippedRecords(statsToDict(std::move(result)));
  }

 private:
  // A tape can carry option-quote and pool-state records, which no strategy
  // callback or the simulated executor consumes. They are skipped, never
  // silently: the stats of a tape run say how many of each it read.
  py::object withSkippedRecords(py::object stats)
  {
    const lrvx::BacktestState st = _runner->state();
    py::dict d = stats.cast<py::dict>();
    d["skipped_option_quotes"] = st.optionQuoteCount;
    d["skipped_pool_states"] = st.poolStateCount;
    return d;
  }

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
    refuseWithoutDepth("run_ohlcv");
    auto id = resolveRunSymbol(symbol);
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
    refuseWithoutDepth("run_bars");
    auto id = resolveRunSymbol(symbol);
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

 public:
  // ── Live session ────────────────────────────────────────────────────
  //
  // The same runner, fed one market event at a time by the caller instead
  // of by a replay. Every event goes where a tick replay sends it: the
  // runner's clock is moved to the event's time, the built-in simulator
  // sees it first, then the strategy. A signal the strategy emits reaches
  // BacktestRunner::onSignal, so it is routed, gated and turned into an
  // order exactly as in a backtest (every order type the simulator takes),
  // and every order event comes back through the runner's own listener
  // chain: the position tracker first, then the attached listeners, then
  // the strategy's on_fill / on_order_update -- the same events, fields and
  // order as a run_tape over the same trades, and the same ctx.position.
  // The registry's ticks are applied as in a backtest. PaperBroker is built
  // on this.
  void begin_session(py::object on_signal)
  {
    if (!_host)
    {
      throw lrvx::LrvxError("E_RUN_001",
                            "BacktestRunner.begin_session() called before set_strategy().");
    }
    if (_sessionOpen)
    {
      throw lrvx::LrvxError("E_RUN_001", "BacktestRunner.begin_session(): a session is open.");
    }
    if (_runner->customExecutor() != nullptr || _runner->simulatedExecutorOverride() != nullptr)
    {
      throw lrvx::LrvxError(
          "E_VAL_002",
          "BacktestRunner.begin_session(): a session drives the built-in simulator; detach "
          "set_executor / set_venue_stack first.");
    }
    if (_sessionUsed || _lastResult.has_value())
    {
      throw lrvx::LrvxError(
          "E_RUN_001",
          "BacktestRunner.begin_session(): this runner already ran; a session starts from a "
          "fresh runner, so its fills and positions are its own.");
    }
    _sessionTee.runner = _runner.get();
    _sessionTee.callback = std::move(on_signal);
    // After set_strategy made the runner the bridge's signal handler: the
    // tee hands every signal to the runner first, then to the callback.
    _host->bridge->setSignalHandler(&_sessionTee);
    _sessionOpen = true;
    _sessionUsed = true;
    _host->bridge->start();
  }

  void session_trade(uint32_t symbol, double price, double qty, bool is_buy, int64_t ts_ns)
  {
    requireSession("session_trade");
    const UnixNanos ts = UnixNanos::fromRaw(ts_ns);
    _runner->clock().advanceTo(ts);

    TradeEvent ev;
    ev.trade.symbol = symbol;
    ev.trade.price = Price::fromDouble(price);
    ev.trade.quantity = Quantity::fromDouble(qty);
    ev.trade.isBuy = is_buy;
    ev.trade.exchangeTsNs = ts;
    if (auto info = _reg->getSymbolInfo(symbol))
    {
      ev.trade.instrument = info->type;
    }
    ev.exchangeMsgTsNs = ts;

    _runner->executor().onTrade(symbol, ev.trade.price, ev.trade.quantity, is_buy);
    _host->bridge->onTrade(ev);
  }

  void session_book_snapshot(uint32_t symbol, const std::vector<std::pair<double, double>>& bids,
                             const std::vector<std::pair<double, double>>& asks, int64_t ts_ns)
  {
    requireSession("session_book_snapshot");
    const UnixNanos ts = UnixNanos::fromRaw(ts_ns);
    _runner->clock().advanceTo(ts);

    std::pmr::monotonic_buffer_resource pool(4096);
    BookUpdateEvent ev(&pool);
    ev.update.symbol = symbol;
    ev.update.type = BookUpdateType::SNAPSHOT;
    ev.update.exchangeTsNs = ts;
    if (auto info = _reg->getSymbolInfo(symbol))
    {
      ev.update.instrument = info->type;
    }
    ev.update.bids.reserve(bids.size());
    ev.update.asks.reserve(asks.size());
    for (const auto& [p, q] : bids)
    {
      ev.update.bids.emplace_back(Price::fromDouble(p), Quantity::fromDouble(q));
    }
    for (const auto& [p, q] : asks)
    {
      ev.update.asks.emplace_back(Price::fromDouble(p), Quantity::fromDouble(q));
    }
    _runner->executor().onBookUpdate(symbol, ev.update.bids, ev.update.asks);
    _host->bridge->onBookUpdate(ev);
  }

  // Ends the session: the strategy's on_stop, the signal handler back to the
  // runner, and the run's stats in the shape run_tape returns (equity_curve()
  // and trades() then read this session).
  py::object end_session()
  {
    requireSession("end_session");
    _host->bridge->stop();
    _host->bridge->setSignalHandler(_runner.get());
    _sessionOpen = false;
    _sessionTee.callback = py::none();
    return statsToDict(_runner->result());
  }

  bool session_open() const noexcept { return _sessionOpen; }

  // The built-in simulator's fills so far, in the shapes
  // SimulatedExecutor.fills() / fills_list() return.
  py::array_t<PyFill> session_fills() const
  {
    const auto& f = _runner->executor().fills();
    py::array_t<PyFill> result(f.size());
    auto* out = result.mutable_data();
    for (size_t i = 0; i < f.size(); ++i)
    {
      out[i] = fillToPyFill(f[i]);
    }
    return result;
  }

  py::list session_fills_list() const
  {
    py::list result;
    for (const auto& f : _runner->executor().fills())
    {
      py::dict d;
      d["order_id"] = f.orderId;
      d["symbol"] = f.symbol;
      d["side"] = (f.side == Side::BUY) ? "buy" : "sell";
      d["price"] = f.price.toDouble();
      d["quantity"] = f.quantity.toDouble();
      d["timestamp_ns"] = f.timestampNs.raw();
      d["is_maker"] = f.isMaker;
      result.append(d);
    }
    return result;
  }

  // Per-symbol slippage on the built-in simulator, the counterpart of
  // SimulatedExecutor.set_symbol_slippage. The default model is the
  // constructor's slippage_model.
  void set_symbol_slippage(uint32_t symbol, const std::string& model, int32_t ticks,
                           double tick_size, double bps, double impact)
  {
    _runner->executor().setSymbolSlippage(
        symbol, makePyProfile("BacktestRunner.set_symbol_slippage", model, ticks, tick_size,
                              bps, impact));
  }

 private:
  // Forwards every signal to the runner, then hands a copy to the session's
  // Python callback. The runner routes first, so by the time the callback
  // sees a signal the simulator has already taken the order.
  struct SessionSignalTee final : public lrvx::ISignalHandler
  {
    BacktestRunner* runner{nullptr};
    py::object callback = py::none();

    void onSignal(const Signal& sig) override
    {
      runner->onSignal(sig);
      if (!callback.is_none())
      {
        callback(pySignalFromCxx(sig));
      }
    }
  };

  void requireSession(const char* method) const
  {
    if (!_sessionOpen)
    {
      throw lrvx::LrvxError("E_RUN_001", std::string("BacktestRunner.") + method +
                                             "() called outside begin_session() / "
                                             "end_session().");
    }
  }

  SessionSignalTee _sessionTee;
  bool _sessionOpen{false};
  bool _sessionUsed{false};

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

  lrvx::RunnerFillSettings _fill;
  // Keeps a Python-defined LatencyModel subclass alive for as long as the
  // runner's config holds the C++ side of it.
  py::object _latencyOwner;

  // set_executor / set_venue_stack route orders to an executor this runner
  // did not configure, so slippage and latency given to the constructor would
  // silently stop applying. Refuse the combination instead.
  void refuseExternalExecutor(const char* method) const
  {
    if (_fill.customized())
    {
      throw lrvx::LrvxError(
          "E_VAL_002",
          std::string("BacktestRunner.") + method +
              ": this runner was built with slippage or latency settings, which apply to the "
              "built-in executor only; the attached executor would not use them. Configure "
              "the external executor itself and build the runner without those keywords.");
    }
  }

  // VOLUME_IMPACT divides by the visible level size. Bars and trade-only
  // replays carry no book, so the model would produce no slippage at all.
  void refuseWithoutDepth(const char* method) const
  {
    if (_fill.slippageModel == SlippageModel::VOLUME_IMPACT)
    {
      throw lrvx::LrvxError(
          "E_VAL_002",
          std::string("BacktestRunner.") + method +
              ": slippage_model='volume_impact' needs book depth, and this replay has none "
              "(bars and trade-only data report no level sizes), so it would add no slippage. "
              "Use fixed_ticks or fixed_bps here, or volume_impact with run_tape on a tape "
              "that records the book.");
    }
  }

  // The symbol run_ohlcv and run_bars replay for: the one named, or with no
  // name the registry's first symbol, as lrvx_backtest_runner_run_ohlcv and
  // lrvx_backtest_runner_run_bars choose it.
  uint32_t resolveRunSymbol(const std::string& sym)
  {
    if (!sym.empty())
    {
      return resolveSymbol(sym);
    }
    const auto all = _reg->getAllSymbols();
    return all.empty() ? 0 : all.front().id;
  }

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
  // The declared quantity step; None when the symbol was registered without.
  std::optional<double> lot_size;

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
      .def_readonly("lot_size", &PySymbol::lot_size, "The quantity step the symbol was registered with, or None.")
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
      // tick_size is the venue's price increment; a backtest refuses orders
      // priced off it. Left out, the symbol gets a 0.01 tick for its book and
      // order prices are not checked. lot_size is the venue's quantity step;
      // a backtest refuses an order whose quantity is not a whole number of
      // it. Left out, quantities are not checked.
      .def("add_symbol", [](SymbolRegistry& reg, const std::string& exchange, const std::string& symbol, std::optional<double> tick_size, std::optional<double> lot_size) -> PySymbol
           {
             lrvx::SymbolInfo info;
             info.exchange  = exchange;
             info.symbol    = symbol;
             if (tick_size)
             {
               info.tickSize = Price::fromDouble(*tick_size);
               info.tickSizeKnown = true;
             }
             if (lot_size)
             {
               info.lotSize = Quantity::fromDouble(*lot_size);
               if (info.lotSize.raw() <= 0)
               {
                 throw py::value_error("add_symbol: lot_size must be a positive quantity, got " + std::to_string(*lot_size));
               }
             }
             uint32_t id = static_cast<uint32_t>(reg.registerSymbol(info));
             return PySymbol{id, exchange, symbol, info.tickSize.toDouble(), lot_size ? std::optional<double>(info.lotSize.toDouble()) : std::nullopt}; }, py::arg("exchange"), py::arg("symbol"), py::arg("tick_size") = py::none(), py::arg("lot_size") = py::none(), "Register a symbol. tick_size is the venue's price increment and lot_size its quantity step; a backtest rejects an order priced off a given tick (price_off_tick) or sized off a given lot (quantity_off_lot). Left out, that side of the order is not checked.")
      .def("tick_size", [](const SymbolRegistry& reg, uint32_t symbol) -> double
           {
            const auto info = reg.getSymbolInfo(symbol);
            if (!info)
            {
              throw py::value_error("tick_size: symbol id " + std::to_string(symbol) + " is not registered");
            }
            return info->tickSize.toDouble(); }, py::arg("symbol"), "The price tick the entry holds now: the one given to add_symbol, one a connector read from the venue and applied since, or the 0.01 a symbol registered without a tick gets for its book. See tick_size_declared.")
      .def("tick_size_declared", [](const SymbolRegistry& reg, uint32_t symbol) -> bool
           {
            const auto info = reg.getSymbolInfo(symbol);
            if (!info)
            {
              throw py::value_error("tick_size_declared: symbol id " + std::to_string(symbol) + " is not registered");
            }
            return orderTickRaw(*info) != 0; }, py::arg("symbol"), "True when order prices on the symbol are checked against tick_size: a tick was given at registration or applied from the venue.")
      .def("lot_size", [](const SymbolRegistry& reg, uint32_t symbol) -> std::optional<double>
           {
            const auto info = reg.getSymbolInfo(symbol);
            if (!info)
            {
              throw py::value_error("lot_size: symbol id " + std::to_string(symbol) + " is not registered");
            }
            const int64_t lot = orderLotRaw(*info);
            if (lot == 0)
            {
              return std::nullopt;
            }
            return Quantity::fromRaw(lot).toDouble(); }, py::arg("symbol"), "The quantity step the entry holds now (given to add_symbol or applied from the venue), or None when it declares none and quantities are not checked.")
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
      .def_readwrite("last_update_ns", &PySymbolCtx::last_update_ns)
      .def_readwrite("last_touch_ns", &PySymbolCtx::last_touch_ns)
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
      .def("on_touch", &PyStrategyBase::on_touch, py::arg("ctx"))
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
      .def("on_book_top", &lrvx::PyMarketDataRecorderHook::on_book_top,
           "A top-of-book frame: bid / ask are (price, qty), or None for an "
           "empty side. Called only when a subclass defines it; snapshots and "
           "deltas keep going to on_book_update.",
           py::arg("symbol"), py::arg("bid"), py::arg("ask"), py::arg("ts_ns"))
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
      .def_readwrite("trailing_bps", &lrvx::PyOrder::trailing_bps,
                     "Callback rate of a percent trailing stop (trailing_stop_percent), in "
                     "basis points; 0 for a trailing stop with a fixed trailing_offset and for "
                     "every other order.")
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
      .def("order_closed", &PyRunner::order_closed, py::arg("order_id"),
           "Tell the runner an order it sent to the executor has reached a terminal state "
           "(filled, cancelled by the venue, rejected, expired). With an executor attached the "
           "runner keeps every order that can rest so that a modify can be sent as that order "
           "with its price and quantity changed, and nothing tells it of a fill: call this from "
           "wherever the executor learns the order is finished and the entry is dropped. An id "
           "that is not on record is ignored; market, IOC and FOK orders are never kept. A "
           "modify that names an id not on record is not sent to the executor, and "
           "lrvx.last_error() then says why, ending in [unknown_order].")
      .def("replay_tape", [](PyRunner& r, const std::string& path, PySimulatedExecutor& simulator, py::object tape_symbol_id, py::object symbol, py::object lock, py::object after_event)
           { return r.replay_tape(
                 path, simulator,
                 tape_symbol_id.is_none() ? std::nullopt
                                          : std::optional<uint32_t>(tape_symbol_id.cast<uint32_t>()),
                 symbol.is_none() ? std::nullopt : std::optional<uint32_t>(symId(symbol)),
                 std::move(lock), std::move(after_event)); }, py::arg("path"), py::arg("simulator"), py::kw_only(), py::arg("tape_symbol_id") = py::none(), py::arg("symbol") = py::none(), py::arg("lock") = py::none(), py::arg("after_event") = py::none(),
           "Replay a `.lrvx` tape into this runner's strategies with `simulator` (a "
           "SimulatedExecutor) standing in for the venue, through the loop BacktestRunner.run_tape "
           "uses: every trade, book snapshot, book delta and top-of-book frame, in the tape's time "
           "order, moves the simulator's clock and reaches the simulator before the strategies, "
           "so an order resting in the simulator fills from the books as in a backtest of the "
           "same tape. The simulator is reset first, as at the start of a backtest run. With "
           "tape_symbol_id and symbol, only that tape symbol is replayed and its events are "
           "delivered under the registered `symbol`. `lock` (anything with acquire / release) is "
           "held while the simulator is shown an event and released before the strategies see "
           "it; `after_event()` is called after each event. The strategies receive trades, book "
           "snapshots and tops; a live runner has no delta entry, so deltas reach the simulator "
           "only. Returns a dict: events, trades, books, book_deltas_not_delivered, "
           "skipped_option_quotes, skipped_pool_states.")
      .def("on_order_event", [](PyRunner& r, uint64_t order_id, py::object sym, const std::string& side, const std::string& status, const std::string& order_type, double fill_qty, double fill_price, int64_t ts_ns, const std::string& reject_reason, bool is_maker)
           { r.on_order_event(order_id, symId(sym), side, status, order_type, fill_qty,
                              fill_price, ts_ns, reject_reason, is_maker); }, py::arg("order_id"), py::arg("symbol"), py::arg("side"), py::arg("status"), py::arg("order_type") = "limit", py::arg("fill_qty") = 0.0, py::arg("fill_price") = 0.0, py::arg("ts_ns") = 0, py::arg("reject_reason") = "", py::arg("is_maker") = false,
           "Report one order event a venue sent (a broker's order update or fill) to the "
           "strategy that issued the order, through the callbacks a backtest uses: "
           "PARTIALLY_FILLED and FILLED reach Strategy.on_fill(ctx, event) and only on_fill, "
           "every other status reaches Strategy.on_order_update(ctx, event). Each strategy has "
           "a position of its own, as in a backtest: the fill counts in the position of the "
           "strategy that issued the order, before on_fill runs, so ctx.position inside on_fill "
           "counts it. side is 'buy' or 'sell'; status and order_type are the names "
           "OrderEventData carries ('ACCEPTED', 'CANCELED', 'limit', ...); fill_qty and "
           "fill_price are this fill's own, not the order's running totals. An order id no "
           "attached strategy issued goes to every strategy subscribed to the symbol and counts "
           "in no position. On a synchronous runner the callbacks run on the calling thread. On "
           "a threaded runner the event is queued and the callbacks run on the strategy's "
           "order-event consumer thread, after every trade, book and bar published before the "
           "report has been handed to that strategy; before start() and after stop() they run "
           "on the calling thread. ValueError for an unknown side, status or order type.")
      .def("seed_position", [](PyRunner& r, PyStrategyBase* strategy, py::object sym, double position, double avg_entry_price)
           { r.seed_position(strategy, symId(sym), position, avg_entry_price); }, py::arg("strategy"), py::arg("symbol"), py::arg("position"), py::arg("avg_entry_price") = 0.0,
           "Set what `strategy` holds in `symbol` before any fill is reported: the position an "
           "account already has when a host starts or restarts, which no on_order_event will "
           "tell of. `position` is signed (negative for a short) and `avg_entry_price` its "
           "average entry (0.0 when unknown). The strategy then reads it from self.position() "
           "and ctx.position, and the fills reported afterwards add to it. Positions are per "
           "strategy, so name the one the position belongs to. ValueError for a strategy that "
           "is not attached to this runner and for one that already holds a position in the "
           "symbol: a position is seeded once. 0 is accepted and changes nothing.")
      .def("drain", &PyRunner::drain,
           "Return once every strategy has been handed everything published so far (trades, "
           "books, bars, order events) and whatever those callbacks published in turn. A "
           "threaded runner's barrier; a synchronous runner has nothing queued and returns at "
           "once, as does a runner that is not started. Never call it from a strategy callback.")
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
      .def("on_book_top", [](PyRunner& r, py::object sym, const std::optional<std::pair<double, double>>& bid, const std::optional<std::pair<double, double>>& ask, int64_t ts_ns)
           { r.on_book_top(symId(sym), bid, ask, ts_ns); }, py::arg("symbol"), py::arg("bid"), py::arg("ask"), py::arg("ts_ns") = 0, "Inject a touch-only frame. bid / ask are (price, qty) tuples; None means that side of the book is empty")
      .def("on_bar", [](PyRunner& r, py::object sym, double o, double h, double l, double c, double v, double bv, int64_t start_ns, int64_t end_ns, uint8_t bar_type, uint64_t bar_type_param, uint8_t close_reason)
           { r.on_bar(symId(sym), o, h, l, c, v, bv, start_ns, end_ns,
                      bar_type, bar_type_param, close_reason); }, py::arg("symbol"), py::arg("open"), py::arg("high"), py::arg("low"), py::arg("close"), py::arg("volume") = 0.0, py::arg("buy_volume") = 0.0, py::arg("start_time_ns") = 0, py::arg("end_time_ns") = 0, py::arg("bar_type") = 0, py::arg("bar_type_param") = 0, py::arg("close_reason") = 0);

  py::class_<PyBacktestRunner>(m, "BacktestRunner")
      .def(py::init(
               [](SymbolRegistry* reg, double fee_rate, double initial_capital,
                  const std::string& slippage_model, int32_t slippage_ticks,
                  double slippage_tick_size, double slippage_bps, double slippage_impact_coeff,
                  int64_t submit_ack_latency_ns, int64_t submit_ack_jitter_ns,
                  int64_t cancel_ack_latency_ns, int64_t cancel_ack_jitter_ns,
                  uint64_t cancel_ack_seed, int64_t replace_ack_latency_ns,
                  int64_t replace_ack_jitter_ns, py::object latency)
               {
                 lrvx::RunnerFillSettings fill;
                 fill.slippageModel = pySlippageModel(slippage_model);
                 fill.slippageTicks = slippage_ticks;
                 fill.slippageTickSize = slippage_tick_size;
                 fill.slippageBps = slippage_bps;
                 fill.slippageImpactCoeff = slippage_impact_coeff;
                 fill.submitAckLatencyNs = submit_ack_latency_ns;
                 fill.submitAckJitterNs = submit_ack_jitter_ns;
                 fill.cancelAckLatencyNs = cancel_ack_latency_ns;
                 fill.cancelAckJitterNs = cancel_ack_jitter_ns;
                 fill.cancelAckSeed = cancel_ack_seed;
                 fill.replaceAckLatencyNs = replace_ack_latency_ns;
                 fill.replaceAckJitterNs = replace_ack_jitter_ns;
                 if (!latency.is_none())
                 {
                   if (!py::isinstance<lrvx::LatencyModel>(latency))
                   {
                     throw py::type_error(
                         "BacktestRunner: latency must be a lrvx.LatencyModel (ConstantLatency, "
                         "GaussianLatency, ExponentialLatency, EmpiricalLatency or a subclass) "
                         "or None");
                   }
                   fill.latency = latency.cast<std::shared_ptr<lrvx::LatencyModel>>();
                 }
                 return std::make_unique<PyBacktestRunner>(reg, fee_rate, initial_capital,
                                                           std::move(fill), latency);
               }),
           py::arg("registry"),
           py::arg("fee_rate") = 0.0004,
           py::arg("initial_capital") = 100000.0,
           py::kw_only(),
           py::arg("slippage_model") = "none",
           py::arg("slippage_ticks") = 0,
           py::arg("slippage_tick_size") = 0.0,
           py::arg("slippage_bps") = 0.0,
           py::arg("slippage_impact_coeff") = 0.0,
           py::arg("submit_ack_latency_ns") = 0,
           py::arg("submit_ack_jitter_ns") = 0,
           py::arg("cancel_ack_latency_ns") = 0,
           py::arg("cancel_ack_jitter_ns") = 0,
           py::arg("cancel_ack_seed") = 42,
           py::arg("replace_ack_latency_ns") = 0,
           py::arg("replace_ack_jitter_ns") = 0,
           py::arg("latency") = py::none(),
           py::keep_alive<1, 2>(),
           "Backtest runner over the built-in SimulatedExecutor. The keyword-only "
           "arguments are BacktestConfig's executor fields under their snake_case "
           "names. slippage_model: none | fixed_ticks (slippage_ticks x "
           "slippage_tick_size) | fixed_bps (slippage_bps) | volume_impact "
           "(slippage_impact_coeff x order/level size; needs book depth), each "
           "against the taker; a setting the chosen model does not read is refused. "
           "submit/cancel/replace_ack_latency_ns and *_jitter_ns: venue ack delay, "
           "drawn uniformly from [latency - jitter, latency + jitter] floored at 0, "
           "with cancel_ack_seed seeding the draw. latency: a lrvx.LatencyModel "
           "whose order_delay() is added to the submit deferral. On bar replay the "
           "clock reads the bar's end, so any positive submit deferral moves a fill "
           "to a later bar's open (docs/how-to/backtest-with-latency.md). The "
           "defaults reproduce the instant, slippage-free run.")
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
           "Returns the same stats dict shape as run_csv / run_bars, plus "
           "skipped_option_quotes and skipped_pool_states: option-quote and "
           "pool-state records on the tape are read but reach no strategy "
           "callback and do not move the simulated executor, and these count them.")
      .def("run_tape_bars", &PyBacktestRunner::run_tape_bars, py::arg("path"),
           py::kw_only(), py::arg("interval_seconds"), py::arg("symbol") = "",
           "Replay a `.lrvx` tape as time bars of interval_seconds: the trades "
           "are aggregated while the tape streams (wall-clock aligned, the last "
           "still-open bar dropped, exactly as aggregate_time_bars does) and every "
           "closed bar goes to Strategy.on_bar with run_bars' fill rules -- an order "
           "sent from on_bar fills at the next bar's open. on_trade does not fire. "
           "symbol names the registry symbol to replay and is required when the "
           "tape holds more than one; its registry id must be the tape's symbol "
           "id. Same result as read_trades -> aggregate_time_bars -> run_bars for "
           "the same interval, without the trade array ever reaching Python.")
      .def("run_tape_bars_on_ticks", &PyBacktestRunner::run_tape_bars_on_ticks,
           py::arg("path"), py::kw_only(), py::arg("interval_seconds"),
           py::arg("symbol") = "",
           "Decide on bars, fill on ticks. Strategy.on_bar receives the same "
           "bars run_tape_bars builds from the tape, while the simulated "
           "executor is fed every trade and book record as run_tape feeds it. "
           "Bar n reaches on_bar when the first trade stamped at or after its "
           "end arrives, before that trade reaches the executor; an order sent "
           "from on_bar is submitted at the bar's end and can first match "
           "against that trade (with a submit latency L: the first record at or "
           "after end + L). Working stops and limits fill on the prints inside "
           "the next bars, a stop never better than its trigger. on_trade and "
           "on_book_update do not fire. One symbol per run, as run_tape_bars. "
           "Stats shape as run_tape, including skipped_option_quotes and "
           "skipped_pool_states.")
      .def("run_tapes", &PyBacktestRunner::run_tapes, py::arg("paths"),
           "Run a backtest off N `.lrvx` tapes merged on read. "
           "Symbols are rekeyed by (metadata.exchange, name) so two "
           "captures of the same venue/symbol collapse, and two "
           "different venues stay distinct. `run_tapes([t])` is "
           "equivalent to `run_tape(t)`. Stats shape mirrors run_tape, "
           "including skipped_option_quotes and skipped_pool_states.")
      .def("run_ohlcv", &PyBacktestRunner::run_ohlcv,
           py::arg("ts"), py::arg("close"), py::arg("symbol") = "")
      .def("run_bars", &PyBacktestRunner::run_bars,
           py::arg("start_time_ns"), py::arg("end_time_ns"),
           py::arg("open"), py::arg("high"), py::arg("low"), py::arg("close"),
           py::arg("volume"),
           py::arg("symbol") = "",
           py::arg("bar_type") = 0,
           py::arg("bar_type_param") = 0)
      .def("begin_session", &PyBacktestRunner::begin_session,
           py::arg("on_signal") = py::none(),
           "Open a live session: market data is then pushed one event at a time with "
           "session_trade / session_book_snapshot instead of replayed, and goes exactly where "
           "a tick replay sends it -- clock, built-in simulator, strategy. Signals are routed "
           "and order events delivered (position tracker, listeners, on_fill / "
           "on_order_update) as in run_tape. on_signal, when given, receives each Signal after "
           "the runner has routed it. Needs set_strategy and a runner that has not run.")
      .def("session_trade", [](PyBacktestRunner& r, py::object sym, double price, double qty, bool is_buy, int64_t ts_ns)
           { r.session_trade(symId(sym), price, qty, is_buy, ts_ns); }, py::arg("symbol"), py::arg("price"), py::arg("qty"), py::arg("is_buy"), py::arg("ts_ns") = 0)
      .def("session_book_snapshot", [](PyBacktestRunner& r, py::object sym, const std::vector<std::pair<double, double>>& bids, const std::vector<std::pair<double, double>>& asks, int64_t ts_ns)
           { r.session_book_snapshot(symId(sym), bids, asks, ts_ns); }, py::arg("symbol"), py::arg("bid_levels"), py::arg("ask_levels"), py::arg("ts_ns") = 0, "bid_levels / ask_levels: sequences of (price, qty), best first.")
      .def("end_session", &PyBacktestRunner::end_session, "Close the session (strategy on_stop) and return its stats dict.")
      .def_property_readonly("session_open", &PyBacktestRunner::session_open)
      .def("session_fills", &PyBacktestRunner::session_fills, "The built-in simulator's fills, as a numpy structured array.")
      .def("session_fills_list", &PyBacktestRunner::session_fills_list, "The built-in simulator's fills, as a list of dicts.")
      .def("set_symbol_slippage", [](PyBacktestRunner& r, py::object sym, const std::string& model, int32_t ticks, double tick_size, double bps, double impact)
           { r.set_symbol_slippage(symId(sym), model, ticks, tick_size, bps, impact); }, py::arg("symbol"), py::arg("model"), py::arg("ticks") = 0, py::arg("tick_size") = 0.0, py::arg("bps") = 0.0, py::arg("impact") = 0.0,
           "Per-symbol slippage on the built-in simulator; the constructor's slippage_model "
           "is the default for every other symbol.")
      .def("equity_curve", &PyBacktestRunner::equity_curve,
           "Return the equity curve from the most recent run as a dict of "
           "numpy arrays (timestamp_ns, equity, drawdown_pct).")
      .def("trades", &PyBacktestRunner::trades,
           "Return closed trades from the most recent run as a dict of numpy "
           "arrays (symbol, side, entry_price, exit_price, quantity, pnl, "
           "fee, entry_time_ns, exit_time_ns).");
}
