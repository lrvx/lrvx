/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "lrvx-connectors/execution/timeout_order_tracker.h"
#include "lrvx-connectors/util/rate_limit_config.h"

#include <lrvx/common.h>
#include <lrvx/log/log.h>
#include <lrvx/util/concurrency/thread_body.h>
#include <lrvx/util/rate_limiter.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace lrvx
{

// ============================================================================
// Rate Limit Policies - compile-time dispatch, zero overhead when disabled
// ============================================================================

/// No rate limiting - zero overhead
struct NoRateLimitPolicy
{
  static constexpr bool enabled = false;

  void init(const RateLimitConfig&) {}

  template <typename Action, typename OnRejected>
  void gate(OrderId, Action&& action, OnRejected&&)
  {
    action();
  }

  void stop() {}
};

/// Active rate limiting with configurable behavior.
///
/// Every send path of every executor goes through gate(): it owns the decision
/// of whether the request may leave, and when. A path that consults nothing is
/// a path the venue budget does not cover, which is how a single trailing stop
/// used to spend a whole endpoint quota by itself.
///
/// Threading model. A request that holds a token is sent inline on the calling
/// thread -- ordering and latency are unchanged, and nothing is queued. Only
/// RateLimitPolicy::WAIT defers, onto this policy's own sender thread: the
/// action is queued, gate() returns, and the sender sends it once the bucket
/// really has a token. WAIT used to sleep_for on the calling thread (the
/// strategy / event-bus consumer thread, so one contended submit stopped the
/// engine from processing market data) and then proceed whether or not the
/// retry found a token, which sent over the budget -- the one thing a
/// client-side limiter exists to prevent.
///
/// The handoff is bounded: past kMaxDeferred queued requests the submit is
/// refused through onRejected instead of queueing without limit. Deferred
/// actions run after gate() returned, so they must own everything they touch;
/// both callbacks are stored by value.
class ActiveRateLimitPolicy
{
 public:
  static constexpr bool enabled = true;

  ActiveRateLimitPolicy() = default;
  ActiveRateLimitPolicy(const ActiveRateLimitPolicy&) = delete;
  ActiveRateLimitPolicy& operator=(const ActiveRateLimitPolicy&) = delete;

  ~ActiveRateLimitPolicy() { stop(); }

  // Joins the sender thread and refuses every request still queued, through
  // its onRejected. No deferred action runs once this returns, so an owner
  // calls it before tearing down anything a deferred action touches. Safe to
  // call more than once.
  void stop()
  {
    {
      std::lock_guard<std::mutex> lock(_mutex);
      _stopping = true;
    }
    _cv.notify_all();
    if (_sender.joinable())
    {
      _sender.join();
    }

    std::deque<Deferred> leftovers;
    {
      std::lock_guard<std::mutex> lock(_mutex);
      leftovers.swap(_deferred);
    }
    for (auto& item : leftovers)
    {
      LRVX_LOG_WARN("[RateLimit] Dropping deferred orderId=" << item.orderId << " on shutdown");
      item.onRejected();
    }
  }

  void init(RateLimitConfig config)
  {
    _config = std::move(config);
    if (_config.isValid())
    {
      _limiter.emplace(_config.toRateLimiterConfig());
    }
  }

  // onRejected fires whenever the request is refused -- REJECT and CALLBACK
  // both deny it, and so does a full deferral queue. Until this existed none
  // of them told the caller anything beyond a log line: a rate-limited
  // cancelOrder() silently never reached the transport while OrderTracker kept
  // reporting the order active. Callers wire onRejected to publish a
  // REJECTED_RATE_LIMIT event on their OrderExecutionBus, matching what
  // SimulatedExecutor already does for the backtest path.
  template <typename Action, typename OnRejected>
  void gate(OrderId orderId, Action&& action, OnRejected&& onRejected)
  {
    // Fail-open: an invalid config leaves no limiter, and an unlimited
    // executor is the documented outcome of a misconfiguration, not a mute
    // one.
    if (!_limiter)
    {
      action();
      return;
    }

    if (_limiter->tryAcquire())
    {
      action();
      return;
    }

    const auto waitTime = _limiter->timeUntilAvailable();

    switch (_config.policy)
    {
      case RateLimitPolicy::REJECT:
        LRVX_LOG_WARN("[RateLimit] Rejected orderId="
                      << orderId << " wait="
                      << std::chrono::duration_cast<std::chrono::milliseconds>(waitTime).count()
                      << "ms");
        onRejected();
        return;

      case RateLimitPolicy::WAIT:
        defer(orderId, std::function<void()>(std::forward<Action>(action)),
              std::function<void()>(std::forward<OnRejected>(onRejected)));
        return;

      case RateLimitPolicy::CALLBACK:
        if (_config.onRateLimited)
        {
          _config.onRateLimited(orderId, waitTime);
        }
        onRejected();
        return;
    }
  }

 private:
  struct Deferred
  {
    OrderId orderId{};
    std::function<void()> action;
    std::function<void()> onRejected;
  };

  // Deep enough that a burst against a venue budget rides through, shallow
  // enough that a venue which stops draining is noticed instead of being
  // absorbed into memory.
  static constexpr std::size_t kMaxDeferred = 1024;

  // Upper bound on one sleep, so a stop is noticed promptly even when the
  // bucket says to wait for a long time.
  static constexpr auto kMaxSleepSlice = std::chrono::milliseconds(50);

  void defer(OrderId orderId, std::function<void()> action, std::function<void()> onRejected)
  {
    {
      std::unique_lock<std::mutex> lock(_mutex);
      if (_stopping)
      {
        lock.unlock();
        onRejected();
        return;
      }
      if (_deferred.size() >= kMaxDeferred)
      {
        lock.unlock();
        LRVX_LOG_WARN("[RateLimit] Deferral queue full, rejecting orderId=" << orderId);
        onRejected();
        return;
      }
      if (!_sender.joinable())
      {
        // Started on the first deferral: a WAIT-configured executor that never
        // hits its budget pays for no thread.
        _sender = lrvx::makeThread("rate-limit-sender",
                                   [this]
                                   {
                                     senderLoop();
                                   });
      }
      _deferred.push_back(Deferred{orderId, std::move(action), std::move(onRejected)});
    }
    _cv.notify_one();
  }

  void senderLoop()
  {
    for (;;)
    {
      Deferred item;
      {
        std::unique_lock<std::mutex> lock(_mutex);
        _cv.wait(lock,
                 [this]
                 {
                   return _stopping || !_deferred.empty();
                 });
        if (_stopping)
        {
          return;
        }
        item = std::move(_deferred.front());
        _deferred.pop_front();
      }

      if (!waitForToken())
      {
        item.onRejected();
        continue;
      }

      item.action();
    }
  }

  // Returns once this call has taken a token, or false if the request must be
  // refused instead. The budget is re-checked after every sleep rather than
  // assumed: several deferred requests wake into the same refill and only the
  // one that actually takes the token may send.
  bool waitForToken()
  {
    for (;;)
    {
      if (_limiter->tryAcquire())
      {
        return true;
      }

      const auto wait = _limiter->timeUntilAvailable();
      if (wait == RateLimiter::Duration::max())
      {
        // A bucket that never refills cannot be waited out.
        return false;
      }

      const auto slice = std::min<std::chrono::nanoseconds>(wait, kMaxSleepSlice);
      std::unique_lock<std::mutex> lock(_mutex);
      if (_stopping)
      {
        return false;
      }
      _cv.wait_for(lock, slice,
                   [this]
                   {
                     return _stopping;
                   });
      if (_stopping)
      {
        return false;
      }
    }
  }

  RateLimitConfig _config{};
  std::optional<RateLimiter> _limiter;

  std::mutex _mutex;
  std::condition_variable _cv;
  std::deque<Deferred> _deferred;
  bool _stopping{false};
  std::thread _sender;
};

// ============================================================================
// Timeout Tracking Policies - compile-time dispatch, zero overhead when disabled
// ============================================================================

/// No timeout tracking - zero overhead
struct NoTimeoutPolicy
{
  static constexpr bool enabled = false;

  void init(const OrderTimeoutConfig&) {}
  void start() {}
  void trackSubmit(OrderId) {}
  void trackCancel(OrderId) {}
  void trackReplace(OrderId) {}
  void clearPending(OrderId) {}
};

/// Active timeout tracking
class ActiveTimeoutPolicy
{
 public:
  static constexpr bool enabled = true;

  void init(OrderTimeoutConfig config)
  {
    if (config.isValid())
    {
      _tracker = std::make_unique<TimeoutOrderTracker>(std::move(config));
    }
  }

  void start()
  {
    if (_tracker)
    {
      _tracker->start();
    }
  }

  void trackSubmit(OrderId id)
  {
    if (_tracker)
    {
      _tracker->trackSubmit(id);
    }
  }

  void trackCancel(OrderId id)
  {
    if (_tracker)
    {
      _tracker->trackCancel(id);
    }
  }

  void trackReplace(OrderId id)
  {
    if (_tracker)
    {
      _tracker->trackReplace(id);
    }
  }

  void clearPending(OrderId id)
  {
    if (_tracker)
    {
      _tracker->clearPending(id);
    }
  }

 private:
  std::unique_ptr<TimeoutOrderTracker> _tracker;
};

// ============================================================================
// Policy Bundle - combines rate limit and timeout policies
// ============================================================================

template <typename RateLimitPolicyT = NoRateLimitPolicy, typename TimeoutPolicyT = NoTimeoutPolicy>
struct ExecutorPolicies
{
  using RateLimitType = RateLimitPolicyT;
  using TimeoutType = TimeoutPolicyT;

  RateLimitPolicyT rateLimit;
  TimeoutPolicyT timeout;
};

// Common policy configurations
using NoPolicies = ExecutorPolicies<NoRateLimitPolicy, NoTimeoutPolicy>;
using WithRateLimit = ExecutorPolicies<ActiveRateLimitPolicy, NoTimeoutPolicy>;
using WithTimeout = ExecutorPolicies<NoRateLimitPolicy, ActiveTimeoutPolicy>;
using FullPolicies = ExecutorPolicies<ActiveRateLimitPolicy, ActiveTimeoutPolicy>;

}  // namespace lrvx
