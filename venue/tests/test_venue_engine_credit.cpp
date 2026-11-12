/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * engine::Credit on its own -- no engine, no book, no matcher.
 *
 * The component decides three things and the tests below are grouped by them:
 * whether a counterparty may send this order at all, what has to be
 * ring-fenced in the ledger before it may rest (and what comes back when it
 * stops resting), and how much of a prospective fill the perp risk limits
 * leave each leg. Driving those through MatchingEngine::submit would test
 * them through matching, validation and the sink; here the question is asked
 * directly, so a wrong answer names the rule that is wrong.
 */

#include "lrvx-venue/engine/credit.h"
#include "lrvx-venue/ledger.h"

#include <gtest/gtest.h>

using namespace lrvx;
using namespace lrvx::venue;

namespace
{
constexpr SymbolId SYM = 7;
constexpr AssetId BTC = 0;
constexpr AssetId USD = 1;

Amount amt(double v) { return amountOf(Volume::fromDouble(v)); }
// __int128 is not gtest-printable; compare the low 64 bits (test values fit).
int64_t i64(Amount a) { return static_cast<int64_t>(a); }

SymbolConfig spotCfg()
{
  SymbolConfig c;
  c.id = SYM;
  c.tickSize = Price::fromDouble(0.01);
  c.lotSize = Quantity::fromDouble(0.001);
  c.minPrice = Price::fromDouble(50.0);
  c.maxPrice = Price::fromDouble(150.0);
  c.baseAsset = BTC;
  c.quoteAsset = USD;
  return c;
}

SymbolConfig perpCfg()
{
  SymbolConfig c = spotCfg();
  c.linearPerp = true;
  c.initialMarginBps = 1000;  // 10% == 10x leverage
  return c;
}

NewOrder limitBuy(OrderId id, double px, double qty, uint64_t acct = 1)
{
  NewOrder o;
  o.id = id;
  o.symbol = SYM;
  o.side = Side::BUY;
  o.type = OrderType::LIMIT;
  o.price = Price::fromDouble(px);
  o.quantity = Quantity::fromDouble(qty);
  o.accountId = acct;
  return o;
}

NewOrder limitSell(OrderId id, double px, double qty, uint64_t acct = 1)
{
  NewOrder o = limitBuy(id, px, qty, acct);
  o.side = Side::SELL;
  return o;
}
}  // namespace

// ---- entitlement ---------------------------------------------------------

TEST(EngineCredit, NoProfileMeansEverythingPermitted)
{
  engine::Credit c;
  NewOrder o = limitBuy(1, 100.0, 1.0);
  EXPECT_EQ(c.admissionGate(o, false), RejectReason::None);
  EXPECT_TRUE(c.admissionProfiles().empty());
  EXPECT_EQ(c.admissionRejects(), 0u);
}

TEST(EngineCredit, DefaultProfileClearsTheEntry)
{
  engine::Credit c;
  AdmissionProfile p;
  p.deny = AdmissionDeny::DenyCancel;
  c.setAdmissionProfile(9, p);
  EXPECT_EQ(c.admissionProfiles().size(), 1u);
  EXPECT_TRUE(c.admissionDenies(9, AdmissionDeny::DenyCancel));
  EXPECT_FALSE(c.admissionDenies(9, AdmissionDeny::DenyAmend));

  c.setAdmissionProfile(9, AdmissionProfile{});  // back to "everything permitted"
  EXPECT_TRUE(c.admissionProfiles().empty());
  EXPECT_FALSE(c.admissionDenies(9, AdmissionDeny::DenyCancel));
}

TEST(EngineCredit, DenyRestingRefusesWhatWouldRest)
{
  engine::Credit c;
  AdmissionProfile p;
  p.deny = AdmissionDeny::DenyResting;
  c.setAdmissionProfile(1, p);

  NewOrder gtc = limitBuy(1, 100.0, 1.0);
  EXPECT_EQ(c.admissionGate(gtc, false), RejectReason::RestingNotPermitted);

  NewOrder ioc = limitBuy(2, 100.0, 1.0);
  ioc.tif = TimeInForce::IOC;
  EXPECT_EQ(c.admissionGate(ioc, false), RejectReason::None);
  // A call auction rests everything it admits, so the same IOC is refused.
  EXPECT_EQ(c.admissionGate(ioc, true), RejectReason::RestingNotPermitted);

  NewOrder post = limitBuy(3, 100.0, 1.0);
  post.tif = TimeInForce::IOC;
  post.postOnly = true;
  EXPECT_EQ(c.admissionGate(post, false), RejectReason::RestingNotPermitted);
}

