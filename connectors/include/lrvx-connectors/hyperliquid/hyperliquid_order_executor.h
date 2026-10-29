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
#include <lrvx/execution/bus/order_execution_bus.h>
#include <lrvx/execution/order_tracker.h>
#include <lrvx/log/abstract_logger.h>
#include <lrvx/net/abstract_transport.h>

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lrvx
{

// Hyperliquid has no market-order primitive. Its own SDK (Exchange.market_open
// -> _slippage_price in hyperliquid-python-sdk exchange.py) sends an IOC limit
// at reference * (1 + slippage) for a buy and reference * (1 - slippage) for a
// sell, rounded to 5 significant figures. This is the same computation, so a
// caller that wants a market order sets order.type = MARKET and
// order.price = marketPriceWithSlippage(mid, side, 0.05) (0.05 is the SDK's
// DEFAULT_SLIPPAGE). Prices of 100000 and above are rounded to an integer
// instead, which HL accepts regardless of significant figures. The executor
// still clamps the decimals to the asset's 6 - szDecimals when it sends.
// Returns a zero Price when reference is not positive or slippage is outside
// [0, 1), which submitOrder then rejects as an unpriced market order.
Price marketPriceWithSlippage(Price reference, Side side, double slippage);

template <typename Policies = NoPolicies>
class HyperliquidOrderExecutorT
{
 public:
  using RateLimitPolicy = typename Policies::RateLimitType;
  using TimeoutPolicy = typename Policies::TimeoutType;

  HyperliquidOrderExecutorT(std::string restUrl, std::string privateKeyHex,
                            SymbolRegistry* registry, OrderTracker* orderTracker,
                            std::shared_ptr<ILogger> logger, std::string accountAddress,
                            std::optional<std::string> vaultAddress, bool mainnet);

  // Transport-injecting overload. Production code always goes through the
  // constructor above, which owns a real CurlTransport; this one exists so
  // tests can hand in a fake ITransport and observe the exact request body
  // this executor builds, without a live signing helper or network access.
  HyperliquidOrderExecutorT(std::unique_ptr<ITransport> transport, std::string restUrl,
                            std::string privateKeyHex, SymbolRegistry* registry,
                            OrderTracker* orderTracker, std::shared_ptr<ILogger> logger,
                            std::string accountAddress, std::optional<std::string> vaultAddress,
                            bool mainnet);

  // Transport-injecting overload of the rate-limited constructor. The
  // rate-limited constructor below builds its own CurlTransport and fetches
  // the asset map over the network before it returns, so the limiter
  // could not be exercised without a network; this one takes both the
  // transport and the budget.
  //
  // Defined here rather than in the .cpp on purpose: an explicit
  // instantiation of a class template does not instantiate its member
  // templates, so a constructor template defined in the .cpp is not available
  // to any other translation unit.
  template <typename P = Policies, typename = std::enable_if_t<P::RateLimitType::enabled>>
  HyperliquidOrderExecutorT(std::unique_ptr<ITransport> transport, std::string restUrl,
                            std::string privateKeyHex, SymbolRegistry* registry,
                            OrderTracker* orderTracker, std::shared_ptr<ILogger> logger,
                            std::string accountAddress, std::optional<std::string> vaultAddress,
                            bool mainnet, RateLimitConfig rateLimitConfig)
      : _url(std::move(restUrl)),
        _privateKey(std::move(privateKeyHex)),
        _accountAddress(std::move(accountAddress)),
        _vaultAddress(std::move(vaultAddress)),
        _mainnet(mainnet),
        _registry(registry),
        _orderTracker(orderTracker),
        _logger(std::move(logger)),
        _transport(std::move(transport))
  {
    _policies.rateLimit.init(std::move(rateLimitConfig));
    loadAssetIds();
  }

  ~HyperliquidOrderExecutorT();

  template <typename P = Policies, typename = std::enable_if_t<P::RateLimitType::enabled>>
  HyperliquidOrderExecutorT(std::string restUrl, std::string privateKeyHex,
                            SymbolRegistry* registry, OrderTracker* orderTracker,
                            std::shared_ptr<ILogger> logger, std::string accountAddress,
                            std::optional<std::string> vaultAddress, bool mainnet,
                            RateLimitConfig rateLimitConfig);

  template <typename P = Policies, typename = std::enable_if_t<P::TimeoutType::enabled>>
  HyperliquidOrderExecutorT(std::string restUrl, std::string privateKeyHex,
                            SymbolRegistry* registry, OrderTracker* orderTracker,
                            std::shared_ptr<ILogger> logger, std::string accountAddress,
                            std::optional<std::string> vaultAddress, bool mainnet,
                            OrderTimeoutConfig timeoutConfig);

  template <typename P = Policies,
            typename = std::enable_if_t<P::RateLimitType::enabled && P::TimeoutType::enabled>>
  HyperliquidOrderExecutorT(std::string restUrl, std::string privateKeyHex,
                            SymbolRegistry* registry, OrderTracker* orderTracker,
                            std::shared_ptr<ILogger> logger, std::string accountAddress,
                            std::optional<std::string> vaultAddress, bool mainnet,
                            RateLimitConfig rateLimitConfig, OrderTimeoutConfig timeoutConfig);

  // Outcome of setLeverage. ok is true only when the venue answered
  // {"status":"ok"}; detail carries the venue's error text, the transport
  // error, or the reason the request never left the process.
  using LeverageCallback = std::function<void(bool ok, const std::string& detail)>;

  // submitOrder sends HL's "limit" order object for LIMIT and MARKET (MARKET
  // is an IOC limit at order.price, which must be set -- see
  // marketPriceWithSlippage) and HL's "trigger" object for STOP_MARKET,
  // STOP_LIMIT, TAKE_PROFIT_MARKET and TAKE_PROFIT_LIMIT, with
  // order.triggerPrice as triggerPx. TRAILING_STOP and ICEBERG have no HL
  // primitive and are rejected with a REJECTED event. A trigger order that
  // the venue accepts rests until it triggers; it is published as SUBMITTED
  // followed by PENDING_TRIGGER and stays cancellable through cancelOrder.
  void submitOrder(const Order& order);
  void cancelOrder(OrderId id);
  // Sends HL's modify action, addressed by the venue's oid or, when the venue
  // acknowledged the order without one, by its cloid. HL's modify without
  // always_place (which this connector does not send) takes a non-trigger
  // order only, with tif Alo or a Gtc that would not execute, which the venue
  // overrides to Alo; a trigger order passed as n is rejected with a REJECTED
  // event, cancel it and submit a new one instead.
  void replaceOrder(OrderId oldId, const Order& n);

  // Position-attached stop-loss and take-profit, the HL counterpart of
  // BitgetOrderExecutorT::placePosTpsl. Both legs go out as reduce-only
  // market trigger orders in ONE "order" action with grouping
  // "positionTpsl", so the venue ties them to the position and cancels them
  // when the position is closed by other means. positionSide is the side of
  // the open position (BUY = long); the legs trade the opposite side. Pass a
  // zero trigger to skip that leg. qty must be positive: HL documents a
  // position TP/SL without a size as closing the whole position, but the
  // wire form of that is not in the API docs, so this connector does not
  // guess it. slLocalId / tpLocalId are the engine order ids the legs are
  // tracked and published under; cancelOrder works on them like on any
  // other order.
  void placePosTpsl(SymbolId symbol, Side positionSide, Price slTrigger, Price tpTrigger,
                    Quantity qty, OrderId slLocalId, OrderId tpLocalId);

  // Sends HL's updateLeverage action for the symbol's asset, signed like
  // every other L1 action. The answer is logged and handed to onResult (on
  // the transport's thread, or inline when the request is refused before it
  // is sent) so a caller knows whether the leverage is in effect.
  void setLeverage(SymbolId symbol, int leverage, bool isCross, LeverageCallback onResult = {});

  // Without this the executor was mute: it held no bus and published no
  // OrderEvent on any path, so a fill Hyperliquid returned inline with the
  // submit reached no listener and a venue rejection was not even parsed --
  // the order was handed to the tracker as submitted and the strategy went on
  // believing a resting order existed. Same hook BybitOrderExecutorT has.
  void setOrderBus(OrderExecutionBus* bus) { _orderBus = bus; }

  // True once the venue's asset map (coin -> asset index, szDecimals) has
  // been fetched. Until then every order, cancel, TP/SL and leverage call is
  // refused, and each one starts a new fetch, at most once every 2 s, without
  // waiting for it; a caller that must not run without the map checks this
  // after construction.
  bool assetsLoaded() const;

 private:
  // One order of an "order" action: the engine order it stands for, its
  // serialized HL order wire and the cloid it was sent with.
  struct OrderLeg
  {
    Order order;
    std::string wire;
    std::string cloid;
  };

  // Constructor-time fetch: waits up to 15 s for the venue's answer.
  void loadAssetIds();
  // Lazy retry while the map is still empty, throttled to one attempt per
  // kAssetRetryInterval. Never waits for the answer; true when the map is
  // loaded on return.
  bool ensureAssetIds();
  // One meta request to _infoUrl, waiting up to `wait` for its answer, or
  // not at all without one. True when the map is loaded on return.
  bool fetchAssetIds(std::optional<std::chrono::milliseconds> wait);
  // Why no asset index is known for coin: the map is not loaded yet, or it
  // does not list the coin.
  std::string assetMissingReason(const std::string& coin) const;
  int assetIdFor(std::string_view coin);
  // szDecimals from the venue's meta response, -1 when the meta carried none.
  int szDecimalsFor(std::string_view coin);

  // Signs actionJson as an L1 action and wraps it in the /exchange request
  // body; nullopt when signing failed.
  std::optional<std::string> signedBody(const std::string& actionJson, uint64_t nonceMs);
  void sendOrderAction(std::vector<OrderLeg> legs, std::string_view grouping);
  void sendPlacePosTpsl(SymbolId symbol, Side positionSide, Price slTrigger, Price tpTrigger,
                        Quantity qty, OrderId slLocalId, OrderId tpLocalId);
  void sendSetLeverage(SymbolId symbol, int leverage, bool isCross, LeverageCallback onResult);

  // What the rate-limit gate runs once the venue budget allows the request:
  // inline on the caller when a token was there, on the policy's sender
  // thread when the WAIT policy had to wait for one. A deferred send runs
  // after its entry point returned, so it re-reads the tracker instead of
  // borrowing state from the caller's frame.
  void sendSubmitOrder(const Order& order);
  void sendCancelOrder(OrderId localId);
  void sendReplaceOrder(OrderId oldLocalId, const Order& n);

  void publishRejection(const Order& order, const std::string& reason);
  void publishRateLimited(const Order& order);
  void publishSubmitted(const Order& order);
  void publishPendingTrigger(const Order& order);
  void publishFill(const Order& order, Quantity fillQty, Price fillPrice);

  std::string _url;
  std::string _privateKey;
  std::string _accountAddress;
  std::optional<std::string> _vaultAddress;
  bool _mainnet{true};

  SymbolRegistry* _registry;
  OrderTracker* _orderTracker;
  std::shared_ptr<ILogger> _logger;
  std::unique_ptr<ITransport> _transport;
  OrderExecutionBus* _orderBus = nullptr;

  // Thread-safe asset ID cache
  mutable std::mutex _assetMutex;
  std::unordered_map<std::string, int> _assetIds;
  std::unordered_map<std::string, int> _szDecimals;
  bool _assetsLoaded{false};
  // The /info endpoint next to _url; set by the first loadAssetIds.
  std::string _infoUrl;
  std::chrono::steady_clock::time_point _lastAssetAttempt{};
  bool _assetAttempted{false};

  Policies _policies;
};

using HyperliquidOrderExecutor = HyperliquidOrderExecutorT<NoPolicies>;
using HyperliquidOrderExecutorWithRateLimit = HyperliquidOrderExecutorT<WithRateLimit>;
using HyperliquidOrderExecutorWithTimeout = HyperliquidOrderExecutorT<WithTimeout>;
using HyperliquidOrderExecutorFull = HyperliquidOrderExecutorT<FullPolicies>;

extern template class HyperliquidOrderExecutorT<NoPolicies>;
extern template class HyperliquidOrderExecutorT<WithRateLimit>;
extern template class HyperliquidOrderExecutorT<WithTimeout>;
extern template class HyperliquidOrderExecutorT<FullPolicies>;

}  // namespace lrvx
