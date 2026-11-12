# Integration Flow

!!! info "C++ engine integration"
    This page walks through wiring up the C++ engine end-to-end (registry → buses → strategies → executor → connector). If you use Python, Node.js, or Codon, all of this is hidden behind `Runner` and `BacktestRunner` — you don't wire buses by hand. Read this if you embed lrvx in a C++ host or want to understand what happens beneath the bindings.

Step-by-step guide to integrating lrvx components into a complete trading system.

## Overview

```mermaid
flowchart TD
    subgraph Setup
        A[1. Register Symbols] --> B[2. Create Core Components]
        B --> C[3. Create Executors & Connectors]
        C --> D[4. Wire Event Delivery]
    end

    subgraph Runtime
        D --> E[5. Launch Components]
        E --> F[6. Handle Execution Feedback]
    end

    subgraph Teardown
        F --> G[7. Shutdown]
    end
```

## 1. Register Symbols

Before anything else, define which trading instruments the system will operate on. Each symbol is registered with an internal registry, which assigns a unique identifier and stores metadata.

```cpp
#include "lrvx/engine/symbol_registry.h"

SymbolRegistry registry;

// Short form: exchange + symbol only, everything else defaulted
auto btcId = registry.registerSymbol("binance", "BTCUSDT");

// Full form: pass a SymbolInfo to set instrument type, tick size, etc.
// InstrumentType is {Spot, Future, Inverse, Option}.
SymbolInfo eth{.exchange = "binance",
               .symbol = "ETHUSDT",
               .type = InstrumentType::Future,
               .tickSize = Price::fromDouble(0.01)};
auto ethId = registry.registerSymbol(eth);

// Retrieve later by name — returns std::optional<SymbolId>
auto symbolId = registry.getSymbolId("binance", "BTCUSDT");
```

**Key points:**

- `SymbolId` is a compact integer for fast comparison
- Registry maps `(exchange, symbol)` pairs to `SymbolId`
- Metadata includes instrument type, tick size, lot size

## 2. Create Core Components

Several core components must be created and wired together:

```cpp
#include "lrvx/book/bus/trade_bus.h"
#include "lrvx/book/bus/book_update_bus.h"
#include "lrvx/execution/bus/order_execution_bus.h"
#include "lrvx/execution/order_tracker.h"

// Event buses for market data
auto tradeBus = std::make_unique<TradeBus>();
auto bookBus = std::make_unique<BookUpdateBus>();

// Execution infrastructure
auto execBus = std::make_unique<OrderExecutionBus>();
auto orderTracker = std::make_unique<OrderTracker>();
```

**Component responsibilities:**

| Component | Purpose |
|-----------|---------|
| `SymbolRegistry` | Resolve and map symbols |
| `OrderTracker` | Monitor orders and maintain state |
| `TradeBus` | Distribute trade events |
| `BookUpdateBus` | Distribute order book updates |
| `OrderExecutionBus` | Distribute execution events |

## 3. Create Executors and Connectors

```cpp
#include "lrvx/execution/abstract_executor.h"
#include "lrvx/connector/abstract_exchange_connector.h"

// Order executor sends orders to exchange
auto executor = std::make_unique<BinanceExecutor>(
    transport,
    registry,
    orderTracker.get()
);

// Connector receives market data
auto connector = std::make_shared<BinanceConnector>(
    registry,
    *tradeBus,
    *bookBus
);
```

**Executor responsibilities:**

- Send orders to exchange
- Report order status updates
- Update order tracker

**Connector responsibilities:**

- Connect to exchange (WebSocket, REST, FIX)
- Parse wire protocol
- Publish events to buses

## 4. Wire Event Delivery

Configure buses to distribute events to subscribers:

```cpp
#include "lrvx/strategy/abstract_strategy.h"

// Create strategy
auto strategy = std::make_unique<MyStrategy>(executor.get());

// Subscribe to market data
tradeBus->subscribe(strategy.get());
bookBus->subscribe(strategy.get());

// Subscribe to execution events
execBus->subscribe(strategy.get());

// Add optional subscribers. The second parameter is `required` and
// defaults to true — pass false to let a slow consumer drop events.
tradeBus->subscribe(logger.get(), /*required=*/false);
tradeBus->subscribe(metrics.get(), /*required=*/false);
```

**Subscriber types:**

| Type | Behavior |
|------|----------|
| Required | System waits for slow consumers |
| Optional | May miss events if too slow |

**Common subscribers:**

