# Writing Your First Codon Strategy

This tutorial walks through creating an SMA crossover strategy in Codon.
The strategy buys when a fast SMA crosses above a slow SMA, and sells on the
reverse crossover.

## Prerequisites

1. lrvx built with C API enabled
2. Codon compiler installed

```bash
cmake -B build -DLRVX_BUILD_CAPI=ON -DLRVX_ENABLE_BACKTEST=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Step 1: Create the Strategy File

Create `my_sma_strategy.codon`:

```codon
from lrvx.strategy import Strategy
from lrvx.context import SymbolContext
from lrvx.types import TradeData
from lrvx.indicators import SMA


class SmaCrossover(Strategy):
    fast_sma: SMA
    slow_sma: SMA
    order_size: float
    long_position: bool

    def __init__(self, symbols: List[int], fast: int = 10, slow: int = 30):
        super().__init__(symbols)
        self.fast_sma = SMA(fast)
        self.slow_sma = SMA(slow)
        self.order_size = 1.0
        self.long_position = False

    def on_trade(self, ctx: SymbolContext, trade: TradeData):
        price = trade.price.to_double()
        fast = self.fast_sma.update(price)
        slow = self.slow_sma.update(price)

        # Wait until both SMAs have enough data
        if not self.slow_sma.ready:
            return

        sym = self.primary_symbol

        # Golden cross: fast crosses above slow
        if fast > slow and not self.long_position:
            self.emit_market_buy(sym, self.order_size)
            self.long_position = True

        # Death cross: fast crosses below slow
        elif fast < slow and self.long_position:
            self.emit_market_sell(sym, self.order_size)
            self.long_position = False

    def on_start(self):
        print("Strategy started!")

    def on_stop(self):
        pos = self.position()
        print(f"Strategy stopped. Final position: {pos}")
```

## Step 2: Compile to Native Binary

```bash
codon build -exe \
  -o my_sma_strategy \
  -L build/src/capi \
  -llrvx_capi \
  my_sma_strategy.codon
```

## Step 3: Understanding the Code

### Imports

- `Strategy` -- base class providing emit/query methods
- `SymbolContext` -- per-symbol state (position, book, prices)
- `TradeData` -- trade event with price, quantity, side
- `SMA` -- SMA indicator class with both `compute()` (batch) and `update()` / `value` / `ready` / `reset` (incremental)

### Key Patterns

1. **Override `on_trade`** to receive trade events
2. **Use streaming indicators** for per-tick computation
3. **Call `emit_*` methods** to submit orders
4. **Query `position()`** to check current state

### Performance

Because Codon compiles to native code:
- `on_trade` is compiled to a native function, not interpreted
- No GIL, no interpreter, no garbage collector pauses

`SMA.update()` is one FFI call that runs one step of the C++ `SMA` held by
the instance (a running sum, O(1)). The indicator classes in
`lrvx.indicators` keep no history, so an update costs the same on the
millionth tick as on the first, and the values match `compute()` and the
C++ classes bit for bit. Each instance owns a C handle; `close()` frees it
early, otherwise the collector frees it with the instance.

## Step 4: Compare with C++ and Python

The same strategy in C++:

```cpp
void onSymbolTrade(SymbolContext& c, const TradeEvent& ev) override {
    prices_.push_back(ev.trade.price.toDouble());
    // ... SMA logic ...
    if (fast_above && !long_position_)
        emitMarketBuy(symbol(), orderSize_);
}
```

And in Python:

```codon
def on_trade(self, ctx, trade):
    self.prices.append(trade.price)
    # ... SMA logic ...
    if fast > slow and not self.long_position:
        self.emit_market_buy(ctx.symbol_id, 1.0)
```

All three produce identical trading behavior with the same API shape.

## Next Steps


- See [Indicators Reference](../reference/codon/indicators.md) for all available indicators
- See [Strategy Classes](../how-to/strategy-classes.md) for the full API reference
