/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "lrvx-connectors/bitget/bitget_exchange_connector.h"
#include "lrvx-connectors/net/ix_websocket_client.h"
#include "lrvx-connectors/util/safe_parse.h"
#include "lrvx/engine/symbol_registry.h"
#include "lrvx/util/concurrency/thread_body.h"

#include <lrvx/log/log.h>

#include <openssl/hmac.h>
#include <zlib.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <simdjson.h>

namespace lrvx
{
static constexpr auto BITGET_ORIGIN = "https://www.bitget.com";
static constexpr auto BITGET_USER_AGENT =
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/120.0.0.0 Safari/537.36";

namespace
{

static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64Encode(const unsigned char* data, size_t len)
{
  std::string out((len + 2) / 3 * 4, '=');
  size_t idx = 0;
  for (size_t i = 0; i < len; i += 3)
  {
    int val =
        (data[i] << 16) + ((i + 1 < len ? data[i + 1] : 0) << 8) + (i + 2 < len ? data[i + 2] : 0);
    out[idx++] = b64_table[(val >> 18) & 0x3F];
    out[idx++] = b64_table[(val >> 12) & 0x3F];
    out[idx++] = b64_table[(val >> 6) & 0x3F];
    out[idx++] = b64_table[val & 0x3F];
  }
  size_t mod = len % 3;
  if (mod)
  {
    out[out.size() - (3 - mod)] = '=';
  }
  return out;
}

static std::string makeLoginPayload(std::string_view apiKey, std::string_view apiSecret,
                                    std::string_view passphrase)
{
  using namespace std::chrono;
  auto ts = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
  std::string toSign = std::to_string(ts) + "GET/user/verify";

  unsigned char hash[EVP_MAX_MD_SIZE];
  unsigned int len = 0;
  HMAC(EVP_sha256(), apiSecret.data(), static_cast<int>(apiSecret.size()),
       reinterpret_cast<const unsigned char*>(toSign.data()), toSign.size(), hash, &len);

  std::string sign = base64Encode(hash, len);

  std::string payload;
  payload.reserve(256);
  payload.append(R"({"op":"login","args":[{)");
  payload.append("\"apiKey\":\"").append(apiKey).append("\",");
  payload.append("\"passphrase\":\"").append(passphrase).append("\",");
  payload.append("\"timestamp\":\"").append(std::to_string(ts)).append("\",");
  payload.append("\"sign\":\"").append(sign).append("\"}]}");

  return payload;
}

static std::string_view bitgetBookChannel(BitgetConfig::BookDepth depth)
{
  switch (depth)
  {
    case BitgetConfig::BookDepth::Depth1:
      return "books1";
    case BitgetConfig::BookDepth::Depth5:
      return "books5";
    case BitgetConfig::BookDepth::Depth15:
      return "books15";
    case BitgetConfig::BookDepth::DepthFull:
    default:
      return "books";
  }
}

// Bitget's books-channel checksum, as the venue documents it: take up to the
// first 25 levels of each side, interleave them
// (bid0:ask0:bid1:ask1:...), join price and amount with ':', and CRC32 the
// result. Whichever side runs out is simply skipped, the other keeps going.
// The venue's own strings go in verbatim -- "0.5000" is not "0.5" -- which is
// why the raw JSON text is carried down here instead of the parsed
// fixed-point values.
static constexpr size_t BITGET_CHECKSUM_LEVELS = 25;

using RawLevel = std::pair<std::string_view, std::string_view>;

static uint32_t bitgetBookChecksum(const std::vector<RawLevel>& bids,
                                   const std::vector<RawLevel>& asks)
{
  std::string joined;
  joined.reserve(BITGET_CHECKSUM_LEVELS * 2 * 24);

  const size_t levels = std::min(BITGET_CHECKSUM_LEVELS, std::max(bids.size(), asks.size()));
  for (size_t i = 0; i < levels; ++i)
  {
    for (const auto* side : {&bids, &asks})
    {
      if (i >= side->size())
      {
        continue;
      }
      if (!joined.empty())
      {
        joined += ':';
      }
      joined += (*side)[i].first;
      joined += ':';
      joined += (*side)[i].second;
    }
  }

  return static_cast<uint32_t>(::crc32(::crc32(0L, Z_NULL, 0),
                                       reinterpret_cast<const Bytef*>(joined.data()),
                                       static_cast<uInt>(joined.size())));
}

static std::string_view bitgetWsInstType(InstrumentType type)
{
  switch (type)
  {
    case InstrumentType::Spot:
      return "SPOT";
    case InstrumentType::Future:
      return "USDT-FUTURES";
    case InstrumentType::Inverse:
      return "COIN-FUTURES";
    case InstrumentType::Option:
      return "SUSDT-FUTURES";  // Bitget uses this for simulation
  }
  return "unknown";
}

}  // namespace

BitgetExchangeConnector::BitgetExchangeConnector(const BitgetConfig& cfg, BookUpdateBus* bookBus,
                                                 TradeBus* tradeBus, OrderExecutionBus* orderBus,
                                                 SymbolRegistry* registry,
                                                 std::shared_ptr<ILogger> logger)
    : _config(cfg),
      _bookUpdateBus(bookBus),
      _tradeBus(tradeBus),
      _orderBus(orderBus),
      _registry(registry),
      _logger(std::move(logger))
{
  // Registering here rather than waiting for someone else to do it: the
  // registry is idempotent, and an id resolved lazily on the first frame would
  // be InvalidExchangeId for whatever ran before it.
  if (_registry)
  {
    _exchangeId = _registry->registerExchange("bitget");
  }

  _wsClient = makePublicClient();
}

std::unique_ptr<IWebSocketClient> BitgetExchangeConnector::makePublicClient() const
{
  return std::make_unique<IxWebSocketClient>(_config.publicEndpoint, BITGET_ORIGIN,
                                             _config.reconnectDelayMs, _logger.get(), 0,
                                             BITGET_USER_AGENT);
}

void BitgetExchangeConnector::start()
{
  if (!_config.isValid())
  {
    _logger->error("[Bitget] Invalid config");
    return;
  }

  if (_running.exchange(true))
  {
    return;
  }

  // stop() releases the public client, so a start() after it builds a fresh
  // one.
  if (!_wsClient)
  {
    _wsClient = makePublicClient();
  }

  _wsClient->onOpen(
      [this]()
      {
        constexpr size_t BATCH_SIZE = 10;

        for (size_t batchStart = 0; batchStart < _config.symbols.size(); batchStart += BATCH_SIZE)
        {
          std::string sub;
          sub.reserve(64 + BATCH_SIZE * 200);
          sub += R"({"op":"subscribe","args":[)";

          bool first = true;
          size_t batchEnd = std::min(batchStart + BATCH_SIZE, _config.symbols.size());

          for (size_t i = batchStart; i < batchEnd; ++i)
          {
            const auto& s = _config.symbols[i];

            if (!first)
            {
              sub += ',';
            }
            first = false;

            sub += R"({"instType":")";
            sub += bitgetWsInstType(s.type);
            sub += R"(","channel":")";
            sub += bitgetBookChannel(s.depth);
            sub += R"(","instId":")";
            sub += s.name;
            sub += R"("})";

            sub += ",";

            sub += R"({"instType":")";
            sub += bitgetWsInstType(s.type);
            sub += R"(","channel":"trade","instId":")";
            sub += s.name;
            sub += R"("})";
          }
          sub += "]}";

          _logger->info(std::string("[Bitget] subscribe batch ") +
                        std::to_string(batchStart / BATCH_SIZE + 1) + ": " +
                        std::to_string(batchEnd - batchStart) + " symbols");

          _wsClient->send(sub);
        }
      });

  _wsClient->onMessage(
      [this](std::string_view payload)
      {
        handleMessage(payload);
      });

  // The public socket had no close handler at all, so a feed that went away
  // left nothing behind but silence.
  _wsClient->onClose(
      [this](int code, std::string_view reason)
      {
        handleDisconnect(code, reason);
      });

  // Baseline for the staleness check: without it a feed that never delivers a
  // single frame has no stamp to age out from, which is the loudest failure
  // of the two this check exists for.
  const MonoNanos startedAt = nowMonoNanos();
  for (const auto& entry : _config.symbols)
  {
    markFeedActivity(resolveSymbolId(entry.name), startedAt);
  }

  _wsClient->start();
  _pingThread = makeThread("conn.bitget.ping",
                           [this]
                           {
                             pingLoop();
                           });

  if (_config.enablePrivate)
  {
    _wsClientPrivate = std::make_unique<IxWebSocketClient>(_config.privateEndpoint, BITGET_ORIGIN,
                                                           _config.reconnectDelayMs, _logger.get(),
                                                           0, BITGET_USER_AGENT);

    _wsClientPrivate->onOpen(
        [this]()
        {
          _logger->info("[Bitget] Private WS connected, sending login");
          auto auth = makeLoginPayload(_config.apiKey, _config.apiSecret, _config.passphrase);
          _wsClientPrivate->send(auth);
        });

    _wsClientPrivate->onClose(
        [this](int code, std::string_view reason)
        {
          // The private stream carries order and execution reports: losing it
          // stops fills reaching the engine, so it is the same class of event
          // as losing the public book.
          handleDisconnect(code, std::string("private stream: ").append(reason));
        });

    _wsClientPrivate->onMessage(
        [this](std::string_view payload)
        {
          handlePrivateMessage(payload);
        });

    _wsClientPrivate->start();
  }
}

