/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "lrvx/common.h"
#include "lrvx/engine/symbol_registry.h"
#include "lrvx/execution/order.h"

#include <cstdint>

namespace lrvx
{

// The rule a simulated venue applies to the prices and the quantity on an
// order, in one place.
//
// A venue refuses an order whose limit price, trigger price or trailing
// distance is not a whole number of ticks (Binance -1013 PRICE_FILTER, Bybit
// priceFilter.tickSize, the lrvx venue's TickSizeViolation). The simulator
// does the same at submit and on replace, so a backtest never trades at a
// price the venue would not have accepted.
//
// Prices the simulator computes itself -- a fill moved by a slippage model, the
// trigger of a percentage trailing stop -- are snapped onto the tick instead,
// on the side that is worse for the order's owner.

// Reject reasons carried on OrderEvent::rejectReason.
inline constexpr const char* kRejectPriceOffTick = "price_off_tick";
inline constexpr const char* kRejectTriggerOffTick = "trigger_off_tick";
inline constexpr const char* kRejectTrailingOffsetOffTick = "trailing_offset_off_tick";

// An order quantity, or an iceberg's visible quantity, that is not a whole
// number of the symbol's declared lot (Binance -1013 LOT_SIZE, Bybit
// lotSizeFilter.qtyStep, the lrvx venue's LotSizeViolation).
inline constexpr const char* kRejectQuantityOffLot = "quantity_off_lot";

// Why a replace was refused, other than its prices.
inline constexpr const char* kRejectUnknownOrder = "unknown_order";
inline constexpr const char* kRejectReplaceChangesType = "replace_changes_order_type";
inline constexpr const char* kRejectLateReplaceAfterTrigger = "late_replace_after_trigger";

// The tick orders on this symbol are checked against, as a raw price, or 0
// when the symbol's tick is not known. A tick is known when it was declared:
// tickSizeKnown is set, or tickSize differs from the registry default. A
// symbol registered without a tick keeps the 0.01 default for its book but is
// not checked, and a tick of zero is never checked. The second half exists
// because a SymbolInfo cannot tell an assigned 0.01 from the default: any
// other value was set on purpose, so C++ code that writes a venue's tick into
// tickSize is checked without knowing the flag exists.
inline int64_t orderTickRaw(const SymbolInfo& info) noexcept
{
  const int64_t tick = info.tickSize.raw();
  if (tick <= 0)
  {
    return 0;
  }
  if (!info.tickSizeKnown && tick == kDefaultTickSize.raw())
  {
    return 0;
  }
  return tick;
}

// The lot order quantities on this symbol are checked against, as a raw
// quantity, or 0 when the symbol declares none. Unlike the tick there is no
// default to tell apart: a lot is declared by being positive.
inline int64_t orderLotRaw(const SymbolInfo& info) noexcept
{
  const int64_t lot = info.lotSize.raw();
  return lot > 0 ? lot : 0;
}

// Why a venue with this lot would refuse the order's quantity, or nullptr
// when it would not. Every order type carries a quantity, a market order
// included.
inline const char* offLotReason(const Order& order, int64_t lotRaw) noexcept
{
  if (lotRaw <= 0)
  {
    return nullptr;
  }
  if (order.quantity.raw() % lotRaw != 0 || order.visibleQuantity.raw() % lotRaw != 0)
  {
    return kRejectQuantityOffLot;
  }
  return nullptr;
}

// Snap a price onto the tick grid on the owner's worse side: a buy is moved up
// (it pays at least the price), a sell down (it receives at most the price).
// A price already on the grid, or a tick of 0, leaves it unchanged.
inline int64_t snapAgainstOwner(int64_t priceRaw, int64_t tickRaw, Side side) noexcept
{
  if (tickRaw <= 0)
  {
    return priceRaw;
  }
  int64_t q = priceRaw / tickRaw;
  const int64_t r = priceRaw % tickRaw;
  if (r == 0)
  {
    return priceRaw;
  }
  // C++ division truncates toward zero; step to floor, then up for a buy.
  if (r < 0)
  {
    --q;
  }
  if (side == Side::BUY)
  {
    ++q;
  }
  return q * tickRaw;
}

// Why a venue with this tick would refuse the order, or nullptr when it would
// not. Market orders carry no price to check.
inline const char* offTickReason(const Order& order, int64_t tickRaw) noexcept
{
  if (tickRaw <= 0)
  {
    return nullptr;
  }
  auto off = [tickRaw](Price p)
  { return p.raw() % tickRaw != 0; };
  switch (order.type)
  {
    case OrderType::LIMIT:
    case OrderType::ICEBERG:
      return off(order.price) ? kRejectPriceOffTick : nullptr;
    case OrderType::STOP_MARKET:
    case OrderType::TAKE_PROFIT_MARKET:
      return off(order.triggerPrice) ? kRejectTriggerOffTick : nullptr;
    case OrderType::STOP_LIMIT:
    case OrderType::TAKE_PROFIT_LIMIT:
      if (off(order.triggerPrice))
      {
        return kRejectTriggerOffTick;
      }
      return off(order.price) ? kRejectPriceOffTick : nullptr;
    case OrderType::TRAILING_STOP:
      return off(order.trailingOffset) ? kRejectTrailingOffsetOffTick : nullptr;
    case OrderType::MARKET:
      return nullptr;
  }
  return nullptr;
}

}  // namespace lrvx
