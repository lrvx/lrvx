/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <thread>
#include <type_traits>

#include "lrvx/engine/abstract_subsystem.h"
#include "lrvx/engine/engine_config.h"
#include "lrvx/engine/event_dispatcher.h"
#include "lrvx/log/log.h"
#include "lrvx/util/base/sanitizer.h"
#include "lrvx/util/concurrency/jthread.h"
#include "lrvx/util/eventing/wake_set.h"
#include "lrvx/util/memory/pool.h"
#include "lrvx/util/performance/busy_backoff.h"
#include "lrvx/util/performance/profile.h"

#if LRVX_CPU_AFFINITY_ENABLED
#include "lrvx/util/performance/cpu_affinity.h"
#include "lrvx/util/performance/rt_spin_guard.h"
#endif

namespace lrvx
{

template <typename T>
struct ListenerType
{
  using type = typename T::Listener;
};

template <typename T>
struct ListenerType<pool::Handle<T>>
{
  using type = typename T::Listener;
};

// How a consumer waits when its ring is empty.
//
// ACTIVE is the historical behaviour and the default: spin, yield, then short
// sleeps (see BusyBackoff). It never blocks, which is why an idle consumer
// still costs about a fifth of a core -- the right trade for one bus on
// hardware it owns, and the wrong one for a process holding hundreds of them.
//
// PARKED blocks on a condition variable and is woken by the publisher. It
// costs nothing while idle and pays for that in wake-up latency.
//
// Deliberately not a member of the bus template: a process wires shards,
// feeds and sinks of several bus types to one decision, and a per
// instantiation enum would make that decision untypeable.
enum class ConsumerWaitMode : uint8_t
{
  ACTIVE,
  PARKED
};

// The publish path's one seam, and the only window in the bus a test cannot
// reach from outside: the instant after publish() has read _running and before
// it claims a sequence. That window is the whole reason stop() closes the
// sequence line instead of trusting the flag -- a publisher preempted in it
// comes back with an answer that is arbitrarily old -- and a fix for a window
// no test can open is a fix nobody can check.
//
// A policy rather than an #ifdef because the seam must cost nothing and must
// not change what a production bus IS. The default's hook is an empty static
// function: it inlines to nothing, leaves no branch behind and adds no member,
// so the publish path is instruction-for-instruction what it was. And a bus
// carrying a test seam is a different type from the bus the engine builds,
// rather than the same type compiled two ways in two translation units, which
// is how a probe added under a macro ends up with one definition in the test
// and another in the library.
struct NoPublishSeam
{
  // Called between publish()'s read of _running and its claim. Also on the
  // batch path, between the same read and the batch's reservation.
  static void beforeClaim() noexcept {}
};

template <typename Event,
          size_t CapacityPow2 = config::DEFAULT_EVENTBUS_CAPACITY,
          size_t MaxConsumers = config::DEFAULT_EVENTBUS_MAX_CONSUMERS,
          typename PublishSeam = NoPublishSeam>
class EventBus : public ISubsystem
{
  static_assert(CapacityPow2 > 0, "Capacity must be > 0");
  static_assert((CapacityPow2 & (CapacityPow2 - 1)) == 0, "Capacity must be power of 2");
  static constexpr size_t Mask = CapacityPow2 - 1;
  // Upper bound on how many contiguous published events a consumer delivers
  // before publishing its progress (see the worker loop batch effect).
  static constexpr size_t kMaxConsumeRun = 1024;

 public:
  using Listener = typename ListenerType<Event>::type;

#if LRVX_CPU_AFFINITY_ENABLED
  enum class ComponentType
  {
    MARKET_DATA,
    EXECUTION,
    STRATEGY,
    RISK,
    GENERAL
  };

  struct AffinityConfig
  {
    ComponentType componentType = ComponentType::GENERAL;
    bool enableRealTimePriority = true;
    int realTimePriority = config::DEFAULT_REALTIME_PRIORITY;
    bool enableNumaAwareness = true;
    bool preferIsolatedCores = true;
    // Policy for "RT priority + AGGRESSIVE backoff on a non-isolated core".
    // preferIsolatedCores is a preference, not a guarantee: on a host without
    // isolated cores the consumer would otherwise spin forever at RT priority
    // on a shared core. See rt_spin_guard.h.
    performance::RtSpinPolicy rtSpinPolicy = performance::RtSpinPolicy::DOWNGRADE;

    AffinityConfig() = default;
    AffinityConfig(ComponentType t, int prio = config::DEFAULT_REALTIME_PRIORITY)
        : componentType(t), realTimePriority(prio) {}
  };
#endif

  // Named here too, because most callers reach it through a bus type.
  using WaitMode = ConsumerWaitMode;

  using ConsumerRunnerFn = void (*)(EventBus*, uint32_t, void*, bool, BackoffMode);
  // One step over a consumer, with the listener type erased: returns true if
  // it delivered anything. This is what lets something other than a dedicated
  // thread drive a consumer -- see pollConsumer().
  using ConsumerStepFn = bool (*)(EventBus*, uint32_t);
  // The drain takes the last sequence the run ever handed out, because after
  // a stop the ring may hold a gap: see drainSlot.
  using ConsumerDrainFn = void (*)(EventBus*, uint32_t, int64_t);

  struct ConsumerSlot
  {
    void* listener{nullptr};
    ConsumerRunnerFn runner{nullptr};
    ConsumerStepFn stepper{nullptr};
    ConsumerDrainFn drainer{nullptr};
    bool required{true};  // influence on gating
    WaitMode wait{WaitMode::ACTIVE};
    // Step state. Owned by whoever steps this consumer -- its own thread, or
    // an executor -- and never touched concurrently, exactly like the ring's
    // single-reader contract it implements. It lives here rather than in a
    // loop's stack frame so that the step can be a call.
    int64_t next{-1};                          // last handled seq, driver's private copy
    size_t maxRun{kMaxConsumeRun};             // run cap, resolved at start()
    bool dropBehind{false};                    // resolved at start()
    alignas(64) std::atomic<int64_t> seq{-1};  // last handled seq, published
    std::optional<jthread> thread{};
    uint32_t coreIndex{0};  // index for core distribution
    // Set when the listener threw: the slot is out of service, whoever drives
    // it stops driving it, and health reports it dead. Deliberately NOT "the
    // loop is running": a consumer stepped by a shared executor has no loop of
    // its own to be inside, and liveness there is progress, not presence.
    std::atomic<bool> failed{false};
    std::atomic<uint64_t> droppedBehind{0};  // events skipped by drop-behind
  };

  // ---------- consumer health ----------
  //
  // A stalled REQUIRED consumer stops the publisher at wrap gating; a stalled
  // OPTIONAL consumer stops it at the reclaim fence (use-after-free
  // protection scans every consumer). Either way the bus freezes silently.
  // The health layer makes the failure loud and gives optional consumers a
  // documented degradation: drop-behind.

  enum class ConsumerHealth : uint8_t
  {
    HEALTHY,
    STALLED,  // no progress for stallThreshold while work is pending
    DEAD      // the handler threw; the slot is out of service
  };

  enum class DeadConsumerPolicy : uint8_t
  {
    ALERT,    // callback + log only
    STOP_BUS  // additionally stop() the bus on a dead REQUIRED consumer
  };

  using HealthCallback = void (*)(uint32_t consumerIndex, ConsumerHealth state, void* user);

  struct HealthConfig
  {
    std::chrono::milliseconds stallThreshold{100};
    // Optional consumers may jump to the head instead of stalling the
    // reclaim fence; skipped events are counted, never silently lost.
    bool dropBehindOptional{false};
    size_t dropBehindSlack{CapacityPow2 / 2};
    DeadConsumerPolicy deadPolicy{DeadConsumerPolicy::ALERT};
    HealthCallback callback{nullptr};
    void* callbackUser{nullptr};
    bool enableMonitorThread{false};  // poll checkHealth() at stallThreshold/2
  };

  // Must be called before start().
  void setHealthConfig(const HealthConfig& cfg) { _healthCfg = cfg; }

  // How long the built-in monitor thread sleeps between sweeps: half the stall
  // threshold, and never nothing. The arithmetic is integer milliseconds, so
  // any threshold below 2 ms halves to zero -- and a zero sleep is not "poll
  // often", it is a thread holding a core to run checkHealth(), an
  // O(consumers) scan over atomics, with nothing in between. A millisecond
  // threshold is a reasonable setting for a bus where a millisecond of stall
  // matters; a spinning core is not what it should buy.
  static constexpr std::chrono::milliseconds monitorPeriod(
      std::chrono::milliseconds stallThreshold) noexcept
  {
    constexpr auto floor = std::chrono::milliseconds{1};
    const auto half = stallThreshold / 2;
    return half < floor ? floor : half;
  }

  struct HealthSweep
  {
    uint32_t stalled{0};
    uint32_t dead{0};
  };

  // Level-triggered query: the last observed health of one consumer, readable
  // at any time (including after stop()), unlike the edge-triggered callback
  // and unlike checkHealth() which returns an empty sweep once stopped. A
  // supervisor that starts after a consumer already stalled uses this to ask
  // "who is unhealthy right now".
  ConsumerHealth consumerHealth(uint32_t consumerIndex) const
  {
    if (consumerIndex >= MaxConsumers)
    {
      return ConsumerHealth::HEALTHY;
    }
    return _healthBook[consumerIndex].state.load(std::memory_order_acquire);
  }

