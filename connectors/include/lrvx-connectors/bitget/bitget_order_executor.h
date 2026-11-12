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
#include <string_view>

namespace lrvx
{

class BitgetAuthenticatedRestClient;

namespace Bitget
{

inline std::string_view category(InstrumentType type)
{
  switch (type)
  {
    case InstrumentType::Spot:
      return "spot";
    case InstrumentType::Future:
    case InstrumentType::Inverse:
      return "mix";
    case InstrumentType::Option:
      return "option";
  }
  return "mix";
}

// Bitget's account-level position mode. tradeSide (open/close) and posSide
// (long/short) only mean what the venue's mix v2 API documents them to mean
// once the caller's account mode is known; sending them unconditionally,
// derived only from reduceOnly, is what caused a real bug. This makes the mode
// an explicit input instead of an assumption baked into every request body.
enum class PositionMode
{
  OneWay,
  Hedge,
};

struct Params
{
  std::string productType;
  std::string marginCoin;
  std::string marginMode;
  // Unused by the executor: "force" (GTC/IOC/FOK/post_only) is now derived
  // per order from Order::timeInForce / ExecutionFlags::postOnly, not from
  // one static value applied to every order regardless of what it asked
  // for. Kept on the struct for source compatibility with existing config
  // loaders that set it.
  std::string forcePolicy;
  PositionMode positionMode = PositionMode::OneWay;
};

}  // namespace Bitget

template <typename Policies = NoPolicies>
class BitgetOrderExecutorT : public IOrderExecutor
{
 public:
  using RateLimitPolicy = typename Policies::RateLimitType;
  using TimeoutPolicy = typename Policies::TimeoutType;

  explicit BitgetOrderExecutorT(std::unique_ptr<BitgetAuthenticatedRestClient> client,
                                SymbolRegistry* registry, OrderTracker* orderTracker,
                                Bitget::Params params)
      : _client(std::move(client)),
        _registry(registry),
        _orderTracker(orderTracker),
        _params(std::move(params))
  {
  }

  template <typename P = Policies, typename = std::enable_if_t<P::RateLimitType::enabled>>
  explicit BitgetOrderExecutorT(std::unique_ptr<BitgetAuthenticatedRestClient> client,
                                SymbolRegistry* registry, OrderTracker* orderTracker,
                                Bitget::Params params, RateLimitConfig rateLimitConfig)
      : _client(std::move(client)),
        _registry(registry),
        _orderTracker(orderTracker),
        _params(std::move(params))
  {
    _policies.rateLimit.init(std::move(rateLimitConfig));
  }

  template <typename P = Policies, typename = std::enable_if_t<P::TimeoutType::enabled>>
  explicit BitgetOrderExecutorT(std::unique_ptr<BitgetAuthenticatedRestClient> client,
                                SymbolRegistry* registry, OrderTracker* orderTracker,
                                Bitget::Params params, OrderTimeoutConfig timeoutConfig)
      : _client(std::move(client)),
        _registry(registry),
        _orderTracker(orderTracker),
        _params(std::move(params))
  {
    _policies.timeout.init(std::move(timeoutConfig));
    _policies.timeout.start();
  }

  template <typename P = Policies,
            typename = std::enable_if_t<P::RateLimitType::enabled && P::TimeoutType::enabled>>
  explicit BitgetOrderExecutorT(std::unique_ptr<BitgetAuthenticatedRestClient> client,
                                SymbolRegistry* registry, OrderTracker* orderTracker,
                                Bitget::Params params, RateLimitConfig rateLimitConfig,
                                OrderTimeoutConfig timeoutConfig)
      : _client(std::move(client)),
        _registry(registry),
        _orderTracker(orderTracker),
        _params(std::move(params))
  {
    _policies.rateLimit.init(std::move(rateLimitConfig));
    _policies.timeout.init(std::move(timeoutConfig));
    _policies.timeout.start();
  }

  ~BitgetOrderExecutorT() override;

  void submitOrder(const Order& order) override;
  void cancelOrder(OrderId orderId) override;
  void replaceOrder(OrderId oldOrderId, const Order& newOrder) override;

