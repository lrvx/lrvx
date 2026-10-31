/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "lrvx-connectors/bybit/bybit_exchange_connector.h"
#include "lrvx-connectors/net/ix_websocket_client.h"
#include "lrvx-connectors/util/safe_parse.h"
#include "lrvx/engine/symbol_registry.h"
#include "lrvx/util/concurrency/thread_body.h"

#include <lrvx/log/atomic_logger.h>
#include <lrvx/log/log.h>
#include <lrvx/util/base/hash.h>

#include <openssl/hmac.h>
#include <cctype>
#include <chrono>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#include <simdjson.h>

namespace lrvx
{

static constexpr auto BYBIT_ORIGIN = "https://www.bybit.com";

namespace
{
// orderLinkId is Bybit's client order id: the engine's own OrderId, sent on
// submit and echoed back on every private frame for that order. Absent (or
// non-numeric) on orders this engine did not place, which is the only case
// the venue's orderId has to stand in for it.
template <typename Value>
std::optional<uint64_t> parseOrderLinkId(Value& d)
{
  auto field = d["orderLinkId"];
  if (field.error())
  {
    return std::nullopt;
  }
  auto sv = field.get_string();
  if (sv.error())
  {
    return std::nullopt;
  }
  return util::parseUint64(sv.value_unsafe());
}
}  // namespace

// "30AUG24" -> 2024-08-30T00:00:00Z. Digits and the three-letter month are
// matched against fixed ASCII tables, and the date is turned into an instant
// through std::chrono::sys_days, which is UTC by definition -- neither the
// host's locale nor its timezone takes part.
static std::optional<std::chrono::system_clock::time_point> parseOptionExpiryUtc(
    std::string_view token)
{
  static constexpr std::string_view kMonths[12] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                                   "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};

  // "30AUG24" and the single-digit-day form "1JAN25" are both what the venue
  // writes; everything after the day is a fixed five characters.
  if (token.size() != 7 && token.size() != 6)
  {
    return std::nullopt;
  }
  const std::size_t dayDigits = token.size() - 5;

  auto digitsToInt = [](std::string_view sv) -> std::optional<int>
  {
    int value = 0;
    for (char c : sv)
    {
      if (c < '0' || c > '9')
      {
        return std::nullopt;
      }
      value = value * 10 + (c - '0');
    }
    return value;
  };

  const auto day = digitsToInt(token.substr(0, dayDigits));
  const auto year = digitsToInt(token.substr(dayDigits + 3, 2));
  if (!day || !year)
  {
    return std::nullopt;
  }

  std::string monthToken(token.substr(dayDigits, 3));
  for (char& c : monthToken)
  {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }

  unsigned month = 0;
  for (unsigned i = 0; i < 12; ++i)
  {
    if (kMonths[i] == monthToken)
    {
      month = i + 1;
      break;
    }
  }
  if (month == 0)
  {
    return std::nullopt;
  }

  // Two-digit years on this venue are this century; the option chain carries
  // nothing older.
  const std::chrono::year_month_day ymd{std::chrono::year{2000 + *year}, std::chrono::month{month},
                                        std::chrono::day{static_cast<unsigned>(*day)}};
  if (!ymd.ok())
  {
    return std::nullopt;
  }

  return std::chrono::system_clock::time_point(
      std::chrono::duration_cast<std::chrono::system_clock::duration>(
          std::chrono::sys_days(ymd).time_since_epoch()));
}

std::optional<SymbolInfo> parseOptionSymbol(std::string_view fullSymbol,
                                            std::string_view exchange = "bybit")
{
  if (fullSymbol.ends_with("-USDT"))
  {
    // "-USDT" is 5 characters; stripping 6 ate the option-type letter
    // (BTC-30AUG24-50000-C-USDT -> BTC-30AUG24-50000-, typeStr empty ->
    // nullopt, instrument registered as Spot with no strike/expiry/type).
    fullSymbol.remove_suffix(5);
  }

  // Format: BTC-30AUG24-50000-C
  size_t dash1 = fullSymbol.find('-');
  size_t dash2 = fullSymbol.find('-', dash1 + 1);
  size_t dash3 = fullSymbol.find('-', dash2 + 1);

  if (dash1 == std::string_view::npos || dash2 == std::string_view::npos ||
      dash3 == std::string_view::npos)
  {
    return std::nullopt;
  }

  std::string underlying = std::string(fullSymbol.substr(0, dash1));
  std::string expiryStr =
      std::string(fullSymbol.substr(dash1 + 1, dash2 - dash1 - 1));  // e.g. 30AUG24
  std::string strikeStr =
      std::string(fullSymbol.substr(dash2 + 1, dash3 - dash2 - 1));  // e.g. 50000
  std::string typeStr = std::string(fullSymbol.substr(dash3 + 1));   // e.g. C or P

  // Date, e.g. "30AUG24". The venue writes it as a fixed ASCII protocol
  // token in UTC, so it is parsed as one: std::get_time("%b") matches month
  // names out of the global locale and fails outright under any locale that
  // does not abbreviate August as "AUG" (the symbol then registers as a Spot
  // instrument with no strike and no expiry), and std::mktime reads the
  // broken-down time as *local*, which moved the expiry by the host's UTC
  // offset -- up to a full calendar day.
  const auto parsedExpiry = parseOptionExpiryUtc(expiryStr);
  if (!parsedExpiry)
  {
    return std::nullopt;
  }
  const auto expiry_tp = *parsedExpiry;

  // Strike
  auto strikeOpt = util::safeParseDouble(strikeStr);
  if (!strikeOpt || *strikeOpt <= 0.0)
  {
    return std::nullopt;
  }
  double strike = *strikeOpt;

  OptionType optType;
  if (typeStr == "C")
  {
    optType = OptionType::CALL;
  }
  else if (typeStr == "P")
  {
    optType = OptionType::PUT;
  }
  else
  {
    return std::nullopt;
  }

  SymbolInfo info;
  info.exchange = std::string(exchange);
  info.symbol = std::string(fullSymbol);
  info.type = InstrumentType::Option;
  info.strike = Price::fromDouble(strike);
  // expiry_tp is a system_clock (wall/unix) time_point; TimePoint is
  // LrvxClock (steady_clock). A duration_cast between them silently
  // reinterprets a unix epoch as a steady-clock reading -- fromUnixMs is the
  // documented conversion (time.h) for exactly this crossing. The mapping it
  // reads is anchored by Engine::start(), before any connector runs; this
  // used to anchor it here in a call_once of its own because nothing on the
  // startup path did.
  info.expiry = fromUnixMs(
      std::chrono::duration_cast<std::chrono::milliseconds>(expiry_tp.time_since_epoch()).count());
  info.optionType = optType;

  return info;
}

static std::string makePrivateAuthPayload(std::string_view apiKey, std::string_view apiSecret,
                                          std::chrono::milliseconds ttl = std::chrono::seconds{15})
{
  using namespace std::chrono;

  const auto expires = duration_cast<milliseconds>(system_clock::now().time_since_epoch()) + ttl;
  const auto expiresMs = expires.count();
  const std::string toSign = "GET/realtime" + std::to_string(expiresMs);

  unsigned char hash[EVP_MAX_MD_SIZE];
  unsigned int hashLen = 0;
  HMAC(EVP_sha256(), apiSecret.data(), static_cast<int>(apiSecret.size()),
       reinterpret_cast<const unsigned char*>(toSign.data()), toSign.size(), hash, &hashLen);

  char hex[EVP_MAX_MD_SIZE * 2 + 1];
  for (unsigned i = 0; i < hashLen; ++i)
  {
    std::snprintf(hex + i * 2, 3, "%02x", hash[i]);
  }
  hex[hashLen * 2] = '\0';

  std::string payload;
  payload.reserve(128);
  payload.append(R"({"op":"auth","args":[")")
      .append(apiKey)
      .append("\",")
      .append(std::to_string(expiresMs))
      .append(",\"")
      .append(hex)
      .append("\"]}");

  return payload;
}

BybitExchangeConnector::BybitExchangeConnector(const BybitConfig& config,
                                               BookUpdateBus* bookUpdateBus, TradeBus* tradeBus,
                                               OrderExecutionBus* orderBus,
                                               SymbolRegistry* registry,
                                               std::shared_ptr<ILogger> logger)
    : _config(config),
      _bookUpdateBus(bookUpdateBus),
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
    _exchangeId = _registry->registerExchange("bybit");
  }

  _wsClient = makePublicClient();
}