  // What the last sweep saw for one consumer, as one update: the state, the
  // sequence it had reached, and the instant that sequence last moved.
  // lastChange moves only together with lastSeen, so the pair answers "is this
  // consumer progressing, and if not, since when" -- which the state alone
  // does not.
  struct ConsumerHealthReport
  {
    ConsumerHealth state{ConsumerHealth::HEALTHY};
    int64_t lastSeen{-1};
    std::chrono::steady_clock::time_point lastChange{};
  };

  ConsumerHealthReport consumerHealthReport(uint32_t consumerIndex) const
  {
    ConsumerHealthReport report;
    if (consumerIndex >= MaxConsumers)
    {
      return report;
    }
    const HealthBook& book = _healthBook[consumerIndex];
    for (;;)
    {
      const uint32_t before = book.version.load(std::memory_order_acquire);
      if ((before & 1u) != 0u)
      {
        continue;  // a sweep is mid-update; its next store releases us
      }
      report.state = book.state.load(std::memory_order_relaxed);
      report.lastSeen = book.lastSeen.load(std::memory_order_relaxed);
      const int64_t ticks = book.lastChangeTicks.load(std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_acquire);
      if (book.version.load(std::memory_order_relaxed) == before)
      {
        report.lastChange = HealthBook::timeOf(ticks);
        return report;
      }
    }
  }

  // Counts from the last recorded per-consumer states, without re-sweeping and
  // without the running-guard, so a stopped bus still reports its dead/stalled
  // consumers.
  HealthSweep healthSnapshot() const
  {
    HealthSweep sweep;
    const uint32_t n = _consumerCount.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < n; ++i)
    {
      const ConsumerHealth state = _healthBook[i].state.load(std::memory_order_acquire);
      if (state == ConsumerHealth::STALLED)
      {
        ++sweep.stalled;
      }
      else if (state == ConsumerHealth::DEAD)
      {
        ++sweep.dead;
      }
    }
    return sweep;
  }

  // One health sweep over all consumers. Fires the callback on state
  // transitions. Single-checker contract: call from one thread at a time
  // (the built-in monitor thread or your own health checker, not both).
  HealthSweep checkHealth()
  {
    HealthSweep sweep;
    if (!_running.load(std::memory_order_acquire))
    {
      return sweep;
    }

    const auto now = std::chrono::steady_clock::now();
    const int64_t head = _next.load(std::memory_order_acquire);
    const uint32_t n = _consumerCount.load(std::memory_order_acquire);

    for (uint32_t i = 0; i < n; ++i)
    {
      auto& book = _healthBook[i];
      // The checker is the only writer, so it reads its own last update back
      // relaxed and republishes the whole of it below.
      int64_t seen = book.lastSeen.load(std::memory_order_relaxed);
      int64_t changeTicks = book.lastChangeTicks.load(std::memory_order_relaxed);
      const ConsumerHealth previous = book.state.load(std::memory_order_relaxed);
      ConsumerHealth next = ConsumerHealth::HEALTHY;
      bool progressed = false;

      if (_consumers[i].failed.load(std::memory_order_acquire))
      {
        next = ConsumerHealth::DEAD;
      }
      else
      {
        const int64_t s = _consumers[i].seq.load(std::memory_order_acquire);
        if (s != seen)
        {
          seen = s;
          changeTicks = HealthBook::ticksOf(now);
          progressed = true;
        }
        else if (s < head &&
                 now - HealthBook::timeOf(changeTicks) >= _healthCfg.stallThreshold)
        {
          next = ConsumerHealth::STALLED;
        }
      }

      if (progressed || next != previous)
      {
        book.write(seen, changeTicks, next);
      }

      if (next != previous)
      {
        if (next == ConsumerHealth::DEAD)
        {
          LRVX_LOG_ERROR("EventBus consumer " << i << " is dead (handler threw)");
        }
        else if (next == ConsumerHealth::STALLED)
        {
          LRVX_LOG_WARN("EventBus consumer " << i << " stalled: seq="
                                             << seen << " head=" << head);
        }
        if (_healthCfg.callback)
        {
          _healthCfg.callback(i, next, _healthCfg.callbackUser);
        }
        if (next == ConsumerHealth::DEAD && _consumers[i].required &&
            _healthCfg.deadPolicy == DeadConsumerPolicy::STOP_BUS)
        {
          LRVX_LOG_ERROR("EventBus: required consumer " << i << " dead, stopping bus");
          // checkHealth() runs on the monitor thread, so joining that thread
          // here (as full stop() does) would join self -> terminate. Stop
          // everything EXCEPT the monitor thread; it exits its own loop when
          // it sees _running==false, and is joined later by stop()/dtor.
          doStop(/*joinMonitor=*/false);
          return sweep;
        }
      }

      if (next == ConsumerHealth::STALLED)
      {
        ++sweep.stalled;
      }
      if (next == ConsumerHealth::DEAD)
      {
        ++sweep.dead;
      }
    }
    return sweep;
  }

  enum class PublishResult
  {
    SUCCESS,
    TIMEOUT,
    STOPPED
  };

  struct Stats
  {
    uint64_t published{0};
    uint64_t dropped{0};
    uint64_t consumed{0};
    uint64_t droppedBehind{0};  // skipped by optional drop-behind consumers
  };

 public:
  EventBus()
#if LRVX_CPU_AFFINITY_ENABLED
      : _cpuAffinity(performance::createCpuAffinity())
#endif
  {
    // _constructed is value-initialized to 0, which is its empty state; only
    // _published has a non-zero one.
    for (auto& p : _published)
    {
      p.store(-1, std::memory_order_relaxed);
    }
  }

  ~EventBus() { stop(); }

  EventBus(const EventBus&) = delete;
  EventBus& operator=(const EventBus&) = delete;

  bool subscribe(Listener* listener, bool required = true, WaitMode wait = WaitMode::ACTIVE)
  {
    return subscribeImpl(listener, &runConsumer<Listener>, &stepConsumer<Listener>,
                         &drainConsumerSlot<Listener>, required, wait);
  }

  // Subscribe with a concrete type. The consumer loop is instantiated
  // around L, so dispatch is a direct call and the handler inlines into the
  // run over published events. L only needs the handler methods the Event's
  // dispatcher calls; deriving from Listener is not required.
  template <typename L>
  bool subscribeStatic(L* listener, bool required = true, WaitMode wait = WaitMode::ACTIVE)
  {
    return subscribeImpl(listener, &runConsumer<L>, &stepConsumer<L>, &drainConsumerSlot<L>,
                         required, wait);
  }

  // ---- driving consumers from outside ----
  //
  // By default every consumer gets a thread of its own, which is the right
  // shape for one bus on a machine it owns and the wrong one for a process
  // holding hundreds of them: the threads cost more than the work. Turn this
  // off and start() spawns nothing; whoever owns the process steps the
  // consumers itself -- one thread over many of them, across many buses --
  // and decides when to wait. Must be set before start().
  void setOwnConsumerThreads(bool own) { _ownConsumerThreads = own; }
  bool ownConsumerThreads() const noexcept { return _ownConsumerThreads; }

  // One step over consumer i: returns true if it delivered anything, false if
  // the ring held nothing for it. The caller that gets false everywhere is
  // the caller that should wait.
  //
  // Only for a bus with setOwnConsumerThreads(false): stepping a consumer
  // that also has its own thread is two readers on one cursor.
  bool pollConsumer(uint32_t i)
  {
    if (i >= _consumerCount.load(std::memory_order_acquire) ||
        _consumers[i].failed.load(std::memory_order_relaxed))
    {
      return false;
    }
    return _consumers[i].stepper(this, i);
  }

  // Everything still published for consumer i, uncapped. The externally
  // driven counterpart of drain-on-stop: call it before stop() if the
  // listener is owed what is left in the ring.
  void drainConsumer(uint32_t i)
  {
    if (i >= _consumerCount.load(std::memory_order_acquire) ||
        _consumers[i].failed.load(std::memory_order_relaxed))
    {
      return;
    }
    // A running bus has no resolved gaps: every sequence below the head is
    // either stamped or still being written by a publisher that owns it.
    _consumers[i].drainer(this, i, std::numeric_limits<int64_t>::min());
  }

  // Is there anything published for consumer i right now? Exactly the look
  // pollConsumer() starts with, without the delivery: this is what a thread
  // stepping many consumers asks about all of them before it goes to sleep.
  //
  // Reads the stepping driver's own cursor, so it is for the driver of this
  // consumer to call and nobody else -- the same single-reader contract as
  // pollConsumer().
  bool consumerHasPending(uint32_t i) const noexcept
  {
    if (i >= _consumerCount.load(std::memory_order_acquire))
    {
      return false;
    }
    const ConsumerSlot& slot = _consumers[i];
    if (slot.failed.load(std::memory_order_relaxed))
    {
      return false;  // out of service: it will never have work again
    }
    const int64_t want = slot.next + 1;
    const size_t idx = size_t(want) & Mask;
    return _published[idx].load(std::memory_order_acquire) == want;
  }

