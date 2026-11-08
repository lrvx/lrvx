# Read the touch from a strategy

The best bid and ask a strategy callback receives follow the touch, which a venue's one-level top-of-book frames move between depth updates. This page is the working side of [The touch and the book](../explanation/the-touch.md): where the values are in each binding, how to tell a touch move from a depth update, and how to replay a tape that carries tops.

## Where the touch is

Every callback's context quotes the touch. The depth book is not what the quotes read.

| Binding | Best bid / ask, mid | Stamps |
|---|---|---|
| Python | `ctx.best_bid`, `ctx.best_ask`, `ctx.mid_price` | `ctx.last_touch_ns`, `ctx.last_update_ns` |
| Node | `ctx.bestBid`, `ctx.bestAsk`, `ctx.midPrice` | `ctx.lastTouchNs`, `ctx.lastUpdateNs` (`BigInt`) |
| Codon | `ctx.best_bid()`, `ctx.best_ask()`, `ctx.mid_price()` | `ctx.last_touch_ns()`, `ctx.last_update_ns()` |
| C++ | `c.touchBid`, `c.touchAsk`, `c.touchMid()` | `c.lastTouchNs`, `c.lastUpdateNs` |

`last_update_ns` is the exchange timestamp of the event the callback is for: a trade, a depth frame or a bar. `last_touch_ns` is the exchange timestamp of the frame that last wrote the touch. A depth frame writes both; a top-of-book frame writes `last_touch_ns` only and fires no callback.

## Tell a touch move from a depth update

Keep the `last_update_ns` you saw in your last book callback. On any later callback, `ctx.last_touch_ns` greater than that value means a top arrived after the last depth frame you were shown, and the quotes in `ctx` are that top's. The example replays a tape of one depth snapshot, three tops and a trade, and prints what each callback sees:

```python
--8<-- "examples/python_read_the_touch.py"
```

The tape under `docs/examples/data/touch_tape` is synthetic, written for this page. Its shape is what a Hyperliquid recording with the `bbo` channel on looks like between two depth snapshots.

## Mark against the touch

`mid()` on the C++ context is the depth book's mid and last moved on a depth frame. `touchMid()` also moves on top-of-book frames, so between depth frames it is the fresher mark. A strategy that quotes off the mid wants the second one, and the same bookkeeping as above to know whether it moved:

```cpp
#include "lrvx/strategy/strategy.h"

using namespace lrvx;

class QuoteOffTheTouch : public lrvx::Strategy
{
 public:
  using Strategy::Strategy;

 protected:
  void onSymbolBook(SymbolContext& c, const BookUpdateEvent&) override
  {
    // A depth frame: the book and the touch were both written just now.
    _lastBookNs = c.lastUpdateNs;
  }

  void onSymbolTrade(SymbolContext& c, const TradeEvent& ev) override
  {
    // c.mid() last moved on a depth frame. c.touchMid() also moves on
    // TOP_OF_BOOK frames, which the depth book ignores.
    auto mark = c.touchMid();
    if (!mark)
    {
      return;  // one side of the touch is empty
    }
    // A top arrived after the last depth frame this strategy was shown.
    const bool touchMoved = c.lastTouchNs > _lastBookNs;
    if (touchMoved && c.position.raw() == 0)
    {
      emitLimitBuy(ev.trade.symbol, *mark, lrvx::Quantity::fromDouble(0.01));
    }
  }

 private:
  int64_t _lastBookNs{0};
};
```

In Python, Node and Codon the context's mid already is the touch mid, so the comparison is the only extra step.

## Replay a tape that carries tops

Nothing special: `BacktestRunner.run_tape` delivers a top the way live did. The strategy's touch and `last_touch_ns` move, no callback fires, and the simulated venue moves its own touch while keeping the depth from the last snapshot, so a market order fills at the touch the strategy saw rather than at the stale depth frame.

## Record tops

A recording made through the Hyperliquid connector with `subscribeBbo` on (the default) carries tops as frame type `6`. `EventTypeStatsAggregator` does not count them, since its per-symbol table has columns for trades, snapshots and deltas only; `metadata.json` says `has_book_tops`. Custom recorder hooks in Python and Node receive snapshots and deltas and skip tops, since their book callback takes a bool; a C recorder hook receives the byte `2`.

## See also

- [The touch and the book](../explanation/the-touch.md)
- [Strategy classes](strategy-classes.md), for the full context table
- [Record and replay tapes](tape-record.md)
