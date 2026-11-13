/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// stop() against a live publisher.
//
// A publisher reads _running once, on the way in, and everything after that
// read -- claiming a sequence, destroying what the slot held, constructing the
// event into it, stamping the slot -- has to be accounted for by stop() rather
// than hoped about. Two windows matter and they are not the same window: a
// publisher that already holds a sequence is inside the ring and stop() has to
// wait for it, and a publisher preempted before its claim must be turned away
// instead of walking into a ring that is being torn down.
//
// Every test here drives those windows from the test rather than from a sleep:
// a copy constructor that blocks until the test releases it, a hook that holds
// a publisher on the claim itself, an event whose constructor throws. The one
// exception is the stress loop, whose assertion is ThreadSanitizer.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "lrvx/util/eventing/event_bus.h"

namespace lrvx
{

// The bus writes the sequence into the event AFTER it has stamped the slot as
// constructed, and that write is the one place where a live publisher and
// stop()'s teardown touch the same bytes with nothing ordering them. In the
// real thing the window is two instructions wide; a proxy whose assignment
// takes its time makes it microseconds, which is what gives a race detector
// something to catch.
struct Stamp
{
  static std::atomic<int> delayUs;

  uint64_t value{0};

  Stamp& operator=(uint64_t v)
  {
    const int delay = delayUs.load(std::memory_order_relaxed);
    if (delay != 0)
    {
      std::this_thread::sleep_for(std::chrono::microseconds(delay));
    }
    value = v;
    return *this;
  }
};

std::atomic<int> Stamp::delayUs{0};

// What the tests do to the copy the bus makes into a ring slot. The copy IS
// the construct-into-the-slot step, so this is where a test stands to hold a
// publisher inside the ring, or to make its constructor fail.
struct CopyControl
{
  // Microseconds each copy costs. Widens the window a race detector has to
  // catch; nothing about a test's verdict depends on it.
  static std::atomic<int> delayUs;
  // A copy blocks until the test clears this. Nobody's timer decides when the
  // publisher leaves the ring -- the test does.
  static std::atomic<bool> hold;
  // Raised by a copy that is being held, so the test can wait for a publisher
  // to actually be inside the ring instead of guessing.
  static std::atomic<bool> held;
  // The next copy throws, once. An event constructor that fails leaves a
  // claimed sequence behind that nobody will ever stamp.
  static std::atomic<bool> throwNext;
  static std::atomic<uint64_t> finished;

  static void reset()
  {
    delayUs.store(0, std::memory_order_relaxed);
    hold.store(false, std::memory_order_relaxed);
    held.store(false, std::memory_order_relaxed);
    throwNext.store(false, std::memory_order_relaxed);
    finished.store(0, std::memory_order_relaxed);
    Stamp::delayUs.store(0, std::memory_order_relaxed);
  }
};

std::atomic<int> CopyControl::delayUs{0};
std::atomic<bool> CopyControl::hold{false};
std::atomic<bool> CopyControl::held{false};
std::atomic<bool> CopyControl::throwNext{false};
std::atomic<uint64_t> CopyControl::finished{0};

// A destructor that touches the object, the way an event holding a pool
// handle or a refcount does. Without one the teardown's ~Event() compiles to
// nothing and its overlap with a publisher still inside the ring is invisible
// to a race detector.
struct LifecycleEvent
{
  using Listener = struct ILifecycleListener
  {
    virtual ~ILifecycleListener() = default;
    virtual void onEvent(const LifecycleEvent& e) = 0;
  };

  int64_t payload[8]{};
  int value{0};
  Stamp tickSequence{};

  ~LifecycleEvent()
  {
    tickSequence.value = 0;
    payload[0] = -1;
  }

  LifecycleEvent() = default;

  LifecycleEvent(const LifecycleEvent& other) : value(other.value)
  {
    tickSequence.value = other.tickSequence.value;
    for (size_t i = 0; i < 8; ++i)
    {
      payload[i] = other.payload[i];
    }

    if (CopyControl::throwNext.exchange(false, std::memory_order_acq_rel))
    {
      throw std::runtime_error("event constructor failed");
    }

    const int delay = CopyControl::delayUs.load(std::memory_order_relaxed);
    if (delay != 0)
    {
      std::this_thread::sleep_for(std::chrono::microseconds(delay));
    }

    if (CopyControl::hold.load(std::memory_order_acquire))
    {
      CopyControl::held.store(true, std::memory_order_release);
      while (CopyControl::hold.load(std::memory_order_acquire))
      {
        std::this_thread::sleep_for(std::chrono::microseconds(200));
      }
      CopyControl::held.store(false, std::memory_order_release);
    }

    CopyControl::finished.fetch_add(1, std::memory_order_release);
  }

