/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Session state and the funding calendar as ENGINE STATE: a closed session that
 * is not a halt, a funding rate that survives a checkpoint, and a next-funding
 * boundary that is a fact the operator sets rather than a formula over config.
 * Each of the three is driven through all four paths it has to hold on --
 * live command, published transition, journal replay, checkpoint -- plus the
 * wire, where the new status value must not disturb a reader of the previous
 * schema.
 */
#include "lrvx-venue/event_hash.h"
#include "lrvx-venue/fix_md_codec.h"
#include "lrvx-venue/journal.h"
#include "lrvx-venue/market_data.h"
#include "lrvx-venue/matching_book.h"
#include "lrvx-venue/matching_engine.h"
#include "lrvx-venue/sbe_md_codec.h"
#include "support/tmp_path.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace lrvx;
using namespace lrvx::venue;
using lrvx::venue::test::tmpPath;

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
constexpr int64_t SEC = 1'000'000'000LL;

Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }

SymbolConfig cfg()
{
  SymbolConfig c;
  c.id = SYM;
  c.tickSize = px(0.01);
  c.minPrice = px(1.0);
  c.maxPrice = px(1000.0);
  return c;
}

SymbolConfig perpCfg()
{
  SymbolConfig c = cfg();
  c.linearPerp = true;
  c.initialMarginBps = 1000;
  c.fundingIntervalNs = DurationNs{8 * SEC};  // the config-derived calendar, kept as fallback
  return c;
}

NewOrder limit(OrderId id, Side s, double p, double q, uint64_t acct = 1)
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

InboundCommand admin(AdminAction a) { return InboundCommand{AdminCmd{SYM, a}}; }

// Engine plus everything it emitted, folded into the determinism digest as it
// goes -- the same digest the reproducibility tests compare on.
struct Eng
{
  std::vector<OutboundEvent> out;
  uint64_t hash{0};
  MatchingEngine<MatchingBook> eng;

  explicit Eng(SymbolConfig c = cfg())
      : eng(c, [this](const OutboundEvent& e)
            {
              out.push_back(e);
              hash = hashEvent(hash, e); })
  {
  }

  const TradingStatusChanged* lastStatus() const
  {
    const TradingStatusChanged* found = nullptr;
    for (const OutboundEvent& e : out)
    {
      if (const auto* s = std::get_if<TradingStatusChanged>(&e))
      {
        found = s;
      }
    }
    return found;
  }
  const DerivativesUpdated* lastDerivatives() const
  {
    const DerivativesUpdated* found = nullptr;
    for (const OutboundEvent& e : out)
    {
      if (const auto* d = std::get_if<DerivativesUpdated>(&e))
      {
        found = d;
      }
    }
    return found;
  }
  size_t statusCount() const
  {
    size_t n = 0;
    for (const OutboundEvent& e : out)
    {
      n += std::holds_alternative<TradingStatusChanged>(e) ? 1 : 0;
    }
    return n;
  }
  const OrderRejected* lastReject() const
  {
    const OrderRejected* found = nullptr;
    for (const OutboundEvent& e : out)
    {
      if (const auto* r = std::get_if<OrderRejected>(&e))
      {
        found = r;
      }
    }
    return found;
  }
};

// Write a snapshot of `src` and load it into a fresh engine; returns the loader
// (every record must apply, SnapshotEnd re-verifies the state hash) and the
// records, so a test can assert what the file does and does not carry.
struct Restored
{
  std::vector<std::pair<int64_t, InboundCommand>> records;
  bool allApplied{true};
};

Restored roundTrip(const MatchingEngine<MatchingBook>& src, MatchingEngine<MatchingBook>& dst,
                   const std::string& path)
{
  std::remove(path.c_str());
  {
    Journal out(path, Journal::Sync::Off, Journal::OpenMode::Truncate);
    src.writeSnapshot(out);
    out.flush();
  }
  Restored r;
  r.records = Journal::loadTimed(path);
  for (const auto& [ts, cmd] : r.records)
  {
    r.allApplied = dst.applySnapshotRecord(cmd, ts) && r.allApplied;
  }
  std::remove(path.c_str());
  return r;
}

bool carriesFunding(const Restored& r)
{
  for (const auto& [ts, cmd] : r.records)
  {
    (void)ts;
    if (std::holds_alternative<RestoreFunding>(cmd))
    {
      return true;
    }
  }
  return false;
}

