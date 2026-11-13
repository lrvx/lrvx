/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "lrvx-connectors/bitget/bitget_order_executor.h"
#include "lrvx-connectors/bitget/authenticated_rest_client.h"
#include "lrvx-connectors/execution/order_tif.h"
#include "lrvx-connectors/execution/venue_price.h"

#include <lrvx/common.h>
#include <lrvx/engine/symbol_registry.h>
#include <lrvx/execution/events/order_event.h>
#include <lrvx/execution/order_tracker.h>
#include <lrvx/log/log.h>

#include <simdjson.h>

#include <cmath>
#include <cstdio>

namespace lrvx
{

template <typename Policies>
BitgetOrderExecutorT<Policies>::~BitgetOrderExecutorT() = default;

static constexpr std::string_view kPathPlace = "/api/v2/mix/order/place-order";
static constexpr std::string_view kPathPlan = "/api/v2/mix/order/place-plan-order";
static constexpr std::string_view kPathCancel = "/api/v2/mix/order/cancel-order";
static constexpr std::string_view kPathCancelPlan = "/api/v2/mix/order/cancel-plan-order";
static constexpr std::string_view kPathModify = "/api/v2/mix/order/modify-order";
static constexpr std::string_view kPathSetLeverage = "/api/v2/mix/account/set-leverage";
static constexpr std::string_view kPathPosTpsl = "/api/v2/mix/order/place-pos-tpsl";
static constexpr std::string_view kPathModifyTpsl = "/api/v2/mix/order/modify-tpsl-order";

// Format a double as a fixed-point decimal with up to `max_decimals` fractional
// digits, rounded to nearest, trailing zeros stripped. Only the size of a
// position TP/SL modify still goes through it; prices never do (see below).
static std::string trimDouble(double v, int max_decimals)
{
  // Round to max_decimals first.
  double scale = 1.0;
  for (int i = 0; i < max_decimals; ++i)
  {
    scale *= 10.0;
  }
  double rounded = std::round(v * scale) / scale;
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.*f", max_decimals, rounded);
  std::string s(buf);
  if (s.find('.') != std::string::npos)
  {
    while (!s.empty() && s.back() == '0')
    {
      s.pop_back();
    }
    if (!s.empty() && s.back() == '.')
    {
      s.pop_back();
    }
  }
  return s;
}

// Prices are not rounded here at all. Bitget refuses a price with more digits
// than the instrument quotes, so the digit count is a property of the
// instrument -- but rounding to it moves the price the strategy asked for, and
// rounding to the registry default (two digits, which every live symbol had
// before the venue's tick was loaded) moved protective triggers on anything
// quoted finer. A price off a known tick is refused before a request is built
// (venue_price::offTickReason, the simulator's rule and reason), so whatever
// is sent is on the tick and its exact decimal is the venue's own format; with
// the tick unknown the price goes out exactly as given.

namespace
{

// Bitget answers {"code":"00000","msg":"success","data":...} on success and
// another code with a msg on refusal. A body that is not that shape is
// reported as a refusal too: the request's fate is then unknown, and saying
// nothing was the one answer known to be wrong.
struct BitgetAck
{
  bool ok{false};
  std::string reason;
  std::string orderId;
};

BitgetAck readBitgetAck(std::string_view resp)
{
  BitgetAck out;
  simdjson::dom::parser parser;
  simdjson::padded_string padded(resp);
  auto parsed = parser.parse(padded);
  if (parsed.error())
  {
    out.reason = "unparseable response: " + std::string(resp.substr(0, 200));
    return out;
  }
  auto doc = parsed.value();
  std::string_view code;
  if (doc["code"].get_string().get(code))
  {
    out.reason = "response has no code: " + std::string(resp.substr(0, 200));
    return out;
  }
  if (code != "00000")
  {
    std::string_view msg;
    (void)doc["msg"].get_string().get(msg);
    out.reason = msg.empty() ? "code=" + std::string(code)
                             : std::string(msg) + " (code=" + std::string(code) + ")";
    return out;
  }
  out.ok = true;
  auto data = doc["data"];
  std::string_view id;
  if (!data.error())
  {
    const simdjson::dom::element dataEl = data.value_unsafe();
    simdjson::dom::array arr;
    if (!dataEl.get_array().get(arr))
    {
      for (auto el : arr)
      {
        if (!el["orderId"].get_string().get(id))
        {
          out.orderId = std::string(id);
          break;
        }
      }
    }
    else if (!dataEl["orderId"].get_string().get(id))
    {
      out.orderId = std::string(id);
    }
  }
  return out;
}

std::string_view bitgetForceToken(NormalizedTif tif)
{
  switch (tif)
  {
    case NormalizedTif::GTC:
      return "gtc";
    case NormalizedTif::IOC:
      return "ioc";
    case NormalizedTif::FOK:
      return "fok";
    case NormalizedTif::POST_ONLY:
      return "post_only";
    case NormalizedTif::UNSUPPORTED:
      return "";
  }
  return "";
}

// tradeSide/posSide only get sent once the account's position mode is
// actually known. Sending them unconditionally off reduceOnly alone (the
// pre-fix behaviour) meant the field's meaning depended on an account
// setting this connector never tracked.
void appendPositionFields(std::string& body, const Order& order, const Bitget::Params& params)
{
  if (params.positionMode != Bitget::PositionMode::Hedge)
  {
    return;
  }
  body.append("\"tradeSide\":\"").append(order.flags.reduceOnly ? "close" : "open").append("\",");
  const auto holdSide = static_cast<HoldSide>(order.flags.holdSide);
  if (holdSide == HoldSide::Long)
  {
    body.append("\"posSide\":\"long\",");
  }
  else if (holdSide == HoldSide::Short)
  {
    body.append("\"posSide\":\"short\",");
  }
}

}  // namespace

// setLeverage, submitOrderWithLeverage, placePosTpsl and modifyPosTpsl are
// ordinary REST requests against the same venue budget as a submit, and the
// trailing stop walks modifyPosTpsl on every bar -- the paths that used to
// skip the limiter were the ones that fire most often. Neither setLeverage
// nor modifyPosTpsl carries an OrderId to report against, so a refusal is a
// log line rather than an event; kNoOrderId only names that in the log.
static constexpr OrderId kNoOrderId = 0;

template <typename Policies>
void BitgetOrderExecutorT<Policies>::setLeverage(const std::string& symbol, int leverage)
{
  _policies.rateLimit.gate(
      kNoOrderId,
      [this, symbol, leverage]
      {
        sendSetLeverage(symbol, leverage);
      },
      [symbol]
      {
        LRVX_LOG_WARN("[BitgetOE] setLeverage for " << symbol
                                                    << " refused by the client-side rate limit");
      });
}

template <typename Policies>
void BitgetOrderExecutorT<Policies>::sendSetLeverage(const std::string& symbol, int leverage)
{
  std::string body;
  body.reserve(128);
  body.append("{\"symbol\":\"")
      .append(symbol)
      .append("\",")
      .append("\"productType\":\"")
      .append(_params.productType)
      .append("\",")
      .append("\"marginCoin\":\"")
      .append(_params.marginCoin)
      .append("\",")
      .append("\"leverage\":\"")
      .append(std::to_string(leverage))
      .append("\"}");

  _client->post(
      std::string(kPathSetLeverage), body,
      [symbol, leverage](std::string_view resp)
      {
        const BitgetAck ack = readBitgetAck(resp);
        if (!ack.ok)
        {
          LRVX_LOG_ERROR("[BitgetOE] setLeverage failed for " << symbol << ": " << ack.reason);
        }
        else
        {
          LRVX_LOG_INFO("[BitgetOE] Leverage set to " << leverage << "x for " << symbol);
        }
      },
      [symbol](std::string_view err)
      {
        LRVX_LOG_ERROR("[BitgetOE] setLeverage transport: " << err);
      });
}

template <typename Policies>
void BitgetOrderExecutorT<Policies>::submitOrderWithLeverage(const Order& order, int leverage,
                                                             double slPrice, double tpPrice)
{
  _policies.rateLimit.gate(
      order.id,
      [this, order, leverage, slPrice, tpPrice]
      {
        sendSubmitOrderWithLeverage(order, leverage, slPrice, tpPrice);
      },
      [this, order]
      {
        publishRateLimited(order);
      });
}

template <typename Policies>
void BitgetOrderExecutorT<Policies>::sendSubmitOrderWithLeverage(const Order& order, int leverage,
                                                                 double slPrice, double tpPrice)
{
  auto info = _registry->getSymbolInfo(order.symbol);
  if (!info)
  {
    LRVX_LOG_ERROR("[BitgetOE] submitOrderWithLeverage: unknown symbolId=" << order.symbol);
    publishRejection(order, "unknown symbol");
    return;
  }

  std::string levBody;
  levBody.reserve(128);
  levBody.append("{\"symbol\":\"")
      .append(info->symbol)
      .append("\",")
      .append("\"productType\":\"")
      .append(_params.productType)
      .append("\",")
      .append("\"marginCoin\":\"")
      .append(_params.marginCoin)
      .append("\",")
      .append("\"leverage\":\"")
      .append(std::to_string(leverage))
      .append("\"}");

  _client->post(
      std::string(kPathSetLeverage), levBody,
      [this, order, leverage, slPrice, tpPrice](std::string_view resp)
      {
        const BitgetAck levAck = readBitgetAck(resp);
        if (!levAck.ok)
        {
          LRVX_LOG_ERROR("[BitgetOE] setLeverage failed: " << levAck.reason);
          publishRejection(order, "setLeverage failed: " + levAck.reason);
          return;
        }
        LRVX_LOG_INFO("[BitgetOE] Leverage " << leverage << "x, submitting order");

        if (slPrice <= 0 && tpPrice <= 0)
        {
          submitOrder(order);
          return;
        }

        auto info2 = _registry->getSymbolInfo(order.symbol);
        if (!info2)
        {
          publishRejection(order, "unknown symbol");
          return;
        }

        if (order.type == OrderType::TRAILING_STOP || order.type == OrderType::ICEBERG)
        {
          publishRejection(order, "order type not implemented by this connector");
          return;
        }

        bool isMarket = (order.type == OrderType::MARKET);
        const NormalizedTif tif = normalizeTif(order.timeInForce, order.flags.postOnly);
        if (!isMarket && tif == NormalizedTif::UNSUPPORTED)
        {
          publishRejection(order, "Bitget has no native good-till-date order");
          return;
        }
        if (const char* offTick = venue_price::offTickReason(order, *info2))
        {
          publishRejection(order, offTick);
          return;
        }
        if (const char* offLot = venue_price::offLotReason(order, *info2))
        {
          publishRejection(order, offLot);
          return;
        }
        const Price slPx = slPrice > 0 ? Price::fromDouble(slPrice) : Price{};
        const Price tpPx = tpPrice > 0 ? Price::fromDouble(tpPrice) : Price{};
        if (venue_price::isOffTick(slPx, *info2) || venue_price::isOffTick(tpPx, *info2))
        {
          publishRejection(order, kRejectTriggerOffTick);
          return;
        }

        std::string body;
        body.reserve(384);
        body.append("{\"symbol\":\"")
            .append(info2->symbol)
            .append("\",")
            .append("\"productType\":\"")
            .append(_params.productType)
            .append("\",")
            .append("\"marginMode\":\"")
            .append(_params.marginMode)
            .append("\",")
            .append("\"marginCoin\":\"")
            .append(_params.marginCoin)
            .append("\",")
            .append("\"size\":\"")
            .append(order.quantity.toString())
            .append("\",");

        if (!isMarket)
        {
          body.append("\"price\":\"").append(venue_price::wireDecimal(order.price)).append("\",");
        }

        body.append("\"side\":\"").append(order.side == Side::BUY ? "buy" : "sell").append("\",");

        appendPositionFields(body, order, _params);

        body.append("\"orderType\":\"").append(isMarket ? "market" : "limit").append("\",");

        if (!isMarket)
        {
          body.append("\"force\":\"").append(bitgetForceToken(tif)).append("\",");
        }

        if (slPrice > 0)
        {
          body.append("\"presetStopLossPrice\":\"")
              .append(venue_price::wireDecimal(slPx))
              .append("\",");
        }
        if (tpPrice > 0)
        {
          body.append("\"presetStopSurplusPrice\":\"")
              .append(venue_price::wireDecimal(tpPx))
              .append("\",");
        }

        body.append("\"clientOid\":\"").append(std::to_string(order.id)).append("\"}");

        _policies.timeout.trackSubmit(order.id);

        _client->post(
            std::string(kPathPlace), body,
            [this, order](std::string_view resp2)
            {
              _policies.timeout.clearPending(order.id);
              const BitgetAck ack = readBitgetAck(resp2);
              if (!ack.ok)
              {
                LRVX_LOG_ERROR("[BitgetOE] submitOrder failed: " << ack.reason);
                publishRejection(order, ack.reason);
                return;
              }
              _orderTracker->onSubmitted(order, ack.orderId);
            },
            [this, order](std::string_view err2)
            {
              _policies.timeout.clearPending(order.id);
              std::string msg(err2);
              LRVX_LOG_ERROR("[BitgetOE] submitOrder transport: " << msg);
              publishRejection(order, msg);
            });
      },
      [this, order](std::string_view err)
      {
        std::string msg(err);
        LRVX_LOG_ERROR("[BitgetOE] setLeverage transport: " << msg);
        publishRejection(order, "setLeverage transport: " + msg);
      });
}

template <typename Policies>
void BitgetOrderExecutorT<Policies>::publishRejection(const Order& order, const std::string& reason)
{
  _orderTracker->onRejected(order.id, reason);
  if (_orderBus)
  {
    OrderEvent ev;
    ev.status = OrderEventStatus::REJECTED;
    ev.order = order;
    ev.rejectReason = reason;
    _orderBus->publish(std::move(ev));
  }
}

template <typename Policies>
void BitgetOrderExecutorT<Policies>::publishCancelRejected(const Order& order,
                                                           const std::string& reason)
{
  LRVX_LOG_ERROR("[BitgetOE] cancel rejected: " << reason << " (orderId=" << order.id << ")");
  if (_orderBus)
  {
    OrderEvent ev;
    ev.status = OrderEventStatus::REJECTED;
    ev.order = order;
    ev.rejectReason = "cancel rejected: " + reason;
    _orderBus->publish(std::move(ev));
  }
}

template <typename Policies>
void BitgetOrderExecutorT<Policies>::publishReplaceRejected(const Order& order,
                                                            const Order& newOrder,
                                                            const std::string& reason)
{
  LRVX_LOG_ERROR("[BitgetOE] replace rejected: " << reason << " (orderId=" << order.id << ")");
  if (_orderBus)
  {
    OrderEvent ev;
    ev.status = OrderEventStatus::REPLACE_REJECTED;
    ev.order = order;
    ev.newOrder = newOrder;
    ev.rejectReason = reason;
    _orderBus->publish(std::move(ev));
  }
}

// A client-side rate-limit rejection never reached the venue and left no
// trace anywhere -- the tracker (if any record existed) kept reporting the
// order active with no signal that a cancel or replace silently never left
// the process. Deliberately does not touch OrderTracker: unlike
// publishRejection, there was no submission attempt to mark rejected.
template <typename Policies>
void BitgetOrderExecutorT<Policies>::publishRateLimited(const Order& order)
{
  if (_orderBus)
  {
    OrderEvent ev;
    ev.status = OrderEventStatus::REJECTED_RATE_LIMIT;
    ev.order = order;
    ev.rejectReason = "client-side rate limit";
    _orderBus->publish(std::move(ev));
  }
}

template <typename Policies>
void BitgetOrderExecutorT<Policies>::submitOrder(const Order& order)
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
void BitgetOrderExecutorT<Policies>::sendSubmitOrder(const Order& order)
{
  auto info = _registry->getSymbolInfo(order.symbol);
  if (!info)
  {
    LRVX_LOG_ERROR("[BitgetOE] submitOrder: unknown symbolId=" << order.symbol);
    publishRejection(order, "unknown symbol");
    return;
  }

  // The simulator's tick rule, when the tick is known: refused here with its
  // reason rather than sent for the venue to refuse with another.
  if (const char* offTick = venue_price::offTickReason(order, *info))
  {
    publishRejection(order, offTick);
    return;
  }
  if (const char* offLot = venue_price::offLotReason(order, *info))
  {
    publishRejection(order, offLot);
    return;
  }

  bool isPlan =
      (order.type == OrderType::STOP_MARKET || order.type == OrderType::TAKE_PROFIT_MARKET ||
       order.type == OrderType::STOP_LIMIT || order.type == OrderType::TAKE_PROFIT_LIMIT);

  if (isPlan)
  {
    submitPlanOrder(order, *info);
    return;
  }

  if (order.type == OrderType::TRAILING_STOP)
  {
    publishRejection(order, "Bitget trailing stop is not implemented by this connector");
    return;
  }
  if (order.type == OrderType::ICEBERG)
  {
    publishRejection(order, "Bitget iceberg (visible-quantity) orders are not implemented");
    return;
  }

  bool isMarket = (order.type == OrderType::MARKET);

  const NormalizedTif tif = normalizeTif(order.timeInForce, order.flags.postOnly);
  if (!isMarket && tif == NormalizedTif::UNSUPPORTED)
  {
    publishRejection(order, "Bitget has no native good-till-date order");
    return;
  }

  std::string body;
  body.reserve(256);
  body.append("{\"symbol\":\"")
      .append(info->symbol)
      .append("\",")
      .append("\"productType\":\"")
      .append(_params.productType)
      .append("\",")
      .append("\"marginMode\":\"")
      .append(_params.marginMode)
      .append("\",")
      .append("\"marginCoin\":\"")
      .append(_params.marginCoin)
      .append("\",")
      .append("\"size\":\"")
      .append(order.quantity.toString())
      .append("\",");

  if (!isMarket)
  {
    body.append("\"price\":\"").append(venue_price::wireDecimal(order.price)).append("\",");
  }

  body.append("\"side\":\"").append(order.side == Side::BUY ? "buy" : "sell").append("\",");

  appendPositionFields(body, order, _params);

  body.append("\"orderType\":\"").append(isMarket ? "market" : "limit").append("\",");

  if (!isMarket)
  {
    // force carries the order's own timeInForce/postOnly now, not a static
    // config value that ignored per-order intent.
    body.append("\"force\":\"").append(bitgetForceToken(tif)).append("\",");
  }

  body.append("\"clientOid\":\"").append(std::to_string(order.id)).append("\"}");

  _policies.timeout.trackSubmit(order.id);

  _client->post(
      std::string(kPathPlace), body,
      [this, order](std::string_view resp)
      {
        _policies.timeout.clearPending(order.id);

        const BitgetAck ack = readBitgetAck(resp);
        if (!ack.ok)
        {
          LRVX_LOG_ERROR("[BitgetOE] submitOrder failed: " << ack.reason);
          publishRejection(order, ack.reason);
          return;
        }
        _orderTracker->onSubmitted(order, ack.orderId);
      },
      [this, order](std::string_view err)
      {
        _policies.timeout.clearPending(order.id);
        std::string msg(err);
        LRVX_LOG_ERROR("[BitgetOE] submitOrder transport: " << msg);
        publishRejection(order, msg);
      });
}

template <typename Policies>
void BitgetOrderExecutorT<Policies>::submitPlanOrder(const Order& order, const SymbolInfo& info)
{
  // A STOP_LIMIT is a stop plus the limit price that bounds what it may fill
  // at; sending it as a plan order of type "market" with no price, which is
  // what this path used to do for every conditional order, turns it into a
  // stop-market at the venue and removes that bound with no rejection and no
  // event. The limit price is what the strategy asked for, so an order that
  // cannot carry it is rejected instead of approximated.
  const bool isLimitPlan =
      (order.type == OrderType::STOP_LIMIT || order.type == OrderType::TAKE_PROFIT_LIMIT);
  if (isLimitPlan && order.price.raw() <= 0)
  {
    publishRejection(order, "conditional limit order without a limit price");
    return;
  }

  std::string body;
  body.reserve(320);
  body.append("{\"planType\":\"normal_plan\",")
      .append("\"symbol\":\"")
      .append(info.symbol)
      .append("\",")
      .append("\"productType\":\"")
      .append(_params.productType)
      .append("\",")
      .append("\"marginMode\":\"")
      .append(_params.marginMode)
      .append("\",")
      .append("\"marginCoin\":\"")
      .append(_params.marginCoin)
      .append("\",")
      .append("\"size\":\"")
      .append(order.quantity.toString())
      .append("\",")
      .append("\"triggerPrice\":\"")
      .append(venue_price::trimmedDecimal(order.triggerPrice))
      .append("\",")
      .append("\"triggerType\":\"mark_price\",")
      .append("\"side\":\"")
      .append(order.side == Side::BUY ? "buy" : "sell")
      .append("\",");

  appendPositionFields(body, order, _params);

  body.append("\"orderType\":\"").append(isLimitPlan ? "limit" : "market").append("\",");

  if (isLimitPlan)
  {
    body.append("\"price\":\"").append(venue_price::trimmedDecimal(order.price)).append("\",");
  }

  body.append("\"clientOid\":\"").append(std::to_string(order.id)).append("\"}");

  _policies.timeout.trackSubmit(order.id);

  _client->post(
      std::string(kPathPlan), body,
      [this, order](std::string_view resp)
      {
        _policies.timeout.clearPending(order.id);

        const BitgetAck ack = readBitgetAck(resp);
        if (!ack.ok)
        {
          LRVX_LOG_ERROR("[BitgetOE] submitPlanOrder failed: " << ack.reason);
          publishRejection(order, ack.reason);
          return;
        }
        _orderTracker->onSubmitted(order, ack.orderId);
      },
      [this, order](std::string_view err)
      {
        _policies.timeout.clearPending(order.id);
        std::string msg(err);
        LRVX_LOG_ERROR("[BitgetOE] submitPlanOrder transport: " << msg);
        publishRejection(order, msg);
      });
}

template <typename Policies>
void BitgetOrderExecutorT<Policies>::cancelOrder(OrderId id)
{
  // Looked up before the rate-limit gate so a refused cancel can still be
  // reported against the order it targeted instead of vanishing with no event
  // while the tracker keeps reporting the order active. The send path reads
  // the tracker again: a deferred cancel runs later, and the order it names
  // may have moved on in the meantime.
  auto st = _orderTracker->get(id);
  if (!st)
  {
    LRVX_LOG_ERROR("[BitgetOE] cancelOrder: unknown id=" << id);
    return;
  }

  Order target = st->localOrder;
  _policies.rateLimit.gate(
      id,
      [this, id]
      {
        sendCancelOrder(id);
      },
      [this, target]
      {
        publishRateLimited(target);
      });
}

template <typename Policies>
void BitgetOrderExecutorT<Policies>::sendCancelOrder(OrderId id)
{
  auto st = _orderTracker->get(id);
  if (!st)
  {
    LRVX_LOG_ERROR("[BitgetOE] cancelOrder: unknown id=" << id);
    return;
  }

  const Order target = st->localOrder;
  auto info = _registry->getSymbolInfo(target.symbol);
  if (!info)
  {
    publishCancelRejected(target, "no symbol info registered");
    return;
  }

  bool isPlan = (st->localOrder.type == OrderType::STOP_MARKET ||
                 st->localOrder.type == OrderType::TAKE_PROFIT_MARKET ||
                 st->localOrder.type == OrderType::STOP_LIMIT ||
                 st->localOrder.type == OrderType::TAKE_PROFIT_LIMIT);

  std::string_view endpoint = isPlan ? kPathCancelPlan : kPathCancel;

  std::string body;
  body.reserve(128);
  body.append("{\"symbol\":\"")
      .append(info->symbol)
      .append("\",")
      .append("\"productType\":\"")
      .append(_params.productType)
      .append("\",")
      .append("\"marginCoin\":\"")
      .append(_params.marginCoin)
      .append("\",");

  if (!st->exchangeOrderId.empty())
  {
    body.append("\"orderId\":\"").append(st->exchangeOrderId).append("\"}");
  }
  else
  {
    body.append("\"clientOid\":\"").append(std::to_string(id)).append("\"}");
  }

  _policies.timeout.trackCancel(id);

  _client->post(
      std::string(endpoint), body,
      [this, id, target](std::string_view resp)
      {
        _policies.timeout.clearPending(id);

        const BitgetAck ack = readBitgetAck(resp);
        if (ack.ok)
        {
          _orderTracker->onCanceled(id);
        }
        else
        {
          publishCancelRejected(target, ack.reason);
        }
      },
      [this, id, target](std::string_view err)
      {
        _policies.timeout.clearPending(id);
        publishCancelRejected(target, "transport error: " + std::string(err));
      });
}

template <typename Policies>
void BitgetOrderExecutorT<Policies>::replaceOrder(OrderId oldId, const Order& newOrd)
{
  auto st = _orderTracker->get(oldId);
  if (!st)
  {
    LRVX_LOG_ERROR("[BitgetOE] replaceOrder: unknown id=" << oldId);
    return;
  }

  Order target = st->localOrder;
  _policies.rateLimit.gate(
      oldId,
      [this, oldId, newOrd]
      {
        sendReplaceOrder(oldId, newOrd);
      },
      [this, target]
      {
        publishRateLimited(target);
      });
}

template <typename Policies>
void BitgetOrderExecutorT<Policies>::sendReplaceOrder(OrderId oldId, const Order& newOrd)
{
  auto st = _orderTracker->get(oldId);
  if (!st)
  {
    LRVX_LOG_ERROR("[BitgetOE] replaceOrder: unknown id=" << oldId);
    return;
  }

  const Order target = st->localOrder;
  auto info = _registry->getSymbolInfo(target.symbol);
  if (!info)
  {
    publishReplaceRejected(target, newOrd, "no symbol info registered");
    return;
  }
  if (venue_price::isOffTick(newOrd.price, *info))
  {
    publishReplaceRejected(target, newOrd, kRejectPriceOffTick);
    return;
  }
  if (const char* offLot = venue_price::offLotReason(newOrd, *info))
  {
    publishReplaceRejected(target, newOrd, offLot);
    return;
  }

  std::string body;
  body.reserve(256);
  body.append("{\"orderId\":\"")
      .append(st->exchangeOrderId)
      .append("\",")
      .append("\"symbol\":\"")
      .append(info->symbol)
      .append("\",")
      .append("\"productType\":\"")
      .append(_params.productType)
      .append("\",")
      .append("\"marginCoin\":\"")
      .append(_params.marginCoin)
      .append("\",")
      .append("\"newPrice\":\"")
      .append(venue_price::wireDecimal(newOrd.price))
      .append("\",")
      .append("\"newSize\":\"")
      .append(newOrd.quantity.toString())
      .append("\",")
      .append("\"newClientOid\":\"")
      .append(std::to_string(newOrd.id))
      .append("\"}");

  _policies.timeout.trackReplace(oldId);

  _client->post(
      std::string(kPathModify), body,
      [this, oldId, newOrd, target](std::string_view resp)
      {
        _policies.timeout.clearPending(oldId);

        const BitgetAck ack = readBitgetAck(resp);
        if (!ack.ok)
        {
          publishReplaceRejected(target, newOrd, ack.reason);
          return;
        }
        _orderTracker->onReplaced(oldId, newOrd, ack.orderId);
      },
      [this, oldId, newOrd, target](std::string_view err)
      {
        _policies.timeout.clearPending(oldId);
        publishReplaceRejected(target, newOrd, "transport error: " + std::string(err));
      });
}

template <typename Policies>
void BitgetOrderExecutorT<Policies>::placePosTpsl(SymbolId symbol, HoldSide holdSide,
                                                  double slPrice, double tpPrice, OrderId localId)
{
  _policies.rateLimit.gate(
      localId,
      [this, symbol, holdSide, slPrice, tpPrice, localId]
      {
        sendPlacePosTpsl(symbol, holdSide, slPrice, tpPrice, localId);
      },
      [this, symbol, localId]
      {
        // A protective stop that never left the process is worth an event:
        // the position is unprotected and only this connector knows it.
        Order refused;
        refused.id = localId;
        refused.symbol = symbol;
        publishRateLimited(refused);
      });
}

template <typename Policies>
void BitgetOrderExecutorT<Policies>::sendPlacePosTpsl(SymbolId symbol, HoldSide holdSide,
                                                      double slPrice, double tpPrice,
                                                      OrderId localId)
{
  // The order this protective request is reported against: the local id the
  // caller tracks it by, on the position's symbol.
  Order tracked;
  tracked.id = localId;
  tracked.symbol = symbol;

  auto info = _registry->getSymbolInfo(symbol);
  if (!info)
  {
    publishRejection(tracked, "placePosTpsl: unknown symbol");
    return;
  }
  if (holdSide == HoldSide::Unspecified)
  {
    publishRejection(tracked, "placePosTpsl: holdSide must be Long or Short");
    return;
  }
  const Price slPx = slPrice > 0 ? Price::fromDouble(slPrice) : Price{};
  const Price tpPx = tpPrice > 0 ? Price::fromDouble(tpPrice) : Price{};
  if (venue_price::isOffTick(slPx, *info) || venue_price::isOffTick(tpPx, *info))
  {
    publishRejection(tracked, kRejectTriggerOffTick);
    return;
  }

  std::string body;
  body.reserve(384);
  body.append("{\"symbol\":\"")
      .append(info->symbol)
      .append("\",")
      .append("\"productType\":\"")
      .append(_params.productType)
      .append("\",")
      .append("\"marginCoin\":\"")
      .append(_params.marginCoin)
      .append("\",")
      .append("\"holdSide\":\"")
      .append(holdSide == HoldSide::Long ? "long" : "short")
      .append("\",")
      .append("\"planType\":\"")
      .append(slPrice > 0 && tpPrice > 0 ? "profit_loss"
              : slPrice > 0              ? "pos_loss"
                                         : "pos_profit")
      .append("\",");
  if (slPrice > 0)
  {
    body.append("\"stopLossTriggerPrice\":\"")
        .append(venue_price::trimmedDecimal(slPx))
        .append("\",")
        .append("\"stopLossTriggerType\":\"mark_price\",");
  }
  if (tpPrice > 0)
  {
    body.append("\"stopSurplusTriggerPrice\":\"")
        .append(venue_price::trimmedDecimal(tpPx))
        .append("\",")
        .append("\"stopSurplusTriggerType\":\"mark_price\",");
  }
  body.append("\"clientOid\":\"").append(std::to_string(localId)).append("\"}");

  _client->post(
      std::string(kPathPosTpsl), body,
      [this, tracked](std::string_view resp)
      {
        const BitgetAck ack = readBitgetAck(resp);
        if (!ack.ok)
        {
          publishRejection(tracked, "placePosTpsl: " + ack.reason);
          return;
        }
        // /data is an array; the first orderId is the one tracked.
        if (!ack.orderId.empty())
        {
          _orderTracker->onSubmitted(tracked, ack.orderId);
        }
      },
      [this, tracked](std::string_view err)
      {
        publishRejection(tracked, "placePosTpsl: transport error: " + std::string(err));
      });
}

template <typename Policies>
void BitgetOrderExecutorT<Policies>::modifyPosTpsl(SymbolId symbol,
                                                   const std::string& exchangeOrderId,
                                                   double newTriggerPrice, double qty)
{
  _policies.rateLimit.gate(
      kNoOrderId,
      [this, symbol, exchangeOrderId, newTriggerPrice, qty]
      {
        sendModifyPosTpsl(symbol, exchangeOrderId, newTriggerPrice, qty);
      },
      [exchangeOrderId]
      {
        LRVX_LOG_WARN("[BitgetOE] modifyPosTpsl for "
                      << exchangeOrderId
                      << " refused by the client-side rate limit: the stop "
                         "stays where the venue last accepted it");
      });
}

template <typename Policies>
void BitgetOrderExecutorT<Policies>::sendModifyPosTpsl(SymbolId symbol,
                                                       const std::string& exchangeOrderId,
                                                       double newTriggerPrice, double qty)
{
  // The order being moved, as the tracker knows it, so a refusal reaches the
  // strategy as REPLACE_REJECTED on that order rather than as a log line
  // while the stop stays where the venue last accepted it.
  Order current;
  current.symbol = symbol;
  if (auto localId = _orderTracker->findLocalIdByExchangeOrderId(exchangeOrderId))
  {
    if (auto st = _orderTracker->get(*localId))
    {
      current = st->localOrder;
    }
  }
  Order moved = current;
  moved.triggerPrice = Price::fromDouble(newTriggerPrice);
  moved.quantity = Quantity::fromDouble(qty);

  auto info = _registry->getSymbolInfo(symbol);
  if (!info)
  {
    publishReplaceRejected(current, moved, "modifyPosTpsl: unknown symbol");
    return;
  }
  if (venue_price::isOffTick(moved.triggerPrice, *info))
  {
    publishReplaceRejected(current, moved, kRejectTriggerOffTick);
    return;
  }

  std::string body;
  body.reserve(256);
  body.append("{\"orderId\":\"")
      .append(exchangeOrderId)
      .append("\",")
      .append("\"symbol\":\"")
      .append(info->symbol)
      .append("\",")
      .append("\"productType\":\"")
      .append(_params.productType)
      .append("\",")
      .append("\"marginCoin\":\"")
      .append(_params.marginCoin)
      .append("\",")
      .append("\"size\":\"")
      .append(trimDouble(qty, /*max_decimals=*/4))
      .append("\",")
      .append("\"triggerPrice\":\"")
      .append(venue_price::trimmedDecimal(moved.triggerPrice))
      .append("\",")
      .append("\"triggerType\":\"mark_price\"}");

  _client->post(
      std::string(kPathModifyTpsl), body,
      [this, current, moved, exchangeOrderId](std::string_view resp)
      {
        const BitgetAck ack = readBitgetAck(resp);
        if (!ack.ok)
        {
          publishReplaceRejected(
              current, moved,
              "modifyPosTpsl: " + ack.reason + " (orderId=" + exchangeOrderId + ")");
        }
      },
      [this, current, moved, exchangeOrderId](std::string_view err)
      {
        publishReplaceRejected(current, moved,
                               "modifyPosTpsl: transport error: " + std::string(err) +
                                   " (orderId=" + exchangeOrderId + ")");
      });
}

// Explicit instantiations
template class BitgetOrderExecutorT<NoPolicies>;
template class BitgetOrderExecutorT<WithRateLimit>;
template class BitgetOrderExecutorT<WithTimeout>;
template class BitgetOrderExecutorT<FullPolicies>;

}  // namespace lrvx