std::unique_ptr<IWebSocketClient> BybitExchangeConnector::makePublicClient() const
{
  return std::make_unique<IxWebSocketClient>(_config.publicEndpoint, BYBIT_ORIGIN,
                                             _config.reconnectDelayMs, _logger.get(), 20);
}

void BybitExchangeConnector::start()
{
  if (!_config.isValid())
  {
    LRVX_LOG_ERROR("[Bybit] Invalid connector config");
    _logger->error("[Bybit] Invalid connector config");
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

        std::vector<std::string> topics;
        topics.reserve(_config.symbols.size() * 2);

        for (const auto& entry : _config.symbols)
        {
          topics.push_back("orderbook." + std::to_string(static_cast<int>(entry.depth)) + "." +
                           entry.name);
          topics.push_back("publicTrade." + entry.name);
        }

        // Send in batches
        for (size_t i = 0; i < topics.size(); i += BATCH_SIZE)
        {
          std::string sub = R"({"op":"subscribe","args":[)";
          bool first = true;

          for (size_t j = i; j < std::min(i + BATCH_SIZE, topics.size()); ++j)
          {
            if (!first)
            {
              sub += ',';
            }
            first = false;
            sub += '"';
            sub += topics[j];
            sub += '"';
          }

          sub += "]}";

          LRVX_LOG("[Bybit] Sending subscription batch " << (i / BATCH_SIZE + 1) << ": " << sub);
          _wsClient->send(sub);
        }

        _logger->info("[Bybit] WebSocket connected, sent " +
                      std::to_string((topics.size() + BATCH_SIZE - 1) / BATCH_SIZE) +
                      " subscription batches");
      });

  _wsClient->onMessage(
      [this](std::string_view payload)
      {
        try
        {
          handleMessage(payload);
        }
        catch (const std::exception& e)
        {
          LRVX_LOG_ERROR("[Bybit] Exception while handling message: " << e.what());
          _logger->error(std::string("[Bybit] Exception while handling message: ") + e.what());
        }
      });

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

  // Application-level ping thread (Bybit requires {"op":"ping"} every 20s)
  _pingThread = makeThread("conn.bybit.ping",
                           [this]()
                           {
                             // Wait for initial connection
                             for (int i = 0; i < 50 && _running.load(); ++i)
                             {
                               std::this_thread::sleep_for(std::chrono::milliseconds(100));
                             }

                             while (_running.load())
                             {
                               if (_wsClient)
                               {
                                 _wsClient->send(R"({"op":"ping"})");
                               }
                               for (int i = 0; i < 200 && _running.load(); ++i)
                               {
                                 std::this_thread::sleep_for(std::chrono::milliseconds(100));
                               }
                             }
                           });

  if (_config.enablePrivate)
  {
    _wsClientPrivate = std::make_unique<IxWebSocketClient>(
        _config.privateEndpoint, BYBIT_ORIGIN, _config.reconnectDelayMs, _logger.get(), 20);

    _wsClientPrivate->onOpen(
        [this]()
        {
          auto auth = makePrivateAuthPayload(_config.apiKey, _config.apiSecret);
          _wsClientPrivate->send(auth);
        });

    _wsClientPrivate->onMessage(
        [this](std::string_view payload)
        {
          handlePrivateMessage(payload);
        });

    _wsClientPrivate->onClose(
        [this](int code, std::string_view reason)
        {
          // The private stream carries order and execution reports: losing it
          // stops fills reaching the engine, so it is the same class of event
          // as losing the public book.
          handleDisconnect(code, std::string("private stream: ").append(reason));
        });

    _wsClientPrivate->start();
  }
}

