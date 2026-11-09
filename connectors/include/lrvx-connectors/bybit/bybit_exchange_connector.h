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
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace lrvx
{

struct BybitConfig
{
  enum class BookDepth
  {
    Invalid = -1,
    Top1 = 1,
    Top25 = 25,
    Top50 = 50,
    Top100 = 100,
    Top200 = 200,
    Top500 = 500
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
  bool enablePrivate = false;
  // Subscribe orderbook.1 next to each symbol's depth topic and publish it as
  // BookUpdateType::TOP_OF_BOOK. Bybit pushes level 1 every 10 ms and the
  // depth topics every 20 to 200 ms, so between depth frames the touch is as
  // fresh as this stream; the depth book ignores the frames, the strategy's
  // touch and the cross-venue matrix follow them. Off by default: it is an
  // extra event per tick per symbol, and Bybit's depth is not throttled the
  // way Hyperliquid's l2Book is. A symbol configured with BookDepth::Top1
  // already takes its depth from that topic and gets no second subscription;
  // options have no level-1 topic and are skipped.
  bool subscribeBbo{false};
};

class BybitExchangeConnector : public IExchangeConnector
{
 public:
  BybitExchangeConnector(const BybitConfig& config, BookUpdateBus* bookUpdateBus,
                         TradeBus* tradeBus, OrderExecutionBus* orderBus, SymbolRegistry* registry,
                         std::shared_ptr<ILogger> logger);

  // _pingThread is a joinable std::thread once start() has run; destroying it
  // joinable is std::terminate. stop() already joins it, so the destructor
  // only needs to make sure stop() has happened on every teardown path, not
  // just the ones that call it explicitly.
  ~BybitExchangeConnector() override { stop(); }

  void start() override;
  void stop() override;

  std::string exchangeId() const override { return "bybit"; }

  SymbolId resolveSymbolId(std::string_view symbol);

  // The topics start() subscribes, in send order: each symbol's depth topic
  // and publicTrade, then orderbook.1 for the symbols subscribeBbo covers.
  // Static and public so a test can pin them -- the WS client is built in
  // the constructor, so start() itself is not observable offline.
  static std::vector<std::string> subscriptionTopics(const BybitConfig& config);

  // Whether orderbook.1 is subscribed as the touch for this symbol, next to
  // its depth topic (see BybitConfig::subscribeBbo).
  static bool takesLevelOneAsTop(const BybitConfig& config, const BybitConfig::SymbolEntry& entry);

  // Public so protocol behaviour (sequencing, gap resync) is testable offline
  // by feeding raw frames without a live socket.
  void handleMessage(std::string_view payload);

  // Transport close. Called from the websocket onClose handler; public for
  // the same reason as handleMessage -- a close can then be delivered without
  // a live socket.
  void handleDisconnect(int code, std::string_view reason);

  void pollFeedHealth(MonoNanos now) override;

  // Same rationale as handleMessage: public so the private-stream (order /
  // execution) protocol handling is testable offline by feeding raw frames,
  // without a live authenticated socket.
  void handlePrivateMessage(std::string_view payload);

  // Book deltas whose update id broke continuity (dropped + resync triggered).
  uint64_t bookGapCount() const noexcept { return _bookGapCount.load(std::memory_order_relaxed); }

 private:
  // Re-subscribe one symbol's orderbook topic so the exchange re-sends a
  // snapshot (gap recovery). The constructor creates _wsClient unconditionally,
  // so this is never a no-op "before start()" -- the guard below only fires
  // once stop() has reset() the socket. If the send itself fails (closed
  // socket, no connection yet), the resubscribe request never reaches the
  // exchange and the caller's resync latch stays set until the next full
  // reconnect re-subscribes everything from onOpen.
  void resubscribeBook(std::string_view symbolName);

  // The public market-data client. Built by the constructor, released by
  // stop() and built again by the next start().
  std::unique_ptr<IWebSocketClient> makePublicClient() const;

  // Advances the per-order fill watermark to the venue's cumulative filled
  // quantity and writes the increment into ev.fillQty. Returns false when a
  // fill status brings no new quantity -- the same execution already reached
  // the bus from the other private topic, and republishing it would double the
  // position.
  bool applyFillWatermark(OrderEvent& ev, Quantity cumulative);

  BybitConfig _config;

  BookUpdateBus* _bookUpdateBus;
  TradeBus* _tradeBus;

  SymbolRegistry* _registry = nullptr;

  // This connector's own id in the registry, resolved once in the
  // constructor. Every published book event carries it as sourceExchange:
  // CompositeBookMatrix::onBookUpdate drops any update whose sourceExchange is
  // out of range, so leaving it at InvalidExchangeId kept the cross-venue book
  // permanently empty in live.
  ExchangeId _exchangeId{InvalidExchangeId};

  // Per-symbol book continuity: Bybit v5 orderbook deltas carry an update id
  // ("u") that increments by 1 per message; a jump means a dropped frame and a
  // silently wrong book, so the connector drops the delta and re-subscribes for
  // a fresh snapshot. resyncInFlight suppresses repeat resubscribes while the
  // snapshot is on its way.
  struct BookSeqState
  {
    int64_t lastUpdateId{-1};
    bool resyncInFlight{false};
  };
  std::unordered_map<SymbolId, BookSeqState> _bookSeq;
  std::atomic<uint64_t> _bookGapCount{0};

  // Symbols whose orderbook.1 frames are the touch (TOP_OF_BOOK), filled from
  // the config in the constructor and read on the message thread. Keyed by
  // the venue's symbol name, which both the topic and the frame carry.
  struct TransparentStringHash
  {
    using is_transparent = void;
    size_t operator()(std::string_view s) const noexcept
    {
      return std::hash<std::string_view>{}(s);
    }
  };
  std::unordered_set<std::string, TransparentStringHash, std::equal_to<>> _bboSymbols;

  // Cross-topic fill de-duplication and cumulative-to-delta conversion.
  //
  // The private stream subscribes to both "order" and "execution", which
  // announce the *same* execution -- the order topic as a cumulative total
  // (cumExecQty), the execution topic as a single fill. Publishing both booked
  // twice the quantity that actually traded, and the cumulative half left
  // OrderEvent::fillQty at zero. Both are the watermark's job; see
  // FillWatermark.
  FillWatermark _reportedFill;

  std::shared_ptr<ILogger> _logger;

  std::unique_ptr<IWebSocketClient> _wsClient;
  std::unique_ptr<IWebSocketClient> _wsClientPrivate;
  std::atomic<bool> _running{false};

  pool::Pool<BookUpdateEvent, config::DEFAULT_CONNECTOR_POOL_CAPACITY> _bookPool;
  OrderExecutionBus* _orderBus = nullptr;
  std::thread _pingThread;
};

}  // namespace lrvx