void BitgetExchangeConnector::stop()
{
  if (!_running.exchange(false))
  {
    return;
  }

  if (_pingThread.joinable())
  {
    _pingThread.join();
  }

  if (_wsClient)
  {
    _wsClient->stop();
    _wsClient.reset();
  }
  if (_wsClientPrivate)
  {
    _wsClientPrivate->stop();
    _wsClientPrivate.reset();
  }
}

void BitgetExchangeConnector::handleDisconnect(int code, std::string_view reason)
{
  const std::string detail = "code=" + std::to_string(code) + ", reason=" + std::string(reason);
  if (_logger)
  {
    _logger->info("[Bitget] WebSocket closed: " + detail);
  }
  emitDisconnect(detail);
}

void BitgetExchangeConnector::pollFeedHealth(MonoNanos now)
{
  checkStaleFeeds(now, _config.staleDataTimeoutMs);
}

// Re-subscribe one symbol's books topic so the venue re-sends a snapshot. Same
// recovery as Bybit's: an invalidated book can only be re-baselined by a full
// frame, and nothing but a fresh subscribe asks for one.
void BitgetExchangeConnector::resubscribeBook(std::string_view symbolName)
{
  if (!_wsClient)
  {
    return;  // stop() already reset the socket: nothing left to resubscribe on
  }

  for (const auto& entry : _config.symbols)
  {
    if (entry.name != symbolName)
    {
      continue;
    }

    std::string arg = R"([{"instType":")";
    arg += bitgetWsInstType(entry.type);
    arg += R"(","channel":")";
    arg += bitgetBookChannel(entry.depth);
    arg += R"(","instId":")";
    arg += entry.name;
    arg += R"("}])";

    const bool unsubOk = _wsClient->send(R"({"op":"unsubscribe","args":)" + arg + "}");
    const bool subOk = _wsClient->send(R"({"op":"subscribe","args":)" + arg + "}");
    if ((!unsubOk || !subOk) && _logger)
    {
      // Without a subscribe frame reaching the venue no snapshot comes back,
      // so this symbol's book stays suppressed until the next full reconnect
      // re-subscribes everything from onOpen. Log it so that window is
      // observable instead of looking like a quiet market.
      _logger->error("[Bitget] resubscribe send failed for " + std::string(symbolName) +
                     " -- book stays suppressed until the next reconnect");
    }
    return;
  }
}

