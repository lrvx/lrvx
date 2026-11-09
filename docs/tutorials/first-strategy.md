# First Strategy

Write a simple trading strategy that reacts to market data. After your [language quickstart](README.md) you should already have lrvx building. This tutorial introduces the callback model that every binding shares.

## The model

Every Strategy subclass gets ten callbacks. You override the ones you need; the rest stay no-ops.

| Callback (Python) | Node.js | Fires when |
|---|---|---|
| `on_trade(ctx, trade)` | `onTrade(ctx, trade, emit)` | A trade tick arrives |
| `on_book_update(ctx)` | `onBookUpdate(ctx, emit)` | A depth frame (snapshot or delta) is applied |
| `on_bar(ctx, bar)` | `onBar(ctx, bar, emit)` | A closed OHLC bar is dispatched |
| `on_touch(ctx)` | `onTouch(ctx, emit)` | The touch (best bid / ask, price or size) changed |
| `on_fill(ctx, event)` | `onFill(ctx, ev, emit)` | One of your orders partially or fully fills |
| `on_order_update(ctx, event)` | `onOrderUpdate(ctx, ev, emit)` | Any order-lifecycle status change (fills included) |
| `on_queue_position_change(ctx, event)` | `onQueuePositionChange(ctx, ev, emit)` | A resting limit order's queue position moved (backtest only) |
| `on_market_position_change(ctx, event)` | `onMarketPositionChange(ctx, ev, emit)` | A resting limit order crossed a categorical position boundary (backtest only) |
| `on_start()` / `on_stop()` | `onStart()` / `onStop()` | Strategy lifecycle |

The first three are the ones most strategies need. Node.js callbacks take an extra `emit` argument; Python calls order helpers on `self`.

Inside callbacks you query state via `ctx` and place orders with helpers like `market_buy(qty)` / `limit_sell(price, qty)`.

## A printing strategy

This strategy logs every trade and tracks the best bid / ask.

=== "Python"

    ```python
    import lrvx

    class PrintingStrategy(lrvx.Strategy):
        def __init__(self, symbols):
            super().__init__(symbols)
            self.trade_count = 0

        def on_start(self):
            print(f"PrintingStrategy started")

        def on_stop(self):
            print(f"PrintingStrategy stopped. Trades seen: {self.trade_count}")

        def on_trade(self, ctx, trade):
            self.trade_count += 1
            side = "BUY" if trade.is_buy else "SELL"
            print(f"Trade: {trade.price:.2f} x {trade.quantity:.4f} ({side})")

        def on_book_update(self, ctx):
            if ctx.best_bid is not None and ctx.best_ask is not None:
                print(f"Book: {ctx.best_bid:.2f} / {ctx.best_ask:.2f}")
    ```

=== "Node.js"

    ```javascript
    class PrintingStrategy {
      constructor(symbols) {
        this.symbols = symbols;
        this.tradeCount = 0;
      }
      onStart() { console.log("PrintingStrategy started"); }
      onStop()  { console.log(`PrintingStrategy stopped. Trades seen: ${this.tradeCount}`); }

      onTrade(ctx, trade) {
        this.tradeCount++;
        const side = trade.isBuy ? "BUY" : "SELL";
        console.log(`Trade: ${trade.price.toFixed(2)} x ${trade.qty.toFixed(4)} (${side})`);
      }
      onBookUpdate(ctx) {
        console.log(`Book: ${ctx.bestBid.toFixed(2)} / ${ctx.bestAsk.toFixed(2)}`);
      }
    }
    ```

=== "Codon"

    ```python
    from lrvx.strategy import Strategy
    from lrvx.context import SymbolContext
    from lrvx.types import TradeData

    class PrintingStrategy(Strategy):
        trade_count: int = 0

        def __init__(self, symbols: List[int]):
            super().__init__(symbols)

        def on_start(self):
            print("PrintingStrategy started")

        def on_trade(self, ctx: SymbolContext, trade: TradeData):
            self.trade_count += 1
            side = "BUY" if trade.is_buy else "SELL"
            print(f"Trade: {trade.price.to_double():.2f} ({side})")
    ```

