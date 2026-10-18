# NLevelOrderBook

`NLevelOrderBook` is a fixed-depth limit order book. Uses tick-based indexing and no heap allocations during updates.

```cpp
template <size_t MaxLevels = 8192>
class NLevelOrderBook : public IOrderBook
{
public:
  static constexpr size_t MAX_LEVELS = MaxLevels;

  explicit NLevelOrderBook(Price tickSize);

  void applyBookUpdate(const BookUpdateEvent& ev) override;

  std::optional<Price> bestBid() const override;
  std::optional<Price> bestAsk() const override;
  Quantity bidAtPrice(Price p) const override;
  Quantity askAtPrice(Price p) const override;

  // Market state helpers
  bool isCrossed() const;
  std::optional<Price> spread() const;
  std::optional<Price> mid() const;

  // Depth consumption (slippage calculation)
  std::pair<Quantity, Volume> consumeAsks(Quantity needQty) const;
  std::pair<Quantity, Volume> consumeBids(Quantity needQty) const;

  // Level extraction
  struct PriceLevel { Price price; Quantity quantity; };
  std::vector<PriceLevel> getBidLevels(size_t maxLevels) const;
  std::vector<PriceLevel> getAskLevels(size_t maxLevels) const;

  // Utilities
  Price tickSize() const;
  void clear();
  void dump(std::ostream& os, size_t levels,
            int pricePrec = 4, int qtyPrec = 3, bool ansi = false) const;
};
```

## Purpose

* Maintain and query an efficient in-memory representation of top-of-book and full depth using indexed price levels.

## Responsibilities

| Aspect      | Details                                                                    |
| ----------- | -------------------------------------------------------------------------- |
| Input       | Consumes `BookUpdateEvent` messages, supports both `SNAPSHOT` and `DELTA`. `TOP_OF_BOOK` frames are ignored: they carry the touch, not the book. |
| Resolution  | Tick-based price quantization via `_tickSize`.                             |
| Depth Query | Provides `bestBid`, `bestAsk`, and `Quantity` at arbitrary price levels.   |
| Storage     | Preallocated arrays for bids and asks indexed by tick-level offset.        |

## Internal Behavior

