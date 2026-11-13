# Who owns a strategy's per-symbol state

Every strategy carries a `SymbolContext` per symbol: the order book, the last trade price, the position and cost basis the attached position manager last reported, and the timestamp of the last update. Your hooks read it; the engine writes it. This page says which thread is allowed to do what, and where that state actually lives.

The rules belong to `lrvx::Strategy`, so every binding gets them. Python, Node, Codon and the embedded JavaScript runtime all reach user code through the same hooks.

## One lock per symbol, held across the hook

A strategy subscribes to three buses (trades, book updates, bars) and each bus runs its own consumer thread for each subscriber. On a live engine the order events a venue reports (`lrvx_live_engine_on_order_event`) travel a fourth bus, again with a consumer thread per strategy, so `on_fill` and `on_order_update` take the same per-symbol lock as the market-data hooks. So two events for the *same* symbol can arrive at the same instant on two different threads.

A symbol's state belongs to whoever holds that symbol's lock. A dispatch takes the lock before it touches the context and keeps it until your hook returns:

```cpp
void onSymbolBook(SymbolContext& c, const BookUpdateEvent& ev) override
{
  // The symbol's lock is held for the whole of this call. `c` is yours
  // until you return; no bus thread can rewrite it underneath you.
  auto mid = c.mid();
  if (mid && shouldQuote(*mid))
  {
    emitLimitBuy(ev.update.symbol, *mid, Quantity::fromDouble(1.0));
  }
}
```

Three things follow.

Your hook sees a coherent context. Best bid, best ask, last trade price and position all come from the same instant; a half-applied book update is never visible.

Two events for one symbol never overlap. A trade and a book update for `BTC-USDT` are serialised, and the second one waits for the first hook to return.

Two symbols still run in parallel, because the lock is per symbol rather than per strategy. A trade on `BTC-USDT` and a book update on `ETH-USDT` go through at the same time. Short hooks still matter, though: a slow one holds up its own symbol.

### Why one lock and not several

The obvious refinement is to split the lock by what it guards: one for the order book, another for the scalars, so that a trade and a book update stop waiting on each other. It does not work here, for two separate reasons.

The three writing paths share `refreshPosition`, which copies position and cost basis out of the attached position manager into the context. So `position`, `avgEntryPrice` and `lastUpdateNs` are written by the trade path, the book path and the bar path alike: three of the five mutable fields are common ground before the book is even considered.

And the book is not private to the book path either. Every hook is handed the context and reads it whole, best bid and best ask included, so a trade hook reading the book conflicts with a book update writing it. Splitting the lock would leave each path needing both halves on every event.

Making the scalars atomic instead runs into the public surface: `position`, `lastTradePrice` and `avgEntryPrice` are fields your strategy reads directly, and changing their types would change that surface in every binding.

### Re-entering from inside a hook

A hook can emit an order that an executor fills straight away, which calls the strategy back for the same symbol on the same thread. The simulated executor does this in every backtest. It is supported: the lock is reentrant, so a nested dispatch on the owning thread goes through instead of deadlocking.

### What the lock does not cover

`ctx()` and `ctx(symbol)` hand back a bare reference. Use them from inside a hook, where your thread already holds the lock, or before the engine starts. Reading a context from an unrelated thread while the engine runs is outside the contract, and no lock taken inside the accessor would fix that, because the reference outlives the call.

The per-(symbol, timeframe) bar rings behind `lastClosedBar` and `lastNClosedBars` have their own lock and are safe to read from any thread.

## Where the state lives

The context table is 256 slots, each holding a full 512-level order book. That is 8,384 bytes per slot in a release build and 16,640 with `LRVX_SCALE_CHECKS` on, since the scale checks widen `Price` and `Quantity` from 8 bytes to 16.

Those slots used to sit inside the strategy object by value, which put a strategy at roughly 2 MB release and 4 MB checked. Two of them in one stack frame overran a default 8 MB stack, and the overrun landed in the constructor prologue, before a single line of the strategy had run. It only showed up in checked builds, so the release run passed and the sanitiser lane crashed.

The table now lives in one heap block owned by the map, allocated once at construction. A strategy object is a few hundred bytes, so you can put it wherever you like:

```cpp
// All fine now.
MyStrategy onTheStack(1, symbols, registry);
auto onTheHeap = std::make_unique<MyStrategy>(2, symbols, registry);
std::vector<std::unique_ptr<MyStrategy>> many;
```

The hot path pays one extra dependent load for the indirection, and it measures as free: the pointer is in L1 after the first event, and dropping a blind store to the slot's initialised flag on every event gives the cost back. Taking the lock is the real price, about 2 ns per dispatch on an Apple M-series core when nothing else wants that symbol.

## Symbols past the table

`SymbolStateMap` holds 256 flat slots. A symbol id at or above that falls back to overflow storage, which is a linear scan, and every out-of-range symbol shares one lock. If your registry issues ids that high, raise the template parameter instead of paying for the scan.

## Related

- [Disruptor pattern](disruptor.md), for how the buses deliver and why each subscriber gets its own consumer thread.
- [Per-symbol scale](per-symbol-scale.md), for what `LRVX_SCALE_CHECKS` changes about `Price` and `Quantity`.
- [Memory model](memory-model.md), for the wider allocation story on the hot path.
