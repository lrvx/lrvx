/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "lrvx/common.h"

namespace lrvx
{

enum class SignalType : uint8_t
{
  Market,
  Limit,
  Cancel,
  CancelAll,
  Modify,
  StopMarket,
  StopLimit,
  TakeProfitMarket,
  TakeProfitLimit,
  TrailingStop,
  OCO,
  // DEX/AMM liquidity provision. ProvideLiquidity supplies liquidity to a
  // pool over [priceLower, priceUpper]; WithdrawLiquidity removes it. The
  // pool is identified by `symbol`. Executed by the on-chain connector, not
  // the CEX/backtest path.
  ProvideLiquidity,
  WithdrawLiquidity
};

struct Signal
{
  SignalType type{SignalType::Market};
  SymbolId symbol{};
  Side side{};
  Price price{};
  Quantity quantity{};
  OrderId orderId{};
  Price newPrice{};        // for Modify
  Quantity newQuantity{};  // for Modify

  // Advanced order fields
  Price triggerPrice{};
  Price trailingOffset{};
  int32_t trailingCallbackRate{0};  // bps, 100 = 1%
  TimeInForce timeInForce{TimeInForce::GTC};
  bool reduceOnly{false};
  bool postOnly{false};

  // OCO: linked order id
  OrderId linkedOrderId{};

  // DEX/AMM liquidity provision. priceLower/priceUpper bound the range and
  // liquidity is the position size. Zero on non-LP signals.
  Price priceLower{};
  Price priceUpper{};
  Quantity liquidity{};

  // Basic orders
  static Signal marketBuy(SymbolId sym, Quantity qty, OrderId id)
  {
    Signal s{};
    s.type = SignalType::Market;
    s.symbol = sym;
    s.side = Side::BUY;
    s.quantity = qty;
    s.orderId = id;
    return s;
  }

  static Signal marketSell(SymbolId sym, Quantity qty, OrderId id)
  {
    Signal s{};
    s.type = SignalType::Market;
    s.symbol = sym;
    s.side = Side::SELL;
    s.quantity = qty;
    s.orderId = id;
    return s;
  }

  static Signal limitBuy(SymbolId sym, Price px, Quantity qty, OrderId id)
  {
    Signal s{};
    s.type = SignalType::Limit;
    s.symbol = sym;
    s.side = Side::BUY;
    s.price = px;
    s.quantity = qty;
    s.orderId = id;
    return s;
  }

  static Signal limitSell(SymbolId sym, Price px, Quantity qty, OrderId id)
  {
    Signal s{};
    s.type = SignalType::Limit;
    s.symbol = sym;
    s.side = Side::SELL;
    s.price = px;
    s.quantity = qty;
    s.orderId = id;
    return s;
  }

  static Signal cancel(OrderId id)
  {
    Signal s{};
    s.type = SignalType::Cancel;
    s.orderId = id;
    return s;
  }

  static Signal cancelAll(SymbolId sym)
  {
    Signal s{};
    s.type = SignalType::CancelAll;
    s.symbol = sym;
    return s;
  }

  static Signal modify(OrderId id, Price newPx, Quantity newQty)
  {
    Signal s{};
    s.type = SignalType::Modify;
    s.orderId = id;
    s.newPrice = newPx;
    s.newQuantity = newQty;
    return s;
  }

  // Stop orders
  static Signal stopMarket(SymbolId sym, Side side, Price trigger, Quantity qty, OrderId id)
  {
    Signal s{};
    s.type = SignalType::StopMarket;
    s.symbol = sym;
    s.side = side;
    s.triggerPrice = trigger;
    s.quantity = qty;
    s.orderId = id;
    return s;
  }

  static Signal stopLimit(SymbolId sym, Side side, Price trigger, Price limit, Quantity qty,
                          OrderId id)
  {
    Signal s{};
    s.type = SignalType::StopLimit;
    s.symbol = sym;
    s.side = side;
    s.triggerPrice = trigger;
    s.price = limit;
    s.quantity = qty;
    s.orderId = id;
    return s;
  }

  // Take profit orders
  static Signal takeProfitMarket(SymbolId sym, Side side, Price trigger, Quantity qty, OrderId id)
  {
    Signal s{};
    s.type = SignalType::TakeProfitMarket;
    s.symbol = sym;
    s.side = side;
    s.triggerPrice = trigger;
    s.quantity = qty;
    s.orderId = id;
    return s;
  }

  static Signal takeProfitLimit(SymbolId sym, Side side, Price trigger, Price limit, Quantity qty,
                                OrderId id)
  {
    Signal s{};
    s.type = SignalType::TakeProfitLimit;
    s.symbol = sym;
    s.side = side;
    s.triggerPrice = trigger;
    s.price = limit;
    s.quantity = qty;
    s.orderId = id;
    return s;
  }

  // Trailing stop
  static Signal trailingStop(SymbolId sym, Side side, Price offset, Quantity qty, OrderId id)
  {
    Signal s{};
    s.type = SignalType::TrailingStop;
    s.symbol = sym;
    s.side = side;
    s.trailingOffset = offset;
    s.quantity = qty;
    s.orderId = id;
    return s;
  }

  static Signal trailingStopPercent(SymbolId sym, Side side, int32_t callbackBps, Quantity qty,
                                    OrderId id)
  {
    Signal s{};
    s.type = SignalType::TrailingStop;
    s.symbol = sym;
    s.side = side;
    s.trailingCallbackRate = callbackBps;
    s.quantity = qty;
    s.orderId = id;
    return s;
  }

  // OCO (one-cancels-other)
  static Signal oco(SymbolId sym, Side side, Price price1, Price price2, Quantity qty, OrderId id)
  {
    Signal s{};
    s.type = SignalType::OCO;
    s.symbol = sym;
    s.side = side;
    s.price = price1;
    s.triggerPrice = price2;  // reuse for second price
    s.quantity = qty;
    s.orderId = id;
    return s;
  }

  // DEX/AMM liquidity provision
  static Signal provideLiquidity(SymbolId pool, Price priceLower, Price priceUpper,
                                 Quantity liquidity, OrderId id)
  {
    Signal s{};
    s.type = SignalType::ProvideLiquidity;
    s.symbol = pool;
    s.priceLower = priceLower;
    s.priceUpper = priceUpper;
    s.liquidity = liquidity;
    s.orderId = id;
    return s;
  }

  static Signal withdrawLiquidity(SymbolId pool, Quantity liquidity, OrderId id)
  {
    Signal s{};
    s.type = SignalType::WithdrawLiquidity;
    s.symbol = pool;
    s.liquidity = liquidity;
    s.orderId = id;
    return s;
  }

  // With TimeInForce modifiers
  Signal& withTimeInForce(TimeInForce tif)
  {
    timeInForce = tif;
    return *this;
  }

  Signal& withReduceOnly(bool val = true)
  {
    reduceOnly = val;
    return *this;
  }

  Signal& withPostOnly(bool val = true)
  {
    postOnly = val;
    return *this;
  }
};

// The one rule for which signals the pre-trade gates (kill switch, order
// validator, risk manager) are not asked about, on every runner: a Cancel, a
// CancelAll and a Modify add no exposure, and a reduce-only order must get
// through so that a tightened limit cannot hold a strategy in a position.
constexpr bool passesPreTradeGatesUnasked(const Signal& signal) noexcept
{
  switch (signal.type)
  {
    case SignalType::Cancel:
    case SignalType::CancelAll:
    case SignalType::Modify:
      return true;
    default:
      return signal.reduceOnly;
  }
}

}  // namespace lrvx
