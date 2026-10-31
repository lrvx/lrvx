/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "lrvx-connectors/execution/fill_watermark.h"

#include <lrvx/book/bus/book_update_bus.h>
#include <lrvx/book/bus/trade_bus.h>
#include <lrvx/common.h>
#include <lrvx/connector/abstract_exchange_connector.h>
#include <lrvx/engine/symbol_registry.h>
#include <lrvx/execution/bus/order_execution_bus.h>
#include <lrvx/log/abstract_logger.h>
#include <lrvx/net/abstract_websocket_client.h>

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lrvx
{

struct BitgetConfig
{
  enum class BookDepth
  {
    Invalid = -1,
    Depth1 = 1,
    Depth5 = 5,
    Depth15 = 15,
    DepthFull = 100  // "books" channel - all levels
  };

  struct SymbolEntry
  {
    std::string name;
    InstrumentType type;
    BookDepth depth = BookDepth::Invalid;
  };

  bool isValid() const;

  std::string publicEndpoint;
  std::string privateEndpoint;
  std::vector<SymbolEntry> symbols;
  int reconnectDelayMs{2000};
  // Window after which a symbol that stopped ticking is reported through
  // emitStaleData. 0 disables the check: the right window is a property of
  // the instrument's liquidity, not of the venue, so there is no default the
  // connector can pick for you.
  int staleDataTimeoutMs{0};
  std::string apiKey;
  std::string apiSecret;
  std::string passphrase;
  bool enablePrivate = false;
};

class BitgetExchangeConnector : public IExchangeConnector
{
 public:
  BitgetExchangeConnector(const BitgetConfig& config, BookUpdateBus* bookUpdateBus,
                          TradeBus* tradeBus, OrderExecutionBus* orderBus, SymbolRegistry* registry,
                          std::shared_ptr<ILogger> logger);

  // _pingThread is a joinable std::thread once start() has run; destroying it
  // joinable is std::terminate. stop() already joins it, so the destructor
  // only needs to make sure stop() has happened on every teardown path, not
  // just the ones that call it explicitly. Same pattern as
  // BybitExchangeConnector.
  ~BitgetExchangeConnector() override { stop(); }

  void start() override;
  void stop() override;

  std::string exchangeId() const override { return "bitget"; }

  SymbolId resolveSymbolId(std::string_view symbol);

  // Public so the public-feed protocol handling (book snapshots and deltas,
  // trades) is testable offline by feeding raw frames without a live socket.
  // Same seam BybitExchangeConnector exposes.
  void handleMessage(std::string_view payload);

  // Transport close. Called from the websocket onClose handler; public for
  // the same reason as handleMessage.
  void handleDisconnect(int code, std::string_view reason);

  void pollFeedHealth(MonoNanos now) override;

  // Same rationale: public so the private "orders" channel handling is
  // testable offline, without a live authenticated socket.
  void handlePrivateMessage(std::string_view payload);

  // Book frames whose "seq" broke continuity (delta dropped, resync forced).
  uint64_t bookGapCount() const noexcept { return _bookGapCount.load(std::memory_order_relaxed); }

  // Snapshots whose levels did not hash to the "checksum" the venue sent.
  uint64_t bookChecksumFailureCount() const noexcept
  {
    return _bookChecksumFailureCount.load(std::memory_order_relaxed);
  }

 private:
  // Bitget's books channel ships both integrity fields it defines: "seq",
  // which increments once per frame, and "checksum", a CRC32 over the levels
  // of a snapshot. Either one breaking means the local book is no longer a
  // valid continuation of the venue's, so the frame is dropped and counted,
  // the break is reported, and deltas stay suppressed until a fresh snapshot
  // re-baselines. Returns false when the caller must not publish.
  bool verifyBookIntegrity(SymbolId symbol, std::string_view instId, BookUpdateType type,
                           int64_t seq, std::optional<uint32_t> venueChecksum,
                           const std::vector<std::pair<std::string_view, std::string_view>>& bids,
                           const std::vector<std::pair<std::string_view, std::string_view>>& asks);
  void subscribePrivateOrders();
  void pingLoop();

  BitgetConfig _config;

  BookUpdateBus* _bookUpdateBus;
  TradeBus* _tradeBus;

  SymbolRegistry* _registry = nullptr;

  // This connector's own id in the registry, resolved once in the
  // constructor. Every published event carries it as sourceExchange:
  // CompositeBookMatrix::onBookUpdate drops any update whose sourceExchange is
  // out of range, so leaving it at InvalidExchangeId kept the cross-venue book
  // permanently empty in live.
  ExchangeId _exchangeId{InvalidExchangeId};

  // The venue re-pushes an order's current state after every private
  // resubscribe, so the same fill can arrive more than once. accBaseVolume is
  // the order's cumulative filled quantity; publishing only what it adds makes
  // the repeat a no-op instead of a second fill. See FillWatermark.
  FillWatermark _reportedFill;
  // Per-symbol book integrity. Bitget's books channel carries a "seq" that
  // increments per frame and, on a snapshot, a "checksum" over the levels;
  // either one breaking means the local book is no longer a valid
  // continuation of the venue's, so deltas are suppressed until a fresh,
  // verified snapshot re-baselines.
  struct BookSeqState
  {
    int64_t lastSeq{-1};
    bool invalid{true};
  };
  std::unordered_map<SymbolId, BookSeqState> _bookSeq;
  std::atomic<uint64_t> _bookGapCount{0};
  std::atomic<uint64_t> _bookChecksumFailureCount{0};

  void resubscribeBook(std::string_view symbolName);

  // The public market-data client. Built by the constructor, released by
  // stop() and built again by the next start().
  std::unique_ptr<IWebSocketClient> makePublicClient() const;

  std::shared_ptr<ILogger> _logger;

  std::unique_ptr<IWebSocketClient> _wsClient;
  std::unique_ptr<IWebSocketClient> _wsClientPrivate;
  std::atomic<bool> _running{false};
  std::thread _pingThread;

  pool::Pool<BookUpdateEvent, config::DEFAULT_CONNECTOR_POOL_CAPACITY> _bookPool;
  OrderExecutionBus* _orderBus = nullptr;
};

}  // namespace lrvx
