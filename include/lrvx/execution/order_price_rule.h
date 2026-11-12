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

// The rule a simulated venue applies to the prices on an order, in one place.
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

// The tick orders on this symbol are checked against, as a raw price, or 0
// when the symbol's tick is not known. A tick is known when it was declared:
// tickSizeKnown is set, or tickSize differs from the registry default. A
// symbol registered without a tick keeps the 0.01 default for its book but is
// not checked, and a tick of zero is never checked.
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