void BybitExchangeConnector::stop()
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

void BybitExchangeConnector::handleDisconnect(int code, std::string_view reason)
{
  const std::string detail = "code=" + std::to_string(code) + ", reason=" + std::string(reason);
  LRVX_LOG("[Bybit] WebSocket closed: " << detail);
  if (_logger)
  {
    _logger->info("[Bybit] WebSocket closed: " + detail);
  }
  emitDisconnect(detail);
}

void BybitExchangeConnector::pollFeedHealth(MonoNanos now)
{
  checkStaleFeeds(now, _config.staleDataTimeoutMs);
}

void BybitExchangeConnector::resubscribeBook(std::string_view symbolName)
{
  if (!_wsClient)
  {
    return;  // stop() already reset the socket: nothing left to resubscribe on
  }
  for (const auto& entry : _config.symbols)
  {
    if (entry.name == symbolName)
    {
      const std::string topic =
          "orderbook." + std::to_string(static_cast<int>(entry.depth)) + "." + entry.name;
      const bool unsubOk = _wsClient->send(R"({"op":"unsubscribe","args":[")" + topic + R"("]})");
      const bool subOk = _wsClient->send(R"({"op":"subscribe","args":[")" + topic + R"("]})");
      if (!unsubOk || !subOk)
      {
        // The resync latch (seqState.resyncInFlight) was already set by the
        // caller; without a subscribe frame reaching the exchange no snapshot
        // will ever come back to clear it, and every subsequent delta for
        // this symbol is dropped silently until the next full reconnect
        // (onOpen re-subscribes every topic). Log it so that window is
        // observable instead of looking like a quiet market.
        _logger->error("[Bybit] resubscribe send failed for " + std::string(symbolName) +
                       " -- book stays frozen until the next reconnect");
      }
      return;
    }
  }
}

