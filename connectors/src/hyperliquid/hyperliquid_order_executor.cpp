/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "lrvx-connectors/hyperliquid/hyperliquid_order_executor.h"
#include "lrvx-connectors/execution/order_tif.h"
#include "lrvx-connectors/hyperliquid/hl_signer.h"
#include "lrvx-connectors/net/curl_transport.h"
#include "lrvx-connectors/util/safe_parse.h"

#include <lrvx/execution/events/order_event.h>
#include <lrvx/log/log.h>

#include <simdjson.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace lrvx
{

template <typename Policies>
HyperliquidOrderExecutorT<Policies>::~HyperliquidOrderExecutorT()
{
  // A request deferred by the rate limit runs on the policy's sender thread
  // and posts through the transport, so that thread is joined first. The
  // transport's sender threads then run response callbacks that touch the
  // asset map and the timeout policy, all declared after it and so destroyed
  // before it would be. Stopping it next lets an in-flight response (a late
  // meta retry above all) finish against live members.
  _policies.rateLimit.stop();
  _transport.reset();
}

namespace
{

std::string tidy(double v, int prec)
{
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.*f", prec, v);
  std::string s(buf);
  // Only a fractional part has zeros to drop: at prec 0 "%.0f" of 1000 is
  // "1000", and stripping it would send a size or price a thousand times off.
  if (s.find('.') == std::string::npos)
  {
    return s;
  }
  while (!s.empty() && s.back() == '0')
  {
    s.pop_back();
  }
  if (!s.empty() && s.back() == '.')
  {
    s.pop_back();
  }
  return s;
}

std::string genCloid128()
{
  std::random_device rd;
  std::uniform_int_distribution<uint64_t> dist;
  uint64_t hi = dist(rd);
  uint64_t lo = dist(rd);

  std::ostringstream oss;
  oss << "0x" << std::hex << std::setfill('0') << std::setw(16) << hi << std::setw(16) << lo;
  return oss.str();
}

// Hyperliquid's tif token spelling for the "limit" order-type object. Alo =
// Add Liquidity Only, HL's post-only. There is no native FOK or
// good-till-date tif on this venue.
std::string_view hlTifToken(NormalizedTif tif)
{
  switch (tif)
  {
    case NormalizedTif::GTC:
      return "Gtc";
    case NormalizedTif::IOC:
      return "Ioc";
    case NormalizedTif::POST_ONLY:
      return "Alo";
    case NormalizedTif::FOK:
    case NormalizedTif::UNSUPPORTED:
      return "";
  }
  return "";
}

// Hyperliquid wire conventions this file relies on, from the exchange
// endpoint docs
// (https://hyperliquid.gitbook.io/hyperliquid-docs/for-developers/api/exchange-endpoint)
// and the official hyperliquid-python-sdk (utils/signing.py
// order_request_to_order_wire / order_type_to_wire, exchange.py):
//
//  - an order wire is {"a","b","p","s","r","t","c"} in that key order; the
//    action is signed as msgpack of the JSON as written, so the key order is
//    part of the signature and must match the SDK's.
//  - "t" is either {"limit":{"tif":"Alo"|"Ioc"|"Gtc"}} or
//    {"trigger":{"isMarket":bool,"triggerPx":"...","tpsl":"tp"|"sl"}}.
//  - the order action is {"type":"order","orders":[...],"grouping":g} with g
//    one of "na", "normalTpsl", "positionTpsl".
//  - each order's status in the response is {"resting":{"oid"}},
//    {"filled":{"totalSz","avgPx","oid"}} or {"error":"..."}; orders in a TP/SL
//    grouping may instead answer with a bare string such as
//    "waitingForTrigger" / "waitingForFill" (the SDK's response types), which
//    carries no oid.
//
// Precision (docs: for-developers/api/tick-and-lot-size): a size has at most
// szDecimals decimals (szDecimals comes from the meta response); a perp
// price has at most 5 significant figures and at most 6 - szDecimals
// decimals, except that an integer price is always accepted. HL rejects
// anything finer, so prices are rounded to that grid here (to nearest, the
// SDK's _slippage_price rounding) and sizes are truncated toward zero, never
// up, so a reduce-only close cannot grow past what was asked; an order
// whose size truncates to zero is rejected. SymbolInfo::tickSize is not used:
// HL has no fixed tick, the significant-figure rule above is the tick.
constexpr int kPerpMaxDecimals = 6;
constexpr int kUnknownPrecisionDecimals = 8;

// Market TP/SL on HL carries a 10% slippage tolerance (docs: trading/
// take-profit-and-stop-loss-orders-tp-sl). A market trigger still needs a
// limit price on the wire; the SDK's own TP/SL example passes one with room
// on the far side of the trigger, and this is that room.
constexpr double kTriggerMarketSlippage = 0.10;

double roundToDecimals(double v, int decimals)
{
  const double scale = std::pow(10.0, decimals);
  return std::round(v * scale) / scale;
}

// 5 significant figures, or an integer from 100000 up (HL accepts any
// integer price).
double roundPxSigFigs(double px)
{
  if (px >= 100000.0)
  {
    return std::round(px);
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.5g", px);
  return std::strtod(buf, nullptr);
}

std::string formatHlPx(double px, int szDecimals)
{
  const int maxDecimals =
      szDecimals >= 0 ? std::max(0, kPerpMaxDecimals - szDecimals) : kUnknownPrecisionDecimals;
  return tidy(roundToDecimals(roundPxSigFigs(px), maxDecimals), maxDecimals);
}

// Truncated on the fixed-point value itself, so no floating-point rounding
// can carry a size up to the next step.
std::string formatHlSz(Quantity qty, int szDecimals)
{
  constexpr int kScaleDecimals = 8;
  static_assert(Quantity::Scale == 100'000'000, "formatHlSz assumes 8 fixed-point decimals");
  const int decimals =
      std::clamp(szDecimals >= 0 ? szDecimals : kUnknownPrecisionDecimals, 0, kScaleDecimals);
  int64_t step = 1;
  for (int i = decimals; i < kScaleDecimals; ++i)
  {
    step *= 10;
  }
  const int64_t raw = std::max<int64_t>(qty.raw(), 0);
  const int64_t truncated = raw - raw % step;

  std::string out = std::to_string(truncated / Quantity::Scale);
  int64_t frac = truncated % Quantity::Scale;
  if (frac != 0)
  {
    std::string digits = std::to_string(frac);
    digits.insert(0, kScaleDecimals - digits.size(), '0');
    while (!digits.empty() && digits.back() == '0')
    {
      digits.pop_back();
    }
    out += "." + digits;
  }
  return out;
}

bool isTriggerType(OrderType t)
{
  return t == OrderType::STOP_MARKET || t == OrderType::STOP_LIMIT ||
         t == OrderType::TAKE_PROFIT_MARKET || t == OrderType::TAKE_PROFIT_LIMIT;
}

// The "t" object for a non-trigger order: HL's "limit" object, used both for
// resting limit orders and, per HL's own SDK convention, IOC-priced market
// orders. Empty when the order has no HL shape: trailing stops and icebergs
// have no server-side primitive on HL, and FOK / good-till-date have no tif.
// Such an order is rejected rather than sent as a plain resting Gtc limit,
// which would leave the caller believing an order existed on the venue that
// is not the one resting there.
std::optional<std::string_view> hlLimitTifOrReject(const Order& order)
{
  if (order.type == OrderType::TRAILING_STOP || order.type == OrderType::ICEBERG)
  {
    return std::nullopt;
  }

  if (order.type == OrderType::MARKET)
  {
    // HL has no distinct market-order primitive; the standard SDK
    // convention is an IOC limit at a slippage-adjusted price, which the
    // caller supplies via order.price (see marketPriceWithSlippage).
    return "Ioc";
  }

  const NormalizedTif tif = normalizeTif(order.timeInForce, order.flags.postOnly);
  const auto token = hlTifToken(tif);
  if (token.empty())
  {
    return std::nullopt;
  }
  return token;
}

struct OrderWire
{
  std::string wire;
  std::string error;  // non-empty: the order has no valid HL wire, reject it
};

OrderWire buildOrderWire(const Order& order, int asset, int szDecimals, const std::string& cloid)
{
  OrderWire out;
  std::string typeObj;
  Price limitPx = order.price;

  if (isTriggerType(order.type))
  {
    if (order.triggerPrice.raw() <= 0)
    {
      out.error = "trigger order without a trigger price";
      return out;
    }
    const bool isMarket =
        order.type == OrderType::STOP_MARKET || order.type == OrderType::TAKE_PROFIT_MARKET;
    const bool isStop = order.type == OrderType::STOP_MARKET || order.type == OrderType::STOP_LIMIT;
    if (isMarket)
    {
      // Always derived from the trigger, never taken from order.price: a
      // caller that sets price = trigger for a stop-market (which the
      // backtester ignores) would otherwise send a stop with no room to fill.
      limitPx = marketPriceWithSlippage(order.triggerPrice, order.side, kTriggerMarketSlippage);
    }
    else if (order.price.raw() <= 0)
    {
      out.error = "conditional limit order without a limit price";
      return out;
    }
    typeObj = std::string("{\"trigger\":{\"isMarket\":") + (isMarket ? "true" : "false") +
              ",\"triggerPx\":\"" + formatHlPx(order.triggerPrice.toDouble(), szDecimals) +
              "\",\"tpsl\":\"" + (isStop ? "sl" : "tp") + "\"}}";
  }
  else
  {
    auto tif = hlLimitTifOrReject(order);
    if (!tif)
    {
      out.error = "order type " + std::to_string(static_cast<int>(order.type)) +
                  " / time in force is not supported on Hyperliquid";
      return out;
    }
    if (order.price.raw() <= 0)
    {
      out.error = order.type == OrderType::MARKET
                      ? "market order without a price: Hyperliquid has no market primitive, "
                        "set order.price (marketPriceWithSlippage) for the IOC limit"
                      : "limit order without a price";
      return out;
    }
    typeObj = "{\"limit\":{\"tif\":\"" + std::string(*tif) + "\"}}";
  }

  if (limitPx.raw() <= 0)
  {
    out.error = "order price rounds to zero";
    return out;
  }

  const std::string sz = formatHlSz(order.quantity, szDecimals);
  if (sz.empty() || sz == "0")
  {
    out.error = "order size rounds to zero at szDecimals=" + std::to_string(szDecimals);
    return out;
  }

  out.wire.reserve(192);
  out.wire += "{\"a\":" + std::to_string(asset);
  out.wire += ",\"b\":" + std::string(order.side == Side::BUY ? "true" : "false");
  out.wire += ",\"p\":\"" + formatHlPx(limitPx.toDouble(), szDecimals) + "\"";
  out.wire += ",\"s\":\"" + sz + "\"";
  out.wire += ",\"r\":" + std::string(order.flags.reduceOnly ? "true" : "false");
  out.wire += ",\"t\":" + typeObj;
  out.wire += ",\"c\":\"" + cloid + "\"}";
  return out;
}

}  // namespace

Price marketPriceWithSlippage(Price reference, Side side, double slippage)
{
  if (reference.raw() <= 0 || !(slippage >= 0.0 && slippage < 1.0))
  {
    return Price{};
  }
  const double px =
      reference.toDouble() * (side == Side::BUY ? (1.0 + slippage) : (1.0 - slippage));
  return Price::fromDouble(roundPxSigFigs(px));
}

template <typename Policies>
HyperliquidOrderExecutorT<Policies>::HyperliquidOrderExecutorT(
    std::string url, std::string privateKey, SymbolRegistry* registry, OrderTracker* orderTracker,
    std::shared_ptr<ILogger> logger, std::string accountAddress,
    std::optional<std::string> vaultAddress, bool mainnet)
    : _url(std::move(url)),
      _privateKey(std::move(privateKey)),
      _registry(registry),
      _orderTracker(orderTracker),
      _logger(std::move(logger)),
      _transport(std::make_unique<CurlTransport>()),
      _accountAddress(std::move(accountAddress)),
      _vaultAddress(std::move(vaultAddress)),
      _mainnet(mainnet)
{
  loadAssetIds();
}

template <typename Policies>
HyperliquidOrderExecutorT<Policies>::HyperliquidOrderExecutorT(
    std::unique_ptr<ITransport> transport, std::string url, std::string privateKey,
    SymbolRegistry* registry, OrderTracker* orderTracker, std::shared_ptr<ILogger> logger,
    std::string accountAddress, std::optional<std::string> vaultAddress, bool mainnet)
    : _url(std::move(url)),
      _privateKey(std::move(privateKey)),
      _registry(registry),
      _orderTracker(orderTracker),
      _logger(std::move(logger)),
      _transport(std::move(transport)),
      _accountAddress(std::move(accountAddress)),
      _vaultAddress(std::move(vaultAddress)),
      _mainnet(mainnet)
{
  loadAssetIds();
}

template <typename Policies>
template <typename P, typename>
HyperliquidOrderExecutorT<Policies>::HyperliquidOrderExecutorT(
    std::string url, std::string privateKey, SymbolRegistry* registry, OrderTracker* orderTracker,
    std::shared_ptr<ILogger> logger, std::string accountAddress,
    std::optional<std::string> vaultAddress, bool mainnet, RateLimitConfig rateLimitConfig)
    : _url(std::move(url)),
      _privateKey(std::move(privateKey)),
      _registry(registry),
      _orderTracker(orderTracker),
      _logger(std::move(logger)),
      _transport(std::make_unique<CurlTransport>()),
      _accountAddress(std::move(accountAddress)),
      _vaultAddress(std::move(vaultAddress)),
      _mainnet(mainnet)
{
  _policies.rateLimit.init(std::move(rateLimitConfig));
  loadAssetIds();
}

template <typename Policies>
template <typename P, typename>
HyperliquidOrderExecutorT<Policies>::HyperliquidOrderExecutorT(
    std::string url, std::string privateKey, SymbolRegistry* registry, OrderTracker* orderTracker,
    std::shared_ptr<ILogger> logger, std::string accountAddress,
    std::optional<std::string> vaultAddress, bool mainnet, OrderTimeoutConfig timeoutConfig)
    : _url(std::move(url)),
      _privateKey(std::move(privateKey)),
      _registry(registry),
      _orderTracker(orderTracker),
      _logger(std::move(logger)),
      _transport(std::make_unique<CurlTransport>()),
      _accountAddress(std::move(accountAddress)),
      _vaultAddress(std::move(vaultAddress)),
      _mainnet(mainnet)
{
  _policies.timeout.init(std::move(timeoutConfig));
  _policies.timeout.start();
  loadAssetIds();
}

template <typename Policies>
template <typename P, typename>
HyperliquidOrderExecutorT<Policies>::HyperliquidOrderExecutorT(
    std::string url, std::string privateKey, SymbolRegistry* registry, OrderTracker* orderTracker,
    std::shared_ptr<ILogger> logger, std::string accountAddress,
    std::optional<std::string> vaultAddress, bool mainnet, RateLimitConfig rateLimitConfig,
    OrderTimeoutConfig timeoutConfig)
    : _url(std::move(url)),
      _privateKey(std::move(privateKey)),
      _registry(registry),
      _orderTracker(orderTracker),
      _logger(std::move(logger)),
      _transport(std::make_unique<CurlTransport>()),
      _accountAddress(std::move(accountAddress)),
      _vaultAddress(std::move(vaultAddress)),
      _mainnet(mainnet)
{
  _policies.rateLimit.init(std::move(rateLimitConfig));
  _policies.timeout.init(std::move(timeoutConfig));
  _policies.timeout.start();
  loadAssetIds();
}

// The /info endpoint that belongs to an /exchange URL: same host, so a
// testnet executor reads the testnet's asset indices. Asset indices differ
// between the two networks, and an order signed with the other network's
// index goes to the wrong market.
constexpr std::string_view kExchangeSuffix = "/exchange";
constexpr const char* kMainnetInfoUrl = "https://api.hyperliquid.xyz/info";

// The asset map has to be there before the first order is serialized. The
// constructor waits for it this long. A retry from the order path does not
// wait at all: it starts the request and refuses the action that found the
// map empty, and the response loads the map for the actions that follow.
constexpr auto kInitialMetaWait = std::chrono::milliseconds(15000);
constexpr auto kAssetRetryInterval = std::chrono::milliseconds(2000);

template <typename Policies>
void HyperliquidOrderExecutorT<Policies>::loadAssetIds()
{
  if (_infoUrl.empty())
  {
    if (_url.size() > kExchangeSuffix.size() && std::string_view(_url).ends_with(kExchangeSuffix))
    {
      _infoUrl = _url.substr(0, _url.size() - kExchangeSuffix.size()) + "/info";
    }
    else
    {
      _infoUrl = kMainnetInfoUrl;
      _logger->warn("[HL] rest url " + _url +
                    " does not end with /exchange; reading the asset map from " + _infoUrl);
    }
  }
  fetchAssetIds(kInitialMetaWait);
}

template <typename Policies>
bool HyperliquidOrderExecutorT<Policies>::assetsLoaded() const
{
  std::lock_guard lock(_assetMutex);
  return _assetsLoaded;
}

template <typename Policies>
bool HyperliquidOrderExecutorT<Policies>::ensureAssetIds()
{
  {
    std::lock_guard lock(_assetMutex);
    if (_assetsLoaded)
    {
      return true;
    }
    // Checked and claimed under one lock, so threads that all find the map
    // empty send one request between them, not one each.
    const auto now = std::chrono::steady_clock::now();
    if (_assetAttempted && now - _lastAssetAttempt < kAssetRetryInterval)
    {
      return false;
    }
    _assetAttempted = true;
    _lastAssetAttempt = now;
  }
  return fetchAssetIds(std::nullopt);
}

template <typename Policies>
bool HyperliquidOrderExecutorT<Policies>::fetchAssetIds(
    std::optional<std::chrono::milliseconds> wait)
{
  static constexpr char BODY[] = R"({"type":"meta"})";
  std::vector<std::pair<std::string_view, std::string_view>> hdr = {
      {"Content-Type", "application/json"}};

  // The transport answers on its own sender thread (or inline, for a fake),
  // hence the shared state and the bound: a venue that never answers must not
  // hold the caller for longer than `wait`, and without a `wait` the caller is
  // not held at all. The map is marked loaded only by a response that parsed
  // and carried at least one coin; anything else leaves it empty and a later
  // use retries.
  struct MetaFetch
  {
    std::mutex mutex;
    std::condition_variable cv;
    bool done{false};
    bool ok{false};

    void finish(bool success)
    {
      {
        std::lock_guard<std::mutex> lock(mutex);
        done = true;
        ok = success;
      }
      cv.notify_all();
    }
  };
  auto fetch = std::make_shared<MetaFetch>();

  _transport->post(
      _infoUrl, BODY, hdr,
      [this, fetch](std::string_view resp)
      {
        std::unordered_map<std::string, int> ids;
        std::unordered_map<std::string, int> szDecimals;

        simdjson::ondemand::parser p;
        simdjson::padded_string ps(resp);
        auto doc = p.iterate(ps);
        auto univ = doc["universe"].get_array();
        if (univ.error())
        {
          _logger->error("[HL] meta response has no universe; asset map not loaded, will retry");
          fetch->finish(false);
          return;
        }
        size_t idx = 0;
        for (auto c : univ.value())
        {
          auto name = c["name"].get_string();
          if (!name.error())
          {
            std::string coin(name.value_unsafe());
            ids[coin] = static_cast<int>(idx);
            auto szDec = c["szDecimals"].get_int64();
            if (!szDec.error())
            {
              szDecimals[coin] = static_cast<int>(szDec.value_unsafe());
            }
          }
          ++idx;
        }
        if (ids.empty())
        {
          _logger->error("[HL] meta response listed no coins; asset map not loaded, will retry");
          fetch->finish(false);
          return;
        }

        const size_t count = ids.size();
        {
          std::lock_guard lock(_assetMutex);
          _assetIds = std::move(ids);
          _szDecimals = std::move(szDecimals);
          _assetsLoaded = true;
        }
        _logger->info("[HL] asset map " + std::to_string(count));
        fetch->finish(true);
      },
      [this, fetch](std::string_view e)
      {
        _logger->error(std::string("[HL] meta fetch failed: ") + std::string(e) +
                       "; asset map not loaded, will retry");
        fetch->finish(false);
      });

  if (!wait)
  {
    return assetsLoaded();
  }

  std::unique_lock<std::mutex> lock(fetch->mutex);
  if (!fetch->cv.wait_for(lock, *wait,
                          [&fetch]
                          {
                            return fetch->done;
                          }))
  {
    _logger->error("[HL] meta fetch did not answer in time; asset map not loaded, will retry");
    return false;
  }
  return fetch->ok;
}

template <typename Policies>
int HyperliquidOrderExecutorT<Policies>::assetIdFor(std::string_view coin)
{
  {
    std::lock_guard lock(_assetMutex);
    if (_assetsLoaded)
    {
      auto it = _assetIds.find(std::string(coin));
      return it == _assetIds.end() ? -1 : it->second;
    }
  }
  if (!ensureAssetIds())
  {
    return -1;
  }
  std::lock_guard lock(_assetMutex);
  auto it = _assetIds.find(std::string(coin));
  return it == _assetIds.end() ? -1 : it->second;
}

template <typename Policies>
int HyperliquidOrderExecutorT<Policies>::szDecimalsFor(std::string_view coin)
{
  std::lock_guard lock(_assetMutex);
  auto it = _szDecimals.find(std::string(coin));
  return it == _szDecimals.end() ? -1 : it->second;
}

template <typename Policies>
std::string HyperliquidOrderExecutorT<Policies>::assetMissingReason(const std::string& coin) const
{
  return assetsLoaded() ? "assetId not cached for " + coin
                        : "asset map not loaded; no asset index for " + coin;
}

template <typename Policies>
void HyperliquidOrderExecutorT<Policies>::publishRejection(const Order& order,
                                                           const std::string& reason)
{
  LRVX_LOG_ERROR("[HL] " << reason << " (orderId=" << order.id << ")");
  if (!_orderBus)
  {
    return;
  }
  OrderEvent ev;
  ev.status = OrderEventStatus::REJECTED;
  ev.order = order;
  ev.rejectReason = reason;
  ev.publishNs = nowMonoNanos();
  _orderBus->publish(std::move(ev));
}

// A client-side rate-limit refusal never reached the venue and used to leave
// no trace at all: no transport call, no event, and for a cancel a tracker
// that goes on reporting the order live. Deliberately does not touch
// OrderTracker -- unlike publishRejection there was no submission attempt to
// mark rejected. Same shape as the Bitget executor's.
template <typename Policies>
void HyperliquidOrderExecutorT<Policies>::publishRateLimited(const Order& order)
{
  LRVX_LOG_WARN("[HL] client-side rate limit refused orderId=" << order.id);
  if (!_orderBus)
  {
    return;
  }
  OrderEvent ev;
  ev.status = OrderEventStatus::REJECTED_RATE_LIMIT;
  ev.order = order;
  ev.rejectReason = "client-side rate limit";
  ev.publishNs = nowMonoNanos();
  _orderBus->publish(std::move(ev));
}

template <typename Policies>
void HyperliquidOrderExecutorT<Policies>::publishSubmitted(const Order& order)
{
  if (!_orderBus)
  {
    return;
  }
  OrderEvent ev;
  ev.status = OrderEventStatus::SUBMITTED;
  ev.order = order;
  ev.publishNs = nowMonoNanos();
  _orderBus->publish(std::move(ev));
}

// A trigger order the venue accepted rests there untriggered. The backtester
// publishes SUBMITTED, ACCEPTED and then PENDING_TRIGGER for a conditional
// order; this connector does not publish ACCEPTED (HL's "resting" is the
// only acknowledgement and it goes out as SUBMITTED), so a trigger order is
// SUBMITTED followed by PENDING_TRIGGER: a listener that only knows
// onOrderSubmitted still sees the order live, and one that tells stops apart
// gets onOrderPendingTrigger as in a backtest.
template <typename Policies>
void HyperliquidOrderExecutorT<Policies>::publishPendingTrigger(const Order& order)
{
  if (!_orderBus)
  {
    return;
  }
  OrderEvent ev;
  ev.status = OrderEventStatus::PENDING_TRIGGER;
  ev.order = order;
  ev.publishNs = nowMonoNanos();
  _orderBus->publish(std::move(ev));
}

template <typename Policies>
void HyperliquidOrderExecutorT<Policies>::publishFill(const Order& order, Quantity fillQty,
                                                      Price fillPrice)
{
  // The same rule the Bybit and Bitget connectors follow: a fill the venue has
  // not priced is never published as a fill. Price has no unset state, so a
  // zero fillPrice is indistinguishable from a fill that traded at zero and a
  // position tracker builds the cost basis there. avgPx is a required field of
  // a "filled" status, so a missing one is a malformed response and is logged
  // as such; unlike Bybit and Bitget there is no later report to hold the
  // increment for, so the quantity still goes out -- on the order, not as a
  // position-moving fill.
  if (fillPrice.raw() <= 0)
  {
    _logger->error(
        "[HL] venue reported a fill with no usable avgPx; publishing the order state without a "
        "fill rather than booking a position at price 0");
    if (_orderBus)
    {
      OrderEvent held;
      held.status = OrderEventStatus::ACCEPTED;
      held.order = order;
      held.order.filledQuantity = fillQty;
      held.publishNs = nowMonoNanos();
      _orderBus->publish(std::move(held));
    }
    return;
  }

  if (!_orderBus)
  {
    return;
  }
  OrderEvent ev;
  // Hyperliquid has no private order stream in this connector, so the submit
  // response is the only place a fill is ever reported; the order is complete
  // when the venue says the whole size traded.
  ev.status = (fillQty.raw() >= order.quantity.raw()) ? OrderEventStatus::FILLED
                                                      : OrderEventStatus::PARTIALLY_FILLED;
  ev.order = order;
  ev.order.filledQuantity = fillQty;
  ev.fillQty = fillQty;
  ev.fillPrice = fillPrice;
  ev.publishNs = nowMonoNanos();
  _orderBus->publish(std::move(ev));
}

template <typename Policies>
void HyperliquidOrderExecutorT<Policies>::submitOrder(const Order& order)
{
  _policies.rateLimit.gate(
      order.id,
      [this, order]
      {
        sendSubmitOrder(order);
      },
      [this, order]
      {
        publishRateLimited(order);
      });
}

template <typename Policies>
void HyperliquidOrderExecutorT<Policies>::sendSubmitOrder(const Order& order)
{
  auto info = _registry->getSymbolInfo(order.symbol);
  if (!info)
  {
    publishRejection(order, "unknown symbol id");
    return;
  }
  int asset = assetIdFor(info->symbol);
  if (asset < 0)
  {
    publishRejection(order, assetMissingReason(info->symbol));
    return;
  }

  OrderLeg leg{order, {}, genCloid128()};
  auto wire = buildOrderWire(order, asset, szDecimalsFor(info->symbol), leg.cloid);
  if (!wire.error.empty())
  {
    publishRejection(order, wire.error);
    return;
  }
  leg.wire = std::move(wire.wire);

  std::vector<OrderLeg> legs;
  legs.push_back(std::move(leg));
  sendOrderAction(std::move(legs), "na");
}

template <typename Policies>
std::optional<std::string> HyperliquidOrderExecutorT<Policies>::signedBody(
    const std::string& actionJson, uint64_t nonceMs)
{
  // The vault address goes into both the signature and the body, or into
  // neither: the venue hashes the action, the nonce and the vault address
  // together to recover the signer, so a body and a signature that disagree
  // on the vault recover the wrong signer and the action is refused.
  const bool hasVault = _vaultAddress && !_vaultAddress->empty();

  hl::HlSignParams sp;
  sp.actionJson = actionJson;
  sp.nonceMs = static_cast<long long>(nonceMs);
  sp.isMainnet = _mainnet;
  sp.privateKeyHex = _privateKey;
  // activePoolJson is spliced into the signer request as a JSON value, so
  // the address goes in as a quoted string.
  sp.activePoolJson =
      hasVault ? std::optional<std::string>("\"" + *_vaultAddress + "\"") : std::nullopt;
  sp.expiresAfterMs = std::nullopt;

  auto sigOpt = hl::hl_sign_with_sdk(sp);
  if (!sigOpt)
  {
    return std::nullopt;
  }
  const auto& sig = *sigOpt;

  std::string body;
  body.reserve(actionJson.size() + 256);
  body += "{\"action\":" + actionJson;
  body += ",\"nonce\":" + std::to_string(nonceMs);
  if (hasVault)
  {
    body += ",\"vaultAddress\":\"" + *_vaultAddress + "\"";
  }
  body += ",\"signature\":{";
  body += "\"r\":\"" + sig.r + "\",";
  body += "\"s\":\"" + sig.s + "\",";
  body += "\"v\":" + std::to_string(sig.v);
  body += "}}";
  return body;
}

template <typename Policies>
void HyperliquidOrderExecutorT<Policies>::sendOrderAction(std::vector<OrderLeg> legs,
                                                          std::string_view grouping)
{
  std::string actionJson = "{\"type\":\"order\",\"orders\":[";
  for (size_t i = 0; i < legs.size(); ++i)
  {
    if (i)
    {
      actionJson += ",";
    }
    actionJson += legs[i].wire;
  }
  actionJson += "],\"grouping\":\"" + std::string(grouping) + "\"}";

  auto body = signedBody(actionJson, hl::nextNonceMs());
  if (!body)
  {
    for (const auto& leg : legs)
    {
      publishRejection(leg.order, "signing via SDK helper failed");
    }
    return;
  }

  // The signed body carries the signature; the log gets the action only.
  std::string localIds;
  for (const auto& leg : legs)
  {
    if (!localIds.empty())
    {
      localIds += ",";
    }
    localIds += std::to_string(leg.order.id);
  }
  _logger->info("[HL] order grouping=" + std::string(grouping) + " localIds=" + localIds +
                " action=" + actionJson);

  for (const auto& leg : legs)
  {
    _policies.timeout.trackSubmit(leg.order.id);
  }

  auto sharedLegs = std::make_shared<std::vector<OrderLeg>>(std::move(legs));

  _transport->post(
      _url, *body, {{"Content-Type", "application/json"}},
      [this, sharedLegs](std::string_view resp)
      {
        const auto& legs = *sharedLegs;
        for (const auto& leg : legs)
        {
          _policies.timeout.clearPending(leg.order.id);
        }

        // Hyperliquid answers HTTP 200 whatever happens and reports the
        // outcome in the body: a top-level "err", or one status object per
        // submitted order, in the order they were sent. Each status is read
        // in full, so a rejection is published as one and an inline fill
        // reaches the bus. dom rather than ondemand because these branches
        // read the same status object several times over.
        simdjson::dom::parser parser;
        simdjson::padded_string padded(resp);
        auto parsed = parser.parse(padded);
        if (parsed.error())
        {
          for (const auto& leg : legs)
          {
            publishRejection(leg.order, "Unparseable order response from venue");
          }
          return;
        }
        auto doc = parsed.value();

        auto asString = [](auto element) -> std::string_view
        {
          if (element.error())
          {
            return {};
          }
          auto sv = element.get_string();
          return sv.error() ? std::string_view{} : sv.value_unsafe();
        };

        if (asString(doc["status"]) == "err")
        {
          std::string_view reason = asString(doc["response"]);
          for (const auto& leg : legs)
          {
            publishRejection(leg.order, reason.empty() ? std::string("Venue rejected the order")
                                                       : std::string(reason));
          }
          return;
        }

        auto oidOf = [](auto element) -> std::string
        {
          auto oid = element["oid"].get_uint64();
          return oid.error() ? std::string{} : std::to_string(oid.value_unsafe());
        };

        auto statuses = doc["response"]["data"]["statuses"];
        for (size_t i = 0; i < legs.size(); ++i)
        {
          const Order& order = legs[i].order;
          const std::string& cloid = legs[i].cloid;
          const bool trigger = isTriggerType(order.type);

          auto st = statuses.at(i);
          if (st.error())
          {
            publishRejection(order, "Order response carried no status");
            continue;
          }

          // A TP/SL leg can be acknowledged with a bare string
          // ("waitingForTrigger", "waitingForFill") instead of an object.
          // It is live at the venue and cancellable by its cloid, but has
          // no oid yet.
          if (std::string_view bare = asString(st); !bare.empty())
          {
            _orderTracker->onSubmitted(order, "", cloid);
            publishSubmitted(order);
            if (trigger)
            {
              publishPendingTrigger(order);
            }
            continue;
          }

          if (std::string_view reason = asString(st["error"]); !reason.empty())
          {
            // The order never came into existence at the venue, so it is not
            // handed to the tracker at all -- same convention as the Bybit
            // executor, whose rejected submits leave no tracker record either.
            publishRejection(order, std::string(reason));
            continue;
          }

          if (auto filled = st["filled"]; !filled.error())
          {
            Quantity fillQty{};
            Price fillPrice{};
            if (auto totalSz = asString(filled["totalSz"]); !totalSz.empty())
            {
              if (auto q = util::parseQty(totalSz))
              {
                fillQty = *q;
              }
            }
            if (auto avgPx = asString(filled["avgPx"]); !avgPx.empty())
            {
              if (auto px = util::parsePrice(avgPx))
              {
                fillPrice = *px;
              }
            }

            _orderTracker->onSubmitted(order, oidOf(filled.value()), cloid);
            _orderTracker->onFilled(order.id, fillQty);
            publishFill(order, fillQty, fillPrice);
            continue;
          }

          if (auto resting = st["resting"]; !resting.error())
          {
            // A trigger order answers "resting" too: it rests until its
            // trigger price is hit. The oid goes to the tracker either way so
            // cancelOrder can find it.
            _orderTracker->onSubmitted(order, oidOf(resting.value()), cloid);
            publishSubmitted(order);
            if (trigger)
            {
              publishPendingTrigger(order);
            }
            continue;
          }

          // A status shape this connector does not know. The order may well be
          // live at the venue, so it is recorded, and its cloid is enough for
          // cancelOrder.
          _logger->warn("[HL] unrecognised order status in submit response");
          _orderTracker->onSubmitted(order, "", cloid);
          publishSubmitted(order);
        }
      },
      [this, sharedLegs](std::string_view err)
      {
        for (const auto& leg : *sharedLegs)
        {
          _policies.timeout.clearPending(leg.order.id);
          publishRejection(leg.order, std::string("Transport error: ") + std::string(err));
        }
      });
}

template <typename Policies>
void HyperliquidOrderExecutorT<Policies>::placePosTpsl(SymbolId symbol, Side positionSide,
                                                       Price slTrigger, Price tpTrigger,
                                                       Quantity qty, OrderId slLocalId,
                                                       OrderId tpLocalId)
{
  const OrderId gateId = slTrigger.raw() > 0 ? slLocalId : tpLocalId;
  _policies.rateLimit.gate(
      gateId,
      [this, symbol, positionSide, slTrigger, tpTrigger, qty, slLocalId, tpLocalId]
      {
        sendPlacePosTpsl(symbol, positionSide, slTrigger, tpTrigger, qty, slLocalId, tpLocalId);
      },
      [this, symbol, positionSide, slTrigger, tpTrigger, qty, slLocalId, tpLocalId]
      {
        // A protective stop that never left the process is worth an event:
        // the position is unprotected and only this connector knows it.
        const Side closing = positionSide == Side::BUY ? Side::SELL : Side::BUY;
        auto refused = [&](OrderId id, OrderType type, Price trigger)
        {
          Order o;
          o.id = id;
          o.symbol = symbol;
          o.side = closing;
          o.type = type;
          o.triggerPrice = trigger;
          o.quantity = qty;
          o.flags.reduceOnly = 1;
          publishRateLimited(o);
        };
        if (slTrigger.raw() > 0)
        {
          refused(slLocalId, OrderType::STOP_MARKET, slTrigger);
        }
        if (tpTrigger.raw() > 0)
        {
          refused(tpLocalId, OrderType::TAKE_PROFIT_MARKET, tpTrigger);
        }
      });
}

template <typename Policies>
void HyperliquidOrderExecutorT<Policies>::sendPlacePosTpsl(SymbolId symbol, Side positionSide,
                                                           Price slTrigger, Price tpTrigger,
                                                           Quantity qty, OrderId slLocalId,
                                                           OrderId tpLocalId)
{
  const Side closing = positionSide == Side::BUY ? Side::SELL : Side::BUY;

  std::vector<OrderLeg> legs;
  auto addLeg = [&](OrderId id, OrderType type, Price trigger)
  {
    Order o;
    o.id = id;
    o.symbol = symbol;
    o.side = closing;
    o.type = type;
    o.triggerPrice = trigger;
    o.quantity = qty;
    o.flags.reduceOnly = 1;
    legs.push_back(OrderLeg{o, {}, genCloid128()});
  };
  // SL first, then TP: the order the SDK's TP/SL example lists them in.
  if (slTrigger.raw() > 0)
  {
    addLeg(slLocalId, OrderType::STOP_MARKET, slTrigger);
  }
  if (tpTrigger.raw() > 0)
  {
    addLeg(tpLocalId, OrderType::TAKE_PROFIT_MARKET, tpTrigger);
  }

  if (legs.empty())
  {
    LRVX_LOG_ERROR("[HL] placePosTpsl: neither a stop-loss nor a take-profit trigger was given");
    return;
  }

  // Every failure below rejects the whole bracket: sending half of what the
  // caller asked for would leave it believing the other half is on the venue.
  auto rejectAll = [&](const std::string& reason)
  {
    for (const auto& leg : legs)
    {
      publishRejection(leg.order, "placePosTpsl: " + reason);
    }
  };

  for (const auto& leg : legs)
  {
    if (leg.order.id == 0)
    {
      rejectAll("every leg needs a local order id to be tracked and cancelled by");
      return;
    }
  }
  if (qty.raw() <= 0)
  {
    rejectAll("size must be positive");
    return;
  }
  if (slTrigger.raw() > 0 && tpTrigger.raw() > 0)
  {
    const bool ordered = positionSide == Side::BUY ? slTrigger.raw() < tpTrigger.raw()
                                                   : slTrigger.raw() > tpTrigger.raw();
    if (!ordered)
    {
      rejectAll("stop-loss and take-profit are on the wrong sides of each other for this position");
      return;
    }
  }

  auto info = _registry->getSymbolInfo(symbol);
  if (!info)
  {
    rejectAll("unknown symbol id");
    return;
  }
  int asset = assetIdFor(info->symbol);
  if (asset < 0)
  {
    rejectAll(assetMissingReason(info->symbol));
    return;
  }
  const int szDecimals = szDecimalsFor(info->symbol);

  for (auto& leg : legs)
  {
    auto wire = buildOrderWire(leg.order, asset, szDecimals, leg.cloid);
    if (!wire.error.empty())
    {
      rejectAll(wire.error);
      return;
    }
    leg.wire = std::move(wire.wire);
  }

  sendOrderAction(std::move(legs), "positionTpsl");
}

// Neither setLeverage nor a refusal of it carries an OrderId to report
// against; kNoOrderId only names that for the rate-limit gate.
static constexpr OrderId kNoOrderId = 0;

template <typename Policies>
void HyperliquidOrderExecutorT<Policies>::setLeverage(SymbolId symbol, int leverage, bool isCross,
                                                      LeverageCallback onResult)
{
  _policies.rateLimit.gate(
      kNoOrderId,
      [this, symbol, leverage, isCross, onResult]
      {
        sendSetLeverage(symbol, leverage, isCross, onResult);
      },
      [symbol, onResult]
      {
        LRVX_LOG_WARN("[HL] setLeverage for symbol " << symbol
                                                     << " refused by the client-side rate limit");
        if (onResult)
        {
          onResult(false, "client-side rate limit");
        }
      });
}

template <typename Policies>
void HyperliquidOrderExecutorT<Policies>::sendSetLeverage(SymbolId symbol, int leverage,
                                                          bool isCross, LeverageCallback onResult)
{
  auto fail = [&](const std::string& reason)
  {
    LRVX_LOG_ERROR("[HL] setLeverage: " << reason);
    if (onResult)
    {
      onResult(false, reason);
    }
  };

  if (leverage < 1)
  {
    fail("leverage must be at least 1, got " + std::to_string(leverage));
    return;
  }
  auto info = _registry->getSymbolInfo(symbol);
  if (!info)
  {
    fail("unknown symbol id");
    return;
  }
  int asset = assetIdFor(info->symbol);
  if (asset < 0)
  {
    fail(assetMissingReason(info->symbol));
    return;
  }

  // Key order as in the SDK's Exchange.update_leverage; it is signed.
  const std::string action = "{\"type\":\"updateLeverage\",\"asset\":" + std::to_string(asset) +
                             ",\"isCross\":" + (isCross ? "true" : "false") +
                             ",\"leverage\":" + std::to_string(leverage) + "}";

  auto body = signedBody(action, hl::nextNonceMs());
  if (!body)
  {
    fail("signing via SDK helper failed");
    return;
  }

  _logger->info("[HL] updateLeverage " + info->symbol + " action=" + action);

  const std::string coin = info->symbol;
  _transport->post(
      _url, *body, {{"Content-Type", "application/json"}},
      [coin, leverage, isCross, onResult](std::string_view resp)
      {
        // Success is {"status":"ok","response":{"type":"default"}}; a
        // refusal is {"status":"err","response":"<reason>"}.
        simdjson::dom::parser parser;
        simdjson::padded_string padded(resp);
        auto parsed = parser.parse(padded);
        std::string_view status;
        if (!parsed.error())
        {
          auto st = parsed.value()["status"].get_string();
          if (!st.error())
          {
            status = st.value_unsafe();
          }
        }
        if (status == "ok")
        {
          LRVX_LOG_INFO("[HL] leverage set to "
                        << leverage << "x " << (isCross ? "cross" : "isolated") << " for " << coin);
          if (onResult)
          {
            onResult(true, "");
          }
          return;
        }
        std::string reason;
        if (!parsed.error())
        {
          auto r = parsed.value()["response"].get_string();
          if (!r.error())
          {
            reason = std::string(r.value_unsafe());
          }
        }
        if (reason.empty())
        {
          reason = std::string(resp);
        }
        LRVX_LOG_ERROR("[HL] setLeverage failed for " << coin << ": " << reason);
        if (onResult)
        {
          onResult(false, reason);
        }
      },
      [coin, onResult](std::string_view err)
      {
        LRVX_LOG_ERROR("[HL] setLeverage transport error for " << coin << ": " << err);
        if (onResult)
        {
          onResult(false, std::string("Transport error: ") + std::string(err));
        }
      });
}

template <typename Policies>
void HyperliquidOrderExecutorT<Policies>::cancelOrder(OrderId localId)
{
  // Resolved before the gate so a refused cancel is reported against the
  // order it targeted instead of vanishing while the tracker keeps reporting
  // that order live.
  auto target = _orderTracker->get(localId);
  if (!target)
  {
    LRVX_LOG_ERROR("[HL] cancelOrder: no orderState for localId " << localId);
    return;
  }

  Order local = target->localOrder;
  _policies.rateLimit.gate(
      localId,
      [this, localId]
      {
        sendCancelOrder(localId);
      },
      [this, local]
      {
        publishRateLimited(local);
      });
}

template <typename Policies>
void HyperliquidOrderExecutorT<Policies>::sendCancelOrder(OrderId localId)
{
  auto orderState = _orderTracker->get(localId);
  if (!orderState)
  {
    LRVX_LOG_ERROR("[HL] cancelOrder: no orderState for localId " << localId);
    return;
  }
  if (orderState->clientOrderId.empty())
  {
    LRVX_LOG_ERROR("[HL] cancelOrder: no clientOrderId for localId " << localId);
    return;
  }

  auto symbol = orderState->localOrder.symbol;
  auto info = _registry->getSymbolInfo(symbol);
  if (!info)
  {
    LRVX_LOG_ERROR("[HL] cancelOrder: no symbolInfo for " << symbol);
    return;
  }

  int asset = assetIdFor(info->symbol);
  if (asset < 0)
  {
    LRVX_LOG_ERROR("[HL] cancelOrder: " << assetMissingReason(info->symbol));
    return;
  }

  std::string action;
  action.reserve(128);
  action += "{\"type\":\"cancelByCloid\",\"cancels\":[{\"asset\":" + std::to_string(asset) +
            ",\"cloid\":\"" + orderState->clientOrderId + "\"}]}";

  auto body = signedBody(action, hl::nextNonceMs());
  if (!body)
  {
    LRVX_LOG_ERROR("[HL] cancelOrder: signing failed");
    return;
  }

  _logger->info("[HL] cancelByCloid localId=" + std::to_string(localId) + " action=" + action);

  _policies.timeout.trackCancel(localId);

  _transport->post(
      _url, *body, {{"Content-Type", "application/json"}},
      [this, localId](std::string_view resp)
      {
        _policies.timeout.clearPending(localId);

        simdjson::ondemand::parser p;
        simdjson::padded_string ps(resp);
        auto doc = p.iterate(ps);
        auto st = doc["status"].get_string();
        if (!st.error() && std::string_view(st.value_unsafe()) == "ok")
        {
          _orderTracker->onCanceled(localId);
        }
        else
        {
          LRVX_LOG_ERROR("[HL] cancel failed: " << resp);
        }
      },
      [this, localId](std::string_view err)
      {
        _policies.timeout.clearPending(localId);
        LRVX_LOG_ERROR("[HL] cancel error: " << err);
      });
}

template <typename Policies>
void HyperliquidOrderExecutorT<Policies>::replaceOrder(OrderId oldLocalId, const Order& n)
{
  auto target = _orderTracker->get(oldLocalId);
  if (!target)
  {
    LRVX_LOG_ERROR("[HL] replaceOrder: no orderState for oldLocalId " << oldLocalId);
    return;
  }

  Order local = target->localOrder;
  _policies.rateLimit.gate(
      oldLocalId,
      [this, oldLocalId, n]
      {
        sendReplaceOrder(oldLocalId, n);
      },
      [this, local]
      {
        publishRateLimited(local);
      });
}

template <typename Policies>
void HyperliquidOrderExecutorT<Policies>::sendReplaceOrder(OrderId oldLocalId, const Order& n)
{
  auto orderState = _orderTracker->get(oldLocalId);
  if (!orderState)
  {
    LRVX_LOG_ERROR("[HL] replaceOrder: no orderState for oldLocalId " << oldLocalId);
    return;
  }

  auto exId = orderState->exchangeOrderId;
  auto cloid = orderState->clientOrderId;

  // HL's modify without always_place takes a non-trigger order only, with
  // tif Alo or a Gtc that would not execute, which the venue overrides to
  // Alo (exchange endpoint docs, "Modify an order"). This connector does not
  // send always_place: it places the new order even when the cancel of the
  // old one failed, which for a stop means two of them. A trigger order is
  // therefore replaced by cancelling it and submitting a new one, and a
  // modify that names one is refused here rather than by the venue.
  if (isTriggerType(n.type))
  {
    publishRejection(n,
                     "replaceOrder: Hyperliquid's modify takes a non-trigger order; cancel the "
                     "trigger order and submit a new one (oldLocalId " +
                         std::to_string(oldLocalId) + ")");
    return;
  }

  auto info = _registry->getSymbolInfo(n.symbol);
  if (!info)
  {
    LRVX_LOG_ERROR("[HL] unknown symbol id in replaceOrder");
    return;
  }
  int asset = assetIdFor(info->symbol);
  if (asset < 0)
  {
    LRVX_LOG_ERROR("[HL] replaceOrder: " << assetMissingReason(info->symbol));
    return;
  }

  auto wire = buildOrderWire(n, asset, szDecimalsFor(info->symbol), cloid);
  if (!wire.error.empty())
  {
    LRVX_LOG_ERROR("[HL] replaceOrder: " << wire.error << " (oldLocalId=" << oldLocalId << ")");
    return;
  }
  const std::string& orderObj = wire.wire;

  // HL's modify takes the order as an oid (a number) or a cloid (a string).
  // A leg the venue acknowledged without an oid (a bare "waitingForTrigger",
  // or a status shape this connector does not know) is addressed by cloid.
  std::string target;
  if (!exId.empty())
  {
    target = exId;
  }
  else if (!cloid.empty())
  {
    target = "\"" + cloid + "\"";
  }
  else
  {
    publishRejection(n,
                     "replaceOrder: neither an exchange order id nor a cloid is known for "
                     "localId " +
                         std::to_string(oldLocalId));
    return;
  }

  std::string action = "{\"type\":\"modify\",\"oid\":" + target + ",\"order\":" + orderObj + "}";

  auto body = signedBody(action, hl::nextNonceMs());
  if (!body)
  {
    LRVX_LOG_ERROR("[HL] replaceOrder: signing failed");
    return;
  }

  _logger->info("[HL] modify localId=" + std::to_string(oldLocalId) +
                " newLocalId=" + std::to_string(n.id) + " action=" + action);

  _policies.timeout.trackReplace(oldLocalId);

  _transport->post(
      _url, *body, {{"Content-Type", "application/json"}},
      [this, oldLocalId, exId, n, cloid](std::string_view resp)
      {
        _policies.timeout.clearPending(oldLocalId);

        simdjson::ondemand::parser parser;
        simdjson::padded_string padded(resp);
        auto doc = parser.iterate(padded);

        auto status = doc["status"].get_string();
        if (!status.error() && status.value() == "ok")
        {
          _orderTracker->onReplaced(oldLocalId, n, exId, cloid);
        }
        else
        {
          LRVX_LOG_ERROR("[HL] modify error: " << status);
        }
      },
      [this, oldLocalId](std::string_view err)
      {
        _policies.timeout.clearPending(oldLocalId);
        LRVX_LOG_ERROR("[HL] modify error: " << err);
      });
}

template class HyperliquidOrderExecutorT<NoPolicies>;
template class HyperliquidOrderExecutorT<WithRateLimit>;
template class HyperliquidOrderExecutorT<WithTimeout>;
template class HyperliquidOrderExecutorT<FullPolicies>;

}  // namespace lrvx
