# Strategy

`Strategy` is the unified base class for implementing trading strategies. It supports both single-symbol and multi-symbol strategies through a single, consistent API.

```cpp
class Strategy : public IStrategy
{
public:
  // Multi-symbol constructor
  Strategy(SubscriberId id, std::vector<SymbolId> symbols,
           const SymbolRegistry& registry);

  // Single-symbol convenience constructor
  Strategy(SubscriberId id, SymbolId symbol,
           const SymbolRegistry& registry);
};
```

The strategy requires a `SymbolRegistry` reference to look up per-symbol metadata (tick size, instrument type, etc.).

## The subscriber id has to be unique

Every strategy in a process needs its own. Nothing checks it, and the id does
double duty: the event buses route on it, and it is also the namespace for the
strategy's order ids, which number from 1 within each strategy. Two strategies
built with the same id hand out the same order ids, and the executor, the order
tracker and the `.lrvxrun` trace cannot tell their orders apart.

An order id is `(subscriber id << 32) + n`, so the id has to fit 32 bits. The
constructor and `setSubscriberId` throw `std::invalid_argument` for an id of
2^32 or more, which would otherwise shift out of the word and issue the order
ids of another subscriber, and for 2^32 - 1 (`kExplicitFillOrderIdSubscriber`,
`4294967295`): its block is where `MultiModePositionTracker` numbers the fills
it is handed without an order, so a strategy holding it issued those ids. Id
`2097151` constructs, and is refused when the strategy is attached to a
runner, which takes its own order ids from that block.

Numbering within the strategy rather than across the process is what makes a
second run of the same tape in the same process reproducible. A grid search or
a batch runner gets the same ids every time, so two traces of an identical run
compare equal byte for byte.

## Allocate it on the heap

A `Strategy` holds 256 `SymbolContext` slots by value and each one carries a
full 512-level book, so the object is about 2 MB in a release build and about
4 MB in a checked one. Two of them in a single stack frame overrun a default
8 MB stack wherever scale checks are on, and the overflow lands in the
constructor prologue before a line of the strategy has run.

## Purpose

- Provide a base class for all trading strategies
- Automatically manage per-symbol state (order books, positions, prices)
- Route market data events to symbol-specific handlers
- Emit trading signals to an executor

## Per-Symbol Context

Each symbol has a `SymbolContext` that aggregates all relevant state:

```cpp
struct SymbolContext
{
  NLevelOrderBook<512> book;     // Order book
  Quantity position{};           // Net position
  std::optional<Price> avgEntryPrice{};  // VWAP entry price, empty if unknown
  Price lastTradePrice{};        // Last trade price
  int64_t lastUpdateNs{0};       // Last update timestamp
  SymbolId symbolId{0};          // Symbol identifier

  std::optional<Price> mid() const noexcept;      // Mid price
  std::optional<Price> bookSpread() const noexcept; // Bid-ask spread
  std::optional<double> unrealizedPnl(Price markPrice) const noexcept;
  std::optional<double> unrealizedPnl() const noexcept;  // Uses mid()

  bool isLong() const noexcept;
  bool isShort() const noexcept;
  bool isFlat() const noexcept;
};
```

## Event Handlers

Override these methods to receive per-symbol market data:

```cpp
protected:
  // Called on trade events for subscribed symbols
  virtual void onSymbolTrade(SymbolContext& ctx, const TradeEvent& ev) {}

  // Called on book updates for subscribed symbols
  virtual void onSymbolBook(SymbolContext& ctx, const BookUpdateEvent& ev) {}

  // Called when the touch changed: on a TOP_OF_BOOK frame, and after
  // onSymbolBook on a depth frame that moved it
  virtual void onSymbolTouch(SymbolContext& ctx, const BookUpdateEvent& ev) {}

  // Called when a bar closes for a subscribed symbol
  virtual void onSymbolBar(SymbolContext& ctx, const BarEvent& ev) {}

  // Order-event hooks. Override onSymbolFill for fill notifications (the common
  // case) and onSymbolOrderUpdate for everything else — cancels, rejects,
  // pending-trigger transitions. Without these, native stop_market is unusable:
  // there is no other path for the strategy to learn its stop fired.
  virtual void onSymbolFill(SymbolContext& ctx, const OrderEvent& ev) {}
  virtual void onSymbolOrderUpdate(SymbolContext& ctx, const OrderEvent& ev) {}

  // Backtest only. Live exchanges do not publish queue position.
  virtual void onSymbolQueuePositionChange(SymbolContext& ctx, const OrderEvent& ev) {}
  virtual void onSymbolMarketPositionChange(SymbolContext& ctx, const OrderEvent& ev) {}
```