  // ---- one wait point over many buses ----
  //
  // WaitMode::PARKED gives a consumer thread somewhere to sleep, and that is
  // as far as it goes: the condition variable belongs to this bus. A thread
  // stepping consumers across many buses (see setOwnConsumerThreads) cannot
  // block on any one of them, so without this it has nowhere to sleep and
  // spins -- the exact cost parking exists to remove, moved up one level.
  //
  // Point every such bus at one WakeSet and the publisher wakes the set
  // instead. Must be set before start(); a bus with no set behaves exactly as
  // it did before there were any.
  void setWakeSet(WakeSet* set) noexcept
  {
    if (_running.load(std::memory_order_acquire))
    {
      return;  // same rule as subscribe(): the publish path is fixed at start
    }
    _wakeSet = set;
    _wakeOnPublish = _anyParked || (set != nullptr);
  }

  WakeSet* wakeSet() const noexcept { return _wakeSet; }

  // Test seam for the one race parking has to survive: a publish landing
  // between a consumer's last empty look at the ring and it raising its hand.
  // That window is a couple of hundred nanoseconds wide and sits inside
  // park(), so no test can aim at it from outside -- and an untestable
  // correctness claim is a claim nobody checks. The hook is called inside
  // park(), at exactly that instant, and a test publishes from it.
  //
  // Null by default: one predictable branch on the parked path, nothing at
  // all on the active one.
  void setParkProbe(void (*probe)(void*), void* user) noexcept
  {
    _parkProbe = probe;
    _parkProbeUser = user;
  }

  // How long a parked consumer sleeps when nothing wakes it. Only a test has
  // a reason to move it: pushing the net far out is how a test proves that a
  // wake-up came from the publisher and not from the schedule, without
  // putting a number on the scheduler. Set it before start().
  void setParkNetInterval(std::chrono::milliseconds interval) noexcept
  {
    _parkNetInterval = interval;
  }
  std::chrono::milliseconds parkNetInterval() const noexcept { return _parkNetInterval; }

  // The listener threw and the slot is out of service. A driver stepping many
  // consumers uses this to stop stepping this one; checkHealth() reports the
  // same thing as DEAD.
  bool consumerFailed(uint32_t i) const noexcept
  {
    return i < MaxConsumers && _consumers[i].failed.load(std::memory_order_acquire);
  }

  void start() override
  {
    if (_running.exchange(true, std::memory_order_acq_rel))
    {
      return;
    }

    // The run's claim accounting starts here: stop() counts this run's
    // resolutions, and the counters themselves are cumulative over the life of
    // the bus (stats().published is one of them). Nobody is publishing -- the
    // bus is stopped and the previous stop() waited its publishers out -- so
    // the baseline and the reopening below have the ring to themselves.
    // Release, and acquired by every reader of the baseline: a stop() that
    // read a previous run's baseline would count this run's claims as already
    // resolved and walk into the teardown early.
    _claimBase.store(_publishCount.load(std::memory_order_relaxed) +
                         _abandonedClaims.load(std::memory_order_relaxed),
                     std::memory_order_release);
    _sealed.store(false, std::memory_order_relaxed);
    _next.store(-1, std::memory_order_release);

    const uint32_t n = _consumerCount.load(std::memory_order_acquire);

    // Resolved once, here, because they depend on the health config, which is
    // fixed before start(): a consumer's run cap and whether it may skip.
    const auto startedAt = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < n; ++i)
    {
      // The stall clock starts now. Left at the epoch it reads as "no progress
      // since 1970", so the first sweep after the first publish calls a
      // consumer stalled before it has had any time at all -- which a consumer
      // thread hid by being faster than the first sweep, and a consumer
      // stepped from outside does not.
      _healthBook[i].reset(startedAt);
      auto& slot = _consumers[i];
      slot.next = -1;
      slot.failed.store(false, std::memory_order_relaxed);
      slot.dropBehind = !slot.required && _healthCfg.dropBehindOptional;
      // Drop-behind lag is only examined at the top of a step, so cap the run
      // for such consumers: with an uncapped run the consumer re-checks only
      // after consuming the whole backlog and the lag never looks large. The
      // required hot path keeps the full run.
      slot.maxRun = slot.dropBehind ? std::max<size_t>(1, _healthCfg.dropBehindSlack / 2)
                                    : kMaxConsumeRun;
    }

    // Somebody else may be stepping these consumers (see pollConsumer), in
    // which case there is nothing to spawn and nothing to wait for. The
    // health monitor below is orthogonal and still applies.
    _active.store(_ownConsumerThreads ? n : 0, std::memory_order_relaxed);

    for (uint32_t i = 0; _ownConsumerThreads && i < n; ++i)
    {
      auto* l = _consumers[i].listener;
      auto runner = _consumers[i].runner;
      auto required = _consumers[i].required;
      auto coreIdx = _consumers[i].coreIndex;
      auto backoffMode = _backoffMode;

      _consumers[i].thread.emplace([this, i, l, runner, required, coreIdx, backoffMode]
                                   {
         auto effectiveBackoff = backoffMode;
#if LRVX_CPU_AFFINITY_ENABLED
         auto threadCpuAffinity = performance::createCpuAffinity();
         if (_coreAssignment.has_value() && _affinityConfig.has_value())
         {
           auto& assignment = _coreAssignment.value();
           auto& config     = _affinityConfig.value();
           std::vector<int> targetCores;
           switch (config.componentType)
           {
             case ComponentType::MARKET_DATA: targetCores = assignment.marketDataCores; break;
             case ComponentType::EXECUTION:   targetCores = assignment.executionCores;  break;
             case ComponentType::STRATEGY:    targetCores = assignment.strategyCores;   break;
             case ComponentType::RISK:        targetCores = assignment.riskCores;       break;
             case ComponentType::GENERAL:     targetCores = assignment.generalCores;    break;
           }
           if (!targetCores.empty())
           {
             // Distribute consumers across available cores using round-robin
             const auto coreId = targetCores[coreIdx % targetCores.size()];
             const auto pinned = threadCpuAffinity->pinToCore(coreId);
             const bool isolated = pinned && assignment.hasIsolatedCores &&
                                   std::find(assignment.allIsolatedCores.begin(),
                                             assignment.allIsolatedCores.end(), coreId) != assignment.allIsolatedCores.end();
             const auto guard = performance::resolveRtSpinGuard(
                 config.enableRealTimePriority, isolated, effectiveBackoff, config.rtSpinPolicy);
             if (guard.guardTriggered)
             {
               effectiveBackoff = guard.backoffMode;
               LRVX_LOG_WARN("EventBus consumer " << i << ": RT priority with AGGRESSIVE backoff on non-isolated core "
                                                  << coreId << "; "
                                                  << (guard.applyRtPriority ? "backoff downgraded to ADAPTIVE"
                                                                            : "RT priority refused"));
             }
             if (guard.applyRtPriority && config.enableRealTimePriority)
             {
               auto pr = config.realTimePriority;
               if (isolated)
               {
                 pr += config::ISOLATED_CORE_PRIORITY_BOOST;
               }
               threadCpuAffinity->setRealTimePriority(pr);
             }
           }
         }
         else if (_coreAssignment.has_value())
         {
           auto& assignment = _coreAssignment.value();
           if (!assignment.marketDataCores.empty())
           {
             // Distribute across market data cores
             const auto coreId = assignment.marketDataCores[coreIdx % assignment.marketDataCores.size()];
             const auto pinned = threadCpuAffinity->pinToCore(coreId);
             const bool isolated = pinned && assignment.hasIsolatedCores &&
                                   std::find(assignment.allIsolatedCores.begin(),
                                             assignment.allIsolatedCores.end(), coreId) != assignment.allIsolatedCores.end();
             const auto guard = performance::resolveRtSpinGuard(true, isolated, effectiveBackoff);
             if (guard.guardTriggered)
             {
               effectiveBackoff = guard.backoffMode;
               LRVX_LOG_WARN("EventBus consumer " << i << ": fallback RT priority with AGGRESSIVE backoff on non-isolated core "
                                                  << coreId << "; backoff downgraded to ADAPTIVE");
             }
             if (guard.applyRtPriority)
             {
               threadCpuAffinity->setRealTimePriority(config::FALLBACK_REALTIME_PRIORITY);
             }
           }
         }
#endif
         {
           std::lock_guard<std::mutex> lk(_readyMutex);
           if (_active.fetch_sub(1, std::memory_order_acq_rel) == 1) _cv.notify_one();
         }
 
         runner(this, i, l, required, effectiveBackoff); });
    }

    std::unique_lock lk(_readyMutex);
    _cv.wait(lk, [&]
             { return _active.load(std::memory_order_acquire) == 0; });

