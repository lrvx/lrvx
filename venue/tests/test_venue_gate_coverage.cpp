/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

/*
 * Gate coverage: no order reaches the book, and no trade prints, without the
 * gates that are supposed to guard it.
 *
 * Three defects of one shape were found by hand in this module: the credit hook
 * was never called on a funded path (both money branches returned first), the
 * reservation-trim hook only fired on instruments that enable last look, and
 * reduce-only was checked at submit and never at fill. Each was a mechanism
 * applied on some paths and skipped on others, and no test noticed because the
 * tests followed paths, not mechanisms.
 *
 * Listing paths would only pin the ones that exist today. These tests assert a
 * PROPERTY instead -- every admitted order was authorized, every printed trade
 * was risk-checked -- so a path added tomorrow that forgets a gate fails here
 * without anyone remembering to extend a list.
 */
#include "lrvx-venue/matching_book.h"
#include "lrvx-venue/matching_engine.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <functional>
#include <set>
#include <string>
#include <vector>

using namespace lrvx;
using namespace lrvx::venue;

namespace
{
int g_failures = 0;
int g_checks = 0;
void check(bool ok, const char* e, int line)
{
  ++g_checks;
  if (!ok)
  {
    ++g_failures;
    std::printf("  FAIL line %d: %s\n", line, e);
  }
}
#define CHECK(x) check((x), #x, __LINE__)

constexpr SymbolId SYM = 1;
constexpr AssetId BASE = 0;
constexpr AssetId QUOTE = 1;
constexpr uint64_t VENUE = 999;
using Eng = MatchingEngine<MatchingBook>;

Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }

SymbolConfig cfg(bool perp = false)
{
  SymbolConfig c;
  c.id = SYM;
  c.tickSize = px(0.01);
  c.minPrice = px(1.0);
  c.maxPrice = px(1000.0);
  c.baseAsset = BASE;
  c.quoteAsset = QUOTE;
  if (perp)
  {
    c.linearPerp = true;
    c.initialMarginBps = 1000;
  }
  return c;
}

NewOrder limit(OrderId id, Side s, double p, double q, uint64_t acct)
{
  NewOrder o;
  o.id = id;
  o.symbol = SYM;
  o.side = s;
  o.type = OrderType::LIMIT;
  o.price = px(p);
  o.quantity = qty(q);
  o.accountId = acct;
  return o;
}

// Records which orders the risk owner was asked about.
struct Authorizer
{
  std::set<OrderId> asked;
  Eng::CreditCheck hook()
  {
    return [this](const Eng::CreditRequest& r) -> Eng::CreditDecision
    {
      asked.insert(r.order);
      return {true, RejectReason::None};
    };
  }
};

// Every id that ended up resting on the book.
std::vector<OrderId> restingIds(const Eng& eng)
{
  std::vector<OrderId> out;
  eng.book().forEachOrder([&](const RestingOrder& r)
                          { out.push_back(r.id); });
  return out;
}

// ---- property 1: every admission asks permission, every time ---------------
//
// The naive form of this ("every id resting was authorized at some point")
// does not hold anyone honest: a modify re-enters an id that was authorized on
// submit, so a modify path that skips the gate still passes. The authorizer is
// therefore cleared before each step and each step must ask on its own.

struct PathCase
{
  const char* name;
  OrderId expect;  // the id the gate must be asked about
  std::function<void(Eng&, int64_t)> run;
};