`onTrade`, `onBookUpdate` and `onBar` are **`final`** on `Strategy` — do not try to override them.
They filter by subscription, update `SymbolContext`, push into the bar ring, and then dispatch to the
`onSymbol*` hooks above.

`onBookUpdate` sends a `TOP_OF_BOOK` frame to `onSymbolTouch` only, and only when it changed the
touch. A depth frame goes to `onSymbolBook`, then to `onSymbolTouch` if it moved the touch, so one
frame can reach both hooks.

`onOrderEvent` routes by status: `FILLED` / `PARTIALLY_FILLED` to `onSymbolFill`,
`QUEUE_POSITION_UPDATED` to `onSymbolQueuePositionChange`, `MARKET_POSITION_CHANGED` to
`onSymbolMarketPositionChange`, everything else to `onSymbolOrderUpdate`. Before it routes, it
reads the position from the attached position manager into the context, as the market-data
handlers do. The backtest runner hands a fill to its position tracker before the strategy, so
`ctx.position` and `ctx.avgEntryPrice` inside `onSymbolFill` already count that fill and agree with
`position(sym)`.

The base class automatically:
1. Filters events by subscription
2. Updates `SymbolContext` (book, prices, timestamps)
3. Maintains the per-(symbol, timeframe) bar ring
4. Dispatches to the appropriate handler

## Context Access

```cpp
protected:
  // Access context by symbol ID
  SymbolContext& ctx(SymbolId sym) noexcept;
  const SymbolContext& ctx(SymbolId sym) const noexcept;

  // Single-symbol convenience (returns first symbol's context)
  SymbolContext& ctx() noexcept;
  const SymbolContext& ctx() const noexcept;

  // Get primary symbol ID (for single-symbol strategies)
  SymbolId symbol() const noexcept;

  // All subscribed symbols
  const std::vector<SymbolId>& symbols() const noexcept;

  // Check if symbol is subscribed
  bool isSubscribed(SymbolId sym) const noexcept;
```

```cpp
public:
  // Each symbol's registry entry, in symbols() order: as copied by the
  // constructor, with the venue tick and lot applied since
  std::vector<SymbolInfo> symbolInfos() const;

  // The venue tick log of the registry the strategy was built from
  std::shared_ptr<const VenueTickLog> venueTickLog() const noexcept;
```

