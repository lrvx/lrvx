# BookUpdate

`BookUpdate` is a zero-allocation container for transmitting order-book snapshots or deltas.
It supports multiple instrument classes (spot, futures, options) and includes optional option metadata.

```cpp
enum class BookUpdateType
{
  SNAPSHOT,
  DELTA,
  TOP_OF_BOOK
};

struct BookLevel
{
  Price price{};
  Quantity quantity{};
  BookLevel() = default;
  BookLevel(Price p, Quantity q) : price(p), quantity(q) {}
};

struct BookUpdate
{
  SymbolId symbol{};
  InstrumentType instrument = InstrumentType::Spot;
  BookUpdateType type{};
  std::pmr::vector<BookLevel> bids;
  std::pmr::vector<BookLevel> asks;

  UnixNanos exchangeTsNs{0};
  UnixNanos systemTsNs{0};

  std::optional<Price> strike;
  std::optional<TimePoint> expiry;
  std::optional<OptionType> optionType;

  BookUpdate(std::pmr::memory_resource* res) : bids(res), asks(res) {}
};
```

## Purpose

* Provide a **normalized, memory-efficient** representation of an order-book update (full snapshot or incremental delta).
* Embed the **instrument class** so downstream components can branch without a registry lookup.

## Fields

| Field            | Description                                                      |
| ---------------- | ---------------------------------------------------------------- |
| **symbol**       | Unique `SymbolId` of the instrument.                             |
| **instrument**   | `Spot`, `Future`, or `Option`.                                   |
| **type**         | `SNAPSHOT` (full overwrite), `DELTA` (incremental change), or `TOP_OF_BOOK` (one-level refresh of the touch; an empty side means that side is empty). |
| **bids / asks**  | Depth updates stored in PMR vectors (`BookLevel`).               |
| **exchangeTsNs** | Exchange timestamp in nanoseconds since Unix epoch.              |
| **systemTsNs**   | Local system receive time in nanoseconds, for latency metrics.   |
| **strike**       | Strike price — *only* for option updates.                        |
| **expiry**       | Expiry date/time — *only* for option updates.                    |
| **optionType**   | `Call` or `Put` — *only* for option updates.                     |

## Notes

* When `type == SNAPSHOT`, consumers **must** fully replace their local book for `symbol`.
* `bids` and `asks` are typically reserved to capacity in an object pool, avoiding runtime allocations.
* Downstream filters can quickly ignore instruments by checking `instrument` without a `SymbolRegistry` lookup.
* **Option fields are optional:** they are populated only when `instrument == InstrumentType::Option`.
* Uses `UnixNanos` (int64_t nanoseconds) for precise timestamps.
