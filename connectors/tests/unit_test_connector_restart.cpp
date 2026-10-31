/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2026 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Lifecycle of the websocket connectors against endpoints that refuse every
 * connection: start(), stop(), start() again brings up fresh websocket
 * clients; a second start() while running is a no-op; stop() is idempotent;
 * and the destructor is safe after any of these.
 *
 * Bybit and Bitget build their public client in the constructor and release
 * it in stop(), so the start() after a stop() has to build it again.
 * Polymarket builds its client in start() and is covered for the same
 * contract.
 */

#include "lrvx-connectors/bitget/bitget_exchange_connector.h"
#include "lrvx-connectors/bybit/bybit_exchange_connector.h"
#include "lrvx-connectors/polymarket/polymarket_exchange_connector.h"

#include <lrvx/book/bus/book_update_bus.h>
#include <lrvx/book/bus/trade_bus.h>
#include <lrvx/common.h>
#include <lrvx/engine/symbol_registry.h>
#include <lrvx/log/atomic_logger.h>

#include "loopback_ws_server.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

using namespace lrvx;

namespace
{

std::filesystem::path logDir()
{
  return std::filesystem::temp_directory_path() / "lrvx_conn_restart_logs";
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

std::shared_ptr<AtomicLogger> makeLogger(const char* basename)
{
  const auto dir = logDir();
  std::filesystem::create_directories(dir);
  AtomicLoggerOptions opts;
  opts.directory = dir.string();
  opts.basename = basename;
  return std::make_shared<AtomicLogger>(opts);
}

struct Buses
{
  BookUpdateBus book;
  TradeBus trade;

  Buses()
  {
    book.start();
    trade.start();
  }

  ~Buses()
  {
    book.stop();
    trade.stop();
  }
};

BybitConfig bybitConfig(bool withPrivate)
{
  BybitConfig cfg;
  cfg.publicEndpoint = test::refusingUrl();
  cfg.reconnectDelayMs = 5000;
  cfg.symbols = {{"BTCUSDT", InstrumentType::Future, BybitConfig::BookDepth::Top50}};
  if (withPrivate)
  {
    cfg.privateEndpoint = test::refusingUrl();
    cfg.enablePrivate = true;
    cfg.apiKey = "test-key";
    cfg.apiSecret = "test-secret";
  }
  return cfg;
}

BitgetConfig bitgetConfig(bool withPrivate)
{
  BitgetConfig cfg;
  cfg.publicEndpoint = test::refusingUrl();
  cfg.reconnectDelayMs = 5000;
  cfg.symbols = {{"BTCUSDT", InstrumentType::Future, BitgetConfig::BookDepth::Depth15}};
  if (withPrivate)
  {
    cfg.privateEndpoint = test::refusingUrl();
    cfg.enablePrivate = true;
    cfg.apiKey = "test-key";
    cfg.apiSecret = "test-secret";
    cfg.passphrase = "test-pass";
  }
  return cfg;
}

PolymarketConfig polymarketConfig()
{
  PolymarketConfig cfg;
  cfg.wsEndpoint = test::refusingUrl();
  cfg.reconnectDelayMs = 5000;
  cfg.tokenIds = {"TOKEN"};
  return cfg;
}

// start, stop, start, start, stop, stop on one connector, then destroy it.
void exerciseLifecycle(IExchangeConnector& connector)
{
  connector.start();
  connector.stop();
  connector.start();
  connector.start();
  connector.stop();
  connector.stop();
}

}  // namespace

TEST(ConnectorRestart, BybitPublicOnly)
{
  Buses buses;
  SymbolRegistry registry;
  BybitExchangeConnector connector(bybitConfig(false), &buses.book, &buses.trade, nullptr,
                                   &registry, makeLogger("bybit_restart.log"));
  exerciseLifecycle(connector);
}

TEST(ConnectorRestart, BybitWithPrivateStream)
{
  Buses buses;
  SymbolRegistry registry;
  BybitExchangeConnector connector(bybitConfig(true), &buses.book, &buses.trade, nullptr, &registry,
                                   makeLogger("bybit_private_restart.log"));
  exerciseLifecycle(connector);
}

TEST(ConnectorRestart, BybitDestroyedWhileRunningAfterRestart)
{
  Buses buses;
  SymbolRegistry registry;
  BybitExchangeConnector connector(bybitConfig(true), &buses.book, &buses.trade, nullptr, &registry,
                                   makeLogger("bybit_restart_running.log"));
  connector.start();
  connector.stop();
  connector.start();
}

TEST(ConnectorRestart, BitgetPublicOnly)
{
  Buses buses;
  SymbolRegistry registry;
  BitgetExchangeConnector connector(bitgetConfig(false), &buses.book, &buses.trade, nullptr,
                                    &registry, makeLogger("bitget_restart.log"));
  exerciseLifecycle(connector);
}

TEST(ConnectorRestart, BitgetWithPrivateStream)
{
  Buses buses;
  SymbolRegistry registry;
  BitgetExchangeConnector connector(bitgetConfig(true), &buses.book, &buses.trade, nullptr,
                                    &registry, makeLogger("bitget_private_restart.log"));
  exerciseLifecycle(connector);
}

TEST(ConnectorRestart, BitgetDestroyedWhileRunningAfterRestart)
{
  Buses buses;
  SymbolRegistry registry;
  BitgetExchangeConnector connector(bitgetConfig(true), &buses.book, &buses.trade, nullptr,
                                    &registry, makeLogger("bitget_restart_running.log"));
  connector.start();
  connector.stop();
  connector.start();
}

TEST(ConnectorRestart, Polymarket)
{
  Buses buses;
  SymbolRegistry registry;
  PolymarketExchangeConnector connector(polymarketConfig(), &buses.book, &buses.trade, &registry,
                                        makeLogger("polymarket_restart.log"));
  exerciseLifecycle(connector);
}
