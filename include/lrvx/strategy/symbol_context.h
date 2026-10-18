/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "lrvx/book/nlevel_order_book.h"
#include "lrvx/common.h"

#include <cmath>
#include <optional>

namespace lrvx
{

struct SymbolContext
{
  static constexpr size_t kDefaultBookLevels = 512;

  NLevelOrderBook<kDefaultBookLevels> book;

  // Freshest known top of book, whichever frame kind carried it: depth
  // frames refresh it from the applied book, TOP_OF_BOOK — which the depth
  // book ignores — refreshes only this. Empty: no quote on that side.
  std::optional<BookLevel> touchBid{};
  std::optional<BookLevel> touchAsk{};
  // Stamp of the frame that last wrote the touch. Separate from
  // lastUpdateNs, which stamps events the strategy was dispatched for.
  int64_t lastTouchNs{0};

  Quantity position{};
  // Empty when the attached position manager reports no cost basis: the
  // position is flat, or the manager keeps none. This used to be a plain
  // Price that nothing in the engine ever wrote, so it read back as 0 and
  // unrealized PnL came out as the entire notional.
  std::optional<Price> avgEntryPrice{};
  Price lastTradePrice{};
  int64_t lastUpdateNs{0};
  SymbolId symbolId{0};

  SymbolContext() : book(Price::fromDouble(0.01)) {}
  SymbolContext(Price tickSize) : book(tickSize) {}

  [[nodiscard]] std::optional<Price> mid() const noexcept
  {
    auto bid = book.bestBid();
    auto ask = book.bestAsk();
    if (!bid || !ask)
    {
      return std::nullopt;
    }
    return Price::fromRaw((bid->raw() + ask->raw()) / 2);
  }

  // Mid of the freshest known touch; unlike mid(), moves on TOP_OF_BOOK
  // frames too.
  [[nodiscard]] std::optional<Price> touchMid() const noexcept
  {
    if (!touchBid || !touchAsk)
    {
      return std::nullopt;
    }
    return Price::fromRaw((touchBid->price.raw() + touchAsk->price.raw()) / 2);
  }

  [[nodiscard]] std::optional<Price> bookSpread() const noexcept
  {
    auto bid = book.bestBid();
    auto ask = book.bestAsk();
    if (!bid || !ask)
    {
      return std::nullopt;
    }
    return Price::fromRaw(ask->raw() - bid->raw());
  }

  // Empty when there is no entry price to measure against. A quietly wrong
  // number is worse than an honest "unknown": the old code substituted zero
  // for the entry and returned position times mark.
  [[nodiscard]] std::optional<double> unrealizedPnl(Price markPrice) const noexcept
  {
    if (position.isZero())
    {
      return 0.0;
    }
    if (!avgEntryPrice)
    {
      return std::nullopt;
    }
    double posQty = position.toDouble();
    double entryPx = avgEntryPrice->toDouble();
    double markPx = markPrice.toDouble();
    return posQty * (markPx - entryPx);
  }

  [[nodiscard]] std::optional<double> unrealizedPnl() const noexcept
  {
    auto midOpt = mid();
    if (!midOpt)
    {
      return std::nullopt;
    }
    return unrealizedPnl(*midOpt);
  }

  bool isLong() const noexcept { return position.raw() > 0; }

  bool isShort() const noexcept { return position.raw() < 0; }

  bool isFlat() const noexcept { return position.isZero(); }

  void reset() noexcept
  {
    book.clear();
    touchBid.reset();
    touchAsk.reset();
    lastTouchNs = 0;
    position = Quantity{};
    avgEntryPrice.reset();
    lastTradePrice = Price{};
    lastUpdateNs = 0;
  }
};

[[nodiscard]] inline std::optional<Price> spread(const SymbolContext& a,
                                                 const SymbolContext& b) noexcept
{
  auto midA = a.mid();
  auto midB = b.mid();
  if (!midA || !midB)
  {
    return std::nullopt;
  }
  return Price::fromRaw(midA->raw() - midB->raw());
}

[[nodiscard]] inline std::optional<double> ratio(const SymbolContext& a,
                                                 const SymbolContext& b) noexcept
{
  auto midA = a.mid();
  auto midB = b.mid();
  if (!midA || !midB || midB->isZero())
  {
    return std::nullopt;
  }
  return midA->toDouble() / midB->toDouble();
}

}  // namespace lrvx