- Trading strategies
- Data aggregators (bars, VWAP)
- Metric collectors
- Risk modules
- Loggers

## 5. Launch Components

Start components in the correct order:

```cpp
#include "lrvx/engine/engine.h"

// Collect all subsystems
std::vector<std::unique_ptr<ISubsystem>> subsystems;
subsystems.push_back(std::move(tradeBus));
subsystems.push_back(std::move(bookBus));
subsystems.push_back(std::move(execBus));
subsystems.push_back(std::move(strategy));

// Collect connectors
std::vector<std::shared_ptr<IExchangeConnector>> connectors;
connectors.push_back(connector);

// Create and start engine. There is no configuration-file parser —
// EngineConfig is a plain struct you fill in yourself.
EngineConfig config{};
Engine engine(config, std::move(subsystems), std::move(connectors));

engine.start();  // Returns immediately once subsystems and connectors are up
```

`Engine::start()` starts every subsystem, then every connector, and
returns. The host keeps the process alive and calls `engine.stop()`
when it wants to shut down — see `demo/src/main.cpp` for the
start / sleep / stop shape.

**Startup sequence:**

1. Event buses start threads
2. Connectors establish connections
3. Strategies begin processing
4. Executors ready to handle orders

## 6. Handle Execution Feedback

When a fill arrives from the venue, the executor updates the tracker and publishes the resulting event:

```cpp
// In your executor implementation
void MyExecutor::onOrderFill(const FillMessage& msg) {
    // Update tracker
    _orderTracker->updateOrder(msg.orderId, OrderStatus::FILLED);

    // Emit event
    OrderEvent event;
    event.order.orderId = msg.orderId;
    event.order.status = OrderStatus::FILLED;
    event.order.filledQty = msg.quantity;
    event.order.avgPrice = msg.price;

    _execBus->publish(event);
}
```

**Order lifecycle:**

```mermaid
stateDiagram-v2
    [*] --> NEW
    NEW --> SUBMITTED: send to exchange
    SUBMITTED --> ACCEPTED: exchange confirms
    SUBMITTED --> REJECTED: validation failed

    ACCEPTED --> PARTIALLY_FILLED: partial execution
    ACCEPTED --> FILLED: full execution
    ACCEPTED --> PENDING_CANCEL: cancel requested
    ACCEPTED --> EXPIRED: time-in-force expired
    ACCEPTED --> REPLACED: modify accepted

    PARTIALLY_FILLED --> PARTIALLY_FILLED: more fills
    PARTIALLY_FILLED --> FILLED: complete
    PARTIALLY_FILLED --> PENDING_CANCEL: cancel remaining

    PENDING_CANCEL --> CANCELED: cancel confirmed

    FILLED --> [*]
    CANCELED --> [*]
    REJECTED --> [*]
    EXPIRED --> [*]
```

**Conditional order states** (stop-loss, take-profit, trailing stop):

```mermaid
stateDiagram-v2
    [*] --> PENDING_TRIGGER: conditional order created
    PENDING_TRIGGER --> TRIGGERED: price condition met
    PENDING_TRIGGER --> TRAILING_UPDATED: trailing stop adjusted
    TRAILING_UPDATED --> PENDING_TRIGGER: continue monitoring
    TRIGGERED --> SUBMITTED: becomes regular order
```

**OrderEventStatus values:**

| Status | Description |
|--------|-------------|
| `NEW` | Order created locally |
| `SUBMITTED` | Sent to exchange |
| `ACCEPTED` | Exchange acknowledged |
| `PARTIALLY_FILLED` | Partial execution |
| `FILLED` | Fully executed |
| `PENDING_CANCEL` | Cancel request sent |
| `CANCELED` | Successfully canceled |
| `EXPIRED` | Time-in-force expired |
| `REJECTED` | Exchange rejected |
| `REPLACED` | Order modified (price/qty) |
| `PENDING_TRIGGER` | Conditional order waiting |
| `TRIGGERED` | Condition met, converting to market/limit |
| `TRAILING_UPDATED` | Trailing stop price adjusted |
| `QUEUE_POSITION_UPDATED` | Queue position moved, no other transition (backtest only) |
| `MARKET_POSITION_CHANGED` | Resting order crossed a categorical position boundary (backtest only) |
| `REPLACE_SUBMITTED` | Replace request sent |
| `REPLACE_ACCEPTED` | Replace acknowledged after ack latency |
| `REPLACE_REJECTED` | Replace refused (original already filled or not replaceable) |
| `REJECTED_RATE_LIMIT` | Refused pre-submission by client-side rate-limit enforcement |
| `PENDING_ONCHAIN` | Broadcast to the mempool, not yet confirmed (DEX) |
| `REVERTED` | Chain rejected the transaction (DEX) |
| `REPLACED_GAS` | Re-broadcast with higher gas, superseding the pending tx (DEX) |

