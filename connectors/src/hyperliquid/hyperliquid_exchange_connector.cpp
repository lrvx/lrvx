/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "lrvx-connectors/hyperliquid/hyperliquid_exchange_connector.h"
#include "lrvx-connectors/net/ix_websocket_client.h"
#include "lrvx-connectors/util/safe_parse.h"
#include "lrvx/util/concurrency/thread_body.h"

#include <lrvx/log/log.h>

#include <simdjson.h>

#include <chrono>
#include <cstdlib>

namespace lrvx
{

HyperliquidExchangeConnector::HyperliquidExchangeConnector(const HyperliquidConfig& config,
                                                           BookUpdateBus* bookBus,
                                                           TradeBus* tradeBus,
                                                           SymbolRegistry* symbolRegistry,
                                                           std::shared_ptr<ILogger> logger)
    : _config(config),
      _bookBus(bookBus),
      _tradeBus(tradeBus),
      _registry(symbolRegistry),
      _logger(std::move(logger))
{
  assert(_registry && "symbols registry not set");
  assert(_bookBus && "book bus not set");
  assert(_tradeBus && "trade bus not set");

  // Registering here rather than waiting for someone else to do it: the
  // registry is idempotent, and an id resolved lazily on the first frame would
  // be InvalidExchangeId for whatever ran before it.
  _exchangeId = _registry->registerExchange("hyperliquid");

  // Disable WS protocol ping (pingIntervalSec = 0) - Hyperliquid requires application-level ping
  _wsClient = std::make_unique<IxWebSocketClient>(config.wsEndpoint, "https://app.hyperliquid.xyz",
                                                  config.reconnectDelayMs, _logger.get(), 0);
}

void HyperliquidExchangeConnector::start()
{
  if (_running.exchange(true))
  {
    return;
  }
  _stopped.store(false, std::memory_order_release);

  if (!_wsClient || !_bookBus || !_tradeBus)
  {
    _logger->warn("[Hyperliquid] missing client or buses");
    return;
  }

  // Collects a delivery thread that a stop() from inside a callback left
  // running, so the callbacks below are never replaced while it may still
  // invoke them. A no-op when nothing is running.
  _wsClient->stop();

  _wsClient->onOpen(
      [this]()
      {
        LRVX_LOG("[Hyperliquid] WS open, sending subscriptions");

        for (size_t i = 0; i < _config.symbols.size() && _running.load(); ++i)
        {
          for (const auto& payload : subscriptionPayloads(_config, _config.symbols[i]))
          {
            _wsClient->send(payload);
          }

          // Hyperliquid drops subscriptions when too many frames arrive at
          // once; pause every few coins to stay under that.
          if ((i + 1) % 5 == 0 && i + 1 < _config.symbols.size())
          {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
          }
        }

        LRVX_LOG("[Hyperliquid] Subscribed to " << _config.symbols.size() << " symbols");
      });

  _wsClient->onClose(
      [this](int code, std::string_view reason)
      {
        handleDisconnect(code, reason);
      });

  _wsClient->onMessage(
      [this](std::string_view payload)
      {
        handleMessage(payload);
      });

  // Baseline for the staleness check: without it a feed that never delivers a
  // single frame has no stamp to age out from, which is the loudest failure
  // of the two this check exists for. coinRef() also fills the coin cache
  // here, before the socket exists, so the message thread never resolves
  // through the registry.
  const MonoNanos startedAt = nowMonoNanos();
  for (const auto& coin : _config.symbols)
  {
    markFeedActivity(coinRef(coin).id, startedAt);
  }

  _wsClient->start();

  // Start ping thread to keep connection alive
  _pingThread = makeThread("conn.hyperliquid.ping",
                           [this]
                           {
                             pingLoop();
                           });
}

void HyperliquidExchangeConnector::handleDisconnect(int code, std::string_view reason)
{
  const std::string detail = "code=" + std::to_string(code) + ", reason=" + std::string(reason);
  LRVX_LOG("[Hyperliquid] WS closed: " << detail);
  if (_logger)
  {
    _logger->info("[Hyperliquid] WS closed: " + detail);
  }
  emitDisconnect(detail);
}

void HyperliquidExchangeConnector::pollFeedHealth(MonoNanos now)
{
  checkStaleFeeds(now, _config.staleDataTimeoutMs);
}

void HyperliquidExchangeConnector::pingLoop()
{
  // Initial delay to let connection establish before starting ping loop
  for (int i = 0; i < 50 && _running.load(); ++i)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  while (_running.load())
  {
    if (_wsClient)
    {
      _wsClient->send(R"({"method":"ping"})");
    }
    // Send heartbeat every 30 seconds (Hyperliquid timeout is 60 seconds)
    // Sleep in small intervals to allow quick shutdown
    for (int i = 0; i < 300 && _running.load(); ++i)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
}

void HyperliquidExchangeConnector::stop()
{
  _stopped.store(true, std::memory_order_release);
  if (_running.exchange(false) && _pingThread.joinable())
  {
    _pingThread.join();
  }

  // Waits for the websocket delivery thread to exit, so no callback reaches
  // handleMessage or handleDisconnect once stop() has returned and the
  // members they touch (_coins, _bookPool) can be destroyed. Runs even when
  // the connector was already stopped: a stop() from inside a callback only
  // requests the client stop, and the destructor's stop() is what collects
  // the thread. _wsClient stays set until the destructor because the onOpen
  // callback sends through it.
  if (_wsClient)
  {
    _wsClient->stop();
  }
}

SymbolId HyperliquidExchangeConnector::resolveSymbolId(std::string_view symbol)
{
  if (auto existing = _registry->getSymbolId("hyperliquid", std::string(symbol)))
  {
    return *existing;
  }

  SymbolInfo info;
  info.exchange = "hyperliquid";
  info.symbol = std::string(symbol);
  info.type = InstrumentType::Future;
  return _registry->registerSymbol(info);
}

std::vector<std::string> HyperliquidExchangeConnector::subscriptionPayloads(
    const HyperliquidConfig& config, std::string_view coin)
{
  auto sub = [&](std::string_view channel)
  {
    std::string s{R"({"method":"subscribe","subscription":{"type":")"};
    s += channel;
    s += R"(","coin":")";
    s += coin;
    s += R"("}})";
    return s;
  };

  std::vector<std::string> out{sub("l2Book"), sub("trades")};
  if (config.subscribeBbo)
  {
    out.push_back(sub("bbo"));
  }
  return out;
}