  LifecycleEvent& operator=(const LifecycleEvent&) = default;
};

template <>
struct EventDispatcher<LifecycleEvent>
{
  static void dispatch(const LifecycleEvent& event, LifecycleEvent::Listener& listener)
  {
    listener.onEvent(event);
  }
};

}  // namespace lrvx

namespace
{

using namespace lrvx;

using LifecycleBus = EventBus<LifecycleEvent, 64, 4>;

class LifecycleCounting : public LifecycleEvent::Listener
{
 public:
  void onEvent(const LifecycleEvent& e) override
  {
    last.store(e.value, std::memory_order_relaxed);
    count.fetch_add(1, std::memory_order_relaxed);
  }
  std::atomic<uint64_t> count{0};
  std::atomic<int> last{0};
};

// Blocks inside the handler until the test lets go, which is how a consumer is
// made to stop moving at an instant the test picks: gating stops with it, and
// a publisher ends up on whichever gate the test wanted it on.
class HeldListener : public LifecycleEvent::Listener
{
 public:
  void onEvent(const LifecycleEvent&) override
  {
    entered.fetch_add(1, std::memory_order_release);
    while (hold.load(std::memory_order_acquire))
    {
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    count.fetch_add(1, std::memory_order_relaxed);
  }
  void release() { hold.store(false, std::memory_order_release); }

  std::atomic<bool> hold{true};
  std::atomic<uint64_t> entered{0};
  std::atomic<uint64_t> count{0};
};

template <typename Pred>
bool waitUntil(Pred pred, std::chrono::milliseconds bound)
{
  const auto deadline = std::chrono::steady_clock::now() + bound;
  while (!pred())
  {
    if (std::chrono::steady_clock::now() > deadline)
    {
      return false;
    }
    std::this_thread::yield();
  }
  return true;
}

// stop() on a thread of its own, with a bound on how long the test is willing
// to wait for it.
//
// A stop() that never returns is a real failure mode here -- a claim nobody
// resolved keeps it waiting for good -- and a test that pins it must not take
// the whole binary down with it. On the bound the stopper is detached and the
// bus is deliberately leaked: a thread is still inside it, and destroying it
// underneath would turn a clear failure into a crash somewhere else.
struct Stopper
{
  std::shared_ptr<std::atomic<bool>> done = std::make_shared<std::atomic<bool>>(false);
  std::thread thread;

  explicit Stopper(LifecycleBus* bus)
  {
    auto flag = done;
    thread = std::thread(
        [bus, flag]
        {
          bus->stop();
          flag->store(true, std::memory_order_release);
        });
  }

  bool returned() const { return done->load(std::memory_order_acquire); }

  bool wait(std::chrono::milliseconds bound)
  {
    if (!waitUntil([this]
                   { return returned(); }, bound))
    {
      thread.detach();
      return false;
    }
    thread.join();
    return true;
  }
};

// The publish path's seam (see NoPublishSeam): a publisher is held at the one
// instant no test can otherwise reach -- past the _running check, not yet
// holding a sequence. Publishers are let go one at a time, in the order they
// arrived, so what the second one is told does not depend on how the first one
// was scheduled.
struct ParkingSeam
{
  static std::atomic<bool> arm;
  static std::atomic<int> parked;
  static std::atomic<int> allowed;
  static std::atomic<int> finished;