void test_every_admission_asks_permission()
{
  std::printf("test_every_admission_asks_permission\n");

  const std::vector<PathCase> paths = {
      {"limit", 10, [](Eng& e, int64_t ts)
       { e.submit(InboundCommand{limit(10, Side::BUY, 100, 5, 1)}, ts); }},
      {"post-only", 11,
       [](Eng& e, int64_t ts)
       {
         NewOrder o = limit(11, Side::BUY, 99, 5, 1);
         o.tif = TimeInForce::POST_ONLY;
         e.submit(InboundCommand{o}, ts);
       }},
      {"iceberg", 12,
       [](Eng& e, int64_t ts)
       {
         NewOrder o = limit(12, Side::BUY, 98, 10, 1);
         o.type = OrderType::ICEBERG;
         o.visibleQuantity = qty(2);
         e.submit(InboundCommand{o}, ts);
       }},
      {"peg", 13,
       [](Eng& e, int64_t ts)
       {
         NewOrder o = limit(13, Side::BUY, 97, 3, 1);
         o.peg = PegRef::Bid;
         e.submit(InboundCommand{o}, ts);
       }},
      {"quote bid leg", 14,
       [](Eng& e, int64_t ts)
       {
         Quote q{};
         q.bidId = 14;
         q.askId = 15;
         q.symbol = SYM;
         q.bidPrice = px(96);
         q.bidQty = qty(4);
         q.askPrice = px(120);
         q.askQty = qty(4);
         q.accountId = 2;
         e.submit(InboundCommand{q}, ts);
       }},
      {"peg reprice", 13,
       [](Eng& e, int64_t ts)
       {
         // A peg re-enters the book at a new price whenever the reference
         // moves. That is a fresh admission -- and the one path that used to
         // skip the gate, so the mutation test needs this case to bite.
         // repeg() runs at the START of a submit, so the order that moves the
         // reference only takes effect on the NEXT command: post first, then
         // drive one more submit to make the peg re-enter at the new price.
         e.submit(InboundCommand{limit(30, Side::BUY, 100.5, 1, 2)}, ts);
         e.submit(InboundCommand{limit(31, Side::SELL, 300, 1, 1)}, ts + 1);
       }},
      {"modify reprice", 10,
       [](Eng& e, int64_t ts)
       {
         ModifyOrder m{};
         m.id = 10;
         m.symbol = SYM;
         m.newPrice = px(101);
         m.newQty = qty(5);
         e.submit(InboundCommand{m}, ts);
       }},
      {"stop trigger", 20,
       [](Eng& e, int64_t ts)
       {
         // The stop was admitted when it was placed; triggering makes it a live
         // aggressor, which is a fresh admission and must be asked about again.
         e.submit(InboundCommand{limit(21, Side::SELL, 115, 1, 1)}, ts);
         e.submit(InboundCommand{limit(22, Side::BUY, 115, 1, 2)}, ts + 1);
       }},
  };

  Ledger led;
  for (uint64_t acct : {1ULL, 2ULL})
  {
    // Generous on purpose: this suite proves gates are CALLED, so an order
    // must never be rejected for lack of funds -- an unfunded order simply
    // never reaches the path under test (which is how the peg case first
    // "passed" while testing nothing).
    led.deposit(acct, QUOTE, 100000000000000LL);
    led.deposit(acct, BASE, 100000000000000LL);
  }
  std::vector<OutboundEvent> ev;
  Authorizer auth;
  Eng eng(cfg(), [&](const OutboundEvent& e)
          { ev.push_back(e); });
  eng.setLedger(&led, VENUE);
  eng.setCreditCheck(auth.hook());

  int64_t ts = 100;
  // Park a stop so the "stop trigger" case has something to fire.
  NewOrder st = limit(20, Side::BUY, 130, 2, 2);
  st.type = OrderType::STOP_LIMIT;
  st.triggerPrice = px(110);
  eng.submit(InboundCommand{st}, ++ts);

  for (const auto& p : paths)
  {
    auth.asked.clear();
    p.run(eng, ts += 10);
    const bool asked = auth.asked.count(p.expect) != 0;
    if (!asked)
    {
      std::printf("  path '%s' admitted order %llu without asking the gate (asked:", p.name,
                  static_cast<unsigned long long>(p.expect));
      for (OrderId a : auth.asked)
      {
        std::printf(" %llu", static_cast<unsigned long long>(a));
      }
      std::printf(")\n");
    }
    CHECK(asked);
  }
}

// ---- property 2: every perp fill was risk-checked at fill time -------------
//
// Covers the matcher paths and the auction uncross, which prints its own fills
// outside the matcher and had exactly this hole.

void test_every_perp_fill_was_risk_checked()
{
  std::printf("test_every_perp_fill_was_risk_checked\n");
  for (int mode = 0; mode < 2; ++mode)  // 0 = continuous, 1 = auction uncross
  {
    Ledger led;
    led.deposit(1, QUOTE, 100000000000000LL);
    led.deposit(2, QUOTE, 100000000000000LL);
    std::vector<OutboundEvent> ev;
    Eng eng(cfg(/*perp=*/true), [&](const OutboundEvent& e)
            { ev.push_back(e); });
    eng.setLedger(&led, VENUE);

    int64_t ts = 0;
    if (mode == 1)
    {
      eng.beginPreOpen();
    }
    eng.submit(InboundCommand{limit(1, Side::SELL, 100, 5, 2)}, ++ts);
    eng.submit(InboundCommand{limit(2, Side::BUY, 100, 5, 1)}, ++ts);
    if (mode == 1)
    {
      eng.openContinuous();  // uncross prints here
    }

    int trades = 0;
    for (auto& e : ev)
    {
      if (std::get_if<Trade>(&e) != nullptr)
      {
        ++trades;
      }
    }
    CHECK(trades > 0);
    // The position the fills produced is the observable proof the fill-time
    // risk path ran over them: it is maintained by the same code the fill
    // limit gates. A fill that bypassed it would leave the position empty.
    CHECK(eng.positionQty(1) == qty(5).raw());
    CHECK(eng.positionQty(2) == -qty(5).raw());
  }
}

