# SimulatedExecutor

`SimulatedExecutor` simulates order execution against historical market data for backtesting. It supports slippage models on market-style fills and optional queue simulation for resting limit orders.

```cpp
class SimulatedExecutor : public IOrderExecutor
{
public:
  static constexpr size_t kMaxSymbols = 256;
  static constexpr size_t kDefaultOrderCapacity = 64;
  static constexpr size_t kDefaultFillCapacity = 4096;

  using OrderEventCallback = std::function<void(const OrderEvent&)>;

  explicit SimulatedExecutor(IClock& clock);

  void setOrderEventCallback(OrderEventCallback cb);

  // Apply slippage and queue-simulation settings from a BacktestConfig.
  void applyConfig(const BacktestConfig& config);

  // Convenience setters for callers without a full BacktestConfig.
  void setDefaultSlippage(const SlippageProfile& profile);
  void setSymbolSlippage(SymbolId symbol, const SlippageProfile& profile);
  void setQueueModel(QueueModel model, size_t depth);

  void start() override;
  void stop() override;

  void submitOrder(const Order& order) override;
  void cancelOrder(OrderId orderId) override;
  void cancelAllOrders(SymbolId symbol) override;
  void replaceOrder(OrderId oldOrderId, const Order& newOrder) override;
  void submitOCO(const OCOParams& params) override;

  ExchangeCapabilities capabilities() const override;

  void onBookUpdate(SymbolId symbol, const std::pmr::vector<BookLevel>& bids,
                    const std::pmr::vector<BookLevel>& asks);
  void onTrade(SymbolId symbol, Price price, bool isBuy);
  void onTrade(SymbolId symbol, Price price, Quantity qty, bool isBuy);
  void onBar(SymbolId symbol, Price close);
  void onBar(SymbolId symbol, Price high, Price low, Price close);
  void onBar(SymbolId symbol, Price open, Price high, Price low, Price close);

  // Bar-callback window: orders arriving while it is open are held for the
  // next bar's open instead of being matched against a bar already walked.
  // BarCallbackScope is the RAII pairing; prefer it to the raw calls.
  class BarCallbackScope { explicit BarCallbackScope(SimulatedExecutor&); };
  void beginBarCallbackWindow() noexcept;
  void endBarCallbackWindow() noexcept;
  bool barCallbackWindowOpen() const noexcept;
  size_t heldOrderCount() const noexcept;
  void releaseHeldOrders(SymbolId symbol);

  // Drop the state of a finished run, keep the configuration.
  void reset();

  const std::vector<Fill>& fills() const;
  std::vector<Fill> extractFills();
  const std::vector<Order>& conditionalOrders() const;

  // Top-of-book accessors. Return 0 when the side is empty.
  Price bestBidPrice(SymbolId symbol) const;
  Price bestAskPrice(SymbolId symbol) const;
  Price bookMidPrice(SymbolId symbol) const;  // 0 when either side is empty

  CompositeOrderLogic& compositeLogic();
};
```

`start()` and `stop()` are empty overrides. `compositeLogic()` is not `noexcept`.

### Brackets

A single call submits the entry leg and arms a take-profit plus stop pair on entry fill. The simulator
runs the state machine: the first child to fill cancels the other, and cancelling the bracket cancels
every still-live leg. Order ids are derived from the bracket id — `entry = bracketId * 3 + 0`,
`tp = bracketId * 3 + 1`, `stop = bracketId * 3 + 2`.

```cpp
void submitBracket(const BracketOrder& bracket);
void cancelBracket(uint64_t bracketId);
BracketStatus bracketStatus(uint64_t bracketId) const;

enum class BracketArmMode : uint8_t { OnFullFill = 0, OnPartialFill = 1 };
void setBracketChildArmMode(BracketArmMode mode) noexcept;
BracketArmMode bracketChildArmMode() const noexcept;
```

`OnFullFill` (the default) arms the children once, at full entry fill. `OnPartialFill` arms them at the
running entry-fill quantity on every partial, resizing via replace as more entry quantity fills.

### Iceberg

Applies to orders of `OrderType::ICEBERG`.

```cpp
void setIcebergRefreshLatency(int64_t latencyNs) noexcept;
int64_t icebergRefreshLatencyNs() const noexcept;

void setIcebergSizeRandomisationPct(double pct) noexcept;
double icebergSizeRandomisationPct() const noexcept;
void setIcebergJitterSeed(uint64_t seed) noexcept;

enum class IcebergPriorityMode : uint8_t { Back = 0, Retain = 1 };
void setIcebergPriorityMode(IcebergPriorityMode mode) noexcept;
IcebergPriorityMode icebergPriorityMode() const noexcept;
void setIcebergPriorityModeByName(const std::string& name) noexcept;

int64_t icebergHiddenRemainingRaw(OrderId id) const;  // 0 if not an iceberg
```

