/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

// MatchingEngine<Book>: pre-trade validation, admission, risk gates and order entry.
//
// Included only from lrvx-venue/matching_engine.h, which declares every member
// defined here. Including it directly gives a fragment with no class to attach
// to, so the include is refused rather than left to fail on the first method.
#ifndef LRVX_VENUE_MATCHING_ENGINE_INL
#error "lrvx-venue/engine/validate.inl is a fragment of lrvx-venue/matching_engine.h; include that instead"
#endif

namespace lrvx::venue
{

template <class Book>
void MatchingEngine<Book>::setCreditCheck(CreditCheck c)
{
  credit_.setCreditCheck(std::move(c));
}

template <class Book>
void MatchingEngine<Book>::setAdmissionProfile(uint64_t account, const AdmissionProfile& p)
{
  credit_.setAdmissionProfile(account, p);
}

template <class Book>
std::unordered_map<uint64_t, AdmissionProfile> MatchingEngine<Book>::admissionProfiles() const
{
  return credit_.admissionProfiles();
}

template <class Book>
uint64_t MatchingEngine<Book>::admissionRejects() const noexcept
{
  return credit_.admissionRejects();
}

template <class Book>
RejectReason MatchingEngine<Book>::validateConditional(const NewOrder& o) const
{
  return credit_.validateConditional(o, cfg_);
}

// Whether the instrument is in a state that accepts order entry at all, and
// the refusal if it is not. Outermost first: a client whose order is refused
// deserves the reason that tells it what to do next. Delisted means do not
// come back; closed means next session; halted means something is wrong with
// the instrument now.
//
// Order-shaped commands that do not route through validate() ask this
// directly: a quote replaces two resting orders, so it has to know the
// replacements are admissible BEFORE it pulls what is there.
template <class Book>
RejectReason MatchingEngine<Book>::instrumentStateRefusal() const
{
  if (session_.delisted())
  {
    return RejectReason::InstrumentDelisted;
  }
  if (session_.closed())
  {
    return RejectReason::MarketClosed;
  }
  if (cfg_.halted)
  {
    return RejectReason::Halted;
  }
  return RejectReason::None;
}

template <class Book>
RejectReason MatchingEngine<Book>::validate(const NewOrder& o) const
{
  if (o.symbol != cfg_.id)
  {
    return RejectReason::UnknownSymbol;
  }
  if (const RejectReason r = instrumentStateRefusal(); r != RejectReason::None)
  {
    return r;
  }
  // Before any gate keyed on the type. Everything below that asks about a
  // price -- the tick, the band, the fat-finger notional -- is written as
  // `o.type == OrderType::LIMIT`, so a type this build does not name passed
  // every one of them and rested at whatever price the submission carried.
  // Turning the checks around (run them unless the type is MARKET) would
  // only move the guess: the answer is that the venue does not match an
  // order type it cannot name, whichever side of the comparison it is on.
  if (!inRange(o.type))
  {
    return RejectReason::UnknownOrderType;
  }
  if (o.quantity.raw() <= 0)
  {
    return RejectReason::InvalidQuantity;
  }
  // Pro-rata allocation does not honour last look (a held slice cannot be
  // carved out of a proportional round), so the combination is refused at
  // admission instead of silently filling the maker as firm.
  if (o.lastLook && matcher_.policy() == MatchPolicy::ProRata)
  {
    return RejectReason::LastLookUnsupported;
  }
  // A peg needs a tick for the same kind of reason. The never-cross clamp
  // keeps a tracking order strictly inside the price it tracks, and the tick
  // is the only distance it has to step back by; tickSize 0 means "unchecked"
  // everywhere else in the config, but here it makes the clamp land on the
  // opposite touch itself -- and repeg() re-rests the order through
  // addResting, which runs no matching pass, so the instrument quotes
  // bid == ask and nobody can trade out of it. Refused at admission, where a
  // refusal still costs nothing and the owner gets a reason it can act on.
  if (o.peg != PegRef::None && cfg_.tickSize.isZero())
  {
    return RejectReason::PegRequiresTick;
  }
  if (!cfg_.minQty.isZero() && o.quantity < cfg_.minQty)
  {
    return RejectReason::InvalidQuantity;
  }
  if (!cfg_.lotSize.isZero() && (o.quantity.raw() % cfg_.lotSize.raw()) != 0)
  {
    return RejectReason::LotSizeViolation;
  }
  if (!cfg_.maxOrderQty.isZero() && cfg_.maxOrderQty < o.quantity)
  {
    return RejectReason::OrderTooLarge;  // fat-finger size
  }
  // The account's own caps, refused the same way the symbol's
  // are: an owner of risk above the venue that tightened one account gets
  // the refusal it asked for, and a replay gets it too.
  const auto* acctLimits = credit_.accountLimits(o.accountId);
  if (acctLimits != nullptr && !acctLimits->maxOrderQty.isZero() && acctLimits->maxOrderQty < o.quantity)
  {
    return RejectReason::OrderTooLarge;  // the account's fat-finger size
  }
  if (acctLimits != nullptr && o.type == OrderType::LIMIT && !acctLimits->maxOrderNotional.isZero() &&
      static_cast<Amount>(acctLimits->maxOrderNotional.raw()) <
          notionalRaw(o.price.raw(), o.quantity.raw(), cfg_.priceScale, cfg_.qtyScale))
  {
    return RejectReason::OrderTooLarge;  // the account's fat-finger notional
  }
  // Notional at the SYMBOL's scale, the same arithmetic reservations, margin
  // and fees use. `quantity * price` reads both raws under the compile-time
  // 1e8 scale instead, so on a symbol that declared its own the gate compares
  // a number that is off by (1e8/priceScale) * (1e8/qtyScale) -- coarser
  // scales turn the fat-finger check off, finer ones reject everything.
  // Bit-identical to the old expression at the default 1e8/1e8.
  if (o.type == OrderType::LIMIT && !cfg_.maxOrderNotional.isZero() &&
      static_cast<Amount>(cfg_.maxOrderNotional.raw()) <
          notionalRaw(o.price.raw(), o.quantity.raw(), cfg_.priceScale, cfg_.qtyScale))
  {
    return RejectReason::OrderTooLarge;  // fat-finger notional
  }
  if (o.type == OrderType::LIMIT)
  {
    if (o.price.raw() <= 0)
    {
      return RejectReason::InvalidPrice;
    }
    if (!cfg_.tickSize.isZero() && (o.price.raw() % cfg_.tickSize.raw()) != 0)
    {
      return RejectReason::TickSizeViolation;
    }
    if (!cfg_.minPrice.isZero() && o.price < cfg_.minPrice)
    {
      return RejectReason::InvalidPrice;
    }
    if (!cfg_.maxPrice.isZero() && cfg_.maxPrice < o.price)
    {
      return RejectReason::InvalidPrice;
    }
    // The book's own band, which the collar above knows nothing about: the
    // collar is optional config, the ladder's geometry is not. Asked here,
    // before the order is committed to matching, so a price the book cannot
    // represent is refused while refusing still costs nothing -- by the time
    // addResting sees it the order may already have printed.
    if (!book_.canRest(o.price))
    {
      return RejectReason::InvalidPrice;
    }
  }
  if (book_.contains(o.id) || stops_.contains(o.id))
  {
    return RejectReason::DuplicateOrderId;
  }
  // An id referenced by an active last-look hold is still live even when its
  // order is (fully held) out of the book: a reject will restore quantity
  // under that id, so a reused id would merge two unrelated orders.
  if (hasHoldsFor(o.id))
  {
    return RejectReason::DuplicateOrderId;
  }
  return RejectReason::None;
}

// The book's last node and its band, asked in the one place every resting
// path goes through. full() is the book's own answer about the node pool and
// is consulted first: it decides without touching anything, and it is what
// the book's own comment always said the engine should ask.
template <class Book>
RejectReason MatchingEngine<Book>::restOnBook(Side side, const RestingOrder& ro)
{
  // Price first, so a book that is both full and out of band answers for the
  // thing the owner can act on.
  if (!book_.canRest(ro.price))
  {
    return RejectReason::InvalidPrice;
  }
  if (book_.full())
  {
    return RejectReason::BookCapacityExceeded;
  }
  switch (book_.addResting(side, ro))
  {
    case BookAddResult::Accepted:
      return RejectReason::None;
    case BookAddResult::PriceOutOfBand:
      // The client's price, and nothing the venue can do about it -- the same
      // answer the collar gives for a price it will not take.
      return RejectReason::InvalidPrice;
    case BookAddResult::PoolExhausted:
      break;
  }
  return RejectReason::BookCapacityExceeded;
}

// Per-order perp risk gates shared by onNew and the trigger path (a triggered
// stop re-enters matching directly, NOT through onNew, so it must run these too
// -- otherwise a reduce-only stop opens an uncollateralized position and a
// plain stop bypasses the position cap). Caps a reduce-only order to the
// opposing position (0 reducible -> reject: nothing to reduce) and rejects a
// fill that would breach maxPositionQty. Mutates o.quantity for the cap.
// Entitlement gate. Every admission path consults this before anything else
// decides the order's fate, so a counterparty cannot reach the book through
// a path that forgot to ask. Declared in scripts/check_gate_reachability.py,
// which fails the build if a path stops calling it. The rules live in
// engine::Credit; the auction flag is the engine's, so it is handed over.
template <class Book>
RejectReason MatchingEngine<Book>::admissionGate(const NewOrder& o) const
{
  return credit_.admissionGate(o, session_.auction());
}

template <class Book>
bool MatchingEngine<Book>::admissionDenies(uint64_t account, uint8_t bit) const
{
  return credit_.admissionDenies(account, bit);
}

template <class Book>
RejectReason MatchingEngine<Book>::perpRiskGate(NewOrder& o)
{
  if (!cfg_.linearPerp)
  {
    return RejectReason::None;
  }
  if (o.reduceOnly)
  {
    const int64_t posQ = clearing_.positionQty(o.accountId);
    int64_t reducible = (o.side == Side::BUY && posQ < 0)    ? -posQ
                        : (o.side == Side::SELL && posQ > 0) ? posQ
                                                             : 0;
    // The account's reduce-only orders already resting on this side are
    // queued against the SAME position. Sizing each one against the whole
    // position lets N of them rest against a position only one of them can
    // close, so the book advertises reduce-only depth that cannot trade --
    // and an all-or-none taker that believed the depth gets a partial print.
    reducible -= restingReduceOnlyRaw(o.accountId, o.side, o.id);
    if (reducible < 0)
    {
      reducible = 0;
    }
    if (o.quantity.raw() > reducible)
    {
      o.quantity = Quantity::fromRaw(reducible);
    }
    if (o.quantity.raw() <= 0)
    {
      return RejectReason::InvalidQuantity;  // nothing to reduce -> reduce-only never opens
    }
  }
  if (!cfg_.maxPositionQty.isZero())
  {
    const int64_t posQ = clearing_.positionQty(o.accountId);
    const int64_t worst = posQ + (o.side == Side::BUY ? o.quantity.raw() : -o.quantity.raw());
    if (iabs64(worst) > cfg_.maxPositionQty.raw())
    {
      return RejectReason::PositionLimitExceeded;
    }
  }
  return RejectReason::None;
}

// Total reduce-only quantity the account already has resting on `side`
// (displayed peak plus hidden reserve), ignoring `exclude` -- the order being
// re-admitted on the modify and stop-trigger paths.
template <class Book>
int64_t MatchingEngine<Book>::restingReduceOnlyRaw(uint64_t account, Side side, OrderId exclude) const
{
  const auto* own = pub_.ordersOf(account);
  if (own == nullptr)
  {
    return 0;
  }
  const std::unordered_set<OrderId>& ids = *own;
  int64_t sum = 0;
  // order: not observable -- int64 sum of the account's reduce-only leaves
  for (OrderId id : ids)
  {
    if (id == exclude)
    {
      continue;
    }
    const RestingOrder* r = book_.find(id);
    if (r != nullptr && r->reduceOnly && r->side == side)
    {
      sum += r->leaves.raw() + r->hidden.raw();
    }
  }
  return sum;
}

// legFillLimit against the position the account holds RIGHT NOW (plus
// whatever a dry run has already planned for it). Resolving the position is
// the engine's half; the limits themselves are engine::Credit's.
template <class Book>
int64_t MatchingEngine<Book>::legFillLimit(uint64_t account, Side side, bool reduceOnly, int64_t want,
                                           CancelReason& reason, int64_t posDeltaRaw) const
{
  const int64_t posQ = clearing_.positionQty(account) + posDeltaRaw;
  return credit_.legFillLimit(posQ, side, reduceOnly, want, reason, cfg_,
                              credit_.positionCapRaw(account, cfg_));
}

// Fill-time risk re-check for one prospective bite (see the FillLimit hook).
// reduceOnly and maxPositionQty are gated at submit / stop-trigger / modify,
// but a RESTING order fills later, against a position that has since moved:
// a reduce-only order whose position shrank would open or flip it (with zero
// margin -- reduce-only reserves none), and orders that each passed the cap
// individually would settle past it together. Both legs are therefore
// re-measured here, on live engine state, so a replay reproduces it.
template <class Book>
FillLimit MatchingEngine<Book>::fillLimit(const RestingOrder& maker, const NewOrder& taker, Quantity want) const
{
  if (ledger_ == nullptr)
  {
    // No ledger: the engine keeps no positions to measure against.
    return FillLimit{want, want, want, false, false, CancelReason::ReduceOnlyNotReducing};
  }
  return pairFillLimit(maker.accountId, maker.side, maker.reduceOnly, taker.accountId, taker.side,
                       taker.reduceOnly, want);
}

// fillLimit against a state the sweep has not reached yet: `deltas` holds the
// position change each account would already have taken on. Used by the
// all-or-none precheck, which has to ask about every maker in the crossing
// range as though the earlier ones had already printed.
template <class Book>
FillLimit MatchingEngine<Book>::fillLimitDry(const RestingOrder& maker, const NewOrder& taker, Quantity want,
                                             const PositionDeltas& deltas) const
{
  if (ledger_ == nullptr)
  {
    return FillLimit{want, want, want, false, false, CancelReason::ReduceOnlyNotReducing};
  }
  return pairFillLimit(maker.accountId, maker.side, maker.reduceOnly, taker.accountId, taker.side,
                       taker.reduceOnly, want, deltas.of(maker.accountId),
                       deltas.of(taker.accountId));
}

// fillLimit over two legs described directly -- used by the auction uncross,
// where BOTH legs are resting orders and neither is an incoming NewOrder.
template <class Book>
FillLimit MatchingEngine<Book>::pairFillLimit(uint64_t makerAcct, Side makerSide, bool makerReduceOnly,
                                              uint64_t takerAcct, Side takerSide, bool takerReduceOnly, Quantity want,
                                              int64_t makerPosDeltaRaw, int64_t takerPosDeltaRaw) const
{
  const int64_t makerPosQ = clearing_.positionQty(makerAcct) + makerPosDeltaRaw;
  const int64_t takerPosQ = clearing_.positionQty(takerAcct) + takerPosDeltaRaw;
  return credit_.pairFillLimit(makerPosQ, makerSide, makerReduceOnly, takerPosQ, takerSide,
                               takerReduceOnly, want, cfg_, makerAcct, takerAcct);
}

// True if `clOrdId` was already used by `account` inside the dedup window
// (see engine/clordid_window.h) and the caller must therefore refuse the
// submission; false and newly registered otherwise. Factored out of onNew so
// a Quote -- one submission that becomes two child orders (see onQuote) --
// can register its clientOrderId ONCE for both legs instead of racing
// itself: the second leg's onNew would otherwise find the first leg's insert
// already sitting in the current generation and refuse a submission that
// never repeated anything.
template <class Book>
bool MatchingEngine<Book>::clOrdIdDuplicate(uint64_t account, uint64_t clOrdId)
{
  return clOrdIds_.duplicate(account, clOrdId, now_.raw(), cfg_.clOrdIdWindowNs);
}

// `clOrdIdChecked` is true only for the two calls onQuote makes on behalf
// of one Quote's legs: the dedup registration already happened once, for
// the quote as a whole, before either leg was built.
template <class Book>
void MatchingEngine<Book>::onNew(NewOrder o, bool clOrdIdChecked)
{
  // Quotes-only admission: DenyNewOrder refuses a genuine client
  // NewOrder while leaving a Quote/QuoteLadder's own legs untouched, even
  // though both arrive here as the same NewOrder struct -- clOrdIdChecked is
  // true ONLY for a quote's legs (see quote_mmp.inl: their dedup was already
  // registered once, for the whole quote, before onNew was ever called), so
  // it doubles as exactly the signal needed here. Checked before
  // admissionGate rather than inside it: admissionGate runs for both kinds
  // of caller and has no other way to tell them apart.
  if (!clOrdIdChecked && admissionDenies(o.accountId, AdmissionDeny::DenyNewOrder))
  {
    credit_.countAdmissionReject();
    sink_(OrderRejected{o.id, o.symbol, RejectReason::NewOrderNotPermitted, o.accountId,
                        o.clientOrderId});
    return;
  }
  // Entitlement first: an order the counterparty may not send should not
  // consume a clientOrderId, link an OCO group or reach any later gate.
  if (const RejectReason r = credit_.admissionGate(o, session_.auction()); r != RejectReason::None)
  {
    credit_.countAdmissionReject();
    sink_(OrderRejected{o.id, o.symbol, r, o.accountId, o.clientOrderId});
    return;
  }
  // clientOrderId dedup (per account, window = engine session/uptime; see
  // docs/venue/matching.md). Registered on receipt, BEFORE any other gate:
  // a resend of an already-seen clOrdId must reject deterministically even
  // when the first instance has long filled or canceled -- that is the whole
  // point (double execution after an ambiguous disconnect). Replay-safe: the
  // index is rebuilt by the same submits during journal replay.
  if (!clOrdIdChecked && clOrdIdDuplicate(o.accountId, o.clientOrderId))
  {
    sink_(OrderRejected{o.id, o.symbol, RejectReason::DuplicateClientOrderId, o.accountId, o.clientOrderId});
    return;
  }
  if (o.ocoGroup > 0)
  {
    oco_.link(o.id, o.ocoGroup);  // link before matching so a taker fill triggers OCO too
  }
  // Any early exit before the order commits (parks as a stop, or passes every
  // gate and reaches matching/resting) must unlink it from its OCO group --
  // otherwise a rejected leg lingers in the group and later cancels a reused
  // id. `committed` is set once the order is live; the guard cleans up the rest.
  //
  // The guard covers every gate ABOVE the commit point. Three refusals live
  // below it -- the LULD band, the matcher's own out.reject and the zero-fill
  // residual cancel -- and each unlinks for itself, at the point where it
  // decides the order is not going to live after all.
  bool committed = false;
  struct OcoCleanup
  {
    MatchingEngine* self;
    OrderId id;
    bool linked;
    const bool* committed;
    ~OcoCleanup()
    {
      if (linked && !*committed)
      {
        self->oco_.unlink(id);
      }
    }
  } ocoCleanup{this, o.id, o.ocoGroup > 0, &committed};
  // POST_ONLY is spelled two ways -- a time in force and a flag -- and the
  // matcher reads only the flag. Every wire decoder sets the flag, so this
  // normalization exists for the in-process caller that chose the other
  // spelling and would otherwise get an ordinary aggressor.
  if (o.tif == TimeInForce::POST_ONLY)
  {
    o.postOnly = true;
  }
  if (o.peg != PegRef::None)
  {
    o.type = OrderType::LIMIT;  // a peg is a passive limit priced off the book
    o.price = Price::fromRaw(pegTargetRaw(o.side, o.peg, o.pegOffsetRaw));
  }
  if (isConditional(o.type))
  {
    // Conditional orders branch off before validate(), which is written for
    // an order carrying a live limit price. They still have a price (the
    // trigger) and a quantity, and both must obey the instrument -- an
    // off-tick or out-of-band trigger, or a size below the minimum or off
    // the lot, used to be parked happily and only surfaced when the stop
    // fired.
    if (const RejectReason r = credit_.validateConditional(o, cfg_); r != RejectReason::None)
    {
      sink_(OrderRejected{o.id, o.symbol, r, o.accountId, o.clientOrderId});
      return;
    }
    committed = onStop(o);  // parked in the stop book -> committed (keep OCO link)
    return;
  }
  // The tighter of the symbol's and the account's open-order cap.
  uint32_t openCap = cfg_.maxOpenOrders;
  if (const auto* al = credit_.accountLimits(o.accountId); al != nullptr && al->maxOpenOrders > 0)
  {
    openCap = openCap == 0 ? al->maxOpenOrders : (al->maxOpenOrders < openCap ? al->maxOpenOrders : openCap);
  }
  if (openCap > 0)
  {
    // Ingress DoS / risk gate: cap live resting orders per account. Once at
    // the cap the account must cancel before adding more.
    if (pub_.accountOrderCount(o.accountId) >= openCap)
    {
      sink_(OrderRejected{o.id, o.symbol, RejectReason::TooManyOpenOrders, o.accountId, o.clientOrderId});
      return;
    }
  }
  // Perp risk gate (reduce-only cap + position-limit check). The single
  // source of truth shared with the triggered-stop and modify paths -- see
  // perpRiskGate. Runs BEFORE validate so the fat-finger size check sees the
  // reduce-only-capped quantity, matching the pre-refactor behaviour. Spot:
  // a no-op.
  if (const RejectReason r = perpRiskGate(o); r != RejectReason::None)
  {
    sink_(OrderRejected{o.id, o.symbol, r, o.accountId, o.clientOrderId});
    return;
  }
  if (const RejectReason r = validate(o); r != RejectReason::None)
  {
    sink_(OrderRejected{o.id, o.symbol, r, o.accountId, o.clientOrderId});
    return;
  }
  if (!credit_.reserveFunds(o, cfg_, ledger_))
  {
    sink_(OrderRejected{o.id, o.symbol, credit_.creditReason(), o.accountId, o.clientOrderId});
    credit_.resetCreditReason();  // reset for the next order
    return;                       // pre-trade buying-power (ledger reservation or credit hook)
  }
  committed = true;  // past all reject gates: the order will match/rest, and any
                     // OCO resolution is now owned by processOco / forgetOrder.

  if (session_.auction())
  {
    // Pre-open / auction: accumulate without matching (a crossed book is
    // allowed). Market orders rest at the band edge so they always execute in
    // the uncross.
    Price restPx = o.price;
    if (o.type == OrderType::MARKET)
    {
      restPx = (o.side == Side::BUY) ? cfg_.maxPrice : cfg_.minPrice;
    }
    // Everything an order carries into continuous trading it also carries
    // into an auction. The uncross prices on displayed-plus-hidden depth, so
    // an iceberg's peak was never load-bearing for price discovery -- the one
    // thing the peak does is limit what the public feed sees, and that is
    // exactly what a book entry built without it gives away. A good-till-date
    // order that never reaches the expiry book never expires at all, in the auction
    // or after it.
    RestingOrder ro{o.id, o.accountId, restPx, o.quantity, o.side};
    ro.clientOrderId = o.clientOrderId;
    ro.reduceOnly = o.reduceOnly;
    ro.postOnly = o.postOnly;
    ro.lastLook = o.lastLook && cfg_.lastLookWindowNs.count() > 0;
    if (o.visibleQuantity.raw() > 0 && o.visibleQuantity < o.quantity)
    {
      ro.peak = o.visibleQuantity;
      ro.leaves = o.visibleQuantity;
      ro.hidden = o.quantity - o.visibleQuantity;
    }
    // An auction accumulates without matching, so nothing has printed and a
    // book that will not take the order can still refuse it outright: the
    // reservation goes back, the order is never tracked, and clearing
    // `committed` lets the OCO guard unlink it the way the earlier gates do.
    if (const RejectReason why = restOnBook(o.side, ro); why != RejectReason::None)
    {
      committed = false;
      releaseReservation(o.id);
      sink_(OrderRejected{o.id, o.symbol, why, o.accountId, o.clientOrderId});
      return;
    }
    trackResting(o.id, o.accountId, o.stp);
    if (o.tif == TimeInForce::GTD && static_cast<bool>(o.expiryNs))
    {
      expiry_.set(o.id, o.expiryNs);
    }
    if (o.peg != PegRef::None)
    {
      pegs_.set(o.id, PegBook::Peg{o.side, o.peg, o.pegOffsetRaw});
    }
    // Pre-open accumulation never matches, so this order has filled
    // nothing yet -- cumQty is always 0.
    sink_(OrderAccepted{o.id, o.symbol, o.side, restPx, o.quantity, true, ro.leaves, o.accountId,
                        o.clientOrderId, Quantity{}});
    return;
  }

  // LULD band, captured from the pre-trade reference (matching below moves the
  // last price). Absent before the first trade -- documented pre-first-trade
  // window where no band exists yet.
  int64_t luldLo = 0, luldHi = 0;
  const bool luldOn = luldBand(luldLo, luldHi);

  if (luldOn && o.type != OrderType::MARKET)
  {
    // A limit order priced outside the band is rejected pre-trade and trips the
    // pause -- it never executes or rests out of band.
    if ((o.side == Side::BUY && luldHi < o.price.raw()) ||
        (o.side == Side::SELL && o.price.raw() < luldLo))
    {
      releaseReservation(o.id);
      oco_.unlink(o.id);  // refused, so it never joined the group it was linked into
      sink_(OrderRejected{o.id, o.symbol, RejectReason::LuldBreach, o.accountId, o.clientOrderId});
      tripLuldHalt();
      return;
    }
  }

  const MatchOutcome out =
      matcher_.cross(o, book_, [this]()
                     { return ++tradeSeq_; }, emit_);
  stampFreshHolds();

  if (out.reject != RejectReason::None)
  {
    releaseReservation(o.id);  // post-only-would-cross / FOK-unfulfillable: free the reserve
    oco_.unlink(o.id);         // and it never joined the group it was linked into
    sink_(OrderRejected{o.id, o.symbol, out.reject, o.accountId, o.clientOrderId});
    return;
  }
  if (out.residualRests)
  {
    RestingOrder ro{o.id, o.accountId, o.price, out.leaves, o.side};
    ro.clientOrderId = o.clientOrderId;
    ro.lastLook = o.lastLook && cfg_.lastLookWindowNs.count() > 0;  // window 0 = feature off
    ro.reduceOnly = o.reduceOnly;                                   // carried so a later modify preserves it
    ro.postOnly = o.postOnly;                                       // same reason
    // This order may have partially filled itself (as aggressor)
    // before its residual rests -- out.filled is that fill. Stamped onto the
    // RestingOrder now so a later report (cancel, exec, modify) on this
    // order carries the real running total instead of starting over at 0.
    ro.cumQty = out.filled;
    if (o.visibleQuantity.raw() > 0 && o.visibleQuantity < out.leaves)
    {
      ro.peak = o.visibleQuantity;    // iceberg: show a peak, hide the rest
      ro.leaves = o.visibleQuantity;  // displayed
      ro.hidden = out.leaves - o.visibleQuantity;
    }
    if (const RejectReason why = restOnBook(o.side, ro); why != RejectReason::None)
    {
      // The book would not take the residual. What the owner is told depends
      // on whether this order printed on the way in: with nothing filled it is
      // an ordinary reject, but after a trade a reject would contradict the
      // executions already on the wire, so the residual is canceled the way an
      // IOC residual is. Either way the order is on no book and is not tracked.
      if (out.filled.isZero())
      {
        committed = false;
        releaseReservation(o.id);
        sink_(OrderRejected{o.id, o.symbol, why, o.accountId, o.clientOrderId});
      }
      else
      {
        releaseReservationExceptHeld(o.id);
        sink_(OrderCanceled{o.id, o.symbol, CancelReason::BookRefused, o.accountId,
                            o.clientOrderId, out.leaves, out.filled});
      }
    }
    else
    {
      trackResting(o.id, o.accountId, o.stp);
      if (o.tif == TimeInForce::GTD && static_cast<bool>(o.expiryNs))
      {
        expiry_.set(o.id, o.expiryNs);
      }
      if (o.peg != PegRef::None)
      {
        pegs_.set(o.id, PegBook::Peg{o.side, o.peg, o.pegOffsetRaw});
      }
      // Public feed shows only the displayed peak (ro.leaves); the hidden
      // iceberg reserve (out.leaves - ro.leaves) is not leaked. Non-iceberg:
      // they match.
      sink_(OrderAccepted{o.id, o.symbol, o.side, o.price, out.leaves, true, ro.leaves,
                          o.accountId, o.clientOrderId, out.filled});
    }
  }
  else if (out.residualCanceled)
  {
    // The unfilled residual (IOC/FOK/MARKET/STP-newest) never rests, so its
    // buying-power reservation must be released here -- this raw-sink path does
    // not run the emit_ wrapper's release-on-cancel. Held slices stay
    // reserved: their accept still has to settle (reject releases later).
    releaseReservationExceptHeld(o.id);
    // The order is gone and was never tracked, so no forgetOrder will ever run
    // for it: this is its only chance to leave the group. Whether it filled
    // first is decided already -- the print is in oco_'s pending list and
    // processOco still cancels the siblings, this id simply is not one of them.
    oco_.unlink(o.id);
    sink_(OrderCanceled{o.id, o.symbol, out.residualCancelReason, o.accountId, o.clientOrderId,
                        out.leaves, out.filled});
  }

  processTriggers();  // this order's trades may have crossed resting stops

  // Post-trade LULD: a MARKET order (no limit to gate it pre-trade) can sweep
  // the book and print outside the band, and a stop cascade in processTriggers
  // can too. The breaching print stands, but it trips the volatility pause so
  // subsequent trading halts -- the exchange-style volatility interruption. A
  // limit order cannot breach here: it never trades through its own in-band
  // limit, so this condition is naturally false for it.
  if (luldOn && hasLast_ && (lastPrice_.raw() > luldHi || lastPrice_.raw() < luldLo))
  {
    tripLuldHalt();
  }
}

// LULD band [loRaw, hiRaw] around the current last price, computed in 128-bit
// to avoid int64 overflow on a high-priced instrument and clamped so the upper
// bound cannot overflow. Returns false when LULD is off or there is no last
// price yet (no reference -> no band).
template <class Book>
bool MatchingEngine<Book>::luldBand(int64_t& loRaw, int64_t& hiRaw) const
{
  if (cfg_.luldBps <= 0 || !hasLast_)
  {
    return false;
  }
  const __int128 prod =
      static_cast<__int128>(lastPrice_.raw()) * static_cast<__int128>(cfg_.luldBps) / 10000;
  const int64_t maxBand = INT64_MAX - lastPrice_.raw();
  const int64_t band = prod > static_cast<__int128>(maxBand) ? maxBand : static_cast<int64_t>(prod);
  loRaw = lastPrice_.raw() - band;
  hiRaw = lastPrice_.raw() + band;
  return true;
}

template <class Book>
void MatchingEngine<Book>::tripLuldHalt()
{
  applySession(engine::SessionEvent::LuldBreach, now_ + cfg_.luldHaltNs);
}

}  // namespace lrvx::venue
