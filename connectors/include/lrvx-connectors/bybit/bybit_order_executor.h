/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "lrvx-connectors/execution/executor_policies.h"

#include <lrvx/engine/symbol_registry.h>
#include <lrvx/execution/abstract_executor.h>
#include <lrvx/execution/bus/order_execution_bus.h>
#include <lrvx/execution/order.h>
#include <lrvx/execution/order_tracker.h>

#include <memory>
#include <string>

namespace lrvx
{

class AuthenticatedRestClient;

namespace Bybit
{

inline std::string_view toString(InstrumentType type)
{
  switch (type)
  {
    case InstrumentType::Spot:
      return "spot";
    case InstrumentType::Future:
      return "linear";
    case InstrumentType::Inverse:
      return "inverse";
    case InstrumentType::Option:
      return "option";
  }

  return "unknown";
}

}  // namespace Bybit

template <typename Policies = NoPolicies>
class BybitOrderExecutorT : public IOrderExecutor
{
 public:
  using RateLimitPolicy = typename Policies::RateLimitType;
  using TimeoutPolicy = typename Policies::TimeoutType;

  explicit BybitOrderExecutorT(std::unique_ptr<AuthenticatedRestClient> client,
                               SymbolRegistry* registry, OrderTracker* orderTracker)
      : _client(std::move(client)), _registry(registry), _orderTracker(orderTracker)
  {
  }

  template <typename P = Policies, typename = std::enable_if_t<P::RateLimitType::enabled>>
  explicit BybitOrderExecutorT(std::unique_ptr<AuthenticatedRestClient> client,
                               SymbolRegistry* registry, OrderTracker* orderTracker,
                               RateLimitConfig rateLimitConfig)
      : _client(std::move(client)), _registry(registry), _orderTracker(orderTracker)
  {
    _policies.rateLimit.init(std::move(rateLimitConfig));
  }

  template <typename P = Policies, typename = std::enable_if_t<P::TimeoutType::enabled>>
  explicit BybitOrderExecutorT(std::unique_ptr<AuthenticatedRestClient> client,
                               SymbolRegistry* registry, OrderTracker* orderTracker,
                               OrderTimeoutConfig timeoutConfig)
      : _client(std::move(client)), _registry(registry), _orderTracker(orderTracker)
  {
    _policies.timeout.init(std::move(timeoutConfig));
    _policies.timeout.start();
  }

  template <typename P = Policies,
            typename = std::enable_if_t<P::RateLimitType::enabled && P::TimeoutType::enabled>>
  explicit BybitOrderExecutorT(std::unique_ptr<AuthenticatedRestClient> client,
                               SymbolRegistry* registry, OrderTracker* orderTracker,
                               RateLimitConfig rateLimitConfig, OrderTimeoutConfig timeoutConfig)
      : _client(std::move(client)), _registry(registry), _orderTracker(orderTracker)
  {
    _policies.rateLimit.init(std::move(rateLimitConfig));
    _policies.timeout.init(std::move(timeoutConfig));
    _policies.timeout.start();
  }

  ~BybitOrderExecutorT() override;

  void submitOrder(const Order& order) override;
  void cancelOrder(OrderId orderId) override;
  void replaceOrder(OrderId oldOrderId, const Order& newOrder) override;

  // Without this, a venue rejection was logged and dropped: no event on any
  // bus, no tracker record, and (since clearPending() ran before the
  // retCode check) the timeout watchdog disarmed itself at the exact moment
  // it should have fired. Bitget already has this; Bybit did not.
  void setOrderBus(OrderExecutionBus* bus) { _orderBus = bus; }

 private:
  // The gate decides whether the request may leave and when; these are what
  // it runs -- inline on the caller when the budget had a token, on the
  // rate-limit policy's sender thread when the WAIT policy had to wait for
  // one. A deferred send runs after its entry point returned, so it re-reads
  // the tracker rather than borrowing state from the caller's frame.
  void sendSubmitOrder(const Order& order);
  void sendCancelOrder(OrderId orderId);
  void sendReplaceOrder(OrderId oldOrderId, const Order& newOrder);

  void publishRejection(const Order& order, const std::string& reason);
  void publishRateLimited(const Order& order);
  // A cancel the venue refused, or that never reached it. Published as
  // REJECTED on the order the cancel targeted, as the simulator does for a
  // cancel it cannot carry out; the tracker is left alone, because the order
  // is still live.
  void publishCancelRejected(const Order& order, const std::string& reason);
  // An amend the venue refused, or that never reached it: REPLACE_REJECTED
  // with the order that stays live and the replacement that was refused.
  void publishReplaceRejected(const Order& order, const Order& newOrder, const std::string& reason);

  std::unique_ptr<AuthenticatedRestClient> _client;
  SymbolRegistry* _registry;
  OrderTracker* _orderTracker;
  Policies _policies;
  OrderExecutionBus* _orderBus = nullptr;
};

using BybitOrderExecutor = BybitOrderExecutorT<NoPolicies>;
using BybitOrderExecutorWithRateLimit = BybitOrderExecutorT<WithRateLimit>;
using BybitOrderExecutorWithTimeout = BybitOrderExecutorT<WithTimeout>;
using BybitOrderExecutorFull = BybitOrderExecutorT<FullPolicies>;

extern template class BybitOrderExecutorT<NoPolicies>;
extern template class BybitOrderExecutorT<WithRateLimit>;
extern template class BybitOrderExecutorT<WithTimeout>;
extern template class BybitOrderExecutorT<FullPolicies>;

}  // namespace lrvx
