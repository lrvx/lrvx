/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * BybitExchangeConnector declared no destructor, and _pingThread
 * (a joinable std::thread once start() has run) was its last member.
 * Destroying a running connector on any path that skips an explicit stop()
 * call -- an early return, an exception, or simply falling out of scope --
 * hit std::terminate (measured: SIGABRT, exit 134) because a joinable
 * std::thread's destructor terminates the process instead of unwinding.
 *
 * This constructs a real connector against an address nothing listens on
 * (fast, deterministic "connection refused" loop instead of a real network
 * dependency), starts it, and lets it go out of scope WITHOUT calling
 * stop(). Reaching the end of the test is the assertion: before the fix this
 * process never got here.
 */

#include "lrvx-connectors/bybit/bybit_exchange_connector.h"

#include <lrvx/book/bus/book_update_bus.h>
#include <lrvx/book/bus/trade_bus.h>
#include <lrvx/engine/symbol_registry.h>
#include <lrvx/log/abstract_logger.h>

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>

using namespace lrvx;

namespace
{

struct NullLogger final : public lrvx::ILogger
{
  void info(std::string_view) override {}
  void warn(std::string_view) override {}
  void error(std::string_view) override {}
};

BybitConfig makeConfig()
{
  BybitConfig cfg;
  cfg.instrumentInfoUrl.clear();  // offline: start() reads no instrument info
  // Port 1 on loopback: nothing listens there, so every attempt is refused
  // instantly and the ping thread actually starts (start() only bails out
  // early on an INVALID config, not an unreachable one).
  cfg.publicEndpoint = "ws://127.0.0.1:1";
  cfg.symbols = {{"BTCUSDT", InstrumentType::Future, BybitConfig::BookDepth::Top50}};
  return cfg;
}

}  // namespace

TEST(BybitConnectorLifecycle, DestructorWithoutExplicitStopDoesNotAbort)
{
  BookUpdateBus bookBus;
  TradeBus tradeBus;
  bookBus.start();
  tradeBus.start();

  SymbolRegistry registry;
  auto logger = std::make_shared<NullLogger>();

  {
    BybitExchangeConnector connector(makeConfig(), &bookBus, &tradeBus, nullptr, &registry, logger);
    connector.start();
    // Let the ping thread and the ix reconnect loop actually get going.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    // connector is destroyed here WITHOUT calling stop(). Before the fix
    // this is a guaranteed std::terminate from the joinable _pingThread.
  }

  bookBus.stop();
  tradeBus.stop();

  SUCCEED();
}