void BybitExchangeConnector::handleMessage(std::string_view payload)
{
  static thread_local simdjson::ondemand::parser parser;
  const uint64_t recvNs = nowNsMonotonic();

  try
  {
    std::string json(payload);
    auto doc = parser.iterate(json);
    auto root = doc.get_object();

    auto topic_field = root.find_field_unordered("topic");
    if (topic_field.error())
    {
      // Check for subscription confirmation/error
      auto op_field = root.find_field_unordered("op");
      if (!op_field.error())
      {
        auto op = op_field.get_string().value();
        if (op == "ping")
        {
          _wsClient->send(R"({"op":"pong"})");
          return;
        }
        if (op == "subscribe")
        {
          auto success_field = root.find_field_unordered("success");
          if (!success_field.error() && !success_field.get_bool().value())
          {
            auto msg_field = root.find_field_unordered("ret_msg");
            std::string reason =
                msg_field.error() ? "unknown" : std::string(msg_field.get_string().value());
            _logger->error("[Bybit] Subscription failed: " + reason);
          }
        }
      }
      return;
    }

    // `data` is fetched LAST in each branch, after the root scalar fields, so
    // every simdjson ondemand access is forward-only (grabbing `data` -- the
    // last field -- early and then reading earlier root fields throws
    // OUT_OF_ORDER_ITERATION).
    auto topic = topic_field.get_string().value();

    if (topic.starts_with("orderbook."))
    {
      auto evOpt = _bookPool.acquire();
      if (!evOpt)
      {
        _logger->error(
            "[Bybit] Book pool exhausted, dropping orderbook update. Increase "
            "LRVX_DEFAULT_CONNECTOR_POOL_CAPACITY or ensure EventBus consumers are draining fast "
            "enough.");
        return;
      }
      auto& ev = *evOpt;
      ev->recvNs = MonoNanos::fromRaw(recvNs);
      ev->sourceExchange = _exchangeId;

      BookUpdateType updateType = BookUpdateType::SNAPSHOT;
      auto utv = root.find_field_unordered("type").get_string().value();
      if (utv == "delta")
      {
        updateType = BookUpdateType::DELTA;
      }

      ev->update.type = updateType;

      // Root scalars first (document order: topic, type, ts, cts, data), then
      // descend into data -- keeps every access forward-only.
      auto tsv = root.find_field_unordered("ts").get_int64().value();
      auto ctsv = root.find_field_unordered("cts").get_int64().value();
      ev->update.systemTsNs = msToUnixNs(tsv);
      ev->update.exchangeTsNs = msToUnixNs(ctsv);

      auto data_field = root.find_field_unordered("data");
      if (data_field.error())
      {
        return;
      }
      auto data_obj = data_field.get_object();

      // symbol
      std::string_view ssv = data_obj.find_field_unordered("s").get_string().value();
      SymbolId sym = resolveSymbolId(ssv);
      ev->update.symbol = sym;
      markFeedActivity(sym, MonoNanos::fromRaw(recvNs));

      if (_registry)
      {
        if (const auto info = _registry->getSymbolInfo(sym))
        {
          ev->update.instrument = info->type;
          ev->update.strike = info->strike;
          ev->update.expiry = info->expiry;
          ev->update.optionType = info->optionType;
        }
      }

      ev->update.bids.clear();
      ev->update.asks.clear();

      auto bf = data_obj.find_field_unordered("b");
      for (auto lvl : bf.get_array().value())
      {
        auto arr = lvl.get_array();
        auto it = arr.begin();
        std::string_view psv = (*it).get_string().value();
        ++it;
        std::string_view qsv = (*it).get_string().value();

        auto priceOpt = util::parsePrice(psv);
        auto qtyOpt = util::parseQty(qsv);
        if (!priceOpt || !qtyOpt)
        {
          _logger->warn("[Bybit] Invalid bid price/qty in book update");
          continue;
        }
        ev->update.bids.emplace_back(*priceOpt, *qtyOpt);
      }

      auto af = data_obj.find_field_unordered("a");
      for (auto lvl : af.get_array().value())
      {
        auto arr = lvl.get_array();
        auto it = arr.begin();
        std::string_view psv = (*it).get_string().value();
        ++it;
        std::string_view qsv = (*it).get_string().value();

        auto priceOpt = util::parsePrice(psv);
        auto qtyOpt = util::parseQty(qsv);
        if (!priceOpt || !qtyOpt)
        {
          _logger->warn("[Bybit] Invalid ask price/qty in book update");
          continue;
        }
        ev->update.asks.emplace_back(*priceOpt, *qtyOpt);
      }

      // Continuity check (read after b/a to keep simdjson field access forward-
      // only; data field order is s,b,a,u,seq). Bybit v5 update id ("u")
      // increments by 1 per topic message -- a jump means a dropped frame, so
      // applying this delta would silently corrupt the local book.
      const int64_t updateId = data_obj.find_field_unordered("u").get_int64().value();
      auto& seqState = _bookSeq[sym];
      int64_t prev = 0;
      if (updateType == BookUpdateType::SNAPSHOT)
      {
        seqState.lastUpdateId = updateId;
        seqState.resyncInFlight = false;  // fresh baseline; any resync is complete
      }
      else
      {
        if (seqState.resyncInFlight)
        {
          return;  // deltas racing our re-subscribe; wait for the snapshot
        }
        if (seqState.lastUpdateId < 0 || updateId != seqState.lastUpdateId + 1)
        {
          // Gap: never apply onto a stale book. Drop, invalidate, and force a
          // fresh snapshot by re-subscribing the topic. Detecting it is only
          // half the job -- a supervisor that cannot see the hole keeps
          // trading off a book that has quietly stopped updating, so the gap
          // goes out on the framework's health channel as well. With no
          // baseline yet there is no id to have expected: 0 says so.
          const uint64_t expected =
              seqState.lastUpdateId < 0 ? 0u : static_cast<uint64_t>(seqState.lastUpdateId + 1);
          _bookGapCount.fetch_add(1, std::memory_order_relaxed);
          _logger->warn("[Bybit] book gap on " + std::string(ssv) +
                        ": expected u=" + std::to_string(expected) +
                        " got u=" + std::to_string(updateId) + " -- resyncing");
          seqState.lastUpdateId = -1;
          seqState.resyncInFlight = true;
          resubscribeBook(ssv);
          emitSequenceGap(expected, static_cast<uint64_t>(updateId));
          return;
        }
        prev = seqState.lastUpdateId;
        seqState.lastUpdateId = updateId;
      }
      ev->seq = updateId;
      ev->prevSeq = prev;

      if (!ev->update.bids.empty() || !ev->update.asks.empty())
      {
        ev->publishTsNs = nowMonoNanos();
        auto [res, _] = _bookUpdateBus->tryPublish(std::move(ev));
        if (res != BookUpdateBus::PublishResult::SUCCESS)
        {
          _logger->error(
              "[Bybit] Book update dropped: EventBus full. Increase EventBus capacity or reduce "
              "number of subscriptions.");
        }
      }
    }
    else if (topic.starts_with("publicTrade."))
    {
      auto data_field = root.find_field_unordered("data");
      if (data_field.error())
      {
        return;
      }
      auto data_arr = data_field.get_array().value();
      for (auto t : data_arr)
      {
        auto obj = t.get_object();

        std::string_view ssv = obj.find_field_unordered("s").get_string().value();
        SymbolId sym = resolveSymbolId(ssv);
        markFeedActivity(sym, MonoNanos::fromRaw(recvNs));

        TradeEvent ev{};
        ev.recvNs = MonoNanos::fromRaw(recvNs);
        ev.trade.symbol = sym;

        if (_registry)
        {
          if (const auto info = _registry->getSymbolInfo(sym))
          {
            ev.trade.instrument = info->type;
          }
        }

        auto T = obj.find_field_unordered("T");
        ev.trade.exchangeTsNs = msToUnixNs(T.get_int64().value());

        ev.seq = obj.find_field_unordered("seq").get_int64().value();

        auto sv = obj.find_field_unordered("i").get_string().value();
        ev.trade_id = hash::fnv1a_64(sv.data(), sv.size());

        ev.trade.isBuy = (obj.find_field_unordered("S").get_string().value() == "Buy");

        auto priceOpt = util::parsePrice(obj.find_field_unordered("p").get_string().value());
        auto qtyOpt = util::parseQty(obj.find_field_unordered("v").get_string().value());
        if (!priceOpt || !qtyOpt)
        {
          _logger->warn("[Bybit] Invalid trade price/qty");
          continue;
        }
        ev.trade.price = *priceOpt;
        ev.trade.quantity = *qtyOpt;

        ev.publishTsNs = nowMonoNanos();
        auto [res, _] = _tradeBus->tryPublish(ev);
        if (res != TradeBus::PublishResult::SUCCESS)
        {
          _logger->error(
              "[Bybit] Trade dropped: EventBus full. Increase EventBus capacity or reduce number "
              "of subscriptions.");
        }
      }
    }
  }
  catch (const simdjson::simdjson_error& e)
  {
    _logger->warn(std::string("[Bybit] simdjson error: ") + e.what());
  }
}