| Setting | Description |
|---------|-------------|
| `setIcebergRefreshLatency` | Delay between a visible tranche filling and the next being exposed. Applies to orders submitted after the call |
| `setIcebergSizeRandomisationPct` | Per-refresh visible-slice jitter as a fraction. `0.0` is deterministic, `0.10` is +/-10% uniform. Sampled from an internal RNG |
| `setIcebergJitterSeed` | Reseed that RNG to reproduce a specific draw sequence |
| `setIcebergPriorityMode` | `Back`: the refreshed slice goes to the back of the queue, behind whatever is still resting at the level (most crypto venues). `Retain`: it keeps the prior slice's queue position and trades on the next print (CME options, some Eurex contracts) |
| `icebergHiddenRemainingRaw` | Diagnostic: remaining hidden quantity, or 0 |

### Self-trade prevention

STP keys on `Order::accountId` plus an optional STP group. Two orders share an STP scope when their
`accountId`s are equal, or when both accounts map to the same non-zero group.

```cpp
void setSTPMode(STPMode mode) noexcept;
STPMode stpMode() const noexcept;

void setSTPGroupMembership(uint64_t accountId, uint64_t groupId);  // groupId 0 removes
uint64_t stpGroupFor(uint64_t accountId) const;
bool sameStpScope(uint64_t a, uint64_t b) const;
```