TEST(EngineCredit, TypeAndTifListsAreBothEnforced)
{
  engine::Credit c;
  AdmissionProfile p;
  p.allowedTypes = 1u << static_cast<uint32_t>(OrderType::LIMIT);
  p.allowedTif = 1u << static_cast<uint32_t>(TimeInForce::GTC);
  c.setAdmissionProfile(1, p);

  EXPECT_EQ(c.admissionGate(limitBuy(1, 100.0, 1.0), false), RejectReason::None);

  NewOrder mkt = limitBuy(2, 100.0, 1.0);
  mkt.type = OrderType::MARKET;
  EXPECT_EQ(c.admissionGate(mkt, false), RejectReason::OrderTypeNotPermitted);

  NewOrder ioc = limitBuy(3, 100.0, 1.0);
  ioc.tif = TimeInForce::IOC;
  EXPECT_EQ(c.admissionGate(ioc, false), RejectReason::TimeInForceNotPermitted);
}

TEST(EngineCredit, AdmissionRejectsCountsWhatTheCallerRefused)
{
  engine::Credit c;
  EXPECT_EQ(c.admissionRejects(), 0u);
  c.countAdmissionReject();
  c.countAdmissionReject();
  EXPECT_EQ(c.admissionRejects(), 2u);
}

// ---- conditional-order conformance ---------------------------------------

TEST(EngineCredit, ConditionalTriggerObeysTickLotAndBand)
{
  engine::Credit c;
  const SymbolConfig cfg = spotCfg();

  NewOrder ok = limitBuy(1, 0.0, 1.0);
  ok.type = OrderType::STOP_MARKET;
  ok.triggerPrice = Price::fromDouble(100.0);
  EXPECT_EQ(c.validateConditional(ok, cfg), RejectReason::None);

  NewOrder offTick = ok;
  offTick.triggerPrice = Price::fromRaw(Price::fromDouble(100.0).raw() + 1);
  EXPECT_EQ(c.validateConditional(offTick, cfg), RejectReason::TickSizeViolation);

  NewOrder outOfBand = ok;
  outOfBand.triggerPrice = Price::fromDouble(200.0);
  EXPECT_EQ(c.validateConditional(outOfBand, cfg), RejectReason::InvalidPrice);

  NewOrder subLot = ok;
  subLot.quantity = Quantity::fromRaw(Quantity::fromDouble(0.001).raw() / 2);
  EXPECT_EQ(c.validateConditional(subLot, cfg), RejectReason::LotSizeViolation);

  NewOrder zero = ok;
  zero.quantity = Quantity::fromRaw(0);
  EXPECT_EQ(c.validateConditional(zero, cfg), RejectReason::InvalidQuantity);

  // A stop-LIMIT also carries the limit price it will rest at.
  NewOrder stopLimit = ok;
  stopLimit.type = OrderType::STOP_LIMIT;
  stopLimit.price = Price::fromDouble(200.0);
  EXPECT_EQ(c.validateConditional(stopLimit, cfg), RejectReason::InvalidPrice);
  stopLimit.price = Price::fromRaw(Price::fromDouble(101.0).raw() + 1);
  EXPECT_EQ(c.validateConditional(stopLimit, cfg), RejectReason::TickSizeViolation);
  stopLimit.price = Price::fromDouble(101.0);
  EXPECT_EQ(c.validateConditional(stopLimit, cfg), RejectReason::None);
}

// ---- buying power --------------------------------------------------------

TEST(EngineCredit, SpotBuyRingFencesQuoteAtTheLimitPrice)
{
  engine::Credit c;
  Ledger l;
  const SymbolConfig cfg = spotCfg();
  l.deposit(1, USD, amt(1000));

  ASSERT_TRUE(c.reserveFunds(limitBuy(1, 100.0, 2.0), cfg, &l));
  EXPECT_EQ(i64(l.reserved(1, USD)), i64(amt(200)));
  EXPECT_EQ(i64(l.available(1, USD)), i64(amt(800)));

  const engine::Credit::Reservation* r = c.find(1);
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(r->account, 1u);
  EXPECT_EQ(r->asset, USD);
  EXPECT_EQ(r->side, Side::BUY);
  EXPECT_EQ(r->limitPriceRaw, Price::fromDouble(100.0).raw());
  EXPECT_EQ(i64(r->reservedRaw), i64(amt(200)));
}