bool BybitExchangeConnector::applyFillWatermark(OrderEvent& ev, Quantity cumulative)
{
  ev.fillQty = _reportedFill.advance(ev.order.id, cumulative);

  const bool terminal =
      (ev.status == OrderEventStatus::FILLED || ev.status == OrderEventStatus::CANCELED ||
       ev.status == OrderEventStatus::REJECTED || ev.status == OrderEventStatus::EXPIRED);
  if (terminal)
  {
    _reportedFill.complete(ev.order.id);
  }

  const bool isFill =
      (ev.status == OrderEventStatus::PARTIALLY_FILLED || ev.status == OrderEventStatus::FILLED);
  return !isFill || !ev.fillQty.isZero();
}

void BybitExchangeConnector::handlePrivateMessage(std::string_view payload)
{
  static thread_local simdjson::ondemand::parser parser;

  const uint64_t recvNs = nowNsMonotonic();

  try
  {
    std::string json(payload);
    auto doc = parser.iterate(json);

    if (auto opField = doc["op"]; !opField.error())
    {
      auto op = opField.get_string().value();

      if (op == "auth")
      {
        bool success = doc["success"].get_bool().value();
        if (success)
        {
          std::string sub = "{\"op\":\"subscribe\",\"args\":[\"order\",\"execution\"]}";
          _wsClientPrivate->send(sub);
        }
      }

      LRVX_LOG("[Bybit] service op=" << op);
      _logger->info(std::string("[Bybit] service op=") + std::string(op));
      return;
    }

    auto topicField = doc["topic"];
    if (topicField.error())
    {
      LRVX_LOG_ERROR("[Bybit] frame without topic, skip");
      _logger->warn("[Bybit] frame without topic, skip");
      return;
    }

    auto topic = topicField.get_string().value();
    auto data = doc["data"].value();

    if (topic == "order")
    {
      for (auto d : data.get_array().value())
      {
        OrderEvent ev{};
        ev.recvNs = MonoNanos::fromRaw(recvNs);

        std::string_view symbol = d["symbol"].get_string().value();
        ev.order.symbol = resolveSymbolId(symbol);

        auto orderIdOpt = util::parseUint64(d["orderId"].get_string().value());
        // Bybit echoes back the client id the executor sent as orderLinkId,
        // and that is the OrderId the engine issued -- the only id an
        // OrderTracker or a position book has ever heard of. The venue's own
        // orderId is an unrelated number, so an event published under it
        // refers to an order that does not exist on this side. orderLinkId
        // therefore wins; the venue id is the fallback for orders this engine
        // did not place (placed from the venue UI, or by another process).
        auto linkIdOpt = parseOrderLinkId(d);
        if (!linkIdOpt && !orderIdOpt)
        {
          _logger->warn("[Bybit] Invalid orderId in order event");
          continue;
        }
        ev.order.id = static_cast<OrderId>(linkIdOpt ? *linkIdOpt : *orderIdOpt);
        ev.order.side = (d["side"].get_string().value() == "Buy") ? Side::BUY : Side::SELL;

        auto priceOpt = util::parsePrice(d["price"].get_string().value());
        auto qtyOpt = util::parseQty(d["qty"].get_string().value());
        // avgPrice is the order's volume-weighted execution price so far and
        // the only price the order topic reports for a fill. Without it every
        // live fill reached PositionTracker at price 0 and built the cost
        // basis there. It reads "0" before anything has traded, which is not a
        // price but the venue saying it has none yet.
        //
        // The condition is read twice on purpose. The assignment guard states
        // the intent -- a parsed zero never becomes a fill price -- and is a
        // no-op on the bits today, because Price has no unset state and its
        // default and a parsed "0" are the same 64 bits. The flag is the
        // load-bearing half: it is what decides, below, whether this frame may
        // be published as a fill at all.
        bool avgPriceReported = false;
        if (auto avg = d["avgPrice"]; !avg.error())
        {
          if (auto avgOpt = util::parsePrice(avg.get_string().value()))
          {
            avgPriceReported = avgOpt->raw() > 0;
            if (avgOpt->raw() > 0)
            {
              ev.fillPrice = *avgOpt;
            }
          }
        }
        auto filledOpt = util::safeParseDouble(d["cumExecQty"].get_string().value());
        if (!priceOpt || !qtyOpt || !filledOpt)
        {
          _logger->warn("[Bybit] Invalid price/qty in order event");
          continue;
        }
        ev.order.price = *priceOpt;
        ev.order.quantity = *qtyOpt;
        ev.order.filledQuantity = Quantity::fromDouble(*filledOpt);

        ev.exchangeTsNs = msToUnixNs(d["updatedTime"].get_int64().value());
        ev.publishNs = nowMonoNanos();

        std::string_view status = d["orderStatus"].get_string().value();
        if (status == "New")
        {
          ev.status = OrderEventStatus::SUBMITTED;
        }
        else if (status == "PartiallyFilled")
        {
          ev.status = OrderEventStatus::PARTIALLY_FILLED;
        }
        else if (status == "Filled")
        {
          ev.status = OrderEventStatus::FILLED;
        }
        else if (status == "Cancelled")
        {
          ev.status = OrderEventStatus::CANCELED;
        }
        else if (status == "Rejected")
        {
          ev.status = OrderEventStatus::REJECTED;
        }
        else if (status == "Expired")
        {
          ev.status = OrderEventStatus::EXPIRED;
        }
        else
        {
          ev.status = OrderEventStatus::SUBMITTED;
        }

        // An increment the venue has not priced must not be published as a
        // fill. Price has no unset state, so a listener cannot tell "no price
        // reported" from "traded at zero" and builds the position's cost basis
        // at zero -- which is not a small error but a position whose entry
        // price is wrong by its whole value. The watermark is deliberately
        // left alone as well, so the increment is held rather than dropped:
        // the execution topic, which always carries execPrice, publishes it.
        // The order's own status and cumulative quantity still go out, demoted
        // to ACCEPTED so nothing moves a position.
        const bool isFillStatus = (ev.status == OrderEventStatus::PARTIALLY_FILLED ||
                                   ev.status == OrderEventStatus::FILLED);
        const bool advancesFill =
            ev.order.filledQuantity.raw() > _reportedFill.reported(ev.order.id).raw();
        if (isFillStatus && advancesFill && !avgPriceReported)
        {
          ev.status = OrderEventStatus::ACCEPTED;
          ev.fillQty = Quantity{};
          ev.publishNs = nowMonoNanos();
          _orderBus->publish(std::move(ev));
          continue;
        }

        // cumExecQty is cumulative, so the increment (and whether this frame
        // reports an execution the execution topic has already published) comes
        // from the watermark; see _reportedFill.
        if (!applyFillWatermark(ev, ev.order.filledQuantity))
        {
          continue;
        }

        ev.publishNs = nowMonoNanos();
        _orderBus->publish(std::move(ev));
      }
    }
    else if (topic == "execution")
    {
      for (auto d : data.get_array().value())
      {
        OrderEvent ev{};
        ev.recvNs = MonoNanos::fromRaw(recvNs);

        auto orderIdOpt = util::parseUint64(d["orderId"].get_string().value());
        // Same reasoning as the order topic: orderLinkId is the id the engine
        // issued, the venue's orderId only stands in for orders placed
        // elsewhere.
        auto linkIdOpt = parseOrderLinkId(d);
        if (!linkIdOpt && !orderIdOpt)
        {
          _logger->warn("[Bybit] Invalid orderId in execution event");
          continue;
        }
        ev.order.id = static_cast<OrderId>(linkIdOpt ? *linkIdOpt : *orderIdOpt);
        ev.order.symbol = resolveSymbolId(d["symbol"].get_string().value());
        ev.order.side = d["side"].get_string().value() == "Buy" ? Side::BUY : Side::SELL;

        auto priceOpt = util::parsePrice(d["execPrice"].get_string().value());
        auto qtyOpt = util::parseQty(d["execQty"].get_string().value());
        if (!priceOpt || !qtyOpt)
        {
          _logger->warn("[Bybit] Invalid price/qty in execution event");
          continue;
        }
        // execPrice is the price this execution traded at, which is exactly
        // what a position tracker needs for cost basis and realized PnL; it
        // used to be dropped, leaving every live fill at price 0. The order's
        // own limit price is a different number and the venue reports it
        // separately as orderPrice; fall back to execPrice only when the frame
        // omits it, which is what this handler did unconditionally before.
        ev.fillPrice = *priceOpt;
        ev.order.price = *priceOpt;
        if (auto op = d["orderPrice"]; !op.error())
        {
          if (auto orderPriceOpt = util::parsePrice(op.get_string().value()))
          {
            ev.order.price = *orderPriceOpt;
          }
        }

        // orderQty/leavesQty are the documented Bybit V5 execution-topic
        // fields for the order's real size and remaining size. execQty is this
        // single fill's size, not the order's, and writing it into
        // order.quantity/filledQuantity used to fabricate an order size equal
        // to whatever fraction happened to fill. When the two are absent,
        // order.quantity/filledQuantity are left unset (0) rather than
        // silently wrong, same as any other unparseable-but-optional field
        // here.
        bool haveCumulative = false;
        if (auto oq = d["orderQty"]; !oq.error())
        {
          if (auto orderQtyOpt = util::parseQty(oq.get_string().value()))
          {
            ev.order.quantity = *orderQtyOpt;
            if (auto lq = d["leavesQty"]; !lq.error())
            {
              if (auto leavesOpt = util::parseQty(lq.get_string().value()))
              {
                ev.order.filledQuantity =
                    (orderQtyOpt->raw() > leavesOpt->raw())
                        ? Quantity::fromRaw(orderQtyOpt->raw() - leavesOpt->raw())
                        : Quantity{};
                haveCumulative = true;
              }
            }
          }
        }

        if (auto et = d["execTime"]; !et.error())
        {
          ev.exchangeTsNs = msToUnixNs(et.get_int64().value());
        }
        else if (auto ts = d["ts"]; !ts.error())
        {
          ev.exchangeTsNs = msToUnixNs(ts.get_int64().value());
        }

        const bool isTrade = (d["execType"].get_string().value() == "Trade");
        const bool fullyFilled = isTrade && ev.order.filledQuantity.raw() > 0 &&
                                 ev.order.filledQuantity.raw() >= ev.order.quantity.raw();
        ev.status = !isTrade      ? OrderEventStatus::SUBMITTED
                    : fullyFilled ? OrderEventStatus::FILLED
                                  : OrderEventStatus::PARTIALLY_FILLED;

        // Same rule as the order topic: a fill with no usable price is held,
        // not published. execPrice is the execution topic's only price source
        // and the venue always sends it, so a zero here is a malformed frame
        // rather than "not priced yet" -- and there is no third channel to
        // hold the increment for, so it stays held until a well-formed frame
        // reports the same cumulative total.
        if (isTrade && priceOpt->raw() <= 0)
        {
          _logger->warn("[Bybit] Execution without a usable execPrice; not published as a fill");
          ev.status = OrderEventStatus::ACCEPTED;
          ev.fillQty = Quantity{};
          ev.publishNs = nowMonoNanos();
          _orderBus->publish(std::move(ev));
          continue;
        }

        // The watermark is cumulative, so an execution frame has to be
        // expressed as a cumulative total too. orderQty - leavesQty is that
        // total straight from the venue, and Bybit V5 sends both on every
        // execution frame. The fallback below covers a frame that arrives
        // without them: treating execQty as an increment on what has already
        // been published keeps a repeat of that same frame from booking twice,
        // but it cannot tell whether the order topic has already reported this
        // execution, so cross-topic de-duplication is only as good as those two
        // fields.
        const Quantity cumulative =
            haveCumulative
                ? ev.order.filledQuantity
                : Quantity::fromRaw(_reportedFill.reported(ev.order.id).raw() + qtyOpt->raw());
        if (!applyFillWatermark(ev, cumulative))
        {
          continue;
        }

        ev.publishNs = nowMonoNanos();
        _orderBus->publish(std::move(ev));
      }
    }
  }
  catch (const simdjson::simdjson_error& e)
  {
    LRVX_LOG_ERROR("[Bybit] simdjson private error: " << e.what());
    _logger->warn(std::string("[Bybit] simdjson private error: ") + e.what());
  }
}