See [`STPMode`](../common.md#stpmode) for the modes.

### FOK semantics

```cpp
enum class FokMode : uint8_t { AnyPrice = 0, SinglePrice = 1 };
void setFokMode(FokMode mode) noexcept;
FokMode fokMode() const noexcept;
void setFokModeByName(const std::string& name);  // "any_price" | "single_price"
```

`AnyPrice` (the default, matching crypto venues) fills when cumulative liquidity at prices crossing
the order's limit is at least the order quantity. `SinglePrice` (CME, Eurex, most US equities) fills
only when the level at the limit price holds the whole quantity in one trade. The simulator currently
consults top-of-book quantity only. `setFokModeByName` is case-insensitive and ignores unknown values.

### Latency

```cpp
void setSubmitAckLatency(int64_t latencyNs, int64_t jitterNs);
void setCancelAckLatency(int64_t latencyNs, int64_t jitterNs);
void setReplaceAckLatency(int64_t latencyNs, int64_t jitterNs);

void setSubmitAckLatencyDistribution(const LatencyDistribution& dist);
void setCancelAckLatencyDistribution(const LatencyDistribution& dist);
void setReplaceAckLatencyDistribution(const LatencyDistribution& dist);

void applyLatencyProfile(const char* name);
```

The scalar setters delegate to the distribution setters: zero jitter becomes `Constant`, non-zero
becomes `Uniform` over `[base - jitter, base + jitter]`.

### Venue availability and rate limits

```cpp
void setVenueAvailability(VenueAvailability* availability);  // nullptr disables
VenueAvailability* venueAvailability() noexcept;

void setRateLimitPolicy(const RateLimitPolicy& policy);
void clearRateLimitPolicy();
bool hasRateLimitPolicy() const noexcept;
RateLimitPolicy& rateLimitPolicy();
```

Submit, cancel and replace issued while the venue is down are buffered and flushed at the recovery
edge in FIFO order. Market-data callbacks (`onTrade`, `onBookUpdate`, `onBar`) are silently dropped
during an outage, so the strategy sees a feed gap.

Submit, cancel and replace consult the rate-limit policy first; an overflow emits
`OrderEventStatus::REJECTED_RATE_LIMIT` and the action is not committed.

### Queue tuning

`setQueueFifoTopN`, `setTopPriorityShare`, `setLmmOrders`, `setLmmBonusMultiplier`,
`setOrderPriorityMultiplier` and `setQueuePositionMinChangeFraction` are documented in
[Queue simulation](queue_simulation.md).

## Execution logic

### Market orders

A taker walks the visible ladder. `onBookUpdate` keeps every level it reports, not just the touch,
and a crossing order consumes them in order until its size is covered:

| Case | Fill price |
|------|-----------|
| Size covered by the visible ladder | Volume-weighted price of the levels consumed |
| Size past the visible ladder | The deepest level the walk reached, for the **whole** order |
| No ladder for the symbol (bar or trade-only feed) | Best ask / best bid, or the last trade when neither side is known |

Three consequences worth knowing:

- **Depth is consumed.** What one order ate is gone for the next order in the same step, and the
  touch left behind is republished: `bestAskPrice` / `bestBidPrice` report what is still there, and
  a side eaten to the last lot reports no touch at all. The next book snapshot restores the ladder —
  consumption is within a step, not permanent.
- **Size past the ladder is not free.** The feed says nothing past the deepest visible level, so the
  simulator refuses to invent liquidity there and prices the whole order at that level. The order
  still fills in full; it just stops being cheap. Sweeping a book you could not really sweep is a
  modelling error either way — size the orders to the book you replayed.
- **A maker takes nothing.** A resting limit provides the liquidity, so it consumes no depth.

The walk is one fill per order, reported at the volume-weighted price, not one fill per level.

Slippage is applied on top of the walk for market orders, from the active `SlippageProfile` — it is
an independent friction, and a configured model stays configured whether or not there was depth to
walk. `VOLUME_IMPACT` and a full ladder both charge for size, so use one or the other. See
[Slippage](./slippage.md).

The ladder is whatever the last `onBookUpdate` call reported. The executor is not told whether an
update was a snapshot or a delta, so a feed that publishes only changed levels leaves a partial
ladder behind and a large order will clear against the deepest level of that delta. Replay snapshots
when the depth of the walk matters.

### Limit orders

Without queue simulation (`QueueModel::NONE`, the default), a limit order fills as soon as the book crosses its price:

| Side | Condition to fill |
|------|-------------------|
| BUY  | `orderPrice >= bestAsk` |
| SELL | `orderPrice <= bestBid` |

With queue simulation enabled, non-crossing limits are registered in an `OrderQueueTracker` and fill only when trades at the level consume the queue ahead of them. Crossing ("marketable") limits still fill immediately, walking the ladder up to their own limit price, without slippage. A limit the tracker never registered — one submitted before the model was configured, or a stop-limit that rested when it triggered — has no queue to wait behind and falls back to the crossing rule above. See [Queue simulation](./queue_simulation.md).

### Conditional orders

Stop, take-profit, and trailing-stop orders are stored separately and checked on each market update.

| Type | Trigger condition |
|------|-------------------|
| STOP_MARKET / STOP_LIMIT | SELL: price ≤ trigger, BUY: price ≥ trigger |
| TAKE_PROFIT_MARKET / TAKE_PROFIT_LIMIT | SELL: price ≥ trigger, BUY: price ≤ trigger |
| TRAILING_STOP | Trigger follows price; executes on reversal |

When triggered, conditional orders convert to market or limit and execute through the usual path
(slippage applies to the market leg), with one bound: **a triggered order may fill worse than the
price that armed it, never better.** A sell cannot print above its trigger, a buy cannot print below
it. On bar data the trigger is tested against a synthetic `bid = ask = extreme`, so without the
bound a take-profit sell armed at 105 would book the bar's 110 high — a price it had no claim on,
since it only exists because the bar is over. The bound is one-sided, so a protective stop still
pays the adverse extreme: a sell stop armed at 95 on a bar that traded down to 90 books 90.

A trailing stop is bounded by its moving trigger, which is written onto the order as it fires, so
the `TRIGGERED` event reports the arming price. A stop-limit is additionally capped by its own limit;
one armed at a price past that limit cannot trade there and rests instead, the way a venue treats a
stop-limit triggered on a gap.

### OCO orders

```cpp
OCOParams params;
params.order1 = orderA;
params.order2 = orderB;
executor.submitOCO(params);
```

When one order fills, the other is canceled automatically.

### Trailing stop

```cpp
struct TrailingState
{
  Price activationPrice{};  // price when trailing stop was activated
  Price currentTrigger{};   // current trigger price (moves with price)
};
```

SELL trailing: trigger follows price up (never down). BUY trailing: trigger follows price down (never up).

## Feeding market data

| Call | When to use |
|------|-------------|
| `onBookUpdate(symbol, bids, asks)` | Full L2 snapshot. Replaces the visible ladder, updates top-of-book state and drives the queue tracker's level-update heuristic. |
| `onTopOfBook(symbol, bids, asks)` | Touch-only frame (`BookUpdateType::TOP_OF_BOOK`): moves the touch, drops levels more aggressive than it, keeps deeper depth from the last `onBookUpdate`, then the same matching pass. An empty side means an empty side. |
| `onTrade(symbol, price, isBuy)` | Trade event without quantity. Keeps legacy behavior but does **not** drive queue-simulated fills. |
| `onTrade(symbol, price, qty, isBuy)` | Trade event with quantity. Required for queue simulation. |
| `onBar(symbol, close)` | Bar close shortcut. Sets best bid, best ask, and last trade to the close price. Intrabar stops and targets that would have triggered against the wick are missed. |
| `onBar(symbol, high, low, close)` | Walks low, then high, then close, so wick-triggered stops and take-profits match. Carries no open, so it releases no held orders. |
| `onBar(symbol, open, high, low, close)` | The form a bar-driven run should use: moves the market to the open, releases the orders held from the previous bar's callback there, then walks low, high and close. |

A bar reports no depth, so a bar step drops the symbol's ladder: on bar data any size trades at the
step price, and a stale book from an earlier update is not walked at prices the bar has left behind.

### Bar data and look-ahead

A bar reaches the strategy only after the simulator has walked it, which means the whole bar — open,
high, low and close — is in the market state by the time the callback runs. Matching an order
submitted from there against that state is look-ahead: the order prints at a price that exists only
because the bar is over.

`beginBarCallbackWindow()` / `endBarCallbackWindow()` mark that callback. While the window is open
an arriving order is held rather than submitted; the next `onBar(symbol, open, high, low, close)`
for that symbol releases it at the open, through the full submit path, so rate limits, reduce-only
and self-trade prevention are evaluated where the order actually reaches the venue. The deferral
does not resize or split it.

Hold a `SimulatedExecutor::BarCallbackScope` for the span of the callback rather than calling the
two by hand. The window is a counter, and an early return or a callback that throws leaves it up;
a window that never closes holds every order submitted after it for the rest of the run, and says
nothing while it does. The raw calls remain public for drivers that cannot put the callback inside
a scope.

- An order held when the run ends never reached the venue: it neither fills nor cancels.
  `heldOrderCount()` reports how many are waiting.
- Cancel and replace reach a held order where it waits, so an order submitted and pulled inside one
  callback cannot leak into the next bar.
- The hold is per symbol. An order for a symbol whose bar never comes back is never released.
- Window depth is counted, so a re-entrant callback cannot close a window it did not open.

`BacktestRunner::runBars` opens and closes the window around the strategy callback for you.

### Reset

`reset()` drops everything a finished run left behind — fills, live, held and conditional orders,
market state, ladder, net positions, queue positions, brackets, icebergs and in-flight acks — and
keeps the configuration: slippage, queue model, latency distributions, rate limits, STP, the order
event callback and the attached `VenueAvailability`. Seeded generators go back to their configured
seeds, so a repeated run repeats. `BacktestRunner` calls it at the start of every run.

## Order events

| Event | When |
|-------|------|
| `SUBMITTED` | Order received |
| `ACCEPTED` | Order validated |
| `PENDING_TRIGGER` | Conditional order waiting for trigger |
| `TRIGGERED` | Conditional order triggered |
| `FILLED` | Fully executed |
| `PARTIALLY_FILLED` | Partial execution (common with queue simulation) |
| `CANCELED` | Order canceled |
| `REPLACED` | Order modified |

Trailing stop updates emit `TRAILING_UPDATED` events with the new trigger price.

## Market state

Per-symbol best bid, best ask, last trade and level quantities are updated via `onBookUpdate()` and
`onTrade()`, and by the taker walk, which republishes the touch it leaves behind. The state struct
itself is **private**; read it through the public accessors:

```cpp
Price bestBidPrice(SymbolId symbol) const;   // 0 when the side is empty
Price bestAskPrice(SymbolId symbol) const;
Price bookMidPrice(SymbolId symbol) const;   // 0 when either side is empty
```

The tracked level quantities are consumed internally by `VOLUME_IMPACT` slippage and by the queue
tracker; they are not exposed.

## Performance

- Fixed-size array for symbols 0-255 (fast path)
- Overflow vector for symbol IDs >= 256
- O(n) pending order scan on each market update
- Pre-allocated fill vector (default 4096)

## See also

- [BacktestRunner](./backtest_runner.md) — Run backtests with strategies
- [BacktestResult](./backtest_result.md) — Performance statistics
- [Slippage](./slippage.md)
- [Queue simulation](./queue_simulation.md)