TEST(EngineCredit, SpotSellRingFencesBase)
{
  engine::Credit c;
  Ledger l;
  const SymbolConfig cfg = spotCfg();
  l.deposit(1, BTC, amt(5));

  ASSERT_TRUE(c.reserveFunds(limitSell(1, 100.0, 2.0), cfg, &l));
  EXPECT_EQ(i64(l.reserved(1, BTC)), i64(amt(2)));
  ASSERT_NE(c.find(1), nullptr);
  EXPECT_EQ(c.find(1)->asset, BTC);
}

TEST(EngineCredit, UnfundedOrderReservesNothingAndLeavesNoEntry)
{
  engine::Credit c;
  Ledger l;
  const SymbolConfig cfg = spotCfg();
  l.deposit(1, USD, amt(100));

  EXPECT_FALSE(c.reserveFunds(limitBuy(1, 100.0, 2.0), cfg, &l));
  EXPECT_EQ(i64(l.reserved(1, USD)), 0);
  EXPECT_EQ(i64(l.available(1, USD)), i64(amt(100)));
  EXPECT_FALSE(c.contains(1));
}

TEST(EngineCredit, MarketBuyBoundedByTheBandTopNotByItsAbsentPrice)
{
  engine::Credit c;
  Ledger l;
  const SymbolConfig cfg = spotCfg();  // band top 150
  l.deposit(1, USD, amt(1000));

  NewOrder mkt = limitBuy(1, 0.0, 2.0);
  mkt.type = OrderType::MARKET;
  ASSERT_TRUE(c.reserveFunds(mkt, cfg, &l));
  EXPECT_EQ(i64(l.reserved(1, USD)), i64(amt(300)));  // 2 * band top, not 2 * 0
}

TEST(EngineCredit, MarketBuyWithNoBandIsRefusedRatherThanUnbounded)
{
  engine::Credit c;
  Ledger l;
  SymbolConfig cfg = spotCfg();
  cfg.maxPrice = Price::fromRaw(0);
  l.deposit(1, USD, amt(1000));

  NewOrder mkt = limitBuy(1, 0.0, 2.0);
  mkt.type = OrderType::MARKET;
  EXPECT_FALSE(c.reserveFunds(mkt, cfg, &l));
  EXPECT_EQ(i64(l.reserved(1, USD)), 0);
}

TEST(EngineCredit, PerpReservesInitialMarginInQuote)
{
  engine::Credit c;
  Ledger l;
  const SymbolConfig cfg = perpCfg();  // 1000 bps
  l.deposit(1, USD, amt(1000));

  ASSERT_TRUE(c.reserveFunds(limitBuy(1, 100.0, 2.0), cfg, &l));
  EXPECT_EQ(i64(l.reserved(1, USD)), i64(amt(20)));  // 10% of 200 notional
  EXPECT_EQ(i64(c.imForRaw(Quantity::fromDouble(2.0).raw(), Price::fromDouble(100.0).raw(), cfg)),
            i64(amt(20)));
}

TEST(EngineCredit, PerpSellIsBoundedAtTheBandTopOnBothSides)
{
  engine::Credit c;
  Ledger l;
  const SymbolConfig cfg = perpCfg();  // band 50..150
  l.deposit(1, USD, amt(1000));

  NewOrder mkt = limitSell(1, 0.0, 2.0);
  mkt.type = OrderType::MARKET;
  ASSERT_TRUE(c.reserveFunds(mkt, cfg, &l));
  // Notional grows with price whether the account is long or short: 10% of
  // (2 * 150), not 10% of (2 * 50).
  EXPECT_EQ(i64(l.reserved(1, USD)), i64(amt(30)));
}

TEST(EngineCredit, PerpReduceOnlyReservesNothingButIsStillTracked)
{
  engine::Credit c;
  Ledger l;
  const SymbolConfig cfg = perpCfg();
  l.deposit(1, USD, amt(1000));

  NewOrder ro = limitSell(1, 100.0, 2.0);
  ro.reduceOnly = true;
  ASSERT_TRUE(c.reserveFunds(ro, cfg, &l));
  EXPECT_EQ(i64(l.reserved(1, USD)), 0);
  ASSERT_NE(c.find(1), nullptr);  // tracked: a fill still has to find its bound
  EXPECT_EQ(i64(c.find(1)->reservedRaw), 0);
}