const HyperliquidExchangeConnector::CoinRef& HyperliquidExchangeConnector::coinRef(
    std::string_view coin)
{
  auto it = _coins.find(coin);
  if (it != _coins.end())
  {
    return it->second;
  }

  // First sight of this coin — start() fills the cache for every configured
  // one, so this is the fallback for an unexpected frame: resolve through
  // the registry once and remember the answer.
  SymbolId sid = resolveSymbolId(coin);
  InstrumentType instrument = InstrumentType::Future;
  if (const auto info = _registry->getSymbolInfo(sid))
  {
    instrument = info->type;
  }
  return _coins.emplace(std::string(coin), CoinRef{sid, instrument}).first->second;
}

void HyperliquidExchangeConnector::primeBookEvent(BookUpdateEvent& ev, const CoinRef& coin,
                                                  BookUpdateType type, uint64_t recvNs,
                                                  int64_t exchangeTsMs)
{
  // The pool recycles events and clear() resets only the level vectors, so
  // every header field is written unconditionally — a field skipped here
  // would carry the previous occupant's value.
  ev.update.symbol = coin.id;
  ev.update.instrument = coin.instrument;
  ev.update.type = type;
  ev.update.exchangeTsNs = msToUnixNs(exchangeTsMs);
  ev.recvNs = MonoNanos::fromRaw(recvNs);
  ev.sourceExchange = _exchangeId;
}