All 21 values are in `lrvx/execution/events/order_event.h`. The three
on-chain states are covered in
[On-chain order lifecycle](onchain-order-lifecycle.md).

## 7. Shutdown Procedure

Shut down in order so queued events are drained before their consumers stop:

```cpp
// Signal shutdown (e.g., from signal handler)
engine.stop();

// Or programmatically
void gracefulShutdown() {
    // Stop accepting new orders
    executor->stop();

    // Stop connectors (no new market data)
    for (auto& conn : connectors) {
        conn->stop();
    }

    // Stop buses (drains queues)
    tradeBus->stop();
    bookBus->stop();
    execBus->stop();

    // Stop strategies
    strategy->stop();
}
```

**Shutdown order:**

1. Stop connectors (no new data)
2. Drain event buses
3. Stop strategies
4. Complete in-flight orders
5. Clean up resources

## Complete Example

```cpp
#include "lrvx/engine/engine.h"
#include "lrvx/engine/symbol_registry.h"
#include "lrvx/book/bus/trade_bus.h"
#include "lrvx/book/bus/book_update_bus.h"
#include "lrvx/execution/bus/order_execution_bus.h"

int main() {
    // 1. Registry
    SymbolRegistry registry;
    registry.registerSymbol("binance", "BTCUSDT");

    // 2. Core components
    auto tradeBus = std::make_unique<TradeBus>();
    auto bookBus = std::make_unique<BookUpdateBus>();
    auto execBus = std::make_unique<OrderExecutionBus>();

    // 3. Executor and connector
    auto executor = createExecutor(registry);
    auto connector = createConnector(registry, *tradeBus, *bookBus);

    // 4. Strategy
    auto strategy = std::make_unique<MyStrategy>(executor.get());
    tradeBus->subscribe(strategy.get());
    bookBus->subscribe(strategy.get());

    // 5. Engine
    std::vector<std::unique_ptr<ISubsystem>> subsystems;
    subsystems.push_back(std::move(tradeBus));
    subsystems.push_back(std::move(bookBus));
    subsystems.push_back(std::move(execBus));
    subsystems.push_back(std::move(strategy));

    std::vector<std::shared_ptr<IExchangeConnector>> connectors;
    connectors.push_back(connector);

    EngineConfig config{};
    Engine engine(config, std::move(subsystems), std::move(connectors));
    engine.start();

    // start() returns immediately; keep the process alive, then:
    engine.stop();

    return 0;
}
```

## Broker pattern (paper and live)

The wiring above shows a C++ host with its own executor and connector. The broker pattern is a shorter version of the same thing — a connector reads a live feed, and a broker swaps in for the executor:

```mermaid
flowchart LR
    FEED[Live feed<br/>ccxt.pro / WS] --> CONN[Connector]
    CONN --> TB[TradeBus]
    TB --> STRAT[Your Strategy]
    STRAT --> SIG[Signal]
    SIG --> BR{Broker}
    BR -->|paper| SIM[SimulatedExecutor]
    BR -->|live| EXCH[Exchange]
```

`lrvx.paper.PaperBroker` runs a `BacktestRunner` session fed live, so signals reach its in-process `SimulatedExecutor` and order events come back exactly as in a backtest. `lrvx.ccxt.CcxtBroker` routes them to a real exchange. Each broker owns its runner; the strategy class is untouched — only the broker swaps. One strategy class runs backtest, paper, and live.

See [Paper trading](../how-to/paper-trading.md) and [Connect lrvx to a CCXT exchange](../how-to/ccxt-adapter.md).

## Design Principles

| Principle | Implementation |
|-----------|----------------|
| **Modularity** | Components can be swapped independently |
| **Thread safety** | Lock-free constructs in hot paths |
| **Zero allocation** | Pre-allocated pools and ring buffers |
| **Determinism** | Sync mode available for backtesting |

## See Also

- [Architecture](architecture.md) — Component overview
- [Run the Demo](../tutorials/demo.md) — See integration in action
- [Configuration](../how-to/configuration.md) — Configure the system
