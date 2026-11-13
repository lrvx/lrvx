/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Tests for the C-API logger callback.
//
// `lrvx_set_log_callback(cb, ud)` redirects LRVX_LOG_INFO/WARN/ERROR to
// the user-supplied function. NULL restores the default ConsoleLogger.

#include "lrvx/capi/lrvx_capi.h"
#include "lrvx/log/log.h"

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{

struct Captured
{
  int32_t level;
  std::string msg;
};

struct State
{
  std::mutex mu;
  std::vector<Captured> entries;
  std::atomic<int> calls{0};
};

void capture_cb(void* ud, int32_t level, const char* msg)
{
  auto* s = static_cast<State*>(ud);
  s->calls.fetch_add(1, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lk(s->mu);
  s->entries.push_back({level, std::string(msg ? msg : "")});
}

}  // namespace

TEST(CapiLogger, CallbackReceivesInfoMessage)
{
  State state;
  lrvx_set_log_callback(capture_cb, &state);

  LRVX_LOG_INFO("hello info");

  ASSERT_EQ(state.calls.load(), 1);
  std::lock_guard<std::mutex> lk(state.mu);
  ASSERT_EQ(state.entries.size(), 1u);
  EXPECT_EQ(state.entries[0].level, 0);
  EXPECT_EQ(state.entries[0].msg, "hello info");

  lrvx_set_log_callback(nullptr, nullptr);  // cleanup
}

TEST(CapiLogger, LevelMapping)
{
  State state;
  lrvx_set_log_callback(capture_cb, &state);

  LRVX_LOG_INFO("i");
  LRVX_LOG_WARN("w");
  LRVX_LOG_ERROR("e");

  std::lock_guard<std::mutex> lk(state.mu);
  ASSERT_EQ(state.entries.size(), 3u);
  EXPECT_EQ(state.entries[0].level, 0);
  EXPECT_EQ(state.entries[0].msg, "i");
  EXPECT_EQ(state.entries[1].level, 1);
  EXPECT_EQ(state.entries[1].msg, "w");
  EXPECT_EQ(state.entries[2].level, 2);
  EXPECT_EQ(state.entries[2].msg, "e");

  lrvx_set_log_callback(nullptr, nullptr);
}

TEST(CapiLogger, NullCallbackRestoresDefault)
{
  State state;
  lrvx_set_log_callback(capture_cb, &state);
  LRVX_LOG_INFO("captured");
  ASSERT_EQ(state.calls.load(), 1);

  // Detach. Subsequent logs go to the default ConsoleLogger and don't
  // touch the state.
  lrvx_set_log_callback(nullptr, nullptr);
  LRVX_LOG_INFO("not captured");

  EXPECT_EQ(state.calls.load(), 1) << "callback was detached; should not fire";
}

TEST(CapiLogger, ReplacingCallbackRoutesToNewTarget)
{
  State a, b;

  lrvx_set_log_callback(capture_cb, &a);
  LRVX_LOG_INFO("to a");

  lrvx_set_log_callback(capture_cb, &b);
  LRVX_LOG_INFO("to b");

  EXPECT_EQ(a.calls.load(), 1);
  EXPECT_EQ(b.calls.load(), 1);

  lrvx_set_log_callback(nullptr, nullptr);
}

TEST(CapiLogger, MessageContentSurvivesStreamFormatting)
{
  // The macros build the message via std::ostringstream <<; verify the
  // composed text reaches the callback intact.
  State state;
  lrvx_set_log_callback(capture_cb, &state);

  int n = 42;
  double pi = 3.14;
  LRVX_LOG_WARN("n=" << n << " pi=" << pi);

  std::lock_guard<std::mutex> lk(state.mu);
  ASSERT_EQ(state.entries.size(), 1u);
  EXPECT_NE(state.entries[0].msg.find("n=42"), std::string::npos);
  EXPECT_NE(state.entries[0].msg.find("pi=3.14"), std::string::npos);

  lrvx_set_log_callback(nullptr, nullptr);
}

TEST(CapiLogger, LoggingDisabledMacroSuppressesCallback)
{
  // LRVX_LOG_OFF / ON are part of the public log.h API; verify the
  // callback path respects them.
  State state;
  lrvx_set_log_callback(capture_cb, &state);

  LRVX_LOG_OFF();
  LRVX_LOG_INFO("suppressed");
  EXPECT_EQ(state.calls.load(), 0);

  LRVX_LOG_ON();
  LRVX_LOG_INFO("delivered");
  EXPECT_EQ(state.calls.load(), 1);

  lrvx_set_log_callback(nullptr, nullptr);
}

namespace
{

// A callback's user data. Once lrvx_set_log_callback has replaced or removed
// the callback that owns it, the caller may free it, so a call that still
// reaches it is a call into freed memory.
struct Sink
{
  std::atomic<bool> retired{false};
};

std::atomic<int> g_lateCalls{0};
std::atomic<int> g_calls{0};

void sinkCb(void* ud, int32_t, const char*)
{
  auto* s = static_cast<Sink*>(ud);
  g_calls.fetch_add(1, std::memory_order_relaxed);
  // Stay in the callback a moment, as a real sink formatting or queueing
  // the line would, so a replacement lands while calls are in flight.
  std::this_thread::yield();
  if (s->retired.load(std::memory_order_acquire))
  {
    g_lateCalls.fetch_add(1, std::memory_order_relaxed);
  }
}

}  // namespace

// Replacing or removing the callback while other threads log through it.
// The old adapter was freed right after the global logger was swapped, while
// a thread that had already loaded it could still be inside it, and the old
// callback could still be called with user data its owner had been told it
// could free. Once lrvx_set_log_callback returns, the old callback is neither
// running nor called again.
TEST(CapiLogger, ReplacingAndRemovingIsSafeWhileThreadsLog)
{
  g_lateCalls = 0;
  g_calls = 0;
  std::atomic<bool> stop{false};
  std::vector<std::thread> loggers;
  for (int t = 0; t < 4; ++t)
  {
    loggers.emplace_back(
        [&stop]
        {
          while (!stop.load(std::memory_order_acquire))
          {
            LRVX_LOG_WARN("concurrent line");
          }
        });
  }

  std::vector<std::unique_ptr<Sink>> sinks;
  Sink* current = nullptr;
  for (int i = 0; i < 3000; ++i)
  {
    Sink* previous = current;
    if (i % 3 == 2)
    {
      lrvx_set_log_callback(nullptr, nullptr);
      current = nullptr;
    }
    else
    {
      sinks.push_back(std::make_unique<Sink>());
      current = sinks.back().get();
      const int before = g_calls.load();
      lrvx_set_log_callback(sinkCb, current);
      // Let the loggers reach it before it is replaced.
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
      while (g_calls.load() == before && std::chrono::steady_clock::now() < deadline)
      {
        std::this_thread::yield();
      }
    }
    if (previous != nullptr)
    {
      previous->retired.store(true, std::memory_order_release);
    }
  }
  lrvx_set_log_callback(nullptr, nullptr);
  if (current != nullptr)
  {
    current->retired.store(true, std::memory_order_release);
  }
  const int callsAtDetach = g_calls.load();
  stop.store(true, std::memory_order_release);
  for (auto& th : loggers)
  {
    th.join();
  }

  EXPECT_GT(callsAtDetach, 0) << "the loggers reached the callback";
  EXPECT_EQ(g_lateCalls.load(), 0) << "a replaced or removed callback was still called";
  EXPECT_EQ(g_calls.load(), callsAtDetach) << "a call arrived after the last detach";
}

// A callback may replace or remove itself: the call that does it does not
// wait for itself to finish.
TEST(CapiLogger, ACallbackMayDetachItself)
{
  static std::atomic<int> calls{0};
  calls = 0;
  lrvx_set_log_callback(
      [](void*, int32_t, const char*)
      {
        calls.fetch_add(1);
        lrvx_set_log_callback(nullptr, nullptr);
      },
      nullptr);
  LRVX_LOG_INFO("first");
  LRVX_LOG_INFO("second");
  EXPECT_EQ(calls.load(), 1);
}