TEST(EngineCredit, PerpMarketWithNoBandIsRefusedRatherThanUncollateralized)
{
  engine::Credit c;
  Ledger l;
  SymbolConfig cfg = perpCfg();
  cfg.maxPrice = Price::fromRaw(0);
  l.deposit(1, USD, amt(1000));

  NewOrder mkt = limitBuy(1, 0.0, 2.0);
  mkt.type = OrderType::MARKET;
  EXPECT_FALSE(c.reserveFunds(mkt, cfg, &l));
  EXPECT_FALSE(c.contains(1));
}

TEST(EngineCredit, WithNoLedgerTheOrderPassesAndNothingIsTracked)
{
  engine::Credit c;
  EXPECT_TRUE(c.reserveFunds(limitBuy(1, 100.0, 2.0), spotCfg(), nullptr));
  EXPECT_FALSE(c.contains(1));
}

// ---- the external credit hook --------------------------------------------

TEST(EngineCredit, CreditHookIsAskedEvenWhenALedgerIsBound)
{
  engine::Credit c;
  Ledger l;
  const SymbolConfig cfg = spotCfg();
  l.deposit(1, USD, amt(1000));

  int asked = 0;
  CreditRequest seen{};
  c.setCreditCheck(
      [&](const CreditRequest& q)
      {
        ++asked;
        seen = q;
        return CreditDecision{true, RejectReason::InsufficientFunds};
      });

  ASSERT_TRUE(c.reserveFunds(limitBuy(42, 100.0, 2.0), cfg, &l));
  EXPECT_EQ(asked, 1);
  EXPECT_EQ(seen.order, 42u);
  EXPECT_EQ(seen.account, 1u);
  EXPECT_EQ(seen.symbol, SYM);  // which instrument, so a portfolio can answer
  EXPECT_EQ(seen.side, Side::BUY);
  EXPECT_FALSE(seen.reduceOnly);
  EXPECT_EQ(i64(l.reserved(1, USD)), i64(amt(200)));
}

TEST(EngineCredit, RefusedCreditReservesNothingAndSurfacesItsOwnReason)
{
  engine::Credit c;
  Ledger l;
  const SymbolConfig cfg = spotCfg();
  l.deposit(1, USD, amt(1000));
  c.setCreditCheck([](const CreditRequest&)
                   { return CreditDecision{false, RejectReason::CreditRefused}; });

  EXPECT_EQ(c.creditReason(), RejectReason::InsufficientFunds);  // the default answer
  EXPECT_FALSE(c.reserveFunds(limitBuy(1, 100.0, 2.0), cfg, &l));
  EXPECT_EQ(c.creditReason(), RejectReason::CreditRefused);
  EXPECT_EQ(i64(l.reserved(1, USD)), 0);
  EXPECT_FALSE(c.contains(1));

  c.resetCreditReason();
  EXPECT_EQ(c.creditReason(), RejectReason::InsufficientFunds);
}

TEST(EngineCredit, CreditHookIsAskedWithNoLedgerToo)
{
  engine::Credit c;
  int asked = 0;
  c.setCreditCheck(
      [&](const CreditRequest&)
      {
        ++asked;
        return CreditDecision{false, RejectReason::CreditRefused};
      });
  EXPECT_FALSE(c.reserveFunds(limitBuy(1, 100.0, 2.0), spotCfg(), nullptr));
  EXPECT_EQ(asked, 1);
}

// ---- giving it back ------------------------------------------------------

TEST(EngineCredit, ReleaseReturnsEverythingAndForgetsTheOrder)
{
  engine::Credit c;
  Ledger l;
  const SymbolConfig cfg = spotCfg();
  l.deposit(1, USD, amt(1000));
  ASSERT_TRUE(c.reserveFunds(limitBuy(1, 100.0, 2.0), cfg, &l));
  ASSERT_EQ(i64(l.reserved(1, USD)), i64(amt(200)));

  c.releaseReservation(1, &l);
  EXPECT_EQ(i64(l.reserved(1, USD)), 0);
  EXPECT_EQ(i64(l.available(1, USD)), i64(amt(1000)));
  EXPECT_FALSE(c.contains(1));  // the entry goes with the money

  c.releaseReservation(1, &l);  // idempotent: an unknown id changes nothing
  EXPECT_EQ(i64(l.available(1, USD)), i64(amt(1000)));
}