  static void beforeClaim() noexcept
  {
    if (!arm.load(std::memory_order_acquire))
    {
      return;
    }
    const int ticket = parked.fetch_add(1, std::memory_order_acq_rel);
    while (allowed.load(std::memory_order_acquire) <= ticket)
    {
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
  }

  static void reset()
  {
    arm.store(false, std::memory_order_relaxed);
    parked.store(0, std::memory_order_relaxed);
    allowed.store(0, std::memory_order_relaxed);
    finished.store(0, std::memory_order_relaxed);
  }
};

std::atomic<bool> ParkingSeam::arm{false};
std::atomic<int> ParkingSeam::parked{0};
std::atomic<int> ParkingSeam::allowed{0};
std::atomic<int> ParkingSeam::finished{0};

using SeamBus = EventBus<LifecycleEvent, 64, 4, ParkingSeam>;

// The window a flag cannot cover: a publisher reads _running, is preempted,
// and comes back after the bus has stopped and its ring has been torn down.
// What turns it away is the sequence line itself, and the line has to stay
// closed -- including for the publisher that comes after a refused claim,
// which must not be able to put the line back.
TEST(EventBusLifecycle, APublisherPreemptedBeforeItsClaimIsTurnedAway)
{
  CopyControl::reset();
  ParkingSeam::reset();

  SeamBus bus;
  LifecycleCounting listener;
  ASSERT_TRUE(bus.subscribe(&listener, /*required=*/true));
  bus.start();
  ASSERT_FALSE(bus.sequenceLineClosed()) << "a running bus has an open sequence line";

  {
    LifecycleEvent ev;
    for (int i = 0; i < 3; ++i)
    {
      ev.value = i;
      ASSERT_GE(bus.publish(ev), 0);
    }
  }
  ASSERT_TRUE(waitUntil([&]
                        { return listener.count.load(std::memory_order_relaxed) == 3u; },
                        std::chrono::milliseconds(5000)));

  // Two publishers, both preempted in the seam before the stop.
  ParkingSeam::arm.store(true, std::memory_order_release);
  std::atomic<int64_t> first{1};
  std::atomic<int64_t> second{1};  // 1 is not a sequence this bus can hand out
  std::thread lateA(
      [&bus, &first]
      {
        LifecycleEvent ev;
        ev.value = 41;
        first.store(bus.publish(ev), std::memory_order_release);
        ParkingSeam::finished.fetch_add(1, std::memory_order_acq_rel);
      });
  std::thread lateB(
      [&bus, &second]
      {
        LifecycleEvent ev;
        ev.value = 42;
        second.store(bus.publish(ev), std::memory_order_release);
        ParkingSeam::finished.fetch_add(1, std::memory_order_acq_rel);
      });

  ASSERT_TRUE(waitUntil([]
                        { return ParkingSeam::parked.load(std::memory_order_acquire) == 2; },
                        std::chrono::milliseconds(5000)))
      << "the publishers never reached the seam";

  // Neither holds a sequence, so there is nothing for stop() to wait out.
  bus.stop();
  EXPECT_TRUE(bus.sequenceLineClosed()) << "stop() left the sequence line open";

  ParkingSeam::allowed.store(1, std::memory_order_release);
  ASSERT_TRUE(waitUntil([]
                        { return ParkingSeam::finished.load(std::memory_order_acquire) == 1; },
                        std::chrono::milliseconds(5000)));
  EXPECT_TRUE(bus.sequenceLineClosed())
      << "a claim refused by the closed line put the line back";

  ParkingSeam::allowed.store(2, std::memory_order_release);
  lateA.join();
  lateB.join();

  EXPECT_LT(first.load(std::memory_order_acquire), 0)
      << "a publisher preempted before its claim was given a sequence by a stopped bus";
  EXPECT_LT(second.load(std::memory_order_acquire), 0)
      << "the publisher behind a refused claim was given a sequence by a stopped bus";
  EXPECT_TRUE(bus.sequenceLineClosed());
  EXPECT_EQ(bus.stats().published, 3u) << "a turned-away publisher touched the ring";
  EXPECT_EQ(listener.count.load(std::memory_order_relaxed), 3u);

  ParkingSeam::reset();
  CopyControl::reset();
}

// ---------------------------------------------------------------------------
// A publisher already inside the ring.
//
// The copy into the slot blocks until this test lets go, so the window is not
// a sleep that might be too short on a loaded machine: it is open until the
// test closes it. stop() returning is the moment the ring may be torn down, so
// the contract is that it does not return while somebody is writing into a
// slot -- for as long as that takes, not for as long as some bound inside
// stop() is prepared to wait.
TEST(EventBusLifecycle, StopDoesNotReturnWhileAPublisherIsInsideTheRing)
{
  CopyControl::reset();
  auto* bus = new LifecycleBus;
  LifecycleCounting listener;
  ASSERT_TRUE(bus->subscribe(&listener));
  bus->start();

  CopyControl::hold.store(true, std::memory_order_release);
  std::thread publisher(
      [bus]
      {
        LifecycleEvent ev;
        ev.value = 7;
        bus->publish(ev);
      });

  ASSERT_TRUE(waitUntil([]
                        { return CopyControl::held.load(std::memory_order_acquire); },
                        std::chrono::milliseconds(5000)))
      << "the publisher never reached the slot";

  Stopper stopper(bus);

  // Longer than any bound a stop() might be tempted to put on its own wait.
  std::this_thread::sleep_for(std::chrono::milliseconds(2500));
  const bool returnedWhileHeld = stopper.returned();

  CopyControl::hold.store(false, std::memory_order_release);
  const bool returnedAfterRelease = stopper.wait(std::chrono::milliseconds(5000));
  publisher.join();

  EXPECT_FALSE(returnedWhileHeld)
      << "stop() returned while a publisher was still constructing into a slot";
  ASSERT_TRUE(returnedAfterRelease)
      << "stop() never returned after the publisher left the ring";  // bus leaked on purpose
  EXPECT_EQ(CopyControl::finished.load(std::memory_order_acquire), 1u);
  delete bus;
  CopyControl::reset();
}

// The same contract on a bus that has been round the ISubsystem cycle once
// already. The counters stop() reckons with are cumulative over the life of
// the bus, so a second run that does not take its own baseline finds itself
// even before it starts and waits for nobody.
TEST(EventBusLifecycle, StopOnASecondRunStillWaitsForAPublisherInsideTheRing)
{
  CopyControl::reset();
  auto* bus = new LifecycleBus;
  LifecycleCounting listener;
  ASSERT_TRUE(bus->subscribe(&listener));

  bus->start();
  {
    LifecycleEvent ev;
    for (int i = 0; i < 100; ++i)
    {
      ev.value = i;
      ASSERT_GE(bus->publish(ev), 0);
    }
  }
  bus->flush();
  bus->stop();
  ASSERT_EQ(listener.count.load(std::memory_order_relaxed), 100u);

  bus->start();
  {
    LifecycleEvent ev;
    for (int i = 0; i < 10; ++i)
    {
      ev.value = i;
      ASSERT_GE(bus->publish(ev), 0);
    }
  }

  CopyControl::hold.store(true, std::memory_order_release);
  std::thread publisher(
      [bus]
      {
        LifecycleEvent ev;
        ev.value = 11;
        bus->publish(ev);
      });

  ASSERT_TRUE(waitUntil([]
                        { return CopyControl::held.load(std::memory_order_acquire); },
                        std::chrono::milliseconds(5000)))
      << "the publisher never reached the slot";

  Stopper stopper(bus);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  const bool returnedWhileHeld = stopper.returned();

  CopyControl::hold.store(false, std::memory_order_release);
  const bool returnedAfterRelease = stopper.wait(std::chrono::milliseconds(5000));
  publisher.join();

  EXPECT_FALSE(returnedWhileHeld)
      << "the second run's stop() returned while a publisher was inside the ring: "
         "it counted the first run's publishes as its own";
  ASSERT_TRUE(returnedAfterRelease);  // bus leaked on purpose
  delete bus;
  CopyControl::reset();
}

// An event the bus accepted (publish returned a sequence) and then dropped on
// the way down is a silent loss: the publisher has no way to learn about it.
// Either the publish is refused -- the bus says no, the caller knows -- or the
// event is delivered. Two publishers, so that a sequence one of them gives up
// on has another's accepted events behind it.
TEST(EventBusLifecycle, EveryPublishAcrossAStopIsEitherDeliveredOrRefused)
{
  for (int round = 0; round < 24; ++round)
  {
    CopyControl::reset();
    LifecycleBus bus;
    LifecycleCounting listener;
    ASSERT_TRUE(bus.subscribe(&listener, /*required=*/true));
    bus.enableDrainOnStop();
    bus.start();
    CopyControl::delayUs.store(20, std::memory_order_relaxed);

    std::atomic<uint64_t> accepted{0};
    std::atomic<uint64_t> refused{0};
    std::vector<std::thread> publishers;
    for (int t = 0; t < 2; ++t)
    {
      publishers.emplace_back(
          [&bus, &accepted, &refused]
          {
            LifecycleEvent ev;
            for (int i = 0; i < 1000000; ++i)
            {
              ev.value = i;
              if (bus.publish(ev) >= 0)
              {
                accepted.fetch_add(1, std::memory_order_relaxed);
              }
              else
              {
                refused.fetch_add(1, std::memory_order_relaxed);
                return;  // the bus said no; nothing after this can be accepted
              }
            }
          });
    }

    // Wait for the publishers to be going rather than for a clock: a fixed
    // sleep on a loaded machine can stop a bus nobody has published to yet,
    // and then the round tests nothing.
    ASSERT_TRUE(waitUntil([&]
                          { return accepted.load(std::memory_order_relaxed) >= 200; },
                          std::chrono::milliseconds(10000)))
        << "round " << round << ": the publishers never got going";
    bus.stop();
    for (auto& p : publishers)
    {
      p.join();
    }
    CopyControl::reset();

    const uint64_t delivered = listener.count.load(std::memory_order_relaxed);
    const uint64_t taken = accepted.load(std::memory_order_relaxed);
    ASSERT_GT(refused.load(std::memory_order_relaxed), 0u)
        << "round " << round << ": no publisher was turned away; nothing was raced";
    EXPECT_EQ(delivered, taken)
        << "round " << round << ": " << (taken - delivered)
        << " events were accepted by publish() and never delivered";
  }
}

// A sequence nobody will ever stamp, with accepted events behind it, without
// waiting for a race to produce one: an event constructor that throws leaves
// exactly that hole. The consumer stops at it -- it cannot know whether the
// slot is late or dead -- and the drain on the way down is what has to step
// over the number and hand over what was published after it.
TEST(EventBusLifecycle, ADrainStepsOverASequenceItsPublisherGaveUpOn)
{
  CopyControl::reset();
  auto* bus = new LifecycleBus;
  LifecycleCounting listener;
  ASSERT_TRUE(bus->subscribe(&listener, /*required=*/true));
  bus->enableDrainOnStop();
  bus->start();

  LifecycleEvent ev;
  for (int i = 0; i < 3; ++i)
  {
    ev.value = i;
    ASSERT_GE(bus->publish(ev), 0);
  }
  ASSERT_TRUE(waitUntil([&]
                        { return listener.count.load(std::memory_order_relaxed) == 3u; },
                        std::chrono::milliseconds(5000)));

  CopyControl::throwNext.store(true, std::memory_order_release);
  ev.value = 3;
  EXPECT_THROW(bus->publish(ev), std::runtime_error);

  for (int i = 4; i < 6; ++i)
  {
    ev.value = i;
    ASSERT_GE(bus->publish(ev), 0);
  }

  // The hole is in front of the consumer, so nothing else reaches it while the
  // bus runs.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  ASSERT_EQ(listener.count.load(std::memory_order_relaxed), 3u);

  Stopper stopper(bus);
  ASSERT_TRUE(stopper.wait(std::chrono::milliseconds(5000)))
      << "stop() never returned: the sequence whose constructor threw was never resolved";

  EXPECT_EQ(listener.count.load(std::memory_order_relaxed), 5u)
      << "the drain stopped at the sequence its publisher gave up on and stranded the "
         "accepted events behind it";
  EXPECT_EQ(listener.last.load(std::memory_order_relaxed), 5);
  delete bus;
  CopyControl::reset();
}

// The same throwing constructor, asked about stop() rather than the drain: the
// sequence it holds is resolved on the way out or stop() waits for it for ever.
TEST(EventBusLifecycle, StopReturnsWhenAnEventConstructorThrew)
{
  CopyControl::reset();
  auto* bus = new LifecycleBus;
  LifecycleCounting listener;
  ASSERT_TRUE(bus->subscribe(&listener, /*required=*/true));
  bus->start();

  LifecycleEvent ev;
  ev.value = 1;
  ASSERT_GE(bus->publish(ev), 0);

  CopyControl::throwNext.store(true, std::memory_order_release);
  ev.value = 2;
  EXPECT_THROW(bus->publish(ev), std::runtime_error);

  Stopper stopper(bus);
  ASSERT_TRUE(stopper.wait(std::chrono::milliseconds(5000)))
      << "stop() waited for a claim whose event constructor threw";  // bus leaked on purpose
  delete bus;
  CopyControl::reset();
}

// A publisher that gives up rather than one that publishes. The consumer is
// held inside its first event, so gating never moves and the ring fills; the
// next publisher parks on the wrap gate and leaves empty-handed when the bus
// stops. The sequence it was holding still has to be accounted for, or stop()
// waits for a publish that is never coming.
TEST(EventBusLifecycle, StopReturnsWithAPublisherGivingUpAtTheWrapGate)
{
  CopyControl::reset();
  auto* bus = new LifecycleBus;  // capacity 64
  HeldListener held;
  ASSERT_TRUE(bus->subscribe(&held, /*required=*/true));
  bus->start();

  std::atomic<uint64_t> accepted{0};
  std::atomic<uint64_t> refused{0};
  std::thread publisher(
      [bus, &accepted, &refused]
      {
        LifecycleEvent ev;
        for (int i = 0; i < 200; ++i)
        {
          ev.value = i;
          if (bus->publish(ev) >= 0)
          {
            accepted.fetch_add(1, std::memory_order_relaxed);
          }
          else
          {
            refused.fetch_add(1, std::memory_order_relaxed);
            return;
          }
        }
      });

  ASSERT_TRUE(waitUntil([&]
                        { return accepted.load(std::memory_order_relaxed) == 64u; },
                        std::chrono::milliseconds(5000)))
      << "the ring did not fill; nobody is on the wrap gate";
  // It cannot get any further: the consumer is holding the only gating line.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  ASSERT_EQ(accepted.load(std::memory_order_relaxed), 64u);

  Stopper stopper(bus);
  // Released on the publisher's give-up, not on a timer: the consumer has to
  // stay put until the publisher has actually been turned away from the gate,
  // and it has to be let go for stop() to be able to join it.
  waitUntil([&]
            { return refused.load(std::memory_order_relaxed) > 0; },
            std::chrono::milliseconds(5000));
  held.release();

  // Joined before the verdict: a test that walks out on a joinable thread takes
  // the process with it, and the message below is the point of the test.
  const bool returned = stopper.wait(std::chrono::milliseconds(5000));
  publisher.join();
  EXPECT_EQ(refused.load(std::memory_order_relaxed), 1u);
  CopyControl::reset();
  ASSERT_TRUE(returned)
      << "stop() waited for a sequence the wrap gate gave up on without counting it";
  delete bus;
}

// The other give-up path. The required consumer keeps up, so the wrap gate
// opens; an optional consumer held inside its first event keeps the reclaim
// fence shut, which is where the publisher is standing when the bus stops.
TEST(EventBusLifecycle, StopReturnsWithAPublisherGivingUpAtTheReclaimFence)
{
  CopyControl::reset();
  auto* bus = new LifecycleBus;  // capacity 64
  LifecycleCounting fast;
  HeldListener slow;
  ASSERT_TRUE(bus->subscribe(&fast, /*required=*/true));
  ASSERT_TRUE(bus->subscribe(&slow, /*required=*/false));
  bus->start();

  std::atomic<uint64_t> accepted{0};
  std::atomic<uint64_t> refused{0};
  std::thread publisher(
      [bus, &accepted, &refused]
      {
        LifecycleEvent ev;
        for (int i = 0; i < 200; ++i)
        {
          ev.value = i;
          if (bus->publish(ev) >= 0)
          {
            accepted.fetch_add(1, std::memory_order_relaxed);
          }
          else
          {
            refused.fetch_add(1, std::memory_order_relaxed);
            return;
          }
        }
      });

  ASSERT_TRUE(waitUntil([&]
                        { return accepted.load(std::memory_order_relaxed) == 64u; },
                        std::chrono::milliseconds(5000)))
      << "the publisher never reached the reclaim fence";
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  ASSERT_EQ(accepted.load(std::memory_order_relaxed), 64u);
  ASSERT_EQ(slow.entered.load(std::memory_order_relaxed), 1u);

  Stopper stopper(bus);
  waitUntil([&]
            { return refused.load(std::memory_order_relaxed) > 0; },
            std::chrono::milliseconds(5000));
  slow.release();

  const bool returned = stopper.wait(std::chrono::milliseconds(5000));
  publisher.join();
  EXPECT_EQ(refused.load(std::memory_order_relaxed), 1u);
  CopyControl::reset();
  ASSERT_TRUE(returned)
      << "stop() waited for a sequence the reclaim fence gave up on without counting it";
  delete bus;
}

// The stress half: publishers in a loop against a stop. In a plain build this
// only has to survive; under ThreadSanitizer it is the reproduction, because
// the teardown's ~Event() and a publisher's write into the same slot are a
// race TSan names. The restart at the end is the outcome assertion that
// accompanies it: stop() returning has to mean the ring is quiescent and
// usable again.
TEST(EventBusLifecycle, PublishInALoopAgainstStopIsRaceFree)
{
  for (int round = 0; round < 24; ++round)
  {
    CopyControl::reset();
    LifecycleBus bus;
    LifecycleCounting listener;
    ASSERT_TRUE(bus.subscribe(&listener, /*required=*/false));
    bus.start();
    Stamp::delayUs.store(5, std::memory_order_relaxed);

    std::atomic<uint64_t> published{0};
    std::vector<std::thread> publishers;
    for (int t = 0; t < 3; ++t)
    {
      publishers.emplace_back(
          [&bus, &published]
          {
            LifecycleEvent ev;
            for (int i = 0; i < 100000; ++i)
            {
              ev.value = i;
              if (bus.publish(ev) < 0)
              {
                return;  // the bus stopped underneath; that is the point
              }
              published.fetch_add(1, std::memory_order_relaxed);
            }
          });
    }

    ASSERT_TRUE(waitUntil([&]
                          { return published.load(std::memory_order_relaxed) >= 50; },
                          std::chrono::milliseconds(10000)))
        << "round " << round << ": the publishers never got going";
    // A different stop instant every round, so the teardown lands somewhere
    // else in the publishers' loops each time.
    std::this_thread::sleep_for(std::chrono::microseconds(50 + 97 * round));
    bus.stop();

    for (auto& p : publishers)
    {
      p.join();
    }
    CopyControl::reset();

    bus.start();
    LifecycleEvent ev;
    ev.value = 42;
    EXPECT_GE(bus.publish(ev), 0) << "round " << round << ": the bus did not come back up";
    bus.stop();
  }
}

// Control: the same shapes with no overlap at all. A stop that follows the
// last publish delivers everything and loses nothing, today and after.
TEST(EventBusLifecycle, StopAfterTheLastPublishDeliversEverything)
{
  CopyControl::reset();
  LifecycleBus bus;
  LifecycleCounting listener;
  ASSERT_TRUE(bus.subscribe(&listener, /*required=*/true));
  bus.enableDrainOnStop();
  bus.start();

  LifecycleEvent ev;
  for (int i = 0; i < 1000; ++i)
  {
    ev.value = i;
    ASSERT_GE(bus.publish(ev), 0);
  }
  bus.flush();
  bus.stop();

  EXPECT_EQ(listener.count.load(std::memory_order_relaxed), 1000u);
  EXPECT_EQ(listener.last.load(std::memory_order_relaxed), 999);
}

}  // namespace

namespace
{

// An event that counts its constructions into a slot and its destructions, so
// a test can tell that stop() destroyed every event still in the ring, once.
struct CountedEvent
{
  using Listener = struct ICountedListener
  {
    virtual ~ICountedListener() = default;
    virtual void onEvent(const CountedEvent& e) = 0;
  };