1. **Price Indexing**
   Prices are mapped to array indices using `price / tickSize`, enabling constant-time access.
   A quote that does not sit on a tick is snapped away from the mid: an ask rounds
   up to the next tick, a bid down to the previous one, so a stored ask is never
   below the quoted ask and a stored bid never above the quoted bid. A price
   already on a tick is stored unchanged. `bidAtPrice` / `askAtPrice` snap the
   same way as the side they query, so a level is found at the price it was sent
   at. See [Tick snapping](#tick-snapping).

2. **Snapshot Handling**
   A `SNAPSHOT` clears all state and resets index bounds before applying levels.

2b. **Window Tracking on Deltas**
   The array covers `MaxLevels` consecutive ticks, centred on the market, so the
   headroom in either direction is half the level count. A `DELTA` that adds a
   level past the edge re-anchors the window and carries the existing levels
   across, as long as they all still fit alongside the new one; when they do not,
   the out-of-window level is dropped and the book is left alone, so one stray
   quote cannot evict live depth. Removals never re-anchor. See
   [The order book's tick window](../../../explanation/order-book-tick-window.md).

3. **Bounds Tracking**
   Maintains `_minBid`, `_maxBid`, `_minAsk`, `_maxAsk` for efficient best-level scans.

4. **Cached Best Bid/Ask**
   Tracks `_bestBidIdx` and `_bestAskIdx` for O(1) best price queries without
   scanning; an index outside `[0, MAX_LEVELS)` is what "this side is empty"
   means, and the tick of the best quote is derived from it. See
   [Negative prices](#negative-prices).

5. **No Dynamic Allocation**
   Uses `std::array` of fixed size; fully cache-friendly and allocation-free after construction.

6. **Cache-Aligned Storage**
   Bids and asks arrays are 64-byte aligned for optimal cache performance.

## Market State Helpers

```cpp
bool isCrossed() const;
std::optional<Price> spread() const;
std::optional<Price> mid() const;
```

| Method      | Description                                                      |
| ----------- | ---------------------------------------------------------------- |
| `isCrossed` | Returns `true` if best bid >= best ask (crossed/locked market).  |
| `spread`    | Returns ask - bid spread, or `nullopt` if either side is empty.  |
| `mid`       | Returns midpoint price, or `nullopt` if either side is empty.    |

`mid()` is `tickSize * (bidTick + askTick) / 2` with one division at the end, so
it agrees exactly with `SymbolContext::mid()` and with the Python and C surfaces
that read it. An odd sum of tick indices truncates by at most half a raw unit,
since `Price` is an integer underneath.

## Negative prices

A price below zero is a quote like any other. WTI settled at -37.63 in April
2020, day-ahead power clears below zero on a windy afternoon, and a calendar
spread is negative whenever the market is in contango. `bestBid`, `bestAsk`,
`mid`, `spread` and `isCrossed` report such a book exactly as they report a
positive one, and a bid at exactly 0.0 is a quote, not an absence.

"No quote" is signalled by the `std::optional` itself: `bestBid`, `bestAsk`,
`mid` and `spread` return `nullopt`, and `isCrossed` returns `false`, when the
side is empty -- an untouched book, a `clear()`, an empty `SNAPSHOT`, or the
last level on the side pulled by a `DELTA`. No price value is spent as a
sentinel for it, so no price can be mistaken for one.

On the C surface, `lrvx_book_best_bid` and its siblings answer the same way:
the return value is the presence flag and the price arrives through the out
parameter. The strategy-side `lrvx_best_bid_raw` family has no such channel and
returns 0 for both answers; use `lrvx_best_bid_raw_opt`, `lrvx_best_ask_raw_opt`
and `lrvx_mid_price_raw_opt` where a book can quote through zero. See the
[C API reference](../capi/lrvx_capi.md#context-queries).

## Depth Consumption

Calculate fill price and slippage by walking through book levels:

```cpp
// Simulate market buy: consume asks up to 10 BTC
auto [filledQty, totalCost] = book.consumeAsks(Quantity::fromDouble(10.0));
Price avgPrice = totalCost / filledQty;

// Simulate market sell: consume bids
auto [filledQty, totalProceeds] = book.consumeBids(Quantity::fromDouble(10.0));
```

Uses 128-bit arithmetic on GCC/Clang for precision; portable fallback otherwise.

## Level Extraction

Get multiple price levels for display or analysis:

```cpp
// Get top 10 bid levels
auto bids = book.getBidLevels(10);
for (const auto& level : bids) {
  std::cout << level.price.toDouble() << " @ " << level.quantity.toDouble() << "\n";
}

// Get top 10 ask levels
auto asks = book.getAskLevels(10);
```

## Debug Output

```cpp
// Print order book to console with ANSI colors
book.dump(std::cout, 20, /*pricePrec=*/2, /*qtyPrec=*/4, /*ansi=*/true);
```

Output includes tick size, base index, spread, and mid price in header.

## Tick size

The constructor throws `std::invalid_argument` when the tick size is zero or
negative. A tick size reaches the book from instrument configuration and from
user code, so a bad one is bad input and is reported rather than asserted;
`lrvx_book_create` returns `NULL` for the same reason, and the Python, Node,
QuickJS and Codon wrappers raise in whatever way their language expects.

`Price` has a fixed scale of 1e8, so the smallest representable tick is 1e-8.
Ticks of 1e-8 and 2e-8, which is what the sub-cent pairs quote in, behave like
any other.

## Tick snapping

An off-tick quote has to be stored at some tick, and which one it is decides
whether the book can report a price the venue never offered. Snapping to the
nearest tick moved an ask down by up to half a tick and a bid up by the same,
so `bestAsk()` handed a strategy a better price than the quote and it sized
against liquidity that is not there. The book snaps conservatively instead:

| Side | Rule       | Example, tick 1.0 | Stored |
| ---- | ---------- | ----------------- | ------ |
| Ask  | round up   | 100.4             | 101.00 |
| Bid  | round down | 99.6              | 99.00  |
| Either | unchanged on a tick | 100.0    | 100.00 |

The stored price is therefore equal to the quote or one tick worse, never
better, whatever the tick size and wherever inside the tick the quote falls.
A venue that publishes more precision than its own tick (a cent tick with a
sub-cent quote) is the ordinary case for this, not the corner.

## Notes

* Extremely fast and deterministic — suitable for backtests and production.
* Off-tick prices are accepted and snapped conservatively, see above; feeding
  tick-aligned prices still avoids the loss of precision entirely.
* Offers predictable latency across workloads, assuming sparse updates.
* Uses `math::FastDiv64` for optimized tick division.