TEST(EngineCredit, ReleaseExceptHeldKeepsTheSliceAnAcceptStillNeeds)
{
  engine::Credit c;
  Ledger l;
  const SymbolConfig cfg = spotCfg();
  l.deposit(1, USD, amt(1000));
  ASSERT_TRUE(c.reserveFunds(limitBuy(1, 100.0, 4.0), cfg, &l));
  ASSERT_EQ(i64(l.reserved(1, USD)), i64(amt(400)));

  c.releaseReservationExceptHeld(1, Quantity::fromDouble(1.0), cfg, &l);
  EXPECT_EQ(i64(l.reserved(1, USD)), i64(amt(100)));  // 1 @ the reservation's bound
  ASSERT_NE(c.find(1), nullptr);
  EXPECT_EQ(i64(c.find(1)->reservedRaw), i64(amt(100)));

  // Nothing held: the whole reservation goes, entry and all.
  c.releaseReservationExceptHeld(1, Quantity{}, cfg, &l);
  EXPECT_EQ(i64(l.reserved(1, USD)), 0);
  EXPECT_FALSE(c.contains(1));
}

TEST(EngineCredit, ReleaseProFreesTheFractionThatStoppedResting)
{
  engine::Credit c;
  Ledger l;
  const SymbolConfig cfg = spotCfg();
  l.deposit(1, USD, amt(1000));
  ASSERT_TRUE(c.reserveFunds(limitBuy(1, 100.0, 4.0), cfg, &l));

  // 4 down to 1: three quarters of the buying power is the account's again.
  c.releaseReservationPro(1, Quantity::fromDouble(4.0).raw(), Quantity::fromDouble(1.0).raw(), &l);
  EXPECT_EQ(i64(l.reserved(1, USD)), i64(amt(100)));
  EXPECT_EQ(i64(c.find(1)->reservedRaw), i64(amt(100)));

  // A shrink that is not a shrink frees nothing.
  c.releaseReservationPro(1, Quantity::fromDouble(1.0).raw(), Quantity::fromDouble(2.0).raw(), &l);
  EXPECT_EQ(i64(l.reserved(1, USD)), i64(amt(100)));
}

TEST(EngineCredit, PerpBackingForAQuantityIsInitialMarginAtTheBound)
{
  engine::Credit c;
  Ledger l;
  const SymbolConfig cfg = perpCfg();
  l.deposit(1, USD, amt(1000));
  ASSERT_TRUE(c.reserveFunds(limitBuy(1, 100.0, 4.0), cfg, &l));
  ASSERT_EQ(i64(l.reserved(1, USD)), i64(amt(40)));

  const engine::Credit::Reservation* r = c.find(1);
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(i64(c.backingFor(*r, Quantity::fromDouble(1.0), cfg)), i64(amt(10)));

  c.releaseReservationExceptHeld(1, Quantity::fromDouble(1.0), cfg, &l);
  EXPECT_EQ(i64(l.reserved(1, USD)), i64(amt(10)));
}

TEST(EngineCredit, TheTableIsPutErasedAndHandedOutWhole)
{
  engine::Credit c;
  engine::Credit::Reservation r{3, USD, amt(50), Price::fromDouble(100.0).raw(), Side::BUY};
  c.put(7, r);
  EXPECT_TRUE(c.contains(7));
  EXPECT_EQ(c.reservations().size(), 1u);
  EXPECT_EQ(i64(c.reservations().at(7).reservedRaw), i64(amt(50)));

  engine::Credit other;
  other.restoreReservations(c.reservations());
  EXPECT_TRUE(other.contains(7));

  c.erase(7);
  EXPECT_FALSE(c.contains(7));
  EXPECT_EQ(c.find(7), nullptr);
  EXPECT_TRUE(other.contains(7));  // the copy is the clone's own

  AdmissionProfile p;
  p.deny = AdmissionDeny::DenyQuote;
  c.setAdmissionProfile(4, p);
  other.restoreAdmission(c.admissionProfiles());
  EXPECT_TRUE(other.admissionDenies(4, AdmissionDeny::DenyQuote));
}

