/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "lrvx/util/base/decimal.h"

#include <chrono>
#include <cstdint>

#if defined(_MSC_VER) && defined(_M_X64) && !defined(__clang__)
#include <intrin.h>
#endif

namespace lrvx
{

enum class InstrumentType
{
  Spot,
  Future,
  Inverse,
  Option
};

enum class OptionType
{
  CALL,
  PUT
};

enum class SettlementType
{
  Cash,
  Physical
};

enum class ExerciseStyle
{
  European,
  American
};

// This is the only order-type code space the C++ engine uses. It is NOT
// the same space as the C-ABI's LRVX_SIGNAL_TYPE_* codes
// (include/lrvx/capi/lrvx_capi.h), which govern LrvxSignal.order_type
// only and swap LIMIT/MARKET relative to this enum. See
// include/lrvx/capi/order_type_names.hpp for the canonical string tables
// of both spaces.
enum class OrderType : uint8_t
{
  LIMIT = 0,
  MARKET = 1,
  STOP_MARKET = 2,
  STOP_LIMIT = 3,
  TAKE_PROFIT_MARKET = 4,
  TAKE_PROFIT_LIMIT = 5,
  TRAILING_STOP = 6,
  ICEBERG = 7,
};

enum class TimeInForce : uint8_t
{
  GTC = 0,        // Good Till Cancel (default)
  IOC = 1,        // Immediate Or Cancel
  FOK = 2,        // Fill Or Kill
  GTD = 3,        // Good Till Date
  POST_ONLY = 4,  // Maker only
};

// Self-trade prevention modes. Real venues apply these when a new
// order from the same account would cross an existing resting order
// of the same account.
enum class STPMode : uint8_t
{
  None = 0,          // self-match allowed (default)
  CancelNewest = 1,  // cancel the incoming order
  CancelOldest = 2,  // cancel the resting order; new one goes through
  CancelBoth = 3,    // cancel both legs
  Decrement = 4,     // cancel smaller side fully; reduce larger by smaller qty
};

enum class Side
{
  BUY,
  SELL
};

using SymbolId = uint32_t;
using OrderId = uint64_t;
using ExchangeId = uint16_t;

// Order ids a runner assigns itself: the second leg of an OCO, and an order
// built from a signal that carries no id. A strategy's ids are
// (subscriber id << 32) + n, so this is one subscriber id's block, and a
// strategy holding that subscriber id is refused when it is attached. The
// block ends below 2^53: every id in it is exact as a double, which is how
// the JavaScript bindings carry order ids.
inline constexpr uint64_t kRunnerOrderIdSubscriber = 0x1F'FFFFULL;
inline constexpr OrderId kRunnerOrderIdBase = kRunnerOrderIdSubscriber << 32;

// The last subscriber id's block is where MultiModePositionTracker numbers the
// fills it is handed without an order (its explicit fills). A Strategy
// refuses that subscriber id: its orders would carry those ids.
inline constexpr uint64_t kExplicitFillOrderIdSubscriber = 0xFFFF'FFFFULL;
inline constexpr OrderId kExplicitFillOrderIdBase = kExplicitFillOrderIdSubscriber << 32;

static constexpr ExchangeId InvalidExchangeId = 0xFFFF;

enum class VenueType : uint8_t
{
  CentralizedExchange,
  AmmDex,
  HybridDex
};

struct PriceTag
{
};
struct QuantityTag
{
};
struct VolumeTag
{
};

// tick = 0.00000001 (8 decimals)
using Price = Decimal<PriceTag, 100'000'000, 1>;
using Quantity = Decimal<QuantityTag, 100'000'000, 1>;
using Volume = Decimal<VolumeTag, 100'000'000, 1>;

inline Volume operator*(Quantity qty, Price px)
{
  LRVX_SCALE_CHECK(qty.scale() == Quantity::Scale && px.scale() == Price::Scale,
                   "Quantity*Price requires default scale; rescale a per-symbol value first");
  // One implementation of "multiply two fixed-point numbers and divide by a
  // scale, exactly". It used to be three branches here, and the third -- the
  // portable one -- was hand-rolled, wrong, and the branch clang-cl landed on:
  // its remainder term overflows int64 at a price of 60000 and a quantity of
  // 0.5, and a backtest came back with -534 where 5000 was expected.
  return Volume::fromRaw(mulDivI64(qty.raw(), px.raw(), Volume::Scale));
}

inline Volume operator*(Price px, Quantity qty)
{
  return qty * px;
}

inline Price operator/(Volume vol, Quantity qty)
{
  LRVX_SCALE_CHECK(vol.scale() == Volume::Scale && qty.scale() == Quantity::Scale,
                   "Volume/Quantity requires default scale; rescale a per-symbol value first");
  // An unfilled order divides a zero volume by a zero quantity here. Left to
  // the platform that is a hardware trap on x86 and an unspecified value on
  // arm64, which is how a zero fill became a plausible execution price.
  if (qty.raw() == 0)
  {
    return Price::fromRaw(dividedByZeroI64(vol.raw()));
  }
  return Price::fromRaw(mulDivI64(vol.raw(), Price::Scale, qty.raw()));
}

inline Quantity operator/(Volume vol, Price px)
{
  LRVX_SCALE_CHECK(vol.scale() == Volume::Scale && px.scale() == Price::Scale,
                   "Volume/Price requires default scale; rescale a per-symbol value first");
  if (px.raw() == 0)
  {
    return Quantity::fromRaw(dividedByZeroI64(vol.raw()));
  }
  return Quantity::fromRaw(mulDivI64(vol.raw(), Quantity::Scale, px.raw()));
}

}  // namespace lrvx