    if (_healthCfg.enableMonitorThread)
    {
      _monitorThread.emplace([this]
                             {
        const auto period = monitorPeriod(_healthCfg.stallThreshold);
        while (_running.load(std::memory_order_acquire))
        {
          checkHealth();
          std::this_thread::sleep_for(period);
        } });
    }
  }

  void stop() override { doStop(/*joinMonitor=*/true); }

 private:
  // Where the sequence line goes when the bus stops. Deep enough in the
  // negatives that no run of refused claims can walk it back into the valid
  // range: claimBlocking and publishBatch put back what they took, and what is
  // left is a transient +1 per publisher in flight.
  static constexpr int64_t kSequenceLineClosed = std::numeric_limits<int64_t>::min() / 2;
  // Anything below this can only be a closed line: an open one starts at -1
  // and counts up, and a closed one sits at kSequenceLineClosed with at most a
  // handful of transient claims on top of it, each of which puts itself back.
  static constexpr int64_t kSequenceLineClosedFloor = std::numeric_limits<int64_t>::min() / 4;

  // A claim that will never be stamped. Every sequence handed out under a
  // run's sequence line is resolved exactly once -- published, or given up on
  // here -- because that is what lets stop() count publishers out of the ring
  // without the publish path counting itself in. See awaitPublishersQuiescent.
  void abandonClaims(size_t count) noexcept
  {
    _abandonedClaims.fetch_add(static_cast<uint64_t>(count), std::memory_order_release);
  }

  // The resolution a publisher owes for the sequence it holds, taken by the
  // destructor if it leaves through an exception from the event's constructor.
  // One predictable branch on the way out of publish(); the store it makes on
  // the normal path is the one the publish path already made.
  struct ClaimGuard
  {
    EventBus* bus;
    size_t count;

    ~ClaimGuard()
    {
      if (count != 0)
      {
        bus->abandonClaims(count);
      }
    }

    void resolvePublished() noexcept
    {
      bus->_publishCount.fetch_add(static_cast<uint64_t>(count), std::memory_order_release);
      count = 0;
    }
  };

  // Close the sequence line and record what the run handed out.
  //
  // A publisher reads _running once, on the way in, and nothing stops it from
  // being preempted between that read and its claim -- so a flag cannot keep a
  // late publisher out of a ring that is being torn down, however carefully it
  // is ordered. The claim itself can: one exchange puts the sequence line out
  // of reach, and from then on every fetch_add on it comes back negative and
  // the publisher walks away without having touched a slot. It is the
  // modification order of _next that splits publishers into the ones already
  // inside the ring and the ones that never will be, and the exchange returns
  // the boundary: every claim this run ever made is at or below it.
  void sealSequenceLine() noexcept
  {
    const int64_t lastClaim = _next.exchange(kSequenceLineClosed, std::memory_order_acq_rel);
    _runLastClaim.store(lastClaim, std::memory_order_relaxed);
    _sealed.store(true, std::memory_order_release);
  }

  uint64_t resolvedClaims() const noexcept
  {
    const uint64_t published = _publishCount.load(std::memory_order_acquire);
    const uint64_t abandoned = _abandonedClaims.load(std::memory_order_acquire);
    return published + abandoned - _claimBase.load(std::memory_order_acquire);
  }

  // Wait until nobody is inside the ring any more.
  //
  // The sequence line is sealed above, so the run's claims are exactly
  // lastClaim + 1 of them, and each is resolved once -- by the publish that
  // stamped it or by the publisher that gave up on it. When that many
  // resolutions have been counted, every publisher that ever held a sequence
  // has left the slots alone for good, and the acquire on the counters carries
  // its writes with it. Nothing new was added to the publish path to make this
  // work: the successful resolution is the counter publish() already bumped.
  //
  // This never waits on a publisher that is waiting on us. _running is already
  // false when it is called, and both gates re-read it on every turn, so a
  // publisher parked at the wrap gate or the reclaim fence gives up rather
  // than waiting for progress that stop() is no longer going to produce.
  void awaitPublishersQuiescent()
  {
    BusyBackoff bo;
    while (!_sealed.load(std::memory_order_acquire))
    {
      bo.pause();
    }
    const int64_t lastClaim = _runLastClaim.load(std::memory_order_acquire);
    const uint64_t claims =
        lastClaim >= 0 ? static_cast<uint64_t>(lastClaim) + 1 : uint64_t{0};
    BusyBackoff resolveBo;
    while (resolvedClaims() < claims)
    {
      resolveBo.pause();
    }
  }

  // joinMonitor=false is the path taken when checkHealth() (running ON the
  // monitor thread) trips DeadConsumerPolicy::STOP_BUS: resetting the monitor
  // jthread from within its own body would join self and terminate. The
  // monitor loop exits on its own once _running is false; its thread is joined
  // later by a subsequent stop() or by the destructor.
  void doStop(bool joinMonitor)
  {
    if (!_running.exchange(false, std::memory_order_acq_rel))
    {
      // Already stopping; still make sure the monitor thread is joined if a
      // real stop()/dtor asks for it (the STOP_BUS path left it dangling).
      if (joinMonitor)
      {
        _monitorThread.reset();
      }
      return;
    }

    // Before anything is joined: a consumer draining on its way out waits for
    // the same seal, and the sooner it is in place the less either of them
    // spins.
    sealSequenceLine();

    if (joinMonitor)
    {
      _monitorThread.reset();
    }

    // _running is already false, so a parked consumer is waiting for an event
    // that will never come. Joining it below would wait out its net instead.
    if (_anyParked)
    {
      {
        std::lock_guard<std::mutex> lk(_parkMx);
      }
      _parkCv.notify_all();
    }
    // A driver parked on a shared set is in the same position, except that
    // its other buses may still be running: the set is woken, it looks again,
    // and this bus simply has nothing more for it.
    if (_wakeSet != nullptr)
    {
      _wakeSet->wake();
    }

    // Everything below rewrites the ring, and a publisher that claimed its
    // sequence before the seal may still be constructing into a slot the
    // teardown is about to destroy.
    awaitPublishersQuiescent();

    const uint32_t n = _consumerCount.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < n; ++i)
    {
      _consumers[i].thread.reset();
    }

    // Externally driven bus: the drain a consumer thread would have taken on
    // its way out has nobody to take it, so stop() takes it here. Sound only
    // because the driver is required to have stopped stepping by now -- the
    // same requirement as a thread being joined above.
    if (!_ownConsumerThreads && _drainOnStop)
    {
      for (uint32_t i = 0; i < n; ++i)
      {
        if (!_consumers[i].failed.load(std::memory_order_relaxed))
        {
          _consumers[i].drainer(this, i, _runLastClaim.load(std::memory_order_relaxed));
        }
      }
    }

    // Only the slots this run claimed can hold an event or a published
    // sequence. Every claim is at or below the sealed line (see
    // sealSequenceLine), a slot is written only by the publish that owns its
    // sequence, and a run starts on a ring this loop left empty: so a run
    // that never wrapped touched slots [0, lastClaim] and nothing else, and
    // the rest of a large ring is not walked for a bus that carried a few
    // events.
    const int64_t lastClaim = _runLastClaim.load(std::memory_order_relaxed);
    const size_t touched =
        lastClaim < 0 ? size_t{0}
                      : (static_cast<uint64_t>(lastClaim) + 1 < CapacityPow2
                             ? static_cast<size_t>(lastClaim) + 1
                             : CapacityPow2);
    for (size_t i = 0; i < touched; ++i)
    {
      if (_constructed[i].exchange(0, std::memory_order_acq_rel))
      {
        slot_ptr(i)->~Event();
      }
      _published[i].store(-1, std::memory_order_relaxed);
    }
    _cachedMinConsumed.store(-1, std::memory_order_relaxed);

    // The gating lines go back to where a fresh bus starts. A consumer thread
    // always resumes from sequence 0; leaving them at the previous run's
    // positions describes a ring that no longer exists, and start() after
    // stop() -- the other half of the ISubsystem contract, taken on every
    // reconnect -- would then accept publishes, hand back valid sequence
    // numbers and deliver nothing at all. The sequence line itself is NOT
    // reopened here: it stays closed until start() reopens it, because a
    // publisher preempted between reading _running and taking its claim is
    // still out there and the closed line is the only thing that turns it
    // away. Consumer threads are joined above and the publishers have been
    // waited out, so this races nobody.
    _cachedMin.store(-1, std::memory_order_relaxed);
    for (uint32_t i = 0; i < n; ++i)
    {
      _consumers[i].seq.store(-1, std::memory_order_relaxed);
      _consumers[i].next = -1;
      _gating[i].v.store(_consumers[i].required ? -1 : INT64_MAX, std::memory_order_relaxed);
    }
  }

 public:
  int64_t publish(const Event& ev) { return do_publish(ev, std::nullopt).second; }
  int64_t publish(Event&& ev) { return do_publish(std::move(ev), std::nullopt).second; }

  // Publish a contiguous batch: one sequence reservation, one wrap/reclaim
  // wait for the whole range, then a single release fence covering all slot
  // stamps instead of one release store per event. Blocking, like publish().
  // Returns the last sequence, or -1 if the bus is stopped or the batch is
  // outside the bound below.
  int64_t publishBatch(const Event* evs, size_t count)
  {
    LRVX_PROFILE_SCOPE("Disruptor::publishBatch");

    static_assert(CapacityPow2 >= 2);

    // The bound is a refusal, not an assertion. An assert is nothing at all
    // under NDEBUG -- which every build type in this tree carries, Release and
    // RelWithDebInfo alike -- and past the bound the call does not merely
    // publish more than it promised: a range wider than the ring reserves
    // slots it wraps back onto, so the wrap gate waits on sequences that are
    // inside this very batch and that only this publisher can stamp. That is a
    // publisher which never returns. Half the ring is the documented bound
    // because the other half is what the consumers are still reading.
    if (count == 0 || count > CapacityPow2 / 2)
    {
      return -1;
    }

    if (!_running.load(std::memory_order_acquire))
    {
      return -1;
    }

    PublishSeam::beforeClaim();

    const int64_t lastSeq = _next.fetch_add(static_cast<int64_t>(count),
                                            std::memory_order_acq_rel) +
                            static_cast<int64_t>(count);
    const int64_t firstSeq = lastSeq - static_cast<int64_t>(count) + 1;
    if (firstSeq < 0)
    {
      // The sequence line is closed (or, never seen, overflowed): no claim was
      // made, so put back what the reservation took and leave without touching
      // a slot.
      _next.fetch_sub(static_cast<int64_t>(count), std::memory_order_acq_rel);
      return -1;
    }

    // The range is ours from here, and stop() is counting it.
    ClaimGuard claim{this, count};

    // Wrap gating for the whole range (required consumers).
    const int64_t wrap = lastSeq - static_cast<int64_t>(CapacityPow2);
    {
      BusyBackoff bo;
      int64_t cachedMin = _cachedMin.load(std::memory_order_acquire);
      while (wrap > cachedMin)
      {
        if (!_running.load(std::memory_order_relaxed))
        {
          return -1;
        }
        cachedMin = minGating();
        _cachedMin.store(cachedMin, std::memory_order_release);
        if (wrap <= cachedMin)
        {
          break;
        }
        bo.pause();
      }
    }

    // Reclaim gating for the whole range (all consumers), through the
    // monotonic cache.
    if (wrap >= 0 && _cachedMinConsumed.load(std::memory_order_acquire) < wrap)
    {
      BusyBackoff reclaimBo;
      int64_t observed;
      while ((observed = minConsumed()) < wrap)
      {
        if (!_running.load(std::memory_order_relaxed))
        {
          return -1;
        }
        reclaimBo.pause();
      }
      _cachedMinConsumed.store(observed, std::memory_order_release);
    }

    for (size_t k = 0; k < count; ++k)
    {
      const int64_t seq = firstSeq + static_cast<int64_t>(k);
      const size_t idx = size_t(seq) & Mask;
      if (_constructed[idx].exchange(0, std::memory_order_acq_rel))
      {
        slot_ptr(idx)->~Event();
      }
      ::new (slot_ptr(idx)) Event(evs[k]);
      auto& obj = slot_ref(idx);
      if constexpr (requires { obj->tickSequence; })
      {
        obj->tickSequence = static_cast<uint64_t>(seq);
      }
      if constexpr (requires { obj.tickSequence; })
      {
        obj.tickSequence = static_cast<uint64_t>(seq);
      }
    }

    // ThreadSanitizer cannot see thread fences, so under TSan every stamp is
    // its own release store. Same correctness, and TSan gets a visible
    // happens-before edge. The dead branch costs nothing.
    if constexpr (LRVX_TSAN_ENABLED != 0)
    {
      for (size_t k = 0; k < count; ++k)
      {
        const int64_t seq = firstSeq + static_cast<int64_t>(k);
        const size_t idx = size_t(seq) & Mask;
        _constructed[idx].store(1, std::memory_order_release);
        _published[idx].store(seq, std::memory_order_release);
      }
    }
    else
    {
      // The _constructed flags must be stored BEFORE the release fence: the
      // fence only orders writes sequenced before it against the relaxed
      // _published stamps that consumers acquire. With constructed stamped
      // after the fence, a consumer on a weakly-ordered CPU can observe
      // _published == seq while _constructed still reads 0, classify the
      // slot as reclaimed and silently skip the event.
      for (size_t k = 0; k < count; ++k)
      {
        const int64_t seq = firstSeq + static_cast<int64_t>(k);
        const size_t idx = size_t(seq) & Mask;
        _constructed[idx].store(1, std::memory_order_relaxed);
      }
      std::atomic_thread_fence(std::memory_order_release);
      for (size_t k = 0; k < count; ++k)
      {
        const int64_t seq = firstSeq + static_cast<int64_t>(k);
        const size_t idx = size_t(seq) & Mask;
        _published[idx].store(seq, std::memory_order_relaxed);
      }
    }

    if (_wakeOnPublish)
    {
      wakeWaiters();
    }
    // Last, so that a stop() waiting the ring out also waits out the wake-up
    // this publisher is making through the bus's own condition variable.
    claim.resolvePublished();
    return lastSeq;
  }

  // Publish with timeout - returns result and sequence number (-1 on failure).
  // Default 1ms: zero-timeout is an anti-pattern on multi-core due to cache coherency latency.
  std::pair<PublishResult, int64_t> tryPublish(const Event& ev,
                                               std::chrono::microseconds timeout = std::chrono::microseconds{1000})
  {
    return do_publish(ev, timeout);
  }
  std::pair<PublishResult, int64_t> tryPublish(Event&& ev,
                                               std::chrono::microseconds timeout = std::chrono::microseconds{1000})
  {
    return do_publish(std::move(ev), timeout);
  }