// ---- execution limits ----------------------------------------------------

TEST(EngineCredit, ReduceOnlyMayOnlyCloseWhatIsOpenOnTheOtherSide)
{
  engine::Credit c;
  SymbolConfig cfg = perpCfg();
  cfg.maxPositionQty = Quantity{};  // cap off: the reduce-only rule alone
  const int64_t want = Quantity::fromDouble(5.0).raw();
  CancelReason why = CancelReason::UserRequested;

  // Short 3: a BUY may reduce 3 of the 5 it wants.
  const int64_t shortPos = -Quantity::fromDouble(3.0).raw();
  EXPECT_EQ(c.legFillLimit(shortPos, Side::BUY, true, want, why, cfg),
            Quantity::fromDouble(3.0).raw());
  EXPECT_EQ(why, CancelReason::ReduceOnlyNotReducing);

  // Same position, same side, NOT reduce-only: nothing cuts it.
  why = CancelReason::UserRequested;
  EXPECT_EQ(c.legFillLimit(shortPos, Side::BUY, false, want, why, cfg), want);
  EXPECT_EQ(why, CancelReason::UserRequested);  // untouched: nothing was cut

  // Flat, or long already: a reduce-only BUY may do nothing at all.
  EXPECT_EQ(c.legFillLimit(0, Side::BUY, true, want, why, cfg), 0);
  EXPECT_EQ(c.legFillLimit(Quantity::fromDouble(3.0).raw(), Side::BUY, true, want, why, cfg), 0);

  // The mirror case: a reduce-only SELL closes a long.
  EXPECT_EQ(c.legFillLimit(Quantity::fromDouble(2.0).raw(), Side::SELL, true, want, why, cfg),
            Quantity::fromDouble(2.0).raw());
}

// The account's own position cap, where it is tighter than the
// symbol's, is the one a leg is measured against; an account with no entry
// keeps the symbol's; the symbol's binds where it is the tighter one.
TEST(EngineCredit, AnAccountsOwnPositionCapBindsWhereItIsTheTighterOne)
{
  engine::Credit c;
  SymbolConfig cfg = perpCfg();
  cfg.maxPositionQty = Quantity::fromDouble(10.0);
  SetAccountRiskLimits caps;
  caps.fields = AccountRiskLimitField::AccountRiskMaxPosition;
  caps.account = 7;
  caps.maxPositionQty = Quantity::fromDouble(2.0);
  c.setAccountLimits(caps);
  EXPECT_EQ(c.positionCapRaw(7, cfg), Quantity::fromDouble(2.0).raw());
  EXPECT_EQ(c.positionCapRaw(8, cfg), Quantity::fromDouble(10.0).raw());
  // A symbol with no cap of its own: the account's is the whole cap.
  SymbolConfig uncapped = cfg;
  uncapped.maxPositionQty = Quantity{};
  EXPECT_EQ(c.positionCapRaw(7, uncapped), Quantity::fromDouble(2.0).raw());
  EXPECT_EQ(c.positionCapRaw(8, uncapped), 0);
  const Quantity want = Quantity::fromDouble(5.0);
  // Account 7 flat, buying 5: two of them fit under its own cap.
  FillLimit a = c.pairFillLimit(0, Side::BUY, false, 0, Side::SELL, false, want, cfg, 7, 8);
  EXPECT_EQ(a.makerQty, Quantity::fromDouble(2.0));
  EXPECT_EQ(a.takerQty, want);
  EXPECT_EQ(a.qty, Quantity::fromDouble(2.0));
  EXPECT_EQ(a.reason, CancelReason::PositionLimitExceeded);
  // Selling builds a short under the same cap: 7 as the taker is cut to two.
  FillLimit b = c.pairFillLimit(0, Side::BUY, false, 0, Side::SELL, false, want, cfg, 8, 7);
  EXPECT_EQ(b.qty, Quantity::fromDouble(2.0));
  EXPECT_TRUE(!b.takerBlocked);
  // Two accounts with no entry of their own: the symbol's ten, so five fit.
  FillLimit d = c.pairFillLimit(0, Side::BUY, false, 0, Side::SELL, false, want, cfg, 8, 9);
  EXPECT_EQ(d.qty, want);
  // A wider account cap does not loosen the symbol's.
  caps.account = 8;
  caps.maxPositionQty = Quantity::fromDouble(50.0);
  c.setAccountLimits(caps);
  EXPECT_EQ(c.positionCapRaw(8, cfg), Quantity::fromDouble(10.0).raw());
}