void HyperliquidExchangeConnector::publishBookEvent(pool::Handle<BookUpdateEvent>&& ev)
{
  // An empty SNAPSHOT is dropped: a depth consumer would wipe its book on
  // it, and this connector has always treated such frames as noise. An
  // empty TOP_OF_BOOK is the venue saying the touch is gone on both sides,
  // and must reach the matrix so it stops quoting a stale top.
  if (ev->update.bids.empty() && ev->update.asks.empty() &&
      ev->update.type != BookUpdateType::TOP_OF_BOOK)
  {
    return;
  }
  ev->publishTsNs = nowMonoNanos();
  auto [res, _] = _bookBus->tryPublish(std::move(ev));
  if (res != BookUpdateBus::PublishResult::SUCCESS)
  {
    _logger->error(
        "[Hyperliquid] Book update dropped: EventBus full. Increase EventBus capacity or "
        "reduce number of subscriptions.");
  }
}

namespace
{

// {"px":"...","sz":"..."} → level. False when present but unparseable: the
// caller must drop the whole frame, because a published side that silently
// lost a level is indistinguishable downstream from a side that is
// genuinely empty (the cross-venue matrix zeroes it).
bool parseLevel(simdjson::dom::element lvl, BookLevel& out)
{
  auto pxEl = lvl["px"];
  auto szEl = lvl["sz"];
  if (pxEl.error() || szEl.error())
  {
    return false;
  }
  auto price = util::parsePrice(pxEl.get_string().value());
  auto qty = util::parseQty(szEl.get_string().value());
  if (!price || !qty)
  {
    return false;
  }
  out = BookLevel(*price, *qty);
  return true;
}

// Frame time in ms, 0 when absent. Takes a dom::element or a
// simdjson_result wrapping one — both expose operator[].
template <typename Json>
int64_t frameTimeMs(Json&& data)
{
  auto timeEl = data["time"];
  return timeEl.error() ? 0 : timeEl.get_int64().value();
}

}  // namespace

