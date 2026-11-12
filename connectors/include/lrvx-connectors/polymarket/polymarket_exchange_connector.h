/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2026 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "polymarket_config.h"

#include <lrvx/book/bus/book_update_bus.h>
#include <lrvx/book/bus/trade_bus.h>
#include <lrvx/common.h>
#include <lrvx/connector/abstract_exchange_connector.h>
#include <lrvx/engine/symbol_registry.h>
#include <lrvx/log/abstract_logger.h>
#include <lrvx/net/abstract_websocket_client.h>

#include <simdjson.h>

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace lrvx
{

class PolymarketExchangeConnector : public IExchangeConnector
{
 public:
  PolymarketExchangeConnector(const PolymarketConfig& config, BookUpdateBus* bookUpdateBus,
                              TradeBus* tradeBus, SymbolRegistry* registry,
                              std::shared_ptr<ILogger> logger);

  void start() override;
  void stop() override;

  std::string exchangeId() const override { return "polymarket"; }

  SymbolId resolveSymbolId(std::string_view tokenId);

  // Reads every configured token's tick size (see
  // PolymarketConfig::instrumentInfoUrl) and registers the tokens with it.
  // start() calls it; public so a test can drive it with a recorded response.
  void loadInstrumentSpecs();

  // What the venue reported for a token, when it was loaded.
  std::optional<InstrumentSpec> instrumentSpec(std::string_view tokenId) const
  {
    return _specs.find(tokenId);
  }

  // Why some tick sizes are missing; empty when everything loaded.
  std::string instrumentInfoError() const { return _specs.error(); }

  // Public so protocol behaviour (snapshot vs incremental price_change) is
  // testable offline by feeding raw frames without a live socket.
  void handleMessage(std::string_view payload);

  // Transport close. Called from the websocket onClose handler; public for
  // the same reason as handleMessage.
  void handleDisconnect(int code, std::string_view reason);

  void pollFeedHealth(MonoNanos now) override;

 private:
  // exchangeTs is the venue-supplied timestamp for this message (read once,
  // outside these functions, via a fresh re-parse so field access here stays
  // forward-only regardless of where "timestamp" sits in the payload).
  void processBookSnapshot(simdjson::ondemand::object obj, uint64_t recvNs, UnixNanos exchangeTs);
  void processPriceChanges(simdjson::ondemand::object obj, uint64_t recvNs, UnixNanos exchangeTs);
  void sendSubscribe(const std::vector<std::string>& tokenIds, const std::string& operation);

  PolymarketConfig _config;

  BookUpdateBus* _bookUpdateBus;
  TradeBus* _tradeBus;
  SymbolRegistry* _registry = nullptr;

  // This connector's own id in the registry, resolved once in the
  // constructor. Every published book event carries it as sourceExchange:
  // CompositeBookMatrix::onBookUpdate drops any update whose sourceExchange is
  // out of range, so leaving it at InvalidExchangeId kept the cross-venue book
  // permanently empty in live.
  ExchangeId _exchangeId{InvalidExchangeId};

  std::shared_ptr<ILogger> _logger;

  std::unique_ptr<IWebSocketClient> _wsMarket;
  std::atomic<bool> _running{false};

  pool::Pool<BookUpdateEvent, config::DEFAULT_CONNECTOR_POOL_CAPACITY> _bookPool;

  std::unordered_map<std::string, SymbolId> _tokenToSymbol;

  InstrumentSpecTable _specs;
};

}  // namespace lrvx
