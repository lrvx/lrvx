/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2026 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Callback lifetime of IxWebSocketClient against a loopback server that
 * streams frames without pause.
 *
 * What is pinned:
 *
 *  - stop() from an outside thread returns only after the delivery thread
 *    has exited: the onMessage count is frozen the instant stop() returns,
 *    and an onClose that the stop itself dispatches has completed by then.
 *
 *  - stop() from inside a callback only requests the stop, and a start()
 *    issued right after it from another thread collects the old run()
 *    thread and starts a fresh session instead of re-arming the old one.
 *
 *  - The client may be destroyed from inside its own onMessage: the
 *    destructor returns, no callback runs afterwards, and the process
 *    survives.
 */

#include "lrvx-connectors/net/ix_websocket_client.h"

#include <lrvx/log/abstract_logger.h>

#include "loopback_ws_server.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <memory>
#include <string>
#include <thread>

using namespace lrvx;

namespace
{

struct NullLogger final : public ILogger
{
  void info(std::string_view) override {}
  void warn(std::string_view) override {}
  void error(std::string_view) override {}
};

constexpr const char* kFrame = R"({"channel":"trades","data":[]})";

bool waitFor(const std::atomic<uint64_t>& counter, uint64_t atLeast,
             std::chrono::milliseconds budget)
{
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (counter.load() < atLeast && std::chrono::steady_clock::now() < deadline)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return counter.load() >= atLeast;
}

bool waitFor(const std::atomic<bool>& flag, std::chrono::milliseconds budget)
{
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (!flag.load() && std::chrono::steady_clock::now() < deadline)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return flag.load();
}

// Runs fn on a helper thread. A call that does not return within the budget
// is a deadlock: the process exits with a message, since a thread stuck in a
// join cannot be unwound by the test.
template <typename Fn>
void runOrDie(const char* what, Fn fn, std::chrono::seconds budget)
{
  auto done = std::async(std::launch::async, std::move(fn));
  if (done.wait_for(budget) != std::future_status::ready)
  {
    std::fprintf(stderr, "%s did not return within %llds: deadlock\n", what,
                 static_cast<long long>(budget.count()));
    std::fflush(stderr);
    std::_Exit(1);
  }
  done.get();
}

}  // namespace

TEST(IxWebSocketClientCallbackLifetime, NoCallbackRunsAfterStopReturns)
{
  NullLogger logger;
  test::StreamingServer server(kFrame);

  for (int i = 0; i < 5; ++i)
  {
    std::atomic<uint64_t> messages{0};
    std::atomic<bool> closeDone{false};

    IxWebSocketClient client(server.url(), "", /*reconnectDelayMs*/ 5000, &logger,
                             /*pingIntervalSec*/ 0);
    client.onMessage(
        [&](std::string_view)
        {
          messages.fetch_add(1);
        });
    client.onClose(
        [&](int, std::string_view)
        {
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
          closeDone.store(true);
        });

    client.start();
    ASSERT_TRUE(waitFor(messages, 200, std::chrono::seconds(10)))
        << "iteration " << i << ": the stream never arrived";

    client.stop();
    const uint64_t atStop = messages.load();
    const bool closeDoneAtStop = closeDone.load();

    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    EXPECT_EQ(messages.load(), atStop)
        << "iteration " << i << ": onMessage ran after stop() returned";
    // ix reports the locally initiated close through onClose while the
    // socket stops, so the handler has run to completion by the time stop()
    // returns.
    EXPECT_TRUE(closeDoneAtStop) << "iteration " << i
                                 << ": the onClose dispatched by stop() had not completed";
  }
}

TEST(IxWebSocketClientCallbackLifetime, StartAfterStopFromCallbackStartsAFreshSession)
{
  NullLogger logger;
  test::StreamingServer server(kFrame);

  std::atomic<uint64_t> messages{0};
  std::atomic<uint64_t> opens{0};
  std::atomic<bool> stopRequested{false};
  auto client = std::make_unique<IxWebSocketClient>(server.url(), "", /*reconnectDelayMs*/ 5000,
                                                    &logger, /*pingIntervalSec*/ 0);
  IxWebSocketClient* raw = client.get();

  client->onOpen(
      [&]
      {
        opens.fetch_add(1);
      });
  client->onMessage(
      [&](std::string_view)
      {
        if (messages.fetch_add(1) + 1 == 50)
        {
          raw->stop();
          stopRequested.store(true);
        }
      });

  client->start();
  ASSERT_TRUE(waitFor(stopRequested, std::chrono::seconds(10))) << "the stream never arrived";

  // Lands while the old run() thread is still winding down the session the
  // callback asked to stop.
  runOrDie(
      "start() after a stop() from a callback",
      [&]
      {
        raw->start();
      },
      std::chrono::seconds(10));

  EXPECT_TRUE(waitFor(opens, 2, std::chrono::seconds(10))) << "no fresh session was opened";
  const uint64_t before = messages.load();
  EXPECT_TRUE(waitFor(messages, before + 50, std::chrono::seconds(10)))
      << "frames stopped arriving after the restart";

  runOrDie(
      "stop() after the restart",
      [&]
      {
        raw->stop();
      },
      std::chrono::seconds(10));
  client.reset();
}

TEST(IxWebSocketClientCallbackLifetime, DestroyFromInsideOnMessage)
{
  NullLogger logger;
  test::StreamingServer server(kFrame);

  for (int i = 0; i < 5; ++i)
  {
    std::atomic<uint64_t> messages{0};
    std::atomic<uint64_t> afterDestroy{0};
    std::atomic<bool> destroyed{false};
    auto client = std::make_unique<IxWebSocketClient>(server.url(), "", /*reconnectDelayMs*/ 5000,
                                                      &logger, /*pingIntervalSec*/ 0);
    std::unique_ptr<IxWebSocketClient>* owner = &client;

    client->onMessage(
        [&, owner](std::string_view)
        {
          if (destroyed.load())
          {
            afterDestroy.fetch_add(1);
            return;
          }
          if (messages.fetch_add(1) + 1 == 50)
          {
            owner->reset();
            destroyed.store(true);
          }
        });
    client->onClose(
        [&](int, std::string_view)
        {
          if (destroyed.load())
          {
            afterDestroy.fetch_add(1);
          }
        });

    client->start();
    ASSERT_TRUE(waitFor(destroyed, std::chrono::seconds(10)))
        << "iteration " << i << ": the destructor did not return inside onMessage";

    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    EXPECT_EQ(afterDestroy.load(), 0u)
        << "iteration " << i << ": a callback ran after the client was destroyed";
  }
}
