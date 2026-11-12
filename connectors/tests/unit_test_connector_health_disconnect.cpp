/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2026 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Offline feed-health test: a transport close is the one thing every connector
 * already observes -- each of the four installs an onClose handler -- and all
 * four do nothing with it but write a log line. IExchangeConnector declares
 * DisconnectCallback and emitDisconnect for exactly this, and neither has a
 * single caller anywhere in the tree, so a supervisor cannot learn that a feed
 * went away.
 *
 * The close is delivered through a public entry point rather than a live
 * socket, mirroring how handleMessage is public so protocol behaviour stays
 * testable offline.
 *
 * needs, per connector (Bybit, Bitget, Hyperliquid, Polymarket):
 *   void handleDisconnect(int code, std::string_view reason);
 * public, invoked from the connector's own ws onClose handler, calling
 * IExchangeConnector::emitDisconnect(reason).
 */

#include "lrvx-connectors/bitget/bitget_exchange_connector.h"
#include "lrvx-connectors/bybit/bybit_exchange_connector.h"
#include "lrvx-connectors/hyperliquid/hyperliquid_exchange_connector.h"
#include "lrvx-connectors/polymarket/polymarket_exchange_connector.h"

#include <lrvx/book/bus/book_update_bus.h>
#include <lrvx/book/bus/trade_bus.h>
#include <lrvx/common.h>
#include <lrvx/log/atomic_logger.h>

// ix's own header-only Sec-WebSocket-Accept generator: the listener below has
// to complete a real handshake before a close means anything, and using ix's
// keygen keeps this test free of a hand-rolled SHA1.
#include <ixwebsocket/IXWebSocketHandshakeKeyGen.h>

#include <gtest/gtest.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace lrvx;

namespace
{

std::shared_ptr<AtomicLogger> makeLogger(const char* basename)
{
  auto dir = std::filesystem::temp_directory_path() / "lrvx_conn_disconnect_test_logs";
  std::filesystem::create_directories(dir);
  AtomicLoggerOptions opts;
  opts.directory = dir.string();
  opts.basename = basename;
  return std::make_shared<AtomicLogger>(opts);
}

class Recorder
{
 public:
  void install(IExchangeConnector& c)
  {
    c.setErrorCallbacks(
        [this](std::string_view reason)
        {
          std::lock_guard<std::mutex> lk(_m);
          _reasons.emplace_back(reason);
        },
        [](uint64_t, uint64_t)
        {
        },
        [](SymbolId, uint64_t)
        {
        });
  }

  std::vector<std::string> reasons()
  {
    std::lock_guard<std::mutex> lk(_m);
    return _reasons;
  }

