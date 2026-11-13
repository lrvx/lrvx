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
#include "lrvx/execution/order.h"

namespace lrvx
{

/// Apply a Modify's new price and quantity to `order`, the order it modifies.
/// Shared by BacktestRunner and the Python SimulatedExecutor binding so a
/// modify means the same on both.
inline void applyModify(Order& order, Price newPrice, Quantity newQuantity)
{
  // The one price a Modify carries is the price the order works at: the
  // trigger of a stop, take-profit or trailing order that fills at market,
  // the limit of everything else (a stop-limit keeps its trigger).
  switch (order.type)
  {
    case OrderType::STOP_MARKET:
    case OrderType::TAKE_PROFIT_MARKET:
    case OrderType::TRAILING_STOP:
      order.triggerPrice = newPrice;
      break;
    default:
      order.price = newPrice;
      break;
  }
  order.quantity = newQuantity;
  // The replacement is a fresh order: newQuantity is what is left to work.
  order.filledQuantity = Quantity{};
}

}  // namespace lrvx