// ------------------------------------------------------------------- session

// A closed session rejects with its own reason. Halted stays what it was: the
// two states are not interchangeable, and a client can tell "come back next
// session" from "something is wrong with this instrument".
void test_closed_rejects_with_its_own_reason()
{
  std::printf("test_closed_rejects_with_its_own_reason\n");
  Eng e;
  e.eng.submit(InboundCommand{limit(1, Side::SELL, 100, 5)}, 10);
  CHECK(e.eng.tradingStatus() == TradingStatus::Trading);

  e.eng.submit(admin(AdminAction::CloseSession), 20);
  CHECK(e.eng.tradingStatus() == TradingStatus::Closed);
  CHECK(e.eng.sessionClosed());

  e.eng.submit(InboundCommand{limit(2, Side::BUY, 99, 1, 2)}, 30);
  CHECK(e.lastReject() != nullptr && e.lastReject()->reason == RejectReason::MarketClosed);

  // A conditional order takes the same door.
  NewOrder stop = limit(3, Side::SELL, 0, 1, 2);
  stop.type = OrderType::STOP_MARKET;
  stop.triggerPrice = px(95);
  e.eng.submit(InboundCommand{stop}, 40);
  CHECK(e.lastReject()->id == 3 && e.lastReject()->reason == RejectReason::MarketClosed);

  // A halt is still a halt: the two reject reasons stay distinct.
  Eng h;
  h.eng.submit(admin(AdminAction::Halt), 10);
  h.eng.submit(InboundCommand{limit(1, Side::BUY, 99, 1)}, 20);
  CHECK(h.lastReject() != nullptr && h.lastReject()->reason == RejectReason::Halted);

  // Closing does NOT pull the book, and a cancel still works while closed --
  // a client must be able to get out of a position it cannot add to.
  CHECK(e.eng.book().find(1) != nullptr);
  e.eng.submit(InboundCommand{CancelOrder{1, SYM, {}, 1}}, 50);
  CHECK(e.eng.book().find(1) == nullptr);

  // Reopening restores matching.
  e.eng.submit(admin(AdminAction::OpenSession), 60);
  CHECK(e.eng.tradingStatus() == TradingStatus::Trading);
  e.eng.submit(InboundCommand{limit(4, Side::BUY, 99, 1, 2)}, 70);
  CHECK(e.eng.book().find(4) != nullptr);
}

// The transition reaches the feed, once per transition, with the session
// reason -- and the halt underneath a close survives it.
void test_close_is_a_published_transition()
{
  std::printf("test_close_is_a_published_transition\n");
  Eng e;
  e.eng.submit(admin(AdminAction::CloseSession), 10);
  CHECK(e.statusCount() == 1);
  CHECK(e.lastStatus()->status == TradingStatus::Closed);
  CHECK(e.lastStatus()->reason == TradingStatusReason::Session);
  CHECK(e.lastStatus()->untilNs == 0);  // a session close has no deadline

  e.eng.submit(admin(AdminAction::CloseSession), 20);
  CHECK(e.statusCount() == 1);  // not a transition: no duplicate

  e.eng.submit(admin(AdminAction::OpenSession), 30);
  CHECK(e.statusCount() == 2 && e.lastStatus()->status == TradingStatus::Trading);

  // Halt, then close over it: the feed shows the outermost state, and the halt
  // is still there when the session reopens.
  Eng g;
  g.eng.submit(admin(AdminAction::Halt), 10);
  CHECK(g.lastStatus()->status == TradingStatus::Halted);
  g.eng.submit(admin(AdminAction::CloseSession), 20);
  CHECK(g.lastStatus()->status == TradingStatus::Closed);
  g.eng.submit(admin(AdminAction::OpenSession), 30);
  CHECK(g.lastStatus()->status == TradingStatus::Halted);
  CHECK(g.eng.config().halted);
}

