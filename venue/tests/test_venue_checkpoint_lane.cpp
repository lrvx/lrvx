/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * A checkpoint pauses its shard: the book is cloned and the journal rotated
 * with matching stopped. That is the price of a deterministic snapshot, and
 * while a shard owns a thread it is paid by one symbol.
 *
 * These tests cover what changes once many shards share a driver: the pause
 * must not be taken by two of them at once, an automatic checkpoint must skip
 * rather than wait for a disk, the triggers must not fire in unison, and a
 * skipped checkpoint must cost a snapshot and never a record.
 */
#include "lrvx-venue/checkpoint_lane.h"
#include "lrvx-venue/journal.h"
#include "lrvx-venue/matching_book.h"
#include "lrvx-venue/matching_engine.h"
#include "lrvx-venue/sequenced_shard.h"
#include "support/tmp_path.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace lrvx;
using namespace lrvx::venue;
using lrvx::venue::test::tmpPath;

namespace
{

constexpr AssetId BASE = 0;
constexpr AssetId QUOTE = 1;

Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }

venue::SymbolConfig cfg(SymbolId sym)
{
  venue::SymbolConfig c;
  c.id = sym;
  c.tickSize = px(0.01);
  c.minPrice = px(50.0);
  c.maxPrice = px(150.0);
  c.baseAsset = BASE;
  c.quoteAsset = QUOTE;
  return c;
}

NewOrder limit(SymbolId sym, OrderId id, Side s, double p, double q, uint64_t acct)
{
  NewOrder o;
  o.id = id;
  o.symbol = sym;
  o.side = s;
  o.type = OrderType::LIMIT;
  o.price = px(p);
  o.quantity = qty(q);
  o.accountId = acct;
  return o;
}

SequencedShard<>::TimeSource stepClock()
{
  auto t = std::make_shared<std::atomic<int64_t>>(1000);
  return [t]()
  { return t->fetch_add(1000) + 1000; };
}

struct CountingSink : IEngineEventListener
{
  std::atomic<uint64_t> count{0};
  void onEngineEvent(const EngineEventMsg&) override { count.fetch_add(1); }
};

void cleanFiles(const std::string& base)
{
  std::remove(base.c_str());
  const auto g = SequencedShard<>::scanGenerations(base);
  std::error_code ec;
  for (auto ts : g.snapshots)
  {
    std::filesystem::remove(SequencedShard<>::snapshotPath(base, ts), ec);
  }
  for (auto ts : g.segments)
  {
    std::filesystem::remove(SequencedShard<>::segmentPath(base, ts), ec);
  }
}

// ---------------------------------------------------------------------------

