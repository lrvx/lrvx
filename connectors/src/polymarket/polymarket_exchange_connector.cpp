/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2026 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "lrvx-connectors/polymarket/polymarket_exchange_connector.h"
#include "lrvx-connectors/net/ix_websocket_client.h"
#include "lrvx-connectors/util/safe_parse.h"

#include <lrvx/log/log.h>
#include <lrvx/util/base/hash.h>

#include <simdjson.h>

#include <algorithm>
#include <sstream>

namespace lrvx
{

static constexpr auto POLYMARKET_ORIGIN = "https://polymarket.com";

PolymarketExchangeConnector::PolymarketExchangeConnector(const PolymarketConfig& config,
                                                         BookUpdateBus* bookUpdateBus,
                                                         TradeBus* tradeBus,
                                                         SymbolRegistry* registry,
                                                         std::shared_ptr<ILogger> logger)
    : _config(config),
      _bookUpdateBus(bookUpdateBus),
      _tradeBus(tradeBus),
      _registry(registry),
      _logger(std::move(logger))
{
  // Registering here rather than waiting for someone else to do it: the
  // registry is idempotent, and an id resolved lazily on the first frame would
  // be InvalidExchangeId for whatever ran before it.
  if (_registry)
  {
    _exchangeId = _registry->registerExchange("polymarket");
  }
}

void PolymarketExchangeConnector::start()
{
  if (!_config.isValid())
  {
    if (_logger)
    {
      _logger->error("[Polymarket] Invalid connector config");
    }
    return;
  }

  if (_running.exchange(true))
  {
    return;
  }

  // Before the tokens are resolved below, so each one is registered with the
  // venue's tick.
  loadInstrumentSpecs();

  _wsMarket = std::make_unique<IxWebSocketClient>(_config.wsEndpoint, POLYMARKET_ORIGIN,
                                                  _config.reconnectDelayMs, _logger.get(),
                                                  _config.pingIntervalSec);

  _wsMarket->onOpen(
      [this]()
      {
        if (_logger)
        {
          _logger->info("[Polymarket] WebSocket connected");
        }

        if (!_config.tokenIds.empty())
        {
          sendSubscribe(_config.tokenIds, "subscribe");
        }
      });

  _wsMarket->onMessage(
      [this](std::string_view payload)
      {
        try
        {
          handleMessage(payload);
        }
        catch (const std::exception& e)
        {
          if (_logger)
          {
            _logger->error(std::string("[Polymarket] Exception: ") + e.what());
          }
        }
      });

  _wsMarket->onClose(
      [this](int code, std::string_view reason)
      {
        handleDisconnect(code, reason);
      });

  // Baseline for the staleness check: without it a feed that never delivers a
  // single frame has no stamp to age out from, which is the loudest failure
  // of the two this check exists for.
  const MonoNanos startedAt = nowMonoNanos();
  for (const auto& tokenId : _config.tokenIds)
  {
    markFeedActivity(resolveSymbolId(tokenId), startedAt);
  }

  _wsMarket->start();

  if (_logger)
  {
    _logger->info("[Polymarket] Connector started");
  }
}

void PolymarketExchangeConnector::stop()
{
  if (!_running.exchange(false))
  {
    return;
  }

  if (_wsMarket)
  {
    _wsMarket->stop();
    _wsMarket.reset();
  }

  if (_logger)
  {
    _logger->info("[Polymarket] Connector stopped");
  }
}

void PolymarketExchangeConnector::handleDisconnect(int code, std::string_view reason)
{
  const std::string detail = "code=" + std::to_string(code) + ", reason=" + std::string(reason);
  if (_logger)
  {
    _logger->info("[Polymarket] WebSocket closed: " + detail);
  }
  emitDisconnect(detail);
}

void PolymarketExchangeConnector::pollFeedHealth(MonoNanos now)
{
  checkStaleFeeds(now, _config.staleDataTimeoutMs);
}

void PolymarketExchangeConnector::loadInstrumentSpecs()
{
  _specs.clear();
  if (_config.instrumentInfoUrl.empty() || !_registry)
  {
    return;
  }
  const InstrumentFetch fetch =
      _config.instrumentFetch ? _config.instrumentFetch : defaultInstrumentFetch();
  std::string unreachable;

  for (const auto& tokenId : _config.tokenIds)
  {
    // One unreachable venue is enough to know: the rest would each wait out
    // the same timeout inside start().
    if (!unreachable.empty())
    {
      _specs.addError(tokenId + ": not requested, " + unreachable);
      continue;
    }
    auto body = fetch(_config.instrumentInfoUrl + "/tick-size?token_id=" + tokenId, "");
    if (!body)
    {
      _specs.addError(tokenId + ": " + body.error());
      unreachable = "the previous request failed";
      continue;
    }
    auto spec = instrument_info::parsePolymarketTickSize(*body);
    if (!spec)
    {
      _specs.addError(tokenId + ": " + spec.error());
      continue;
    }
    _specs.add(tokenId, *spec);
  }
  reportInstrumentLoad(_specs, "Polymarket", _logger.get());

  // Past the token cache, which a previous start() already filled: every
  // load brings the registry entry to the venue's tick.
  if (!_registry)
  {
    return;
  }
  for (const auto& tokenId : _config.tokenIds)
  {
    _tokenToSymbol[tokenId] = registerToken(tokenId);
  }
}

SymbolId PolymarketExchangeConnector::registerToken(const std::string& tokenId)
{
  SymbolInfo info;
  info.exchange = "polymarket";
  info.symbol = tokenId;
  info.type = InstrumentType::Spot;  // TODO: Add PredictionMarket type
  return registerVenueSymbol(*_registry, info, _specs, _logger.get());
}

SymbolId PolymarketExchangeConnector::resolveSymbolId(std::string_view tokenId)
{
  std::string key(tokenId);

  auto it = _tokenToSymbol.find(key);
  if (it != _tokenToSymbol.end())
  {
    return it->second;
  }

  if (_registry)
  {
    SymbolId id = registerToken(key);
    _tokenToSymbol[key] = id;
    return id;
  }

  SymbolId id = static_cast<SymbolId>(hash::fnv1a_64(tokenId.data(), tokenId.size()) & 0xFFFFFFFF);
  _tokenToSymbol[key] = id;
  return id;
}

// Helper to parse value that can be string or number
static std::optional<double> parseStringOrDouble(simdjson::ondemand::value val)
{
  // Try as string first
  auto strResult = val.get_string();
  if (!strResult.error())
  {
    return util::safeParseDouble(strResult.value());
  }

  // Try as double
  auto dblResult = val.get_double();
  if (!dblResult.error())
  {
    return dblResult.value();
  }

  // Try as int64
  auto intResult = val.get_int64();
  if (!intResult.error())
  {
    return static_cast<double>(intResult.value());
  }

  return std::nullopt;
}

// Polymarket messages carry their own "timestamp" (epoch millis, sometimes a
// JSON string, sometimes a bare number in test fixtures) but the field can
// sit after content this connector has already read forward past (bids/asks,
// price_changes...), and simdjson ondemand rejects a backward field access.
// A second, independent parse of the same raw text sidesteps the ordering
// constraint entirely: "timestamp" is the first (and only) field this fresh
// iterator ever touches, so it is reachable regardless of its real position.
// Falls back to processing time -- the documented honest fallback in
// time.h -- if the field is absent or unparseable.
static UnixNanos readVenueTimestampNs(std::string_view payload)
{
  static thread_local simdjson::ondemand::parser tsParser;
  try
  {
    simdjson::padded_string padded(payload);
    auto doc = tsParser.iterate(padded);
    auto objResult = doc.get_object();
    if (objResult.error())
    {
      return nowUnixNanos();
    }
    auto tsField = objResult.value()["timestamp"];
    if (tsField.error())
    {
      return nowUnixNanos();
    }
    auto msOpt = parseStringOrDouble(tsField.value());
    if (!msOpt)
    {
      return nowUnixNanos();
    }
    return msToUnixNs(static_cast<int64_t>(*msOpt));
  }
  catch (const simdjson::simdjson_error&)
  {
    return nowUnixNanos();
  }
}

void PolymarketExchangeConnector::handleMessage(std::string_view payload)
{
  static thread_local simdjson::ondemand::parser parser;
  const uint64_t recvNs = nowNsMonotonic();

  try
  {
    simdjson::padded_string padded(payload);
    auto doc = parser.iterate(padded);

    // Check first character to determine message type
    if (!payload.empty() && payload[0] == '[')
    {
      // Initial snapshot - array of book snapshots, one shared message with
      // no reliable single top-level timestamp to key off; processing time
      // is the honest fallback here (this is a connect-time state dump, not
      // a streamed, possibly-delayed update).
      for (auto item : doc.get_array())
      {
        processBookSnapshot(item.get_object().value(), recvNs, nowUnixNanos());
      }
      return;
    }

    auto obj = doc.get_object().value();
    const UnixNanos venueTs = readVenueTimestampNs(payload);

    // event_type is the first field: read it first so every field access below
    // stays forward-only (simdjson ondemand throws OUT_OF_ORDER when a later
    // field is read and then an earlier one).
    auto eventTypeField = obj["event_type"];
    if (eventTypeField.error())
    {
      return;
    }

    std::string_view eventType = eventTypeField.get_string().value();

    if (eventType == "book")
    {
      processBookSnapshot(std::move(obj), recvNs, venueTs);
    }
    else if (eventType == "price_change")
    {
      // Incremental book updates: apply them (previously dropped, leaving the
      // book stale between snapshots).
      processPriceChanges(std::move(obj), recvNs, venueTs);
    }
    else if (eventType == "tick_size_change")
    {
      processTickSizeChange(payload);
    }
    else if (eventType == "last_trade_price" || eventType == "trade")
    {
      auto assetIdField = obj["asset_id"];
      if (assetIdField.error())
      {
        return;
      }

      std::string_view tokenId = assetIdField.get_string().value();
      SymbolId sym = resolveSymbolId(tokenId);

      TradeEvent ev{};
      ev.recvNs = MonoNanos::fromRaw(recvNs);
      ev.trade.symbol = sym;

      auto priceField = obj["price"];
      auto sizeField = obj["size"];
      auto sideField = obj["side"];

      if (!priceField.error() && !sizeField.error())
      {
        auto priceOpt = parseStringOrDouble(priceField.value());
        auto sizeOpt = parseStringOrDouble(sizeField.value());

        if (priceOpt && sizeOpt)
        {
          // Polymarket trade price/size can arrive as a JSON number, so this
          // path keeps parseStringOrDouble (string-or-number). Book levels
          // below are always strings and use the fixed-point parser.
          ev.trade.price = Price::fromDouble(*priceOpt);
          ev.trade.quantity = Quantity::fromDouble(*sizeOpt);

          if (!sideField.error())
          {
            ev.trade.isBuy = (sideField.get_string().value() == "BUY");
          }

          ev.trade.exchangeTsNs = venueTs;
          ev.publishTsNs = nowMonoNanos();
          _tradeBus->publish(ev);
        }
      }
    }
  }
  catch (const simdjson::simdjson_error& e)
  {
    if (_logger)
    {
      _logger->warn(std::string("[Polymarket] simdjson error: ") + e.what());
    }
  }
}

// {"event_type":"tick_size_change","market":"0x..","asset_id":"..",
//  "old_tick_size":"0.01","new_tick_size":"0.001","timestamp":"<ms>"}
// The venue moves a market to a finer tick as its price nears 0 or 1, and
// back. The new tick takes the place of the one read at start: in the spec
// table, and in the registry through the same call start() makes, so a
// strategy re-sizes its book on its next event and a declared tick that
// differs is kept and reported.
void PolymarketExchangeConnector::processTickSizeChange(std::string_view payload)
{
  simdjson::dom::parser parser;
  simdjson::padded_string padded(payload);
  auto parsed = parser.parse(padded);
  if (parsed.error())
  {
    return;
  }
  auto doc = parsed.value();
  std::string_view assetId;
  auto tickField = doc["new_tick_size"];
  if (doc["asset_id"].get_string().get(assetId) != simdjson::SUCCESS || tickField.error())
  {
    if (_logger)
    {
      _logger->warn("[Polymarket] tick_size_change without asset_id or new_tick_size");
    }
    return;
  }
  const std::string tokenId(assetId);
  // Only a market this connector subscribed to: a tick message does not
  // register one.
  if (std::find(_config.tokenIds.begin(), _config.tokenIds.end(), tokenId) ==
      _config.tokenIds.end())
  {
    return;
  }

  // The text of the value, not its double: 0.001 must become exactly 100000
  // raw. The venue sends a string; a bare number reads the same way.
  std::string text = simdjson::minify(tickField.value_unsafe());
  if (!text.empty() && text.front() == '"')
  {
    text = text.substr(1, text.size() >= 2 ? text.size() - 2 : 0);
  }
  const auto tick = util::parsePrice(text);
  if (!tick || tick->raw() <= 0)
  {
    if (_logger)
    {
      _logger->warn("[Polymarket] tick_size_change for " + tokenId + ": new_tick_size " + text +
                    " is not a positive decimal");
    }
    return;
  }

  const auto held = _specs.find(tokenId);
  if (held && held->tickSize.raw() == tick->raw())
  {
    return;
  }
  InstrumentSpec spec = held.value_or(InstrumentSpec{});
  spec.tickSize = *tick;
  _specs.add(tokenId, spec);
  if (_logger)
  {
    _logger->info("[Polymarket] " + tokenId + " tick is now " + tick->toString());
  }
  if (_registry)
  {
    _tokenToSymbol[tokenId] = registerToken(tokenId);
  }
}

void PolymarketExchangeConnector::sendSubscribe(const std::vector<std::string>& tokenIds,
                                                const std::string& operation)
{
  if (!_wsMarket || tokenIds.empty())
  {
    return;
  }

  std::ostringstream json;
  json << R"({"assets_ids":[)";
  for (size_t i = 0; i < tokenIds.size(); ++i)
  {
    if (i > 0)
    {
      json << ",";
    }
    json << "\"" << tokenIds[i] << "\"";
  }
  json << R"(],"type":"market","operation":")" << operation << "\"}";

