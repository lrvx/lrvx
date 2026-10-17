/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * A shard with no threads of its own is stepped by somebody, and until now
 * that somebody had nowhere to sleep. Both of the shard's buses are private,
 * so the WakeSet the buses already understand was out of reach from outside:
 * a command published into a shard's ingress from a gateway thread did not
 * wake the thread that was supposed to match it, and the driver had to spin a
 * core or run a clock of its own.
 *
 * These tests cover the shard joining a set -- a submit into any of the
 * shards a parked driver steps wakes it, including one landing inside the
 * sleep window itself -- and the other half of what such a driver needs: a
 * sleep bounded by its own cadence rather than by the set's 50 ms net.
 */
#include "lrvx-venue/journal.h"
#include "lrvx-venue/matching_book.h"
#include "lrvx-venue/sequenced_shard.h"
#include "lrvx/util/concurrency/jthread.h"
#include "lrvx/util/eventing/wake_set.h"
#include "support/tmp_path.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>

using namespace lrvx;
using namespace lrvx::venue;
using lrvx::venue::test::tmpPath;
using namespace std::chrono;

namespace
{

constexpr int kShards = 3;

Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }

venue::SymbolConfig cfg(SymbolId sym, int64_t lastLookWindowNs = 0)
{
  venue::SymbolConfig c;
  c.id = sym;
  c.tickSize = px(0.01);
  c.minPrice = px(50.0);
  c.maxPrice = px(150.0);
  c.baseAsset = 0;
  c.quoteAsset = 1;
  c.lastLookWindowNs = DurationNs{lastLookWindowNs};
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

struct Sink : IEngineEventListener
{
  std::atomic<int> events{0};
  std::atomic<int> rejects{0};
  void onEngineEvent(const EngineEventMsg& e) override
  {
    if (std::get_if<FillRejected>(&e.event))
    {
      rejects.fetch_add(1, std::memory_order_release);
    }
    events.fetch_add(1, std::memory_order_release);
  }
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

int64_t monoNs()
{
  return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

// Busy-wait for a counter to move past where it was. Relative to a baseline
// rather than an absolute target, because how many outbound events one order
// produces is the engine's business and not this test's. Spin rather than
// poll: a sleeping poll notices a delivery up to its interval late, which is
// useless when what is being timed is a wake-up measured in microseconds.
bool spinPast(const std::atomic<int>& what, int baseline, milliseconds budget)
{
  const auto deadline = steady_clock::now() + budget;
  while (what.load(std::memory_order_acquire) <= baseline)
  {
    if (steady_clock::now() >= deadline)
    {
      return false;
    }
  }
  return true;
}

// Spin for a wall-clock interval. sleep_for is millisecond-grained on some
// platforms and these tests measure microseconds.
void spinFor(microseconds d)
{
  const auto until = steady_clock::now() + d;
  while (steady_clock::now() < until)
  {
  }
}

// N shards with no threads of their own and one thread stepping all of them,
// parked on one WakeSet -- the shape a process holding hundreds of shards
// ends up with, in miniature.
class Driver
{
 public:
  // The set's net is pushed far out: a wake-up inside a round's spin budget
  // then cannot have come from the net, whatever the runner's scheduler did
  // to it, so the tests below prove the mechanism without a millisecond
  // bound a loaded shared runner is free to miss.
  static constexpr auto kFarNet = std::chrono::seconds(10);

  explicit Driver(int64_t lastLookWindowNs = 0)
  {
    _set.setNetInterval(kFarNet);
    for (int i = 0; i < kShards; ++i)
    {
      auto& base = _bases[size_t(i)];
      base = tmpPath("venue_shard_wake_" + std::to_string(i), ".bin");
      cleanFiles(base);
      auto& s = _shards[size_t(i)];
      s = std::make_unique<SequencedShard<>>(cfg(SymbolId(i + 1), lastLookWindowNs), base,
                                             MatchingBook{}, Journal::Sync::Off);
      s->setOwnThreads(false);
      s->setWakeSet(&_set);
      s->subscribeOutbound(&_sinks[size_t(i)]);
      s->start();
    }
  }

  ~Driver() { shutdown(); }

  SequencedShard<>& shard(int i) { return *_shards[size_t(i)]; }
  Sink& sink(int i) { return _sinks[size_t(i)]; }
  WakeSet& set() { return _set; }

  // The driver's last look at every shard it steps. Delivers nothing: it runs
  // under the set's mutex.
  bool pending() const
  {
    for (const auto& s : _shards)
    {
      if (s->hasPending())
      {
        return true;
      }
    }
    return false;
  }

  // One shard's order, submitted the way a gateway thread would: from a
  // thread that is not the one stepping the shard.
  void submit(int shardIdx, OrderId id, Side side = Side::SELL)
  {
    shard(shardIdx).submit(
        InboundCommand{limit(SymbolId(shardIdx + 1), id, side, 100.0, 1.0, 1)});
  }

  // Sleeps on the set with no deadline of its own: only a publish (or the
  // net behind it) ends the sleep.
  void run() { start(/*cadenceNs=*/0); }

  // The same driver with a cadence: it sweeps every `cadenceNs` and sleeps
  // until the earlier of that and somebody publishing.
  void runWithCadence(int64_t cadenceNs) { start(cadenceNs); }

  void shutdown()
  {
    if (_thread.has_value())
    {
      _running.store(false, std::memory_order_release);
      _set.wake();
      _thread.reset();
    }
    for (auto& s : _shards)
    {
      if (s)
      {
        s->stop();
        s.reset();
      }
    }
    for (const auto& base : _bases)
    {
      if (!base.empty())
      {
        cleanFiles(base);
      }
    }
  }

 private:
  void start(int64_t cadenceNs)
  {
    _running.store(true, std::memory_order_release);
    _thread.emplace(
        [this, cadenceNs]
        {
          int64_t nextSweep = cadenceNs > 0 ? monoNs() : 0;
          while (_running.load(std::memory_order_acquire))
          {
            if (cadenceNs > 0)
            {
              const int64_t now = monoNs();
              if (now >= nextSweep)
              {
                nextSweep = now + cadenceNs;
                for (auto& s : _shards)
                {
                  s->sweepOnce();
                }
              }
            }
            bool any = false;
            for (auto& s : _shards)
            {
              any = s->pollOnce() || any;
            }
            if (any)
            {
              continue;
            }
            const auto stillAsleep = [this]
            { return !_running.load(std::memory_order_acquire) || pending(); };
            if (cadenceNs > 0)
            {
              _set.parkUntil(nextSweep, stillAsleep);
            }
            else
            {
              _set.parkUnless(stillAsleep);
            }
          }
        });
  }

  WakeSet _set;
  std::array<std::string, kShards> _bases{};
  std::array<std::unique_ptr<SequencedShard<>>, kShards> _shards{};
  std::array<Sink, kShards> _sinks{};
  std::atomic<bool> _running{false};
  std::optional<jthread> _thread{};
};

// ---------------------------------------------------------------------------

// The wiring, before any of the behaviour: a shard nobody pointed at a set
// has none, and a shard pointed at one hands it to BOTH of its buses -- the
// ingress a gateway publishes into and the outbound the driver's own step
// publishes into. Either half missing is a wake-up nobody sends.
TEST(VenueShardWakeSet, AShardHandsTheSetToBothOfItsBuses)
{
  const std::string base = tmpPath("venue_shard_wake_wiring", ".bin");
  cleanFiles(base);

  Sink sink;
  auto s = std::make_unique<SequencedShard<>>(cfg(1), base, MatchingBook{}, Journal::Sync::Off);
  s->setOwnThreads(false);
  EXPECT_EQ(s->wakeSet(), nullptr);

  WakeSet set;
  s->setWakeSet(&set);
  EXPECT_EQ(s->wakeSet(), &set);
  s->subscribeOutbound(&sink);
  s->start();

  // The look the driver parks on, without the delivery: a submitted command
  // is there to be seen, and once it has been stepped it is not.
  s->submit(InboundCommand{limit(1, 1, Side::SELL, 100.0, 5.0, 1)});
  EXPECT_TRUE(s->hasPending()) << "a submitted command is not visible to the driver's look";
  while (s->pollOnce())
  {
  }
  EXPECT_FALSE(s->hasPending()) << "the look still sees work after everything was stepped";
  EXPECT_GT(sink.events.load(), 0);

  s->stop();
  s.reset();
  cleanFiles(base);
}

// The point of the task: a submit into ANY of the shards the driver steps has
// to reach a driver that is asleep. One shard at a time, from a cold park
// each round, so every round exercises the wake-up path rather than riding on
// the one before it.
TEST(VenueShardWakeSet, ADriverParkedOverThreeShardsWakesOnASubmitToAnyOfThem)
{
  Driver driver;
  driver.run();

  constexpr int kRounds = 45;
  const auto t0 = steady_clock::now();
  for (int i = 0; i < kRounds; ++i)
  {
    const int sh = i % kShards;
    // Long enough for the driver to have finished the previous round and
    // gone to sleep, so the submit below lands on a sleeper.
    spinFor(microseconds(120 + (i % 17) * 5));
    const int before = driver.sink(sh).events.load(std::memory_order_acquire);
    driver.submit(sh, OrderId(i + 1));
    ASSERT_TRUE(spinPast(driver.sink(sh).events, before, milliseconds(2000)))
        << "the driver never woke for shard " << sh << " at round " << i;
  }
  const auto elapsed = duration_cast<milliseconds>(steady_clock::now() - t0);

  // The timed wait inside the park is a net, not the mechanism. If wake-ups
  // were arriving on its schedule instead of from the submitter, every round
  // would wait out Driver::kFarNet and the spin above would have given up
  // long before; the total is bounded by one net for the same reason.
  std::printf("%d submits to a driver parked over %d shards: %lld ms total\n", kRounds, kShards,
              static_cast<long long>(elapsed.count()));
  EXPECT_LT(elapsed, Driver::kFarNet) << "wake-ups are riding the safety net";
  driver.shutdown();
}

// The window the set has to survive is the one between the driver's last look
// at its shards and it raising its hand: a submit landing there sees no
// waiters, sends no wake-up, and -- if nothing follows it -- leaves the
// driver asleep until the net expires. Hitting a couple of hundred
// nanoseconds inside another thread from outside is luck, so this test
// submits FROM that instant, through the set's probe.
//
// Each round does both halves in order. The first submit lands on a driver
// that is already asleep, so it arrives only if the shard's ingress wakes the
// set. The probe submit lands inside the window with silence behind it, so it
// arrives only if the driver takes one last look -- through hasPending() --
// after raising its hand. The probe owns the LAST shard and the rounds use
// the others, so the two halves are counted apart and neither can stand in
// for the other.
TEST(VenueShardWakeSet, ASubmitInsideTheSleepWindowStillWakesTheDriver)
{
  static_assert(kShards >= 2, "the probe needs a shard of its own");
  constexpr int kProbeShard = kShards - 1;

  Driver driver;

  struct Probe
  {
    Driver* driver{nullptr};
    std::atomic<int> armed{0};
    std::atomic<int> fired{0};
    std::atomic<int> nextId{10'000};
  } probe;
  probe.driver = &driver;

  driver.set().setProbe(
      [](void* user)
      {
        auto* p = static_cast<Probe*>(user);
        if (p->armed.exchange(0, std::memory_order_acq_rel) == 0)
        {
          return;
        }
        // The last shard the driver steps, so it has to look past the ones it
        // already found empty.
        p->driver->submit(kProbeShard, OrderId(p->nextId.fetch_add(1, std::memory_order_relaxed)));
        p->fired.fetch_add(1, std::memory_order_release);
      },
      &probe);

  driver.run();

  constexpr int kRounds = 40;
  const auto t0 = steady_clock::now();
  for (int i = 0; i < kRounds; ++i)
  {
    const int sh = i % kProbeShard;
    spinFor(microseconds(150 + (i % 13) * 7));
    const int beforeWake = driver.sink(sh).events.load(std::memory_order_acquire);
    const int beforeProbe = driver.sink(kProbeShard).events.load(std::memory_order_acquire);
    probe.armed.store(1, std::memory_order_release);
    // Wakes the driver; when it runs out of work and starts going to sleep,
    // the probe fires from inside the window and submits into the silence.
    driver.submit(sh, OrderId(i + 1));
    ASSERT_TRUE(spinPast(driver.sink(sh).events, beforeWake, milliseconds(3000)))
        << "the driver slept through a submit to shard " << sh << ", round " << i;
    ASSERT_TRUE(spinPast(driver.sink(kProbeShard).events, beforeProbe, milliseconds(3000)))
        << "the driver slept through a submit made inside its sleep window, round " << i;
  }
  const auto elapsed = duration_cast<milliseconds>(steady_clock::now() - t0);

  EXPECT_EQ(probe.fired.load(), kRounds);
  std::printf("%d rounds across the sleep window over %d shards: %lld ms total\n", kRounds, kShards,
              static_cast<long long>(elapsed.count()));
  EXPECT_LT(elapsed, Driver::kFarNet) << "a wake-up came from the net, not from the submitter";
  driver.shutdown();
}

// A driver rarely only waits for a publisher: it sweeps, it watches a
// checkpoint threshold, it has periodic business of its own. What it needs is
// a sleep that ends at the earlier of "somebody published" and "the next
// thing is due" -- and with only the net under it, "due in 5 ms" means woken
// in 50.
//
// No publisher at all here: the deadline is the only thing that can end these
// sleeps, and the wall clock is the assertion.
TEST(VenueShardWakeSet, ParkUntilWakesOnItsDeadlineWithNobodyPublishing)
{
  WakeSet set;
  constexpr int kRounds = 32;
  constexpr int64_t kDeadlineMs = 5;
  constexpr int64_t kNetMs = WakeSet::kDefaultNetInterval.count();

  int64_t worstMs = 0;
  int64_t totalMs = 0;
  for (int i = 0; i < kRounds; ++i)
  {
    const auto t0 = steady_clock::now();
    set.parkUntil(monoNs() + kDeadlineMs * 1'000'000,
                  []
                  { return false; });  // nothing is pending and nothing ever will be
    const auto took = duration_cast<milliseconds>(steady_clock::now() - t0);
    worstMs = took.count() > worstMs ? took.count() : worstMs;
    totalMs += took.count();
    // It slept: a deadline is a ceiling on the sleep, not a way out of it.
    EXPECT_GE(took, milliseconds(kDeadlineMs - 1)) << "round " << i << " did not sleep";
  }
  const int64_t meanMs = totalMs / kRounds;

  // A control series, right after, on the same machine: the same kRounds
  // sleeping with no deadline of their own -- parkUntil(kNoDeadline, ...),
  // which is what parkUnless() already is -- so the only bound left is the
  // net. A shared macOS CI runner has been seen turning a 5 ms parkUntil into
  // a 32 ms mean (worst 45 ms) while its own net sleeps average 77 ms against
  // a nominal 50 ms -- the runner oversleeps whatever it is asked to wait for
  // by about that much, deadline or net alike. A fixed millisecond ceiling on
  // the deadline series cannot survive that; what the runner cannot move is
  // the RATIO between a bounded sleep and an unbounded one measured back to
  // back on it, and that is what this asserts instead.
  int64_t totalNetMs = 0;
  int64_t worstNetMs = 0;
  for (int i = 0; i < kRounds; ++i)
  {
    const auto t0 = steady_clock::now();
    set.parkUntil(WakeSet::kNoDeadline,
                  []
                  { return false; });
    const auto took = duration_cast<milliseconds>(steady_clock::now() - t0);
    totalNetMs += took.count();
    worstNetMs = took.count() > worstNetMs ? took.count() : worstNetMs;
  }
  const int64_t meanNetMs = totalNetMs / kRounds;

  std::printf(
      "parkUntil(%lld ms), %d rounds: worst %lld ms, mean %lld ms (the net is %lld ms, "
      "mean(net) %lld ms, worst(net) %lld ms)\n",
      static_cast<long long>(kDeadlineMs), kRounds, static_cast<long long>(worstMs),
      static_cast<long long>(meanMs), static_cast<long long>(kNetMs),
      static_cast<long long>(meanNetMs), static_cast<long long>(worstNetMs));

  // The mean against the control series' OWN mean on this same machine, not
  // against a fixed millisecond budget. A sleep bounded by the net is bounded
  // by it every round, deadline or no deadline -- that is exactly what the
  // control series measures -- so a parkUntil() that silently ignored its
  // deadline would land its mean right next to meanNetMs, same as the control
  // series does by construction. A parkUntil() that honours its deadline
  // lands well under it, on any machine: ~32 ms against ~77 ms (a ratio of
  // 0.42) on the flaky macOS runner above, ~5 ms against ~50 ms (0.1) on a
  // quiet one. 0.6 leaves wide margin on both those measurements.
  EXPECT_LT(meanMs, meanNetMs * 6 / 10)
      << "the deadline was ignored and the sleeps rode the net just like the control series";
  // The worst round is no longer a mutation detector on its own -- the mean
  // comparison above is that -- this is just a sanity bound so one wildly
  // descheduled round does not pass silently while wrecking the printed
  // numbers. Held against the control series' own WORST round, not its mean:
  // a single round in either series can be descheduled for far longer than
  // that series' average on a genuinely contended runner (another job's
  // build, a neighbour's noisy container), and comparing worst-to-mean
  // let exactly that kind of round in the deadline series fail against a
  // mean that a symmetric bad round in the net series would not have moved
  // as much. Worst-to-worst is the like-for-like comparison, still measured
  // back to back on the same machine, with the same flat +10ms floor for a
  // quiet machine where both series round to single-digit milliseconds.
  EXPECT_LT(worstMs, worstNetMs + 10)
      << "a round waited far longer than the control series' own worst round";
}

// A deadline already in the past is "look once and come straight back".
TEST(VenueShardWakeSet, ParkUntilWithAPastDeadlineReturnsAtOnce)
{
  WakeSet set;
  const auto t0 = steady_clock::now();
  set.parkUntil(monoNs() - 1'000'000'000,
                []
                { return false; });
  EXPECT_LT(duration_cast<milliseconds>(steady_clock::now() - t0), milliseconds(20));
}

// A submit still wins when it is the earlier of the two: the deadline is a
// ceiling on the sleep, not a schedule the wake-up has to wait for.
TEST(VenueShardWakeSet, ParkUntilStillWakesOnASubmitBeforeTheDeadline)
{
  Driver driver;
  // The cadence is pushed out the way the net is, and kept under the net so
  // the deadline is what bounds each sleep. A submit that only got through on
  // the deadline would then hold its round for the whole cadence, well past
  // the spin budget, and the round fails on that rather than on a wall-clock
  // figure for the run that a loaded runner is free to miss. With no holds
  // open there is nothing for the sweep itself to submit.
  constexpr auto kCadence = duration_cast<milliseconds>(Driver::kFarNet) / 2;
  driver.runWithCadence(duration_cast<nanoseconds>(kCadence).count());

  constexpr int kRounds = 20;
  const auto t0 = steady_clock::now();
  for (int i = 0; i < kRounds; ++i)
  {
    const int sh = i % kShards;
    spinFor(microseconds(150));
    const int before = driver.sink(sh).events.load(std::memory_order_acquire);
    driver.submit(sh, OrderId(i + 1));
    ASSERT_TRUE(spinPast(driver.sink(sh).events, before, milliseconds(2000)))
        << "a submit did not beat the cadence deadline at round " << i;
  }
  const auto elapsed = duration_cast<milliseconds>(steady_clock::now() - t0);

  // Every round came back inside its spin budget, so no wake-up rode the
  // deadline; the total is bounded by one cadence for the same reason.
  std::printf("%d submits under a %lld ms cadence: %lld ms total\n", kRounds,
              static_cast<long long>(kCadence.count()),
              static_cast<long long>(elapsed.count()));
  EXPECT_LT(elapsed, kCadence) << "submits were waiting for the cadence deadline";
  driver.shutdown();
}

// And the cadence itself, end to end: a hold whose window has passed expires
// because the driver woke on its own deadline and swept. Nobody submits
// anything after the hold is taken; without the deadline the driver would
// find the expiry a net interval late, every time.
TEST(VenueShardWakeSet, TheCadenceExpiresAHoldWithNobodySubmitting)
{
  Driver driver(/*lastLookWindowNs=*/2'000'000);  // 2 ms last-look window
  driver.runWithCadence(/*cadenceNs=*/2'000'000);

  NewOrder mk = limit(1, 1, Side::SELL, 100.0, 5.0, 1);
  mk.lastLook = true;
  driver.shard(0).submit(InboundCommand{mk});
  driver.shard(0).submit(InboundCommand{limit(1, 2, Side::BUY, 100.0, 3.0, 2)});

  // From here nothing is submitted from outside: the expiry has to come from
  // the driver waking on its own cadence.
  const auto t0 = steady_clock::now();
  const bool expired = spinPast(driver.sink(0).rejects, 0, milliseconds(500));
  const auto took = duration_cast<milliseconds>(steady_clock::now() - t0);

  std::printf("a hold expired by the driver's own 2 ms cadence after %lld ms\n",
              static_cast<long long>(took.count()));
  EXPECT_TRUE(expired) << "nothing expired the hold: the driver never woke on its cadence";
  EXPECT_LT(took, milliseconds(40)) << "the expiry arrived on the net, not on the cadence";
  driver.shutdown();
}

}  // namespace
