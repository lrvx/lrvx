/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * GENERATED — do not edit by hand.
 * Source: include/lrvx/capi/lrvx_capi_spec.hpp
 * Tool:   tools/codegen/lrvx_codegen/emit_capi.py
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

/* ABI version of this header and of the library that ships with it.
 * Bumped whenever a struct on this boundary changes shape, a function
 * changes signature, or an existing code space gains a meaning. The
 * structs here are solid, with no reserved tail, so a header from one
 * version used against a library from another produces wrong numbers
 * rather than a failed load -- compare LRVX_CAPI_ABI_VERSION against
 * lrvx_capi_abi_version() once at startup and refuse the mismatch.
 * 3: LrvxBar grew close_reason. 4: LrvxBookSnapshot grew has_bid and
 * has_ask. 5: LrvxSymbolContext grew last_touch_ns. */
#define LRVX_CAPI_ABI_VERSION 5

/* Declared by a header that carries lrvx_best_bid_raw_opt and its two
 * siblings, so a binding can compile against a header with or without them.
 * Adding a function does not move LRVX_CAPI_ABI_VERSION: the version marks a
 * struct whose shape changed, a function whose signature changed, or a code
 * space that gained a meaning, and an addition is none of those. */
#define LRVX_HAS_OPTIONAL_RAW_BEST_QUOTE 1

/* ============================================================
 * Calling contract
 * ============================================================
 *
 * Handles. Every lrvx_*Handle is an opaque pointer owned by whoever
 * created it. Passing NULL where a handle is expected is safe on every
 * function on this page: the call does nothing and returns the value it
 * documents for failure -- 0 for the integer and floating-point returns,
 * NULL for handles and strings, an all-zero struct for the by-value
 * struct returns. It is not an error to destroy NULL either. Arguments
 * that are not handles -- strings, output buffers, arrays -- are checked
 * where the function says so and are otherwise required to be valid.
 *
 * Ownership. A function whose name ends in _create returns a handle the
 * caller owns and must pass to the matching _destroy. Accessors that
 * reach inside a composite -- lrvx_venue_stack_account and its siblings
 * -- return a BORROWED handle: it stays valid while the composite lives,
 * it must not be destroyed, and calling _destroy on it does nothing.
 * The name alone does not settle it: a handle-returning function whose
 * name does not end in _create says owned or borrowed in a comment on
 * its own declaration, and every such function carries one.
 *
 * Exceptions. None escape. The engine underneath is C++ and throws;
 * unwinding out of a frame with C linkage is undefined behaviour, so
 * every function on this page catches. A call that was stopped this way
 * returns its failure value and leaves a description behind:
 * lrvx_last_error_code() and lrvx_last_error_message() report it for the
 * calling thread.
 *
 * Threads. Handles are not synchronised. Two threads may use two
 * different handles freely; sharing one handle between threads is the
 * caller's problem to serialise. Results that a function stores for a
 * follow-up call -- the delta-book encoder's level lists, the portfolio
 * risk breach list -- belong to the handle, not to the calling thread,
 * so the pair may be split across threads.
 */

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
  typedef void* LrvxCurveHandle;
  typedef void* LrvxPoolTapeHandle;
  typedef void* LrvxPoolReplayHandle;
  typedef void* LrvxIndicatorGraphHandle;
  typedef LrvxIndicatorGraphHandle LrvxStreamingGraphHandle;
  typedef void* LrvxOrderGroupHandle;
  typedef void* LrvxFeedClockHandle;
  typedef void* LrvxL3BookHandle;
  typedef void* LrvxDataWriterHandle;
  typedef void* LrvxDataReaderHandle;
  typedef void* LrvxLatencyDistributionHandle;
  typedef void* LrvxRateLimitPolicyHandle;
  typedef void* LrvxVenueAvailabilityHandle;
  typedef void* LrvxBacktestResultHandle;
  typedef void* LrvxMergedTapeReaderHandle;
  typedef void* LrvxPartitionerHandle;
  typedef void* LrvxRiskManagerHandle;
  typedef void* LrvxKillSwitchHandle;
  typedef void* LrvxOrderValidatorHandle;
  typedef void* LrvxPnLTrackerHandle;
  typedef void* LrvxStorageSinkHandle;
  typedef void* LrvxMarketDataRecorderHandle;
  typedef void* LrvxBinaryLogRecorderHookHandle;
  typedef void* LrvxReplaySourceHandle;
  typedef void* LrvxExecutionListenerHandle;
  typedef void* LrvxExecutorHandle;
  typedef void* LrvxLiveEngineHandle;
  typedef void* LrvxRunnerHandle;
  typedef void* LrvxBacktestRunnerHandle;
  typedef void* LrvxGridSearchHandle;
  typedef void* LrvxLatencyModelHandle;
  typedef void* LrvxTapeDiffHandle;
  typedef void* LrvxPortfolioRiskHandle;
  typedef void* LrvxExecAlgoHandle;
  typedef void* LrvxDeltaBookEncoderHandle;
  typedef void* LrvxDeltaBookReplayerHandle;
  typedef void* LrvxRunRecorderHandle;
  typedef void* LrvxRunReaderHandle;
  typedef void* LrvxBarDispatchRecorderHandle;
  typedef void* LrvxAggregatorHandle;
  typedef void* LrvxFeeScheduleHandle;
  typedef void* LrvxFundingScheduleHandle;
  typedef void* LrvxLiveQueuePositionHandle;
  typedef void* LrvxLiquidationEngineHandle;
  typedef void* LrvxAccountHandle;
  typedef void* LrvxVenueStackHandle;
  // ============================================================
  // Enums
  // ============================================================

  typedef enum
  {
    LRVX_SLIPPAGE_NONE = 0,
    LRVX_SLIPPAGE_FIXED_TICKS = 1,
    LRVX_SLIPPAGE_FIXED_BPS = 2,
    LRVX_SLIPPAGE_VOLUME_IMPACT = 3,
  } LrvxSlippageModel;

  typedef enum
  {
    LRVX_QUEUE_NONE = 0,
    LRVX_QUEUE_TOB = 1,
    LRVX_QUEUE_FULL = 2,
    LRVX_QUEUE_PRO_RATA = 3,
    LRVX_QUEUE_PRO_RATA_WITH_FIFO = 4,
  } LrvxQueueModel;

  typedef enum
  {
    LRVX_AGG_FILTER_TRADES = 1,
    LRVX_AGG_FILTER_BOOKS_ONLY = 2,
    LRVX_AGG_FILTER_BOTH = 3,
  } LrvxAggregatorEventFilter;

  // ============================================================
  // Flat event structs (C-compatible)
  // ============================================================

  typedef struct
  {
    uint32_t symbol;
    int64_t price_raw;
    int64_t quantity_raw;
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
    int64_t bid_price_raw; /* a price only when has_bid is 1, else 0 */
    int64_t bid_qty_raw;   /* size at the best bid, 0 when the book cannot say */
    int64_t ask_price_raw; /* a price only when has_ask is 1, else 0 */
    int64_t ask_qty_raw;   /* size at the best ask, 0 when the book cannot say */
    int64_t mid_raw;       /* a price only when both flags are 1, else 0 */
    int64_t spread_raw;    /* a price only when both flags are 1, else 0 */
    uint8_t has_bid;       /* 1 when the bid side has a best level, at any price */
    uint8_t has_ask;       /* 1 when the ask side has a best level, at any price */
  } LrvxBookSnapshot;

  typedef struct
  {
    uint32_t symbol;
    int64_t exchange_ts_ns;
    LrvxBookSnapshot snapshot;
  } LrvxBookData;

  /* OHLC bar event, delivered to LrvxStrategyCallbacks.on_bar.
   * bar_type: 0=Time, 1=Tick, 2=Volume, 3=Renko, 4=Range, 5=HeikinAshi,
   *           6=BpsRange (mirrors lrvx::BarType in aggregator/bar.h).
   * bar_type_param: interval in NANOSECONDS for Time bars, tick count for
   *           Tick, volume threshold for Volume, and the brick / range /
   *           bps size for the others.
   * close_reason: 0=Threshold, 1=Gap, 2=Forced, 3=Warmup. */
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

  /* has_avg_entry_price is 0 when the position manager reports no cost basis
   * -- a flat position, or a manager that keeps none. avg_entry_price_raw is
   * then 0 and means nothing, and bindings surface the unrealized PnL built
   * on it as NaN rather than as the position's whole notional. The flag sits
   * in padding the struct already had, so the layout does not move. */
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

  /* order_type here is the C++ lrvx::OrderType space (LIMIT=0, MARKET=1,
   * ...), the same space as LrvxOrder.type below. It is NOT the
   * LRVX_SIGNAL_TYPE_* space used by LrvxSignal.order_type. */
  typedef struct
  {
    uint64_t order_id;
    uint32_t symbol_id;
    uint8_t side;
    uint8_t order_type;
    uint8_t status;
    uint8_t _pad;
    int64_t fill_qty_raw;
    int64_t fill_price_raw;
    int64_t exchange_ts_ns;
    const char* reject_reason;
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
    uint8_t is_maker;
    uint8_t market_position;
    int32_t distance_to_best_ticks;
    uint8_t _pad2[2];
  } LrvxOrderEventData;

  /* close_reason: 0=Threshold, 1=Gap, 2=Forced, 3=Warmup (mirrors
   * lrvx::BarCloseReason, and LrvxBarData::close_reason above). Added in
   * ABI version 3. */
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

  typedef struct
  {
    uint64_t order_id;
    uint32_t symbol;
    uint8_t side;
    int64_t price_raw;
    int64_t quantity_raw;
    int64_t timestamp_ns;
  } LrvxFill;

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

  typedef struct
  {
    int64_t timestamp_ns;
    double equity;
    double drawdown_pct;
  } LrvxEquityPoint;

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

  typedef struct
  {
    int64_t first_event_ns;
    int64_t last_event_ns;
    uint64_t total_events;
    uint32_t segment_count;
    uint64_t total_bytes;
    double duration_seconds;
  } LrvxDatasetSummary;

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
    uint8_t event_type;
  } LrvxBBO;

  typedef struct
  {
    int64_t exchange_ts_ns;
    int64_t recv_ts_ns;
    int64_t seq;
    uint64_t level_offset;
    uint32_t symbol_id;
    uint16_t bid_count;
    uint16_t ask_count;
    uint8_t event_type;
  } LrvxBookUpdateHeader;

  typedef struct
  {
    int64_t price_raw;
    int64_t qty_raw;
    uint8_t side;
  } LrvxLevel;

  typedef struct
  {
    uint32_t global_id;
    int8_t price_precision;
    int8_t qty_precision;
    uint8_t _pad[2];
    const char* exchange;
    const char* name;
  } LrvxMergedSymbol;

  typedef struct
  {
    int64_t first_event_ns;
    int64_t last_event_ns;
    uint64_t trades;
    uint64_t books;
    const char* path;
  } LrvxMergedTapeStats;

  typedef struct
  {
    uint64_t bytes_written;
    uint64_t events_written;
    uint64_t segments_created;
    uint64_t trades_written;
  } LrvxWriterStats;

  typedef struct
  {
    uint32_t partition_id;
    int64_t from_ns;
    int64_t to_ns;
    int64_t warmup_from_ns;
    uint64_t estimated_events;
    uint64_t estimated_bytes;
  } LrvxPartition;

  /* order_type uses the LRVX_SIGNAL_TYPE_* codes below on every path
   * that fills this struct (strategy signals, and the risk-manager /
   * kill-switch / order-validator / PnL-tracker callbacks). It is NOT
   * the C++ lrvx::OrderType enum, whose LIMIT and MARKET are swapped
   * relative to these codes. */
  typedef struct
  {
    uint64_t order_id;
    uint32_t symbol;
    uint8_t side;
    uint8_t order_type;
    double price;
    double quantity;
    double trigger_price;
    double trailing_offset;
    int32_t trailing_bps;
    double new_price;
    double new_quantity;
    double range_lower;
    double range_upper;
    double liquidity;
  } LrvxSignal;

/* LRVX_SIGNAL_TYPE_* is the code space used ONLY by LrvxSignal.order_type
 * (see the comment above LrvxSignal). It swaps LIMIT and MARKET relative
 * to the C++ lrvx::OrderType enum (include/lrvx/common.h), which is the
 * space every other order_type / type field on this page uses (LrvxOrder,
 * LrvxOrderEventData, and the order_type parameter of
 * lrvx_simulated_executor_submit_order[_ex]). Do not reuse these codes to
 * decode any of those. include/lrvx/capi/order_type_names.hpp carries the
 * canonical string tables for both spaces. */
#define LRVX_SIGNAL_TYPE_MARKET 0
#define LRVX_SIGNAL_TYPE_LIMIT 1
#define LRVX_SIGNAL_TYPE_STOP_MARKET 2
#define LRVX_SIGNAL_TYPE_STOP_LIMIT 3
#define LRVX_SIGNAL_TYPE_TAKE_PROFIT_MARKET 4
#define LRVX_SIGNAL_TYPE_TAKE_PROFIT_LIMIT 5
#define LRVX_SIGNAL_TYPE_TRAILING_STOP 6
#define LRVX_SIGNAL_TYPE_CANCEL 7
#define LRVX_SIGNAL_TYPE_CANCEL_ALL 8
#define LRVX_SIGNAL_TYPE_MODIFY 9
#define LRVX_SIGNAL_TYPE_ICEBERG 10
#define LRVX_SIGNAL_TYPE_OCO 11
/* The liquidity pair acts on an AMM pool rather than on an order book, so
 * `price` and `quantity` on the signal carry nothing for these two codes --
 * the range and the position size are on `range_lower` / `range_upper` /
 * `liquidity` instead. A pre-trade gate that filters on notional will wave
 * these through regardless: check the code first, then read the right
 * fields. Before the codes existed all three fell into the switch default
 * and arrived as LRVX_SIGNAL_TYPE_MARKET with every field zero, which no
 * gate could tell from a real market order. */
