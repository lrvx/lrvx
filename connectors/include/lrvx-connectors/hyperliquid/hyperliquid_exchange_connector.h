/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <lrvx/book/bus/book_update_bus.h>
#include <lrvx/book/bus/trade_bus.h>
#include <lrvx/common.h>
#include <lrvx/connector/abstract_exchange_connector.h>
#include <lrvx/engine/symbol_registry.h>
#include <lrvx/log/abstract_logger.h>
#include <lrvx/net/abstract_websocket_client.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace lrvx
{

struct HyperliquidConfig
{
  // The connector is WS-only, so validity gates on the WS endpoint alone.
  // Order signing credentials live on the executor (passed to its ctor), not
  // here, and the REST endpoint the executor posts to is likewise its own
  // constructor argument -- neither has ever been read off this struct.
  bool isValid() const { return !wsEndpoint.empty(); }

  std::string wsEndpoint{"wss://api.hyperliquid.xyz/ws"};
  std::vector<std::string> symbols;
  int reconnectDelayMs{2000};
  // Window after which a symbol that stopped ticking is reported through
  // emitStaleData. 0 disables the check: the right window is a property of
  // the instrument's liquidity, not of the venue, so there is no default the
  // connector can pick for you.
  int staleDataTimeoutMs{0};
  // Subscribe the bbo channel next to l2Book. Hyperliquid throttles l2Book
  // server-side, so between snapshots the touch is only as fresh as the bbo
  // stream. Frames are published as BookUpdateType::TOP_OF_BOOK: the
  // cross-venue matrix consumes them, a depth book ignores them, so this is
  // safe to leave on. Turn it off to shed the extra per-tick events.
  bool subscribeBbo{true};
};

class HyperliquidExchangeConnector : public IExchangeConnector
{
 public:
  HyperliquidExchangeConnector(const HyperliquidConfig& config, BookUpdateBus* bookBus,
                               TradeBus* tradeBus, SymbolRegistry* symbolRegistry,
                               std::shared_ptr<ILogger> logger);

  // _pingThread is a joinable std::thread once start() has run; destroying it
  // joinable is std::terminate. stop() already joins it, so the destructor
  // only needs to make sure stop() has happened on every teardown path, not
  // just the ones that call it explicitly. Same pattern as
  // BybitExchangeConnector.
  ~HyperliquidExchangeConnector() override { stop(); }

  void start() override;

  // Joins the ping thread and the websocket delivery thread before
  // returning: afterwards no callback of this connector runs, and
  // handleMessage drops any frame until the next start(). Called from inside
  // one of the connector's own websocket callbacks it requests the stop
  // instead, and the next start() or the destructor collects the delivery
  // thread.
  void stop() override;

  std::string exchangeId() const override { return "hyperliquid"; }

  SymbolId resolveSymbolId(std::string_view symbol);

  // The subscribe frames start() sends for one coin, in send order. Static
  // and public so a test can pin the wire payloads — the WS client is built
  // in the constructor, so start() itself is not observable offline.
  static std::vector<std::string> subscriptionPayloads(const HyperliquidConfig& config,
                                                       std::string_view coin);

  // Public so the feed protocol handling (l2Book snapshots, bbo tops,
  // trades) is testable offline by feeding raw frames without a live
  // socket. Same seam BybitExchangeConnector exposes.
  void handleMessage(std::string_view payload);

  // Transport close. Called from the websocket onClose handler; public for
  // the same reason as handleMessage.
  void handleDisconnect(int code, std::string_view reason);

  void pollFeedHealth(MonoNanos now) override;

 private:
  HyperliquidConfig _config;

  BookUpdateBus* _bookBus;
  TradeBus* _tradeBus;
  SymbolRegistry* _registry{nullptr};

  // This connector's own id in the registry, resolved once in the
  // constructor. Every published event carries it as sourceExchange:
  // CompositeBookMatrix::onBookUpdate drops any update whose sourceExchange is
  // out of range, so leaving it at InvalidExchangeId kept the cross-venue book
  // permanently empty in live.
  ExchangeId _exchangeId{InvalidExchangeId};

  std::shared_ptr<ILogger> _logger;

  std::unique_ptr<IWebSocketClient> _wsClient;
  std::atomic<bool> _running{false};
  // Set by stop(), cleared by start(). Separate from _running because
  // handleMessage is also driven offline without start(), and those frames
  // must still be processed.
  std::atomic<bool> _stopped{false};

  std::thread _pingThread;
  void pingLoop();

  // coin → (symbol id, instrument), resolved once per coin so the message
  // thread never takes the registry lock or builds a std::string per frame.
  // Touched only from start() (before the socket exists) and from the WS
  // message thread, so it needs no lock.
  struct CoinRef
  {
    SymbolId id;
    InstrumentType instrument;
  };
  struct TransparentStringHash
  {
    using is_transparent = void;
    size_t operator()(std::string_view s) const noexcept
    {
      return std::hash<std::string_view>{}(s);
    }
  };
  const CoinRef& coinRef(std::string_view coin);
  std::unordered_map<std::string, CoinRef, TransparentStringHash, std::equal_to<>> _coins;

  // Shared tail of every book path: header fields a recycled pool slot must
  // have overwritten, and the publish itself. exchangeTsMs is 0 when the
  // frame carried no time — written through regardless, never inherited.
  void primeBookEvent(BookUpdateEvent& ev, const CoinRef& coin, BookUpdateType type,
                      uint64_t recvNs, int64_t exchangeTsMs);
  void publishBookEvent(pool::Handle<BookUpdateEvent>&& ev);

  pool::Pool<BookUpdateEvent, config::DEFAULT_CONNECTOR_POOL_CAPACITY> _bookPool;
};

}  // namespace lrvx