 private:
  std::mutex _m;
  std::vector<std::string> _reasons;
};

// Completes the websocket handshake, holds the session open long enough for
// the client to reach its open state, then drops the connection. ix reports
// that to its owner as a Close (code 1006), which is the only way to reach a
// connector's onClose handler without a real venue -- and the private-stream
// handler is installed inside start(), so it cannot be called directly the way
// handleDisconnect() can.
class HandshakeThenCloseListener
{
 public:
  HandshakeThenCloseListener()
  {
    _fd = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    ::setsockopt(_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    ::bind(_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    socklen_t len = sizeof(addr);
    ::getsockname(_fd, reinterpret_cast<sockaddr*>(&addr), &len);
    _port = ntohs(addr.sin_port);
    ::listen(_fd, 16);
    _thread = std::thread(
        [this]
        {
          while (_running.load())
          {
            const int c = ::accept(_fd, nullptr, nullptr);
            if (c < 0)
            {
              break;
            }
            if (answerHandshake(c))
            {
              // Long enough for the client to settle into its open state, so
              // the drop below is reported as a close of an established
              // session rather than as a failed connection attempt.
              std::this_thread::sleep_for(std::chrono::milliseconds(150));
            }
            ::close(c);
          }
        });
  }

  ~HandshakeThenCloseListener()
  {
    _running.store(false);
    ::shutdown(_fd, SHUT_RDWR);
    ::close(_fd);
    if (_thread.joinable())
    {
      _thread.join();
    }
  }

  std::string url() const { return "ws://127.0.0.1:" + std::to_string(_port); }

 private:
  static bool answerHandshake(int c)
  {
    std::string req;
    char buf[2048];
    while (req.find("\r\n\r\n") == std::string::npos)
    {
      const ssize_t n = ::recv(c, buf, sizeof(buf), 0);
      if (n <= 0)
      {
        return false;
      }
      req.append(buf, static_cast<size_t>(n));
    }
    const std::string marker = "Sec-WebSocket-Key: ";
    const size_t at = req.find(marker);
    if (at == std::string::npos)
    {
      return false;
    }
    const size_t end = req.find("\r\n", at);
    const std::string key = req.substr(at + marker.size(), end - (at + marker.size()));

    char accept[29] = {};
    WebSocketHandshakeKeyGen::generate(key, accept);
    std::string resp =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: ";
    resp += accept;
    resp += "\r\n\r\n";
    return ::send(c, resp.data(), resp.size(), 0) > 0;
  }

  int _fd{-1};
  uint16_t _port{0};
  std::atomic<bool> _running{true};
  std::thread _thread;
};

// Nothing listens on loopback port 1, so a client pointed at it never
// establishes a session and never produces a close: it keeps the connector's
// public stream out of the way of a test about the private one.
constexpr const char* kUnreachable = "ws://127.0.0.1:1";

bool waitForReason(Recorder& rec, std::string_view needle, std::chrono::milliseconds budget)
{
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (std::chrono::steady_clock::now() < deadline)
  {
    for (const auto& reason : rec.reasons())
    {
      if (reason.find(needle) != std::string::npos)
      {
        return true;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return false;
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

}  // namespace

TEST(ConnectorHealthDisconnect, BybitReportsATransportClose)
{
  Buses buses;
  SymbolRegistry registry;

  BybitConfig cfg;
  cfg.instrumentInfoUrl.clear();  // offline: start() reads no instrument info
  cfg.publicEndpoint = "wss://unused.invalid";
  cfg.symbols = {{"BTCUSDT", InstrumentType::Future, BybitConfig::BookDepth::Top50}};

  BybitExchangeConnector connector(cfg, &buses.book, &buses.trade, nullptr, &registry,
                                   makeLogger("bybit_disconnect.log"));
  Recorder rec;
  rec.install(connector);

  connector.handleDisconnect(1006, "abnormal closure");

  const auto reasons = rec.reasons();
  ASSERT_EQ(reasons.size(), 1u) << "a transport close must reach emitDisconnect";
  EXPECT_NE(reasons[0].find("abnormal closure"), std::string::npos);
}

TEST(ConnectorHealthDisconnect, BitgetReportsATransportClose)
{
  Buses buses;
  SymbolRegistry registry;

  BitgetConfig cfg;
  cfg.instrumentInfoUrl.clear();  // offline: start() reads no instrument info
  cfg.publicEndpoint = "wss://unused.invalid";
  cfg.symbols = {{"BTCUSDT", InstrumentType::Future, BitgetConfig::BookDepth::Depth15}};

  BitgetExchangeConnector connector(cfg, &buses.book, &buses.trade, nullptr, &registry,
                                    makeLogger("bitget_disconnect.log"));
  Recorder rec;
  rec.install(connector);

  connector.handleDisconnect(1006, "abnormal closure");

  const auto reasons = rec.reasons();
  ASSERT_EQ(reasons.size(), 1u) << "a transport close must reach emitDisconnect";
  EXPECT_NE(reasons[0].find("abnormal closure"), std::string::npos);
}

TEST(ConnectorHealthDisconnect, HyperliquidReportsATransportClose)
{
  Buses buses;
  SymbolRegistry registry;

  HyperliquidConfig cfg;
  cfg.instrumentInfoUrl.clear();  // offline: start() reads no instrument info
  cfg.wsEndpoint = "wss://unused.invalid";
  cfg.symbols = {"BTC"};

  HyperliquidExchangeConnector connector(cfg, &buses.book, &buses.trade, &registry,
                                         makeLogger("hyperliquid_disconnect.log"));
  Recorder rec;
  rec.install(connector);

  connector.handleDisconnect(1006, "abnormal closure");

  const auto reasons = rec.reasons();
  ASSERT_EQ(reasons.size(), 1u) << "a transport close must reach emitDisconnect";
  EXPECT_NE(reasons[0].find("abnormal closure"), std::string::npos);
}

TEST(ConnectorHealthDisconnect, PolymarketReportsATransportClose)
{
  Buses buses;
  SymbolRegistry registry;

  PolymarketConfig cfg;
  cfg.instrumentInfoUrl.clear();  // offline: start() reads no instrument info
  cfg.wsEndpoint = "wss://unused.invalid";
  cfg.tokenIds = {"TOKEN"};

  PolymarketExchangeConnector connector(cfg, &buses.book, &buses.trade, &registry,
                                        makeLogger("polymarket_disconnect.log"));
  Recorder rec;
  rec.install(connector);

  connector.handleDisconnect(1006, "abnormal closure");

  const auto reasons = rec.reasons();
  ASSERT_EQ(reasons.size(), 1u) << "a transport close must reach emitDisconnect";
  EXPECT_NE(reasons[0].find("abnormal closure"), std::string::npos);
}

// The private stream carries the order and execution reports: losing it stops
// fills reaching the engine, so its close is the same class of event as losing
// the public book and must be reported, not just logged. The handler is
// installed inside start(), so the close is delivered by a real -- if very
// short-lived -- websocket session instead of a direct call.
TEST(ConnectorHealthDisconnect, BybitReportsAPrivateStreamClose)
{
  Buses buses;
  SymbolRegistry registry;
  HandshakeThenCloseListener listener;

  BybitConfig cfg;
  cfg.instrumentInfoUrl.clear();  // offline: start() reads no instrument info
  cfg.publicEndpoint = kUnreachable;
  cfg.privateEndpoint = listener.url();
  cfg.enablePrivate = true;
  cfg.apiKey = "test-key";
  cfg.apiSecret = "test-secret";
  cfg.reconnectDelayMs = 5000;
  cfg.symbols = {{"BTCUSDT", InstrumentType::Future, BybitConfig::BookDepth::Top50}};

  BybitExchangeConnector connector(cfg, &buses.book, &buses.trade, nullptr, &registry,
                                   makeLogger("bybit_private_disconnect.log"));
  Recorder rec;
  rec.install(connector);

  connector.start();
  const bool reported = waitForReason(rec, "private stream", std::chrono::seconds(10));
  connector.stop();

  EXPECT_TRUE(reported) << "a private-stream close must reach emitDisconnect, naming the stream";
}

TEST(ConnectorHealthDisconnect, BitgetReportsAPrivateStreamClose)
{
  Buses buses;
  SymbolRegistry registry;
  HandshakeThenCloseListener listener;

  BitgetConfig cfg;
  cfg.instrumentInfoUrl.clear();  // offline: start() reads no instrument info
  cfg.publicEndpoint = kUnreachable;
  cfg.privateEndpoint = listener.url();
  cfg.enablePrivate = true;
  cfg.apiKey = "test-key";
  cfg.apiSecret = "test-secret";
  cfg.passphrase = "test-pass";
  cfg.reconnectDelayMs = 5000;
  cfg.symbols = {{"BTCUSDT", InstrumentType::Future, BitgetConfig::BookDepth::Depth15}};

  BitgetExchangeConnector connector(cfg, &buses.book, &buses.trade, nullptr, &registry,
                                    makeLogger("bitget_private_disconnect.log"));
  Recorder rec;
  rec.install(connector);

  connector.start();
  const bool reported = waitForReason(rec, "private stream", std::chrono::seconds(10));
  connector.stop();

  EXPECT_TRUE(reported) << "a private-stream close must reach emitDisconnect, naming the stream";
}

// Control: a connector with no error callbacks installed must survive the same
// close -- emitDisconnect on an empty MoveOnlyFunction is a no-op, and wiring
// the call must not turn a disconnect into a crash.
TEST(ConnectorHealthDisconnect, CloseWithNoCallbacksInstalledIsHarmless)
{
  Buses buses;
  SymbolRegistry registry;

  BybitConfig cfg;
  cfg.instrumentInfoUrl.clear();  // offline: start() reads no instrument info
  cfg.publicEndpoint = "wss://unused.invalid";
  cfg.symbols = {{"BTCUSDT", InstrumentType::Future, BybitConfig::BookDepth::Top50}};

  BybitExchangeConnector connector(cfg, &buses.book, &buses.trade, nullptr, &registry,
                                   makeLogger("bybit_disconnect_nocb.log"));

  EXPECT_NO_THROW(connector.handleDisconnect(1000, "normal closure"));
}