TEST(EngineCredit, PositionCapMeasuresTheRESULTINGPositionNotTheOrder)
{
  engine::Credit c;
  SymbolConfig cfg = perpCfg();
  cfg.maxPositionQty = Quantity::fromDouble(10.0);
  CancelReason why = CancelReason::UserRequested;
  const int64_t want = Quantity::fromDouble(5.0).raw();

  // Long 8 of a cap of 10: two more, not five.
  EXPECT_EQ(c.legFillLimit(Quantity::fromDouble(8.0).raw(), Side::BUY, false, want, why, cfg),
            Quantity::fromDouble(2.0).raw());
  EXPECT_EQ(why, CancelReason::PositionLimitExceeded);

  // Selling from that same long moves away from the cap: nothing is cut.
  why = CancelReason::UserRequested;
  EXPECT_EQ(c.legFillLimit(Quantity::fromDouble(8.0).raw(), Side::SELL, false, want, why, cfg),
            want);

  // Already past the cap: the allowance floors at zero, never negative.
  EXPECT_EQ(c.legFillLimit(Quantity::fromDouble(12.0).raw(), Side::BUY, false, want, why, cfg), 0);
}

TEST(EngineCredit, PairFillLimitTakesTheTIGHTEROfTheTwoLegs)
{
  engine::Credit c;
  SymbolConfig cfg = perpCfg();
  cfg.maxPositionQty = Quantity::fromDouble(10.0);
  const Quantity want = Quantity::fromDouble(5.0);

  // Maker long 8 (2 left), taker flat (5 left) -> the maker's 2 is the answer.
  FillLimit a = c.pairFillLimit(Quantity::fromDouble(8.0).raw(), Side::BUY, false, 0, Side::SELL,
                                false, want, cfg);
  EXPECT_EQ(a.qty.raw(), Quantity::fromDouble(2.0).raw());
  EXPECT_EQ(a.makerQty.raw(), Quantity::fromDouble(2.0).raw());
  EXPECT_EQ(a.takerQty.raw(), want.raw());
  EXPECT_FALSE(a.makerBlocked);
  EXPECT_EQ(a.reason, CancelReason::PositionLimitExceeded);

  // The cut on the TAKER's side is just as binding: maker flat, taker long 9.
  FillLimit b = c.pairFillLimit(0, Side::SELL, false, Quantity::fromDouble(9.0).raw(), Side::BUY,
                                false, want, cfg);
  EXPECT_EQ(b.qty.raw(), Quantity::fromDouble(1.0).raw());
  EXPECT_EQ(b.makerQty.raw(), want.raw());
  EXPECT_EQ(b.takerQty.raw(), Quantity::fromDouble(1.0).raw());
  EXPECT_FALSE(b.takerBlocked);
  EXPECT_EQ(b.reason, CancelReason::PositionLimitExceeded);

  // Neither leg constrained: the whole bite stands.
  FillLimit c2 = c.pairFillLimit(0, Side::BUY, false, 0, Side::SELL, false, want, cfg);
  EXPECT_EQ(c2.qty.raw(), want.raw());
  EXPECT_FALSE(c2.makerBlocked);
  EXPECT_FALSE(c2.takerBlocked);
}

TEST(EngineCredit, WhenBothLegsAreBlockedTheMakerIsTheOneReported)
{
  engine::Credit c;
  SymbolConfig cfg = perpCfg();
  cfg.maxPositionQty = Quantity{};
  const Quantity want = Quantity::fromDouble(5.0);

  // Both reduce-only against a position that cannot be reduced: 0 and 0. The
  // maker is named, because the maker is the leg the matcher can pull.
  FillLimit f = c.pairFillLimit(0, Side::BUY, true, 0, Side::SELL, true, want, cfg);
  EXPECT_EQ(f.qty.raw(), 0);
  EXPECT_TRUE(f.makerBlocked);
  EXPECT_FALSE(f.takerBlocked);
  EXPECT_EQ(f.reason, CancelReason::ReduceOnlyNotReducing);
}