`symbolInfos()` is where a `BacktestRunner` without `setSymbolRegistry` reads the symbols' declared
ticks and lots ([BacktestRunner](../backtest/backtest_runner.md#strategy-setup)). It is built from
the strategy's own copy of the entries and the registry's venue tick log, so the registry the
strategy was built with need not outlive it, and it returns by value: each call gives the entries
with whatever tick and lot a connector has applied to the registry since the strategy was built
(`applyVenueTick` / `applyVenueLot`) in place of the ones copied then. It used to return a
reference to the construction-time copy, which never saw an applied value.

Each symbol's book (`ctx(sym).book`) is sized on the tick the registry held when the strategy was
built. A strategy built before its connector starts usually sees the 0.01 placeholder; when the
connector then applies the venue's tick to the registry entry
([applyVenueTick](../engine/symbol_registry.md#the-venues-tick-for-an-entry-registered-first)),
the strategy rebuilds that symbol's book on it at the symbol's next event, before any callback
runs. The connector applies it in `start()` before its socket opens, so the book is still empty
then. A venue that moves a market's tick while connected (Polymarket's `tick_size_change`) is
followed the same way: the book is rebuilt on the new tick at the next event, empty, and the
venue's next snapshot fills it. `symbolInfos()` reports the applied tick from the call that
applied it, without waiting for an event.

## Signal Emission

All order methods return an `OrderId` for tracking:

```cpp
protected:
  void emit(const Signal& signal);

  // One tick's signals handed to the handler together (see
  // ISignalHandler::onSignalBatch), so a batching bridge can coalesce them
  // into a single venue request. Takes fully-built signals: unlike the
  // emit* helpers below, emitBatch does not assign order ids — the caller
  // already has.
  void emitBatch(std::span<const Signal> signals);

  // Market orders
  OrderId emitMarketBuy(SymbolId symbol, Quantity qty);
  OrderId emitMarketSell(SymbolId symbol, Quantity qty);

  // Limit orders
  OrderId emitLimitBuy(SymbolId symbol, Price price, Quantity qty);
  OrderId emitLimitSell(SymbolId symbol, Price price, Quantity qty);
  OrderId emitLimitBuy(SymbolId symbol, Price price, Quantity qty, TimeInForce tif);
  OrderId emitLimitSell(SymbolId symbol, Price price, Quantity qty, TimeInForce tif);

  // Order management
  void emitCancel(OrderId orderId);
  void emitCancelAll(SymbolId symbol);
  void emitModify(OrderId orderId, Price newPrice, Quantity newQty);

  // Stop orders
  OrderId emitStopMarket(SymbolId symbol, Side side, Price triggerPrice, Quantity qty);
  OrderId emitStopLimit(SymbolId symbol, Side side, Price triggerPrice, Price limitPrice, Quantity qty);

  // Take profit orders
  OrderId emitTakeProfitMarket(SymbolId symbol, Side side, Price triggerPrice, Quantity qty);
  OrderId emitTakeProfitLimit(SymbolId symbol, Side side, Price triggerPrice, Price limitPrice, Quantity qty);

  // Trailing stop
  OrderId emitTrailingStop(SymbolId symbol, Side side, Price offset, Quantity qty);
  OrderId emitTrailingStopPercent(SymbolId symbol, Side side, int32_t callbackBps, Quantity qty);

  // Close position (reduce-only market order)
  OrderId emitClosePosition(SymbolId symbol);

  // DEX/AMM liquidity provision. Executed by the on-chain connector, not the
  // CEX or backtest path; `pool` identifies the pool.
  OrderId emitProvideLiquidity(SymbolId pool, Price priceLower, Price priceUpper,
                               Quantity liquidity);
  OrderId emitWithdrawLiquidity(SymbolId pool, Quantity liquidity);
```

### Conditional Order Examples

```cpp
// Stop-loss: sell when price drops to 95
emitStopMarket(symbol, Side::SELL, Price::fromDouble(95.0), qty);

// Take-profit: sell when price rises to 110
emitTakeProfitMarket(symbol, Side::SELL, Price::fromDouble(110.0), qty);

// Trailing stop: sell if price drops 2% from peak
emitTrailingStopPercent(symbol, Side::SELL, 200, qty);  // 200 bps = 2%

// Close entire position
emitClosePosition(symbol);

// IOC limit order
emitLimitBuy(symbol, price, qty, TimeInForce::IOC);
```

## Multi-Timeframe Bar Ring

`Strategy` keeps a ring of closed bars per (symbol, timeframe), so a multi-timeframe strategy can
recall the last N closed bars without its own bookkeeping. These are `public`:

```cpp
size_t barRingCapacity() const noexcept;
void   setBarRingCapacity(size_t n) noexcept;   // clamped to >= 1

std::optional<Bar> lastClosedBar(SymbolId sym, BarType type, uint64_t param) const;
std::vector<Bar>   lastNClosedBars(SymbolId sym, BarType type, uint64_t param, size_t n) const;
```

| Method | Behavior |
|--------|----------|
| `barRingCapacity()` | Bars retained per (symbol, timeframe) |
| `setBarRingCapacity(n)` | Set the retention count. `n` is clamped to at least 1 |
| `lastClosedBar(sym, type, param)` | The most recent closed bar, or `std::nullopt` before a `BarAggregator` of that timeframe has emitted one |
| `lastNClosedBars(sym, type, param, n)` | The most recent `n` bars in chronological order, oldest first. Returns fewer than `n` (possibly empty) when the ring holds less |

`param` follows `BarEvent::barTypeParam`: nanoseconds for `BarType::Time`, count for `Tick`,
threshold for `Volume`. The ring evicts the oldest bar when full.

## Order and Position Tracking

Query order status and positions (requires `setOrderTracker` / `setPositionManager`):

```cpp
protected:
  // Position queries
  Quantity position(SymbolId sym) const;  // Net position for symbol
  Quantity position() const;              // Primary symbol position

  // Order status queries
  std::optional<OrderEventStatus> getOrderStatus(OrderId orderId) const;
  std::optional<OrderState> getOrder(OrderId orderId) const;
```

Connect trackers:

```cpp
strategy.setOrderTracker(&orderTracker);
strategy.setPositionManager(&positionTracker);
```

## Cross-Symbol Helpers

Free functions in `symbol_context.h` for multi-symbol strategies:

```cpp
// Price spread between two symbols
std::optional<Price> spread(const SymbolContext& a, const SymbolContext& b);

// Price ratio between two symbols
std::optional<double> ratio(const SymbolContext& a, const SymbolContext& b);
```

## Examples

### Single-Symbol Strategy

```cpp
// Stand-in for your own entry logic, so the example below compiles as shown.
bool shouldBuy(Price) { return false; }

class MomentumStrategy : public Strategy
{
public:
  MomentumStrategy(SymbolId sym, const SymbolRegistry& registry)
    : Strategy(1, sym, registry) {}

  void start() override { _running = true; }
  void stop() override { _running = false; }

protected:
  void onSymbolTrade(SymbolContext& c, const TradeEvent& ev) override
  {
    if (!_running) return;

    // Access order book
    auto bid = c.book.bestBid();
    auto ask = c.book.bestAsk();
    if (!bid || !ask) return;

    // Check position via tracker
    if (position().isZero() && shouldBuy(ev.trade.price))
    {
      // Returns OrderId for tracking
      OrderId id = emitMarketBuy(c.symbolId, Quantity::fromDouble(1.0));
      _pendingOrder = id;
    }

    // Check order status
    if (_pendingOrder)
    {
      auto status = getOrderStatus(*_pendingOrder);
      if (status && *status == OrderEventStatus::FILLED)
      {
        _pendingOrder = std::nullopt;
      }
    }
  }

private:
  bool _running{false};
  std::optional<OrderId> _pendingOrder;
};
```

### Multi-Symbol Pairs Strategy

```cpp
// Stand-in for a real z-score computed over your own rolling stats, so
// the example below compiles as shown.
double zscore(Price) { return 0.0; }

class PairsStrategy : public Strategy
{
public:
  PairsStrategy(SymbolId leg1, SymbolId leg2, const SymbolRegistry& registry)
    : Strategy(1, {leg1, leg2}, registry), _leg1(leg1), _leg2(leg2) {}

  void start() override {}
  void stop() override {}

protected:
  void onSymbolBook(SymbolContext& c, const BookUpdateEvent& ev) override
  {
    // Check spread between legs
    auto spreadOpt = spread(ctx(_leg1), ctx(_leg2));
    if (!spreadOpt) return;

    double z = zscore(*spreadOpt);

    if (ctx(_leg1).isFlat() && std::abs(z) > 2.0)
    {
      // Open spread position
      if (z > 0)
      {
        emitMarketSell(_leg1, _size);
        emitMarketBuy(_leg2, _size);
      }
      else
      {
        emitMarketBuy(_leg1, _size);
        emitMarketSell(_leg2, _size);
      }
    }
  }

private:
  SymbolId _leg1, _leg2;
  Quantity _size{Quantity::fromDouble(1.0)};
};
```

## Integration

Connect strategy to backtest or live execution:

```cpp
// Create registry
SymbolRegistry registry;
SymbolInfo info;
info.exchange = "BINANCE";
info.symbol = "BTCUSDT";
info.tickSize = Price::fromDouble(0.01);
SymbolId symbolId = registry.registerSymbol(info);

// Backtest
BacktestRunner runner(config);
MyStrategy strategy(symbolId, registry);
runner.setStrategy(&strategy);
auto result = runner.run(*reader);

// Live (with signal handler)
strategy.setSignalHandler(&executor);

// Engine takes its subsystems and connectors through the constructor; there is
// no addSubscriber / addConnector. Move the strategy in as a subsystem.
std::vector<std::unique_ptr<ISubsystem>> subsystems;
subsystems.push_back(std::move(strategyPtr));

Engine engine(engineConfig, std::move(subsystems), connectors);
engine.start();
```

`Engine`'s entire public surface is the three-argument constructor plus `start()` and `stop()`.

## See Also

- [PositionTracker](../position/position_tracker.md) - Track realized PnL with FIFO/LIFO/AVERAGE
- [SymbolContext](symbol_context.md) - Per-symbol state details
- [BacktestRunner](../backtest/backtest_runner.md) - Backtesting framework
