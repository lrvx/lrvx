# SymbolContext

`SymbolContext` aggregates all per-symbol state needed by trading strategies: order book, position, prices, and timestamps.

```cpp
struct SymbolContext
{
  static constexpr size_t kDefaultBookLevels = 512;

  NLevelOrderBook<kDefaultBookLevels> book;
  Quantity position{};
  std::optional<Price> avgEntryPrice{};
  Price lastTradePrice{};
  int64_t lastUpdateNs{0};
  SymbolId symbolId{0};
};
```

## Purpose

- Consolidate all per-symbol data in a cache-friendly structure
- Provide computed properties (mid price, spread, PnL)
- Support multi-symbol strategies with O(1) state access

## Fields

| Field | Type | Description |
|-------|------|-------------|
| `book` | `NLevelOrderBook<512>` | Order book with 512 price levels |
| `touchBid` / `touchAsk` | `std::optional<BookLevel>` | The most recent best bid/ask. Depth frames set it from the applied book; `TOP_OF_BOOK` frames, which the book ignores, update it directly. Empty when that side has no quote |
| `lastTouchNs` | `int64_t` | Exchange timestamp of the frame that last wrote the touch; `lastUpdateNs` only covers dispatched events |
| `position` | `Quantity` | Net position (positive=long, negative=short) |
| `avgEntryPrice` | `std::optional<Price>` | Volume-weighted average entry price, empty when the attached position manager reports none |
| `lastTradePrice` | `Price` | Most recent trade price |
| `lastUpdateNs` | `int64_t` | Last update timestamp (nanoseconds) |
| `symbolId` | `SymbolId` | Symbol identifier |

## Computed Properties

### Mid Price

```cpp
std::optional<Price> mid() const noexcept;
```

Returns midpoint between best bid and ask. Returns `nullopt` if either side is missing.

### Touch Mid

```cpp
std::optional<Price> touchMid() const noexcept;
```

Mid of the freshest known touch. `mid()` answers from the depth book and moves at depth-frame cadence; this one also moves on `TOP_OF_BOOK` frames.

### Book Spread

```cpp
std::optional<Price> bookSpread() const noexcept;
```

Returns bid-ask spread. Returns `nullopt` if book is one-sided.

### Unrealized PnL

```cpp
std::optional<double> unrealizedPnl(Price markPrice) const noexcept;
std::optional<double> unrealizedPnl() const noexcept;  // Uses mid() as mark
```

Unrealized PnL against the average entry price, which the engine reads from the
attached `IPositionManager` before every handler call. Empty when there is no
entry price to measure against, either because the manager keeps no cost basis
or because the no-argument form has no mid to mark at. A flat position returns
`0.0`, not an empty optional.

Substituting zero for a missing entry price would report `position * mark`, the
whole notional of the position dressed up as profit. That is what this returned
before `IPositionManager` could be asked for one, and it meant a rule like
"close when the loss passes X" never fired on a long and fired on the first
tick of a short.

### Position State

```cpp
bool isLong() const noexcept;   // position > 0
bool isShort() const noexcept;  // position < 0
bool isFlat() const noexcept;   // position == 0
```

### Reset

```cpp
void reset() noexcept;
```

Clears all state: book, position, prices, timestamps.

## Cross-Symbol Helpers

Free functions for multi-symbol analysis:

```cpp
// Price spread: midA - midB
std::optional<Price> spread(const SymbolContext& a, const SymbolContext& b);

// Price ratio: midA / midB
std::optional<double> ratio(const SymbolContext& a, const SymbolContext& b);
```

## Example

```cpp
class SpreadStrategy : public Strategy
{
public:
  SpreadStrategy(SymbolId front, SymbolId back, const SymbolRegistry& registry)
    : Strategy(1, {front, back}, registry), _front(front), _back(back) {}

protected:
  void onSymbolBook(SymbolContext& c, const BookUpdateEvent& ev) override
  {
    // Get spread between front and back month
    auto spreadOpt = spread(ctx(_front), ctx(_back));
    if (!spreadOpt) return;

    double spreadValue = spreadOpt->toDouble();

    // Check unrealized PnL. Either leg can report "unknown".
    auto frontPnl = ctx(_front).unrealizedPnl();
    auto backPnl = ctx(_back).unrealizedPnl();
    if (!frontPnl || !backPnl) return;

    double pnl = *frontPnl + *backPnl;

    // Check position state
    if (ctx(_front).isLong() && pnl > _target)
    {
      emitClosePosition(_front);
      emitClosePosition(_back);
    }
  }

private:
  SymbolId _front, _back;
  double _target{100.0};
};
```

## Memory Layout

`SymbolContext` is designed for cache efficiency:

- 8,384 bytes per symbol in a release build; the 512-level book dominates at
  8,320 of them. A build with `LRVX_SCALE_CHECKS` on (any build without
  `NDEBUG`) carries a scale field on every `Decimal` and doubles this to 16,640
- `Strategy` holds 256 of these by value through `SymbolStateMap`, so the
  strategy object is about 2 MB in release and about 4 MB in a checked build.
  Allocate a strategy on the heap: two on one stack frame overrun a default
  8 MB stack in a checked build, and the overflow lands in the constructor
  prologue
- All fields in single contiguous struct
- Access via `SymbolStateMap` provides O(1) lookup

## See Also

- [Strategy](strategy.md) - Strategy base class
- [NLevelOrderBook](../book/nlevel_order_book.md) - Order book implementation
- [SymbolStateMap](symbol_state_map.md) - O(1) container for per-symbol state