SymbolId BybitExchangeConnector::resolveSymbolId(std::string_view symbol)
{
  auto existing = _registry->getSymbolId("bybit", std::string(symbol));
  if (existing)
  {
    return *existing;
  }

  SymbolInfo info;
  info.exchange = "bybit";
  info.symbol = std::string(symbol);

  // Try to parse as option
  if (auto parsed = parseOptionSymbol(symbol, "bybit"))
  {
    return _registry->registerSymbol(*parsed);
  }

  auto it = std::find_if(_config.symbols.begin(), _config.symbols.end(),
                         [&](const BybitConfig::SymbolEntry& entry)
                         {
                           return entry.name == symbol;
                         });

  if (it != _config.symbols.end())
  {
    info.type = it->type;
  }
  else if (auto parsed = parseOptionSymbol(symbol))
  {
    return _registry->registerSymbol(*parsed);
  }
  else
  {
    info.type = InstrumentType::Spot;
  }

  return _registry->registerSymbol(info);
}

bool BybitConfig::isValid() const
{
  if (publicEndpoint.empty())
  {
    LRVX_LOG_ERROR("BybitConfig validation failed: public endpoint is empty");
    return false;
  }

  for (const auto& s : symbols)
  {
    if (s.name.empty())
    {
      LRVX_LOG_ERROR("BybitConfig validation failed: symbol name is empty");
      return false;
    }

    if (s.depth == BookDepth::Invalid)
    {
      LRVX_LOG_ERROR("BybitConfig validation failed: symbol " << s.name
                                                              << " has invalid BookDepth");
      return false;
    }

    switch (s.type)
    {
      case InstrumentType::Spot:
        if (s.depth != BookDepth::Top1 && s.depth != BookDepth::Top50 &&
            s.depth != BookDepth::Top200)
        {
          LRVX_LOG_ERROR("BybitConfig validation failed: symbol "
                         << s.name << " (Spot) has unsupported BookDepth: "
                         << static_cast<int>(s.depth) << ". Allowed: 1, 50, 200");
          return false;
        }
        break;

      case InstrumentType::Future:
        if (s.depth != BookDepth::Top1 && s.depth != BookDepth::Top50 &&
            s.depth != BookDepth::Top200 && s.depth != BookDepth::Top500)
        {
          LRVX_LOG_ERROR("BybitConfig validation failed: symbol "
                         << s.name << " (Future) has unsupported BookDepth: "
                         << static_cast<int>(s.depth) << ". Allowed: 1, 50, 200, 500");
          return false;
        }
        break;

      case InstrumentType::Option:
        if (s.depth != BookDepth::Top25 && s.depth != BookDepth::Top100)
        {
          LRVX_LOG_ERROR("BybitConfig validation failed: symbol "
                         << s.name << " (Option) has unsupported BookDepth: "
                         << static_cast<int>(s.depth) << ". Allowed: 25, 100");
          return false;
        }
        break;

      default:
        LRVX_LOG_ERROR("BybitConfig validation failed: symbol " << s.name
                                                                << " has unknown InstrumentType");
        return false;
    }
  }

  if (enablePrivate)
  {
    if (privateEndpoint.empty())
    {
      LRVX_LOG_ERROR("BybitConfig validation failed: private endpoint is empty");
      return false;
    }

    if (apiKey.empty() || apiSecret.empty())
    {
      LRVX_LOG_ERROR("BybitConfig validation failed: private API credentials missing");
      return false;
    }
  }

  return true;
}
}  // namespace lrvx