// Replay reproduces the session state and the event stream bit for bit, and
// the closed state changes the state hash (it is state, not a memo).
void test_session_replays_through_the_journal()
{
  std::printf("test_session_replays_through_the_journal\n");
  const std::string path = tmpPath("venue_session_replay", ".journal");
  std::remove(path.c_str());

  const std::vector<std::pair<int64_t, InboundCommand>> stream{
      {10, InboundCommand{limit(1, Side::SELL, 100, 5)}},
      {20, admin(AdminAction::CloseSession)},
      {30, InboundCommand{limit(2, Side::BUY, 100, 5, 2)}},  // rejected: MarketClosed
      {40, admin(AdminAction::OpenSession)},
      {50, InboundCommand{limit(3, Side::BUY, 100, 5, 2)}},  // trades
  };

  Eng live;
  {
    Journal j(path, Journal::Sync::Off, Journal::OpenMode::Truncate);
    for (const auto& [ts, c] : stream)
    {
      j.append(c, ts);
      live.eng.submit(c, ts);
    }
    j.flush();
  }

  Eng replayed;
  for (const auto& [ts, c] : Journal::loadTimed(path))
  {
    replayed.eng.submit(c, ts);
  }
  CHECK(replayed.hash == live.hash);
  CHECK(replayed.eng.stateHash() == live.eng.stateHash());
  CHECK(replayed.eng.tradingStatus() == TradingStatus::Trading);
  CHECK(replayed.eng.tradesGenerated() == 1);  // only the post-reopen order matched

  // The same stream without the close is a different run: the session state is
  // hashed, so a divergence here cannot hide.
  Eng noClose;
  for (const auto& [ts, c] : stream)
  {
    if (std::holds_alternative<AdminCmd>(c))
    {
      continue;
    }
    noClose.eng.submit(c, ts);
  }
  CHECK(noClose.hash != live.hash);
  std::remove(path.c_str());
}

// A checkpoint carries the session state: an engine that went down closed comes
// back closed, and still refuses orders for the right reason.
void test_session_survives_a_checkpoint()
{
  std::printf("test_session_survives_a_checkpoint\n");
  Eng src;
  src.eng.submit(InboundCommand{limit(1, Side::SELL, 100, 5)}, 10);
  src.eng.submit(admin(AdminAction::CloseSession), 20);

  Eng dst;
  const Restored r = roundTrip(src.eng, dst.eng, tmpPath("venue_session_ckpt", ".snap"));
  CHECK(r.allApplied);  // SnapshotEnd re-verified the state hash
  CHECK(dst.eng.stateHash() == src.eng.stateHash());
  CHECK(dst.eng.tradingStatus() == TradingStatus::Closed);
  CHECK(dst.eng.book().find(1) != nullptr);  // the book stood through the close

  dst.eng.submit(InboundCommand{limit(2, Side::BUY, 99, 1, 2)}, 30);
  CHECK(dst.lastReject() != nullptr && dst.lastReject()->reason == RejectReason::MarketClosed);

  // Halt underneath a close survives the round trip too, in the right order.
  Eng halted;
  halted.eng.submit(admin(AdminAction::Halt), 10);
  halted.eng.submit(admin(AdminAction::CloseSession), 20);
  Eng back;
  CHECK(roundTrip(halted.eng, back.eng, tmpPath("venue_session_ckpt2", ".snap")).allApplied);
  CHECK(back.eng.stateHash() == halted.eng.stateHash());
  CHECK(back.eng.tradingStatus() == TradingStatus::Closed);
  back.eng.submit(admin(AdminAction::OpenSession), 30);
  CHECK(back.eng.tradingStatus() == TradingStatus::Halted);
}

// ------------------------------------------------------------------- funding

