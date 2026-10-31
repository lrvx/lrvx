/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2026 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Shutdown contract of HyperliquidExchangeConnector: once stop() returns, no
 * callback of the connector runs, so the connector can be destroyed right
 * away.
 *
 * What is pinned:
 *
 *  - A frame handed to the public handleMessage seam after stop() publishes
 *    nothing, whether or not the connector was ever started, and start()
 *    re-enables processing.
 *
 *  - Against a loopback server that streams trades frames without pause,
 *    no trade reaches the bus once stop() has returned, and destroying the
 *    connector straight after stop() is safe. A delivery thread that
 *    outlives stop() reaches the coin cache after the destructor has torn it
 *    down. The client-level guarantee is pinned on IxWebSocketClient itself
 *    in unit_test_ix_ws_client_callback_lifetime.cpp.
 *
 * Everything stays on 127.0.0.1.
 */

#include "lrvx-connectors/hyperliquid/hyperliquid_exchange_connector.h"

#include <lrvx/book/bus/book_update_bus.h>
#include <lrvx/book/bus/trade_bus.h>
#include <lrvx/common.h>
#include <lrvx/engine/symbol_registry.h>
#include <lrvx/log/atomic_logger.h>

#include "loopback_ws_server.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <thread>

using namespace lrvx;

namespace
{

constexpr const char* kTradeFrame =
    R"({"channel":"trades","data":[{"coin":"BTC","side":"B","px":"100.5","sz":"0.1",)"
    R"("time":1700000000000}]})";

std::filesystem::path logDir()
{
  return std::filesystem::temp_directory_path() / "lrvx_hl_stop_logs";
}

// Removes the log directory once every test has run and every logger is gone.
class LogDirCleanup final : public ::testing::Environment
{
 public:
  void TearDown() override
  {
    std::error_code ec;
    std::filesystem::remove_all(logDir(), ec);
  }
};

[[maybe_unused]] ::testing::Environment* const kLogDirCleanup =
    ::testing::AddGlobalTestEnvironment(new LogDirCleanup);

std::shared_ptr<AtomicLogger> makeLogger(const char* name)
{
  const auto dir = logDir();
  std::filesystem::create_directories(dir);
  AtomicLoggerOptions opts;
  opts.directory = dir.string();
  opts.basename = name;
  return std::make_shared<AtomicLogger>(opts);
}

class TradeCounter final : public IMarketDataSubscriber
{
 public:
  SubscriberId id() const override { return 81; }

  void onTrade(const TradeEvent&) override { _count.fetch_add(1, std::memory_order_relaxed); }

  uint64_t count() const { return _count.load(std::memory_order_relaxed); }

 private:
  std::atomic<uint64_t> _count{0};
};

struct Buses
{
  BookUpdateBus book;
  TradeBus trade;
  TradeCounter trades;

  Buses()
  {
    trade.subscribe(&trades);
    book.start();
    trade.start();
  }

  ~Buses()
  {
    book.stop();
    trade.stop();
  }

  // The bus delivers on its own thread; wait until the count has been still
  // for a while so everything published so far has been consumed.
  uint64_t settledTradeCount()
  {
    uint64_t last = trades.count();
    for (int stable = 0; stable < 5;)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      const uint64_t now = trades.count();
      stable = (now == last) ? stable + 1 : 0;
      last = now;
    }
    return last;
  }
};

HyperliquidConfig configFor(std::string endpoint)
{
  HyperliquidConfig cfg;
  cfg.wsEndpoint = std::move(endpoint);
  cfg.symbols = {"BTC"};
  cfg.reconnectDelayMs = 5000;
  return cfg;
}

}  // namespace

TEST(HyperliquidStop, FrameAfterStopPublishesNothing)
{
  Buses buses;
  SymbolRegistry registry;
  HyperliquidExchangeConnector connector(configFor("wss://unused.invalid"), &buses.book,
                                         &buses.trade, &registry, makeLogger("hl_stop_seam"));

  connector.handleMessage(kTradeFrame);
  ASSERT_EQ(buses.settledTradeCount(), 1u) << "a frame before stop() must be processed";

  connector.stop();
  connector.handleMessage(kTradeFrame);
  EXPECT_EQ(buses.settledTradeCount(), 1u) << "a frame after stop() must be dropped";
}

TEST(HyperliquidStop, FrameAfterStartedStopPublishesNothingAndRestartResumes)
{
  Buses buses;
  SymbolRegistry registry;
  // Nothing listens on the endpoint: start() runs its whole body and the
  // client keeps retrying without ever opening a session.
  HyperliquidExchangeConnector connector(configFor(test::refusingUrl()), &buses.book, &buses.trade,
                                         &registry, makeLogger("hl_stop_started"));

  connector.start();
  connector.stop();
  connector.handleMessage(kTradeFrame);
  EXPECT_EQ(buses.settledTradeCount(), 0u) << "a frame after stop() must be dropped";

  connector.start();
  connector.handleMessage(kTradeFrame);
  EXPECT_EQ(buses.settledTradeCount(), 1u) << "start() must re-enable frame processing";
  connector.stop();
}

TEST(HyperliquidStop, NoFrameIsDeliveredAfterStopReturnsWhileStreaming)
{
  test::StreamingServer server(kTradeFrame);
  Buses buses;
  SymbolRegistry registry;

  for (int i = 0; i < 5; ++i)
  {
    auto connector = std::make_unique<HyperliquidExchangeConnector>(
        configFor(server.url()), &buses.book, &buses.trade, &registry,
        makeLogger("hl_stop_streaming"));

    const uint64_t before = buses.trades.count();
    connector->start();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (buses.trades.count() < before + 200 && std::chrono::steady_clock::now() < deadline)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_GE(buses.trades.count(), before + 200)
        << "iteration " << i << ": the stream never reached the bus";

    connector->stop();
    // A fixed pause, not settledTradeCount(): it lets the bus drain what was
    // published before stop() returned, while a delivery thread that is still
    // running keeps the count moving past this snapshot.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const uint64_t atStop = buses.trades.count();
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    EXPECT_EQ(buses.settledTradeCount(), atStop)
        << "iteration " << i << ": frames reached the bus after stop() returned";

    // Destroying the connector straight after stop() is the shutdown path
    // this test guards: the delivery thread must be gone by then.
    connector.reset();
  }
}
