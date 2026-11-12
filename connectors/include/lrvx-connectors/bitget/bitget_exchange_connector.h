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
#include "lrvx-connectors/execution/instrument_spec.h"

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
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
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
  // Subscribe books1 next to each symbol's depth channel and publish it as
  // BookUpdateType::TOP_OF_BOOK: one level per side, a snapshot on every
  // change, no seq continuity and no checksum of its own. The depth book
  // ignores the frames, the strategy's touch and the cross-venue matrix follow
  // them. Off by default: it is an extra event per tick per symbol. A symbol
  // configured with BookDepth::Depth1 already takes its depth from books1 and
  // gets no second subscription.
  bool subscribeBbo{false};
  // Public REST base the instrument info (price tick, size step) of every
  // configured symbol is read from at start(): GET
  // /api/v2/mix/market/contracts?productType=...&symbol=... for futures,
  // /api/v2/spot/public/symbols?symbol=... for spot. Each symbol is then
  // registered with the venue's tick, marked known, so the executor refuses
  // an off-tick price locally (price_off_tick / trigger_off_tick, as the
  // simulator does) and formats on-tick prices exactly. Empty skips the load.
  // A symbol whose info could not be read is logged as an error and
  // registered with its tick unknown: its prices are sent as given and the
  // venue's own refusal comes back as an event.
  std::string instrumentInfoUrl{"https://api.bitget.com"};
  // The request the load goes through; empty uses defaultInstrumentFetch().
  InstrumentFetch instrumentFetch;
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

  // Reads every configured symbol's instrument info (see the config's
  // instrumentInfoUrl) and registers the symbols with it. start() calls it;
  // public so a test can drive it with a recorded response.
  void loadInstrumentSpecs();

  // What the venue reported for a symbol, when it was loaded.
  std::optional<InstrumentSpec> instrumentSpec(std::string_view symbol) const
  {
    return _specs.find(symbol);
  }

  // Why some configured symbol does not carry the venue's tick: its info
  // could not be read, or the registry entry declares a different tick,
  // which it keeps (see registerVenueSymbol). Empty means that, by the time
  // start() returns, every configured symbol -- including one registered
  // before start() with no tick declared -- holds the venue's tick, known.
  std::string instrumentInfoError() const { return _specs.error(); }

  // The subscribe args start() sends, one JSON object each, in send order:
  // each symbol's depth channel and trade, then books1 for the symbols
  // subscribeBbo covers. Static and public so a test can pin them -- the WS
  // client is built in the constructor, so start() itself is not observable
  // offline.
  static std::vector<std::string> subscriptionArgs(const BitgetConfig& config);

  // Whether books1 is subscribed as the touch for this symbol, next to its
  // depth channel (see BitgetConfig::subscribeBbo).
  static bool takesBooks1AsTop(const BitgetConfig& config, const BitgetConfig::SymbolEntry& entry);

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

  InstrumentSpecTable _specs;
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

  // Symbols whose books1 frames are the touch (TOP_OF_BOOK), filled from the
  // config in the constructor and read on the message thread. Keyed by the
  // venue's instId, which the frame's arg carries.
  struct TransparentStringHash
  {
    using is_transparent = void;
    size_t operator()(std::string_view s) const noexcept
    {
      return std::hash<std::string_view>{}(s);
    }
  };
  std::unordered_set<std::string, TransparentStringHash, std::equal_to<>> _bboSymbols;

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
