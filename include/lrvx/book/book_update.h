/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <memory_resource>
#include <vector>

#include "lrvx/common.h"
#include "lrvx/util/base/time.h"

namespace lrvx
{

enum class BookUpdateType
{
  SNAPSHOT,
  DELTA,
  // One-level refresh of the touch (an exchange bbo stream between throttled
  // depth snapshots). Carries the best bid/ask only, and an empty side means
  // that side of the book is genuinely empty. The cross-venue matrix
  // consumes it like a snapshot of the touch; a depth book must ignore it —
  // merging it as a delta would strand a stale better level when the top
  // moves away, replacing like a snapshot would drop the depth.
  TOP_OF_BOOK
};

struct BookLevel
{
  Price price{};
  Quantity quantity{};
  BookLevel() = default;
  BookLevel(Price p, Quantity q) : price(p), quantity(q) {}
};

struct BookUpdate
{
  SymbolId symbol{};
  InstrumentType instrument = InstrumentType::Spot;
  BookUpdateType type{};
  std::pmr::vector<BookLevel> bids;
  std::pmr::vector<BookLevel> asks;

  UnixNanos exchangeTsNs{0};
  UnixNanos systemTsNs{0};

  std::optional<Price> strike;
  std::optional<TimePoint> expiry;
  std::optional<OptionType> optionType;

  BookUpdate(std::pmr::memory_resource* res) : bids(res), asks(res) {}
};

}  // namespace lrvx
