/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * A conditional order below the instrument's minimum size is refused at
 * admission, with the reason a resting limit below it gets (InvalidQuantity).
 *
 * validateConditional checked the quantity's sign, the lot and the trigger,
 * but not minQty, so a stop below the minimum was accepted, parked in the stop
 * book, and only refused when it fired -- a refusal its owner could no longer
 * act on, after the price it was guarding had already moved.
 */
#include "lrvx-venue/engine/credit.h"
#include "lrvx-venue/matching_book.h"
#include "lrvx-venue/matching_engine.h"

#include <gtest/gtest.h>

#include <vector>

using namespace lrvx;
using namespace lrvx::venue;

namespace
{

constexpr SymbolId SYM = 1;

Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }

SymbolConfig cfg()
{
  SymbolConfig c;
  c.id = SYM;
  c.tickSize = px(0.01);
  c.minPrice = px(1.0);
  c.maxPrice = px(1000.0);
  c.lotSize = qty(0.001);
  c.minQty = qty(0.01);
  return c;
}

NewOrder order(OrderId id, OrderType type, Side side, double price, double quantity,
               uint64_t acct = 1)
{
  NewOrder o;
  o.id = id;
  o.symbol = SYM;
  o.side = side;
  o.type = type;
  o.price = px(price);
  o.quantity = qty(quantity);
  o.accountId = acct;
  return o;
}

struct Venue
{
  std::vector<OutboundEvent> ev;
  MatchingEngine<MatchingBook> eng;

  Venue() : eng(cfg(), [this](const OutboundEvent& e)
                { ev.push_back(e); }) {}

  const OrderRejected* rejectOf(OrderId id) const
  {
    for (const auto& e : ev)
    {
      if (const auto* r = std::get_if<OrderRejected>(&e); r != nullptr && r->id == id)
      {
        return r;
      }
    }
    return nullptr;
  }

  bool accepted(OrderId id) const
  {
    for (const auto& e : ev)
    {
      if (const auto* a = std::get_if<OrderAccepted>(&e); a != nullptr && a->id == id)
      {
        return true;
      }
    }
    return false;
  }
};

constexpr OrderType kConditionals[] = {OrderType::STOP_MARKET, OrderType::STOP_LIMIT,
                                       OrderType::TAKE_PROFIT_MARKET,
                                       OrderType::TAKE_PROFIT_LIMIT, OrderType::TRAILING_STOP};

}  // namespace

TEST(VenueConditionalMinQty, CreditRefusesBelowMinimumLikeALimit)
{
  engine::Credit c;
  for (OrderType t : kConditionals)
  {
    SCOPED_TRACE(static_cast<int>(t));
    NewOrder small = order(1, t, Side::SELL, 100.0, 0.005);
    small.triggerPrice = px(90.0);
    EXPECT_EQ(c.validateConditional(small, cfg()), RejectReason::InvalidQuantity);

    NewOrder atMin = small;
    atMin.quantity = qty(0.01);
    EXPECT_EQ(c.validateConditional(atMin, cfg()), RejectReason::None);
  }
}

TEST(VenueConditionalMinQty, RefusedAtAdmissionWithTheLimitsReason)
{
  Venue v;
  v.eng.submit(InboundCommand{order(10, OrderType::LIMIT, Side::BUY, 100.0, 0.005)}, 1);
  ASSERT_NE(v.rejectOf(10), nullptr);
  const RejectReason limitReason = v.rejectOf(10)->reason;
  ASSERT_EQ(limitReason, RejectReason::InvalidQuantity);

  OrderId id = 20;
  for (OrderType t : kConditionals)
  {
    SCOPED_TRACE(static_cast<int>(t));
    NewOrder stop = order(id, t, Side::SELL, 100.0, 0.005);
    stop.triggerPrice = px(90.0);
    v.eng.submit(InboundCommand{stop}, 2);
    const OrderRejected* r = v.rejectOf(id);
    ASSERT_NE(r, nullptr) << "parked instead of refused";
    EXPECT_EQ(r->reason, limitReason);
    EXPECT_FALSE(v.accepted(id));
    ++id;
  }
}

TEST(VenueConditionalMinQty, AtTheMinimumItIsParked)
{
  Venue v;
  NewOrder stop = order(30, OrderType::STOP_MARKET, Side::SELL, 0.0, 0.01);
  stop.triggerPrice = px(90.0);
  v.eng.submit(InboundCommand{stop}, 1);
  EXPECT_EQ(v.rejectOf(30), nullptr);
  EXPECT_TRUE(v.accepted(30));
}