  _wsMarket->send(json.str());

  if (_logger)
  {
    _logger->info("[Polymarket] Subscribed to " + std::to_string(tokenIds.size()) + " tokens");
  }
}

void PolymarketExchangeConnector::processBookSnapshot(simdjson::ondemand::object obj,
                                                      uint64_t recvNs, UnixNanos exchangeTs)
{
  auto assetIdField = obj["asset_id"];
  if (assetIdField.error())
  {
    return;
  }

  std::string_view tokenId = assetIdField.get_string().value();
  SymbolId sym = resolveSymbolId(tokenId);
  markFeedActivity(sym, MonoNanos::fromRaw(recvNs));

  auto evOpt = _bookPool.acquire();
  if (!evOpt)
  {
    if (_logger)
    {
      _logger->warn("[Polymarket] Book pool exhausted");
    }
    return;
  }

  auto& ev = *evOpt;
  ev->recvNs = MonoNanos::fromRaw(recvNs);
  ev->sourceExchange = _exchangeId;
  ev->update.symbol = sym;
  // A "book" frame is the whole book. The type has to be written on every
  // event, not just the first: BookUpdateEvent::clear() resets the levels
  // only, so a pooled slot last used by a price_change still carries DELTA
  // and the consumer would merge this snapshot into the book it is meant to
  // replace.
  ev->update.type = BookUpdateType::SNAPSHOT;
  ev->update.bids.clear();
  ev->update.asks.clear();

  // Parse bids
  if (auto bids = obj["bids"]; !bids.error())
  {
    for (auto level : bids.get_array())
    {
      auto lobj = level.get_object();
      std::optional<Price> priceOpt;
      std::optional<Quantity> sizeOpt;

      if (auto p = lobj["price"]; !p.error())
      {
        priceOpt = util::parsePrice(p.get_string().value());
      }
      if (auto s = lobj["size"]; !s.error())
      {
        sizeOpt = util::parseQty(s.get_string().value());
      }

      if (priceOpt && sizeOpt && priceOpt->raw() > 0 && sizeOpt->raw() > 0)
      {
        ev->update.bids.push_back({*priceOpt, *sizeOpt});
      }
    }
  }

  // Parse asks
  if (auto asks = obj["asks"]; !asks.error())
  {
    for (auto level : asks.get_array())
    {
      auto lobj = level.get_object();
      std::optional<Price> priceOpt;
      std::optional<Quantity> sizeOpt;

      if (auto p = lobj["price"]; !p.error())
      {
        priceOpt = util::parsePrice(p.get_string().value());
      }
      if (auto s = lobj["size"]; !s.error())
      {
        sizeOpt = util::parseQty(s.get_string().value());
      }

      if (priceOpt && sizeOpt && priceOpt->raw() > 0 && sizeOpt->raw() > 0)
      {
        ev->update.asks.push_back({*priceOpt, *sizeOpt});
      }
    }
  }

  ev->update.exchangeTsNs = exchangeTs;
  ev->publishTsNs = nowMonoNanos();

  _bookUpdateBus->publish(std::move(ev));
}