bool BitgetExchangeConnector::verifyBookIntegrity(
    SymbolId symbol, std::string_view instId, BookUpdateType type, int64_t seq,
    std::optional<uint32_t> venueChecksum,
    const std::vector<std::pair<std::string_view, std::string_view>>& bids,
    const std::vector<std::pair<std::string_view, std::string_view>>& asks)
{
  auto& state = _bookSeq[symbol];

  if (type == BookUpdateType::SNAPSHOT)
  {
    // A snapshot is the whole book, so its checksum covers exactly the levels
    // in this frame and can be verified here. A delta's checksum covers the
    // merged book instead, which this connector does not maintain -- it
    // forwards frames -- so deltas are checked on "seq" alone rather than
    // failed against a book that was never built.
    if (venueChecksum)
    {
      const uint32_t computed = bitgetBookChecksum(bids, asks);
      if (computed != *venueChecksum)
      {
        _bookChecksumFailureCount.fetch_add(1, std::memory_order_relaxed);
        _logger->error("[Bitget] book checksum mismatch on " + std::string(instId) + ": computed " +
                       std::to_string(computed) + " got " + std::to_string(*venueChecksum) +
                       " -- dropping and resyncing");
        state.lastSeq = -1;
        state.invalid = true;
        resubscribeBook(instId);
        // The framework has one channel for "this book is no longer a valid
        // continuation"; a checksum failure means that as surely as a gap
        // does, so it rides the same event carrying the two CRCs.
        emitSequenceGap(computed, *venueChecksum);
        return false;
      }
    }

    state.lastSeq = seq;
    state.invalid = false;
    return true;
  }

  if (state.invalid)
  {
    // Already reported when the book was invalidated; deltas racing the
    // re-subscribe would apply onto a book known to be wrong.
    return false;
  }

  if (seq >= 0 && state.lastSeq >= 0 && seq != state.lastSeq + 1)
  {
    const uint64_t expected = static_cast<uint64_t>(state.lastSeq + 1);
    _bookGapCount.fetch_add(1, std::memory_order_relaxed);
    _logger->warn("[Bitget] book gap on " + std::string(instId) + ": expected seq=" +
                  std::to_string(expected) + " got seq=" + std::to_string(seq) + " -- resyncing");
    state.lastSeq = -1;
    state.invalid = true;
    resubscribeBook(instId);
    emitSequenceGap(expected, static_cast<uint64_t>(seq));
    return false;
  }

  if (seq >= 0)
  {
    state.lastSeq = seq;
  }
  return true;
}