#define LRVX_SIGNAL_TYPE_PROVIDE_LIQUIDITY 12
#define LRVX_SIGNAL_TYPE_WITHDRAW_LIQUIDITY 13

  typedef struct
  {
    uint8_t type;
    uint8_t _pad[3];
    int64_t timestamp_ns;
    uint32_t trade_symbol;
    uint8_t trade_is_buy;
    uint8_t _pad2[3];
    int64_t trade_price_raw;
    int64_t trade_quantity_raw;
    uint32_t book_symbol;
    uint32_t n_bids;
    uint32_t n_asks;
    uint32_t _pad3;
    const LrvxBookLevel* bids;
    const LrvxBookLevel* asks;
  } LrvxReplayEvent;

  /* type is the C++ lrvx::OrderType space (LIMIT=0, MARKET=1, ...,
   * ICEBERG=7), the same space as LrvxOrderEventData.order_type above.
   * It is NOT the LRVX_SIGNAL_TYPE_* space used by LrvxSignal.order_type
   * a few lines up in this file. */
  typedef struct
  {
    uint64_t id;
    uint64_t client_order_id;
    uint32_t symbol;
    uint16_t strategy_id;
    uint16_t order_tag;
    uint8_t side;
    uint8_t type;
    uint8_t time_in_force;
    uint8_t flags;
    int64_t price_raw;
    int64_t quantity_raw;
    int64_t filled_quantity_raw;
    int64_t trigger_price_raw;
    int64_t trailing_offset_raw;
    int64_t created_at_ns;
    int64_t exchange_ts_ns;
  } LrvxOrder;

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

  typedef struct
  {
    uint8_t mode;
    uint64_t train_size;
    uint64_t test_size;
    uint64_t step;
    uint64_t min_train_size;
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

  typedef struct
  {
    const double* z;
    uint32_t rows;
    uint32_t cols;
    const char* const* row_labels;
    uint32_t num_row_labels;
    const char* const* col_labels;
    uint32_t num_col_labels;
    const char* title;
    const char* x_axis_name;
    const char* y_axis_name;
    const char* metric_name;
  } LrvxHeatmapData;

  typedef struct
  {
    int64_t feed_ns;
    int64_t order_ns;
    int64_t fill_ns;
  } LrvxLatencySample;

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

  /* A single breach.
   *
   * `rule` and `detail` point into storage the library keeps for this
   * handle, not into the caller's memory. They stay valid until the next
   * call that refills that storage for the same handle --
   * lrvx_portfolio_risk_check_order, lrvx_portfolio_risk_breach_count --
   * or until lrvx_portfolio_risk_destroy. A call on a different handle,
   * or on another thread, does not disturb them. Copy the strings out if
   * they have to live longer than that. */
  typedef struct
  {
    const char* rule;
    double value;
    double limit;
    const char* detail;
  } LrvxBreach;

  typedef struct
  {
    uint64_t order_id;
    int64_t timestamp_ns;
    double qty;
    double price;
    uint8_t type;
  } LrvxExecChildOrder;

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

  // ============================================================
  // Callback function pointer types
  // ============================================================

  typedef void (*LrvxOnTradeCallback)(void*, const LrvxSymbolContext*, const LrvxTradeData*);
  typedef void (*LrvxOnBookCallback)(void*, const LrvxSymbolContext*, const LrvxBookData*);
  typedef void (*LrvxOnBarCallback)(void*, const LrvxSymbolContext*, const LrvxBarData*);
  typedef void (*LrvxOnFillCallback)(void*, const LrvxSymbolContext*, const LrvxOrderEventData*);
  typedef void (*LrvxOnOrderUpdateCallback)(void*, const LrvxSymbolContext*, const LrvxOrderEventData*);
  typedef void (*LrvxOnStartCallback)(void*);
  typedef void (*LrvxOnStopCallback)(void*);
  typedef void (*LrvxOnQueuePositionChangeCallback)(void*, const LrvxSymbolContext*, const LrvxOrderEventData*);
  typedef void (*LrvxOnMarketPositionChangeCallback)(void*, const LrvxSymbolContext*, const LrvxOrderEventData*);
  typedef const double* (*LrvxGraphNodeFn)(void*, LrvxIndicatorGraphHandle, uint32_t, size_t*);
  typedef void (*LrvxOnSignalCallback)(void*, const LrvxSignal*);
  typedef uint8_t (*LrvxRiskManagerAllowFn)(void*, const LrvxSignal*);
  typedef uint8_t (*LrvxKillSwitchCheckFn)(void*, const LrvxSignal*);
  typedef uint8_t (*LrvxOrderValidatorValidateFn)(void*, const LrvxSignal*);
  typedef void (*LrvxLogCallback)(void*, int32_t, const char*);
  typedef void (*LrvxPnLTrackerOnSignalFn)(void*, const LrvxSignal*);
  typedef void (*LrvxStorageSinkStoreFn)(void*, const LrvxSignal*);
  typedef void (*LrvxRecorderOnTradeFn)(void*, const LrvxTradeData*);
  typedef void (*LrvxRecorderOnBookUpdateFn)(void*, uint32_t, uint8_t, const LrvxBookLevel*, uint32_t, const LrvxBookLevel*, uint32_t, int64_t);
  typedef void (*LrvxRecorderLifecycleFn)(void*);
  typedef uint8_t (*LrvxReplaySourceNextFn)(void*, LrvxReplayEvent*);
  typedef uint8_t (*LrvxReplaySourceSeekFn)(void*, int64_t);
  typedef void (*LrvxReplaySourceLifecycleFn)(void*);
  typedef void (*LrvxExecListenerOnOrderFn)(void*, const LrvxOrder*);
  typedef void (*LrvxExecListenerOnPartialFillFn)(void*, const LrvxOrder*, int64_t);
  typedef void (*LrvxExecListenerOnRejectedFn)(void*, const LrvxOrder*, const char*);
  typedef void (*LrvxExecListenerOnReplacedFn)(void*, const LrvxOrder*, const LrvxOrder*);
  typedef void (*LrvxExecListenerOnTrailingUpdateFn)(void*, const LrvxOrder*, int64_t);
  typedef void (*LrvxExecListenerOnQueuePositionChangeFn)(void*, const LrvxOrder*, int64_t, int64_t);
  typedef void (*LrvxExecListenerOnMarketPositionChangeFn)(void*, const LrvxOrder*, uint8_t, int32_t);
  typedef void (*LrvxExecListenerOnReplaceSubmittedFn)(void*, const LrvxOrder*, const LrvxOrder*);
  typedef void (*LrvxExecListenerOnReplaceAcceptedFn)(void*, const LrvxOrder*, const LrvxOrder*);
  typedef void (*LrvxExecListenerOnReplaceRejectedFn)(void*, const LrvxOrder*, const LrvxOrder*, const char*);
  typedef void (*LrvxExecutorSubmitFn)(void*, const LrvxOrder*);
  typedef void (*LrvxExecutorCancelFn)(void*, uint64_t);
  typedef void (*LrvxExecutorCancelAllFn)(void*, uint32_t);
  typedef void (*LrvxExecutorReplaceFn)(void*, uint64_t, const LrvxOrder*);
  typedef void (*LrvxExecutorSubmitOCOFn)(void*, const LrvxOrder*, const LrvxOrder*);
  typedef void (*LrvxExecutorCapabilitiesFn)(void*, LrvxExchangeCapabilities*);
  typedef void (*LrvxExecutorLifecycleFn)(void*);
  typedef LrvxStrategyHandle (*LrvxWalkForwardFactoryFn)(void*, uint64_t);
  typedef int (*LrvxGridSearchFactoryFn)(void*, uint64_t, const double*, uint32_t, LrvxBacktestStats*);
  typedef uint8_t (*LrvxProgressCallback)(void*, double, int64_t);

  // ============================================================
  // Callback bundles
  // ============================================================

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

  typedef struct
  {
    LrvxRiskManagerAllowFn allow;
    void* user_data;
  } LrvxRiskManagerCallbacks;

  typedef struct
  {
    LrvxKillSwitchCheckFn check;
    void* user_data;
  } LrvxKillSwitchCallbacks;

  typedef struct
  {
    LrvxOrderValidatorValidateFn validate;
    void* user_data;
  } LrvxOrderValidatorCallbacks;

  typedef struct
  {
    LrvxPnLTrackerOnSignalFn on_signal;
    void* user_data;
  } LrvxPnLTrackerCallbacks;

  typedef struct
  {
    LrvxStorageSinkStoreFn store;
    void* user_data;
  } LrvxStorageSinkCallbacks;

  typedef struct
  {
    LrvxRecorderOnTradeFn on_trade;
    LrvxRecorderOnBookUpdateFn on_book_update;
    LrvxRecorderLifecycleFn on_start;
    LrvxRecorderLifecycleFn on_stop;
    void* user_data;
  } LrvxMarketDataRecorderCallbacks;

  typedef struct
  {
    LrvxReplaySourceLifecycleFn on_start;
    LrvxReplaySourceLifecycleFn on_stop;
    LrvxReplaySourceSeekFn seek_to;
    LrvxReplaySourceNextFn next;
    void* user_data;
  } LrvxReplaySourceCallbacks;

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

  // ============================================================
  // Fixed-point conversion helpers
  // ============================================================

  int64_t lrvx_price_from_double(double value);
  double lrvx_price_to_double(int64_t raw);
  int64_t lrvx_quantity_from_double(double value);
  double lrvx_quantity_to_double(int64_t raw);

  // ============================================================
  // Account
  // ============================================================

  LrvxAccountHandle lrvx_account_create(uint64_t account_id, double equity);
  void lrvx_account_destroy(LrvxAccountHandle h);
  uint64_t lrvx_account_id(LrvxAccountHandle h);
  double lrvx_account_equity(LrvxAccountHandle h);
  void lrvx_account_set_equity(LrvxAccountHandle h, double equity);
  void lrvx_account_add_equity(LrvxAccountHandle h, double delta);
  uint8_t lrvx_account_margin_mode(LrvxAccountHandle h);
  void lrvx_account_set_margin_mode(LrvxAccountHandle h, uint8_t mode);
  void lrvx_account_open_position(LrvxAccountHandle h, uint32_t symbol, double quantity,
                                  double entry_price);
  void lrvx_account_open_position_isolated(LrvxAccountHandle h, uint32_t symbol, double quantity,
                                           double entry_price, double isolated_equity);
  void lrvx_account_close_position(LrvxAccountHandle h, uint32_t symbol);
  uint32_t lrvx_account_position_count(LrvxAccountHandle h);
  void lrvx_account_set_mark(LrvxAccountHandle h, uint32_t symbol, double price);
  void lrvx_account_set_mark_at(LrvxAccountHandle h, uint32_t symbol, double price, int64_t ts_ns);
  int64_t lrvx_account_mark_ts(LrvxAccountHandle h, uint32_t symbol);
  uint8_t lrvx_account_has_stale_marks(LrvxAccountHandle h, int64_t now_ns, int64_t budget_ns);
  double lrvx_account_total_notional(LrvxAccountHandle h);
  double lrvx_account_total_unrealised_pnl(LrvxAccountHandle h);
  void lrvx_account_record_fill(LrvxAccountHandle h, int64_t ts_ns, double notional);
  void lrvx_account_record_fill_ex(LrvxAccountHandle h, int64_t ts_ns, double notional,
                                   uint32_t symbol);
  uint32_t lrvx_account_rolling_notional_by_symbol_size(LrvxAccountHandle h);
  uint32_t lrvx_account_rolling_notional_by_symbol_copy(LrvxAccountHandle h, uint32_t* symbols_out,
                                                        double* notionals_out, uint32_t max);
  double lrvx_account_rolling_notional_30d(LrvxAccountHandle h);
  void lrvx_account_reset_rolling(LrvxAccountHandle h);

  // ============================================================
  // Additional Bar
  // ============================================================

  uint32_t lrvx_aggregate_range_bars(const int64_t* timestamps, const double* prices,
                                     const double* quantities, const uint8_t* is_buy, size_t len,
                                     double range_size, LrvxBar* bars_out, uint32_t max_bars);
  uint32_t lrvx_aggregate_renko_bars(const int64_t* timestamps, const double* prices,
                                     const double* quantities, const uint8_t* is_buy, size_t len,
                                     double brick_size, LrvxBar* bars_out, uint32_t max_bars);

  // ============================================================
  // Additional Stats
  // ============================================================

  double lrvx_stat_permutation_test(const double* group1, size_t len1, const double* group2,
                                    size_t len2, uint32_t num_permutations);
  void lrvx_stat_bootstrap_ci(const double* data, size_t len, double confidence,
                              uint32_t num_samples, double* lower_out, double* median_out,
                              double* upper_out);
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
  void lrvx_stat_pbo_cscv(const double* returns, size_t num_trials, size_t num_periods,
                          uint32_t num_blocks, double* pbo_out, uint64_t* splits_out,
                          double* mean_oos_rank_out, double* mean_logit_out);
  /* Deflated Sharpe ratio (Bailey, Lopez de Prado). returns is the selected
   * strategy's per-period return series; trial_sharpes holds the per-period
   * Sharpe ratio of every trial the selection ran over (NULL or fewer than 2
   * of them means no selection, SR0 = 0). Every output is NaN with fewer than
   * 2 periods or a zero return variance; dsr_out alone is NaN when the
   * non-normality correction has no real root. */
  void lrvx_stat_deflated_sharpe(const double* returns, size_t num_periods,
                                 const double* trial_sharpes, size_t num_trials,
                                 double* sharpe_out, double* skewness_out, double* kurtosis_out,
                                 double* expected_max_sharpe_out, double* dsr_out);
  /* Per-cell neighbourhood of a parameter grid: the mean of the neighbouring
   * cells' values (every cell whose index differs by at most radius on every
   * axis, the cell itself excluded, NaN values skipped) and their count.
   * values is row-major over shape[ndim]; both outputs have as many cells as
   * values. A cell with no neighbour gets a NaN mean and a count of 0. */
  void lrvx_stat_grid_neighborhood(const double* values, const size_t* shape, size_t ndim,
                                   size_t radius, double* neighbor_mean_out,
                                   uint32_t* neighbor_count_out);

  // ============================================================
  // Amm Curve
  // ============================================================

  /* lrvx_curve_constant_product, lrvx_curve_raydium_cp and
   * lrvx_curve_uniswap_v3 do not end in _create, but they follow the
   * _create half of the ownership rule: each returns a handle the caller
   * OWNS and must pass to lrvx_curve_destroy. NULL on bad input. */
  LrvxCurveHandle lrvx_curve_constant_product(const char* reserve0, const char* reserve1,
                                              uint64_t fee_num, uint64_t fee_den);
  LrvxCurveHandle lrvx_curve_raydium_cp(const char* reserve0, const char* reserve1,
                                        uint64_t trade_fee_rate, uint64_t creator_fee_rate,
                                        uint8_t creator_fee_on_input);
  LrvxCurveHandle lrvx_curve_uniswap_v3(const char* sqrt_price_x96, const char* liquidity,
                                        uint32_t fee_pips, const char* const* tick_sqrt_ratio,
                                        const char* const* tick_liquidity_net, size_t n_ticks);
  size_t lrvx_curve_token_count(LrvxCurveHandle curve);
  uint8_t lrvx_curve_amount_out(LrvxCurveHandle curve, size_t i, size_t j, const char* amount_in,
                                char* out, size_t out_len);
  uint8_t lrvx_curve_apply_swap(LrvxCurveHandle curve, size_t i, size_t j, const char* amount_in,
                                char* out, size_t out_len);
  uint8_t lrvx_curve_balance(LrvxCurveHandle curve, size_t i, char* out, size_t out_len);
  uint8_t lrvx_curve_sqrt_price(LrvxCurveHandle curve, char* out, size_t out_len);
  uint8_t lrvx_curve_liquidity(LrvxCurveHandle curve, char* out, size_t out_len);
  /* Owned: a fresh copy the caller destroys with lrvx_curve_destroy,
   * independent of the curve it was cloned from. */
  LrvxCurveHandle lrvx_curve_clone(LrvxCurveHandle curve);
  void lrvx_curve_destroy(LrvxCurveHandle curve);

  // ============================================================
  // Backtest Slippage
  // ============================================================

  void lrvx_simulated_executor_set_default_slippage(LrvxSimulatedExecutorHandle executor,
                                                    int32_t model, int32_t ticks, double tick_size,
                                                    double bps, double impact_coeff);
  void lrvx_simulated_executor_set_symbol_slippage(LrvxSimulatedExecutorHandle executor,
                                                   uint32_t symbol, int32_t model, int32_t ticks,
                                                   double tick_size, double bps, double impact_coeff);
  void lrvx_simulated_executor_set_queue_model(LrvxSimulatedExecutorHandle executor, int32_t model,
                                               uint32_t depth);
  void lrvx_simulated_executor_set_queue_fifo_top_n(LrvxSimulatedExecutorHandle executor,
                                                    uint32_t top_n);
  void lrvx_simulated_executor_set_top_priority_share(LrvxSimulatedExecutorHandle executor,
                                                      double share);
  void lrvx_simulated_executor_set_lmm_orders(LrvxSimulatedExecutorHandle executor,
                                              const uint64_t* ids, uint32_t n_ids);
  void lrvx_simulated_executor_set_lmm_bonus_multiplier(LrvxSimulatedExecutorHandle executor,
                                                        double multiplier);
  void lrvx_simulated_executor_set_order_priority_multiplier(LrvxSimulatedExecutorHandle executor,
                                                             uint64_t order_id, double multiplier);
  void lrvx_simulated_executor_set_submit_ack_latency(LrvxSimulatedExecutorHandle executor,
                                                      int64_t latency_ns, int64_t jitter_ns);
  void lrvx_simulated_executor_set_cancel_ack_latency(LrvxSimulatedExecutorHandle executor,
                                                      int64_t latency_ns, int64_t jitter_ns);
  void lrvx_simulated_executor_set_replace_ack_latency(LrvxSimulatedExecutorHandle executor,
                                                       int64_t latency_ns, int64_t jitter_ns);
  int lrvx_simulated_executor_apply_latency_profile(LrvxSimulatedExecutorHandle executor,
                                                    const char* profile_name);
  void lrvx_simulated_executor_set_stp_mode(LrvxSimulatedExecutorHandle executor, uint8_t mode);
  void lrvx_simulated_executor_set_fok_mode(LrvxSimulatedExecutorHandle executor, uint8_t mode);
  uint8_t lrvx_simulated_executor_fok_mode(LrvxSimulatedExecutorHandle executor);
  void lrvx_simulated_executor_set_stp_group_membership(LrvxSimulatedExecutorHandle executor,
                                                        uint64_t account_id, uint64_t group_id);
  uint64_t lrvx_simulated_executor_stp_group_for(LrvxSimulatedExecutorHandle executor,
                                                 uint64_t account_id);
  LrvxLatencyDistributionHandle lrvx_latency_distribution_create(void);
  void lrvx_latency_distribution_destroy(LrvxLatencyDistributionHandle h);
  void lrvx_latency_distribution_set_constant(LrvxLatencyDistributionHandle h, int64_t ns);
  void lrvx_latency_distribution_set_uniform(LrvxLatencyDistributionHandle h, int64_t lo_ns,
                                             int64_t hi_ns);
  void lrvx_latency_distribution_set_lognormal(LrvxLatencyDistributionHandle h, int64_t median_ns,
                                               double sigma);
  void lrvx_latency_distribution_set_empirical(LrvxLatencyDistributionHandle h,
                                               const int64_t* samples_ns, uint32_t n_samples);
  void lrvx_latency_distribution_set_burst_correlation(LrvxLatencyDistributionHandle h, double rho);
  int64_t lrvx_latency_distribution_median_ns(LrvxLatencyDistributionHandle h);
  void lrvx_simulated_executor_set_submit_ack_latency_distribution(LrvxSimulatedExecutorHandle executor,
                                                                   LrvxLatencyDistributionHandle dist);
  void lrvx_simulated_executor_set_cancel_ack_latency_distribution(LrvxSimulatedExecutorHandle executor,
                                                                   LrvxLatencyDistributionHandle dist);
  void lrvx_simulated_executor_set_replace_ack_latency_distribution(LrvxSimulatedExecutorHandle executor,
                                                                    LrvxLatencyDistributionHandle dist);
  void lrvx_simulated_executor_on_trade_qty(LrvxSimulatedExecutorHandle executor, uint32_t symbol,
                                            double price, double quantity, uint8_t is_buy);
  void lrvx_simulated_executor_on_best_levels(LrvxSimulatedExecutorHandle executor, uint32_t symbol,
                                              double bid_price, double bid_qty, double ask_price,
                                              double ask_qty);
  void lrvx_simulated_executor_on_book_snapshot(LrvxSimulatedExecutorHandle executor,
                                                uint32_t symbol, const double* bid_prices,
                                                const double* bid_qtys, uint32_t n_bids,
                                                const double* ask_prices, const double* ask_qtys,
                                                uint32_t n_asks);
  LrvxBacktestResultHandle lrvx_backtest_result_create(double initial_capital, double fee_rate,
                                                       uint8_t use_percentage_fee,
                                                       double fixed_fee_per_trade,
                                                       double risk_free_rate,
                                                       double annualization_factor);
  void lrvx_backtest_result_destroy(LrvxBacktestResultHandle result);
  void lrvx_backtest_result_record_fill(LrvxBacktestResultHandle result, uint64_t order_id,
                                        uint32_t symbol, uint8_t side, double price, double quantity,
                                        int64_t timestamp_ns);
  void lrvx_backtest_result_ingest_executor(LrvxBacktestResultHandle result,
                                            LrvxSimulatedExecutorHandle executor);
  void lrvx_backtest_result_stats(LrvxBacktestResultHandle result, LrvxBacktestStats* out);
  uint32_t lrvx_backtest_result_equity_curve(LrvxBacktestResultHandle result,
                                             LrvxEquityPoint* points_out, uint32_t max_points);
  uint8_t lrvx_backtest_result_write_equity_curve_csv(LrvxBacktestResultHandle result,
                                                      const char* path);
  uint32_t lrvx_backtest_result_trades(LrvxBacktestResultHandle result,
                                       LrvxBacktestTrade* trades_out, uint32_t max_trades);

  // ============================================================
  // Backtestrunner Replay
  // ============================================================

  LrvxBacktestRunnerHandle lrvx_backtest_runner_create(LrvxRegistryHandle registry, double fee_rate,
                                                       double initial_capital);
  void lrvx_backtest_runner_destroy(LrvxBacktestRunnerHandle runner);
  void lrvx_backtest_runner_set_strategy(LrvxBacktestRunnerHandle runner,
                                         LrvxStrategyHandle strategy);
  int lrvx_backtest_runner_run_csv(LrvxBacktestRunnerHandle runner, const char* path,
                                   const char* symbol, LrvxBacktestStats* stats_out);
  int lrvx_backtest_runner_run_tape(LrvxBacktestRunnerHandle runner, const char* tape_dir,
                                    LrvxBacktestStats* stats_out);
  int lrvx_backtest_runner_run_tapes(LrvxBacktestRunnerHandle runner, const char* const* tape_dirs,
                                     uint32_t n_dirs, LrvxBacktestStats* stats_out);
  int lrvx_backtest_runner_run_ohlcv(LrvxBacktestRunnerHandle runner, const int64_t* timestamps_ns,
                                     const double* close_prices, uint32_t n, const char* symbol,
                                     LrvxBacktestStats* stats_out);
  int lrvx_backtest_runner_run_bars(LrvxBacktestRunnerHandle runner, const int64_t* start_time_ns,
                                    const int64_t* end_time_ns, const double* open,
                                    const double* high, const double* low, const double* close,
                                    const double* volume, uint32_t n, const char* symbol,
                                    uint8_t bar_type, uint64_t bar_type_param,
                                    LrvxBacktestStats* stats_out);
  int lrvx_backtest_runner_run_replay_source(LrvxBacktestRunnerHandle runner,
                                             LrvxReplaySourceHandle source,
                                             LrvxBacktestStats* stats_out);
  /* Owned: a snapshot of the last run, detached from the runner. The
   * caller destroys it with lrvx_backtest_result_destroy; the runner may
   * be destroyed first. NULL when no run has finished yet. */
  LrvxBacktestResultHandle lrvx_backtest_runner_take_result(LrvxBacktestRunnerHandle runner);
  void lrvx_backtest_runner_set_risk_manager(LrvxBacktestRunnerHandle runner,
                                             LrvxRiskManagerHandle rm);
  void lrvx_backtest_runner_set_kill_switch(LrvxBacktestRunnerHandle runner, LrvxKillSwitchHandle ks);
  void lrvx_backtest_runner_set_order_validator(LrvxBacktestRunnerHandle runner,
                                                LrvxOrderValidatorHandle ov);
  void lrvx_backtest_runner_set_pnl_tracker(LrvxBacktestRunnerHandle runner,
                                            LrvxPnLTrackerHandle tracker);

  // ============================================================
  // Bar Aggregation
  // ============================================================

  uint32_t lrvx_aggregate_time_bars(const int64_t* timestamps, const double* prices,
                                    const double* quantities, const uint8_t* is_buy, size_t len,
                                    double interval_seconds, LrvxBar* bars_out, uint32_t max_bars);
  uint32_t lrvx_aggregate_tick_bars(const int64_t* timestamps, const double* prices,
                                    const double* quantities, const uint8_t* is_buy, size_t len,
                                    uint32_t tick_count, LrvxBar* bars_out, uint32_t max_bars);
  uint32_t lrvx_aggregate_volume_bars(const int64_t* timestamps, const double* prices,
                                      const double* quantities, const uint8_t* is_buy, size_t len,
                                      double volume_threshold, LrvxBar* bars_out, uint32_t max_bars);

  // ============================================================
  // Bar Dispatch
  // ============================================================

  LrvxBarDispatchRecorderHandle lrvx_bar_dispatch_recorder_create(void);
  void lrvx_bar_dispatch_recorder_destroy(LrvxBarDispatchRecorderHandle h);
  uint32_t lrvx_bar_dispatch_recorder_add_time_seconds(LrvxBarDispatchRecorderHandle h,
                                                       uint32_t seconds);
  void lrvx_bar_dispatch_recorder_on_trade(LrvxBarDispatchRecorderHandle h, uint32_t symbol,
                                           double price, double qty, int64_t ts_ns);
  void lrvx_bar_dispatch_recorder_finalize(LrvxBarDispatchRecorderHandle h);
  uint32_t lrvx_bar_dispatch_recorder_count(LrvxBarDispatchRecorderHandle h);
  uint8_t lrvx_bar_dispatch_recorder_type_at(LrvxBarDispatchRecorderHandle h, uint32_t index);
  uint64_t lrvx_bar_dispatch_recorder_param_at(LrvxBarDispatchRecorderHandle h, uint32_t index);

  // ============================================================
  // Binary Log Recorder Hook
  // ============================================================

  LrvxBinaryLogRecorderHookHandle lrvx_binary_log_recorder_hook_create(const char* output_dir,
                                                                       uint64_t max_segment_mb,
                                                                       uint8_t exchange_id,
                                                                       uint8_t compression);
  LrvxBinaryLogRecorderHookHandle lrvx_binary_log_recorder_hook_create_ex(const char* output_dir,
                                                                          uint64_t max_segment_mb,
                                                                          uint8_t exchange_id,
                                                                          uint8_t compression,
                                                                          const char* exchange_name,
                                                                          const char* instrument_type);
  void lrvx_binary_log_recorder_hook_destroy(LrvxBinaryLogRecorderHookHandle hook);
  /* Borrowed: a view of a member of the hook. It stays valid while the
   * hook lives and must not be passed to lrvx_market_data_recorder_destroy,
   * which is a no-op on it. */
  LrvxMarketDataRecorderHandle lrvx_binary_log_recorder_hook_as_recorder(LrvxBinaryLogRecorderHookHandle hook);
  void lrvx_binary_log_recorder_hook_add_symbol(LrvxBinaryLogRecorderHookHandle hook,
                                                uint32_t symbol_id, const char* name,
                                                const char* base, const char* quote,
                                                int8_t price_precision, int8_t qty_precision);
  void lrvx_binary_log_recorder_hook_flush(LrvxBinaryLogRecorderHookHandle hook);
  LrvxWriterStats lrvx_binary_log_recorder_hook_stats(LrvxBinaryLogRecorderHookHandle hook);

  // ============================================================
  // Composite Book
  // ============================================================

  LrvxCompositeBookHandle lrvx_composite_book_create(void);
  void lrvx_composite_book_destroy(LrvxCompositeBookHandle book);
  uint8_t lrvx_composite_book_best_bid(LrvxCompositeBookHandle book, uint32_t symbol,
                                       double* price_out, double* qty_out);
  uint8_t lrvx_composite_book_best_ask(LrvxCompositeBookHandle book, uint32_t symbol,
                                       double* price_out, double* qty_out);
  uint8_t lrvx_composite_book_has_arb(LrvxCompositeBookHandle book, uint32_t symbol);
  void lrvx_composite_book_mark_stale(LrvxCompositeBookHandle book, uint32_t exchange,
                                      uint32_t symbol);
  void lrvx_composite_book_check_staleness(LrvxCompositeBookHandle book, int64_t now_ns,
                                           int64_t threshold_ns);
  /* Feeds one exchange/symbol slot of the composite book. is_delta=false
   * (apply_snapshot) replaces both sides of that exchange's top-of-book
   * wholesale; is_delta=true (apply_delta) updates only the side(s)
   * present here and leaves the other side exactly as it was, matching
   * BookUpdateType::SNAPSHOT / DELTA in the C++ CompositeBookMatrix this
   * wraps. recv_ns is a monotonic receive timestamp used for staleness
   * checks (see lrvx_composite_book_check_staleness above). */
  void lrvx_composite_book_apply_snapshot(LrvxCompositeBookHandle book, uint32_t exchange,
                                          uint32_t symbol, const double* bid_prices,
                                          const double* bid_qtys, size_t bid_len,
                                          const double* ask_prices, const double* ask_qtys,
                                          size_t ask_len, int64_t recv_ns);
  void lrvx_composite_book_apply_delta(LrvxCompositeBookHandle book, uint32_t exchange,
                                       uint32_t symbol, const double* bid_prices,
                                       const double* bid_qtys, size_t bid_len,
                                       const double* ask_prices, const double* ask_qtys,
                                       size_t ask_len, int64_t recv_ns);

  // ============================================================
  // Context Queries
  // ============================================================

  int64_t lrvx_position_raw(LrvxStrategyHandle s, uint32_t symbol);
  int64_t lrvx_last_trade_price_raw(LrvxStrategyHandle s, uint32_t symbol);
  /* The quote accessors below answer from the touch — the same top the
   * LrvxBookSnapshot in every callback carries — so they also follow
   * TOP_OF_BOOK frames the depth book ignores. */
  int64_t lrvx_best_bid_raw(LrvxStrategyHandle s, uint32_t symbol);
  int64_t lrvx_best_ask_raw(LrvxStrategyHandle s, uint32_t symbol);
  int64_t lrvx_mid_price_raw(LrvxStrategyHandle s, uint32_t symbol);

  /* The _opt trio answers the question the three above cannot: they return 0
   * both when the side is empty and when the best quote is a price of exactly
   * 0.0, and a book quoting through zero reaches that price. Here the return
   * value is the presence flag -- 1 with the raw price written to price_out,
   * 0 with price_out untouched -- so "no quote" has its own answer. price_out
   * may be NULL when only the flag is wanted. The three int64_t forms above
   * are unchanged and stay for callers that cannot see below zero.
   * LRVX_HAS_OPTIONAL_RAW_BEST_QUOTE marks a header that declares them. */
  uint8_t lrvx_best_bid_raw_opt(LrvxStrategyHandle s, uint32_t symbol, int64_t* price_out);
  uint8_t lrvx_best_ask_raw_opt(LrvxStrategyHandle s, uint32_t symbol, int64_t* price_out);
  uint8_t lrvx_mid_price_raw_opt(LrvxStrategyHandle s, uint32_t symbol, int64_t* price_out);

  void lrvx_get_symbol_context(LrvxStrategyHandle s, uint32_t symbol, LrvxSymbolContext* out);
  int32_t lrvx_get_order_status(LrvxStrategyHandle s, uint64_t order_id);

  // ============================================================
  // Data Reader
  // ============================================================

  LrvxDataReaderHandle lrvx_data_reader_create(const char* data_dir);
  void lrvx_data_reader_destroy(LrvxDataReaderHandle reader);
  uint64_t lrvx_data_reader_count(LrvxDataReaderHandle reader);
  void lrvx_data_reader_set_progress_callback(LrvxDataReaderHandle reader, LrvxProgressCallback cb,
                                              void* user_data, uint32_t interval_ms);
  void lrvx_data_reader_clear_progress_callback(LrvxDataReaderHandle reader);

  // ============================================================
  // Data Writer
  // ============================================================

  LrvxDataWriterHandle lrvx_data_writer_create(const char* output_dir, uint64_t max_segment_mb,
                                               uint8_t exchange_id);
  void lrvx_data_writer_destroy(LrvxDataWriterHandle writer);
  uint8_t lrvx_data_writer_write_trade(LrvxDataWriterHandle writer, int64_t exchange_ts_ns,
                                       int64_t recv_ts_ns, double price, double qty,
                                       uint64_t trade_id, uint32_t symbol_id, uint8_t side);
  uint8_t lrvx_data_writer_write_book(LrvxDataWriterHandle writer, int64_t exchange_ts_ns,
                                      int64_t recv_ts_ns, int64_t seq, uint32_t symbol_id,
                                      uint8_t is_snapshot, const LrvxBookLevel* bids,
                                      uint32_t n_bids, const LrvxBookLevel* asks, uint32_t n_asks);
  void lrvx_data_writer_flush(LrvxDataWriterHandle writer);
  void lrvx_data_writer_close(LrvxDataWriterHandle writer);

  // ============================================================
  // Datareader
  // ============================================================

  LrvxDataReaderHandle lrvx_data_reader_create_filtered(const char* data_dir, int64_t from_ns,
                                                        int64_t to_ns, const uint32_t* symbols,
                                                        uint32_t num_symbols);
  LrvxDataReaderHandle lrvx_data_reader_create_ordered(const char* data_dir, int64_t from_ns,
                                                       int64_t to_ns, const uint32_t* symbols,
                                                       uint32_t num_symbols,
                                                       int64_t reorder_window_ns,
                                                       int32_t strict_ordering);
  LrvxDatasetSummary lrvx_data_reader_summary(LrvxDataReaderHandle reader);
  LrvxReaderStats lrvx_data_reader_stats(LrvxDataReaderHandle reader);
  uint64_t lrvx_data_reader_read_trades(LrvxDataReaderHandle reader, LrvxTradeRecord* trades_out,
                                        uint64_t max_trades);
  uint64_t lrvx_data_reader_read_bbo(LrvxDataReaderHandle reader, LrvxBBO* bbos_out,
                                     uint64_t max_events);
  uint64_t lrvx_data_reader_count_book_updates(LrvxDataReaderHandle reader,
                                               uint64_t* total_levels_out);
  uint64_t lrvx_data_reader_read_book_updates(LrvxDataReaderHandle reader,
                                              LrvxBookUpdateHeader* headers_out,
                                              uint64_t max_events, LrvxLevel* levels_out,
                                              uint64_t max_levels);
  uint64_t lrvx_data_reader_read_trades_from(LrvxDataReaderHandle reader, int64_t start_ts_ns,
                                             LrvxTradeRecord* trades_out, uint64_t max_trades);
  uint64_t lrvx_data_reader_read_bbo_from(LrvxDataReaderHandle reader, int64_t start_ts_ns,
                                          LrvxBBO* bbos_out, uint64_t max_events);
  uint64_t lrvx_data_reader_count_book_updates_from(LrvxDataReaderHandle reader, int64_t start_ts_ns,
                                                    uint64_t* total_levels_out);
  uint64_t lrvx_data_reader_read_book_updates_from(LrvxDataReaderHandle reader, int64_t start_ts_ns,
                                                   LrvxBookUpdateHeader* headers_out,
                                                   uint64_t max_events, LrvxLevel* levels_out,
                                                   uint64_t max_levels);

  // ============================================================
  // Datawriter
  // ============================================================

  LrvxWriterStats lrvx_data_writer_stats(LrvxDataWriterHandle writer);
  uint64_t lrvx_data_writer_write_books(LrvxDataWriterHandle writer,
                                        const LrvxBookUpdateHeader* headers, uint64_t n_events,
                                        const LrvxLevel* levels, uint64_t total_levels);

  // ============================================================
  // Delta Book
  // ============================================================

  LrvxDeltaBookEncoderHandle lrvx_delta_book_encoder_create(uint32_t anchor_every);
  void lrvx_delta_book_encoder_destroy(LrvxDeltaBookEncoderHandle handle);
  void lrvx_delta_book_encoder_reset(LrvxDeltaBookEncoderHandle handle, uint32_t symbol_id);
  void lrvx_delta_book_encoder_reset_all(LrvxDeltaBookEncoderHandle handle);
  void lrvx_delta_book_encoder_encode(LrvxDeltaBookEncoderHandle handle, uint32_t symbol_id,
                                      const LrvxBookLevel* bids, size_t bid_count,
                                      const LrvxBookLevel* asks, size_t ask_count,
                                      uint8_t* out_is_delta, uint64_t* out_bid_count,
                                      uint64_t* out_ask_count);
  uint64_t lrvx_delta_book_encoder_copy_bids(LrvxDeltaBookEncoderHandle handle, LrvxBookLevel* out,
                                             uint64_t max_entries);
  uint64_t lrvx_delta_book_encoder_copy_asks(LrvxDeltaBookEncoderHandle handle, LrvxBookLevel* out,
                                             uint64_t max_entries);
  LrvxDeltaBookReplayerHandle lrvx_delta_book_replayer_create(void);
  void lrvx_delta_book_replayer_destroy(LrvxDeltaBookReplayerHandle handle);
  void lrvx_delta_book_replayer_reset(LrvxDeltaBookReplayerHandle handle, uint32_t symbol_id);
  void lrvx_delta_book_replayer_apply(LrvxDeltaBookReplayerHandle handle, uint8_t type,
                                      uint32_t symbol_id, const LrvxBookLevel* bids,
                                      size_t bid_count, const LrvxBookLevel* asks, size_t ask_count,
                                      uint64_t* out_bid_count, uint64_t* out_ask_count);
  uint64_t lrvx_delta_book_replayer_copy_bids(LrvxDeltaBookReplayerHandle handle,
                                              LrvxBookLevel* out, uint64_t max_entries);
  uint64_t lrvx_delta_book_replayer_copy_asks(LrvxDeltaBookReplayerHandle handle,
                                              LrvxBookLevel* out, uint64_t max_entries);

  // ============================================================
  // Dex Amount
  // ============================================================

  uint8_t lrvx_u256_roundtrip(const char* dec, char* out, size_t out_len);
  uint8_t lrvx_i256_roundtrip(const char* dec, char* out, size_t out_len);
  uint8_t lrvx_u256_from_hex(const char* hex, char* out, size_t out_len);
  uint8_t lrvx_u256_to_words(const char* dec, uint64_t* words);
  uint8_t lrvx_u256_from_words(const uint64_t* words, char* out, size_t out_len);

  // ============================================================
  // Execution
  // ============================================================

  LrvxExecutionListenerHandle lrvx_execution_listener_create(LrvxExecutionListenerCallbacks callbacks);
  LrvxExecutionListenerHandle lrvx_execution_listener_create_p(const LrvxExecutionListenerCallbacks* callbacks);
  void lrvx_execution_listener_destroy(LrvxExecutionListenerHandle listener);
  LrvxExecutorHandle lrvx_executor_create(LrvxExecutorCallbacks callbacks);
  LrvxExecutorHandle lrvx_executor_create_p(const LrvxExecutorCallbacks* callbacks);
  void lrvx_executor_destroy(LrvxExecutorHandle executor);
  void lrvx_executor_get_capabilities(LrvxExecutorHandle executor,
                                      LrvxExchangeCapabilities* caps_out);
  void lrvx_live_engine_set_executor(LrvxLiveEngineHandle engine, LrvxExecutorHandle executor);
  void lrvx_runner_set_executor(LrvxRunnerHandle runner, LrvxExecutorHandle executor);
  void lrvx_backtest_runner_add_execution_listener(LrvxBacktestRunnerHandle runner,
                                                   LrvxExecutionListenerHandle listener);
  void lrvx_backtest_runner_set_executor(LrvxBacktestRunnerHandle runner,
                                         LrvxExecutorHandle executor);

  // ============================================================
  // Execution Algos
  // ============================================================

  LrvxExecAlgoHandle lrvx_exec_twap_create(double target_qty, uint8_t side, uint32_t symbol,
                                           uint8_t type, double limit_price, int64_t duration_ns,
                                           uint32_t slice_count, int64_t start_time_ns);
  LrvxExecAlgoHandle lrvx_exec_vwap_create(double target_qty, uint8_t side, uint32_t symbol,
                                           uint8_t type, double limit_price,
                                           const int64_t* volume_curve_ts,
                                           const double* volume_curve_vol, size_t n);
  LrvxExecAlgoHandle lrvx_exec_iceberg_create(double target_qty, uint8_t side, uint32_t symbol,
                                              uint8_t type, double limit_price, double visible_qty);
  LrvxExecAlgoHandle lrvx_exec_pov_create(double target_qty, uint8_t side, uint32_t symbol,
                                          uint8_t type, double limit_price,
                                          double participation_rate, double min_slice_qty);
  void lrvx_exec_destroy(LrvxExecAlgoHandle handle);
  void lrvx_exec_step(LrvxExecAlgoHandle handle, int64_t now_ns);
  void lrvx_exec_report_fill(LrvxExecAlgoHandle handle, double qty);
  void lrvx_exec_observe_volume(LrvxExecAlgoHandle handle, double qty);
  size_t lrvx_exec_pending_count(LrvxExecAlgoHandle handle);
  uint8_t lrvx_exec_pending_at(LrvxExecAlgoHandle handle, size_t index, LrvxExecChildOrder* out);
  void lrvx_exec_clear_pending(LrvxExecAlgoHandle handle);
  double lrvx_exec_target_qty(LrvxExecAlgoHandle handle);
  double lrvx_exec_submitted_qty(LrvxExecAlgoHandle handle);
  double lrvx_exec_filled_qty(LrvxExecAlgoHandle handle);
  double lrvx_exec_remaining_qty(LrvxExecAlgoHandle handle);
  uint8_t lrvx_exec_is_done(LrvxExecAlgoHandle handle);

  // ============================================================
  // Executor Fill
  // ============================================================

  uint32_t lrvx_simulated_executor_get_fills(LrvxSimulatedExecutorHandle executor,
                                             LrvxFill* fills_out, uint32_t max_fills);

  // ============================================================
  // Fee Schedule
  // ============================================================

  LrvxFeeScheduleHandle lrvx_fee_schedule_create(void);
  void lrvx_fee_schedule_destroy(LrvxFeeScheduleHandle h);
  void lrvx_fee_schedule_add_tier(LrvxFeeScheduleHandle h, double min_notional_30d, double maker_bps,
                                  double taker_bps);
  int lrvx_fee_schedule_load_profile(LrvxFeeScheduleHandle h, const char* profile_name);
  void lrvx_fee_schedule_record_fill(LrvxFeeScheduleHandle h, int64_t ts_ns, double notional);
  double lrvx_fee_schedule_fee_for(LrvxFeeScheduleHandle h, int64_t ts_ns, double notional,
                                   uint8_t is_maker);
  uint32_t lrvx_fee_schedule_current_tier(LrvxFeeScheduleHandle h);
  double lrvx_fee_schedule_rolling_notional(LrvxFeeScheduleHandle h);
  uint32_t lrvx_fee_schedule_tier_transitions(LrvxFeeScheduleHandle h, int64_t* out_buf,
                                              uint32_t max_events);
  void lrvx_fee_schedule_reset_rolling(LrvxFeeScheduleHandle h);
  void lrvx_fee_schedule_bind_account(LrvxFeeScheduleHandle h, LrvxAccountHandle account);
  void lrvx_fee_schedule_clear_account_binding(LrvxFeeScheduleHandle h);

  // ============================================================
  // Feed Clock
  // ============================================================

  LrvxFeedClockHandle lrvx_feed_clock_create(const uint32_t* symbols, uint32_t symbol_count,
                                             uint8_t policy, int64_t timeout_ms,
                                             uint32_t leader_symbol, int64_t staleness_budget_ms);
  void lrvx_feed_clock_destroy(LrvxFeedClockHandle h);
  uint32_t lrvx_feed_clock_symbol_count(LrvxFeedClockHandle h);
  uint32_t lrvx_feed_clock_symbol_at(LrvxFeedClockHandle h, uint32_t index);
  uint8_t lrvx_feed_clock_tick(LrvxFeedClockHandle h, int64_t ts_ns, uint32_t symbol);
  uint8_t lrvx_feed_clock_last_fired(LrvxFeedClockHandle h);
  uint32_t lrvx_feed_clock_last_triggered_by(LrvxFeedClockHandle h);
  int64_t lrvx_feed_clock_last_seen_at(LrvxFeedClockHandle h, uint32_t index);
  int64_t lrvx_feed_clock_staleness_at(LrvxFeedClockHandle h, uint32_t index);
  void lrvx_feed_clock_reset(LrvxFeedClockHandle h);

  // ============================================================
  // Lrvxliveengine Disruptor
  // ============================================================

  LrvxLiveEngineHandle lrvx_live_engine_create(LrvxRegistryHandle registry);
  void lrvx_live_engine_destroy(LrvxLiveEngineHandle engine);
  void lrvx_live_engine_add_strategy(LrvxLiveEngineHandle engine, LrvxStrategyHandle strategy,
                                     LrvxOnSignalCallback on_signal, void* user_data);
  void lrvx_live_engine_start(LrvxLiveEngineHandle engine);
  void lrvx_live_engine_stop(LrvxLiveEngineHandle engine);
  void lrvx_live_engine_publish_trade(LrvxLiveEngineHandle engine, uint32_t symbol, double price,
                                      double qty, uint8_t is_buy, int64_t exchange_ts_ns);
  void lrvx_live_engine_publish_book_snapshot(LrvxLiveEngineHandle engine, uint32_t symbol,
                                              const double* bid_prices, const double* bid_qtys,
                                              uint32_t n_bids, const double* ask_prices,
                                              const double* ask_qtys, uint32_t n_asks,
                                              int64_t exchange_ts_ns);
  void lrvx_live_engine_publish_bar(LrvxLiveEngineHandle engine, uint32_t symbol, uint8_t bar_type,
                                    uint64_t bar_type_param, double open, double high, double low,
                                    double close, double volume, double buy_volume,
                                    int64_t start_time_ns, int64_t end_time_ns, uint8_t close_reason);

  // ============================================================
  // Lrvxrun
  // ============================================================

  LrvxRunRecorderHandle lrvx_run_recorder_create(const char* path, const char* strategy_id,
                                                 const char* strategy_hash, int64_t run_started_ns);
  void lrvx_run_recorder_destroy(LrvxRunRecorderHandle handle);
  void lrvx_run_recorder_add_tape_ref(LrvxRunRecorderHandle handle, const char* path,
                                      const char* content_hash, int64_t first_event_ns,
                                      int64_t last_event_ns);
  void lrvx_run_recorder_set_run_ended_ns(LrvxRunRecorderHandle handle, int64_t ns);
  void lrvx_run_recorder_write_signal(LrvxRunRecorderHandle handle, int64_t run_ts_ns,
                                      int64_t feed_ts_ns, uint32_t signal_id, uint32_t flags,
                                      int64_t strength_raw, const char* name, size_t name_len,
                                      const uint32_t* symbol_ids, size_t symbol_count,
                                      const uint8_t* payload, size_t payload_len);
  void lrvx_run_recorder_write_order_event(LrvxRunRecorderHandle handle, int64_t run_ts_ns,
                                           int64_t feed_ts_ns, uint64_t order_id,
                                           uint64_t parent_signal_id, int64_t price_raw,
                                           int64_t qty_raw, uint32_t symbol_id, uint8_t event_kind,
                                           uint8_t side, uint8_t order_type, uint32_t flags,
                                           const char* reason, size_t reason_len);
  void lrvx_run_recorder_write_fill(LrvxRunRecorderHandle handle, int64_t run_ts_ns,
                                    int64_t feed_ts_ns, uint64_t order_id, uint64_t fill_id,
                                    int64_t price_raw, int64_t qty_raw, int64_t fee_raw,
                                    uint32_t symbol_id, uint8_t side, uint8_t liquidity);
  void lrvx_run_recorder_close(LrvxRunRecorderHandle handle);
  /* Owned, but the partner is lrvx_run_reader_close rather than a
   * _destroy: the reader loads the whole trace up front, and closing it
   * frees both the file and those buffers. NULL when the path cannot be
   * read. */
  LrvxRunReaderHandle lrvx_run_reader_open(const char* path);
  void lrvx_run_reader_close(LrvxRunReaderHandle handle);
  uint64_t lrvx_run_reader_strategy_id(LrvxRunReaderHandle handle, char* out, uint64_t max_bytes);
  uint64_t lrvx_run_reader_strategy_hash(LrvxRunReaderHandle handle, char* out, uint64_t max_bytes);
  int64_t lrvx_run_reader_run_started_ns(LrvxRunReaderHandle handle);
  int64_t lrvx_run_reader_run_ended_ns(LrvxRunReaderHandle handle);
  uint64_t lrvx_run_reader_tape_ref_count(LrvxRunReaderHandle handle);
  uint64_t lrvx_run_reader_tape_ref_path(LrvxRunReaderHandle handle, uint64_t index, char* out,
                                         uint64_t max_bytes);
  uint64_t lrvx_run_reader_signal_count(LrvxRunReaderHandle handle);
  uint64_t lrvx_run_reader_order_event_count(LrvxRunReaderHandle handle);
  uint64_t lrvx_run_reader_fill_count(LrvxRunReaderHandle handle);
  void lrvx_run_reader_signal_header(LrvxRunReaderHandle handle, uint64_t index,
                                     int64_t* out_run_ts, int64_t* out_feed_ts,
                                     uint32_t* out_signal_id, uint32_t* out_flags,
                                     int64_t* out_strength_raw, uint64_t* out_name_len,
                                     uint64_t* out_symbol_count, uint64_t* out_payload_len);
  uint64_t lrvx_run_reader_signal_name(LrvxRunReaderHandle handle, uint64_t index, char* out,
                                       uint64_t max_bytes);
  uint64_t lrvx_run_reader_signal_symbol_ids(LrvxRunReaderHandle handle, uint64_t index,
                                             uint32_t* out, uint64_t max_entries);
  uint64_t lrvx_run_reader_signal_payload(LrvxRunReaderHandle handle, uint64_t index, uint8_t* out,
                                          uint64_t max_bytes);
  void lrvx_run_reader_order_event_header(LrvxRunReaderHandle handle, uint64_t index,
                                          int64_t* out_run_ts, int64_t* out_feed_ts,
                                          uint64_t* out_order_id, uint64_t* out_parent_signal_id,
                                          int64_t* out_price_raw, int64_t* out_qty_raw,
                                          uint32_t* out_symbol_id, uint8_t* out_event_kind,
                                          uint8_t* out_side, uint8_t* out_order_type,
                                          uint32_t* out_flags, uint64_t* out_reason_len);
  uint64_t lrvx_run_reader_order_event_reason(LrvxRunReaderHandle handle, uint64_t index, char* out,
                                              uint64_t max_bytes);
  void lrvx_run_reader_fill(LrvxRunReaderHandle handle, uint64_t index, int64_t* out_run_ts,
                            int64_t* out_feed_ts, uint64_t* out_order_id, uint64_t* out_fill_id,
                            int64_t* out_price_raw, int64_t* out_qty_raw, int64_t* out_fee_raw,
                            uint32_t* out_symbol_id, uint8_t* out_side, uint8_t* out_liquidity);

  // ============================================================
  // Footprint Bar
  // ============================================================

  LrvxFootprintHandle lrvx_footprint_create(double tick_size);
  void lrvx_footprint_destroy(LrvxFootprintHandle footprint);
  void lrvx_footprint_add_trade(LrvxFootprintHandle footprint, double price, double quantity,
                                uint8_t is_buy);
  double lrvx_footprint_total_delta(LrvxFootprintHandle footprint);
  double lrvx_footprint_total_volume(LrvxFootprintHandle footprint);
  uint32_t lrvx_footprint_num_levels(LrvxFootprintHandle footprint);
  void lrvx_footprint_clear(LrvxFootprintHandle footprint);

  // ============================================================
  // Funding Schedule
  // ============================================================

  LrvxFundingScheduleHandle lrvx_funding_schedule_create(void);
  void lrvx_funding_schedule_destroy(LrvxFundingScheduleHandle h);
  void lrvx_funding_schedule_set_constant(LrvxFundingScheduleHandle h, int64_t interval_ns,
                                          double rate);
  void lrvx_funding_schedule_set_tape(LrvxFundingScheduleHandle h, const int64_t* timestamps_ns,
                                      const double* rates, uint32_t n_events);
  void lrvx_funding_schedule_set_tape_by_symbol(LrvxFundingScheduleHandle h,
                                                const int64_t* timestamps_ns,
                                                const uint32_t* symbols, const double* rates,
                                                uint32_t n_entries);
  uint8_t lrvx_funding_schedule_load_tape(LrvxFundingScheduleHandle h, const char* path);
  int lrvx_funding_schedule_load_profile(LrvxFundingScheduleHandle h, const char* profile_name);
  void lrvx_funding_schedule_set_constant_rate(LrvxFundingScheduleHandle h, double rate);
  void lrvx_funding_schedule_reset(LrvxFundingScheduleHandle h);
  uint32_t lrvx_funding_schedule_tick(LrvxFundingScheduleHandle h, int64_t now_ns,
                                      const uint32_t* symbols, const double* positions,
                                      const double* mark_prices, uint32_t n_symbols,
                                      double* out_buf, uint32_t max_events);

  // ============================================================
  // Grid Search
  // ============================================================

  LrvxGridSearchHandle lrvx_grid_search_create(void);
  void lrvx_grid_search_destroy(LrvxGridSearchHandle gs);
  void lrvx_grid_search_add_axis(LrvxGridSearchHandle gs, const double* values, uint32_t num_values);
  uint64_t lrvx_grid_search_total(LrvxGridSearchHandle gs);
  uint32_t lrvx_grid_search_params_for_index(LrvxGridSearchHandle gs, uint64_t index,
                                             double* params_out, uint32_t max_params);
  uint64_t lrvx_grid_search_run(LrvxGridSearchHandle gs, LrvxGridSearchFactoryFn factory,
                                void* user_data, LrvxBacktestStats* stats_out,
                                uint32_t max_results);

  // ============================================================
  // Heatmap
  // ============================================================

  uint64_t lrvx_render_heatmap_html(const LrvxHeatmapData* data, char* out_buf, uint64_t max_size);

  // ============================================================
  // Heikin Ashi
  // ============================================================

  uint32_t lrvx_aggregate_heikin_ashi_bars(const int64_t* timestamps, const double* prices,
                                           const double* quantities, const uint8_t* is_buy,
                                           size_t len, double interval_seconds, LrvxBar* bars_out,
                                           uint32_t max_bars);

  // ============================================================
  // Indicator Functions
  // ============================================================

  void lrvx_indicator_ema(const double* input, size_t len, size_t period, double* output);
  void lrvx_indicator_sma(const double* input, size_t len, size_t period, double* output);
  void lrvx_indicator_rsi(const double* input, size_t len, size_t period, double* output);
  void lrvx_indicator_atr(const double* high, const double* low, const double* close, size_t len,
                          size_t period, double* output);
  void lrvx_indicator_macd(const double* input, size_t len, size_t fast_period, size_t slow_period,
                           size_t signal_period, double* macd_out, double* signal_out,
                           double* hist_out);
  void lrvx_indicator_bollinger(const double* input, size_t len, size_t period, double multiplier,
                                double* upper, double* middle, double* lower);
  void lrvx_indicator_rma(const double* input, size_t len, size_t period, double* output);
  void lrvx_indicator_dema(const double* input, size_t len, size_t period, double* output);
  void lrvx_indicator_tema(const double* input, size_t len, size_t period, double* output);
  void lrvx_indicator_kama(const double* input, size_t len, size_t period, size_t fast, size_t slow,
                           double* output);
  void lrvx_indicator_slope(const double* input, size_t len, size_t length, double* output);
  void lrvx_indicator_adx(const double* high, const double* low, const double* close, size_t len,
                          size_t period, double* adx_out, double* plus_di_out,
                          double* minus_di_out);
  void lrvx_indicator_cci(const double* high, const double* low, const double* close, size_t len,
                          size_t period, double* output);
  void lrvx_indicator_stochastic(const double* high, const double* low, const double* close,
                                 size_t len, size_t k_period, size_t d_period, double* k_out,
                                 double* d_out);
  void lrvx_indicator_chop(const double* high, const double* low, const double* close, size_t len,
                           size_t period, double* output);
  void lrvx_indicator_obv(const double* close, const double* volume, size_t len, double* output);
  void lrvx_indicator_vwap(const double* close, const double* volume, size_t len, size_t window,
                           double* output);
  void lrvx_indicator_cvd(const double* open, const double* high, const double* low,
                          const double* close, const double* volume, size_t len, double* output);
  void lrvx_indicator_skewness(const double* input, size_t len, size_t period, double* output);
  void lrvx_indicator_kurtosis(const double* input, size_t len, size_t period, double* output);
  void lrvx_indicator_parkinson_vol(const double* high, const double* low, size_t len,
                                    size_t period, double* output);
  void lrvx_indicator_rogers_satchell_vol(const double* open, const double* high,
                                          const double* low, const double* close, size_t len,
                                          size_t period, double* output);
  void lrvx_indicator_rolling_zscore(const double* input, size_t len, size_t period,
                                     double* output);
  void lrvx_indicator_shannon_entropy(const double* input, size_t len, size_t period, size_t bins,
                                      double* output);
  void lrvx_indicator_correlation(const double* x, const double* y, size_t len, size_t period,
                                  double* output);
  void lrvx_indicator_adf(const double* input, size_t len, size_t max_lag, const char* regression,
                          double* test_stat_out, double* p_value_out, size_t* used_lag_out);
  void lrvx_indicator_autocorrelation(const double* input, size_t len, size_t window, size_t lag,
                                      double* output);

  // ============================================================
  // Indicatorgraph
  // ============================================================

  LrvxIndicatorGraphHandle lrvx_indicator_graph_create(void);
  void lrvx_indicator_graph_destroy(LrvxIndicatorGraphHandle g);
  void lrvx_indicator_graph_set_bars(LrvxIndicatorGraphHandle g, uint32_t symbol,
                                     const double* close, const double* high, const double* low,
                                     const double* volume, size_t len);
  void lrvx_indicator_graph_add_node(LrvxIndicatorGraphHandle g, const char* name,
                                     const char* const* deps, size_t num_deps, LrvxGraphNodeFn fn,
                                     void* user_data);
  const double* lrvx_indicator_graph_require(LrvxIndicatorGraphHandle g, uint32_t symbol,
                                             const char* name, size_t* len_out);
  const double* lrvx_indicator_graph_get(LrvxIndicatorGraphHandle g, uint32_t symbol,
                                         const char* name, size_t* len_out);
  const double* lrvx_indicator_graph_close(LrvxIndicatorGraphHandle g, uint32_t symbol,
                                           size_t* len_out);
  const double* lrvx_indicator_graph_high(LrvxIndicatorGraphHandle g, uint32_t symbol,
                                          size_t* len_out);
  const double* lrvx_indicator_graph_low(LrvxIndicatorGraphHandle g, uint32_t symbol,
                                         size_t* len_out);
  const double* lrvx_indicator_graph_volume(LrvxIndicatorGraphHandle g, uint32_t symbol,
                                            size_t* len_out);
  void lrvx_indicator_graph_invalidate(LrvxIndicatorGraphHandle g, uint32_t symbol);
  void lrvx_indicator_graph_invalidate_all(LrvxIndicatorGraphHandle g);
  void lrvx_indicator_graph_step(LrvxIndicatorGraphHandle g, uint32_t symbol, double open,
                                 double high, double low, double close, double volume);
  double lrvx_indicator_graph_current(LrvxIndicatorGraphHandle g, uint32_t symbol, const char* name);
  uint32_t lrvx_indicator_graph_bar_count(LrvxIndicatorGraphHandle g, uint32_t symbol);
  void lrvx_indicator_graph_reset(LrvxIndicatorGraphHandle g, uint32_t symbol);
  void lrvx_indicator_graph_reset_all(LrvxIndicatorGraphHandle g);
  LrvxStreamingGraphHandle lrvx_streaming_graph_create(void);
  void lrvx_streaming_graph_destroy(LrvxStreamingGraphHandle sg);
  void lrvx_streaming_graph_add_node(LrvxStreamingGraphHandle sg, const char* name,
                                     const char* const* deps, size_t num_deps, LrvxGraphNodeFn fn,
                                     void* user_data);
  void lrvx_streaming_graph_step(LrvxStreamingGraphHandle sg, uint32_t symbol, double open,
                                 double high, double low, double close, double volume);
  double lrvx_streaming_graph_current(LrvxStreamingGraphHandle sg, uint32_t symbol,
                                      const char* name);
  uint32_t lrvx_streaming_graph_bar_count(LrvxStreamingGraphHandle sg, uint32_t symbol);
  void lrvx_streaming_graph_reset(LrvxStreamingGraphHandle sg, uint32_t symbol);
  void lrvx_streaming_graph_reset_all(LrvxStreamingGraphHandle sg);
  const double* lrvx_streaming_graph_close(LrvxStreamingGraphHandle sg, uint32_t symbol,
                                           size_t* len_out);
  const double* lrvx_streaming_graph_high(LrvxStreamingGraphHandle sg, uint32_t symbol,
                                          size_t* len_out);
  const double* lrvx_streaming_graph_low(LrvxStreamingGraphHandle sg, uint32_t symbol,
                                         size_t* len_out);
  const double* lrvx_streaming_graph_volume(LrvxStreamingGraphHandle sg, uint32_t symbol,
                                            size_t* len_out);

  // ============================================================
  // L3 Order
  // ============================================================

  LrvxL3BookHandle lrvx_l3_book_create(void);
  void lrvx_l3_book_destroy(LrvxL3BookHandle book);
  int32_t lrvx_l3_book_add_order(LrvxL3BookHandle book, uint64_t order_id, double price,
                                 double quantity, uint8_t side);
  int32_t lrvx_l3_book_remove_order(LrvxL3BookHandle book, uint64_t order_id);
  int32_t lrvx_l3_book_modify_order(LrvxL3BookHandle book, uint64_t order_id, double new_qty);
  uint8_t lrvx_l3_book_best_bid(LrvxL3BookHandle book, double* price_out);
  uint8_t lrvx_l3_book_best_ask(LrvxL3BookHandle book, double* price_out);
  double lrvx_l3_book_bid_at_price(LrvxL3BookHandle book, double price);
  double lrvx_l3_book_ask_at_price(LrvxL3BookHandle book, double price);

  // ============================================================
  // Latency Models
  // ============================================================

  LrvxLatencyModelHandle lrvx_latency_constant_create(int64_t feed_ns, int64_t order_ns,
                                                      int64_t fill_ns);
  LrvxLatencyModelHandle lrvx_latency_gaussian_create(double feed_mean_ns, double feed_stddev_ns,
                                                      double order_mean_ns, double order_stddev_ns,
                                                      double fill_mean_ns, double fill_stddev_ns,
                                                      uint64_t seed);
  LrvxLatencyModelHandle lrvx_latency_exponential_create(double feed_mean_ns, double order_mean_ns,
                                                         double fill_mean_ns, uint64_t seed);
  LrvxLatencyModelHandle lrvx_latency_empirical_create(const int64_t* feed_samples,
                                                       size_t feed_count,
                                                       const int64_t* order_samples,
                                                       size_t order_count,
                                                       const int64_t* fill_samples,
                                                       size_t fill_count, uint64_t seed);
  void lrvx_latency_destroy(LrvxLatencyModelHandle model);
  int64_t lrvx_latency_feed_delay(LrvxLatencyModelHandle model);
  int64_t lrvx_latency_order_delay(LrvxLatencyModelHandle model);
  int64_t lrvx_latency_fill_delay(LrvxLatencyModelHandle model);
  void lrvx_latency_sample(LrvxLatencyModelHandle model, LrvxLatencySample* out);
  void lrvx_latency_reset(LrvxLatencyModelHandle model, uint64_t seed);

  // ============================================================
  // Liquidation Engine
  // ============================================================

  LrvxLiquidationEngineHandle lrvx_liquidation_engine_create(void);
  void lrvx_liquidation_engine_destroy(LrvxLiquidationEngineHandle h);
  void lrvx_liquidation_engine_add_tier(LrvxLiquidationEngineHandle h, double min_notional,
                                        double mm_fraction);
  void lrvx_liquidation_engine_set_insurance_fund_capital(LrvxLiquidationEngineHandle h,
                                                          double capital);
  double lrvx_liquidation_engine_insurance_fund_balance(LrvxLiquidationEngineHandle h);
  void lrvx_liquidation_engine_set_adl_enabled(LrvxLiquidationEngineHandle h, uint8_t enabled);
  void lrvx_liquidation_engine_set_adl_ranking(LrvxLiquidationEngineHandle h, uint8_t ranking);
  uint8_t lrvx_liquidation_engine_adl_ranking(LrvxLiquidationEngineHandle h);
  void lrvx_liquidation_engine_set_liquidation_slippage_bps(LrvxLiquidationEngineHandle h,
                                                            double bps);
  void lrvx_liquidation_engine_open_position(LrvxLiquidationEngineHandle h, uint64_t account_id,
                                             uint32_t symbol, double quantity, double entry_price,
                                             double equity);
  void lrvx_liquidation_engine_close_position(LrvxLiquidationEngineHandle h, uint64_t account_id,
                                              uint32_t symbol);
  uint32_t lrvx_liquidation_engine_on_mark(LrvxLiquidationEngineHandle h, uint32_t symbol,
                                           double mark_price);
  uint32_t lrvx_liquidation_engine_on_marks(LrvxLiquidationEngineHandle h, uint32_t n,
                                            const uint32_t* symbols, const double* prices,
                                            int64_t ts_ns);
  uint64_t lrvx_liquidation_engine_liquidations_count(LrvxLiquidationEngineHandle h);
  uint64_t lrvx_liquidation_engine_insurance_payments_count(LrvxLiquidationEngineHandle h);
  uint64_t lrvx_liquidation_engine_adl_closeouts_count(LrvxLiquidationEngineHandle h);
  void lrvx_liquidation_engine_load_profile(LrvxLiquidationEngineHandle h, uint8_t profile);
  void lrvx_liquidation_engine_set_executor(LrvxLiquidationEngineHandle h,
                                            LrvxSimulatedExecutorHandle executor);
  uint32_t lrvx_liquidation_engine_deficits_paid_by_fund_size(LrvxLiquidationEngineHandle h);
  uint32_t lrvx_liquidation_engine_deficits_paid_by_fund_copy(LrvxLiquidationEngineHandle h,
                                                              double* out, uint32_t max);
  uint32_t lrvx_liquidation_engine_deficits_paid_by_adl_size(LrvxLiquidationEngineHandle h);
  uint32_t lrvx_liquidation_engine_deficits_paid_by_adl_copy(LrvxLiquidationEngineHandle h,
                                                             double* out, uint32_t max);
  uint32_t lrvx_liquidation_engine_cascade_sizes_size(LrvxLiquidationEngineHandle h);
  uint32_t lrvx_liquidation_engine_cascade_sizes_copy(LrvxLiquidationEngineHandle h, uint32_t* out,
                                                      uint32_t max);
  uint32_t lrvx_liquidation_engine_fund_balance_history_size(LrvxLiquidationEngineHandle h);
  uint32_t lrvx_liquidation_engine_fund_balance_history_copy(LrvxLiquidationEngineHandle h,
                                                             double* out, uint32_t max);
  uint64_t lrvx_liquidation_engine_ticks_to_first_adl(LrvxLiquidationEngineHandle h);
  void lrvx_liquidation_engine_reset_stats(LrvxLiquidationEngineHandle h);
  void lrvx_liquidation_engine_set_mark_impact_model(LrvxLiquidationEngineHandle h, uint8_t model,
                                                     double weight);
  uint8_t lrvx_liquidation_engine_mark_impact_model(LrvxLiquidationEngineHandle h);
  double lrvx_liquidation_engine_mark_impact_weight(LrvxLiquidationEngineHandle h);
  void lrvx_liquidation_engine_set_max_cascade_depth(LrvxLiquidationEngineHandle h, uint32_t depth);
  uint32_t lrvx_liquidation_engine_max_cascade_depth(LrvxLiquidationEngineHandle h);
  void lrvx_liquidation_engine_attach_account(LrvxLiquidationEngineHandle h,
                                              LrvxAccountHandle account);
  void lrvx_liquidation_engine_detach_account(LrvxLiquidationEngineHandle h, uint64_t account_id);

  // ============================================================
  // Live Queue Position
  // ============================================================

  LrvxLiveQueuePositionHandle lrvx_live_queue_position_create(void);
  void lrvx_live_queue_position_destroy(LrvxLiveQueuePositionHandle h);
  void lrvx_live_queue_position_set_confidence_half_life_ns(LrvxLiveQueuePositionHandle h,
                                                            int64_t half_life_ns);
  void lrvx_live_queue_position_set_shrink_factor(LrvxLiveQueuePositionHandle h, double factor);
  void lrvx_live_queue_position_on_order_placed(LrvxLiveQueuePositionHandle h, uint32_t symbol,
                                                uint8_t side, int64_t price_raw, uint64_t order_id,
                                                int64_t order_qty_raw, int64_t level_qty_raw,
                                                int64_t ts_ns);
  void lrvx_live_queue_position_on_order_cancelled(LrvxLiveQueuePositionHandle h, uint64_t order_id,
                                                   int64_t ts_ns);
  void lrvx_live_queue_position_on_order_filled(LrvxLiveQueuePositionHandle h, uint64_t order_id,
                                                int64_t cumulative_fill_raw, int64_t ts_ns);
  void lrvx_live_queue_position_on_trade(LrvxLiveQueuePositionHandle h, uint32_t symbol,
                                         int64_t price_raw, int64_t qty_raw, int64_t ts_ns);
  void lrvx_live_queue_position_on_trade_with_flag(LrvxLiveQueuePositionHandle h, uint32_t symbol,
                                                   int64_t price_raw, int64_t qty_raw, int64_t ts_ns,
                                                   uint8_t is_hidden);
  void lrvx_live_queue_position_set_hidden_order_policy(LrvxLiveQueuePositionHandle h,
                                                        uint8_t policy);
  void lrvx_live_queue_position_on_level_update(LrvxLiveQueuePositionHandle h, uint32_t symbol,
                                                uint8_t side, int64_t price_raw, int64_t new_qty_raw,
                                                int64_t ts_ns);
  uint8_t lrvx_live_queue_position_snapshot(LrvxLiveQueuePositionHandle h, uint64_t order_id,
                                            int64_t now_ns, int64_t* out_slots);
  uint32_t lrvx_live_queue_position_tracked_count(LrvxLiveQueuePositionHandle h);

  // ============================================================
  // Logger
  // ============================================================

  void lrvx_set_log_callback(LrvxLogCallback callback, void* user_data);

  // ============================================================
  // Market Profile
  // ============================================================

  LrvxMarketProfileHandle lrvx_market_profile_create(double tick_size, uint32_t period_minutes,
                                                     int64_t session_start_ns);
  void lrvx_market_profile_destroy(LrvxMarketProfileHandle profile);
  void lrvx_market_profile_add_trade(LrvxMarketProfileHandle profile, int64_t timestamp_ns,
                                     double price, double qty, uint8_t is_buy);
  double lrvx_market_profile_poc(LrvxMarketProfileHandle profile);
  double lrvx_market_profile_vah(LrvxMarketProfileHandle profile);
  double lrvx_market_profile_val(LrvxMarketProfileHandle profile);
  double lrvx_market_profile_ib_high(LrvxMarketProfileHandle profile);
  double lrvx_market_profile_ib_low(LrvxMarketProfileHandle profile);
  uint8_t lrvx_market_profile_is_poor_high(LrvxMarketProfileHandle profile);
  uint8_t lrvx_market_profile_is_poor_low(LrvxMarketProfileHandle profile);
  uint32_t lrvx_market_profile_num_levels(LrvxMarketProfileHandle profile);
  void lrvx_market_profile_clear(LrvxMarketProfileHandle profile);

  // ============================================================
  // Merged Tape Reader
  // ============================================================

  LrvxMergedTapeReaderHandle lrvx_merged_tape_reader_create(const char* const* paths,
                                                            uint32_t n_paths, int64_t from_ns,
                                                            int64_t to_ns,
                                                            const uint32_t* symbol_filter,
                                                            uint32_t n_filter);
  void lrvx_merged_tape_reader_destroy(LrvxMergedTapeReaderHandle reader);
  uint32_t lrvx_merged_tape_reader_symbol_count(LrvxMergedTapeReaderHandle reader);
  uint32_t lrvx_merged_tape_reader_get_symbols(LrvxMergedTapeReaderHandle reader,
                                               LrvxMergedSymbol* out, uint32_t max);
  uint32_t lrvx_merged_tape_reader_tape_count(LrvxMergedTapeReaderHandle reader);
  uint32_t lrvx_merged_tape_reader_get_tape_stats(LrvxMergedTapeReaderHandle reader,
                                                  LrvxMergedTapeStats* out, uint32_t max);
  void lrvx_merged_tape_reader_time_range(LrvxMergedTapeReaderHandle reader,
                                          int64_t* min_first_ns_out, int64_t* max_last_ns_out);
  uint64_t lrvx_merged_tape_reader_count_trades(LrvxMergedTapeReaderHandle reader);
  uint64_t lrvx_merged_tape_reader_read_trades(LrvxMergedTapeReaderHandle reader,
                                               LrvxTradeRecord* trades_out, uint64_t max_trades);
  uint64_t lrvx_merged_tape_reader_count_books(LrvxMergedTapeReaderHandle reader,
                                               uint64_t* total_levels_out);
  uint64_t lrvx_merged_tape_reader_read_books(LrvxMergedTapeReaderHandle reader,
                                              LrvxBookUpdateHeader* headers_out,
                                              uint64_t max_events, LrvxLevel* levels_out,
                                              uint64_t max_levels);

  // ============================================================
  // Metrics
  // ============================================================

  LrvxPnLTrackerHandle lrvx_pnl_tracker_create(LrvxPnLTrackerCallbacks callbacks);
  LrvxPnLTrackerHandle lrvx_pnl_tracker_create_p(const LrvxPnLTrackerCallbacks* callbacks);
  void lrvx_pnl_tracker_destroy(LrvxPnLTrackerHandle tracker);
  void lrvx_live_engine_set_pnl_tracker(LrvxLiveEngineHandle engine, LrvxPnLTrackerHandle tracker);
  void lrvx_runner_set_pnl_tracker(LrvxRunnerHandle runner, LrvxPnLTrackerHandle tracker);

  // ============================================================
  // Multi Tf Helpers
  // ============================================================

  uint8_t lrvx_strategy_last_closed_bar(LrvxStrategyHandle s, uint32_t symbol, uint8_t bar_type,
                                        uint64_t param, LrvxBar* out);
  uint32_t lrvx_strategy_last_n_closed_bars(LrvxStrategyHandle s, uint32_t symbol, uint8_t bar_type,
                                            uint64_t param, LrvxBar* bars_out, uint32_t max_bars);
  uint32_t lrvx_strategy_get_bar_ring_capacity(LrvxStrategyHandle s);
  void lrvx_strategy_set_bar_ring_capacity(LrvxStrategyHandle s, uint32_t capacity);

  // ============================================================
  // Order Book
  // ============================================================

  LrvxBookHandle lrvx_book_create(double tick_size);
  void lrvx_book_destroy(LrvxBookHandle book);
  void lrvx_book_apply_snapshot(LrvxBookHandle book, const double* bid_prices,
                                const double* bid_qtys, size_t bid_len, const double* ask_prices,
                                const double* ask_qtys, size_t ask_len);
  void lrvx_book_apply_delta(LrvxBookHandle book, const double* bid_prices, const double* bid_qtys,
                             size_t bid_len, const double* ask_prices, const double* ask_qtys,
                             size_t ask_len);
  uint8_t lrvx_book_best_bid(LrvxBookHandle book, double* price_out);
  uint8_t lrvx_book_best_ask(LrvxBookHandle book, double* price_out);
  uint8_t lrvx_book_mid(LrvxBookHandle book, double* price_out);
  uint8_t lrvx_book_spread(LrvxBookHandle book, double* spread_out);
  double lrvx_book_bid_at_price(LrvxBookHandle book, double price);
  double lrvx_book_ask_at_price(LrvxBookHandle book, double price);
  uint8_t lrvx_book_is_crossed(LrvxBookHandle book);
  void lrvx_book_clear(LrvxBookHandle book);
  uint32_t lrvx_book_get_bids(LrvxBookHandle book, double* prices_out, double* qtys_out,
                              uint32_t max_levels);
  uint32_t lrvx_book_get_asks(LrvxBookHandle book, double* prices_out, double* qtys_out,
                              uint32_t max_levels);

  // ============================================================
  // Order Group
  // ============================================================

  LrvxOrderGroupHandle lrvx_order_group_create(uint64_t parent_signal_id, uint8_t policy);
  void lrvx_order_group_destroy(LrvxOrderGroupHandle h);
  uint32_t lrvx_order_group_add_market_leg(LrvxOrderGroupHandle h, uint32_t symbol, uint8_t side,
                                           int64_t qty_raw);
  uint32_t lrvx_order_group_add_limit_leg(LrvxOrderGroupHandle h, uint32_t symbol, uint8_t side,
                                          int64_t price_raw, int64_t qty_raw);
  uint32_t lrvx_order_group_leg_count(LrvxOrderGroupHandle h);
  uint8_t lrvx_order_group_leg_state(LrvxOrderGroupHandle h, uint32_t leg_index);
  int64_t lrvx_order_group_leg_filled_raw(LrvxOrderGroupHandle h, uint32_t leg_index);
  uint64_t lrvx_order_group_leg_order_id(LrvxOrderGroupHandle h, uint32_t leg_index);
  void lrvx_order_group_record_submit(LrvxOrderGroupHandle h, uint32_t leg_index, uint64_t order_id);
  void lrvx_order_group_record_fill(LrvxOrderGroupHandle h, uint32_t leg_index,
                                    int64_t cumulative_qty_raw);
  void lrvx_order_group_record_cancel(LrvxOrderGroupHandle h, uint32_t leg_index);
  void lrvx_order_group_record_failure(LrvxOrderGroupHandle h, uint32_t leg_index);
  void lrvx_order_group_record_replace_accepted(LrvxOrderGroupHandle h, uint32_t leg_index,
                                                uint64_t new_order_id);
  void lrvx_order_group_record_replace_rejected(LrvxOrderGroupHandle h, uint32_t leg_index);
  uint32_t lrvx_order_group_find_leg_by_order_id(LrvxOrderGroupHandle h, uint64_t order_id);
  uint8_t lrvx_order_group_state(LrvxOrderGroupHandle h);
  uint32_t lrvx_order_group_recommended_actions(LrvxOrderGroupHandle h, int64_t* actions_out,
                                                uint32_t max_actions);
  void lrvx_order_group_mark_action_dispatched(LrvxOrderGroupHandle h, uint32_t leg_index,
                                               uint8_t kind);
  void lrvx_order_group_set_risk_limits(LrvxOrderGroupHandle h, int64_t max_gross_notional_raw,
                                        double max_concentration_pct, int64_t max_leg_qty_raw);
  uint8_t lrvx_order_group_precheck_submission(LrvxOrderGroupHandle h, double equity,
                                               const int64_t* market_ref_prices_raw,
                                               uint32_t market_ref_prices_len, char* rule_out,
                                               size_t rule_capacity, char* detail_out,
                                               size_t detail_capacity);
  void lrvx_order_group_set_pair_latency_budget_ns(LrvxOrderGroupHandle h, int64_t budget_ns);
  uint8_t lrvx_order_group_pair_latency_decision(LrvxOrderGroupHandle h, int64_t leader_submit_ts_ns,
                                                 int64_t leader_ack_ts_ns, uint8_t ack_received);

  // ============================================================
  // Order Journey Tracer
  // ============================================================

  LrvxOrderJourneyTracerHandle lrvx_order_journey_tracer_create(uint64_t max_orders,
                                                                uint64_t max_records_per_order,
                                                                double sample_rate,
                                                                uint64_t sample_salt);
  void lrvx_order_journey_tracer_destroy(LrvxOrderJourneyTracerHandle tracer);
  uint64_t lrvx_order_journey_tracer_order_count(LrvxOrderJourneyTracerHandle tracer);
  uint64_t lrvx_order_journey_tracer_record_count(LrvxOrderJourneyTracerHandle tracer);
  double lrvx_order_journey_tracer_median_ack_latency_ns(LrvxOrderJourneyTracerHandle tracer);
  double lrvx_order_journey_tracer_median_time_to_first_fill_ns(LrvxOrderJourneyTracerHandle tracer);
  double lrvx_order_journey_tracer_maker_fill_ratio(LrvxOrderJourneyTracerHandle tracer);
  double lrvx_order_journey_tracer_cancel_race_loss_rate(LrvxOrderJourneyTracerHandle tracer);
  uint64_t lrvx_order_journey_tracer_result(LrvxOrderJourneyTracerHandle tracer,
                                            LrvxOrderTraceRow* out, uint64_t max_rows);
  uint64_t lrvx_order_journey_tracer_journey(LrvxOrderJourneyTracerHandle tracer, uint64_t order_id,
                                             LrvxOrderTraceRow* out, uint64_t max_rows);
  void lrvx_order_journey_tracer_clear(LrvxOrderJourneyTracerHandle tracer);
  void lrvx_backtest_runner_add_journey_tracer(LrvxBacktestRunnerHandle runner,
                                               LrvxOrderJourneyTracerHandle tracer);

  // ============================================================
  // Order Tracker
  // ============================================================

  LrvxOrderTrackerHandle lrvx_order_tracker_create(void);
  void lrvx_order_tracker_destroy(LrvxOrderTrackerHandle tracker);
  uint8_t lrvx_order_tracker_on_submitted(LrvxOrderTrackerHandle tracker, uint64_t order_id,
                                          uint32_t symbol, uint8_t side, double price, double qty);
  uint8_t lrvx_order_tracker_on_filled(LrvxOrderTrackerHandle tracker, uint64_t order_id,
                                       double fill_qty);
  uint8_t lrvx_order_tracker_on_canceled(LrvxOrderTrackerHandle tracker, uint64_t order_id);
  uint8_t lrvx_order_tracker_is_active(LrvxOrderTrackerHandle tracker, uint64_t order_id);
  uint32_t lrvx_order_tracker_active_count(LrvxOrderTrackerHandle tracker);
  uint32_t lrvx_order_tracker_total_count(LrvxOrderTrackerHandle tracker);
  void lrvx_order_tracker_prune(LrvxOrderTrackerHandle tracker);

  // ============================================================
  // Partitioner
  // ============================================================

  LrvxPartitionerHandle lrvx_partitioner_create(const char* data_dir);
  void lrvx_partitioner_destroy(LrvxPartitionerHandle partitioner);
  uint32_t lrvx_partitioner_by_time(LrvxPartitionerHandle p, uint32_t num_partitions,
                                    int64_t warmup_ns, LrvxPartition* partitions_out,
                                    uint32_t max_partitions);
  uint32_t lrvx_partitioner_by_duration(LrvxPartitionerHandle p, int64_t duration_ns,
                                        int64_t warmup_ns, LrvxPartition* partitions_out,
                                        uint32_t max_partitions);
  uint32_t lrvx_partitioner_by_calendar(LrvxPartitionerHandle p, uint8_t unit, int64_t warmup_ns,
                                        LrvxPartition* partitions_out, uint32_t max_partitions);
  uint32_t lrvx_partitioner_by_symbol(LrvxPartitionerHandle p, uint32_t num_partitions,
                                      LrvxPartition* partitions_out, uint32_t max_partitions);
  uint32_t lrvx_partitioner_per_symbol(LrvxPartitionerHandle p, LrvxPartition* partitions_out,
                                       uint32_t max_partitions);
  uint32_t lrvx_partitioner_by_event_count(LrvxPartitionerHandle p, uint32_t num_partitions,
                                           LrvxPartition* partitions_out, uint32_t max_partitions);

  // ============================================================
  // Pointer Out
  // ============================================================

  void lrvx_data_reader_summary_p(LrvxDataReaderHandle reader, void* out);
  void lrvx_data_reader_stats_p(LrvxDataReaderHandle reader, void* out);
  void lrvx_data_writer_stats_p(LrvxDataWriterHandle writer, void* out);
  void lrvx_binary_log_recorder_hook_stats_p(void* hook, void* out);
  void lrvx_segment_merge_full_p(const char* input_paths, size_t num_paths, const char* output_dir,
                                 const char* output_name, uint8_t sort, void* out);
  void lrvx_segment_merge_dir_p(const char* input_dir, const char* output_dir, void* out);
  void lrvx_segment_split_p(const char* input_path, const char* output_dir, uint8_t mode,
                            int64_t time_interval_ns, uint64_t events_per_file, void* out);
  void lrvx_segment_export_p(const char* input_path, const char* output_path, uint8_t format,
                             int64_t from_ns, int64_t to_ns, const uint32_t* symbols,
                             uint32_t num_symbols, void* out);
  void lrvx_segment_validate_full_p(const char* path, uint8_t verify_crc, uint8_t verify_timestamps,
                                    void* out);
  void lrvx_dataset_validate_p(const char* data_dir, void* out);

  // ============================================================
  // Pool Tape
  // ============================================================

  LrvxPoolTapeHandle lrvx_pool_tape_create(void);
  void lrvx_pool_tape_descriptor_constant_product(LrvxPoolTapeHandle tape, uint64_t fee_num,
                                                  uint64_t fee_den, uint8_t base_dec,
                                                  uint8_t quote_dec);
  void lrvx_pool_tape_descriptor_raydium_cp(LrvxPoolTapeHandle tape, uint64_t trade_fee_rate,
                                            uint64_t creator_fee_rate, uint8_t creator_fee_on_input,
                                            uint8_t base_dec, uint8_t quote_dec);
  void lrvx_pool_tape_descriptor_clmm(LrvxPoolTapeHandle tape, uint8_t venue, uint32_t fee_pips,
                                      uint8_t base_dec, uint8_t quote_dec);
  uint8_t lrvx_pool_tape_checkpoint(LrvxPoolTapeHandle tape, int64_t ts_ns, const char* reserve0,
                                    const char* reserve1);
  uint8_t lrvx_pool_tape_checkpoint_clmm(LrvxPoolTapeHandle tape, int64_t ts_ns,
                                         const char* sqrt_price, const char* liquidity,
                                         const char* const* tick_sqrt_ratio,
                                         const char* const* tick_liquidity_net, size_t n_ticks);
  uint8_t lrvx_pool_tape_swap(LrvxPoolTapeHandle tape, int64_t ts_ns, uint8_t base_for_quote,
                              const char* amount_in);
  void lrvx_pool_tape_destroy(LrvxPoolTapeHandle tape);
  /* Owned: the replay runs the tape and keeps the rebuilt state. Destroy
   * it with lrvx_pool_replay_destroy; the tape it was built from is
   * unaffected. */
  LrvxPoolReplayHandle lrvx_pool_tape_replay(LrvxPoolTapeHandle tape, size_t base_idx,
                                             size_t quote_idx, uint8_t base_dec, uint8_t quote_dec);
  size_t lrvx_pool_replay_drift_count(LrvxPoolReplayHandle replay);
  size_t lrvx_pool_replay_trade_count(LrvxPoolReplayHandle replay);
  /* Borrowed: the replay owns this curve. It stays valid while the replay
   * lives and lrvx_curve_destroy on it is a no-op. */
  LrvxCurveHandle lrvx_pool_replay_curve(LrvxPoolReplayHandle replay);
  void lrvx_pool_replay_destroy(LrvxPoolReplayHandle replay);

  // ============================================================
  // Portfolio Risk
  // ============================================================

  LrvxPortfolioRiskHandle lrvx_portfolio_risk_create(const LrvxPortfolioRiskRules* rules,
                                                     double initial_equity);
  void lrvx_portfolio_risk_destroy(LrvxPortfolioRiskHandle handle);
  void lrvx_portfolio_risk_update(LrvxPortfolioRiskHandle handle, const char* name,
                                  const LrvxStrategyAccountFields* fields, uint8_t field_mask);
  void lrvx_portfolio_risk_remove(LrvxPortfolioRiskHandle handle, const char* name);
  void lrvx_portfolio_risk_reset_kill_switch(LrvxPortfolioRiskHandle handle);
  uint8_t lrvx_portfolio_risk_check_order(LrvxPortfolioRiskHandle handle, const char* strategy,
                                          double notional, const char* side,
                                          LrvxBreach* out_breach);
  double lrvx_portfolio_risk_total_daily_pnl(LrvxPortfolioRiskHandle handle);
  double lrvx_portfolio_risk_total_gross_exposure(LrvxPortfolioRiskHandle handle);
  double lrvx_portfolio_risk_current_equity(LrvxPortfolioRiskHandle handle);
  double lrvx_portfolio_risk_drawdown_pct(LrvxPortfolioRiskHandle handle);
  uint8_t lrvx_portfolio_risk_kill_switch_active(LrvxPortfolioRiskHandle handle);
  uint64_t lrvx_portfolio_risk_breach_count(LrvxPortfolioRiskHandle handle);
  uint8_t lrvx_portfolio_risk_breach_at(LrvxPortfolioRiskHandle handle, uint64_t index,
                                        LrvxBreach* out);
  uint64_t lrvx_portfolio_risk_account_count(LrvxPortfolioRiskHandle handle);

  // ============================================================
  // Position
  // ============================================================

  LrvxPositionTrackerHandle lrvx_position_tracker_create(uint8_t cost_basis);
  void lrvx_position_tracker_destroy(LrvxPositionTrackerHandle tracker);
  void lrvx_position_tracker_on_fill(LrvxPositionTrackerHandle tracker, uint32_t symbol,
                                     uint8_t side, double price, double quantity);
  double lrvx_position_tracker_position(LrvxPositionTrackerHandle tracker, uint32_t symbol);
  double lrvx_position_tracker_avg_entry(LrvxPositionTrackerHandle tracker, uint32_t symbol);
  double lrvx_position_tracker_realized_pnl(LrvxPositionTrackerHandle tracker, uint32_t symbol);
  double lrvx_position_tracker_total_pnl(LrvxPositionTrackerHandle tracker);

  // ============================================================
  // Position Group
  // ============================================================

  LrvxPositionGroupHandle lrvx_position_group_create(void);
  void lrvx_position_group_destroy(LrvxPositionGroupHandle tracker);
  uint64_t lrvx_position_group_open(LrvxPositionGroupHandle tracker, uint64_t order_id,
                                    uint32_t symbol, uint8_t side, double price, double qty);
  void lrvx_position_group_close(LrvxPositionGroupHandle tracker, uint64_t position_id,
                                 double exit_price);
  void lrvx_position_group_partial_close(LrvxPositionGroupHandle tracker, uint64_t position_id,
                                         double qty, double exit_price);
  double lrvx_position_group_net_position(LrvxPositionGroupHandle tracker, uint32_t symbol);
  double lrvx_position_group_realized_pnl(LrvxPositionGroupHandle tracker, uint32_t symbol);
  double lrvx_position_group_total_pnl(LrvxPositionGroupHandle tracker);
  uint32_t lrvx_position_group_open_count(LrvxPositionGroupHandle tracker, uint32_t symbol);
  void lrvx_position_group_prune(LrvxPositionGroupHandle tracker);

  // ============================================================
  // Rate Limit
  // ============================================================

  LrvxRateLimitPolicyHandle lrvx_rate_limit_policy_create(void);
  void lrvx_rate_limit_policy_destroy(LrvxRateLimitPolicyHandle h);
  void lrvx_rate_limit_policy_add_bucket(LrvxRateLimitPolicyHandle h, const char* name,
                                         int64_t window_ns, uint32_t capacity,
                                         uint32_t submit_weight, uint32_t cancel_weight,
                                         uint32_t replace_weight);
  void lrvx_rate_limit_policy_add_bucket_family(LrvxRateLimitPolicyHandle h, const char* name,
                                                int64_t window_ns, uint32_t capacity,
                                                uint32_t submit_weight, uint32_t cancel_weight,
                                                uint32_t replace_weight, uint8_t family,
                                                uint32_t query_weight);
  void lrvx_rate_limit_policy_set_ban(LrvxRateLimitPolicyHandle h,
                                      uint32_t after_consecutive_rejects, int64_t ban_duration_ns);
  int lrvx_rate_limit_policy_load_profile(LrvxRateLimitPolicyHandle h, const char* profile_name);
  int64_t lrvx_rate_limit_policy_ban_until_ns(LrvxRateLimitPolicyHandle h);
  uint32_t lrvx_rate_limit_policy_consecutive_rejects(LrvxRateLimitPolicyHandle h);
  uint32_t lrvx_rate_limit_policy_bucket_state(LrvxRateLimitPolicyHandle h, int64_t now_ns,
                                               int64_t* out_buf, uint32_t max_buckets);
  void lrvx_simulated_executor_set_rate_limit_policy(LrvxSimulatedExecutorHandle executor,
                                                     LrvxRateLimitPolicyHandle policy);
  void lrvx_simulated_executor_clear_rate_limit_policy(LrvxSimulatedExecutorHandle executor);

  // ============================================================
  // Recorder
  // ============================================================

  LrvxMarketDataRecorderHandle lrvx_market_data_recorder_create(LrvxMarketDataRecorderCallbacks callbacks);
  LrvxMarketDataRecorderHandle lrvx_market_data_recorder_create_p(const LrvxMarketDataRecorderCallbacks* callbacks);
  void lrvx_market_data_recorder_destroy(LrvxMarketDataRecorderHandle recorder);
  void lrvx_live_engine_set_market_data_recorder(LrvxLiveEngineHandle engine,
                                                 LrvxMarketDataRecorderHandle recorder);
  void lrvx_runner_set_market_data_recorder(LrvxRunnerHandle runner,
                                            LrvxMarketDataRecorderHandle recorder);

  // ============================================================
  // Replay
  // ============================================================

  LrvxReplaySourceHandle lrvx_replay_source_create(LrvxReplaySourceCallbacks callbacks);
  LrvxReplaySourceHandle lrvx_replay_source_create_p(const LrvxReplaySourceCallbacks* callbacks);
  void lrvx_replay_source_destroy(LrvxReplaySourceHandle source);
  uint8_t lrvx_replay_source_seek_to(LrvxReplaySourceHandle source, int64_t timestamp_ns);

  // ============================================================
  // Risk
  // ============================================================

  LrvxRiskManagerHandle lrvx_risk_manager_create(LrvxRiskManagerCallbacks callbacks);
  LrvxRiskManagerHandle lrvx_risk_manager_create_p(const LrvxRiskManagerCallbacks* callbacks);
  void lrvx_risk_manager_destroy(LrvxRiskManagerHandle rm);
  LrvxKillSwitchHandle lrvx_kill_switch_create(LrvxKillSwitchCallbacks callbacks);
  LrvxKillSwitchHandle lrvx_kill_switch_create_p(const LrvxKillSwitchCallbacks* callbacks);
  void lrvx_kill_switch_destroy(LrvxKillSwitchHandle ks);
  LrvxOrderValidatorHandle lrvx_order_validator_create(LrvxOrderValidatorCallbacks callbacks);
  LrvxOrderValidatorHandle lrvx_order_validator_create_p(const LrvxOrderValidatorCallbacks* callbacks);
  void lrvx_order_validator_destroy(LrvxOrderValidatorHandle ov);
  void lrvx_live_engine_set_risk_manager(LrvxLiveEngineHandle engine, LrvxRiskManagerHandle rm);
  void lrvx_live_engine_set_kill_switch(LrvxLiveEngineHandle engine, LrvxKillSwitchHandle ks);
  void lrvx_live_engine_set_order_validator(LrvxLiveEngineHandle engine, LrvxOrderValidatorHandle ov);
  void lrvx_runner_set_risk_manager(LrvxRunnerHandle runner, LrvxRiskManagerHandle rm);
  void lrvx_runner_set_kill_switch(LrvxRunnerHandle runner, LrvxKillSwitchHandle ks);
  void lrvx_runner_set_order_validator(LrvxRunnerHandle runner, LrvxOrderValidatorHandle ov);

  // ============================================================
  // Segment
  // ============================================================

  uint8_t lrvx_segment_validate(const char* path);
  uint8_t lrvx_segment_merge(const char* input_dir, const char* output_path);
  /* input_paths is one buffer holding num_paths consecutive NUL-terminated
   * paths, back to back ("a.lrvx\0b.lrvx\0"). The function walks forward
   * exactly num_paths times and has no way to notice a buffer that ends
   * early -- pass the count the buffer actually contains. NULL input_paths
   * or output_dir returns success = 0.
   *
   * lrvx_backtest_runner_run_tapes takes the ordinary const char* const*
   * array instead; prefer that shape in new code. */
  LrvxMergeResult lrvx_segment_merge_full(const char* input_paths, size_t num_paths,
                                          const char* output_dir, const char* output_name,
                                          uint8_t sort);
  LrvxMergeResult lrvx_segment_merge_dir(const char* input_dir, const char* output_dir);
  LrvxSplitResult lrvx_segment_split(const char* input_path, const char* output_dir, uint8_t mode,
                                     int64_t time_interval_ns, uint64_t events_per_file);
  LrvxExportResult lrvx_segment_export(const char* input_path, const char* output_path,
                                       uint8_t format, int64_t from_ns, int64_t to_ns,
                                       const uint32_t* symbols, uint32_t num_symbols);
  uint8_t lrvx_segment_recompress(const char* input_path, const char* output_path,
                                  uint8_t compression);
  uint64_t lrvx_segment_extract_symbols(const char* input_path, const char* output_path,
                                        const uint32_t* symbols, uint32_t num_symbols);
  uint64_t lrvx_segment_extract_time_range(const char* input_path, const char* output_path,
                                           int64_t from_ns, int64_t to_ns);

  // ============================================================
  // Signal Emission
  // ============================================================

  uint64_t lrvx_emit_market_buy(LrvxStrategyHandle s, uint32_t symbol, int64_t qty_raw);
  uint64_t lrvx_emit_market_sell(LrvxStrategyHandle s, uint32_t symbol, int64_t qty_raw);
  uint64_t lrvx_emit_limit_buy(LrvxStrategyHandle s, uint32_t symbol, int64_t price_raw,
                               int64_t qty_raw);
  uint64_t lrvx_emit_limit_sell(LrvxStrategyHandle s, uint32_t symbol, int64_t price_raw,
                                int64_t qty_raw);
  void lrvx_emit_cancel(LrvxStrategyHandle s, uint64_t order_id);
  void lrvx_emit_cancel_all(LrvxStrategyHandle s, uint32_t symbol);
  void lrvx_emit_modify(LrvxStrategyHandle s, uint64_t order_id, int64_t new_price_raw,
                        int64_t new_qty_raw);
  uint64_t lrvx_emit_stop_market(LrvxStrategyHandle s, uint32_t symbol, uint8_t side,
                                 int64_t trigger_raw, int64_t qty_raw);
  uint64_t lrvx_emit_stop_limit(LrvxStrategyHandle s, uint32_t symbol, uint8_t side,
                                int64_t trigger_raw, int64_t limit_raw, int64_t qty_raw);
  uint64_t lrvx_emit_take_profit_market(LrvxStrategyHandle s, uint32_t symbol, uint8_t side,
                                        int64_t trigger_raw, int64_t qty_raw);
  uint64_t lrvx_emit_trailing_stop(LrvxStrategyHandle s, uint32_t symbol, uint8_t side,
                                   int64_t offset_raw, int64_t qty_raw);
  uint64_t lrvx_emit_trailing_stop_percent(LrvxStrategyHandle s, uint32_t symbol, uint8_t side,
                                           int32_t callback_bps, int64_t qty_raw);
  uint64_t lrvx_emit_take_profit_limit(LrvxStrategyHandle s, uint32_t symbol, uint8_t side,
                                       int64_t trigger_raw, int64_t limit_raw, int64_t qty_raw);
  uint64_t lrvx_emit_limit_buy_tif(LrvxStrategyHandle s, uint32_t symbol, int64_t price_raw,
                                   int64_t qty_raw, uint8_t time_in_force);
  uint64_t lrvx_emit_limit_sell_tif(LrvxStrategyHandle s, uint32_t symbol, int64_t price_raw,
                                    int64_t qty_raw, uint8_t time_in_force);
  uint64_t lrvx_emit_close_position(LrvxStrategyHandle s, uint32_t symbol);

  // ============================================================
  // Simulated Executor
  // ============================================================

  LrvxSimulatedExecutorHandle lrvx_simulated_executor_create(void);
  void lrvx_simulated_executor_destroy(LrvxSimulatedExecutorHandle executor);
  void lrvx_simulated_executor_submit_order(LrvxSimulatedExecutorHandle executor, uint64_t id,
                                            uint8_t side, double price, double quantity,
                                            uint8_t order_type, uint32_t symbol);
  void lrvx_simulated_executor_submit_order_ex(LrvxSimulatedExecutorHandle executor, uint64_t id,
                                               uint8_t side, double price, double quantity,
                                               uint8_t order_type, uint32_t symbol, uint8_t tif,
                                               uint8_t reduce_only, int64_t expires_at_ns);
  void lrvx_simulated_executor_submit_bracket(LrvxSimulatedExecutorHandle executor,
                                              uint64_t bracket_id, uint32_t symbol,
                                              uint8_t entry_side, uint8_t entry_type,
                                              double entry_price, double quantity, uint8_t tp_side,
                                              uint8_t tp_type, double tp_price, uint8_t stop_side,
                                              uint8_t stop_type, double stop_trigger_price);
  void lrvx_simulated_executor_cancel_bracket(LrvxSimulatedExecutorHandle executor,
                                              uint64_t bracket_id);
  uint8_t lrvx_simulated_executor_bracket_state(LrvxSimulatedExecutorHandle executor,
                                                uint64_t bracket_id);
  void lrvx_simulated_executor_set_bracket_child_arm_mode(LrvxSimulatedExecutorHandle executor,
                                                          uint8_t mode);
  void lrvx_simulated_executor_submit_iceberg(LrvxSimulatedExecutorHandle executor, uint64_t id,
                                              uint8_t side, double price, double total_quantity,
                                              double visible_quantity, uint32_t symbol);
  void lrvx_simulated_executor_set_iceberg_refresh_latency(LrvxSimulatedExecutorHandle executor,
                                                           int64_t latency_ns);
  int64_t lrvx_simulated_executor_iceberg_hidden_remaining_raw(LrvxSimulatedExecutorHandle executor,
                                                               uint64_t id);
  void lrvx_simulated_executor_set_iceberg_size_randomisation_pct(LrvxSimulatedExecutorHandle executor,
                                                                  double pct);
  void lrvx_simulated_executor_set_iceberg_priority_mode(LrvxSimulatedExecutorHandle executor,
                                                         uint8_t mode);
  void lrvx_simulated_executor_set_iceberg_jitter_seed(LrvxSimulatedExecutorHandle executor,
                                                       uint64_t seed);
  void lrvx_simulated_executor_cancel_order(LrvxSimulatedExecutorHandle executor, uint64_t order_id);
  void lrvx_simulated_executor_cancel_all(LrvxSimulatedExecutorHandle executor, uint32_t symbol);
  void lrvx_simulated_executor_on_bar(LrvxSimulatedExecutorHandle executor, uint32_t symbol,
                                      double close_price);
  /* Open-aware form: moves the market to the open (releasing any order held
   * from a bar-callback window at the previous bar's open), then walks
   * low -> high -> close so resting stops/targets match the intrabar
   * extremes, not just the close. Use this, not the close-only
   * lrvx_simulated_executor_on_bar, to drive the executor by hand to the
   * same fills BacktestRunner::runBars produces. */
  void lrvx_simulated_executor_on_bar_ohlc(LrvxSimulatedExecutorHandle executor, uint32_t symbol,
                                           double open_price, double high_price,
                                           double low_price, double close_price);
  /* Bar-callback window: while open, every arriving order is held instead of
   * matched, released at the next lrvx_simulated_executor_on_bar_ohlc call's
   * open. Depth-counted; call begin before a hand-driven bar callback and
   * end right after. */
  void lrvx_simulated_executor_begin_bar_callback_window(LrvxSimulatedExecutorHandle executor);
  void lrvx_simulated_executor_end_bar_callback_window(LrvxSimulatedExecutorHandle executor);
  /* Drops fills and run-scoped state while keeping installed configuration,
   * so a second hand-driven run reports that run and not the sum of every
   * run. */
  void lrvx_simulated_executor_reset(LrvxSimulatedExecutorHandle executor);
  void lrvx_simulated_executor_on_trade(LrvxSimulatedExecutorHandle executor, uint32_t symbol,
                                        double price, uint8_t is_buy);
  void lrvx_simulated_executor_advance_clock(LrvxSimulatedExecutorHandle executor,
                                             int64_t timestamp_ns);
  uint32_t lrvx_simulated_executor_fill_count(LrvxSimulatedExecutorHandle executor);

  // ============================================================
  // Statistics
  // ============================================================

  double lrvx_stat_correlation(const double* x, const double* y, size_t len);
  double lrvx_stat_profit_factor(const double* pnl, size_t len);
  double lrvx_stat_win_rate(const double* pnl, size_t len);

  // ============================================================
  // Storage
  // ============================================================

  LrvxStorageSinkHandle lrvx_storage_sink_create(LrvxStorageSinkCallbacks callbacks);
  LrvxStorageSinkHandle lrvx_storage_sink_create_p(const LrvxStorageSinkCallbacks* callbacks);
  void lrvx_storage_sink_destroy(LrvxStorageSinkHandle sink);
  void lrvx_live_engine_set_storage_sink(LrvxLiveEngineHandle engine, LrvxStorageSinkHandle sink);
  void lrvx_runner_set_storage_sink(LrvxRunnerHandle runner, LrvxStorageSinkHandle sink);

  // ============================================================
  // Strategy Lifecycle
  // ============================================================

  LrvxStrategyHandle lrvx_strategy_create(uint32_t id, const uint32_t* symbols,
                                          uint32_t num_symbols, LrvxRegistryHandle registry,
                                          LrvxStrategyCallbacks callbacks);
  void lrvx_strategy_destroy(LrvxStrategyHandle strategy);
  void lrvx_strategy_replace_callbacks(LrvxStrategyHandle strategy, LrvxStrategyCallbacks callbacks);
  LrvxStrategyHandle lrvx_strategy_create_p(uint32_t id, const uint32_t* symbols,
                                            uint32_t num_symbols, LrvxRegistryHandle registry,
                                            const LrvxStrategyCallbacks* callbacks);
  void lrvx_strategy_replace_callbacks_p(LrvxStrategyHandle strategy,
                                         const LrvxStrategyCallbacks* callbacks);

  // ============================================================
  // Strategyrunner Synchronous
  // ============================================================

  LrvxRunnerHandle lrvx_runner_create(LrvxRegistryHandle registry, LrvxOnSignalCallback on_signal,
                                      void* user_data);
  void lrvx_runner_destroy(LrvxRunnerHandle runner);
  void lrvx_runner_add_strategy(LrvxRunnerHandle runner, LrvxStrategyHandle strategy);
  void lrvx_runner_start(LrvxRunnerHandle runner);
  void lrvx_runner_stop(LrvxRunnerHandle runner);
  void lrvx_runner_on_trade(LrvxRunnerHandle runner, uint32_t symbol, double price, double qty,
                            uint8_t is_buy, int64_t exchange_ts_ns);
  void lrvx_runner_on_book_snapshot(LrvxRunnerHandle runner, uint32_t symbol,
                                    const double* bid_prices, const double* bid_qtys,
                                    uint32_t n_bids, const double* ask_prices,
                                    const double* ask_qtys, uint32_t n_asks, int64_t exchange_ts_ns);
  void lrvx_runner_on_bar(LrvxRunnerHandle runner, uint32_t symbol, uint8_t bar_type,
                          uint64_t bar_type_param, double open, double high, double low,
                          double close, double volume, double buy_volume, int64_t start_time_ns,
                          int64_t end_time_ns, uint8_t close_reason);

  // ============================================================
  // Symbol Registry
  // ============================================================

  LrvxRegistryHandle lrvx_registry_create(void);
  void lrvx_registry_destroy(LrvxRegistryHandle registry);
  uint32_t lrvx_registry_add_symbol(LrvxRegistryHandle registry, const char* exchange,
                                    const char* name, double tick_size);
  uint8_t lrvx_registry_get_symbol_id(LrvxRegistryHandle registry, const char* exchange,
                                      const char* name, uint32_t* id_out);
  uint8_t lrvx_registry_get_symbol_name(LrvxRegistryHandle registry, uint32_t symbol_id,
                                        char* exchange_out, size_t exchange_len, char* name_out,
                                        size_t name_len);
  uint32_t lrvx_registry_symbol_count(LrvxRegistryHandle registry);

  // ============================================================
  // Tape Aggregator
  // ============================================================

  LrvxAggregatorHandle lrvx_event_type_stats_aggregator_create(LrvxAggregatorEventFilter event_filter,
                                                               const uint32_t* symbol_filter,
                                                               uint32_t symbol_filter_count);
  LrvxAggregatorHandle lrvx_bin_count_aggregator_create(int64_t bucket_ns, uint8_t by_side,
                                                        uint8_t by_symbol,
                                                        LrvxAggregatorEventFilter event_filter,
                                                        const uint32_t* symbol_filter,
                                                        uint32_t symbol_filter_count);
  LrvxAggregatorHandle lrvx_volume_bin_aggregator_create(int64_t bucket_ns, uint8_t by_side,
                                                         uint8_t by_symbol,
                                                         LrvxAggregatorEventFilter event_filter,
                                                         const uint32_t* symbol_filter,
                                                         uint32_t symbol_filter_count);
  LrvxAggregatorHandle lrvx_ohlc_bin_aggregator_create(int64_t bucket_ns, uint8_t by_symbol,
                                                       LrvxAggregatorEventFilter event_filter,
                                                       const uint32_t* symbol_filter,
                                                       uint32_t symbol_filter_count);
  LrvxAggregatorHandle lrvx_peak_aggregator_create(const int64_t* window_ns_list,
                                                   uint32_t window_count, uint32_t top_n,
                                                   uint32_t oversample_factor,
                                                   LrvxAggregatorEventFilter event_filter,
                                                   const uint32_t* symbol_filter,
                                                   uint32_t symbol_filter_count);
  LrvxAggregatorHandle lrvx_quantile_aggregator_create(const int64_t* window_ns_list,
                                                       uint32_t window_count,
                                                       const double* quantiles,
                                                       uint32_t quantile_count,
                                                       LrvxAggregatorEventFilter event_filter,
                                                       const uint32_t* symbol_filter,
                                                       uint32_t symbol_filter_count);
  LrvxAggregatorHandle lrvx_book_snapshot_bin_aggregator_create(int64_t bucket_ns, uint16_t levels,
                                                                LrvxAggregatorEventFilter event_filter,
                                                                const uint32_t* symbol_filter,
                                                                uint32_t symbol_filter_count);
  void lrvx_aggregator_destroy(LrvxAggregatorHandle h);
  uint8_t lrvx_data_reader_run(LrvxDataReaderHandle reader, LrvxAggregatorHandle* aggregators,
                               uint32_t aggregator_count, uint32_t n_threads);
  uint8_t lrvx_merged_tape_reader_run(LrvxMergedTapeReaderHandle reader,
                                      LrvxAggregatorHandle* aggregators, uint32_t aggregator_count,
                                      uint32_t n_threads);
  uint32_t lrvx_event_type_stats_read_result(LrvxAggregatorHandle h,
                                             LrvxEventTypeStatsRow* rows_out, uint32_t max_rows);
  uint32_t lrvx_bin_count_read_result(LrvxAggregatorHandle h, LrvxBinCountRow* rows_out,
                                      uint32_t max_rows);
  uint32_t lrvx_volume_bin_read_result(LrvxAggregatorHandle h, LrvxVolumeBinRow* rows_out,
                                       uint32_t max_rows);
  uint32_t lrvx_ohlc_bin_read_result(LrvxAggregatorHandle h, LrvxOHLCBinRow* rows_out,
                                     uint32_t max_rows);
  uint32_t lrvx_peak_read_result(LrvxAggregatorHandle h, LrvxPeakRow* rows_out, uint32_t max_rows);
  uint32_t lrvx_quantile_read_result(LrvxAggregatorHandle h, LrvxQuantileRow* rows_out,
                                     uint32_t max_rows);
  uint32_t lrvx_book_snapshot_bin_read_result(LrvxAggregatorHandle h,
                                              LrvxBookSnapshotBinRow* rows_out, uint32_t max_rows);

  // ============================================================
  // Tape Diff
  // ============================================================

  LrvxTapeDiffHandle lrvx_tape_diff_create(const char* left_path, const char* right_path,
                                           uint32_t max_mismatches, int64_t field_tolerance_ns);
  void lrvx_tape_diff_destroy(LrvxTapeDiffHandle handle);
  uint64_t lrvx_tape_diff_left_count(LrvxTapeDiffHandle handle);
  uint64_t lrvx_tape_diff_right_count(LrvxTapeDiffHandle handle);
  uint8_t lrvx_tape_diff_first_divergence(LrvxTapeDiffHandle handle, uint64_t* out_index);
  uint8_t lrvx_tape_diff_equal(LrvxTapeDiffHandle handle);
  uint64_t lrvx_tape_diff_mismatch_count(LrvxTapeDiffHandle handle);
  uint64_t lrvx_tape_diff_copy_mismatches(LrvxTapeDiffHandle handle, LrvxTapeDiffMismatch* out,
                                          uint64_t max_entries);

  // ============================================================
  // Targets
  // ============================================================

  void lrvx_target_future_return(const double* close, size_t len, size_t horizon, double* output);
  void lrvx_target_future_ctc_volatility(const double* close, size_t len, size_t horizon,
                                         double* output);
  void lrvx_target_future_linear_slope(const double* close, size_t len, size_t horizon,
                                       double* output);

  // ============================================================
  // Trace Attach
  // ============================================================

  void lrvx_runner_attach_trace_recorder(LrvxRunnerHandle runner, LrvxRunRecorderHandle recorder);
  void lrvx_runner_set_trace_feed_ts_ns(LrvxRunnerHandle runner, int64_t feed_ts_ns);
  void lrvx_runner_trace_order_event(LrvxRunnerHandle runner, uint64_t order_id,
                                     uint64_t parent_signal_id, uint32_t symbol_id,
                                     uint8_t event_kind, uint8_t side, uint8_t order_type,
                                     int64_t price_raw, int64_t qty_raw, uint32_t flags);
  void lrvx_runner_trace_fill(LrvxRunnerHandle runner, uint64_t order_id, uint64_t fill_id,
                              int64_t price_raw, int64_t qty_raw, int64_t fee_raw,
                              uint32_t symbol_id, uint8_t side, uint8_t liquidity);

  // ============================================================
  // Validation
  // ============================================================

  LrvxSegmentValidation lrvx_segment_validate_full(const char* path, uint8_t verify_crc,
                                                   uint8_t verify_timestamps);
  LrvxDatasetValidation lrvx_dataset_validate(const char* data_dir);

  // ============================================================
  // Venue Availability
  // ============================================================

  LrvxVenueAvailabilityHandle lrvx_venue_availability_create(void);
  void lrvx_venue_availability_destroy(LrvxVenueAvailabilityHandle h);
  void lrvx_venue_availability_schedule_outage(LrvxVenueAvailabilityHandle h, int64_t start_ns,
                                               int64_t duration_ns, uint8_t policy,
                                               int64_t gtc_ttl_ns);
  void lrvx_venue_availability_schedule_outage_ex(LrvxVenueAvailabilityHandle h, int64_t start_ns,
                                                  int64_t duration_ns, uint8_t outage_type,
                                                  uint8_t policy, int64_t gtc_ttl_ns,
                                                  double degradation_latency_multiplier,
                                                  double wrong_side_recovery_bps);
  uint8_t lrvx_venue_availability_submits_allowed(LrvxVenueAvailabilityHandle h, int64_t now_ns);
  uint8_t lrvx_venue_availability_cancels_allowed(LrvxVenueAvailabilityHandle h, int64_t now_ns);
  uint8_t lrvx_venue_availability_book_updates_allowed(LrvxVenueAvailabilityHandle h, int64_t now_ns);
  uint8_t lrvx_venue_availability_trades_allowed(LrvxVenueAvailabilityHandle h, int64_t now_ns);
  double lrvx_venue_availability_latency_multiplier(LrvxVenueAvailabilityHandle h, int64_t now_ns);
  double lrvx_venue_availability_consume_wrong_side_recovery_bps(LrvxVenueAvailabilityHandle h);
  void lrvx_venue_availability_auto_random_outages(LrvxVenueAvailabilityHandle h, double per_day,
                                                   int64_t mean_duration_ns, uint8_t policy,
                                                   uint64_t seed);
  uint8_t lrvx_venue_availability_is_up(LrvxVenueAvailabilityHandle h, int64_t now_ns);
  void lrvx_simulated_executor_set_venue_availability(LrvxSimulatedExecutorHandle executor,
                                                      LrvxVenueAvailabilityHandle availability);

  // ============================================================
  // Venue Stack
  // ============================================================

  LrvxVenueStackHandle lrvx_venue_stack_create(uint8_t venue, uint64_t account_id, double equity);
  void lrvx_venue_stack_destroy(LrvxVenueStackHandle h);
  /* lrvx_venue_stack_executor, lrvx_venue_stack_account,
   * lrvx_venue_stack_liquidation, lrvx_venue_stack_fees,
   * lrvx_venue_stack_funding and lrvx_venue_stack_venue reach inside the
   * stack and return BORROWED handles: each stays valid while the stack
   * lives, must not be destroyed, and the matching _destroy on it does
   * nothing. */
  LrvxSimulatedExecutorHandle lrvx_venue_stack_executor(LrvxVenueStackHandle h);
  LrvxAccountHandle lrvx_venue_stack_account(LrvxVenueStackHandle h);
  LrvxLiquidationEngineHandle lrvx_venue_stack_liquidation(LrvxVenueStackHandle h);
  LrvxFeeScheduleHandle lrvx_venue_stack_fees(LrvxVenueStackHandle h);
  LrvxFundingScheduleHandle lrvx_venue_stack_funding(LrvxVenueStackHandle h);
  LrvxVenueAvailabilityHandle lrvx_venue_stack_venue(LrvxVenueStackHandle h);
  const char* lrvx_venue_stack_venue_name(LrvxVenueStackHandle h);

  // ============================================================
  // Volume Profile
  // ============================================================

  LrvxVolumeProfileHandle lrvx_volume_profile_create(double tick_size);
  void lrvx_volume_profile_destroy(LrvxVolumeProfileHandle profile);
  void lrvx_volume_profile_add_trade(LrvxVolumeProfileHandle profile, double price, double quantity,
                                     uint8_t is_buy);
  double lrvx_volume_profile_poc(LrvxVolumeProfileHandle profile);
  double lrvx_volume_profile_vah(LrvxVolumeProfileHandle profile);
  double lrvx_volume_profile_val(LrvxVolumeProfileHandle profile);
  double lrvx_volume_profile_total_volume(LrvxVolumeProfileHandle profile);
  double lrvx_volume_profile_total_delta(LrvxVolumeProfileHandle profile);
  uint32_t lrvx_volume_profile_num_levels(LrvxVolumeProfileHandle profile);
  void lrvx_volume_profile_clear(LrvxVolumeProfileHandle profile);

  // ============================================================
  // Walk Forward
  // ============================================================

  uint32_t lrvx_walk_forward_run_csv(LrvxRegistryHandle registry, const char* csv_path,
                                     const char* symbol, double fee_rate, double initial_capital,
                                     const LrvxWalkForwardConfig* cfg,
                                     LrvxWalkForwardFactoryFn factory, void* user_data,
                                     LrvxWalkForwardFold* folds_out, uint32_t max_folds);

  // ============================================================
  // Diagnostics
  // ============================================================

  /* ABI version compiled into the library. Compare against the
   * LRVX_CAPI_ABI_VERSION macro from the header you built with and refuse
   * the mismatch: the structs on this boundary have no reserved tail, so
   * a skew shows up as wrong numbers rather than a failed load. */
  uint32_t lrvx_capi_abi_version(void);

  /* The last failure on the calling thread, set by a call that returned
   * its failure value. 0 = no error recorded, 1 = a NULL handle was
   * passed, 2 = an exception was stopped at the boundary. The value is
   * only meaningful straight after a call that reported failure; it is
   * not cleared by a successful call. */
  int lrvx_last_error_code(void);

  /* Text for the code above: the function that failed, and what the
   * exception said. Never NULL -- an empty string means nothing has been
   * recorded on this thread. Valid until the next failure on this
   * thread or the next lrvx_clear_last_error. */
  const char* lrvx_last_error_message(void);

  void lrvx_clear_last_error(void);

#ifdef __cplusplus
}
#endif