// The rate is a fact the venue published; it must not evaporate on a restart.
void test_funding_rate_survives_a_checkpoint()
{
  std::printf("test_funding_rate_survives_a_checkpoint\n");
  Eng src(perpCfg());
  src.eng.submit(InboundCommand{SetMark{SYM, {}, px(100)}}, 1 * SEC);
  src.eng.submit(InboundCommand{ApplyFunding{SYM, {}, 0.0001, px(100)}}, 2 * SEC);
  CHECK(src.eng.fundingRateRaw() == kFundingRateScale / 10'000);

  Eng dst(perpCfg());
  const Restored r = roundTrip(src.eng, dst.eng, tmpPath("venue_funding_ckpt", ".snap"));
  CHECK(r.allApplied);
  CHECK(carriesFunding(r));
  CHECK(dst.eng.stateHash() == src.eng.stateHash());
  CHECK(dst.eng.fundingRateRaw() == src.eng.fundingRateRaw());

  // And the restored engine PUBLISHES it: the first mark after recovery carries
  // the real rate, not a zero placeholder that lies until the next settlement.
  dst.eng.submit(InboundCommand{SetMark{SYM, {}, px(101)}}, 3 * SEC);
  CHECK(dst.lastDerivatives() != nullptr);
  CHECK(dst.lastDerivatives()->fundingRateRaw == kFundingRateScale / 10'000);
}

// Read compatibility: a snapshot from an engine with no funding state at all
// carries no RestoreFunding record -- the exact file shape that existed before
// the record did -- and still loads, restoring rate 0 and the config fallback.
void test_snapshot_without_funding_record_still_loads()
{
  std::printf("test_snapshot_without_funding_record_still_loads\n");
  Eng src(perpCfg());
  src.eng.submit(InboundCommand{limit(1, Side::SELL, 100, 5)}, 1 * SEC);

  Eng dst(perpCfg());
  const Restored r = roundTrip(src.eng, dst.eng, tmpPath("venue_funding_old", ".snap"));
  CHECK(r.allApplied);
  CHECK(!carriesFunding(r));  // nothing to record -> the pre-record file shape
  CHECK(dst.eng.stateHash() == src.eng.stateHash());
  CHECK(dst.eng.fundingRateRaw() == 0);
  CHECK(dst.eng.book().find(1) != nullptr);

  // The calendar falls back to the configured interval, exactly as before.
  dst.eng.submit(InboundCommand{SetMark{SYM, {}, px(100)}}, 1 * SEC);
  CHECK(dst.lastDerivatives()->nextFundingNs.raw() == 8 * SEC);
}

// The calendar is state, not a formula: what the operator set is what the feed
// publishes, even when it disagrees with the configured interval.
void test_funding_schedule_is_state_not_a_formula()
{
  std::printf("test_funding_schedule_is_state_not_a_formula\n");
  Eng e(perpCfg());
  e.eng.submit(InboundCommand{SetMark{SYM, {}, px(100)}}, 1 * SEC);
  CHECK(e.lastDerivatives()->nextFundingNs.raw() == 8 * SEC);  // config-derived, pre-schedule

  // A settlement that does not sit on the config grid: 10s intervals, next at 25s.
  e.eng.submit(InboundCommand{SetFundingSchedule{SYM, {}, DurationNs{10 * SEC}, SeqNanos::fromRaw(25 * SEC)}}, 2 * SEC);
  CHECK(e.eng.nextFundingNs().raw() == 25 * SEC);
  CHECK(e.eng.fundingIntervalNs() == 10 * SEC);
  // Setting it is news on the feed (the engine has a mark), not a silent change.
  CHECK(e.lastDerivatives()->nextFundingNs.raw() == 25 * SEC);

  e.eng.submit(InboundCommand{SetMark{SYM, {}, px(101)}}, 3 * SEC);
  CHECK(e.lastDerivatives()->nextFundingNs.raw() == 25 * SEC);  // survives an unrelated mark

  // The settlement moves the calendar on by one whole interval.
  e.eng.submit(InboundCommand{ApplyFunding{SYM, {}, 0.0002, px(101)}}, 25 * SEC);
  CHECK(e.eng.nextFundingNs().raw() == 35 * SEC);
  CHECK(e.lastDerivatives()->nextFundingNs.raw() == 35 * SEC);
  CHECK(e.lastDerivatives()->fundingRateRaw == 2 * (kFundingRateScale / 10'000));

  // A settlement run late (an operator catching up after an outage) skips whole
  // intervals rather than leaving a boundary in the past.
  e.eng.submit(InboundCommand{ApplyFunding{SYM, {}, 0.0002, px(101)}}, 68 * SEC);
  CHECK(e.eng.nextFundingNs().raw() == 75 * SEC);

  // Clearing the schedule falls back to the config derivation.
  e.eng.submit(InboundCommand{SetFundingSchedule{SYM, {}, DurationNs{0}, SeqNanos::fromRaw(0)}}, 69 * SEC);
  CHECK(e.eng.nextFundingNs().raw() == 72 * SEC);  // 8s grid, first boundary past 69s
}

// An engine that never learned a schedule behaves exactly as it did before the
// command existed -- including publishing 0 when the venue funds nothing.
void test_no_schedule_falls_back_to_config()
{
  std::printf("test_no_schedule_falls_back_to_config\n");
  Eng withInterval(perpCfg());
  withInterval.eng.submit(InboundCommand{SetMark{SYM, {}, px(100)}}, 9 * SEC);
  CHECK(withInterval.lastDerivatives()->nextFundingNs.raw() == 16 * SEC);

  Eng spot(cfg());
  spot.eng.submit(InboundCommand{SetMark{SYM, {}, px(100)}}, 9 * SEC);
  CHECK(spot.lastDerivatives()->nextFundingNs.raw() == 0);

  // A schedule set before the first mark publishes nothing (the feed's standing
  // promise: no mark, no derivatives message) but is in force when one arrives.
  Eng early(perpCfg());
  early.eng.submit(InboundCommand{SetFundingSchedule{SYM, {}, DurationNs{10 * SEC}, SeqNanos::fromRaw(25 * SEC)}}, 1 * SEC);
  CHECK(early.lastDerivatives() == nullptr);
  early.eng.submit(InboundCommand{SetMark{SYM, {}, px(100)}}, 2 * SEC);
  CHECK(early.lastDerivatives()->nextFundingNs.raw() == 25 * SEC);
}

// The schedule replays from the journal and survives a checkpoint, like every
// other piece of engine state.
void test_funding_schedule_replays_and_checkpoints()
{
  std::printf("test_funding_schedule_replays_and_checkpoints\n");
  const std::string path = tmpPath("venue_funding_schedule", ".journal");
  std::remove(path.c_str());

  const std::vector<std::pair<int64_t, InboundCommand>> stream{
      {1 * SEC, InboundCommand{SetMark{SYM, {}, px(100)}}},
      {2 * SEC, InboundCommand{SetFundingSchedule{SYM, {}, DurationNs{10 * SEC}, SeqNanos::fromRaw(25 * SEC)}}},
      {25 * SEC, InboundCommand{ApplyFunding{SYM, {}, 0.0001, px(100)}}},
      {26 * SEC, InboundCommand{SetMark{SYM, {}, px(101)}}},
  };

  Eng live(perpCfg());
  {
    Journal j(path, Journal::Sync::Off, Journal::OpenMode::Truncate);
    for (const auto& [ts, c] : stream)
    {
      j.append(c, ts);
      live.eng.submit(c, ts);
    }
    j.flush();
  }
  CHECK(live.eng.nextFundingNs().raw() == 35 * SEC);

  Eng replayed(perpCfg());
  for (const auto& [ts, c] : Journal::loadTimed(path))
  {
    replayed.eng.submit(c, ts);
  }
  CHECK(replayed.hash == live.hash);  // the published calendar is in the digest
  CHECK(replayed.eng.stateHash() == live.eng.stateHash());
  CHECK(replayed.eng.nextFundingNs().raw() == 35 * SEC);
  std::remove(path.c_str());

  Eng restored(perpCfg());
  const Restored r =
      roundTrip(live.eng, restored.eng, tmpPath("venue_funding_schedule", ".snap"));
  CHECK(r.allApplied);
  CHECK(carriesFunding(r));
  CHECK(restored.eng.stateHash() == live.eng.stateHash());
  CHECK(restored.eng.nextFundingNs().raw() == 35 * SEC);
  CHECK(restored.eng.fundingIntervalNs() == 10 * SEC);
  CHECK(restored.eng.fundingRateRaw() == kFundingRateScale / 10'000);

  // The restored calendar keeps advancing from where it was, not from config.
  restored.eng.submit(InboundCommand{ApplyFunding{SYM, {}, 0.0001, px(101)}}, 35 * SEC);
  CHECK(restored.eng.nextFundingNs().raw() == 45 * SEC);
}

// ---------------------------------------------------------------------- wire

// A pre-Closed SBE reader, transcribed against its own offsets: it knows the
// TradingStatus template but only status values 0..4. Returns the fields it
// decodes plus whether the status code was one it recognises.
struct OldStatusRead
{
  bool ok{false};
  bool statusKnown{false};
  uint8_t statusRaw{0};
  uint64_t seq{0};
  SymbolId symbol{0};
  uint64_t epoch{0};
  int64_t engineTs{0};
  int64_t sendTs{0};
  uint8_t reason{0};
  int64_t untilNs{0};
};

OldStatusRead decodeStatusPreClosed(const uint8_t* p, size_t n)
{
  OldStatusRead r;
  if (n < SbeMdCodec::kHeaderSize)
  {
    return r;
  }
  const sbe::Header h = sbe::readHeader(p);
  const uint16_t preClosedBlock = 46;  // seq,sym,epoch,engineTs,sendTs,status,reason,untilNs
  if (h.schemaId != SbeMdCodec::kSchemaId || h.templateId != 14 ||
      h.blockLength < preClosedBlock || n < SbeMdCodec::kHeaderSize + h.blockLength)
  {
    return r;
  }
  const uint8_t* b = p + SbeMdCodec::kHeaderSize;
  r.ok = true;
  r.seq = sbe::getU64(b + 0);
  r.symbol = static_cast<SymbolId>(sbe::getU32(b + 8));
  r.epoch = sbe::getU64(b + 12);
  r.engineTs = sbe::getI64(b + 20);
  r.sendTs = sbe::getI64(b + 28);
  r.statusRaw = b[36];
  r.statusKnown = r.statusRaw <= 4;
  r.reason = b[37];
  r.untilNs = sbe::getI64(b + 38);
  return r;
}

void test_closed_on_the_sbe_wire()
{
  std::printf("test_closed_on_the_sbe_wire\n");
  MdMessage m{};
  m.type = MdType::TradingStatus;
  m.seq = 11;
  m.symbol = SYM;
  m.epoch = 4242;
  m.engineTsNs = 111;
  m.sendTsNs = 222;
  m.status = TradingStatus::Closed;
  m.reason = TradingStatusReason::Session;

  std::vector<uint8_t> buf;
  SbeMdCodec::encode(m, buf);
  MdMessage back{};
  CHECK(SbeMdCodec::decode(buf.data(), buf.size(), back));
  CHECK(back.status == TradingStatus::Closed);
  CHECK(back.reason == TradingStatusReason::Session);
  CHECK(back.seq == 11 && back.epoch == 4242 && back.engineTsNs == 111);

  // The frame is byte-identical in LAYOUT to a pre-Closed one: the enum grew at
  // the end, no root block changed. So the previous reader decodes every field
  // it knows and sees only an unrecognised status code -- which it must treat
  // as "not tradeable", never as Trading.
  CHECK(buf.size() == SbeMdCodec::kHeaderSize + SbeMdCodec::kBlockTradingStatus);
  const OldStatusRead old = decodeStatusPreClosed(buf.data(), buf.size());
  CHECK(old.ok);
  CHECK(old.seq == 11 && old.symbol == SYM && old.epoch == 4242);
  CHECK(old.engineTs == 111 && old.sendTs == 222 && old.untilNs == 0);
  CHECK(old.statusRaw == 5 && !old.statusKnown);

  // A value the old reader DOES know still decodes as it always did, out of a
  // frame produced by the new encoder.
  m.status = TradingStatus::Halted;
  m.reason = TradingStatusReason::Administrative;
  m.untilNs = 999;
  SbeMdCodec::encode(m, buf);
  const OldStatusRead halted = decodeStatusPreClosed(buf.data(), buf.size());
  CHECK(halted.ok && halted.statusKnown && halted.statusRaw == 1);
  CHECK(halted.reason == 1 && halted.untilNs == 999);
}

void test_closed_on_the_fix_wire()
{
  std::printf("test_closed_on_the_fix_wire\n");
  FixMdEncoder enc;
  std::vector<MdRequest> reqs;
  std::vector<uint8_t> reply;
  size_t consumed = 0;

  auto feedIn = [&](const std::string& msg)
  {
    reqs.clear();
    reply.clear();
    consumed = 0;
    const std::string framed = FixCodec::frame(msg);
    return enc.parse(reinterpret_cast<const uint8_t*>(framed.data()), framed.size(), consumed, reqs,
                     reply);
  };

  std::string logon;
  FixMdCodec::add(logon, 35, "A");
  FixMdCodec::add(logon, 34, "1");
  FixMdCodec::add(logon, 49, "CLIENT");
  FixMdCodec::add(logon, 56, "VENUE");
  FixMdCodec::add(logon, 52, FixMdCodec::nowStamp());
  FixMdCodec::add(logon, 108, "30");
  CHECK(feedIn(logon) == MdEncoder::Parse::Ok);

  std::string req;
  FixMdCodec::add(req, 35, "V");
  FixMdCodec::add(req, 34, "2");
  FixMdCodec::add(req, 49, "CLIENT");
  FixMdCodec::add(req, 56, "VENUE");
  FixMdCodec::add(req, 52, FixMdCodec::nowStamp());
  FixMdCodec::add(req, 262, "REQ1");
  FixMdCodec::add(req, 263, "1");
  FixMdCodec::add(req, 55, std::to_string(SYM));
  CHECK(feedIn(req) == MdEncoder::Parse::Ok);

  MdMessage st{};
  st.type = MdType::TradingStatus;
  st.seq = 1;
  st.symbol = SYM;
  st.engineTsNs = 1'700'000'000LL * SEC;
  st.sendTsNs = st.engineTsNs;
  st.status = TradingStatus::Closed;
  st.reason = TradingStatusReason::Session;

  std::vector<uint8_t> out;
  enc.increment(st, out);
  const std::string closed(out.begin(), out.end());
  CHECK(closed.find("\x01"
                    "35=f") != std::string::npos);
  // 326=18 "Not available for trading" is the end-of-session value, NOT 2
  // (Trading Halt): the distinction the state exists for survives to FIX.
  CHECK(closed.find("\x01"
                    "326=18") != std::string::npos);
  CHECK(closed.find("Session closed") != std::string::npos);

  st.status = TradingStatus::Halted;
  st.reason = TradingStatusReason::Administrative;
  out.clear();
  enc.increment(st, out);
  const std::string halted(out.begin(), out.end());
  CHECK(halted.find("\x01"
                    "326=2") != std::string::npos);

  // A delisted instrument is not tradeable and must never go out as 17
  // (Ready to Trade). 326 has no delisting value, so it is 18 with the
  // reason in Text.
  st.status = TradingStatus::Delisted;
  st.reason = TradingStatusReason::Administrative;
  out.clear();
  enc.increment(st, out);
  const std::string delisted(out.begin(), out.end());
  CHECK(delisted.find("\x01"
                      "326=18") != std::string::npos);
  CHECK(delisted.find("\x01"
                      "326=17") == std::string::npos);
  CHECK(delisted.find("Delisted") != std::string::npos);

  // A status value this encoder does not know (a newer engine's) is
  // published as not tradeable too.
  st.status = static_cast<TradingStatus>(200);
  out.clear();
  enc.increment(st, out);
  const std::string unknown(out.begin(), out.end());
  CHECK(unknown.find("\x01"
                     "326=18") != std::string::npos);
  CHECK(unknown.find("Not available for trading") != std::string::npos);
}

// End to end: the engine's own close reaches a market-data subscriber as a
// Closed status message, and a late joiner's snapshot starts closed.
void test_close_reaches_the_feed()
{
  std::printf("test_close_reaches_the_feed\n");
  std::vector<MdMessage> feed;
  auto mdHolder = std::make_unique<MarketDataPublisher<>>([&](const MdMessage& m)
                                                          { feed.push_back(m); }, px(0.01), SYM);
  auto& md = *mdHolder;
  MatchingEngine<MatchingBook> eng(cfg(), [&](const OutboundEvent& e)
                                   { md.onEvent(e, eng.engineTimeNs()); });

  eng.submit(InboundCommand{limit(1, Side::SELL, 100, 5)}, 1 * SEC);
  eng.submit(admin(AdminAction::CloseSession), 2 * SEC);

  const MdMessage* status = nullptr;
  for (const MdMessage& m : feed)
  {
    if (m.type == MdType::TradingStatus)
    {
      status = &m;
    }
  }
  CHECK(status != nullptr);
  CHECK(status->status == TradingStatus::Closed);
  CHECK(status->reason == TradingStatusReason::Session);
  CHECK(status->engineTsNs == 2 * SEC);

  const MdSnapshot snap = md.snapshotAtomic();
  CHECK(snap.hasStatus && snap.status.status == TradingStatus::Closed);
  CHECK(snap.orders.size() == 1);  // the book stands through a close
}

}  // namespace

TEST(VenueSessionAndFunding, EngineSuite)
{
  test_closed_rejects_with_its_own_reason();
  test_close_is_a_published_transition();
  test_session_replays_through_the_journal();
  test_session_survives_a_checkpoint();
  test_funding_rate_survives_a_checkpoint();
  test_snapshot_without_funding_record_still_loads();
  test_funding_schedule_is_state_not_a_formula();
  test_no_schedule_falls_back_to_config();
  test_funding_schedule_replays_and_checkpoints();
  test_closed_on_the_sbe_wire();
  test_closed_on_the_fix_wire();
  test_close_reaches_the_feed();
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  EXPECT_EQ(g_failures, 0);
}
