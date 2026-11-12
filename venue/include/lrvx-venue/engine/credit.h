/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Who may send an order, and what it costs to have one live.
 *
 * Everything here answers a question about the ACCOUNT, not about the book:
 * is this counterparty entitled to send this order, does an external risk
 * owner allow it, what has to be ring-fenced in the ledger before it may
 * rest, what is given back when it stops resting, and how much of a
 * prospective fill each leg's perp risk limits still permit.
 *
 * None of that needs the resting book, so none of it is a template. The one
 * place the book does come into it -- the reduce-only quantity an account
 * already has resting -- is measured by the engine and passed in as a number,
 * the same way a position is: this component is handed state, it does not go
 * looking for it.
 */
#pragma once

#include "lrvx-venue/engine/snapshot_lock.h"
#include "lrvx-venue/engine/sorted_keys.h"
#include "lrvx-venue/engine/state_hash_tags.h"
#include "lrvx-venue/event_hash.h"
#include "lrvx-venue/journal.h"
#include "lrvx-venue/ledger.h"
#include "lrvx-venue/matcher.h"
#include "lrvx-venue/messages.h"
#include "lrvx-venue/reject_reason.h"
#include "lrvx-venue/symbol_config.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace lrvx::venue
{

// Pre-trade credit / buying-power gate. Returns true if the account may place
// the order. A real deployment binds this to the account/balance service.
// What an external risk owner is told about an order it must approve. The
// old shape (account, side, price, quantity) could not answer a portfolio
// question: it did not say WHICH instrument -- the engine knows its own, the
// risk layer serves many -- nor whether the order reduces exposure, and a
// bare false gave the client no reason for the refusal.
struct CreditRequest
{
  OrderId order{};
  uint64_t account{};
  SymbolId symbol{};
  Side side{};
  OrderType type{};
  Price price{};
  Quantity quantity{};
  bool reduceOnly{false};
};

struct CreditDecision
{
  bool allowed{true};
  RejectReason reason{RejectReason::InsufficientFunds};  // used when allowed == false
};

using CreditCheck = std::function<CreditDecision(const CreditRequest&)>;

namespace engine
{

// Entitlement, buying power and the per-fill risk allowance. Owns the
// admission table, the credit hook and the reservation table; borrows the
// instrument config and the ledger from the engine on each call rather than
// holding pointers to them, so a copy of this object has no way to reach
// another engine's state.
class Credit
{
 public:
  // One buying-power reservation: what was ring-fenced for one order, in
  // which asset, against which worst-case price. `limitPriceRaw` is the
  // price the reservation was sized at, so a fill can give back the
  // difference between the bound and the print.
  struct Reservation
  {
    uint64_t account{};
    AssetId asset{};
    Amount reservedRaw{};
    int64_t limitPriceRaw{};
    Side side{};
  };

  // ---- entitlement table ------------------------------------------------

  void setCreditCheck(CreditCheck c) { credit_ = std::move(c); }

  // Admission profile of one account. A default-constructed profile clears the
  // entry (back to "everything permitted"), which keeps the table canonical
  // for the checkpoint state hash.
  void setAdmissionProfile(uint64_t account, const AdmissionProfile& p)
  {
    std::lock_guard<SnapshotLock> lk(admissionLock_);
    if (p.allowedTypes == 0 && p.allowedTif == 0 && p.deny == 0)
    {
      admission_.erase(account);
      return;
    }
    admission_[account] = p;
  }

  // Handed out BY VALUE, under engine::SnapshotLock: the /metrics thread
  // samples this table (docs/venue/perimeter.md) while the consumer is free to
  // retune an account, and a reference into the live map would put a rehash
  // under a reader's iterator. The engine's own paths read admission_ directly
  // -- they are the consumer thread, and the consumer never races itself.
  std::unordered_map<uint64_t, AdmissionProfile> admissionProfiles() const
  {
    std::lock_guard<SnapshotLock> lk(admissionLock_);
    return admission_;
  }

  // Orders refused because the sender was not entitled to send them. Non-zero
  // means a counterparty is sending what it may not -- visible immediately
  // rather than a week later as a position nobody can explain.
  uint64_t admissionRejects() const noexcept { return admissionRejects_; }

  // Counted by the caller that emits the reject, because the same refusal
  // reaches the client from three entry paths (order, quote, modify) and only
  // the caller knows which event carries it.
  void countAdmissionReject() noexcept { ++admissionRejects_; }

  // ---- gates ------------------------------------------------------------

  // Entitlement gate. Every admission path consults this before anything else
  // decides the order's fate, so a counterparty cannot reach the book through
  // a path that forgot to ask. Declared in scripts/check_gate_reachability.py,
  // which fails the build if a path stops calling it.
  //
  // `auctionMode` is the engine's, passed in: a call auction rests everything,
  // which changes the answer for a counterparty that may not rest.
  RejectReason admissionGate(const NewOrder& o, bool auctionMode) const
  {
    auto it = admission_.find(o.accountId);
    if (it == admission_.end())
    {
      return RejectReason::None;  // no profile: everything permitted
    }
    const AdmissionProfile& p = it->second;
    // DenyNewOrder is deliberately NOT checked here: admissionGate
    // runs for every NewOrder onNew sees, including the synthetic legs a
    // Quote/QuoteLadder builds for itself (engine/quote.h), and a
    // quotes-only profile must deny exactly the client's OWN NewOrder
    // submissions while leaving its Quote/QuoteLadder legs untouched -- the
    // two are the same struct by the time they get here. See onNew, which
    // checks DenyNewOrder itself, gated on clOrdIdChecked (false only for a
    // genuine top-level NewOrder).
    // allowedTypes and allowedTif are 32-bit bitmaps indexed by the enum
    // value, so a field carrying a value the enum does not name has no bit
    // to test -- and shifting a 32-bit word by 32 or more is undefined, not
    // merely a wrong answer. The refusal is the same either way: a profile
    // that lists what it permits cannot have listed a value that does not
    // exist. Checked here rather than left to validate(), because admission
    // runs first and this is the shift.
    if (p.allowedTypes != 0 &&
        (!inRange(o.type) || (p.allowedTypes & (1u << static_cast<uint32_t>(o.type))) == 0))
    {
      return RejectReason::OrderTypeNotPermitted;
    }
    // Tested before the TIF list so the answer names the policy rather than the
    // field: "you may not leave an order resting" tells the counterparty what
    // to change, "this time in force is not allowed" leaves it guessing which
    // of the allowed ones is safe.
    //
    // Refused on admission, not killed on the way out: an order that could rest
    // must never be accepted from a counterparty that does not track resting
    // orders. POST_ONLY exists only to rest; GTC and GTD outlive the message
    // that carried them.
    //
    // A call auction rests EVERYTHING it admits -- there is no matching to be
    // immediate about, so an IOC accumulates like any other order and sits in
    // the book until the uncross. A counterparty that does not track resting
    // orders therefore cannot participate in one at all: its order would sit
    // there for the length of the auction while it believes the order either
    // filled or died on arrival, and at the uncross it can trade against that
    // counterparty's own other side.
    if ((p.deny & AdmissionDeny::DenyResting) != 0 &&
        (auctionMode || o.tif == TimeInForce::GTC || o.tif == TimeInForce::GTD ||
         o.tif == TimeInForce::POST_ONLY || o.postOnly))
    {
      return RejectReason::RestingNotPermitted;
    }
    if (p.allowedTif != 0 &&
        (!inRange(o.tif) || (p.allowedTif & (1u << static_cast<uint32_t>(o.tif))) == 0))
    {
      return RejectReason::TimeInForceNotPermitted;
    }
    return RejectReason::None;
  }

  bool admissionDenies(uint64_t account, uint8_t bit) const
  {
    auto it = admission_.find(account);
    return it != admission_.end() && (it->second.deny & bit) != 0;
  }

  // Instrument conformance for a conditional order: the trigger is a price and
  // the size is a size, so tick, band and lot apply exactly as they do to a
  // resting limit -- with the same reasons, so a client handles an off-grid
  // stop the way it handles an off-grid limit. State and duplicate checks
  // already ran before this point.
  RejectReason validateConditional(const NewOrder& o, const SymbolConfig& cfg) const
  {
    if (o.quantity.raw() <= 0)
    {
      return RejectReason::InvalidQuantity;
    }
    if (!cfg.lotSize.isZero() && (o.quantity.raw() % cfg.lotSize.raw()) != 0)
    {
      return RejectReason::LotSizeViolation;
    }
    const int64_t trig = o.triggerPrice.raw();
    if (trig > 0)
    {
      if (!cfg.tickSize.isZero() && (trig % cfg.tickSize.raw()) != 0)
      {
        return RejectReason::TickSizeViolation;
      }
      if (!cfg.minPrice.isZero() && trig < cfg.minPrice.raw())
      {
        return RejectReason::InvalidPrice;
      }
      if (!cfg.maxPrice.isZero() && trig > cfg.maxPrice.raw())
      {
        return RejectReason::InvalidPrice;
      }
    }
    // A stop-LIMIT also carries the limit price it will rest at.
    if (o.type == OrderType::STOP_LIMIT || o.type == OrderType::TAKE_PROFIT_LIMIT)
    {
      if (!cfg.tickSize.isZero() && (o.price.raw() % cfg.tickSize.raw()) != 0)
      {
        return RejectReason::TickSizeViolation;
      }
      if (!cfg.minPrice.isZero() && o.price.raw() < cfg.minPrice.raw())
      {
        return RejectReason::InvalidPrice;
      }
      if (!cfg.maxPrice.isZero() && o.price.raw() > cfg.maxPrice.raw())
      {
        return RejectReason::InvalidPrice;
      }
    }
    return RejectReason::None;
  }

  // ---- buying power -----------------------------------------------------

  Amount imForRaw(int64_t qtyRaw, int64_t priceRaw, const SymbolConfig& cfg) const
  {
    const Amount notional = notionalRaw(priceRaw, qtyRaw, cfg.priceScale, cfg.qtyScale);
    return notional * cfg.initialMarginBps / 10000;
  }

  bool reserveFunds(const NewOrder& o, const SymbolConfig& cfg, Ledger* ledger)
  {
    // Permission first, funding second. An external risk owner answers a
    // question the engine cannot ("is this account good for it across every
    // instrument it holds?"), so it has to be asked whether or not this engine
    // also posts collateral -- both money branches below used to return before
    // the hook was ever reached, so binding a ledger silently disabled it.
    if (credit_)
    {
      const CreditDecision d = credit_(CreditRequest{o.id, o.accountId, cfg.id, o.side, o.type,
                                                     o.price, o.quantity, o.reduceOnly});
      if (!d.allowed)
      {
        creditReason_ = d.reason;  // surfaced by the caller's reject
        return false;
      }
    }
    if (ledger != nullptr && cfg.linearPerp)
    {
      // Derivatives: reserve initial margin in quote collateral (reduce-only
      // reserves nothing -- it frees position margin instead).
      //
      // An unpriced (market / triggered-stop) order is bounded by the price
      // band, and for a linear perp that bound is the band's TOP on BOTH sides.
      // Nothing is delivered here: the exposure is notional, and notional --
      // hence initial margin -- grows with price whether the account is long or
      // short. Bounding a perp SELL at minPrice (which is the correct worst case
      // for a SPOT seller, who delivers base and whose quote proceeds only grow
      // with price -- see the spot branch below) under-reserves it by the whole
      // maxPrice/minPrice ratio, and consumeOrderIM then caps the position's
      // margin at that under-reserved number.
      const int64_t limitRaw = (o.type == OrderType::LIMIT) ? o.price.raw() : cfg.maxPrice.raw();
      // A market/stop order with no price band (limitRaw == 0) cannot be bounded
      // for margin: reserving 0 IM would let it open a position with no
      // collateral. Reject rather than admit an uncollateralized fill.
      if (!o.reduceOnly && o.type != OrderType::LIMIT && limitRaw <= 0)
      {
        return false;
      }
      const Amount im = o.reduceOnly ? 0 : imForRaw(o.quantity.raw(), limitRaw, cfg);
      if (im > 0 && !ledger->reserve(o.accountId, cfg.quoteAsset, im))
      {
        return false;
      }
      reserve_[o.id] = Reservation{o.accountId, cfg.quoteAsset, im, limitRaw, o.side};
      return true;
    }
    if (ledger != nullptr)
    {
      AssetId asset;
      Amount amt;
      int64_t limitRaw;
      if (o.side == Side::BUY)
      {
        const Price px = (o.type == OrderType::LIMIT || cfg.maxPrice.raw() == 0) ? o.price
                                                                                 : cfg.maxPrice;
        // A market/stop buy with no price band (px == 0) cannot bound its quote
        // spend, so reserving amountOf(0) would gate nothing and let the fill
        // drive the account's reserved balance negative. Reject it instead.
        if (px.raw() <= 0)
        {
          return false;
        }
        asset = cfg.quoteAsset;
        amt = notionalRaw(px.raw(), o.quantity.raw(), cfg.priceScale, cfg.qtyScale);
        limitRaw = px.raw();
      }
      else
      {
        asset = cfg.baseAsset;
        amt = amountOf(o.quantity);
        limitRaw = o.price.raw();
      }
      if (!ledger->reserve(o.accountId, asset, amt))
      {
        return false;
      }
      reserve_[o.id] = Reservation{o.accountId, asset, amt, limitRaw, o.side};
      return true;
    }
    return true;
  }

  // Reason from the last refused credit check, so the reject the client sees
  // says why ("portfolio margin", say) instead of a flat InsufficientFunds.
  // Read by the caller that emits the reject, then reset for the next order.
  RejectReason creditReason() const noexcept { return creditReason_; }
  void resetCreditReason() noexcept { creditReason_ = RejectReason::InsufficientFunds; }

  void releaseReservation(OrderId id, Ledger* ledger)
  {
    if (ledger == nullptr)
    {
      return;
    }
    auto it = reserve_.find(id);
    if (it == reserve_.end())
    {
      return;
    }
    if (it->second.reservedRaw > 0)
    {
      ledger->release(it->second.account, it->second.asset, it->second.reservedRaw);
    }
    reserve_.erase(it);
  }

  // What backing a quantity carries under one reservation: initial margin on a
  // perp, quote at the reservation's bound for a spot buy, base for a sell.
  Amount backingFor(const Reservation& r, Quantity qty, const SymbolConfig& cfg) const
  {
    if (cfg.linearPerp)
    {
      return imForRaw(qty.raw(), r.limitPriceRaw, cfg);
    }
    if (r.side == Side::BUY)
    {
      return notionalRaw(r.limitPriceRaw, qty.raw(), cfg.priceScale, cfg.qtyScale);
    }
    return amountOf(qty);
  }

  // releaseReservation, but keep the slice backing `heldQty` reserved: a
  // canceled residual must not strip the collateral an open last-look accept
  // still needs to settle from (the unchecked-debit fallback would credit the
  // counterparty against a possibly failing debit). `heldQty` is measured by
  // the engine, which owns the hold table.
  void releaseReservationExceptHeld(OrderId id, Quantity heldQty, const SymbolConfig& cfg,
                                    Ledger* ledger)
  {
    if (ledger == nullptr)
    {
      return;
    }
    if (heldQty.isZero())
    {
      releaseReservation(id, ledger);
      return;
    }
    auto it = reserve_.find(id);
    if (it == reserve_.end())
    {
      return;
    }
    Amount keep = backingFor(it->second, heldQty, cfg);
    if (keep > it->second.reservedRaw)
    {
      keep = it->second.reservedRaw;
    }
    const Amount rel = it->second.reservedRaw - keep;
    if (rel > 0)
    {
      ledger->release(it->second.account, it->second.asset, rel);
      it->second.reservedRaw = keep;
    }
  }

  // Free the reservation covering the quantity an order just lost. Any path
  // that shrinks a resting order owes this: buying power held against size
  // that no longer rests is the account's money, frozen for nothing.
  void releaseReservationPro(OrderId id, int64_t fromQtyRaw, int64_t toQtyRaw, Ledger* ledger)
  {
    if (ledger == nullptr || fromQtyRaw <= 0 || toQtyRaw >= fromQtyRaw)
    {
      return;
    }
    auto it = reserve_.find(id);
    if (it == reserve_.end())
    {
      return;
    }
    const Amount freed = static_cast<Amount>(static_cast<__int128>(it->second.reservedRaw) *
                                             (fromQtyRaw - toQtyRaw) / fromQtyRaw);
    if (freed <= 0)
    {
      return;
    }
    ledger->release(it->second.account, it->second.asset, freed);
    it->second.reservedRaw -= freed;
  }

  // ---- reservation table ------------------------------------------------
  //
  // Handed out because clearing, settlement and last look each adjust one
  // entry they already know the id of. The traversal that reaches something
  // observable -- the checkpoint's, over SORTED keys -- is this component's
  // own; see the checkpoint section below.

  const std::unordered_map<OrderId, Reservation>& reservations() const noexcept
  {
    return reserve_;
  }

  Reservation* find(OrderId id) noexcept
  {
    auto it = reserve_.find(id);
    return it == reserve_.end() ? nullptr : &it->second;
  }

  const Reservation* find(OrderId id) const noexcept
  {
    auto it = reserve_.find(id);
    return it == reserve_.end() ? nullptr : &it->second;
  }

  bool contains(OrderId id) const noexcept { return reserve_.count(id) != 0; }

  void put(OrderId id, const Reservation& r) { reserve_[id] = r; }

  void erase(OrderId id) { reserve_.erase(id); }

  // Snapshot clone only: copy the two tables that belong in the clone, and
  // nothing else. The credit hook, the last refusal reason and the reject
  // counter are not engine state a snapshot carries.
  void restoreReservations(const std::unordered_map<OrderId, Reservation>& m) { reserve_ = m; }

  void restoreAdmission(const std::unordered_map<uint64_t, AdmissionProfile>& m)
  {
    std::lock_guard<SnapshotLock> lk(admissionLock_);
    admission_ = m;
  }

  // Per-account limits. A record carries the fields its mask
  // names; the rest of the account's entry stays. An account with no entry
  // is bound by the symbol's limits alone.
  struct AccountLimits
  {
    Quantity maxOrderQty{};     // 0 = unchecked
    Volume maxOrderNotional{};  // 0 = unchecked
    uint32_t maxOpenOrders{0};  // 0 = unchecked
    Quantity maxPositionQty{};  // 0 = unchecked
  };
  void setAccountLimits(const SetAccountRiskLimits& r)
  {
    if (r.fields == 0)
    {
      return;
    }
    AccountLimits& l = accountLimits_[r.account];
    if ((r.fields & AccountRiskLimitField::AccountRiskFatFinger) != 0)
    {
      l.maxOrderQty = r.maxOrderQty;
      l.maxOrderNotional = r.maxOrderNotional;
    }
    if ((r.fields & AccountRiskLimitField::AccountRiskMaxOpenOrders) != 0)
    {
      l.maxOpenOrders = r.maxOpenOrders;
    }
    if ((r.fields & AccountRiskLimitField::AccountRiskMaxPosition) != 0)
    {
      l.maxPositionQty = r.maxPositionQty;
    }
  }
  const AccountLimits* accountLimits(uint64_t account) const noexcept
  {
    auto it = accountLimits_.find(account);
    return it == accountLimits_.end() ? nullptr : &it->second;
  }
  const std::unordered_map<uint64_t, AccountLimits>& accountLimitsTable() const noexcept
  {
    return accountLimits_;
  }
  void restoreAccountLimits(const std::unordered_map<uint64_t, AccountLimits>& m)
  {
    accountLimits_ = m;
  }
  uint64_t hashAccountLimits(uint64_t h) const
  {
    for (uint64_t acct : sortedKeysOf(accountLimits_))
    {
      const AccountLimits& l = accountLimits_.at(acct);
      h = mix(h, hash_tags::kAccountRiskLimits);
      h = mix(h, acct);
      h = mix(h, static_cast<uint64_t>(l.maxOrderQty.raw()));
      h = mix(h, static_cast<uint64_t>(l.maxOrderNotional.raw()));
      h = mix(h, l.maxOpenOrders);
      h = mix(h, static_cast<uint64_t>(l.maxPositionQty.raw()));
    }
    return h;
  }
  // The snapshot half: one record per account, every field carried, so the
  // loader rebuilds the entry whole through the live submit path.
  void writeAccountLimits(Journal& out, SymbolId symbol, int64_t ts) const
  {
    for (uint64_t acct : sortedKeysOf(accountLimits_))
    {
      const AccountLimits& l = accountLimits_.at(acct);
      SetAccountRiskLimits r;
      r.symbol = symbol;
      r.fields = AccountRiskLimitField::AccountRiskFatFinger |
                 AccountRiskLimitField::AccountRiskMaxOpenOrders |
                 AccountRiskLimitField::AccountRiskMaxPosition;
      r.account = acct;
      r.maxOrderQty = l.maxOrderQty;
      r.maxOrderNotional = l.maxOrderNotional;
      r.maxOpenOrders = l.maxOpenOrders;
      r.maxPositionQty = l.maxPositionQty;
      out.append(InboundCommand{r}, ts);
    }
  }
  // The position cap that binds `account` on this symbol: the tighter of the
  // symbol's and the account's, 0 when neither is set.
  int64_t positionCapRaw(uint64_t account, const SymbolConfig& cfg) const noexcept
  {
    const int64_t sym = cfg.maxPositionQty.raw();
    const AccountLimits* l = accountLimits(account);
    const int64_t acct = l == nullptr ? 0 : l->maxPositionQty.raw();
    if (sym <= 0)
    {
      return acct;
    }
    if (acct <= 0)
    {
      return sym;
    }
    return acct < sym ? acct : sym;
  }

  // ---- checkpoint: credit's own records ----------------------------------
  // The admission table and the reservation table are hashed and written at
  // four different points of the engine's traversal -- admission leads the
  // config section and the reservations close the file -- so these are two
  // pairs of methods rather than one. The record order, the tags and the
  // bytes are what the engine wrote inline.
  uint64_t hashAdmission(uint64_t h) const
  {
    for (uint64_t acct : sortedKeysOf(admission_))
    {
      const AdmissionProfile& p = admission_.at(acct);
      h = mix(h, hash_tags::kAdmissionProfile);
      h = mix(h, acct);
      h = mix(h, p.allowedTypes);
      h = mix(h, p.allowedTif);
      h = mix(h, static_cast<uint64_t>(p.deny));
    }
    return h;
  }

  uint64_t hashReservations(uint64_t h) const
  {
    for (OrderId id : sortedKeysOf(reserve_))
    {
      const Reservation& r = reserve_.at(id);
      h = mix(h, hash_tags::kReservation);
      h = mix(h, id);
      h = mix(h, r.account);
      h = mix(h, r.asset);
      h = mix(h, static_cast<uint64_t>(r.side));
      h = mix(h, static_cast<uint64_t>(r.limitPriceRaw));
      h = mixAmount(h, r.reservedRaw);
    }
    return h;
  }

  // Admission profiles are engine state re-emitted as the command that set
  // them, applied through the ordinary submit path on load: they decide what
  // is accepted, so a recovered engine that lost them would accept orders the
  // live one refused.
  void writeAdmission(Journal& out, SymbolId symbol, int64_t ts) const
  {
    for (uint64_t acct : sortedKeysOf(admission_))
    {
      out.append(InboundCommand{SetAdmissionProfile{symbol, {}, acct, admission_.at(acct)}}, ts);
    }
  }

  // The exact live amounts, not a formula re-derivation: they are
  // history-dependent (partial fills, held slices, STP), so nothing downstream
  // could reconstruct them.
  void writeReservations(Journal& out, int64_t ts) const
  {
    for (OrderId id : sortedKeysOf(reserve_))
    {
      const Reservation& r = reserve_.at(id);
      out.append(InboundCommand{RestoreReservation{id, r.account, r.asset, {}, r.side, r.limitPriceRaw, r.reservedRaw}},
                 ts);
    }
  }

  // The credit half of the snapshot clone: the two tables, and nothing else.
  void copyStateFrom(const Credit& other)
  {
    std::lock_guard<SnapshotLock> lk(admissionLock_);
    admission_ = other.admission_;
    accountLimits_ = other.accountLimits_;
    reserve_ = other.reserve_;
  }

  // ---- execution limits -------------------------------------------------

  // How much of one leg's prospective fill the perp risk limits still allow,
  // measured against `posQtyRaw` -- the position that leg holds at the moment
  // the question is asked, which on a dry run is the position plus whatever
  // the sweep has already planned for it. `reason` names the limit that cut it
  // (meaningful only when the result is below `want`).
  // The symbol's cap alone: what every caller that names no account gets.
  int64_t legFillLimit(int64_t posQtyRaw, Side side, bool reduceOnly, int64_t want,
                       CancelReason& reason, const SymbolConfig& cfg) const
  {
    return legFillLimit(posQtyRaw, side, reduceOnly, want, reason, cfg, cfg.maxPositionQty.raw());
  }
  int64_t legFillLimit(int64_t posQtyRaw, Side side, bool reduceOnly, int64_t want,
                       CancelReason& reason, const SymbolConfig& cfg, int64_t capRaw) const
  {
    (void)cfg;  // the cap arrives resolved (positionCapRaw): the symbol's, the account's, or the tighter
    int64_t allowed = want;
    if (reduceOnly)
    {
      // A reduce-only order may only close what is open on the other side. Its
      // reserved IM is 0 by construction, so any part of it that opened a
      // position would open it with NO margin at all.
      const int64_t reducible = (side == Side::BUY && posQtyRaw < 0)    ? -posQtyRaw
                                : (side == Side::SELL && posQtyRaw > 0) ? posQtyRaw
                                                                        : 0;
      if (reducible < allowed)
      {
        allowed = reducible;
        reason = CancelReason::ReduceOnlyNotReducing;
      }
    }
    if (capRaw > 0)
    {
      // Room left before the RESULTING position breaches the cap. Checking the
      // incoming order alone (the admission gate) lets several orders, each
      // under the cap, settle into a position past it.
      const int64_t room = (side == Side::BUY) ? capRaw - posQtyRaw : capRaw + posQtyRaw;
      if (room < allowed)
      {
        allowed = room;
        reason = CancelReason::PositionLimitExceeded;
      }
    }
    return allowed < 0 ? 0 : allowed;
  }

  // legFillLimit over both legs of one prospective bite. Used by the fill-time
  // re-check, by the all-or-none precheck (which asks against positions the
  // sweep has planned but not printed) and by the auction uncross, where both
  // legs are resting orders and neither is an incoming order.
  FillLimit pairFillLimit(int64_t makerPosQtyRaw, Side makerSide, bool makerReduceOnly,
                          int64_t takerPosQtyRaw, Side takerSide, bool takerReduceOnly,
                          Quantity want, const SymbolConfig& cfg) const
  {
    return pairFillLimit(makerPosQtyRaw, makerSide, makerReduceOnly, takerPosQtyRaw, takerSide,
                         takerReduceOnly, want, cfg, /*makerAccount=*/0, /*takerAccount=*/0);
  }
  FillLimit pairFillLimit(int64_t makerPosQtyRaw, Side makerSide, bool makerReduceOnly,
                          int64_t takerPosQtyRaw, Side takerSide, bool takerReduceOnly,
                          Quantity want, const SymbolConfig& cfg, uint64_t makerAccount,
                          uint64_t takerAccount) const
  {
    FillLimit out;
    out.qty = want;
    out.makerQty = want;
    out.takerQty = want;
    CancelReason makerReason = CancelReason::ReduceOnlyNotReducing;
    CancelReason takerReason = CancelReason::ReduceOnlyNotReducing;
    const int64_t makerAllowed = legFillLimit(makerPosQtyRaw, makerSide, makerReduceOnly, want.raw(),
                                              makerReason, cfg, positionCapRaw(makerAccount, cfg));
    const int64_t takerAllowed = legFillLimit(takerPosQtyRaw, takerSide, takerReduceOnly, want.raw(),
                                              takerReason, cfg, positionCapRaw(takerAccount, cfg));
    out.makerQty = Quantity::fromRaw(makerAllowed);
    out.takerQty = Quantity::fromRaw(takerAllowed);
    // The maker is the leg reported as blocked when both are: it is the one the
    // matcher can act on (pull it from the book) without killing an aggressor
    // that may still trade elsewhere.
    if (makerAllowed <= takerAllowed)
    {
      out.qty = out.makerQty;
      out.makerBlocked = makerAllowed <= 0;
      out.reason = makerReason;
    }
    else
    {
      out.qty = out.takerQty;
      out.takerBlocked = takerAllowed <= 0;
      out.reason = takerReason;
    }
    return out;
  }

 private:
  // Amount is __int128; mix() takes 64 bits at a time. Spelled out here, as
  // in clearing.h, so the component stays standalone.
  static uint64_t mixAmount(uint64_t h, Amount a) noexcept
  {
    h = mix(h, static_cast<uint64_t>(static_cast<unsigned __int128>(a)));
    h = mix(h, static_cast<uint64_t>(static_cast<unsigned __int128>(a) >> 64));
    return h;
  }

  CreditCheck credit_;

  RejectReason creditReason_{RejectReason::InsufficientFunds};

  // account -> admission profile. Empty table and absent entries both mean
  // "everything permitted", so an engine that was never given profiles behaves
  // exactly as before.
  std::unordered_map<uint64_t, AdmissionProfile> admission_;
  // Mutable because taking a snapshot is a const operation on the engine.
  mutable SnapshotLock admissionLock_;
  std::unordered_map<uint64_t, AccountLimits> accountLimits_;

  uint64_t admissionRejects_{0};  // observability: a counterparty sending what it may not

  std::unordered_map<OrderId, Reservation> reserve_;
};

}  // namespace engine
}  // namespace lrvx::venue