  static std::atomic<int64_t> alive;

  int value{0};
  uint64_t tickSequence{0};
  bool inRing{false};

  CountedEvent() = default;
  CountedEvent(const CountedEvent& other)
      : value(other.value), tickSequence(other.tickSequence), inRing(true)
  {
    alive.fetch_add(1, std::memory_order_relaxed);
  }
  CountedEvent& operator=(const CountedEvent&) = default;
  ~CountedEvent()
  {
    if (inRing)
    {
      alive.fetch_sub(1, std::memory_order_relaxed);
    }
  }
};

std::atomic<int64_t> CountedEvent::alive{0};

class CountedListener : public CountedEvent::Listener
{
 public:
  void onEvent(const CountedEvent& e) override
  {
    values.push_back(e.value);
    sequences.push_back(e.tickSequence);
  }
  std::vector<int> values;
  std::vector<uint64_t> sequences;
};

}  // namespace

namespace lrvx
{
template <>
struct EventDispatcher<CountedEvent>
{
  static void dispatch(const CountedEvent& event, CountedEvent::Listener& listener)
  {
    listener.onEvent(event);
  }
};
}  // namespace lrvx

namespace
{

// stop() resets only the slots the run claimed. Whatever the run did -- nothing
// at all, a handful of events, exactly a ring of them, several rings -- the
// events left in the ring are destroyed once each, and the next run on the
// same bus starts on an empty ring at sequence 0.
TEST(EventBusLifecycle, StopEmptiesTheRingHoweverMuchOfItARunUsed)
{
  constexpr size_t kCapacity = 64;
  using Bus = EventBus<CountedEvent, kCapacity, 2>;
  auto bus = std::make_unique<Bus>();
  CountedListener listener;
  ASSERT_TRUE(bus->subscribe(&listener, /*required=*/true));
  bus->enableDrainOnStop();
  CountedEvent::alive.store(0, std::memory_order_relaxed);

  int nextValue = 0;
  // The run after a large one is a small one, so slots the earlier run used
  // and this one does not are seen to have been emptied.
  for (const size_t published : {size_t{0}, size_t{5}, kCapacity - 1, kCapacity, kCapacity + 1,
                                 3 * kCapacity + 7, size_t{1}, size_t{0}, size_t{2}})
  {
    SCOPED_TRACE("a run of " + std::to_string(published) + " events");
    listener.values.clear();
    listener.sequences.clear();
    bus->start();
    const int first = nextValue;
    CountedEvent ev;
    for (size_t i = 0; i < published; ++i)
    {
      ev.value = nextValue++;
      ASSERT_EQ(bus->publish(ev), static_cast<int64_t>(i));
    }
    bus->flush();
    bus->stop();

    EXPECT_EQ(CountedEvent::alive.load(std::memory_order_relaxed), 0)
        << "every event constructed into the ring was destroyed, once";
    ASSERT_EQ(listener.values.size(), published);
    for (size_t i = 0; i < published; ++i)
    {
      EXPECT_EQ(listener.values[i], first + static_cast<int>(i));
      EXPECT_EQ(listener.sequences[i], i) << "a run starts at sequence 0";
    }
  }

  // And a bus destroyed with events still in it, never stopped by hand.
  bus->start();
  CountedEvent ev;
  for (int i = 0; i < 9; ++i)
  {
    ASSERT_GE(bus->publish(ev), 0);
  }
  bus.reset();
  EXPECT_EQ(CountedEvent::alive.load(std::memory_order_relaxed), 0);
}

// The same on a ring the size a venue shard uses, where the walk stop() no
// longer makes was 65536 slots for every run.
TEST(EventBusLifecycle, AShortRunOnALargeRingLeavesItEmpty)
{
  using Bus = EventBus<CountedEvent, 1 << 16, 2>;
  auto bus = std::make_unique<Bus>();
  CountedListener listener;
  ASSERT_TRUE(bus->subscribe(&listener, /*required=*/true));
  bus->enableDrainOnStop();
  CountedEvent::alive.store(0, std::memory_order_relaxed);

  for (int run = 0; run < 3; ++run)
  {
    listener.values.clear();
    bus->start();
    CountedEvent ev;
    for (int i = 0; i < 10; ++i)
    {
      ev.value = run * 100 + i;
      ASSERT_EQ(bus->publish(ev), i);
    }
    bus->flush();
    bus->stop();
    EXPECT_EQ(CountedEvent::alive.load(std::memory_order_relaxed), 0);
    ASSERT_EQ(listener.values.size(), 10u);
    EXPECT_EQ(listener.values.front(), run * 100);
    EXPECT_EQ(listener.values.back(), run * 100 + 9);
  }
}

// The constructor stamps slot 0 alone: it is the one slot a reader can ask
// for sequence 0, which is what every other stamp starts as. A bus that has
// published nothing must have nothing for a consumer, at construction and
// after any number of runs, and what it then publishes must arrive once, in
// order, from sequence 0.
template <size_t Capacity>
void expectNothingBeforeTheFirstPublish()
{
  SCOPED_TRACE("capacity " + std::to_string(Capacity));
  using Bus = EventBus<CountedEvent, Capacity, 2>;
  auto bus = std::make_unique<Bus>();
  CountedListener listener;
  bus->setOwnConsumerThreads(false);
  ASSERT_TRUE(bus->subscribe(&listener, /*required=*/true));
  CountedEvent::alive.store(0, std::memory_order_relaxed);

  // Runs that publish nothing, then runs that end exactly on slot 0, one
  // slot past it and well past it, each followed by an empty run.
  for (const size_t published :
       {size_t{0}, size_t{0}, size_t{1}, size_t{0}, Capacity, size_t{0}, Capacity + 1, size_t{0},
        2 * Capacity + 3, size_t{0}})
  {
    SCOPED_TRACE("a run of " + std::to_string(published) + " events");
    listener.values.clear();
    listener.sequences.clear();
    bus->start();
    EXPECT_FALSE(bus->consumerHasPending(0)) << "nothing is published yet";
    EXPECT_FALSE(bus->pollConsumer(0));
    EXPECT_TRUE(listener.values.empty()) << "an event nobody published was delivered";

    CountedEvent ev;
    for (size_t i = 0; i < published; ++i)
    {
      ev.value = static_cast<int>(i);
      ASSERT_EQ(bus->publish(ev), static_cast<int64_t>(i));
      // Stepped after every publish, so the ring never fills.
      ASSERT_TRUE(bus->consumerHasPending(0));
      ASSERT_TRUE(bus->pollConsumer(0));
      ASSERT_FALSE(bus->consumerHasPending(0));
    }
    bus->stop();

    ASSERT_EQ(listener.values.size(), published);
    for (size_t i = 0; i < published; ++i)
    {
      ASSERT_EQ(listener.values[i], static_cast<int>(i));
      ASSERT_EQ(listener.sequences[i], i);
    }
    EXPECT_EQ(CountedEvent::alive.load(std::memory_order_relaxed), 0);
  }
}

TEST(EventBusLifecycle, ABusThatPublishedNothingHasNothingAtSequenceZero)
{
  expectNothingBeforeTheFirstPublish<2>();
  expectNothingBeforeTheFirstPublish<8>();
  expectNothingBeforeTheFirstPublish<64>();
  expectNothingBeforeTheFirstPublish<1 << 12>();
}

// The same with consumer threads: a thread started on a fresh bus waits; it
// does not deliver from slot 0.
TEST(EventBusLifecycle, AConsumerThreadOnAFreshBusDeliversOnlyWhatIsPublished)
{
  using Bus = EventBus<CountedEvent, 16, 2>;
  for (int round = 0; round < 50; ++round)
  {
    auto bus = std::make_unique<Bus>();
    CountedListener listener;
    ASSERT_TRUE(bus->subscribe(&listener, /*required=*/true));
    bus->enableDrainOnStop();
    bus->start();
    std::this_thread::sleep_for(std::chrono::microseconds(200));
    const size_t published = static_cast<size_t>(round % 3);
    CountedEvent ev;
    for (size_t i = 0; i < published; ++i)
    {
      ev.value = static_cast<int>(i) + 1000;
      ASSERT_EQ(bus->publish(ev), static_cast<int64_t>(i));
    }
    bus->flush();
    bus->stop();
    ASSERT_EQ(listener.values.size(), published) << "round " << round;
    for (size_t i = 0; i < published; ++i)
    {
      EXPECT_EQ(listener.values[i], static_cast<int>(i) + 1000);
    }
  }
}

}  // namespace
