# The touch and the book

A strategy has two views of the inside market. The book is the depth feed applied level by level, and it is only as fresh as the venue's last depth frame. The touch is the best bid and ask as last reported by any frame, including the one-level frames a venue sends between depth updates. This page says where the two come from, who reads which, and how the one-level frame travels through the engine, the backtester and the tape.

## Two feeds, one price

A venue may throttle its depth stream and run a separate best-bid/offer stream next to it. Hyperliquid is the case in the tree: it throttles `l2Book` server-side and offers `bbo` for the top. A strategy fed only `l2Book` sees a touch that is stale by up to one throttle interval; a strategy that merges `bbo` into its depth book corrupts the book.

The engine keeps the two apart with a third kind of book update. `BookUpdateType` has `SNAPSHOT` (replace the book), `DELTA` (apply changes) and `TOP_OF_BOOK`: one level per side, nothing deeper. The Hyperliquid connector subscribes `bbo` next to `l2Book` by default (`subscribeBbo`) and publishes each frame as `TOP_OF_BOOK`. The other connectors in the tree publish depth only. A custom connector can publish the type itself.

## The contract of a top-of-book frame

A `TOP_OF_BOOK` frame is the venue's complete word on the touch and nothing else:

- it carries at most one level per side;
- an empty side means that side of the book is empty, not "unchanged" (that is the difference from a `DELTA`, which only carries the sides that changed);
- it says nothing about levels behind the touch.

A depth book cannot apply such a frame. Merging it as a delta leaves a stale better level in place when the top moves away; replacing the book with it as a snapshot throws the depth away. So consumers split into two groups.

Consumers that read it as a snapshot of the touch:

- `CompositeBookMatrix`, the cross-venue matrix: a top replaces both sides of that venue's touch, like a snapshot would.
- `SymbolContext`, the per-symbol state every strategy callback receives: a top writes the touch fields and nothing else (next section).
- `SimulatedExecutor`, the backtest venue: a top moves the touch, drops its own levels that are more aggressive than it, keeps the deeper levels from the last depth frame, then runs the same matching pass as a depth update. The backtest runner routes top frames there. A strategy that sees a fresher touch than the last depth frame therefore fills at that touch, not at the depth frame's best.

Consumers that ignore it:

- `NLevelOrderBook`, the depth book inside `SymbolContext` and anywhere else it is used.
- `BookSnapshotBinAggregator`, which reconstructs depth from snapshots and deltas.
- `EventTypeStatsAggregator`, the per-symbol count of trades, snapshots and deltas: a top is in none of its three columns, so a tape that is all tops reads as having no book events there. `BinCountAggregator` and the writer's `total_book_updates` count them.

## What the strategy sees

`SymbolContext` carries both views.

| | The book | The touch |
|---|---|---|
| State | `book` (an `NLevelOrderBook`) | `touchBid`, `touchAsk` (each an optional `BookLevel`) |
| Mid | `mid()` | `touchMid()` |
| Written by | depth frames | depth frames and `TOP_OF_BOOK` frames |
| Stamp | `lastUpdateNs` | `lastTouchNs` |

A depth frame writes both: it applies the book, refreshes the touch from the book's best levels, and stamps `lastUpdateNs` and `lastTouchNs` with the frame's exchange timestamp. A top writes the touch and `lastTouchNs` only. Trades and bars stamp `lastUpdateNs` and leave the touch alone; order events write neither.

No callback fires for a top. `onSymbolBook` would hand the strategy the previous depth frame's book at touch rate, stamped fresh, so the frame lands in the fields and the strategy reads them on its next trade, bar or depth callback. The stamps are how a strategy tells what happened in between: `lastTouchNs` later than the `lastUpdateNs` it saw on its last book callback means the touch moved since the last depth frame it was shown.

The bindings read the touch, not the book, wherever they expose a best price:

| | Best bid, best ask, mid | Stamps |
|---|---|---|
| C++ | `touchBid`, `touchAsk`, `touchMid()`; the depth book stays `book`, `mid()` | `lastTouchNs`, `lastUpdateNs` |
| C API | `LrvxSymbolContext.book` (an `LrvxBookSnapshot` with `has_bid` / `has_ask`), `lrvx_best_bid_raw_opt` and its siblings | `last_touch_ns`, `last_update_ns` |
| Python | `best_bid`, `best_ask`, `mid_price` | `last_touch_ns`, `last_update_ns` |
| Node | `bestBid`, `bestAsk`, `midPrice` | `lastTouchNs`, `lastUpdateNs` (`BigInt`) |
| Codon | `best_bid()`, `best_ask()`, `mid_price()` | `last_touch_ns()`, `last_update_ns()` |
| Embedded JavaScript | `ctx.book` | `ctx.lastTouchNs`, `ctx.lastUpdateNs` (`BigInt`) |

`None` / `null` on a side means that side of the touch is empty. A price of exactly zero is a price, never the empty marker.

## On the tape

The recorder keeps the frame as its own kind. A top is frame type `6` (`BookTop`) after `2` snapshot and `3` delta, with the same payload shape as a depth frame: a `BookRecordHeader` whose `type` byte is `2`, followed by up to one level per side. The frame is additive in the sense the format spec gives the word: a reader that predates it skips the frame by its size and goes on.

Two byte spaces meet here and the readers say which they speak. The record byte is `0` snapshot, `1` delta, `2` top, and that is what `event_type` carries on the rows of `DataReader.read_book_updates()` and `read_bbo()` and on their C API, Node and Codon equivalents. The frame type is `2`, `3`, `6`, and that is what `event_type` carries on the rows of `MergedTapeReader.read_books()`. A consumer that reconstructs depth from either must treat the top value as a touch, or skip it; applying it as a delta is the stranded-level failure above.

The recorder hook's book callback carries a third value as well. Its `is_snapshot` argument is a byte: `1` snapshot, `0` delta, `2` top. The two boolean values predate the third and keep their meaning. Surfaces whose shape is a bool skip tops the way an old reader skips the frame: the custom recorder hooks in Python and Node, `MergedTapeReader.stream_events`, `replay_tapes`, and `DataWriter.write_book`, which can write snapshots and deltas only. `metadata.json` carries `has_book_tops` next to `has_book_snapshots` and `has_book_deltas`.

A backtest from such a tape replays the frame the way live delivered it: the strategy's touch moves, the depth book does not, and the simulated venue fills at the touch the strategy saw.

## See also

- [Read the touch from a strategy](../how-to/read-the-touch.md), with a runnable example
- [BookUpdate](../reference/api/book/book_update.md) and [NLevelOrderBook](../reference/api/book/nlevel_order_book.md)
- [SymbolContext](../reference/api/strategy/symbol_context.md)
- [CompositeBookMatrix](../reference/api/cex/composite_book_matrix.md)
- [The `.lrvx` format](../spec/lrvx.md)
- [Connectors](connectors.md), for the Hyperliquid `bbo` channel
