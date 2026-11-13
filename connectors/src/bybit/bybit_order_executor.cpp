/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "lrvx-connectors/bybit/bybit_order_executor.h"
#include "lrvx-connectors/bybit/authenticated_rest_client.h"
#include "lrvx-connectors/execution/order_tif.h"
#include "lrvx-connectors/execution/venue_price.h"

#include <lrvx/common.h>
#include <lrvx/engine/symbol_registry.h>
#include <lrvx/execution/events/order_event.h>
#include <lrvx/execution/order_tracker.h>
#include <lrvx/log/log.h>

#include <simdjson.h>

namespace lrvx
{

template <typename Policies>
BybitOrderExecutorT<Policies>::~BybitOrderExecutorT() = default;

namespace
{

// Bybit V5's tif token spelling; "" means the tif has no wire representation
// on this venue (GTD) and the caller must reject before building a body.
std::string_view bybitTifToken(NormalizedTif tif)
{
  switch (tif)
  {
    case NormalizedTif::GTC:
      return "GTC";
    case NormalizedTif::IOC:
      return "IOC";
    case NormalizedTif::FOK:
      return "FOK";
    case NormalizedTif::POST_ONLY:
      return "PostOnly";
    case NormalizedTif::UNSUPPORTED:
      return "";
  }
  return "";
}

// Standard stop/take-profit trigger-direction convention (rise=1, fall=2):
// a stop is placed on the far side of the current price from a favourable
// move, a take-profit on the near side. Independent of Bybit-account state,
// unlike the tradeSide/posMode question elsewhere in this file -- this is just "which way
// does the trigger price sit relative to the position being protected".
int triggerDirectionFor(OrderType type, Side side)
{
  const bool isStop = (type == OrderType::STOP_MARKET || type == OrderType::STOP_LIMIT);
  const bool sellSide = (side == Side::SELL);
  if (isStop)
  {
    return sellSide ? 2 : 1;  // sell-stop fires on the way down, buy-stop on the way up
  }
  return sellSide ? 1 : 2;  // sell take-profit fires on the way up, buy on the way down
}

bool isConditionalOrder(OrderType type)
{
  return type == OrderType::STOP_MARKET || type == OrderType::STOP_LIMIT ||
         type == OrderType::TAKE_PROFIT_MARKET || type == OrderType::TAKE_PROFIT_LIMIT;
}

// Bybit v5 answers {"retCode":0,...} on success and a non-zero retCode with a
// retMsg on refusal. A body that is not that shape (a gateway's HTML page, a
// truncated answer) is reported as a refusal too: the order's fate is then
// unknown, and saying nothing was the one answer known to be wrong.
struct BybitAck
{
  bool ok{false};
  std::string reason;
  std::string orderId;
};

BybitAck readBybitAck(std::string_view response)
{
  BybitAck out;
  simdjson::dom::parser parser;
  simdjson::padded_string padded(response);
  auto parsed = parser.parse(padded);
  if (parsed.error())
  {
    out.reason = "unparseable response: " + std::string(response.substr(0, 200));
    return out;
  }
  auto doc = parsed.value();
  auto retCode = doc["retCode"].get_int64();
  if (retCode.error())
  {
    out.reason = "response has no retCode: " + std::string(response.substr(0, 200));
    return out;
  }
  if (retCode.value_unsafe() != 0)
  {
    std::string_view msg;
    (void)doc["retMsg"].get_string().get(msg);
    out.reason =
        "retCode=" + std::to_string(retCode.value_unsafe()) + " retMsg=" + std::string(msg);
    return out;
  }
  out.ok = true;
  std::string_view id;
  if (!doc["result"]["orderId"].get_string().get(id))
  {
    out.orderId = std::string(id);
  }
  return out;
}

}  // namespace

template <typename Policies>
void BybitOrderExecutorT<Policies>::publishRejection(const Order& order, const std::string& reason)
{
  LRVX_LOG_ERROR("[BybitOrderExecutor] " << reason << " (orderId=" << order.id << ")");
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
void BybitOrderExecutorT<Policies>::publishCancelRejected(const Order& order,
                                                          const std::string& reason)
{
  LRVX_LOG_ERROR("[BybitOrderExecutor] cancel rejected: " << reason << " (orderId=" << order.id
                                                          << ")");
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
void BybitOrderExecutorT<Policies>::publishReplaceRejected(const Order& order,
                                                           const Order& newOrder,
                                                           const std::string& reason)
{
  LRVX_LOG_ERROR("[BybitOrderExecutor] replace rejected: " << reason << " (orderId=" << order.id
                                                           << ")");
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

template <typename Policies>
void BybitOrderExecutorT<Policies>::publishRateLimited(const Order& order)
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
void BybitOrderExecutorT<Policies>::submitOrder(const Order& order)
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
void BybitOrderExecutorT<Policies>::sendSubmitOrder(const Order& order)
{
  auto info = _registry->getSymbolInfo(order.symbol);
  if (!info.has_value())
  {
    publishRejection(order, "No symbol info registered");
    return;
  }

  if (order.type == OrderType::TRAILING_STOP)
  {
    // Bybit's trailing stop is a position-attached parameter on
    // /v5/position/trading-stop, not an order/create body -- there is no
    // wire shape here that would make this a real trailing stop rather
    // than a resting limit the strategy mistakes for one. Reject loudly
    // instead of sending something that looks accepted but protects
    // nothing.
    publishRejection(order, "Bybit trailing stop is not supported via order/create");
    return;
  }
  if (order.type == OrderType::ICEBERG)
  {
    publishRejection(order, "Bybit iceberg (visible-quantity) orders are not supported");
    return;
  }

  const NormalizedTif tif = normalizeTif(order.timeInForce, order.flags.postOnly);
  if (tif == NormalizedTif::UNSUPPORTED)
  {
    publishRejection(order, "Bybit has no native good-till-date order");
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

  const bool isConditional = isConditionalOrder(order.type);
  const bool priceIsLimit =
      (order.type == OrderType::LIMIT || order.type == OrderType::STOP_LIMIT ||
       order.type == OrderType::TAKE_PROFIT_LIMIT);

  std::string body;
  body.reserve(256);
  body.append("{\"category\":\"").append(Bybit::toString(info->type)).append("\",");
  body.append("\"symbol\":\"").append(info->symbol).append("\",");
  // The client order id. Bybit echoes it on every private "order" and
  // "execution" frame for this order, and it is the only id the engine's
  // OrderTracker and position book recognise -- without it every fill comes
  // back under the venue's own orderId and matches nothing. Bybit allows up to
  // 36 characters here; a decimal OrderId is far inside that.
  body.append("\"orderLinkId\":\"").append(std::to_string(order.id)).append("\",");
  body.append("\"side\":\"").append(order.side == Side::BUY ? "Buy" : "Sell").append("\",");
  body.append("\"orderType\":\"").append(priceIsLimit ? "Limit" : "Market").append("\",");
  body.append("\"qty\":\"").append(order.quantity.toString()).append("\",");
  // Bybit reads the qty of a spot market buy as quote currency unless told
  // otherwise ("Spot: Market Buy order by value by default", v5 order/create).
  // Order::quantity is base asset everywhere in the engine, and the lot check
  // above is against the base step, so every spot market order names its unit.
  if (!priceIsLimit && info->type == InstrumentType::Spot)
  {
    body.append("\"marketUnit\":\"baseCoin\",");
  }
  if (priceIsLimit)
  {
    body.append("\"price\":\"").append(venue_price::wireDecimal(order.price)).append("\",");
  }
  body.append("\"timeInForce\":\"").append(bybitTifToken(tif)).append("\",");
  body.append("\"reduceOnly\":").append(order.flags.reduceOnly ? "true" : "false");
  if (isConditional)
  {
    body.append(",\"triggerPrice\":\"")
        .append(venue_price::wireDecimal(order.triggerPrice))
        .append("\"");
    body.append(",\"triggerDirection\":")
        .append(std::to_string(triggerDirectionFor(order.type, order.side)));
  }
  body.append("}");

  LRVX_LOG("[BybitOrderExecutor] Submitting order: id="
           << order.id << " symbol=" << info->symbol << " side="
           << (order.side == Side::BUY ? "Buy" : "Sell") << " qty=" << order.quantity.toDouble()
           << " price=" << order.price.toDouble() << " category=" << Bybit::toString(info->type));

  _policies.timeout.trackSubmit(order.id);

  _client->post(
      "/v5/order/create", body,
      [this, order](std::string_view response)
      {
        const BybitAck ack = readBybitAck(response);
        if (!ack.ok)
        {
          // clearPending() runs AFTER the retCode check now: on the old
          // ordering, a rejected submit disarmed the timeout watchdog in
          // the same line that discovered the rejection, so the one
          // safety net meant to catch a hung exchange never had a chance
          // to fire on a hung-then-erroring one either.
          publishRejection(order, "Order submission failed: " + ack.reason);
          _policies.timeout.clearPending(order.id);
          return;
        }

        const std::string& orderId = ack.orderId;

        LRVX_LOG("[BybitOrderExecutor] Order submitted. id=" << order.id
                                                             << " → exchangeOrderId=" << orderId);

        _orderTracker->onSubmitted(order, orderId);
        _policies.timeout.clearPending(order.id);
      },
      [this, order](std::string_view error)
      {
        publishRejection(order, std::string("Transport error: ") + std::string(error));
        _policies.timeout.clearPending(order.id);
      });
}

template <typename Policies>
void BybitOrderExecutorT<Policies>::cancelOrder(OrderId orderId)
{
  auto state = _orderTracker->get(orderId);
  if (!state)
  {
    LRVX_LOG_ERROR("[BybitOrderExecutor] Cannot cancel, unknown orderId=" << orderId);
    return;
  }

  // Resolved before the rate-limit gate (rather than after, as submit/
  // replace do) so a refused cancel can still be reported against the order
  // it was trying to cancel: a silently dropped cancel is worse than a
  // silently dropped submit, since the tracker keeps reporting the order
  // active with no hint that the cancel never left the process.
  Order target = state->localOrder;
  _policies.rateLimit.gate(
      orderId,
      [this, orderId]
      {
        sendCancelOrder(orderId);
      },
      [this, target]
      {
        publishRateLimited(target);
      });
}

template <typename Policies>
void BybitOrderExecutorT<Policies>::sendCancelOrder(OrderId orderId)
{
  auto state = _orderTracker->get(orderId);
  if (!state)
  {
    LRVX_LOG_ERROR("[BybitOrderExecutor] Cannot cancel, unknown orderId=" << orderId);
    return;
  }

  const Order target = state->localOrder;
  auto info = _registry->getSymbolInfo(target.symbol);
  if (!info.has_value())
  {
    publishCancelRejected(target, "no symbol info registered");
    return;
  }

  const std::string& exchangeOrderId = state->exchangeOrderId;

  std::string body;
  body.reserve(128);
  body.append("{\"category\":\"").append(Bybit::toString(info->type)).append("\",");
  body.append("\"symbol\":\"").append(info->symbol).append("\",");
  body.append("\"orderId\":\"").append(exchangeOrderId).append("\"}");

  LRVX_LOG_INFO("[BybitOrderExecutor] Cancelling order: localId=" << orderId << " exchangeId="
                                                                  << exchangeOrderId);

  _policies.timeout.trackCancel(orderId);

  _client->post(
      "/v5/order/cancel", body,
      [this, orderId, target](std::string_view response)
      {
        _policies.timeout.clearPending(orderId);

        const BybitAck ack = readBybitAck(response);
        if (!ack.ok)
        {
          publishCancelRejected(target, ack.reason);
          return;
        }
        LRVX_LOG_INFO("[BybitOrderExecutor] Cancel successful: orderId=" << orderId);
        _orderTracker->onCanceled(orderId);
      },
      [this, orderId, target](std::string_view err)
      {
        _policies.timeout.clearPending(orderId);
        publishCancelRejected(target, "transport error: " + std::string(err));
      });
}

template <typename Policies>
void BybitOrderExecutorT<Policies>::replaceOrder(OrderId oldOrderId, const Order& newOrder)
{
  auto state = _orderTracker->get(oldOrderId);
  if (!state)
  {
    LRVX_LOG_ERROR("[BybitOrderExecutor] Cannot replace, unknown orderId=" << oldOrderId);
    return;
  }

  Order target = state->localOrder;
  _policies.rateLimit.gate(
      oldOrderId,
      [this, oldOrderId, newOrder]
      {
        sendReplaceOrder(oldOrderId, newOrder);
      },
      [this, target]
      {
        publishRateLimited(target);
      });
}

template <typename Policies>
void BybitOrderExecutorT<Policies>::sendReplaceOrder(OrderId oldOrderId, const Order& newOrder)
{
  auto state = _orderTracker->get(oldOrderId);
  if (!state)
  {
    LRVX_LOG_ERROR("[BybitOrderExecutor] Cannot replace, unknown orderId=" << oldOrderId);
    return;
  }
  const Order target = state->localOrder;

  // The amended order lives on the original's symbol; a replacement built
  // from a price and a size alone (what a strategy's modify sends) carries
  // no symbol of its own.
  auto info = _registry->getSymbolInfo(target.symbol);
  if (!info.has_value())
  {
    publishReplaceRejected(target, newOrder, "no symbol info registered");
    return;
  }
  if (venue_price::isOffTick(newOrder.price, *info))
  {
    publishReplaceRejected(target, newOrder, kRejectPriceOffTick);
    return;
  }
  if (const char* offLot = venue_price::offLotReason(newOrder, *info))
  {
    publishReplaceRejected(target, newOrder, offLot);
    return;
  }

  const std::string& exchangeOrderId = state->exchangeOrderId;

  std::string qty = newOrder.quantity.toString();
  std::string price = venue_price::wireDecimal(newOrder.price);

  std::string body;
  body.reserve(128 + qty.size() + price.size());
  body.append("{\"category\":\"").append(Bybit::toString(info->type)).append("\",");
  body.append("\"symbol\":\"").append(info->symbol).append("\",");
  body.append("\"orderId\":\"").append(exchangeOrderId).append("\",");
  body.append("\"qty\":\"").append(qty).append("\",");
  body.append("\"price\":\"").append(price).append("\"}");

  LRVX_LOG_INFO("[BybitOrderExecutor] Replacing order: localId="
                << oldOrderId << " exchangeId=" << exchangeOrderId << " newQty=" << qty
                << " newPrice=" << price);

  _policies.timeout.trackReplace(oldOrderId);

  _client->post(
      "/v5/order/amend", body,
      [this, oldOrderId, newOrder, exchangeOrderId, target](std::string_view response)
      {
        _policies.timeout.clearPending(oldOrderId);

        const BybitAck ack = readBybitAck(response);
        if (!ack.ok)
        {
          publishReplaceRejected(target, newOrder, ack.reason);
          return;
        }

        // The amend response carries the (unchanged) exchange order id in
        // result.orderId; onReplaced() used to be called with a literal ""
        // here instead of reading it, so the tracker's new record had no
        // exchange id at all -- a subsequent cancelOrder() built
        // {"orderId":""}, the venue rejected it, and that rejection was
        // itself swallowed, leaving the order live on the
        // exchange and locally stuck "active" forever.
        std::string_view newExchangeId = exchangeOrderId;
        if (!ack.orderId.empty())
        {
          newExchangeId = ack.orderId;
        }

        LRVX_LOG_INFO("[BybitOrderExecutor] Replace successful: orderId=" << oldOrderId);
        _orderTracker->onReplaced(oldOrderId, newOrder, newExchangeId);
      },
      [this, oldOrderId, newOrder, target](std::string_view err)
      {
        _policies.timeout.clearPending(oldOrderId);
        publishReplaceRejected(target, newOrder, "transport error: " + std::string(err));
      });
}

// Explicit instantiations
template class BybitOrderExecutorT<NoPolicies>;
template class BybitOrderExecutorT<WithRateLimit>;
template class BybitOrderExecutorT<WithTimeout>;
template class BybitOrderExecutorT<FullPolicies>;

}  // namespace lrvx
