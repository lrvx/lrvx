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
 * Tool:   tools/codegen/lrvx_codegen/emit_layout.py
 *
 * Byte layout of the event structs a binding may read through raw memory.
 * codon/lrvx/layout.codon is generated from the same pass; the offsets there
 * are these offsets. tests/test_capi_event_layout.cpp compares every entry
 * below against offsetof/sizeof on the real struct, so a layout the compiler
 * disagrees with fails the build instead of a user's strategy.
 */

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

  typedef struct
  {
    const char* struct_name;
    const char* field_name;
    size_t offset;
    size_t size;
  } LrvxFieldLayout;

  typedef struct
  {
    const char* struct_name;
    size_t size;
    size_t alignment;
  } LrvxStructLayout;

  static const LrvxFieldLayout kLrvxEventLayout[] = {
      {"LrvxTradeData", "symbol", 0, 4},
      {"LrvxTradeData", "price_raw", 8, 8},
      {"LrvxTradeData", "quantity_raw", 16, 8},
      {"LrvxTradeData", "is_buy", 24, 1},
      {"LrvxTradeData", "exchange_ts_ns", 32, 8},
      {"LrvxBookSnapshot", "bid_price_raw", 0, 8},
      {"LrvxBookSnapshot", "bid_qty_raw", 8, 8},
      {"LrvxBookSnapshot", "ask_price_raw", 16, 8},
      {"LrvxBookSnapshot", "ask_qty_raw", 24, 8},
      {"LrvxBookSnapshot", "mid_raw", 32, 8},
      {"LrvxBookSnapshot", "spread_raw", 40, 8},
      {"LrvxBookSnapshot", "has_bid", 48, 1},
      {"LrvxBookSnapshot", "has_ask", 49, 1},
      {"LrvxSymbolContext", "symbol_id", 0, 4},
      {"LrvxSymbolContext", "has_avg_entry_price", 4, 1},
      {"LrvxSymbolContext", "position_raw", 8, 8},
      {"LrvxSymbolContext", "avg_entry_price_raw", 16, 8},
      {"LrvxSymbolContext", "last_trade_price_raw", 24, 8},
      {"LrvxSymbolContext", "last_update_ns", 32, 8},
      {"LrvxSymbolContext", "book", 40, 56},
      {"LrvxSymbolContext", "last_touch_ns", 96, 8},
      {"LrvxBarData", "symbol", 0, 4},
      {"LrvxBarData", "bar_type", 4, 1},
      {"LrvxBarData", "close_reason", 5, 1},
      {"LrvxBarData", "bar_type_param", 8, 8},
      {"LrvxBarData", "open_raw", 16, 8},
      {"LrvxBarData", "high_raw", 24, 8},
      {"LrvxBarData", "low_raw", 32, 8},
      {"LrvxBarData", "close_raw", 40, 8},
      {"LrvxBarData", "volume_raw", 48, 8},
      {"LrvxBarData", "buy_volume_raw", 56, 8},
      {"LrvxBarData", "trade_count_raw", 64, 8},
      {"LrvxBarData", "start_time_ns", 72, 8},
      {"LrvxBarData", "end_time_ns", 80, 8},
      {"LrvxOrderEventData", "order_id", 0, 8},
      {"LrvxOrderEventData", "symbol_id", 8, 4},
      {"LrvxOrderEventData", "side", 12, 1},
      {"LrvxOrderEventData", "order_type", 13, 1},
      {"LrvxOrderEventData", "status", 14, 1},
      {"LrvxOrderEventData", "fill_qty_raw", 16, 8},
      {"LrvxOrderEventData", "fill_price_raw", 24, 8},
      {"LrvxOrderEventData", "exchange_ts_ns", 32, 8},
      {"LrvxOrderEventData", "reject_reason", 40, 8},
      {"LrvxOrderEventData", "queue_ahead_raw", 48, 8},
      {"LrvxOrderEventData", "queue_total_raw", 56, 8},
      {"LrvxOrderEventData", "submitted_at_ns", 64, 8},
      {"LrvxOrderEventData", "accepted_at_ns", 72, 8},
      {"LrvxOrderEventData", "first_fill_at_ns", 80, 8},
      {"LrvxOrderEventData", "last_fill_at_ns", 88, 8},
      {"LrvxOrderEventData", "canceled_at_ns", 96, 8},
      {"LrvxOrderEventData", "rejected_at_ns", 104, 8},
      {"LrvxOrderEventData", "triggered_at_ns", 112, 8},
      {"LrvxOrderEventData", "expired_at_ns", 120, 8},
      {"LrvxOrderEventData", "is_maker", 128, 1},
      {"LrvxOrderEventData", "market_position", 129, 1},
      {"LrvxOrderEventData", "distance_to_best_ticks", 132, 4},
      {"LrvxBar", "start_time_ns", 0, 8},
      {"LrvxBar", "end_time_ns", 8, 8},
      {"LrvxBar", "open_raw", 16, 8},
      {"LrvxBar", "high_raw", 24, 8},
      {"LrvxBar", "low_raw", 32, 8},
      {"LrvxBar", "close_raw", 40, 8},
      {"LrvxBar", "volume_raw", 48, 8},
      {"LrvxBar", "buy_volume_raw", 56, 8},
      {"LrvxBar", "trade_count", 64, 4},
      {"LrvxBar", "close_reason", 68, 1},
  };

  static const LrvxStructLayout kLrvxEventStructLayout[] = {
      {"LrvxTradeData", 40, 8},
      {"LrvxBookSnapshot", 56, 8},
      {"LrvxSymbolContext", 104, 8},
      {"LrvxBarData", 88, 8},
      {"LrvxOrderEventData", 144, 8},
      {"LrvxBar", 72, 8},
  };

#ifdef __cplusplus
}
#endif