// ---- property 3: instrument state is honoured on EVERY path ----------------
//
// Halt and session-close are checked in the new-order path. A stop resting from
// before the halt triggers on a later print, and a peg reprices on book moves:
// neither goes through that check, so each is asserted directly.

void test_instrument_state_honoured_on_every_path()
{
  std::printf("test_instrument_state_honoured_on_every_path\n");
  // Both trigger references, because they reach processTriggers differently: a
  // last-price trigger needs a print (which a halt already blocks), while a
  // mark trigger arrives on setMarkPrice and used to fire straight through a
  // halt or a closed session -- the state check simply was not on that path.
  for (int state = 0; state < 4; ++state)
  {
    const bool closeSession = (state % 2) == 1;
    const bool markTrigger = state >= 2;
    std::vector<OutboundEvent> ev;
    SymbolConfig c = cfg();
    if (markTrigger)
    {
      c.triggerRef = TriggerRef::Mark;
    }
    Eng eng(c, [&](const OutboundEvent& e)
            { ev.push_back(e); });
    int64_t ts = 0;

    eng.submit(InboundCommand{limit(1, Side::BUY, 90, 5, 2)}, ++ts);  // resting liquidity
    NewOrder st = limit(2, Side::SELL, 0, 1, 3);
    st.type = OrderType::STOP_MARKET;
    st.triggerPrice = px(95);
    eng.submit(InboundCommand{st}, ++ts);

    AdminCmd a{};
    a.symbol = SYM;
    a.action = closeSession ? AdminAction::CloseSession : AdminAction::Halt;
    eng.submit(InboundCommand{a}, ++ts);

    const size_t before = ev.size();
    eng.setMarkPrice(px(94));  // would cross the stop trigger
    eng.submit(InboundCommand{limit(3, Side::SELL, 94, 1, 4)}, ++ts);

    int trades = 0;
    for (size_t i = before; i < ev.size(); ++i)
    {
      if (std::get_if<Trade>(&ev[i]) != nullptr)
      {
        ++trades;
      }
    }
    if (trades != 0)
    {
      std::printf("  %d trade(s) printed while %s (%s trigger)\n", trades,
                  closeSession ? "session-closed" : "halted",
                  markTrigger ? "mark" : "last");
    }
    CHECK(trades == 0);
  }
}

// ---- property 4: instrument conformance applies to conditionals too --------
//
// A conditional order branches off before validate(), which is written around a
// live limit price. It still carries a trigger price and a size, and both used
// to be parked unchecked: an off-tick trigger, one outside the price band, or a
// sub-lot quantity was accepted and only surfaced when the stop fired.

void test_conditional_orders_obey_the_instrument()
{
  std::printf("test_conditional_orders_obey_the_instrument\n");
  struct Case
  {
    const char* name;
    double trigger;
    double quantity;
  };
  const Case bad[] = {
      {"off-tick trigger", 50.005, 5},
      {"trigger below band", 0.5, 5},
      {"trigger above band", 5000, 5},
      {"sub-lot quantity", 50, 0.5},
  };

  for (const auto& c : bad)
  {
    std::vector<OutboundEvent> ev;
    SymbolConfig sc = cfg();
    sc.minPrice = px(10);
    sc.lotSize = qty(1);
    Eng eng(sc, [&](const OutboundEvent& e)
            { ev.push_back(e); });

    NewOrder st = limit(1, Side::SELL, 0, c.quantity, 1);
    st.type = OrderType::STOP_MARKET;
    st.triggerPrice = px(c.trigger);
    eng.submit(InboundCommand{st}, 1);

    int accepted = 0;
    for (auto& e : ev)
    {
      if (std::get_if<OrderAccepted>(&e) != nullptr)
      {
        ++accepted;
      }
    }
    if (accepted != 0)
    {
      std::printf("  conditional with %s was parked anyway\n", c.name);
    }
    CHECK(accepted == 0);
  }

  // The conforming case still works -- the guard must not reject everything.
  std::vector<OutboundEvent> ok;
  SymbolConfig sc = cfg();
  sc.minPrice = px(10);
  sc.lotSize = qty(1);
  Eng eng(sc, [&](const OutboundEvent& e)
          { ok.push_back(e); });
  NewOrder good = limit(2, Side::SELL, 0, 5, 1);
  good.type = OrderType::STOP_MARKET;
  good.triggerPrice = px(50);
  eng.submit(InboundCommand{good}, 1);
  int accepted = 0;
  for (auto& e : ok)
  {
    if (std::get_if<OrderAccepted>(&e) != nullptr)
    {
      ++accepted;
    }
  }
  CHECK(accepted == 1);
}