// Incremental book update. Each element of price_changes carries its own
// asset_id, a price level, its new absolute size (0 = level removed), and a
// side (BUY -> bid, SELL -> ask). A single message may touch several assets;
// changes are grouped per symbol into one DELTA BookUpdateEvent each so the
// downstream book applies them atomically.
void PolymarketExchangeConnector::processPriceChanges(simdjson::ondemand::object obj,
                                                      uint64_t recvNs, UnixNanos exchangeTs)
{
  auto pcField = obj["price_changes"];
  if (pcField.error())
  {
    return;
  }

  struct Change
  {
    SymbolId sym;
    bool isBid;
    Price price;
    Quantity qty;
  };
  std::vector<Change> changes;
  for (auto el : pcField.get_array())
  {
    auto e = el.get_object();

    // Field order per element: asset_id, price, size, side (read forward).
    std::string_view tokenId;
    if (auto a = e["asset_id"]; !a.error())
    {
      tokenId = a.get_string().value();
    }
    else
    {
      continue;
    }

    std::optional<Price> priceOpt;
    if (auto p = e["price"]; !p.error())
    {
      priceOpt = util::parsePrice(p.get_string().value());
    }
    std::optional<Quantity> qtyOpt;  // size 0 is a valid level removal
    if (auto s = e["size"]; !s.error())
    {
      qtyOpt = util::parseQty(s.get_string().value());
    }
    bool isBid = true;
    if (auto sd = e["side"]; !sd.error())
    {
      isBid = (sd.get_string().value() == "BUY");
    }

    if (!priceOpt || !qtyOpt)
    {
      continue;  // unparseable level; skip rather than corrupt the delta
    }
    const SymbolId sym = resolveSymbolId(tokenId);
    markFeedActivity(sym, MonoNanos::fromRaw(recvNs));
    changes.push_back({sym, isBid, *priceOpt, *qtyOpt});
  }

  if (changes.empty())
  {
    return;
  }

  // Emit one DELTA event per distinct symbol touched by this message.
  std::vector<SymbolId> seen;
  for (const auto& c : changes)
  {
    if (std::find(seen.begin(), seen.end(), c.sym) != seen.end())
    {
      continue;
    }
    seen.push_back(c.sym);

    auto evOpt = _bookPool.acquire();
    if (!evOpt)
    {
      if (_logger)
      {
        _logger->warn("[Polymarket] Book pool exhausted (price_change)");
      }
      return;
    }
    auto& ev = *evOpt;
    ev->recvNs = MonoNanos::fromRaw(recvNs);
    ev->sourceExchange = _exchangeId;
    ev->update.symbol = c.sym;
    ev->update.type = BookUpdateType::DELTA;
    ev->update.bids.clear();
    ev->update.asks.clear();

    for (const auto& d : changes)
    {
      if (d.sym != c.sym)
      {
        continue;
      }
      // size 0 -> level removed; forwarded as a zero-quantity level, which the
      // aggregate book erases.
      const BookLevel lvl{d.price, d.qty};
      if (d.isBid)
      {
        ev->update.bids.push_back(lvl);
      }
      else
      {
        ev->update.asks.push_back(lvl);
      }
    }

    ev->update.exchangeTsNs = exchangeTs;
    ev->publishTsNs = nowMonoNanos();
    _bookUpdateBus->publish(std::move(ev));
  }
}

}  // namespace lrvx
