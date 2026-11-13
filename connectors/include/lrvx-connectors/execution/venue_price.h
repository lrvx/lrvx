/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2026 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <lrvx/common.h>
#include <lrvx/engine/symbol_registry.h>
#include <lrvx/execution/order.h>
#include <lrvx/execution/order_price_rule.h>

#include <cstdint>
#include <string>

namespace lrvx::venue_price
{

// The price rule every executor applies before a request is built, the same
// one the simulator applies (execution/order_price_rule.h): when the symbol's
// tick is known, a limit price, trigger price or trailing distance that is
// not a whole number of ticks is refused here with the simulator's reason
// (price_off_tick, trigger_off_tick, trailing_offset_off_tick), instead of
// being rounded to a price the strategy did not ask for. When the tick is not
// known nothing is refused and the price is sent exactly as given; the venue
// then applies its own rule and its refusal comes back as an event.
inline const char* offTickReason(const Order& order, const SymbolInfo& info) noexcept
{
  return lrvx::offTickReason(order, orderTickRaw(info));
}

// The quantity rule beside it, also the simulator's: when the symbol declares
// a lot (the venue's, applied at start(), or the caller's), an order quantity
// that is not a whole number of lots is refused here with quantity_off_lot.
// With no lot declared the quantity is sent as given.
inline const char* offLotReason(const Order& order, const SymbolInfo& info) noexcept
{
  return lrvx::offLotReason(order, orderLotRaw(info));
}

// Off-tick check for a single price the caller passes outside an Order (a
// position stop-loss or take-profit trigger). Same rule: false when on tick
// or when the tick is not known.
inline bool isOffTick(Price p, const SymbolInfo& info) noexcept
{
  const int64_t tick = orderTickRaw(info);
  return tick > 0 && p.raw() % tick != 0;
}

// Fractional digits the fixed-point Price carries (its scale is a power of
// ten).
inline constexpr int kScaleDigits = []
{
  int digits = 0;
  for (int64_t scale = Price::Scale; scale > 1; scale /= 10)
  {
    ++digits;
  }
  return digits;
}();

// The exact decimal of a fixed-point value: every fractional digit it has,
// and at least minFractionDigits of them (padded with zeros). No floating
// point and no rounding, so the string always parses back to the same raw
// value. With minFractionDigits = 6 it is the string Decimal::toString()
// produces for any value with at most six fractional digits; toString()
// rounds a seventh and eighth digit away, this keeps them.
inline std::string formatExact(int64_t raw, int minFractionDigits)
{
  const bool negative = raw < 0;
  const uint64_t magnitude =
      negative ? (0ULL - static_cast<uint64_t>(raw)) : static_cast<uint64_t>(raw);
  const uint64_t scale = static_cast<uint64_t>(Price::Scale);
  const uint64_t whole = magnitude / scale;
  uint64_t fraction = magnitude % scale;

  int digits = kScaleDigits;
  while (digits > 0 && fraction % 10 == 0 && digits > minFractionDigits)
  {
    fraction /= 10;
    --digits;
  }

  std::string out;
  out.reserve(24);
  if (negative && magnitude != 0)
  {
    out.push_back('-');
  }
  out.append(std::to_string(whole));
  if (digits > 0)
  {
    std::string frac = std::to_string(fraction);
    out.push_back('.');
    out.append(static_cast<size_t>(digits) - frac.size(), '0');
    out.append(frac);
  }
  return out;
}

// The form Bybit and Bitget's regular order endpoints have always been sent:
// six fractional digits, more when the value has more.
inline std::string wireDecimal(Price p) { return formatExact(p.raw(), 6); }

// Trailing zeros (and a bare decimal point) dropped: "58000", "0.1234".
inline std::string trimmedDecimal(Price p) { return formatExact(p.raw(), 0); }

}  // namespace lrvx::venue_price