// ---- property 5: a conditional is refused for the same reason a limit is ----
//
// The same violation gets the same reason whichever order type carries it: a
// client that rounds an off-grid limit on TickSizeViolation must not need a
// second rule for an off-grid stop. Each case is submitted as a stop-market
// (the price is its trigger), as a stop-limit (the price is both trigger and
// limit), and as a plain limit, and the three reasons must agree.

RejectReason firstReject(const SymbolConfig& sc, const NewOrder& o)
{
  std::vector<OutboundEvent> ev;
  Eng eng(sc, [&](const OutboundEvent& e)
          { ev.push_back(e); });
  eng.submit(InboundCommand{o}, 1);
  for (auto& e : ev)
  {
    if (const auto* r = std::get_if<OrderRejected>(&e))
    {
      return r->reason;
    }
  }
  return RejectReason::None;
}

void test_conditional_reasons_match_limit_reasons()
{
  std::printf("test_conditional_reasons_match_limit_reasons\n");
  struct Case
  {
    const char* name;
    double price;
    double quantity;
    RejectReason expected;
  };
  const Case bad[] = {
      {"off-tick price", 50.005, 5, RejectReason::TickSizeViolation},
      {"price below band", 0.5, 5, RejectReason::InvalidPrice},
      {"price above band", 5000, 5, RejectReason::InvalidPrice},
      {"sub-lot quantity", 50, 0.5, RejectReason::LotSizeViolation},
  };
  SymbolConfig sc = cfg();
  sc.minPrice = px(10);
  sc.lotSize = qty(1);

  for (const auto& c : bad)
  {
    const NewOrder lim = limit(1, Side::SELL, c.price, c.quantity, 1);
    NewOrder stopMkt = limit(1, Side::SELL, 0, c.quantity, 1);
    stopMkt.type = OrderType::STOP_MARKET;
    stopMkt.triggerPrice = px(c.price);
    NewOrder stopLim = lim;
    stopLim.type = OrderType::STOP_LIMIT;
    stopLim.triggerPrice = px(c.price);

    const RejectReason rl = firstReject(sc, lim);
    const RejectReason rm = firstReject(sc, stopMkt);
    const RejectReason rs = firstReject(sc, stopLim);
    if (rl != c.expected || rm != c.expected || rs != c.expected)
    {
      std::printf("  %s: limit %s, stop-market %s, stop-limit %s, expected %s\n", c.name, toString(rl),
                  toString(rm), toString(rs), toString(c.expected));
    }
    CHECK(rl == c.expected);
    CHECK(rm == c.expected);
    CHECK(rs == c.expected);
  }

  // The stop-limit's own limit price is held to the tick with the same
  // reason when only it is off the grid.
  NewOrder offTickLimitOnly = limit(1, Side::SELL, 50.005, 5, 1);
  offTickLimitOnly.type = OrderType::STOP_LIMIT;
  offTickLimitOnly.triggerPrice = px(50);
  CHECK(firstReject(sc, offTickLimitOnly) == RejectReason::TickSizeViolation);
}

}  // namespace

TEST(VenueGateCoverage, EveryPathIsGated)
{
  test_every_admission_asks_permission();
  test_every_perp_fill_was_risk_checked();
  test_instrument_state_honoured_on_every_path();
  test_conditional_orders_obey_the_instrument();
  test_conditional_reasons_match_limit_reasons();
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  EXPECT_EQ(g_failures, 0);
}