#ifdef LRVX_UNIT_TEST
  // Test-only, and a member for the same reason the seam above exists: from
  // outside, a publish refused by the closed sequence line and one refused by
  // the _running flag look exactly alike, so a test that means to hold the
  // line accountable has to be able to see the line. True once stop() has
  // closed it and until start() reopens it.
  bool sequenceLineClosed() const noexcept
  {
    return _next.load(std::memory_order_acquire) < kSequenceLineClosedFloor;
  }
#endif

  Stats stats() const
  {
    uint64_t droppedBehind = 0;
    const uint32_t n = _consumerCount.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < n; ++i)
    {
      droppedBehind += _consumers[i].droppedBehind.load(std::memory_order_relaxed);
    }
    return Stats{
        _publishCount.load(std::memory_order_relaxed),
        _dropCount.load(std::memory_order_relaxed),
        _consumeCount.load(std::memory_order_relaxed),
        droppedBehind};
  }

  void waitConsumed(int64_t seq)
  {
    LRVX_PROFILE_SCOPE("Disruptor::waitConsumed");
    BusyBackoff bo;
    while (_running.load(std::memory_order_acquire) && minGating() < seq)
    {
      bo.pause();
    }
  }

  void flush()
  {
    const int64_t last = _next.load(std::memory_order_acquire);
    waitConsumed(last);
  }

  uint32_t consumerCount() const { return _consumerCount.load(std::memory_order_acquire); }
  void enableDrainOnStop() { _drainOnStop = true; }

  void setBackoffMode(BackoffMode mode) { _backoffMode = mode; }

#if LRVX_CPU_AFFINITY_ENABLED
  // ---------- CPU Affinity / RT priority ----------
  void setAffinityConfig(const AffinityConfig& cfg)
  {
    _affinityConfig = cfg;

    performance::CriticalComponentConfig coreCfg;
    coreCfg.preferIsolatedCores = cfg.preferIsolatedCores;
    coreCfg.exclusiveIsolatedCores = true;
    coreCfg.allowSharedCriticalCores = false;

    if (cfg.enableNumaAwareness)
    {
      _coreAssignment = _cpuAffinity->getNumaAwareCoreAssignment(coreCfg);
    }
    else
    {
      _coreAssignment = _cpuAffinity->getRecommendedCoreAssignment(coreCfg);
    }
  }

  void setCoreAssignment(const performance::CoreAssignment& assignment)
  {
    _coreAssignment = assignment;
    _affinityConfig = AffinityConfig{ComponentType::GENERAL, config::DEFAULT_REALTIME_PRIORITY};
  }

  std::optional<performance::CoreAssignment> getCoreAssignment() const { return _coreAssignment; }
  std::optional<AffinityConfig> getAffinityConfig() const { return _affinityConfig; }

  bool setupOptimalConfiguration(ComponentType componentType, bool enablePerformanceOptimizations = false)
  {
    AffinityConfig cfg;
    cfg.componentType = componentType;
    cfg.enableRealTimePriority = (componentType != ComponentType::GENERAL);
    cfg.enableNumaAwareness = true;
    cfg.preferIsolatedCores = true;

    switch (componentType)
    {
      case ComponentType::MARKET_DATA:
        cfg.realTimePriority = config::MARKET_DATA_PRIORITY;
        break;
      case ComponentType::EXECUTION:
        cfg.realTimePriority = config::EXECUTION_PRIORITY;
        break;
      case ComponentType::STRATEGY:
        cfg.realTimePriority = config::STRATEGY_PRIORITY;
        break;
      case ComponentType::RISK:
        cfg.realTimePriority = config::RISK_PRIORITY;
        break;
      case ComponentType::GENERAL:
        cfg.realTimePriority = config::GENERAL_PRIORITY;
        break;
    }
    setAffinityConfig(cfg);

    if (enablePerformanceOptimizations)
    {
      _cpuAffinity->disableCpuFrequencyScaling();
    }
    return _coreAssignment.has_value();
  }

  bool verifyIsolatedCoreConfiguration() const
  {
    if (!_coreAssignment.has_value())
    {
      return false;
    }
    return _cpuAffinity->verifyCriticalCoreIsolation(_coreAssignment.value());
  }
