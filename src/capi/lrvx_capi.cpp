/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "lrvx/capi/lrvx_capi.h"
#include "lrvx/capi/bridge_strategy.h"
#include "lrvx/engine/symbol_registry.h"
#include "lrvx/log/abstract_logger.h"
#include "lrvx/log/log_stream.h"
#include "tracker_ownership.h"

#include "lrvx/aggregator/bar.h"
#include "lrvx/aggregator/policies/tick_bar_policy.h"
#include "lrvx/aggregator/policies/time_bar_policy.h"
#include "lrvx/aggregator/policies/volume_bar_policy.h"
#include "lrvx/backtest/backtest_config.h"
#include "lrvx/backtest/backtest_result.h"
#include "lrvx/backtest/backtest_runner.h"
#include "lrvx/backtest/grid_search.h"
#include "lrvx/backtest/latency_model.h"
#include "lrvx/backtest/simulated_clock.h"
#include "lrvx/backtest/simulated_executor.h"
#include "lrvx/backtest/walk_forward.h"
#include "lrvx/book/events/book_update_event.h"
#include "lrvx/book/nlevel_order_book.h"
#include "lrvx/execution/algos.h"
#include "lrvx/replay/abstract_event_reader.h"
#include "lrvx/replay/aggregator.h"
#include "lrvx/replay/aggregators/bin_count.h"
#include "lrvx/replay/aggregators/book_snapshot_bin.h"
#include "lrvx/replay/aggregators/event_type_stats.h"
#include "lrvx/replay/aggregators/ohlc_bin.h"
#include "lrvx/replay/aggregators/peak.h"
#include "lrvx/replay/aggregators/quantile.h"
#include "lrvx/replay/aggregators/volume_bin.h"
#include "lrvx/replay/binary_format_v1.h"
#include "lrvx/replay/delta_book.h"
#include "lrvx/replay/ohlcv_replay_source.h"
#include "lrvx/replay/tape_diff.h"
#include "lrvx/report/heatmap_html.h"
#include "lrvx/risk/portfolio_risk.h"
#include "lrvx/run/trace_reader.h"
#include "lrvx/run/trace_recorder.h"
#include "lrvx/stats/whites_reality_check.h"

#include "lrvx/indicator/adf.h"
#include "lrvx/indicator/adx.h"
#include "lrvx/indicator/atr.h"
#include "lrvx/indicator/autocorrelation.h"
#include "lrvx/indicator/bollinger.h"
#include "lrvx/indicator/cci.h"
#include "lrvx/indicator/chop.h"
#include "lrvx/indicator/correlation.h"
#include "lrvx/indicator/cvd.h"
#include "lrvx/indicator/dema.h"
#include "lrvx/indicator/ema.h"
#include "lrvx/indicator/indicator_pipeline.h"
#include "lrvx/indicator/kama.h"
#include "lrvx/indicator/kurtosis.h"
#include "lrvx/indicator/macd.h"
#include "lrvx/indicator/obv.h"
#include "lrvx/indicator/parkinson_vol.h"
#include "lrvx/indicator/rma.h"
#include "lrvx/indicator/rogers_satchell_vol.h"
#include "lrvx/indicator/rolling_zscore.h"
#include "lrvx/indicator/rsi.h"
#include "lrvx/indicator/shannon_entropy.h"
#include "lrvx/indicator/skewness.h"
#include "lrvx/indicator/slope.h"
#include "lrvx/indicator/sma.h"
#include "lrvx/indicator/stochastic.h"
#include "lrvx/indicator/streaming_graph.h"
#include "lrvx/indicator/vwap.h"
#include "lrvx/target/future_ctc_volatility.h"
#include "lrvx/target/future_linear_slope.h"
#include "lrvx/target/future_return.h"

#include "lrvx/aggregator/bus/bar_bus.h"
#include "lrvx/aggregator/custom/footprint_bar.h"
#include "lrvx/aggregator/custom/market_profile.h"
#include "lrvx/aggregator/custom/volume_profile.h"
#include "lrvx/aggregator/policies/heikin_ashi_bar_policy.h"
#include "lrvx/aggregator/policies/range_bar_policy.h"
#include "lrvx/aggregator/policies/renko_bar_policy.h"
#include "lrvx/backtest/concentrated_liquidity_curve.h"
#include "lrvx/backtest/constant_product_curve.h"
#include "lrvx/backtest/ntoken_curve.h"
#include "lrvx/backtest/raydium_cp_curve.h"
#include "lrvx/book/bus/book_update_bus.h"
#include "lrvx/book/bus/trade_bus.h"
#include "lrvx/book/composite_book_matrix.h"
#include "lrvx/book/events/book_update_event.h"
#include "lrvx/book/events/trade_event.h"
#include "lrvx/book/l3/l3_order_book.h"
#include "lrvx/connector/amm_dex_connector.h"
#include "lrvx/execution/abstract_execution_listener.h"
#include "lrvx/execution/abstract_executor.h"
#include "lrvx/execution/exchange_capabilities.h"
#include "lrvx/execution/order.h"
#include "lrvx/execution/order_journey_tracer.h"
#include "lrvx/execution/order_tracker.h"
#include "lrvx/position/position_group.h"
#include "lrvx/position/position_tracker.h"
#include "lrvx/replay/binary_log_recorder_hook.h"
#include "lrvx/replay/merged_tape_reader.h"
#include "lrvx/replay/ops/partitioner.h"
#include "lrvx/replay/ops/segment_ops.h"
#include "lrvx/replay/ops/validator.h"
#include "lrvx/replay/pool_state_tape.h"
#include "lrvx/replay/readers/binary_log_reader.h"
#include "lrvx/replay/writers/binary_log_writer.h"
#include "lrvx/strategy/abstract_signal_handler.h"
#include "lrvx/testing/bar_dispatch_recorder.h"
#include "lrvx/util/int/i256.h"
#include "lrvx/util/int/u256.h"
#include "lrvx/util/memory/pool.h"

#include <random>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <vector>

using namespace lrvx;

// ============================================================
// C ABI entry contract
// ============================================================
//
// Every exported function opens with LRVX_CAPI_ENTER (or one of its
// variants) and closes with the matching LRVX_CAPI_LEAVE. The pair does two
// things a C caller cannot do for itself. Both are written down for callers
// in the "Calling contract" section at the top of lrvx_capi.h.
//
// A null handle returns the function's documented zero value. A foreign
// runtime holds a handle as an opaque integer, so passing one that was never
// created, or one a finaliser already zeroed, is a one-line mistake.
// Dereferencing it is undefined behaviour; returning zero is not. Before the
// contract was written down, 446 of the 523 handle-taking functions
// dereferenced without looking while their neighbours in the same layer
// quietly returned zero, and nothing said which did what.
//
// No exception leaves the boundary. Unwinding out of a frame with C linkage
// is undefined behaviour, and the friendly outcome -- std::terminate --
// takes down the host process with nothing the caller can do about it. One
// unparsable number in a user's CSV was enough to do that through the
// shipped Node binding. The firewall turns any exception into the same zero
// return and records what happened, which lrvx_last_error_code and
// lrvx_last_error_message hand back.
//
// `return {}` value-initialises whatever the function returns: zero for the
// integer and floating-point returns, NULL for handles and strings, an
// all-zero struct for the handful of by-value struct returns. Each of those
// is the value the function already documents as its failure result, so the
// guard adds a path callers already handle rather than a new one.

namespace
{

// Mirrors the codes documented on lrvx_last_error_code in lrvx_capi.h.
constexpr int kLrvxCapiOk = 0;
constexpr int kLrvxCapiErrNullHandle = 1;
constexpr int kLrvxCapiErrException = 2;

struct LrvxCapiLastError
{
  int code{kLrvxCapiOk};
  std::string message;
};

LrvxCapiLastError& lrvxCapiLastError() noexcept
{
  static thread_local LrvxCapiLastError e;
  return e;
}

// Recording an error must never itself throw out of the guard, hence the
// swallow: a bad_alloc while building the message string would otherwise
// escape the very frame the guard exists to protect.
void lrvxCapiNoteNullHandle(const char* fn) noexcept
{
  try
  {
    auto& e = lrvxCapiLastError();
    e.code = kLrvxCapiErrNullHandle;
    e.message = std::string(fn != nullptr ? fn : "?") + ": null handle";
  }
  catch (...)
  {
  }
}

void lrvxCapiNoteNullArgument(const char* fn, const char* argument) noexcept
{
  try
  {
    auto& e = lrvxCapiLastError();
    e.code = kLrvxCapiErrNullHandle;
    e.message = std::string(fn != nullptr ? fn : "?") + ": argument '" +
                (argument != nullptr ? argument : "?") + "' must not be NULL";
  }
  catch (...)
  {
  }
}

void lrvxCapiNoteException(const char* fn, const char* what) noexcept
{
  try
  {
    auto& e = lrvxCapiLastError();
    e.code = kLrvxCapiErrException;
    e.message = std::string(fn != nullptr ? fn : "?") + ": " +
                (what != nullptr ? what : "unknown exception");
  }
  catch (...)
  {
  }
}

// Accessors that reach inside a composite -- lrvx_venue_stack_account and its
// siblings -- hand back a pointer the composite still owns. A consumer cannot
// tell that apart from an owned handle, because both are void*, so a binding
// that wraps every handle it receives in a finaliser deleted the stack's
// member and the stack then deleted it again. The first delete looked healthy;
// the abort landed later, inside lrvx_venue_stack_destroy.
//
// Registering the borrowed addresses lets _destroy recognise and ignore them.
// Entries go in when the owning composite is created and come out when it is
// destroyed, so a registered address always belongs to a live object -- the
// address-reuse trap that made the portfolio risk scratch serve a dead
// session's breach does not apply here.
class LrvxBorrowedHandles
{
 public:
  static void add(const void* p)
  {
    if (p == nullptr)
    {
      return;
    }
    const std::lock_guard<std::mutex> lock(mutex());
    registry().insert(p);
  }

  static void remove(const void* p)
  {
    if (p == nullptr)
    {
      return;
    }
    const std::lock_guard<std::mutex> lock(mutex());
    registry().erase(p);
  }

  static bool contains(const void* p)
  {
    if (p == nullptr)
    {
      return false;
    }
    const std::lock_guard<std::mutex> lock(mutex());
    return registry().find(p) != registry().end();
  }

 private:
  static std::mutex& mutex()
  {
    static std::mutex m;
    return m;
  }

  static std::set<const void*>& registry()
  {
    static std::set<const void*> r;
    return r;
  }
};

}  // namespace

// Every _destroy opens with this. Destroying NULL has always been legal and
// stays legal -- a binding's finaliser runs on a half-constructed object often
// enough that making it an error would be a downgrade -- and it is not
// recorded as an error. Destroying a borrowed handle is the same no-op: the
// composite that owns the object frees it.
//
// A plain `delete` would need neither check, but several _destroy bodies
// flush or close before deleting, and those do dereference.
// clang-format off
#define LRVX_CAPI_ENTER_DESTROY(handle)                                     \
  if ((handle) == nullptr || LrvxBorrowedHandles::contains(handle))         \
  {                                                                         \
    return;                                                                 \
  }                                                                         \
  try                                                                       \
  {
// clang-format on

// clang-format off
#define LRVX_CAPI_ENTER(handle)                                             \
  if ((handle) == nullptr)                                                  \
  {                                                                         \
    lrvxCapiNoteNullHandle(__func__);                                       \
    return {};                                                              \
  }                                                                         \
  try                                                                       \
  {

#define LRVX_CAPI_ENTER_VOID(handle)                                        \
  if ((handle) == nullptr)                                                  \
  {                                                                         \
    lrvxCapiNoteNullHandle(__func__);                                       \
    return;                                                                 \
  }                                                                         \
  try                                                                       \
  {

#define LRVX_CAPI_ENTER_NOHANDLE                                            \
  try                                                                       \
  {

// A required non-handle argument: a path, a name, an output buffer. The entry
// guard covers handles only, so these say so one by one. Reported through
// lrvx_last_error_code the same way a null handle is.
#define LRVX_CAPI_REQUIRE(argument)                                         \
  if ((argument) == nullptr)                                                \
  {                                                                         \
    lrvxCapiNoteNullArgument(__func__, #argument);                          \
    return {};                                                              \
  }

#define LRVX_CAPI_REQUIRE_VOID(argument)                                    \
  if ((argument) == nullptr)                                                \
  {                                                                         \
    lrvxCapiNoteNullArgument(__func__, #argument);                          \
    return;                                                                 \
  }

#define LRVX_CAPI_LEAVE                                                     \
  }                                                                         \
  catch (const std::exception& lrvxCapiEx)                                  \
  {                                                                         \
    lrvxCapiNoteException(__func__, lrvxCapiEx.what());                     \
    return {};                                                              \
  }                                                                         \
  catch (...)                                                               \
  {                                                                         \
    lrvxCapiNoteException(__func__, nullptr);                               \
    return {};                                                              \
  }

#define LRVX_CAPI_LEAVE_VOID                                                \
  }                                                                         \
  catch (const std::exception& lrvxCapiEx)                                  \
  {                                                                         \
    lrvxCapiNoteException(__func__, lrvxCapiEx.what());                     \
    return;                                                                 \
  }                                                                         \
  catch (...)                                                               \
  {                                                                         \
    lrvxCapiNoteException(__func__, nullptr);                               \
    return;                                                                 \
  }
// clang-format on

// ============================================================
// Internal helpers
// ============================================================

static BridgeStrategy* toStrategy(LrvxStrategyHandle h)
{
  return static_cast<BridgeStrategy*>(h);
}

static SymbolRegistry* toRegistry(LrvxRegistryHandle h)
{
  return static_cast<SymbolRegistry*>(h);
}

// ============================================================
// Registry
// ============================================================

LrvxRegistryHandle lrvx_registry_create(void)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  return static_cast<LrvxRegistryHandle>(new SymbolRegistry());
  LRVX_CAPI_LEAVE;
}

void lrvx_registry_destroy(LrvxRegistryHandle registry)
{
  LRVX_CAPI_ENTER_DESTROY(registry);
  delete toRegistry(registry);
  LRVX_CAPI_LEAVE_VOID;
}

uint32_t lrvx_registry_add_symbol(LrvxRegistryHandle registry, const char* exchange,
                                  const char* name, double tick_size)
{
  LRVX_CAPI_ENTER(registry);
  LRVX_CAPI_REQUIRE(exchange);
  LRVX_CAPI_REQUIRE(name);
  auto* reg = toRegistry(registry);
  SymbolInfo info;
  info.exchange = exchange;
  info.symbol = name;
  info.tickSize = Price::fromDouble(tick_size);
  return reg->registerSymbol(info);
  LRVX_CAPI_LEAVE;
}

uint8_t lrvx_registry_get_symbol_id(LrvxRegistryHandle registry, const char* exchange,
                                    const char* name, uint32_t* id_out)
{
  LRVX_CAPI_ENTER(registry);
  LRVX_CAPI_REQUIRE(exchange);
  LRVX_CAPI_REQUIRE(name);
  LRVX_CAPI_REQUIRE(id_out);
  auto* reg = toRegistry(registry);
  auto result = reg->getSymbolId(exchange, name);
  if (result.has_value())
  {
    *id_out = result.value();
    return 1;
  }
  return 0;
  LRVX_CAPI_LEAVE;
}

uint8_t lrvx_registry_get_symbol_name(LrvxRegistryHandle registry, uint32_t symbol_id,
                                      char* exchange_out, size_t exchange_len, char* name_out,
                                      size_t name_len)
{
  LRVX_CAPI_ENTER(registry);
  LRVX_CAPI_REQUIRE(exchange_out);
  LRVX_CAPI_REQUIRE(name_out);
  // A buffer the caller declared as holding nothing gets nothing written.
  // `exchange_len - 1` is unsigned, so zero wrapped to SIZE_MAX: strncpy
  // copied with no bound and the terminator landed at buf[SIZE_MAX], which
  // wraps to buf[-1]. Measured 136 zeroed bytes from buf-65 to buf+70, and
  // the call still reported success.
  if (exchange_len == 0 || name_len == 0)
  {
    return 0;
  }
  auto* reg = toRegistry(registry);
  auto info = reg->getSymbolInfo(symbol_id);
  if (!info.has_value())
  {
    return 0;
  }
  std::strncpy(exchange_out, info->exchange.c_str(), exchange_len - 1);
  exchange_out[exchange_len - 1] = '\0';
  std::strncpy(name_out, info->symbol.c_str(), name_len - 1);
  name_out[name_len - 1] = '\0';
  return 1;
  LRVX_CAPI_LEAVE;
}

uint32_t lrvx_registry_symbol_count(LrvxRegistryHandle registry)
{
  LRVX_CAPI_ENTER(registry);
  return static_cast<uint32_t>(toRegistry(registry)->size());
  LRVX_CAPI_LEAVE;
}

// ============================================================
// Strategy lifecycle
// ============================================================

// Returns NULL if the strategy cannot be constructed. BridgeStrategy's base
// throws std::invalid_argument for a SymbolId absent from the registry -- an
// easy mistake to make from a foreign runtime, where the id is just an integer
// the caller made up. Letting that propagate out of extern "C" is undefined
// behaviour, so it becomes a NULL handle the caller can test.
LrvxStrategyHandle lrvx_strategy_create(uint32_t id, const uint32_t* symbols,
                                        uint32_t num_symbols, LrvxRegistryHandle registry,
                                        LrvxStrategyCallbacks callbacks)
{
  LRVX_CAPI_ENTER(registry);
  try
  {
    auto* reg = toRegistry(registry);
    if (reg == nullptr || (num_symbols > 0 && symbols == nullptr))
    {
      return nullptr;
    }
    std::vector<SymbolId> syms(symbols, symbols + num_symbols);
    auto* strat =
        new BridgeStrategy(static_cast<SubscriberId>(id), std::move(syms), *reg, callbacks);
    return static_cast<LrvxStrategyHandle>(strat);
  }
  catch (const std::exception&)
  {
    return nullptr;
  }
  LRVX_CAPI_LEAVE;
}

LrvxStrategyHandle lrvx_strategy_create_p(uint32_t id, const uint32_t* symbols,
                                          uint32_t num_symbols, LrvxRegistryHandle registry,
                                          const LrvxStrategyCallbacks* callbacks)
{
  LRVX_CAPI_ENTER(registry);
  LrvxStrategyCallbacks cbs = callbacks ? *callbacks : LrvxStrategyCallbacks{};
  return lrvx_strategy_create(id, symbols, num_symbols, registry, cbs);
  LRVX_CAPI_LEAVE;
}

void lrvx_strategy_destroy(LrvxStrategyHandle strategy)
{
  LRVX_CAPI_ENTER_DESTROY(strategy);
  delete toStrategy(strategy);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_strategy_replace_callbacks(LrvxStrategyHandle strategy, LrvxStrategyCallbacks callbacks)
{
  LRVX_CAPI_ENTER_VOID(strategy);
  if (!strategy)
  {
    return;
  }
  toStrategy(strategy)->replaceCallbacks(callbacks);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_strategy_replace_callbacks_p(LrvxStrategyHandle strategy,
                                       const LrvxStrategyCallbacks* callbacks)
{
  LRVX_CAPI_ENTER_VOID(strategy);
  LrvxStrategyCallbacks cbs = callbacks ? *callbacks : LrvxStrategyCallbacks{};
  lrvx_strategy_replace_callbacks(strategy, cbs);
  LRVX_CAPI_LEAVE_VOID;
}

// ============================================================
// Signal emission
// ============================================================

uint64_t lrvx_emit_market_buy(LrvxStrategyHandle s, uint32_t symbol, int64_t qty_raw)
{
  LRVX_CAPI_ENTER(s);
  return toStrategy(s)->publicEmitMarketBuy(symbol, Quantity::fromRaw(qty_raw));
  LRVX_CAPI_LEAVE;
}

uint64_t lrvx_emit_market_sell(LrvxStrategyHandle s, uint32_t symbol, int64_t qty_raw)
{
  LRVX_CAPI_ENTER(s);
  return toStrategy(s)->publicEmitMarketSell(symbol, Quantity::fromRaw(qty_raw));
  LRVX_CAPI_LEAVE;
}

uint64_t lrvx_emit_limit_buy(LrvxStrategyHandle s, uint32_t symbol, int64_t price_raw,
                             int64_t qty_raw)
{
  LRVX_CAPI_ENTER(s);
  return toStrategy(s)->publicEmitLimitBuy(symbol, Price::fromRaw(price_raw),
                                           Quantity::fromRaw(qty_raw));
  LRVX_CAPI_LEAVE;
}

uint64_t lrvx_emit_limit_sell(LrvxStrategyHandle s, uint32_t symbol, int64_t price_raw,
                              int64_t qty_raw)
{
  LRVX_CAPI_ENTER(s);
  return toStrategy(s)->publicEmitLimitSell(symbol, Price::fromRaw(price_raw),
                                            Quantity::fromRaw(qty_raw));
  LRVX_CAPI_LEAVE;
}

void lrvx_emit_cancel(LrvxStrategyHandle s, uint64_t order_id)
{
  LRVX_CAPI_ENTER_VOID(s);
  toStrategy(s)->publicEmitCancel(order_id);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_emit_cancel_all(LrvxStrategyHandle s, uint32_t symbol)
{
  LRVX_CAPI_ENTER_VOID(s);
  toStrategy(s)->publicEmitCancelAll(symbol);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_emit_modify(LrvxStrategyHandle s, uint64_t order_id, int64_t new_price_raw,
                      int64_t new_qty_raw)
{
  LRVX_CAPI_ENTER_VOID(s);
  toStrategy(s)->publicEmitModify(order_id, Price::fromRaw(new_price_raw),
                                  Quantity::fromRaw(new_qty_raw));
  LRVX_CAPI_LEAVE_VOID;
}

uint64_t lrvx_emit_stop_market(LrvxStrategyHandle s, uint32_t symbol, uint8_t side,
                               int64_t trigger_raw, int64_t qty_raw)
{
  LRVX_CAPI_ENTER(s);
  return toStrategy(s)->publicEmitStopMarket(symbol, side == 0 ? Side::BUY : Side::SELL,
                                             Price::fromRaw(trigger_raw),
                                             Quantity::fromRaw(qty_raw));
  LRVX_CAPI_LEAVE;
}

uint64_t lrvx_emit_stop_limit(LrvxStrategyHandle s, uint32_t symbol, uint8_t side,
                              int64_t trigger_raw, int64_t limit_raw, int64_t qty_raw)
{
  LRVX_CAPI_ENTER(s);
  return toStrategy(s)->publicEmitStopLimit(symbol, side == 0 ? Side::BUY : Side::SELL,
                                            Price::fromRaw(trigger_raw),
                                            Price::fromRaw(limit_raw),
                                            Quantity::fromRaw(qty_raw));
  LRVX_CAPI_LEAVE;
}

uint64_t lrvx_emit_take_profit_market(LrvxStrategyHandle s, uint32_t symbol, uint8_t side,
                                      int64_t trigger_raw, int64_t qty_raw)
{
  LRVX_CAPI_ENTER(s);
  return toStrategy(s)->publicEmitTakeProfitMarket(symbol, side == 0 ? Side::BUY : Side::SELL,
                                                   Price::fromRaw(trigger_raw),
                                                   Quantity::fromRaw(qty_raw));
  LRVX_CAPI_LEAVE;
}

uint64_t lrvx_emit_trailing_stop(LrvxStrategyHandle s, uint32_t symbol, uint8_t side,
                                 int64_t offset_raw, int64_t qty_raw)
{
  LRVX_CAPI_ENTER(s);
  return toStrategy(s)->publicEmitTrailingStop(symbol, side == 0 ? Side::BUY : Side::SELL,
                                               Price::fromRaw(offset_raw),
                                               Quantity::fromRaw(qty_raw));
  LRVX_CAPI_LEAVE;
}

uint64_t lrvx_emit_trailing_stop_percent(LrvxStrategyHandle s, uint32_t symbol, uint8_t side,
                                         int32_t callback_bps, int64_t qty_raw)
{
  LRVX_CAPI_ENTER(s);
  return toStrategy(s)->publicEmitTrailingStopPercent(
      symbol, side == 0 ? Side::BUY : Side::SELL, callback_bps, Quantity::fromRaw(qty_raw));
  LRVX_CAPI_LEAVE;
}

uint64_t lrvx_emit_take_profit_limit(LrvxStrategyHandle s, uint32_t symbol, uint8_t side,
                                     int64_t trigger_raw, int64_t limit_raw, int64_t qty_raw)
{
  LRVX_CAPI_ENTER(s);
  return toStrategy(s)->publicEmitTakeProfitLimit(
      symbol, side == 0 ? Side::BUY : Side::SELL, Price::fromRaw(trigger_raw),
      Price::fromRaw(limit_raw), Quantity::fromRaw(qty_raw));
  LRVX_CAPI_LEAVE;
}

uint64_t lrvx_emit_limit_buy_tif(LrvxStrategyHandle s, uint32_t symbol, int64_t price_raw,
                                 int64_t qty_raw, uint8_t time_in_force)
{
  LRVX_CAPI_ENTER(s);
  return toStrategy(s)->publicEmitLimitBuyTif(
      symbol, Price::fromRaw(price_raw), Quantity::fromRaw(qty_raw),
      static_cast<TimeInForce>(time_in_force));
  LRVX_CAPI_LEAVE;
}

uint64_t lrvx_emit_limit_sell_tif(LrvxStrategyHandle s, uint32_t symbol, int64_t price_raw,
                                  int64_t qty_raw, uint8_t time_in_force)
{
  LRVX_CAPI_ENTER(s);
  return toStrategy(s)->publicEmitLimitSellTif(
      symbol, Price::fromRaw(price_raw), Quantity::fromRaw(qty_raw),
      static_cast<TimeInForce>(time_in_force));
  LRVX_CAPI_LEAVE;
}

uint64_t lrvx_emit_close_position(LrvxStrategyHandle s, uint32_t symbol)
{
  LRVX_CAPI_ENTER(s);
  return toStrategy(s)->publicEmitClosePosition(symbol);
  LRVX_CAPI_LEAVE;
}

int32_t lrvx_get_order_status(LrvxStrategyHandle s, uint64_t order_id)
{
  LRVX_CAPI_ENTER(s);
  auto status = toStrategy(s)->getOrderStatus(order_id);
  return status ? static_cast<int32_t>(*status) : -1;
  LRVX_CAPI_LEAVE;
}

// ============================================================
// Context queries
// ============================================================

int64_t lrvx_position_raw(LrvxStrategyHandle s, uint32_t symbol)
{
  LRVX_CAPI_ENTER(s);
  return toStrategy(s)->position(symbol).raw();
  LRVX_CAPI_LEAVE;
}

int64_t lrvx_last_trade_price_raw(LrvxStrategyHandle s, uint32_t symbol)
{
  LRVX_CAPI_ENTER(s);
  return toStrategy(s)->ctx(symbol).lastTradePrice.raw();
  LRVX_CAPI_LEAVE;
}

int64_t lrvx_best_bid_raw(LrvxStrategyHandle s, uint32_t symbol)
{
  LRVX_CAPI_ENTER(s);
  auto bid = toStrategy(s)->ctx(symbol).book.bestBid();
  return bid ? bid->raw() : 0;
  LRVX_CAPI_LEAVE;
}

int64_t lrvx_best_ask_raw(LrvxStrategyHandle s, uint32_t symbol)
{
  LRVX_CAPI_ENTER(s);
  auto ask = toStrategy(s)->ctx(symbol).book.bestAsk();
  return ask ? ask->raw() : 0;
  LRVX_CAPI_LEAVE;
}

int64_t lrvx_mid_price_raw(LrvxStrategyHandle s, uint32_t symbol)
{
  LRVX_CAPI_ENTER(s);
  auto mid = toStrategy(s)->ctx(symbol).mid();
  return mid ? mid->raw() : 0;
  LRVX_CAPI_LEAVE;
}

// The three above cannot separate "no quote" from a best quote of exactly 0.0:
// both come back as a raw of 0, and a book that quotes through zero reaches
// that price. These carry the presence flag in the return value instead.
uint8_t lrvx_best_bid_raw_opt(LrvxStrategyHandle s, uint32_t symbol, int64_t* price_out)
{
  LRVX_CAPI_ENTER(s);
  auto bid = toStrategy(s)->ctx(symbol).book.bestBid();
  if (!bid)
  {
    return 0;
  }
  if (price_out)
  {
    *price_out = bid->raw();
  }
  return 1;
  LRVX_CAPI_LEAVE;
}

uint8_t lrvx_best_ask_raw_opt(LrvxStrategyHandle s, uint32_t symbol, int64_t* price_out)
{
  LRVX_CAPI_ENTER(s);
  auto ask = toStrategy(s)->ctx(symbol).book.bestAsk();
  if (!ask)
  {
    return 0;
  }
  if (price_out)
  {
    *price_out = ask->raw();
  }
  return 1;
  LRVX_CAPI_LEAVE;
}

uint8_t lrvx_mid_price_raw_opt(LrvxStrategyHandle s, uint32_t symbol, int64_t* price_out)
{
  LRVX_CAPI_ENTER(s);
  auto mid = toStrategy(s)->ctx(symbol).mid();
  if (!mid)
  {
    return 0;
  }
  if (price_out)
  {
    *price_out = mid->raw();
  }
  return 1;
  LRVX_CAPI_LEAVE;
}

void lrvx_get_symbol_context(LrvxStrategyHandle s, uint32_t symbol, LrvxSymbolContext* out)
{
  LRVX_CAPI_ENTER_VOID(s);
  const auto& c = toStrategy(s)->ctx(symbol);
  out->symbol_id = c.symbolId;
  out->position_raw = c.position.raw();
  out->has_avg_entry_price = c.avgEntryPrice ? 1u : 0u;
  out->avg_entry_price_raw = c.avgEntryPrice ? c.avgEntryPrice->raw() : 0;
  out->last_trade_price_raw = c.lastTradePrice.raw();
  out->last_update_ns = c.lastUpdateNs;

  // The touch, same as BridgeStrategy::toBookSnapshot. The flags say whether
  // a side has a quote; the price fields cannot, since a book may be quoted
  // at exactly zero, and a size of 0 is reserved for an empty side.
  out->book.has_bid = c.touchBid ? 1u : 0u;
  out->book.has_ask = c.touchAsk ? 1u : 0u;
  out->book.bid_price_raw = c.touchBid ? c.touchBid->price.raw() : 0;
  out->book.bid_qty_raw = c.touchBid ? c.touchBid->quantity.raw() : 0;
  out->book.ask_price_raw = c.touchAsk ? c.touchAsk->price.raw() : 0;
  out->book.ask_qty_raw = c.touchAsk ? c.touchAsk->quantity.raw() : 0;
  auto mid = c.touchMid();
  out->book.mid_raw = mid ? mid->raw() : 0;
  out->book.spread_raw =
      (c.touchBid && c.touchAsk) ? (c.touchAsk->price.raw() - c.touchBid->price.raw()) : 0;
  LRVX_CAPI_LEAVE_VOID;
}

// ============================================================
// Fixed-point conversion
// ============================================================

int64_t lrvx_price_from_double(double value)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  return Price::fromDouble(value).raw();
  LRVX_CAPI_LEAVE;
}

double lrvx_price_to_double(int64_t raw)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  return Price::fromRaw(raw).toDouble();
  LRVX_CAPI_LEAVE;
}

int64_t lrvx_quantity_from_double(double value)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  return Quantity::fromDouble(value).raw();
  LRVX_CAPI_LEAVE;
}

double lrvx_quantity_to_double(int64_t raw)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  return Quantity::fromRaw(raw).toDouble();
  LRVX_CAPI_LEAVE;
}

// ============================================================
// Indicators
// ============================================================

void lrvx_indicator_ema(const double* input, size_t len, size_t period, double* output)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  indicator::EMA ema(period);
  ema.compute(std::span<const double>(input, len), std::span<double>(output, len));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_indicator_sma(const double* input, size_t len, size_t period, double* output)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  indicator::SMA sma(period);
  sma.compute(std::span<const double>(input, len), std::span<double>(output, len));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_indicator_rsi(const double* input, size_t len, size_t period, double* output)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  indicator::RSI rsi(period);
  rsi.compute(std::span<const double>(input, len), std::span<double>(output, len));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_indicator_atr(const double* high, const double* low, const double* close, size_t len,
                        size_t period, double* output)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  indicator::ATR atr(period);
  atr.compute(std::span<const double>(high, len), std::span<const double>(low, len),
              std::span<const double>(close, len), std::span<double>(output, len));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_indicator_macd(const double* input, size_t len, size_t fast_period, size_t slow_period,
                         size_t signal_period, double* macd_out, double* signal_out,
                         double* hist_out)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  indicator::MACD macd(fast_period, slow_period, signal_period);
  macd.compute(std::span<const double>(input, len), std::span<double>(macd_out, len),
               std::span<double>(signal_out, len), std::span<double>(hist_out, len));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_indicator_bollinger(const double* input, size_t len, size_t period, double multiplier,
                              double* upper, double* middle, double* lower)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  indicator::Bollinger bb(period, multiplier);
  bb.compute(std::span<const double>(input, len), std::span<double>(upper, len),
             std::span<double>(middle, len), std::span<double>(lower, len));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_indicator_rma(const double* input, size_t len, size_t period, double* output)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  indicator::RMA rma(period);
  rma.compute(std::span<const double>(input, len), std::span<double>(output, len));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_indicator_dema(const double* input, size_t len, size_t period, double* output)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  indicator::DEMA dema(period);
  auto result = dema.compute(std::span<const double>(input, len));
  std::copy(result.begin(), result.end(), output);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_indicator_tema(const double* input, size_t len, size_t period, double* output)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  indicator::TEMA tema(period);
  auto result = tema.compute(std::span<const double>(input, len));
  std::copy(result.begin(), result.end(), output);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_indicator_kama(const double* input, size_t len, size_t period, size_t fast, size_t slow,
                         double* output)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  indicator::KAMA kama(period, fast, slow);
  auto result = kama.compute(std::span<const double>(input, len));
  std::copy(result.begin(), result.end(), output);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_indicator_slope(const double* input, size_t len, size_t length, double* output)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  indicator::Slope slope(length);
  slope.compute(std::span<const double>(input, len), std::span<double>(output, len));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_indicator_adx(const double* high, const double* low, const double* close, size_t len,
                        size_t period, double* adx_out, double* plus_di_out, double* minus_di_out)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  indicator::ADX adx(period);
  auto result = adx.compute(std::span<const double>(high, len), std::span<const double>(low, len),
                            std::span<const double>(close, len));
  std::copy(result.adx.begin(), result.adx.end(), adx_out);
  std::copy(result.plus_di.begin(), result.plus_di.end(), plus_di_out);
  std::copy(result.minus_di.begin(), result.minus_di.end(), minus_di_out);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_indicator_cci(const double* high, const double* low, const double* close, size_t len,
                        size_t period, double* output)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  indicator::CCI cci(period);
  auto result = cci.compute(std::span<const double>(high, len), std::span<const double>(low, len),
                            std::span<const double>(close, len));
  std::copy(result.begin(), result.end(), output);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_indicator_stochastic(const double* high, const double* low, const double* close,
                               size_t len, size_t k_period, size_t d_period, double* k_out,
                               double* d_out)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  indicator::Stochastic stoch(k_period, d_period);
  auto result =
      stoch.compute(std::span<const double>(high, len), std::span<const double>(low, len),
                    std::span<const double>(close, len));
  std::copy(result.k.begin(), result.k.end(), k_out);
  std::copy(result.d.begin(), result.d.end(), d_out);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_indicator_chop(const double* high, const double* low, const double* close, size_t len,
                         size_t period, double* output)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  indicator::CHOP chop(period);
  auto result = chop.compute(std::span<const double>(high, len), std::span<const double>(low, len),
                             std::span<const double>(close, len));
  std::copy(result.begin(), result.end(), output);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_indicator_obv(const double* close, const double* volume, size_t len, double* output)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  indicator::OBV obv;
  obv.compute(std::span<const double>(close, len), std::span<const double>(volume, len),
              std::span<double>(output, len));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_indicator_vwap(const double* close, const double* volume, size_t len, size_t window,
                         double* output)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  indicator::VWAP vwap(window);
  auto result =
      vwap.compute(std::span<const double>(close, len), std::span<const double>(volume, len));
  std::copy(result.begin(), result.end(), output);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_indicator_cvd(const double* open, const double* high, const double* low,
                        const double* close, const double* volume, size_t len, double* output)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  indicator::CVD cvd;
  auto result = cvd.compute(
      std::span<const double>(open, len), std::span<const double>(high, len),
      std::span<const double>(low, len), std::span<const double>(close, len),
      std::span<const double>(volume, len));
  std::copy(result.begin(), result.end(), output);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_indicator_skewness(const double* input, size_t len, size_t period, double* output)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  indicator::Skewness ind(period);
  ind.compute(std::span<const double>(input, len), std::span<double>(output, len));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_indicator_kurtosis(const double* input, size_t len, size_t period, double* output)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  indicator::Kurtosis ind(period);
  ind.compute(std::span<const double>(input, len), std::span<double>(output, len));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_indicator_parkinson_vol(const double* high, const double* low, size_t len, size_t period,
                                  double* output)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  indicator::ParkinsonVol ind(period);
  ind.compute(std::span<const double>(high, len), std::span<const double>(low, len),
              std::span<double>(output, len));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_indicator_rogers_satchell_vol(const double* open, const double* high, const double* low,
                                        const double* close, size_t len, size_t period,
                                        double* output)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  indicator::RogersSatchellVol ind(period);
  ind.compute(std::span<const double>(open, len), std::span<const double>(high, len),
              std::span<const double>(low, len), std::span<const double>(close, len),
              std::span<double>(output, len));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_indicator_rolling_zscore(const double* input, size_t len, size_t period, double* output)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  indicator::RollingZScore ind(period);
  ind.compute(std::span<const double>(input, len), std::span<double>(output, len));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_indicator_shannon_entropy(const double* input, size_t len, size_t period, size_t bins,
                                    double* output)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  indicator::ShannonEntropy ind(period, bins);
  ind.compute(std::span<const double>(input, len), std::span<double>(output, len));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_indicator_correlation(const double* x, const double* y, size_t len, size_t period,
                                double* output)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  indicator::Correlation ind(period);
  ind.compute(std::span<const double>(x, len), std::span<const double>(y, len),
              std::span<double>(output, len));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_indicator_adf(const double* input, size_t len, size_t max_lag, const char* regression,
                        double* test_stat_out, double* p_value_out, size_t* used_lag_out)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  std::string reg = regression ? regression : "c";
  auto r = indicator::adf(std::span<const double>(input, len), max_lag, reg);
  if (test_stat_out)
  {
    *test_stat_out = r.test_stat;
  }
  if (p_value_out)
  {
    *p_value_out = r.p_value;
  }
  if (used_lag_out)
  {
    *used_lag_out = r.used_lag;
  }
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_indicator_autocorrelation(const double* input, size_t len, size_t window, size_t lag,
                                    double* output)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  indicator::AutoCorrelation ind(window, lag);
  ind.compute(std::span<const double>(input, len), std::span<double>(output, len));
  LRVX_CAPI_LEAVE_VOID;
}

// ============================================================
// Targets
// ============================================================

void lrvx_target_future_return(const double* close, size_t len, size_t horizon, double* output)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  target::FutureReturn t(horizon);
  t.compute(std::span<const double>(close, len), std::span<double>(output, len));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_target_future_ctc_volatility(const double* close, size_t len, size_t horizon,
                                       double* output)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  target::FutureCTCVolatility t(horizon);
  t.compute(std::span<const double>(close, len), std::span<double>(output, len));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_target_future_linear_slope(const double* close, size_t len, size_t horizon,
                                     double* output)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  target::FutureLinearSlope t(horizon);
  t.compute(std::span<const double>(close, len), std::span<double>(output, len));
  LRVX_CAPI_LEAVE_VOID;
}

// ============================================================
// IndicatorGraph (batch) — handle-based wrapper.
// ============================================================

namespace
{

struct LrvxGraphImpl
{
  indicator::IndicatorGraph graph;
  std::unordered_map<uint32_t, std::vector<Bar>> barStorage;
};

}  // namespace

LrvxIndicatorGraphHandle lrvx_indicator_graph_create(void)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  return new LrvxGraphImpl();
  LRVX_CAPI_LEAVE;
}

void lrvx_indicator_graph_destroy(LrvxIndicatorGraphHandle g)
{
  LRVX_CAPI_ENTER_DESTROY(g);
  delete static_cast<LrvxGraphImpl*>(g);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_indicator_graph_set_bars(LrvxIndicatorGraphHandle g, uint32_t symbol,
                                   const double* close, const double* high, const double* low,
                                   const double* volume, size_t len)
{
  LRVX_CAPI_ENTER_VOID(g);
  auto* impl = static_cast<LrvxGraphImpl*>(g);
  std::vector<Bar> bars(len);
  for (size_t i = 0; i < len; ++i)
  {
    bars[i].open = Price::fromDouble(close[i]);
    bars[i].high = Price::fromDouble(high ? high[i] : close[i]);
    bars[i].low = Price::fromDouble(low ? low[i] : close[i]);
    bars[i].close = Price::fromDouble(close[i]);
    bars[i].volume = volume ? Volume::fromDouble(volume[i]) : Volume{};
  }
  impl->barStorage[symbol] = std::move(bars);
  impl->graph.setBars(static_cast<SymbolId>(symbol),
                      std::span<const Bar>(impl->barStorage[symbol]));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_indicator_graph_add_node(LrvxIndicatorGraphHandle g, const char* name,
                                   const char* const* deps, size_t num_deps,
                                   LrvxGraphNodeFn fn, void* user_data)
{
  LRVX_CAPI_ENTER_VOID(g);
  auto* impl = static_cast<LrvxGraphImpl*>(g);
  std::vector<std::string> depList;
  depList.reserve(num_deps);
  for (size_t i = 0; i < num_deps; ++i)
  {
    depList.emplace_back(deps[i]);
  }
  impl->graph.addNode(name, std::move(depList),
                      [g, fn, user_data](indicator::IndicatorGraph&, SymbolId sym)
                      {
                        size_t outLen = 0;
                        const double* p = fn(user_data, g, static_cast<uint32_t>(sym), &outLen);
                        if (!p)
                        {
                          return std::vector<double>{};
                        }
                        return std::vector<double>(p, p + outLen);
                      });
  LRVX_CAPI_LEAVE_VOID;
}

const double* lrvx_indicator_graph_require(LrvxIndicatorGraphHandle g, uint32_t symbol,
                                           const char* name, size_t* len_out)
{
  LRVX_CAPI_ENTER(g);
  auto* impl = static_cast<LrvxGraphImpl*>(g);
  try
  {
    const auto& v = impl->graph.require(static_cast<SymbolId>(symbol), name);
    if (len_out)
    {
      *len_out = v.size();
    }
    return v.data();
  }
  catch (...)
  {
    if (len_out)
    {
      *len_out = 0;
    }
    return nullptr;
  }
  LRVX_CAPI_LEAVE;
}

const double* lrvx_indicator_graph_get(LrvxIndicatorGraphHandle g, uint32_t symbol,
                                       const char* name, size_t* len_out)
{
  LRVX_CAPI_ENTER(g);
  auto* impl = static_cast<LrvxGraphImpl*>(g);
  const auto* v = impl->graph.get(static_cast<SymbolId>(symbol), name);
  if (!v)
  {
    if (len_out)
    {
      *len_out = 0;
    }
    return nullptr;
  }
  if (len_out)
  {
    *len_out = v->size();
  }
  return v->data();
  LRVX_CAPI_LEAVE;
}

const double* lrvx_indicator_graph_close(LrvxIndicatorGraphHandle g, uint32_t symbol,
                                         size_t* len_out)
{
  LRVX_CAPI_ENTER(g);
  auto* impl = static_cast<LrvxGraphImpl*>(g);
  const auto& v = impl->graph.close(static_cast<SymbolId>(symbol));
  if (len_out)
  {
    *len_out = v.size();
  }
  return v.data();
  LRVX_CAPI_LEAVE;
}

const double* lrvx_indicator_graph_high(LrvxIndicatorGraphHandle g, uint32_t symbol,
                                        size_t* len_out)
{
  LRVX_CAPI_ENTER(g);
  auto* impl = static_cast<LrvxGraphImpl*>(g);
  const auto& v = impl->graph.high(static_cast<SymbolId>(symbol));
  if (len_out)
  {
    *len_out = v.size();
  }
  return v.data();
  LRVX_CAPI_LEAVE;
}

const double* lrvx_indicator_graph_low(LrvxIndicatorGraphHandle g, uint32_t symbol,
                                       size_t* len_out)
{
  LRVX_CAPI_ENTER(g);
  auto* impl = static_cast<LrvxGraphImpl*>(g);
  const auto& v = impl->graph.low(static_cast<SymbolId>(symbol));
  if (len_out)
  {
    *len_out = v.size();
  }
  return v.data();
  LRVX_CAPI_LEAVE;
}

const double* lrvx_indicator_graph_volume(LrvxIndicatorGraphHandle g, uint32_t symbol,
                                          size_t* len_out)
{
  LRVX_CAPI_ENTER(g);
  auto* impl = static_cast<LrvxGraphImpl*>(g);
  const auto& v = impl->graph.volume(static_cast<SymbolId>(symbol));
  if (len_out)
  {
    *len_out = v.size();
  }
  return v.data();
  LRVX_CAPI_LEAVE;
}

void lrvx_indicator_graph_invalidate(LrvxIndicatorGraphHandle g, uint32_t symbol)
{
  LRVX_CAPI_ENTER_VOID(g);
  static_cast<LrvxGraphImpl*>(g)->graph.invalidate(static_cast<SymbolId>(symbol));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_indicator_graph_invalidate_all(LrvxIndicatorGraphHandle g)
{
  LRVX_CAPI_ENTER_VOID(g);
  static_cast<LrvxGraphImpl*>(g)->graph.invalidateAll();
  LRVX_CAPI_LEAVE_VOID;
}

// ============================================================
// IndicatorGraph — streaming methods on the same handle.
// ============================================================

void lrvx_indicator_graph_step(LrvxIndicatorGraphHandle g, uint32_t symbol, double open,
                               double high, double low, double close, double volume)
{
  LRVX_CAPI_ENTER_VOID(g);
  auto* impl = static_cast<LrvxGraphImpl*>(g);
  Bar bar;
  bar.open = Price::fromDouble(open);
  bar.high = Price::fromDouble(high);
  bar.low = Price::fromDouble(low);
  bar.close = Price::fromDouble(close);
  bar.volume = Volume::fromDouble(volume);
  impl->graph.step(static_cast<SymbolId>(symbol), bar);
  LRVX_CAPI_LEAVE_VOID;
}

double lrvx_indicator_graph_current(LrvxIndicatorGraphHandle g, uint32_t symbol, const char* name)
{
  LRVX_CAPI_ENTER(g);
  return static_cast<LrvxGraphImpl*>(g)->graph.current(static_cast<SymbolId>(symbol), name);
  LRVX_CAPI_LEAVE;
}

uint32_t lrvx_indicator_graph_bar_count(LrvxIndicatorGraphHandle g, uint32_t symbol)
{
  LRVX_CAPI_ENTER(g);
  return static_cast<uint32_t>(
      static_cast<LrvxGraphImpl*>(g)->graph.barCount(static_cast<SymbolId>(symbol)));
  LRVX_CAPI_LEAVE;
}

void lrvx_indicator_graph_reset(LrvxIndicatorGraphHandle g, uint32_t symbol)
{
  LRVX_CAPI_ENTER_VOID(g);
  static_cast<LrvxGraphImpl*>(g)->graph.reset(static_cast<SymbolId>(symbol));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_indicator_graph_reset_all(LrvxIndicatorGraphHandle g)
{
  LRVX_CAPI_ENTER_VOID(g);
  static_cast<LrvxGraphImpl*>(g)->graph.resetAll();
  LRVX_CAPI_LEAVE_VOID;
}

// ── Deprecated streaming-graph shim — forwards to the unified API ──

LrvxStreamingGraphHandle lrvx_streaming_graph_create(void)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  return lrvx_indicator_graph_create();
  LRVX_CAPI_LEAVE;
}

void lrvx_streaming_graph_destroy(LrvxStreamingGraphHandle sg)
{
  LRVX_CAPI_ENTER_DESTROY(sg);
  lrvx_indicator_graph_destroy(sg);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_streaming_graph_add_node(LrvxStreamingGraphHandle sg, const char* name,
                                   const char* const* deps, size_t num_deps, LrvxGraphNodeFn fn,
                                   void* user_data)
{
  LRVX_CAPI_ENTER_VOID(sg);
  lrvx_indicator_graph_add_node(sg, name, deps, num_deps, fn, user_data);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_streaming_graph_step(LrvxStreamingGraphHandle sg, uint32_t symbol, double open,
                               double high, double low, double close, double volume)
{
  LRVX_CAPI_ENTER_VOID(sg);
  lrvx_indicator_graph_step(sg, symbol, open, high, low, close, volume);
  LRVX_CAPI_LEAVE_VOID;
}

double lrvx_streaming_graph_current(LrvxStreamingGraphHandle sg, uint32_t symbol, const char* name)
{
  LRVX_CAPI_ENTER(sg);
  return lrvx_indicator_graph_current(sg, symbol, name);
  LRVX_CAPI_LEAVE;
}

uint32_t lrvx_streaming_graph_bar_count(LrvxStreamingGraphHandle sg, uint32_t symbol)
{
  LRVX_CAPI_ENTER(sg);
  return lrvx_indicator_graph_bar_count(sg, symbol);
  LRVX_CAPI_LEAVE;
}

void lrvx_streaming_graph_reset(LrvxStreamingGraphHandle sg, uint32_t symbol)
{
  LRVX_CAPI_ENTER_VOID(sg);
  lrvx_indicator_graph_reset(sg, symbol);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_streaming_graph_reset_all(LrvxStreamingGraphHandle sg)
{
  LRVX_CAPI_ENTER_VOID(sg);
  lrvx_indicator_graph_reset_all(sg);
  LRVX_CAPI_LEAVE_VOID;
}

const double* lrvx_streaming_graph_close(LrvxStreamingGraphHandle sg, uint32_t symbol,
                                         size_t* len_out)
{
  LRVX_CAPI_ENTER(sg);
  return lrvx_indicator_graph_close(sg, symbol, len_out);
  LRVX_CAPI_LEAVE;
}
const double* lrvx_streaming_graph_high(LrvxStreamingGraphHandle sg, uint32_t symbol,
                                        size_t* len_out)
{
  LRVX_CAPI_ENTER(sg);
  return lrvx_indicator_graph_high(sg, symbol, len_out);
  LRVX_CAPI_LEAVE;
}
const double* lrvx_streaming_graph_low(LrvxStreamingGraphHandle sg, uint32_t symbol,
                                       size_t* len_out)
{
  LRVX_CAPI_ENTER(sg);
  return lrvx_indicator_graph_low(sg, symbol, len_out);
  LRVX_CAPI_LEAVE;
}
const double* lrvx_streaming_graph_volume(LrvxStreamingGraphHandle sg, uint32_t symbol,
                                          size_t* len_out)
{
  LRVX_CAPI_ENTER(sg);
  return lrvx_indicator_graph_volume(sg, symbol, len_out);
  LRVX_CAPI_LEAVE;
}

// ============================================================
// Order book
// ============================================================

struct LrvxBookImpl
{
  NLevelOrderBook<8192> book;
  explicit LrvxBookImpl(double tickSize) : book(Price::fromDouble(tickSize)) {}

  void applyUpdate(const double* bp, const double* bq, size_t bl, const double* ap,
                   const double* aq, size_t al, BookUpdateType type)
  {
    std::byte buf[32768];
    std::pmr::monotonic_buffer_resource res(buf, sizeof(buf));
    BookUpdateEvent ev(&res);
    ev.update.type = type;
    ev.update.bids.reserve(bl);
    for (size_t i = 0; i < bl; ++i)
    {
      ev.update.bids.push_back({Price::fromDouble(bp[i]), Quantity::fromDouble(bq[i])});
    }
    ev.update.asks.reserve(al);
    for (size_t i = 0; i < al; ++i)
    {
      ev.update.asks.push_back({Price::fromDouble(ap[i]), Quantity::fromDouble(aq[i])});
    }
    book.applyBookUpdate(ev);
  }
};

LrvxBookHandle lrvx_book_create(double tick_size)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  try
  {
    return new LrvxBookImpl(tick_size);
  }
  catch (...)
  {
    // A non-positive tick size is rejected by the book itself. Report it the
    // way the rest of this ABI reports a refused construction, and never let
    // the exception cross back into C.
    return nullptr;
  }
  LRVX_CAPI_LEAVE;
}

void lrvx_book_destroy(LrvxBookHandle book)
{
  LRVX_CAPI_ENTER_DESTROY(book);
  delete static_cast<LrvxBookImpl*>(book);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_book_apply_snapshot(LrvxBookHandle h, const double* bp, const double* bq, size_t bl,
                              const double* ap, const double* aq, size_t al)
{
  LRVX_CAPI_ENTER_VOID(h);
  static_cast<LrvxBookImpl*>(h)->applyUpdate(bp, bq, bl, ap, aq, al, BookUpdateType::SNAPSHOT);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_book_apply_delta(LrvxBookHandle h, const double* bp, const double* bq, size_t bl,
                           const double* ap, const double* aq, size_t al)
{
  LRVX_CAPI_ENTER_VOID(h);
  static_cast<LrvxBookImpl*>(h)->applyUpdate(bp, bq, bl, ap, aq, al, BookUpdateType::DELTA);
  LRVX_CAPI_LEAVE_VOID;
}

uint8_t lrvx_book_best_bid(LrvxBookHandle h, double* price_out)
{
  LRVX_CAPI_ENTER(h);
  auto bid = static_cast<LrvxBookImpl*>(h)->book.bestBid();
  if (bid)
  {
    *price_out = bid->toDouble();
    return 1;
  }
  return 0;
  LRVX_CAPI_LEAVE;
}

uint8_t lrvx_book_best_ask(LrvxBookHandle h, double* price_out)
{
  LRVX_CAPI_ENTER(h);
  auto ask = static_cast<LrvxBookImpl*>(h)->book.bestAsk();
  if (ask)
  {
    *price_out = ask->toDouble();
    return 1;
  }
  return 0;
  LRVX_CAPI_LEAVE;
}

uint8_t lrvx_book_mid(LrvxBookHandle h, double* price_out)
{
  LRVX_CAPI_ENTER(h);
  auto& book = static_cast<LrvxBookImpl*>(h)->book;
  auto mid = book.mid();
  if (mid)
  {
    *price_out = mid->toDouble();
    return 1;
  }
  return 0;
  LRVX_CAPI_LEAVE;
}

uint8_t lrvx_book_spread(LrvxBookHandle h, double* spread_out)
{
  LRVX_CAPI_ENTER(h);
  auto& book = static_cast<LrvxBookImpl*>(h)->book;
  auto sp = book.spread();
  if (sp)
  {
    *spread_out = sp->toDouble();
    return 1;
  }
  return 0;
  LRVX_CAPI_LEAVE;
}

double lrvx_book_bid_at_price(LrvxBookHandle h, double price)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<LrvxBookImpl*>(h)->book.bidAtPrice(Price::fromDouble(price)).toDouble();
  LRVX_CAPI_LEAVE;
}

double lrvx_book_ask_at_price(LrvxBookHandle h, double price)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<LrvxBookImpl*>(h)->book.askAtPrice(Price::fromDouble(price)).toDouble();
  LRVX_CAPI_LEAVE;
}

uint8_t lrvx_book_is_crossed(LrvxBookHandle h)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<LrvxBookImpl*>(h)->book.isCrossed() ? 1 : 0;
  LRVX_CAPI_LEAVE;
}

void lrvx_book_clear(LrvxBookHandle h)
{
  LRVX_CAPI_ENTER_VOID(h);
  static_cast<LrvxBookImpl*>(h)->book.clear();
  LRVX_CAPI_LEAVE_VOID;
}

// ============================================================
// Simulated executor
// ============================================================

// A LrvxSimulatedExecutorHandle points at this wrapper, never at the
// SimulatedExecutor itself: every lrvx_simulated_executor_* function reaches
// `executor` through it, and the engine object sits at a non-zero offset
// inside. Handing out the engine object instead read every field sixteen
// bytes off -- fill counts came back as billions, advancing the clock
// overwrote the executor's own header, and submitting an order crashed.
//
// The members are references so a wrapper can either own the pair or borrow
// one that a VenueStack owns, without every call site having to know which.
struct LrvxSimulatedExecutorImpl
{
 private:
  // Declared first so they are constructed before the references bind.
  std::unique_ptr<SimulatedClock> _ownedClock;
  std::unique_ptr<SimulatedExecutor> _ownedExecutor;

 public:
  SimulatedClock& clock;
  SimulatedExecutor& executor;

  LrvxSimulatedExecutorImpl()
      : _ownedClock(std::make_unique<SimulatedClock>()),
        _ownedExecutor(std::make_unique<SimulatedExecutor>(*_ownedClock)),
        clock(*_ownedClock),
        executor(*_ownedExecutor)
  {
  }

  LrvxSimulatedExecutorImpl(SimulatedClock& borrowedClock, SimulatedExecutor& borrowedExecutor)
      : clock(borrowedClock), executor(borrowedExecutor)
  {
  }
};

LrvxSimulatedExecutorHandle lrvx_simulated_executor_create(void)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  return new LrvxSimulatedExecutorImpl();
  LRVX_CAPI_LEAVE;
}

void lrvx_simulated_executor_destroy(LrvxSimulatedExecutorHandle executor)
{
  LRVX_CAPI_ENTER_DESTROY(executor);
  delete static_cast<LrvxSimulatedExecutorImpl*>(executor);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_simulated_executor_submit_order(LrvxSimulatedExecutorHandle h, uint64_t id, uint8_t side, double price,
                                          double quantity, uint8_t order_type, uint32_t symbol)
{
  LRVX_CAPI_ENTER_VOID(h);
  auto* impl = static_cast<LrvxSimulatedExecutorImpl*>(h);
  Order order{};
  order.id = id;
  order.side = side == 0 ? Side::BUY : Side::SELL;
  order.price = Price::fromDouble(price);
  order.quantity = Quantity::fromDouble(quantity);
  order.type = static_cast<OrderType>(order_type);
  order.symbol = symbol;
  impl->executor.submitOrder(order);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_simulated_executor_submit_order_ex(LrvxSimulatedExecutorHandle h, uint64_t id,
                                             uint8_t side, double price, double quantity,
                                             uint8_t order_type, uint32_t symbol,
                                             uint8_t tif, uint8_t reduce_only,
                                             int64_t expires_at_ns)
{
  LRVX_CAPI_ENTER_VOID(h);
  auto* impl = static_cast<LrvxSimulatedExecutorImpl*>(h);
  Order order{};
  order.id = id;
  order.side = side == 0 ? Side::BUY : Side::SELL;
  order.price = Price::fromDouble(price);
  order.quantity = Quantity::fromDouble(quantity);
  order.type = static_cast<OrderType>(order_type);
  order.symbol = symbol;
  order.timeInForce = static_cast<TimeInForce>(tif);
  order.flags.reduceOnly = reduce_only ? 1 : 0;
  if (expires_at_ns > 0)
  {
    order.expiresAfter = TimePoint(std::chrono::nanoseconds(expires_at_ns));
  }
  impl->executor.submitOrder(order);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_simulated_executor_submit_bracket(LrvxSimulatedExecutorHandle h, uint64_t bracket_id,
                                            uint32_t symbol, uint8_t entry_side,
                                            uint8_t entry_type, double entry_price,
                                            double quantity, uint8_t tp_side, uint8_t tp_type,
                                            double tp_price, uint8_t stop_side,
                                            uint8_t stop_type, double stop_trigger_price)
{
  LRVX_CAPI_ENTER_VOID(h);
  BracketOrder b;
  b.bracketId = bracket_id;
  b.symbol = symbol;
  b.entry.side = entry_side == 0 ? Side::BUY : Side::SELL;
  b.entry.type = static_cast<OrderType>(entry_type);
  b.entry.price = Price::fromDouble(entry_price);
  b.entry.quantity = Quantity::fromDouble(quantity);
  b.takeProfit.side = tp_side == 0 ? Side::BUY : Side::SELL;
  b.takeProfit.type = static_cast<OrderType>(tp_type);
  b.takeProfit.price = Price::fromDouble(tp_price);
  b.takeProfit.quantity = Quantity::fromDouble(quantity);
  b.stop.side = stop_side == 0 ? Side::BUY : Side::SELL;
  b.stop.type = static_cast<OrderType>(stop_type);
  b.stop.triggerPrice = Price::fromDouble(stop_trigger_price);
  b.stop.quantity = Quantity::fromDouble(quantity);
  static_cast<LrvxSimulatedExecutorImpl*>(h)->executor.submitBracket(b);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_simulated_executor_cancel_bracket(LrvxSimulatedExecutorHandle h, uint64_t bracket_id)
{
  LRVX_CAPI_ENTER_VOID(h);
  static_cast<LrvxSimulatedExecutorImpl*>(h)->executor.cancelBracket(bracket_id);
  LRVX_CAPI_LEAVE_VOID;
}

uint8_t lrvx_simulated_executor_bracket_state(LrvxSimulatedExecutorHandle h, uint64_t bracket_id)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<uint8_t>(
      static_cast<LrvxSimulatedExecutorImpl*>(h)->executor.bracketStatus(bracket_id).state);
  LRVX_CAPI_LEAVE;
}

void lrvx_simulated_executor_set_bracket_child_arm_mode(LrvxSimulatedExecutorHandle h,
                                                        uint8_t mode)
{
  LRVX_CAPI_ENTER_VOID(h);
  static_cast<LrvxSimulatedExecutorImpl*>(h)->executor.setBracketChildArmMode(
      mode == 1 ? SimulatedExecutor::BracketArmMode::OnPartialFill
                : SimulatedExecutor::BracketArmMode::OnFullFill);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_simulated_executor_submit_iceberg(LrvxSimulatedExecutorHandle h, uint64_t id,
                                            uint8_t side, double price,
                                            double total_quantity, double visible_quantity,
                                            uint32_t symbol)
{
  LRVX_CAPI_ENTER_VOID(h);
  Order order{};
  order.id = id;
  order.side = side == 0 ? Side::BUY : Side::SELL;
  order.price = Price::fromDouble(price);
  order.quantity = Quantity::fromDouble(total_quantity);
  order.visibleQuantity = Quantity::fromDouble(visible_quantity);
  order.type = OrderType::ICEBERG;
  order.symbol = symbol;
  static_cast<LrvxSimulatedExecutorImpl*>(h)->executor.submitOrder(order);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_simulated_executor_set_iceberg_refresh_latency(LrvxSimulatedExecutorHandle h,
                                                         int64_t latency_ns)
{
  LRVX_CAPI_ENTER_VOID(h);
  static_cast<LrvxSimulatedExecutorImpl*>(h)->executor.setIcebergRefreshLatency(latency_ns);
  LRVX_CAPI_LEAVE_VOID;
}

int64_t lrvx_simulated_executor_iceberg_hidden_remaining_raw(LrvxSimulatedExecutorHandle h,
                                                             uint64_t id)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<LrvxSimulatedExecutorImpl*>(h)->executor.icebergHiddenRemainingRaw(id);
  LRVX_CAPI_LEAVE;
}

void lrvx_simulated_executor_set_iceberg_size_randomisation_pct(
    LrvxSimulatedExecutorHandle h, double pct)
{
  LRVX_CAPI_ENTER_VOID(h);
  static_cast<LrvxSimulatedExecutorImpl*>(h)->executor.setIcebergSizeRandomisationPct(pct);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_simulated_executor_set_iceberg_priority_mode(LrvxSimulatedExecutorHandle h,
                                                       uint8_t mode)
{
  LRVX_CAPI_ENTER_VOID(h);
  static_cast<LrvxSimulatedExecutorImpl*>(h)->executor.setIcebergPriorityMode(
      mode == 1 ? SimulatedExecutor::IcebergPriorityMode::Retain
                : SimulatedExecutor::IcebergPriorityMode::Back);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_simulated_executor_set_iceberg_jitter_seed(LrvxSimulatedExecutorHandle h,
                                                     uint64_t seed)
{
  LRVX_CAPI_ENTER_VOID(h);
  static_cast<LrvxSimulatedExecutorImpl*>(h)->executor.setIcebergJitterSeed(seed);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_simulated_executor_cancel_order(LrvxSimulatedExecutorHandle h, uint64_t order_id)
{
  LRVX_CAPI_ENTER_VOID(h);
  static_cast<LrvxSimulatedExecutorImpl*>(h)->executor.cancelOrder(order_id);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_simulated_executor_cancel_all(LrvxSimulatedExecutorHandle h, uint32_t symbol)
{
  LRVX_CAPI_ENTER_VOID(h);
  static_cast<LrvxSimulatedExecutorImpl*>(h)->executor.cancelAllOrders(symbol);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_simulated_executor_on_bar(LrvxSimulatedExecutorHandle h, uint32_t symbol, double close_price)
{
  LRVX_CAPI_ENTER_VOID(h);
  static_cast<LrvxSimulatedExecutorImpl*>(h)->executor.onBar(symbol, Price::fromDouble(close_price));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_simulated_executor_on_bar_ohlc(LrvxSimulatedExecutorHandle h, uint32_t symbol,
                                         double open_price, double high_price,
                                         double low_price, double close_price)
{
  LRVX_CAPI_ENTER_VOID(h);
  static_cast<LrvxSimulatedExecutorImpl*>(h)->executor.onBar(
      symbol, Price::fromDouble(open_price), Price::fromDouble(high_price),
      Price::fromDouble(low_price), Price::fromDouble(close_price));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_simulated_executor_begin_bar_callback_window(LrvxSimulatedExecutorHandle h)
{
  LRVX_CAPI_ENTER_VOID(h);
  static_cast<LrvxSimulatedExecutorImpl*>(h)->executor.beginBarCallbackWindow();
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_simulated_executor_end_bar_callback_window(LrvxSimulatedExecutorHandle h)
{
  LRVX_CAPI_ENTER_VOID(h);
  static_cast<LrvxSimulatedExecutorImpl*>(h)->executor.endBarCallbackWindow();
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_simulated_executor_reset(LrvxSimulatedExecutorHandle h)
{
  LRVX_CAPI_ENTER_VOID(h);
  static_cast<LrvxSimulatedExecutorImpl*>(h)->executor.reset();
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_simulated_executor_on_trade(LrvxSimulatedExecutorHandle h, uint32_t symbol, double price, uint8_t is_buy)
{
  LRVX_CAPI_ENTER_VOID(h);
  static_cast<LrvxSimulatedExecutorImpl*>(h)->executor.onTrade(symbol, Price::fromDouble(price),
                                                               is_buy != 0);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_simulated_executor_advance_clock(LrvxSimulatedExecutorHandle h, int64_t timestamp_ns)
{
  LRVX_CAPI_ENTER_VOID(h);
  static_cast<LrvxSimulatedExecutorImpl*>(h)->clock.advanceTo(UnixNanos::fromRaw(timestamp_ns));
  LRVX_CAPI_LEAVE_VOID;
}

uint32_t lrvx_simulated_executor_fill_count(LrvxSimulatedExecutorHandle h)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<uint32_t>(static_cast<LrvxSimulatedExecutorImpl*>(h)->executor.fills().size());
  LRVX_CAPI_LEAVE;
}

// ============================================================
// Bar aggregation
// ============================================================

static LrvxBar toLrvxBar(const Bar& bar)
{
  return {bar.startTime.time_since_epoch().count(),
          bar.endTime.time_since_epoch().count(),
          bar.open.raw(),
          bar.high.raw(),
          bar.low.raw(),
          bar.close.raw(),
          bar.volume.raw(),
          bar.buyVolume.raw(),
          static_cast<uint32_t>(bar.tradeCount.raw()),
          static_cast<uint8_t>(bar.reason)};
}

// The batch copy of the close path. It has to agree bar for bar with
// BarAggregator::onTrade and MultiTimeframeAggregator::processPolicy, so the
// two policy customization points -- a late trade the policy recognizes, and
// a policy that owns its own close -- are branched on here in the same order
// and with the same meaning.
template <typename Policy>
static uint32_t doAggregateC(Policy& policy, const int64_t* ts, const double* px,
                             const double* qty, const uint8_t* ib, size_t n, LrvxBar* bars_out,
                             uint32_t max_bars)
{
  uint32_t count = 0;
  Bar currentBar;
  bool initialized = false;

  // Reports the true total even past max_bars so a caller that sized its
  // buffer to the trade count can tell the result was truncated and retry.
  const auto writeBar = [&](const Bar& bar)
  {
    if (count < max_bars)
    {
      bars_out[count] = toLrvxBar(bar);
    }
    count++;
  };

  for (size_t i = 0; i < n; ++i)
  {
    TradeEvent trade;
    trade.trade.price = Price::fromDouble(px[i]);
    trade.trade.quantity = Quantity::fromDouble(qty[i]);
    trade.trade.isBuy = (ib[i] != 0);
    trade.trade.exchangeTsNs = UnixNanos::fromRaw(ts[i]);
    trade.trade.symbol = 1;
    trade.trade.instrument = InstrumentType::Spot;

    if (!initialized)
    {
      policy.initBar(trade, currentBar);
      initialized = true;
      continue;
    }

    if constexpr (DetectsLateTrades<Policy>)
    {
      if (policy.isLate(trade, currentBar))
      {
        continue;
      }
    }

    if (policy.shouldClose(trade, currentBar))
    {
      if constexpr (ClosesAndReopens<Policy>)
      {
        policy.closeAndReopen(trade, currentBar, writeBar);
      }
      else
      {
        writeBar(currentBar);
        policy.initBar(trade, currentBar);
      }
      continue;
    }
    policy.update(trade, currentBar);
  }
  return count;
}

uint32_t lrvx_aggregate_time_bars(const int64_t* ts, const double* px, const double* qty,
                                  const uint8_t* ib, size_t len, double interval_seconds,
                                  LrvxBar* bars_out, uint32_t max_bars)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  TimeBarPolicy policy(std::chrono::nanoseconds(
      static_cast<int64_t>(interval_seconds * 1'000'000'000.0)));
  return doAggregateC(policy, ts, px, qty, ib, len, bars_out, max_bars);
  LRVX_CAPI_LEAVE;
}

uint32_t lrvx_aggregate_tick_bars(const int64_t* ts, const double* px, const double* qty,
                                  const uint8_t* ib, size_t len, uint32_t tick_count,
                                  LrvxBar* bars_out, uint32_t max_bars)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  TickBarPolicy policy(tick_count);
  return doAggregateC(policy, ts, px, qty, ib, len, bars_out, max_bars);
  LRVX_CAPI_LEAVE;
}

uint32_t lrvx_aggregate_volume_bars(const int64_t* ts, const double* px, const double* qty,
                                    const uint8_t* ib, size_t len, double volume_threshold,
                                    LrvxBar* bars_out, uint32_t max_bars)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  auto policy = VolumeBarPolicy::fromDouble(volume_threshold);
  return doAggregateC(policy, ts, px, qty, ib, len, bars_out, max_bars);
  LRVX_CAPI_LEAVE;
}

// ============================================================
// Multi-timeframe alignment helpers
// ============================================================

static void writeLrvxBar(const Bar& bar, LrvxBar* out)
{
  *out = toLrvxBar(bar);
}

uint8_t lrvx_strategy_last_closed_bar(LrvxStrategyHandle s, uint32_t symbol,
                                      uint8_t bar_type, uint64_t param, LrvxBar* out)
{
  LRVX_CAPI_ENTER(s);
  auto bar = toStrategy(s)->lastClosedBar(symbol, static_cast<BarType>(bar_type), param);
  if (!bar || !out)
  {
    return 0;
  }
  writeLrvxBar(*bar, out);
  return 1;
  LRVX_CAPI_LEAVE;
}

uint32_t lrvx_strategy_last_n_closed_bars(LrvxStrategyHandle s, uint32_t symbol,
                                          uint8_t bar_type, uint64_t param,
                                          LrvxBar* bars_out, uint32_t max_bars)
{
  LRVX_CAPI_ENTER(s);
  if (!bars_out || max_bars == 0)
  {
    return 0;
  }
  auto bars = toStrategy(s)->lastNClosedBars(symbol, static_cast<BarType>(bar_type),
                                             param, static_cast<size_t>(max_bars));
  uint32_t count = static_cast<uint32_t>(std::min<size_t>(bars.size(), max_bars));
  for (uint32_t i = 0; i < count; ++i)
  {
    writeLrvxBar(bars[i], &bars_out[i]);
  }
  return count;
  LRVX_CAPI_LEAVE;
}

uint32_t lrvx_strategy_get_bar_ring_capacity(LrvxStrategyHandle s)
{
  LRVX_CAPI_ENTER(s);
  return static_cast<uint32_t>(toStrategy(s)->barRingCapacity());
  LRVX_CAPI_LEAVE;
}

void lrvx_strategy_set_bar_ring_capacity(LrvxStrategyHandle s, uint32_t capacity)
{
  LRVX_CAPI_ENTER_VOID(s);
  toStrategy(s)->setBarRingCapacity(static_cast<size_t>(capacity));
  LRVX_CAPI_LEAVE_VOID;
}

// ============================================================
// Multi-leg order group
// ============================================================

#include "lrvx/execution/order_group.h"

static OrderGroup* toOrderGroup(LrvxOrderGroupHandle h) { return static_cast<OrderGroup*>(h); }

LrvxOrderGroupHandle lrvx_order_group_create(uint64_t parent_signal_id, uint8_t policy)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  return new OrderGroup(parent_signal_id, static_cast<OrderGroupPolicy>(policy));
  LRVX_CAPI_LEAVE;
}

void lrvx_order_group_destroy(LrvxOrderGroupHandle h)
{
  LRVX_CAPI_ENTER_DESTROY(h);
  delete toOrderGroup(h);
  LRVX_CAPI_LEAVE_VOID;
}

uint32_t lrvx_order_group_add_market_leg(LrvxOrderGroupHandle h, uint32_t symbol,
                                         uint8_t side, int64_t qty_raw)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<uint32_t>(
      toOrderGroup(h)->addMarketLeg(symbol, side, Quantity::fromRaw(qty_raw)));
  LRVX_CAPI_LEAVE;
}

uint32_t lrvx_order_group_add_limit_leg(LrvxOrderGroupHandle h, uint32_t symbol,
                                        uint8_t side, int64_t price_raw,
                                        int64_t qty_raw)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<uint32_t>(toOrderGroup(h)->addLimitLeg(
      symbol, side, Price::fromRaw(price_raw), Quantity::fromRaw(qty_raw)));
  LRVX_CAPI_LEAVE;
}

uint32_t lrvx_order_group_leg_count(LrvxOrderGroupHandle h)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<uint32_t>(toOrderGroup(h)->legCount());
  LRVX_CAPI_LEAVE;
}

uint8_t lrvx_order_group_leg_state(LrvxOrderGroupHandle h, uint32_t leg_index)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<uint8_t>(toOrderGroup(h)->leg(leg_index).state);
  LRVX_CAPI_LEAVE;
}

int64_t lrvx_order_group_leg_filled_raw(LrvxOrderGroupHandle h, uint32_t leg_index)
{
  LRVX_CAPI_ENTER(h);
  return toOrderGroup(h)->leg(leg_index).filledQty.raw();
  LRVX_CAPI_LEAVE;
}

uint64_t lrvx_order_group_leg_order_id(LrvxOrderGroupHandle h, uint32_t leg_index)
{
  LRVX_CAPI_ENTER(h);
  return toOrderGroup(h)->leg(leg_index).orderId;
  LRVX_CAPI_LEAVE;
}

void lrvx_order_group_record_submit(LrvxOrderGroupHandle h, uint32_t leg_index,
                                    uint64_t order_id)
{
  LRVX_CAPI_ENTER_VOID(h);
  toOrderGroup(h)->recordSubmit(leg_index, order_id);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_order_group_record_fill(LrvxOrderGroupHandle h, uint32_t leg_index,
                                  int64_t cumulative_qty_raw)
{
  LRVX_CAPI_ENTER_VOID(h);
  toOrderGroup(h)->recordFill(leg_index, Quantity::fromRaw(cumulative_qty_raw));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_order_group_record_cancel(LrvxOrderGroupHandle h, uint32_t leg_index)
{
  LRVX_CAPI_ENTER_VOID(h);
  toOrderGroup(h)->recordCancel(leg_index);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_order_group_record_failure(LrvxOrderGroupHandle h, uint32_t leg_index)
{
  LRVX_CAPI_ENTER_VOID(h);
  toOrderGroup(h)->recordFailure(leg_index);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_order_group_record_replace_accepted(LrvxOrderGroupHandle h, uint32_t leg_index,
                                              uint64_t new_order_id)
{
  LRVX_CAPI_ENTER_VOID(h);
  toOrderGroup(h)->recordReplaceAccepted(leg_index, new_order_id);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_order_group_record_replace_rejected(LrvxOrderGroupHandle h, uint32_t leg_index)
{
  LRVX_CAPI_ENTER_VOID(h);
  toOrderGroup(h)->recordReplaceRejected(leg_index);
  LRVX_CAPI_LEAVE_VOID;
}

uint32_t lrvx_order_group_find_leg_by_order_id(LrvxOrderGroupHandle h, uint64_t order_id)
{
  LRVX_CAPI_ENTER(h);
  auto idx = toOrderGroup(h)->findLegByOrderId(order_id);
  if (idx.has_value())
  {
    return static_cast<uint32_t>(*idx);
  }
  return UINT32_MAX;
  LRVX_CAPI_LEAVE;
}

uint8_t lrvx_order_group_state(LrvxOrderGroupHandle h)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<uint8_t>(toOrderGroup(h)->state());
  LRVX_CAPI_LEAVE;
}

void lrvx_order_group_mark_action_dispatched(LrvxOrderGroupHandle h, uint32_t leg_index,
                                             uint8_t kind)
{
  LRVX_CAPI_ENTER_VOID(h);
  toOrderGroup(h)->markActionDispatched(leg_index,
                                        static_cast<OrderGroupAction::Kind>(kind));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_order_group_set_risk_limits(LrvxOrderGroupHandle h, int64_t max_gross_notional_raw,
                                      double max_concentration_pct, int64_t max_leg_qty_raw)
{
  LRVX_CAPI_ENTER_VOID(h);
  GroupRiskLimits limits;
  limits.maxGrossNotional = Quantity::fromRaw(max_gross_notional_raw);
  limits.maxConcentrationPct = max_concentration_pct;
  limits.maxLegQty = Quantity::fromRaw(max_leg_qty_raw);
  toOrderGroup(h)->setRiskLimits(limits);
  LRVX_CAPI_LEAVE_VOID;
}

namespace
{
inline void copy_truncated(const std::string& src, char* out, size_t capacity)
{
  if (!out || capacity == 0)
  {
    return;
  }
  size_t n = std::min(src.size(), capacity - 1);
  std::memcpy(out, src.data(), n);
  out[n] = '\0';
}
}  // namespace

uint8_t lrvx_order_group_precheck_submission(LrvxOrderGroupHandle h, double equity,
                                             const int64_t* market_ref_prices_raw,
                                             uint32_t market_ref_prices_len, char* rule_out,
                                             size_t rule_capacity, char* detail_out,
                                             size_t detail_capacity)
{
  LRVX_CAPI_ENTER(h);
  std::vector<Price> prices;
  prices.reserve(market_ref_prices_len);
  for (uint32_t i = 0; i < market_ref_prices_len; ++i)
  {
    prices.push_back(Price::fromRaw(market_ref_prices_raw[i]));
  }
  auto breach = toOrderGroup(h)->precheckSubmission(equity, prices);
  if (!breach.denied)
  {
    if (rule_out && rule_capacity > 0)
    {
      rule_out[0] = '\0';
    }
    if (detail_out && detail_capacity > 0)
    {
      detail_out[0] = '\0';
    }
    return 0;
  }
  copy_truncated(breach.rule, rule_out, rule_capacity);
  copy_truncated(breach.detail, detail_out, detail_capacity);
  return 1;
  LRVX_CAPI_LEAVE;
}

void lrvx_order_group_set_pair_latency_budget_ns(LrvxOrderGroupHandle h, int64_t budget_ns)
{
  LRVX_CAPI_ENTER_VOID(h);
  toOrderGroup(h)->setPairLatencyBudgetNs(budget_ns);
  LRVX_CAPI_LEAVE_VOID;
}

uint8_t lrvx_order_group_pair_latency_decision(LrvxOrderGroupHandle h,
                                               int64_t leader_submit_ts_ns,
                                               int64_t leader_ack_ts_ns,
                                               uint8_t ack_received)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<uint8_t>(toOrderGroup(h)->pairLatencyDecision(
      leader_submit_ts_ns, leader_ack_ts_ns, ack_received != 0));
  LRVX_CAPI_LEAVE;
}

uint32_t lrvx_order_group_recommended_actions(LrvxOrderGroupHandle h,
                                              int64_t* actions_out,
                                              uint32_t max_actions)
{
  LRVX_CAPI_ENTER(h);
  if (!actions_out || max_actions == 0)
  {
    return 0;
  }
  auto actions = toOrderGroup(h)->recommendedActions();
  uint32_t n = static_cast<uint32_t>(std::min<size_t>(actions.size(), max_actions));
  for (uint32_t i = 0; i < n; ++i)
  {
    const auto& a = actions[i];
    int64_t* slot = actions_out + i * 5;
    slot[0] = static_cast<int64_t>(a.kind);
    slot[1] = static_cast<int64_t>(a.legIndex);
    if (a.kind == OrderGroupAction::Kind::CancelLeg)
    {
      slot[2] = static_cast<int64_t>(a.orderId);
      slot[3] = 0;
      slot[4] = 0;
    }
    else
    {
      slot[2] = static_cast<int64_t>(a.symbol);
      slot[3] = static_cast<int64_t>(a.side);
      slot[4] = a.qty.raw();
    }
  }
  return n;
  LRVX_CAPI_LEAVE;
}

// ============================================================
// Multi-feed clock
// ============================================================

#include "lrvx/feed/multi_feed_clock.h"

namespace
{

struct FeedClockState
{
  MultiFeedClock clock;
  FeedClockSnapshot last;

  FeedClockState(std::vector<SymbolId> symbols, FeedClockPolicy policy, int64_t timeoutMs,
                 SymbolId leader, int64_t budgetMs)
      : clock(std::move(symbols), policy, timeoutMs, leader, budgetMs)
  {
  }
};

}  // namespace

static FeedClockState* toFeedClock(LrvxFeedClockHandle h)
{
  return static_cast<FeedClockState*>(h);
}

LrvxFeedClockHandle lrvx_feed_clock_create(const uint32_t* symbols, uint32_t symbol_count,
                                           uint8_t policy, int64_t timeout_ms,
                                           uint32_t leader_symbol,
                                           int64_t staleness_budget_ms)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  std::vector<SymbolId> sv;
  sv.reserve(symbol_count);
  for (uint32_t i = 0; i < symbol_count; ++i)
  {
    sv.push_back(symbols[i]);
  }
  return new FeedClockState(std::move(sv), static_cast<FeedClockPolicy>(policy), timeout_ms,
                            static_cast<SymbolId>(leader_symbol), staleness_budget_ms);
  LRVX_CAPI_LEAVE;
}

void lrvx_feed_clock_destroy(LrvxFeedClockHandle h)
{
  LRVX_CAPI_ENTER_DESTROY(h);
  delete toFeedClock(h);
  LRVX_CAPI_LEAVE_VOID;
}

uint32_t lrvx_feed_clock_symbol_count(LrvxFeedClockHandle h)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<uint32_t>(toFeedClock(h)->clock.symbolCount());
  LRVX_CAPI_LEAVE;
}

uint32_t lrvx_feed_clock_symbol_at(LrvxFeedClockHandle h, uint32_t index)
{
  LRVX_CAPI_ENTER(h);
  auto& last = toFeedClock(h)->last;
  if (last.symbols.empty())
  {
    // Snapshot not yet populated; tick once to materialize symbols.
    // Use ts=0 + an out-of-band symbol so the tick records nothing.
    toFeedClock(h)->last = toFeedClock(h)->clock.tick(0, 0);
  }
  if (index >= toFeedClock(h)->last.symbols.size())
  {
    return 0;
  }
  return toFeedClock(h)->last.symbols[index];
  LRVX_CAPI_LEAVE;
}

uint8_t lrvx_feed_clock_tick(LrvxFeedClockHandle h, int64_t ts_ns, uint32_t symbol)
{
  LRVX_CAPI_ENTER(h);
  auto* st = toFeedClock(h);
  st->last = st->clock.tick(ts_ns, static_cast<SymbolId>(symbol));
  return st->last.fired ? 1 : 0;
  LRVX_CAPI_LEAVE;
}

uint8_t lrvx_feed_clock_last_fired(LrvxFeedClockHandle h)
{
  LRVX_CAPI_ENTER(h);
  return toFeedClock(h)->last.fired ? 1 : 0;
  LRVX_CAPI_LEAVE;
}

uint32_t lrvx_feed_clock_last_triggered_by(LrvxFeedClockHandle h)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<uint32_t>(toFeedClock(h)->last.triggeredBy);
  LRVX_CAPI_LEAVE;
}

int64_t lrvx_feed_clock_last_seen_at(LrvxFeedClockHandle h, uint32_t index)
{
  LRVX_CAPI_ENTER(h);
  auto& last = toFeedClock(h)->last;
  if (index >= last.lastTsNs.size())
  {
    return 0;
  }
  return last.lastTsNs[index];
  LRVX_CAPI_LEAVE;
}

int64_t lrvx_feed_clock_staleness_at(LrvxFeedClockHandle h, uint32_t index)
{
  LRVX_CAPI_ENTER(h);
  auto& last = toFeedClock(h)->last;
  if (index >= last.stalenessNs.size())
  {
    return 0;
  }
  return last.stalenessNs[index];
  LRVX_CAPI_LEAVE;
}

void lrvx_feed_clock_reset(LrvxFeedClockHandle h)
{
  LRVX_CAPI_ENTER_VOID(h);
  toFeedClock(h)->clock.reset();
  toFeedClock(h)->last = FeedClockSnapshot{};
  LRVX_CAPI_LEAVE_VOID;
}

// ============================================================
// Position tracking
// ============================================================

LrvxPositionTrackerHandle lrvx_position_tracker_create(uint8_t cost_basis)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  auto method = static_cast<CostBasisMethod>(cost_basis);
  return new PositionTracker(0, method);
  LRVX_CAPI_LEAVE;
}

void lrvx_position_tracker_destroy(LrvxPositionTrackerHandle tracker)
{
  LRVX_CAPI_ENTER_DESTROY(tracker);
  delete static_cast<PositionTracker*>(tracker);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_position_tracker_on_fill(LrvxPositionTrackerHandle h, uint32_t symbol, uint8_t side,
                                   double price, double quantity)
{
  LRVX_CAPI_ENTER_VOID(h);
  auto* tracker = static_cast<PositionTracker*>(h);
  Order order{};
  order.symbol = symbol;
  order.side = side == 0 ? Side::BUY : Side::SELL;
  order.price = Price::fromDouble(price);
  order.quantity = Quantity::fromDouble(quantity);
  order.id = 0;
  tracker->onOrderFilled(order);
  LRVX_CAPI_LEAVE_VOID;
}

double lrvx_position_tracker_position(LrvxPositionTrackerHandle h, uint32_t symbol)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<PositionTracker*>(h)->getPosition(symbol).toDouble();
  LRVX_CAPI_LEAVE;
}

double lrvx_position_tracker_avg_entry(LrvxPositionTrackerHandle h, uint32_t symbol)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<PositionTracker*>(h)->getAvgEntryPrice(symbol).toDouble();
  LRVX_CAPI_LEAVE;
}

double lrvx_position_tracker_realized_pnl(LrvxPositionTrackerHandle h, uint32_t symbol)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<PositionTracker*>(h)->getRealizedPnl(symbol).toDouble();
  LRVX_CAPI_LEAVE;
}

double lrvx_position_tracker_total_pnl(LrvxPositionTrackerHandle h)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<PositionTracker*>(h)->getTotalRealizedPnl().toDouble();
  LRVX_CAPI_LEAVE;
}

// ============================================================
// Volume profile
// ============================================================

LrvxVolumeProfileHandle lrvx_volume_profile_create(double tick_size)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  auto* vp = new VolumeProfile<>();
  vp->setTickSize(Price::fromDouble(tick_size));
  return vp;
  LRVX_CAPI_LEAVE;
}

void lrvx_volume_profile_destroy(LrvxVolumeProfileHandle profile)
{
  LRVX_CAPI_ENTER_DESTROY(profile);
  delete static_cast<VolumeProfile<>*>(profile);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_volume_profile_add_trade(LrvxVolumeProfileHandle h, double price, double quantity,
                                   uint8_t is_buy)
{
  LRVX_CAPI_ENTER_VOID(h);
  TradeEvent te;
  te.trade.price = Price::fromDouble(price);
  te.trade.quantity = Quantity::fromDouble(quantity);
  te.trade.isBuy = (is_buy != 0);
  static_cast<VolumeProfile<>*>(h)->addTrade(te);
  LRVX_CAPI_LEAVE_VOID;
}

double lrvx_volume_profile_poc(LrvxVolumeProfileHandle h)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<VolumeProfile<>*>(h)->poc().toDouble();
  LRVX_CAPI_LEAVE;
}

double lrvx_volume_profile_vah(LrvxVolumeProfileHandle h)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<VolumeProfile<>*>(h)->valueAreaHigh().toDouble();
  LRVX_CAPI_LEAVE;
}

double lrvx_volume_profile_val(LrvxVolumeProfileHandle h)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<VolumeProfile<>*>(h)->valueAreaLow().toDouble();
  LRVX_CAPI_LEAVE;
}

double lrvx_volume_profile_total_volume(LrvxVolumeProfileHandle h)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<VolumeProfile<>*>(h)->totalVolume().toDouble();
  LRVX_CAPI_LEAVE;
}

double lrvx_volume_profile_total_delta(LrvxVolumeProfileHandle h)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<VolumeProfile<>*>(h)->totalDelta().toDouble();
  LRVX_CAPI_LEAVE;
}

uint32_t lrvx_volume_profile_num_levels(LrvxVolumeProfileHandle h)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<uint32_t>(static_cast<VolumeProfile<>*>(h)->numLevels());
  LRVX_CAPI_LEAVE;
}

void lrvx_volume_profile_clear(LrvxVolumeProfileHandle h)
{
  LRVX_CAPI_ENTER_VOID(h);
  static_cast<VolumeProfile<>*>(h)->clear();
  LRVX_CAPI_LEAVE_VOID;
}

// ============================================================
// Footprint bar
// ============================================================

LrvxFootprintHandle lrvx_footprint_create(double tick_size)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  auto* fp = new FootprintBar<>();
  fp->setTickSize(Price::fromDouble(tick_size));
  return fp;
  LRVX_CAPI_LEAVE;
}

void lrvx_footprint_destroy(LrvxFootprintHandle footprint)
{
  LRVX_CAPI_ENTER_DESTROY(footprint);
  delete static_cast<FootprintBar<>*>(footprint);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_footprint_add_trade(LrvxFootprintHandle h, double price, double quantity,
                              uint8_t is_buy)
{
  LRVX_CAPI_ENTER_VOID(h);
  TradeEvent te;
  te.trade.price = Price::fromDouble(price);
  te.trade.quantity = Quantity::fromDouble(quantity);
  te.trade.isBuy = (is_buy != 0);
  static_cast<FootprintBar<>*>(h)->addTrade(te);
  LRVX_CAPI_LEAVE_VOID;
}

double lrvx_footprint_total_delta(LrvxFootprintHandle h)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<FootprintBar<>*>(h)->totalDelta().toDouble();
  LRVX_CAPI_LEAVE;
}

double lrvx_footprint_total_volume(LrvxFootprintHandle h)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<FootprintBar<>*>(h)->totalVolume().toDouble();
  LRVX_CAPI_LEAVE;
}

uint32_t lrvx_footprint_num_levels(LrvxFootprintHandle h)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<uint32_t>(static_cast<FootprintBar<>*>(h)->numLevels());
  LRVX_CAPI_LEAVE;
}

void lrvx_footprint_clear(LrvxFootprintHandle h)
{
  LRVX_CAPI_ENTER_VOID(h);
  static_cast<FootprintBar<>*>(h)->clear();
  LRVX_CAPI_LEAVE_VOID;
}

// ============================================================
// Statistics
// ============================================================

double lrvx_stat_correlation(const double* x, const double* y, size_t len)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  double sumX = 0, sumY = 0, sumXY = 0, sumX2 = 0, sumY2 = 0;
  for (size_t i = 0; i < len; i++)
  {
    sumX += x[i];
    sumY += y[i];
    sumXY += x[i] * y[i];
    sumX2 += x[i] * x[i];
    sumY2 += y[i] * y[i];
  }
  double n = static_cast<double>(len);
  double num = n * sumXY - sumX * sumY;
  double den = std::sqrt((n * sumX2 - sumX * sumX) * (n * sumY2 - sumY * sumY));
  return den != 0 ? num / den : 0;
  LRVX_CAPI_LEAVE;
}

double lrvx_stat_profit_factor(const double* pnl, size_t len)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  double gross_profit = 0, gross_loss = 0;
  for (size_t i = 0; i < len; i++)
  {
    if (pnl[i] > 0)
    {
      gross_profit += pnl[i];
    }
    else
    {
      gross_loss -= pnl[i];
    }
  }
  return gross_loss > 0 ? gross_profit / gross_loss : (gross_profit > 0 ? 1e9 : 0);
  LRVX_CAPI_LEAVE;
}

double lrvx_stat_win_rate(const double* pnl, size_t len)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  if (len == 0)
  {
    return 0;
  }
  size_t wins = 0;
  for (size_t i = 0; i < len; i++)
  {
    if (pnl[i] > 0)
    {
      wins++;
    }
  }
  return static_cast<double>(wins) / static_cast<double>(len);
  LRVX_CAPI_LEAVE;
}

// ============================================================
// Order journey tracer
// ============================================================

namespace
{
lrvx::OrderJourneyTracer* asTracer(LrvxOrderJourneyTracerHandle h)
{
  return static_cast<lrvx::OrderJourneyTracer*>(h);
}

LrvxOrderTraceRow toCapiRow(const lrvx::OrderTraceRecord& r)
{
  LrvxOrderTraceRow row{};
  row.order_id = r.orderId;
  row.seq = r.seq;
  row.status = r.status;
  row.is_maker = r.isMaker;
  row.ts_ns = r.tsNs;
  row.fill_qty_raw = r.fillQtyRaw;
  row.fill_price_raw = r.fillPriceRaw;
  row.queue_ahead_raw = r.queueAheadRaw;
  row.queue_total_raw = r.queueTotalRaw;
  row.submitted_at_ns = r.timestamps.submittedAtNs.raw();
  row.accepted_at_ns = r.timestamps.acceptedAtNs.raw();
  row.first_fill_at_ns = r.timestamps.firstFillAtNs.raw();
  row.last_fill_at_ns = r.timestamps.lastFillAtNs.raw();
  row.canceled_at_ns = r.timestamps.canceledAtNs.raw();
  row.rejected_at_ns = r.timestamps.rejectedAtNs.raw();
  row.triggered_at_ns = r.timestamps.triggeredAtNs.raw();
  row.expired_at_ns = r.timestamps.expiredAtNs.raw();
  return row;
}
}  // namespace

LrvxOrderJourneyTracerHandle lrvx_order_journey_tracer_create(
    uint64_t max_orders, uint64_t max_records_per_order, double sample_rate,
    uint64_t sample_salt)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  lrvx::OrderJourneyTracer::Config cfg{};
  cfg.maxOrders = max_orders;
  cfg.maxRecordsPerOrder = max_records_per_order;
  cfg.sampleRate = sample_rate;
  cfg.sampleSalt = sample_salt;
  return new lrvx::OrderJourneyTracer(cfg);
  LRVX_CAPI_LEAVE;
}

void lrvx_order_journey_tracer_destroy(LrvxOrderJourneyTracerHandle h)
{
  LRVX_CAPI_ENTER_DESTROY(h);
  delete asTracer(h);
  LRVX_CAPI_LEAVE_VOID;
}

uint64_t lrvx_order_journey_tracer_order_count(LrvxOrderJourneyTracerHandle h)
{
  LRVX_CAPI_ENTER(h);
  return asTracer(h) ? asTracer(h)->orderCount() : 0u;
  LRVX_CAPI_LEAVE;
}

uint64_t lrvx_order_journey_tracer_record_count(LrvxOrderJourneyTracerHandle h)
{
  LRVX_CAPI_ENTER(h);
  return asTracer(h) ? asTracer(h)->recordCount() : 0u;
  LRVX_CAPI_LEAVE;
}

double lrvx_order_journey_tracer_median_ack_latency_ns(LrvxOrderJourneyTracerHandle h)
{
  LRVX_CAPI_ENTER(h);
  return asTracer(h) ? asTracer(h)->medianAckLatencyNs() : 0.0;
  LRVX_CAPI_LEAVE;
}

double lrvx_order_journey_tracer_median_time_to_first_fill_ns(
    LrvxOrderJourneyTracerHandle h)
{
  LRVX_CAPI_ENTER(h);
  return asTracer(h) ? asTracer(h)->medianTimeToFirstFillNs() : 0.0;
  LRVX_CAPI_LEAVE;
}

double lrvx_order_journey_tracer_maker_fill_ratio(LrvxOrderJourneyTracerHandle h)
{
  LRVX_CAPI_ENTER(h);
  return asTracer(h) ? asTracer(h)->makerFillRatio() : 0.0;
  LRVX_CAPI_LEAVE;
}

double lrvx_order_journey_tracer_cancel_race_loss_rate(LrvxOrderJourneyTracerHandle h)
{
  LRVX_CAPI_ENTER(h);
  return asTracer(h) ? asTracer(h)->cancelRaceLossRate() : 0.0;
  LRVX_CAPI_LEAVE;
}

uint64_t lrvx_order_journey_tracer_result(LrvxOrderJourneyTracerHandle h,
                                          LrvxOrderTraceRow* out, uint64_t max_rows)
{
  LRVX_CAPI_ENTER(h);
  auto* tracer = asTracer(h);
  if (!tracer)
  {
    return 0u;
  }
  auto rows = tracer->result();
  if (out == nullptr || max_rows == 0)
  {
    return rows.size();
  }
  const uint64_t n = std::min<uint64_t>(rows.size(), max_rows);
  for (uint64_t i = 0; i < n; ++i)
  {
    out[i] = toCapiRow(rows[i]);
  }
  return n;
  LRVX_CAPI_LEAVE;
}

uint64_t lrvx_order_journey_tracer_journey(LrvxOrderJourneyTracerHandle h,
                                           uint64_t order_id, LrvxOrderTraceRow* out,
                                           uint64_t max_rows)
{
  LRVX_CAPI_ENTER(h);
  auto* tracer = asTracer(h);
  if (!tracer)
  {
    return 0u;
  }
  auto rows = tracer->journey(static_cast<lrvx::OrderId>(order_id));
  if (out == nullptr || max_rows == 0)
  {
    return rows.size();
  }
  const uint64_t n = std::min<uint64_t>(rows.size(), max_rows);
  for (uint64_t i = 0; i < n; ++i)
  {
    out[i] = toCapiRow(rows[i]);
  }
  return n;
  LRVX_CAPI_LEAVE;
}

void lrvx_order_journey_tracer_clear(LrvxOrderJourneyTracerHandle h)
{
  LRVX_CAPI_ENTER_VOID(h);
  if (auto* tracer = asTracer(h))
  {
    tracer->clear();
  }
  LRVX_CAPI_LEAVE_VOID;
}

// ============================================================
// Order tracker
// ============================================================

LrvxOrderTrackerHandle lrvx_order_tracker_create(void)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  return new OrderTracker();
  LRVX_CAPI_LEAVE;
}

void lrvx_order_tracker_destroy(LrvxOrderTrackerHandle tracker)
{
  LRVX_CAPI_ENTER_DESTROY(tracker);
  delete static_cast<OrderTracker*>(tracker);
  LRVX_CAPI_LEAVE_VOID;
}

uint8_t lrvx_order_tracker_on_submitted(LrvxOrderTrackerHandle h, uint64_t order_id,
                                        uint32_t symbol, uint8_t side, double price, double qty)
{
  LRVX_CAPI_ENTER(h);
  Order order{};
  order.id = order_id;
  order.symbol = symbol;
  order.side = side == 0 ? Side::BUY : Side::SELL;
  order.price = Price::fromDouble(price);
  order.quantity = Quantity::fromDouble(qty);
  return static_cast<OrderTracker*>(h)->onSubmitted(order, "", "") ? 1 : 0;
  LRVX_CAPI_LEAVE;
}

uint8_t lrvx_order_tracker_on_filled(LrvxOrderTrackerHandle h, uint64_t order_id, double fill_qty)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<OrderTracker*>(h)->onFilled(order_id, Quantity::fromDouble(fill_qty)) ? 1
                                                                                           : 0;
  LRVX_CAPI_LEAVE;
}

uint8_t lrvx_order_tracker_on_canceled(LrvxOrderTrackerHandle h, uint64_t order_id)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<OrderTracker*>(h)->onCanceled(order_id) ? 1 : 0;
  LRVX_CAPI_LEAVE;
}

uint8_t lrvx_order_tracker_is_active(LrvxOrderTrackerHandle h, uint64_t order_id)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<OrderTracker*>(h)->isActive(order_id) ? 1 : 0;
  LRVX_CAPI_LEAVE;
}

uint32_t lrvx_order_tracker_active_count(LrvxOrderTrackerHandle h)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<uint32_t>(static_cast<OrderTracker*>(h)->activeOrderCount());
  LRVX_CAPI_LEAVE;
}

uint32_t lrvx_order_tracker_total_count(LrvxOrderTrackerHandle h)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<uint32_t>(static_cast<OrderTracker*>(h)->totalOrderCount());
  LRVX_CAPI_LEAVE;
}

void lrvx_order_tracker_prune(LrvxOrderTrackerHandle h)
{
  LRVX_CAPI_ENTER_VOID(h);
  static_cast<OrderTracker*>(h)->pruneTerminal();
  LRVX_CAPI_LEAVE_VOID;
}

// ============================================================
// Position group tracker
// ============================================================

LrvxPositionGroupHandle lrvx_position_group_create(void)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  return new PositionGroupTracker();
  LRVX_CAPI_LEAVE;
}

void lrvx_position_group_destroy(LrvxPositionGroupHandle h)
{
  LRVX_CAPI_ENTER_DESTROY(h);
  delete static_cast<PositionGroupTracker*>(h);
  LRVX_CAPI_LEAVE_VOID;
}

uint64_t lrvx_position_group_open(LrvxPositionGroupHandle h, uint64_t order_id, uint32_t symbol,
                                  uint8_t side, double price, double qty)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<PositionGroupTracker*>(h)->openPosition(
      order_id, symbol, side == 0 ? Side::BUY : Side::SELL, Price::fromDouble(price),
      Quantity::fromDouble(qty));
  LRVX_CAPI_LEAVE;
}

void lrvx_position_group_close(LrvxPositionGroupHandle h, uint64_t position_id, double exit_price)
{
  LRVX_CAPI_ENTER_VOID(h);
  static_cast<PositionGroupTracker*>(h)->closePosition(position_id,
                                                       Price::fromDouble(exit_price));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_position_group_partial_close(LrvxPositionGroupHandle h, uint64_t position_id, double qty,
                                       double exit_price)
{
  LRVX_CAPI_ENTER_VOID(h);
  static_cast<PositionGroupTracker*>(h)->partialClose(position_id, Quantity::fromDouble(qty),
                                                      Price::fromDouble(exit_price));
  LRVX_CAPI_LEAVE_VOID;
}

double lrvx_position_group_net_position(LrvxPositionGroupHandle h, uint32_t symbol)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<PositionGroupTracker*>(h)->netPosition(symbol).toDouble();
  LRVX_CAPI_LEAVE;
}

double lrvx_position_group_realized_pnl(LrvxPositionGroupHandle h, uint32_t symbol)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<PositionGroupTracker*>(h)->realizedPnl(symbol).toDouble();
  LRVX_CAPI_LEAVE;
}

double lrvx_position_group_total_pnl(LrvxPositionGroupHandle h)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<PositionGroupTracker*>(h)->totalRealizedPnl().toDouble();
  LRVX_CAPI_LEAVE;
}

uint32_t lrvx_position_group_open_count(LrvxPositionGroupHandle h, uint32_t symbol)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<uint32_t>(
      static_cast<PositionGroupTracker*>(h)->openPositionCount(symbol));
  LRVX_CAPI_LEAVE;
}

void lrvx_position_group_prune(LrvxPositionGroupHandle h)
{
  LRVX_CAPI_ENTER_VOID(h);
  static_cast<PositionGroupTracker*>(h)->pruneClosedPositions();
  LRVX_CAPI_LEAVE_VOID;
}

// ============================================================
// Market profile
// ============================================================

LrvxMarketProfileHandle lrvx_market_profile_create(double tick_size, uint32_t period_minutes,
                                                   int64_t session_start_ns)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  auto* mp = new MarketProfile<>();
  mp->setTickSize(Price::fromDouble(tick_size));
  mp->setPeriodDuration(std::chrono::minutes(period_minutes));
  mp->setSessionStart(static_cast<uint64_t>(session_start_ns));
  return mp;
  LRVX_CAPI_LEAVE;
}

void lrvx_market_profile_destroy(LrvxMarketProfileHandle h)
{
  LRVX_CAPI_ENTER_DESTROY(h);
  delete static_cast<MarketProfile<>*>(h);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_market_profile_add_trade(LrvxMarketProfileHandle h, int64_t timestamp_ns, double price,
                                   double qty, uint8_t is_buy)
{
  LRVX_CAPI_ENTER_VOID(h);
  TradeEvent te;
  te.trade.price = Price::fromDouble(price);
  te.trade.quantity = Quantity::fromDouble(qty);
  te.trade.isBuy = (is_buy != 0);
  te.trade.exchangeTsNs = UnixNanos::fromRaw(timestamp_ns);
  static_cast<MarketProfile<>*>(h)->addTrade(te);
  LRVX_CAPI_LEAVE_VOID;
}

double lrvx_market_profile_poc(LrvxMarketProfileHandle h)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<MarketProfile<>*>(h)->poc().toDouble();
  LRVX_CAPI_LEAVE;
}

double lrvx_market_profile_vah(LrvxMarketProfileHandle h)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<MarketProfile<>*>(h)->valueAreaHigh().toDouble();
  LRVX_CAPI_LEAVE;
}

double lrvx_market_profile_val(LrvxMarketProfileHandle h)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<MarketProfile<>*>(h)->valueAreaLow().toDouble();
  LRVX_CAPI_LEAVE;
}

double lrvx_market_profile_ib_high(LrvxMarketProfileHandle h)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<MarketProfile<>*>(h)->initialBalanceHigh().toDouble();
  LRVX_CAPI_LEAVE;
}

double lrvx_market_profile_ib_low(LrvxMarketProfileHandle h)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<MarketProfile<>*>(h)->initialBalanceLow().toDouble();
  LRVX_CAPI_LEAVE;
}

uint8_t lrvx_market_profile_is_poor_high(LrvxMarketProfileHandle h)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<MarketProfile<>*>(h)->isPoorHigh() ? 1 : 0;
  LRVX_CAPI_LEAVE;
}

uint8_t lrvx_market_profile_is_poor_low(LrvxMarketProfileHandle h)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<MarketProfile<>*>(h)->isPoorLow() ? 1 : 0;
  LRVX_CAPI_LEAVE;
}

uint32_t lrvx_market_profile_num_levels(LrvxMarketProfileHandle h)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<uint32_t>(static_cast<MarketProfile<>*>(h)->numLevels());
  LRVX_CAPI_LEAVE;
}

void lrvx_market_profile_clear(LrvxMarketProfileHandle h)
{
  LRVX_CAPI_ENTER_VOID(h);
  static_cast<MarketProfile<>*>(h)->clear();
  LRVX_CAPI_LEAVE_VOID;
}

// ============================================================
// Composite book
// ============================================================

LrvxCompositeBookHandle lrvx_composite_book_create(void)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  return new CompositeBookMatrix<>();
  LRVX_CAPI_LEAVE;
}

void lrvx_composite_book_destroy(LrvxCompositeBookHandle h)
{
  LRVX_CAPI_ENTER_DESTROY(h);
  delete static_cast<CompositeBookMatrix<>*>(h);
  LRVX_CAPI_LEAVE_VOID;
}

uint8_t lrvx_composite_book_best_bid(LrvxCompositeBookHandle h, uint32_t symbol, double* price_out,
                                     double* qty_out)
{
  LRVX_CAPI_ENTER(h);
  auto q = static_cast<CompositeBookMatrix<>*>(h)->bestBid(symbol);
  if (q.valid)
  {
    *price_out = Price::fromRaw(q.priceRaw).toDouble();
    *qty_out = Quantity::fromRaw(q.qtyRaw).toDouble();
    return 1;
  }
  return 0;
  LRVX_CAPI_LEAVE;
}

uint8_t lrvx_composite_book_best_ask(LrvxCompositeBookHandle h, uint32_t symbol, double* price_out,
                                     double* qty_out)
{
  LRVX_CAPI_ENTER(h);
  auto q = static_cast<CompositeBookMatrix<>*>(h)->bestAsk(symbol);
  if (q.valid)
  {
    *price_out = Price::fromRaw(q.priceRaw).toDouble();
    *qty_out = Quantity::fromRaw(q.qtyRaw).toDouble();
    return 1;
  }
  return 0;
  LRVX_CAPI_LEAVE;
}

uint8_t lrvx_composite_book_has_arb(LrvxCompositeBookHandle h, uint32_t symbol)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<CompositeBookMatrix<>*>(h)->hasArbitrageOpportunity(symbol) ? 1 : 0;
  LRVX_CAPI_LEAVE;
}

void lrvx_composite_book_mark_stale(LrvxCompositeBookHandle h, uint32_t exchange, uint32_t symbol)
{
  LRVX_CAPI_ENTER_VOID(h);
  static_cast<CompositeBookMatrix<>*>(h)->markStale(exchange, symbol);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_composite_book_check_staleness(LrvxCompositeBookHandle h, int64_t now_ns,
                                         int64_t threshold_ns)
{
  LRVX_CAPI_ENTER_VOID(h);
  static_cast<CompositeBookMatrix<>*>(h)->checkStaleness(now_ns, threshold_ns);
  LRVX_CAPI_LEAVE_VOID;
}

namespace
{
// Shared by lrvx_composite_book_apply_snapshot/_delta below -- same shape as
// LrvxBookImpl::applyUpdate above, but addressed at one exchange/symbol slot
// of a multi-venue CompositeBookMatrix rather than at a single-venue book.
void applyCompositeBookUpdate(CompositeBookMatrix<>* matrix, uint32_t exchange, uint32_t symbol,
                              const double* bp, const double* bq, size_t bl, const double* ap,
                              const double* aq, size_t al, int64_t recv_ns, BookUpdateType type)
{
  std::byte buf[32768];
  std::pmr::monotonic_buffer_resource res(buf, sizeof(buf));
  BookUpdateEvent ev(&res);
  ev.update.symbol = symbol;
  ev.update.type = type;
  ev.sourceExchange = static_cast<ExchangeId>(exchange);
  ev.recvNs = MonoNanos::fromRaw(static_cast<uint64_t>(recv_ns));
  ev.update.bids.reserve(bl);
  for (size_t i = 0; i < bl; ++i)
  {
    ev.update.bids.push_back({Price::fromDouble(bp[i]), Quantity::fromDouble(bq[i])});
  }
  ev.update.asks.reserve(al);
  for (size_t i = 0; i < al; ++i)
  {
    ev.update.asks.push_back({Price::fromDouble(ap[i]), Quantity::fromDouble(aq[i])});
  }
  matrix->onBookUpdate(ev);
}
}  // namespace

void lrvx_composite_book_apply_snapshot(LrvxCompositeBookHandle h, uint32_t exchange, uint32_t symbol,
                                        const double* bid_prices, const double* bid_qtys,
                                        size_t bid_len, const double* ask_prices,
                                        const double* ask_qtys, size_t ask_len, int64_t recv_ns)
{
  LRVX_CAPI_ENTER_VOID(h);
  applyCompositeBookUpdate(static_cast<CompositeBookMatrix<>*>(h), exchange, symbol, bid_prices,
                           bid_qtys, bid_len, ask_prices, ask_qtys, ask_len, recv_ns,
                           BookUpdateType::SNAPSHOT);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_composite_book_apply_delta(LrvxCompositeBookHandle h, uint32_t exchange, uint32_t symbol,
                                     const double* bid_prices, const double* bid_qtys, size_t bid_len,
                                     const double* ask_prices, const double* ask_qtys, size_t ask_len,
                                     int64_t recv_ns)
{
  LRVX_CAPI_ENTER_VOID(h);
  applyCompositeBookUpdate(static_cast<CompositeBookMatrix<>*>(h), exchange, symbol, bid_prices,
                           bid_qtys, bid_len, ask_prices, ask_qtys, ask_len, recv_ns,
                           BookUpdateType::DELTA);
  LRVX_CAPI_LEAVE_VOID;
}

// ============================================================
// Executor fill access
// ============================================================

uint32_t lrvx_simulated_executor_get_fills(LrvxSimulatedExecutorHandle h, LrvxFill* fills_out, uint32_t max_fills)
{
  LRVX_CAPI_ENTER(h);
  auto& fills = static_cast<LrvxSimulatedExecutorImpl*>(h)->executor.fills();
  uint32_t count = static_cast<uint32_t>(std::min(fills.size(), static_cast<size_t>(max_fills)));
  for (uint32_t i = 0; i < count; i++)
  {
    fills_out[i].order_id = fills[i].orderId;
    fills_out[i].symbol = fills[i].symbol;
    fills_out[i].side = fills[i].side == Side::BUY ? 0 : 1;
    fills_out[i].price_raw = fills[i].price.raw();
    fills_out[i].quantity_raw = fills[i].quantity.raw();
    fills_out[i].timestamp_ns = fills[i].timestampNs.raw();
  }
  return count;
  LRVX_CAPI_LEAVE;
}

// ============================================================
// Additional bar aggregation
// ============================================================

uint32_t lrvx_aggregate_range_bars(const int64_t* ts, const double* px, const double* qty,
                                   const uint8_t* ib, size_t len, double range_size,
                                   LrvxBar* bars_out, uint32_t max_bars)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  auto policy = RangeBarPolicy::fromDouble(range_size);
  return doAggregateC(policy, ts, px, qty, ib, len, bars_out, max_bars);
  LRVX_CAPI_LEAVE;
}

uint32_t lrvx_aggregate_renko_bars(const int64_t* ts, const double* px, const double* qty,
                                   const uint8_t* ib, size_t len, double brick_size,
                                   LrvxBar* bars_out, uint32_t max_bars)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  auto policy = RenkoBarPolicy::fromDouble(brick_size);
  return doAggregateC(policy, ts, px, qty, ib, len, bars_out, max_bars);
  LRVX_CAPI_LEAVE;
}

// ============================================================
// L3 Order book
// ============================================================

LrvxL3BookHandle lrvx_l3_book_create(void)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  return new L3OrderBook<>();
  LRVX_CAPI_LEAVE;
}

void lrvx_l3_book_destroy(LrvxL3BookHandle h)
{
  LRVX_CAPI_ENTER_DESTROY(h);
  delete static_cast<L3OrderBook<>*>(h);
  LRVX_CAPI_LEAVE_VOID;
}

int32_t lrvx_l3_book_add_order(LrvxL3BookHandle h, uint64_t order_id, double price, double quantity,
                               uint8_t side)
{
  LRVX_CAPI_ENTER(h);
  auto status = static_cast<L3OrderBook<>*>(h)->addOrder(
      order_id, Price::fromDouble(price), Quantity::fromDouble(quantity),
      side == 0 ? Side::BUY : Side::SELL);
  return static_cast<int32_t>(status);
  LRVX_CAPI_LEAVE;
}

int32_t lrvx_l3_book_remove_order(LrvxL3BookHandle h, uint64_t order_id)
{
  LRVX_CAPI_ENTER(h);
  auto status = static_cast<L3OrderBook<>*>(h)->removeOrder(order_id);
  return static_cast<int32_t>(status);
  LRVX_CAPI_LEAVE;
}

int32_t lrvx_l3_book_modify_order(LrvxL3BookHandle h, uint64_t order_id, double new_qty)
{
  LRVX_CAPI_ENTER(h);
  auto status =
      static_cast<L3OrderBook<>*>(h)->modifyOrder(order_id, Quantity::fromDouble(new_qty));
  return static_cast<int32_t>(status);
  LRVX_CAPI_LEAVE;
}

uint8_t lrvx_l3_book_best_bid(LrvxL3BookHandle h, double* price_out)
{
  LRVX_CAPI_ENTER(h);
  auto bid = static_cast<L3OrderBook<>*>(h)->bestBid();
  if (bid)
  {
    *price_out = bid->toDouble();
    return 1;
  }
  return 0;
  LRVX_CAPI_LEAVE;
}

uint8_t lrvx_l3_book_best_ask(LrvxL3BookHandle h, double* price_out)
{
  LRVX_CAPI_ENTER(h);
  auto ask = static_cast<L3OrderBook<>*>(h)->bestAsk();
  if (ask)
  {
    *price_out = ask->toDouble();
    return 1;
  }
  return 0;
  LRVX_CAPI_LEAVE;
}

double lrvx_l3_book_bid_at_price(LrvxL3BookHandle h, double price)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<L3OrderBook<>*>(h)->bidAtPrice(Price::fromDouble(price)).toDouble();
  LRVX_CAPI_LEAVE;
}

double lrvx_l3_book_ask_at_price(LrvxL3BookHandle h, double price)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<L3OrderBook<>*>(h)->askAtPrice(Price::fromDouble(price)).toDouble();
  LRVX_CAPI_LEAVE;
}

// ============================================================
// Data writer
// ============================================================

LrvxDataWriterHandle lrvx_data_writer_create(const char* output_dir, uint64_t max_segment_mb,
                                             uint8_t exchange_id)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  LRVX_CAPI_REQUIRE(output_dir);
  replay::WriterConfig cfg;
  cfg.output_dir = output_dir;
  cfg.max_segment_bytes = max_segment_mb * 1024 * 1024;
  cfg.exchange_id = exchange_id;
  return new replay::BinaryLogWriter(cfg);
  LRVX_CAPI_LEAVE;
}

void lrvx_data_writer_destroy(LrvxDataWriterHandle h)
{
  LRVX_CAPI_ENTER_DESTROY(h);
  auto* w = static_cast<replay::BinaryLogWriter*>(h);
  w->close();
  delete w;
  LRVX_CAPI_LEAVE_VOID;
}

uint8_t lrvx_data_writer_write_trade(LrvxDataWriterHandle h, int64_t exchange_ts_ns,
                                     int64_t recv_ts_ns, double price, double qty,
                                     uint64_t trade_id, uint32_t symbol_id, uint8_t side)
{
  LRVX_CAPI_ENTER(h);
  replay::TradeRecord tr{};
  tr.exchange_ts_ns = exchange_ts_ns;
  tr.recv_ts_ns = recv_ts_ns;
  tr.price_raw = Price::fromDouble(price).raw();
  tr.qty_raw = Quantity::fromDouble(qty).raw();
  tr.trade_id = trade_id;
  tr.symbol_id = symbol_id;
  tr.side = side;
  return static_cast<replay::BinaryLogWriter*>(h)->writeTrade(tr) ? 1 : 0;
  LRVX_CAPI_LEAVE;
}

uint8_t lrvx_data_writer_write_book(LrvxDataWriterHandle h,
                                    int64_t exchange_ts_ns,
                                    int64_t recv_ts_ns,
                                    int64_t seq,
                                    uint32_t symbol_id,
                                    uint8_t is_snapshot,
                                    const LrvxBookLevel* bids, uint32_t n_bids,
                                    const LrvxBookLevel* asks, uint32_t n_asks)
{
  LRVX_CAPI_ENTER(h);
  replay::BookRecordHeader header{};
  header.exchange_ts_ns = exchange_ts_ns;
  header.recv_ts_ns = recv_ts_ns;
  header.seq = seq;
  header.symbol_id = symbol_id;
  header.bid_count = static_cast<uint16_t>(n_bids);
  header.ask_count = static_cast<uint16_t>(n_asks);
  header.type = is_snapshot ? 0 : 1;

  // LrvxBookLevel and replay::BookLevel are layout-compatible
  // (int64 price_raw, int64 qty_raw). Cast is intentional.
  auto* bid_levels = reinterpret_cast<const replay::BookLevel*>(bids);
  auto* ask_levels = reinterpret_cast<const replay::BookLevel*>(asks);
  std::span<const replay::BookLevel> bid_span(bid_levels, n_bids);
  std::span<const replay::BookLevel> ask_span(ask_levels, n_asks);

  return static_cast<replay::BinaryLogWriter*>(h)->writeBook(header, bid_span, ask_span) ? 1 : 0;
  LRVX_CAPI_LEAVE;
}

uint64_t lrvx_data_writer_write_books(LrvxDataWriterHandle h,
                                      const LrvxBookUpdateHeader* headers,
                                      uint64_t n_events,
                                      const LrvxLevel* levels,
                                      uint64_t /*total_levels*/)
{
  LRVX_CAPI_ENTER(h);
  auto* writer = static_cast<replay::BinaryLogWriter*>(h);
  if (!writer || !headers)
  {
    return 0;
  }

  // LrvxLevel has an extra `side` byte we ignore on write — copy into
  // contiguous replay::BookLevel buffers per event.
  std::vector<replay::BookLevel> scratch;
  uint64_t written = 0;
  for (uint64_t i = 0; i < n_events; ++i)
  {
    const auto& fh = headers[i];
    const uint64_t total = static_cast<uint64_t>(fh.bid_count) + fh.ask_count;
    scratch.clear();
    scratch.reserve(total);
    for (uint64_t k = 0; k < total; ++k)
    {
      const auto& lv = levels[fh.level_offset + k];
      scratch.push_back({lv.price_raw, lv.qty_raw});
    }

    replay::BookRecordHeader rh{};
    rh.exchange_ts_ns = fh.exchange_ts_ns;
    rh.recv_ts_ns = fh.recv_ts_ns;
    rh.seq = fh.seq;
    rh.symbol_id = fh.symbol_id;
    rh.bid_count = fh.bid_count;
    rh.ask_count = fh.ask_count;
    rh.type = (fh.event_type == 2) ? 0 : 1;

    std::span<const replay::BookLevel> bid_span(scratch.data(), fh.bid_count);
    std::span<const replay::BookLevel> ask_span(scratch.data() + fh.bid_count, fh.ask_count);
    if (writer->writeBook(rh, bid_span, ask_span))
    {
      ++written;
    }
  }
  return written;
  LRVX_CAPI_LEAVE;
}

void lrvx_data_writer_flush(LrvxDataWriterHandle h)
{
  LRVX_CAPI_ENTER_VOID(h);
  static_cast<replay::BinaryLogWriter*>(h)->flush();
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_data_writer_close(LrvxDataWriterHandle h)
{
  LRVX_CAPI_ENTER_VOID(h);
  static_cast<replay::BinaryLogWriter*>(h)->close();
  LRVX_CAPI_LEAVE_VOID;
}

// ============================================================
// Data reader
// ============================================================

LrvxDataReaderHandle lrvx_data_reader_create(const char* data_dir)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  LRVX_CAPI_REQUIRE(data_dir);
  replay::ReaderConfig cfg;
  cfg.data_dir = data_dir;
  return new replay::BinaryLogReader(cfg);
  LRVX_CAPI_LEAVE;
}

void lrvx_data_reader_destroy(LrvxDataReaderHandle h)
{
  LRVX_CAPI_ENTER_DESTROY(h);
  delete static_cast<replay::BinaryLogReader*>(h);
  LRVX_CAPI_LEAVE_VOID;
}

uint64_t lrvx_data_reader_count(LrvxDataReaderHandle h)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<replay::BinaryLogReader*>(h)->count();
  LRVX_CAPI_LEAVE;
}

// ============================================================
// Order book level access
// ============================================================

uint32_t lrvx_book_get_bids(LrvxBookHandle h, double* prices_out, double* qtys_out,
                            uint32_t max_levels)
{
  LRVX_CAPI_ENTER(h);
  auto levels = static_cast<LrvxBookImpl*>(h)->book.getBidLevels(max_levels);
  uint32_t count = static_cast<uint32_t>(levels.size());
  for (uint32_t i = 0; i < count; i++)
  {
    prices_out[i] = levels[i].price.toDouble();
    qtys_out[i] = levels[i].quantity.toDouble();
  }
  return count;
  LRVX_CAPI_LEAVE;
}

uint32_t lrvx_book_get_asks(LrvxBookHandle h, double* prices_out, double* qtys_out,
                            uint32_t max_levels)
{
  LRVX_CAPI_ENTER(h);
  auto levels = static_cast<LrvxBookImpl*>(h)->book.getAskLevels(max_levels);
  uint32_t count = static_cast<uint32_t>(levels.size());
  for (uint32_t i = 0; i < count; i++)
  {
    prices_out[i] = levels[i].price.toDouble();
    qtys_out[i] = levels[i].quantity.toDouble();
  }
  return count;
  LRVX_CAPI_LEAVE;
}

// ============================================================
// Heikin-Ashi bar aggregation
// ============================================================

uint32_t lrvx_aggregate_heikin_ashi_bars(const int64_t* ts, const double* px, const double* qty,
                                         const uint8_t* ib, size_t len, double interval_seconds,
                                         LrvxBar* bars_out, uint32_t max_bars)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  HeikinAshiBarPolicy policy(
      std::chrono::nanoseconds(static_cast<int64_t>(interval_seconds * 1e9)));
  return doAggregateC(policy, ts, px, qty, ib, len, bars_out, max_bars);
  LRVX_CAPI_LEAVE;
}

// ============================================================
// Additional stats
// ============================================================

double lrvx_stat_permutation_test(const double* group1, size_t len1, const double* group2,
                                  size_t len2, uint32_t num_permutations)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  auto mean = [](const double* d, size_t n)
  {
    double s = 0;
    for (size_t i = 0; i < n; i++)
    {
      s += d[i];
    }
    return n > 0 ? s / static_cast<double>(n) : 0.0;
  };

  double observed = std::abs(mean(group1, len1) - mean(group2, len2));

  std::vector<double> combined(len1 + len2);
  std::copy(group1, group1 + len1, combined.begin());
  std::copy(group2, group2 + len2, combined.begin() + len1);

  std::mt19937 rng(42);
  uint32_t count = 0;
  for (uint32_t i = 0; i < num_permutations; i++)
  {
    std::shuffle(combined.begin(), combined.end(), rng);
    double diff = std::abs(mean(combined.data(), len1) - mean(combined.data() + len1, len2));
    if (diff >= observed)
    {
      count++;
    }
  }
  return static_cast<double>(count) / static_cast<double>(num_permutations);
  LRVX_CAPI_LEAVE;
}

void lrvx_stat_bootstrap_ci(const double* data, size_t len, double confidence,
                            uint32_t num_samples, double* lower_out, double* median_out,
                            double* upper_out)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  std::mt19937 rng(42);
  std::uniform_int_distribution<size_t> dist(0, len - 1);

  std::vector<double> means(num_samples);
  for (uint32_t s = 0; s < num_samples; s++)
  {
    double sum = 0;
    for (size_t i = 0; i < len; i++)
    {
      sum += data[dist(rng)];
    }
    means[s] = sum / static_cast<double>(len);
  }
  std::sort(means.begin(), means.end());

  double alpha = (1.0 - confidence) / 2.0;
  size_t lo = static_cast<size_t>(alpha * num_samples);
  size_t hi = static_cast<size_t>((1.0 - alpha) * num_samples);
  size_t mid = num_samples / 2;

  *lower_out = means[lo];
  *median_out = means[mid];
  *upper_out = means[std::min(hi, static_cast<size_t>(num_samples - 1))];
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_stat_whites_reality_check(const double* returns, size_t num_strategies,
                                    size_t num_periods, uint32_t num_bootstrap,
                                    double avg_block_size, double* p_value_out,
                                    double* best_stat_out, int32_t* best_index_out)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  auto result = lrvx::stats::whitesRealityCheck(
      returns, num_strategies, num_periods, num_bootstrap, avg_block_size);
  if (p_value_out)
  {
    *p_value_out = result.pValue;
  }
  if (best_stat_out)
  {
    *best_stat_out = result.bestStat;
  }
  if (best_index_out)
  {
    *best_index_out = result.bestIndex;
  }
  LRVX_CAPI_LEAVE_VOID;
}

// ============================================================
// Segment operations
// ============================================================

uint8_t lrvx_segment_validate(const char* path)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  LRVX_CAPI_REQUIRE(path);
  replay::SegmentValidator validator;
  auto result = validator.validate(path);
  return result.valid ? 1 : 0;
  LRVX_CAPI_LEAVE;
}

uint8_t lrvx_segment_merge(const char* input_dir, const char* output_path)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  LRVX_CAPI_REQUIRE(input_dir);
  LRVX_CAPI_REQUIRE(output_path);
  replay::MergeConfig cfg;
  cfg.output_dir = output_path;
  auto result = replay::SegmentOps::mergeDirectory(input_dir, cfg);
  return result.segments_merged > 0 ? 1 : 0;
  LRVX_CAPI_LEAVE;
}

// ============================================================
// Backtest: slippage, queue, result, metrics, equity curve
// ============================================================

namespace
{
SlippageProfile makeSlippageProfile(int32_t model, int32_t ticks, double tick_size,
                                    double bps, double impact_coeff)
{
  SlippageProfile prof;
  prof.model = static_cast<SlippageModel>(model);
  prof.ticks = ticks;
  prof.tickSize = (tick_size > 0.0) ? Price::fromDouble(tick_size) : Price{};
  prof.bps = bps;
  prof.impactCoeff = impact_coeff;
  return prof;
}
}  // namespace

void lrvx_simulated_executor_set_default_slippage(LrvxSimulatedExecutorHandle h, int32_t model, int32_t ticks,
                                                  double tick_size, double bps, double impact_coeff)
{
  LRVX_CAPI_ENTER_VOID(h);
  static_cast<LrvxSimulatedExecutorImpl*>(h)->executor.setDefaultSlippage(
      makeSlippageProfile(model, ticks, tick_size, bps, impact_coeff));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_simulated_executor_set_symbol_slippage(LrvxSimulatedExecutorHandle h, uint32_t symbol, int32_t model,
                                                 int32_t ticks, double tick_size, double bps,
                                                 double impact_coeff)
{
  LRVX_CAPI_ENTER_VOID(h);
  static_cast<LrvxSimulatedExecutorImpl*>(h)->executor.setSymbolSlippage(
      symbol, makeSlippageProfile(model, ticks, tick_size, bps, impact_coeff));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_simulated_executor_set_queue_model(LrvxSimulatedExecutorHandle h, int32_t model, uint32_t depth)
{
  LRVX_CAPI_ENTER_VOID(h);
  static_cast<LrvxSimulatedExecutorImpl*>(h)->executor.setQueueModel(
      static_cast<QueueModel>(model), depth);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_simulated_executor_set_queue_fifo_top_n(LrvxSimulatedExecutorHandle h, uint32_t top_n)
{
  LRVX_CAPI_ENTER_VOID(h);
  static_cast<LrvxSimulatedExecutorImpl*>(h)->executor.setQueueFifoTopN(top_n);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_simulated_executor_set_top_priority_share(LrvxSimulatedExecutorHandle h,
                                                    double share)
{
  LRVX_CAPI_ENTER_VOID(h);
  static_cast<LrvxSimulatedExecutorImpl*>(h)->executor.setTopPriorityShare(share);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_simulated_executor_set_lmm_orders(LrvxSimulatedExecutorHandle h,
                                            const uint64_t* ids, uint32_t n_ids)
{
  LRVX_CAPI_ENTER_VOID(h);
  std::vector<OrderId> v;
  v.reserve(n_ids);
  for (uint32_t i = 0; i < n_ids; ++i)
  {
    v.push_back(static_cast<OrderId>(ids ? ids[i] : 0));
  }
  static_cast<LrvxSimulatedExecutorImpl*>(h)->executor.setLmmOrders(v);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_simulated_executor_set_lmm_bonus_multiplier(LrvxSimulatedExecutorHandle h,
                                                      double multiplier)
{
  LRVX_CAPI_ENTER_VOID(h);
  static_cast<LrvxSimulatedExecutorImpl*>(h)->executor.setLmmBonusMultiplier(multiplier);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_simulated_executor_set_order_priority_multiplier(LrvxSimulatedExecutorHandle h,
                                                           uint64_t order_id,
                                                           double multiplier)
{
  LRVX_CAPI_ENTER_VOID(h);
  static_cast<LrvxSimulatedExecutorImpl*>(h)->executor.setOrderPriorityMultiplier(
      static_cast<OrderId>(order_id), multiplier);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_simulated_executor_set_submit_ack_latency(LrvxSimulatedExecutorHandle h,
                                                    int64_t latency_ns, int64_t jitter_ns)
{
  LRVX_CAPI_ENTER_VOID(h);
  static_cast<LrvxSimulatedExecutorImpl*>(h)->executor.setSubmitAckLatency(latency_ns, jitter_ns);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_simulated_executor_set_cancel_ack_latency(LrvxSimulatedExecutorHandle h,
                                                    int64_t latency_ns, int64_t jitter_ns)
{
  LRVX_CAPI_ENTER_VOID(h);
  static_cast<LrvxSimulatedExecutorImpl*>(h)->executor.setCancelAckLatency(latency_ns, jitter_ns);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_simulated_executor_set_replace_ack_latency(LrvxSimulatedExecutorHandle h,
                                                     int64_t latency_ns, int64_t jitter_ns)
{
  LRVX_CAPI_ENTER_VOID(h);
  static_cast<LrvxSimulatedExecutorImpl*>(h)->executor.setReplaceAckLatency(latency_ns, jitter_ns);
  LRVX_CAPI_LEAVE_VOID;
}

int lrvx_simulated_executor_apply_latency_profile(LrvxSimulatedExecutorHandle h,
                                                  const char* profile_name)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<LrvxSimulatedExecutorImpl*>(h)->executor.applyLatencyProfile(
             profile_name)
             ? 1
             : 0;
  LRVX_CAPI_LEAVE;
}

void lrvx_simulated_executor_set_stp_mode(LrvxSimulatedExecutorHandle h, uint8_t mode)
{
  LRVX_CAPI_ENTER_VOID(h);
  static_cast<LrvxSimulatedExecutorImpl*>(h)->executor.setSTPMode(
      static_cast<STPMode>(mode));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_simulated_executor_set_fok_mode(LrvxSimulatedExecutorHandle h, uint8_t mode)
{
  LRVX_CAPI_ENTER_VOID(h);
  const SimulatedExecutor::FokMode m =
      (mode == 1) ? SimulatedExecutor::FokMode::SinglePrice
                  : SimulatedExecutor::FokMode::AnyPrice;
  static_cast<LrvxSimulatedExecutorImpl*>(h)->executor.setFokMode(m);
  LRVX_CAPI_LEAVE_VOID;
}

uint8_t lrvx_simulated_executor_fok_mode(LrvxSimulatedExecutorHandle h)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<uint8_t>(
      static_cast<LrvxSimulatedExecutorImpl*>(h)->executor.fokMode());
  LRVX_CAPI_LEAVE;
}

void lrvx_simulated_executor_set_stp_group_membership(LrvxSimulatedExecutorHandle h,
                                                      uint64_t account_id,
                                                      uint64_t group_id)
{
  LRVX_CAPI_ENTER_VOID(h);
  static_cast<LrvxSimulatedExecutorImpl*>(h)->executor.setSTPGroupMembership(
      account_id, group_id);
  LRVX_CAPI_LEAVE_VOID;
}

uint64_t lrvx_simulated_executor_stp_group_for(LrvxSimulatedExecutorHandle h,
                                               uint64_t account_id)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<LrvxSimulatedExecutorImpl*>(h)->executor.stpGroupFor(account_id);
  LRVX_CAPI_LEAVE;
}

// Latency distribution handle bridging.

namespace
{
inline LatencyDistribution* toDist(LrvxLatencyDistributionHandle h)
{
  return static_cast<LatencyDistribution*>(h);
}
}  // namespace

LrvxLatencyDistributionHandle lrvx_latency_distribution_create(void)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  return new LatencyDistribution();
  LRVX_CAPI_LEAVE;
}

void lrvx_latency_distribution_destroy(LrvxLatencyDistributionHandle h)
{
  LRVX_CAPI_ENTER_DESTROY(h);
  delete toDist(h);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_latency_distribution_set_constant(LrvxLatencyDistributionHandle h, int64_t ns)
{
  LRVX_CAPI_ENTER_VOID(h);
  const double rho = toDist(h)->burstCorrelation();
  *toDist(h) = LatencyDistribution::constant(ns);
  toDist(h)->setBurstCorrelation(rho);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_latency_distribution_set_uniform(LrvxLatencyDistributionHandle h, int64_t lo_ns,
                                           int64_t hi_ns)
{
  LRVX_CAPI_ENTER_VOID(h);
  const double rho = toDist(h)->burstCorrelation();
  *toDist(h) = LatencyDistribution::uniform(lo_ns, hi_ns);
  toDist(h)->setBurstCorrelation(rho);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_latency_distribution_set_lognormal(LrvxLatencyDistributionHandle h,
                                             int64_t median_ns, double sigma)
{
  LRVX_CAPI_ENTER_VOID(h);
  const double rho = toDist(h)->burstCorrelation();
  *toDist(h) = LatencyDistribution::lognormal(median_ns, sigma);
  toDist(h)->setBurstCorrelation(rho);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_latency_distribution_set_empirical(LrvxLatencyDistributionHandle h,
                                             const int64_t* samples_ns, uint32_t n_samples)
{
  LRVX_CAPI_ENTER_VOID(h);
  const double rho = toDist(h)->burstCorrelation();
  std::vector<int64_t> samples;
  samples.reserve(n_samples);
  for (uint32_t i = 0; i < n_samples; ++i)
  {
    samples.push_back(samples_ns ? samples_ns[i] : 0);
  }
  *toDist(h) = LatencyDistribution::empirical(samples);
  toDist(h)->setBurstCorrelation(rho);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_latency_distribution_set_burst_correlation(LrvxLatencyDistributionHandle h, double rho)
{
  LRVX_CAPI_ENTER_VOID(h);
  toDist(h)->setBurstCorrelation(rho);
  LRVX_CAPI_LEAVE_VOID;
}

int64_t lrvx_latency_distribution_median_ns(LrvxLatencyDistributionHandle h)
{
  LRVX_CAPI_ENTER(h);
  return toDist(h)->medianNs();
  LRVX_CAPI_LEAVE;
}

void lrvx_simulated_executor_set_submit_ack_latency_distribution(
    LrvxSimulatedExecutorHandle h, LrvxLatencyDistributionHandle dist)
{
  LRVX_CAPI_ENTER_VOID(h);
  static_cast<LrvxSimulatedExecutorImpl*>(h)->executor.setSubmitAckLatencyDistribution(
      *toDist(dist));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_simulated_executor_set_cancel_ack_latency_distribution(
    LrvxSimulatedExecutorHandle h, LrvxLatencyDistributionHandle dist)
{
  LRVX_CAPI_ENTER_VOID(h);
  static_cast<LrvxSimulatedExecutorImpl*>(h)->executor.setCancelAckLatencyDistribution(
      *toDist(dist));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_simulated_executor_set_replace_ack_latency_distribution(
    LrvxSimulatedExecutorHandle h, LrvxLatencyDistributionHandle dist)
{
  LRVX_CAPI_ENTER_VOID(h);
  static_cast<LrvxSimulatedExecutorImpl*>(h)->executor.setReplaceAckLatencyDistribution(
      *toDist(dist));
  LRVX_CAPI_LEAVE_VOID;
}

// ====== Rate-limit policy bridging ======

#include "lrvx/backtest/rate_limit_policy.h"
#include "lrvx/backtest/venue_availability.h"

namespace
{
inline lrvx::RateLimitPolicy* toRateLimit(LrvxRateLimitPolicyHandle h)
{
  return static_cast<lrvx::RateLimitPolicy*>(h);
}
}  // namespace

extern "C" LrvxRateLimitPolicyHandle lrvx_rate_limit_policy_create(void)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  return new lrvx::RateLimitPolicy();
  LRVX_CAPI_LEAVE;
}

extern "C" void lrvx_rate_limit_policy_destroy(LrvxRateLimitPolicyHandle h)
{
  LRVX_CAPI_ENTER_DESTROY(h);
  delete toRateLimit(h);
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" void lrvx_rate_limit_policy_add_bucket(LrvxRateLimitPolicyHandle h,
                                                  const char* name, int64_t window_ns,
                                                  uint32_t capacity, uint32_t submit_w,
                                                  uint32_t cancel_w, uint32_t replace_w)
{
  LRVX_CAPI_ENTER_VOID(h);
  toRateLimit(h)->addBucket(name ? std::string(name) : std::string("bucket"), window_ns,
                            capacity, submit_w, cancel_w, replace_w);
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" void lrvx_rate_limit_policy_add_bucket_family(
    LrvxRateLimitPolicyHandle h, const char* name, int64_t window_ns,
    uint32_t capacity, uint32_t submit_w, uint32_t cancel_w, uint32_t replace_w,
    uint8_t family, uint32_t query_w)
{
  LRVX_CAPI_ENTER_VOID(h);
  RateLimitPolicy::EndpointFamily f = RateLimitPolicy::EndpointFamily::Trading;
  switch (family)
  {
    case 1:
      f = RateLimitPolicy::EndpointFamily::MarketData;
      break;
    case 2:
      f = RateLimitPolicy::EndpointFamily::Account;
      break;
    default:
      f = RateLimitPolicy::EndpointFamily::Trading;
      break;
  }
  toRateLimit(h)->addBucket(name ? std::string(name) : std::string("bucket"), window_ns,
                            capacity, submit_w, cancel_w, replace_w, f, query_w);
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" void lrvx_rate_limit_policy_set_ban(LrvxRateLimitPolicyHandle h,
                                               uint32_t after_consecutive_rejects,
                                               int64_t ban_duration_ns)
{
  LRVX_CAPI_ENTER_VOID(h);
  toRateLimit(h)->setBan(after_consecutive_rejects, ban_duration_ns);
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" int lrvx_rate_limit_policy_load_profile(LrvxRateLimitPolicyHandle h,
                                                   const char* name)
{
  LRVX_CAPI_ENTER(h);
  if (!h || !name)
  {
    return 0;
  }
  std::string n = name;
  if (n == "binance_um_futures")
  {
    *toRateLimit(h) = lrvx::RateLimitPolicy::binance_um_futures();
  }
  else if (n == "bybit_linear")
  {
    *toRateLimit(h) = lrvx::RateLimitPolicy::bybit_linear();
  }
  else if (n == "okx_swap")
  {
    *toRateLimit(h) = lrvx::RateLimitPolicy::okx_swap();
  }
  else if (n == "deribit")
  {
    *toRateLimit(h) = lrvx::RateLimitPolicy::deribit();
  }
  else
  {
    return 0;
  }
  return 1;
  LRVX_CAPI_LEAVE;
}

extern "C" int64_t lrvx_rate_limit_policy_ban_until_ns(LrvxRateLimitPolicyHandle h)
{
  LRVX_CAPI_ENTER(h);
  return toRateLimit(h)->banUntilNs();
  LRVX_CAPI_LEAVE;
}

extern "C" uint32_t lrvx_rate_limit_policy_consecutive_rejects(LrvxRateLimitPolicyHandle h)
{
  LRVX_CAPI_ENTER(h);
  return toRateLimit(h)->consecutiveRejects();
  LRVX_CAPI_LEAVE;
}

extern "C" uint32_t lrvx_rate_limit_policy_bucket_state(LrvxRateLimitPolicyHandle h,
                                                        int64_t now_ns, int64_t* out_buf,
                                                        uint32_t max_buckets)
{
  LRVX_CAPI_ENTER(h);
  auto states = toRateLimit(h)->bucketStates(now_ns);
  if (out_buf == nullptr || max_buckets == 0)
  {
    return static_cast<uint32_t>(states.size());
  }
  uint32_t n = std::min<uint32_t>(max_buckets, static_cast<uint32_t>(states.size()));
  for (uint32_t i = 0; i < n; ++i)
  {
    out_buf[i * 4 + 0] = states[i].windowNs;
    out_buf[i * 4 + 1] = static_cast<int64_t>(states[i].used);
    out_buf[i * 4 + 2] = static_cast<int64_t>(states[i].capacity);
    out_buf[i * 4 + 3] = 0;
  }
  return n;
  LRVX_CAPI_LEAVE;
}

extern "C" void lrvx_simulated_executor_set_rate_limit_policy(
    LrvxSimulatedExecutorHandle exec_h, LrvxRateLimitPolicyHandle policy_h)
{
  LRVX_CAPI_ENTER_VOID(exec_h);
  static_cast<LrvxSimulatedExecutorImpl*>(exec_h)->executor.setRateLimitPolicy(
      *toRateLimit(policy_h));
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" void lrvx_simulated_executor_clear_rate_limit_policy(
    LrvxSimulatedExecutorHandle exec_h)
{
  LRVX_CAPI_ENTER_VOID(exec_h);
  static_cast<LrvxSimulatedExecutorImpl*>(exec_h)->executor.clearRateLimitPolicy();
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" LrvxVenueAvailabilityHandle lrvx_venue_availability_create(void)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  return new VenueAvailability();
  LRVX_CAPI_LEAVE;
}

extern "C" void lrvx_venue_availability_destroy(LrvxVenueAvailabilityHandle h)
{
  LRVX_CAPI_ENTER_DESTROY(h);
  delete static_cast<VenueAvailability*>(h);
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" void lrvx_venue_availability_schedule_outage(LrvxVenueAvailabilityHandle h,
                                                        int64_t start_ns, int64_t duration_ns,
                                                        uint8_t policy, int64_t gtc_ttl_ns)
{
  LRVX_CAPI_ENTER_VOID(h);
  static_cast<VenueAvailability*>(h)->scheduleOutage(start_ns, duration_ns,
                                                     static_cast<OnOutage>(policy),
                                                     gtc_ttl_ns);
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" void lrvx_venue_availability_auto_random_outages(LrvxVenueAvailabilityHandle h,
                                                            double per_day,
                                                            int64_t mean_duration_ns,
                                                            uint8_t policy, uint64_t seed)
{
  LRVX_CAPI_ENTER_VOID(h);
  static_cast<VenueAvailability*>(h)->autoRandomOutages(per_day, mean_duration_ns,
                                                        static_cast<OnOutage>(policy),
                                                        seed);
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" uint8_t lrvx_venue_availability_is_up(LrvxVenueAvailabilityHandle h, int64_t now_ns)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<VenueAvailability*>(h)->isUp(now_ns) ? 1 : 0;
  LRVX_CAPI_LEAVE;
}

extern "C" void lrvx_venue_availability_schedule_outage_ex(
    LrvxVenueAvailabilityHandle h, int64_t start_ns, int64_t duration_ns,
    uint8_t outage_type, uint8_t policy, int64_t gtc_ttl_ns,
    double degradation_latency_multiplier, double wrong_side_recovery_bps)
{
  LRVX_CAPI_ENTER_VOID(h);
  static_cast<VenueAvailability*>(h)->scheduleOutageEx(
      start_ns, duration_ns, static_cast<OutageType>(outage_type),
      static_cast<OnOutage>(policy), gtc_ttl_ns,
      degradation_latency_multiplier, wrong_side_recovery_bps);
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" uint8_t lrvx_venue_availability_submits_allowed(LrvxVenueAvailabilityHandle h,
                                                           int64_t now_ns)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<VenueAvailability*>(h)->submitsAllowed(now_ns) ? 1 : 0;
  LRVX_CAPI_LEAVE;
}

extern "C" uint8_t lrvx_venue_availability_cancels_allowed(LrvxVenueAvailabilityHandle h,
                                                           int64_t now_ns)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<VenueAvailability*>(h)->cancelsAllowed(now_ns) ? 1 : 0;
  LRVX_CAPI_LEAVE;
}

extern "C" uint8_t lrvx_venue_availability_book_updates_allowed(
    LrvxVenueAvailabilityHandle h, int64_t now_ns)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<VenueAvailability*>(h)->bookUpdatesAllowed(now_ns) ? 1 : 0;
  LRVX_CAPI_LEAVE;
}

extern "C" uint8_t lrvx_venue_availability_trades_allowed(LrvxVenueAvailabilityHandle h,
                                                          int64_t now_ns)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<VenueAvailability*>(h)->tradesAllowed(now_ns) ? 1 : 0;
  LRVX_CAPI_LEAVE;
}

extern "C" double lrvx_venue_availability_latency_multiplier(
    LrvxVenueAvailabilityHandle h, int64_t now_ns)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<VenueAvailability*>(h)->latencyMultiplier(now_ns);
  LRVX_CAPI_LEAVE;
}

extern "C" double lrvx_venue_availability_consume_wrong_side_recovery_bps(
    LrvxVenueAvailabilityHandle h)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<VenueAvailability*>(h)->consumeWrongSideRecoveryBps();
  LRVX_CAPI_LEAVE;
}

extern "C" void lrvx_simulated_executor_set_venue_availability(
    LrvxSimulatedExecutorHandle exec_h, LrvxVenueAvailabilityHandle va_h)
{
  LRVX_CAPI_ENTER_VOID(exec_h);
  static_cast<LrvxSimulatedExecutorImpl*>(exec_h)->executor.setVenueAvailability(
      static_cast<VenueAvailability*>(va_h));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_simulated_executor_on_trade_qty(LrvxSimulatedExecutorHandle h, uint32_t symbol, double price,
                                          double quantity, uint8_t is_buy)
{
  LRVX_CAPI_ENTER_VOID(h);
  static_cast<LrvxSimulatedExecutorImpl*>(h)->executor.onTrade(
      symbol, Price::fromDouble(price), Quantity::fromDouble(quantity), is_buy != 0);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_simulated_executor_on_best_levels(LrvxSimulatedExecutorHandle h, uint32_t symbol, double bid_price,
                                            double bid_qty, double ask_price, double ask_qty)
{
  LRVX_CAPI_ENTER_VOID(h);
  std::pmr::monotonic_buffer_resource pool(512);
  std::pmr::vector<BookLevel> bids(&pool);
  std::pmr::vector<BookLevel> asks(&pool);
  bids.emplace_back(Price::fromDouble(bid_price), Quantity::fromDouble(bid_qty));
  asks.emplace_back(Price::fromDouble(ask_price), Quantity::fromDouble(ask_qty));
  static_cast<LrvxSimulatedExecutorImpl*>(h)->executor.onBookUpdate(symbol, bids, asks);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_simulated_executor_on_book_snapshot(LrvxSimulatedExecutorHandle h, uint32_t symbol,
                                              const double* bid_prices, const double* bid_qtys,
                                              uint32_t n_bids, const double* ask_prices,
                                              const double* ask_qtys, uint32_t n_asks)
{
  LRVX_CAPI_ENTER_VOID(h);
  std::pmr::monotonic_buffer_resource pool(1024);
  std::pmr::vector<BookLevel> bids(&pool);
  std::pmr::vector<BookLevel> asks(&pool);
  bids.reserve(n_bids);
  asks.reserve(n_asks);
  for (uint32_t i = 0; i < n_bids; ++i)
  {
    bids.emplace_back(Price::fromDouble(bid_prices[i]), Quantity::fromDouble(bid_qtys[i]));
  }
  for (uint32_t i = 0; i < n_asks; ++i)
  {
    asks.emplace_back(Price::fromDouble(ask_prices[i]), Quantity::fromDouble(ask_qtys[i]));
  }
  static_cast<LrvxSimulatedExecutorImpl*>(h)->executor.onBookUpdate(symbol, bids, asks);
  LRVX_CAPI_LEAVE_VOID;
}

struct LrvxBacktestResultImpl
{
  BacktestConfig config;
  std::unique_ptr<BacktestResult> result;
};

LrvxBacktestResultHandle lrvx_backtest_result_create(double initial_capital, double fee_rate,
                                                     uint8_t use_percentage_fee,
                                                     double fixed_fee_per_trade,
                                                     double risk_free_rate,
                                                     double annualization_factor)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  auto* impl = new LrvxBacktestResultImpl();
  impl->config.initialCapital = initial_capital;
  impl->config.feeRate = fee_rate;
  impl->config.usePercentageFee = use_percentage_fee != 0;
  impl->config.fixedFeePerTrade = fixed_fee_per_trade;
  impl->config.riskFreeRate = risk_free_rate;
  impl->config.metricsAnnualizationFactor =
      (annualization_factor > 0.0) ? annualization_factor : 252.0;
  impl->result = std::make_unique<BacktestResult>(impl->config);
  return impl;
  LRVX_CAPI_LEAVE;
}

void lrvx_backtest_result_destroy(LrvxBacktestResultHandle h)
{
  LRVX_CAPI_ENTER_DESTROY(h);
  delete static_cast<LrvxBacktestResultImpl*>(h);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_backtest_result_record_fill(LrvxBacktestResultHandle h, uint64_t order_id,
                                      uint32_t symbol, uint8_t side, double price,
                                      double quantity, int64_t timestamp_ns)
{
  LRVX_CAPI_ENTER_VOID(h);
  Fill fill{};
  fill.orderId = order_id;
  fill.symbol = symbol;
  fill.side = (side == 0) ? Side::BUY : Side::SELL;
  fill.price = Price::fromDouble(price);
  fill.quantity = Quantity::fromDouble(quantity);
  fill.timestampNs = UnixNanos::fromRaw(timestamp_ns);
  static_cast<LrvxBacktestResultImpl*>(h)->result->recordFill(fill);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_backtest_result_ingest_executor(LrvxBacktestResultHandle h, LrvxSimulatedExecutorHandle eh)
{
  LRVX_CAPI_ENTER_VOID(h);
  auto* impl = static_cast<LrvxBacktestResultImpl*>(h);
  for (const auto& fill : static_cast<LrvxSimulatedExecutorImpl*>(eh)->executor.fills())
  {
    impl->result->recordFill(fill);
  }
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_backtest_result_stats(LrvxBacktestResultHandle h, LrvxBacktestStats* out)
{
  LRVX_CAPI_ENTER_VOID(h);
  if (!out)
  {
    return;
  }
  auto stats = static_cast<LrvxBacktestResultImpl*>(h)->result->computeStats();
  out->totalTrades = stats.totalTrades;
  out->winningTrades = stats.winningTrades;
  out->losingTrades = stats.losingTrades;
  out->maxConsecutiveWins = stats.maxConsecutiveWins;
  out->maxConsecutiveLosses = stats.maxConsecutiveLosses;
  out->initialCapital = stats.initialCapital;
  out->finalCapital = stats.finalCapital;
  out->totalPnl = stats.totalPnl;
  out->totalFees = stats.totalFees;
  out->netPnl = stats.netPnl;
  out->grossProfit = stats.grossProfit;
  out->grossLoss = stats.grossLoss;
  out->maxDrawdown = stats.maxDrawdown;
  out->maxDrawdownPct = stats.maxDrawdownPct;
  out->winRate = stats.winRate;
  out->profitFactor = stats.profitFactor;
  out->avgWin = stats.avgWin;
  out->avgLoss = stats.avgLoss;
  out->avgWinLossRatio = stats.avgWinLossRatio;
  out->avgTradeDurationNs = stats.avgTradeDurationNs;
  out->medianTradeDurationNs = stats.medianTradeDurationNs;
  out->maxTradeDurationNs = stats.maxTradeDurationNs;
  out->sharpeRatio = stats.sharpeRatio;
  out->sortinoRatio = stats.sortinoRatio;
  out->calmarRatio = stats.calmarRatio;
  out->timeWeightedReturn = stats.timeWeightedReturn;
  out->returnPct = stats.returnPct;
  out->startTimeNs = stats.startTimeNs.raw();
  out->endTimeNs = stats.endTimeNs.raw();
  LRVX_CAPI_LEAVE_VOID;
}

uint32_t lrvx_backtest_result_equity_curve(LrvxBacktestResultHandle h, LrvxEquityPoint* out,
                                           uint32_t max_points)
{
  LRVX_CAPI_ENTER(h);
  const auto& curve = static_cast<LrvxBacktestResultImpl*>(h)->result->equityCurve();
  const uint32_t total = static_cast<uint32_t>(curve.size());
  if (!out)
  {
    return total;
  }
  const uint32_t n = (total < max_points) ? total : max_points;
  for (uint32_t i = 0; i < n; ++i)
  {
    out[i].timestamp_ns = curve[i].timestampNs.raw();
    out[i].equity = curve[i].equity;
    out[i].drawdown_pct = curve[i].drawdownPct;
  }
  return n;
  LRVX_CAPI_LEAVE;
}

uint8_t lrvx_backtest_result_write_equity_curve_csv(LrvxBacktestResultHandle h, const char* path)
{
  LRVX_CAPI_ENTER(h);
  if (!path)
  {
    return 0;
  }
  return static_cast<LrvxBacktestResultImpl*>(h)->result->writeEquityCurveCsv(path) ? 1 : 0;
  LRVX_CAPI_LEAVE;
}

uint32_t lrvx_backtest_result_trades(LrvxBacktestResultHandle h, LrvxBacktestTrade* out,
                                     uint32_t max_trades)
{
  LRVX_CAPI_ENTER(h);
  const auto& trades = static_cast<LrvxBacktestResultImpl*>(h)->result->trades();
  const uint32_t total = static_cast<uint32_t>(trades.size());
  if (!out)
  {
    return total;
  }
  const uint32_t n = (total < max_trades) ? total : max_trades;
  for (uint32_t i = 0; i < n; ++i)
  {
    const auto& t = trades[i];
    out[i].symbol = static_cast<uint32_t>(t.symbol);
    out[i].side = static_cast<uint8_t>(t.side);
    out[i].entry_price = t.entryPrice.toDouble();
    out[i].exit_price = t.exitPrice.toDouble();
    out[i].quantity = t.quantity.toDouble();
    out[i].entry_time_ns = t.entryTimeNs.raw();
    out[i].exit_time_ns = t.exitTimeNs.raw();
    out[i].pnl = t.pnl.toDouble();
    out[i].fee = t.fee.toDouble();
  }
  return n;
  LRVX_CAPI_LEAVE;
}

// ============================================================
// Segment operations (full API)
// ============================================================

static replay::CompressionType toCompression(uint8_t c)
{
  return c == 1 ? replay::CompressionType::LZ4 : replay::CompressionType::None;
}

LrvxMergeResult lrvx_segment_merge_full(const char* input_paths, size_t num_paths,
                                        const char* output_dir, const char* output_name,
                                        uint8_t sort)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  LRVX_CAPI_REQUIRE(input_paths);
  LRVX_CAPI_REQUIRE(output_dir);
  std::vector<std::filesystem::path> paths;
  const char* p = input_paths;
  for (size_t i = 0; i < num_paths; ++i)
  {
    paths.emplace_back(p);
    p += std::strlen(p) + 1;
  }
  replay::MergeConfig cfg;
  cfg.output_dir = output_dir;
  cfg.output_name = output_name ? output_name : "merged";
  cfg.sort_by_timestamp = sort != 0;
  auto r = replay::SegmentOps::merge(paths, cfg);
  return {r.success ? (uint8_t)1 : (uint8_t)0, r.segments_merged, r.events_written,
          r.bytes_written};
  LRVX_CAPI_LEAVE;
}

LrvxMergeResult lrvx_segment_merge_dir(const char* input_dir, const char* output_dir)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  LRVX_CAPI_REQUIRE(input_dir);
  LRVX_CAPI_REQUIRE(output_dir);
  auto r = replay::quickMerge(input_dir, output_dir);
  return {r.success ? (uint8_t)1 : (uint8_t)0, r.segments_merged, r.events_written,
          r.bytes_written};
  LRVX_CAPI_LEAVE;
}

LrvxSplitResult lrvx_segment_split(const char* input_path, const char* output_dir, uint8_t mode,
                                   int64_t time_interval_ns, uint64_t events_per_file)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  LRVX_CAPI_REQUIRE(input_path);
  LRVX_CAPI_REQUIRE(output_dir);
  replay::SplitConfig cfg;
  cfg.output_dir = output_dir;
  cfg.mode = static_cast<replay::SplitMode>(mode);
  cfg.time_interval_ns = time_interval_ns;
  cfg.events_per_file = events_per_file;
  auto r = replay::SegmentOps::split(input_path, cfg);
  return {r.success ? (uint8_t)1 : (uint8_t)0, r.segments_created, r.events_written};
  LRVX_CAPI_LEAVE;
}

LrvxExportResult lrvx_segment_export(const char* input_path, const char* output_path,
                                     uint8_t format, int64_t from_ns, int64_t to_ns,
                                     const uint32_t* symbols, uint32_t num_symbols)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  LRVX_CAPI_REQUIRE(input_path);
  LRVX_CAPI_REQUIRE(output_path);
  replay::ExportConfig cfg;
  cfg.output_path = output_path;
  cfg.format = static_cast<replay::ExportFormat>(format);
  if (from_ns > 0)
  {
    cfg.from_ts = from_ns;
  }
  if (to_ns > 0)
  {
    cfg.to_ts = to_ns;
  }
  if (symbols && num_symbols > 0)
  {
    cfg.symbols.insert(symbols, symbols + num_symbols);
  }
  auto r = replay::SegmentOps::exportData(input_path, cfg);
  return {r.success ? (uint8_t)1 : (uint8_t)0, r.events_exported, r.bytes_written};
  LRVX_CAPI_LEAVE;
}

uint8_t lrvx_segment_recompress(const char* input_path, const char* output_path,
                                uint8_t compression)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  LRVX_CAPI_REQUIRE(input_path);
  LRVX_CAPI_REQUIRE(output_path);
  return replay::SegmentOps::recompress(input_path, output_path, toCompression(compression)) ? 1
                                                                                             : 0;
  LRVX_CAPI_LEAVE;
}

uint64_t lrvx_segment_extract_symbols(const char* input_path, const char* output_path,
                                      const uint32_t* symbols, uint32_t num_symbols)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  LRVX_CAPI_REQUIRE(input_path);
  LRVX_CAPI_REQUIRE(output_path);
  std::set<uint32_t> symSet(symbols, symbols + num_symbols);
  replay::WriterConfig wc;
  auto out = std::filesystem::path(output_path);
  wc.output_dir = out.parent_path();
  wc.output_filename = out.filename().string();
  return replay::SegmentOps::extractSymbols(input_path, output_path, symSet, wc);
  LRVX_CAPI_LEAVE;
}

uint64_t lrvx_segment_extract_time_range(const char* input_path, const char* output_path,
                                         int64_t from_ns, int64_t to_ns)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  LRVX_CAPI_REQUIRE(input_path);
  LRVX_CAPI_REQUIRE(output_path);
  replay::WriterConfig wc;
  auto out = std::filesystem::path(output_path);
  wc.output_dir = out.parent_path();
  wc.output_filename = out.filename().string();
  return replay::SegmentOps::extractTimeRange(input_path, output_path, from_ns, to_ns, wc);
  LRVX_CAPI_LEAVE;
}

// ============================================================
// Validation (full API)
// ============================================================

LrvxSegmentValidation lrvx_segment_validate_full(const char* path, uint8_t verify_crc,
                                                 uint8_t verify_timestamps)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  LRVX_CAPI_REQUIRE(path);
  replay::ValidatorConfig cfg;
  cfg.verify_crc = verify_crc != 0;
  cfg.verify_timestamps = verify_timestamps != 0;
  replay::SegmentValidator validator(cfg);
  auto r = validator.validate(path);
  return {r.valid ? (uint8_t)1 : (uint8_t)0,
          r.header_valid ? (uint8_t)1 : (uint8_t)0,
          r.reported_event_count,
          r.actual_event_count,
          r.has_index ? (uint8_t)1 : (uint8_t)0,
          r.index_valid ? (uint8_t)1 : (uint8_t)0,
          r.trades_found,
          r.book_updates_found,
          r.crc_errors,
          r.timestamp_anomalies};
  LRVX_CAPI_LEAVE;
}

LrvxDatasetValidation lrvx_dataset_validate(const char* data_dir)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  LRVX_CAPI_REQUIRE(data_dir);
  replay::DatasetValidator validator;
  auto r = validator.validate(data_dir);
  return {r.valid ? (uint8_t)1 : (uint8_t)0, r.total_segments, r.valid_segments,
          r.corrupted_segments, r.total_events, r.total_bytes,
          r.first_timestamp, r.last_timestamp};
  LRVX_CAPI_LEAVE;
}

// ============================================================
// DataReader (full API)
// ============================================================

LrvxDataReaderHandle lrvx_data_reader_create_filtered(const char* data_dir, int64_t from_ns,
                                                      int64_t to_ns, const uint32_t* symbols,
                                                      uint32_t num_symbols)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  LRVX_CAPI_REQUIRE(data_dir);
  replay::ReaderConfig cfg;
  cfg.data_dir = data_dir;
  if (from_ns > 0)
  {
    cfg.from_ns = from_ns;
  }
  if (to_ns > 0)
  {
    cfg.to_ns = to_ns;
  }
  if (symbols && num_symbols > 0)
  {
    cfg.symbols.insert(symbols, symbols + num_symbols);
  }
  return new replay::BinaryLogReader(cfg);
  LRVX_CAPI_LEAVE;
}

LrvxDataReaderHandle lrvx_data_reader_create_ordered(const char* data_dir, int64_t from_ns,
                                                     int64_t to_ns, const uint32_t* symbols,
                                                     uint32_t num_symbols,
                                                     int64_t reorder_window_ns,
                                                     int32_t strict_ordering)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  LRVX_CAPI_REQUIRE(data_dir);
  replay::ReaderConfig cfg;
  cfg.data_dir = data_dir;
  if (from_ns > 0)
  {
    cfg.from_ns = from_ns;
  }
  if (to_ns > 0)
  {
    cfg.to_ns = to_ns;
  }
  if (symbols && num_symbols > 0)
  {
    cfg.symbols.insert(symbols, symbols + num_symbols);
  }
  if (reorder_window_ns > 0)
  {
    cfg.reorder_window_ns = reorder_window_ns;
  }
  cfg.strict_ordering = strict_ordering != 0;
  return new replay::BinaryLogReader(cfg);
  LRVX_CAPI_LEAVE;
}

LrvxDatasetSummary lrvx_data_reader_summary(LrvxDataReaderHandle h)
{
  LRVX_CAPI_ENTER(h);
  auto* reader = static_cast<replay::BinaryLogReader*>(h);
  auto s = reader->summary();
  return {s.first_event_ns, s.last_event_ns, s.total_events, s.segment_count, s.total_bytes,
          s.durationSeconds()};
  LRVX_CAPI_LEAVE;
}

LrvxReaderStats lrvx_data_reader_stats(LrvxDataReaderHandle h)
{
  LRVX_CAPI_ENTER(h);
  auto* reader = static_cast<replay::BinaryLogReader*>(h);
  auto s = reader->stats();
  return {s.files_read, s.events_read, s.trades_read, s.book_updates_read,
          s.bytes_read, s.crc_errors, s.late_dropped, s.unknown_frames_skipped};
  LRVX_CAPI_LEAVE;
}

uint64_t lrvx_data_reader_read_trades(LrvxDataReaderHandle h, LrvxTradeRecord* trades_out,
                                      uint64_t max_trades)
{
  LRVX_CAPI_ENTER(h);
  auto* reader = static_cast<replay::BinaryLogReader*>(h);
  uint64_t count = 0;
  reader->forEach(
      [&](const replay::ReplayEvent& ev) -> bool
      {
        if (ev.type == replay::EventType::Trade)
        {
          if (trades_out && count < max_trades)
          {
            trades_out[count] = {ev.trade.exchange_ts_ns, ev.trade.recv_ts_ns, ev.trade.price_raw,
                                 ev.trade.qty_raw, ev.trade.trade_id, ev.trade.symbol_id,
                                 ev.trade.side};
          }
          ++count;
        }
        return !trades_out || count < max_trades;
      });
  return count;
  LRVX_CAPI_LEAVE;
}

// Layout invariants — language bindings (Codon, QuickJS) parse these structs
// from raw byte buffers and depend on exact offsets/sizes.
static_assert(sizeof(LrvxBBO) == 64, "LrvxBBO must be 64 bytes");
static_assert(sizeof(LrvxBookUpdateHeader) == 48, "LrvxBookUpdateHeader must be 48 bytes");
static_assert(sizeof(LrvxLevel) == 24, "LrvxLevel must be 24 bytes");

uint64_t lrvx_data_reader_read_bbo(LrvxDataReaderHandle h, LrvxBBO* bbos_out,
                                   uint64_t max_events)
{
  LRVX_CAPI_ENTER(h);
  auto* reader = static_cast<replay::BinaryLogReader*>(h);
  uint64_t count = 0;
  reader->forEach(
      [&](const replay::ReplayEvent& ev) -> bool
      {
        if (ev.type != replay::EventType::BookSnapshot &&
            ev.type != replay::EventType::BookDelta)
        {
          return true;
        }

        if (bbos_out && count < max_events)
        {
          LrvxBBO b{};
          b.exchange_ts_ns = ev.book_header.exchange_ts_ns;
          b.recv_ts_ns = ev.book_header.recv_ts_ns;
          b.seq = ev.book_header.seq;
          b.symbol_id = ev.book_header.symbol_id;
          b.event_type = ev.book_header.type;
          if (!ev.bids.empty())
          {
            b.bid_price_raw = ev.bids[0].price_raw;
            b.bid_qty_raw = ev.bids[0].qty_raw;
          }
          if (!ev.asks.empty())
          {
            b.ask_price_raw = ev.asks[0].price_raw;
            b.ask_qty_raw = ev.asks[0].qty_raw;
          }
          bbos_out[count] = b;
        }
        ++count;
        return !bbos_out || count < max_events;
      });
  return count;
  LRVX_CAPI_LEAVE;
}

uint64_t lrvx_data_reader_count_book_updates(LrvxDataReaderHandle h, uint64_t* total_levels_out)
{
  LRVX_CAPI_ENTER(h);
  auto* reader = static_cast<replay::BinaryLogReader*>(h);
  uint64_t events = 0;
  uint64_t levels = 0;
  reader->forEach(
      [&](const replay::ReplayEvent& ev) -> bool
      {
        if (ev.type == replay::EventType::BookSnapshot ||
            ev.type == replay::EventType::BookDelta)
        {
          ++events;
          levels += ev.bids.size() + ev.asks.size();
        }
        return true;
      });
  if (total_levels_out)
  {
    *total_levels_out = levels;
  }
  return events;
  LRVX_CAPI_LEAVE;
}

uint64_t lrvx_data_reader_read_book_updates(LrvxDataReaderHandle h,
                                            LrvxBookUpdateHeader* headers_out,
                                            uint64_t max_events,
                                            LrvxLevel* levels_out,
                                            uint64_t max_levels)
{
  LRVX_CAPI_ENTER(h);
  auto* reader = static_cast<replay::BinaryLogReader*>(h);
  uint64_t events = 0;
  uint64_t levels_written = 0;
  reader->forEach(
      [&](const replay::ReplayEvent& ev) -> bool
      {
        if (ev.type != replay::EventType::BookSnapshot &&
            ev.type != replay::EventType::BookDelta)
        {
          return true;
        }

        const uint64_t bid_n = ev.bids.size();
        const uint64_t ask_n = ev.asks.size();
        const uint64_t total = bid_n + ask_n;

        if (headers_out && events < max_events && levels_written + total <= max_levels)
        {
          LrvxBookUpdateHeader hdr{};
          hdr.exchange_ts_ns = ev.book_header.exchange_ts_ns;
          hdr.recv_ts_ns = ev.book_header.recv_ts_ns;
          hdr.seq = ev.book_header.seq;
          hdr.level_offset = levels_written;
          hdr.symbol_id = ev.book_header.symbol_id;
          hdr.bid_count = static_cast<uint16_t>(bid_n);
          hdr.ask_count = static_cast<uint16_t>(ask_n);
          hdr.event_type = ev.book_header.type;
          headers_out[events] = hdr;

          for (uint64_t i = 0; i < bid_n; ++i)
          {
            levels_out[levels_written + i] = {ev.bids[i].price_raw, ev.bids[i].qty_raw, 0};
          }
          for (uint64_t i = 0; i < ask_n; ++i)
          {
            levels_out[levels_written + bid_n + i] = {ev.asks[i].price_raw, ev.asks[i].qty_raw, 1};
          }

          levels_written += total;
          ++events;
          return true;
        }

        return false;
      });
  return events;
  LRVX_CAPI_LEAVE;
}

// ── _from variants ─────────────────────────────────────────────
// Mid-stream seek: start iterating from start_ts_ns instead of segment
// beginning. Behaviour is otherwise identical to the matching non-_from
// reader.

uint64_t lrvx_data_reader_read_trades_from(LrvxDataReaderHandle h, int64_t start_ts_ns,
                                           LrvxTradeRecord* trades_out, uint64_t max_trades)
{
  LRVX_CAPI_ENTER(h);
  auto* reader = static_cast<replay::BinaryLogReader*>(h);
  uint64_t count = 0;
  reader->forEachFrom(start_ts_ns,
                      [&](const replay::ReplayEvent& ev) -> bool
                      {
                        if (ev.type == replay::EventType::Trade)
                        {
                          if (trades_out && count < max_trades)
                          {
                            trades_out[count] = {ev.trade.exchange_ts_ns, ev.trade.recv_ts_ns,
                                                 ev.trade.price_raw, ev.trade.qty_raw,
                                                 ev.trade.trade_id, ev.trade.symbol_id,
                                                 ev.trade.side};
                          }
                          ++count;
                        }
                        return !trades_out || count < max_trades;
                      });
  return count;
  LRVX_CAPI_LEAVE;
}

uint64_t lrvx_data_reader_read_bbo_from(LrvxDataReaderHandle h, int64_t start_ts_ns,
                                        LrvxBBO* bbos_out, uint64_t max_events)
{
  LRVX_CAPI_ENTER(h);
  auto* reader = static_cast<replay::BinaryLogReader*>(h);
  uint64_t count = 0;
  reader->forEachFrom(start_ts_ns,
                      [&](const replay::ReplayEvent& ev) -> bool
                      {
                        if (ev.type != replay::EventType::BookSnapshot &&
                            ev.type != replay::EventType::BookDelta)
                        {
                          return true;
                        }

                        if (bbos_out && count < max_events)
                        {
                          LrvxBBO b{};
                          b.exchange_ts_ns = ev.book_header.exchange_ts_ns;
                          b.recv_ts_ns = ev.book_header.recv_ts_ns;
                          b.seq = ev.book_header.seq;
                          b.symbol_id = ev.book_header.symbol_id;
                          b.event_type = ev.book_header.type;
                          if (!ev.bids.empty())
                          {
                            b.bid_price_raw = ev.bids[0].price_raw;
                            b.bid_qty_raw = ev.bids[0].qty_raw;
                          }
                          if (!ev.asks.empty())
                          {
                            b.ask_price_raw = ev.asks[0].price_raw;
                            b.ask_qty_raw = ev.asks[0].qty_raw;
                          }
                          bbos_out[count] = b;
                        }
                        ++count;
                        return !bbos_out || count < max_events;
                      });
  return count;
  LRVX_CAPI_LEAVE;
}

uint64_t lrvx_data_reader_count_book_updates_from(LrvxDataReaderHandle h, int64_t start_ts_ns,
                                                  uint64_t* total_levels_out)
{
  LRVX_CAPI_ENTER(h);
  auto* reader = static_cast<replay::BinaryLogReader*>(h);
  uint64_t events = 0;
  uint64_t levels = 0;
  reader->forEachFrom(start_ts_ns,
                      [&](const replay::ReplayEvent& ev) -> bool
                      {
                        if (ev.type == replay::EventType::BookSnapshot ||
                            ev.type == replay::EventType::BookDelta)
                        {
                          ++events;
                          levels += ev.bids.size() + ev.asks.size();
                        }
                        return true;
                      });
  if (total_levels_out)
  {
    *total_levels_out = levels;
  }
  return events;
  LRVX_CAPI_LEAVE;
}

uint64_t lrvx_data_reader_read_book_updates_from(LrvxDataReaderHandle h, int64_t start_ts_ns,
                                                 LrvxBookUpdateHeader* headers_out,
                                                 uint64_t max_events, LrvxLevel* levels_out,
                                                 uint64_t max_levels)
{
  LRVX_CAPI_ENTER(h);
  auto* reader = static_cast<replay::BinaryLogReader*>(h);
  uint64_t events = 0;
  uint64_t levels_written = 0;
  reader->forEachFrom(start_ts_ns,
                      [&](const replay::ReplayEvent& ev) -> bool
                      {
                        if (ev.type != replay::EventType::BookSnapshot &&
                            ev.type != replay::EventType::BookDelta)
                        {
                          return true;
                        }

                        const uint64_t bid_n = ev.bids.size();
                        const uint64_t ask_n = ev.asks.size();
                        const uint64_t total = bid_n + ask_n;

                        if (headers_out && events < max_events &&
                            levels_written + total <= max_levels)
                        {
                          LrvxBookUpdateHeader hdr{};
                          hdr.exchange_ts_ns = ev.book_header.exchange_ts_ns;
                          hdr.recv_ts_ns = ev.book_header.recv_ts_ns;
                          hdr.seq = ev.book_header.seq;
                          hdr.level_offset = levels_written;
                          hdr.symbol_id = ev.book_header.symbol_id;
                          hdr.bid_count = static_cast<uint16_t>(bid_n);
                          hdr.ask_count = static_cast<uint16_t>(ask_n);
                          hdr.event_type = ev.book_header.type;
                          headers_out[events] = hdr;

                          for (uint64_t i = 0; i < bid_n; ++i)
                          {
                            levels_out[levels_written + i] = {ev.bids[i].price_raw,
                                                              ev.bids[i].qty_raw, 0};
                          }
                          for (uint64_t i = 0; i < ask_n; ++i)
                          {
                            levels_out[levels_written + bid_n + i] = {
                                ev.asks[i].price_raw, ev.asks[i].qty_raw, 1};
                          }

                          levels_written += total;
                          ++events;
                          return true;
                        }

                        return false;
                      });
  return events;
  LRVX_CAPI_LEAVE;
}

// ============================================================
// DataWriter (extras)
// ============================================================

LrvxWriterStats lrvx_data_writer_stats(LrvxDataWriterHandle h)
{
  LRVX_CAPI_ENTER(h);
  auto* w = static_cast<replay::BinaryLogWriter*>(h);
  auto s = w->stats();
  return {s.bytes_written, s.events_written, s.segments_created, s.trades_written};
  LRVX_CAPI_LEAVE;
}

// ============================================================
// MergedTapeReader — N-tape merged consumption
// ============================================================
namespace capi_impl
{
struct LrvxMergedTapeReaderImpl
{
  std::unique_ptr<lrvx::replay::MergedTapeReader> reader;
  // Cached result of readTrades / readBooks so two-phase count→read
  // doesn't re-merge. Cleared on first read of each kind.
  std::optional<std::vector<lrvx::replay::MergedTradeRow>> cached_trades;
  std::optional<std::pair<std::vector<lrvx::replay::MergedBookRow>,
                          std::vector<lrvx::replay::BookLevel>>>
      cached_books;

  // Owning strings for symbol_table and per-tape paths so the borrowed
  // const char* pointers we hand out stay valid for the reader's lifetime.
  std::vector<std::string> sym_exchanges;
  std::vector<std::string> sym_names;
  std::vector<std::string> tape_paths;
};
}  // namespace capi_impl

LrvxMergedTapeReaderHandle
lrvx_merged_tape_reader_create(const char* const* paths, uint32_t n_paths,
                               int64_t from_ns, int64_t to_ns,
                               const uint32_t* symbol_filter,
                               uint32_t n_filter)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  if (!paths || n_paths == 0)
  {
    return nullptr;
  }
  lrvx::replay::MergedTapeReaderConfig cfg{};
  cfg.tape_dirs.reserve(n_paths);
  for (uint32_t i = 0; i < n_paths; ++i)
  {
    cfg.tape_dirs.emplace_back(paths[i] ? paths[i] : "");
  }
  if (from_ns >= 0)
  {
    cfg.from_ns = from_ns;
  }
  if (to_ns >= 0)
  {
    cfg.to_ns = to_ns;
  }
  if (symbol_filter && n_filter > 0)
  {
    cfg.symbol_filter.assign(symbol_filter, symbol_filter + n_filter);
  }

  try
  {
    auto impl = std::make_unique<capi_impl::LrvxMergedTapeReaderImpl>();
    impl->reader =
        std::make_unique<lrvx::replay::MergedTapeReader>(std::move(cfg));
    // Cache symbol / path strings now so const char* pointers stay stable.
    for (const auto& s : impl->reader->symbols())
    {
      impl->sym_exchanges.push_back(s.exchange);
      impl->sym_names.push_back(s.name);
    }
    for (const auto& t : impl->reader->perTapeStats())
    {
      impl->tape_paths.push_back(t.path.string());
    }
    return static_cast<LrvxMergedTapeReaderHandle>(impl.release());
  }
  catch (...)
  {
    // Construction may throw on bad input or overlapping book streams;
    // surface as a NULL handle, leaving caller to decide.
    return nullptr;
  }
  LRVX_CAPI_LEAVE;
}

void lrvx_merged_tape_reader_destroy(LrvxMergedTapeReaderHandle h)
{
  LRVX_CAPI_ENTER_DESTROY(h);
  delete static_cast<capi_impl::LrvxMergedTapeReaderImpl*>(h);
  LRVX_CAPI_LEAVE_VOID;
}

uint32_t lrvx_merged_tape_reader_symbol_count(LrvxMergedTapeReaderHandle h)
{
  LRVX_CAPI_ENTER(h);
  if (!h)
  {
    return 0;
  }
  auto* impl = static_cast<capi_impl::LrvxMergedTapeReaderImpl*>(h);
  return static_cast<uint32_t>(impl->reader->symbols().size());
  LRVX_CAPI_LEAVE;
}

uint32_t lrvx_merged_tape_reader_get_symbols(LrvxMergedTapeReaderHandle h,
                                             LrvxMergedSymbol* out,
                                             uint32_t max)
{
  LRVX_CAPI_ENTER(h);
  if (!h)
  {
    return 0;
  }
  auto* impl = static_cast<capi_impl::LrvxMergedTapeReaderImpl*>(h);
  const auto& syms = impl->reader->symbols();
  uint32_t n = std::min(static_cast<uint32_t>(syms.size()), max);
  if (!out)
  {
    return static_cast<uint32_t>(syms.size());
  }
  for (uint32_t i = 0; i < n; ++i)
  {
    out[i].global_id = syms[i].global_id;
    out[i].price_precision = syms[i].price_precision;
    out[i].qty_precision = syms[i].qty_precision;
    out[i]._pad[0] = out[i]._pad[1] = 0;
    out[i].exchange = impl->sym_exchanges[i].c_str();
    out[i].name = impl->sym_names[i].c_str();
  }
  return n;
  LRVX_CAPI_LEAVE;
}

uint32_t lrvx_merged_tape_reader_tape_count(LrvxMergedTapeReaderHandle h)
{
  LRVX_CAPI_ENTER(h);
  if (!h)
  {
    return 0;
  }
  return static_cast<uint32_t>(
      static_cast<capi_impl::LrvxMergedTapeReaderImpl*>(h)
          ->reader->perTapeStats()
          .size());
  LRVX_CAPI_LEAVE;
}

uint32_t lrvx_merged_tape_reader_get_tape_stats(LrvxMergedTapeReaderHandle h,
                                                LrvxMergedTapeStats* out,
                                                uint32_t max)
{
  LRVX_CAPI_ENTER(h);
  if (!h)
  {
    return 0;
  }
  auto* impl = static_cast<capi_impl::LrvxMergedTapeReaderImpl*>(h);
  const auto& stats = impl->reader->perTapeStats();
  uint32_t n = std::min(static_cast<uint32_t>(stats.size()), max);
  if (!out)
  {
    return static_cast<uint32_t>(stats.size());
  }
  for (uint32_t i = 0; i < n; ++i)
  {
    out[i].first_event_ns = stats[i].first_event_ns;
    out[i].last_event_ns = stats[i].last_event_ns;
    out[i].trades = stats[i].trades;
    out[i].books = stats[i].books;
    out[i].path = impl->tape_paths[i].c_str();
  }
  return n;
  LRVX_CAPI_LEAVE;
}

void lrvx_merged_tape_reader_time_range(LrvxMergedTapeReaderHandle h,
                                        int64_t* min_first_ns_out,
                                        int64_t* max_last_ns_out)
{
  LRVX_CAPI_ENTER_VOID(h);
  if (!h)
  {
    if (min_first_ns_out)
    {
      *min_first_ns_out = 0;
    }
    if (max_last_ns_out)
    {
      *max_last_ns_out = 0;
    }
    return;
  }
  auto* impl = static_cast<capi_impl::LrvxMergedTapeReaderImpl*>(h);
  auto [a, b] = impl->reader->timeRange();
  if (min_first_ns_out)
  {
    *min_first_ns_out = a;
  }
  if (max_last_ns_out)
  {
    *max_last_ns_out = b;
  }
  LRVX_CAPI_LEAVE_VOID;
}

uint64_t lrvx_merged_tape_reader_count_trades(LrvxMergedTapeReaderHandle h)
{
  LRVX_CAPI_ENTER(h);
  if (!h)
  {
    return 0;
  }
  auto* impl = static_cast<capi_impl::LrvxMergedTapeReaderImpl*>(h);
  if (!impl->cached_trades)
  {
    impl->cached_trades = impl->reader->readTrades();
  }
  return impl->cached_trades->size();
  LRVX_CAPI_LEAVE;
}

uint64_t lrvx_merged_tape_reader_read_trades(LrvxMergedTapeReaderHandle h,
                                             LrvxTradeRecord* trades_out,
                                             uint64_t max_trades)
{
  LRVX_CAPI_ENTER(h);
  if (!h)
  {
    return 0;
  }
  auto* impl = static_cast<capi_impl::LrvxMergedTapeReaderImpl*>(h);
  if (!impl->cached_trades)
  {
    impl->cached_trades = impl->reader->readTrades();
  }
  const auto& src = *impl->cached_trades;
  uint64_t n = std::min<uint64_t>(src.size(), max_trades);
  if (!trades_out)
  {
    return src.size();
  }
  for (uint64_t i = 0; i < n; ++i)
  {
    trades_out[i].exchange_ts_ns = src[i].exchange_ts_ns;
    trades_out[i].recv_ts_ns = src[i].recv_ts_ns;
    trades_out[i].price_raw = src[i].price_raw;
    trades_out[i].qty_raw = src[i].qty_raw;
    trades_out[i].trade_id = src[i].trade_id;
    trades_out[i].symbol_id = src[i].global_symbol_id;
    trades_out[i].side = src[i].side;
  }
  return n;
  LRVX_CAPI_LEAVE;
}

uint64_t lrvx_merged_tape_reader_count_books(LrvxMergedTapeReaderHandle h,
                                             uint64_t* total_levels_out)
{
  LRVX_CAPI_ENTER(h);
  if (!h)
  {
    if (total_levels_out)
    {
      *total_levels_out = 0;
    }
    return 0;
  }
  auto* impl = static_cast<capi_impl::LrvxMergedTapeReaderImpl*>(h);
  if (!impl->cached_books)
  {
    impl->cached_books = impl->reader->readBooks();
  }
  if (total_levels_out)
  {
    *total_levels_out = impl->cached_books->second.size();
  }
  return impl->cached_books->first.size();
  LRVX_CAPI_LEAVE;
}

uint64_t lrvx_merged_tape_reader_read_books(LrvxMergedTapeReaderHandle h,
                                            LrvxBookUpdateHeader* headers_out,
                                            uint64_t max_events,
                                            LrvxLevel* levels_out,
                                            uint64_t max_levels)
{
  LRVX_CAPI_ENTER(h);
  if (!h)
  {
    return 0;
  }
  auto* impl = static_cast<capi_impl::LrvxMergedTapeReaderImpl*>(h);
  if (!impl->cached_books)
  {
    impl->cached_books = impl->reader->readBooks();
  }
  const auto& [rows, levels] = *impl->cached_books;
  uint64_t n_ev = std::min<uint64_t>(rows.size(), max_events);
  uint64_t n_lv = std::min<uint64_t>(levels.size(), max_levels);

  if (headers_out)
  {
    for (uint64_t i = 0; i < n_ev; ++i)
    {
      headers_out[i].exchange_ts_ns = rows[i].exchange_ts_ns;
      headers_out[i].recv_ts_ns = rows[i].recv_ts_ns;
      headers_out[i].seq = rows[i].seq;
      headers_out[i].level_offset = rows[i].level_offset;
      headers_out[i].symbol_id = rows[i].global_symbol_id;
      headers_out[i].bid_count = rows[i].bid_count;
      headers_out[i].ask_count = rows[i].ask_count;
      headers_out[i].event_type = rows[i].event_type;
    }
  }
  if (levels_out)
  {
    // Reader stores levels flat (bids then asks per event). The side
    // byte is reconstructed here using the corresponding header's
    // bid_count split.
    uint64_t k = 0;
    for (uint64_t i = 0; i < rows.size() && k < n_lv; ++i)
    {
      const auto& r = rows[i];
      for (uint16_t b = 0; b < r.bid_count && k < n_lv; ++b, ++k)
      {
        levels_out[k].price_raw = levels[k].price_raw;
        levels_out[k].qty_raw = levels[k].qty_raw;
        levels_out[k].side = 0;
      }
      for (uint16_t a = 0; a < r.ask_count && k < n_lv; ++a, ++k)
      {
        levels_out[k].price_raw = levels[k].price_raw;
        levels_out[k].qty_raw = levels[k].qty_raw;
        levels_out[k].side = 1;
      }
    }
  }
  return n_ev;
  LRVX_CAPI_LEAVE;
}

// ============================================================
// Partitioner
// ============================================================

LrvxPartitionerHandle lrvx_partitioner_create(const char* data_dir)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  return new replay::Partitioner(std::filesystem::path(data_dir));
  LRVX_CAPI_LEAVE;
}

void lrvx_partitioner_destroy(LrvxPartitionerHandle h)
{
  LRVX_CAPI_ENTER_DESTROY(h);
  delete static_cast<replay::Partitioner*>(h);
  LRVX_CAPI_LEAVE_VOID;
}

static uint32_t copyPartitions(const std::vector<replay::Partition>& parts,
                               LrvxPartition* out, uint32_t max)
{
  uint32_t n = static_cast<uint32_t>(parts.size());
  if (!out)
  {
    return n;
  }
  uint32_t count = std::min(n, max);
  for (uint32_t i = 0; i < count; ++i)
  {
    out[i] = {parts[i].partition_id, parts[i].from_ns, parts[i].to_ns,
              parts[i].warmup_from_ns, parts[i].estimated_events, parts[i].estimated_bytes};
  }
  return count;
}

uint32_t lrvx_partitioner_by_time(LrvxPartitionerHandle h, uint32_t num_partitions,
                                  int64_t warmup_ns, LrvxPartition* out, uint32_t max)
{
  LRVX_CAPI_ENTER(h);
  return copyPartitions(
      static_cast<replay::Partitioner*>(h)->partitionByTime(num_partitions, warmup_ns), out, max);
  LRVX_CAPI_LEAVE;
}

uint32_t lrvx_partitioner_by_duration(LrvxPartitionerHandle h, int64_t duration_ns,
                                      int64_t warmup_ns, LrvxPartition* out, uint32_t max)
{
  LRVX_CAPI_ENTER(h);
  return copyPartitions(
      static_cast<replay::Partitioner*>(h)->partitionByDuration(duration_ns, warmup_ns), out, max);
  LRVX_CAPI_LEAVE;
}

uint32_t lrvx_partitioner_by_calendar(LrvxPartitionerHandle h, uint8_t unit, int64_t warmup_ns,
                                      LrvxPartition* out, uint32_t max)
{
  LRVX_CAPI_ENTER(h);
  return copyPartitions(
      static_cast<replay::Partitioner*>(h)->partitionByCalendar(
          static_cast<replay::Partitioner::CalendarUnit>(unit), warmup_ns),
      out, max);
  LRVX_CAPI_LEAVE;
}

uint32_t lrvx_partitioner_by_symbol(LrvxPartitionerHandle h, uint32_t num_partitions,
                                    LrvxPartition* out, uint32_t max)
{
  LRVX_CAPI_ENTER(h);
  return copyPartitions(
      static_cast<replay::Partitioner*>(h)->partitionBySymbol(num_partitions), out, max);
  LRVX_CAPI_LEAVE;
}

uint32_t lrvx_partitioner_per_symbol(LrvxPartitionerHandle h, LrvxPartition* out, uint32_t max)
{
  LRVX_CAPI_ENTER(h);
  return copyPartitions(static_cast<replay::Partitioner*>(h)->partitionPerSymbol(), out, max);
  LRVX_CAPI_LEAVE;
}

uint32_t lrvx_partitioner_by_event_count(LrvxPartitionerHandle h, uint32_t num_partitions,
                                         LrvxPartition* out, uint32_t max)
{
  LRVX_CAPI_ENTER(h);
  return copyPartitions(
      static_cast<replay::Partitioner*>(h)->partitionByEventCount(num_partitions), out, max);
  LRVX_CAPI_LEAVE;
}

// ============================================================
// Pointer-out wrappers for struct-returning functions.
// ============================================================

void lrvx_data_reader_summary_p(LrvxDataReaderHandle h, void* out)
{
  LRVX_CAPI_ENTER_VOID(h);
  auto s = lrvx_data_reader_summary(h);
  memcpy(out, &s, sizeof(s));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_data_reader_stats_p(LrvxDataReaderHandle h, void* out)
{
  LRVX_CAPI_ENTER_VOID(h);
  auto s = lrvx_data_reader_stats(h);
  memcpy(out, &s, sizeof(s));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_data_writer_stats_p(LrvxDataWriterHandle h, void* out)
{
  LRVX_CAPI_ENTER_VOID(h);
  auto s = lrvx_data_writer_stats(h);
  memcpy(out, &s, sizeof(s));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_segment_merge_full_p(const char* input_paths, size_t num_paths,
                               const char* output_dir, const char* output_name,
                               uint8_t sort, void* out)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  auto s = lrvx_segment_merge_full(input_paths, num_paths, output_dir, output_name, sort);
  memcpy(out, &s, sizeof(s));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_segment_merge_dir_p(const char* input_dir, const char* output_dir, void* out)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  auto s = lrvx_segment_merge_dir(input_dir, output_dir);
  memcpy(out, &s, sizeof(s));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_segment_split_p(const char* input_path, const char* output_dir, uint8_t mode,
                          int64_t time_interval_ns, uint64_t events_per_file, void* out)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  auto s = lrvx_segment_split(input_path, output_dir, mode, time_interval_ns, events_per_file);
  memcpy(out, &s, sizeof(s));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_segment_export_p(const char* input_path, const char* output_path, uint8_t format,
                           int64_t from_ns, int64_t to_ns,
                           const uint32_t* symbols, uint32_t num_symbols, void* out)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  auto s = lrvx_segment_export(input_path, output_path, format, from_ns, to_ns,
                               symbols, num_symbols);
  memcpy(out, &s, sizeof(s));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_segment_validate_full_p(const char* path, uint8_t verify_crc,
                                  uint8_t verify_timestamps, void* out)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  auto s = lrvx_segment_validate_full(path, verify_crc, verify_timestamps);
  memcpy(out, &s, sizeof(s));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_dataset_validate_p(const char* data_dir, void* out)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  auto s = lrvx_dataset_validate(data_dir);
  memcpy(out, &s, sizeof(s));
  LRVX_CAPI_LEAVE_VOID;
}

// ============================================================
// Shared internals: RunnerSignalHandler, LrvxRunnerImpl, LrvxLiveEngineImpl
// ============================================================

namespace capi_impl
{

// LrvxRiskManagerImpl — non-owning callback bundle. Outlives any runner /
// engine it's attached to (caller manages destruction). Thread-safe to
// invoke from multiple consumer threads concurrently as long as the user-
// supplied callback is itself thread-safe.
struct LrvxRiskManagerImpl
{
  LrvxRiskManagerCallbacks cb;
};

// Same shape, different intent. See header docs for the evaluation order.
struct LrvxKillSwitchImpl
{
  LrvxKillSwitchCallbacks cb;
};

struct LrvxOrderValidatorImpl
{
  LrvxOrderValidatorCallbacks cb;
};

// Post-emission observers. Fire after the user on_signal callback.
// Never block; return type is void.
struct LrvxPnLTrackerImpl
{
  LrvxPnLTrackerCallbacks cb;
};

struct LrvxStorageSinkImpl
{
  LrvxStorageSinkCallbacks cb;
};

struct LrvxMarketDataRecorderImpl
{
  LrvxMarketDataRecorderCallbacks cb;
};

struct LrvxReplaySourceImpl
{
  LrvxReplaySourceCallbacks cb;
};

// Pack a lrvx::Order into the ABI-stable LrvxOrder for binding callbacks.
inline LrvxOrder packOrder(const lrvx::Order& o) noexcept
{
  LrvxOrder fo{};
  fo.id = o.id;
  fo.client_order_id = o.clientOrderId;
  fo.symbol = o.symbol;
  fo.strategy_id = o.strategyId;
  fo.order_tag = o.orderTag;
  fo.side = (o.side == lrvx::Side::BUY) ? 0u : 1u;
  fo.type = static_cast<uint8_t>(o.type);
  fo.time_in_force = static_cast<uint8_t>(o.timeInForce);
  // Pack ExecutionFlags bit-by-bit for ABI stability.
  uint8_t flags = 0;
  flags |= (o.flags.reduceOnly ? 0x01 : 0);
  flags |= (o.flags.closePosition ? 0x02 : 0);
  flags |= (o.flags.postOnly ? 0x04 : 0);
  flags |= static_cast<uint8_t>((o.flags.holdSide & 0x03) << 3);
  fo.flags = flags;
  fo.price_raw = o.price.raw();
  fo.quantity_raw = o.quantity.raw();
  fo.filled_quantity_raw = o.filledQuantity.raw();
  fo.trigger_price_raw = o.triggerPrice.raw();
  fo.trailing_offset_raw = o.trailingOffset.raw();
  fo.created_at_ns = o.createdAt.time_since_epoch().count();
  fo.exchange_ts_ns = o.exchangeTimestamp.has_value()
                          ? o.exchangeTimestamp->time_since_epoch().count()
                          : 0;
  return fo;
}

struct LrvxExecutionListenerImpl
{
  LrvxExecutionListenerCallbacks cb;
};

// Adapter that exposes a binding's LrvxExecutionListenerCallbacks as a
// lrvx::IOrderExecutionListener — pluggable into BacktestRunner.
class CapiExecutionListener : public lrvx::IOrderExecutionListener
{
 public:
  CapiExecutionListener(lrvx::SubscriberId id, LrvxExecutionListenerImpl* impl)
      : lrvx::IOrderExecutionListener(id), _impl(impl)
  {
  }

  void onOrderSubmitted(const lrvx::Order& o) override
  {
    if (_impl && _impl->cb.on_submitted)
    {
      auto fo = packOrder(o);
      _impl->cb.on_submitted(_impl->cb.user_data, &fo);
    }
  }
  void onOrderAccepted(const lrvx::Order& o) override
  {
    if (_impl && _impl->cb.on_accepted)
    {
      auto fo = packOrder(o);
      _impl->cb.on_accepted(_impl->cb.user_data, &fo);
    }
  }
  void onOrderPartiallyFilled(const lrvx::Order& o, lrvx::Quantity q) override
  {
    if (_impl && _impl->cb.on_partially_filled)
    {
      auto fo = packOrder(o);
      _impl->cb.on_partially_filled(_impl->cb.user_data, &fo, q.raw());
    }
  }
  void onOrderFilled(const lrvx::Order& o) override
  {
    if (_impl && _impl->cb.on_filled)
    {
      auto fo = packOrder(o);
      _impl->cb.on_filled(_impl->cb.user_data, &fo);
    }
  }
  void onOrderPendingCancel(const lrvx::Order& o) override
  {
    if (_impl && _impl->cb.on_pending_cancel)
    {
      auto fo = packOrder(o);
      _impl->cb.on_pending_cancel(_impl->cb.user_data, &fo);
    }
  }
  void onOrderCanceled(const lrvx::Order& o) override
  {
    if (_impl && _impl->cb.on_canceled)
    {
      auto fo = packOrder(o);
      _impl->cb.on_canceled(_impl->cb.user_data, &fo);
    }
  }
  void onOrderExpired(const lrvx::Order& o) override
  {
    if (_impl && _impl->cb.on_expired)
    {
      auto fo = packOrder(o);
      _impl->cb.on_expired(_impl->cb.user_data, &fo);
    }
  }
  void onOrderRejected(const lrvx::Order& o, const std::string& reason) override
  {
    if (_impl && _impl->cb.on_rejected)
    {
      auto fo = packOrder(o);
      _impl->cb.on_rejected(_impl->cb.user_data, &fo, reason.c_str());
    }
  }
  void onOrderReplaced(const lrvx::Order& oldOrder, const lrvx::Order& newOrder) override
  {
    if (_impl && _impl->cb.on_replaced)
    {
      auto fo_old = packOrder(oldOrder);
      auto fo_new = packOrder(newOrder);
      _impl->cb.on_replaced(_impl->cb.user_data, &fo_old, &fo_new);
    }
  }
  void onOrderPendingTrigger(const lrvx::Order& o) override
  {
    if (_impl && _impl->cb.on_pending_trigger)
    {
      auto fo = packOrder(o);
      _impl->cb.on_pending_trigger(_impl->cb.user_data, &fo);
    }
  }
  void onOrderTriggered(const lrvx::Order& o) override
  {
    if (_impl && _impl->cb.on_triggered)
    {
      auto fo = packOrder(o);
      _impl->cb.on_triggered(_impl->cb.user_data, &fo);
    }
  }
  void onTrailingStopUpdated(const lrvx::Order& o, lrvx::Price newTrigger) override
  {
    if (_impl && _impl->cb.on_trailing_stop_updated)
    {
      auto fo = packOrder(o);
      _impl->cb.on_trailing_stop_updated(_impl->cb.user_data, &fo, newTrigger.raw());
    }
  }
  void onOrderQueuePositionChange(const lrvx::Order& o, lrvx::Quantity queueAhead,
                                  lrvx::Quantity queueTotal) override
  {
    if (_impl && _impl->cb.on_queue_position_change)
    {
      auto fo = packOrder(o);
      _impl->cb.on_queue_position_change(_impl->cb.user_data, &fo, queueAhead.raw(),
                                         queueTotal.raw());
    }
  }
  void onOrderMarketPositionChange(const lrvx::Order& o, uint8_t position,
                                   int32_t distanceToBestTicks) override
  {
    if (_impl && _impl->cb.on_market_position_change)
    {
      auto fo = packOrder(o);
      _impl->cb.on_market_position_change(_impl->cb.user_data, &fo, position,
                                          distanceToBestTicks);
    }
  }
  void onOrderReplaceSubmitted(const lrvx::Order& oldOrder,
                               const lrvx::Order& newOrder) override
  {
    if (_impl && _impl->cb.on_replace_submitted)
    {
      auto fo_old = packOrder(oldOrder);
      auto fo_new = packOrder(newOrder);
      _impl->cb.on_replace_submitted(_impl->cb.user_data, &fo_old, &fo_new);
    }
  }
  void onOrderReplaceAccepted(const lrvx::Order& oldOrder,
                              const lrvx::Order& newOrder) override
  {
    if (_impl && _impl->cb.on_replace_accepted)
    {
      auto fo_old = packOrder(oldOrder);
      auto fo_new = packOrder(newOrder);
      _impl->cb.on_replace_accepted(_impl->cb.user_data, &fo_old, &fo_new);
    }
  }
  void onOrderReplaceRejected(const lrvx::Order& oldOrder, const lrvx::Order& newOrder,
                              const std::string& reason) override
  {
    if (_impl && _impl->cb.on_replace_rejected)
    {
      auto fo_old = packOrder(oldOrder);
      auto fo_new = packOrder(newOrder);
      _impl->cb.on_replace_rejected(_impl->cb.user_data, &fo_old, &fo_new,
                                    reason.c_str());
    }
  }

 private:
  LrvxExecutionListenerImpl* _impl;
};

// Unpack an ABI-stable LrvxOrder back into a lrvx::Order. Used by the
// CapiExecutor adapter when reconstructing orders for the binding's
// submit / replace / OCO callbacks (engine-side already constructs an
// Order from a Signal; we round-trip it through LrvxOrder for the C ABI).
inline lrvx::Order unpackOrder(const LrvxOrder& fo) noexcept
{
  lrvx::Order o{};
  o.id = fo.id;
  o.clientOrderId = fo.client_order_id;
  o.symbol = fo.symbol;
  o.strategyId = fo.strategy_id;
  o.orderTag = fo.order_tag;
  o.side = (fo.side == 0) ? lrvx::Side::BUY : lrvx::Side::SELL;
  o.type = static_cast<lrvx::OrderType>(fo.type);
  o.timeInForce = static_cast<lrvx::TimeInForce>(fo.time_in_force);
  o.flags.reduceOnly = (fo.flags & 0x01) ? 1 : 0;
  o.flags.closePosition = (fo.flags & 0x02) ? 1 : 0;
  o.flags.postOnly = (fo.flags & 0x04) ? 1 : 0;
  o.flags.holdSide = static_cast<uint8_t>((fo.flags >> 3) & 0x03);
  o.price = lrvx::Price::fromRaw(fo.price_raw);
  o.quantity = lrvx::Quantity::fromRaw(fo.quantity_raw);
  o.filledQuantity = lrvx::Quantity::fromRaw(fo.filled_quantity_raw);
  o.triggerPrice = lrvx::Price::fromRaw(fo.trigger_price_raw);
  o.trailingOffset = lrvx::Price::fromRaw(fo.trailing_offset_raw);
  o.createdAt = lrvx::TimePoint{std::chrono::nanoseconds{fo.created_at_ns}};
  if (fo.exchange_ts_ns != 0)
  {
    o.exchangeTimestamp = lrvx::TimePoint{std::chrono::nanoseconds{fo.exchange_ts_ns}};
  }
  return o;
}

struct LrvxExecutorImpl
{
  LrvxExecutorCallbacks cb;
};

// Adapter that exposes a binding's LrvxExecutorCallbacks as a
// lrvx::IOrderExecutor — pluggable into BacktestRunner via setExecutor()
// and into LrvxLiveEngineImpl's executor slot.
class CapiExecutor : public lrvx::IOrderExecutor
{
 public:
  explicit CapiExecutor(LrvxExecutorImpl* impl) : _impl(impl) {}

  void start() override
  {
    if (_impl && _impl->cb.on_start)
    {
      _impl->cb.on_start(_impl->cb.user_data);
    }
  }
  void stop() override
  {
    if (_impl && _impl->cb.on_stop)
    {
      _impl->cb.on_stop(_impl->cb.user_data);
    }
  }

  void submitOrder(const lrvx::Order& o) override
  {
    if (_impl && _impl->cb.submit)
    {
      auto fo = packOrder(o);
      _impl->cb.submit(_impl->cb.user_data, &fo);
    }
  }
  void cancelOrder(lrvx::OrderId orderId) override
  {
    if (_impl && _impl->cb.cancel)
    {
      _impl->cb.cancel(_impl->cb.user_data, static_cast<uint64_t>(orderId));
    }
  }
  void cancelAllOrders(lrvx::SymbolId symbol) override
  {
    if (_impl && _impl->cb.cancel_all)
    {
      _impl->cb.cancel_all(_impl->cb.user_data, static_cast<uint32_t>(symbol));
    }
  }
  void replaceOrder(lrvx::OrderId oldId, const lrvx::Order& newOrder) override
  {
    if (_impl && _impl->cb.replace)
    {
      auto fo = packOrder(newOrder);
      _impl->cb.replace(_impl->cb.user_data, static_cast<uint64_t>(oldId), &fo);
    }
  }
  void submitOCO(const lrvx::OCOParams& params) override
  {
    if (_impl && _impl->cb.submit_oco)
    {
      auto fo1 = packOrder(params.order1);
      auto fo2 = packOrder(params.order2);
      _impl->cb.submit_oco(_impl->cb.user_data, &fo1, &fo2);
    }
  }
  lrvx::ExchangeCapabilities capabilities() const override
  {
    if (_impl == nullptr || _impl->cb.capabilities == nullptr)
    {
      return lrvx::ExchangeCapabilities{};
    }
    LrvxExchangeCapabilities cc{};
    _impl->cb.capabilities(_impl->cb.user_data, &cc);
    lrvx::ExchangeCapabilities out{};
    out.supportsStopMarket = (cc.supports_stop_market != 0);
    out.supportsStopLimit = (cc.supports_stop_limit != 0);
    out.supportsTakeProfitMarket = (cc.supports_take_profit_market != 0);
    out.supportsTakeProfitLimit = (cc.supports_take_profit_limit != 0);
    out.supportsTrailingStop = (cc.supports_trailing_stop != 0);
    out.supportsIceberg = (cc.supports_iceberg != 0);
    out.supportsOCO = (cc.supports_oco != 0);
    out.supportsGTC = (cc.supports_gtc != 0);
    out.supportsIOC = (cc.supports_ioc != 0);
    out.supportsFOK = (cc.supports_fok != 0);
    out.supportsGTD = (cc.supports_gtd != 0);
    out.supportsPostOnly = (cc.supports_post_only != 0);
    out.supportsReduceOnly = (cc.supports_reduce_only != 0);
    out.supportsClosePosition = (cc.supports_close_position != 0);
    return out;
  }

 private:
  LrvxExecutorImpl* _impl;
};

// Adapter that exposes a binding's LrvxReplaySourceCallbacks as a
// lrvx::replay::IMultiSegmentReader so BacktestRunner can drive it via
// the existing forEach contract.
class CapiReplaySourceReader : public lrvx::replay::IMultiSegmentReader
{
 public:
  explicit CapiReplaySourceReader(LrvxReplaySourceImpl* source) : _source(source) {}

  uint64_t forEach(EventCallback callback) override
  {
    if (_source == nullptr || _source->cb.next == nullptr)
    {
      return 0;
    }
    uint64_t count = 0;
    LrvxReplayEvent ev{};
    while (_source->cb.next(_source->cb.user_data, &ev) != 0)
    {
      lrvx::replay::ReplayEvent re;
      re.timestamp_ns = ev.timestamp_ns;
      switch (ev.type)
      {
        case 1:  // Trade
        {
          re.type = lrvx::replay::EventType::Trade;
          re.trade.exchange_ts_ns = ev.timestamp_ns;
          re.trade.recv_ts_ns = ev.timestamp_ns;
          re.trade.price_raw = ev.trade_price_raw;
          re.trade.qty_raw = ev.trade_quantity_raw;
          re.trade.symbol_id = ev.trade_symbol;
          re.trade.side = ev.trade_is_buy ? 0u : 1u;
          break;
        }
        case 2:  // BookSnapshot
        case 3:  // BookDelta
        {
          re.type = (ev.type == 2) ? lrvx::replay::EventType::BookSnapshot
                                   : lrvx::replay::EventType::BookDelta;
          re.book_header.exchange_ts_ns = ev.timestamp_ns;
          re.book_header.recv_ts_ns = ev.timestamp_ns;
          re.book_header.symbol_id = ev.book_symbol;
          re.book_header.bid_count = static_cast<uint16_t>(ev.n_bids);
          re.book_header.ask_count = static_cast<uint16_t>(ev.n_asks);
          re.book_header.type = static_cast<uint8_t>(re.type);
          re.bids.clear();
          re.asks.clear();
          re.bids.reserve(ev.n_bids);
          re.asks.reserve(ev.n_asks);
          for (uint32_t i = 0; i < ev.n_bids; ++i)
          {
            lrvx::replay::BookLevel lvl{};
            lvl.price_raw = ev.bids[i].price_raw;
            lvl.qty_raw = ev.bids[i].quantity_raw;
            re.bids.push_back(lvl);
          }
          for (uint32_t i = 0; i < ev.n_asks; ++i)
          {
            lrvx::replay::BookLevel lvl{};
            lvl.price_raw = ev.asks[i].price_raw;
            lvl.qty_raw = ev.asks[i].quantity_raw;
            re.asks.push_back(lvl);
          }
          break;
        }
        default:
          // Unknown event type — skip without aborting playback.
          continue;
      }
      ++count;
      if (!callback(re))
      {
        break;
      }
    }
    return count;
  }

  uint64_t forEachFrom(int64_t start_ts_ns, EventCallback callback) override
  {
    if (_source != nullptr && _source->cb.seek_to != nullptr)
    {
      _source->cb.seek_to(_source->cb.user_data, start_ts_ns);
    }
    return forEach(std::move(callback));
  }

  const std::vector<lrvx::replay::SegmentInfo>& segments() const override
  {
    return _empty_segments;
  }
  uint64_t totalEvents() const override { return 0; }

 private:
  LrvxReplaySourceImpl* _source;
  std::vector<lrvx::replay::SegmentInfo> _empty_segments;
};

// Build a LrvxOrder from a Signal for the binding-side executor hook.
// Maps SignalType → OrderType for the order types that turn into orders;
// flow-control signals (Cancel/CancelAll/Modify/OCO) don't go through
// this path because they call cancel/replace/submit_oco on the executor.
inline LrvxOrder signalToLrvxOrder(const Signal& sig) noexcept
{
  LrvxOrder fo{};
  fo.id = sig.orderId;
  fo.symbol = sig.symbol;
  fo.side = (sig.side == Side::BUY) ? 0 : 1;
  fo.time_in_force = static_cast<uint8_t>(sig.timeInForce);
  uint8_t flags = 0;
  flags |= (sig.reduceOnly ? 0x01 : 0);
  flags |= (sig.postOnly ? 0x04 : 0);
  fo.flags = flags;
  fo.price_raw = sig.price.raw();
  fo.quantity_raw = sig.quantity.raw();
  fo.trigger_price_raw = sig.triggerPrice.raw();
  fo.trailing_offset_raw = sig.trailingOffset.raw();

  switch (sig.type)
  {
    case SignalType::Market:
      fo.type = static_cast<uint8_t>(OrderType::MARKET);
      break;
    case SignalType::Limit:
      fo.type = static_cast<uint8_t>(OrderType::LIMIT);
      break;
    case SignalType::StopMarket:
      fo.type = static_cast<uint8_t>(OrderType::STOP_MARKET);
      break;
    case SignalType::StopLimit:
      fo.type = static_cast<uint8_t>(OrderType::STOP_LIMIT);
      break;
    case SignalType::TakeProfitMarket:
      fo.type = static_cast<uint8_t>(OrderType::TAKE_PROFIT_MARKET);
      break;
    case SignalType::TakeProfitLimit:
      fo.type = static_cast<uint8_t>(OrderType::TAKE_PROFIT_LIMIT);
      break;
    case SignalType::TrailingStop:
      fo.type = static_cast<uint8_t>(OrderType::TRAILING_STOP);
      break;
    default:
      fo.type = static_cast<uint8_t>(OrderType::MARKET);
      break;
  }
  return fo;
}

class RunnerSignalHandler : public ISignalHandler
{
 public:
  RunnerSignalHandler(LrvxOnSignalCallback cb, void* ud) : _cb(cb), _ud(ud) {}

  // Set or clear the optional pre-trade hooks. Safe to call from any
  // thread while consumer threads are active: the handle is resolved to a
  // strong reference through the registry, and set() swaps that reference
  // into the slot atomically. See tracker_ownership.h for why a bare
  // pointer swap is not enough here -- a dispatch thread may already be
  // inside the previous hook's callback with the old reference in hand.
  void setRiskManager(LrvxRiskManagerImpl* rm)
  {
    _risk.set(TrackerRegistry<LrvxRiskManagerImpl>::lookup(rm));
  }
  void setKillSwitch(LrvxKillSwitchImpl* ks)
  {
    _kill.set(TrackerRegistry<LrvxKillSwitchImpl>::lookup(ks));
  }
  void setOrderValidator(LrvxOrderValidatorImpl* ov)
  {
    _validator.set(TrackerRegistry<LrvxOrderValidatorImpl>::lookup(ov));
  }
  void setPnLTracker(LrvxPnLTrackerImpl* p)
  {
    _pnl.set(TrackerRegistry<LrvxPnLTrackerImpl>::lookup(p));
  }
  void setStorageSink(LrvxStorageSinkImpl* s)
  {
    _sink.set(TrackerRegistry<LrvxStorageSinkImpl>::lookup(s));
  }
  void setExecutor(LrvxExecutorImpl* e)
  {
    _executor.set(TrackerRegistry<LrvxExecutorImpl>::lookup(e));
  }
  void setTraceRecorder(void* rec) noexcept
  {
    _traceRecorder.store(rec, std::memory_order_release);
  }
  void* traceRecorder() const noexcept
  {
    return _traceRecorder.load(std::memory_order_acquire);
  }
  void setTraceFeedTsNs(int64_t ts) noexcept
  {
    _traceFeedTsNs.store(ts, std::memory_order_relaxed);
  }
  int64_t traceFeedTsNs() const noexcept
  {
    return _traceFeedTsNs.load(std::memory_order_relaxed);
  }

  void onSignal(const Signal& sig) override
  {
    // Auto-capture into the attached TraceRecorder, if any. Done before
    // the user callback so the recorded signal is in flight even if the
    // user mutates state in their callback.
    if (auto* rec = _traceRecorder.load(std::memory_order_acquire))
    {
      auto* recorder = static_cast<lrvx::run::TraceRecorder*>(rec);
      lrvx::run::SignalView view;
      view.run_ts_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
      view.feed_ts_ns = _traceFeedTsNs.load(std::memory_order_relaxed);
      view.signal_id = static_cast<uint32_t>(sig.orderId);
      view.flags = 0;
      switch (sig.type)
      {
        case SignalType::Market:
        case SignalType::Limit:
        case SignalType::StopMarket:
        case SignalType::StopLimit:
        case SignalType::TakeProfitMarket:
        case SignalType::TakeProfitLimit:
        case SignalType::TrailingStop:
        case SignalType::OCO:
          view.flags = static_cast<uint32_t>(lrvx::run::SignalFlags::Enter);
          break;
        case SignalType::Cancel:
        case SignalType::CancelAll:
          view.flags = static_cast<uint32_t>(lrvx::run::SignalFlags::Exit);
          break;
        case SignalType::Modify:
        case SignalType::ProvideLiquidity:
        case SignalType::WithdrawLiquidity:
          // Neither an entry nor an exit: a modify keeps the position it
          // already has, and the liquidity pair acts on a pool, not a
          // position. Listed rather than left to a default so the next
          // SignalType added does not silently join them.
          break;
      }
      view.strength_raw = 0;
      view.symbol_ids = {static_cast<uint32_t>(sig.symbol)};
      const char* name = "unknown";
      switch (sig.type)
      {
        case SignalType::Market:
          name = "market";
          break;
        case SignalType::Limit:
          name = "limit";
          break;
        case SignalType::Cancel:
          name = "cancel";
          break;
        case SignalType::CancelAll:
          name = "cancel_all";
          break;
        case SignalType::Modify:
          name = "modify";
          break;
        case SignalType::StopMarket:
          name = "stop_market";
          break;
        case SignalType::StopLimit:
          name = "stop_limit";
          break;
        case SignalType::TakeProfitMarket:
          name = "take_profit_market";
          break;
        case SignalType::TakeProfitLimit:
          name = "take_profit_limit";
          break;
        case SignalType::TrailingStop:
          name = "trailing_stop";
          break;
        case SignalType::OCO:
          name = "oco";
          break;
        case SignalType::ProvideLiquidity:
          name = "provide_liquidity";
          break;
        case SignalType::WithdrawLiquidity:
          name = "withdraw_liquidity";
          break;
      }
      view.name = name;
      recorder->writeSignal(view);
    }

    if (!_cb)
    {
      return;
    }
    LrvxSignal fs{};
    fs.order_id = sig.orderId;
    fs.symbol = sig.symbol;
    fs.side = (sig.side == Side::BUY) ? 0 : 1;
    fs.price = sig.price.toDouble();
    fs.quantity = sig.quantity.toDouble();
    fs.trigger_price = sig.triggerPrice.toDouble();
    fs.trailing_offset = sig.trailingOffset.toDouble();
    fs.trailing_bps = sig.trailingCallbackRate;
    fs.new_price = sig.newPrice.toDouble();
    fs.new_quantity = sig.newQuantity.toDouble();
    fs.range_lower = sig.priceLower.toDouble();
    fs.range_upper = sig.priceUpper.toDouble();
    fs.liquidity = sig.liquidity.toDouble();

    switch (sig.type)
    {
      case SignalType::Market:
        fs.order_type = LRVX_SIGNAL_TYPE_MARKET;
        break;
      case SignalType::Limit:
        fs.order_type = LRVX_SIGNAL_TYPE_LIMIT;
        break;
      case SignalType::StopMarket:
        fs.order_type = LRVX_SIGNAL_TYPE_STOP_MARKET;
        break;
      case SignalType::StopLimit:
        fs.order_type = LRVX_SIGNAL_TYPE_STOP_LIMIT;
        break;
      case SignalType::TakeProfitMarket:
        fs.order_type = LRVX_SIGNAL_TYPE_TAKE_PROFIT_MARKET;
        break;
      case SignalType::TakeProfitLimit:
        fs.order_type = LRVX_SIGNAL_TYPE_TAKE_PROFIT_LIMIT;
        break;
      case SignalType::TrailingStop:
        fs.order_type = LRVX_SIGNAL_TYPE_TRAILING_STOP;
        break;
      case SignalType::Cancel:
        fs.order_type = LRVX_SIGNAL_TYPE_CANCEL;
        break;
      case SignalType::CancelAll:
        fs.order_type = LRVX_SIGNAL_TYPE_CANCEL_ALL;
        break;
      case SignalType::Modify:
        fs.order_type = LRVX_SIGNAL_TYPE_MODIFY;
        break;
      case SignalType::OCO:
        fs.order_type = LRVX_SIGNAL_TYPE_OCO;
        break;
      case SignalType::ProvideLiquidity:
        fs.order_type = LRVX_SIGNAL_TYPE_PROVIDE_LIQUIDITY;
        break;
      case SignalType::WithdrawLiquidity:
        fs.order_type = LRVX_SIGNAL_TYPE_WITHDRAW_LIQUIDITY;
        break;
    }

    // Pre-trade gates, evaluated in order: KillSwitch → OrderValidator →
    // RiskManager. Each is optional; an unset hook or a NULL fn pointer
    // is a no-op (let the signal through). Returning 0 drops the signal
    // and skips the remaining hooks.
    // Each get() below returns a strong reference that outlives whatever
    // destroy() may do concurrently on another thread for the rest of this
    // scope -- see tracker_ownership.h.
    if (auto ks = _kill.get(); ks && ks->cb.check != nullptr)
    {
      if (ks->cb.check(ks->cb.user_data, &fs) == 0)
      {
        return;
      }
    }
    if (auto ov = _validator.get(); ov && ov->cb.validate != nullptr)
    {
      if (ov->cb.validate(ov->cb.user_data, &fs) == 0)
      {
        return;
      }
    }
    if (auto rm = _risk.get(); rm && rm->cb.allow != nullptr)
    {
      if (rm->cb.allow(rm->cb.user_data, &fs) == 0)
      {
        return;
      }
    }

    _cb(_ud, &fs);

    // Binding-supplied executor — alternative path for order routing.
    // Runs after the user on_signal so existing on_signal-based wiring
    // (where the user submits orders directly) keeps working unchanged.
    // If a user has both an executor and on_signal-based submission, the
    // order will be sent twice — that's the user's responsibility.
    if (auto exec = _executor.get(); exec)
    {
      switch (sig.type)
      {
        case SignalType::Market:
        case SignalType::Limit:
        case SignalType::StopMarket:
        case SignalType::StopLimit:
        case SignalType::TakeProfitMarket:
        case SignalType::TakeProfitLimit:
        case SignalType::TrailingStop:
          if (exec->cb.submit != nullptr)
          {
            LrvxOrder fo = signalToLrvxOrder(sig);
            exec->cb.submit(exec->cb.user_data, &fo);
          }
          break;
        case SignalType::Cancel:
          if (exec->cb.cancel != nullptr)
          {
            exec->cb.cancel(exec->cb.user_data, sig.orderId);
          }
          break;
        case SignalType::CancelAll:
          if (exec->cb.cancel_all != nullptr)
          {
            exec->cb.cancel_all(exec->cb.user_data, sig.symbol);
          }
          break;
        case SignalType::Modify:
          if (exec->cb.replace != nullptr)
          {
            LrvxOrder fo{};
            fo.id = sig.orderId;
            fo.symbol = sig.symbol;
            fo.side = (sig.side == Side::BUY) ? 0 : 1;
            fo.type = static_cast<uint8_t>(OrderType::LIMIT);
            fo.price_raw = sig.newPrice.raw();
            fo.quantity_raw = sig.newQuantity.raw();
            exec->cb.replace(exec->cb.user_data, sig.orderId, &fo);
          }
          break;
        case SignalType::OCO:
          if (exec->cb.submit_oco != nullptr)
          {
            LrvxOrder fo1 = signalToLrvxOrder(sig);
            fo1.type = static_cast<uint8_t>(OrderType::LIMIT);
            LrvxOrder fo2 = fo1;
            fo2.price_raw = sig.triggerPrice.raw();
            exec->cb.submit_oco(exec->cb.user_data, &fo1, &fo2);
          }
          break;
        case SignalType::ProvideLiquidity:
        case SignalType::WithdrawLiquidity:
          // DEX/AMM liquidity ops are handled by the on-chain connector,
          // not the CEX executor callback surface.
          break;
      }
    }

    // Post-emission observers, fired in declared order: PnL → Storage.
    // Return type is void; observers cannot drop the signal (it's already
    // been delivered). The user callback runs first so the binding's
    // hot-path latency isn't affected by observer cost.
    if (auto pnl = _pnl.get(); pnl && pnl->cb.on_signal != nullptr)
    {
      pnl->cb.on_signal(pnl->cb.user_data, &fs);
    }
    if (auto sink = _sink.get(); sink && sink->cb.store != nullptr)
    {
      sink->cb.store(sink->cb.user_data, &fs);
    }
  }

 private:
  LrvxOnSignalCallback _cb;
  void* _ud;
  TrackerSlot<LrvxRiskManagerImpl> _risk;
  TrackerSlot<LrvxKillSwitchImpl> _kill;
  TrackerSlot<LrvxOrderValidatorImpl> _validator;
  TrackerSlot<LrvxPnLTrackerImpl> _pnl;
  TrackerSlot<LrvxStorageSinkImpl> _sink;
  TrackerSlot<LrvxExecutorImpl> _executor;
  // Optional trace recorder. Owned by caller; written into on every
  // signal so the run captures without per-strategy instrumentation.
  std::atomic<void*> _traceRecorder{nullptr};
  std::atomic<int64_t> _traceFeedTsNs{0};
};

struct LrvxRunnerImpl
{
  SymbolRegistry* registry;
  RunnerSignalHandler handler;
  std::vector<BridgeStrategy*> strategies;
  std::pmr::unsynchronized_pool_resource pool;

  // Optional market data recorder hook. Owned by caller; non-owning ptr.
  std::atomic<LrvxMarketDataRecorderImpl*> recorder{nullptr};
  // Tracks whether on_start has fired without a matching on_stop, so that
  // attaching mid-run or detaching emits the right lifecycle callback.
  std::atomic<bool> recorderRunning{false};

  // Optional binding-supplied executor. Same lifecycle pattern as recorder.
  std::atomic<LrvxExecutorImpl*> executor{nullptr};
  std::atomic<bool> executorRunning{false};

  LrvxRunnerImpl(SymbolRegistry* reg, LrvxOnSignalCallback cb, void* ud)
      : registry(reg), handler(cb, ud)
  {
  }

  void addStrategy(BridgeStrategy* s)
  {
    s->setSignalHandler(&handler);
    strategies.push_back(s);
  }

  void setRiskManager(LrvxRiskManagerImpl* rm) { handler.setRiskManager(rm); }
  void setKillSwitch(LrvxKillSwitchImpl* ks) { handler.setKillSwitch(ks); }
  void setOrderValidator(LrvxOrderValidatorImpl* ov)
  {
    handler.setOrderValidator(ov);
  }
  void setPnLTracker(LrvxPnLTrackerImpl* p) { handler.setPnLTracker(p); }
  void setStorageSink(LrvxStorageSinkImpl* s) { handler.setStorageSink(s); }
  void attachTraceRecorder(void* rec) { handler.setTraceRecorder(rec); }
  void setTraceFeedTsNs(int64_t ts) { handler.setTraceFeedTsNs(ts); }
  lrvx::run::TraceRecorder* traceRecorder() const noexcept
  {
    return static_cast<lrvx::run::TraceRecorder*>(handler.traceRecorder());
  }
  int64_t traceFeedTsNs() const noexcept { return handler.traceFeedTsNs(); }

  // Attach / detach a binding-supplied executor. Lifecycle (on_start /
  // on_stop) is balanced against runner start/stop, with hot-swap
  // semantics so attach-while-running and detach fire the lifecycle
  // callbacks correctly.
  void setExecutor(LrvxExecutorImpl* e)
  {
    auto* prev = executor.exchange(e, std::memory_order_acq_rel);
    handler.setExecutor(e);
    if (executorRunning.load(std::memory_order_acquire))
    {
      if (prev != nullptr && prev->cb.on_stop != nullptr)
      {
        prev->cb.on_stop(prev->cb.user_data);
      }
      if (e != nullptr && e->cb.on_start != nullptr)
      {
        e->cb.on_start(e->cb.user_data);
      }
    }
  }

  // Attach / detach a market data recorder. If the runner is already started
  // (recorderRunning == true), fire on_stop on the outgoing recorder and
  // on_start on the incoming one so the lifecycle stays balanced.
  void setMarketDataRecorder(LrvxMarketDataRecorderImpl* r)
  {
    auto* prev = recorder.exchange(r, std::memory_order_acq_rel);
    if (recorderRunning.load(std::memory_order_acquire))
    {
      if (prev != nullptr && prev->cb.on_stop != nullptr)
      {
        prev->cb.on_stop(prev->cb.user_data);
      }
      if (r != nullptr && r->cb.on_start != nullptr)
      {
        r->cb.on_start(r->cb.user_data);
      }
    }
  }

  void start()
  {
    for (auto* s : strategies)
    {
      s->start();
    }
    recorderRunning.store(true, std::memory_order_release);
    if (auto* r = recorder.load(std::memory_order_acquire);
        r != nullptr && r->cb.on_start != nullptr)
    {
      r->cb.on_start(r->cb.user_data);
    }
    executorRunning.store(true, std::memory_order_release);
    if (auto* e = executor.load(std::memory_order_acquire);
        e != nullptr && e->cb.on_start != nullptr)
    {
      e->cb.on_start(e->cb.user_data);
    }
  }

  void stop()
  {
    if (auto* e = executor.load(std::memory_order_acquire);
        e != nullptr && e->cb.on_stop != nullptr)
    {
      e->cb.on_stop(e->cb.user_data);
    }
    executorRunning.store(false, std::memory_order_release);
    if (auto* r = recorder.load(std::memory_order_acquire);
        r != nullptr && r->cb.on_stop != nullptr)
    {
      r->cb.on_stop(r->cb.user_data);
    }
    recorderRunning.store(false, std::memory_order_release);
    for (auto* s : strategies)
    {
      s->stop();
    }
  }

  void onTrade(uint32_t symbol, double price, double qty, bool is_buy, int64_t ts_ns)
  {
    Trade trade{};
    trade.symbol = symbol;
    trade.price = Price::fromDouble(price);
    trade.quantity = Quantity::fromDouble(qty);
    trade.isBuy = is_buy;
    trade.exchangeTsNs = UnixNanos(ts_ns);

    TradeEvent ev{};
    ev.trade = trade;

    for (auto* s : strategies)
    {
      s->onTrade(ev);
    }

    if (auto* r = recorder.load(std::memory_order_acquire);
        r != nullptr && r->cb.on_trade != nullptr)
    {
      LrvxTradeData td{};
      td.symbol = symbol;
      td.price_raw = trade.price.raw();
      td.quantity_raw = trade.quantity.raw();
      td.is_buy = is_buy ? 1 : 0;
      td.exchange_ts_ns = ts_ns;
      r->cb.on_trade(r->cb.user_data, &td);
    }
  }

  void onBookSnapshot(uint32_t symbol,
                      const double* bid_prices, const double* bid_qtys, uint32_t n_bids,
                      const double* ask_prices, const double* ask_qtys, uint32_t n_asks,
                      int64_t ts_ns)
  {
    BookUpdateEvent ev(&pool);
    ev.update.symbol = symbol;
    ev.update.type = BookUpdateType::SNAPSHOT;
    ev.update.exchangeTsNs = UnixNanos(ts_ns);

    for (uint32_t i = 0; i < n_bids; ++i)
    {
      ev.update.bids.push_back({Price::fromDouble(bid_prices[i]),
                                Quantity::fromDouble(bid_qtys[i])});
    }
    for (uint32_t i = 0; i < n_asks; ++i)
    {
      ev.update.asks.push_back({Price::fromDouble(ask_prices[i]),
                                Quantity::fromDouble(ask_qtys[i])});
    }

    for (auto* s : strategies)
    {
      s->onBookUpdate(ev);
    }

    if (auto* r = recorder.load(std::memory_order_acquire);
        r != nullptr && r->cb.on_book_update != nullptr)
    {
      // Re-pack levels into LrvxBookLevel for the C ABI. Stack buffers up
      // to a small threshold; heap-fall back for deep books.
      constexpr uint32_t kStackLevels = 64;
      LrvxBookLevel stackBids[kStackLevels];
      LrvxBookLevel stackAsks[kStackLevels];
      std::vector<LrvxBookLevel> heapBids;
      std::vector<LrvxBookLevel> heapAsks;
      LrvxBookLevel* bidPtr = stackBids;
      LrvxBookLevel* askPtr = stackAsks;
      if (n_bids > kStackLevels)
      {
        heapBids.resize(n_bids);
        bidPtr = heapBids.data();
      }
      if (n_asks > kStackLevels)
      {
        heapAsks.resize(n_asks);
        askPtr = heapAsks.data();
      }
      for (uint32_t i = 0; i < n_bids; ++i)
      {
        bidPtr[i].price_raw = ev.update.bids[i].price.raw();
        bidPtr[i].quantity_raw = ev.update.bids[i].quantity.raw();
      }
      for (uint32_t i = 0; i < n_asks; ++i)
      {
        askPtr[i].price_raw = ev.update.asks[i].price.raw();
        askPtr[i].quantity_raw = ev.update.asks[i].quantity.raw();
      }
      r->cb.on_book_update(r->cb.user_data, symbol, /*is_snapshot=*/1u,
                           bidPtr, n_bids, askPtr, n_asks, ts_ns);
    }
  }

  void onBar(uint32_t symbol, uint8_t bar_type, uint64_t bar_type_param,
             double open, double high, double low, double close,
             double volume, double buy_volume,
             int64_t start_time_ns, int64_t end_time_ns,
             uint8_t close_reason)
  {
    BarEvent ev{};
    ev.symbol = symbol;
    ev.barType = static_cast<BarType>(bar_type);
    ev.barTypeParam = bar_type_param;
    ev.bar.open = Price::fromDouble(open);
    ev.bar.high = Price::fromDouble(high);
    ev.bar.low = Price::fromDouble(low);
    ev.bar.close = Price::fromDouble(close);
    ev.bar.volume = Volume::fromDouble(volume);
    ev.bar.buyVolume = Volume::fromDouble(buy_volume);
    ev.bar.startTime = TimePoint{std::chrono::nanoseconds{start_time_ns}};
    ev.bar.endTime = TimePoint{std::chrono::nanoseconds{end_time_ns}};
    ev.bar.reason = static_cast<BarCloseReason>(close_reason);

    for (auto* s : strategies)
    {
      s->onBar(ev);
    }
  }
};

static LrvxRunnerImpl* toRunner(LrvxRunnerHandle h)
{
  return static_cast<LrvxRunnerImpl*>(h);
}

// ============================================================
// LrvxLiveEngineImpl — real Disruptor-based live engine
// ============================================================

struct LrvxLiveEngineImpl
{
  SymbolRegistry* registry;
  std::unique_ptr<TradeBus> tradeBus;
  std::unique_ptr<BookUpdateBus> bookBus;
  std::unique_ptr<BarBus> barBus;
  pool::Pool<BookUpdateEvent, config::DEFAULT_CONNECTOR_POOL_CAPACITY> bookPool;

  // Per-strategy signal handlers (owned here, outlive strategies)
  std::vector<std::unique_ptr<RunnerSignalHandler>> handlers;
  std::vector<BridgeStrategy*> strategies;

  // Latest pre-trade hooks attached to this engine. Stored here (rather
  // than only on the handlers) so newly-added strategies inherit the
  // existing setting. Caller owns the *Impl objects; we hold non-owning ptrs.
  std::atomic<LrvxRiskManagerImpl*> riskManager{nullptr};
  std::atomic<LrvxKillSwitchImpl*> killSwitch{nullptr};
  std::atomic<LrvxOrderValidatorImpl*> orderValidator{nullptr};
  std::atomic<LrvxPnLTrackerImpl*> pnlTracker{nullptr};
  std::atomic<LrvxStorageSinkImpl*> storageSink{nullptr};
  std::atomic<LrvxMarketDataRecorderImpl*> recorder{nullptr};
  std::atomic<bool> recorderRunning{false};
  std::atomic<LrvxExecutorImpl*> executor{nullptr};
  std::atomic<bool> executorRunning{false};

  explicit LrvxLiveEngineImpl(SymbolRegistry* reg)
      : registry(reg),
        tradeBus(std::make_unique<TradeBus>()),
        bookBus(std::make_unique<BookUpdateBus>()),
        barBus(std::make_unique<BarBus>())
  {
  }

  void addStrategy(BridgeStrategy* s, LrvxOnSignalCallback cb, void* ud)
  {
    auto h = std::make_unique<RunnerSignalHandler>(cb, ud);
    h->setRiskManager(riskManager.load(std::memory_order_acquire));
    h->setKillSwitch(killSwitch.load(std::memory_order_acquire));
    h->setOrderValidator(orderValidator.load(std::memory_order_acquire));
    h->setPnLTracker(pnlTracker.load(std::memory_order_acquire));
    h->setStorageSink(storageSink.load(std::memory_order_acquire));
    h->setExecutor(executor.load(std::memory_order_acquire));
    s->setSignalHandler(h.get());
    handlers.push_back(std::move(h));
    tradeBus->subscribe(s);
    bookBus->subscribe(s);
    barBus->subscribe(s);
    strategies.push_back(s);
  }

  // Update the pre-trade hooks on every existing handler and remember the
  // setting so subsequently-added strategies inherit it.
  void setRiskManager(LrvxRiskManagerImpl* rm)
  {
    riskManager.store(rm, std::memory_order_release);
    for (auto& h : handlers)
    {
      h->setRiskManager(rm);
    }
  }
  void setKillSwitch(LrvxKillSwitchImpl* ks)
  {
    killSwitch.store(ks, std::memory_order_release);
    for (auto& h : handlers)
    {
      h->setKillSwitch(ks);
    }
  }
  void setOrderValidator(LrvxOrderValidatorImpl* ov)
  {
    orderValidator.store(ov, std::memory_order_release);
    for (auto& h : handlers)
    {
      h->setOrderValidator(ov);
    }
  }
  void setPnLTracker(LrvxPnLTrackerImpl* p)
  {
    pnlTracker.store(p, std::memory_order_release);
    for (auto& h : handlers)
    {
      h->setPnLTracker(p);
    }
  }
  void setStorageSink(LrvxStorageSinkImpl* s)
  {
    storageSink.store(s, std::memory_order_release);
    for (auto& h : handlers)
    {
      h->setStorageSink(s);
    }
  }

  // Attach / detach a market data recorder. Lifecycle (on_start/on_stop)
  // is balanced against engine.start()/stop(), mirroring runner semantics.
  void setMarketDataRecorder(LrvxMarketDataRecorderImpl* r)
  {
    auto* prev = recorder.exchange(r, std::memory_order_acq_rel);
    if (recorderRunning.load(std::memory_order_acquire))
    {
      if (prev != nullptr && prev->cb.on_stop != nullptr)
      {
        prev->cb.on_stop(prev->cb.user_data);
      }
      if (r != nullptr && r->cb.on_start != nullptr)
      {
        r->cb.on_start(r->cb.user_data);
      }
    }
  }

  // Attach / detach a binding-supplied executor. Updates every existing
  // signal handler (each strategy has its own) and remembers the setting
  // so subsequently-added strategies inherit it. Lifecycle balanced
  // against engine.start()/stop().
  void setExecutor(LrvxExecutorImpl* e)
  {
    auto* prev = executor.exchange(e, std::memory_order_acq_rel);
    for (auto& h : handlers)
    {
      h->setExecutor(e);
    }
    if (executorRunning.load(std::memory_order_acquire))
    {
      if (prev != nullptr && prev->cb.on_stop != nullptr)
      {
        prev->cb.on_stop(prev->cb.user_data);
      }
      if (e != nullptr && e->cb.on_start != nullptr)
      {
        e->cb.on_start(e->cb.user_data);
      }
    }
  }

  void start()
  {
    tradeBus->start();
    bookBus->start();
    barBus->start();
    for (auto* s : strategies)
    {
      s->start();
    }
    recorderRunning.store(true, std::memory_order_release);
    if (auto* r = recorder.load(std::memory_order_acquire);
        r != nullptr && r->cb.on_start != nullptr)
    {
      r->cb.on_start(r->cb.user_data);
    }
    executorRunning.store(true, std::memory_order_release);
    if (auto* e = executor.load(std::memory_order_acquire);
        e != nullptr && e->cb.on_start != nullptr)
    {
      e->cb.on_start(e->cb.user_data);
    }
  }

  void stop()
  {
    if (auto* e = executor.load(std::memory_order_acquire);
        e != nullptr && e->cb.on_stop != nullptr)
    {
      e->cb.on_stop(e->cb.user_data);
    }
    executorRunning.store(false, std::memory_order_release);
    if (auto* r = recorder.load(std::memory_order_acquire);
        r != nullptr && r->cb.on_stop != nullptr)
    {
      r->cb.on_stop(r->cb.user_data);
    }
    recorderRunning.store(false, std::memory_order_release);
    for (auto* s : strategies)
    {
      s->stop();
    }
    tradeBus->stop();
    bookBus->stop();
    barBus->stop();
  }

  void publishTrade(uint32_t symbol, double price, double qty, bool is_buy, int64_t ts_ns)
  {
    TradeEvent ev{};
    ev.trade.symbol = symbol;
    ev.trade.price = Price::fromDouble(price);
    ev.trade.quantity = Quantity::fromDouble(qty);
    ev.trade.isBuy = is_buy;
    ev.trade.exchangeTsNs = UnixNanos(ts_ns);
    tradeBus->publish(ev);

    // Recorder runs on the publisher thread (caller), before publish
    // becomes visible to consumer-side strategies. This mirrors "the
    // engine is being fed this event"; consumer-side timing isn't part
    // of the recorder contract.
    if (auto* r = recorder.load(std::memory_order_acquire);
        r != nullptr && r->cb.on_trade != nullptr)
    {
      LrvxTradeData td{};
      td.symbol = symbol;
      td.price_raw = ev.trade.price.raw();
      td.quantity_raw = ev.trade.quantity.raw();
      td.is_buy = is_buy ? 1 : 0;
      td.exchange_ts_ns = ts_ns;
      r->cb.on_trade(r->cb.user_data, &td);
    }
  }

  void publishBookSnapshot(uint32_t symbol,
                           const double* bid_prices, const double* bid_qtys, uint32_t n_bids,
                           const double* ask_prices, const double* ask_qtys, uint32_t n_asks,
                           int64_t ts_ns)
  {
    auto evOpt = bookPool.acquire();
    if (!evOpt)
    {
      return;
    }
    auto& ev = *evOpt;
    ev->update.symbol = symbol;
    ev->update.type = BookUpdateType::SNAPSHOT;
    ev->update.exchangeTsNs = UnixNanos(ts_ns);
    ev->update.bids.clear();
    ev->update.asks.clear();
    for (uint32_t i = 0; i < n_bids; ++i)
    {
      ev->update.bids.push_back({Price::fromDouble(bid_prices[i]),
                                 Quantity::fromDouble(bid_qtys[i])});
    }
    for (uint32_t i = 0; i < n_asks; ++i)
    {
      ev->update.asks.push_back({Price::fromDouble(ask_prices[i]),
                                 Quantity::fromDouble(ask_qtys[i])});
    }

    // Mirror to recorder before publish — see publishTrade comment.
    if (auto* r = recorder.load(std::memory_order_acquire);
        r != nullptr && r->cb.on_book_update != nullptr)
    {
      constexpr uint32_t kStackLevels = 64;
      LrvxBookLevel stackBids[kStackLevels];
      LrvxBookLevel stackAsks[kStackLevels];
      std::vector<LrvxBookLevel> heapBids;
      std::vector<LrvxBookLevel> heapAsks;
      LrvxBookLevel* bidPtr = stackBids;
      LrvxBookLevel* askPtr = stackAsks;
      if (n_bids > kStackLevels)
      {
        heapBids.resize(n_bids);
        bidPtr = heapBids.data();
      }
      if (n_asks > kStackLevels)
      {
        heapAsks.resize(n_asks);
        askPtr = heapAsks.data();
      }
      for (uint32_t i = 0; i < n_bids; ++i)
      {
        bidPtr[i].price_raw = ev->update.bids[i].price.raw();
        bidPtr[i].quantity_raw = ev->update.bids[i].quantity.raw();
      }
      for (uint32_t i = 0; i < n_asks; ++i)
      {
        askPtr[i].price_raw = ev->update.asks[i].price.raw();
        askPtr[i].quantity_raw = ev->update.asks[i].quantity.raw();
      }
      r->cb.on_book_update(r->cb.user_data, symbol, /*is_snapshot=*/1u,
                           bidPtr, n_bids, askPtr, n_asks, ts_ns);
    }

    bookBus->publish(std::move(evOpt.value()));
  }

  void publishBar(uint32_t symbol, uint8_t bar_type, uint64_t bar_type_param,
                  double open, double high, double low, double close,
                  double volume, double buy_volume,
                  int64_t start_time_ns, int64_t end_time_ns,
                  uint8_t close_reason)
  {
    BarEvent ev{};
    ev.symbol = symbol;
    ev.barType = static_cast<BarType>(bar_type);
    ev.barTypeParam = bar_type_param;
    ev.bar.open = Price::fromDouble(open);
    ev.bar.high = Price::fromDouble(high);
    ev.bar.low = Price::fromDouble(low);
    ev.bar.close = Price::fromDouble(close);
    ev.bar.volume = Volume::fromDouble(volume);
    ev.bar.buyVolume = Volume::fromDouble(buy_volume);
    ev.bar.startTime = TimePoint{std::chrono::nanoseconds{start_time_ns}};
    ev.bar.endTime = TimePoint{std::chrono::nanoseconds{end_time_ns}};
    ev.bar.reason = static_cast<BarCloseReason>(close_reason);
    barBus->publish(ev);
  }
};

static LrvxLiveEngineImpl* toLiveEngine(LrvxLiveEngineHandle h)
{
  return static_cast<LrvxLiveEngineImpl*>(h);
}

// ──────────────────────────────────────────────────────────────
// OhlcvBacktestReader — synthesises trade events from OHLCV bars
// ──────────────────────────────────────────────────────────────

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
    for (const auto& b : _bars)
    {
      if (!cb(make(b)))
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
    for (const auto& b : _bars)
    {
      if (b.ts_ns < start_ns)
      {
        continue;
      }
      if (!cb(make(b)))
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
  static replay::ReplayEvent make(const Bar& b)
  {
    replay::ReplayEvent ev{};
    ev.type = replay::EventType::Trade;
    ev.timestamp_ns = b.ts_ns;
    ev.trade.exchange_ts_ns = b.ts_ns;
    ev.trade.price_raw = b.price_raw;
    ev.trade.qty_raw = Quantity::fromDouble(1.0).raw();
    ev.trade.symbol_id = b.symbol_id;
    ev.trade.side = 0;  // buy, in the tape's encoding
    return ev;
  }

  std::vector<Bar> _bars;
  std::vector<replay::SegmentInfo> _segs;
};

// ──────────────────────────────────────────────────────────────
// Adapters: wrap callback-shaped LrvxRiskManagerImpl /
// LrvxKillSwitchImpl / LrvxOrderValidatorImpl / LrvxPnLTrackerImpl
// into engine-side I* interfaces so the new BacktestRunner setters
// (which are C++-typed) accept the existing C ABI handles bindings
// already create. The live runner doesn't need this bridge — it
// gates at signal-level inside RunnerSignalHandler — but the
// engine-level BacktestRunner takes Order-shaped callbacks, so we
// build a LrvxSignal from the Order and forward.
// ──────────────────────────────────────────────────────────────

// lrvx::OrderType and the LRVX_SIGNAL_TYPE_* wire codes disagree on 0
// and 1 (OrderType::LIMIT is 0, the wire code for LIMIT is 1), so a raw
// static_cast here made LrvxSignal.order_type mean one thing on the
// strategy-signal path and the opposite on the risk-manager /
// kill-switch / order-validator / PnL-tracker callbacks. Map explicitly.
inline uint8_t signalTypeCodeFromOrderType(OrderType type) noexcept
{
  switch (type)
  {
    case OrderType::MARKET:
      return LRVX_SIGNAL_TYPE_MARKET;
    case OrderType::LIMIT:
      return LRVX_SIGNAL_TYPE_LIMIT;
    case OrderType::STOP_MARKET:
      return LRVX_SIGNAL_TYPE_STOP_MARKET;
    case OrderType::STOP_LIMIT:
      return LRVX_SIGNAL_TYPE_STOP_LIMIT;
    case OrderType::TAKE_PROFIT_MARKET:
      return LRVX_SIGNAL_TYPE_TAKE_PROFIT_MARKET;
    case OrderType::TAKE_PROFIT_LIMIT:
      return LRVX_SIGNAL_TYPE_TAKE_PROFIT_LIMIT;
    case OrderType::TRAILING_STOP:
      return LRVX_SIGNAL_TYPE_TRAILING_STOP;
    case OrderType::ICEBERG:
      return LRVX_SIGNAL_TYPE_ICEBERG;
  }
  return LRVX_SIGNAL_TYPE_MARKET;
}

inline LrvxSignal orderToLrvxSignal(const Order& order) noexcept
{
  LrvxSignal fs{};
  fs.order_id = order.id;
  fs.symbol = order.symbol;
  fs.side = (order.side == Side::BUY) ? 0 : 1;
  fs.price = order.price.toDouble();
  fs.quantity = order.quantity.toDouble();
  fs.order_type = signalTypeCodeFromOrderType(order.type);
  return fs;
}

class CapiBacktestRiskManager : public lrvx::IRiskManager
{
 public:
  explicit CapiBacktestRiskManager(LrvxRiskManagerImpl* impl) : _impl(impl) {}

  bool allow(const Order& order) const override
  {
    if (_impl == nullptr || _impl->cb.allow == nullptr)
    {
      return true;
    }
    LrvxSignal fs = orderToLrvxSignal(order);
    return _impl->cb.allow(_impl->cb.user_data, &fs) != 0;
  }

 private:
  LrvxRiskManagerImpl* _impl;
};

class CapiBacktestKillSwitch : public lrvx::IKillSwitch
{
 public:
  explicit CapiBacktestKillSwitch(LrvxKillSwitchImpl* impl) : _impl(impl) {}

  // The C ABI kill switch is a per-signal check. The engine contract
  // is "check(order) may trigger; isTriggered() reports current state".
  //
  // Triggering latches, and only lrvx_kill_switch_reset clears it. The
  // assignment this used to be let a single "allow" answer un-trigger the
  // switch: after a callback said HALT, the next five orders executed and two
  // round trips completed. The same user code through the Python adapter
  // stayed halted, so one strategy produced two different backtest results
  // depending on which binding ran it.
  void check(const Order& order) override
  {
    if (_impl == nullptr || _impl->cb.check == nullptr)
    {
      return;
    }
    LrvxSignal fs = orderToLrvxSignal(order);
    // C ABI: 0 → drop / kill-switch active.
    if (_impl->cb.check(_impl->cb.user_data, &fs) == 0)
    {
      _triggered = true;
    }
  }
  void trigger(const std::string& reason) override
  {
    _triggered = true;
    _reason = reason;
  }
  bool isTriggered() const override { return _triggered; }
  std::string reason() const override { return _reason; }

 private:
  LrvxKillSwitchImpl* _impl;
  bool _triggered{false};
  std::string _reason;
};

class CapiBacktestOrderValidator : public lrvx::IOrderValidator
{
 public:
  explicit CapiBacktestOrderValidator(LrvxOrderValidatorImpl* impl) : _impl(impl) {}

  // The C ABI validator returns 0/1 without a reason string. Engine
  // contract returns a `reason` parameter — leave it empty when the
  // C ABI rejects so the rejected event still surfaces upward; the
  // user-visible reason can be plumbed through the binding's own
  // PyOrderValidator if richer messaging is needed.
  bool validate(const Order& order, std::string& /*reason*/) const override
  {
    if (_impl == nullptr || _impl->cb.validate == nullptr)
    {
      return true;
    }
    LrvxSignal fs = orderToLrvxSignal(order);
    return _impl->cb.validate(_impl->cb.user_data, &fs) != 0;
  }

 private:
  LrvxOrderValidatorImpl* _impl;
};

class CapiBacktestPnLTracker : public lrvx::IPnLTracker
{
 public:
  explicit CapiBacktestPnLTracker(LrvxPnLTrackerImpl* impl) : _impl(impl) {}

  // Engine fires onOrderFilled per fill; C ABI signature takes a
  // LrvxSignal. Convert and forward — bindings that want richer fill
  // detail should attach an ExecutionListener instead.
  void onOrderFilled(const Order& order) override
  {
    if (_impl == nullptr || _impl->cb.on_signal == nullptr)
    {
      return;
    }
    LrvxSignal fs = orderToLrvxSignal(order);
    _impl->cb.on_signal(_impl->cb.user_data, &fs);
  }

 private:
  LrvxPnLTrackerImpl* _impl;
};

// ──────────────────────────────────────────────────────────────
// LrvxBacktestRunnerImpl
// ──────────────────────────────────────────────────────────────

struct LrvxBacktestRunnerImpl
{
  SymbolRegistry* registry;
  std::unique_ptr<BacktestRunner> runner;
  // Adapters created when bindings register execution listeners. Owned
  // here so they outlive the listener registration on the runner.
  std::vector<std::unique_ptr<CapiExecutionListener>> executionListenerAdapters;
  // Adapter for the binding-supplied executor. Owned so it outlives the
  // runner's non-owning pointer. Replaced on every set call.
  std::unique_ptr<CapiExecutor> executorAdapter;
  // Adapters for the four pre-trade gate hooks. The runner stores
  // raw IRiskManager / IKillSwitch / etc pointers; we own the
  // adapters so they outlive the runner's non-owning references.
  // Replaced on every set call (NULL detaches).
  std::unique_ptr<CapiBacktestRiskManager> riskAdapter;
  std::unique_ptr<CapiBacktestKillSwitch> killAdapter;
  std::unique_ptr<CapiBacktestOrderValidator> validatorAdapter;
  std::unique_ptr<CapiBacktestPnLTracker> pnlAdapter;
  // Most recent BacktestResult, kept after each run_* call so bindings
  // can fetch the equity curve / trades without re-running the
  // backtest. Replaced on every run.
  std::optional<BacktestResult> lastResult;

  explicit LrvxBacktestRunnerImpl(SymbolRegistry* reg, double feeRate, double initialCapital)
      : registry(reg)
  {
    BacktestConfig cfg{};
    cfg.feeRate = feeRate;
    cfg.initialCapital = initialCapital;
    cfg.usePercentageFee = true;
    runner = std::make_unique<BacktestRunner>(cfg);
  }

  void addExecutionListener(LrvxExecutionListenerImpl* impl)
  {
    if (impl == nullptr)
    {
      return;
    }
    auto id = static_cast<lrvx::SubscriberId>(executionListenerAdapters.size() + 1);
    auto adapter = std::make_unique<CapiExecutionListener>(id, impl);
    runner->addExecutionListener(adapter.get());
    executionListenerAdapters.push_back(std::move(adapter));
  }

  void setExecutor(LrvxExecutorImpl* impl)
  {
    if (impl == nullptr)
    {
      runner->setExecutor(nullptr);
      executorAdapter.reset();
      return;
    }
    executorAdapter = std::make_unique<CapiExecutor>(impl);
    runner->setExecutor(executorAdapter.get());
  }

  void setStrategy(BridgeStrategy* bridge)
  {
    runner->setStrategy(bridge);
  }

  // Pre-trade gate parity with the live runner. NULL detaches.
  // The adapter owns conversion from LrvxSignal-shaped callbacks
  // into the engine's Order-shaped I* interfaces.
  void setRiskManager(LrvxRiskManagerImpl* rm)
  {
    if (rm == nullptr)
    {
      runner->setRiskManager(nullptr);
      riskAdapter.reset();
      return;
    }
    riskAdapter = std::make_unique<CapiBacktestRiskManager>(rm);
    runner->setRiskManager(riskAdapter.get());
  }
  void setKillSwitch(LrvxKillSwitchImpl* ks)
  {
    if (ks == nullptr)
    {
      runner->setKillSwitch(nullptr);
      killAdapter.reset();
      return;
    }
    killAdapter = std::make_unique<CapiBacktestKillSwitch>(ks);
    runner->setKillSwitch(killAdapter.get());
  }
  void setOrderValidator(LrvxOrderValidatorImpl* ov)
  {
    if (ov == nullptr)
    {
      runner->setOrderValidator(nullptr);
      validatorAdapter.reset();
      return;
    }
    validatorAdapter = std::make_unique<CapiBacktestOrderValidator>(ov);
    runner->setOrderValidator(validatorAdapter.get());
  }
  void setPnLTracker(LrvxPnLTrackerImpl* tracker)
  {
    if (tracker == nullptr)
    {
      runner->setPnLTracker(nullptr);
      pnlAdapter.reset();
      return;
    }
    pnlAdapter = std::make_unique<CapiBacktestPnLTracker>(tracker);
    runner->setPnLTracker(pnlAdapter.get());
  }

  // Guesses a timestamp's unit from its magnitude and scales it to
  // nanoseconds. This is ONLY valid where the unit is genuinely unknown --
  // a CSV column can hold seconds, milliseconds, microseconds, or
  // nanoseconds depending on who wrote the file, and there is no contract
  // to consult. runCsv() is the one caller with that excuse.
  //
  // runOhlcv() and runFullBars() are not: their C ABI parameters are
  // documented and named as nanoseconds already (timestamps_ns,
  // start_time_ns / end_time_ns). Running an already-correct nanosecond
  // value back through this guesser corrupts it instead of leaving it
  // alone -- anything under 1e12 ns (the first eleven and a half days
  // after the epoch, an ordinary timestamp for synthetic/offset test data)
  // reads as "probably seconds" and gets rescaled by another 1e9, which
  // silently wraps on overflow in a release build and aborts under a
  // sanitizer. Contract-known callers pass their values through untouched.
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

  uint32_t resolveSymbol(const char* symbol) const
  {
    if (!symbol || symbol[0] == '\0')
    {
      auto all = registry->getAllSymbols();
      return all.empty() ? 0 : all.front().id;
    }
    auto all = registry->getAllSymbols();
    for (const auto& info : all)
    {
      if (info.symbol == symbol)
      {
        return info.id;
      }
    }
    return 0;
  }

  static void fillStats(const BacktestStats& s, LrvxBacktestStats* out)
  {
    out->totalTrades = s.totalTrades;
    out->winningTrades = s.winningTrades;
    out->losingTrades = s.losingTrades;
    out->maxConsecutiveWins = s.maxConsecutiveWins;
    out->maxConsecutiveLosses = s.maxConsecutiveLosses;
    out->initialCapital = s.initialCapital;
    out->finalCapital = s.finalCapital;
    out->totalPnl = s.totalPnl;
    out->totalFees = s.totalFees;
    out->netPnl = s.netPnl;
    out->grossProfit = s.grossProfit;
    out->grossLoss = s.grossLoss;
    out->maxDrawdown = s.maxDrawdown;
    out->maxDrawdownPct = s.maxDrawdownPct;
    out->winRate = s.winRate;
    out->profitFactor = s.profitFactor;
    out->avgWin = s.avgWin;
    out->avgLoss = s.avgLoss;
    out->avgWinLossRatio = s.avgWinLossRatio;
    out->sharpeRatio = s.sharpeRatio;
    out->sortinoRatio = s.sortinoRatio;
    out->calmarRatio = s.calmarRatio;
    out->returnPct = s.returnPct;
  }

  int runBars(std::vector<OhlcvBacktestReader::Bar> bars, LrvxBacktestStats* out)
  {
    OhlcvBacktestReader reader(std::move(bars));
    BacktestResult result = runner->run(reader);
    if (out)
    {
      fillStats(result.computeStats(), out);
    }
    lastResult = std::move(result);
    return 1;
  }

  int runTape(const char* tape_dir, LrvxBacktestStats* out)
  {
    if (!runner)
    {
      return 0;
    }
    try
    {
      BacktestResult result = runner->runTape(std::filesystem::path(tape_dir));
      if (out)
      {
        fillStats(result.computeStats(), out);
      }
      lastResult = std::move(result);
      return 1;
    }
    catch (const std::exception&)
    {
      return 0;
    }
  }

  int runTapes(const char* const* tape_dirs, uint32_t n_dirs,
               LrvxBacktestStats* out)
  {
    if (!runner || !tape_dirs || n_dirs == 0)
    {
      return 0;
    }
    try
    {
      std::vector<std::filesystem::path> paths;
      paths.reserve(n_dirs);
      for (uint32_t i = 0; i < n_dirs; ++i)
      {
        paths.emplace_back(tape_dirs[i] ? tape_dirs[i] : "");
      }
      BacktestResult result = runner->runTapes(paths);
      if (out)
      {
        fillStats(result.computeStats(), out);
      }
      lastResult = std::move(result);
      return 1;
    }
    catch (const std::exception&)
    {
      return 0;
    }
  }

  int runCsv(const char* path, const char* symbol, LrvxBacktestStats* out)
  {
    uint32_t id = resolveSymbol(symbol);
    std::ifstream f(path);
    if (!f.is_open())
    {
      return 0;
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
    return runBars(std::move(bars), out);
  }

  int runOhlcv(const int64_t* ts, const double* close, uint32_t n,
               const char* symbol, LrvxBacktestStats* out)
  {
    uint32_t id = resolveSymbol(symbol);
    std::vector<OhlcvBacktestReader::Bar> bars;
    bars.reserve(n);
    for (uint32_t i = 0; i < n; ++i)
    {
      // ts[i] is lrvx_backtest_runner_run_ohlcv's documented
      // timestamps_ns -- already nanoseconds by contract, not run through
      // normalizeTs()'s unit guesser (see its comment).
      bars.push_back({ts[i], Price::fromDouble(close[i]).raw(), id});
    }
    return runBars(std::move(bars), out);
  }

  int runFullBars(const int64_t* start_ns, const int64_t* end_ns,
                  const double* open, const double* high, const double* low,
                  const double* close, const double* volume, uint32_t n,
                  const char* symbol, uint8_t bar_type, uint64_t bar_type_param,
                  LrvxBacktestStats* out)
  {
    uint32_t id = resolveSymbol(symbol);
    std::vector<BarEvent> events;
    events.reserve(n);
    for (uint32_t i = 0; i < n; ++i)
    {
      BarEvent ev{};
      ev.symbol = id;
      ev.barType = static_cast<BarType>(bar_type);
      ev.barTypeParam = bar_type_param;
      ev.bar.open = Price::fromDouble(open[i]);
      ev.bar.high = Price::fromDouble(high[i]);
      ev.bar.low = Price::fromDouble(low[i]);
      ev.bar.close = Price::fromDouble(close[i]);
      ev.bar.volume = Volume::fromDouble(volume ? volume[i] : 0.0);
      // start_ns[i] / end_ns[i] are lrvx_backtest_runner_run_bars's
      // documented start_time_ns / end_time_ns -- already nanoseconds by
      // contract, not run through normalizeTs()'s unit guesser (see its
      // comment).
      ev.bar.startTime = TimePoint{std::chrono::nanoseconds{start_ns[i]}};
      ev.bar.endTime = TimePoint{std::chrono::nanoseconds{end_ns[i]}};
      ev.bar.reason = BarCloseReason::Threshold;
      events.push_back(ev);
    }
    BacktestResult result = runner->runBars(events);
    if (out)
    {
      fillStats(result.computeStats(), out);
    }
    lastResult = std::move(result);
    return 1;
  }

  int runReplaySource(LrvxReplaySourceImpl* source, LrvxBacktestStats* out)
  {
    if (source == nullptr)
    {
      return 0;
    }
    if (source->cb.on_start != nullptr)
    {
      source->cb.on_start(source->cb.user_data);
    }
    CapiReplaySourceReader reader(source);
    BacktestResult result = runner->run(reader);
    if (source->cb.on_stop != nullptr)
    {
      source->cb.on_stop(source->cb.user_data);
    }
    if (out)
    {
      fillStats(result.computeStats(), out);
    }
    lastResult = std::move(result);
    return 1;
  }
};

static LrvxBacktestRunnerImpl* toBacktestRunner(LrvxBacktestRunnerHandle h)
{
  return static_cast<LrvxBacktestRunnerImpl*>(h);
}

}  // namespace capi_impl

using namespace capi_impl;

// ============================================================
// LrvxRiskManager C API
// ============================================================

LrvxRiskManagerHandle lrvx_risk_manager_create(LrvxRiskManagerCallbacks callbacks)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  return static_cast<LrvxRiskManagerHandle>(
      TrackerRegistry<LrvxRiskManagerImpl>::create(LrvxRiskManagerImpl{callbacks}));
  LRVX_CAPI_LEAVE;
}

LrvxRiskManagerHandle lrvx_risk_manager_create_p(const LrvxRiskManagerCallbacks* callbacks)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  LrvxRiskManagerCallbacks cbs = callbacks ? *callbacks : LrvxRiskManagerCallbacks{};
  return lrvx_risk_manager_create(cbs);
  LRVX_CAPI_LEAVE;
}

void lrvx_risk_manager_destroy(LrvxRiskManagerHandle rm)
{
  LRVX_CAPI_ENTER_DESTROY(rm);
  TrackerRegistry<LrvxRiskManagerImpl>::destroy(static_cast<LrvxRiskManagerImpl*>(rm));
  LRVX_CAPI_LEAVE_VOID;
}

LrvxKillSwitchHandle lrvx_kill_switch_create(LrvxKillSwitchCallbacks callbacks)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  return static_cast<LrvxKillSwitchHandle>(
      TrackerRegistry<LrvxKillSwitchImpl>::create(LrvxKillSwitchImpl{callbacks}));
  LRVX_CAPI_LEAVE;
}

LrvxKillSwitchHandle lrvx_kill_switch_create_p(const LrvxKillSwitchCallbacks* callbacks)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  LrvxKillSwitchCallbacks cbs = callbacks ? *callbacks : LrvxKillSwitchCallbacks{};
  return lrvx_kill_switch_create(cbs);
  LRVX_CAPI_LEAVE;
}

void lrvx_kill_switch_destroy(LrvxKillSwitchHandle ks)
{
  LRVX_CAPI_ENTER_DESTROY(ks);
  TrackerRegistry<LrvxKillSwitchImpl>::destroy(static_cast<LrvxKillSwitchImpl*>(ks));
  LRVX_CAPI_LEAVE_VOID;
}

LrvxOrderValidatorHandle lrvx_order_validator_create(LrvxOrderValidatorCallbacks callbacks)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  return static_cast<LrvxOrderValidatorHandle>(
      TrackerRegistry<LrvxOrderValidatorImpl>::create(LrvxOrderValidatorImpl{callbacks}));
  LRVX_CAPI_LEAVE;
}

LrvxOrderValidatorHandle lrvx_order_validator_create_p(const LrvxOrderValidatorCallbacks* callbacks)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  LrvxOrderValidatorCallbacks cbs = callbacks ? *callbacks : LrvxOrderValidatorCallbacks{};
  return lrvx_order_validator_create(cbs);
  LRVX_CAPI_LEAVE;
}

void lrvx_order_validator_destroy(LrvxOrderValidatorHandle ov)
{
  LRVX_CAPI_ENTER_DESTROY(ov);
  TrackerRegistry<LrvxOrderValidatorImpl>::destroy(static_cast<LrvxOrderValidatorImpl*>(ov));
  LRVX_CAPI_LEAVE_VOID;
}

LrvxPnLTrackerHandle lrvx_pnl_tracker_create(LrvxPnLTrackerCallbacks callbacks)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  return static_cast<LrvxPnLTrackerHandle>(
      TrackerRegistry<LrvxPnLTrackerImpl>::create(LrvxPnLTrackerImpl{callbacks}));
  LRVX_CAPI_LEAVE;
}

LrvxPnLTrackerHandle lrvx_pnl_tracker_create_p(const LrvxPnLTrackerCallbacks* callbacks)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  LrvxPnLTrackerCallbacks cbs = callbacks ? *callbacks : LrvxPnLTrackerCallbacks{};
  return lrvx_pnl_tracker_create(cbs);
  LRVX_CAPI_LEAVE;
}

void lrvx_pnl_tracker_destroy(LrvxPnLTrackerHandle tracker)
{
  LRVX_CAPI_ENTER_DESTROY(tracker);
  TrackerRegistry<LrvxPnLTrackerImpl>::destroy(static_cast<LrvxPnLTrackerImpl*>(tracker));
  LRVX_CAPI_LEAVE_VOID;
}

LrvxStorageSinkHandle lrvx_storage_sink_create(LrvxStorageSinkCallbacks callbacks)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  return static_cast<LrvxStorageSinkHandle>(
      TrackerRegistry<LrvxStorageSinkImpl>::create(LrvxStorageSinkImpl{callbacks}));
  LRVX_CAPI_LEAVE;
}

LrvxStorageSinkHandle lrvx_storage_sink_create_p(const LrvxStorageSinkCallbacks* callbacks)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  LrvxStorageSinkCallbacks cbs = callbacks ? *callbacks : LrvxStorageSinkCallbacks{};
  return lrvx_storage_sink_create(cbs);
  LRVX_CAPI_LEAVE;
}

void lrvx_storage_sink_destroy(LrvxStorageSinkHandle sink)
{
  LRVX_CAPI_ENTER_DESTROY(sink);
  TrackerRegistry<LrvxStorageSinkImpl>::destroy(static_cast<LrvxStorageSinkImpl*>(sink));
  LRVX_CAPI_LEAVE_VOID;
}

LrvxMarketDataRecorderHandle lrvx_market_data_recorder_create(
    LrvxMarketDataRecorderCallbacks callbacks)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  return static_cast<LrvxMarketDataRecorderHandle>(
      new LrvxMarketDataRecorderImpl{callbacks});
  LRVX_CAPI_LEAVE;
}

LrvxMarketDataRecorderHandle lrvx_market_data_recorder_create_p(
    const LrvxMarketDataRecorderCallbacks* callbacks)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  LrvxMarketDataRecorderCallbacks cbs = callbacks ? *callbacks : LrvxMarketDataRecorderCallbacks{};
  return lrvx_market_data_recorder_create(cbs);
  LRVX_CAPI_LEAVE;
}

void lrvx_market_data_recorder_destroy(LrvxMarketDataRecorderHandle recorder)
{
  LRVX_CAPI_ENTER_DESTROY(recorder);
  delete static_cast<LrvxMarketDataRecorderImpl*>(recorder);
  LRVX_CAPI_LEAVE_VOID;
}

// ── Binary-log recorder hook ──────────────────────────────────────────
namespace capi_impl
{
struct LrvxBinaryLogRecorderHookImpl
{
  lrvx::replay::BinaryLogRecorderHook hook;
  // LrvxMarketDataRecorderImpl whose callbacks bridge engine events
  // straight into hook.onTrade / onBookUpdate. user_data points at
  // `this`. Lifetime is tied to the owning impl — DO NOT separately
  // free via lrvx_market_data_recorder_destroy.
  LrvxMarketDataRecorderImpl recorder_view{};

  explicit LrvxBinaryLogRecorderHookImpl(lrvx::replay::BinaryLogRecorderHookConfig cfg)
      : hook(std::move(cfg))
  {
  }
};

static void blrhOnTrade(void* ud, const LrvxTradeData* t)
{
  if (!t)
  {
    return;
  }
  auto* self = static_cast<LrvxBinaryLogRecorderHookImpl*>(ud);
  self->hook.onTrade(t->symbol, t->price_raw, t->quantity_raw,
                     t->is_buy != 0, t->exchange_ts_ns, /*recv_ts_ns=*/0);
}

static void blrhOnBookUpdate(void* ud, uint32_t symbol, uint8_t is_snapshot,
                             const LrvxBookLevel* bids, uint32_t n_bids,
                             const LrvxBookLevel* asks, uint32_t n_asks,
                             int64_t exchange_ts_ns)
{
  auto* self = static_cast<LrvxBinaryLogRecorderHookImpl*>(ud);
  auto* bid_levels = reinterpret_cast<const lrvx::replay::BookLevel*>(bids);
  auto* ask_levels = reinterpret_cast<const lrvx::replay::BookLevel*>(asks);
  self->hook.onBookUpdate(symbol, is_snapshot != 0, bid_levels, n_bids,
                          ask_levels, n_asks, exchange_ts_ns, /*recv_ts_ns=*/0);
}

static void blrhOnStart(void* ud)
{
  static_cast<LrvxBinaryLogRecorderHookImpl*>(ud)->hook.start();
}

static void blrhOnStop(void* ud)
{
  static_cast<LrvxBinaryLogRecorderHookImpl*>(ud)->hook.stop();
}
}  // namespace capi_impl

LrvxBinaryLogRecorderHookHandle
lrvx_binary_log_recorder_hook_create(const char* output_dir,
                                     uint64_t max_segment_mb,
                                     uint8_t exchange_id,
                                     uint8_t compression)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  return lrvx_binary_log_recorder_hook_create_ex(output_dir, max_segment_mb,
                                                 exchange_id, compression,
                                                 nullptr, nullptr);
  LRVX_CAPI_LEAVE;
}

LrvxBinaryLogRecorderHookHandle
lrvx_binary_log_recorder_hook_create_ex(const char* output_dir,
                                        uint64_t max_segment_mb,
                                        uint8_t exchange_id,
                                        uint8_t compression,
                                        const char* exchange_name,
                                        const char* instrument_type)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  lrvx::replay::BinaryLogRecorderHookConfig cfg{};
  cfg.output_dir = output_dir ? output_dir : "";
  cfg.max_segment_bytes = max_segment_mb * 1024ull * 1024ull;
  cfg.exchange_id = exchange_id;
  cfg.compression = static_cast<lrvx::replay::CompressionType>(compression);

  const bool has_exchange = exchange_name && *exchange_name;
  const bool has_instrument = instrument_type && *instrument_type;
  if (has_exchange || has_instrument)
  {
    lrvx::replay::RecordingMetadata meta{};
    if (has_exchange)
    {
      meta.exchange = exchange_name;
    }
    if (has_instrument)
    {
      meta.instrument_type = instrument_type;
    }
    cfg.metadata = std::move(meta);
  }

  auto* impl = new capi_impl::LrvxBinaryLogRecorderHookImpl(std::move(cfg));
  impl->recorder_view.cb.on_trade = &capi_impl::blrhOnTrade;
  impl->recorder_view.cb.on_book_update = &capi_impl::blrhOnBookUpdate;
  impl->recorder_view.cb.on_start = &capi_impl::blrhOnStart;
  impl->recorder_view.cb.on_stop = &capi_impl::blrhOnStop;
  impl->recorder_view.cb.user_data = impl;
  // Borrowed: the recorder handle callers get from
  // lrvx_binary_log_recorder_hook_as_recorder points at this member, so
  // lrvx_market_data_recorder_destroy on it would free part of the hook.
  // The header has always said not to; registering it means a caller that
  // does anyway gets a no-op instead of a corrupted heap.
  LrvxBorrowedHandles::add(&impl->recorder_view);
  return static_cast<LrvxBinaryLogRecorderHookHandle>(impl);
  LRVX_CAPI_LEAVE;
}

void lrvx_binary_log_recorder_hook_destroy(LrvxBinaryLogRecorderHookHandle h)
{
  LRVX_CAPI_ENTER_DESTROY(h);
  auto* impl = static_cast<capi_impl::LrvxBinaryLogRecorderHookImpl*>(h);
  LrvxBorrowedHandles::remove(&impl->recorder_view);
  delete impl;
  LRVX_CAPI_LEAVE_VOID;
}

LrvxMarketDataRecorderHandle
lrvx_binary_log_recorder_hook_as_recorder(LrvxBinaryLogRecorderHookHandle h)
{
  LRVX_CAPI_ENTER(h);
  if (!h)
  {
    return nullptr;
  }
  return static_cast<LrvxMarketDataRecorderHandle>(
      &static_cast<capi_impl::LrvxBinaryLogRecorderHookImpl*>(h)->recorder_view);
  LRVX_CAPI_LEAVE;
}

void lrvx_binary_log_recorder_hook_add_symbol(LrvxBinaryLogRecorderHookHandle h,
                                              uint32_t symbol_id,
                                              const char* name,
                                              const char* base,
                                              const char* quote,
                                              int8_t price_precision,
                                              int8_t qty_precision)
{
  LRVX_CAPI_ENTER_VOID(h);
  if (!h)
  {
    return;
  }
  lrvx::replay::SymbolInfo info;
  info.symbol_id = symbol_id;
  info.name = name ? name : "";
  info.base_asset = base ? base : "";
  info.quote_asset = quote ? quote : "";
  info.price_precision = price_precision;
  info.qty_precision = qty_precision;
  static_cast<capi_impl::LrvxBinaryLogRecorderHookImpl*>(h)->hook.addSymbol(info);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_binary_log_recorder_hook_flush(LrvxBinaryLogRecorderHookHandle h)
{
  LRVX_CAPI_ENTER_VOID(h);
  if (h)
  {
    static_cast<capi_impl::LrvxBinaryLogRecorderHookImpl*>(h)->hook.flush();
  }
  LRVX_CAPI_LEAVE_VOID;
}

LrvxWriterStats lrvx_binary_log_recorder_hook_stats(LrvxBinaryLogRecorderHookHandle h)
{
  LRVX_CAPI_ENTER(h);
  if (!h)
  {
    return {};
  }
  auto s = static_cast<capi_impl::LrvxBinaryLogRecorderHookImpl*>(h)->hook.stats();
  return {s.bytes_written, s.trades_written + s.book_updates_written,
          s.segments_created, s.trades_written};
  LRVX_CAPI_LEAVE;
}

void lrvx_binary_log_recorder_hook_stats_p(void* h, void* out)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  auto s = lrvx_binary_log_recorder_hook_stats(static_cast<LrvxBinaryLogRecorderHookHandle>(h));
  memcpy(out, &s, sizeof(s));
  LRVX_CAPI_LEAVE_VOID;
}

LrvxReplaySourceHandle lrvx_replay_source_create(LrvxReplaySourceCallbacks callbacks)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  return static_cast<LrvxReplaySourceHandle>(
      new LrvxReplaySourceImpl{callbacks});
  LRVX_CAPI_LEAVE;
}

LrvxReplaySourceHandle lrvx_replay_source_create_p(const LrvxReplaySourceCallbacks* callbacks)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  LrvxReplaySourceCallbacks cbs = callbacks ? *callbacks : LrvxReplaySourceCallbacks{};
  return lrvx_replay_source_create(cbs);
  LRVX_CAPI_LEAVE;
}

void lrvx_replay_source_destroy(LrvxReplaySourceHandle source)
{
  LRVX_CAPI_ENTER_DESTROY(source);
  delete static_cast<LrvxReplaySourceImpl*>(source);
  LRVX_CAPI_LEAVE_VOID;
}

uint8_t lrvx_replay_source_seek_to(LrvxReplaySourceHandle source, int64_t timestamp_ns)
{
  LRVX_CAPI_ENTER(source);
  auto* s = static_cast<LrvxReplaySourceImpl*>(source);
  if (s == nullptr || s->cb.seek_to == nullptr)
  {
    return 0;
  }
  return s->cb.seek_to(s->cb.user_data, timestamp_ns);
  LRVX_CAPI_LEAVE;
}

LrvxExecutionListenerHandle
lrvx_execution_listener_create(LrvxExecutionListenerCallbacks callbacks)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  return static_cast<LrvxExecutionListenerHandle>(
      new LrvxExecutionListenerImpl{callbacks});
  LRVX_CAPI_LEAVE;
}

LrvxExecutionListenerHandle
lrvx_execution_listener_create_p(const LrvxExecutionListenerCallbacks* callbacks)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  LrvxExecutionListenerCallbacks cbs = callbacks ? *callbacks : LrvxExecutionListenerCallbacks{};
  return lrvx_execution_listener_create(cbs);
  LRVX_CAPI_LEAVE;
}

void lrvx_execution_listener_destroy(LrvxExecutionListenerHandle listener)
{
  LRVX_CAPI_ENTER_DESTROY(listener);
  delete static_cast<LrvxExecutionListenerImpl*>(listener);
  LRVX_CAPI_LEAVE_VOID;
}

LrvxExecutorHandle lrvx_executor_create(LrvxExecutorCallbacks callbacks)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  return static_cast<LrvxExecutorHandle>(
      TrackerRegistry<LrvxExecutorImpl>::create(LrvxExecutorImpl{callbacks}));
  LRVX_CAPI_LEAVE;
}

LrvxExecutorHandle lrvx_executor_create_p(const LrvxExecutorCallbacks* callbacks)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  LrvxExecutorCallbacks cbs = callbacks ? *callbacks : LrvxExecutorCallbacks{};
  return lrvx_executor_create(cbs);
  LRVX_CAPI_LEAVE;
}

void lrvx_executor_destroy(LrvxExecutorHandle executor)
{
  LRVX_CAPI_ENTER_DESTROY(executor);
  TrackerRegistry<LrvxExecutorImpl>::destroy(static_cast<LrvxExecutorImpl*>(executor));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_executor_get_capabilities(LrvxExecutorHandle executor,
                                    LrvxExchangeCapabilities* caps_out)
{
  LRVX_CAPI_ENTER_VOID(executor);
  if (caps_out == nullptr)
  {
    return;
  }
  *caps_out = LrvxExchangeCapabilities{};
  auto* impl = static_cast<LrvxExecutorImpl*>(executor);
  if (impl == nullptr || impl->cb.capabilities == nullptr)
  {
    return;
  }
  impl->cb.capabilities(impl->cb.user_data, caps_out);
  LRVX_CAPI_LEAVE_VOID;
}

// ============================================================
// Logger callback adapter
// ============================================================

namespace
{

class CapiCallbackLogger : public lrvx::ILogger
{
 public:
  CapiCallbackLogger(LrvxLogCallback cb, void* ud) : _cb(cb), _ud(ud) {}

  void info(std::string_view msg) override { dispatch(0, msg); }
  void warn(std::string_view msg) override { dispatch(1, msg); }
  void error(std::string_view msg) override { dispatch(2, msg); }

 private:
  void dispatch(int32_t level, std::string_view msg)
  {
    // string_view is not guaranteed null-terminated; the C callback expects
    // const char*. Construct a std::string for the call duration.
    std::string s(msg);
    _cb(_ud, level, s.c_str());
  }

  LrvxLogCallback _cb;
  void* _ud;
};

// Owned by the C API; outlives the registered call. Reset via
// lrvx_set_log_callback(NULL, ...).
std::unique_ptr<CapiCallbackLogger> g_capiLogger;

}  // namespace

void lrvx_set_log_callback(LrvxLogCallback callback, void* user_data)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  if (callback == nullptr)
  {
    lrvx::setGlobalLogger(nullptr);
    g_capiLogger.reset();
    return;
  }
  auto next = std::make_unique<CapiCallbackLogger>(callback, user_data);
  lrvx::setGlobalLogger(next.get());
  // Replace AFTER swapping the pointer so concurrent log calls never see
  // a dangling adapter. If two lrvx_set_log_callback calls race, the loser
  // simply destroys its adapter without ever being installed.
  g_capiLogger = std::move(next);
  LRVX_CAPI_LEAVE_VOID;
}

LrvxRunnerHandle lrvx_runner_create(LrvxRegistryHandle registry,
                                    LrvxOnSignalCallback on_signal,
                                    void* user_data)
{
  LRVX_CAPI_ENTER(registry);
  return static_cast<LrvxRunnerHandle>(
      new LrvxRunnerImpl(toRegistry(registry), on_signal, user_data));
  LRVX_CAPI_LEAVE;
}

void lrvx_runner_destroy(LrvxRunnerHandle runner)
{
  LRVX_CAPI_ENTER_DESTROY(runner);
  delete toRunner(runner);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_runner_add_strategy(LrvxRunnerHandle runner, LrvxStrategyHandle strategy)
{
  LRVX_CAPI_ENTER_VOID(runner);
  toRunner(runner)->addStrategy(toStrategy(strategy));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_runner_set_risk_manager(LrvxRunnerHandle runner, LrvxRiskManagerHandle rm)
{
  LRVX_CAPI_ENTER_VOID(runner);
  toRunner(runner)->setRiskManager(static_cast<capi_impl::LrvxRiskManagerImpl*>(rm));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_runner_set_kill_switch(LrvxRunnerHandle runner, LrvxKillSwitchHandle ks)
{
  LRVX_CAPI_ENTER_VOID(runner);
  toRunner(runner)->setKillSwitch(static_cast<capi_impl::LrvxKillSwitchImpl*>(ks));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_runner_set_order_validator(LrvxRunnerHandle runner, LrvxOrderValidatorHandle ov)
{
  LRVX_CAPI_ENTER_VOID(runner);
  toRunner(runner)->setOrderValidator(
      static_cast<capi_impl::LrvxOrderValidatorImpl*>(ov));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_runner_set_pnl_tracker(LrvxRunnerHandle runner, LrvxPnLTrackerHandle tracker)
{
  LRVX_CAPI_ENTER_VOID(runner);
  toRunner(runner)->setPnLTracker(
      static_cast<capi_impl::LrvxPnLTrackerImpl*>(tracker));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_runner_set_storage_sink(LrvxRunnerHandle runner, LrvxStorageSinkHandle sink)
{
  LRVX_CAPI_ENTER_VOID(runner);
  toRunner(runner)->setStorageSink(
      static_cast<capi_impl::LrvxStorageSinkImpl*>(sink));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_runner_set_executor(LrvxRunnerHandle runner, LrvxExecutorHandle executor)
{
  LRVX_CAPI_ENTER_VOID(runner);
  toRunner(runner)->setExecutor(
      static_cast<capi_impl::LrvxExecutorImpl*>(executor));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_runner_set_market_data_recorder(LrvxRunnerHandle runner,
                                          LrvxMarketDataRecorderHandle recorder)
{
  LRVX_CAPI_ENTER_VOID(runner);
  toRunner(runner)->setMarketDataRecorder(
      static_cast<capi_impl::LrvxMarketDataRecorderImpl*>(recorder));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_runner_attach_trace_recorder(LrvxRunnerHandle runner, LrvxRunRecorderHandle recorder)
{
  LRVX_CAPI_ENTER_VOID(runner);
  // recorder is a `lrvx::run::TraceRecorder*` (from
  // `lrvx_run_recorder_create`) or NULL to detach.
  toRunner(runner)->attachTraceRecorder(static_cast<void*>(recorder));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_runner_set_trace_feed_ts_ns(LrvxRunnerHandle runner, int64_t feed_ts_ns)
{
  LRVX_CAPI_ENTER_VOID(runner);
  toRunner(runner)->setTraceFeedTsNs(feed_ts_ns);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_runner_trace_order_event(LrvxRunnerHandle runner, uint64_t order_id,
                                   uint64_t parent_signal_id, uint32_t symbol_id,
                                   uint8_t event_kind, uint8_t side, uint8_t order_type,
                                   int64_t price_raw, int64_t qty_raw, uint32_t flags)
{
  LRVX_CAPI_ENTER_VOID(runner);
  auto* rec = toRunner(runner)->traceRecorder();
  if (!rec)
  {
    return;
  }
  lrvx::run::OrderEventView e;
  e.run_ts_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();
  e.feed_ts_ns = toRunner(runner)->traceFeedTsNs();
  e.order_id = order_id;
  e.parent_signal_id = parent_signal_id;
  e.symbol_id = symbol_id;
  e.event_kind = static_cast<lrvx::run::OrderEventKind>(event_kind);
  e.side = side;
  e.order_type = order_type;
  e.price_raw = price_raw;
  e.qty_raw = qty_raw;
  e.flags = flags;
  rec->writeOrderEvent(e);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_runner_trace_fill(LrvxRunnerHandle runner, uint64_t order_id, uint64_t fill_id,
                            int64_t price_raw, int64_t qty_raw, int64_t fee_raw,
                            uint32_t symbol_id, uint8_t side, uint8_t liquidity)
{
  LRVX_CAPI_ENTER_VOID(runner);
  auto* rec = toRunner(runner)->traceRecorder();
  if (!rec)
  {
    return;
  }
  lrvx::run::FillView f;
  f.run_ts_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();
  f.feed_ts_ns = toRunner(runner)->traceFeedTsNs();
  f.order_id = order_id;
  f.fill_id = fill_id;
  f.price_raw = price_raw;
  f.qty_raw = qty_raw;
  f.fee_raw = fee_raw;
  f.symbol_id = symbol_id;
  f.side = side;
  f.liquidity = static_cast<lrvx::run::FillLiquidity>(liquidity);
  rec->writeFill(f);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_runner_start(LrvxRunnerHandle runner)
{
  LRVX_CAPI_ENTER_VOID(runner);
  toRunner(runner)->start();
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_runner_stop(LrvxRunnerHandle runner)
{
  LRVX_CAPI_ENTER_VOID(runner);
  toRunner(runner)->stop();
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_runner_on_trade(LrvxRunnerHandle runner, uint32_t symbol,
                          double price, double qty, uint8_t is_buy,
                          int64_t exchange_ts_ns)
{
  LRVX_CAPI_ENTER_VOID(runner);
  toRunner(runner)->onTrade(symbol, price, qty, is_buy != 0, exchange_ts_ns);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_runner_on_book_snapshot(LrvxRunnerHandle runner, uint32_t symbol,
                                  const double* bid_prices, const double* bid_qtys,
                                  uint32_t n_bids,
                                  const double* ask_prices, const double* ask_qtys,
                                  uint32_t n_asks,
                                  int64_t exchange_ts_ns)
{
  LRVX_CAPI_ENTER_VOID(runner);
  toRunner(runner)->onBookSnapshot(symbol,
                                   bid_prices, bid_qtys, n_bids,
                                   ask_prices, ask_qtys, n_asks,
                                   exchange_ts_ns);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_runner_on_bar(LrvxRunnerHandle runner, uint32_t symbol,
                        uint8_t bar_type, uint64_t bar_type_param,
                        double open, double high, double low, double close,
                        double volume, double buy_volume,
                        int64_t start_time_ns, int64_t end_time_ns,
                        uint8_t close_reason)
{
  LRVX_CAPI_ENTER_VOID(runner);
  toRunner(runner)->onBar(symbol, bar_type, bar_type_param,
                          open, high, low, close, volume, buy_volume,
                          start_time_ns, end_time_ns, close_reason);
  LRVX_CAPI_LEAVE_VOID;
}

// ============================================================
// LrvxLiveEngine C API
// ============================================================

LrvxLiveEngineHandle lrvx_live_engine_create(LrvxRegistryHandle registry)
{
  LRVX_CAPI_ENTER(registry);
  return static_cast<LrvxLiveEngineHandle>(new LrvxLiveEngineImpl(toRegistry(registry)));
  LRVX_CAPI_LEAVE;
}

void lrvx_live_engine_destroy(LrvxLiveEngineHandle engine)
{
  LRVX_CAPI_ENTER_DESTROY(engine);
  delete toLiveEngine(engine);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_live_engine_add_strategy(LrvxLiveEngineHandle engine,
                                   LrvxStrategyHandle strategy,
                                   LrvxOnSignalCallback on_signal,
                                   void* user_data)
{
  LRVX_CAPI_ENTER_VOID(engine);
  toLiveEngine(engine)->addStrategy(toStrategy(strategy), on_signal, user_data);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_live_engine_set_risk_manager(LrvxLiveEngineHandle engine, LrvxRiskManagerHandle rm)
{
  LRVX_CAPI_ENTER_VOID(engine);
  toLiveEngine(engine)->setRiskManager(
      static_cast<capi_impl::LrvxRiskManagerImpl*>(rm));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_live_engine_set_kill_switch(LrvxLiveEngineHandle engine, LrvxKillSwitchHandle ks)
{
  LRVX_CAPI_ENTER_VOID(engine);
  toLiveEngine(engine)->setKillSwitch(
      static_cast<capi_impl::LrvxKillSwitchImpl*>(ks));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_live_engine_set_order_validator(LrvxLiveEngineHandle engine,
                                          LrvxOrderValidatorHandle ov)
{
  LRVX_CAPI_ENTER_VOID(engine);
  toLiveEngine(engine)->setOrderValidator(
      static_cast<capi_impl::LrvxOrderValidatorImpl*>(ov));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_live_engine_set_pnl_tracker(LrvxLiveEngineHandle engine,
                                      LrvxPnLTrackerHandle tracker)
{
  LRVX_CAPI_ENTER_VOID(engine);
  toLiveEngine(engine)->setPnLTracker(
      static_cast<capi_impl::LrvxPnLTrackerImpl*>(tracker));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_live_engine_set_storage_sink(LrvxLiveEngineHandle engine,
                                       LrvxStorageSinkHandle sink)
{
  LRVX_CAPI_ENTER_VOID(engine);
  toLiveEngine(engine)->setStorageSink(
      static_cast<capi_impl::LrvxStorageSinkImpl*>(sink));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_live_engine_set_market_data_recorder(LrvxLiveEngineHandle engine,
                                               LrvxMarketDataRecorderHandle recorder)
{
  LRVX_CAPI_ENTER_VOID(engine);
  toLiveEngine(engine)->setMarketDataRecorder(
      static_cast<capi_impl::LrvxMarketDataRecorderImpl*>(recorder));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_live_engine_set_executor(LrvxLiveEngineHandle engine,
                                   LrvxExecutorHandle executor)
{
  LRVX_CAPI_ENTER_VOID(engine);
  toLiveEngine(engine)->setExecutor(
      static_cast<capi_impl::LrvxExecutorImpl*>(executor));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_live_engine_start(LrvxLiveEngineHandle engine)
{
  LRVX_CAPI_ENTER_VOID(engine);
  toLiveEngine(engine)->start();
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_live_engine_stop(LrvxLiveEngineHandle engine)
{
  LRVX_CAPI_ENTER_VOID(engine);
  toLiveEngine(engine)->stop();
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_live_engine_publish_trade(LrvxLiveEngineHandle engine,
                                    uint32_t symbol,
                                    double price, double qty, uint8_t is_buy,
                                    int64_t exchange_ts_ns)
{
  LRVX_CAPI_ENTER_VOID(engine);
  toLiveEngine(engine)->publishTrade(symbol, price, qty, is_buy != 0, exchange_ts_ns);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_live_engine_publish_book_snapshot(LrvxLiveEngineHandle engine,
                                            uint32_t symbol,
                                            const double* bid_prices,
                                            const double* bid_qtys,
                                            uint32_t n_bids,
                                            const double* ask_prices,
                                            const double* ask_qtys,
                                            uint32_t n_asks,
                                            int64_t exchange_ts_ns)
{
  LRVX_CAPI_ENTER_VOID(engine);
  toLiveEngine(engine)->publishBookSnapshot(symbol,
                                            bid_prices, bid_qtys, n_bids,
                                            ask_prices, ask_qtys, n_asks,
                                            exchange_ts_ns);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_live_engine_publish_bar(LrvxLiveEngineHandle engine,
                                  uint32_t symbol,
                                  uint8_t bar_type, uint64_t bar_type_param,
                                  double open, double high, double low, double close,
                                  double volume, double buy_volume,
                                  int64_t start_time_ns, int64_t end_time_ns,
                                  uint8_t close_reason)
{
  LRVX_CAPI_ENTER_VOID(engine);
  toLiveEngine(engine)->publishBar(symbol, bar_type, bar_type_param,
                                   open, high, low, close, volume, buy_volume,
                                   start_time_ns, end_time_ns, close_reason);
  LRVX_CAPI_LEAVE_VOID;
}

// ============================================================
// LrvxBacktestRunner C API
// ============================================================

LrvxBacktestRunnerHandle lrvx_backtest_runner_create(LrvxRegistryHandle registry,
                                                     double fee_rate,
                                                     double initial_capital)
{
  LRVX_CAPI_ENTER(registry);
  return static_cast<LrvxBacktestRunnerHandle>(
      new LrvxBacktestRunnerImpl(toRegistry(registry), fee_rate, initial_capital));
  LRVX_CAPI_LEAVE;
}

void lrvx_backtest_runner_destroy(LrvxBacktestRunnerHandle h)
{
  LRVX_CAPI_ENTER_DESTROY(h);
  delete toBacktestRunner(h);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_backtest_runner_set_strategy(LrvxBacktestRunnerHandle h,
                                       LrvxStrategyHandle strategy)
{
  LRVX_CAPI_ENTER_VOID(h);
  toBacktestRunner(h)->setStrategy(toStrategy(strategy));
  LRVX_CAPI_LEAVE_VOID;
}

int lrvx_backtest_runner_run_csv(LrvxBacktestRunnerHandle h,
                                 const char* path,
                                 const char* symbol,
                                 LrvxBacktestStats* out)
{
  LRVX_CAPI_ENTER(h);
  return toBacktestRunner(h)->runCsv(path, symbol, out);
  LRVX_CAPI_LEAVE;
}

int lrvx_backtest_runner_run_tape(LrvxBacktestRunnerHandle h,
                                  const char* tape_dir,
                                  LrvxBacktestStats* out)
{
  LRVX_CAPI_ENTER(h);
  return toBacktestRunner(h)->runTape(tape_dir, out);
  LRVX_CAPI_LEAVE;
}

int lrvx_backtest_runner_run_tapes(LrvxBacktestRunnerHandle h,
                                   const char* const* tape_dirs,
                                   uint32_t n_dirs,
                                   LrvxBacktestStats* out)
{
  LRVX_CAPI_ENTER(h);
  return toBacktestRunner(h)->runTapes(tape_dirs, n_dirs, out);
  LRVX_CAPI_LEAVE;
}

int lrvx_backtest_runner_run_ohlcv(LrvxBacktestRunnerHandle h,
                                   const int64_t* ts,
                                   const double* close,
                                   uint32_t n,
                                   const char* symbol,
                                   LrvxBacktestStats* out)
{
  LRVX_CAPI_ENTER(h);
  return toBacktestRunner(h)->runOhlcv(ts, close, n, symbol, out);
  LRVX_CAPI_LEAVE;
}

int lrvx_backtest_runner_run_bars(LrvxBacktestRunnerHandle h,
                                  const int64_t* start_time_ns,
                                  const int64_t* end_time_ns,
                                  const double* open,
                                  const double* high,
                                  const double* low,
                                  const double* close,
                                  const double* volume,
                                  uint32_t n,
                                  const char* symbol,
                                  uint8_t bar_type,
                                  uint64_t bar_type_param,
                                  LrvxBacktestStats* out)
{
  LRVX_CAPI_ENTER(h);
  return toBacktestRunner(h)->runFullBars(start_time_ns, end_time_ns,
                                          open, high, low, close, volume,
                                          n, symbol, bar_type, bar_type_param, out);
  LRVX_CAPI_LEAVE;
}

int lrvx_backtest_runner_run_replay_source(LrvxBacktestRunnerHandle h,
                                           LrvxReplaySourceHandle source,
                                           LrvxBacktestStats* out)
{
  LRVX_CAPI_ENTER(h);
  return toBacktestRunner(h)->runReplaySource(
      static_cast<capi_impl::LrvxReplaySourceImpl*>(source), out);
  LRVX_CAPI_LEAVE;
}

LrvxBacktestResultHandle lrvx_backtest_runner_take_result(LrvxBacktestRunnerHandle h)
{
  LRVX_CAPI_ENTER(h);
  auto* impl = toBacktestRunner(h);
  if (!impl->lastResult.has_value())
  {
    return nullptr;
  }
  auto* out = new LrvxBacktestResultImpl();
  out->config = impl->lastResult->config();
  out->result = std::make_unique<BacktestResult>(*impl->lastResult);
  return static_cast<LrvxBacktestResultHandle>(out);
  LRVX_CAPI_LEAVE;
}

void lrvx_backtest_runner_add_execution_listener(LrvxBacktestRunnerHandle h,
                                                 LrvxExecutionListenerHandle listener)
{
  LRVX_CAPI_ENTER_VOID(h);
  toBacktestRunner(h)->addExecutionListener(
      static_cast<capi_impl::LrvxExecutionListenerImpl*>(listener));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_backtest_runner_add_journey_tracer(LrvxBacktestRunnerHandle h,
                                             LrvxOrderJourneyTracerHandle tracer)
{
  LRVX_CAPI_ENTER_VOID(h);
  if (h == nullptr || tracer == nullptr)
  {
    return;
  }
  auto* runner = toBacktestRunner(h)->runner.get();
  runner->addExecutionListener(static_cast<lrvx::OrderJourneyTracer*>(tracer));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_backtest_runner_set_executor(LrvxBacktestRunnerHandle h,
                                       LrvxExecutorHandle executor)
{
  LRVX_CAPI_ENTER_VOID(h);
  toBacktestRunner(h)->setExecutor(
      static_cast<capi_impl::LrvxExecutorImpl*>(executor));
  LRVX_CAPI_LEAVE_VOID;
}

// Pre-trade gate parity with the live runner. The Impl wrappers
// already inherit from the IRiskManager / IKillSwitch / IOrderValidator
// / IPnLTracker interfaces, so the cast handed to the BacktestRunner
// setters is direct.
void lrvx_backtest_runner_set_risk_manager(LrvxBacktestRunnerHandle h,
                                           LrvxRiskManagerHandle rm)
{
  LRVX_CAPI_ENTER_VOID(h);
  toBacktestRunner(h)->setRiskManager(
      static_cast<capi_impl::LrvxRiskManagerImpl*>(rm));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_backtest_runner_set_kill_switch(LrvxBacktestRunnerHandle h,
                                          LrvxKillSwitchHandle ks)
{
  LRVX_CAPI_ENTER_VOID(h);
  toBacktestRunner(h)->setKillSwitch(
      static_cast<capi_impl::LrvxKillSwitchImpl*>(ks));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_backtest_runner_set_order_validator(LrvxBacktestRunnerHandle h,
                                              LrvxOrderValidatorHandle ov)
{
  LRVX_CAPI_ENTER_VOID(h);
  toBacktestRunner(h)->setOrderValidator(
      static_cast<capi_impl::LrvxOrderValidatorImpl*>(ov));
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_backtest_runner_set_pnl_tracker(LrvxBacktestRunnerHandle h,
                                          LrvxPnLTrackerHandle tracker)
{
  LRVX_CAPI_ENTER_VOID(h);
  toBacktestRunner(h)->setPnLTracker(
      static_cast<capi_impl::LrvxPnLTrackerImpl*>(tracker));
  LRVX_CAPI_LEAVE_VOID;
}

// ============================================================
// Walk-forward
// ============================================================

namespace
{

uint32_t resolveSymbolId(SymbolRegistry* reg, const char* symbol)
{
  if (!symbol || symbol[0] == '\0')
  {
    auto all = reg->getAllSymbols();
    return all.empty() ? 0 : all.front().id;
  }
  for (const auto& info : reg->getAllSymbols())
  {
    if (info.symbol == symbol)
    {
      return info.id;
    }
  }
  return 0;
}

int64_t normalizeWfTs(int64_t t)
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

// Callers treat an empty result as "could not load" (see the bars.empty()
// check in lrvx_walk_forward_run_csv), which is the only failure channel this
// signature has. std::stoll/std::stod throw std::invalid_argument on a
// malformed row, and this runs under extern "C", where an escaping exception
// is undefined behaviour rather than a catchable error. Abort the whole load
// instead of skipping bad rows: a partially-parsed tape would silently produce
// a backtest over data the caller never supplied.
std::vector<OhlcvReplaySource::Bar> loadOhlcvBarsCsv(const char* path,
                                                     uint32_t symbolId) noexcept
{
  try
  {
    std::vector<OhlcvReplaySource::Bar> out;
    std::ifstream f(path);
    if (!f.is_open())
    {
      return out;
    }
    std::string line;
    std::getline(f, line);  // header
    while (std::getline(f, line))
    {
      if (line.empty())
      {
        continue;
      }
      std::istringstream ss(line);
      std::string tok;
      std::getline(ss, tok, ',');
      int64_t ts = normalizeWfTs(std::stoll(tok));
      std::getline(ss, tok, ',');  // open
      std::getline(ss, tok, ',');  // high
      std::getline(ss, tok, ',');  // low
      std::getline(ss, tok, ',');
      double c = std::stod(tok);
      OhlcvReplaySource::Bar bar;
      bar.ts_ns = ts;
      bar.price_raw = Price::fromDouble(c).raw();
      bar.symbol_id = symbolId;
      out.push_back(bar);
    }
    return out;
  }
  catch (const std::exception&)
  {
    return {};
  }
}

void fillStatsStruct(const BacktestStats& s, LrvxBacktestStats* out)
{
  out->totalTrades = s.totalTrades;
  out->winningTrades = s.winningTrades;
  out->losingTrades = s.losingTrades;
  out->maxConsecutiveWins = s.maxConsecutiveWins;
  out->maxConsecutiveLosses = s.maxConsecutiveLosses;
  out->initialCapital = s.initialCapital;
  out->finalCapital = s.finalCapital;
  out->totalPnl = s.totalPnl;
  out->totalFees = s.totalFees;
  out->netPnl = s.netPnl;
  out->grossProfit = s.grossProfit;
  out->grossLoss = s.grossLoss;
  out->maxDrawdown = s.maxDrawdown;
  out->maxDrawdownPct = s.maxDrawdownPct;
  out->winRate = s.winRate;
  out->profitFactor = s.profitFactor;
  out->avgWin = s.avgWin;
  out->avgLoss = s.avgLoss;
  out->avgWinLossRatio = s.avgWinLossRatio;
  out->avgTradeDurationNs = s.avgTradeDurationNs;
  out->medianTradeDurationNs = s.medianTradeDurationNs;
  out->maxTradeDurationNs = s.maxTradeDurationNs;
  out->sharpeRatio = s.sharpeRatio;
  out->sortinoRatio = s.sortinoRatio;
  out->calmarRatio = s.calmarRatio;
  out->timeWeightedReturn = s.timeWeightedReturn;
  out->returnPct = s.returnPct;
  out->startTimeNs = s.startTimeNs.raw();
  out->endTimeNs = s.endTimeNs.raw();
}

}  // namespace

uint32_t lrvx_walk_forward_run_csv(LrvxRegistryHandle reg_handle,
                                   const char* csv_path, const char* symbol,
                                   double fee_rate, double initial_capital,
                                   const LrvxWalkForwardConfig* cfg,
                                   LrvxWalkForwardFactoryFn factory,
                                   void* user_data,
                                   LrvxWalkForwardFold* folds_out,
                                   uint32_t max_folds)
{
  LRVX_CAPI_ENTER(reg_handle);
  if (!cfg || !csv_path || !factory)
  {
    return 0;
  }
  SymbolRegistry* reg = toRegistry(reg_handle);
  uint32_t symId = resolveSymbolId(reg, symbol);
  std::vector<OhlcvReplaySource::Bar> bars = loadOhlcvBarsCsv(csv_path, symId);
  if (bars.empty())
  {
    return 0;
  }

  BacktestConfig bcfg{};
  bcfg.feeRate = fee_rate;
  bcfg.initialCapital = initial_capital;
  bcfg.usePercentageFee = true;

  WalkForwardConfig wcfg{};
  wcfg.mode = (cfg->mode == 0) ? WalkForwardMode::Anchored
                               : WalkForwardMode::Sliding;
  wcfg.trainSize = cfg->train_size;
  wcfg.testSize = cfg->test_size;
  wcfg.step = cfg->step;
  wcfg.minTrainSize = cfg->min_train_size;

  // If the caller only wants a count, compute it from the config + bar
  // count without invoking the factory or running anything. This lets
  // bindings size the output buffer cheaply.
  if (!folds_out || max_folds == 0)
  {
    const uint64_t n = static_cast<uint64_t>(bars.size());
    const uint64_t step = wcfg.step == 0 ? wcfg.testSize : wcfg.step;
    if (wcfg.testSize == 0 || step == 0)
    {
      return 0;
    }
    if (wcfg.mode == WalkForwardMode::Anchored)
    {
      const uint64_t firstSplit =
          wcfg.minTrainSize > 0 ? wcfg.minTrainSize : wcfg.testSize;
      if (firstSplit + wcfg.testSize > n)
      {
        return 0;
      }
      return static_cast<uint32_t>(
          (n - firstSplit - wcfg.testSize) / step + 1);
    }
    if (wcfg.trainSize + wcfg.testSize > n)
    {
      return 0;
    }
    return static_cast<uint32_t>(
        (n - wcfg.trainSize - wcfg.testSize) / step + 1);
  }

  WalkForwardRunner wfr(bcfg, wcfg);
  wfr.setStrategyFactory([factory, user_data](std::size_t foldIdx) -> IStrategy*
                         {
                           LrvxStrategyHandle h = factory(user_data, foldIdx);
                           return reinterpret_cast<BridgeStrategy*>(h); });

  auto folds = wfr.run(bars);
  const uint32_t total = static_cast<uint32_t>(folds.size());
  const uint32_t n = (total < max_folds) ? total : max_folds;
  for (uint32_t i = 0; i < n; ++i)
  {
    const auto& f = folds[i];
    folds_out[i].fold_index = f.foldIndex;
    folds_out[i].train_start_bar = f.trainStartBar;
    folds_out[i].train_end_bar = f.trainEndBar;
    folds_out[i].test_start_bar = f.testStartBar;
    folds_out[i].test_end_bar = f.testEndBar;
    folds_out[i].train_start_ns = static_cast<int64_t>(f.trainStartNs);
    folds_out[i].train_end_ns = static_cast<int64_t>(f.trainEndNs);
    folds_out[i].test_start_ns = static_cast<int64_t>(f.testStartNs);
    folds_out[i].test_end_ns = static_cast<int64_t>(f.testEndNs);
    fillStatsStruct(f.trainStats, &folds_out[i].train_stats);
    fillStatsStruct(f.testStats, &folds_out[i].test_stats);
  }
  return n;
  LRVX_CAPI_LEAVE;
}

// ============================================================
// Grid search (sequential)
// ============================================================

struct LrvxGridSearchImpl
{
  GridSearch core;
};

LrvxGridSearchHandle lrvx_grid_search_create()
{
  LRVX_CAPI_ENTER_NOHANDLE;
  return static_cast<LrvxGridSearchHandle>(new LrvxGridSearchImpl());
  LRVX_CAPI_LEAVE;
}

void lrvx_grid_search_destroy(LrvxGridSearchHandle gs)
{
  LRVX_CAPI_ENTER_DESTROY(gs);
  delete static_cast<LrvxGridSearchImpl*>(gs);
  LRVX_CAPI_LEAVE_VOID;
}

void lrvx_grid_search_add_axis(LrvxGridSearchHandle gs,
                               const double* values, uint32_t num_values)
{
  LRVX_CAPI_ENTER_VOID(gs);
  if (!gs || !values)
  {
    return;
  }
  std::vector<double> v(values, values + num_values);
  static_cast<LrvxGridSearchImpl*>(gs)->core.addAxis(std::move(v));
  LRVX_CAPI_LEAVE_VOID;
}

uint64_t lrvx_grid_search_total(LrvxGridSearchHandle gs)
{
  LRVX_CAPI_ENTER(gs);
  return gs ? static_cast<LrvxGridSearchImpl*>(gs)->core.totalCombinations() : 0;
  LRVX_CAPI_LEAVE;
}

uint32_t lrvx_grid_search_params_for_index(LrvxGridSearchHandle gs,
                                           uint64_t index,
                                           double* params_out,
                                           uint32_t max_params)
{
  LRVX_CAPI_ENTER(gs);
  if (!gs)
  {
    return 0;
  }
  auto p = static_cast<LrvxGridSearchImpl*>(gs)->core.paramsForIndex(index);
  if (!params_out)
  {
    return static_cast<uint32_t>(p.size());
  }
  const uint32_t n = std::min(static_cast<uint32_t>(p.size()), max_params);
  for (uint32_t i = 0; i < n; ++i)
  {
    params_out[i] = p[i];
  }
  return n;
  LRVX_CAPI_LEAVE;
}

uint64_t lrvx_grid_search_run(LrvxGridSearchHandle gs,
                              LrvxGridSearchFactoryFn factory,
                              void* user_data,
                              LrvxBacktestStats* stats_out,
                              uint32_t max_results)
{
  LRVX_CAPI_ENTER(gs);
  if (!gs || !factory)
  {
    return 0;
  }
  auto* impl = static_cast<LrvxGridSearchImpl*>(gs);
  const uint64_t total = impl->core.totalCombinations();
  if (!stats_out)
  {
    return total;
  }
  // Run sequentially via direct calls to factory; we don't go through
  // GridSearch::run because the factory expects a C-side stats fill,
  // not a BacktestResult.
  const uint64_t n = std::min<uint64_t>(total, max_results);
  for (uint64_t i = 0; i < n; ++i)
  {
    auto params = impl->core.paramsForIndex(i);
    LrvxBacktestStats s{};
    factory(user_data, i, params.data(),
            static_cast<uint32_t>(params.size()), &s);
    stats_out[i] = s;
  }
  return n;
  LRVX_CAPI_LEAVE;
}

// ============================================================
// Heatmap rendering
// ============================================================

uint64_t lrvx_render_heatmap_html(const LrvxHeatmapData* data,
                                  char* out_buf, uint64_t max_size)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  if (!data || data->z == nullptr || data->rows == 0 || data->cols == 0)
  {
    return 0;
  }
  lrvx::report::HeatmapData hd;
  const std::size_t n = static_cast<std::size_t>(data->rows) * data->cols;
  hd.z.assign(data->z, data->z + n);
  hd.rows = data->rows;
  hd.cols = data->cols;
  if (data->row_labels && data->num_row_labels > 0)
  {
    hd.rowLabels.reserve(data->num_row_labels);
    for (uint32_t i = 0; i < data->num_row_labels; ++i)
    {
      hd.rowLabels.emplace_back(data->row_labels[i] ? data->row_labels[i] : "");
    }
  }
  if (data->col_labels && data->num_col_labels > 0)
  {
    hd.colLabels.reserve(data->num_col_labels);
    for (uint32_t i = 0; i < data->num_col_labels; ++i)
    {
      hd.colLabels.emplace_back(data->col_labels[i] ? data->col_labels[i] : "");
    }
  }
  if (data->title)
  {
    hd.title = data->title;
  }
  if (data->x_axis_name)
  {
    hd.xAxisName = data->x_axis_name;
  }
  if (data->y_axis_name)
  {
    hd.yAxisName = data->y_axis_name;
  }
  if (data->metric_name)
  {
    hd.metricName = data->metric_name;
  }

  std::string html = lrvx::report::renderHeatmapHtml(hd);
  const uint64_t total = static_cast<uint64_t>(html.size());
  if (!out_buf)
  {
    return total;
  }
  const uint64_t to_copy = (total < max_size) ? total : max_size;
  std::memcpy(out_buf, html.data(), to_copy);
  return total;
  LRVX_CAPI_LEAVE;
}

// ── Latency models ────────────────────────────────────────────────

namespace
{
lrvx::LatencyModel* asLatency(LrvxLatencyModelHandle h)
{
  return static_cast<lrvx::LatencyModel*>(h);
}
}  // namespace

extern "C" LrvxLatencyModelHandle lrvx_latency_constant_create(int64_t feed_ns,
                                                               int64_t order_ns,
                                                               int64_t fill_ns)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  try
  {
    return new lrvx::ConstantLatency(feed_ns, order_ns, fill_ns);
  }
  catch (...)
  {
    return nullptr;
  }
  LRVX_CAPI_LEAVE;
}

extern "C" LrvxLatencyModelHandle lrvx_latency_gaussian_create(double feed_mean_ns,
                                                               double feed_stddev_ns,
                                                               double order_mean_ns,
                                                               double order_stddev_ns,
                                                               double fill_mean_ns,
                                                               double fill_stddev_ns,
                                                               uint64_t seed)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  try
  {
    return new lrvx::GaussianLatency(feed_mean_ns, feed_stddev_ns,
                                     order_mean_ns, order_stddev_ns,
                                     fill_mean_ns, fill_stddev_ns, seed);
  }
  catch (...)
  {
    return nullptr;
  }
  LRVX_CAPI_LEAVE;
}

extern "C" LrvxLatencyModelHandle lrvx_latency_exponential_create(double feed_mean_ns,
                                                                  double order_mean_ns,
                                                                  double fill_mean_ns,
                                                                  uint64_t seed)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  try
  {
    return new lrvx::ExponentialLatency(feed_mean_ns, order_mean_ns, fill_mean_ns, seed);
  }
  catch (...)
  {
    return nullptr;
  }
  LRVX_CAPI_LEAVE;
}

extern "C" LrvxLatencyModelHandle lrvx_latency_empirical_create(const int64_t* feed_samples,
                                                                size_t feed_count,
                                                                const int64_t* order_samples,
                                                                size_t order_count,
                                                                const int64_t* fill_samples,
                                                                size_t fill_count,
                                                                uint64_t seed)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  try
  {
    std::vector<int64_t> feed(feed_samples, feed_samples + feed_count);
    std::vector<int64_t> order(order_samples, order_samples + order_count);
    std::vector<int64_t> fill(fill_samples, fill_samples + fill_count);
    return new lrvx::EmpiricalLatency(std::move(feed), std::move(order),
                                      std::move(fill), seed);
  }
  catch (...)
  {
    return nullptr;
  }
  LRVX_CAPI_LEAVE;
}

extern "C" void lrvx_latency_destroy(LrvxLatencyModelHandle model)
{
  LRVX_CAPI_ENTER_DESTROY(model);
  delete asLatency(model);
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" int64_t lrvx_latency_feed_delay(LrvxLatencyModelHandle model)
{
  LRVX_CAPI_ENTER(model);
  return model ? asLatency(model)->feedDelay() : 0;
  LRVX_CAPI_LEAVE;
}

extern "C" int64_t lrvx_latency_order_delay(LrvxLatencyModelHandle model)
{
  LRVX_CAPI_ENTER(model);
  return model ? asLatency(model)->orderDelay() : 0;
  LRVX_CAPI_LEAVE;
}

extern "C" int64_t lrvx_latency_fill_delay(LrvxLatencyModelHandle model)
{
  LRVX_CAPI_ENTER(model);
  return model ? asLatency(model)->fillDelay() : 0;
  LRVX_CAPI_LEAVE;
}

extern "C" void lrvx_latency_sample(LrvxLatencyModelHandle model, LrvxLatencySample* out)
{
  LRVX_CAPI_ENTER_VOID(model);
  if (!model || !out)
  {
    return;
  }
  lrvx::LatencySample s = asLatency(model)->sample();
  out->feed_ns = s.feed_ns;
  out->order_ns = s.order_ns;
  out->fill_ns = s.fill_ns;
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" void lrvx_latency_reset(LrvxLatencyModelHandle model, uint64_t seed)
{
  LRVX_CAPI_ENTER_VOID(model);
  if (model)
  {
    asLatency(model)->reset(seed);
  }
  LRVX_CAPI_LEAVE_VOID;
}

// ── Tape diff ─────────────────────────────────────────────────────

namespace
{
lrvx::replay::TapeDiffResult* asDiff(LrvxTapeDiffHandle h)
{
  return static_cast<lrvx::replay::TapeDiffResult*>(h);
}
}  // namespace

extern "C" LrvxTapeDiffHandle lrvx_tape_diff_create(const char* left_path,
                                                    const char* right_path,
                                                    uint32_t max_mismatches,
                                                    int64_t field_tolerance_ns)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  if (!left_path || !right_path)
  {
    return nullptr;
  }
  if (!std::filesystem::is_directory(left_path) ||
      !std::filesystem::is_directory(right_path))
  {
    return nullptr;
  }
  try
  {
    lrvx::replay::TapeDiffOptions opts;
    opts.max_mismatches = max_mismatches;
    opts.field_tolerance_ns = field_tolerance_ns;
    auto* result = new lrvx::replay::TapeDiffResult(
        lrvx::replay::diffTapes(left_path, right_path, opts));
    return result;
  }
  catch (...)
  {
    return nullptr;
  }
  LRVX_CAPI_LEAVE;
}

extern "C" void lrvx_tape_diff_destroy(LrvxTapeDiffHandle handle)
{
  LRVX_CAPI_ENTER_DESTROY(handle);
  delete asDiff(handle);
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" uint64_t lrvx_tape_diff_left_count(LrvxTapeDiffHandle handle)
{
  LRVX_CAPI_ENTER(handle);
  return handle ? asDiff(handle)->left_count : 0;
  LRVX_CAPI_LEAVE;
}

extern "C" uint64_t lrvx_tape_diff_right_count(LrvxTapeDiffHandle handle)
{
  LRVX_CAPI_ENTER(handle);
  return handle ? asDiff(handle)->right_count : 0;
  LRVX_CAPI_LEAVE;
}

extern "C" uint8_t lrvx_tape_diff_first_divergence(LrvxTapeDiffHandle handle,
                                                   uint64_t* out_index)
{
  LRVX_CAPI_ENTER(handle);
  if (!handle)
  {
    return 0;
  }
  auto& r = *asDiff(handle);
  if (!r.first_divergence_index.has_value())
  {
    return 0;
  }
  if (out_index)
  {
    *out_index = *r.first_divergence_index;
  }
  return 1;
  LRVX_CAPI_LEAVE;
}

extern "C" uint8_t lrvx_tape_diff_equal(LrvxTapeDiffHandle handle)
{
  LRVX_CAPI_ENTER(handle);
  return (handle && asDiff(handle)->equal) ? 1 : 0;
  LRVX_CAPI_LEAVE;
}

extern "C" uint64_t lrvx_tape_diff_mismatch_count(LrvxTapeDiffHandle handle)
{
  LRVX_CAPI_ENTER(handle);
  return handle ? asDiff(handle)->mismatches.size() : 0;
  LRVX_CAPI_LEAVE;
}

extern "C" uint64_t lrvx_tape_diff_copy_mismatches(LrvxTapeDiffHandle handle,
                                                   LrvxTapeDiffMismatch* out,
                                                   uint64_t max_entries)
{
  LRVX_CAPI_ENTER(handle);
  if (!handle)
  {
    return 0;
  }
  auto& r = *asDiff(handle);
  const uint64_t total = r.mismatches.size();
  if (!out)
  {
    return total;
  }
  const uint64_t to_copy = std::min<uint64_t>(total, max_entries);
  for (uint64_t i = 0; i < to_copy; ++i)
  {
    const auto& m = r.mismatches[i];
    out[i].index = m.index;
    out[i].left.exchange_ts_ns = m.left.exchange_ts_ns;
    out[i].left.price_raw = m.left.price_raw;
    out[i].left.qty_raw = m.left.qty_raw;
    out[i].left.symbol_id = m.left.symbol_id;
    out[i].left.side = m.left.side;
    out[i].right.exchange_ts_ns = m.right.exchange_ts_ns;
    out[i].right.price_raw = m.right.price_raw;
    out[i].right.qty_raw = m.right.qty_raw;
    out[i].right.symbol_id = m.right.symbol_id;
    out[i].right.side = m.right.side;
  }
  return to_copy;
  LRVX_CAPI_LEAVE;
}

// ── Portfolio risk aggregator ─────────────────────────────────────

namespace
{
lrvx::risk::PortfolioRiskAggregator* asPortfolio(LrvxPortfolioRiskHandle h)
{
  return static_cast<lrvx::risk::PortfolioRiskAggregator*>(h);
}

// Defined alongside the breach scratch table further down; declared here
// because _destroy has to drop the entry before the address is recycled.
void eraseBreachScratch(LrvxPortfolioRiskHandle handle) noexcept;

lrvx::risk::RiskRules unpackRules(const LrvxPortfolioRiskRules* r)
{
  lrvx::risk::RiskRules out;
  if (!r)
  {
    return out;
  }
  if (r->has_max_drawdown_pct)
  {
    out.max_drawdown_pct = r->max_drawdown_pct;
  }
  if (r->has_max_daily_loss)
  {
    out.max_daily_loss = r->max_daily_loss;
  }
  if (r->has_max_gross_exposure)
  {
    out.max_gross_exposure = r->max_gross_exposure;
  }
  if (r->has_max_concentration_pct)
  {
    out.max_concentration_pct = r->max_concentration_pct;
  }
  return out;
}
}  // namespace

extern "C" LrvxPortfolioRiskHandle lrvx_portfolio_risk_create(
    const LrvxPortfolioRiskRules* rules, double initial_equity)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  try
  {
    return new lrvx::risk::PortfolioRiskAggregator(unpackRules(rules), initial_equity);
  }
  catch (...)
  {
    return nullptr;
  }
  LRVX_CAPI_LEAVE;
}

extern "C" void lrvx_portfolio_risk_destroy(LrvxPortfolioRiskHandle handle)
{
  LRVX_CAPI_ENTER_DESTROY(handle);
  eraseBreachScratch(handle);
  delete asPortfolio(handle);
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" void lrvx_portfolio_risk_update(LrvxPortfolioRiskHandle handle,
                                           const char* name,
                                           const LrvxStrategyAccountFields* fields,
                                           uint8_t field_mask)
{
  LRVX_CAPI_ENTER_VOID(handle);
  if (!handle || !name || !fields)
  {
    return;
  }
  lrvx::risk::StrategyAccount row;
  row.name = name;
  row.realized_pnl = fields->realized_pnl;
  row.unrealized_pnl = fields->unrealized_pnl;
  row.fees = fields->fees;
  row.gross_exposure = fields->gross_exposure;
  row.net_exposure = fields->net_exposure;
  row.trade_count = fields->trade_count;
  asPortfolio(handle)->update(name, row, field_mask);
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" void lrvx_portfolio_risk_remove(LrvxPortfolioRiskHandle handle, const char* name)
{
  LRVX_CAPI_ENTER_VOID(handle);
  if (!handle || !name)
  {
    return;
  }
  asPortfolio(handle)->remove(name);
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" void lrvx_portfolio_risk_reset_kill_switch(LrvxPortfolioRiskHandle handle)
{
  LRVX_CAPI_ENTER_VOID(handle);
  if (handle)
  {
    asPortfolio(handle)->resetKillSwitch();
  }
  LRVX_CAPI_LEAVE_VOID;
}

namespace
{
// Scratch storage behind the count-then-read pair, holding the strings a
// LrvxBreach points into.
//
// It used to be a thread_local map keyed by the handle address, which failed
// twice over. Splitting the pair across threads -- the one reason a C layer
// exists at all -- left the reader with nothing and a zero return, which
// reads exactly like "no breaches". And the entry outlived the handle, so
// when the allocator handed the same address to a new object, a freshly
// created, clean risk handle answered breach_at with the destroyed session's
// max_daily_loss breach and reported success.
//
// One process-wide table under a mutex closes both: every thread sees the
// same entry, and _destroy erases it, so a reused address starts empty.
struct PortfolioBreachScratch
{
  std::vector<lrvx::risk::Breach> breaches;
};

std::mutex g_breach_scratch_mutex;
std::map<LrvxPortfolioRiskHandle, PortfolioBreachScratch> g_breach_scratch;

void eraseBreachScratch(LrvxPortfolioRiskHandle handle) noexcept
{
  try
  {
    const std::lock_guard<std::mutex> lock(g_breach_scratch_mutex);
    g_breach_scratch.erase(handle);
  }
  catch (...)
  {
  }
}

void writeBreach(LrvxBreach* out, const lrvx::risk::Breach& b)
{
  if (!out)
  {
    return;
  }
  out->rule = b.rule.c_str();
  out->value = b.value;
  out->limit = b.limit;
  out->detail = b.detail.c_str();
}
}  // namespace

extern "C" uint8_t lrvx_portfolio_risk_check_order(LrvxPortfolioRiskHandle handle,
                                                   const char* strategy, double notional,
                                                   const char* side, LrvxBreach* out_breach)
{
  LRVX_CAPI_ENTER(handle);
  if (!handle)
  {
    return 0;
  }
  auto opt = asPortfolio(handle)->checkOrder(strategy ? strategy : "",
                                             notional, side ? side : "");
  if (!opt.has_value())
  {
    return 0;
  }
  const std::lock_guard<std::mutex> lock(g_breach_scratch_mutex);
  auto& scratch = g_breach_scratch[handle];
  scratch.breaches = {*opt};
  writeBreach(out_breach, scratch.breaches.front());
  return 1;
  LRVX_CAPI_LEAVE;
}

extern "C" double lrvx_portfolio_risk_total_daily_pnl(LrvxPortfolioRiskHandle handle)
{
  LRVX_CAPI_ENTER(handle);
  return handle ? asPortfolio(handle)->snapshot().total_daily_pnl : 0.0;
  LRVX_CAPI_LEAVE;
}

extern "C" double lrvx_portfolio_risk_total_gross_exposure(LrvxPortfolioRiskHandle handle)
{
  LRVX_CAPI_ENTER(handle);
  return handle ? asPortfolio(handle)->snapshot().total_gross_exposure : 0.0;
  LRVX_CAPI_LEAVE;
}

extern "C" double lrvx_portfolio_risk_current_equity(LrvxPortfolioRiskHandle handle)
{
  LRVX_CAPI_ENTER(handle);
  return handle ? asPortfolio(handle)->snapshot().current_equity : 0.0;
  LRVX_CAPI_LEAVE;
}

extern "C" double lrvx_portfolio_risk_drawdown_pct(LrvxPortfolioRiskHandle handle)
{
  LRVX_CAPI_ENTER(handle);
  return handle ? asPortfolio(handle)->snapshot().drawdown_pct : 0.0;
  LRVX_CAPI_LEAVE;
}

extern "C" uint8_t lrvx_portfolio_risk_kill_switch_active(LrvxPortfolioRiskHandle handle)
{
  LRVX_CAPI_ENTER(handle);
  return (handle && asPortfolio(handle)->snapshot().kill_switch_active) ? 1 : 0;
  LRVX_CAPI_LEAVE;
}

extern "C" uint64_t lrvx_portfolio_risk_breach_count(LrvxPortfolioRiskHandle handle)
{
  LRVX_CAPI_ENTER(handle);
  if (!handle)
  {
    return 0;
  }
  auto snap = asPortfolio(handle)->snapshot();
  const std::lock_guard<std::mutex> lock(g_breach_scratch_mutex);
  auto& scratch = g_breach_scratch[handle];
  scratch.breaches = std::move(snap.active_breaches);
  return scratch.breaches.size();
  LRVX_CAPI_LEAVE;
}

extern "C" uint8_t lrvx_portfolio_risk_breach_at(LrvxPortfolioRiskHandle handle,
                                                 uint64_t index, LrvxBreach* out)
{
  LRVX_CAPI_ENTER(handle);
  if (!handle || !out)
  {
    return 0;
  }
  const std::lock_guard<std::mutex> lock(g_breach_scratch_mutex);
  auto it = g_breach_scratch.find(handle);
  if (it == g_breach_scratch.end())
  {
    return 0;
  }
  if (index >= it->second.breaches.size())
  {
    return 0;
  }
  writeBreach(out, it->second.breaches[index]);
  return 1;
  LRVX_CAPI_LEAVE;
}

extern "C" uint64_t lrvx_portfolio_risk_account_count(LrvxPortfolioRiskHandle handle)
{
  LRVX_CAPI_ENTER(handle);
  return handle ? asPortfolio(handle)->snapshot().accounts.size() : 0;
  LRVX_CAPI_LEAVE;
}

// ── Execution algorithms ──────────────────────────────────────────

namespace
{
lrvx::execution::ExecutionAlgo* asAlgo(LrvxExecAlgoHandle h)
{
  return static_cast<lrvx::execution::ExecutionAlgo*>(h);
}
}  // namespace

extern "C" LrvxExecAlgoHandle lrvx_exec_twap_create(double target_qty, uint8_t side,
                                                    uint32_t symbol, uint8_t type,
                                                    double limit_price,
                                                    int64_t duration_ns,
                                                    uint32_t slice_count,
                                                    int64_t start_time_ns)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  try
  {
    return new lrvx::execution::TWAPExecutor(
        target_qty,
        static_cast<lrvx::execution::Side>(side),
        symbol,
        static_cast<lrvx::execution::OrderType>(type),
        limit_price,
        duration_ns, slice_count, start_time_ns);
  }
  catch (...)
  {
    return nullptr;
  }
  LRVX_CAPI_LEAVE;
}

extern "C" LrvxExecAlgoHandle lrvx_exec_vwap_create(double target_qty, uint8_t side,
                                                    uint32_t symbol, uint8_t type,
                                                    double limit_price,
                                                    const int64_t* volume_curve_ts,
                                                    const double* volume_curve_vol,
                                                    size_t n)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  try
  {
    std::vector<std::pair<int64_t, double>> curve;
    curve.reserve(n);
    for (size_t i = 0; i < n; ++i)
    {
      curve.emplace_back(volume_curve_ts[i], volume_curve_vol[i]);
    }
    return new lrvx::execution::VWAPExecutor(
        target_qty,
        static_cast<lrvx::execution::Side>(side),
        symbol,
        static_cast<lrvx::execution::OrderType>(type),
        limit_price,
        std::move(curve));
  }
  catch (...)
  {
    return nullptr;
  }
  LRVX_CAPI_LEAVE;
}

extern "C" LrvxExecAlgoHandle lrvx_exec_iceberg_create(double target_qty, uint8_t side,
                                                       uint32_t symbol, uint8_t type,
                                                       double limit_price,
                                                       double visible_qty)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  try
  {
    return new lrvx::execution::IcebergExecutor(
        target_qty,
        static_cast<lrvx::execution::Side>(side),
        symbol,
        static_cast<lrvx::execution::OrderType>(type),
        limit_price, visible_qty);
  }
  catch (...)
  {
    return nullptr;
  }
  LRVX_CAPI_LEAVE;
}

extern "C" LrvxExecAlgoHandle lrvx_exec_pov_create(double target_qty, uint8_t side,
                                                   uint32_t symbol, uint8_t type,
                                                   double limit_price,
                                                   double participation_rate,
                                                   double min_slice_qty)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  try
  {
    return new lrvx::execution::POVExecutor(
        target_qty,
        static_cast<lrvx::execution::Side>(side),
        symbol,
        static_cast<lrvx::execution::OrderType>(type),
        limit_price, participation_rate, min_slice_qty);
  }
  catch (...)
  {
    return nullptr;
  }
  LRVX_CAPI_LEAVE;
}

extern "C" void lrvx_exec_destroy(LrvxExecAlgoHandle handle)
{
  LRVX_CAPI_ENTER_DESTROY(handle);
  delete asAlgo(handle);
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" void lrvx_exec_step(LrvxExecAlgoHandle handle, int64_t now_ns)
{
  LRVX_CAPI_ENTER_VOID(handle);
  if (handle)
  {
    asAlgo(handle)->step(now_ns);
  }
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" void lrvx_exec_report_fill(LrvxExecAlgoHandle handle, double qty)
{
  LRVX_CAPI_ENTER_VOID(handle);
  if (handle)
  {
    asAlgo(handle)->reportFill(qty);
  }
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" void lrvx_exec_observe_volume(LrvxExecAlgoHandle handle, double qty)
{
  LRVX_CAPI_ENTER_VOID(handle);
  if (handle)
  {
    asAlgo(handle)->observeVolume(qty);
  }
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" size_t lrvx_exec_pending_count(LrvxExecAlgoHandle handle)
{
  LRVX_CAPI_ENTER(handle);
  return handle ? asAlgo(handle)->pending().size() : 0;
  LRVX_CAPI_LEAVE;
}

extern "C" uint8_t lrvx_exec_pending_at(LrvxExecAlgoHandle handle, size_t index,
                                        LrvxExecChildOrder* out)
{
  LRVX_CAPI_ENTER(handle);
  if (!handle || !out)
  {
    return 0;
  }
  const auto& p = asAlgo(handle)->pending();
  if (index >= p.size())
  {
    return 0;
  }
  const auto& c = p[index];
  out->order_id = c.order_id;
  out->timestamp_ns = c.timestamp_ns;
  out->qty = c.qty;
  out->price = c.price;
  out->type = static_cast<uint8_t>(c.type);
  return 1;
  LRVX_CAPI_LEAVE;
}

extern "C" void lrvx_exec_clear_pending(LrvxExecAlgoHandle handle)
{
  LRVX_CAPI_ENTER_VOID(handle);
  if (handle)
  {
    asAlgo(handle)->clearPending();
  }
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" double lrvx_exec_target_qty(LrvxExecAlgoHandle handle)
{
  LRVX_CAPI_ENTER(handle);
  return handle ? asAlgo(handle)->targetQty() : 0.0;
  LRVX_CAPI_LEAVE;
}

extern "C" double lrvx_exec_submitted_qty(LrvxExecAlgoHandle handle)
{
  LRVX_CAPI_ENTER(handle);
  return handle ? asAlgo(handle)->submittedQty() : 0.0;
  LRVX_CAPI_LEAVE;
}

extern "C" double lrvx_exec_filled_qty(LrvxExecAlgoHandle handle)
{
  LRVX_CAPI_ENTER(handle);
  return handle ? asAlgo(handle)->filledQty() : 0.0;
  LRVX_CAPI_LEAVE;
}

extern "C" double lrvx_exec_remaining_qty(LrvxExecAlgoHandle handle)
{
  LRVX_CAPI_ENTER(handle);
  return handle ? asAlgo(handle)->remainingQty() : 0.0;
  LRVX_CAPI_LEAVE;
}

extern "C" uint8_t lrvx_exec_is_done(LrvxExecAlgoHandle handle)
{
  LRVX_CAPI_ENTER(handle);
  return (handle && asAlgo(handle)->isDone()) ? 1 : 0;
  LRVX_CAPI_LEAVE;
}

// ── Delta book compression ────────────────────────────────────────

namespace
{
lrvx::replay::DeltaBookEncoder* asEncoder(LrvxDeltaBookEncoderHandle h)
{
  return static_cast<lrvx::replay::DeltaBookEncoder*>(h);
}
lrvx::replay::DeltaBookReplayer* asReplayer(LrvxDeltaBookReplayerHandle h)
{
  return static_cast<lrvx::replay::DeltaBookReplayer*>(h);
}

std::vector<lrvx::replay::BookLevel> toLevels(const LrvxBookLevel* arr, size_t n)
{
  std::vector<lrvx::replay::BookLevel> out(n);
  for (size_t i = 0; i < n; ++i)
  {
    out[i].price_raw = arr[i].price_raw;
    out[i].qty_raw = arr[i].quantity_raw;
  }
  return out;
}

uint64_t copyLevelsTo(const std::vector<lrvx::replay::BookLevel>& src,
                      LrvxBookLevel* out, uint64_t max_entries)
{
  const uint64_t n = std::min<uint64_t>(src.size(), max_entries);
  for (uint64_t i = 0; i < n; ++i)
  {
    out[i].price_raw = src[i].price_raw;
    out[i].quantity_raw = src[i].qty_raw;
  }
  return n;
}

// Per-handle scratch for last encode/replay output. The C ABI returns
// counts plus a follow-up copy() call, matching the pattern used by
// lrvx_tape_diff and lrvx_portfolio_risk for variable-length data.
struct EncoderScratch
{
  std::vector<lrvx::replay::BookLevel> bids;
  std::vector<lrvx::replay::BookLevel> asks;
};

// Same reasoning as the portfolio risk scratch above: process-wide under a
// mutex, erased by _destroy, so the encode/copy pair works across threads and
// a reused address cannot serve a dead handle's levels.
std::mutex g_encoder_scratch_mutex;
std::map<LrvxDeltaBookEncoderHandle, EncoderScratch> g_encoder_scratch;
std::map<LrvxDeltaBookReplayerHandle, EncoderScratch> g_replayer_scratch;
}  // namespace

extern "C" LrvxDeltaBookEncoderHandle lrvx_delta_book_encoder_create(uint32_t anchor_every)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  try
  {
    return new lrvx::replay::DeltaBookEncoder(anchor_every);
  }
  catch (...)
  {
    return nullptr;
  }
  LRVX_CAPI_LEAVE;
}

extern "C" void lrvx_delta_book_encoder_destroy(LrvxDeltaBookEncoderHandle handle)
{
  LRVX_CAPI_ENTER_DESTROY(handle);
  {
    const std::lock_guard<std::mutex> lock(g_encoder_scratch_mutex);
    g_encoder_scratch.erase(handle);
  }
  delete asEncoder(handle);
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" void lrvx_delta_book_encoder_reset(LrvxDeltaBookEncoderHandle handle, uint32_t symbol_id)
{
  LRVX_CAPI_ENTER_VOID(handle);
  if (handle)
  {
    asEncoder(handle)->reset(symbol_id);
  }
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" void lrvx_delta_book_encoder_reset_all(LrvxDeltaBookEncoderHandle handle)
{
  LRVX_CAPI_ENTER_VOID(handle);
  if (handle)
  {
    asEncoder(handle)->resetAll();
  }
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" void lrvx_delta_book_encoder_encode(LrvxDeltaBookEncoderHandle handle,
                                               uint32_t symbol_id,
                                               const LrvxBookLevel* bids, size_t bid_count,
                                               const LrvxBookLevel* asks, size_t ask_count,
                                               uint8_t* out_is_delta,
                                               uint64_t* out_bid_count,
                                               uint64_t* out_ask_count)
{
  LRVX_CAPI_ENTER_VOID(handle);
  if (!handle)
  {
    return;
  }
  auto bids_in = toLevels(bids, bid_count);
  auto asks_in = toLevels(asks, ask_count);
  auto result = asEncoder(handle)->encode(symbol_id, bids_in, asks_in);
  const std::lock_guard<std::mutex> lock(g_encoder_scratch_mutex);
  auto& scratch = g_encoder_scratch[handle];
  scratch.bids = std::move(result.bids);
  scratch.asks = std::move(result.asks);
  if (out_is_delta)
  {
    *out_is_delta = result.is_delta ? 1 : 0;
  }
  if (out_bid_count)
  {
    *out_bid_count = scratch.bids.size();
  }
  if (out_ask_count)
  {
    *out_ask_count = scratch.asks.size();
  }
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" uint64_t lrvx_delta_book_encoder_copy_bids(LrvxDeltaBookEncoderHandle handle,
                                                      LrvxBookLevel* out, uint64_t max_entries)
{
  LRVX_CAPI_ENTER(handle);
  if (!handle || !out)
  {
    return 0;
  }
  const std::lock_guard<std::mutex> lock(g_encoder_scratch_mutex);
  auto it = g_encoder_scratch.find(handle);
  if (it == g_encoder_scratch.end())
  {
    return 0;
  }
  return copyLevelsTo(it->second.bids, out, max_entries);
  LRVX_CAPI_LEAVE;
}

extern "C" uint64_t lrvx_delta_book_encoder_copy_asks(LrvxDeltaBookEncoderHandle handle,
                                                      LrvxBookLevel* out, uint64_t max_entries)
{
  LRVX_CAPI_ENTER(handle);
  if (!handle || !out)
  {
    return 0;
  }
  const std::lock_guard<std::mutex> lock(g_encoder_scratch_mutex);
  auto it = g_encoder_scratch.find(handle);
  if (it == g_encoder_scratch.end())
  {
    return 0;
  }
  return copyLevelsTo(it->second.asks, out, max_entries);
  LRVX_CAPI_LEAVE;
}

extern "C" LrvxDeltaBookReplayerHandle lrvx_delta_book_replayer_create(void)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  try
  {
    return new lrvx::replay::DeltaBookReplayer();
  }
  catch (...)
  {
    return nullptr;
  }
  LRVX_CAPI_LEAVE;
}

extern "C" void lrvx_delta_book_replayer_destroy(LrvxDeltaBookReplayerHandle handle)
{
  LRVX_CAPI_ENTER_DESTROY(handle);
  {
    const std::lock_guard<std::mutex> lock(g_encoder_scratch_mutex);
    g_replayer_scratch.erase(handle);
  }
  delete asReplayer(handle);
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" void lrvx_delta_book_replayer_reset(LrvxDeltaBookReplayerHandle handle, uint32_t symbol_id)
{
  LRVX_CAPI_ENTER_VOID(handle);
  if (handle)
  {
    asReplayer(handle)->reset(symbol_id);
  }
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" void lrvx_delta_book_replayer_apply(LrvxDeltaBookReplayerHandle handle,
                                               uint8_t type, uint32_t symbol_id,
                                               const LrvxBookLevel* bids, size_t bid_count,
                                               const LrvxBookLevel* asks, size_t ask_count,
                                               uint64_t* out_bid_count,
                                               uint64_t* out_ask_count)
{
  LRVX_CAPI_ENTER_VOID(handle);
  if (!handle)
  {
    return;
  }
  auto bids_in = toLevels(bids, bid_count);
  auto asks_in = toLevels(asks, ask_count);
  auto snap = asReplayer(handle)->apply(type, symbol_id, bids_in, asks_in);
  const std::lock_guard<std::mutex> lock(g_encoder_scratch_mutex);
  auto& scratch = g_replayer_scratch[handle];
  scratch.bids = std::move(snap.bids);
  scratch.asks = std::move(snap.asks);
  if (out_bid_count)
  {
    *out_bid_count = scratch.bids.size();
  }
  if (out_ask_count)
  {
    *out_ask_count = scratch.asks.size();
  }
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" uint64_t lrvx_delta_book_replayer_copy_bids(LrvxDeltaBookReplayerHandle handle,
                                                       LrvxBookLevel* out, uint64_t max_entries)
{
  LRVX_CAPI_ENTER(handle);
  if (!handle || !out)
  {
    return 0;
  }
  const std::lock_guard<std::mutex> lock(g_encoder_scratch_mutex);
  auto it = g_replayer_scratch.find(handle);
  if (it == g_replayer_scratch.end())
  {
    return 0;
  }
  return copyLevelsTo(it->second.bids, out, max_entries);
  LRVX_CAPI_LEAVE;
}

extern "C" uint64_t lrvx_delta_book_replayer_copy_asks(LrvxDeltaBookReplayerHandle handle,
                                                       LrvxBookLevel* out, uint64_t max_entries)
{
  LRVX_CAPI_ENTER(handle);
  if (!handle || !out)
  {
    return 0;
  }
  const std::lock_guard<std::mutex> lock(g_encoder_scratch_mutex);
  auto it = g_replayer_scratch.find(handle);
  if (it == g_replayer_scratch.end())
  {
    return 0;
  }
  return copyLevelsTo(it->second.asks, out, max_entries);
  LRVX_CAPI_LEAVE;
}

// ============================================================
// Strategy run trace (.lrvxrun) — implementation
// ============================================================

namespace
{

inline lrvx::run::TraceRecorder* asRecorder(LrvxRunRecorderHandle h)
{
  return static_cast<lrvx::run::TraceRecorder*>(h);
}

struct RunReaderState
{
  std::unique_ptr<lrvx::run::TraceReader> reader;
  std::vector<lrvx::run::OwnedSignal> signals;
  std::vector<lrvx::run::OwnedOrderEvent> orders;
  std::vector<lrvx::run::FillRecord> fills;
};

inline RunReaderState* asReader(LrvxRunReaderHandle h)
{
  return static_cast<RunReaderState*>(h);
}

uint64_t copyStringTo(const std::string& src, char* out, uint64_t max_bytes)
{
  if (out == nullptr || max_bytes == 0)
  {
    return src.size();
  }
  uint64_t n = std::min(static_cast<uint64_t>(src.size()), max_bytes);
  std::memcpy(out, src.data(), n);
  return n;
}

}  // namespace

extern "C" LrvxRunRecorderHandle lrvx_run_recorder_create(const char* path,
                                                          const char* strategy_id,
                                                          const char* strategy_hash,
                                                          int64_t run_started_ns)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  if (path == nullptr)
  {
    return nullptr;
  }
  lrvx::run::TraceRecorderOptions opts;
  opts.strategy_id = strategy_id ? strategy_id : "";
  opts.strategy_hash = strategy_hash ? strategy_hash : "";
  opts.run_started_ns = run_started_ns;
  try
  {
    return new lrvx::run::TraceRecorder(path, std::move(opts));
  }
  catch (...)
  {
    return nullptr;
  }
  LRVX_CAPI_LEAVE;
}

extern "C" void lrvx_run_recorder_destroy(LrvxRunRecorderHandle handle)
{
  LRVX_CAPI_ENTER_DESTROY(handle);
  delete asRecorder(handle);
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" void lrvx_run_recorder_add_tape_ref(LrvxRunRecorderHandle handle,
                                               const char* path,
                                               const char* content_hash,
                                               int64_t first_event_ns,
                                               int64_t last_event_ns)
{
  LRVX_CAPI_ENTER_VOID(handle);
  auto* rec = asRecorder(handle);
  if (rec == nullptr || path == nullptr)
  {
    return;
  }
  lrvx::run::TapeRef ref;
  ref.path = path;
  ref.content_hash = content_hash ? content_hash : "";
  ref.first_event_ns = first_event_ns;
  ref.last_event_ns = last_event_ns;
  rec->addTapeRef(std::move(ref));
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" void lrvx_run_recorder_set_run_ended_ns(LrvxRunRecorderHandle handle, int64_t ns)
{
  LRVX_CAPI_ENTER_VOID(handle);
  if (auto* rec = asRecorder(handle))
  {
    rec->setRunEndedNs(ns);
  }
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" void lrvx_run_recorder_write_signal(LrvxRunRecorderHandle handle,
                                               int64_t run_ts_ns, int64_t feed_ts_ns,
                                               uint32_t signal_id, uint32_t flags,
                                               int64_t strength_raw,
                                               const char* name, size_t name_len,
                                               const uint32_t* symbol_ids, size_t symbol_count,
                                               const uint8_t* payload, size_t payload_len)
{
  LRVX_CAPI_ENTER_VOID(handle);
  auto* rec = asRecorder(handle);
  if (rec == nullptr)
  {
    return;
  }
  lrvx::run::SignalView s;
  s.run_ts_ns = run_ts_ns;
  s.feed_ts_ns = feed_ts_ns;
  s.signal_id = signal_id;
  s.flags = flags;
  s.strength_raw = strength_raw;
  if (name && name_len > 0)
  {
    s.name = std::string_view(name, name_len);
  }
  if (symbol_ids && symbol_count > 0)
  {
    s.symbol_ids.assign(symbol_ids, symbol_ids + symbol_count);
  }
  if (payload && payload_len > 0)
  {
    s.payload = std::string_view(reinterpret_cast<const char*>(payload), payload_len);
  }
  rec->writeSignal(s);
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" void lrvx_run_recorder_write_order_event(LrvxRunRecorderHandle handle,
                                                    int64_t run_ts_ns, int64_t feed_ts_ns,
                                                    uint64_t order_id, uint64_t parent_signal_id,
                                                    int64_t price_raw, int64_t qty_raw,
                                                    uint32_t symbol_id, uint8_t event_kind,
                                                    uint8_t side, uint8_t order_type,
                                                    uint32_t flags,
                                                    const char* reason, size_t reason_len)
{
  LRVX_CAPI_ENTER_VOID(handle);
  auto* rec = asRecorder(handle);
  if (rec == nullptr)
  {
    return;
  }
  lrvx::run::OrderEventView e;
  e.run_ts_ns = run_ts_ns;
  e.feed_ts_ns = feed_ts_ns;
  e.order_id = order_id;
  e.parent_signal_id = parent_signal_id;
  e.price_raw = price_raw;
  e.qty_raw = qty_raw;
  e.symbol_id = symbol_id;
  e.event_kind = static_cast<lrvx::run::OrderEventKind>(event_kind);
  e.side = side;
  e.order_type = order_type;
  e.flags = flags;
  if (reason && reason_len > 0)
  {
    e.reason = std::string_view(reason, reason_len);
  }
  rec->writeOrderEvent(e);
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" void lrvx_run_recorder_write_fill(LrvxRunRecorderHandle handle,
                                             int64_t run_ts_ns, int64_t feed_ts_ns,
                                             uint64_t order_id, uint64_t fill_id,
                                             int64_t price_raw, int64_t qty_raw, int64_t fee_raw,
                                             uint32_t symbol_id, uint8_t side, uint8_t liquidity)
{
  LRVX_CAPI_ENTER_VOID(handle);
  auto* rec = asRecorder(handle);
  if (rec == nullptr)
  {
    return;
  }
  lrvx::run::FillView f;
  f.run_ts_ns = run_ts_ns;
  f.feed_ts_ns = feed_ts_ns;
  f.order_id = order_id;
  f.fill_id = fill_id;
  f.price_raw = price_raw;
  f.qty_raw = qty_raw;
  f.fee_raw = fee_raw;
  f.symbol_id = symbol_id;
  f.side = side;
  f.liquidity = static_cast<lrvx::run::FillLiquidity>(liquidity);
  rec->writeFill(f);
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" void lrvx_run_recorder_close(LrvxRunRecorderHandle handle)
{
  LRVX_CAPI_ENTER_VOID(handle);
  if (auto* rec = asRecorder(handle))
  {
    rec->close();
  }
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" LrvxRunReaderHandle lrvx_run_reader_open(const char* path)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  if (path == nullptr)
  {
    return nullptr;
  }
  try
  {
    auto state = std::make_unique<RunReaderState>();
    state->reader = std::make_unique<lrvx::run::TraceReader>(path);
    state->signals = state->reader->readAllSignals();
    state->orders = state->reader->readAllOrderEvents();
    state->fills = state->reader->readAllFills();
    return state.release();
  }
  catch (...)
  {
    return nullptr;
  }
  LRVX_CAPI_LEAVE;
}

extern "C" void lrvx_run_reader_close(LrvxRunReaderHandle handle)
{
  LRVX_CAPI_ENTER_VOID(handle);
  delete asReader(handle);
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" uint64_t lrvx_run_reader_strategy_id(LrvxRunReaderHandle handle, char* out, uint64_t max_bytes)
{
  LRVX_CAPI_ENTER(handle);
  auto* state = asReader(handle);
  if (state == nullptr)
  {
    return 0;
  }
  return copyStringTo(state->reader->manifest().strategy_id, out, max_bytes);
  LRVX_CAPI_LEAVE;
}

extern "C" uint64_t lrvx_run_reader_strategy_hash(LrvxRunReaderHandle handle, char* out, uint64_t max_bytes)
{
  LRVX_CAPI_ENTER(handle);
  auto* state = asReader(handle);
  if (state == nullptr)
  {
    return 0;
  }
  return copyStringTo(state->reader->manifest().strategy_hash, out, max_bytes);
  LRVX_CAPI_LEAVE;
}

extern "C" int64_t lrvx_run_reader_run_started_ns(LrvxRunReaderHandle handle)
{
  LRVX_CAPI_ENTER(handle);
  auto* state = asReader(handle);
  return state ? state->reader->manifest().run_started_ns : 0;
  LRVX_CAPI_LEAVE;
}

extern "C" int64_t lrvx_run_reader_run_ended_ns(LrvxRunReaderHandle handle)
{
  LRVX_CAPI_ENTER(handle);
  auto* state = asReader(handle);
  return state ? state->reader->manifest().run_ended_ns : 0;
  LRVX_CAPI_LEAVE;
}

extern "C" uint64_t lrvx_run_reader_tape_ref_count(LrvxRunReaderHandle handle)
{
  LRVX_CAPI_ENTER(handle);
  auto* state = asReader(handle);
  return state ? state->reader->manifest().tape_refs.size() : 0;
  LRVX_CAPI_LEAVE;
}

extern "C" uint64_t lrvx_run_reader_tape_ref_path(LrvxRunReaderHandle handle, uint64_t index, char* out, uint64_t max_bytes)
{
  LRVX_CAPI_ENTER(handle);
  auto* state = asReader(handle);
  if (state == nullptr)
  {
    return 0;
  }
  const auto& refs = state->reader->manifest().tape_refs;
  if (index >= refs.size())
  {
    return 0;
  }
  return copyStringTo(refs[index].path, out, max_bytes);
  LRVX_CAPI_LEAVE;
}

extern "C" uint64_t lrvx_run_reader_signal_count(LrvxRunReaderHandle handle)
{
  LRVX_CAPI_ENTER(handle);
  auto* state = asReader(handle);
  return state ? state->signals.size() : 0;
  LRVX_CAPI_LEAVE;
}

extern "C" uint64_t lrvx_run_reader_order_event_count(LrvxRunReaderHandle handle)
{
  LRVX_CAPI_ENTER(handle);
  auto* state = asReader(handle);
  return state ? state->orders.size() : 0;
  LRVX_CAPI_LEAVE;
}

extern "C" uint64_t lrvx_run_reader_fill_count(LrvxRunReaderHandle handle)
{
  LRVX_CAPI_ENTER(handle);
  auto* state = asReader(handle);
  return state ? state->fills.size() : 0;
  LRVX_CAPI_LEAVE;
}

extern "C" void lrvx_run_reader_signal_header(LrvxRunReaderHandle handle, uint64_t index,
                                              int64_t* out_run_ts, int64_t* out_feed_ts,
                                              uint32_t* out_signal_id, uint32_t* out_flags,
                                              int64_t* out_strength_raw,
                                              uint64_t* out_name_len, uint64_t* out_symbol_count,
                                              uint64_t* out_payload_len)
{
  LRVX_CAPI_ENTER_VOID(handle);
  auto* state = asReader(handle);
  if (state == nullptr || index >= state->signals.size())
  {
    return;
  }
  const auto& s = state->signals[index];
  if (out_run_ts)
  {
    *out_run_ts = s.run_ts_ns;
  }
  if (out_feed_ts)
  {
    *out_feed_ts = s.feed_ts_ns;
  }
  if (out_signal_id)
  {
    *out_signal_id = s.signal_id;
  }
  if (out_flags)
  {
    *out_flags = s.flags;
  }
  if (out_strength_raw)
  {
    *out_strength_raw = s.strength_raw;
  }
  if (out_name_len)
  {
    *out_name_len = s.name.size();
  }
  if (out_symbol_count)
  {
    *out_symbol_count = s.symbol_ids.size();
  }
  if (out_payload_len)
  {
    *out_payload_len = s.payload.size();
  }
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" uint64_t lrvx_run_reader_signal_name(LrvxRunReaderHandle handle, uint64_t index, char* out, uint64_t max_bytes)
{
  LRVX_CAPI_ENTER(handle);
  auto* state = asReader(handle);
  if (state == nullptr || index >= state->signals.size())
  {
    return 0;
  }
  return copyStringTo(state->signals[index].name, out, max_bytes);
  LRVX_CAPI_LEAVE;
}

extern "C" uint64_t lrvx_run_reader_signal_symbol_ids(LrvxRunReaderHandle handle, uint64_t index, uint32_t* out, uint64_t max_entries)
{
  LRVX_CAPI_ENTER(handle);
  auto* state = asReader(handle);
  if (state == nullptr || index >= state->signals.size())
  {
    return 0;
  }
  const auto& ids = state->signals[index].symbol_ids;
  if (out == nullptr || max_entries == 0)
  {
    return ids.size();
  }
  uint64_t n = std::min(static_cast<uint64_t>(ids.size()), max_entries);
  for (uint64_t i = 0; i < n; ++i)
  {
    out[i] = ids[i];
  }
  return n;
  LRVX_CAPI_LEAVE;
}

extern "C" uint64_t lrvx_run_reader_signal_payload(LrvxRunReaderHandle handle, uint64_t index, uint8_t* out, uint64_t max_bytes)
{
  LRVX_CAPI_ENTER(handle);
  auto* state = asReader(handle);
  if (state == nullptr || index >= state->signals.size())
  {
    return 0;
  }
  const auto& p = state->signals[index].payload;
  if (out == nullptr || max_bytes == 0)
  {
    return p.size();
  }
  uint64_t n = std::min(static_cast<uint64_t>(p.size()), max_bytes);
  std::memcpy(out, p.data(), n);
  return n;
  LRVX_CAPI_LEAVE;
}

extern "C" void lrvx_run_reader_order_event_header(LrvxRunReaderHandle handle, uint64_t index,
                                                   int64_t* out_run_ts, int64_t* out_feed_ts,
                                                   uint64_t* out_order_id, uint64_t* out_parent_signal_id,
                                                   int64_t* out_price_raw, int64_t* out_qty_raw,
                                                   uint32_t* out_symbol_id, uint8_t* out_event_kind,
                                                   uint8_t* out_side, uint8_t* out_order_type,
                                                   uint32_t* out_flags, uint64_t* out_reason_len)
{
  LRVX_CAPI_ENTER_VOID(handle);
  auto* state = asReader(handle);
  if (state == nullptr || index >= state->orders.size())
  {
    return;
  }
  const auto& e = state->orders[index];
  if (out_run_ts)
  {
    *out_run_ts = e.run_ts_ns;
  }
  if (out_feed_ts)
  {
    *out_feed_ts = e.feed_ts_ns;
  }
  if (out_order_id)
  {
    *out_order_id = e.order_id;
  }
  if (out_parent_signal_id)
  {
    *out_parent_signal_id = e.parent_signal_id;
  }
  if (out_price_raw)
  {
    *out_price_raw = e.price_raw;
  }
  if (out_qty_raw)
  {
    *out_qty_raw = e.qty_raw;
  }
  if (out_symbol_id)
  {
    *out_symbol_id = e.symbol_id;
  }
  if (out_event_kind)
  {
    *out_event_kind = static_cast<uint8_t>(e.event_kind);
  }
  if (out_side)
  {
    *out_side = e.side;
  }
  if (out_order_type)
  {
    *out_order_type = e.order_type;
  }
  if (out_flags)
  {
    *out_flags = e.flags;
  }
  if (out_reason_len)
  {
    *out_reason_len = e.reason.size();
  }
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" uint64_t lrvx_run_reader_order_event_reason(LrvxRunReaderHandle handle, uint64_t index, char* out, uint64_t max_bytes)
{
  LRVX_CAPI_ENTER(handle);
  auto* state = asReader(handle);
  if (state == nullptr || index >= state->orders.size())
  {
    return 0;
  }
  return copyStringTo(state->orders[index].reason, out, max_bytes);
  LRVX_CAPI_LEAVE;
}

extern "C" void lrvx_run_reader_fill(LrvxRunReaderHandle handle, uint64_t index,
                                     int64_t* out_run_ts, int64_t* out_feed_ts,
                                     uint64_t* out_order_id, uint64_t* out_fill_id,
                                     int64_t* out_price_raw, int64_t* out_qty_raw, int64_t* out_fee_raw,
                                     uint32_t* out_symbol_id, uint8_t* out_side, uint8_t* out_liquidity)
{
  LRVX_CAPI_ENTER_VOID(handle);
  auto* state = asReader(handle);
  if (state == nullptr || index >= state->fills.size())
  {
    return;
  }
  const auto& f = state->fills[index];
  if (out_run_ts)
  {
    *out_run_ts = f.run_ts_ns;
  }
  if (out_feed_ts)
  {
    *out_feed_ts = f.feed_ts_ns;
  }
  if (out_order_id)
  {
    *out_order_id = f.order_id;
  }
  if (out_fill_id)
  {
    *out_fill_id = f.fill_id;
  }
  if (out_price_raw)
  {
    *out_price_raw = f.price_raw;
  }
  if (out_qty_raw)
  {
    *out_qty_raw = f.qty_raw;
  }
  if (out_fee_raw)
  {
    *out_fee_raw = f.fee_raw;
  }
  if (out_symbol_id)
  {
    *out_symbol_id = f.symbol_id;
  }
  if (out_side)
  {
    *out_side = f.side;
  }
  if (out_liquidity)
  {
    *out_liquidity = f.liquidity;
  }
  LRVX_CAPI_LEAVE_VOID;
}

namespace
{
inline lrvx::testing::BarDispatchRecorder* toBarDispatchRecorder(LrvxBarDispatchRecorderHandle h)
{
  return static_cast<lrvx::testing::BarDispatchRecorder*>(h);
}
}  // namespace

extern "C" LrvxBarDispatchRecorderHandle lrvx_bar_dispatch_recorder_create(void)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  return new lrvx::testing::BarDispatchRecorder();
  LRVX_CAPI_LEAVE;
}

extern "C" void lrvx_bar_dispatch_recorder_destroy(LrvxBarDispatchRecorderHandle h)
{
  LRVX_CAPI_ENTER_DESTROY(h);
  delete toBarDispatchRecorder(h);
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" uint32_t lrvx_bar_dispatch_recorder_add_time_seconds(LrvxBarDispatchRecorderHandle h,
                                                                uint32_t seconds)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<uint32_t>(toBarDispatchRecorder(h)->addTimeIntervalSeconds(seconds));
  LRVX_CAPI_LEAVE;
}

extern "C" void lrvx_bar_dispatch_recorder_on_trade(LrvxBarDispatchRecorderHandle h,
                                                    uint32_t symbol, double price, double qty,
                                                    int64_t ts_ns)
{
  LRVX_CAPI_ENTER_VOID(h);
  toBarDispatchRecorder(h)->onTrade(symbol, price, qty, ts_ns);
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" void lrvx_bar_dispatch_recorder_finalize(LrvxBarDispatchRecorderHandle h)
{
  LRVX_CAPI_ENTER_VOID(h);
  toBarDispatchRecorder(h)->finalize();
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" uint32_t lrvx_bar_dispatch_recorder_count(LrvxBarDispatchRecorderHandle h)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<uint32_t>(toBarDispatchRecorder(h)->count());
  LRVX_CAPI_LEAVE;
}

extern "C" uint8_t lrvx_bar_dispatch_recorder_type_at(LrvxBarDispatchRecorderHandle h,
                                                      uint32_t index)
{
  LRVX_CAPI_ENTER(h);
  return toBarDispatchRecorder(h)->typeAt(index);
  LRVX_CAPI_LEAVE;
}

extern "C" uint64_t lrvx_bar_dispatch_recorder_param_at(LrvxBarDispatchRecorderHandle h,
                                                        uint32_t index)
{
  LRVX_CAPI_ENTER(h);
  return toBarDispatchRecorder(h)->paramAt(index);
  LRVX_CAPI_LEAVE;
}

// ============================================================
// Streaming tape aggregators
// ============================================================

namespace
{

// Tagged holder. The C ABI exposes a single LrvxAggregatorHandle
// (void*), but each result accessor needs to know the concrete type
// to do a typed static_cast. The `kind` tag is checked on every
// `*_read_result` entry; mismatched kind → return 0 rows so a buggy
// binding can't read garbage past the end of a smaller Row struct.
struct AggregatorHolder
{
  enum Kind : uint8_t
  {
    KIND_EVENT_TYPE_STATS,
    KIND_BIN_COUNT,
    KIND_VOLUME_BIN,
    KIND_OHLC_BIN,
    KIND_PEAK,
    KIND_QUANTILE,
    KIND_BOOK_SNAPSHOT_BIN,
  };
  Kind kind;
  std::unique_ptr<replay::IAggregator> impl;

  template <typename T>
  T* as(Kind expected)
  {
    if (kind != expected)
    {
      return nullptr;
    }
    return static_cast<T*>(impl.get());
  }
};

inline AggregatorHolder* toAgg(LrvxAggregatorHandle h)
{
  return static_cast<AggregatorHolder*>(h);
}

inline replay::AggregatorEventFilter toAggFilter(LrvxAggregatorEventFilter f)
{
  switch (f)
  {
    case LRVX_AGG_FILTER_TRADES:
      return replay::AggregatorEventFilter::Trades;
    case LRVX_AGG_FILTER_BOOKS_ONLY:
      return replay::AggregatorEventFilter::BooksOnly;
    case LRVX_AGG_FILTER_BOTH:
    default:
      return replay::AggregatorEventFilter::Both;
  }
}

inline std::vector<uint32_t> copySymbolFilter(const uint32_t* sf, uint32_t count)
{
  if (sf == nullptr || count == 0)
  {
    return {};
  }
  return std::vector<uint32_t>(sf, sf + count);
}

}  // namespace

extern "C" LrvxAggregatorHandle lrvx_event_type_stats_aggregator_create(
    LrvxAggregatorEventFilter event_filter, const uint32_t* symbol_filter,
    uint32_t symbol_filter_count)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  auto* holder = new AggregatorHolder{
      AggregatorHolder::KIND_EVENT_TYPE_STATS,
      std::make_unique<replay::EventTypeStatsAggregator>(
          toAggFilter(event_filter),
          copySymbolFilter(symbol_filter, symbol_filter_count))};
  return holder;
  LRVX_CAPI_LEAVE;
}

extern "C" LrvxAggregatorHandle lrvx_bin_count_aggregator_create(
    int64_t bucket_ns, uint8_t by_side, uint8_t by_symbol,
    LrvxAggregatorEventFilter event_filter, const uint32_t* symbol_filter,
    uint32_t symbol_filter_count)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  auto* holder = new AggregatorHolder{
      AggregatorHolder::KIND_BIN_COUNT,
      std::make_unique<replay::BinCountAggregator>(
          bucket_ns, by_side != 0, by_symbol != 0, toAggFilter(event_filter),
          copySymbolFilter(symbol_filter, symbol_filter_count))};
  return holder;
  LRVX_CAPI_LEAVE;
}

extern "C" LrvxAggregatorHandle lrvx_volume_bin_aggregator_create(
    int64_t bucket_ns, uint8_t by_side, uint8_t by_symbol,
    LrvxAggregatorEventFilter event_filter, const uint32_t* symbol_filter,
    uint32_t symbol_filter_count)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  auto* holder = new AggregatorHolder{
      AggregatorHolder::KIND_VOLUME_BIN,
      std::make_unique<replay::VolumeBinAggregator>(
          bucket_ns, by_side != 0, by_symbol != 0, toAggFilter(event_filter),
          copySymbolFilter(symbol_filter, symbol_filter_count))};
  return holder;
  LRVX_CAPI_LEAVE;
}

extern "C" LrvxAggregatorHandle lrvx_ohlc_bin_aggregator_create(
    int64_t bucket_ns, uint8_t by_symbol,
    LrvxAggregatorEventFilter event_filter, const uint32_t* symbol_filter,
    uint32_t symbol_filter_count)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  auto* holder = new AggregatorHolder{
      AggregatorHolder::KIND_OHLC_BIN,
      std::make_unique<replay::OHLCBinAggregator>(
          bucket_ns, by_symbol != 0, toAggFilter(event_filter),
          copySymbolFilter(symbol_filter, symbol_filter_count))};
  return holder;
  LRVX_CAPI_LEAVE;
}

extern "C" LrvxAggregatorHandle lrvx_peak_aggregator_create(
    const int64_t* window_ns_list, uint32_t window_count, uint32_t top_n,
    uint32_t oversample_factor, LrvxAggregatorEventFilter event_filter,
    const uint32_t* symbol_filter, uint32_t symbol_filter_count)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  std::vector<int64_t> windows;
  if (window_ns_list != nullptr && window_count > 0)
  {
    windows.assign(window_ns_list, window_ns_list + window_count);
  }
  // 0 → engine default (100). PeakAggregator's ctor maxes the factor
  // at 1 internally so a 0 here would collapse the heap to top_n
  // candidates only — pass 100 to preserve the spec'd default.
  const std::size_t oversample =
      oversample_factor == 0 ? std::size_t{100} : std::size_t{oversample_factor};
  auto* holder = new AggregatorHolder{
      AggregatorHolder::KIND_PEAK,
      std::make_unique<replay::PeakAggregator>(
          std::move(windows), top_n, oversample, toAggFilter(event_filter),
          copySymbolFilter(symbol_filter, symbol_filter_count))};
  return holder;
  LRVX_CAPI_LEAVE;
}

extern "C" LrvxAggregatorHandle lrvx_quantile_aggregator_create(
    const int64_t* window_ns_list, uint32_t window_count,
    const double* quantiles, uint32_t quantile_count,
    LrvxAggregatorEventFilter event_filter, const uint32_t* symbol_filter,
    uint32_t symbol_filter_count)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  std::vector<int64_t> windows;
  if (window_ns_list != nullptr && window_count > 0)
  {
    windows.assign(window_ns_list, window_ns_list + window_count);
  }
  std::vector<double> qs;
  if (quantiles != nullptr && quantile_count > 0)
  {
    qs.assign(quantiles, quantiles + quantile_count);
  }
  auto* holder = new AggregatorHolder{
      AggregatorHolder::KIND_QUANTILE,
      std::make_unique<replay::QuantileAggregator>(
          std::move(windows), std::move(qs), toAggFilter(event_filter),
          copySymbolFilter(symbol_filter, symbol_filter_count))};
  return holder;
  LRVX_CAPI_LEAVE;
}

extern "C" LrvxAggregatorHandle lrvx_book_snapshot_bin_aggregator_create(
    int64_t bucket_ns, uint16_t levels,
    LrvxAggregatorEventFilter event_filter, const uint32_t* symbol_filter,
    uint32_t symbol_filter_count)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  auto* holder = new AggregatorHolder{
      AggregatorHolder::KIND_BOOK_SNAPSHOT_BIN,
      std::make_unique<replay::BookSnapshotBinAggregator>(
          bucket_ns, levels, toAggFilter(event_filter),
          copySymbolFilter(symbol_filter, symbol_filter_count))};
  return holder;
  LRVX_CAPI_LEAVE;
}

extern "C" void lrvx_aggregator_destroy(LrvxAggregatorHandle h)
{
  LRVX_CAPI_ENTER_DESTROY(h);
  delete toAgg(h);
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" void lrvx_data_reader_set_progress_callback(
    LrvxDataReaderHandle reader, LrvxProgressCallback cb, void* user_data,
    uint32_t interval_ms)
{
  LRVX_CAPI_ENTER_VOID(reader);
  if (reader == nullptr)
  {
    return;
  }
  auto* r = static_cast<replay::BinaryLogReader*>(reader);
  if (cb == nullptr)
  {
    r->clearProgressCallback();
    return;
  }
  std::chrono::milliseconds interval{interval_ms == 0 ? 1000u : interval_ms};
  r->setProgressCallback(
      [cb, user_data](double pct, int64_t cursor_ts_ns) -> bool
      { return cb(user_data, pct, cursor_ts_ns) != 0; },
      interval);
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" void lrvx_data_reader_clear_progress_callback(
    LrvxDataReaderHandle reader)
{
  LRVX_CAPI_ENTER_VOID(reader);
  if (reader == nullptr)
  {
    return;
  }
  static_cast<replay::BinaryLogReader*>(reader)->clearProgressCallback();
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" uint8_t lrvx_data_reader_run(LrvxDataReaderHandle reader,
                                        LrvxAggregatorHandle* aggregators,
                                        uint32_t aggregator_count,
                                        uint32_t n_threads)
{
  LRVX_CAPI_ENTER(reader);
  if (reader == nullptr)
  {
    return 0;
  }
  std::vector<replay::IAggregator*> raw;
  if (aggregators != nullptr && aggregator_count > 0)
  {
    raw.reserve(aggregator_count);
    for (uint32_t i = 0; i < aggregator_count; ++i)
    {
      auto* holder = toAgg(aggregators[i]);
      raw.push_back(holder != nullptr ? holder->impl.get() : nullptr);
    }
  }
  auto* r = static_cast<replay::BinaryLogReader*>(reader);
  const std::size_t nt = n_threads == 0 ? std::size_t{1} : std::size_t{n_threads};
  // Aggregators may refuse a configuration by throwing (e.g. the book
  // snapshot aggregator refuses parallel runs from cloneEmpty). A C++
  // exception must not cross the C ABI — map it to the failure return.
  try
  {
    return r->run(raw, nt) ? 1 : 0;
  }
  catch (const std::exception&)
  {
    return 0;
  }
  LRVX_CAPI_LEAVE;
}

extern "C" uint8_t lrvx_merged_tape_reader_run(LrvxMergedTapeReaderHandle reader,
                                               LrvxAggregatorHandle* aggregators,
                                               uint32_t aggregator_count,
                                               uint32_t /*n_threads*/)
{
  LRVX_CAPI_ENTER(reader);
  // n_threads reserved for future; MergedTapeReader::run is single-
  // threaded for now (per-instance symbol rekey would not align
  // across worker partitions).
  if (reader == nullptr)
  {
    return 0;
  }
  std::vector<replay::IAggregator*> raw;
  if (aggregators != nullptr && aggregator_count > 0)
  {
    raw.reserve(aggregator_count);
    for (uint32_t i = 0; i < aggregator_count; ++i)
    {
      auto* holder = toAgg(aggregators[i]);
      raw.push_back(holder != nullptr ? holder->impl.get() : nullptr);
    }
  }
  auto* impl = static_cast<capi_impl::LrvxMergedTapeReaderImpl*>(reader);
  if (!impl || !impl->reader)
  {
    return 0;
  }
  return impl->reader->run(raw) ? 1 : 0;
  LRVX_CAPI_LEAVE;
}

extern "C" uint32_t lrvx_event_type_stats_read_result(LrvxAggregatorHandle h,
                                                      LrvxEventTypeStatsRow* rows_out,
                                                      uint32_t max_rows)
{
  LRVX_CAPI_ENTER(h);
  auto* holder = toAgg(h);
  if (holder == nullptr)
  {
    return 0;
  }
  auto* impl = holder->as<replay::EventTypeStatsAggregator>(
      AggregatorHolder::KIND_EVENT_TYPE_STATS);
  if (impl == nullptr)
  {
    return 0;
  }
  const auto& rows = impl->result();
  if (rows_out == nullptr || max_rows == 0)
  {
    return static_cast<uint32_t>(rows.size());
  }
  const uint32_t n = static_cast<uint32_t>(std::min<std::size_t>(rows.size(), max_rows));
  for (uint32_t i = 0; i < n; ++i)
  {
    rows_out[i].symbol_id = rows[i].symbol_id;
    rows_out[i].trades = rows[i].trades;
    rows_out[i].book_snapshots = rows[i].book_snapshots;
    rows_out[i].book_deltas = rows[i].book_deltas;
  }
  return n;
  LRVX_CAPI_LEAVE;
}

extern "C" uint32_t lrvx_bin_count_read_result(LrvxAggregatorHandle h,
                                               LrvxBinCountRow* rows_out,
                                               uint32_t max_rows)
{
  LRVX_CAPI_ENTER(h);
  auto* holder = toAgg(h);
  if (holder == nullptr)
  {
    return 0;
  }
  auto* impl = holder->as<replay::BinCountAggregator>(AggregatorHolder::KIND_BIN_COUNT);
  if (impl == nullptr)
  {
    return 0;
  }
  const auto& rows = impl->result();
  if (rows_out == nullptr || max_rows == 0)
  {
    return static_cast<uint32_t>(rows.size());
  }
  const uint32_t n = static_cast<uint32_t>(std::min<std::size_t>(rows.size(), max_rows));
  for (uint32_t i = 0; i < n; ++i)
  {
    rows_out[i].bucket_ts_ns = rows[i].bucket_ts_ns;
    rows_out[i].symbol_id = rows[i].symbol_id;
    rows_out[i].side = rows[i].side;
    rows_out[i].count = rows[i].count;
  }
  return n;
  LRVX_CAPI_LEAVE;
}

extern "C" uint32_t lrvx_volume_bin_read_result(LrvxAggregatorHandle h,
                                                LrvxVolumeBinRow* rows_out,
                                                uint32_t max_rows)
{
  LRVX_CAPI_ENTER(h);
  auto* holder = toAgg(h);
  if (holder == nullptr)
  {
    return 0;
  }
  auto* impl = holder->as<replay::VolumeBinAggregator>(AggregatorHolder::KIND_VOLUME_BIN);
  if (impl == nullptr)
  {
    return 0;
  }
  const auto& rows = impl->result();
  if (rows_out == nullptr || max_rows == 0)
  {
    return static_cast<uint32_t>(rows.size());
  }
  const uint32_t n = static_cast<uint32_t>(std::min<std::size_t>(rows.size(), max_rows));
  for (uint32_t i = 0; i < n; ++i)
  {
    rows_out[i].bucket_ts_ns = rows[i].bucket_ts_ns;
    rows_out[i].symbol_id = rows[i].symbol_id;
    rows_out[i].side = rows[i].side;
    rows_out[i].qty_raw = rows[i].qty_raw;
  }
  return n;
  LRVX_CAPI_LEAVE;
}

extern "C" uint32_t lrvx_ohlc_bin_read_result(LrvxAggregatorHandle h,
                                              LrvxOHLCBinRow* rows_out,
                                              uint32_t max_rows)
{
  LRVX_CAPI_ENTER(h);
  auto* holder = toAgg(h);
  if (holder == nullptr)
  {
    return 0;
  }
  auto* impl = holder->as<replay::OHLCBinAggregator>(AggregatorHolder::KIND_OHLC_BIN);
  if (impl == nullptr)
  {
    return 0;
  }
  const auto& rows = impl->result();
  if (rows_out == nullptr || max_rows == 0)
  {
    return static_cast<uint32_t>(rows.size());
  }
  const uint32_t n = static_cast<uint32_t>(std::min<std::size_t>(rows.size(), max_rows));
  for (uint32_t i = 0; i < n; ++i)
  {
    rows_out[i].bucket_ts_ns = rows[i].bucket_ts_ns;
    rows_out[i].symbol_id = rows[i].symbol_id;
    rows_out[i].open_raw = rows[i].open_raw;
    rows_out[i].high_raw = rows[i].high_raw;
    rows_out[i].low_raw = rows[i].low_raw;
    rows_out[i].close_raw = rows[i].close_raw;
  }
  return n;
  LRVX_CAPI_LEAVE;
}

extern "C" uint32_t lrvx_peak_read_result(LrvxAggregatorHandle h,
                                          LrvxPeakRow* rows_out, uint32_t max_rows)
{
  LRVX_CAPI_ENTER(h);
  auto* holder = toAgg(h);
  if (holder == nullptr)
  {
    return 0;
  }
  auto* impl = holder->as<replay::PeakAggregator>(AggregatorHolder::KIND_PEAK);
  if (impl == nullptr)
  {
    return 0;
  }
  const auto& rows = impl->result();
  if (rows_out == nullptr || max_rows == 0)
  {
    return static_cast<uint32_t>(rows.size());
  }
  const uint32_t n = static_cast<uint32_t>(std::min<std::size_t>(rows.size(), max_rows));
  for (uint32_t i = 0; i < n; ++i)
  {
    rows_out[i].window_ns = rows[i].window_ns;
    rows_out[i].count = rows[i].count;
    rows_out[i].start_ns = rows[i].start_ns;
  }
  return n;
  LRVX_CAPI_LEAVE;
}

extern "C" uint32_t lrvx_quantile_read_result(LrvxAggregatorHandle h,
                                              LrvxQuantileRow* rows_out,
                                              uint32_t max_rows)
{
  LRVX_CAPI_ENTER(h);
  auto* holder = toAgg(h);
  if (holder == nullptr)
  {
    return 0;
  }
  auto* impl = holder->as<replay::QuantileAggregator>(AggregatorHolder::KIND_QUANTILE);
  if (impl == nullptr)
  {
    return 0;
  }
  const auto& rows = impl->result();
  if (rows_out == nullptr || max_rows == 0)
  {
    return static_cast<uint32_t>(rows.size());
  }
  const uint32_t n = static_cast<uint32_t>(std::min<std::size_t>(rows.size(), max_rows));
  for (uint32_t i = 0; i < n; ++i)
  {
    rows_out[i].window_ns = rows[i].window_ns;
    rows_out[i].quantile = rows[i].quantile;
    rows_out[i].count = rows[i].count;
  }
  return n;
  LRVX_CAPI_LEAVE;
}

extern "C" uint32_t lrvx_book_snapshot_bin_read_result(
    LrvxAggregatorHandle h, LrvxBookSnapshotBinRow* rows_out, uint32_t max_rows)
{
  LRVX_CAPI_ENTER(h);
  auto* holder = toAgg(h);
  if (holder == nullptr)
  {
    return 0;
  }
  auto* impl = holder->as<replay::BookSnapshotBinAggregator>(
      AggregatorHolder::KIND_BOOK_SNAPSHOT_BIN);
  if (impl == nullptr)
  {
    return 0;
  }
  const auto& rows = impl->result();
  if (rows_out == nullptr || max_rows == 0)
  {
    return static_cast<uint32_t>(rows.size());
  }
  const uint32_t n = static_cast<uint32_t>(std::min<std::size_t>(rows.size(), max_rows));
  for (uint32_t i = 0; i < n; ++i)
  {
    rows_out[i].bucket_ts_ns = rows[i].bucket_ts_ns;
    rows_out[i].symbol_id = rows[i].symbol_id;
    rows_out[i].level = rows[i].level;
    rows_out[i].flags = rows[i].flags;
    rows_out[i].bid_price_raw = rows[i].bid_price_raw;
    rows_out[i].bid_qty_raw = rows[i].bid_qty_raw;
    rows_out[i].ask_price_raw = rows[i].ask_price_raw;
    rows_out[i].ask_qty_raw = rows[i].ask_qty_raw;
  }
  return n;
  LRVX_CAPI_LEAVE;
}

// ============================================================
// Live queue position estimator
// ============================================================

#include "lrvx/backtest/account.h"
#include "lrvx/backtest/fee_schedule.h"
#include "lrvx/backtest/funding_schedule.h"
#include "lrvx/backtest/liquidation_engine.h"
#include "lrvx/execution/live_queue_position_estimator.h"

namespace
{
inline lrvx::LiveQueuePositionEstimator* toLiveQ(LrvxLiveQueuePositionHandle h)
{
  return static_cast<lrvx::LiveQueuePositionEstimator*>(h);
}
inline lrvx::FundingSchedule* toFunding(LrvxFundingScheduleHandle h)
{
  return static_cast<lrvx::FundingSchedule*>(h);
}
inline lrvx::FeeSchedule* toFee(LrvxFeeScheduleHandle h)
{
  return static_cast<lrvx::FeeSchedule*>(h);
}
inline lrvx::Account* toAccount(LrvxAccountHandle h)
{
  return static_cast<lrvx::Account*>(h);
}
}  // namespace

extern "C" LrvxFeeScheduleHandle lrvx_fee_schedule_create(void)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  return new lrvx::FeeSchedule();
  LRVX_CAPI_LEAVE;
}
extern "C" void lrvx_fee_schedule_destroy(LrvxFeeScheduleHandle h)
{
  LRVX_CAPI_ENTER_DESTROY(h);
  delete toFee(h);
  LRVX_CAPI_LEAVE_VOID;
}
extern "C" void lrvx_fee_schedule_add_tier(LrvxFeeScheduleHandle h,
                                           double min_notional_30d, double maker_bps,
                                           double taker_bps)
{
  LRVX_CAPI_ENTER_VOID(h);
  toFee(h)->addTier(min_notional_30d, maker_bps, taker_bps);
  LRVX_CAPI_LEAVE_VOID;
}
extern "C" int lrvx_fee_schedule_load_profile(LrvxFeeScheduleHandle h, const char* name)
{
  LRVX_CAPI_ENTER(h);
  if (!h || !name)
  {
    return 0;
  }
  std::string n = name;
  if (n == "binance_um_futures")
  {
    *toFee(h) = lrvx::FeeSchedule::binance_um_futures();
  }
  else if (n == "bybit_linear")
  {
    *toFee(h) = lrvx::FeeSchedule::bybit_linear();
  }
  else if (n == "okx_swap")
  {
    *toFee(h) = lrvx::FeeSchedule::okx_swap();
  }
  else if (n == "deribit")
  {
    *toFee(h) = lrvx::FeeSchedule::deribit();
  }
  else
  {
    return 0;
  }
  return 1;
  LRVX_CAPI_LEAVE;
}
extern "C" void lrvx_fee_schedule_record_fill(LrvxFeeScheduleHandle h, int64_t ts_ns,
                                              double notional)
{
  LRVX_CAPI_ENTER_VOID(h);
  toFee(h)->recordFill(ts_ns, notional);
  LRVX_CAPI_LEAVE_VOID;
}
extern "C" double lrvx_fee_schedule_fee_for(LrvxFeeScheduleHandle h, int64_t ts_ns,
                                            double notional, uint8_t is_maker)
{
  LRVX_CAPI_ENTER(h);
  return toFee(h)->feeFor(ts_ns, notional, is_maker != 0);
  LRVX_CAPI_LEAVE;
}
extern "C" uint32_t lrvx_fee_schedule_current_tier(LrvxFeeScheduleHandle h)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<uint32_t>(toFee(h)->currentTierIndex());
  LRVX_CAPI_LEAVE;
}
extern "C" double lrvx_fee_schedule_rolling_notional(LrvxFeeScheduleHandle h)
{
  LRVX_CAPI_ENTER(h);
  return toFee(h)->rollingNotional30d();
  LRVX_CAPI_LEAVE;
}
extern "C" uint32_t lrvx_fee_schedule_tier_transitions(LrvxFeeScheduleHandle h,
                                                       int64_t* out_buf,
                                                       uint32_t max_events)
{
  LRVX_CAPI_ENTER(h);
  const auto& v = toFee(h)->tierTransitionTsNs();
  if (out_buf == nullptr || max_events == 0)
  {
    return static_cast<uint32_t>(v.size());
  }
  uint32_t n = std::min<uint32_t>(max_events, static_cast<uint32_t>(v.size()));
  for (uint32_t i = 0; i < n; ++i)
  {
    out_buf[i] = v[i];
  }
  return n;
  LRVX_CAPI_LEAVE;
}
extern "C" void lrvx_fee_schedule_reset_rolling(LrvxFeeScheduleHandle h)
{
  LRVX_CAPI_ENTER_VOID(h);
  toFee(h)->resetRolling();
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" LrvxFundingScheduleHandle lrvx_funding_schedule_create(void)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  return new lrvx::FundingSchedule();
  LRVX_CAPI_LEAVE;
}
extern "C" void lrvx_funding_schedule_destroy(LrvxFundingScheduleHandle h)
{
  LRVX_CAPI_ENTER_DESTROY(h);
  delete toFunding(h);
  LRVX_CAPI_LEAVE_VOID;
}
extern "C" void lrvx_funding_schedule_set_constant(LrvxFundingScheduleHandle h,
                                                   int64_t interval_ns, double rate)
{
  LRVX_CAPI_ENTER_VOID(h);
  *toFunding(h) = lrvx::FundingSchedule::constant(interval_ns, rate);
  LRVX_CAPI_LEAVE_VOID;
}
extern "C" void lrvx_funding_schedule_set_tape(LrvxFundingScheduleHandle h,
                                               const int64_t* timestamps_ns,
                                               const double* rates, uint32_t n)
{
  LRVX_CAPI_ENTER_VOID(h);
  std::vector<std::pair<int64_t, double>> events;
  events.reserve(n);
  for (uint32_t i = 0; i < n; ++i)
  {
    events.emplace_back(timestamps_ns ? timestamps_ns[i] : 0, rates ? rates[i] : 0.0);
  }
  *toFunding(h) = lrvx::FundingSchedule::tape(std::move(events));
  LRVX_CAPI_LEAVE_VOID;
}
extern "C" void lrvx_funding_schedule_set_tape_by_symbol(
    LrvxFundingScheduleHandle h, const int64_t* timestamps_ns,
    const uint32_t* symbols, const double* rates, uint32_t n)
{
  LRVX_CAPI_ENTER_VOID(h);
  std::vector<lrvx::FundingTapeEntry> entries;
  entries.reserve(n);
  for (uint32_t i = 0; i < n; ++i)
  {
    lrvx::FundingTapeEntry e;
    e.timestampNs = UnixNanos::fromRaw(timestamps_ns ? timestamps_ns[i] : 0);
    e.symbol = symbols ? static_cast<lrvx::SymbolId>(symbols[i])
                       : lrvx::FundingTapeEntry::kAnySymbol;
    e.rate = rates ? rates[i] : 0.0;
    entries.push_back(e);
  }
  *toFunding(h) = lrvx::FundingSchedule::tapeBySymbol(std::move(entries));
  LRVX_CAPI_LEAVE_VOID;
}
extern "C" uint8_t lrvx_funding_schedule_load_tape(LrvxFundingScheduleHandle h,
                                                   const char* path)
{
  LRVX_CAPI_ENTER(h);
  if (!h || !path)
  {
    return 0;
  }
  return toFunding(h)->loadTape(std::string(path)) ? 1 : 0;
  LRVX_CAPI_LEAVE;
}
extern "C" int lrvx_funding_schedule_load_profile(LrvxFundingScheduleHandle h,
                                                  const char* name)
{
  LRVX_CAPI_ENTER(h);
  if (!h || !name)
  {
    return 0;
  }
  std::string n = name;
  if (n == "binance_um_futures")
  {
    *toFunding(h) = lrvx::FundingSchedule::binance_um_futures();
  }
  else if (n == "bybit_linear")
  {
    *toFunding(h) = lrvx::FundingSchedule::bybit_linear();
  }
  else if (n == "okx_swap")
  {
    *toFunding(h) = lrvx::FundingSchedule::okx_swap();
  }
  else if (n == "bitget_hourly")
  {
    *toFunding(h) = lrvx::FundingSchedule::bitget_hourly();
  }
  else
  {
    return 0;
  }
  return 1;
  LRVX_CAPI_LEAVE;
}
extern "C" void lrvx_funding_schedule_set_constant_rate(LrvxFundingScheduleHandle h,
                                                        double rate)
{
  LRVX_CAPI_ENTER_VOID(h);
  toFunding(h)->setConstantRate(rate);
  LRVX_CAPI_LEAVE_VOID;
}
extern "C" void lrvx_funding_schedule_reset(LrvxFundingScheduleHandle h)
{
  LRVX_CAPI_ENTER_VOID(h);
  toFunding(h)->reset();
  LRVX_CAPI_LEAVE_VOID;
}
extern "C" uint32_t lrvx_funding_schedule_tick(LrvxFundingScheduleHandle h, int64_t now_ns,
                                               const uint32_t* symbols,
                                               const double* positions,
                                               const double* mark_prices,
                                               uint32_t n_symbols, double* out_buf,
                                               uint32_t max_events)
{
  LRVX_CAPI_ENTER(h);
  std::vector<lrvx::SymbolId> syms(n_symbols);
  std::vector<double> pos(n_symbols);
  std::vector<double> mark(n_symbols);
  for (uint32_t i = 0; i < n_symbols; ++i)
  {
    syms[i] = symbols ? symbols[i] : 0;
    pos[i] = positions ? positions[i] : 0.0;
    mark[i] = mark_prices ? mark_prices[i] : 0.0;
  }
  auto events = toFunding(h)->tick(now_ns, syms, pos, mark);
  if (out_buf == nullptr || max_events == 0)
  {
    return static_cast<uint32_t>(events.size());
  }
  uint32_t n = std::min<uint32_t>(max_events, static_cast<uint32_t>(events.size()));
  for (uint32_t i = 0; i < n; ++i)
  {
    out_buf[i * 6 + 0] = static_cast<double>(events[i].timestampNs.raw());
    out_buf[i * 6 + 1] = static_cast<double>(events[i].symbol);
    out_buf[i * 6 + 2] = events[i].rate;
    out_buf[i * 6 + 3] = events[i].markPrice;
    out_buf[i * 6 + 4] = events[i].positionSigned;
    out_buf[i * 6 + 5] = events[i].amount;
  }
  return n;
  LRVX_CAPI_LEAVE;
}

extern "C" LrvxLiveQueuePositionHandle lrvx_live_queue_position_create(void)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  return new lrvx::LiveQueuePositionEstimator();
  LRVX_CAPI_LEAVE;
}

extern "C" void lrvx_live_queue_position_destroy(LrvxLiveQueuePositionHandle h)
{
  LRVX_CAPI_ENTER_DESTROY(h);
  delete toLiveQ(h);
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" void lrvx_live_queue_position_set_confidence_half_life_ns(
    LrvxLiveQueuePositionHandle h, int64_t half_life_ns)
{
  LRVX_CAPI_ENTER_VOID(h);
  toLiveQ(h)->setConfidenceHalfLifeNs(half_life_ns);
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" void lrvx_live_queue_position_set_shrink_factor(
    LrvxLiveQueuePositionHandle h, double factor)
{
  LRVX_CAPI_ENTER_VOID(h);
  toLiveQ(h)->setShrinkAttributionFactor(factor);
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" void lrvx_live_queue_position_on_order_placed(
    LrvxLiveQueuePositionHandle h, uint32_t symbol, uint8_t side, int64_t price_raw,
    uint64_t order_id, int64_t order_qty_raw, int64_t level_qty_raw, int64_t ts_ns)
{
  LRVX_CAPI_ENTER_VOID(h);
  toLiveQ(h)->onOrderPlaced(symbol, static_cast<lrvx::Side>(side),
                            lrvx::Price::fromRaw(price_raw), order_id,
                            lrvx::Quantity::fromRaw(order_qty_raw),
                            lrvx::Quantity::fromRaw(level_qty_raw), ts_ns);
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" void lrvx_live_queue_position_on_order_cancelled(
    LrvxLiveQueuePositionHandle h, uint64_t order_id, int64_t ts_ns)
{
  LRVX_CAPI_ENTER_VOID(h);
  toLiveQ(h)->onOrderCancelled(order_id, ts_ns);
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" void lrvx_live_queue_position_on_order_filled(
    LrvxLiveQueuePositionHandle h, uint64_t order_id, int64_t cumulative_fill_raw,
    int64_t ts_ns)
{
  LRVX_CAPI_ENTER_VOID(h);
  toLiveQ(h)->onOrderFilled(order_id, lrvx::Quantity::fromRaw(cumulative_fill_raw), ts_ns);
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" void lrvx_live_queue_position_on_trade(LrvxLiveQueuePositionHandle h,
                                                  uint32_t symbol, int64_t price_raw,
                                                  int64_t qty_raw, int64_t ts_ns)
{
  LRVX_CAPI_ENTER_VOID(h);
  toLiveQ(h)->onTrade(symbol, lrvx::Price::fromRaw(price_raw),
                      lrvx::Quantity::fromRaw(qty_raw), ts_ns);
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" void lrvx_live_queue_position_on_trade_with_flag(
    LrvxLiveQueuePositionHandle h, uint32_t symbol, int64_t price_raw, int64_t qty_raw,
    int64_t ts_ns, uint8_t is_hidden)
{
  LRVX_CAPI_ENTER_VOID(h);
  toLiveQ(h)->onTradeWithFlag(symbol, lrvx::Price::fromRaw(price_raw),
                              lrvx::Quantity::fromRaw(qty_raw), ts_ns, is_hidden != 0);
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" void lrvx_live_queue_position_set_hidden_order_policy(
    LrvxLiveQueuePositionHandle h, uint8_t policy)
{
  LRVX_CAPI_ENTER_VOID(h);
  toLiveQ(h)->setHiddenOrderPolicy(static_cast<lrvx::HiddenOrderPolicy>(policy));
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" void lrvx_live_queue_position_on_level_update(
    LrvxLiveQueuePositionHandle h, uint32_t symbol, uint8_t side, int64_t price_raw,
    int64_t new_qty_raw, int64_t ts_ns)
{
  LRVX_CAPI_ENTER_VOID(h);
  toLiveQ(h)->onLevelUpdate(symbol, static_cast<lrvx::Side>(side),
                            lrvx::Price::fromRaw(price_raw),
                            lrvx::Quantity::fromRaw(new_qty_raw), ts_ns);
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" uint8_t lrvx_live_queue_position_snapshot(LrvxLiveQueuePositionHandle h,
                                                     uint64_t order_id, int64_t now_ns,
                                                     int64_t* out_slots)
{
  LRVX_CAPI_ENTER(h);
  auto snap = toLiveQ(h)->snapshot(order_id, now_ns);
  if (!snap.has_value() || out_slots == nullptr)
  {
    return 0;
  }
  out_slots[0] = static_cast<int64_t>(snap->orderId);
  out_slots[1] = snap->queueAheadEst.raw();
  out_slots[2] = snap->total.raw();
  out_slots[3] = snap->lastUpdateNs;
  double conf = snap->confidence;
  std::memcpy(&out_slots[4], &conf, sizeof(double));
  out_slots[5] = snap->hiddenVolumeSeen.raw();
  return 1;
  LRVX_CAPI_LEAVE;
}

extern "C" uint32_t lrvx_live_queue_position_tracked_count(LrvxLiveQueuePositionHandle h)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<uint32_t>(toLiveQ(h)->trackedOrderCount());
  LRVX_CAPI_LEAVE;
}

// ============================================================
// Liquidation engine
// ============================================================

namespace
{
LiquidationEngine* toLiqEngine(LrvxLiquidationEngineHandle h)
{
  return static_cast<LiquidationEngine*>(h);
}
}  // namespace

extern "C" LrvxLiquidationEngineHandle lrvx_liquidation_engine_create(void)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  return new LiquidationEngine();
  LRVX_CAPI_LEAVE;
}

extern "C" void lrvx_liquidation_engine_destroy(LrvxLiquidationEngineHandle h)
{
  LRVX_CAPI_ENTER_DESTROY(h);
  delete toLiqEngine(h);
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" void lrvx_liquidation_engine_add_tier(LrvxLiquidationEngineHandle h,
                                                 double min_notional, double mm_fraction)
{
  LRVX_CAPI_ENTER_VOID(h);
  toLiqEngine(h)->addTier(min_notional, mm_fraction);
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" void lrvx_liquidation_engine_set_insurance_fund_capital(
    LrvxLiquidationEngineHandle h, double capital)
{
  LRVX_CAPI_ENTER_VOID(h);
  toLiqEngine(h)->setInsuranceFundCapital(capital);
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" double lrvx_liquidation_engine_insurance_fund_balance(LrvxLiquidationEngineHandle h)
{
  LRVX_CAPI_ENTER(h);
  return toLiqEngine(h)->insuranceFundBalance();
  LRVX_CAPI_LEAVE;
}

extern "C" void lrvx_liquidation_engine_set_adl_enabled(LrvxLiquidationEngineHandle h,
                                                        uint8_t enabled)
{
  LRVX_CAPI_ENTER_VOID(h);
  toLiqEngine(h)->setAdlEnabled(enabled != 0);
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" void lrvx_liquidation_engine_set_adl_ranking(LrvxLiquidationEngineHandle h,
                                                        uint8_t ranking)
{
  LRVX_CAPI_ENTER_VOID(h);
  AdlRanking r = AdlRanking::PnlRatio;
  switch (ranking)
  {
    case 1:
      r = AdlRanking::Binance;
      break;
    case 2:
      r = AdlRanking::Bybit;
      break;
    case 3:
      r = AdlRanking::PositionSize;
      break;
    default:
      r = AdlRanking::PnlRatio;
      break;
  }
  toLiqEngine(h)->setAdlRanking(r);
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" uint8_t lrvx_liquidation_engine_adl_ranking(LrvxLiquidationEngineHandle h)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<uint8_t>(toLiqEngine(h)->adlRanking());
  LRVX_CAPI_LEAVE;
}

extern "C" void lrvx_liquidation_engine_set_liquidation_slippage_bps(
    LrvxLiquidationEngineHandle h, double bps)
{
  LRVX_CAPI_ENTER_VOID(h);
  toLiqEngine(h)->setLiquidationSlippageBps(bps);
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" void lrvx_liquidation_engine_open_position(LrvxLiquidationEngineHandle h,
                                                      uint64_t account_id, uint32_t symbol,
                                                      double quantity, double entry_price,
                                                      double equity)
{
  LRVX_CAPI_ENTER_VOID(h);
  toLiqEngine(h)->openPosition(LeveragedPosition{.accountId = account_id,
                                                 .symbol = symbol,
                                                 .quantity = Quantity::fromDouble(quantity),
                                                 .entryPrice = Price::fromDouble(entry_price),
                                                 .equity = Volume::fromDouble(equity)});
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" void lrvx_liquidation_engine_close_position(LrvxLiquidationEngineHandle h,
                                                       uint64_t account_id, uint32_t symbol)
{
  LRVX_CAPI_ENTER_VOID(h);
  toLiqEngine(h)->closePosition(account_id, symbol);
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" uint32_t lrvx_liquidation_engine_on_mark(LrvxLiquidationEngineHandle h,
                                                    uint32_t symbol, double mark_price)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<uint32_t>(toLiqEngine(h)->onMark(symbol, mark_price).liquidationsCount);
  LRVX_CAPI_LEAVE;
}

extern "C" uint32_t lrvx_liquidation_engine_on_marks(LrvxLiquidationEngineHandle h,
                                                     uint32_t n,
                                                     const uint32_t* symbols,
                                                     const double* prices,
                                                     int64_t ts_ns)
{
  LRVX_CAPI_ENTER(h);
  std::vector<std::pair<lrvx::SymbolId, double>> marks;
  marks.reserve(n);
  for (uint32_t i = 0; i < n; ++i)
  {
    marks.emplace_back(symbols[i], prices[i]);
  }
  return static_cast<uint32_t>(
      toLiqEngine(h)->onMarks(marks, ts_ns).liquidationsCount);
  LRVX_CAPI_LEAVE;
}

extern "C" uint64_t lrvx_liquidation_engine_liquidations_count(LrvxLiquidationEngineHandle h)
{
  LRVX_CAPI_ENTER(h);
  return toLiqEngine(h)->liquidationsCount();
  LRVX_CAPI_LEAVE;
}
extern "C" uint64_t lrvx_liquidation_engine_insurance_payments_count(
    LrvxLiquidationEngineHandle h)
{
  LRVX_CAPI_ENTER(h);
  return toLiqEngine(h)->insurancePaymentsCount();
  LRVX_CAPI_LEAVE;
}
extern "C" uint64_t lrvx_liquidation_engine_adl_closeouts_count(LrvxLiquidationEngineHandle h)
{
  LRVX_CAPI_ENTER(h);
  return toLiqEngine(h)->adlCloseoutsCount();
  LRVX_CAPI_LEAVE;
}

extern "C" void lrvx_liquidation_engine_load_profile(LrvxLiquidationEngineHandle h,
                                                     uint8_t profile)
{
  LRVX_CAPI_ENTER_VOID(h);
  auto* eng = toLiqEngine(h);
  LiquidationEngine canned;
  switch (profile)
  {
    case 0:
      canned = LiquidationEngine::binance_um_futures();
      break;
    case 1:
      canned = LiquidationEngine::bybit_linear();
      break;
    case 2:
      canned = LiquidationEngine::okx_swap();
      break;
    default:
      return;
  }
  *eng = canned;
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" void lrvx_liquidation_engine_set_executor(LrvxLiquidationEngineHandle h,
                                                     LrvxSimulatedExecutorHandle exec_h)
{
  LRVX_CAPI_ENTER_VOID(h);
  SimulatedExecutor* ex = nullptr;
  if (exec_h != nullptr)
  {
    ex = &static_cast<LrvxSimulatedExecutorImpl*>(exec_h)->executor;
  }
  toLiqEngine(h)->setExecutor(ex);
  LRVX_CAPI_LEAVE_VOID;
}

// Cascade statistics.
namespace
{
template <typename T>
uint32_t copyVec(const std::vector<T>& src, T* out, uint32_t max)
{
  if (out == nullptr || max == 0)
  {
    return 0;
  }
  const uint32_t n = std::min<uint32_t>(max, static_cast<uint32_t>(src.size()));
  std::copy(src.begin(), src.begin() + n, out);
  return n;
}
}  // namespace

extern "C" uint32_t lrvx_liquidation_engine_deficits_paid_by_fund_size(
    LrvxLiquidationEngineHandle h)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<uint32_t>(toLiqEngine(h)->deficitsPaidByFund().size());
  LRVX_CAPI_LEAVE;
}
extern "C" uint32_t lrvx_liquidation_engine_deficits_paid_by_fund_copy(
    LrvxLiquidationEngineHandle h, double* out, uint32_t max)
{
  LRVX_CAPI_ENTER(h);
  return copyVec(toLiqEngine(h)->deficitsPaidByFund(), out, max);
  LRVX_CAPI_LEAVE;
}
extern "C" uint32_t lrvx_liquidation_engine_deficits_paid_by_adl_size(
    LrvxLiquidationEngineHandle h)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<uint32_t>(toLiqEngine(h)->deficitsPaidByAdl().size());
  LRVX_CAPI_LEAVE;
}
extern "C" uint32_t lrvx_liquidation_engine_deficits_paid_by_adl_copy(
    LrvxLiquidationEngineHandle h, double* out, uint32_t max)
{
  LRVX_CAPI_ENTER(h);
  return copyVec(toLiqEngine(h)->deficitsPaidByAdl(), out, max);
  LRVX_CAPI_LEAVE;
}
extern "C" uint32_t lrvx_liquidation_engine_cascade_sizes_size(
    LrvxLiquidationEngineHandle h)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<uint32_t>(toLiqEngine(h)->cascadeSizesPerTick().size());
  LRVX_CAPI_LEAVE;
}
extern "C" uint32_t lrvx_liquidation_engine_cascade_sizes_copy(
    LrvxLiquidationEngineHandle h, uint32_t* out, uint32_t max)
{
  LRVX_CAPI_ENTER(h);
  return copyVec(toLiqEngine(h)->cascadeSizesPerTick(), out, max);
  LRVX_CAPI_LEAVE;
}
extern "C" uint32_t lrvx_liquidation_engine_fund_balance_history_size(
    LrvxLiquidationEngineHandle h)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<uint32_t>(toLiqEngine(h)->fundBalanceHistory().size());
  LRVX_CAPI_LEAVE;
}
extern "C" uint32_t lrvx_liquidation_engine_fund_balance_history_copy(
    LrvxLiquidationEngineHandle h, double* out, uint32_t max)
{
  LRVX_CAPI_ENTER(h);
  return copyVec(toLiqEngine(h)->fundBalanceHistory(), out, max);
  LRVX_CAPI_LEAVE;
}
extern "C" uint64_t lrvx_liquidation_engine_ticks_to_first_adl(
    LrvxLiquidationEngineHandle h)
{
  LRVX_CAPI_ENTER(h);
  return toLiqEngine(h)->ticksToFirstAdl();
  LRVX_CAPI_LEAVE;
}
extern "C" void lrvx_liquidation_engine_reset_stats(LrvxLiquidationEngineHandle h)
{
  LRVX_CAPI_ENTER_VOID(h);
  toLiqEngine(h)->resetStats();
  LRVX_CAPI_LEAVE_VOID;
}
extern "C" void lrvx_liquidation_engine_set_mark_impact_model(
    LrvxLiquidationEngineHandle h, uint8_t model, double weight)
{
  LRVX_CAPI_ENTER_VOID(h);
  using M = lrvx::LiquidationEngine::MarkImpactModel;
  M m = M::None;
  if (model == 1)
  {
    m = M::BookAnchored;
  }
  else if (model == 2)
  {
    m = M::BookOnly;
  }
  toLiqEngine(h)->setMarkImpactModel(m, weight);
  LRVX_CAPI_LEAVE_VOID;
}
extern "C" uint8_t lrvx_liquidation_engine_mark_impact_model(
    LrvxLiquidationEngineHandle h)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<uint8_t>(toLiqEngine(h)->markImpactModel());
  LRVX_CAPI_LEAVE;
}
extern "C" double lrvx_liquidation_engine_mark_impact_weight(
    LrvxLiquidationEngineHandle h)
{
  LRVX_CAPI_ENTER(h);
  return toLiqEngine(h)->markImpactWeight();
  LRVX_CAPI_LEAVE;
}
extern "C" void lrvx_liquidation_engine_set_max_cascade_depth(
    LrvxLiquidationEngineHandle h, uint32_t depth)
{
  LRVX_CAPI_ENTER_VOID(h);
  toLiqEngine(h)->setMaxCascadeDepth(depth);
  LRVX_CAPI_LEAVE_VOID;
}
extern "C" uint32_t lrvx_liquidation_engine_max_cascade_depth(
    LrvxLiquidationEngineHandle h)
{
  LRVX_CAPI_ENTER(h);
  return toLiqEngine(h)->maxCascadeDepth();
  LRVX_CAPI_LEAVE;
}

// ============================================================
// Account
// ============================================================

extern "C" LrvxAccountHandle lrvx_account_create(uint64_t account_id, double equity)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  return new lrvx::Account(account_id, equity);
  LRVX_CAPI_LEAVE;
}
extern "C" void lrvx_account_destroy(LrvxAccountHandle h)
{
  LRVX_CAPI_ENTER_DESTROY(h);
  delete toAccount(h);
  LRVX_CAPI_LEAVE_VOID;
}
extern "C" uint64_t lrvx_account_id(LrvxAccountHandle h)
{
  LRVX_CAPI_ENTER(h);
  return toAccount(h)->accountId();
  LRVX_CAPI_LEAVE;
}
extern "C" double lrvx_account_equity(LrvxAccountHandle h)
{
  LRVX_CAPI_ENTER(h);
  // The C ABI is double-facing; Account keeps the value in fixed point.
  return toAccount(h)->equity().toDouble();
  LRVX_CAPI_LEAVE;
}
extern "C" void lrvx_account_set_equity(LrvxAccountHandle h, double equity)
{
  LRVX_CAPI_ENTER_VOID(h);
  toAccount(h)->setEquity(equity);
  LRVX_CAPI_LEAVE_VOID;
}
extern "C" void lrvx_account_add_equity(LrvxAccountHandle h, double delta)
{
  LRVX_CAPI_ENTER_VOID(h);
  toAccount(h)->addEquity(delta);
  LRVX_CAPI_LEAVE_VOID;
}
extern "C" uint8_t lrvx_account_margin_mode(LrvxAccountHandle h)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<uint8_t>(toAccount(h)->marginMode());
  LRVX_CAPI_LEAVE;
}
extern "C" void lrvx_account_set_margin_mode(LrvxAccountHandle h, uint8_t mode)
{
  LRVX_CAPI_ENTER_VOID(h);
  using M = lrvx::MarginMode;
  toAccount(h)->setMarginMode(mode == 1 ? M::Isolated : M::Cross);
  LRVX_CAPI_LEAVE_VOID;
}
extern "C" void lrvx_account_open_position(LrvxAccountHandle h, uint32_t symbol,
                                           double quantity, double entry_price)
{
  LRVX_CAPI_ENTER_VOID(h);
  toAccount(h)->openPosition(symbol, quantity, entry_price);
  LRVX_CAPI_LEAVE_VOID;
}
extern "C" void lrvx_account_open_position_isolated(LrvxAccountHandle h,
                                                    uint32_t symbol,
                                                    double quantity,
                                                    double entry_price,
                                                    double isolated_equity)
{
  LRVX_CAPI_ENTER_VOID(h);
  toAccount(h)->openPosition(symbol, quantity, entry_price, isolated_equity);
  LRVX_CAPI_LEAVE_VOID;
}
extern "C" void lrvx_account_close_position(LrvxAccountHandle h, uint32_t symbol)
{
  LRVX_CAPI_ENTER_VOID(h);
  toAccount(h)->closePosition(symbol);
  LRVX_CAPI_LEAVE_VOID;
}
extern "C" uint32_t lrvx_account_position_count(LrvxAccountHandle h)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<uint32_t>(toAccount(h)->positionCount());
  LRVX_CAPI_LEAVE;
}
extern "C" void lrvx_account_set_mark(LrvxAccountHandle h, uint32_t symbol, double price)
{
  LRVX_CAPI_ENTER_VOID(h);
  toAccount(h)->setMark(symbol, price);
  LRVX_CAPI_LEAVE_VOID;
}
extern "C" void lrvx_account_set_mark_at(LrvxAccountHandle h, uint32_t symbol,
                                         double price, int64_t ts_ns)
{
  LRVX_CAPI_ENTER_VOID(h);
  toAccount(h)->setMark(symbol, price, ts_ns);
  LRVX_CAPI_LEAVE_VOID;
}
extern "C" int64_t lrvx_account_mark_ts(LrvxAccountHandle h, uint32_t symbol)
{
  LRVX_CAPI_ENTER(h);
  return toAccount(h)->markTsFor(symbol);
  LRVX_CAPI_LEAVE;
}
extern "C" uint8_t lrvx_account_has_stale_marks(LrvxAccountHandle h,
                                                int64_t now_ns, int64_t budget_ns)
{
  LRVX_CAPI_ENTER(h);
  return toAccount(h)->hasStaleMarks(now_ns, budget_ns) ? 1 : 0;
  LRVX_CAPI_LEAVE;
}
extern "C" double lrvx_account_total_notional(LrvxAccountHandle h)
{
  LRVX_CAPI_ENTER(h);
  return toAccount(h)->totalNotional().toDouble();
  LRVX_CAPI_LEAVE;
}
extern "C" double lrvx_account_total_unrealised_pnl(LrvxAccountHandle h)
{
  LRVX_CAPI_ENTER(h);
  return toAccount(h)->totalUnrealisedPnl().toDouble();
  LRVX_CAPI_LEAVE;
}
extern "C" void lrvx_account_record_fill(LrvxAccountHandle h, int64_t ts_ns,
                                         double notional)
{
  LRVX_CAPI_ENTER_VOID(h);
  toAccount(h)->recordFill(ts_ns, notional);
  LRVX_CAPI_LEAVE_VOID;
}
extern "C" void lrvx_account_record_fill_ex(LrvxAccountHandle h, int64_t ts_ns,
                                            double notional, uint32_t symbol)
{
  LRVX_CAPI_ENTER_VOID(h);
  toAccount(h)->recordFill(ts_ns, notional, symbol);
  LRVX_CAPI_LEAVE_VOID;
}
extern "C" uint32_t lrvx_account_rolling_notional_by_symbol_size(LrvxAccountHandle h)
{
  LRVX_CAPI_ENTER(h);
  return static_cast<uint32_t>(toAccount(h)->rollingNotionalBySymbol30d().size());
  LRVX_CAPI_LEAVE;
}
extern "C" uint32_t lrvx_account_rolling_notional_by_symbol_copy(
    LrvxAccountHandle h, uint32_t* symbols_out, double* notionals_out,
    uint32_t max)
{
  LRVX_CAPI_ENTER(h);
  const auto pairs = toAccount(h)->rollingNotionalBySymbol30d();
  const uint32_t n = static_cast<uint32_t>(
      std::min<size_t>(pairs.size(), static_cast<size_t>(max)));
  for (uint32_t i = 0; i < n; ++i)
  {
    symbols_out[i] = pairs[i].first;
    notionals_out[i] = pairs[i].second.toDouble();
  }
  return n;
  LRVX_CAPI_LEAVE;
}
extern "C" double lrvx_account_rolling_notional_30d(LrvxAccountHandle h)
{
  LRVX_CAPI_ENTER(h);
  return toAccount(h)->rollingNotional30d().toDouble();
  LRVX_CAPI_LEAVE;
}
extern "C" void lrvx_account_reset_rolling(LrvxAccountHandle h)
{
  LRVX_CAPI_ENTER_VOID(h);
  toAccount(h)->resetRolling();
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" void lrvx_liquidation_engine_attach_account(
    LrvxLiquidationEngineHandle h, LrvxAccountHandle account)
{
  LRVX_CAPI_ENTER_VOID(h);
  toLiqEngine(h)->attachAccount(toAccount(account));
  LRVX_CAPI_LEAVE_VOID;
}
extern "C" void lrvx_liquidation_engine_detach_account(
    LrvxLiquidationEngineHandle h, uint64_t account_id)
{
  LRVX_CAPI_ENTER_VOID(h);
  toLiqEngine(h)->detachAccount(account_id);
  LRVX_CAPI_LEAVE_VOID;
}
extern "C" void lrvx_fee_schedule_bind_account(LrvxFeeScheduleHandle h,
                                               LrvxAccountHandle account)
{
  LRVX_CAPI_ENTER_VOID(h);
  toFee(h)->bindAccount(toAccount(account));
  LRVX_CAPI_LEAVE_VOID;
}
extern "C" void lrvx_fee_schedule_clear_account_binding(LrvxFeeScheduleHandle h)
{
  LRVX_CAPI_ENTER_VOID(h);
  toFee(h)->clearAccountBinding();
  LRVX_CAPI_LEAVE_VOID;
}

// ============================================================
// VenueStack
// ============================================================

#include "lrvx/backtest/venue_stack.h"

namespace
{

// The handle points at this wrapper, not at the VenueStack: the executor
// accessor has to hand back a LrvxSimulatedExecutorImpl, and that wrapper
// needs an owner with the same lifetime as the stack. Registering the six
// borrowed addresses here, once, is also what lets _destroy recognise them.
struct LrvxVenueStackImpl
{
  lrvx::VenueStack stack;
  LrvxSimulatedExecutorImpl executorView;

  explicit LrvxVenueStackImpl(lrvx::VenueStack&& s)
      : stack(std::move(s)), executorView(stack.clock(), stack.executor())
  {
    for (const void* p : borrowed())
    {
      LrvxBorrowedHandles::add(p);
    }
  }

  ~LrvxVenueStackImpl()
  {
    for (const void* p : borrowed())
    {
      LrvxBorrowedHandles::remove(p);
    }
  }

  LrvxVenueStackImpl(const LrvxVenueStackImpl&) = delete;
  LrvxVenueStackImpl& operator=(const LrvxVenueStackImpl&) = delete;

 private:
  std::array<const void*, 6> borrowed()
  {
    return {&executorView, &stack.account(), &stack.liquidation(),
            &stack.fees(), &stack.funding(), &stack.venue()};
  }
};

inline LrvxVenueStackImpl* toVenueStackImpl(LrvxVenueStackHandle h)
{
  return static_cast<LrvxVenueStackImpl*>(h);
}

inline lrvx::VenueStack* toVenueStack(LrvxVenueStackHandle h)
{
  return &toVenueStackImpl(h)->stack;
}

}  // namespace

extern "C" LrvxVenueStackHandle lrvx_venue_stack_create(uint8_t venue,
                                                        uint64_t account_id,
                                                        double equity)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  using lrvx::VenueStack;
  switch (venue)
  {
    case 1:
      return new LrvxVenueStackImpl(VenueStack::bybit_linear(account_id, equity));
    case 2:
      return new LrvxVenueStackImpl(VenueStack::okx_swap(account_id, equity));
    case 3:
      return new LrvxVenueStackImpl(VenueStack::deribit(account_id, equity));
    case 0:
    default:
      return new LrvxVenueStackImpl(VenueStack::binance_um_futures(account_id, equity));
  }
  LRVX_CAPI_LEAVE;
}
extern "C" void lrvx_venue_stack_destroy(LrvxVenueStackHandle h)
{
  LRVX_CAPI_ENTER_DESTROY(h);
  delete toVenueStackImpl(h);
  LRVX_CAPI_LEAVE_VOID;
}
// Borrowed: valid while the stack lives, and lrvx_simulated_executor_destroy
// on it does nothing. See the ownership paragraph in lrvx_capi.h.
extern "C" LrvxSimulatedExecutorHandle lrvx_venue_stack_executor(
    LrvxVenueStackHandle h)
{
  LRVX_CAPI_ENTER(h);
  return &toVenueStackImpl(h)->executorView;
  LRVX_CAPI_LEAVE;
}
extern "C" LrvxAccountHandle lrvx_venue_stack_account(LrvxVenueStackHandle h)
{
  LRVX_CAPI_ENTER(h);
  return &toVenueStack(h)->account();
  LRVX_CAPI_LEAVE;
}
extern "C" LrvxLiquidationEngineHandle lrvx_venue_stack_liquidation(
    LrvxVenueStackHandle h)
{
  LRVX_CAPI_ENTER(h);
  return &toVenueStack(h)->liquidation();
  LRVX_CAPI_LEAVE;
}
extern "C" LrvxFeeScheduleHandle lrvx_venue_stack_fees(LrvxVenueStackHandle h)
{
  LRVX_CAPI_ENTER(h);
  return &toVenueStack(h)->fees();
  LRVX_CAPI_LEAVE;
}
extern "C" LrvxFundingScheduleHandle lrvx_venue_stack_funding(
    LrvxVenueStackHandle h)
{
  LRVX_CAPI_ENTER(h);
  return &toVenueStack(h)->funding();
  LRVX_CAPI_LEAVE;
}
extern "C" LrvxVenueAvailabilityHandle lrvx_venue_stack_venue(
    LrvxVenueStackHandle h)
{
  LRVX_CAPI_ENTER(h);
  return &toVenueStack(h)->venue();
  LRVX_CAPI_LEAVE;
}
extern "C" const char* lrvx_venue_stack_venue_name(LrvxVenueStackHandle h)
{
  LRVX_CAPI_ENTER(h);
  return toVenueStack(h)->venueName().c_str();
  LRVX_CAPI_LEAVE;
}

// ============================================================
// DEX amounts (u256 / i256) at the C boundary
// ============================================================

namespace
{
// Write a NUL-terminated string into out; return 1 if it fit, else 0.
inline uint8_t writeOut(const std::string& s, char* out, size_t out_len)
{
  if (out == nullptr || out_len == 0 || s.size() + 1 > out_len)
  {
    return 0;
  }
  std::memcpy(out, s.c_str(), s.size() + 1);
  return 1;
}
}  // namespace

extern "C" uint8_t lrvx_u256_roundtrip(const char* dec, char* out, size_t out_len)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  if (dec == nullptr)
  {
    return 0;
  }
  try
  {
    return writeOut(lrvx::u256::fromDec(dec).toDec(), out, out_len);
  }
  catch (...)
  {
    return 0;
  }
  LRVX_CAPI_LEAVE;
}

extern "C" uint8_t lrvx_i256_roundtrip(const char* dec, char* out, size_t out_len)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  if (dec == nullptr)
  {
    return 0;
  }
  try
  {
    const lrvx::i256 v = lrvx::i256::fromDec(dec);
    const std::string mag = v.magnitude().toDec();
    const std::string s = (v.neg && !v.magnitude().isZero()) ? "-" + mag : mag;
    return writeOut(s, out, out_len);
  }
  catch (...)
  {
    return 0;
  }
  LRVX_CAPI_LEAVE;
}

extern "C" uint8_t lrvx_u256_from_hex(const char* hex, char* out, size_t out_len)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  if (hex == nullptr)
  {
    return 0;
  }
  try
  {
    return writeOut(lrvx::u256::fromHex(hex).toDec(), out, out_len);
  }
  catch (...)
  {
    return 0;
  }
  LRVX_CAPI_LEAVE;
}

extern "C" uint8_t lrvx_u256_to_words(const char* dec, uint64_t* words)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  if (dec == nullptr || words == nullptr)
  {
    return 0;
  }
  try
  {
    const lrvx::u256 v = lrvx::u256::fromDec(dec);
    for (int i = 0; i < 4; ++i)
    {
      words[i] = v.w[static_cast<std::size_t>(i)];  // little-endian, w[0] is the LSB limb
    }
    return 1;
  }
  catch (...)
  {
    return 0;
  }
  LRVX_CAPI_LEAVE;
}

extern "C" uint8_t lrvx_u256_from_words(const uint64_t* words, char* out, size_t out_len)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  if (words == nullptr)
  {
    return 0;
  }
  lrvx::u256 v;
  for (int i = 0; i < 4; ++i)
  {
    v.w[static_cast<std::size_t>(i)] = words[i];
  }
  return writeOut(v.toDec(), out, out_len);
  LRVX_CAPI_LEAVE;
}

// ============================================================
// AMM curves
// ============================================================

namespace
{
inline lrvx::INTokenCurve* toCurve(LrvxCurveHandle h)
{
  return static_cast<lrvx::INTokenCurve*>(h);
}
}  // namespace

extern "C" LrvxCurveHandle lrvx_curve_constant_product(const char* reserve0, const char* reserve1,
                                                       uint64_t fee_num, uint64_t fee_den)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  if (reserve0 == nullptr || reserve1 == nullptr)
  {
    return nullptr;
  }
  try
  {
    return new lrvx::ConstantProductCurve(lrvx::u256::fromDec(reserve0),
                                          lrvx::u256::fromDec(reserve1), fee_num, fee_den);
  }
  catch (...)
  {
    return nullptr;
  }
  LRVX_CAPI_LEAVE;
}

extern "C" LrvxCurveHandle lrvx_curve_raydium_cp(const char* reserve0, const char* reserve1,
                                                 uint64_t trade_fee_rate, uint64_t creator_fee_rate,
                                                 uint8_t creator_fee_on_input)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  if (reserve0 == nullptr || reserve1 == nullptr)
  {
    return nullptr;
  }
  try
  {
    return new lrvx::RaydiumCpCurve(lrvx::u256::fromDec(reserve0), lrvx::u256::fromDec(reserve1),
                                    trade_fee_rate, creator_fee_rate, creator_fee_on_input != 0);
  }
  catch (...)
  {
    return nullptr;
  }
  LRVX_CAPI_LEAVE;
}

extern "C" LrvxCurveHandle lrvx_curve_uniswap_v3(const char* sqrt_price_x96, const char* liquidity,
                                                 uint32_t fee_pips,
                                                 const char* const* tick_sqrt_ratio,
                                                 const char* const* tick_liquidity_net,
                                                 size_t n_ticks)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  if (sqrt_price_x96 == nullptr || liquidity == nullptr)
  {
    return nullptr;
  }
  try
  {
    std::vector<lrvx::ClTick> ticks;
    ticks.reserve(n_ticks);
    for (size_t k = 0; k < n_ticks; ++k)
    {
      ticks.push_back({lrvx::u256::fromDec(tick_sqrt_ratio[k]),
                       lrvx::i256::fromDec(tick_liquidity_net[k])});
    }
    return new lrvx::ConcentratedLiquidityCurve(lrvx::u256::fromDec(sqrt_price_x96),
                                                lrvx::u256::fromDec(liquidity), fee_pips,
                                                std::move(ticks));
  }
  catch (...)
  {
    return nullptr;
  }
  LRVX_CAPI_LEAVE;
}

extern "C" size_t lrvx_curve_token_count(LrvxCurveHandle curve)
{
  LRVX_CAPI_ENTER(curve);
  return curve == nullptr ? 0 : toCurve(curve)->tokenCount();
  LRVX_CAPI_LEAVE;
}

extern "C" uint8_t lrvx_curve_amount_out(LrvxCurveHandle curve, size_t i, size_t j,
                                         const char* amount_in, char* out, size_t out_len)
{
  LRVX_CAPI_ENTER(curve);
  if (curve == nullptr || amount_in == nullptr)
  {
    return 0;
  }
  try
  {
    return writeOut(toCurve(curve)->amountOut(i, j, lrvx::u256::fromDec(amount_in)).toDec(), out,
                    out_len);
  }
  catch (...)
  {
    return 0;
  }
  LRVX_CAPI_LEAVE;
}

extern "C" uint8_t lrvx_curve_apply_swap(LrvxCurveHandle curve, size_t i, size_t j,
                                         const char* amount_in, char* out, size_t out_len)
{
  LRVX_CAPI_ENTER(curve);
  if (curve == nullptr || amount_in == nullptr)
  {
    return 0;
  }
  try
  {
    return writeOut(toCurve(curve)->applySwap(i, j, lrvx::u256::fromDec(amount_in)).toDec(), out,
                    out_len);
  }
  catch (...)
  {
    return 0;
  }
  LRVX_CAPI_LEAVE;
}

extern "C" uint8_t lrvx_curve_balance(LrvxCurveHandle curve, size_t i, char* out, size_t out_len)
{
  LRVX_CAPI_ENTER(curve);
  if (curve == nullptr)
  {
    return 0;
  }
  try
  {
    const std::vector<lrvx::u256>& b = toCurve(curve)->balances();
    if (i >= b.size())
    {
      return 0;
    }
    return writeOut(b[i].toDec(), out, out_len);
  }
  catch (...)
  {
    return 0;
  }
  LRVX_CAPI_LEAVE;
}

extern "C" uint8_t lrvx_curve_sqrt_price(LrvxCurveHandle curve, char* out, size_t out_len)
{
  LRVX_CAPI_ENTER(curve);
  const auto* cl = curve == nullptr
                       ? nullptr
                       : dynamic_cast<const lrvx::ConcentratedLiquidityCurve*>(toCurve(curve));
  if (cl == nullptr)
  {
    writeOut("0", out, out_len);
    return 0;
  }
  return writeOut(cl->sqrtPrice().toDec(), out, out_len);
  LRVX_CAPI_LEAVE;
}

extern "C" uint8_t lrvx_curve_liquidity(LrvxCurveHandle curve, char* out, size_t out_len)
{
  LRVX_CAPI_ENTER(curve);
  const auto* cl = curve == nullptr
                       ? nullptr
                       : dynamic_cast<const lrvx::ConcentratedLiquidityCurve*>(toCurve(curve));
  if (cl == nullptr)
  {
    writeOut("0", out, out_len);
    return 0;
  }
  return writeOut(cl->liquidity().toDec(), out, out_len);
  LRVX_CAPI_LEAVE;
}

extern "C" LrvxCurveHandle lrvx_curve_clone(LrvxCurveHandle curve)
{
  LRVX_CAPI_ENTER(curve);
  return curve == nullptr ? nullptr : toCurve(curve)->clone().release();
  LRVX_CAPI_LEAVE;
}

extern "C" void lrvx_curve_destroy(LrvxCurveHandle curve)
{
  LRVX_CAPI_ENTER_DESTROY(curve);
  delete toCurve(curve);
  LRVX_CAPI_LEAVE_VOID;
}

// ============================================================
// Pool-state tape replay
// ============================================================

namespace
{
struct PoolTapeBuilder
{
  std::vector<uint8_t> bytes;
};

struct PoolReplayResult
{
  std::unique_ptr<lrvx::INTokenCurve> seed;
  std::unique_ptr<lrvx::AmmDexConnector> conn;
  std::unique_ptr<lrvx::PoolStateReplay> replay;
  std::size_t trades{0};
};

inline PoolTapeBuilder* toTape(LrvxPoolTapeHandle h) { return static_cast<PoolTapeBuilder*>(h); }
inline PoolReplayResult* toReplay(LrvxPoolReplayHandle h)
{
  return static_cast<PoolReplayResult*>(h);
}
}  // namespace

extern "C" LrvxPoolTapeHandle lrvx_pool_tape_create(void)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  return new PoolTapeBuilder();
  LRVX_CAPI_LEAVE;
}
extern "C" void lrvx_pool_tape_destroy(LrvxPoolTapeHandle tape)
{
  LRVX_CAPI_ENTER_DESTROY(tape);
  delete toTape(tape);
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" void lrvx_pool_tape_descriptor_constant_product(LrvxPoolTapeHandle tape,
                                                           uint64_t fee_num, uint64_t fee_den,
                                                           uint8_t base_dec, uint8_t quote_dec)
{
  LRVX_CAPI_ENTER_VOID(tape);
  lrvx::PoolStateWriter(toTape(tape)->bytes)
      .descriptorConstantProduct(fee_num, fee_den, base_dec, quote_dec);
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" void lrvx_pool_tape_descriptor_raydium_cp(LrvxPoolTapeHandle tape,
                                                     uint64_t trade_fee_rate,
                                                     uint64_t creator_fee_rate,
                                                     uint8_t creator_fee_on_input, uint8_t base_dec,
                                                     uint8_t quote_dec)
{
  LRVX_CAPI_ENTER_VOID(tape);
  lrvx::PoolStateWriter(toTape(tape)->bytes)
      .descriptorRaydiumCp(trade_fee_rate, creator_fee_rate, creator_fee_on_input != 0, base_dec,
                           quote_dec);
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" void lrvx_pool_tape_descriptor_clmm(LrvxPoolTapeHandle tape, uint8_t venue,
                                               uint32_t fee_pips, uint8_t base_dec,
                                               uint8_t quote_dec)
{
  LRVX_CAPI_ENTER_VOID(tape);
  lrvx::PoolStateWriter(toTape(tape)->bytes)
      .descriptorClmm(static_cast<lrvx::PoolVenue>(venue), fee_pips, base_dec, quote_dec);
  LRVX_CAPI_LEAVE_VOID;
}

extern "C" uint8_t lrvx_pool_tape_checkpoint(LrvxPoolTapeHandle tape, int64_t ts_ns,
                                             const char* reserve0, const char* reserve1)
{
  LRVX_CAPI_ENTER(tape);
  if (reserve0 == nullptr || reserve1 == nullptr)
  {
    return 0;
  }
  try
  {
    lrvx::PoolStateWriter(toTape(tape)->bytes)
        .checkpoint(ts_ns, lrvx::u256::fromDec(reserve0), lrvx::u256::fromDec(reserve1));
    return 1;
  }
  catch (...)
  {
    return 0;
  }
  LRVX_CAPI_LEAVE;
}

extern "C" uint8_t lrvx_pool_tape_checkpoint_clmm(LrvxPoolTapeHandle tape, int64_t ts_ns,
                                                  const char* sqrt_price, const char* liquidity,
                                                  const char* const* tick_sqrt_ratio,
                                                  const char* const* tick_liquidity_net,
                                                  size_t n_ticks)
{
  LRVX_CAPI_ENTER(tape);
  if (sqrt_price == nullptr || liquidity == nullptr)
  {
    return 0;
  }
  try
  {
    std::vector<lrvx::ClTick> ticks;
    ticks.reserve(n_ticks);
    for (size_t k = 0; k < n_ticks; ++k)
    {
      ticks.push_back({lrvx::u256::fromDec(tick_sqrt_ratio[k]),
                       lrvx::i256::fromDec(tick_liquidity_net[k])});
    }
    lrvx::PoolStateWriter(toTape(tape)->bytes)
        .checkpointClmm(ts_ns, lrvx::u256::fromDec(sqrt_price), lrvx::u256::fromDec(liquidity),
                        ticks);
    return 1;
  }
  catch (...)
  {
    return 0;
  }
  LRVX_CAPI_LEAVE;
}

extern "C" uint8_t lrvx_pool_tape_swap(LrvxPoolTapeHandle tape, int64_t ts_ns,
                                       uint8_t base_for_quote, const char* amount_in)
{
  LRVX_CAPI_ENTER(tape);
  if (amount_in == nullptr)
  {
    return 0;
  }
  try
  {
    lrvx::PoolStateWriter(toTape(tape)->bytes)
        .swap(ts_ns, base_for_quote != 0, lrvx::u256::fromDec(amount_in));
    return 1;
  }
  catch (...)
  {
    return 0;
  }
  LRVX_CAPI_LEAVE;
}

extern "C" LrvxPoolReplayHandle lrvx_pool_tape_replay(LrvxPoolTapeHandle tape, size_t base_idx,
                                                      size_t quote_idx, uint8_t base_dec,
                                                      uint8_t quote_dec)
{
  LRVX_CAPI_ENTER(tape);
  auto* r = new PoolReplayResult();
  // A throwaway seed for the connector ctor; the replay re-points it at the curve it
  // rebuilds from the tape's first Checkpoint.
  r->seed = std::make_unique<lrvx::ConstantProductCurve>(lrvx::u256(1), lrvx::u256(1), 1, 1);
  r->conn = std::make_unique<lrvx::AmmDexConnector>("amm", lrvx::SymbolId{1}, *r->seed, base_idx,
                                                    quote_idx, base_dec, quote_dec, 1,
                                                    lrvx::u256(1));
  r->conn->setCallbacks([](const lrvx::BookUpdateEvent&) {},
                        [r](const lrvx::TradeEvent&)
                        { ++r->trades; });
  r->replay = std::make_unique<lrvx::PoolStateReplay>(*r->conn);
  r->replay->run(toTape(tape)->bytes);
  return r;
  LRVX_CAPI_LEAVE;
}

extern "C" size_t lrvx_pool_replay_drift_count(LrvxPoolReplayHandle replay)
{
  LRVX_CAPI_ENTER(replay);
  return replay == nullptr ? 0 : toReplay(replay)->replay->driftCount();
  LRVX_CAPI_LEAVE;
}

extern "C" size_t lrvx_pool_replay_trade_count(LrvxPoolReplayHandle replay)
{
  LRVX_CAPI_ENTER(replay);
  return replay == nullptr ? 0 : toReplay(replay)->trades;
  LRVX_CAPI_LEAVE;
}

extern "C" LrvxCurveHandle lrvx_pool_replay_curve(LrvxPoolReplayHandle replay)
{
  LRVX_CAPI_ENTER(replay);
  // Borrowed: the replay owns this curve. Registered on the way out rather
  // than at construction because the replay only has a curve once the tape
  // carried a checkpoint, and a caller cannot hold the pointer without
  // having come through here first.
  auto* curve = const_cast<lrvx::INTokenCurve*>(toReplay(replay)->replay->curve());
  LrvxBorrowedHandles::add(curve);
  return curve;
  LRVX_CAPI_LEAVE;
}

extern "C" void lrvx_pool_replay_destroy(LrvxPoolReplayHandle replay)
{
  LRVX_CAPI_ENTER_DESTROY(replay);
  auto* impl = toReplay(replay);
  LrvxBorrowedHandles::remove(const_cast<lrvx::INTokenCurve*>(impl->replay->curve()));
  delete impl;
  LRVX_CAPI_LEAVE_VOID;
}

// ============================================================
// Diagnostics: ABI version and the last error on this thread
// ============================================================

// The shared library exported 729 symbols and not one of them said which ABI
// they belonged to, so a consumer loading it through dlopen or ctypes -- the
// way docs/bindings/capi.md suggests -- had no way to notice a mismatch. The
// structs on this boundary are solid, with no reserved tail, so a version
// skew shows up as silently wrong numbers rather than a failed load.
extern "C" uint32_t lrvx_capi_abi_version(void)
{
  LRVX_CAPI_ENTER_NOHANDLE;
  return LRVX_CAPI_ABI_VERSION;
  LRVX_CAPI_LEAVE;
}

// These three are the only exported functions without the LRVX_CAPI_ENTER /
// LRVX_CAPI_LEAVE pair, because the pair reports through them: catching here
// and returning {} would hand back a NULL message pointer and lose the very
// error the caller is asking about. They take no handle and touch nothing
// that throws once the thread-local slot exists, so each carries its own
// narrow catch instead.
extern "C" int lrvx_last_error_code(void)
{
  try
  {
    return lrvxCapiLastError().code;
  }
  catch (...)
  {
    return kLrvxCapiErrException;
  }
}

extern "C" const char* lrvx_last_error_message(void)
{
  try
  {
    return lrvxCapiLastError().message.c_str();
  }
  catch (...)
  {
    return "";
  }
}

extern "C" void lrvx_clear_last_error(void)
{
  try
  {
    auto& e = lrvxCapiLastError();
    e.code = kLrvxCapiOk;
    e.message.clear();
  }
  catch (...)
  {
  }
}