=== "C++"

    ```cpp
    #include "lrvx/strategy/strategy.h"

    using namespace lrvx;

    class PrintingStrategy : public Strategy {
    public:
      PrintingStrategy(SymbolId symbol, const SymbolRegistry& reg)
          : Strategy(/*id=*/1, symbol, reg) {}

      void start() override { LRVX_LOG("PrintingStrategy started"); }
      void stop()  override { LRVX_LOG("Trades seen: " << _tradeCount); }

    protected:
      void onSymbolTrade(SymbolContext& /*ctx*/, const TradeEvent& ev) override {
        ++_tradeCount;
        LRVX_LOG("Trade: " << ev.trade.price.toDouble()
                 << " x " << ev.trade.quantity.toDouble()
                 << " (" << (ev.trade.isBuy ? "BUY" : "SELL") << ")");
      }

      void onSymbolBook(SymbolContext& ctx, const BookUpdateEvent& /*ev*/) override {
        LRVX_LOG("Book: " << ctx.book.bestBid()->toDouble()
                 << " / " << ctx.book.bestAsk()->toDouble());
      }

    private:
      uint64_t _tradeCount{0};
    };
    ```

## A trading strategy

Submit a buy after every 10th trade.

=== "Python"

    ```python
    class TenthTradeBuyer(lrvx.Strategy):
        def __init__(self, symbols):
            super().__init__(symbols)
            self.count = 0

        def on_trade(self, ctx, trade):
            self.count += 1
            if self.count % 10 == 0:
                self.limit_buy(price=trade.price - 0.01, qty=1.0)
    ```

=== "Node.js"

    ```javascript
    class TenthTradeBuyer {
      constructor(symbols) { this.symbols = symbols; this.count = 0; }
      onTrade(ctx, trade, emit) {
        this.count++;
        if (this.count % 10 === 0) emit.limitBuy(trade.price - 0.01, 1.0);
      }
    }
    ```

=== "C++"

    ```cpp
    void onSymbolTrade(SymbolContext& /*ctx*/, const TradeEvent& ev) override {
      if (++_tradeCount % 10 != 0) return;
      auto px = Price::fromRaw(ev.trade.price.raw() - Price::fromDouble(0.01).raw());
      emitLimitBuy(symbol(), px, Quantity::fromDouble(1.0));
    }
    ```

## Wiring it up

Each language has a small bit of boilerplate to register your symbols and run the strategy.

=== "Python"

    ```python
    reg = lrvx.SymbolRegistry()
    btc = reg.add_symbol("binance", "BTCUSDT", tick_size=0.01)

    strat = PrintingStrategy([btc])
    bt = lrvx.BacktestRunner(reg, fee_rate=0.0004, initial_capital=10_000)
    bt.set_strategy(strat)
    bt.run_csv("data/btcusdt_1m.csv", "BTCUSDT")
    ```

=== "Node.js"

    ```javascript
    const reg = new lrvx.SymbolRegistry();
    const btc = reg.addSymbol("binance", "BTCUSDT", 0.01);

    const strat = new PrintingStrategy([btc]);
    const bt = new lrvx.BacktestRunner(reg, 0.0004, 10_000);
    bt.setStrategy(strat);
    bt.runCsv("data/btcusdt_1m.csv", "BTCUSDT");
    ```

=== "C++"

    ```cpp
    auto tradeBus = std::make_unique<TradeBus>();
    auto bookBus  = std::make_unique<BookUpdateBus>();

    SymbolRegistry registry;
    SymbolInfo info{ .exchange = "binance", .symbol = "BTCUSDT",
                      .tickSize = Price::fromDouble(0.01) };
    auto symId = registry.registerSymbol(info);

    auto strat = std::make_unique<PrintingStrategy>(symId, registry);
    tradeBus->subscribe(strat.get());
    bookBus ->subscribe(strat.get());

    std::vector<std::unique_ptr<ISubsystem>> subsystems;
    subsystems.push_back(std::move(tradeBus));
    subsystems.push_back(std::move(bookBus));
    subsystems.push_back(std::move(strat));

    EngineConfig config{};
    Engine engine(config, std::move(subsystems), std::move(connectors));
    engine.start();
    ```

## Best practices

**Do**:

- Keep callbacks fast and non-blocking
- Filter or short-circuit on the first line if the event isn't relevant
- Let `ctx.is_flat()` / `ctx.position` decide entries — don't track position state yourself
- Use the strategy emit helpers (`market_buy(qty)`) — they wire fees, queue position, and reduce-only flags correctly

**Don't**:

- Block in callbacks (no I/O, no locks, no big allocations)
- Hold references to event structs after the callback returns — they're recycled
- Throw exceptions from callbacks

## Next

- [Multi-Timeframe Strategy](multi-timeframe-strategy.md) — work with multiple bar timeframes
- [Recording Data](recording-data.md) — capture market data to disk
- [Backtesting](backtesting.md) — replay recorded data
- [Architecture overview](../explanation/architecture.md) — how events flow
