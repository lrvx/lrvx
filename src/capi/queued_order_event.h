/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2026 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "lrvx/common.h"
#include "lrvx/engine/event_dispatcher.h"
#include "lrvx/execution/events/order_event.h"

#include <array>
#include <cstdint>

namespace lrvx
{

// One order event a venue reported to a live engine, on its way to the
// strategies' order-event consumers.
struct QueuedOrderEvent
{
  struct Listener
  {
  };

  OrderEvent event;
  // The strategy the event belongs to; meaningless when toEveryone is set.
  SubscriberId owner{};
  bool toEveryone{false};
  // The last sequence claimed on the trade, book and bar buses when the event
  // was reported: the market data a strategy is handed before this event.
  std::array<int64_t, 3> marketHead{-1, -1, -1};
};

template <>
struct EventDispatcher<QueuedOrderEvent>
{
  template <typename Sub>
  static void dispatch(const QueuedOrderEvent& ev, Sub& sub)
  {
    sub.onQueuedOrderEvent(ev);
  }
};

}  // namespace lrvx