  void setOrderBus(OrderExecutionBus* bus) { _orderBus = bus; }
  void setLeverage(const std::string& symbol, int leverage);
  void submitOrderWithLeverage(const Order& order, int leverage, double slPrice = 0,
                               double tpPrice = 0);

  // Position-attached stop-loss (and optionally take-profit). Bitget's
  // /api/v2/mix/order/place-pos-tpsl endpoint creates a stop that trails the
  // position itself, identified by holdSide (long|short) in hedge mode. This
  // is preferable to a free-standing plan-order because (a) Bitget always
  // attaches it to the right side of the position and (b) we can move it via
  // /modify-tpsl-order without a cancel+place round-trip.
  //
  // Pass slPrice or tpPrice = 0 to skip that leg. holdSide must be
  // HoldSide::Long or HoldSide::Short.
  //
  // The returned exchangeOrderId is reported via the order bus on success and
  // recorded in the OrderTracker. If the call fails, the order is rejected.
  void placePosTpsl(SymbolId symbol, HoldSide holdSide, double slPrice, double tpPrice,
                    OrderId localId);

  // Modify an existing pos-tpsl order's trigger price (used by the kijun trail
  // to walk the SL up without cancel+replace).
  void modifyPosTpsl(SymbolId symbol, const std::string& exchangeOrderId, double newTriggerPrice,
                     double qty);

 private:
  // Every public entry point above is a gate, and every send* below is what
  // the gate runs once the venue budget allows it -- inline on the caller
  // when a token was there, on the rate-limit policy's sender thread when the
  // WAIT policy had to wait for one. Splitting them is what makes it possible
  // for a deferred request to be sent later instead of dropped, so a send*
  // must own or re-read everything it needs rather than borrow from the
  // caller's frame.
  void sendSubmitOrder(const Order& order);
  void sendCancelOrder(OrderId orderId);
  void sendReplaceOrder(OrderId oldOrderId, const Order& newOrder);
  void sendSetLeverage(const std::string& symbol, int leverage);
  void sendSubmitOrderWithLeverage(const Order& order, int leverage, double slPrice,
                                   double tpPrice);
  void sendPlacePosTpsl(SymbolId symbol, HoldSide holdSide, double slPrice, double tpPrice,
                        OrderId localId);
  void sendModifyPosTpsl(SymbolId symbol, const std::string& exchangeOrderId,
                         double newTriggerPrice, double qty);

  void submitPlanOrder(const Order& order, const SymbolInfo& info);
  void publishRejection(const Order& order, const std::string& reason);
  void publishRateLimited(const Order& order);
  // A cancel the venue refused, or that never reached it: REJECTED on the
  // order the cancel targeted, as the simulator reports a cancel it cannot
  // carry out. The tracker is left alone, because the order is still live.
  void publishCancelRejected(const Order& order, const std::string& reason);
  // A modify the venue refused, or that never reached it: REPLACE_REJECTED
  // with the order that stays live and the replacement that was refused.
  void publishReplaceRejected(const Order& order, const Order& newOrder, const std::string& reason);

  std::unique_ptr<BitgetAuthenticatedRestClient> _client;
  SymbolRegistry* _registry;
  OrderTracker* _orderTracker;
  Bitget::Params _params;
  Policies _policies;
  OrderExecutionBus* _orderBus = nullptr;
};

using BitgetOrderExecutor = BitgetOrderExecutorT<NoPolicies>;
using BitgetOrderExecutorWithRateLimit = BitgetOrderExecutorT<WithRateLimit>;
using BitgetOrderExecutorWithTimeout = BitgetOrderExecutorT<WithTimeout>;
using BitgetOrderExecutorFull = BitgetOrderExecutorT<FullPolicies>;

extern template class BitgetOrderExecutorT<NoPolicies>;
extern template class BitgetOrderExecutorT<WithRateLimit>;
extern template class BitgetOrderExecutorT<WithTimeout>;
extern template class BitgetOrderExecutorT<FullPolicies>;

}  // namespace lrvx
