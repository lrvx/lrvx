/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// lrvx_capi_spec.hpp — IDL spec for the lrvx C API.
//
// Generated from lrvx_capi.h via tools/codegen/scripts/import_capi.py.
// After bootstrap, this file is the source of truth — edit here, regenerate
// the C header via tools/codegen/scripts/regenerate.sh.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "lrvx/capi/lrvx_export.h"

// ABI version of this header and of the library that ships with it. Bumped
// whenever a struct on this boundary changes shape, a function changes
// signature, or an existing code space gains a meaning. 2: LrvxSignal grew
// range_lower / range_upper / liquidity. 3: LrvxBar grew close_reason.
// 4: LrvxBookSnapshot grew has_bid / has_ask. 5: LrvxSymbolContext grew
// last_touch_ns.
// lrvx::export_macro(group="abi_version")
#define LRVX_CAPI_ABI_VERSION 5

#ifdef __cplusplus
extern "C"
{
#endif

  // ============================================================
  // Opaque handles
  // ============================================================

  typedef void* LrvxStrategyHandle;
  typedef void* LrvxRegistryHandle;
  typedef void* LrvxBookHandle;
  typedef void* LrvxSimulatedExecutorHandle;
  typedef void* LrvxPositionTrackerHandle;
  typedef void* LrvxPositionGroupHandle;
  typedef void* LrvxOrderTrackerHandle;
  typedef void* LrvxOrderJourneyTracerHandle;
  typedef void* LrvxFootprintHandle;
  typedef void* LrvxVolumeProfileHandle;
  typedef void* LrvxMarketProfileHandle;
  typedef void* LrvxCompositeBookHandle;
  typedef void* LrvxCurveHandle;       // an INTokenCurve (an AMM pool's exact pricing)
  typedef void* LrvxPoolTapeHandle;    // a pool-state tape being built (delta log)
  typedef void* LrvxPoolReplayHandle;  // the result of replaying a pool-state tape

  // ============================================================
  // Flat event structs (C-compatible, no C++ dependencies)
  // ============================================================

  typedef struct
  {
    uint32_t symbol;
    int64_t price_raw;     // Price * 1e8
    int64_t quantity_raw;  // Quantity * 1e8
    uint8_t is_buy;
    int64_t exchange_ts_ns;
  } LrvxTradeData;

  typedef struct
  {
    int64_t price_raw;
    int64_t quantity_raw;
  } LrvxBookLevel;

  typedef struct
  {
    int64_t bid_price_raw;  // best bid; a price only when has_bid is 1, else 0
    int64_t bid_qty_raw;    // size resting at the best bid, 0 when the book cannot say
    int64_t ask_price_raw;  // best ask; a price only when has_ask is 1, else 0
    int64_t ask_qty_raw;    // size resting at the best ask, 0 when the book cannot say
    int64_t mid_raw;        // mid price; a price only when both flags are 1, else 0
    int64_t spread_raw;     // spread; a price only when both flags are 1, else 0
    uint8_t has_bid;        // 1 when the bid side has a best level, at any price, 0 included
    uint8_t has_ask;        // 1 when the ask side has a best level, at any price, 0 included
  } LrvxBookSnapshot;

  typedef struct
  {
    uint32_t symbol;
    int64_t exchange_ts_ns;
    LrvxBookSnapshot snapshot;
  } LrvxBookData;

  // OHLC bar event. bar_type: 0=Time, 1=Tick, 2=Volume, 3=Renko, 4=Range,
  // 5=HeikinAshi, 6=BpsRange (mirrors lrvx::BarType in aggregator/bar.h).
  // bar_type_param: interval_ns / tick count / volume threshold depending on type.
  // close_reason: 0=Threshold, 1=Gap, 2=Forced, 3=Warmup.
  typedef struct
  {
    uint32_t symbol;
    uint8_t bar_type;
    uint8_t close_reason;
    uint8_t _pad[2];
    uint64_t bar_type_param;
    int64_t open_raw;
    int64_t high_raw;
    int64_t low_raw;
    int64_t close_raw;
    int64_t volume_raw;
    int64_t buy_volume_raw;
    int64_t trade_count_raw;
    int64_t start_time_ns;
    int64_t end_time_ns;
  } LrvxBarData;

  // has_avg_entry_price is 0 when the position manager reports no cost basis:
  // a flat position, or a manager that keeps none. avg_entry_price_raw is
  // then 0 and means nothing, and bindings surface the unrealized PnL built
  // on it as NaN. The flag sits in padding the struct already had.
  /* book is the touch, which a TOP_OF_BOOK frame can refresh without a
   * dispatch. last_update_ns stamps dispatched events; last_touch_ns stamps
   * the touch itself, so the two can differ. */
  typedef struct
  {
    uint32_t symbol_id;
    uint8_t has_avg_entry_price;
    int64_t position_raw;
    int64_t avg_entry_price_raw;
    int64_t last_trade_price_raw;
    int64_t last_update_ns;
    LrvxBookSnapshot book;
    int64_t last_touch_ns;
  } LrvxSymbolContext;

  // ============================================================
  // Callback function pointer types
  // ============================================================

  typedef struct
  {
    uint64_t order_id;
    uint32_t symbol_id;
    uint8_t side;        // 0 BUY, 1 SELL
    uint8_t order_type;  // 0 LIMIT, 1 MARKET, 2 STOP_MARKET, ...
    uint8_t status;      // LrvxOrderEventStatus
    uint8_t _pad;
    int64_t fill_qty_raw;
    int64_t fill_price_raw;
    int64_t exchange_ts_ns;
    const char* reject_reason;  // null when status != REJECTED
    int64_t queue_ahead_raw;    // queue position (backtest limit orders only)
    int64_t queue_total_raw;    // total quantity at the order's price level
    // Per-lifecycle-stage timestamps. Zero means the stage has not
    // fired yet for this order.
    int64_t submitted_at_ns;
    int64_t accepted_at_ns;
    int64_t first_fill_at_ns;
    int64_t last_fill_at_ns;
    int64_t canceled_at_ns;
    int64_t rejected_at_ns;
    int64_t triggered_at_ns;
    int64_t expired_at_ns;
    uint8_t is_maker;         // 0 taker, 1 maker; meaningful only for fill statuses
    uint8_t market_position;  // 0 unknown, 1 best, 2 behind_best, 3 mid_spread, 4 level_empty, 5 crossed
    int32_t distance_to_best_ticks;
    uint8_t _pad2[2];
  } LrvxOrderEventData;

  typedef void (*LrvxOnTradeCallback)(void* user_data, const LrvxSymbolContext* ctx,
                                      const LrvxTradeData* trade);
  typedef void (*LrvxOnBookCallback)(void* user_data, const LrvxSymbolContext* ctx,
                                     const LrvxBookData* book);
  typedef void (*LrvxOnBarCallback)(void* user_data, const LrvxSymbolContext* ctx,
                                    const LrvxBarData* bar);
  typedef void (*LrvxOnFillCallback)(void* user_data, const LrvxSymbolContext* ctx,
                                     const LrvxOrderEventData* ev);
  typedef void (*LrvxOnOrderUpdateCallback)(void* user_data, const LrvxSymbolContext* ctx,
                                            const LrvxOrderEventData* ev);
  typedef void (*LrvxOnStartCallback)(void* user_data);
  typedef void (*LrvxOnStopCallback)(void* user_data);

  typedef void (*LrvxOnQueuePositionChangeCallback)(void* user_data,
                                                    const LrvxSymbolContext* ctx,
                                                    const LrvxOrderEventData* ev);
  typedef void (*LrvxOnMarketPositionChangeCallback)(void* user_data,
                                                     const LrvxSymbolContext* ctx,
                                                     const LrvxOrderEventData* ev);

  typedef struct
  {
    LrvxOnTradeCallback on_trade;
    LrvxOnBookCallback on_book;
    LrvxOnBarCallback on_bar;
    LrvxOnStartCallback on_start;
    LrvxOnStopCallback on_stop;
    LrvxOnFillCallback on_fill;
    LrvxOnOrderUpdateCallback on_order_update;
    LrvxOnQueuePositionChangeCallback on_queue_position_change;
    LrvxOnMarketPositionChangeCallback on_market_position_change;
    void* user_data;
  } LrvxStrategyCallbacks;

  // ============================================================
  // Symbol registry
  // ============================================================

  LRVX_EXPORT(group = "symbol_registry")
  LrvxRegistryHandle lrvx_registry_create(void);
  LRVX_EXPORT(group = "symbol_registry")
  void lrvx_registry_destroy(LrvxRegistryHandle registry);
  LRVX_EXPORT(group = "symbol_registry")
  uint32_t lrvx_registry_add_symbol(LrvxRegistryHandle registry, const char* exchange,
                                    const char* name, double tick_size);

  // Symbol name resolution
  LRVX_EXPORT(group = "symbol_registry")
  uint8_t lrvx_registry_get_symbol_id(LrvxRegistryHandle registry, const char* exchange,
                                      const char* name, uint32_t* id_out);
  LRVX_EXPORT(group = "symbol_registry")
  uint8_t lrvx_registry_get_symbol_name(LrvxRegistryHandle registry, uint32_t symbol_id,
                                        char* exchange_out, size_t exchange_len, char* name_out,
                                        size_t name_len);
  LRVX_EXPORT(group = "symbol_registry")
  uint32_t lrvx_registry_symbol_count(LrvxRegistryHandle registry);

  // ============================================================
  // Strategy lifecycle
  // ============================================================

  LRVX_EXPORT(group = "strategy_lifecycle")
  LrvxStrategyHandle lrvx_strategy_create(uint32_t id, const uint32_t* symbols,
                                          uint32_t num_symbols, LrvxRegistryHandle registry,
                                          LrvxStrategyCallbacks callbacks);
  LRVX_EXPORT(group = "strategy_lifecycle")
  void lrvx_strategy_destroy(LrvxStrategyHandle strategy);

  // Atomically replace the strategy's callback set without dropping
  // any subscriptions, in-flight orders, or open WebSocket / gRPC
  // connections. The next dispatched event sees the new callbacks.
  // Lifecycle: on_stop fires on the old user_data before the swap;
  // on_start fires on the new user_data after. Use this to hot-reload
  // strategy logic in production without tearing down the connector.
  LRVX_EXPORT(group = "strategy_lifecycle")
  void lrvx_strategy_replace_callbacks(LrvxStrategyHandle strategy,
                                       LrvxStrategyCallbacks callbacks);

  // Pointer-variant constructors for FFIs that cannot pass a struct
  // by value. The `_p` form takes a pointer to the same struct;
  // calling `*p` to read is equivalent to passing the struct directly.
  // These exist alongside the by-value variants so existing
  // pybind11/NAPI/QuickJS callers keep working unchanged. (Used by
  // Codon today.)
  LRVX_EXPORT(group = "strategy_lifecycle")
  LrvxStrategyHandle lrvx_strategy_create_p(uint32_t id, const uint32_t* symbols,
                                            uint32_t num_symbols, LrvxRegistryHandle registry,
                                            const LrvxStrategyCallbacks* callbacks);
  LRVX_EXPORT(group = "strategy_lifecycle")
  void lrvx_strategy_replace_callbacks_p(LrvxStrategyHandle strategy,
                                         const LrvxStrategyCallbacks* callbacks);

  // ============================================================
  // Signal emission (returns OrderId, 0 on failure)
  // ============================================================

  LRVX_EXPORT(group = "signal_emission")
  uint64_t lrvx_emit_market_buy(LrvxStrategyHandle s, uint32_t symbol, int64_t qty_raw);
  LRVX_EXPORT(group = "signal_emission")
  uint64_t lrvx_emit_market_sell(LrvxStrategyHandle s, uint32_t symbol, int64_t qty_raw);
  LRVX_EXPORT(group = "signal_emission")
  uint64_t lrvx_emit_limit_buy(LrvxStrategyHandle s, uint32_t symbol, int64_t price_raw,
                               int64_t qty_raw);
  LRVX_EXPORT(group = "signal_emission")
  uint64_t lrvx_emit_limit_sell(LrvxStrategyHandle s, uint32_t symbol, int64_t price_raw,
                                int64_t qty_raw);
  LRVX_EXPORT(group = "signal_emission")
  void lrvx_emit_cancel(LrvxStrategyHandle s, uint64_t order_id);
  LRVX_EXPORT(group = "signal_emission")
  void lrvx_emit_cancel_all(LrvxStrategyHandle s, uint32_t symbol);
  LRVX_EXPORT(group = "signal_emission")
  void lrvx_emit_modify(LrvxStrategyHandle s, uint64_t order_id, int64_t new_price_raw,
                        int64_t new_qty_raw);
  LRVX_EXPORT(group = "signal_emission")
  uint64_t lrvx_emit_stop_market(LrvxStrategyHandle s, uint32_t symbol, uint8_t side,
                                 int64_t trigger_raw, int64_t qty_raw);
  LRVX_EXPORT(group = "signal_emission")
  uint64_t lrvx_emit_stop_limit(LrvxStrategyHandle s, uint32_t symbol, uint8_t side,
                                int64_t trigger_raw, int64_t limit_raw, int64_t qty_raw);
  LRVX_EXPORT(group = "signal_emission")
  uint64_t lrvx_emit_take_profit_market(LrvxStrategyHandle s, uint32_t symbol, uint8_t side,
                                        int64_t trigger_raw, int64_t qty_raw);
  LRVX_EXPORT(group = "signal_emission")
  uint64_t lrvx_emit_trailing_stop(LrvxStrategyHandle s, uint32_t symbol, uint8_t side,
                                   int64_t offset_raw, int64_t qty_raw);
  LRVX_EXPORT(group = "signal_emission")
  uint64_t lrvx_emit_trailing_stop_percent(LrvxStrategyHandle s, uint32_t symbol, uint8_t side,
                                           int32_t callback_bps, int64_t qty_raw);
  LRVX_EXPORT(group = "signal_emission")
  uint64_t lrvx_emit_take_profit_limit(LrvxStrategyHandle s, uint32_t symbol, uint8_t side,
                                       int64_t trigger_raw, int64_t limit_raw, int64_t qty_raw);
  LRVX_EXPORT(group = "signal_emission")
  uint64_t lrvx_emit_limit_buy_tif(LrvxStrategyHandle s, uint32_t symbol, int64_t price_raw,
                                   int64_t qty_raw, uint8_t time_in_force);
  LRVX_EXPORT(group = "signal_emission")
  uint64_t lrvx_emit_limit_sell_tif(LrvxStrategyHandle s, uint32_t symbol, int64_t price_raw,
                                    int64_t qty_raw, uint8_t time_in_force);
  LRVX_EXPORT(group = "signal_emission")
  uint64_t lrvx_emit_close_position(LrvxStrategyHandle s, uint32_t symbol);

  // ============================================================
  // Context queries
  // ============================================================

  LRVX_EXPORT(group = "context_queries")
  int64_t lrvx_position_raw(LrvxStrategyHandle s, uint32_t symbol);
  LRVX_EXPORT(group = "context_queries")
  int64_t lrvx_last_trade_price_raw(LrvxStrategyHandle s, uint32_t symbol);
  LRVX_EXPORT(group = "context_queries")
  /* The quote accessors below answer from the touch — the same top the
   * LrvxBookSnapshot in every callback carries — so they also follow
   * TOP_OF_BOOK frames the depth book ignores. */
  int64_t lrvx_best_bid_raw(LrvxStrategyHandle s, uint32_t symbol);
  LRVX_EXPORT(group = "context_queries")
  int64_t lrvx_best_ask_raw(LrvxStrategyHandle s, uint32_t symbol);
  LRVX_EXPORT(group = "context_queries")
  int64_t lrvx_mid_price_raw(LrvxStrategyHandle s, uint32_t symbol);
  /* The _opt trio answers the question the three above cannot: they return 0
   * both when the side is empty and when the best quote is a price of exactly
   * 0.0, and a book quoting through zero reaches that price. Here the return
   * value is the presence flag -- 1 with the raw price written to price_out,
   * 0 with price_out untouched -- so "no quote" has its own answer. price_out
   * may be NULL when only the flag is wanted. The three int64_t forms stay as
   * they are for callers that cannot see below zero. */
  LRVX_EXPORT(group = "context_queries")
  uint8_t lrvx_best_bid_raw_opt(LrvxStrategyHandle s, uint32_t symbol, int64_t* price_out);
  LRVX_EXPORT(group = "context_queries")
  uint8_t lrvx_best_ask_raw_opt(LrvxStrategyHandle s, uint32_t symbol, int64_t* price_out);
  LRVX_EXPORT(group = "context_queries")
  uint8_t lrvx_mid_price_raw_opt(LrvxStrategyHandle s, uint32_t symbol, int64_t* price_out);
  /* Defined by a header that declares the _opt trio, so a binding can compile
   * against either. */
  // lrvx::export_macro(group="feature_flags")
  #define LRVX_HAS_OPTIONAL_RAW_BEST_QUOTE 1
  LRVX_EXPORT(group = "context_queries")
  void lrvx_get_symbol_context(LrvxStrategyHandle s, uint32_t symbol, LrvxSymbolContext* out);
  LRVX_EXPORT(group = "context_queries")
  int32_t lrvx_get_order_status(LrvxStrategyHandle s, uint64_t order_id);

  // ============================================================
  // Fixed-point conversion helpers
  // ============================================================

  LRVX_EXPORT(group = "fixed_point")
  int64_t lrvx_price_from_double(double value);
  LRVX_EXPORT(group = "fixed_point")
  double lrvx_price_to_double(int64_t raw);
  LRVX_EXPORT(group = "fixed_point")
  int64_t lrvx_quantity_from_double(double value);
  LRVX_EXPORT(group = "fixed_point")
  double lrvx_quantity_to_double(int64_t raw);

  // ============================================================
  // Indicator functions (stateless, array-in/array-out)
  // ============================================================

  LRVX_EXPORT(group = "indicator_functions")
  void lrvx_indicator_ema(const double* input, size_t len, size_t period, double* output);
  LRVX_EXPORT(group = "indicator_functions")
  void lrvx_indicator_sma(const double* input, size_t len, size_t period, double* output);
  LRVX_EXPORT(group = "indicator_functions")
  void lrvx_indicator_rsi(const double* input, size_t len, size_t period, double* output);
  LRVX_EXPORT(group = "indicator_functions")
  void lrvx_indicator_atr(const double* high, const double* low, const double* close, size_t len,
                          size_t period, double* output);
  LRVX_EXPORT(group = "indicator_functions")
  void lrvx_indicator_macd(const double* input, size_t len, size_t fast_period,
                           size_t slow_period, size_t signal_period, double* macd_out,
                           double* signal_out, double* hist_out);
  LRVX_EXPORT(group = "indicator_functions")
  void lrvx_indicator_bollinger(const double* input, size_t len, size_t period, double multiplier,
                                double* upper, double* middle, double* lower);

  // Moving average variants
  LRVX_EXPORT(group = "indicator_functions")
  void lrvx_indicator_rma(const double* input, size_t len, size_t period, double* output);
  LRVX_EXPORT(group = "indicator_functions")
  void lrvx_indicator_dema(const double* input, size_t len, size_t period, double* output);
  LRVX_EXPORT(group = "indicator_functions")
  void lrvx_indicator_tema(const double* input, size_t len, size_t period, double* output);
  LRVX_EXPORT(group = "indicator_functions")
  void lrvx_indicator_kama(const double* input, size_t len, size_t period, size_t fast, size_t slow,
                           double* output);

  // Trend
  LRVX_EXPORT(group = "indicator_functions")
  void lrvx_indicator_slope(const double* input, size_t len, size_t length, double* output);
  LRVX_EXPORT(group = "indicator_functions")
  void lrvx_indicator_adx(const double* high, const double* low, const double* close, size_t len,
                          size_t period, double* adx_out, double* plus_di_out,
                          double* minus_di_out);

  // Oscillators
  LRVX_EXPORT(group = "indicator_functions")
  void lrvx_indicator_cci(const double* high, const double* low, const double* close, size_t len,
                          size_t period, double* output);
  LRVX_EXPORT(group = "indicator_functions")
  void lrvx_indicator_stochastic(const double* high, const double* low, const double* close,
                                 size_t len, size_t k_period, size_t d_period, double* k_out,
                                 double* d_out);
  LRVX_EXPORT(group = "indicator_functions")
  void lrvx_indicator_chop(const double* high, const double* low, const double* close, size_t len,
                           size_t period, double* output);

  // Volume
  LRVX_EXPORT(group = "indicator_functions")
  void lrvx_indicator_obv(const double* close, const double* volume, size_t len, double* output);
  LRVX_EXPORT(group = "indicator_functions")
  void lrvx_indicator_vwap(const double* close, const double* volume, size_t len, size_t window,
                           double* output);
  LRVX_EXPORT(group = "indicator_functions")
  void lrvx_indicator_cvd(const double* open, const double* high, const double* low,
                          const double* close, const double* volume, size_t len, double* output);

  // Statistical
  LRVX_EXPORT(group = "indicator_functions")
  void lrvx_indicator_skewness(const double* input, size_t len, size_t period, double* output);
  LRVX_EXPORT(group = "indicator_functions")
  void lrvx_indicator_kurtosis(const double* input, size_t len, size_t period, double* output);
  LRVX_EXPORT(group = "indicator_functions")
  void lrvx_indicator_parkinson_vol(const double* high, const double* low, size_t len,
                                    size_t period, double* output);
  LRVX_EXPORT(group = "indicator_functions")
  void lrvx_indicator_rogers_satchell_vol(const double* open, const double* high, const double* low,
                                          const double* close, size_t len, size_t period,
                                          double* output);
  LRVX_EXPORT(group = "indicator_functions")
  void lrvx_indicator_rolling_zscore(const double* input, size_t len, size_t period,
                                     double* output);
  LRVX_EXPORT(group = "indicator_functions")
  void lrvx_indicator_shannon_entropy(const double* input, size_t len, size_t period, size_t bins,
                                      double* output);
  LRVX_EXPORT(group = "indicator_functions")
  void lrvx_indicator_correlation(const double* x, const double* y, size_t len, size_t period,
                                  double* output);

  // Augmented Dickey-Fuller test. `regression` accepts: "n", "c", "ct".
  // Writes the t-statistic, approximate p-value, and AIC-selected lag.
  LRVX_EXPORT(group = "indicator_functions")
  void lrvx_indicator_adf(const double* input, size_t len, size_t max_lag, const char* regression,
                          double* test_stat_out, double* p_value_out, size_t* used_lag_out);
  // AutoCorrelation: Pearson correlation between x[t] and x[t-lag] over a
  // rolling window. First valid index is (window + lag - 1).
  LRVX_EXPORT(group = "indicator_functions")
  void lrvx_indicator_autocorrelation(const double* input, size_t len, size_t window, size_t lag,
                                      double* output);

  // ============================================================
  // Targets (forward-looking labels, batch only)
  //
  // Targets read into the future relative to t. They are intentionally
  // separate from indicators: feeding them into a live update loop is a
  // look-ahead-bias bug.
  // ============================================================

  LRVX_EXPORT(group = "targets")
  void lrvx_target_future_return(const double* close, size_t len, size_t horizon, double* output);
  LRVX_EXPORT(group = "targets")
  void lrvx_target_future_ctc_volatility(const double* close, size_t len, size_t horizon,
                                         double* output);
  LRVX_EXPORT(group = "targets")
  void lrvx_target_future_linear_slope(const double* close, size_t len, size_t horizon,
                                       double* output);

  // ============================================================
  // IndicatorGraph (batch)
  //
  // Compose batch indicators with shared intermediate caching. Nodes are
  // user-supplied callbacks; require() walks the DAG in topological order.
  //
  //   LrvxIndicatorGraphHandle g = lrvx_indicator_graph_create();
  //   lrvx_indicator_graph_set_bars(g, sym, close, high, low, volume, n);
  //   lrvx_indicator_graph_add_node(g, "ema50", NULL, 0, ema_fn, ema_state);
  //   const double* out = lrvx_indicator_graph_require(g, sym, "ema50", &len);
  //   lrvx_indicator_graph_destroy(g);
  // ============================================================

  typedef void* LrvxIndicatorGraphHandle;

  // Compute callback. Output array must be allocated by the callback (or pre-
  // allocated by the user). The host language is responsible for keeping it
  // alive until the next graph call. Returning a pointer with len = 0 signals
  // an empty result.
  typedef const double* (*LrvxGraphNodeFn)(void* user_data, LrvxIndicatorGraphHandle g,
                                           uint32_t symbol, size_t* out_len);

  LRVX_EXPORT(group = "indicatorgraph")
  LrvxIndicatorGraphHandle lrvx_indicator_graph_create(void);
  LRVX_EXPORT(group = "indicatorgraph")
  void lrvx_indicator_graph_destroy(LrvxIndicatorGraphHandle g);

  // Pass NULL for high/low/volume to default high=low=close and volume=0.
  LRVX_EXPORT(group = "indicatorgraph")
  void lrvx_indicator_graph_set_bars(LrvxIndicatorGraphHandle g, uint32_t symbol,
                                     const double* close, const double* high, const double* low,
                                     const double* volume, size_t len);

  // deps: array of `num_deps` C-string node names (or NULL if num_deps == 0).
  LRVX_EXPORT(group = "indicatorgraph")
  void lrvx_indicator_graph_add_node(LrvxIndicatorGraphHandle g, const char* name,
                                     const char* const* deps, size_t num_deps,
                                     LrvxGraphNodeFn fn, void* user_data);

  // Returns a pointer to the cached output for (symbol, name) and writes its
  // length to *len_out. The pointer is owned by the graph and is valid until
  // the next invalidate / destroy call. Returns NULL on error (unknown node,
  // cycle, etc.).
  LRVX_EXPORT(group = "indicatorgraph")
  const double* lrvx_indicator_graph_require(LrvxIndicatorGraphHandle g, uint32_t symbol,
                                             const char* name, size_t* len_out);

  // Same as require but only returns a pointer if the node has already been
  // computed; never triggers compute. Returns NULL if not yet computed.
  LRVX_EXPORT(group = "indicatorgraph")
  const double* lrvx_indicator_graph_get(LrvxIndicatorGraphHandle g, uint32_t symbol,
                                         const char* name, size_t* len_out);

  // Field accessors return cached double arrays for the symbol's bars.
  LRVX_EXPORT(group = "indicatorgraph")
  const double* lrvx_indicator_graph_close(LrvxIndicatorGraphHandle g, uint32_t symbol,
                                           size_t* len_out);
  LRVX_EXPORT(group = "indicatorgraph")
  const double* lrvx_indicator_graph_high(LrvxIndicatorGraphHandle g, uint32_t symbol,
                                          size_t* len_out);
  LRVX_EXPORT(group = "indicatorgraph")
  const double* lrvx_indicator_graph_low(LrvxIndicatorGraphHandle g, uint32_t symbol,
                                         size_t* len_out);
  LRVX_EXPORT(group = "indicatorgraph")
  const double* lrvx_indicator_graph_volume(LrvxIndicatorGraphHandle g, uint32_t symbol,
                                            size_t* len_out);

  LRVX_EXPORT(group = "indicatorgraph")
  void lrvx_indicator_graph_invalidate(LrvxIndicatorGraphHandle g, uint32_t symbol);
  LRVX_EXPORT(group = "indicatorgraph")
  void lrvx_indicator_graph_invalidate_all(LrvxIndicatorGraphHandle g);

  // ── Streaming path on the same handle ────────────────────────────
  // Same graph object, both APIs. step() appends one bar; node cache is
  // invalidated; current() returns the latest value of a named node.

  LRVX_EXPORT(group = "indicatorgraph")
  void lrvx_indicator_graph_step(LrvxIndicatorGraphHandle g, uint32_t symbol, double open,
                                 double high, double low, double close, double volume);
  LRVX_EXPORT(group = "indicatorgraph")
  double lrvx_indicator_graph_current(LrvxIndicatorGraphHandle g, uint32_t symbol,
                                      const char* name);
  LRVX_EXPORT(group = "indicatorgraph")
  uint32_t lrvx_indicator_graph_bar_count(LrvxIndicatorGraphHandle g, uint32_t symbol);
  LRVX_EXPORT(group = "indicatorgraph")
  void lrvx_indicator_graph_reset(LrvxIndicatorGraphHandle g, uint32_t symbol);
  LRVX_EXPORT(group = "indicatorgraph")
  void lrvx_indicator_graph_reset_all(LrvxIndicatorGraphHandle g);

  // ── Deprecated streaming-graph shim ──────────────────────────────
  // The old separate StreamingIndicatorGraph type has been collapsed
  // into IndicatorGraph. These names are now thin forwarders to the new
  // ones and will be removed in a future major version. New code: use
  // lrvx_indicator_graph_* (one handle, both compute and step).

  typedef LrvxIndicatorGraphHandle LrvxStreamingGraphHandle;

  LRVX_EXPORT(group = "indicatorgraph")
  LrvxStreamingGraphHandle lrvx_streaming_graph_create(void);
  LRVX_EXPORT(group = "indicatorgraph")
  void lrvx_streaming_graph_destroy(LrvxStreamingGraphHandle sg);
  LRVX_EXPORT(group = "indicatorgraph")
  void lrvx_streaming_graph_add_node(LrvxStreamingGraphHandle sg, const char* name,
                                     const char* const* deps, size_t num_deps,
                                     LrvxGraphNodeFn fn, void* user_data);
  LRVX_EXPORT(group = "indicatorgraph")
  void lrvx_streaming_graph_step(LrvxStreamingGraphHandle sg, uint32_t symbol, double open,
                                 double high, double low, double close, double volume);
  LRVX_EXPORT(group = "indicatorgraph")
  double lrvx_streaming_graph_current(LrvxStreamingGraphHandle sg, uint32_t symbol,
                                      const char* name);
  LRVX_EXPORT(group = "indicatorgraph")
  uint32_t lrvx_streaming_graph_bar_count(LrvxStreamingGraphHandle sg, uint32_t symbol);
  LRVX_EXPORT(group = "indicatorgraph")
  void lrvx_streaming_graph_reset(LrvxStreamingGraphHandle sg, uint32_t symbol);
  LRVX_EXPORT(group = "indicatorgraph")
  void lrvx_streaming_graph_reset_all(LrvxStreamingGraphHandle sg);
  LRVX_EXPORT(group = "indicatorgraph")
  const double* lrvx_streaming_graph_close(LrvxStreamingGraphHandle sg, uint32_t symbol,
                                           size_t* len_out);
  LRVX_EXPORT(group = "indicatorgraph")
  const double* lrvx_streaming_graph_high(LrvxStreamingGraphHandle sg, uint32_t symbol,
                                          size_t* len_out);
  LRVX_EXPORT(group = "indicatorgraph")
  const double* lrvx_streaming_graph_low(LrvxStreamingGraphHandle sg, uint32_t symbol,
                                         size_t* len_out);
  LRVX_EXPORT(group = "indicatorgraph")
  const double* lrvx_streaming_graph_volume(LrvxStreamingGraphHandle sg, uint32_t symbol,
                                            size_t* len_out);

  // ============================================================
  // Order book
  // ============================================================

  LRVX_EXPORT(group = "order_book")
  LrvxBookHandle lrvx_book_create(double tick_size);
  LRVX_EXPORT(group = "order_book")
  void lrvx_book_destroy(LrvxBookHandle book);
  LRVX_EXPORT(group = "order_book")
  void lrvx_book_apply_snapshot(LrvxBookHandle book, const double* bid_prices,
                                const double* bid_qtys, size_t bid_len, const double* ask_prices,
                                const double* ask_qtys, size_t ask_len);
  LRVX_EXPORT(group = "order_book")
  void lrvx_book_apply_delta(LrvxBookHandle book, const double* bid_prices, const double* bid_qtys,
                             size_t bid_len, const double* ask_prices, const double* ask_qtys,
                             size_t ask_len);
  LRVX_EXPORT(group = "order_book")
  uint8_t lrvx_book_best_bid(LrvxBookHandle book, double* price_out);
  LRVX_EXPORT(group = "order_book")
  uint8_t lrvx_book_best_ask(LrvxBookHandle book, double* price_out);
  LRVX_EXPORT(group = "order_book")
  uint8_t lrvx_book_mid(LrvxBookHandle book, double* price_out);
  LRVX_EXPORT(group = "order_book")
  uint8_t lrvx_book_spread(LrvxBookHandle book, double* spread_out);
  LRVX_EXPORT(group = "order_book")
  double lrvx_book_bid_at_price(LrvxBookHandle book, double price);
  LRVX_EXPORT(group = "order_book")
  double lrvx_book_ask_at_price(LrvxBookHandle book, double price);
  LRVX_EXPORT(group = "order_book")
  uint8_t lrvx_book_is_crossed(LrvxBookHandle book);
  LRVX_EXPORT(group = "order_book")
  void lrvx_book_clear(LrvxBookHandle book);

  // ============================================================
  // Simulated executor (backtesting)
  // ============================================================

  LRVX_EXPORT(group = "simulated_executor")
  LrvxSimulatedExecutorHandle lrvx_simulated_executor_create(void);
  LRVX_EXPORT(group = "simulated_executor")
  void lrvx_simulated_executor_destroy(LrvxSimulatedExecutorHandle executor);
  LRVX_EXPORT(group = "simulated_executor")
  void lrvx_simulated_executor_submit_order(LrvxSimulatedExecutorHandle executor, uint64_t id, uint8_t side,
                                            double price, double quantity, uint8_t order_type,
                                            uint32_t symbol);

  // Extended submit with explicit TIF / reduce-only / GTD expiry.
  // tif: 0=GTC, 1=IOC, 2=FOK, 3=GTD, 4=POST_ONLY.
  // reduce_only: 1 to enforce reduce-only against the simulator
  // net position; rejects if would open/grow, truncates if would
  // overshoot flat.
  // expires_at_ns: absolute deadline (nanoseconds since epoch) for
  // GTD; ignored for other TIFs.
  LRVX_EXPORT(group = "simulated_executor")
  void lrvx_simulated_executor_submit_order_ex(LrvxSimulatedExecutorHandle executor,
                                               uint64_t id, uint8_t side, double price,
                                               double quantity, uint8_t order_type,
                                               uint32_t symbol, uint8_t tif,
                                               uint8_t reduce_only, int64_t expires_at_ns);

  // Native bracket primitive. Submits the entry leg and arms a
  // take-profit + stop pair on entry fill. The simulator manages
  // the state machine: first child to fill cancels the other;
  // cancelling the bracket cancels every still-live leg.
  // OrderIds for the legs are derived: entry = bracket_id*3+0,
  // take-profit = bracket_id*3+1, stop = bracket_id*3+2.
  // entry_type / tp_type / stop_type follow the OrderType enum
  // (0=LIMIT, 1=MARKET, 2=STOP_MARKET, 3=STOP_LIMIT).
  LRVX_EXPORT(group = "simulated_executor")
  void lrvx_simulated_executor_submit_bracket(
      LrvxSimulatedExecutorHandle executor, uint64_t bracket_id, uint32_t symbol,
      uint8_t entry_side, uint8_t entry_type, double entry_price, double quantity,
      uint8_t tp_side, uint8_t tp_type, double tp_price,
      uint8_t stop_side, uint8_t stop_type, double stop_trigger_price);

  LRVX_EXPORT(group = "simulated_executor")
  void lrvx_simulated_executor_cancel_bracket(LrvxSimulatedExecutorHandle executor,
                                              uint64_t bracket_id);

  // Read the bracket state. Returns 0=PENDING_ENTRY, 1=ENTRY_FILLED,
  // 2=TP_FILLED, 3=STOP_FILLED, 4=CANCELED. Unknown bracketId
  // returns PENDING_ENTRY.
  LRVX_EXPORT(group = "simulated_executor")
  uint8_t lrvx_simulated_executor_bracket_state(LrvxSimulatedExecutorHandle executor,
                                                uint64_t bracket_id);

  // Bracket child-arm policy. 0=OnFullFill (default), 1=OnPartialFill.
  LRVX_EXPORT(group = "simulated_executor")
  void lrvx_simulated_executor_set_bracket_child_arm_mode(
      LrvxSimulatedExecutorHandle executor, uint8_t mode);

  // Submit a native iceberg order. order_type is forced to ICEBERG
  // internally. `visible_quantity` is the slice exposed to the book;
  // the hidden remainder (total - visible) refreshes automatically
  // as the visible tranche fills. Refresh latency is configured via
  // lrvx_simulated_executor_set_iceberg_refresh_latency.
  LRVX_EXPORT(group = "simulated_executor")
  void lrvx_simulated_executor_submit_iceberg(LrvxSimulatedExecutorHandle executor,
                                              uint64_t id, uint8_t side, double price,
                                              double total_quantity,
                                              double visible_quantity, uint32_t symbol);

  // Set the default refresh latency in nanoseconds for native
  // iceberg orders submitted after this call. Most venues are
  // instant (0); a few inject 1-2ms.
  LRVX_EXPORT(group = "simulated_executor")
  void lrvx_simulated_executor_set_iceberg_refresh_latency(
      LrvxSimulatedExecutorHandle executor, int64_t latency_ns);

  // Diagnostic: hidden remainder (raw fixed-point) for an iceberg
  // order, or 0 if the order is not an iceberg or has been removed.
  LRVX_EXPORT(group = "simulated_executor")
  int64_t lrvx_simulated_executor_iceberg_hidden_remaining_raw(
      LrvxSimulatedExecutorHandle executor, uint64_t id);

  // Per-refresh visible-slice size jitter as a fraction
  // (0.0 = deterministic, 0.10 = ±10% uniform). Default 0.
  LRVX_EXPORT(group = "simulated_executor")
  void lrvx_simulated_executor_set_iceberg_size_randomisation_pct(
      LrvxSimulatedExecutorHandle executor, double pct);

  // Queue priority on refresh. 0=Back (default),
  // 1=Retain (CME options).
  LRVX_EXPORT(group = "simulated_executor")
  void lrvx_simulated_executor_set_iceberg_priority_mode(
      LrvxSimulatedExecutorHandle executor, uint8_t mode);

  // Seed the size-jitter RNG for reproducible refresh
  // sequences. Default seed is fixed at construction time.
  LRVX_EXPORT(group = "simulated_executor")
  void lrvx_simulated_executor_set_iceberg_jitter_seed(
      LrvxSimulatedExecutorHandle executor, uint64_t seed);
  LRVX_EXPORT(group = "simulated_executor")
  void lrvx_simulated_executor_cancel_order(LrvxSimulatedExecutorHandle executor, uint64_t order_id);
  LRVX_EXPORT(group = "simulated_executor")
  void lrvx_simulated_executor_cancel_all(LrvxSimulatedExecutorHandle executor, uint32_t symbol);
  LRVX_EXPORT(group = "simulated_executor")
  void lrvx_simulated_executor_on_bar(LrvxSimulatedExecutorHandle executor, uint32_t symbol, double close_price);

  // Open-aware form. Moves the market to the open (releasing any order held
  // from a bar-callback window at the previous bar's open), then walks
  // low -> high -> close so resting stops/targets match the intrabar
  // extremes, not just the close. Use this, not the close-only
  // lrvx_simulated_executor_on_bar, to drive the executor by hand to the
  // same fills BacktestRunner::runBars produces -- runBars uses this
  // overload internally.
  LRVX_EXPORT(group = "simulated_executor")
  void lrvx_simulated_executor_on_bar_ohlc(LrvxSimulatedExecutorHandle executor, uint32_t symbol,
                                           double open_price, double high_price,
                                           double low_price, double close_price);

  // Bar-callback window. Matching an order submitted while a bar is being
  // shown to a strategy callback is look-ahead: the market has already been
  // walked to the bar's close by the time the callback runs. While the
  // window is open every arriving order is held instead of matched, and the
  // next call to lrvx_simulated_executor_on_bar_ohlc releases whatever is
  // held for that symbol at its open. Call begin before invoking a
  // hand-driven bar callback and end right after; unbalanced calls are
  // depth-counted so a re-entrant callback cannot close a window it did not
  // open.
  LRVX_EXPORT(group = "simulated_executor")
  void lrvx_simulated_executor_begin_bar_callback_window(LrvxSimulatedExecutorHandle executor);
  LRVX_EXPORT(group = "simulated_executor")
  void lrvx_simulated_executor_end_bar_callback_window(LrvxSimulatedExecutorHandle executor);

  // Drops fills, live/held/conditional orders, queue positions, net
  // positions, brackets, in-flight acks, market state and the visible
  // ladder, while keeping installed configuration (slippage, queue model,
  // latency distributions, rate limits, STP, callbacks, attached venue
  // availability). Seeded RNGs go back to their configured seeds. Lets a
  // caller run several hand-driven passes through one executor and have
  // each report only its own fills.
  LRVX_EXPORT(group = "simulated_executor")
  void lrvx_simulated_executor_reset(LrvxSimulatedExecutorHandle executor);

  LRVX_EXPORT(group = "simulated_executor")
  void lrvx_simulated_executor_on_trade(LrvxSimulatedExecutorHandle executor, uint32_t symbol, double price,
                                        uint8_t is_buy);
  LRVX_EXPORT(group = "simulated_executor")
  void lrvx_simulated_executor_advance_clock(LrvxSimulatedExecutorHandle executor, int64_t timestamp_ns);
  LRVX_EXPORT(group = "simulated_executor")
  uint32_t lrvx_simulated_executor_fill_count(LrvxSimulatedExecutorHandle executor);

  // ============================================================
  // Bar aggregation
  // ============================================================

  // close_reason: 0=Threshold, 1=Gap, 2=Forced, 3=Warmup (mirrors
  // lrvx::BarCloseReason, and LrvxBarData::close_reason above). Added in ABI
  // version 3: this is a struct shape change, so any code that constructs a
  // LrvxBar via positional aggregate initialization needs the new trailing
  // field supplied.
  typedef struct
  {
    int64_t start_time_ns;
    int64_t end_time_ns;
    int64_t open_raw;
    int64_t high_raw;
    int64_t low_raw;
    int64_t close_raw;
    int64_t volume_raw;
    int64_t buy_volume_raw;
    uint32_t trade_count;
    uint8_t close_reason;
  } LrvxBar;

  LRVX_EXPORT(group = "bar_aggregation")
  uint32_t lrvx_aggregate_time_bars(const int64_t* timestamps, const double* prices,
                                    const double* quantities, const uint8_t* is_buy, size_t len,
                                    double interval_seconds, LrvxBar* bars_out,
                                    uint32_t max_bars);
  LRVX_EXPORT(group = "bar_aggregation")
  uint32_t lrvx_aggregate_tick_bars(const int64_t* timestamps, const double* prices,
                                    const double* quantities, const uint8_t* is_buy, size_t len,
                                    uint32_t tick_count, LrvxBar* bars_out, uint32_t max_bars);
  LRVX_EXPORT(group = "bar_aggregation")
  uint32_t lrvx_aggregate_volume_bars(const int64_t* timestamps, const double* prices,
                                      const double* quantities, const uint8_t* is_buy, size_t len,
                                      double volume_threshold, LrvxBar* bars_out,
                                      uint32_t max_bars);

  // ============================================================
  // Multi-timeframe alignment helpers
  // ============================================================
  //
  // Read the per-(symbol, timeframe) bar ring the engine populates as
  // it dispatches BarEvents to the strategy. `bar_type` matches
  // LrvxBarData (0=Time, 1=Tick, ...); `param` is interval-ns for
  // time bars, count for tick bars, threshold for volume bars.

  LRVX_EXPORT(group = "multi_tf_helpers")
  uint8_t lrvx_strategy_last_closed_bar(LrvxStrategyHandle s, uint32_t symbol,
                                        uint8_t bar_type, uint64_t param,
                                        LrvxBar* out);
  LRVX_EXPORT(group = "multi_tf_helpers")
  uint32_t lrvx_strategy_last_n_closed_bars(LrvxStrategyHandle s, uint32_t symbol,
                                            uint8_t bar_type, uint64_t param,
                                            LrvxBar* bars_out, uint32_t max_bars);
  LRVX_EXPORT(group = "multi_tf_helpers")
  uint32_t lrvx_strategy_get_bar_ring_capacity(LrvxStrategyHandle s);
  LRVX_EXPORT(group = "multi_tf_helpers")
  void lrvx_strategy_set_bar_ring_capacity(LrvxStrategyHandle s, uint32_t capacity);

  // ============================================================
  // Multi-leg order group
  // ============================================================
  //
  // Passive state machine: legs + policy + recorded events go in,
  // aggregate state + recommended actions come out. The group does
  // no I/O — the caller wires the actions into the executor.

  typedef void* LrvxOrderGroupHandle;

  // policy: 0 = BestEffort, 1 = AllOrNothing, 2 = OneSided.
  LRVX_EXPORT(group = "order_group")
  LrvxOrderGroupHandle lrvx_order_group_create(uint64_t parent_signal_id, uint8_t policy);
  LRVX_EXPORT(group = "order_group")
  void lrvx_order_group_destroy(LrvxOrderGroupHandle h);

  // side: 0 = BUY, 1 = SELL. Returns leg index.
  LRVX_EXPORT(group = "order_group")
  uint32_t lrvx_order_group_add_market_leg(LrvxOrderGroupHandle h, uint32_t symbol,
                                           uint8_t side, int64_t qty_raw);
  LRVX_EXPORT(group = "order_group")
  uint32_t lrvx_order_group_add_limit_leg(LrvxOrderGroupHandle h, uint32_t symbol,
                                          uint8_t side, int64_t price_raw,
                                          int64_t qty_raw);

  LRVX_EXPORT(group = "order_group")
  uint32_t lrvx_order_group_leg_count(LrvxOrderGroupHandle h);
  LRVX_EXPORT(group = "order_group")
  uint8_t lrvx_order_group_leg_state(LrvxOrderGroupHandle h, uint32_t leg_index);
  LRVX_EXPORT(group = "order_group")
  int64_t lrvx_order_group_leg_filled_raw(LrvxOrderGroupHandle h, uint32_t leg_index);
  LRVX_EXPORT(group = "order_group")
  uint64_t lrvx_order_group_leg_order_id(LrvxOrderGroupHandle h, uint32_t leg_index);

  LRVX_EXPORT(group = "order_group")
  void lrvx_order_group_record_submit(LrvxOrderGroupHandle h, uint32_t leg_index,
                                      uint64_t order_id);
  LRVX_EXPORT(group = "order_group")
  void lrvx_order_group_record_fill(LrvxOrderGroupHandle h, uint32_t leg_index,
                                    int64_t cumulative_qty_raw);
  LRVX_EXPORT(group = "order_group")
  void lrvx_order_group_record_cancel(LrvxOrderGroupHandle h, uint32_t leg_index);
  LRVX_EXPORT(group = "order_group")
  void lrvx_order_group_record_failure(LrvxOrderGroupHandle h, uint32_t leg_index);
  LRVX_EXPORT(group = "order_group")
  void lrvx_order_group_record_replace_accepted(LrvxOrderGroupHandle h, uint32_t leg_index,
                                                uint64_t new_order_id);
  LRVX_EXPORT(group = "order_group")
  void lrvx_order_group_record_replace_rejected(LrvxOrderGroupHandle h, uint32_t leg_index);

  // Returns leg index for the given exchange order id, or UINT32_MAX
  // if no leg owns this id. Used to route a replace/fill event from
  // an executor back to the correct OrderGroup leg.
  LRVX_EXPORT(group = "order_group")
  uint32_t lrvx_order_group_find_leg_by_order_id(LrvxOrderGroupHandle h, uint64_t order_id);

  LRVX_EXPORT(group = "order_group")
  uint8_t lrvx_order_group_state(LrvxOrderGroupHandle h);

  // Recommended-actions output. Each action is laid out as 5 i64 slots:
  //   [0] kind (0=CancelLeg, 1=RevertLeg)
  //   [1] leg_index
  //   [2] order_id (CancelLeg) | symbol_id (RevertLeg)
  //   [3] side (RevertLeg, 0=BUY/1=SELL) | 0
  //   [4] qty_raw (RevertLeg) | 0
  // Returns number of actions written; bounded by max_actions.
  LRVX_EXPORT(group = "order_group")
  uint32_t lrvx_order_group_recommended_actions(LrvxOrderGroupHandle h,
                                                int64_t* actions_out,
                                                uint32_t max_actions);

  // Mark a leg's action as dispatched so it stops surfacing in
  // `recommended_actions`. Called by the auto-dispatch helper after
  // each emit. kind: 0 = CancelLeg, 1 = RevertLeg.
  LRVX_EXPORT(group = "order_group")
  void lrvx_order_group_mark_action_dispatched(LrvxOrderGroupHandle h, uint32_t leg_index,
                                               uint8_t kind);

  // Group-level risk limits. Zero in any field means "no cap on
  // that dimension". Pass marketRefPrices_raw with one entry per
  // leg in addMarketLeg order; entries for limit legs are ignored.
  LRVX_EXPORT(group = "order_group")
  void lrvx_order_group_set_risk_limits(LrvxOrderGroupHandle h,
                                        int64_t max_gross_notional_raw,
                                        double max_concentration_pct,
                                        int64_t max_leg_qty_raw);

  // Run the configured limits against the current legs. Returns 1 if
  // a breach was found, 0 otherwise. When 1, the breach rule + detail
  // are written into the caller-supplied buffers (truncated at the
  // given capacities). `equity` only matters when
  // max_concentration_pct > 0.
  LRVX_EXPORT(group = "order_group")
  uint8_t lrvx_order_group_precheck_submission(LrvxOrderGroupHandle h, double equity,
                                               const int64_t* market_ref_prices_raw,
                                               uint32_t market_ref_prices_len,
                                               char* rule_out, size_t rule_capacity,
                                               char* detail_out, size_t detail_capacity);

  // OneSided pair latency budget. After the leader submits, the
  // strategy waits up to `budget_ns` of feed-time for the leader's
  // ack before sending the follower. Zero disables the gate.
  LRVX_EXPORT(group = "order_group")
  void lrvx_order_group_set_pair_latency_budget_ns(LrvxOrderGroupHandle h,
                                                   int64_t budget_ns);

  // Returns the decision the strategy should act on:
  //   0 = Wait, 1 = SubmitFollower, 2 = CancelLeader.
  // Pass `ack_received = 1` and the actual ack ts when the leader has
  // acked; pass 0 + the current feed-time as `leader_ack_ts_ns` to
  // poll for timeout.
  LRVX_EXPORT(group = "order_group")
  uint8_t lrvx_order_group_pair_latency_decision(LrvxOrderGroupHandle h,
                                                 int64_t leader_submit_ts_ns,
                                                 int64_t leader_ack_ts_ns,
                                                 uint8_t ack_received);

  // ============================================================
  // Multi-feed clock
  // ============================================================
  //
  // Latency-aware multi-feed wait policy. The strategy creates a
  // clock for a list of symbols + a policy + a staleness budget; on
  // each tick the clock decides whether to fire and reports per-feed
  // staleness so the strategy can weight or skip a decision.
  //
  // The snapshot is read through accessor functions (no struct
  // returned by value over the C ABI) so bindings can shape it
  // however they want.

  typedef void* LrvxFeedClockHandle;

  // policy: 0 = WaitForAll, 1 = FireOnAny, 2 = LeaderFollower.
  // leader_symbol: ignored unless policy == LeaderFollower; defaults
  // to symbols[0] when zero.
  LRVX_EXPORT(group = "feed_clock")
  LrvxFeedClockHandle lrvx_feed_clock_create(const uint32_t* symbols, uint32_t symbol_count,
                                             uint8_t policy, int64_t timeout_ms,
                                             uint32_t leader_symbol,
                                             int64_t staleness_budget_ms);
  LRVX_EXPORT(group = "feed_clock")
  void lrvx_feed_clock_destroy(LrvxFeedClockHandle h);

  LRVX_EXPORT(group = "feed_clock")
  uint32_t lrvx_feed_clock_symbol_count(LrvxFeedClockHandle h);
  LRVX_EXPORT(group = "feed_clock")
  uint32_t lrvx_feed_clock_symbol_at(LrvxFeedClockHandle h, uint32_t index);

  // Drive a tick. Returns 1 if the clock fired, 0 otherwise. The
  // snapshot it produced (last-ts and staleness per registered feed)
  // is cached on the clock and read with the accessors below.
  LRVX_EXPORT(group = "feed_clock")
  uint8_t lrvx_feed_clock_tick(LrvxFeedClockHandle h, int64_t ts_ns, uint32_t symbol);

  // Last computed snapshot. `triggered_by` is the symbol that
  // produced the most recent tick.
  LRVX_EXPORT(group = "feed_clock")
  uint8_t lrvx_feed_clock_last_fired(LrvxFeedClockHandle h);
  LRVX_EXPORT(group = "feed_clock")
  uint32_t lrvx_feed_clock_last_triggered_by(LrvxFeedClockHandle h);
  LRVX_EXPORT(group = "feed_clock")
  int64_t lrvx_feed_clock_last_seen_at(LrvxFeedClockHandle h, uint32_t index);
  LRVX_EXPORT(group = "feed_clock")
  int64_t lrvx_feed_clock_staleness_at(LrvxFeedClockHandle h, uint32_t index);

  LRVX_EXPORT(group = "feed_clock")
  void lrvx_feed_clock_reset(LrvxFeedClockHandle h);

  // ============================================================
  // Position tracking
  // ============================================================

  LRVX_EXPORT(group = "position")
  LrvxPositionTrackerHandle lrvx_position_tracker_create(uint8_t cost_basis);
  LRVX_EXPORT(group = "position")
  void lrvx_position_tracker_destroy(LrvxPositionTrackerHandle tracker);
  LRVX_EXPORT(group = "position")
  void lrvx_position_tracker_on_fill(LrvxPositionTrackerHandle tracker, uint32_t symbol,
                                     uint8_t side, double price, double quantity);
  LRVX_EXPORT(group = "position")
  double lrvx_position_tracker_position(LrvxPositionTrackerHandle tracker, uint32_t symbol);
  LRVX_EXPORT(group = "position")
  double lrvx_position_tracker_avg_entry(LrvxPositionTrackerHandle tracker, uint32_t symbol);
  LRVX_EXPORT(group = "position")
  double lrvx_position_tracker_realized_pnl(LrvxPositionTrackerHandle tracker, uint32_t symbol);
  LRVX_EXPORT(group = "position")
  double lrvx_position_tracker_total_pnl(LrvxPositionTrackerHandle tracker);

  // ============================================================
  // Volume profile
  // ============================================================

  LRVX_EXPORT(group = "volume_profile")
  LrvxVolumeProfileHandle lrvx_volume_profile_create(double tick_size);
  LRVX_EXPORT(group = "volume_profile")
  void lrvx_volume_profile_destroy(LrvxVolumeProfileHandle profile);
  LRVX_EXPORT(group = "volume_profile")
  void lrvx_volume_profile_add_trade(LrvxVolumeProfileHandle profile, double price, double quantity,
                                     uint8_t is_buy);
  LRVX_EXPORT(group = "volume_profile")
  double lrvx_volume_profile_poc(LrvxVolumeProfileHandle profile);
  LRVX_EXPORT(group = "volume_profile")
  double lrvx_volume_profile_vah(LrvxVolumeProfileHandle profile);
  LRVX_EXPORT(group = "volume_profile")
  double lrvx_volume_profile_val(LrvxVolumeProfileHandle profile);
  LRVX_EXPORT(group = "volume_profile")
  double lrvx_volume_profile_total_volume(LrvxVolumeProfileHandle profile);
  LRVX_EXPORT(group = "volume_profile")
  double lrvx_volume_profile_total_delta(LrvxVolumeProfileHandle profile);
  LRVX_EXPORT(group = "volume_profile")
  uint32_t lrvx_volume_profile_num_levels(LrvxVolumeProfileHandle profile);
  LRVX_EXPORT(group = "volume_profile")
  void lrvx_volume_profile_clear(LrvxVolumeProfileHandle profile);

  // ============================================================
  // Footprint bar
  // ============================================================

  LRVX_EXPORT(group = "footprint_bar")
  LrvxFootprintHandle lrvx_footprint_create(double tick_size);
  LRVX_EXPORT(group = "footprint_bar")
  void lrvx_footprint_destroy(LrvxFootprintHandle footprint);
  LRVX_EXPORT(group = "footprint_bar")
  void lrvx_footprint_add_trade(LrvxFootprintHandle footprint, double price, double quantity,
                                uint8_t is_buy);
  LRVX_EXPORT(group = "footprint_bar")
  double lrvx_footprint_total_delta(LrvxFootprintHandle footprint);
  LRVX_EXPORT(group = "footprint_bar")
  double lrvx_footprint_total_volume(LrvxFootprintHandle footprint);
  LRVX_EXPORT(group = "footprint_bar")
  uint32_t lrvx_footprint_num_levels(LrvxFootprintHandle footprint);
  LRVX_EXPORT(group = "footprint_bar")
  void lrvx_footprint_clear(LrvxFootprintHandle footprint);

  // ============================================================
  // Statistics
  // ============================================================

  LRVX_EXPORT(group = "statistics")
  double lrvx_stat_correlation(const double* x, const double* y, size_t len);
  LRVX_EXPORT(group = "statistics")
  double lrvx_stat_profit_factor(const double* pnl, size_t len);
  LRVX_EXPORT(group = "statistics")
  double lrvx_stat_win_rate(const double* pnl, size_t len);

  // ============================================================
  // Order journey tracer
  // ============================================================

  // One row of the order-journey trace. Mirrors lrvx::OrderTraceRecord.
  typedef struct
  {
    uint64_t order_id;
    uint32_t seq;
    uint8_t status;
    uint8_t is_maker;
    uint8_t _pad[2];
    int64_t ts_ns;
    int64_t fill_qty_raw;
    int64_t fill_price_raw;
    int64_t queue_ahead_raw;
    int64_t queue_total_raw;
    int64_t submitted_at_ns;
    int64_t accepted_at_ns;
    int64_t first_fill_at_ns;
    int64_t last_fill_at_ns;
    int64_t canceled_at_ns;
    int64_t rejected_at_ns;
    int64_t triggered_at_ns;
    int64_t expired_at_ns;
  } LrvxOrderTraceRow;

  LRVX_EXPORT(group = "order_journey_tracer")
  LrvxOrderJourneyTracerHandle lrvx_order_journey_tracer_create(
      uint64_t max_orders, uint64_t max_records_per_order, double sample_rate,
      uint64_t sample_salt);
  LRVX_EXPORT(group = "order_journey_tracer")
  void lrvx_order_journey_tracer_destroy(LrvxOrderJourneyTracerHandle tracer);
  LRVX_EXPORT(group = "order_journey_tracer")
  uint64_t lrvx_order_journey_tracer_order_count(LrvxOrderJourneyTracerHandle tracer);
  LRVX_EXPORT(group = "order_journey_tracer")
  uint64_t lrvx_order_journey_tracer_record_count(LrvxOrderJourneyTracerHandle tracer);
  LRVX_EXPORT(group = "order_journey_tracer")
  double lrvx_order_journey_tracer_median_ack_latency_ns(
      LrvxOrderJourneyTracerHandle tracer);
  LRVX_EXPORT(group = "order_journey_tracer")
  double lrvx_order_journey_tracer_median_time_to_first_fill_ns(
      LrvxOrderJourneyTracerHandle tracer);
  LRVX_EXPORT(group = "order_journey_tracer")
  double lrvx_order_journey_tracer_maker_fill_ratio(
      LrvxOrderJourneyTracerHandle tracer);
  LRVX_EXPORT(group = "order_journey_tracer")
  double lrvx_order_journey_tracer_cancel_race_loss_rate(
      LrvxOrderJourneyTracerHandle tracer);
  // Fills out (up to max_rows) with the full trace; returns the actual row
  // count written. Pass max_rows = 0 to query the required size only.
  LRVX_EXPORT(group = "order_journey_tracer")
  uint64_t lrvx_order_journey_tracer_result(LrvxOrderJourneyTracerHandle tracer,
                                            LrvxOrderTraceRow* out,
                                            uint64_t max_rows);
  // Same shape for a single order's trace.
  LRVX_EXPORT(group = "order_journey_tracer")
  uint64_t lrvx_order_journey_tracer_journey(LrvxOrderJourneyTracerHandle tracer,
                                             uint64_t order_id,
                                             LrvxOrderTraceRow* out,
                                             uint64_t max_rows);
  LRVX_EXPORT(group = "order_journey_tracer")
  void lrvx_order_journey_tracer_clear(LrvxOrderJourneyTracerHandle tracer);
  // Attach a journey tracer to a BacktestRunner. The runner takes the
  // handle by void* to keep this declaration position-independent; see
  // lrvx_backtest_runner_add_journey_tracer in the backtest section.

  // ============================================================
  // Order tracker
  // ============================================================

  typedef void* LrvxOrderTrackerHandle;

  LRVX_EXPORT(group = "order_tracker")
  LrvxOrderTrackerHandle lrvx_order_tracker_create(void);
  LRVX_EXPORT(group = "order_tracker")
  void lrvx_order_tracker_destroy(LrvxOrderTrackerHandle tracker);
  LRVX_EXPORT(group = "order_tracker")
  uint8_t lrvx_order_tracker_on_submitted(LrvxOrderTrackerHandle tracker, uint64_t order_id,
                                          uint32_t symbol, uint8_t side, double price, double qty);
  LRVX_EXPORT(group = "order_tracker")
  uint8_t lrvx_order_tracker_on_filled(LrvxOrderTrackerHandle tracker, uint64_t order_id,
                                       double fill_qty);
  LRVX_EXPORT(group = "order_tracker")
  uint8_t lrvx_order_tracker_on_canceled(LrvxOrderTrackerHandle tracker, uint64_t order_id);
  LRVX_EXPORT(group = "order_tracker")
  uint8_t lrvx_order_tracker_is_active(LrvxOrderTrackerHandle tracker, uint64_t order_id);
  LRVX_EXPORT(group = "order_tracker")
  uint32_t lrvx_order_tracker_active_count(LrvxOrderTrackerHandle tracker);
  LRVX_EXPORT(group = "order_tracker")
  uint32_t lrvx_order_tracker_total_count(LrvxOrderTrackerHandle tracker);
  LRVX_EXPORT(group = "order_tracker")
  void lrvx_order_tracker_prune(LrvxOrderTrackerHandle tracker);

  // ============================================================
  // Position group tracker
  // ============================================================

  typedef void* LrvxPositionGroupHandle;

  LRVX_EXPORT(group = "position_group")
  LrvxPositionGroupHandle lrvx_position_group_create(void);
  LRVX_EXPORT(group = "position_group")
  void lrvx_position_group_destroy(LrvxPositionGroupHandle tracker);
  LRVX_EXPORT(group = "position_group")
  uint64_t lrvx_position_group_open(LrvxPositionGroupHandle tracker, uint64_t order_id,
                                    uint32_t symbol, uint8_t side, double price, double qty);
  LRVX_EXPORT(group = "position_group")
  void lrvx_position_group_close(LrvxPositionGroupHandle tracker, uint64_t position_id,
                                 double exit_price);
  LRVX_EXPORT(group = "position_group")
  void lrvx_position_group_partial_close(LrvxPositionGroupHandle tracker, uint64_t position_id,
                                         double qty, double exit_price);
  LRVX_EXPORT(group = "position_group")
  double lrvx_position_group_net_position(LrvxPositionGroupHandle tracker, uint32_t symbol);
  LRVX_EXPORT(group = "position_group")
  double lrvx_position_group_realized_pnl(LrvxPositionGroupHandle tracker, uint32_t symbol);
  LRVX_EXPORT(group = "position_group")
  double lrvx_position_group_total_pnl(LrvxPositionGroupHandle tracker);
  LRVX_EXPORT(group = "position_group")
  uint32_t lrvx_position_group_open_count(LrvxPositionGroupHandle tracker, uint32_t symbol);
  LRVX_EXPORT(group = "position_group")
  void lrvx_position_group_prune(LrvxPositionGroupHandle tracker);

  // ============================================================
  // Market profile
  // ============================================================

  typedef void* LrvxMarketProfileHandle;

  LRVX_EXPORT(group = "market_profile")
  LrvxMarketProfileHandle lrvx_market_profile_create(double tick_size, uint32_t period_minutes,
                                                     int64_t session_start_ns);
  LRVX_EXPORT(group = "market_profile")
  void lrvx_market_profile_destroy(LrvxMarketProfileHandle profile);
  LRVX_EXPORT(group = "market_profile")
  void lrvx_market_profile_add_trade(LrvxMarketProfileHandle profile, int64_t timestamp_ns,
                                     double price, double qty, uint8_t is_buy);
  LRVX_EXPORT(group = "market_profile")
  double lrvx_market_profile_poc(LrvxMarketProfileHandle profile);
  LRVX_EXPORT(group = "market_profile")
  double lrvx_market_profile_vah(LrvxMarketProfileHandle profile);
  LRVX_EXPORT(group = "market_profile")
  double lrvx_market_profile_val(LrvxMarketProfileHandle profile);
  LRVX_EXPORT(group = "market_profile")
  double lrvx_market_profile_ib_high(LrvxMarketProfileHandle profile);
  LRVX_EXPORT(group = "market_profile")
  double lrvx_market_profile_ib_low(LrvxMarketProfileHandle profile);
  LRVX_EXPORT(group = "market_profile")
  uint8_t lrvx_market_profile_is_poor_high(LrvxMarketProfileHandle profile);
  LRVX_EXPORT(group = "market_profile")
  uint8_t lrvx_market_profile_is_poor_low(LrvxMarketProfileHandle profile);
  LRVX_EXPORT(group = "market_profile")
  uint32_t lrvx_market_profile_num_levels(LrvxMarketProfileHandle profile);
  LRVX_EXPORT(group = "market_profile")
  void lrvx_market_profile_clear(LrvxMarketProfileHandle profile);

  // ============================================================
  // Composite book matrix
  // ============================================================

  typedef void* LrvxCompositeBookHandle;

  LRVX_EXPORT(group = "composite_book")
  LrvxCompositeBookHandle lrvx_composite_book_create(void);
  LRVX_EXPORT(group = "composite_book")
  void lrvx_composite_book_destroy(LrvxCompositeBookHandle book);
  LRVX_EXPORT(group = "composite_book")
  uint8_t lrvx_composite_book_best_bid(LrvxCompositeBookHandle book, uint32_t symbol,
                                       double* price_out, double* qty_out);
  LRVX_EXPORT(group = "composite_book")
  uint8_t lrvx_composite_book_best_ask(LrvxCompositeBookHandle book, uint32_t symbol,
                                       double* price_out, double* qty_out);
  LRVX_EXPORT(group = "composite_book")
  uint8_t lrvx_composite_book_has_arb(LrvxCompositeBookHandle book, uint32_t symbol);
  LRVX_EXPORT(group = "composite_book")
  void lrvx_composite_book_mark_stale(LrvxCompositeBookHandle book, uint32_t exchange,
                                      uint32_t symbol);
  LRVX_EXPORT(group = "composite_book")
  void lrvx_composite_book_check_staleness(LrvxCompositeBookHandle book, int64_t now_ns,
                                           int64_t threshold_ns);
  // Feeds one exchange/symbol slot of the composite book. `is_delta=false`
  // (apply_snapshot) replaces both sides of that exchange's top-of-book
  // wholesale; `is_delta=true` (apply_delta) updates only the side(s)
  // present here and leaves the other side exactly as it was, matching
  // BookUpdateType::SNAPSHOT / DELTA in the C++ CompositeBookMatrix this
  // wraps. `recv_ns` is a monotonic receive timestamp used for staleness
  // checks (see lrvx_composite_book_check_staleness above).
  LRVX_EXPORT(group = "composite_book")
  void lrvx_composite_book_apply_snapshot(LrvxCompositeBookHandle book, uint32_t exchange,
                                          uint32_t symbol, const double* bid_prices,
                                          const double* bid_qtys, size_t bid_len,
                                          const double* ask_prices, const double* ask_qtys,
                                          size_t ask_len, int64_t recv_ns);
  LRVX_EXPORT(group = "composite_book")
  void lrvx_composite_book_apply_delta(LrvxCompositeBookHandle book, uint32_t exchange,
                                       uint32_t symbol, const double* bid_prices,
                                       const double* bid_qtys, size_t bid_len,
                                       const double* ask_prices, const double* ask_qtys,
                                       size_t ask_len, int64_t recv_ns);

  // ============================================================
  // Executor fill access
  // ============================================================

  typedef struct
  {
    uint64_t order_id;
    uint32_t symbol;
    uint8_t side;
    int64_t price_raw;
    int64_t quantity_raw;
    int64_t timestamp_ns;
  } LrvxFill;

  LRVX_EXPORT(group = "executor_fill")
  uint32_t lrvx_simulated_executor_get_fills(LrvxSimulatedExecutorHandle executor, LrvxFill* fills_out,
                                             uint32_t max_fills);

  // ============================================================
  // Additional bar aggregation
  // ============================================================

  LRVX_EXPORT(group = "additional_bar")
  uint32_t lrvx_aggregate_range_bars(const int64_t* timestamps, const double* prices,
                                     const double* quantities, const uint8_t* is_buy, size_t len,
                                     double range_size, LrvxBar* bars_out, uint32_t max_bars);
  LRVX_EXPORT(group = "additional_bar")
  uint32_t lrvx_aggregate_renko_bars(const int64_t* timestamps, const double* prices,
                                     const double* quantities, const uint8_t* is_buy, size_t len,
                                     double brick_size, LrvxBar* bars_out, uint32_t max_bars);

  // ============================================================
  // L3 Order book
  // ============================================================

  typedef void* LrvxL3BookHandle;

  LRVX_EXPORT(group = "l3_order")
  LrvxL3BookHandle lrvx_l3_book_create(void);
  LRVX_EXPORT(group = "l3_order")
  void lrvx_l3_book_destroy(LrvxL3BookHandle book);
  LRVX_EXPORT(group = "l3_order")
  int32_t lrvx_l3_book_add_order(LrvxL3BookHandle book, uint64_t order_id, double price,
                                 double quantity, uint8_t side);
  LRVX_EXPORT(group = "l3_order")
  int32_t lrvx_l3_book_remove_order(LrvxL3BookHandle book, uint64_t order_id);
  LRVX_EXPORT(group = "l3_order")
  int32_t lrvx_l3_book_modify_order(LrvxL3BookHandle book, uint64_t order_id, double new_qty);
  LRVX_EXPORT(group = "l3_order")
  uint8_t lrvx_l3_book_best_bid(LrvxL3BookHandle book, double* price_out);
  LRVX_EXPORT(group = "l3_order")
  uint8_t lrvx_l3_book_best_ask(LrvxL3BookHandle book, double* price_out);
  LRVX_EXPORT(group = "l3_order")
  double lrvx_l3_book_bid_at_price(LrvxL3BookHandle book, double price);
  LRVX_EXPORT(group = "l3_order")
  double lrvx_l3_book_ask_at_price(LrvxL3BookHandle book, double price);

  // ============================================================
  // Data writer (binary log)
  // ============================================================

  typedef void* LrvxDataWriterHandle;

  LRVX_EXPORT(group = "data_writer")
  LrvxDataWriterHandle lrvx_data_writer_create(const char* output_dir, uint64_t max_segment_mb,
                                               uint8_t exchange_id);
  LRVX_EXPORT(group = "data_writer")
  void lrvx_data_writer_destroy(LrvxDataWriterHandle writer);
  LRVX_EXPORT(group = "data_writer")
  uint8_t lrvx_data_writer_write_trade(LrvxDataWriterHandle writer, int64_t exchange_ts_ns,
                                       int64_t recv_ts_ns, double price, double qty,
                                       uint64_t trade_id, uint32_t symbol_id, uint8_t side);
  // Single book-update writer. Levels are raw int64 (Price/Quantity
  // scale = 1e8); no double conversion in the hot path. Returns 1 on
  // success, 0 on failure (e.g. writer closed). bids/asks may be NULL
  // when the matching count is 0.
  LRVX_EXPORT(group = "data_writer")
  uint8_t lrvx_data_writer_write_book(LrvxDataWriterHandle writer,
                                      int64_t exchange_ts_ns,
                                      int64_t recv_ts_ns,
                                      int64_t seq,
                                      uint32_t symbol_id,
                                      uint8_t is_snapshot,
                                      const LrvxBookLevel* bids, uint32_t n_bids,
                                      const LrvxBookLevel* asks, uint32_t n_asks);
  LRVX_EXPORT(group = "data_writer")
  void lrvx_data_writer_flush(LrvxDataWriterHandle writer);
  LRVX_EXPORT(group = "data_writer")
  void lrvx_data_writer_close(LrvxDataWriterHandle writer);

  // ============================================================
  // Data reader (binary log)
  // ============================================================

  typedef void* LrvxDataReaderHandle;

  LRVX_EXPORT(group = "data_reader")
  LrvxDataReaderHandle lrvx_data_reader_create(const char* data_dir);
  LRVX_EXPORT(group = "data_reader")
  void lrvx_data_reader_destroy(LrvxDataReaderHandle reader);
  LRVX_EXPORT(group = "data_reader")
  uint64_t lrvx_data_reader_count(LrvxDataReaderHandle reader);

  // ============================================================
  // Order book level access
  // ============================================================

  LRVX_EXPORT(group = "order_book")
  uint32_t lrvx_book_get_bids(LrvxBookHandle book, double* prices_out, double* qtys_out,
                              uint32_t max_levels);
  LRVX_EXPORT(group = "order_book")
  uint32_t lrvx_book_get_asks(LrvxBookHandle book, double* prices_out, double* qtys_out,
                              uint32_t max_levels);

  // ============================================================
  // Heikin-Ashi bar aggregation
  // ============================================================

  LRVX_EXPORT(group = "heikin_ashi")
  uint32_t lrvx_aggregate_heikin_ashi_bars(const int64_t* timestamps, const double* prices,
                                           const double* quantities, const uint8_t* is_buy,
                                           size_t len, double interval_seconds, LrvxBar* bars_out,
                                           uint32_t max_bars);

  // ============================================================
  // Additional stats
  // ============================================================

  LRVX_EXPORT(group = "additional_stats")
  double lrvx_stat_permutation_test(const double* group1, size_t len1, const double* group2,
                                    size_t len2, uint32_t num_permutations);
  LRVX_EXPORT(group = "additional_stats")
  void lrvx_stat_bootstrap_ci(const double* data, size_t len, double confidence,
                              uint32_t num_samples, double* lower_out, double* median_out,
                              double* upper_out);
  LRVX_EXPORT(group = "additional_stats")
  void lrvx_stat_whites_reality_check(const double* returns, size_t num_strategies,
                                      size_t num_periods, uint32_t num_bootstrap,
                                      double avg_block_size, double* p_value_out,
                                      double* best_stat_out, int32_t* best_index_out);
  /* Probability of backtest overfitting by CSCV (Bailey, Borwein, Lopez de
   * Prado, Zhu). returns is trials x periods, row-major; num_blocks is the
   * even number of period blocks the splits are drawn from. pbo_out is NaN
   * when the inputs cannot be split (fewer than 2 trials, an odd or
   * out-of-range block count, fewer periods than blocks), and splits_out is
   * then 0. Deterministic. */
  LRVX_EXPORT(group = "additional_stats")
  void lrvx_stat_pbo_cscv(const double* returns, size_t num_trials, size_t num_periods,
                          uint32_t num_blocks, double* pbo_out, uint64_t* splits_out,
                          double* mean_oos_rank_out, double* mean_logit_out);
  /* Deflated Sharpe ratio (Bailey, Lopez de Prado). returns is the selected
   * strategy's per-period return series; trial_sharpes holds the per-period
   * Sharpe ratio of every trial the selection ran over (NULL or fewer than 2
   * of them means no selection, SR0 = 0). Every output is NaN with fewer than
   * 2 periods or a zero return variance; dsr_out alone is NaN when the
   * non-normality correction has no real root. */
  LRVX_EXPORT(group = "additional_stats")
  void lrvx_stat_deflated_sharpe(const double* returns, size_t num_periods,
                                 const double* trial_sharpes, size_t num_trials,
                                 double* sharpe_out, double* skewness_out, double* kurtosis_out,
                                 double* expected_max_sharpe_out, double* dsr_out);
  /* Per-cell neighbourhood of a parameter grid: the mean of the neighbouring
   * cells' values (every cell whose index differs by at most radius on every
   * axis, the cell itself excluded, NaN values skipped) and their count.
   * values is row-major over shape[ndim]; both outputs have as many cells as
   * values. A cell with no neighbour gets a NaN mean and a count of 0. */
  LRVX_EXPORT(group = "additional_stats")
  void lrvx_stat_grid_neighborhood(const double* values, const size_t* shape, size_t ndim,
                                   size_t radius, double* neighbor_mean_out,
                                   uint32_t* neighbor_count_out);

  // ============================================================
  // Segment operations
  // ============================================================

  LRVX_EXPORT(group = "segment")
  uint8_t lrvx_segment_validate(const char* path);
  LRVX_EXPORT(group = "segment")
  uint8_t lrvx_segment_merge(const char* input_dir, const char* output_path);

  // ============================================================
  // Backtest: slippage, queue, result, metrics, equity curve
  // ============================================================

  typedef enum
  {
    LRVX_SLIPPAGE_NONE = 0,
    LRVX_SLIPPAGE_FIXED_TICKS = 1,
    LRVX_SLIPPAGE_FIXED_BPS = 2,
    LRVX_SLIPPAGE_VOLUME_IMPACT = 3
  } LrvxSlippageModel;

  typedef enum
  {
    LRVX_QUEUE_NONE = 0,
    LRVX_QUEUE_TOB = 1,
    LRVX_QUEUE_FULL = 2,
    LRVX_QUEUE_PRO_RATA = 3,
    LRVX_QUEUE_PRO_RATA_WITH_FIFO = 4
  } LrvxQueueModel;

  // Configure slippage. Applies to market-style fills on all symbols unless
  // a per-symbol override is set. `tick_size` is the venue tick size in
  // price units (e.g. 0.01 for 1-cent ticks); pass 0.0 to fall back to one
  // raw price unit.
  LRVX_EXPORT(group = "backtest_slippage")
  void lrvx_simulated_executor_set_default_slippage(LrvxSimulatedExecutorHandle executor,
                                                    int32_t model, int32_t ticks,
                                                    double tick_size, double bps,
                                                    double impact_coeff);
  LRVX_EXPORT(group = "backtest_slippage")
  void lrvx_simulated_executor_set_symbol_slippage(LrvxSimulatedExecutorHandle executor, uint32_t symbol,
                                                   int32_t model, int32_t ticks,
                                                   double tick_size, double bps,
                                                   double impact_coeff);

  // Configure queue simulation for limit orders.
  LRVX_EXPORT(group = "backtest_slippage")
  void lrvx_simulated_executor_set_queue_model(LrvxSimulatedExecutorHandle executor, int32_t model,
                                               uint32_t depth);

  // For LRVX_QUEUE_PRO_RATA_WITH_FIFO: the first N orders at a level
  // consume the trade FIFO; the remainder is split pro-rata across
  // the rest. Ignored in NONE / TOB / FULL / PRO_RATA modes.
  LRVX_EXPORT(group = "backtest_slippage")
  void lrvx_simulated_executor_set_queue_fifo_top_n(LrvxSimulatedExecutorHandle executor,
                                                    uint32_t top_n);

  // TOP_PRO_LMM: fraction of each trade reserved for the queue-front
  // order; default 0.40. Ignored in other queue models.
  LRVX_EXPORT(group = "backtest_slippage")
  void lrvx_simulated_executor_set_top_priority_share(LrvxSimulatedExecutorHandle executor,
                                                      double share);

  // TOP_PRO_LMM: mark these order ids as Lead Market Makers.
  // `ids` is an array of `n_ids` order ids.
  LRVX_EXPORT(group = "backtest_slippage")
  void lrvx_simulated_executor_set_lmm_orders(LrvxSimulatedExecutorHandle executor,
                                              const uint64_t* ids, uint32_t n_ids);

  // TOP_PRO_LMM: LMM bonus multiplier applied during the tail
  // pro-rata distribution. Default 1.5.
  LRVX_EXPORT(group = "backtest_slippage")
  void lrvx_simulated_executor_set_lmm_bonus_multiplier(
      LrvxSimulatedExecutorHandle executor, double multiplier);

  // TOP_PRO_LMM / PRO_RATA_WITH_PRIORITY: per-order priority weight.
  // Defaults to 1.0 when unset.
  LRVX_EXPORT(group = "backtest_slippage")
  void lrvx_simulated_executor_set_order_priority_multiplier(
      LrvxSimulatedExecutorHandle executor, uint64_t order_id, double multiplier);

  // Set submit / cancel / replace ack latency knobs directly (without
  // touching queue model or slippage). Jitter is the uniform band on
  // top of the base latency; both are in nanoseconds.
  LRVX_EXPORT(group = "backtest_slippage")
  void lrvx_simulated_executor_set_submit_ack_latency(LrvxSimulatedExecutorHandle executor,
                                                      int64_t latency_ns, int64_t jitter_ns);
  LRVX_EXPORT(group = "backtest_slippage")
  void lrvx_simulated_executor_set_cancel_ack_latency(LrvxSimulatedExecutorHandle executor,
                                                      int64_t latency_ns, int64_t jitter_ns);
  LRVX_EXPORT(group = "backtest_slippage")
  void lrvx_simulated_executor_set_replace_ack_latency(LrvxSimulatedExecutorHandle executor,
                                                       int64_t latency_ns, int64_t jitter_ns);

  // Apply a named latency profile. Valid names: binance_um_futures,
  // bybit_linear, okx_swap, deribit, idealized, adversarial. Unknown
  // names are a no-op.
  LRVX_EXPORT(group = "backtest_slippage")
  int lrvx_simulated_executor_apply_latency_profile(LrvxSimulatedExecutorHandle executor,
                                                    const char* profile_name);

  // Self-trade prevention mode. 0=None, 1=CancelNewest, 2=CancelOldest,
  // 3=CancelBoth, 4=Decrement. Default None.
  LRVX_EXPORT(group = "backtest_slippage")
  void lrvx_simulated_executor_set_stp_mode(LrvxSimulatedExecutorHandle executor,
                                            uint8_t mode);

  // FOK fill semantics. 0=any_price (default; multi-level walk), 1=single_price
  // (only the limit-price level counts). See FokMode in
  // lrvx/backtest/simulated_executor.h for details.
  LRVX_EXPORT(group = "backtest_slippage")
  void lrvx_simulated_executor_set_fok_mode(LrvxSimulatedExecutorHandle executor,
                                            uint8_t mode);
  LRVX_EXPORT(group = "backtest_slippage")
  uint8_t lrvx_simulated_executor_fok_mode(LrvxSimulatedExecutorHandle executor);

  // Multi-account STP group membership. Two orders share an STP scope
  // when their accountIds match OR both accounts map into the same
  // non-zero group. group_id=0 removes any prior mapping.
  LRVX_EXPORT(group = "backtest_slippage")
  void lrvx_simulated_executor_set_stp_group_membership(
      LrvxSimulatedExecutorHandle executor, uint64_t account_id, uint64_t group_id);
  LRVX_EXPORT(group = "backtest_slippage")
  uint64_t lrvx_simulated_executor_stp_group_for(LrvxSimulatedExecutorHandle executor,
                                                 uint64_t account_id);

  // Latency-distribution handle. The simulator copies the distribution
  // on set_*_distribution; the caller still owns and destroys the
  // handle. kind: 0=Constant, 1=Uniform, 2=Lognormal, 3=Empirical.
  typedef void* LrvxLatencyDistributionHandle;

  LRVX_EXPORT(group = "backtest_slippage")
  LrvxLatencyDistributionHandle lrvx_latency_distribution_create(void);
  LRVX_EXPORT(group = "backtest_slippage")
  void lrvx_latency_distribution_destroy(LrvxLatencyDistributionHandle h);

  // Mutate the held distribution to one of the four shapes. Subsequent
  // calls overwrite. Each leaves burst-correlation at its current
  // value.
  LRVX_EXPORT(group = "backtest_slippage")
  void lrvx_latency_distribution_set_constant(LrvxLatencyDistributionHandle h, int64_t ns);
  LRVX_EXPORT(group = "backtest_slippage")
  void lrvx_latency_distribution_set_uniform(LrvxLatencyDistributionHandle h, int64_t lo_ns,
                                             int64_t hi_ns);
  LRVX_EXPORT(group = "backtest_slippage")
  void lrvx_latency_distribution_set_lognormal(LrvxLatencyDistributionHandle h,
                                               int64_t median_ns, double sigma);
  LRVX_EXPORT(group = "backtest_slippage")
  void lrvx_latency_distribution_set_empirical(LrvxLatencyDistributionHandle h,
                                               const int64_t* samples_ns, uint32_t n_samples);

  // Burst correlation in [0, 1). Higher = stronger autocorrelation
  // between consecutive draws (AR(1) on log-residuals for Lognormal).
  LRVX_EXPORT(group = "backtest_slippage")
  void lrvx_latency_distribution_set_burst_correlation(LrvxLatencyDistributionHandle h,
                                                       double rho);

  LRVX_EXPORT(group = "backtest_slippage")
  int64_t lrvx_latency_distribution_median_ns(LrvxLatencyDistributionHandle h);

  // Distribution-based ack-latency setters on SimulatedExecutor.
  LRVX_EXPORT(group = "backtest_slippage")
  void lrvx_simulated_executor_set_submit_ack_latency_distribution(
      LrvxSimulatedExecutorHandle executor, LrvxLatencyDistributionHandle dist);
  LRVX_EXPORT(group = "backtest_slippage")
  void lrvx_simulated_executor_set_cancel_ack_latency_distribution(
      LrvxSimulatedExecutorHandle executor, LrvxLatencyDistributionHandle dist);
  LRVX_EXPORT(group = "backtest_slippage")
  void lrvx_simulated_executor_set_replace_ack_latency_distribution(
      LrvxSimulatedExecutorHandle executor, LrvxLatencyDistributionHandle dist);

  // Client-side rate-limit policy. Simulator copies the policy on
  // set_rate_limit_policy; caller still owns + destroys the handle.
  typedef void* LrvxRateLimitPolicyHandle;

  LRVX_EXPORT(group = "rate_limit")
  LrvxRateLimitPolicyHandle lrvx_rate_limit_policy_create(void);
  LRVX_EXPORT(group = "rate_limit")
  void lrvx_rate_limit_policy_destroy(LrvxRateLimitPolicyHandle h);

  LRVX_EXPORT(group = "rate_limit")
  void lrvx_rate_limit_policy_add_bucket(LrvxRateLimitPolicyHandle h, const char* name,
                                         int64_t window_ns, uint32_t capacity,
                                         uint32_t submit_weight, uint32_t cancel_weight,
                                         uint32_t replace_weight);

  // Add a bucket scoped to an endpoint family. `family`: 0=Trading,
  // 1=MarketData, 2=Account. `query_weight` applies when the action
  // is QueryAccount / QueryMarketData; the submit / cancel / replace
  // weights only apply when family=Trading.
  LRVX_EXPORT(group = "rate_limit")
  void lrvx_rate_limit_policy_add_bucket_family(LrvxRateLimitPolicyHandle h, const char* name,
                                                int64_t window_ns, uint32_t capacity,
                                                uint32_t submit_weight, uint32_t cancel_weight,
                                                uint32_t replace_weight,
                                                uint8_t family, uint32_t query_weight);

  LRVX_EXPORT(group = "rate_limit")
  void lrvx_rate_limit_policy_set_ban(LrvxRateLimitPolicyHandle h,
                                      uint32_t after_consecutive_rejects,
                                      int64_t ban_duration_ns);

  // Canned profiles: "binance_um_futures", "bybit_linear",
  // "okx_swap", "deribit". Unknown names are a no-op.
  LRVX_EXPORT(group = "rate_limit")
  int lrvx_rate_limit_policy_load_profile(LrvxRateLimitPolicyHandle h,
                                          const char* profile_name);

  LRVX_EXPORT(group = "rate_limit")
  int64_t lrvx_rate_limit_policy_ban_until_ns(LrvxRateLimitPolicyHandle h);
  LRVX_EXPORT(group = "rate_limit")
  uint32_t lrvx_rate_limit_policy_consecutive_rejects(LrvxRateLimitPolicyHandle h);

  // Per-bucket readback. Two-call: pass out_buf=NULL to learn the
  // count; pass a buffer with `max_buckets * 4` int64 slots laid as
  // [window_ns, used, capacity, _pad] per bucket to read. Bucket
  // names are not surfaced via this call; use the bucket index +
  // your original add_bucket ordering.
  LRVX_EXPORT(group = "rate_limit")
  uint32_t lrvx_rate_limit_policy_bucket_state(LrvxRateLimitPolicyHandle h, int64_t now_ns,
                                               int64_t* out_buf, uint32_t max_buckets);

  LRVX_EXPORT(group = "rate_limit")
  void lrvx_simulated_executor_set_rate_limit_policy(LrvxSimulatedExecutorHandle executor,
                                                     LrvxRateLimitPolicyHandle policy);
  LRVX_EXPORT(group = "rate_limit")
  void lrvx_simulated_executor_clear_rate_limit_policy(LrvxSimulatedExecutorHandle executor);

  // Venue downtime model. Caller owns + destroys the handle; the
  // simulator holds a non-owning pointer that lives as long as the
  // executor uses it. set_venue_availability(executor, NULL) detaches.
  typedef void* LrvxVenueAvailabilityHandle;

  LRVX_EXPORT(group = "venue_availability")
  LrvxVenueAvailabilityHandle lrvx_venue_availability_create(void);
  LRVX_EXPORT(group = "venue_availability")
  void lrvx_venue_availability_destroy(LrvxVenueAvailabilityHandle h);

  // Add a scheduled outage. policy: 0=CANCEL_ALL, 1=HOLD,
  // 2=EXPIRE_GTC_AFTER. gtc_ttl_ns is honored for EXPIRE_GTC_AFTER
  // only.
  LRVX_EXPORT(group = "venue_availability")
  void lrvx_venue_availability_schedule_outage(LrvxVenueAvailabilityHandle h,
                                               int64_t start_ns, int64_t duration_ns,
                                               uint8_t policy, int64_t gtc_ttl_ns);

  // Extended schedule with outage pathology. outage_type:
  // 0=Total (legacy), 1=SubmitOnlyDown, 2=CancelOnlyDown,
  // 3=SlowDegradation, 4=StaleBook, 5=WrongSideRecovery.
  LRVX_EXPORT(group = "venue_availability")
  void lrvx_venue_availability_schedule_outage_ex(
      LrvxVenueAvailabilityHandle h, int64_t start_ns, int64_t duration_ns,
      uint8_t outage_type, uint8_t policy, int64_t gtc_ttl_ns,
      double degradation_latency_multiplier, double wrong_side_recovery_bps);

  // Per-action / per-feed gates.
  LRVX_EXPORT(group = "venue_availability")
  uint8_t lrvx_venue_availability_submits_allowed(LrvxVenueAvailabilityHandle h,
                                                  int64_t now_ns);
  LRVX_EXPORT(group = "venue_availability")
  uint8_t lrvx_venue_availability_cancels_allowed(LrvxVenueAvailabilityHandle h,
                                                  int64_t now_ns);
  LRVX_EXPORT(group = "venue_availability")
  uint8_t lrvx_venue_availability_book_updates_allowed(LrvxVenueAvailabilityHandle h,
                                                       int64_t now_ns);
  LRVX_EXPORT(group = "venue_availability")
  uint8_t lrvx_venue_availability_trades_allowed(LrvxVenueAvailabilityHandle h,
                                                 int64_t now_ns);
  LRVX_EXPORT(group = "venue_availability")
  double lrvx_venue_availability_latency_multiplier(LrvxVenueAvailabilityHandle h,
                                                    int64_t now_ns);
  LRVX_EXPORT(group = "venue_availability")
  double lrvx_venue_availability_consume_wrong_side_recovery_bps(
      LrvxVenueAvailabilityHandle h);

  // Enable Poisson random outages. per_day is the expected number of
  // outages per UTC day; mean_duration_ns is the mean exponential
  // duration. seed makes sampling reproducible.
  LRVX_EXPORT(group = "venue_availability")
  void lrvx_venue_availability_auto_random_outages(LrvxVenueAvailabilityHandle h,
                                                   double per_day, int64_t mean_duration_ns,
                                                   uint8_t policy, uint64_t seed);

  LRVX_EXPORT(group = "venue_availability")
  uint8_t lrvx_venue_availability_is_up(LrvxVenueAvailabilityHandle h, int64_t now_ns);

  LRVX_EXPORT(group = "venue_availability")
  void lrvx_simulated_executor_set_venue_availability(LrvxSimulatedExecutorHandle executor,
                                                      LrvxVenueAvailabilityHandle availability);

  // Feed a trade with quantity (enables queue-fill simulation for limit orders).
  LRVX_EXPORT(group = "backtest_slippage")
  void lrvx_simulated_executor_on_trade_qty(LrvxSimulatedExecutorHandle executor, uint32_t symbol,
                                            double price, double quantity, uint8_t is_buy);

  // Feed a top-of-book snapshot (both best bid and best ask in one call).
  // For multi-level updates, build a BookUpdate on the C++ side; the C API
  // intentionally does not expose a stateful per-side helper because that
  // makes it too easy to accidentally clear the opposite side.
  LRVX_EXPORT(group = "backtest_slippage")
  void lrvx_simulated_executor_on_best_levels(LrvxSimulatedExecutorHandle executor, uint32_t symbol,
                                              double bid_price, double bid_qty, double ask_price,
                                              double ask_qty);

  // Feed a full L2 snapshot with parallel bid/ask arrays.
  LRVX_EXPORT(group = "backtest_slippage")
  void lrvx_simulated_executor_on_book_snapshot(LrvxSimulatedExecutorHandle executor, uint32_t symbol,
                                                const double* bid_prices, const double* bid_qtys,
                                                uint32_t n_bids, const double* ask_prices,
                                                const double* ask_qtys, uint32_t n_asks);

  // BacktestResult handle: aggregates fills into trades + stats + equity curve.
  typedef void* LrvxBacktestResultHandle;

  LRVX_EXPORT(group = "backtest_slippage")
  LrvxBacktestResultHandle lrvx_backtest_result_create(double initial_capital,
                                                       double fee_rate,
                                                       uint8_t use_percentage_fee,
                                                       double fixed_fee_per_trade,
                                                       double risk_free_rate,
                                                       double annualization_factor);
  LRVX_EXPORT(group = "backtest_slippage")
  void lrvx_backtest_result_destroy(LrvxBacktestResultHandle result);

  // Feed fills produced by a SimulatedExecutor. Fills are processed in order.
  LRVX_EXPORT(group = "backtest_slippage")
  void lrvx_backtest_result_record_fill(LrvxBacktestResultHandle result,
                                        uint64_t order_id, uint32_t symbol, uint8_t side,
                                        double price, double quantity, int64_t timestamp_ns);

  // Drain all fills from a SimulatedExecutor into a BacktestResult in FIFO order.
  LRVX_EXPORT(group = "backtest_slippage")
  void lrvx_backtest_result_ingest_executor(LrvxBacktestResultHandle result,
                                            LrvxSimulatedExecutorHandle executor);

  typedef struct
  {
    uint64_t totalTrades;
    uint64_t winningTrades;
    uint64_t losingTrades;
    uint64_t maxConsecutiveWins;
    uint64_t maxConsecutiveLosses;

    double initialCapital;
    double finalCapital;
    double totalPnl;
    double totalFees;
    double netPnl;
    double grossProfit;
    double grossLoss;

    double maxDrawdown;
    double maxDrawdownPct;

    double winRate;
    double profitFactor;
    double avgWin;
    double avgLoss;
    double avgWinLossRatio;

    double avgTradeDurationNs;
    double medianTradeDurationNs;
    double maxTradeDurationNs;

    double sharpeRatio;
    double sortinoRatio;
    double calmarRatio;
    double timeWeightedReturn;
    double returnPct;

    int64_t startTimeNs;
    int64_t endTimeNs;
  } LrvxBacktestStats;

  LRVX_EXPORT(group = "backtest_slippage")
  void lrvx_backtest_result_stats(LrvxBacktestResultHandle result, LrvxBacktestStats* out);

  typedef struct
  {
    int64_t timestamp_ns;
    double equity;
    double drawdown_pct;
  } LrvxEquityPoint;

  // Returns total available points. If points_out is non-NULL, writes up to
  // max_points entries and returns the number written.
  LRVX_EXPORT(group = "backtest_slippage")
  uint32_t lrvx_backtest_result_equity_curve(LrvxBacktestResultHandle result,
                                             LrvxEquityPoint* points_out,
                                             uint32_t max_points);

  LRVX_EXPORT(group = "backtest_slippage")
  uint8_t lrvx_backtest_result_write_equity_curve_csv(LrvxBacktestResultHandle result,
                                                      const char* path);

  // Closed-trade record from a backtest. side: 0 = long (buy entry,
  // sell exit), 1 = short (sell entry, buy exit). Times are in
  // nanoseconds since epoch. Distinct from LrvxTradeRecord (datareader),
  // which carries raw trade events from market data.
  typedef struct
  {
    uint32_t symbol;
    uint8_t side;
    double entry_price;
    double exit_price;
    double quantity;
    int64_t entry_time_ns;
    int64_t exit_time_ns;
    double pnl;
    double fee;
  } LrvxBacktestTrade;

  // Returns total available trades. If trades_out is non-NULL, writes up to
  // max_trades entries and returns the number written.
  LRVX_EXPORT(group = "backtest_slippage")
  uint32_t lrvx_backtest_result_trades(LrvxBacktestResultHandle result,
                                       LrvxBacktestTrade* trades_out,
                                       uint32_t max_trades);

  // ============================================================
  // Segment operations (full API)
  // ============================================================

  typedef struct
  {
    uint8_t success;
    uint64_t segments_merged;
    uint64_t events_written;
    uint64_t bytes_written;
  } LrvxMergeResult;

  typedef struct
  {
    uint8_t success;
    uint32_t segments_created;
    uint64_t events_written;
  } LrvxSplitResult;

  typedef struct
  {
    uint8_t success;
    uint64_t events_exported;
    uint64_t bytes_written;
  } LrvxExportResult;

  // merge: input_paths is one buffer holding num_paths consecutive
  // NUL-terminated paths, back to back ("a.lrvx\0b.lrvx\0"). The function
  // walks forward exactly num_paths times and has no way to notice a buffer
  // that ends early -- pass the count the buffer actually contains. NULL
  // input_paths or output_dir returns success = 0.
  //
  // lrvx_backtest_runner_run_tapes takes the ordinary const char* const*
  // array instead; prefer that shape in new code.
  LRVX_EXPORT(group = "segment")
  LrvxMergeResult lrvx_segment_merge_full(const char* input_paths, size_t num_paths,
                                          const char* output_dir, const char* output_name,
                                          uint8_t sort);

  LRVX_EXPORT(group = "segment")
  LrvxMergeResult lrvx_segment_merge_dir(const char* input_dir, const char* output_dir);

  LRVX_EXPORT(group = "segment")
  LrvxSplitResult lrvx_segment_split(const char* input_path, const char* output_dir,
                                     uint8_t mode, int64_t time_interval_ns,
                                     uint64_t events_per_file);

  LRVX_EXPORT(group = "segment")
  LrvxExportResult lrvx_segment_export(const char* input_path, const char* output_path,
                                       uint8_t format, int64_t from_ns, int64_t to_ns,
                                       const uint32_t* symbols, uint32_t num_symbols);

  LRVX_EXPORT(group = "segment")
  uint8_t lrvx_segment_recompress(const char* input_path, const char* output_path,
                                  uint8_t compression);

  LRVX_EXPORT(group = "segment")
  uint64_t lrvx_segment_extract_symbols(const char* input_path, const char* output_path,
                                        const uint32_t* symbols, uint32_t num_symbols);

  LRVX_EXPORT(group = "segment")
  uint64_t lrvx_segment_extract_time_range(const char* input_path, const char* output_path,
                                           int64_t from_ns, int64_t to_ns);

  // ============================================================
  // Validation (full API)
  // ============================================================

  typedef struct
  {
    uint8_t valid;
    uint8_t header_valid;
    uint64_t reported_event_count;
    uint64_t actual_event_count;
    uint8_t has_index;
    uint8_t index_valid;
    uint64_t trades_found;
    uint64_t book_updates_found;
    uint32_t crc_errors;
    uint32_t timestamp_anomalies;
  } LrvxSegmentValidation;

  LRVX_EXPORT(group = "validation")
  LrvxSegmentValidation lrvx_segment_validate_full(const char* path, uint8_t verify_crc,
                                                   uint8_t verify_timestamps);

  typedef struct
  {
    uint8_t valid;
    uint32_t total_segments;
    uint32_t valid_segments;
    uint32_t corrupted_segments;
    uint64_t total_events;
    uint64_t total_bytes;
    int64_t first_timestamp;
    int64_t last_timestamp;
  } LrvxDatasetValidation;

  LRVX_EXPORT(group = "validation")
  LrvxDatasetValidation lrvx_dataset_validate(const char* data_dir);

  // ============================================================
  // DataReader (full API)
  // ============================================================

  typedef struct
  {
    int64_t first_event_ns;
    int64_t last_event_ns;
    uint64_t total_events;
    uint32_t segment_count;
    uint64_t total_bytes;
    double duration_seconds;
  } LrvxDatasetSummary;

  LRVX_EXPORT(group = "datareader")
  LrvxDataReaderHandle lrvx_data_reader_create_filtered(const char* data_dir, int64_t from_ns,
                                                        int64_t to_ns, const uint32_t* symbols,
                                                        uint32_t num_symbols);

  // Same reader, with the ordering knobs the filtered form cannot reach.
  // reorder_window_ns <= 0 keeps the 10s default. strict_ordering != 0 makes
  // the reader raise on the first event that arrives past the window instead
  // of dropping it and counting it in LrvxReaderStats.late_dropped.
  LRVX_EXPORT(group = "datareader")
  LrvxDataReaderHandle lrvx_data_reader_create_ordered(const char* data_dir, int64_t from_ns,
                                                       int64_t to_ns, const uint32_t* symbols,
                                                       uint32_t num_symbols,
                                                       int64_t reorder_window_ns,
                                                       int32_t strict_ordering);

  LRVX_EXPORT(group = "datareader")
  LrvxDatasetSummary lrvx_data_reader_summary(LrvxDataReaderHandle reader);

  typedef struct
  {
    uint64_t files_read;
    uint64_t events_read;
    uint64_t trades_read;
    uint64_t book_updates_read;
    uint64_t bytes_read;
    uint64_t crc_errors;
    // Events discarded because they arrived past the reorder window. Always 0
    // when the reader was created with strict ordering (it raises instead).
    uint64_t late_dropped;
    // Frames whose record type this build does not know. The tape format is
    // additive, so they are stepped over rather than ending the read.
    uint64_t unknown_frames_skipped;
  } LrvxReaderStats;

  LRVX_EXPORT(group = "datareader")
  LrvxReaderStats lrvx_data_reader_stats(LrvxDataReaderHandle reader);

  typedef struct
  {
    int64_t exchange_ts_ns;
    int64_t recv_ts_ns;
    int64_t price_raw;
    int64_t qty_raw;
    uint64_t trade_id;
    uint32_t symbol_id;
    uint8_t side;
  } LrvxTradeRecord;

  // Returns number of trades read. If trades_out is NULL, counts only.
  LRVX_EXPORT(group = "datareader")
  uint64_t lrvx_data_reader_read_trades(LrvxDataReaderHandle reader, LrvxTradeRecord* trades_out,
                                        uint64_t max_trades);

  // Best bid/ask extracted from each book update event.
  typedef struct
  {
    int64_t exchange_ts_ns;
    int64_t recv_ts_ns;
    int64_t seq;
    int64_t bid_price_raw;
    int64_t bid_qty_raw;
    int64_t ask_price_raw;
    int64_t ask_qty_raw;
    uint32_t symbol_id;
    uint8_t event_type;  // 2=snapshot, 3=delta
  } LrvxBBO;

  // Per-event header for read_book_updates(). Levels live in a separate
  // flat array; level_offset/bid_count/ask_count slice it for this event.
  typedef struct
  {
    int64_t exchange_ts_ns;
    int64_t recv_ts_ns;
    int64_t seq;
    uint64_t level_offset;
    uint32_t symbol_id;
    uint16_t bid_count;
    uint16_t ask_count;
    uint8_t event_type;  // 2=snapshot, 3=delta
  } LrvxBookUpdateHeader;

  typedef struct
  {
    int64_t price_raw;
    int64_t qty_raw;
    uint8_t side;  // 0=bid, 1=ask
  } LrvxLevel;

  // Returns number of book update events read. If bbos_out is NULL, counts only.
  LRVX_EXPORT(group = "datareader")
  uint64_t lrvx_data_reader_read_bbo(LrvxDataReaderHandle reader, LrvxBBO* bbos_out,
                                     uint64_t max_events);

  // Counts book update events and total levels in a single pass.
  // Returns event count; writes total level count to *total_levels_out (may be NULL).
  LRVX_EXPORT(group = "datareader")
  uint64_t lrvx_data_reader_count_book_updates(LrvxDataReaderHandle reader,
                                               uint64_t* total_levels_out);

  // Reads book updates into pre-sized arrays. Caller must size headers_out
  // (>= event count) and levels_out (>= total levels) using
  // lrvx_data_reader_count_book_updates().
  // Returns number of events read.
  LRVX_EXPORT(group = "datareader")
  uint64_t lrvx_data_reader_read_book_updates(LrvxDataReaderHandle reader,
                                              LrvxBookUpdateHeader* headers_out,
                                              uint64_t max_events,
                                              LrvxLevel* levels_out,
                                              uint64_t max_levels);

  // Mid-stream seek variants. Each starts iteration from the first event whose
  // exchange_ts_ns >= start_ts_ns and otherwise behaves like the matching
  // non-_from reader. Useful when you keep one reader open and want to pull
  // batches from arbitrary timestamps without re-opening segments. If
  // start_ts_ns falls before the dataset's first event, behaves like the
  // non-_from variant.

  LRVX_EXPORT(group = "datareader")
  uint64_t lrvx_data_reader_read_trades_from(LrvxDataReaderHandle reader,
                                             int64_t start_ts_ns,
                                             LrvxTradeRecord* trades_out,
                                             uint64_t max_trades);

  LRVX_EXPORT(group = "datareader")
  uint64_t lrvx_data_reader_read_bbo_from(LrvxDataReaderHandle reader,
                                          int64_t start_ts_ns,
                                          LrvxBBO* bbos_out,
                                          uint64_t max_events);

  LRVX_EXPORT(group = "datareader")
  uint64_t lrvx_data_reader_count_book_updates_from(LrvxDataReaderHandle reader,
                                                    int64_t start_ts_ns,
                                                    uint64_t* total_levels_out);

  LRVX_EXPORT(group = "datareader")
  uint64_t lrvx_data_reader_read_book_updates_from(LrvxDataReaderHandle reader,
                                                   int64_t start_ts_ns,
                                                   LrvxBookUpdateHeader* headers_out,
                                                   uint64_t max_events,
                                                   LrvxLevel* levels_out,
                                                   uint64_t max_levels);

  // ============================================================
  // MergedTapeReader — read N `.lrvx` directories as one merged
  // stream, sorted by exchange_ts_ns. Symbols are rekeyed into a global
  // id space keyed by (metadata.exchange, name). Trades may overlap in
  // time across tapes; overlapping book streams for the same
  // (exchange, name) raise on construction (returns NULL handle and
  // sets the LrvxError thread-local). See docs/errors/E_INPUT_003.md.
  // ============================================================

  typedef void* LrvxMergedTapeReaderHandle;

  // Metadata about one global symbol after rekey. Strings are owned by
  // the reader — valid until the reader is destroyed.
  typedef struct
  {
    uint32_t global_id;
    int8_t price_precision;
    int8_t qty_precision;
    uint8_t _pad[2];
    const char* exchange;  // borrowed pointer
    const char* name;      // borrowed pointer
  } LrvxMergedSymbol;

  // Per-tape contribution counts. Path is borrowed.
  typedef struct
  {
    int64_t first_event_ns;
    int64_t last_event_ns;
    uint64_t trades;
    uint64_t books;
    const char* path;
  } LrvxMergedTapeStats;

  LRVX_EXPORT(group = "merged_tape_reader")
  LrvxMergedTapeReaderHandle
  lrvx_merged_tape_reader_create(const char* const* paths, uint32_t n_paths,
                                 int64_t from_ns,  // -1 = no lower bound
                                 int64_t to_ns,    // -1 = no upper bound
                                 const uint32_t* symbol_filter,
                                 uint32_t n_filter);

  LRVX_EXPORT(group = "merged_tape_reader")
  void lrvx_merged_tape_reader_destroy(LrvxMergedTapeReaderHandle reader);

  LRVX_EXPORT(group = "merged_tape_reader")
  uint32_t lrvx_merged_tape_reader_symbol_count(LrvxMergedTapeReaderHandle reader);

  // Populate `out` with up to `max` symbols. Returns the number written.
  // Pass NULL out to size — returns the actual count.
  LRVX_EXPORT(group = "merged_tape_reader")
  uint32_t lrvx_merged_tape_reader_get_symbols(LrvxMergedTapeReaderHandle reader,
                                               LrvxMergedSymbol* out,
                                               uint32_t max);

  LRVX_EXPORT(group = "merged_tape_reader")
  uint32_t lrvx_merged_tape_reader_tape_count(LrvxMergedTapeReaderHandle reader);

  LRVX_EXPORT(group = "merged_tape_reader")
  uint32_t lrvx_merged_tape_reader_get_tape_stats(LrvxMergedTapeReaderHandle reader,
                                                  LrvxMergedTapeStats* out,
                                                  uint32_t max);

  LRVX_EXPORT(group = "merged_tape_reader")
  void lrvx_merged_tape_reader_time_range(LrvxMergedTapeReaderHandle reader,
                                          int64_t* min_first_ns_out,
                                          int64_t* max_last_ns_out);

  // Two-phase trade read: count first, then allocate, then fill.
  LRVX_EXPORT(group = "merged_tape_reader")
  uint64_t lrvx_merged_tape_reader_count_trades(LrvxMergedTapeReaderHandle reader);

  // LrvxTradeRecord matches the reader's struct (datareader group);
  // symbol_id field carries the global_id.
  LRVX_EXPORT(group = "merged_tape_reader")
  uint64_t lrvx_merged_tape_reader_read_trades(LrvxMergedTapeReaderHandle reader,
                                               LrvxTradeRecord* trades_out,
                                               uint64_t max_trades);

  // Books — same shape as lrvx_data_reader_count_book_updates /
  // _read_book_updates.
  LRVX_EXPORT(group = "merged_tape_reader")
  uint64_t lrvx_merged_tape_reader_count_books(LrvxMergedTapeReaderHandle reader,
                                               uint64_t* total_levels_out);

  LRVX_EXPORT(group = "merged_tape_reader")
  uint64_t lrvx_merged_tape_reader_read_books(LrvxMergedTapeReaderHandle reader,
                                              LrvxBookUpdateHeader* headers_out,
                                              uint64_t max_events,
                                              LrvxLevel* levels_out,
                                              uint64_t max_levels);

  // ============================================================
  // DataWriter (extras)
  // ============================================================

  typedef struct
  {
    uint64_t bytes_written;
    uint64_t events_written;
    uint64_t segments_created;
    uint64_t trades_written;
  } LrvxWriterStats;

  LRVX_EXPORT(group = "datawriter")
  LrvxWriterStats lrvx_data_writer_stats(LrvxDataWriterHandle writer);

  // Batched book writer. headers + flat levels array, sliced per event
  // via header.level_offset / bid_count / ask_count. Same struct layout
  // as lrvx_data_reader_read_book_updates, so round-trip works.
  // event_type 2=snapshot, 3=delta. LrvxLevel.side is ignored on write
  // (bid vs ask is derived from the bid_count/ask_count split). Returns
  // count successfully written.
  LRVX_EXPORT(group = "datawriter")
  uint64_t lrvx_data_writer_write_books(LrvxDataWriterHandle writer,
                                        const LrvxBookUpdateHeader* headers,
                                        uint64_t n_events,
                                        const LrvxLevel* levels,
                                        uint64_t total_levels);

  // ============================================================
  // Partitioner
  // ============================================================

  typedef void* LrvxPartitionerHandle;

  typedef struct
  {
    uint32_t partition_id;
    int64_t from_ns;
    int64_t to_ns;
    int64_t warmup_from_ns;
    uint64_t estimated_events;
    uint64_t estimated_bytes;
  } LrvxPartition;

  LRVX_EXPORT(group = "partitioner")
  LrvxPartitionerHandle lrvx_partitioner_create(const char* data_dir);
  LRVX_EXPORT(group = "partitioner")
  void lrvx_partitioner_destroy(LrvxPartitionerHandle partitioner);

  // All partition functions return number of partitions.
  // If partitions_out is NULL, returns count only.
  LRVX_EXPORT(group = "partitioner")
  uint32_t lrvx_partitioner_by_time(LrvxPartitionerHandle p, uint32_t num_partitions,
                                    int64_t warmup_ns, LrvxPartition* partitions_out,
                                    uint32_t max_partitions);
  LRVX_EXPORT(group = "partitioner")
  uint32_t lrvx_partitioner_by_duration(LrvxPartitionerHandle p, int64_t duration_ns,
                                        int64_t warmup_ns, LrvxPartition* partitions_out,
                                        uint32_t max_partitions);
  LRVX_EXPORT(group = "partitioner")
  uint32_t lrvx_partitioner_by_calendar(LrvxPartitionerHandle p, uint8_t unit,
                                        int64_t warmup_ns, LrvxPartition* partitions_out,
                                        uint32_t max_partitions);
  LRVX_EXPORT(group = "partitioner")
  uint32_t lrvx_partitioner_by_symbol(LrvxPartitionerHandle p, uint32_t num_partitions,
                                      LrvxPartition* partitions_out, uint32_t max_partitions);
  LRVX_EXPORT(group = "partitioner")
  uint32_t lrvx_partitioner_per_symbol(LrvxPartitionerHandle p, LrvxPartition* partitions_out,
                                       uint32_t max_partitions);
  LRVX_EXPORT(group = "partitioner")
  uint32_t lrvx_partitioner_by_event_count(LrvxPartitionerHandle p, uint32_t num_partitions,
                                           LrvxPartition* partitions_out,
                                           uint32_t max_partitions);

  // ============================================================
  // Pointer-out wrappers for struct-returning functions.
  // These exist for language bindings (Codon, QuickJS) that cannot
  // consume C structs returned by value via their FFI.
  // Each writes sizeof(OriginalStruct) bytes to *out.
  // ============================================================

  LRVX_EXPORT(group = "pointer_out")
  void lrvx_data_reader_summary_p(LrvxDataReaderHandle reader, void* out);
  LRVX_EXPORT(group = "pointer_out")
  void lrvx_data_reader_stats_p(LrvxDataReaderHandle reader, void* out);
  LRVX_EXPORT(group = "pointer_out")
  void lrvx_data_writer_stats_p(LrvxDataWriterHandle writer, void* out);
  LRVX_EXPORT(group = "pointer_out")
  void lrvx_binary_log_recorder_hook_stats_p(void* hook, void* out);
  LRVX_EXPORT(group = "pointer_out")
  void lrvx_segment_merge_full_p(const char* input_paths, size_t num_paths,
                                 const char* output_dir, const char* output_name,
                                 uint8_t sort, void* out);
  LRVX_EXPORT(group = "pointer_out")
  void lrvx_segment_merge_dir_p(const char* input_dir, const char* output_dir, void* out);
  LRVX_EXPORT(group = "pointer_out")
  void lrvx_segment_split_p(const char* input_path, const char* output_dir, uint8_t mode,
                            int64_t time_interval_ns, uint64_t events_per_file, void* out);
  LRVX_EXPORT(group = "pointer_out")
  void lrvx_segment_export_p(const char* input_path, const char* output_path, uint8_t format,
                             int64_t from_ns, int64_t to_ns,
                             const uint32_t* symbols, uint32_t num_symbols, void* out);
  LRVX_EXPORT(group = "pointer_out")
  void lrvx_segment_validate_full_p(const char* path, uint8_t verify_crc,
                                    uint8_t verify_timestamps, void* out);
  LRVX_EXPORT(group = "pointer_out")
  void lrvx_dataset_validate_p(const char* data_dir, void* out);

  // ============================================================
  // Signal type — emitted by strategies, received by order backends.
  // Shared by LrvxLiveEngine and StrategyRunner.
  // ============================================================

  typedef struct
  {
    uint64_t order_id;
    uint32_t symbol;
    uint8_t side;        // 0=buy, 1=sell
    uint8_t order_type;  // 0=market, 1=limit, 2=stop_market, 3=stop_limit,
                         // 4=tp_market, 5=tp_limit, 6=trailing_stop,
                         // 7=cancel, 8=cancel_all, 9=modify
    double price;        // limit price (0 for market orders)
    double quantity;
    double trigger_price;    // stop/take-profit trigger
    double trailing_offset;  // trailing stop — absolute price offset
    int32_t trailing_bps;    // trailing stop — callback rate in basis points
    double new_price;        // modify: updated price
    double new_quantity;     // modify: updated quantity
    double range_lower;      // provide/withdraw liquidity: lower bound of the range
    double range_upper;      // provide liquidity: upper bound of the range (0 on withdraw)
    double liquidity;        // provide/withdraw liquidity: position size
  } LrvxSignal;

  typedef void (*LrvxOnSignalCallback)(void* user_data, const LrvxSignal* signal);

  // LRVX_SIGNAL_TYPE_* is the code space used ONLY by LrvxSignal.order_type
  // above. It swaps LIMIT and MARKET relative to the C++ lrvx::OrderType
  // enum, which is the space every other order_type / type field on this
  // page uses. Do not reuse these codes to decode any of those.
  // lrvx::export_macro(group="signal_type")
  #define LRVX_SIGNAL_TYPE_MARKET 0
  // lrvx::export_macro(group="signal_type")
  #define LRVX_SIGNAL_TYPE_LIMIT 1
  // lrvx::export_macro(group="signal_type")
  #define LRVX_SIGNAL_TYPE_STOP_MARKET 2
  // lrvx::export_macro(group="signal_type")
  #define LRVX_SIGNAL_TYPE_STOP_LIMIT 3
  // lrvx::export_macro(group="signal_type")
  #define LRVX_SIGNAL_TYPE_TAKE_PROFIT_MARKET 4
  // lrvx::export_macro(group="signal_type")
  #define LRVX_SIGNAL_TYPE_TAKE_PROFIT_LIMIT 5
  // lrvx::export_macro(group="signal_type")
  #define LRVX_SIGNAL_TYPE_TRAILING_STOP 6
  // lrvx::export_macro(group="signal_type")
  #define LRVX_SIGNAL_TYPE_CANCEL 7
  // lrvx::export_macro(group="signal_type")
  #define LRVX_SIGNAL_TYPE_CANCEL_ALL 8
  // lrvx::export_macro(group="signal_type")
  #define LRVX_SIGNAL_TYPE_MODIFY 9
  // lrvx::export_macro(group="signal_type")
  #define LRVX_SIGNAL_TYPE_ICEBERG 10
  // lrvx::export_macro(group="signal_type")
  #define LRVX_SIGNAL_TYPE_OCO 11
  // The liquidity pair acts on an AMM pool rather than on an order book, so
  // `price` and `quantity` on the signal carry nothing: the range and the
  // amount live on the C++ Signal and do not cross this struct.
  // lrvx::export_macro(group="signal_type")
  #define LRVX_SIGNAL_TYPE_PROVIDE_LIQUIDITY 12
  // lrvx::export_macro(group="signal_type")
  #define LRVX_SIGNAL_TYPE_WITHDRAW_LIQUIDITY 13

  // ============================================================
  // RiskManager — pre-trade hook callable from runner / live engine.
  //
  // The `allow` callback is invoked synchronously on every signal a
  // strategy emits, *before* the user's on_signal callback fires. Returning
  // 0 (deny) drops the signal entirely; returning 1 (allow) lets it
  // propagate. Use this to enforce risk limits, kill switches, or
  // jurisdiction-specific rules without modifying the strategy itself.
  //
  // The signal pointer passed to `allow` aliases the same LrvxSignal that
  // would otherwise be delivered to on_signal — fields are read-only;
  // mutations are not propagated.
  //
  // What a gate owes the caller when it cannot answer — this applies to
  // KillSwitch.check and OrderValidator.validate below as much as to
  // RiskManager.allow, and every binding implements it the same way:
  //
  //   The policy, one for every binding: a host-language throw and a
  //   non-boolean return both DENY the order, and both are reported. Neither
  //   ever lets the order through, and neither escapes through the C
  //   boundary. A gate that plainly returns false is a decision, not a
  //   failure, and is not reported.
  //
  // A binding therefore catches its own host-language exception inside the
  // bridge — an exception crossing a C function pointer is undefined
  // behaviour — records it somewhere the caller can read synchronously, and
  // returns 0. Returning anything other than 0 or 1 from this callback is a
  // binding bug; the engine reads any non-zero value as allow.
  //
  // Lifecycle: created via lrvx_risk_manager_create, attached to a
  // runner/engine via lrvx_runner_set_risk_manager /
  // lrvx_live_engine_set_risk_manager (NULL to detach), destroyed via
  // lrvx_risk_manager_destroy. The handle may be shared across multiple
  // runners/engines; the caller owns destruction.
  // ============================================================

  typedef uint8_t (*LrvxRiskManagerAllowFn)(void* user_data,
                                            const LrvxSignal* signal);

  typedef struct
  {
    LrvxRiskManagerAllowFn allow;
    void* user_data;
  } LrvxRiskManagerCallbacks;

  typedef void* LrvxRiskManagerHandle;

  LRVX_EXPORT(group = "risk")
  LrvxRiskManagerHandle lrvx_risk_manager_create(LrvxRiskManagerCallbacks callbacks);
  LRVX_EXPORT(group = "risk")
  LrvxRiskManagerHandle lrvx_risk_manager_create_p(const LrvxRiskManagerCallbacks* callbacks);
  LRVX_EXPORT(group = "risk")
  void lrvx_risk_manager_destroy(LrvxRiskManagerHandle rm);

  // ============================================================
  // KillSwitch — global halt hook. Fires before OrderValidator and
  // RiskManager (cheap check first); when `check` returns 0, the signal
  // is dropped and downstream hooks are skipped.
  //
  // Use this for "trading halted, no orders allowed" semantics. The
  // binding manages the trigger state itself; the C API just asks via
  // the callback. NULL `check` is a no-op.
  // ============================================================

  typedef uint8_t (*LrvxKillSwitchCheckFn)(void* user_data,
                                           const LrvxSignal* signal);

  typedef struct
  {
    LrvxKillSwitchCheckFn check;
    void* user_data;
  } LrvxKillSwitchCallbacks;

  typedef void* LrvxKillSwitchHandle;

  LRVX_EXPORT(group = "risk")
  LrvxKillSwitchHandle lrvx_kill_switch_create(LrvxKillSwitchCallbacks callbacks);
  LRVX_EXPORT(group = "risk")
  LrvxKillSwitchHandle lrvx_kill_switch_create_p(const LrvxKillSwitchCallbacks* callbacks);
  LRVX_EXPORT(group = "risk")
  void lrvx_kill_switch_destroy(LrvxKillSwitchHandle ks);

  // ============================================================
  // OrderValidator — sanity check. Fires after KillSwitch and before
  // RiskManager. When `validate` returns 0, the signal is dropped and
  // RiskManager is skipped.
  //
  // Use this to catch malformed orders (qty <= 0, missing price on a
  // limit, etc.) before any business-logic evaluation. NULL `validate`
  // is a no-op.
  // ============================================================

  typedef uint8_t (*LrvxOrderValidatorValidateFn)(void* user_data,
                                                  const LrvxSignal* signal);

  typedef struct
  {
    LrvxOrderValidatorValidateFn validate;
    void* user_data;
  } LrvxOrderValidatorCallbacks;

  typedef void* LrvxOrderValidatorHandle;

  LRVX_EXPORT(group = "risk")
  LrvxOrderValidatorHandle lrvx_order_validator_create(LrvxOrderValidatorCallbacks callbacks);
  LRVX_EXPORT(group = "risk")
  LrvxOrderValidatorHandle lrvx_order_validator_create_p(const LrvxOrderValidatorCallbacks* callbacks);
  LRVX_EXPORT(group = "risk")
  void lrvx_order_validator_destroy(LrvxOrderValidatorHandle ov);

  // ============================================================
  // Logger — process-wide log redirection callback.
  //
  // LRVX_LOG_INFO / WARN / ERROR macros normally write to stderr via
  // the bundled ConsoleLogger. Setting a callback here redirects every
  // subsequent log call to the binding's language-native logger
  // (Python `logging`, Node `console`, etc.).
  //
  // `level`: 0 = Info, 1 = Warn, 2 = Error.
  // `msg`:   null-terminated UTF-8 string. Valid only for the duration
  //          of the callback; copy if retained.
  //
  // Single global setter — no per-engine isolation, by design (logs
  // are a process concern). Pass NULL to restore the default
  // ConsoleLogger.
  //
  // Thread-safe: the global pointer is atomic; callbacks may fire from
  // any consumer thread. The user-supplied callback must therefore be
  // thread-safe.
  // ============================================================

  typedef void (*LrvxLogCallback)(void* user_data, int32_t level, const char* msg);

  LRVX_EXPORT(group = "logger")
  void lrvx_set_log_callback(LrvxLogCallback callback, void* user_data);

  // ============================================================
  // PnLTracker — post-emission observer hook.
  //
  // Unlike the pre-trade gates (KillSwitch / OrderValidator /
  // RiskManager) which can DROP a signal, PnLTracker fires **after**
  // the user's on_signal callback runs. It's an observer; the return
  // type is void and the signal has already been delivered to the
  // binding. Use this for shadow-tracking exposure based on emitted
  // signals — independent of any later fill confirmation from a
  // broker.
  //
  // For real fill-driven P&L, attach the binding's tracker to its
  // own broker callback path; the C API doesn't surface fill events
  // beyond the SimulatedExecutor.
  //
  // Lifecycle / threading match RiskManager: NULL detaches; atomic
  // pointer swap is safe with consumer threads active.
  // ============================================================

  typedef void (*LrvxPnLTrackerOnSignalFn)(void* user_data, const LrvxSignal* signal);

  typedef struct
  {
    LrvxPnLTrackerOnSignalFn on_signal;
    void* user_data;
  } LrvxPnLTrackerCallbacks;

  typedef void* LrvxPnLTrackerHandle;

  LRVX_EXPORT(group = "metrics")
  LrvxPnLTrackerHandle lrvx_pnl_tracker_create(LrvxPnLTrackerCallbacks callbacks);
  LRVX_EXPORT(group = "metrics")
  LrvxPnLTrackerHandle lrvx_pnl_tracker_create_p(const LrvxPnLTrackerCallbacks* callbacks);
  LRVX_EXPORT(group = "metrics")
  void lrvx_pnl_tracker_destroy(LrvxPnLTrackerHandle tracker);

  // ============================================================
  // StorageSink — persist every emitted signal.
  //
  // Same shape and timing as PnLTracker — fires after on_signal,
  // never blocks. Use this to write each emitted signal to the
  // binding's storage of choice (DB, append-only log, broker audit
  // trail).
  //
  // The signal pointer is read-only and valid only for the duration
  // of the callback; copy if retained.
  // ============================================================

  typedef void (*LrvxStorageSinkStoreFn)(void* user_data, const LrvxSignal* signal);

  typedef struct
  {
    LrvxStorageSinkStoreFn store;
    void* user_data;
  } LrvxStorageSinkCallbacks;

  typedef void* LrvxStorageSinkHandle;

  LRVX_EXPORT(group = "storage")
  LrvxStorageSinkHandle lrvx_storage_sink_create(LrvxStorageSinkCallbacks callbacks);
  LRVX_EXPORT(group = "storage")
  LrvxStorageSinkHandle lrvx_storage_sink_create_p(const LrvxStorageSinkCallbacks* callbacks);
  LRVX_EXPORT(group = "storage")
  void lrvx_storage_sink_destroy(LrvxStorageSinkHandle sink);

  // ============================================================
  // MarketDataRecorder — receive every market data event fed into the
  // engine, for custom recording (CSV, parquet, custom binary).
  //
  // Two flavours share the same LrvxMarketDataRecorderHandle plug
  // socket on `lrvx_runner_set_market_data_recorder` and
  // `lrvx_live_engine_set_market_data_recorder`:
  //   1. User-callback hook (`lrvx_market_data_recorder_create`). The
  //      binding implements on_trade / on_book_update etc. in the host
  //      language.
  //   2. Binary-log sink (`lrvx_binary_log_recorder_hook_create`).
  //      Built-in `.lrvx` writer; events stay in C++ on the hot
  //      path and are written via `BinaryLogWriter` without crossing
  //      the binding boundary.
  //
  // Callbacks fire on every published trade and book update:
  // synchronously for the runner, on the consumer thread for the live
  // engine. on_start / on_stop fire on engine.start() / engine.stop()
  // while the recorder is attached. Any callback may be NULL (no-op).
  //
  // The trade / book pointers and book level arrays are valid only for
  // the duration of the callback; copy if retained.
  //
  // on_book_update's uint8_t is a byte, not a bool: 1 snapshot, 0 delta,
  // 2 top-of-book (one level per side, not book depth). The two boolean
  // values predate the third and keep their meaning.
  // ============================================================

  typedef void (*LrvxRecorderOnTradeFn)(void* user_data, const LrvxTradeData* trade);
  typedef void (*LrvxRecorderOnBookUpdateFn)(void* user_data,
                                             uint32_t symbol,
                                             uint8_t is_snapshot,
                                             const LrvxBookLevel* bids,
                                             uint32_t n_bids,
                                             const LrvxBookLevel* asks,
                                             uint32_t n_asks,
                                             int64_t exchange_ts_ns);
  typedef void (*LrvxRecorderLifecycleFn)(void* user_data);

  typedef struct
  {
    LrvxRecorderOnTradeFn on_trade;
    LrvxRecorderOnBookUpdateFn on_book_update;
    LrvxRecorderLifecycleFn on_start;
    LrvxRecorderLifecycleFn on_stop;
    void* user_data;
  } LrvxMarketDataRecorderCallbacks;

  typedef void* LrvxMarketDataRecorderHandle;

  LRVX_EXPORT(group = "recorder")
  LrvxMarketDataRecorderHandle
  lrvx_market_data_recorder_create(LrvxMarketDataRecorderCallbacks callbacks);
  LRVX_EXPORT(group = "recorder")
  LrvxMarketDataRecorderHandle
  lrvx_market_data_recorder_create_p(const LrvxMarketDataRecorderCallbacks* callbacks);
  LRVX_EXPORT(group = "recorder")
  void lrvx_market_data_recorder_destroy(LrvxMarketDataRecorderHandle recorder);

  // Binary-log recorder hook. Owns a BinaryLogWriter internally and
  // writes trades + books on the engine thread without crossing the
  // binding boundary. `compression` matches
  // `lrvx::replay::CompressionType`: 0 = None, 1 = LZ4.
  typedef void* LrvxBinaryLogRecorderHookHandle;

  LRVX_EXPORT(group = "binary_log_recorder_hook")
  LrvxBinaryLogRecorderHookHandle
  lrvx_binary_log_recorder_hook_create(const char* output_dir,
                                       uint64_t max_segment_mb,
                                       uint8_t exchange_id,
                                       uint8_t compression);

  // Variant that stamps RecordingMetadata.exchange / instrument_type
  // into the tape manifest. MergedTapeReader keys tapes by
  // (exchange, name), so any binding that writes tapes that need to be
  // merge-readable must call this form. Pass NULL or "" for either
  // string to leave it empty.
  LRVX_EXPORT(group = "binary_log_recorder_hook")
  LrvxBinaryLogRecorderHookHandle
  lrvx_binary_log_recorder_hook_create_ex(const char* output_dir,
                                          uint64_t max_segment_mb,
                                          uint8_t exchange_id,
                                          uint8_t compression,
                                          const char* exchange_name,
                                          const char* instrument_type);

  LRVX_EXPORT(group = "binary_log_recorder_hook")
  void lrvx_binary_log_recorder_hook_destroy(LrvxBinaryLogRecorderHookHandle hook);

  // Borrowed handle compatible with lrvx_runner_set_market_data_recorder
  // and lrvx_live_engine_set_market_data_recorder. Lifetime is tied to
  // the owning hook. DO NOT pass to lrvx_market_data_recorder_destroy.
  LRVX_EXPORT(group = "binary_log_recorder_hook")
  LrvxMarketDataRecorderHandle
  lrvx_binary_log_recorder_hook_as_recorder(LrvxBinaryLogRecorderHookHandle hook);

  LRVX_EXPORT(group = "binary_log_recorder_hook")
  void lrvx_binary_log_recorder_hook_add_symbol(LrvxBinaryLogRecorderHookHandle hook,
                                                uint32_t symbol_id,
                                                const char* name,
                                                const char* base,
                                                const char* quote,
                                                int8_t price_precision,
                                                int8_t qty_precision);

  LRVX_EXPORT(group = "binary_log_recorder_hook")
  void lrvx_binary_log_recorder_hook_flush(LrvxBinaryLogRecorderHookHandle hook);

  LRVX_EXPORT(group = "binary_log_recorder_hook")
  LrvxWriterStats
  lrvx_binary_log_recorder_hook_stats(LrvxBinaryLogRecorderHookHandle hook);

  // ============================================================
  // ReplaySource — binding-side market data event source.
  //
  // Bindings provide a custom event reader (e.g. exchange-specific
  // archive format, S3-streamed segments) that BacktestRunner pulls
  // events from. The engine drives playback by repeatedly calling
  // `next` until it returns 0.
  //
  // Event shape: `next` populates a LrvxReplayEvent describing one
  // event (Trade, BookSnapshot, BookDelta, or BookTop). For book events, the
  // binding sets `bids` / `asks` to point at its own buffer; the
  // pointers must remain valid until the next `next` call. The engine
  // copies the levels it needs before invoking the callback again.
  // ============================================================

  // 1=Trade, 2=BookSnapshot, 3=BookDelta, 6=BookTop. Matches lrvx::replay::EventType.
  typedef struct
  {
    uint8_t type;
    uint8_t _pad[3];
    int64_t timestamp_ns;

    // Trade payload — valid only when type == 1.
    uint32_t trade_symbol;
    uint8_t trade_is_buy;
    uint8_t _pad2[3];
    int64_t trade_price_raw;
    int64_t trade_quantity_raw;

    // Book payload — valid when type == 2 or 3.
    uint32_t book_symbol;
    uint32_t n_bids;
    uint32_t n_asks;
    uint32_t _pad3;
    // Pointers owned by the binding; must remain valid for the duration
    // of the engine's processing of this event (until the next `next`
    // call returns).
    const LrvxBookLevel* bids;
    const LrvxBookLevel* asks;
  } LrvxReplayEvent;

  // Yield the next event. Return 1 if an event was produced (event_out
  // populated), 0 if the source is exhausted.
  typedef uint8_t (*LrvxReplaySourceNextFn)(void* user_data, LrvxReplayEvent* event_out);
  // Seek to a timestamp. Return 1 if the seek succeeded, 0 otherwise.
  typedef uint8_t (*LrvxReplaySourceSeekFn)(void* user_data, int64_t timestamp_ns);
  typedef void (*LrvxReplaySourceLifecycleFn)(void* user_data);

  typedef struct
  {
    LrvxReplaySourceLifecycleFn on_start;
    LrvxReplaySourceLifecycleFn on_stop;
    LrvxReplaySourceSeekFn seek_to;
    LrvxReplaySourceNextFn next;
    void* user_data;
  } LrvxReplaySourceCallbacks;

  typedef void* LrvxReplaySourceHandle;

  LRVX_EXPORT(group = "replay")
  LrvxReplaySourceHandle lrvx_replay_source_create(LrvxReplaySourceCallbacks callbacks);
  LRVX_EXPORT(group = "replay")
  LrvxReplaySourceHandle lrvx_replay_source_create_p(const LrvxReplaySourceCallbacks* callbacks);
  LRVX_EXPORT(group = "replay")
  void lrvx_replay_source_destroy(LrvxReplaySourceHandle source);

  // Seek the underlying source to a timestamp. Returns 1 on success.
  // Convenience wrapper that forwards to the binding's seek_to callback.
  LRVX_EXPORT(group = "replay")
  uint8_t lrvx_replay_source_seek_to(LrvxReplaySourceHandle source, int64_t timestamp_ns);

  // ============================================================
  // ExecutionListener — observe order lifecycle events.
  //
  // Bindings register callbacks that fire as orders move through the
  // execution path: submitted → accepted → (partially) filled / canceled
  // / rejected / expired / replaced. Used as a backtest observer (the
  // SimulatedExecutor inside BacktestRunner emits these events) or to
  // surface live-broker fills back into binding code.
  //
  // The order pointer is read-only and valid only for the duration of
  // the callback; copy the fields you need to retain.
  // Any callback may be NULL (no-op).
  // ============================================================

  // ABI-stable order snapshot, mirrors lrvx::Order. Raw fields are
  // fixed-point int64 (Price * 1e8, Quantity * 1e8) — matches the rest
  // of the C API.
  typedef struct
  {
    uint64_t id;
    uint64_t client_order_id;
    uint32_t symbol;
    uint16_t strategy_id;
    uint16_t order_tag;
    uint8_t side;           // 0=BUY, 1=SELL
    uint8_t type;           // OrderType enum (0=LIMIT, 1=MARKET, ...)
    uint8_t time_in_force;  // TimeInForce enum
    uint8_t flags;          // ExecutionFlags packed bits
    int64_t price_raw;
    int64_t quantity_raw;
    int64_t filled_quantity_raw;
    int64_t trigger_price_raw;
    int64_t trailing_offset_raw;
    int64_t created_at_ns;
    int64_t exchange_ts_ns;
  } LrvxOrder;

  typedef void (*LrvxExecListenerOnOrderFn)(void* user_data, const LrvxOrder* order);
  typedef void (*LrvxExecListenerOnPartialFillFn)(void* user_data,
                                                  const LrvxOrder* order,
                                                  int64_t fill_quantity_raw);
  typedef void (*LrvxExecListenerOnRejectedFn)(void* user_data,
                                               const LrvxOrder* order,
                                               const char* reason);
  typedef void (*LrvxExecListenerOnReplacedFn)(void* user_data,
                                               const LrvxOrder* old_order,
                                               const LrvxOrder* new_order);
  typedef void (*LrvxExecListenerOnTrailingUpdateFn)(void* user_data,
                                                     const LrvxOrder* order,
                                                     int64_t new_trigger_price_raw);
  typedef void (*LrvxExecListenerOnQueuePositionChangeFn)(void* user_data,
                                                          const LrvxOrder* order,
                                                          int64_t queue_ahead_raw,
                                                          int64_t queue_total_raw);
  typedef void (*LrvxExecListenerOnMarketPositionChangeFn)(void* user_data,
                                                           const LrvxOrder* order,
                                                           uint8_t market_position,
                                                           int32_t distance_to_best_ticks);
  typedef void (*LrvxExecListenerOnReplaceSubmittedFn)(void* user_data,
                                                       const LrvxOrder* old_order,
                                                       const LrvxOrder* new_order);
  typedef void (*LrvxExecListenerOnReplaceAcceptedFn)(void* user_data,
                                                      const LrvxOrder* old_order,
                                                      const LrvxOrder* new_order);
  typedef void (*LrvxExecListenerOnReplaceRejectedFn)(void* user_data,
                                                      const LrvxOrder* old_order,
                                                      const LrvxOrder* new_order,
                                                      const char* reason);

  typedef struct
  {
    LrvxExecListenerOnOrderFn on_submitted;
    LrvxExecListenerOnOrderFn on_accepted;
    LrvxExecListenerOnPartialFillFn on_partially_filled;
    LrvxExecListenerOnOrderFn on_filled;
    LrvxExecListenerOnOrderFn on_pending_cancel;
    LrvxExecListenerOnOrderFn on_canceled;
    LrvxExecListenerOnOrderFn on_expired;
    LrvxExecListenerOnRejectedFn on_rejected;
    LrvxExecListenerOnReplacedFn on_replaced;
    LrvxExecListenerOnOrderFn on_pending_trigger;
    LrvxExecListenerOnOrderFn on_triggered;
    LrvxExecListenerOnTrailingUpdateFn on_trailing_stop_updated;
    LrvxExecListenerOnQueuePositionChangeFn on_queue_position_change;
    LrvxExecListenerOnMarketPositionChangeFn on_market_position_change;
    LrvxExecListenerOnReplaceSubmittedFn on_replace_submitted;
    LrvxExecListenerOnReplaceAcceptedFn on_replace_accepted;
    LrvxExecListenerOnReplaceRejectedFn on_replace_rejected;
    void* user_data;
  } LrvxExecutionListenerCallbacks;

  typedef void* LrvxExecutionListenerHandle;

  LRVX_EXPORT(group = "execution")
  LrvxExecutionListenerHandle
  lrvx_execution_listener_create(LrvxExecutionListenerCallbacks callbacks);
  LRVX_EXPORT(group = "execution")
  LrvxExecutionListenerHandle
  lrvx_execution_listener_create_p(const LrvxExecutionListenerCallbacks* callbacks);
  LRVX_EXPORT(group = "execution")
  void lrvx_execution_listener_destroy(LrvxExecutionListenerHandle listener);

  // Attaching listeners to BacktestRunner is declared with the rest of
  // the BacktestRunner API below (after LrvxBacktestRunnerHandle).

  // ============================================================
  // Executor — binding-supplied IOrderExecutor.
  //
  // Bindings provide an executor that places, cancels, replaces and
  // OCO-submits orders on a real broker (or on a custom simulator). The
  // engine routes every signal through this executor instead of the
  // built-in SimulatedExecutor when one is attached.
  //
  // The concrete in-process SimulatedExecutor is exposed separately as
  // lrvx_simulated_executor_*.
  //
  // The order pointer is read-only and valid only for the duration of
  // the callback; copy fields you need to retain. Any callback may be
  // NULL (no-op).
  // ============================================================

  // Mirror of lrvx::ExchangeCapabilities. Bindings fill this in their
  // capabilities() callback so the engine knows which order types,
  // time-in-force values and execution flags the venue supports.
  typedef struct
  {
    uint8_t supports_stop_market;
    uint8_t supports_stop_limit;
    uint8_t supports_take_profit_market;
    uint8_t supports_take_profit_limit;
    uint8_t supports_trailing_stop;
    uint8_t supports_iceberg;
    uint8_t supports_oco;
    uint8_t supports_gtc;
    uint8_t supports_ioc;
    uint8_t supports_fok;
    uint8_t supports_gtd;
    uint8_t supports_post_only;
    uint8_t supports_reduce_only;
    uint8_t supports_close_position;
    // Native batch submit behind IOrderExecutor::submitOrders. Takes a pad
    // byte, so the struct layout is unchanged.
    uint8_t supports_batch_submit;
    uint8_t _pad[1];
  } LrvxExchangeCapabilities;

  typedef void (*LrvxExecutorSubmitFn)(void* user_data, const LrvxOrder* order);
  typedef void (*LrvxExecutorCancelFn)(void* user_data, uint64_t order_id);
  typedef void (*LrvxExecutorCancelAllFn)(void* user_data, uint32_t symbol);
  typedef void (*LrvxExecutorReplaceFn)(void* user_data,
                                        uint64_t old_order_id,
                                        const LrvxOrder* new_order);
  typedef void (*LrvxExecutorSubmitOCOFn)(void* user_data,
                                          const LrvxOrder* order1,
                                          const LrvxOrder* order2);
  typedef void (*LrvxExecutorCapabilitiesFn)(void* user_data,
                                             LrvxExchangeCapabilities* caps_out);
  typedef void (*LrvxExecutorLifecycleFn)(void* user_data);

  typedef struct
  {
    LrvxExecutorSubmitFn submit;
    LrvxExecutorCancelFn cancel;
    LrvxExecutorCancelAllFn cancel_all;
    LrvxExecutorReplaceFn replace;
    LrvxExecutorSubmitOCOFn submit_oco;
    LrvxExecutorCapabilitiesFn capabilities;
    LrvxExecutorLifecycleFn on_start;
    LrvxExecutorLifecycleFn on_stop;
    void* user_data;
  } LrvxExecutorCallbacks;

  typedef void* LrvxExecutorHandle;

  LRVX_EXPORT(group = "execution")
  LrvxExecutorHandle lrvx_executor_create(LrvxExecutorCallbacks callbacks);
  LRVX_EXPORT(group = "execution")
  LrvxExecutorHandle lrvx_executor_create_p(const LrvxExecutorCallbacks* callbacks);
  LRVX_EXPORT(group = "execution")
  void lrvx_executor_destroy(LrvxExecutorHandle executor);

  // Query the executor's reported capabilities. Forwards to the binding's
  // capabilities() callback. caps_out is zeroed if no callback registered.
  LRVX_EXPORT(group = "execution")
  void lrvx_executor_get_capabilities(LrvxExecutorHandle executor,
                                      LrvxExchangeCapabilities* caps_out);

  // ============================================================
  // LrvxLiveEngine — Disruptor-based live trading engine.
  //
  // Uses real EventBus (SPSC ring buffer / Disruptor) internally.
  // Each subscribed strategy runs in its own bus consumer thread.
  // Publish functions are called from the caller's thread (e.g., Python
  // asyncio, Node.js event loop, Codon main thread) and are lock-free.
  //
  // Usage:
  //   1. Create a registry and register symbols.
  //   2. Create a live engine.
  //   3. For each strategy: create via lrvx_strategy_create, then
  //      call lrvx_live_engine_add_strategy (assigns a signal handler).
  //   4. lrvx_live_engine_start() — spawns consumer threads.
  //   5. Push market data: lrvx_live_engine_publish_trade / book_snapshot.
  //      Returns immediately; strategy callbacks fire in consumer threads.
  //   6. lrvx_live_engine_stop() — drains buses, joins threads.
  // ============================================================

  typedef void* LrvxLiveEngineHandle;

  LRVX_EXPORT(group = "lrvxliveengine_disruptor")
  LrvxLiveEngineHandle lrvx_live_engine_create(LrvxRegistryHandle registry);
  LRVX_EXPORT(group = "lrvxliveengine_disruptor")
  void lrvx_live_engine_destroy(LrvxLiveEngineHandle engine);

  // Attach a strategy to both TradeBus and BookUpdateBus.
  // on_signal is called from the consumer thread when the strategy emits an order.
  // The caller must ensure thread safety when submitting orders from on_signal.
  LRVX_EXPORT(group = "lrvxliveengine_disruptor")
  void lrvx_live_engine_add_strategy(LrvxLiveEngineHandle engine,
                                     LrvxStrategyHandle strategy,
                                     LrvxOnSignalCallback on_signal,
                                     void* user_data);

  // Attach (or detach with rm = NULL) a risk manager to this engine.
  // The risk manager's `allow` callback fires on every signal before
  // it reaches the user-supplied on_signal callback. Safe to call before
  // or after start(); the engine takes a non-owning reference.
  LRVX_EXPORT(group = "risk")
  void lrvx_live_engine_set_risk_manager(LrvxLiveEngineHandle engine,
                                         LrvxRiskManagerHandle rm);

  // Attach (or detach with NULL) the global kill-switch and the per-order
  // validator. Evaluation order is KillSwitch → OrderValidator → RiskManager.
  LRVX_EXPORT(group = "risk")
  void lrvx_live_engine_set_kill_switch(LrvxLiveEngineHandle engine,
                                        LrvxKillSwitchHandle ks);
  LRVX_EXPORT(group = "risk")
  void lrvx_live_engine_set_order_validator(LrvxLiveEngineHandle engine,
                                            LrvxOrderValidatorHandle ov);

  // Post-emission observers. Fire after on_signal, in the order
  // PnLTracker → StorageSink. Detach with NULL.
  LRVX_EXPORT(group = "metrics")
  void lrvx_live_engine_set_pnl_tracker(LrvxLiveEngineHandle engine,
                                        LrvxPnLTrackerHandle tracker);
  LRVX_EXPORT(group = "storage")
  void lrvx_live_engine_set_storage_sink(LrvxLiveEngineHandle engine,
                                         LrvxStorageSinkHandle sink);

  // Attach (or detach with NULL) a market data recorder. Fires on every
  // published trade and book update; on_start / on_stop fire on engine
  // start/stop while attached.
  LRVX_EXPORT(group = "recorder")
  void lrvx_live_engine_set_market_data_recorder(LrvxLiveEngineHandle engine,
                                                 LrvxMarketDataRecorderHandle recorder);

  // Attach (or detach with NULL) a binding-supplied executor. When set,
  // emitted signals are forwarded to the executor (submit / cancel /
  // replace / cancel_all / submit_oco) instead of relying on the user's
  // on_signal callback to route the order to a broker. on_start /
  // on_stop fire balanced against engine start/stop.
  LRVX_EXPORT(group = "execution")
  void lrvx_live_engine_set_executor(LrvxLiveEngineHandle engine,
                                     LrvxExecutorHandle executor);

  LRVX_EXPORT(group = "lrvxliveengine_disruptor")
  void lrvx_live_engine_start(LrvxLiveEngineHandle engine);
  LRVX_EXPORT(group = "lrvxliveengine_disruptor")
  void lrvx_live_engine_stop(LrvxLiveEngineHandle engine);

  // Publish a trade tick to the TradeBus.
  // Lock-free. Returns immediately; consumer threads process asynchronously.
  LRVX_EXPORT(group = "lrvxliveengine_disruptor")
  void lrvx_live_engine_publish_trade(LrvxLiveEngineHandle engine,
                                      uint32_t symbol,
                                      double price, double qty, uint8_t is_buy,
                                      int64_t exchange_ts_ns);

  // Publish a full L2 book snapshot to the BookUpdateBus.
  // Lock-free. Returns immediately; consumer threads process asynchronously.
  LRVX_EXPORT(group = "lrvxliveengine_disruptor")
  void lrvx_live_engine_publish_book_snapshot(LrvxLiveEngineHandle engine,
                                              uint32_t symbol,
                                              const double* bid_prices,
                                              const double* bid_qtys,
                                              uint32_t n_bids,
                                              const double* ask_prices,
                                              const double* ask_qtys,
                                              uint32_t n_asks,
                                              int64_t exchange_ts_ns);

  // Publish a closed OHLC bar to the BarBus.
  // Lock-free. Returns immediately; consumer threads process asynchronously.
  LRVX_EXPORT(group = "lrvxliveengine_disruptor")
  void lrvx_live_engine_publish_bar(LrvxLiveEngineHandle engine,
                                    uint32_t symbol,
                                    uint8_t bar_type, uint64_t bar_type_param,
                                    double open, double high, double low, double close,
                                    double volume, double buy_volume,
                                    int64_t start_time_ns, int64_t end_time_ns,
                                    uint8_t close_reason);

  // ============================================================
  // StrategyRunner — synchronous strategy host for live trading.
  //
  // Designed for Python, Codon, Node.js, and other language runtimes
  // that bring their own event loop and market data source (e.g. CCXT).
  //
  // Usage:
  //   1. Create a registry and register symbols.
  //   2. Create a runner with an on_signal callback.
  //   3. Create strategies and add them to the runner.
  //   4. Call lrvx_runner_start().
  //   5. Push market events (trades, book snapshots) as they arrive.
  //      Strategy callbacks fire synchronously before the call returns.
  //      When a strategy emits an order, on_signal is called immediately.
  //   6. Submit the received orders via your own execution backend.
  // ============================================================

  typedef void* LrvxRunnerHandle;

  LRVX_EXPORT(group = "strategyrunner_synchronous")
  LrvxRunnerHandle lrvx_runner_create(LrvxRegistryHandle registry,
                                      LrvxOnSignalCallback on_signal,
                                      void* user_data);
  LRVX_EXPORT(group = "strategyrunner_synchronous")
  void lrvx_runner_destroy(LrvxRunnerHandle runner);

  // Attach a strategy (created via lrvx_strategy_create) to the runner.
  // The runner does NOT take ownership; call lrvx_strategy_destroy separately.
  LRVX_EXPORT(group = "strategyrunner_synchronous")
  void lrvx_runner_add_strategy(LrvxRunnerHandle runner, LrvxStrategyHandle strategy);

  // Attach (or detach with rm = NULL) a risk manager to this runner.
  // Same semantics as lrvx_live_engine_set_risk_manager — the `allow`
  // callback fires synchronously on every signal before on_signal does.
  LRVX_EXPORT(group = "risk")
  void lrvx_runner_set_risk_manager(LrvxRunnerHandle runner,
                                    LrvxRiskManagerHandle rm);

  // Same shape: attach/detach a kill-switch and an order validator.
  // Evaluation order on every signal: KillSwitch → OrderValidator → RiskManager.
  LRVX_EXPORT(group = "risk")
  void lrvx_runner_set_kill_switch(LrvxRunnerHandle runner,
                                   LrvxKillSwitchHandle ks);
  LRVX_EXPORT(group = "risk")
  void lrvx_runner_set_order_validator(LrvxRunnerHandle runner,
                                       LrvxOrderValidatorHandle ov);

  // Post-emission observers. Fire after on_signal in the order
  // PnLTracker → StorageSink.
  LRVX_EXPORT(group = "metrics")
  void lrvx_runner_set_pnl_tracker(LrvxRunnerHandle runner,
                                   LrvxPnLTrackerHandle tracker);
  LRVX_EXPORT(group = "storage")
  void lrvx_runner_set_storage_sink(LrvxRunnerHandle runner,
                                    LrvxStorageSinkHandle sink);

  // Attach (or detach with NULL) a market data recorder. on_trade and
  // on_book_update fire synchronously from lrvx_runner_on_trade /
  // lrvx_runner_on_book_snapshot. on_start / on_stop fire on runner
  // start/stop.
  LRVX_EXPORT(group = "recorder")
  void lrvx_runner_set_market_data_recorder(LrvxRunnerHandle runner,
                                            LrvxMarketDataRecorderHandle recorder);

  // Attach (or detach with NULL) a binding-supplied executor. When set,
  // emitted signals are forwarded to the executor (submit / cancel /
  // replace / cancel_all / submit_oco) in addition to the user's
  // on_signal callback. on_start / on_stop fire balanced against runner
  // start/stop.
  LRVX_EXPORT(group = "execution")
  void lrvx_runner_set_executor(LrvxRunnerHandle runner,
                                LrvxExecutorHandle executor);

  LRVX_EXPORT(group = "strategyrunner_synchronous")
  void lrvx_runner_start(LrvxRunnerHandle runner);
  LRVX_EXPORT(group = "strategyrunner_synchronous")
  void lrvx_runner_stop(LrvxRunnerHandle runner);

  // Push a trade tick. Strategy on_trade callbacks fire synchronously.
  LRVX_EXPORT(group = "strategyrunner_synchronous")
  void lrvx_runner_on_trade(LrvxRunnerHandle runner, uint32_t symbol,
                            double price, double qty, uint8_t is_buy,
                            int64_t exchange_ts_ns);

  // Push a full L2 book snapshot. Strategy on_book callbacks fire synchronously.
  LRVX_EXPORT(group = "strategyrunner_synchronous")
  void lrvx_runner_on_book_snapshot(LrvxRunnerHandle runner, uint32_t symbol,
                                    const double* bid_prices, const double* bid_qtys,
                                    uint32_t n_bids,
                                    const double* ask_prices, const double* ask_qtys,
                                    uint32_t n_asks,
                                    int64_t exchange_ts_ns);

  // Push a closed OHLC bar. Strategy on_bar callbacks fire synchronously.
  // bar_type / bar_type_param / close_reason match LrvxBarData.
  LRVX_EXPORT(group = "strategyrunner_synchronous")
  void lrvx_runner_on_bar(LrvxRunnerHandle runner, uint32_t symbol,
                          uint8_t bar_type, uint64_t bar_type_param,
                          double open, double high, double low, double close,
                          double volume, double buy_volume,
                          int64_t start_time_ns, int64_t end_time_ns,
                          uint8_t close_reason);

  // ============================================================
  // BacktestRunner — replay OHLCV data through a Strategy.
  //
  // Same Strategy class as StrategyRunner / LiveEngine; just a different
  // host. Emitted orders go to SimulatedExecutor; stats returned at end.
  //
  // Usage:
  //   1. Create registry, register symbols (lrvx_registry_add_symbol).
  //   2. Create a strategy (lrvx_strategy_create) with on_trade callbacks.
  //   3. lrvx_backtest_runner_create + lrvx_backtest_runner_set_strategy.
  //   4. lrvx_backtest_runner_run_csv  OR  lrvx_backtest_runner_run_ohlcv.
  //   5. Read LrvxBacktestStats out parameter for results.
  //   6. lrvx_backtest_runner_destroy.
  // ============================================================

  typedef void* LrvxBacktestRunnerHandle;

  LRVX_EXPORT(group = "backtestrunner_replay")
  LrvxBacktestRunnerHandle lrvx_backtest_runner_create(LrvxRegistryHandle registry,
                                                       double fee_rate,
                                                       double initial_capital);
  LRVX_EXPORT(group = "backtestrunner_replay")
  void lrvx_backtest_runner_destroy(LrvxBacktestRunnerHandle runner);

  // Attach a strategy. BacktestRunner becomes the signal handler — emitted
  // orders are routed to SimulatedExecutor automatically.
  LRVX_EXPORT(group = "backtestrunner_replay")
  void lrvx_backtest_runner_set_strategy(LrvxBacktestRunnerHandle runner,
                                         LrvxStrategyHandle strategy);

  // Replay a CSV file (columns: timestamp, open, high, low, close, volume).
  // Returns 1 on success, 0 on error.
  LRVX_EXPORT(group = "backtestrunner_replay")
  int lrvx_backtest_runner_run_csv(LrvxBacktestRunnerHandle runner,
                                   const char* path,
                                   const char* symbol,
                                   LrvxBacktestStats* stats_out);

  // Drive the backtest off a `.lrvx` tape directory. Opens the
  // tape via `replay::createMultiSegmentReader` and dispatches every
  // event through the strategy (Strategy.on_trade / on_book_update
  // depending on what the tape contains).
  // Returns 1 on success, 0 on error.
  LRVX_EXPORT(group = "backtestrunner_replay")
  int lrvx_backtest_runner_run_tape(LrvxBacktestRunnerHandle runner,
                                    const char* tape_dir,
                                    LrvxBacktestStats* stats_out);

  // Drive the backtest off N `.lrvx` tapes merged on read.
  // Returns 1 on success, 0 on error.
  LRVX_EXPORT(group = "backtestrunner_replay")
  int lrvx_backtest_runner_run_tapes(LrvxBacktestRunnerHandle runner,
                                     const char* const* tape_dirs,
                                     uint32_t n_dirs,
                                     LrvxBacktestStats* stats_out);

  // Replay raw OHLCV arrays (timestamps in nanoseconds, close prices as double).
  // Each row produces one synthetic trade (price=close, qty=1). Strategy.on_trade fires.
  // Returns 1 on success, 0 on error.
  LRVX_EXPORT(group = "backtestrunner_replay")
  int lrvx_backtest_runner_run_ohlcv(LrvxBacktestRunnerHandle runner,
                                     const int64_t* timestamps_ns,
                                     const double* close_prices,
                                     uint32_t n,
                                     const char* symbol,
                                     LrvxBacktestStats* stats_out);

  // Replay full OHLCV bars (open/high/low/close/volume). Each row produces one
  // BarEvent. Strategy.on_bar fires; on_trade does NOT.
  // bar_type matches LrvxBarData (0=Time, 1=Tick, …); bar_type_param is the
  // interval in ns / tick count / volume threshold.
  // Returns 1 on success, 0 on error.
  LRVX_EXPORT(group = "backtestrunner_replay")
  int lrvx_backtest_runner_run_bars(LrvxBacktestRunnerHandle runner,
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
                                    LrvxBacktestStats* stats_out);

  // Run a backtest pulling events from a binding-supplied replay source.
  // Fires source.on_start before reading, source.on_stop after the runner
  // has drained the stream. Returns 1 on success, 0 on error.
  LRVX_EXPORT(group = "backtestrunner_replay")
  int lrvx_backtest_runner_run_replay_source(LrvxBacktestRunnerHandle runner,
                                             LrvxReplaySourceHandle source,
                                             LrvxBacktestStats* stats_out);

  // Returns a NEW BacktestResult handle that owns a copy of the runner's
  // last completed result, or NULL if no run has happened yet. Caller
  // takes ownership and must free with lrvx_backtest_result_destroy().
  // Stable across subsequent runs of the same runner — each run
  // overwrites the runner's internal copy, but already-taken handles
  // stay valid until destroyed.
  LRVX_EXPORT(group = "backtestrunner_replay")
  LrvxBacktestResultHandle lrvx_backtest_runner_take_result(LrvxBacktestRunnerHandle runner);

  // Attach a binding-side execution listener to the runner. Multiple
  // listeners may be attached; each one fires for every order event
  // emitted by the SimulatedExecutor. Caller retains ownership.
  LRVX_EXPORT(group = "execution")
  void lrvx_backtest_runner_add_execution_listener(LrvxBacktestRunnerHandle runner,
                                                   LrvxExecutionListenerHandle listener);

  // Attach an OrderJourneyTracer (an IOrderExecutionListener internally) to
  // the runner. The runner does not take ownership; the caller must keep
  // the tracer alive across the run.
  LRVX_EXPORT(group = "order_journey_tracer")
  void lrvx_backtest_runner_add_journey_tracer(LrvxBacktestRunnerHandle runner,
                                               LrvxOrderJourneyTracerHandle tracer);

  // Attach (or detach with NULL) a binding-supplied executor. When set,
  // emitted signals are routed to the executor instead of the built-in
  // SimulatedExecutor. The simulator stays in the runner so backtest
  // result accounting still works against simulator fills; if you want
  // the binding's executor to drive PnL accounting, surface fills via
  // a separate ExecutionListener path.
  LRVX_EXPORT(group = "execution")
  void lrvx_backtest_runner_set_executor(LrvxBacktestRunnerHandle runner,
                                         LrvxExecutorHandle executor);

  // Pre-trade gate parity with the live runner. All four hooks are
  // optional (NULL = no-op). The gate fires on entry-type signals
  // (Market / Limit / Stop* / TP* / TrailingStop); Cancel / CancelAll
  // / Modify pass through. Reduce-only orders bypass the gate so
  // tightening caps cannot strand a strategy in a position.
  LRVX_EXPORT(group = "backtestrunner_replay")
  void lrvx_backtest_runner_set_risk_manager(LrvxBacktestRunnerHandle runner,
                                             LrvxRiskManagerHandle rm);

  LRVX_EXPORT(group = "backtestrunner_replay")
  void lrvx_backtest_runner_set_kill_switch(LrvxBacktestRunnerHandle runner,
                                            LrvxKillSwitchHandle ks);

  LRVX_EXPORT(group = "backtestrunner_replay")
  void lrvx_backtest_runner_set_order_validator(LrvxBacktestRunnerHandle runner,
                                                LrvxOrderValidatorHandle ov);

  LRVX_EXPORT(group = "backtestrunner_replay")
  void lrvx_backtest_runner_set_pnl_tracker(LrvxBacktestRunnerHandle runner,
                                            LrvxPnLTrackerHandle tracker);

  // ============================================================
  // Walk-forward
  // ============================================================
  //
  // Anchored mode: train [0, t]; test [t, t + test_size]; t advances
  // by `step`. min_train_size sets the first split.
  // Sliding mode: train [t, t + train_size]; test [t + train_size,
  // t + train_size + test_size]; t advances by `step`.
  typedef struct
  {
    uint8_t mode;             // 0 = Anchored, 1 = Sliding
    uint64_t train_size;      // sliding mode only (bars)
    uint64_t test_size;       // bars per test window
    uint64_t step;            // 0 → defaults to test_size
    uint64_t min_train_size;  // anchored mode only (bars before first fold)
  } LrvxWalkForwardConfig;

  typedef struct
  {
    uint64_t fold_index;
    uint64_t train_start_bar;
    uint64_t train_end_bar;
    uint64_t test_start_bar;
    uint64_t test_end_bar;
    int64_t train_start_ns;
    int64_t train_end_ns;
    int64_t test_start_ns;
    int64_t test_end_ns;
    LrvxBacktestStats train_stats;
    LrvxBacktestStats test_stats;
  } LrvxWalkForwardFold;

  // Per-fold strategy factory. Called twice per fold (once for train,
  // once for test). Caller (binding) returns a fresh strategy handle
  // each call; the handle ownership stays with the caller — the
  // engine does not destroy it.
  typedef LrvxStrategyHandle (*LrvxWalkForwardFactoryFn)(
      void* user_data, uint64_t fold_index);

  // Run walk-forward over a CSV. Returns the total fold count.
  // If folds_out is NULL, computes the total without running anything;
  // pass max_folds = total in a second call to fill the buffer.
  LRVX_EXPORT(group = "walk_forward")
  uint32_t lrvx_walk_forward_run_csv(LrvxRegistryHandle registry,
                                     const char* csv_path, const char* symbol,
                                     double fee_rate, double initial_capital,
                                     const LrvxWalkForwardConfig* cfg,
                                     LrvxWalkForwardFactoryFn factory,
                                     void* user_data,
                                     LrvxWalkForwardFold* folds_out,
                                     uint32_t max_folds);

  // ============================================================
  // Grid search (sequential)
  // ============================================================
  //
  // Type-erased over vector<double> params. Each axis carries a list
  // of values; total combinations = product of axis lengths. Last axis
  // varies fastest (row-major flatten).
  typedef void* LrvxGridSearchHandle;

  // Factory: caller fills out_stats from a backtest run on `params`.
  // Returns 1 if stats are valid, 0 to leave the slot zero-filled.
  typedef int (*LrvxGridSearchFactoryFn)(
      void* user_data, uint64_t param_index,
      const double* params, uint32_t num_params,
      LrvxBacktestStats* out_stats);

  LRVX_EXPORT(group = "grid_search")
  LrvxGridSearchHandle lrvx_grid_search_create();

  LRVX_EXPORT(group = "grid_search")
  void lrvx_grid_search_destroy(LrvxGridSearchHandle gs);

  LRVX_EXPORT(group = "grid_search")
  void lrvx_grid_search_add_axis(LrvxGridSearchHandle gs,
                                 const double* values, uint32_t num_values);

  LRVX_EXPORT(group = "grid_search")
  uint64_t lrvx_grid_search_total(LrvxGridSearchHandle gs);

  // Decode a flat index into (params_out[0..num_axes]). Returns the
  // number of axes (== params written), or 0 on bad index.
  LRVX_EXPORT(group = "grid_search")
  uint32_t lrvx_grid_search_params_for_index(LrvxGridSearchHandle gs,
                                             uint64_t index,
                                             double* params_out,
                                             uint32_t max_params);

  // Run sequentially. Returns total combinations. If stats_out is NULL,
  // computes total without invoking the factory.
  LRVX_EXPORT(group = "grid_search")
  uint64_t lrvx_grid_search_run(LrvxGridSearchHandle gs,
                                LrvxGridSearchFactoryFn factory,
                                void* user_data,
                                LrvxBacktestStats* stats_out,
                                uint32_t max_results);

  // ============================================================
  // Heatmap rendering
  // ============================================================
  //
  // z is row-major (length = rows * cols). row_labels / col_labels
  // are arrays of NUL-terminated strings; pass NULL (with the count
  // set to 0) to fall back to numeric indices.
  //
  // Two-call size-first pattern: pass NULL out_buf to get the byte
  // count, then allocate and call again. Returns the total size of
  // the rendered HTML in bytes (NOT including a NUL terminator).
  typedef struct
  {
    const double* z;
    uint32_t rows;
    uint32_t cols;
    const char* const* row_labels;
    uint32_t num_row_labels;
    const char* const* col_labels;
    uint32_t num_col_labels;
    const char* title;        // may be NULL
    const char* x_axis_name;  // may be NULL
    const char* y_axis_name;  // may be NULL
    const char* metric_name;  // may be NULL
  } LrvxHeatmapData;

  LRVX_EXPORT(group = "heatmap")
  uint64_t lrvx_render_heatmap_html(const LrvxHeatmapData* data,
                                    char* out_buf, uint64_t max_size);

  // ============================================================
  // Latency models (sampling primitive for backtest realism)
  // ============================================================
  //
  // Each model is created via a typed factory and queried through the
  // shared `lrvx_latency_*` accessors. Construction validates inputs
  // and returns NULL on failure (negative means/stddevs, empty
  // empirical inputs). All delays are returned as non-negative
  // nanoseconds.

  typedef void* LrvxLatencyModelHandle;

  typedef struct
  {
    int64_t feed_ns;
    int64_t order_ns;
    int64_t fill_ns;
  } LrvxLatencySample;

  LRVX_EXPORT(group = "latency_models")
  LrvxLatencyModelHandle lrvx_latency_constant_create(int64_t feed_ns,
                                                      int64_t order_ns,
                                                      int64_t fill_ns);

  LRVX_EXPORT(group = "latency_models")
  LrvxLatencyModelHandle lrvx_latency_gaussian_create(double feed_mean_ns,
                                                      double feed_stddev_ns,
                                                      double order_mean_ns,
                                                      double order_stddev_ns,
                                                      double fill_mean_ns,
                                                      double fill_stddev_ns,
                                                      uint64_t seed);

  LRVX_EXPORT(group = "latency_models")
  LrvxLatencyModelHandle lrvx_latency_exponential_create(double feed_mean_ns,
                                                         double order_mean_ns,
                                                         double fill_mean_ns,
                                                         uint64_t seed);

  // Each samples array is copied; pass NULL with count 0 if a
  // component should always return 0.
  LRVX_EXPORT(group = "latency_models")
  LrvxLatencyModelHandle lrvx_latency_empirical_create(const int64_t* feed_samples,
                                                       size_t feed_count,
                                                       const int64_t* order_samples,
                                                       size_t order_count,
                                                       const int64_t* fill_samples,
                                                       size_t fill_count,
                                                       uint64_t seed);

  LRVX_EXPORT(group = "latency_models")
  void lrvx_latency_destroy(LrvxLatencyModelHandle model);

  LRVX_EXPORT(group = "latency_models")
  int64_t lrvx_latency_feed_delay(LrvxLatencyModelHandle model);

  LRVX_EXPORT(group = "latency_models")
  int64_t lrvx_latency_order_delay(LrvxLatencyModelHandle model);

  LRVX_EXPORT(group = "latency_models")
  int64_t lrvx_latency_fill_delay(LrvxLatencyModelHandle model);

  LRVX_EXPORT(group = "latency_models")
  void lrvx_latency_sample(LrvxLatencyModelHandle model, LrvxLatencySample* out);

  LRVX_EXPORT(group = "latency_models")
  void lrvx_latency_reset(LrvxLatencyModelHandle model, uint64_t seed);

  // ============================================================
  // Tape diff (replay-equivalence localization)
  // ============================================================
  //
  // Walks two .lrvx directories trade-by-trade and produces a
  // structured result the binding can render. The handle owns the
  // result vector; copy fields out via the accessors and free with
  // lrvx_tape_diff_destroy. NULL on read failure.

  typedef void* LrvxTapeDiffHandle;

  typedef struct
  {
    int64_t exchange_ts_ns;
    int64_t price_raw;
    int64_t qty_raw;
    uint32_t symbol_id;
    uint8_t side;
  } LrvxTapeDiffTrade;

  typedef struct
  {
    uint64_t index;
    LrvxTapeDiffTrade left;
    LrvxTapeDiffTrade right;
  } LrvxTapeDiffMismatch;

  // max_mismatches=0 means "no cap"; field_tolerance_ns=0 means "exact".
  LRVX_EXPORT(group = "tape_diff")
  LrvxTapeDiffHandle lrvx_tape_diff_create(const char* left_path,
                                           const char* right_path,
                                           uint32_t max_mismatches,
                                           int64_t field_tolerance_ns);

  LRVX_EXPORT(group = "tape_diff")
  void lrvx_tape_diff_destroy(LrvxTapeDiffHandle handle);

  LRVX_EXPORT(group = "tape_diff")
  uint64_t lrvx_tape_diff_left_count(LrvxTapeDiffHandle handle);

  LRVX_EXPORT(group = "tape_diff")
  uint64_t lrvx_tape_diff_right_count(LrvxTapeDiffHandle handle);

  // Returns 1 and writes *out_index on present, 0 if there is no
  // divergence (tapes are equal).
  LRVX_EXPORT(group = "tape_diff")
  uint8_t lrvx_tape_diff_first_divergence(LrvxTapeDiffHandle handle,
                                          uint64_t* out_index);

  LRVX_EXPORT(group = "tape_diff")
  uint8_t lrvx_tape_diff_equal(LrvxTapeDiffHandle handle);

  LRVX_EXPORT(group = "tape_diff")
  uint64_t lrvx_tape_diff_mismatch_count(LrvxTapeDiffHandle handle);

  // Two-call size pattern: pass NULL out to learn the count, then
  // allocate and pass a buffer big enough for at least
  // lrvx_tape_diff_mismatch_count entries. Writes
  // min(count, max_entries) entries and returns the number written.
  LRVX_EXPORT(group = "tape_diff")
  uint64_t lrvx_tape_diff_copy_mismatches(LrvxTapeDiffHandle handle,
                                          LrvxTapeDiffMismatch* out,
                                          uint64_t max_entries);

  // ============================================================
  // Portfolio risk aggregator
  // ============================================================
  //
  // Cross-strategy daily PnL, gross / net exposure, kill switch on
  // drawdown / loss / gross / concentration limits. Single-process,
  // mutex-guarded; one handle per portfolio.

  typedef void* LrvxPortfolioRiskHandle;

  // Field-mask bits for lrvx_portfolio_risk_update.
  // Set ALL (0x3F) to overwrite every field; combine bits to update
  // only specific dimensions (e.g. gross+net only).
  // bit 0: realized_pnl, 1: unrealized_pnl, 2: fees,
  // bit 3: gross_exposure, 4: net_exposure, 5: trade_count.

  typedef struct
  {
    uint8_t has_max_drawdown_pct;
    double max_drawdown_pct;
    uint8_t has_max_daily_loss;
    double max_daily_loss;
    uint8_t has_max_gross_exposure;
    double max_gross_exposure;
    uint8_t has_max_concentration_pct;
    double max_concentration_pct;
  } LrvxPortfolioRiskRules;

  typedef struct
  {
    double realized_pnl;
    double unrealized_pnl;
    double fees;
    double gross_exposure;
    double net_exposure;
    uint64_t trade_count;
  } LrvxStrategyAccountFields;

  // A single breach.
  //
  // `rule` and `detail` point into storage the library keeps for this handle,
  // not into the caller's memory. They stay valid until the next call that
  // refills that storage for the same handle -- check_order, breach_count --
  // or until lrvx_portfolio_risk_destroy. A call on a different handle, or on
  // another thread, does not disturb them. Copy the strings out if they have
  // to live longer than that.
  typedef struct
  {
    const char* rule;
    double value;
    double limit;
    const char* detail;
  } LrvxBreach;

  LRVX_EXPORT(group = "portfolio_risk")
  LrvxPortfolioRiskHandle lrvx_portfolio_risk_create(
      const LrvxPortfolioRiskRules* rules, double initial_equity);

  LRVX_EXPORT(group = "portfolio_risk")
  void lrvx_portfolio_risk_destroy(LrvxPortfolioRiskHandle handle);

  LRVX_EXPORT(group = "portfolio_risk")
  void lrvx_portfolio_risk_update(LrvxPortfolioRiskHandle handle,
                                  const char* name,
                                  const LrvxStrategyAccountFields* fields,
                                  uint8_t field_mask);

  LRVX_EXPORT(group = "portfolio_risk")
  void lrvx_portfolio_risk_remove(LrvxPortfolioRiskHandle handle, const char* name);

  LRVX_EXPORT(group = "portfolio_risk")
  void lrvx_portfolio_risk_reset_kill_switch(LrvxPortfolioRiskHandle handle);

  // Returns 1 and writes *out_breach if the order is rejected.
  // Returns 0 and leaves out_breach untouched if the order is allowed.
  LRVX_EXPORT(group = "portfolio_risk")
  uint8_t lrvx_portfolio_risk_check_order(LrvxPortfolioRiskHandle handle,
                                          const char* strategy,
                                          double notional,
                                          const char* side,
                                          LrvxBreach* out_breach);

  // Headline snapshot fields. The aggregator owns the strings
  // referenced via lrvx_portfolio_risk_breach_at; they remain valid
  // until the next mutating call.
  LRVX_EXPORT(group = "portfolio_risk")
  double lrvx_portfolio_risk_total_daily_pnl(LrvxPortfolioRiskHandle handle);

  LRVX_EXPORT(group = "portfolio_risk")
  double lrvx_portfolio_risk_total_gross_exposure(LrvxPortfolioRiskHandle handle);

  LRVX_EXPORT(group = "portfolio_risk")
  double lrvx_portfolio_risk_current_equity(LrvxPortfolioRiskHandle handle);

  LRVX_EXPORT(group = "portfolio_risk")
  double lrvx_portfolio_risk_drawdown_pct(LrvxPortfolioRiskHandle handle);

  LRVX_EXPORT(group = "portfolio_risk")
  uint8_t lrvx_portfolio_risk_kill_switch_active(LrvxPortfolioRiskHandle handle);

  LRVX_EXPORT(group = "portfolio_risk")
  uint64_t lrvx_portfolio_risk_breach_count(LrvxPortfolioRiskHandle handle);

  // Returns 1 and writes *out on success, 0 if index is out of range.
  // The handle owns the string memory; it is invalidated by the next
  // mutating call.
  LRVX_EXPORT(group = "portfolio_risk")
  uint8_t lrvx_portfolio_risk_breach_at(LrvxPortfolioRiskHandle handle,
                                        uint64_t index,
                                        LrvxBreach* out);

  LRVX_EXPORT(group = "portfolio_risk")
  uint64_t lrvx_portfolio_risk_account_count(LrvxPortfolioRiskHandle handle);

  // ============================================================
  // Execution algorithms (TWAP / VWAP / Iceberg / POV)
  // ============================================================
  //
  // Each algo is a state machine. The caller drives it with `step`
  // and reads any newly emitted child orders via the `pending_*`
  // accessors, then dispatches them to its own executor and calls
  // `clear_pending` before the next step. Iceberg gates the next
  // slice on `report_fill`. POV uses `observe_volume` to track
  // market activity.

  typedef void* LrvxExecAlgoHandle;

  typedef struct
  {
    uint64_t order_id;
    int64_t timestamp_ns;
    double qty;
    double price;
    uint8_t type;  // 0 = market, 1 = limit
  } LrvxExecChildOrder;

  // side: 0 = buy, 1 = sell. type: 0 = market, 1 = limit.
  // limit_price is ignored for market orders.

  LRVX_EXPORT(group = "execution_algos")
  LrvxExecAlgoHandle lrvx_exec_twap_create(double target_qty, uint8_t side,
                                           uint32_t symbol, uint8_t type,
                                           double limit_price,
                                           int64_t duration_ns,
                                           uint32_t slice_count,
                                           int64_t start_time_ns);

  // volume_curve_ts and volume_curve_vol must be parallel arrays of
  // length n. ts in nanoseconds, vol must be non-negative; total
  // volume across the curve must be positive.
  LRVX_EXPORT(group = "execution_algos")
  LrvxExecAlgoHandle lrvx_exec_vwap_create(double target_qty, uint8_t side,
                                           uint32_t symbol, uint8_t type,
                                           double limit_price,
                                           const int64_t* volume_curve_ts,
                                           const double* volume_curve_vol,
                                           size_t n);

  LRVX_EXPORT(group = "execution_algos")
  LrvxExecAlgoHandle lrvx_exec_iceberg_create(double target_qty, uint8_t side,
                                              uint32_t symbol, uint8_t type,
                                              double limit_price,
                                              double visible_qty);

  LRVX_EXPORT(group = "execution_algos")
  LrvxExecAlgoHandle lrvx_exec_pov_create(double target_qty, uint8_t side,
                                          uint32_t symbol, uint8_t type,
                                          double limit_price,
                                          double participation_rate,
                                          double min_slice_qty);

  LRVX_EXPORT(group = "execution_algos")
  void lrvx_exec_destroy(LrvxExecAlgoHandle handle);

  LRVX_EXPORT(group = "execution_algos")
  void lrvx_exec_step(LrvxExecAlgoHandle handle, int64_t now_ns);

  LRVX_EXPORT(group = "execution_algos")
  void lrvx_exec_report_fill(LrvxExecAlgoHandle handle, double qty);

  LRVX_EXPORT(group = "execution_algos")
  void lrvx_exec_observe_volume(LrvxExecAlgoHandle handle, double qty);

  LRVX_EXPORT(group = "execution_algos")
  size_t lrvx_exec_pending_count(LrvxExecAlgoHandle handle);

  LRVX_EXPORT(group = "execution_algos")
  uint8_t lrvx_exec_pending_at(LrvxExecAlgoHandle handle, size_t index,
                               LrvxExecChildOrder* out);

  LRVX_EXPORT(group = "execution_algos")
  void lrvx_exec_clear_pending(LrvxExecAlgoHandle handle);

  LRVX_EXPORT(group = "execution_algos")
  double lrvx_exec_target_qty(LrvxExecAlgoHandle handle);

  LRVX_EXPORT(group = "execution_algos")
  double lrvx_exec_submitted_qty(LrvxExecAlgoHandle handle);

  LRVX_EXPORT(group = "execution_algos")
  double lrvx_exec_filled_qty(LrvxExecAlgoHandle handle);

  LRVX_EXPORT(group = "execution_algos")
  double lrvx_exec_remaining_qty(LrvxExecAlgoHandle handle);

  LRVX_EXPORT(group = "execution_algos")
  uint8_t lrvx_exec_is_done(LrvxExecAlgoHandle handle);

  // ============================================================
  // Delta book compression (tape format)
  // ============================================================
  //
  // Encodes a stream of L2 snapshots into anchor snapshots plus
  // deltas. The on-disk format already supports both event types
  // through BookRecordHeader.type (0 = snapshot, 1 = delta). This
  // surface is the state-keeping layer that decides what to emit
  // per call.
  //
  // Convention in a delta payload: a level with qty_raw == 0 means
  // "remove this price level"; a level with qty_raw > 0 means "set
  // this price level to this quantity".

  typedef void* LrvxDeltaBookEncoderHandle;
  typedef void* LrvxDeltaBookReplayerHandle;

  // anchor_every controls cadence: every N events the encoder emits
  // a full snapshot regardless of diff size, so a reader can seek
  // and replay forward. 0 means "always anchor" (snapshot-only,
  // i.e. the existing writer behaviour).
  LRVX_EXPORT(group = "delta_book")
  LrvxDeltaBookEncoderHandle lrvx_delta_book_encoder_create(uint32_t anchor_every);

  LRVX_EXPORT(group = "delta_book")
  void lrvx_delta_book_encoder_destroy(LrvxDeltaBookEncoderHandle handle);

  LRVX_EXPORT(group = "delta_book")
  void lrvx_delta_book_encoder_reset(LrvxDeltaBookEncoderHandle handle, uint32_t symbol_id);

  LRVX_EXPORT(group = "delta_book")
  void lrvx_delta_book_encoder_reset_all(LrvxDeltaBookEncoderHandle handle);

  // Feed a full snapshot. After the call:
  //   *out_is_delta -> 0 if the encoder emits an anchor snapshot,
  //                    1 if it emits a delta.
  //   *out_bid_count / *out_ask_count -> number of BookLevel entries
  //   the caller can then pull via lrvx_delta_book_encoder_copy_*.
  // Buffers are owned by the encoder and remain valid until the next
  // encode() on the same symbol.
  LRVX_EXPORT(group = "delta_book")
  void lrvx_delta_book_encoder_encode(LrvxDeltaBookEncoderHandle handle,
                                      uint32_t symbol_id,
                                      const LrvxBookLevel* bids, size_t bid_count,
                                      const LrvxBookLevel* asks, size_t ask_count,
                                      uint8_t* out_is_delta,
                                      uint64_t* out_bid_count,
                                      uint64_t* out_ask_count);

  // Copy the most recent encode() output into caller-allocated
  // buffers. Returns the number of entries written, capped at
  // max_entries.
  LRVX_EXPORT(group = "delta_book")
  uint64_t lrvx_delta_book_encoder_copy_bids(LrvxDeltaBookEncoderHandle handle,
                                             LrvxBookLevel* out, uint64_t max_entries);

  LRVX_EXPORT(group = "delta_book")
  uint64_t lrvx_delta_book_encoder_copy_asks(LrvxDeltaBookEncoderHandle handle,
                                             LrvxBookLevel* out, uint64_t max_entries);

  // Replayer: takes the events back and reconstructs full
  // snapshots per symbol.
  LRVX_EXPORT(group = "delta_book")
  LrvxDeltaBookReplayerHandle lrvx_delta_book_replayer_create(void);

  LRVX_EXPORT(group = "delta_book")
  void lrvx_delta_book_replayer_destroy(LrvxDeltaBookReplayerHandle handle);

  LRVX_EXPORT(group = "delta_book")
  void lrvx_delta_book_replayer_reset(LrvxDeltaBookReplayerHandle handle, uint32_t symbol_id);

  // Apply one event (type=0 snapshot, type=1 delta) and write the
  // resulting per-side counts into *out_bid_count / *out_ask_count.
  // Pull the levels via copy_bids / copy_asks.
  LRVX_EXPORT(group = "delta_book")
  void lrvx_delta_book_replayer_apply(LrvxDeltaBookReplayerHandle handle,
                                      uint8_t type, uint32_t symbol_id,
                                      const LrvxBookLevel* bids, size_t bid_count,
                                      const LrvxBookLevel* asks, size_t ask_count,
                                      uint64_t* out_bid_count,
                                      uint64_t* out_ask_count);

  LRVX_EXPORT(group = "delta_book")
  uint64_t lrvx_delta_book_replayer_copy_bids(LrvxDeltaBookReplayerHandle handle,
                                              LrvxBookLevel* out, uint64_t max_entries);

  LRVX_EXPORT(group = "delta_book")
  uint64_t lrvx_delta_book_replayer_copy_asks(LrvxDeltaBookReplayerHandle handle,
                                              LrvxBookLevel* out, uint64_t max_entries);

  // ============================================================
  // Strategy run trace (.lrvxrun)
  // ============================================================
  //
  // A `.lrvxrun` directory holds the events a strategy emitted during
  // a run: signals (decisions), order events (submit / cancel /
  // ack / reject), and fills. It sits alongside the `.lrvx`
  // tape(s) the run consumed and complements them — tape carries
  // exchange-side market data, this carries strategy-side trace.
  //
  // The recorder writes per-kind segment files (signals-NNN.bin,
  // orders-NNN.bin, fills-NNN.bin) plus manifest.json. The reader
  // opens an existing directory and exposes one read pass per
  // record kind. Variable-length payloads (signal name / payload,
  // order reject reason) come back via the standard two-call size
  // pattern: call once with NULL buffer to learn the size, then
  // again with a sized buffer to pull the bytes.

  typedef void* LrvxRunRecorderHandle;
  typedef void* LrvxRunReaderHandle;

  // Recorder. Opens (or creates) a directory at `path`. The
  // strategy_id, strategy_hash, and run_started_ns flow into
  // manifest.json on close.
  LRVX_EXPORT(group = "lrvxrun")
  LrvxRunRecorderHandle lrvx_run_recorder_create(const char* path,
                                                 const char* strategy_id,
                                                 const char* strategy_hash,
                                                 int64_t run_started_ns);

  LRVX_EXPORT(group = "lrvxrun")
  void lrvx_run_recorder_destroy(LrvxRunRecorderHandle handle);

  // Append one tape reference to the manifest. Call before close().
  LRVX_EXPORT(group = "lrvxrun")
  void lrvx_run_recorder_add_tape_ref(LrvxRunRecorderHandle handle,
                                      const char* path,
                                      const char* content_hash,
                                      int64_t first_event_ns,
                                      int64_t last_event_ns);

  LRVX_EXPORT(group = "lrvxrun")
  void lrvx_run_recorder_set_run_ended_ns(LrvxRunRecorderHandle handle, int64_t ns);

  LRVX_EXPORT(group = "lrvxrun")
  void lrvx_run_recorder_write_signal(LrvxRunRecorderHandle handle,
                                      int64_t run_ts_ns, int64_t feed_ts_ns,
                                      uint32_t signal_id, uint32_t flags,
                                      int64_t strength_raw,
                                      const char* name, size_t name_len,
                                      const uint32_t* symbol_ids, size_t symbol_count,
                                      const uint8_t* payload, size_t payload_len);

  LRVX_EXPORT(group = "lrvxrun")
  void lrvx_run_recorder_write_order_event(LrvxRunRecorderHandle handle,
                                           int64_t run_ts_ns, int64_t feed_ts_ns,
                                           uint64_t order_id, uint64_t parent_signal_id,
                                           int64_t price_raw, int64_t qty_raw,
                                           uint32_t symbol_id, uint8_t event_kind,
                                           uint8_t side, uint8_t order_type,
                                           uint32_t flags,
                                           const char* reason, size_t reason_len);

  LRVX_EXPORT(group = "lrvxrun")
  void lrvx_run_recorder_write_fill(LrvxRunRecorderHandle handle,
                                    int64_t run_ts_ns, int64_t feed_ts_ns,
                                    uint64_t order_id, uint64_t fill_id,
                                    int64_t price_raw, int64_t qty_raw, int64_t fee_raw,
                                    uint32_t symbol_id, uint8_t side, uint8_t liquidity);

  // Flush + finalize all open segments + manifest.json. Idempotent.
  LRVX_EXPORT(group = "lrvxrun")
  void lrvx_run_recorder_close(LrvxRunRecorderHandle handle);

  // ============================================================
  // Trace recorder auto-attach
  // ============================================================
  //
  // Attach a `LrvxRunRecorderHandle` to a `LrvxRunnerHandle` so every
  // signal the strategy emits gets captured into the .lrvxrun trace
  // without per-strategy instrumentation. Pass `recorder = NULL` to
  // detach. Order / fill auto-capture is a follow-up; today the
  // recorder captures signals only — order events still need to be
  // written by the user's executor / on_fill callback.

  LRVX_EXPORT(group = "trace_attach")
  void lrvx_runner_attach_trace_recorder(LrvxRunnerHandle runner,
                                         LrvxRunRecorderHandle recorder);
  LRVX_EXPORT(group = "trace_attach")
  void lrvx_runner_set_trace_feed_ts_ns(LrvxRunnerHandle runner, int64_t feed_ts_ns);

  // Mirror an order event into the attached recorder. No-op when no
  // recorder is attached. Wire from the user's executor wrapper —
  // call this after the wrapper's `on_submitted` / `on_canceled` /
  // `on_rejected` / etc. The runner stamps `run_ts_ns` with the
  // current wall-clock and uses the most recent `feed_ts_ns` from
  // `lrvx_runner_set_trace_feed_ts_ns`.
  // event_kind matches OrderEventKind in run_format_v1.h:
  //   1=Submit, 2=Cancel, 3=Modify, 4=Ack, 5=Reject, 6=Expire.
  LRVX_EXPORT(group = "trace_attach")
  void lrvx_runner_trace_order_event(LrvxRunnerHandle runner, uint64_t order_id,
                                     uint64_t parent_signal_id, uint32_t symbol_id,
                                     uint8_t event_kind, uint8_t side, uint8_t order_type,
                                     int64_t price_raw, int64_t qty_raw, uint32_t flags);

  // Mirror a fill into the attached recorder. liquidity: 0=Unknown,
  // 1=Maker, 2=Taker.
  LRVX_EXPORT(group = "trace_attach")
  void lrvx_runner_trace_fill(LrvxRunnerHandle runner, uint64_t order_id, uint64_t fill_id,
                              int64_t price_raw, int64_t qty_raw, int64_t fee_raw,
                              uint32_t symbol_id, uint8_t side, uint8_t liquidity);

  // Reader. Opens an existing `.lrvxrun` directory and parses
  // manifest.json.
  LRVX_EXPORT(group = "lrvxrun")
  LrvxRunReaderHandle lrvx_run_reader_open(const char* path);

  LRVX_EXPORT(group = "lrvxrun")
  void lrvx_run_reader_close(LrvxRunReaderHandle handle);

  // Manifest accessors (read-only views into the parsed manifest).
  LRVX_EXPORT(group = "lrvxrun")
  uint64_t lrvx_run_reader_strategy_id(LrvxRunReaderHandle handle, char* out, uint64_t max_bytes);

  LRVX_EXPORT(group = "lrvxrun")
  uint64_t lrvx_run_reader_strategy_hash(LrvxRunReaderHandle handle, char* out, uint64_t max_bytes);

  LRVX_EXPORT(group = "lrvxrun")
  int64_t lrvx_run_reader_run_started_ns(LrvxRunReaderHandle handle);

  LRVX_EXPORT(group = "lrvxrun")
  int64_t lrvx_run_reader_run_ended_ns(LrvxRunReaderHandle handle);

  LRVX_EXPORT(group = "lrvxrun")
  uint64_t lrvx_run_reader_tape_ref_count(LrvxRunReaderHandle handle);

  LRVX_EXPORT(group = "lrvxrun")
  uint64_t lrvx_run_reader_tape_ref_path(LrvxRunReaderHandle handle, uint64_t index, char* out, uint64_t max_bytes);

  // Counts. Returns 0 if the kind was never written.
  LRVX_EXPORT(group = "lrvxrun")
  uint64_t lrvx_run_reader_signal_count(LrvxRunReaderHandle handle);

  LRVX_EXPORT(group = "lrvxrun")
  uint64_t lrvx_run_reader_order_event_count(LrvxRunReaderHandle handle);

  LRVX_EXPORT(group = "lrvxrun")
  uint64_t lrvx_run_reader_fill_count(LrvxRunReaderHandle handle);

  // Per-record accessors. Index in [0, count). Variable-length
  // strings/payloads use the two-call size pattern: pass NULL/0 to
  // learn the size, then call again with a sized buffer.
  LRVX_EXPORT(group = "lrvxrun")
  void lrvx_run_reader_signal_header(LrvxRunReaderHandle handle, uint64_t index,
                                     int64_t* out_run_ts, int64_t* out_feed_ts,
                                     uint32_t* out_signal_id, uint32_t* out_flags,
                                     int64_t* out_strength_raw,
                                     uint64_t* out_name_len, uint64_t* out_symbol_count,
                                     uint64_t* out_payload_len);

  LRVX_EXPORT(group = "lrvxrun")
  uint64_t lrvx_run_reader_signal_name(LrvxRunReaderHandle handle, uint64_t index,
                                       char* out, uint64_t max_bytes);

  LRVX_EXPORT(group = "lrvxrun")
  uint64_t lrvx_run_reader_signal_symbol_ids(LrvxRunReaderHandle handle, uint64_t index,
                                             uint32_t* out, uint64_t max_entries);

  LRVX_EXPORT(group = "lrvxrun")
  uint64_t lrvx_run_reader_signal_payload(LrvxRunReaderHandle handle, uint64_t index,
                                          uint8_t* out, uint64_t max_bytes);

  LRVX_EXPORT(group = "lrvxrun")
  void lrvx_run_reader_order_event_header(LrvxRunReaderHandle handle, uint64_t index,
                                          int64_t* out_run_ts, int64_t* out_feed_ts,
                                          uint64_t* out_order_id, uint64_t* out_parent_signal_id,
                                          int64_t* out_price_raw, int64_t* out_qty_raw,
                                          uint32_t* out_symbol_id, uint8_t* out_event_kind,
                                          uint8_t* out_side, uint8_t* out_order_type,
                                          uint32_t* out_flags, uint64_t* out_reason_len);

  LRVX_EXPORT(group = "lrvxrun")
  uint64_t lrvx_run_reader_order_event_reason(LrvxRunReaderHandle handle, uint64_t index,
                                              char* out, uint64_t max_bytes);

  LRVX_EXPORT(group = "lrvxrun")
  void lrvx_run_reader_fill(LrvxRunReaderHandle handle, uint64_t index,
                            int64_t* out_run_ts, int64_t* out_feed_ts,
                            uint64_t* out_order_id, uint64_t* out_fill_id,
                            int64_t* out_price_raw, int64_t* out_qty_raw, int64_t* out_fee_raw,
                            uint32_t* out_symbol_id, uint8_t* out_side, uint8_t* out_liquidity);

  // Bar-close dispatch recorder.
  //
  // Cross-binding parity test fixture for the documented bar-close
  // ordering rule: on tied closes, MultiTimeframeAggregator dispatches
  // bars in the order their timeframes were registered. The recorder
  // wraps a BarBus + aggregator + recording subscriber so a binding
  // only needs to register timeframes, push trades, and read back the
  // dispatch sequence.

  typedef void* LrvxBarDispatchRecorderHandle;

  LRVX_EXPORT(group = "bar_dispatch")
  LrvxBarDispatchRecorderHandle lrvx_bar_dispatch_recorder_create(void);
  LRVX_EXPORT(group = "bar_dispatch")
  void lrvx_bar_dispatch_recorder_destroy(LrvxBarDispatchRecorderHandle h);

  // Returns slot index of the registered timeframe (or kMaxTimeframes on full).
  LRVX_EXPORT(group = "bar_dispatch")
  uint32_t lrvx_bar_dispatch_recorder_add_time_seconds(LrvxBarDispatchRecorderHandle h,
                                                       uint32_t seconds);

  LRVX_EXPORT(group = "bar_dispatch")
  void lrvx_bar_dispatch_recorder_on_trade(LrvxBarDispatchRecorderHandle h,
                                           uint32_t symbol, double price, double qty,
                                           int64_t ts_ns);

  // Drains the bus so all bars at the final tied close fire. Must be
  // called before reading count / param_at / type_at.
  LRVX_EXPORT(group = "bar_dispatch")
  void lrvx_bar_dispatch_recorder_finalize(LrvxBarDispatchRecorderHandle h);

  LRVX_EXPORT(group = "bar_dispatch")
  uint32_t lrvx_bar_dispatch_recorder_count(LrvxBarDispatchRecorderHandle h);
  LRVX_EXPORT(group = "bar_dispatch")
  uint8_t lrvx_bar_dispatch_recorder_type_at(LrvxBarDispatchRecorderHandle h, uint32_t index);
  LRVX_EXPORT(group = "bar_dispatch")
  uint64_t lrvx_bar_dispatch_recorder_param_at(LrvxBarDispatchRecorderHandle h, uint32_t index);

  // ============================================================
  // Streaming tape aggregators
  //
  // Single-pass dispatch over a captured `.lrvx` via a panel of
  // streaming aggregators. Five concrete aggregators (event-type
  // stats / time-bucketed count / volume-bin / top-N peak windows /
  // window-count quantiles) implement a common contract; the reader
  // walks the tape once and forwards every event to every attached
  // aggregator's `onEvent`, then calls `finalize` on each.
  //
  // Lifecycle: create an aggregator handle, pass an array of them to
  // `lrvx_data_reader_run` (or the merged equivalent), read the
  // result via the type-specific `read_result` family, destroy the
  // handle when done. All result accessors are call-twice: first to
  // get the row count, second with a caller-allocated buffer.
  // ============================================================

  typedef void* LrvxAggregatorHandle;

  // Event-type filter applied inside each aggregator's onEvent.
  // Matches `lrvx::replay::AggregatorEventFilter`: 1 = Trades only,
  // 2 = Books only, 3 = Both.
  typedef enum
  {
    LRVX_AGG_FILTER_TRADES = 1,
    LRVX_AGG_FILTER_BOOKS_ONLY = 2,
    LRVX_AGG_FILTER_BOTH = 3,
  } LrvxAggregatorEventFilter;

  // Side encoding shared across BinCount / VolumeBin result rows:
  // 0 = aggregate (no split), 1 = BUY, 2 = SELL.
  //
  // Constructors take `symbol_filter` as `(uint32_t*, uint32_t count)`
  // — pass NULL/0 to disable filtering (all symbols admitted).

  LRVX_EXPORT(group = "tape_aggregator")
  LrvxAggregatorHandle lrvx_event_type_stats_aggregator_create(
      LrvxAggregatorEventFilter event_filter, const uint32_t* symbol_filter,
      uint32_t symbol_filter_count);

  LRVX_EXPORT(group = "tape_aggregator")
  LrvxAggregatorHandle lrvx_bin_count_aggregator_create(
      int64_t bucket_ns, uint8_t by_side, uint8_t by_symbol,
      LrvxAggregatorEventFilter event_filter, const uint32_t* symbol_filter,
      uint32_t symbol_filter_count);

  LRVX_EXPORT(group = "tape_aggregator")
  LrvxAggregatorHandle lrvx_volume_bin_aggregator_create(
      int64_t bucket_ns, uint8_t by_side, uint8_t by_symbol,
      LrvxAggregatorEventFilter event_filter, const uint32_t* symbol_filter,
      uint32_t symbol_filter_count);

  // OHLCBinAggregator: per-bucket open/high/low/close of trade
  // price_raw. Trade-only; by_symbol controls per-symbol split (no
  // by_side because OHLC by side is not a generally useful primitive).
  LRVX_EXPORT(group = "tape_aggregator")
  LrvxAggregatorHandle lrvx_ohlc_bin_aggregator_create(
      int64_t bucket_ns, uint8_t by_symbol,
      LrvxAggregatorEventFilter event_filter, const uint32_t* symbol_filter,
      uint32_t symbol_filter_count);

  // PeakAggregator: `oversample_factor` 0 means "use the engine default"
  // (currently 100). Pass non-zero to override.
  LRVX_EXPORT(group = "tape_aggregator")
  LrvxAggregatorHandle lrvx_peak_aggregator_create(
      const int64_t* window_ns_list, uint32_t window_count, uint32_t top_n,
      uint32_t oversample_factor, LrvxAggregatorEventFilter event_filter,
      const uint32_t* symbol_filter, uint32_t symbol_filter_count);

  LRVX_EXPORT(group = "tape_aggregator")
  LrvxAggregatorHandle lrvx_quantile_aggregator_create(
      const int64_t* window_ns_list, uint32_t window_count,
      const double* quantiles, uint32_t quantile_count,
      LrvxAggregatorEventFilter event_filter, const uint32_t* symbol_filter,
      uint32_t symbol_filter_count);

  // BookSnapshotBinAggregator: time-bucketed order-book snapshots
  // reconstructed from book snapshot/delta events. At each bucket
  // boundary emits the latest ladder state observed inside the closed
  // bucket as up to `levels` rows per side (a row pairs the bid and
  // ask at the same depth; the shorter side is zero-padded). Book
  // reconstruction is order-dependent across the whole tape, so the
  // aggregator refuses parallel runs (cloneEmpty throws) — call
  // lrvx_data_reader_run with n_threads=1. `levels` must be > 0.
  LRVX_EXPORT(group = "tape_aggregator")
  LrvxAggregatorHandle lrvx_book_snapshot_bin_aggregator_create(
      int64_t bucket_ns, uint16_t levels,
      LrvxAggregatorEventFilter event_filter, const uint32_t* symbol_filter,
      uint32_t symbol_filter_count);

  // Universal destroy — works on any handle returned by the create
  // functions above. Safe to call with NULL.
  LRVX_EXPORT(group = "tape_aggregator")
  void lrvx_aggregator_destroy(LrvxAggregatorHandle h);

  // Single-pass dispatch. `aggregators` is an array of handles (any
  // mix of types); returns non-zero on success, zero on failure
  // (typically: empty data directory or read error). An empty array
  // is a no-op and returns success without decompressing anything.
  // n_threads=1 (or 0 → treated as 1) preserves single-threaded
  // semantics. n_threads>1 partitions the segment list across worker
  // threads; each worker clones the panel via cloneEmpty(), and the
  // reader merges worker panels into the caller's originals before
  // finalize. Effective worker count is clamped to segment count.
  LRVX_EXPORT(group = "tape_aggregator")
  uint8_t lrvx_data_reader_run(LrvxDataReaderHandle reader,
                               LrvxAggregatorHandle* aggregators,
                               uint32_t aggregator_count, uint32_t n_threads);

  // Progress callback for `lrvx_data_reader_run` (single-threaded
  // path only — n_threads=1). The callback fires at most once per
  // `interval_ms` from inside the run loop. `pct` is the fraction of
  // total events dispatched, in [0.0, 1.0]; `cursor_ts_ns` is the
  // exchange timestamp of the most recent dispatched event. The
  // callback returns non-zero to keep running, zero to request
  // cancellation — on cancel, the run finalizes aggregators with
  // whatever state has been accumulated and `lrvx_data_reader_run`
  // returns 0. `interval_ms = 0` is treated as 1000ms.
  typedef uint8_t (*LrvxProgressCallback)(void* user_data, double pct,
                                          int64_t cursor_ts_ns);

  LRVX_EXPORT(group = "data_reader")
  void lrvx_data_reader_set_progress_callback(
      LrvxDataReaderHandle reader, LrvxProgressCallback cb,
      void* user_data, uint32_t interval_ms);

  // Clear any callback previously installed via
  // `lrvx_data_reader_set_progress_callback`. Equivalent to passing
  // NULL as the callback function pointer.
  LRVX_EXPORT(group = "data_reader")
  void lrvx_data_reader_clear_progress_callback(LrvxDataReaderHandle reader);

  // MergedTapeReader::run is single-threaded for the moment — symbol
  // rekey is per-instance and a per-worker MergedTapeReader would not
  // share global symbol ids. The n_threads parameter is reserved for
  // future use; values > 1 are accepted but currently ignored.
  LRVX_EXPORT(group = "tape_aggregator")
  uint8_t lrvx_merged_tape_reader_run(LrvxMergedTapeReaderHandle reader,
                                      LrvxAggregatorHandle* aggregators,
                                      uint32_t aggregator_count,
                                      uint32_t n_threads);

  // Result row layouts. Mirror the C++ Row structs byte-for-byte;
  // bindings memcpy from the C ABI buffer into their native form.

  typedef struct
  {
    uint32_t symbol_id;
    uint64_t trades;
    uint64_t book_snapshots;
    uint64_t book_deltas;
  } LrvxEventTypeStatsRow;

  typedef struct
  {
    int64_t bucket_ts_ns;
    uint32_t symbol_id;
    uint8_t side;
    uint64_t count;
  } LrvxBinCountRow;

  typedef struct
  {
    int64_t bucket_ts_ns;
    uint32_t symbol_id;
    uint8_t side;
    int64_t qty_raw;
  } LrvxVolumeBinRow;

  typedef struct
  {
    int64_t bucket_ts_ns;
    uint32_t symbol_id;
    int64_t open_raw;
    int64_t high_raw;
    int64_t low_raw;
    int64_t close_raw;
  } LrvxOHLCBinRow;

  typedef struct
  {
    int64_t window_ns;
    uint64_t count;
    int64_t start_ns;
  } LrvxPeakRow;

  typedef struct
  {
    int64_t window_ns;
    double quantile;
    uint64_t count;
  } LrvxQuantileRow;

  // flags bit 0: best bid >= best ask (crossed book) at this bucket.
  // Zero price+qty on a side = no level at that depth.
  typedef struct
  {
    int64_t bucket_ts_ns;
    uint32_t symbol_id;
    uint16_t level;
    uint16_t flags;
    int64_t bid_price_raw;
    int64_t bid_qty_raw;
    int64_t ask_price_raw;
    int64_t ask_qty_raw;
  } LrvxBookSnapshotBinRow;

  // Result accessors. Each is two-call: first form returns the row
  // count (pass `rows_out=NULL`, `max_rows=0`); second form fills the
  // caller's buffer (returns the number of rows actually written,
  // capped at `max_rows`). The shape returned by each aggregator is
  // documented at its create-function site.
  LRVX_EXPORT(group = "tape_aggregator")
  uint32_t lrvx_event_type_stats_read_result(LrvxAggregatorHandle h,
                                             LrvxEventTypeStatsRow* rows_out,
                                             uint32_t max_rows);

  LRVX_EXPORT(group = "tape_aggregator")
  uint32_t lrvx_bin_count_read_result(LrvxAggregatorHandle h,
                                      LrvxBinCountRow* rows_out,
                                      uint32_t max_rows);

  LRVX_EXPORT(group = "tape_aggregator")
  uint32_t lrvx_volume_bin_read_result(LrvxAggregatorHandle h,
                                       LrvxVolumeBinRow* rows_out,
                                       uint32_t max_rows);

  LRVX_EXPORT(group = "tape_aggregator")
  uint32_t lrvx_ohlc_bin_read_result(LrvxAggregatorHandle h,
                                     LrvxOHLCBinRow* rows_out,
                                     uint32_t max_rows);

  LRVX_EXPORT(group = "tape_aggregator")
  uint32_t lrvx_peak_read_result(LrvxAggregatorHandle h,
                                 LrvxPeakRow* rows_out, uint32_t max_rows);

  LRVX_EXPORT(group = "tape_aggregator")
  uint32_t lrvx_quantile_read_result(LrvxAggregatorHandle h,
                                     LrvxQuantileRow* rows_out,
                                     uint32_t max_rows);

  LRVX_EXPORT(group = "tape_aggregator")
  uint32_t lrvx_book_snapshot_bin_read_result(LrvxAggregatorHandle h,
                                              LrvxBookSnapshotBinRow* rows_out,
                                              uint32_t max_rows);

  // ---- Tiered fee schedule ---------------------------------------
  //
  // Volume-tiered maker/taker fee ladder. Add tiers + record fills;
  // the schedule maintains a 30-day rolling notional and resolves
  // the active tier on lookup.

  typedef void* LrvxFeeScheduleHandle;

  LRVX_EXPORT(group = "fee_schedule")
  LrvxFeeScheduleHandle lrvx_fee_schedule_create(void);
  LRVX_EXPORT(group = "fee_schedule")
  void lrvx_fee_schedule_destroy(LrvxFeeScheduleHandle h);

  LRVX_EXPORT(group = "fee_schedule")
  void lrvx_fee_schedule_add_tier(LrvxFeeScheduleHandle h, double min_notional_30d,
                                  double maker_bps, double taker_bps);

  // Canned profile: "binance_um_futures" | "bybit_linear" | "okx_swap" |
  // "deribit". Returns 1 if the profile was applied, 0 if the name is
  // unknown -- a silently-ignored typo would otherwise produce a zero-fee
  // backtest that looks healthy.
  LRVX_EXPORT(group = "fee_schedule")
  int lrvx_fee_schedule_load_profile(LrvxFeeScheduleHandle h,
                                     const char* profile_name);

  LRVX_EXPORT(group = "fee_schedule")
  void lrvx_fee_schedule_record_fill(LrvxFeeScheduleHandle h, int64_t ts_ns,
                                     double notional);

  LRVX_EXPORT(group = "fee_schedule")
  double lrvx_fee_schedule_fee_for(LrvxFeeScheduleHandle h, int64_t ts_ns,
                                   double notional, uint8_t is_maker);

  LRVX_EXPORT(group = "fee_schedule")
  uint32_t lrvx_fee_schedule_current_tier(LrvxFeeScheduleHandle h);

  LRVX_EXPORT(group = "fee_schedule")
  double lrvx_fee_schedule_rolling_notional(LrvxFeeScheduleHandle h);

  LRVX_EXPORT(group = "fee_schedule")
  uint32_t lrvx_fee_schedule_tier_transitions(LrvxFeeScheduleHandle h,
                                              int64_t* out_buf, uint32_t max_events);

  LRVX_EXPORT(group = "fee_schedule")
  void lrvx_fee_schedule_reset_rolling(LrvxFeeScheduleHandle h);

  // ---- Funding schedule ------------------------------------------
  //
  // Models perpetual-futures funding payments. Build via constant
  // interval + rate or via a recorded tape, then call `tick` to walk
  // funding events between the last tick and now.

  typedef void* LrvxFundingScheduleHandle;

  LRVX_EXPORT(group = "funding_schedule")
  LrvxFundingScheduleHandle lrvx_funding_schedule_create(void);
  LRVX_EXPORT(group = "funding_schedule")
  void lrvx_funding_schedule_destroy(LrvxFundingScheduleHandle h);

  // Configure as constant-interval with the given fixed rate.
  LRVX_EXPORT(group = "funding_schedule")
  void lrvx_funding_schedule_set_constant(LrvxFundingScheduleHandle h, int64_t interval_ns,
                                          double rate);

  // Configure as a tape: timestamps_ns and rates arrays must have
  // length n_events.
  LRVX_EXPORT(group = "funding_schedule")
  void lrvx_funding_schedule_set_tape(LrvxFundingScheduleHandle h,
                                      const int64_t* timestamps_ns, const double* rates,
                                      uint32_t n_events);

  // Configure as a per-symbol tape. Each of the three arrays must
  // hold `n_entries` elements: timestamps_ns[i], symbols[i], rates[i]
  // describe row i. symbol == 0 acts as a wildcard (every symbol gets
  // this rate at that timestamp).
  LRVX_EXPORT(group = "funding_schedule")
  void lrvx_funding_schedule_set_tape_by_symbol(LrvxFundingScheduleHandle h,
                                                const int64_t* timestamps_ns,
                                                const uint32_t* symbols,
                                                const double* rates,
                                                uint32_t n_entries);

  // Read a CSV with columns timestamp_ns, symbol, funding_rate. Lines
  // starting with '#' and a leading alphabetic header row are
  // skipped. Returns 1 on success, 0 if the file could not be opened
  // (existing tape state preserved on failure).
  LRVX_EXPORT(group = "funding_schedule")
  uint8_t lrvx_funding_schedule_load_tape(LrvxFundingScheduleHandle h,
                                          const char* path);

  // Load a canned profile: "binance_um_futures" | "bybit_linear" |
  // "okx_swap" | "bitget_hourly". Rate defaults to zero; override
  // with set_constant_rate or use a tape.
  LRVX_EXPORT(group = "funding_schedule")
  int lrvx_funding_schedule_load_profile(LrvxFundingScheduleHandle h,
                                         const char* profile_name);

  LRVX_EXPORT(group = "funding_schedule")
  void lrvx_funding_schedule_set_constant_rate(LrvxFundingScheduleHandle h, double rate);

  LRVX_EXPORT(group = "funding_schedule")
  void lrvx_funding_schedule_reset(LrvxFundingScheduleHandle h);

  // Walk funding events in (last_tick, now_ns]. The caller supplies
  // per-symbol positions (signed, long positive) and mark prices.
  // Output is laid out as 6 doubles per event in `out_buf`:
  //   [0] timestamp_ns (as f64 — cast to int64 to recover)
  //   [1] symbol_id    (as f64)
  //   [2] rate
  //   [3] mark_price
  //   [4] position_signed
  //   [5] amount
  // First call with out_buf=NULL returns the event count.
  LRVX_EXPORT(group = "funding_schedule")
  uint32_t lrvx_funding_schedule_tick(LrvxFundingScheduleHandle h, int64_t now_ns,
                                      const uint32_t* symbols, const double* positions,
                                      const double* mark_prices, uint32_t n_symbols,
                                      double* out_buf, uint32_t max_events);

  // ---- Live queue position estimator -----------------------------
  //
  // Client-side estimator: feed it order placements, trades, book
  // updates and (our) fills, read back estimated queue-ahead per
  // resting order. The estimator wraps OrderQueueTracker; see
  // include/lrvx/execution/live_queue_position_estimator.h for the
  // heuristic and its limits.

  typedef void* LrvxLiveQueuePositionHandle;

  LRVX_EXPORT(group = "live_queue_position")
  LrvxLiveQueuePositionHandle lrvx_live_queue_position_create(void);
  LRVX_EXPORT(group = "live_queue_position")
  void lrvx_live_queue_position_destroy(LrvxLiveQueuePositionHandle h);

  // halfLifeNs: confidence decays as `exp(-elapsed * ln(2) / halfLife)`.
  // Zero disables time decay. Default ≈ 60s.
  LRVX_EXPORT(group = "live_queue_position")
  void lrvx_live_queue_position_set_confidence_half_life_ns(
      LrvxLiveQueuePositionHandle h, int64_t half_life_ns);

  // Per-shrink confidence multiplier (proportional-shrink heuristic).
  // Default 0.85.
  LRVX_EXPORT(group = "live_queue_position")
  void lrvx_live_queue_position_set_shrink_factor(LrvxLiveQueuePositionHandle h,
                                                  double factor);

  // side: 0 = BUY, 1 = SELL.
  LRVX_EXPORT(group = "live_queue_position")
  void lrvx_live_queue_position_on_order_placed(LrvxLiveQueuePositionHandle h,
                                                uint32_t symbol, uint8_t side,
                                                int64_t price_raw, uint64_t order_id,
                                                int64_t order_qty_raw,
                                                int64_t level_qty_raw, int64_t ts_ns);

  LRVX_EXPORT(group = "live_queue_position")
  void lrvx_live_queue_position_on_order_cancelled(LrvxLiveQueuePositionHandle h,
                                                   uint64_t order_id, int64_t ts_ns);

  LRVX_EXPORT(group = "live_queue_position")
  void lrvx_live_queue_position_on_order_filled(LrvxLiveQueuePositionHandle h,
                                                uint64_t order_id,
                                                int64_t cumulative_fill_raw,
                                                int64_t ts_ns);

  LRVX_EXPORT(group = "live_queue_position")
  void lrvx_live_queue_position_on_trade(LrvxLiveQueuePositionHandle h, uint32_t symbol,
                                         int64_t price_raw, int64_t qty_raw,
                                         int64_t ts_ns);

  // Trade with the venue's per-trade `is_hidden` flag. Behaviour
  // depends on the configured hidden-order policy.
  LRVX_EXPORT(group = "live_queue_position")
  void lrvx_live_queue_position_on_trade_with_flag(LrvxLiveQueuePositionHandle h,
                                                   uint32_t symbol, int64_t price_raw,
                                                   int64_t qty_raw, int64_t ts_ns,
                                                   uint8_t is_hidden);

  // Hidden-order attribution policy.
  // 0 = Ignore (default), 1 = TrustTradeFlag, 2 = InferIfTradeExceedsVisible.
  LRVX_EXPORT(group = "live_queue_position")
  void lrvx_live_queue_position_set_hidden_order_policy(LrvxLiveQueuePositionHandle h,
                                                        uint8_t policy);

  LRVX_EXPORT(group = "live_queue_position")
  void lrvx_live_queue_position_on_level_update(LrvxLiveQueuePositionHandle h,
                                                uint32_t symbol, uint8_t side,
                                                int64_t price_raw, int64_t new_qty_raw,
                                                int64_t ts_ns);

  // Snapshot out is laid out as 5 i64 + 1 f64 slots in a single int64
  // buffer (the f64 is bit-cast). The caller provides storage for 6
  // slots:
  //   [0] order_id
  //   [1] queue_ahead_raw
  //   [2] total_raw
  //   [3] last_update_ns
  //   [4] confidence as f64 bit-cast (use memcpy to read)
  //   [5] hidden_volume_seen_raw
  // Returns 1 if the order is tracked, 0 otherwise.
  LRVX_EXPORT(group = "live_queue_position")
  uint8_t lrvx_live_queue_position_snapshot(LrvxLiveQueuePositionHandle h,
                                            uint64_t order_id, int64_t now_ns,
                                            int64_t* out_slots);

  LRVX_EXPORT(group = "live_queue_position")
  uint32_t lrvx_live_queue_position_tracked_count(LrvxLiveQueuePositionHandle h);

  // ============================================================
  // Liquidation engine + insurance fund + ADL
  // ============================================================
  typedef void* LrvxLiquidationEngineHandle;

  LRVX_EXPORT(group = "liquidation_engine")
  LrvxLiquidationEngineHandle lrvx_liquidation_engine_create(void);
  LRVX_EXPORT(group = "liquidation_engine")
  void lrvx_liquidation_engine_destroy(LrvxLiquidationEngineHandle h);

  LRVX_EXPORT(group = "liquidation_engine")
  void lrvx_liquidation_engine_add_tier(LrvxLiquidationEngineHandle h,
                                        double min_notional, double mm_fraction);

  LRVX_EXPORT(group = "liquidation_engine")
  void lrvx_liquidation_engine_set_insurance_fund_capital(LrvxLiquidationEngineHandle h,
                                                          double capital);
  LRVX_EXPORT(group = "liquidation_engine")
  double lrvx_liquidation_engine_insurance_fund_balance(LrvxLiquidationEngineHandle h);
  LRVX_EXPORT(group = "liquidation_engine")
  void lrvx_liquidation_engine_set_adl_enabled(LrvxLiquidationEngineHandle h, uint8_t enabled);
  // ADL ranking strategy: 0=pnl_ratio, 1=binance, 2=bybit, 3=position_size.
  LRVX_EXPORT(group = "liquidation_engine")
  void lrvx_liquidation_engine_set_adl_ranking(LrvxLiquidationEngineHandle h, uint8_t ranking);
  LRVX_EXPORT(group = "liquidation_engine")
  uint8_t lrvx_liquidation_engine_adl_ranking(LrvxLiquidationEngineHandle h);
  LRVX_EXPORT(group = "liquidation_engine")
  void lrvx_liquidation_engine_set_liquidation_slippage_bps(LrvxLiquidationEngineHandle h,
                                                            double bps);

  // Side is encoded in signed `quantity` (+long, -short).
  LRVX_EXPORT(group = "liquidation_engine")
  void lrvx_liquidation_engine_open_position(LrvxLiquidationEngineHandle h,
                                             uint64_t account_id, uint32_t symbol,
                                             double quantity, double entry_price,
                                             double equity);
  LRVX_EXPORT(group = "liquidation_engine")
  void lrvx_liquidation_engine_close_position(LrvxLiquidationEngineHandle h,
                                              uint64_t account_id, uint32_t symbol);

  // Apply a mark; returns liquidations on this tick.
  LRVX_EXPORT(group = "liquidation_engine")
  uint32_t lrvx_liquidation_engine_on_mark(LrvxLiquidationEngineHandle h,
                                           uint32_t symbol, double mark_price);

  // Multi-symbol atomic mark update + walk. `symbols` and
  // `prices` are parallel arrays of length `n`. `ts_ns` is recorded
  // on each account's mark for the stale-mark guard. Returns
  // aggregated liquidation count across all symbols walked.
  LRVX_EXPORT(group = "liquidation_engine")
  uint32_t lrvx_liquidation_engine_on_marks(LrvxLiquidationEngineHandle h,
                                            uint32_t n, const uint32_t* symbols,
                                            const double* prices, int64_t ts_ns);

  LRVX_EXPORT(group = "liquidation_engine")
  uint64_t lrvx_liquidation_engine_liquidations_count(LrvxLiquidationEngineHandle h);
  LRVX_EXPORT(group = "liquidation_engine")
  uint64_t lrvx_liquidation_engine_insurance_payments_count(LrvxLiquidationEngineHandle h);
  LRVX_EXPORT(group = "liquidation_engine")
  uint64_t lrvx_liquidation_engine_adl_closeouts_count(LrvxLiquidationEngineHandle h);

  // Canned profiles: 0=binance_um_futures, 1=bybit_linear, 2=okx_swap.
  LRVX_EXPORT(group = "liquidation_engine")
  void lrvx_liquidation_engine_load_profile(LrvxLiquidationEngineHandle h, uint8_t profile);

  // Attach a SimulatedExecutor so liquidation orders route through
  // it as market orders. Pass NULL to detach.
  LRVX_EXPORT(group = "liquidation_engine")
  void lrvx_liquidation_engine_set_executor(LrvxLiquidationEngineHandle h,
                                            LrvxSimulatedExecutorHandle executor);

  // Distribution-level cascade statistics. Each *_size returns
  // the vector size; *_copy copies up to `max` entries to caller's
  // buffer and returns the number copied. Use *_size first to size
  // the allocation.
  LRVX_EXPORT(group = "liquidation_engine")
  uint32_t lrvx_liquidation_engine_deficits_paid_by_fund_size(LrvxLiquidationEngineHandle h);
  LRVX_EXPORT(group = "liquidation_engine")
  uint32_t lrvx_liquidation_engine_deficits_paid_by_fund_copy(LrvxLiquidationEngineHandle h,
                                                              double* out, uint32_t max);
  LRVX_EXPORT(group = "liquidation_engine")
  uint32_t lrvx_liquidation_engine_deficits_paid_by_adl_size(LrvxLiquidationEngineHandle h);
  LRVX_EXPORT(group = "liquidation_engine")
  uint32_t lrvx_liquidation_engine_deficits_paid_by_adl_copy(LrvxLiquidationEngineHandle h,
                                                             double* out, uint32_t max);
  LRVX_EXPORT(group = "liquidation_engine")
  uint32_t lrvx_liquidation_engine_cascade_sizes_size(LrvxLiquidationEngineHandle h);
  LRVX_EXPORT(group = "liquidation_engine")
  uint32_t lrvx_liquidation_engine_cascade_sizes_copy(LrvxLiquidationEngineHandle h,
                                                      uint32_t* out, uint32_t max);
  LRVX_EXPORT(group = "liquidation_engine")
  uint32_t lrvx_liquidation_engine_fund_balance_history_size(LrvxLiquidationEngineHandle h);
  LRVX_EXPORT(group = "liquidation_engine")
  uint32_t lrvx_liquidation_engine_fund_balance_history_copy(LrvxLiquidationEngineHandle h,
                                                             double* out, uint32_t max);

  // Ordinal of the onMark call that fired the first ADL closeout.
  // UINT64_MAX if no ADL has fired.
  LRVX_EXPORT(group = "liquidation_engine")
  uint64_t lrvx_liquidation_engine_ticks_to_first_adl(LrvxLiquidationEngineHandle h);

  LRVX_EXPORT(group = "liquidation_engine")
  void lrvx_liquidation_engine_reset_stats(LrvxLiquidationEngineHandle h);

  // Mark-impact feedback. After liquidation fills, the engine
  // recomputes the mark from the post-cascade book and may trigger
  // additional liquidations within the same on_mark call.
  //   model: 0=none (default), 1=book_anchored, 2=book_only.
  //   weight: blend factor in [0,1] used by book_anchored.
  LRVX_EXPORT(group = "liquidation_engine")
  void lrvx_liquidation_engine_set_mark_impact_model(LrvxLiquidationEngineHandle h,
                                                     uint8_t model, double weight);
  LRVX_EXPORT(group = "liquidation_engine")
  uint8_t lrvx_liquidation_engine_mark_impact_model(LrvxLiquidationEngineHandle h);
  LRVX_EXPORT(group = "liquidation_engine")
  double lrvx_liquidation_engine_mark_impact_weight(LrvxLiquidationEngineHandle h);
  // Bound on second-order cascade depth within a single on_mark call.
  // Set to 0 to disable cascading.
  LRVX_EXPORT(group = "liquidation_engine")
  void lrvx_liquidation_engine_set_max_cascade_depth(LrvxLiquidationEngineHandle h,
                                                     uint32_t depth);
  LRVX_EXPORT(group = "liquidation_engine")
  uint32_t lrvx_liquidation_engine_max_cascade_depth(LrvxLiquidationEngineHandle h);

  // ===== Account — cross-margin shared state =====
  typedef void* LrvxAccountHandle;

  LRVX_EXPORT(group = "account")
  LrvxAccountHandle lrvx_account_create(uint64_t account_id, double equity);
  LRVX_EXPORT(group = "account")
  void lrvx_account_destroy(LrvxAccountHandle h);

  LRVX_EXPORT(group = "account")
  uint64_t lrvx_account_id(LrvxAccountHandle h);
  LRVX_EXPORT(group = "account")
  double lrvx_account_equity(LrvxAccountHandle h);
  LRVX_EXPORT(group = "account")
  void lrvx_account_set_equity(LrvxAccountHandle h, double equity);
  LRVX_EXPORT(group = "account")
  void lrvx_account_add_equity(LrvxAccountHandle h, double delta);

  // Margin mode: 0=cross (default), 1=isolated.
  LRVX_EXPORT(group = "account")
  uint8_t lrvx_account_margin_mode(LrvxAccountHandle h);
  LRVX_EXPORT(group = "account")
  void lrvx_account_set_margin_mode(LrvxAccountHandle h, uint8_t mode);

  // Side encoded in signed `quantity`.
  LRVX_EXPORT(group = "account")
  void lrvx_account_open_position(LrvxAccountHandle h, uint32_t symbol,
                                  double quantity, double entry_price);
  // Open a position with an isolated-equity slice. Required
  // when the account runs in MarginMode::Isolated; the slice backs
  // the position in the per-position MM check. Cross-mode accounts
  // ignore the slice.
  LRVX_EXPORT(group = "account")
  void lrvx_account_open_position_isolated(LrvxAccountHandle h, uint32_t symbol,
                                           double quantity, double entry_price,
                                           double isolated_equity);
  LRVX_EXPORT(group = "account")
  void lrvx_account_close_position(LrvxAccountHandle h, uint32_t symbol);
  LRVX_EXPORT(group = "account")
  uint32_t lrvx_account_position_count(LrvxAccountHandle h);

  LRVX_EXPORT(group = "account")
  void lrvx_account_set_mark(LrvxAccountHandle h, uint32_t symbol, double price);
  // set_mark with timestamp for stale-mark guard.
  LRVX_EXPORT(group = "account")
  void lrvx_account_set_mark_at(LrvxAccountHandle h, uint32_t symbol,
                                double price, int64_t ts_ns);
  LRVX_EXPORT(group = "account")
  int64_t lrvx_account_mark_ts(LrvxAccountHandle h, uint32_t symbol);
  // Returns 1 when any position in the account has a mark older
  // than `budget_ns` relative to `now_ns`, or no mark at all.
  LRVX_EXPORT(group = "account")
  uint8_t lrvx_account_has_stale_marks(LrvxAccountHandle h, int64_t now_ns,
                                       int64_t budget_ns);
  LRVX_EXPORT(group = "account")
  double lrvx_account_total_notional(LrvxAccountHandle h);
  LRVX_EXPORT(group = "account")
  double lrvx_account_total_unrealised_pnl(LrvxAccountHandle h);

  LRVX_EXPORT(group = "account")
  void lrvx_account_record_fill(LrvxAccountHandle h, int64_t ts_ns, double notional);
  // Per-symbol fill recording. Same as record_fill but tags
  // the fill with a symbol so rollingNotionalBySymbol30d can break
  // the aggregate down.
  LRVX_EXPORT(group = "account")
  void lrvx_account_record_fill_ex(LrvxAccountHandle h, int64_t ts_ns,
                                   double notional, uint32_t symbol);
  // Returns the number of unique symbols in the rolling window.
  // Use rolling_notional_by_symbol_copy to read them.
  LRVX_EXPORT(group = "account")
  uint32_t lrvx_account_rolling_notional_by_symbol_size(LrvxAccountHandle h);
  // Copies up to `max` (symbol, notional) pairs into the caller's
  // buffers. Returns the number actually copied.
  LRVX_EXPORT(group = "account")
  uint32_t lrvx_account_rolling_notional_by_symbol_copy(
      LrvxAccountHandle h, uint32_t* symbols_out, double* notionals_out,
      uint32_t max);
  LRVX_EXPORT(group = "account")
  double lrvx_account_rolling_notional_30d(LrvxAccountHandle h);
  LRVX_EXPORT(group = "account")
  void lrvx_account_reset_rolling(LrvxAccountHandle h);

  // Attach/detach to a LiquidationEngine (engine-level walk participates
  // in cross-margin MM checks). Multiple accounts may be attached.
  LRVX_EXPORT(group = "liquidation_engine")
  void lrvx_liquidation_engine_attach_account(LrvxLiquidationEngineHandle h,
                                              LrvxAccountHandle account);
  LRVX_EXPORT(group = "liquidation_engine")
  void lrvx_liquidation_engine_detach_account(LrvxLiquidationEngineHandle h,
                                              uint64_t account_id);

  // Bind/unbind to a FeeSchedule so 30d rolling notional reads from the
  // account's aggregate counter instead of the schedule's internal one.
  LRVX_EXPORT(group = "fee_schedule")
  void lrvx_fee_schedule_bind_account(LrvxFeeScheduleHandle h,
                                      LrvxAccountHandle account);
  LRVX_EXPORT(group = "fee_schedule")
  void lrvx_fee_schedule_clear_account_binding(LrvxFeeScheduleHandle h);

  // ===== VenueStack — single-call venue-realistic backtest =====
  // Each factory returns a fully-wired stack containing the
  // executor, account, liquidation engine, fee schedule, funding
  // schedule, rate-limit policy, and venue-availability instance.
  // Use the *_subhandle accessors to obtain non-owning handles to
  // each subsystem; destroying the venue stack invalidates them.
  typedef void* LrvxVenueStackHandle;

  // Venue codes: 0=binance_um_futures (default), 1=bybit_linear,
  // 2=okx_swap, 3=deribit.
  LRVX_EXPORT(group = "venue_stack")
  LrvxVenueStackHandle lrvx_venue_stack_create(uint8_t venue, uint64_t account_id,
                                               double equity);
  LRVX_EXPORT(group = "venue_stack")
  void lrvx_venue_stack_destroy(LrvxVenueStackHandle h);

  // Non-owning sub-handle accessors.
  LRVX_EXPORT(group = "venue_stack")
  LrvxSimulatedExecutorHandle lrvx_venue_stack_executor(LrvxVenueStackHandle h);
  LRVX_EXPORT(group = "venue_stack")
  LrvxAccountHandle lrvx_venue_stack_account(LrvxVenueStackHandle h);
  LRVX_EXPORT(group = "venue_stack")
  LrvxLiquidationEngineHandle lrvx_venue_stack_liquidation(LrvxVenueStackHandle h);
  LRVX_EXPORT(group = "venue_stack")
  LrvxFeeScheduleHandle lrvx_venue_stack_fees(LrvxVenueStackHandle h);
  LRVX_EXPORT(group = "venue_stack")
  LrvxFundingScheduleHandle lrvx_venue_stack_funding(LrvxVenueStackHandle h);
  LRVX_EXPORT(group = "venue_stack")
  LrvxVenueAvailabilityHandle lrvx_venue_stack_venue(LrvxVenueStackHandle h);

  // Venue name, NUL-terminated. Caller must not free; the pointer
  // is valid until destroy. Returns empty string for unknown venue.
  LRVX_EXPORT(group = "venue_stack")
  const char* lrvx_venue_stack_venue_name(LrvxVenueStackHandle h);

  // ============================================================
  // DEX amounts: u256 / i256 at the C boundary
  // ============================================================
  //
  // The AMM/DEX curves compute in exact 256-bit integers -- u256, and i256 for
  // signed values such as a tick's net liquidity -- in native token units (wei /
  // lamports). At the C-ABI a 256-bit amount crosses as a NUL-terminated decimal
  // string: language-agnostic and lossless, mapping to a native bignum in each
  // binding (Python int, Node BigInt). A struct-of-limbs form would be zero-copy but
  // a backtest is not a hot loop, so the string form is the chosen representation;
  // the limb struct stays the option if profiling ever demands it.
  //
  // These are the canonical parse/format of that representation, the round-trip a
  // binding uses to cross the boundary: it formats its native integer as a decimal
  // string, and these validate and re-canonicalise it. lrvx_u256_from_hex accepts
  // chain data that arrives in hex (with or without a 0x prefix). All write a
  // NUL-terminated decimal string into out and return 1 on success, 0 on a parse
  // error or a too-small buffer (a u256 needs up to 78 digits + sign + NUL).

  LRVX_EXPORT(group = "dex_amount")
  uint8_t lrvx_u256_roundtrip(const char* dec, char* out, size_t out_len);
  LRVX_EXPORT(group = "dex_amount")
  uint8_t lrvx_i256_roundtrip(const char* dec, char* out, size_t out_len);
  LRVX_EXPORT(group = "dex_amount")
  uint8_t lrvx_u256_from_hex(const char* hex, char* out, size_t out_len);

  // The limb form (little-endian uint64[4]), for a binding whose native bignum is
  // limb-addressable (a Node BigInt, via ToWords / FromWords). lrvx_u256_to_words
  // parses a decimal string into 4 little-endian limbs; lrvx_u256_from_words formats
  // 4 limbs as decimal. u256 only -- a signed value carries its sign in the binding.
  // `words` points to exactly 4 uint64_t. Return 1 on success, 0 on a parse error or
  // a too-small buffer.
  LRVX_EXPORT(group = "dex_amount")
  uint8_t lrvx_u256_to_words(const char* dec, uint64_t* words);
  LRVX_EXPORT(group = "dex_amount")
  uint8_t lrvx_u256_from_words(const uint64_t* words, char* out, size_t out_len);

  // ============================================================
  // AMM curves: price a DEX swap from any binding
  // ============================================================
  //
  // An exact AMM pool curve (the INTokenCurve), as an opaque handle. A constructor
  // takes the pool's parameters (amounts as decimal strings, the dex_amount boundary)
  // and returns a handle; the generic ops price and move it. The handle owns the
  // curve; lrvx_curve_destroy frees it. A constructor returns NULL on a bad parameter
  // (e.g. a non-numeric amount).
  //
  // Token indices i / j are [0, token_count). lrvx_curve_amount_out prices a swap
  // without moving the pool; lrvx_curve_apply_swap moves it and returns the output;
  // lrvx_curve_balance reads a reserve. The amount-out / balance writers fill a
  // decimal string and return 1, or 0 on a too-small buffer (96 bytes is always
  // enough for a u256).
  //
  // This first set is constant-product (Uniswap v2 forks), Raydium CP (Solana), and
  // Uniswap v3 concentrated liquidity; the other venues extend the same shape.

  LRVX_EXPORT(group = "amm_curve")
  LrvxCurveHandle lrvx_curve_constant_product(const char* reserve0, const char* reserve1,
                                              uint64_t fee_num, uint64_t fee_den);
  LRVX_EXPORT(group = "amm_curve")
  LrvxCurveHandle lrvx_curve_raydium_cp(const char* reserve0, const char* reserve1,
                                        uint64_t trade_fee_rate, uint64_t creator_fee_rate,
                                        uint8_t creator_fee_on_input);
  // Uniswap v3 (Q64.96). ticks: n_ticks initialized ticks, as parallel arrays of the
  // sqrt-ratio (u256 decimal) and the net liquidity (i256 decimal, signed). Pass
  // n_ticks = 0 for an in-range swap that crosses no tick.
  LRVX_EXPORT(group = "amm_curve")
  LrvxCurveHandle lrvx_curve_uniswap_v3(const char* sqrt_price_x96, const char* liquidity,
                                        uint32_t fee_pips, const char* const* tick_sqrt_ratio,
                                        const char* const* tick_liquidity_net, size_t n_ticks);

  LRVX_EXPORT(group = "amm_curve")
  size_t lrvx_curve_token_count(LrvxCurveHandle curve);
  LRVX_EXPORT(group = "amm_curve")
  uint8_t lrvx_curve_amount_out(LrvxCurveHandle curve, size_t i, size_t j, const char* amount_in,
                                char* out, size_t out_len);
  LRVX_EXPORT(group = "amm_curve")
  uint8_t lrvx_curve_apply_swap(LrvxCurveHandle curve, size_t i, size_t j, const char* amount_in,
                                char* out, size_t out_len);
  LRVX_EXPORT(group = "amm_curve")
  uint8_t lrvx_curve_balance(LrvxCurveHandle curve, size_t i, char* out, size_t out_len);
  // Concentrated-liquidity state read-back. For a CLMM pool (Uniswap v3, Orca
  // Whirlpool, Raydium CLMM) writes the current sqrtPriceX96 / active liquidity as a
  // decimal string and returns 1. For any other venue writes "0" and returns 0, so a
  // caller can tell "not a CLMM pool" from a genuine zero.
  LRVX_EXPORT(group = "amm_curve")
  uint8_t lrvx_curve_sqrt_price(LrvxCurveHandle curve, char* out, size_t out_len);
  LRVX_EXPORT(group = "amm_curve")
  uint8_t lrvx_curve_liquidity(LrvxCurveHandle curve, char* out, size_t out_len);
  LRVX_EXPORT(group = "amm_curve")
  LrvxCurveHandle lrvx_curve_clone(LrvxCurveHandle curve);
  LRVX_EXPORT(group = "amm_curve")
  void lrvx_curve_destroy(LrvxCurveHandle curve);

  // ============================================================
  // Pool-state tape: replay a recorded DEX pool history
  // ============================================================
  //
  // A pool-state tape is a delta log -- a Descriptor (the venue), Checkpoints (full
  // state), and the SwapDeltas that move it -- and a pool's state is derived by
  // replaying the deltas through the exact curve. Build a tape with the writer calls,
  // then replay it: the replay rebuilds the curve from each Checkpoint, applies the
  // swaps, and counts drift (a Checkpoint that disagrees with the replayed state).
  // This lets a binding backtest a DEX strategy on recorded pool data.
  //
  // Build order: a descriptor (once), then alternating Checkpoints and SwapDeltas in
  // timestamp order. Two-reserve venues (constant-product, Raydium CP) use
  // lrvx_pool_tape_checkpoint; a concentrated-liquidity venue uses
  // lrvx_pool_tape_checkpoint_clmm. The writers return 0 on a bad amount string.

  LRVX_EXPORT(group = "pool_tape")
  LrvxPoolTapeHandle lrvx_pool_tape_create(void);
  LRVX_EXPORT(group = "pool_tape")
  void lrvx_pool_tape_descriptor_constant_product(LrvxPoolTapeHandle tape, uint64_t fee_num,
                                                  uint64_t fee_den, uint8_t base_dec,
                                                  uint8_t quote_dec);
  LRVX_EXPORT(group = "pool_tape")
  void lrvx_pool_tape_descriptor_raydium_cp(LrvxPoolTapeHandle tape, uint64_t trade_fee_rate,
                                            uint64_t creator_fee_rate, uint8_t creator_fee_on_input,
                                            uint8_t base_dec, uint8_t quote_dec);
  // venue: 2 = Uniswap v3, 3 = Orca Whirlpool, 4 = Raydium CLMM.
  LRVX_EXPORT(group = "pool_tape")
  void lrvx_pool_tape_descriptor_clmm(LrvxPoolTapeHandle tape, uint8_t venue, uint32_t fee_pips,
                                      uint8_t base_dec, uint8_t quote_dec);
  LRVX_EXPORT(group = "pool_tape")
  uint8_t lrvx_pool_tape_checkpoint(LrvxPoolTapeHandle tape, int64_t ts_ns, const char* reserve0,
                                    const char* reserve1);
  LRVX_EXPORT(group = "pool_tape")
  uint8_t lrvx_pool_tape_checkpoint_clmm(LrvxPoolTapeHandle tape, int64_t ts_ns,
                                         const char* sqrt_price, const char* liquidity,
                                         const char* const* tick_sqrt_ratio,
                                         const char* const* tick_liquidity_net, size_t n_ticks);
  LRVX_EXPORT(group = "pool_tape")
  uint8_t lrvx_pool_tape_swap(LrvxPoolTapeHandle tape, int64_t ts_ns, uint8_t base_for_quote,
                              const char* amount_in);
  LRVX_EXPORT(group = "pool_tape")
  void lrvx_pool_tape_destroy(LrvxPoolTapeHandle tape);

  // Replay the tape, deriving the pool state through the exact curve. base_idx /
  // quote_idx and the decimals are the pair the synthetic book is presented as.
  LRVX_EXPORT(group = "pool_tape")
  LrvxPoolReplayHandle lrvx_pool_tape_replay(LrvxPoolTapeHandle tape, size_t base_idx,
                                             size_t quote_idx, uint8_t base_dec, uint8_t quote_dec);
  LRVX_EXPORT(group = "pool_tape")
  size_t lrvx_pool_replay_drift_count(LrvxPoolReplayHandle replay);
  LRVX_EXPORT(group = "pool_tape")
  size_t lrvx_pool_replay_trade_count(LrvxPoolReplayHandle replay);
  // The final curve, borrowed -- valid until lrvx_pool_replay_destroy; do not pass it
  // to lrvx_curve_destroy. NULL if the tape had no checkpoint.
  LRVX_EXPORT(group = "pool_tape")
  LrvxCurveHandle lrvx_pool_replay_curve(LrvxPoolReplayHandle replay);
  LRVX_EXPORT(group = "pool_tape")
  void lrvx_pool_replay_destroy(LrvxPoolReplayHandle replay);

  // ============================================================
  // Diagnostics
  // ============================================================

  // ABI version compiled into the library. Compare against the
  // LRVX_CAPI_ABI_VERSION macro from the header you built with and refuse the
  // mismatch: the structs on this boundary have no reserved tail, so a skew
  // shows up as wrong numbers rather than a failed load.
  LRVX_EXPORT(group = "capi_diagnostics")
  uint32_t lrvx_capi_abi_version(void);

  // The last failure on the calling thread, set by a call that returned its
  // failure value. 0 = no error recorded, 1 = a NULL handle was passed,
  // 2 = an exception was stopped at the boundary. Only meaningful straight
  // after a call that reported failure; a successful call does not clear it.
  LRVX_EXPORT(group = "capi_diagnostics")
  int lrvx_last_error_code(void);

  // Text for the code above: the function that failed, and what the exception
  // said. Never NULL -- an empty string means nothing has been recorded on
  // this thread. Valid until the next failure on this thread or the next
  // lrvx_clear_last_error.
  LRVX_EXPORT(group = "capi_diagnostics")
  const char* lrvx_last_error_message(void);

  LRVX_EXPORT(group = "capi_diagnostics")
  void lrvx_clear_last_error(void);

#ifdef __cplusplus
}
#endif