#endif

 private:
  bool subscribeImpl(void* listener, ConsumerRunnerFn runner, ConsumerStepFn stepper,
                     ConsumerDrainFn drainer, bool required, WaitMode wait)
  {
    if (!listener)
    {
      return false;
    }
    if (_running.load(std::memory_order_acquire))
    {
      return false;  // Cannot subscribe after start
    }
    const uint32_t idx = _consumerCount.fetch_add(1, std::memory_order_acq_rel);
    if (idx >= MaxConsumers)
    {
      _consumerCount.fetch_sub(1, std::memory_order_acq_rel);
      return false;
    }
    _consumers[idx].listener = listener;
    _consumers[idx].runner = runner;
    _consumers[idx].stepper = stepper;
    _consumers[idx].drainer = drainer;
    _consumers[idx].required = required;
    _consumers[idx].wait = wait;
    if (wait == WaitMode::PARKED)
    {
      // Read on the publish path. A plain bool set before start(), so a bus
      // with no parked consumer pays one predictable load and nothing else --
      // the parking machinery must not slow down the buses that do not use
      // it.
      _anyParked = true;
      _wakeOnPublish = true;
    }
    _consumers[idx].next = -1;
    _consumers[idx].failed.store(false, std::memory_order_relaxed);
    _consumers[idx].seq.store(-1, std::memory_order_relaxed);
    _consumers[idx].coreIndex = idx;  // Store index for core distribution
    _gating[idx].v.store(required ? -1 : INT64_MAX, std::memory_order_relaxed);
    return true;
  }

  template <typename L>
  static void runConsumer(EventBus* self, uint32_t i, void* obj, bool required, BackoffMode mode)
  {
    self->consumerLoop(i, static_cast<L*>(obj), required, mode);
  }

  // The listener type is known only at subscribe(); these carry it to a
  // caller that has nothing but an index.
  template <typename L>
  static bool stepConsumer(EventBus* self, uint32_t i)
  {
    return self->pollOnce<L>(i, static_cast<L*>(self->_consumers[i].listener));
  }

  template <typename L>
  static void drainConsumerSlot(EventBus* self, uint32_t i, int64_t resolvedThrough)
  {
    self->drainSlot<L>(i, static_cast<L*>(self->_consumers[i].listener), resolvedThrough);
  }

  // The whole consume loop, monomorphic in the subscriber type. For
  // L = Listener this is exactly the historical virtual path; for a concrete
  // L (via subscribeStatic) dispatch resolves statically and the handler
  // inlines into the batched run.
  // ---- the step ----
  //
  // One step over one consumer: everything published and contiguous right
  // now, up to the run cap, handed to the listener, progress published once
  // at the end. Returns false when the ring holds nothing for this consumer.
  //
  // The wait is deliberately NOT in here. That is the whole point of the
  // split: a thread that owns one consumer waits however it likes, while a
  // thread stepping many of them may only wait after every one of them came
  // back empty. Before the split there was no way to express the second.
  //
  // Single-reader, like the ring it reads: one consumer is stepped by one
  // thread at a time. Two threads stepping the same index is the same bug as
  // two consumers sharing a gating slot, and neither is detected here.
  template <typename L>
  bool pollOnce(uint32_t i, L* l)
  {
    ConsumerSlot& slot = _consumers[i];
    const bool required = slot.required;
    int64_t next = slot.next;

    // Optional consumers may fall arbitrarily far behind; without
    // drop-behind they stall the publisher at the reclaim fence. Jump to
    // the head, publish the skipped range as consumed (the events are
    // never delivered here -- reclaim only needs to know nobody will read
    // them) and account every skipped event.
    if (slot.dropBehind)
    {
      const int64_t head = _next.load(std::memory_order_acquire);
      if (head - next > static_cast<int64_t>(_healthCfg.dropBehindSlack))
      {
        const int64_t target = head - 1;
        slot.droppedBehind.fetch_add(static_cast<uint64_t>(target - next),
                                     std::memory_order_relaxed);
        next = target;
        slot.next = next;
        slot.seq.store(next, std::memory_order_release);
      }
    }

    // Batch effect: consume the whole contiguous published run and publish
    // progress once at its end, instead of two release stores per event. The
    // run is bounded so producers waiting on the wrap never starve for
    // progress longer than maxRun events.
    int64_t last = next;
    int64_t cur = next + 1;
    uint64_t delivered = 0;
    size_t run = 0;
    while (run < slot.maxRun)
    {
      const size_t cidx = size_t(cur) & Mask;
      if (_published[cidx].load(std::memory_order_acquire) != cur)
      {
        break;
      }
      // Value 1 = valid event, value 0 = reclaimed. Only a constructed
      // slot is dispatched: anything else would read stale memory from a
      // previous wrap-around. Relaxed is enough: the construction store is
      // ordered before the slot stamp acquired above.
      if (_constructed[cidx].load(std::memory_order_relaxed) == 1)
      {
        LRVX_PROFILE_SCOPE("Disruptor::deliver");
        try
        {
          EventDispatcher<Event>::dispatch(slot_ref(cidx), *l);
        }
        catch (...)
        {
          // Publish progress up to the previous event and take the slot out
          // of service. Swallowing would keep a broken handler in the loop;
          // rethrowing would terminate the process. And the failure stays on
          // the slot rather than on whoever is stepping it, because under a
          // shared executor that is every other consumer on the same thread.
          // A dead REQUIRED consumer stalls gating by design -- checkHealth()
          // surfaces it and applies the configured policy.
          slot.next = last;
          slot.seq.store(last, std::memory_order_release);
          if (required)
          {
            _gating[i].v.store(last, std::memory_order_release);
          }
          slot.failed.store(true, std::memory_order_release);
          if (delivered != 0)
          {
            _consumeCount.fetch_add(delivered, std::memory_order_relaxed);
          }
          LRVX_LOG_ERROR("EventBus consumer " << i << ": handler threw, consumer is dead");
          return delivered != 0;
        }
        ++delivered;
      }
      last = cur;
      ++cur;
      ++run;
    }

    if (last == next)
    {
      return false;  // nothing for this consumer: the one case worth waiting on
    }

    // End of everything that was available: the consumer has drained the
    // ring for now. A listener that batches work -- amortising an fsync, a
    // syscall, a flush -- needs exactly this edge, because it is the moment
    // where waiting longer buys nothing. Opt-in: a dispatcher without the
    // hook compiles to nothing.
    if (delivered != 0)
    {
      if constexpr (requires { EventDispatcher<Event>::endOfBatch(*l); })
      {
        EventDispatcher<Event>::endOfBatch(*l);
      }
    }
    slot.seq.store(last, std::memory_order_release);
    if (required)
    {
      _gating[i].v.store(last, std::memory_order_release);
    }
    if (delivered != 0)
    {
      _consumeCount.fetch_add(delivered, std::memory_order_relaxed);
    }
    slot.next = last;
    return true;
  }

  // Block until somebody publishes, the bus stops, or the safety net expires.
  //
  // The race this has to survive is the one every parked consumer has: the
  // ring looks empty, and a publish lands in the instant between looking and
  // sleeping. Two things close it. The waiter count is incremented BEFORE the
  // last look, with a sequentially consistent fence on either side (here and
  // on the publish path), so a publisher that misses the count cannot also be
  // missed by the look. And the mutex is held across the look and the wait,
  // so a publisher that sees the count cannot slip its notify in between.
  //
  // The timed wait is a net under both of those, not the mechanism: a missed
  // wake-up costs one net interval instead of forever. Nothing is expected to
  // rely on it, and a test asserts exactly that by failing if wake-ups start
  // arriving on the net's schedule.
  void park(uint32_t i)
  {
    if (_parkProbe != nullptr)
    {
      // Before the mutex and before the hand goes up: the exact instant a
      // publisher can miss this consumer. See setParkProbe().
      _parkProbe(_parkProbeUser);
    }
    std::unique_lock<std::mutex> lk(_parkMx);
    _parkWaiters.fetch_add(1, std::memory_order_seq_cst);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    const int64_t want = _consumers[i].next + 1;
    const size_t idx = size_t(want) & Mask;
    if (_published[idx].load(std::memory_order_acquire) != want &&
        _running.load(std::memory_order_acquire))
    {
      _parkCv.wait_for(lk, _parkNetInterval);
    }
    _parkWaiters.fetch_sub(1, std::memory_order_relaxed);
  }

  // Everything a publish has to wake, in the order it was added. Reached
  // through a single flag, so the buses that wake nobody stay untouched.
  void wakeWaiters()
  {
    if (_anyParked)
    {
      wakeParked();
    }
    if (_wakeSet != nullptr)
    {
      _wakeSet->wake();
    }
  }

  // Wake whoever is parked. Called after a publish and on the way down.
  void wakeParked()
  {
    // The fence pairs with the one in park(): between them, a publisher that
    // reads no waiters is guaranteed to have made its event visible to a
    // consumer that is about to take its last look.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (_parkWaiters.load(std::memory_order_seq_cst) == 0)
    {
      return;
    }
    {
      std::lock_guard<std::mutex> lk(_parkMx);
    }
    _parkCv.notify_all();
  }

  // Everything still published for this consumer, no run cap, no waiting.
  // Taken on the way down when the bus was told to drain on stop: the events
  // are already in the ring and the producer is gone, so the only question is
  // whether the listener gets them.
  // resolvedThrough: every sequence at or below it has been resolved -- either
  // stamped into the ring or given up on -- so an unstamped one is a gap the
  // drain steps over rather than the head of the ring.
  template <typename L>
  void drainSlot(uint32_t i, L* l, int64_t resolvedThrough)
  {
    ConsumerSlot& slot = _consumers[i];
    const bool required = slot.required;
    int64_t seq = slot.seq.load(std::memory_order_relaxed);
    uint64_t delivered = 0;
    // The drain owes the listener the same batch contract as the step above.
    // A listener that batches -- amortising an fsync, a durability barrier,
    // a flush -- commits what it was handed only on this edge, so a drain
    // that dispatches without it applies the events and never lets them out.
    const auto endBatch = [&]
    {
      if (delivered == 0)
      {
        return;
      }
      delivered = 0;
      if constexpr (requires { EventDispatcher<Event>::endOfBatch(*l); })
      {
        EventDispatcher<Event>::endOfBatch(*l);
      }
    };
    for (;;)
    {
      const int64_t want = seq + 1;
      const size_t idx = size_t(want) & Mask;
      if (_published[idx].load(std::memory_order_acquire) != want)
      {
        if (want > resolvedThrough)
        {
          break;  // the ring ends here
        }
        // A sequence claimed and then given up on when the bus stopped: it
        // will never be stamped. Stopping at it would strand every event
        // another publisher had already put behind it, and those were accepted
        // publishes. The slot holds an older event this consumer has already
        // been given -- it could not have reached `want` otherwise -- so there
        // is nothing here to deliver, only a number to step over.
        slot.seq.store(want, std::memory_order_release);
        _gating[i].v.store(required ? want : INT64_MAX, std::memory_order_release);
        seq = want;
        continue;
      }
      if (_constructed[idx].load(std::memory_order_acquire) == 1)
      {
        LRVX_PROFILE_SCOPE("Disruptor::drain_deliver");
        try
        {
          EventDispatcher<Event>::dispatch(slot_ref(idx), *l);
        }
        catch (...)
        {
          // Same rule as the step, and it matters more here: the drain can
          // run on the thread that called stop(), and an exception escaping
          // into a shutdown path takes the process with it.
          slot.failed.store(true, std::memory_order_release);
          LRVX_LOG_ERROR("EventBus consumer " << i << ": handler threw during drain");
          break;
        }
        _consumeCount.fetch_add(1, std::memory_order_relaxed);
        ++delivered;
      }
      slot.seq.store(want, std::memory_order_release);
      _gating[i].v.store(required ? want : INT64_MAX, std::memory_order_release);
      seq = want;
      // Bounded like the run above, so a long drain does not leave a
      // batching listener holding an unbounded backlog.
      if (delivered == kMaxConsumeRun)
      {
        endBatch();
      }
    }
    endBatch();
    slot.next = seq;
  }

  // The whole consume loop, monomorphic in the subscriber type. For
  // L = Listener this is exactly the historical virtual path; for a concrete
  // L (via subscribeStatic) dispatch resolves statically and the handler
  // inlines into the batched run.
  //
  // What is left of it after the split is the part that was never about
  // consuming: deciding when to wait.
  template <typename L>
  void consumerLoop(uint32_t i, L* l, bool, BackoffMode backoffMode)
  {
    BusyBackoff backoff(backoffMode);
    const bool parked = _consumers[i].wait == WaitMode::PARKED;
    while (_running.load(std::memory_order_acquire))
    {
      const bool worked = pollOnce<L>(i, l);
      if (_consumers[i].failed.load(std::memory_order_relaxed))
      {
        return;  // out of service: waiting for it to recover is waiting forever
      }
      if (worked)
      {
        backoff.reset();
      }
      else if (parked)
      {
        park(i);
      }
      else
      {
        backoff.pause();
      }
    }
    if (_drainOnStop)
    {
      // Everything published before the bus stopped is owed to this listener,
      // and a publisher that was inside the ring when stop() was called is
      // still writing some of it. Waiting for the ring to go quiet before the
      // drain is what makes "publish() accepted it" mean "the listener got
      // it": the drain would otherwise stop at the first slot that publisher
      // had not stamped yet and call the rest of the run absent.
      awaitPublishersQuiescent();
      drainSlot<L>(i, l, _runLastClaim.load(std::memory_order_acquire));
    }
  }

  // Blocking claim. Reserve first -- one fetch_add, the hot path -- then wait
  // out both gates. A blocking publish never gives up, so the reservation is
  // always honoured and no sequence is ever left unstamped except when the bus
  // stops underneath, which throws the whole ring away anyway.
  bool claimBlocking(int64_t& claimed)
  {
    const int64_t seq = _next.fetch_add(1, std::memory_order_acq_rel) + 1;

    // Check for overflow (very unlikely but safe)
    if (seq < 0)
    {
      _next.fetch_sub(1, std::memory_order_acq_rel);
      return false;
    }

    const int64_t wrap = seq - static_cast<int64_t>(CapacityPow2);

    BusyBackoff bo;
    int64_t cachedMin = _cachedMin.load(std::memory_order_acquire);
    while (wrap > cachedMin)
    {
      if (!_running.load(std::memory_order_relaxed))
      {
        // The gate will not open again: nothing is going to consume past this
        // point. The sequence is given up on rather than written, because the
        // slot it names still holds an event a consumer has not read -- that
        // is why the gate was closed -- and stop() is what destroys it.
        abandonClaims(1);
        return false;
      }
      cachedMin = minGating();
      _cachedMin.store(cachedMin, std::memory_order_release);
      if (wrap <= cachedMin)
      {
        break;
      }
      bo.pause();
    }

    // Wait for ALL consumers (including optional) to process the old event
    // before destroying it. This prevents use-after-free for optional
    // consumers. minConsumed() is a scan over every consumer's progress line,
    // so consult the monotonic cache first: any previously observed lower
    // bound stays valid forever, and the scan runs only when it is not enough.
    if (wrap >= 0 && _cachedMinConsumed.load(std::memory_order_acquire) < wrap)
    {
      BusyBackoff reclaimBo;
      int64_t observed;
      while ((observed = minConsumed()) < wrap)
      {
        if (!_running.load(std::memory_order_relaxed))
        {
          abandonClaims(1);
          return false;
        }
        reclaimBo.pause();
      }
      _cachedMinConsumed.store(observed, std::memory_order_release);
    }

    claimed = seq;
    return true;
  }

  // Bounded claim. A publish that is allowed to give up must give up with no
  // trace: a sequence handed out and then abandoned would have to be stamped
  // into the ring for consumers to get past it, and that stamp lands on the
  // slot of an event the slowest consumer has not read yet -- it reads a
  // sequence it can never match and spins there for good. So the sequence is
  // taken LAST, with a compare-exchange, and only once both gates already
  // admit it. Losing that race to another producer costs one re-evaluation,
  // never a stamped slot.
  //
  // Consumer progress only moves forward, so a gate that admitted the
  // candidate sequence before the exchange still admits it after.
  PublishResult claimBounded(std::chrono::microseconds timeout, int64_t& claimed)
  {
    const auto startTime = std::chrono::steady_clock::now();
    const auto expired = [&]
    { return std::chrono::steady_clock::now() - startTime >= timeout; };

    BusyBackoff bo;
    for (;;)
    {
      if (!_running.load(std::memory_order_relaxed))
      {
        return PublishResult::STOPPED;
      }

      const int64_t cur = _next.load(std::memory_order_acquire);
      const int64_t seq = cur + 1;
      if (seq < 0)
      {
        return PublishResult::STOPPED;
      }

      const int64_t wrap = seq - static_cast<int64_t>(CapacityPow2);

      if (wrap > _cachedMin.load(std::memory_order_acquire))
      {
        const int64_t cachedMin = minGating();
        _cachedMin.store(cachedMin, std::memory_order_release);
        if (wrap > cachedMin)
        {
          if (expired())
          {
            return PublishResult::TIMEOUT;
          }
          bo.pause();
          continue;
        }
      }

      if (wrap >= 0 && _cachedMinConsumed.load(std::memory_order_acquire) < wrap)
      {
        const int64_t observed = minConsumed();
        if (observed < wrap)
        {
          if (expired())
          {
            return PublishResult::TIMEOUT;
          }
          bo.pause();
          continue;
        }
        _cachedMinConsumed.store(observed, std::memory_order_release);
      }

      int64_t expectedNext = cur;
      if (_next.compare_exchange_weak(expectedNext, seq, std::memory_order_acq_rel,
                                      std::memory_order_relaxed))
      {
        claimed = seq;
        return PublishResult::SUCCESS;
      }

      if (expired())
      {
        return PublishResult::TIMEOUT;
      }
    }
  }

  template <typename Ev>
  std::pair<PublishResult, int64_t> do_publish(Ev&& ev, std::optional<std::chrono::microseconds> timeout)
  {
    LRVX_PROFILE_SCOPE("Disruptor::publish");

    if (!_running.load(std::memory_order_acquire))
    {
      return {PublishResult::STOPPED, -1};
    }

    // The answer above is now as old as this call is slow, and everything that
    // keeps a late publisher out of a torn-down ring happens below it.
    PublishSeam::beforeClaim();

    int64_t seq = -1;
    if (timeout.has_value())
    {
      const PublishResult claim = claimBounded(timeout.value(), seq);
      if (claim != PublishResult::SUCCESS)
      {
        if (claim == PublishResult::TIMEOUT)
        {
          _dropCount.fetch_add(1, std::memory_order_relaxed);
        }
        return {claim, -1};
      }
    }
    else if (!claimBlocking(seq))
    {
      return {PublishResult::STOPPED, -1};
    }

    const size_t idx = size_t(seq) & Mask;

    // The sequence is ours from here: it is at or below the boundary a stop()
    // seals, so stop() waits for the resolution this guard owes before it
    // destroys a single slot.
    ClaimGuard claim{this, 1};

    // Destroy old event if present - only if not already reclaimed
    // The _constructed flag ensures only one thread destroys
    if (_constructed[idx].exchange(0, std::memory_order_acq_rel))
    {
      slot_ptr(idx)->~Event();
    }

    ::new (slot_ptr(idx)) Event(std::forward<Ev>(ev));
    _constructed[idx].store(1, std::memory_order_release);

    auto& obj = slot_ref(idx);
    if constexpr (requires { obj->tickSequence; })
    {
      obj->tickSequence = static_cast<uint64_t>(seq);
    }
    if constexpr (requires { obj.tickSequence; })
    {
      obj.tickSequence = static_cast<uint64_t>(seq);
    }

    _published[idx].store(seq, std::memory_order_release);

    if (_wakeOnPublish)
    {
      wakeWaiters();
    }
    // Last, so that a stop() waiting the ring out also waits out the wake-up
    // this publisher is making through the bus's own condition variable.
    claim.resolvePublished();
    return {PublishResult::SUCCESS, seq};
  }

  int64_t minGating() const
  {
    const uint32_t n = _consumerCount.load(std::memory_order_acquire);
    int64_t mn = INT64_MAX;
    for (uint32_t i = 0; i < n; ++i)
    {
      const int64_t s = _gating[i].v.load(std::memory_order_acquire);
      mn = s < mn ? s : mn;
    }
    return (mn == INT64_MAX) ? _next.load(std::memory_order_acquire) : mn;
  }

  // Returns minimum sequence consumed by ALL consumers (including optional)
  // Used for safe reclaim - events can only be destroyed after ALL consumers processed them
  // If no consumers, returns INT64_MAX to indicate all events are "consumed"
  int64_t minConsumed() const
  {
    const uint32_t n = _consumerCount.load(std::memory_order_acquire);
    if (n == 0)
    {
      return INT64_MAX;  // No consumers = everything is consumed
    }
    int64_t mn = INT64_MAX;
    for (uint32_t i = 0; i < n; ++i)
    {
      const int64_t s = _consumers[i].seq.load(std::memory_order_acquire);
      mn = s < mn ? s : mn;
    }
    return mn;
  }

 private:
  alignas(64) std::atomic<bool> _running{false};
  alignas(64) std::atomic<int64_t> _next{-1};
  alignas(64) std::atomic<int64_t> _cachedMin{-1};

  struct alignas(alignof(Event)) Storage
  {
    std::byte data[sizeof(Event)];
  };
  // Raw bytes, deliberately not zeroed: a slot is read only while
  // _constructed says an event lives in it.
  alignas(64) std::array<Storage, CapacityPow2> _storage;
  inline Event* slot_ptr(size_t idx) noexcept { return std::launder(reinterpret_cast<Event*>(_storage[idx].data)); }
  inline Event& slot_ref(size_t idx) noexcept { return *slot_ptr(idx); }

  alignas(64) std::array<std::atomic<int64_t>, CapacityPow2> _published{};
  // _constructed values: 0 = empty/reclaimed, 1 = valid event. A slot is
  // stamped only by a publish that owns its sequence, so there is no third
  // state for a sequence that was claimed and then given up on.
  alignas(64) std::array<std::atomic<uint8_t>, CapacityPow2> _constructed{};

  // Monotonic lower bound of minConsumed(); stale values are always safe.
  // Release/acquire so a publisher that trusts the cached bound inherits the
  // happens-before edges the publisher that scanned it established.
  alignas(64) std::atomic<int64_t> _cachedMinConsumed{-1};

  alignas(64) std::array<ConsumerSlot, MaxConsumers> _consumers{};
  // One cache line per consumer: packed gating atomics false-share between
  // consumers storing progress and the producer scanning it.
  struct alignas(64) PaddedSeq
  {
    std::atomic<int64_t> v{0};
  };
  alignas(64) std::array<PaddedSeq, MaxConsumers> _gating{};
  alignas(64) std::atomic<uint32_t> _consumerCount{0};

  std::condition_variable _cv;
  std::mutex _readyMutex;
  std::atomic<uint32_t> _active{0};

  // Health checker bookkeeping. Written by the single health checker (see
  // checkHealth) and read by whoever supervises the bus: consumerHealth(),
  // healthSnapshot() and consumerHealthReport() are advertised as callable at
  // any time, from any thread, so these fields are typed for the threads that
  // read them and not only for the one that writes them.
  //
  // The three fields are also one update. A state on its own does not say
  // whether a consumer is making progress; the sequence and the instant it
  // last moved do, and a report that takes the sequence from one sweep and the
  // instant from another describes progress that never happened. A version
  // counter around the writer's stores -- odd while a sweep is writing -- lets
  // a reader retry until it has a whole update. It costs the reader a retry it
  // almost never takes and the writer two stores per changed consumer, and
  // neither is anywhere near the publish path.
  struct HealthBook
  {
    std::atomic<uint32_t> version{0};
    std::atomic<int64_t> lastSeen{-1};
    // The time_point's representation, because an atomic needs a trivially
    // copyable arithmetic type and steady_clock::time_point is reassembled
    // from this without loss.
    std::atomic<int64_t> lastChangeTicks{0};
    std::atomic<ConsumerHealth> state{ConsumerHealth::HEALTHY};

    static int64_t ticksOf(std::chrono::steady_clock::time_point tp) noexcept
    {
      return static_cast<int64_t>(tp.time_since_epoch().count());
    }
    static std::chrono::steady_clock::time_point timeOf(int64_t ticks) noexcept
    {
      return std::chrono::steady_clock::time_point{
          std::chrono::steady_clock::duration{ticks}};
    }

    // Single writer: the health checker. Everything a reader must see as one
    // update goes between the two odd/even version stores.
    void write(int64_t seen, int64_t changeTicks, ConsumerHealth st) noexcept
    {
      const uint32_t v = version.load(std::memory_order_relaxed);
      version.store(v + 1, std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_release);
      lastSeen.store(seen, std::memory_order_relaxed);
      lastChangeTicks.store(changeTicks, std::memory_order_relaxed);
      state.store(st, std::memory_order_release);
      version.store(v + 2, std::memory_order_release);
    }

    void reset(std::chrono::steady_clock::time_point at) noexcept
    {
      write(-1, ticksOf(at), ConsumerHealth::HEALTHY);
    }
  };
  std::array<HealthBook, MaxConsumers> _healthBook{};
  HealthConfig _healthCfg{};
  std::optional<jthread> _monitorThread{};

  bool _drainOnStop{false};
  bool _ownConsumerThreads{true};
  // Set at subscribe time, read on the publish path: a bus nobody parks on
  // must not pay for parking.
  bool _anyParked{false};
  // The shared wait point, if this bus was pointed at one. Not owned.
  WakeSet* _wakeSet{nullptr};
  // _anyParked || _wakeSet: the single load the publish path makes, so that a
  // bus with neither pays one predictable branch and nothing else.
  bool _wakeOnPublish{false};
  // A missed wake-up costs one of these, not forever. Long on purpose: it is
  // a net, and a net that catches things often is a mechanism nobody meant to
  // build. See setParkNetInterval().
  static constexpr auto kDefaultParkNetInterval = std::chrono::milliseconds(50);
  std::chrono::milliseconds _parkNetInterval{kDefaultParkNetInterval};
  alignas(64) std::atomic<uint32_t> _parkWaiters{0};
  void (*_parkProbe)(void*){nullptr};
  void* _parkProbeUser{nullptr};
  std::mutex _parkMx;
  std::condition_variable _parkCv;
  BackoffMode _backoffMode{config::defaultBackoffMode};

  // Claim accounting. The run's claims are known exactly from the sequence
  // line stop() seals; these count them back out, so that stop() can tell an
  // empty ring from one a publisher is still inside without the publish path
  // announcing itself on the way in. _publishCount doubles as the successful
  // half -- it is a counter the publish path already kept.
  alignas(64) std::atomic<uint64_t> _abandonedClaims{0};
  alignas(64) std::atomic<uint64_t> _claimBase{0};
  alignas(64) std::atomic<int64_t> _runLastClaim{-1};
  alignas(64) std::atomic<bool> _sealed{false};

  // Monitoring counters (relaxed ordering -- advisory only)
  alignas(64) std::atomic<uint64_t> _publishCount{0};
  alignas(64) std::atomic<uint64_t> _dropCount{0};
  alignas(64) std::atomic<uint64_t> _consumeCount{0};

#if LRVX_CPU_AFFINITY_ENABLED
  // CPU affinity / RT
  std::unique_ptr<performance::CpuAffinity> _cpuAffinity;
  std::optional<performance::CoreAssignment> _coreAssignment;
  std::optional<AffinityConfig> _affinityConfig;
#endif
};

}  // namespace lrvx