TEST(VenueCheckpointLane, TwoShardsOnOneLaneNeverPauseTogether)
{
  const std::string baseA = tmpPath("venue_lane_a", ".bin");
  const std::string baseB = tmpPath("venue_lane_b", ".bin");
  cleanFiles(baseA);
  cleanFiles(baseB);

  CheckpointLane lane;
  std::atomic<int> inPause{0};
  std::atomic<int> overlaps{0};
  std::atomic<int> pauses{0};

  // The hook runs inside the pause, on the consumer thread. Two shards on one
  // lane must never be in here at the same time.
  const auto hook = [&](int64_t)
  {
    if (inPause.fetch_add(1) != 0)
    {
      overlaps.fetch_add(1);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    pauses.fetch_add(1);
    inPause.fetch_sub(1);
  };

  auto a = std::make_unique<SequencedShard<>>(cfg(1), baseA, MatchingBook{}, Journal::Sync::Off,
                                              stepClock(), /*idleSweepIntervalNs*/ 0);
  auto b = std::make_unique<SequencedShard<>>(cfg(2), baseB, MatchingBook{}, Journal::Sync::Off,
                                              stepClock(), /*idleSweepIntervalNs*/ 0);
  a->setCheckpointLane(&lane);
  b->setCheckpointLane(&lane);
  a->onCheckpoint(hook);
  b->onCheckpoint(hook);
  a->start();
  b->start();

  a->submit(InboundCommand{limit(1, 1, Side::BUY, 99.0, 1.0, 1)});
  b->submit(InboundCommand{limit(2, 1, Side::BUY, 99.0, 1.0, 1)});
  a->flush();
  b->flush();

  constexpr int kRounds = 4;
  std::thread ta(
      [&]
      {
        for (int i = 0; i < kRounds; ++i)
        {
          a->submit(InboundCommand{limit(1, 100 + i, Side::BUY, 98.0, 1.0, 1)});
          EXPECT_TRUE(a->checkpointNow());
        }
      });
  for (int i = 0; i < kRounds; ++i)
  {
    b->submit(InboundCommand{limit(2, 100 + i, Side::BUY, 98.0, 1.0, 1)});
    EXPECT_TRUE(b->checkpointNow());
  }
  ta.join();

  EXPECT_EQ(overlaps.load(), 0);
  EXPECT_EQ(pauses.load(), kRounds * 2);
  EXPECT_EQ(lane.checkpoints(), static_cast<uint64_t>(kRounds * 2));
  // A checkpoint asked for by name is never skipped, however busy the lane.
  EXPECT_EQ(lane.skipped(), 0u);
  EXPECT_GT(lane.pauseMaxNs(), 0);
  EXPECT_GE(lane.pauseTotalNs(), lane.pauseMaxNs());

  a->stop();
  b->stop();
  a.reset();
  b.reset();
  cleanFiles(baseA);
  cleanFiles(baseB);
}

TEST(VenueCheckpointLane, AutomaticCheckpointSkipsRatherThanWaits)
{
  const std::string base = tmpPath("venue_lane_skip", ".bin");
  cleanFiles(base);

  CheckpointLane lane;
  CountingSink sink;

  CheckpointConfig ck;
  ck.maxSegmentRecords = 4;  // crossed almost immediately
  ck.maxSegmentBytes = 0;

  auto s = std::make_unique<SequencedShard<>>(cfg(1), base, MatchingBook{}, Journal::Sync::Off,
                                              stepClock(), /*idleSweepIntervalNs*/ 0, ck);
  s->setCheckpointLane(&lane);
  s->subscribeOutbound(&sink);
  s->start();

  // Somebody else on this lane is in their pause, and will be for a while.
  ASSERT_TRUE(lane.tryEnter());

  // Matching must go on regardless. Deliberately NOT flush(): under a
  // checkpoint that waits for the lane the consumer would never come back,
  // and a test that hangs is not a test that failed.
  const uint64_t want = 40;
  for (uint64_t i = 0; i < want; ++i)
  {
    s->submit(InboundCommand{limit(1, static_cast<OrderId>(i + 1), Side::BUY, 99.0, 1.0, 1)});
  }
  // Sweep until the threshold is seen crossed and the checkpoint it asks for
  // has been skipped -- the sweep reads the journal from this thread, so it
  // has to be asked more than once while the consumer catches up.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline)
  {
    s->sweepOnce();
    if (sink.count.load() >= want && s->checkpointsSkippedBusy() > 0)
    {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const bool keptMatching = sink.count.load() >= want;
  const uint64_t skipped = s->checkpointsSkippedBusy();

  lane.leave();  // whatever happened above, do not leave the lane taken

  EXPECT_TRUE(keptMatching) << "matching stopped while another shard held the lane";
  EXPECT_GT(skipped, 0u) << "the automatic checkpoint was never even attempted";
  EXPECT_EQ(lane.skipped(), skipped);
  EXPECT_EQ(s->checkpointsTaken(), 0u);

  // The lane is free again: the threshold is still crossed, so the next sweep
  // takes the checkpoint that was skipped, without anybody asking twice.
  s->sweepOnce();
  s->flush();
  const auto deadline2 = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (s->checkpointsTaken() == 0 && std::chrono::steady_clock::now() < deadline2)
  {
    s->sweepOnce();
    s->flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_GT(s->checkpointsTaken(), 0u);

  s->stop();
  s.reset();
  cleanFiles(base);
}

TEST(VenueCheckpointLane, SkippedCheckpointCostsASnapshotNotARecord)
{
  const std::string base = tmpPath("venue_lane_replay", ".bin");
  cleanFiles(base);

  CheckpointLane lane;
  CheckpointConfig ck;
  ck.maxSegmentRecords = 4;
  ck.maxSegmentBytes = 0;

  uint64_t liveHash = 0;
  uint64_t skipped = 0;
  {
    auto s = std::make_unique<SequencedShard<>>(cfg(1), base, MatchingBook{}, Journal::Sync::Off,
                                                stepClock(), /*idleSweepIntervalNs*/ 0, ck);
    s->setCheckpointLane(&lane);
    s->start();

    ASSERT_TRUE(lane.tryEnter());
    for (uint64_t i = 0; i < 30; ++i)
    {
      s->submit(InboundCommand{limit(1, static_cast<OrderId>(i + 1), Side::BUY,
                                     99.0 - static_cast<double>(i % 5) * 0.01, 1.0, 1 + i % 3)});
    }
    // No flush() while the lane is held: flush waits for the consumer, and a
    // consumer waiting for the lane would hang the test instead of failing it.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (s->checkpointsSkippedBusy() == 0 && std::chrono::steady_clock::now() < deadline)
    {
      s->sweepOnce();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    skipped = s->checkpointsSkippedBusy();

    // Stop BEFORE reading the engine, and hold the lane until after: stop()
    // joins the consumer, so the state is read by the only thread left, and a
    // lane still taken means the drain cannot slip a checkpoint in and put a
    // snapshot on the disk that the assertions below say is not there.
    s->stop();
    liveHash = s->engine().stateHash();
    lane.leave();
  }
  ASSERT_GT(skipped, 0u);

  // No snapshot was taken, so recovery replays the journal from the start --
  // further than it would have, and to exactly the same state.
  const auto gens = SequencedShard<>::scanGenerations(base);
  EXPECT_TRUE(gens.snapshots.empty());
  {
    auto s2 = std::make_unique<SequencedShard<>>(cfg(1), base, MatchingBook{}, Journal::Sync::Off,
                                                 stepClock(), /*idleSweepIntervalNs*/ 0, ck);
    s2->start();  // recovery happens here, on this thread
    s2->stop();
    EXPECT_EQ(s2->engine().stateHash(), liveHash);
  }
  cleanFiles(base);
}

TEST(VenueCheckpointLane, LaneComesBackWhenTheSnapshotIsWritten)
{
  const std::string base = tmpPath("venue_lane_return", ".bin");
  cleanFiles(base);

  CheckpointLane lane;
  CheckpointConfig ck;
  ck.maxSegmentRecords = 1000;
  ck.maxSegmentBytes = 0;
  ck.triggerJitterPct = 50;

  auto s = std::make_unique<SequencedShard<>>(cfg(1), base, MatchingBook{}, Journal::Sync::Off,
                                              stepClock(), /*idleSweepIntervalNs*/ 0, ck);
  s->setCheckpointLane(&lane);
  s->start();
  const uint64_t firstThreshold = s->effectiveMaxSegmentRecords();

  for (uint64_t i = 0; i < 1200; ++i)
  {
    s->submit(InboundCommand{limit(1, static_cast<OrderId>(i + 1), Side::BUY,
                                   51.0 + static_cast<double>(i % 4000) * 0.01, 1.0, 1)});
  }
  s->flush();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (s->checkpointsTaken() == 0 && std::chrono::steady_clock::now() < deadline)
  {
    s->sweepOnce();
    s->flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_GT(s->checkpointsTaken(), 0u);

  // A fresh cut for the next segment: shards that drifted into step during
  // this one do not stay there. Deterministic, so this is an equality about
  // two known numbers, not a coin flip.
  EXPECT_NE(s->effectiveMaxSegmentRecords(), firstThreshold);
  EXPECT_LE(s->effectiveMaxSegmentRecords(), ck.maxSegmentRecords);
  EXPECT_GT(s->effectiveMaxSegmentRecords(), 0u);

  // The lane covers the snapshot write, not just the pause -- but it does come
  // back. Asked without blocking on purpose: a lane that is never handed back
  // must show up as a failing test, not as a test that never ends.
  bool freed = false;
  const auto deadline2 = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline2)
  {
    if (lane.tryEnter())
    {
      lane.leave();
      freed = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_TRUE(freed) << "the lane was never handed back after the snapshot";

  s->stop();
  s.reset();
  cleanFiles(base);
}

TEST(VenueCheckpointLane, WithNoLaneAnAutomaticCheckpointStillDoesNotWaitForTheDisk)
{
  const std::string base = tmpPath("venue_lane_nodisk", ".bin");
  cleanFiles(base);

  CheckpointConfig ck;
  ck.maxSegmentRecords = 8;
  ck.maxSegmentBytes = 0;

  // No lane at all: the only thing between this shard and a disk wait inside
  // its pause is the in-flight check.
  auto s = std::make_unique<SequencedShard<>>(cfg(1), base, MatchingBook{}, Journal::Sync::Off,
                                              stepClock(), /*idleSweepIntervalNs*/ 0, ck);
  s->start();

  // A book worth writing: the snapshot publish has to take long enough for
  // the next threshold crossing to land while it is still running.
  OrderId id = 1;
  for (int i = 0; i < 20000; ++i)
  {
    s->submit(InboundCommand{
        limit(1, id++, Side::BUY, 51.0 + static_cast<double>(i % 4000) * 0.01, 1.0, 1)});
  }
  s->flush();

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (s->checkpointsSkippedBusy() == 0 && std::chrono::steady_clock::now() < deadline)
  {
    s->sweepOnce();
    // Keep crossing the threshold so there is always another checkpoint to
    // ask for while the previous snapshot is still being written.
    for (int i = 0; i < 16; ++i)
    {
      const OrderId order = id++;
      s->submit(InboundCommand{
          limit(1, order, Side::BUY, 51.0 + static_cast<double>(order % 4000) * 0.01, 1.0, 1)});
    }
    s->flush();
  }

  EXPECT_GT(s->checkpointsSkippedBusy(), 0u)
      << "an automatic checkpoint waited for the previous snapshot instead of skipping";

  // The skip above happened because a snapshot was still being written -- so
  // that snapshot lands shortly after, and the skipped checkpoint is a
  // deferral, not a loss.
  const auto deadline2 = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (s->checkpointsTaken() == 0 && std::chrono::steady_clock::now() < deadline2)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_GT(s->checkpointsTaken(), 0u);

  s->stop();
  s.reset();
  cleanFiles(base);
}

TEST(VenueCheckpointLane, JitterSpreadsTheTriggersAndIsOffByDefault)
{
  std::vector<std::string> bases;
  std::vector<std::unique_ptr<SequencedShard<>>> plain;
  std::vector<std::unique_ptr<SequencedShard<>>> spread;

  CheckpointConfig same;
  same.maxSegmentRecords = 1000;
  same.maxSegmentBytes = 0;

  CheckpointConfig jittered = same;
  jittered.triggerJitterPct = 50;

  constexpr int kShards = 8;
  for (int i = 0; i < kShards; ++i)
  {
    bases.push_back(tmpPath("venue_lane_jit_a" + std::to_string(i), ".bin"));
    bases.push_back(tmpPath("venue_lane_jit_b" + std::to_string(i), ".bin"));
    cleanFiles(bases[bases.size() - 2]);
    cleanFiles(bases.back());
    plain.push_back(std::make_unique<SequencedShard<>>(
        cfg(static_cast<SymbolId>(i + 1)), bases[bases.size() - 2], MatchingBook{},
        Journal::Sync::Off, stepClock(), 0, same));
    spread.push_back(std::make_unique<SequencedShard<>>(
        cfg(static_cast<SymbolId>(i + 1)), bases.back(), MatchingBook{}, Journal::Sync::Off,
        stepClock(), 0, jittered));
  }

  // Off by default -- and by default is what every existing deployment runs,
  // so the thresholds must be exactly what the config says.
  for (int i = 0; i < kShards; ++i)
  {
    EXPECT_EQ(plain[i]->effectiveMaxSegmentRecords(), same.maxSegmentRecords);
  }

  size_t distinct = 0;
  std::vector<uint64_t> seen;
  for (int i = 0; i < kShards; ++i)
  {
    const uint64_t v = spread[i]->effectiveMaxSegmentRecords();
    // Never above the configured threshold, and never down to zero (which
    // means "disabled" everywhere else).
    EXPECT_LE(v, jittered.maxSegmentRecords);
    EXPECT_GE(v, jittered.maxSegmentRecords / 2);
    if (std::find(seen.begin(), seen.end(), v) == seen.end())
    {
      seen.push_back(v);
      ++distinct;
    }
  }
  // Eight shards, same config, same load: they must not all reach their
  // thresholds on the same record.
  EXPECT_GT(distinct, 1u);

  // Deterministic: the same symbol with the same config gets the same cut,
  // so a venue's checkpoints land where they landed last run.
  auto again = std::make_unique<SequencedShard<>>(cfg(1), tmpPath("venue_lane_jit_again", ".bin"),
                                                  MatchingBook{}, Journal::Sync::Off, stepClock(),
                                                  0, jittered);
  EXPECT_EQ(again->effectiveMaxSegmentRecords(), spread[0]->effectiveMaxSegmentRecords());
  again.reset();

  plain.clear();
  spread.clear();
  for (const auto& b : bases)
  {
    cleanFiles(b);
  }
  cleanFiles(tmpPath("venue_lane_jit_again", ".bin"));
}

}  // namespace