void HyperliquidExchangeConnector::handleMessage(std::string_view payload)
{
  // A stopped connector publishes nothing, whether the frame comes from the
  // socket while stop() is draining it or through this public entry point.
  if (_stopped.load(std::memory_order_acquire))
  {
    return;
  }

  // Stamped before parsing, so it measures when the frame reached this
  // process. CompositeBookMatrix::checkStaleness skips any venue whose
  // lastUpdateNs is still zero, so a book event without it left a frozen
  // Hyperliquid feed quotable forever.
  const uint64_t recvNs = nowNsMonotonic();

  static thread_local simdjson::dom::parser parser;
  const MonoNanos arrivedAt = nowMonoNanos();

  try
  {
    auto doc = parser.parse(payload);

    auto channelEl = doc["channel"];
    if (channelEl.error())
    {
      // Check for error responses
      auto errorEl = doc["error"];
      if (!errorEl.error())
      {
        _logger->error("[Hyperliquid] WS error: " + std::string(errorEl.get_string().value()));
      }
      return;
    }

    std::string_view channel = channelEl.get_string().value();
    auto dataEl = doc["data"];
    if (dataEl.error())
    {
      return;
    }

    if (channel == "l2Book")
    {
      auto coinEl = dataEl["coin"];
      if (coinEl.error())
      {
        return;
      }
      const CoinRef& coin = coinRef(coinEl.get_string().value());
      markFeedActivity(coin.id, arrivedAt);

      auto levelsEl = dataEl["levels"];
      if (levelsEl.error())
      {
        return;
      }

      auto evOpt = _bookPool.acquire();
      if (!evOpt)
      {
        _logger->error(
            "[Hyperliquid] Book pool exhausted, dropping orderbook update. Increase "
            "LRVX_DEFAULT_CONNECTOR_POOL_CAPACITY or ensure EventBus consumers are draining fast "
            "enough.");
        return;
      }
      auto& ev = *evOpt;
      primeBookEvent(*ev, coin, BookUpdateType::SNAPSHOT, recvNs, frameTimeMs(dataEl));

      // levels = [bids, asks]. Hyperliquid sends full snapshots on each
      // update; a level that fails to parse drops the whole frame.
      auto levels = levelsEl.get_array();
      size_t idx = 0;
      for (auto levelArr : levels)
      {
        for (auto lvl : levelArr.get_array())
        {
          BookLevel level;
          if (!parseLevel(lvl, level))
          {
            _logger->warn("[Hyperliquid] dropping l2Book frame: invalid level");
            return;
          }
          (idx == 0 ? ev->update.bids : ev->update.asks).emplace_back(level.price, level.quantity);
        }
        idx++;
      }

      publishBookEvent(std::move(ev));
    }
    else if (channel == "bbo")
    {
      // {"coin","time","bbo":[bid, ask]}; a side is {"px","sz"} or null when
      // that side of the book is empty.
      auto coinEl = dataEl["coin"];
      if (coinEl.error())
      {
        return;
      }
      const CoinRef& coin = coinRef(coinEl.get_string().value());
      markFeedActivity(coin.id, arrivedAt);

      auto bboEl = dataEl["bbo"];
      if (bboEl.error())
      {
        return;
      }

      auto evOpt = _bookPool.acquire();
      if (!evOpt)
      {
        _logger->error(
            "[Hyperliquid] Book pool exhausted, dropping bbo update. Increase "
            "LRVX_DEFAULT_CONNECTOR_POOL_CAPACITY or ensure EventBus consumers are draining fast "
            "enough.");
        return;
      }
      auto& ev = *evOpt;
      primeBookEvent(*ev, coin, BookUpdateType::TOP_OF_BOOK, recvNs, frameTimeMs(dataEl));

      auto bbo = bboEl.get_array();
      size_t idx = 0;
      for (auto lvl : bbo)
      {
        if (!lvl.is_null())
        {
          BookLevel level;
          if (!parseLevel(lvl, level))
          {
            _logger->warn("[Hyperliquid] dropping bbo frame: invalid px/sz");
            return;
          }
          (idx == 0 ? ev->update.bids : ev->update.asks).emplace_back(level.price, level.quantity);
        }
        idx++;
      }

      publishBookEvent(std::move(ev));
    }
    else if (channel == "trades")
    {
      auto arr = dataEl.get_array();

      for (auto t : arr)
      {
        auto coinEl = t["coin"];
        auto pxEl = t["px"];
        auto szEl = t["sz"];
        auto sideEl = t["side"];

        if (coinEl.error() || pxEl.error() || szEl.error() || sideEl.error())
        {
          continue;
        }

        auto priceOpt = util::parsePrice(pxEl.get_string().value());
        auto qtyOpt = util::parseQty(szEl.get_string().value());
        if (!priceOpt || !qtyOpt)
        {
          _logger->warn("[Hyperliquid] Invalid trade price/qty");
          continue;
        }

        const CoinRef& coin = coinRef(coinEl.get_string().value());
        markFeedActivity(coin.id, arrivedAt);

        TradeEvent ev;
        ev.recvNs = MonoNanos::fromRaw(recvNs);
        ev.trade.symbol = coin.id;
        ev.trade.price = *priceOpt;
        ev.trade.quantity = *qtyOpt;
        ev.trade.instrument = coin.instrument;
        std::string_view side = sideEl.get_string().value();
        ev.trade.isBuy = (side == "B" || side == "buy");
        ev.trade.exchangeTsNs = msToUnixNs(frameTimeMs(t));

        ev.publishTsNs = nowMonoNanos();
        auto [res, _] = _tradeBus->tryPublish(ev);
        if (res != TradeBus::PublishResult::SUCCESS)
        {
          _logger->error(
              "[Hyperliquid] Trade dropped: EventBus full. Increase EventBus capacity or reduce "
              "number of subscriptions.");
        }
      }
    }
  }
  catch (const simdjson::simdjson_error& e)
  {
    _logger->warn(std::string("[Hyperliquid] simdjson error: ") + e.what());
  }
}

}  // namespace lrvx