void BitgetExchangeConnector::pingLoop()
{
  for (int i = 0; i < 50 && _running.load(); ++i)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  while (_running.load())
  {
    if (_wsClient)
    {
      _wsClient->send("ping");
    }
    if (_wsClientPrivate)
    {
      _wsClientPrivate->send("ping");
    }

    for (int i = 0; i < 250 && _running.load(); ++i)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
}

void BitgetExchangeConnector::handleMessage(std::string_view payload)
{
  if (payload == "pong")
  {
    return;
  }

  // Stamped before parsing, so it measures when the frame reached this
  // process. CompositeBookMatrix::checkStaleness skips any venue whose
  // lastUpdateNs is still zero, so a book event without it left a frozen
  // Bitget feed quotable forever.
  const uint64_t recvNs = nowNsMonotonic();

  static thread_local simdjson::dom::parser parser;
  const MonoNanos arrivedAt = nowMonoNanos();

  try
  {
    auto doc = parser.parse(payload);

    // Check if this is a data message
    auto actionEl = doc["action"];
    auto dataEl = doc["data"];
    if (actionEl.error() && dataEl.error())
    {
      // Check for subscription error response
      auto eventEl = doc["event"];
      if (!eventEl.error())
      {
        std::string_view event = eventEl.get_string().value();
        if (event == "error")
        {
          auto codeEl = doc["code"];
          auto msgEl = doc["msg"];
          std::string code = codeEl.error() ? "?" : std::string(codeEl.get_string().value());
          std::string msg = msgEl.error() ? "?" : std::string(msgEl.get_string().value());
          _logger->error("[Bitget] Subscription error: code=" + code + " msg=" + msg);
        }
      }
      return;
    }

    std::string_view action{};
    if (!actionEl.error())
    {
      action = actionEl.get_string().value();
    }

    auto arg = doc["arg"];
    if (arg.error())
    {
      return;
    }

    std::string_view channel = arg["channel"].get_string().value();
    std::string_view inst = arg["instId"].get_string().value();

    if (dataEl.error())
    {
      return;
    }
    auto data = dataEl.get_array();

    if (channel.substr(0, 5) == "books")
    {
      auto evOpt = _bookPool.acquire();
      if (!evOpt)
      {
        _logger->error(
            "[Bitget] Book pool exhausted, dropping orderbook update. Increase "
            "LRVX_DEFAULT_CONNECTOR_POOL_CAPACITY or ensure EventBus consumers are draining fast "
            "enough.");
        return;
      }
      auto& ev = *evOpt;
      SymbolId sid = resolveSymbolId(inst);
      ev->update.symbol = sid;
      ev->recvNs = MonoNanos::fromRaw(recvNs);
      ev->sourceExchange = _exchangeId;
      markFeedActivity(sid, arrivedAt);

      BookUpdateType updateType = BookUpdateType::SNAPSHOT;
      if (action == "update")
      {
        updateType = BookUpdateType::DELTA;
      }

      ev->update.type = updateType;

      if (_registry)
      {
        if (const auto info = _registry->getSymbolInfo(sid))
        {
          ev->update.instrument = info->type;
        }
      }

      // The venue's own price/size strings, kept alongside the parsed levels
      // because the checksum is defined over the text, not over the values.
      std::vector<RawLevel> rawBids;
      std::vector<RawLevel> rawAsks;
      int64_t seq = -1;
      std::optional<uint32_t> venueChecksum;

      for (auto d : data)
      {
        auto bidsEl = d["bids"];
        if (!bidsEl.error())
        {
          for (auto lvl : bidsEl.get_array())
          {
            auto row = lvl.get_array();
            auto it = row.begin();
            std::string_view p = (*it).get_string().value();
            ++it;
            std::string_view q = (*it).get_string().value();

            auto priceOpt = util::parsePrice(p);
            auto qtyOpt = util::parseQty(q);
            if (!priceOpt || !qtyOpt)
            {
              _logger->warn("[Bitget] Invalid bid price/qty in book update");
              continue;
            }
            rawBids.emplace_back(p, q);
            ev->update.bids.emplace_back(*priceOpt, *qtyOpt);
          }
        }
        auto asksEl = d["asks"];
        if (!asksEl.error())
        {
          for (auto lvl : asksEl.get_array())
          {
            auto row = lvl.get_array();
            auto it = row.begin();
            std::string_view p = (*it).get_string().value();
            ++it;
            std::string_view q = (*it).get_string().value();

            auto priceOpt = util::parsePrice(p);
            auto qtyOpt = util::parseQty(q);
            if (!priceOpt || !qtyOpt)
            {
              _logger->warn("[Bitget] Invalid ask price/qty in book update");
              continue;
            }
            rawAsks.emplace_back(p, q);
            ev->update.asks.emplace_back(*priceOpt, *qtyOpt);
          }
        }

        // Parse timestamp if available
        auto tsEl = d["ts"];
        if (!tsEl.error())
        {
          std::string_view tsStr = tsEl.get_string().value();
          auto tsOpt = util::parseInt64(tsStr);
          if (tsOpt)
          {
            ev->update.exchangeTsNs = UnixNanos::fromRaw(*tsOpt * 1'000'000);
          }
        }

        if (auto seqEl = d["seq"]; !seqEl.error())
        {
          if (int64_t v{}; seqEl.get(v) == simdjson::SUCCESS)
          {
            seq = v;
          }
          else if (std::string_view sv{}; seqEl.get(sv) == simdjson::SUCCESS)
          {
            if (auto parsed = util::parseInt64(sv))
            {
              seq = *parsed;
            }
          }
        }

        // The venue ships the checksum as a signed 32-bit integer; the
        // comparison below is on the unsigned CRC, so the sign is just a
        // reinterpretation of the same 32 bits. A checksum of 0 is the
        // venue saying it computed none for this push (the depth-limited
        // channels carry the field that way), not a CRC to verify against:
        // a real CRC32 of a ladder is 0 with probability 2^-32, and that
        // one push is merely unverified, never wrongly dropped.
        if (auto csEl = d["checksum"]; !csEl.error())
        {
          int64_t v{};
          if (csEl.get(v) != simdjson::SUCCESS)
          {
            v = 0;
            if (std::string_view sv{}; csEl.get(sv) == simdjson::SUCCESS)
            {
              if (auto parsed = util::parseInt64(sv))
              {
                v = *parsed;
              }
            }
          }
          if (v != 0)
          {
            venueChecksum = static_cast<uint32_t>(static_cast<int32_t>(v));
          }
        }
      }

      if (!verifyBookIntegrity(sid, inst, updateType, seq, venueChecksum, rawBids, rawAsks))
      {
        return;  // the book is not a valid continuation; never publish it
      }

      ev->seq = seq;

      if (!ev->update.bids.empty() || !ev->update.asks.empty())
      {
        ev->publishTsNs = nowMonoNanos();
        auto [res, _] = _bookUpdateBus->tryPublish(std::move(ev));
        if (res != BookUpdateBus::PublishResult::SUCCESS)
        {
          _logger->error(
              "[Bitget] Book update dropped: EventBus full. Increase EventBus capacity or reduce "
              "number of subscriptions.");
        }
      }
    }
    else if (channel == "trade")
    {
      for (auto val : data)
      {
        std::string_view priceSv = val["price"].get_string().value();
        std::string_view qtySv = val["size"].get_string().value();
        std::string_view sideSv = val["side"].get_string().value();

        auto priceOpt = util::parsePrice(priceSv);
        auto qtyOpt = util::parseQty(qtySv);
        if (!priceOpt || !qtyOpt)
        {
          _logger->warn("[Bitget] Invalid trade price/qty");
          continue;
        }

        TradeEvent ev;
        SymbolId sid = resolveSymbolId(inst);
        markFeedActivity(sid, arrivedAt);
        ev.trade.symbol = sid;
        ev.recvNs = MonoNanos::fromRaw(recvNs);
        if (_registry)
        {
          if (const auto info = _registry->getSymbolInfo(sid))
          {
            ev.trade.instrument = info->type;
          }
        }

        ev.trade.price = *priceOpt;
        ev.trade.quantity = *qtyOpt;
        ev.trade.isBuy = (sideSv == "buy" || sideSv == "Buy");

        // Parse timestamp
        auto tsEl = val["ts"];
        if (!tsEl.error())
        {
          std::string_view tsStr = tsEl.get_string().value();
          auto tsOpt = util::parseInt64(tsStr);
          if (tsOpt)
          {
            ev.trade.exchangeTsNs = UnixNanos::fromRaw(*tsOpt * 1'000'000);
          }
        }

        ev.publishTsNs = nowMonoNanos();
        auto [res, _] = _tradeBus->tryPublish(ev);
        if (res != TradeBus::PublishResult::SUCCESS)
        {
          _logger->error(
              "[Bitget] Trade dropped: EventBus full. Increase EventBus capacity or reduce number "
              "of subscriptions.");
        }
      }
    }
  }
  catch (const simdjson::simdjson_error& e)
  {
    LRVX_LOG_ERROR(std::string("[Bitget] JSON parse error: ") + e.what() +
                   ", payload=" + std::string(payload));
    _logger->warn(std::string("[Bitget] json error: ") + e.what());
  }
}

void BitgetExchangeConnector::subscribePrivateOrders()
{
  std::string sub;
  sub.reserve(128);
  sub.append(
      R"({"op":"subscribe","args":[{"instType":"USDT-FUTURES","channel":"orders","instId":"default"}]})");
  _wsClientPrivate->send(sub);
  _logger->info("[Bitget] Subscribed to private orders channel");
}

void BitgetExchangeConnector::handlePrivateMessage(std::string_view payload)
{
  if (payload == "pong")
  {
    return;
  }

  const uint64_t recvNs = nowNsMonotonic();

  static thread_local simdjson::ondemand::parser parser;
  try
  {
    std::string json(payload);
    auto doc = parser.iterate(json);

    auto eventField = doc["event"];
    if (!eventField.error())
    {
      auto ev = eventField.get_string().value();
      if (ev == "login")
      {
        _logger->info("[Bitget] Private WS authenticated");
        subscribePrivateOrders();
      }
      return;
    }

    doc.rewind();
    auto channelField = doc["arg"]["channel"];
    if (channelField.error())
    {
      return;
    }
    auto channel = channelField.get_string().value();
    auto data = doc["data"].get_array().value();
    if (channel == "orders")
    {
      for (auto d : data)
      {
        OrderEvent ev;
        ev.recvNs = MonoNanos::fromRaw(recvNs);
        ev.order.symbol = resolveSymbolId(d["instId"].get_string().value());

        auto clientOidField = d["clientOid"];
        std::string_view clientOid =
            clientOidField.error() ? "" : clientOidField.get_string().value();
        if (clientOid.empty())
        {
          auto orderIdOpt = util::parseUint64(d["orderId"].get_string().value());
          if (!orderIdOpt)
          {
            _logger->warn("[Bitget] Invalid orderId in order event");
            continue;
          }
          ev.order.id = static_cast<OrderId>(*orderIdOpt);
        }
        else
        {
          auto coidOpt = util::parseUint64(clientOid);
          if (!coidOpt)
          {
            _logger->warn("[Bitget] Invalid clientOid in order event");
            continue;
          }
          ev.order.id = static_cast<OrderId>(*coidOpt);
        }
        ev.order.side = d["side"].get_string().value() == "buy" ? Side::BUY : Side::SELL;

        auto priceOpt = util::parsePrice(d["price"].get_string().value());
        auto qtyOpt = util::parseQty(d["size"].get_string().value());
        if (!priceOpt || !qtyOpt)
        {
          _logger->warn("[Bitget] Invalid price/qty in order event");
          continue;
        }
        ev.order.price = *priceOpt;
        ev.order.quantity = *qtyOpt;

        // The three fields that make a Bitget push a usable fill and that this
        // handler used to ignore entirely, dispatching onOrderFilled(order, 0,
        // 0) and moving no position at all: fillPrice is where the latest fill
        // traded, baseVolume how much of it traded, accBaseVolume the order's
        // cumulative filled quantity. All three are absent (or empty) on a
        // push that reports no new execution, such as the first "live" one.
        bool fillPriceReported = false;
        if (auto fp = d["fillPrice"]; !fp.error())
        {
          if (auto fillPriceOpt = util::parsePrice(fp.get_string().value()))
          {
            fillPriceReported = fillPriceOpt->raw() > 0;
            ev.fillPrice = *fillPriceOpt;
          }
        }
        if (auto bv = d["baseVolume"]; !bv.error())
        {
          if (auto fillQtyOpt = util::parseQty(bv.get_string().value()))
          {
            ev.fillQty = *fillQtyOpt;
          }
        }
        bool haveCumulative = false;
        if (auto acc = d["accBaseVolume"]; !acc.error())
        {
          if (auto filledOpt = util::parseQty(acc.get_string().value()))
          {
            ev.order.filledQuantity = *filledOpt;
            haveCumulative = true;
          }
        }

        std::string_view status = d["status"].get_string().value();
        if (status == "filled")
        {
          ev.status = OrderEventStatus::FILLED;
        }
        else if (status == "partially_filled")
        {
          // Used to fall into the else below and reach the engine as
          // SUBMITTED, so a partial fill dispatched onOrderSubmitted and the
          // position never moved.
          ev.status = OrderEventStatus::PARTIALLY_FILLED;
        }
        else if (status == "canceled" || status == "cancelled")
        {
          ev.status = OrderEventStatus::CANCELED;
        }
        else
        {
          ev.status = OrderEventStatus::SUBMITTED;
        }

        const bool isFill = (ev.status == OrderEventStatus::FILLED ||
                             ev.status == OrderEventStatus::PARTIALLY_FILLED);

        // The same rule the Bybit order topic follows: an increment the venue
        // has not priced is held, not published as a fill. Price has no unset
        // state, so a zero fillPrice is indistinguishable from a fill that
        // traded at zero and a position tracker builds the cost basis there.
        // The watermark is left alone so the quantity is not lost -- the next
        // push that does carry a fillPrice reports the same accBaseVolume and
        // publishes the whole held increment. The order's status and
        // cumulative quantity still go out, demoted to ACCEPTED.
        const bool advancesFill = !haveCumulative || ev.order.filledQuantity.raw() >
                                                         _reportedFill.reported(ev.order.id).raw();
        if (isFill && advancesFill && !fillPriceReported)
        {
          ev.status = OrderEventStatus::ACCEPTED;
          ev.fillQty = Quantity{};
          ev.publishNs = nowMonoNanos();
          _orderBus->publish(std::move(ev));
          continue;
        }

        if (isFill && haveCumulative)
        {
          // A push that carries no new cumulative quantity is the venue
          // repeating itself, not a second execution.
          const Quantity newlyFilled = _reportedFill.advance(ev.order.id, ev.order.filledQuantity);
          if (newlyFilled.isZero())
          {
            continue;
          }
          // baseVolume is the venue's view of the execution it is reporting
          // right now; the increment is everything not yet published, which is
          // larger whenever an earlier push was held for want of a price.
          // Publishing the larger of the two carries a held quantity forward
          // instead of dropping it, and still lets either field stand in when
          // the other is missing.
          if (ev.fillQty.raw() < newlyFilled.raw())
          {
            ev.fillQty = newlyFilled;
          }
        }
        if (ev.status == OrderEventStatus::FILLED || ev.status == OrderEventStatus::CANCELED)
        {
          _reportedFill.complete(ev.order.id);
        }

        ev.publishNs = nowMonoNanos();
        _orderBus->publish(std::move(ev));
      }
    }
  }
  catch (const simdjson::simdjson_error& e)
  {
    LRVX_LOG_ERROR(std::string("[Bitget] priv json error: ") + e.what());
    _logger->warn(std::string("[Bitget] priv json error: ") + e.what());
  }
}

SymbolId BitgetExchangeConnector::resolveSymbolId(std::string_view sym)
{
  if (auto existing = _registry->getSymbolId("bitget", std::string(sym)))
  {
    return *existing;
  }

  SymbolInfo info;
  info.exchange = "bitget";
  info.symbol = std::string(sym);
  info.type = InstrumentType::Spot;

  for (const auto& s : _config.symbols)
  {
    if (s.name == sym)
    {
      info.type = s.type;
      break;
    }
  }
  return _registry->registerSymbol(info);
}

bool BitgetConfig::isValid() const
{
  if (publicEndpoint.empty())
  {
    return false;
  }
  if (enablePrivate &&
      (privateEndpoint.empty() || apiKey.empty() || apiSecret.empty() || passphrase.empty()))
  {
    return false;
  }
  for (const auto& s : symbols)
  {
    if (s.name.empty() || s.depth == BookDepth::Invalid)
    {
      return false;
    }
  }
  return true;
}

}  // namespace lrvx
