# SymbolRegistry

`SymbolRegistry` assigns and resolves stable `SymbolId` and `ExchangeId` values for instruments and exchanges. Exposes full metadata (`SymbolInfo`, `ExchangeInfo`) for fast, type-safe access across the engine.

```cpp
struct SymbolInfo
{
  SymbolId id{0};
  std::string exchange;
  std::string symbol;
  InstrumentType type = InstrumentType::Spot;
  Price tickSize{kDefaultTickSize};  // 0.01
  // True when tickSize is the venue's price increment. A simulated venue
  // refuses order prices off a known tick. Any tickSize other than the default
  // counts as known without this flag; set it to have an explicit 0.01 checked.
  bool tickSizeKnown{false};
  // The venue's order-quantity increment; zero (the default) when none is
  // declared. A simulated venue refuses an order quantity off a declared lot.
  Quantity lotSize{};
  ExchangeId exchangeId{InvalidExchangeId};

  std::optional<Price> strike;
  std::optional<TimePoint> expiry;
  std::optional<OptionType> optionType;

  // Contract spec. Defaults preserve spot/perp behavior: 1.0 multiplier, cash
  // settlement, European exercise. Options override per venue (US equity 100x,
  // physical-settled American; crypto 1x, cash, European).
  double contractMultiplier{1.0};
  SettlementType settlementType{SettlementType::Cash};
  ExerciseStyle exerciseStyle{ExerciseStyle::European};
  std::optional<std::string> settlementCcy;

  // Per-symbol fixed-point scale. The default 1e8 reproduces the compile-time
  // Price/Quantity scale exactly, so CEX symbols and persisted registries are
  // unchanged. A DEX token whose price (~1e-10) or supply (~1e12) range does not
  // fit 1e8 in int64 sets a different scale here and converts through the
  // scale-aware Decimal overloads.
  int64_t priceScale{Price::Scale};
  int64_t qtyScale{Quantity::Scale};
};

// Validate a symbol's per-symbol scale before registration. A scale must be
// positive and small enough to leave usable integer headroom in int64. The
// default 1e8 always passes, so CEX symbols and old registries are never
// rejected. The cap is 1e18, which still leaves ~9 integer units in int64.
std::expected<void, std::string> validateSymbolScale(const SymbolInfo& info);

class SymbolRegistry : public ISubsystem
{
public:
  static constexpr size_t kMaxExchanges = 32;
  static constexpr size_t kMaxSymbols = 4096;
  static constexpr size_t kMaxEquivalentsPerSymbol = 8;

  // Exchange management
  ExchangeId registerExchange(std::string_view name,
                              VenueType type = VenueType::CentralizedExchange);
  std::optional<ExchangeInfo> getExchange(ExchangeId id) const;
  ExchangeId getExchangeId(std::string_view name) const;
  size_t exchangeCount() const;

  // Symbol registration (legacy string-based API)
  SymbolId registerSymbol(const std::string& exchange, const std::string& symbol);
  SymbolId registerSymbol(const SymbolInfo& info);
  std::optional<SymbolId> getSymbolId(const std::string& exchange,
                                      const std::string& symbol) const;
  std::optional<SymbolInfo> getSymbolInfo(SymbolId id) const;
  std::pair<std::string, std::string> getSymbolName(SymbolId id) const;

  // A venue's tick for an entry registered before the venue was read
  VenueTickResult applyVenueTick(SymbolId id, Price venueTick);
  VenueLotResult applyVenueLot(SymbolId id, Quantity venueLot);
  std::shared_ptr<const VenueTickLog> venueTickLog() const noexcept;

  // Symbol registration (ExchangeId-based API)
  SymbolId registerSymbol(ExchangeId exchange, std::string_view symbol);
  ExchangeId getExchangeForSymbol(SymbolId symbol) const;

  // Symbol equivalence (cross-exchange mapping)
  void mapEquivalentSymbols(std::span<const SymbolId> equivalentSymbols);
  EquivalentSymbols getEquivalentSymbols(SymbolId symbol) const;
  SymbolId getEquivalentOnExchange(SymbolId symbol, ExchangeId exchange) const;

  // Persistence
  bool saveToFile(const std::filesystem::path& path) const;
  bool loadFromFile(const std::filesystem::path& path);
  bool loadFromFile(const std::filesystem::path& path, std::string& why);
  std::vector<std::byte> serialize() const;
  bool deserialize(std::span<const std::byte> data);
  bool deserialize(std::span<const std::byte> data, std::string& why);

  // Utilities
  void clear();
  // In registration order (ascending id).
  std::vector<SymbolInfo> getAllSymbols() const;
  size_t size() const;
};
```

## Purpose

* Provide a thread-safe, bidirectional mapping between human-readable instrument keys and compact numeric `SymbolId`s.
* Manage exchange registrations with `ExchangeId` for multi-exchange scenarios.
* Support cross-exchange symbol equivalence for arbitrage and routing.
* Expose complete instrument metadata (`SymbolInfo`) for latency-critical components without repeated parsing.
* Support persistence and serialization for replay and data recording scenarios.

## Exchange Management

| Method            | Description                                           |
| ----------------- | ----------------------------------------------------- |
| `registerExchange`| Registers an exchange, returns `ExchangeId`.          |
| `getExchange`     | Returns a copy of the `ExchangeInfo` for the given ID, or `std::nullopt`. |
| `getExchangeId`   | Lookup `ExchangeId` by name.                          |
| `exchangeCount`   | Returns number of registered exchanges.               |

## Symbol Registration

| Method                 | Description                                                         |
| ---------------------- | ------------------------------------------------------------------- |
| `registerSymbol(str)`  | Registers a spot instrument, returns existing `id` if present.      |
| `registerSymbol(info)` | Registers any instrument type with full metadata.                   |
| `registerSymbol(ExchangeId, symbol)` | Registers using `ExchangeId` instead of string.      |
| `getSymbolId`          | Forward lookup from `(exchange, symbol)` to `SymbolId`.             |
| `getSymbolName`        | Reverse lookup from `SymbolId` to `(exchange, symbol)` string pair. |
| `getSymbolInfo`        | Returns `std::optional<SymbolInfo>` for the given ID.               |
| `getExchangeForSymbol` | Returns `ExchangeId` for a given symbol.                            |
| `applyVenueTick`       | Writes a venue's tick into an existing entry whose tick is not declared, or was applied by an earlier call (see below). |
| `applyVenueLot`        | The same for the venue's quantity step: writes it into an entry that declares no lot, or whose lot an earlier call applied. |
| `venueTickLog`         | The ticks `applyVenueTick` and the lots `applyVenueLot` wrote, shared with copies taken earlier. |

### Ids

Every `registerSymbol` overload draws a new id from one sequence: one above the highest id the
registry has handed out or loaded, starting at 1. `0` is never a symbol's id. It is what a refused
registration returns: `registerSymbol(ExchangeId, symbol)` for an exchange that is not registered,
and `registerSymbol(info)` for an invalid scale or for an explicit `info.id` that a symbol with
another name already holds (nothing is changed then).

The largest id is `SymbolRegistry::kMaxSymbolId`, 1,048,575. The id-to-name and id-to-exchange
tables are arrays indexed by id, which is what makes `getSymbolName` and `getExchangeForSymbol` an
array read, so one explicit id sizes them for every id below it: `registerSymbol(info)` with
`info.id = 4'000'000'000` asked for about 190 GB. Every `registerSymbol` overload returns `0` and
changes nothing for an id above the limit, explicit or next in the sequence, and a load that holds
one fails.

The two name overloads used to number from 0 (the size of the id-to-name table) while
`registerSymbol(info)` numbered from the entry count plus one. The first symbol registered by name
therefore had id 0, the same value a refused registration returns, and copying its `SymbolInfo`
into another registry (as `BacktestRunner` does with a strategy's symbols) gave it a different id
there, because an `info.id` of 0 asks for a new one. A registry that mixes explicit and assigned
ids now assigns above the highest explicit id, where it used to fill in from the entry count: after
an explicit id 9, the next assigned id is 10. An explicit id that another symbol holds used to
replace that symbol's entry while its name still resolved to the id.

## The venue's tick for an entry registered first

`registerSymbol` never changes an existing entry: it returns its id. The usual live setup registers
a symbol, builds strategies from its id, and only then starts the connector, which reads the
venue's tick. The connector writes that tick into the existing entry with
`applyVenueTick(id, tick)`, which changes `tickSize` and `tickSizeKnown` and nothing else (the id,
names, type, exchange and scales stay, since consumers index by them), under the registry's
exclusive lock:

| Entry's tick before the call | Outcome | Entry after |
| --- | --- | --- |
| Not declared (`tickSizeKnown` false and the 0.01 default) | `Applied` | The venue's tick, known |
| A tick an earlier `applyVenueTick` applied, now different from the venue's | `Applied` | The venue's new tick, known |
| Declared, equal to the venue's | `Unchanged` | Unchanged |
| Declared, different | `Conflict` | Unchanged: the declared tick is kept |
| No entry with this id | `UnknownSymbol` | - |
| Venue tick not positive | `InvalidTick` | Unchanged |

A tick the venue applied is not a declaration. A venue can move a market's tick while it trades
(Polymarket does, and says so on its market channel); the connector calls `applyVenueTick` again and
the entry follows, each change recorded in `venueTickLog()`. The connectors register their own
symbols the same way, with the tick applied after registration, so those follow too.

A declared tick is never overwritten. The caller may know better than the venue's metadata (a
product the venue lists under a different increment, or a test that wants the refusal path), and
replacing a value someone set on purpose without saying so is worse than reporting the
disagreement. The connectors report a `Conflict` as an error naming both ticks, in the log and in
their `instrumentInfoError()`.

Every reader of the registry (`getSymbolInfo` returns a copy) sees the new tick from the call on.
A copy taken earlier does not by itself, so the holders of one keep the shared `venueTickLog()` and
follow it. `Strategy` sizes each symbol's book on the tick it read in its constructor and rebuilds a
symbol's book on the applied tick at that symbol's next event; its `symbolInfos()` returns the
copied entries with the applied tick and lot in place. `SimulatedExecutor` caches each symbol's
tick and lot and drops both caches when the log's revision moves, so an executor that already read
a symbol checks the next order against the applied value. The log is a `shared_ptr`, so a strategy
may outlive the registry. `clear()`, `loadFromFile` and `deserialize` reset it.

### The venue's lot

`applyVenueLot(id, lot)` does for `lotSize` what `applyVenueTick` does for the tick, and changes
nothing else in the entry. It returns a `VenueLotResult` with the same outcomes and the lot the
entry holds after the call:

| Entry's lot before the call | Outcome | Entry after |
| --- | --- | --- |
| None declared (`lotSize` zero) | `Applied` | The venue's lot |
| A lot an earlier `applyVenueLot` applied, now different from the venue's | `Applied` | The venue's new lot |
| Declared, equal to the venue's | `Unchanged` | Unchanged |
| Declared, different | `Conflict` | Unchanged: the declared lot is kept |
| No entry with this id | `UnknownSymbol` | - |
| Venue lot not positive | `InvalidTick` | Unchanged |

An applied lot is recorded in `venueTickLog()` (`lotFor(id)`), which is how the registry tells it
from a declared one on the next call, and it moves the log's revision as an applied tick does, so
the same holders follow it: a `SimulatedExecutor` that had already read "no lot" for the symbol
checks the next order against the applied one, and a strategy's `symbolInfos()` reports it. A lot
read back from a saved registry is a declared one.

## Symbol Equivalence

For cross-exchange trading, symbols on different exchanges can be mapped as equivalent:

```cpp
// Register symbols on different exchanges
auto btcBinance = registry.registerSymbol("binance", "BTCUSDT");
auto btcBybit = registry.registerSymbol("bybit", "BTCUSDT");
auto btcOkx = registry.registerSymbol("okx", "BTC-USDT");

// Map them as equivalent
registry.mapEquivalentSymbols({btcBinance, btcBybit, btcOkx});

// Query equivalents
auto equivalents = registry.getEquivalentSymbols(btcBinance);  // Returns all 3
auto bybitEquiv = registry.getEquivalentOnExchange(btcBinance, bybitExchangeId);
```

| Method                    | Description                                               |
| ------------------------- | --------------------------------------------------------- |
| `mapEquivalentSymbols`    | Links symbols as equivalent across exchanges.             |
| `getEquivalentSymbols`    | Returns a copy of the equivalence list, at most `kMaxEquivalentsPerSymbol` entries. |
| `getEquivalentOnExchange` | Returns equivalent symbol on a specific exchange.         |

## Persistence

| Method          | Description                                         |
| --------------- | --------------------------------------------------- |
| `saveToFile`    | Persists registry to a JSON file.                   |
| `loadFromFile`  | Loads registry from a JSON file.                    |
| `serialize`     | Returns binary representation as `std::vector<std::byte>`. |
| `deserialize`   | Restores state from binary data.                    |

Both writers emit the symbols in ascending id order, so two registries holding the same entries
write the same bytes however the entries were added; the order used to be the hash map's.

`saveToFile` writes a temporary file beside `path`, syncs it to the device (`fsync`, `_commit` on
Windows) and renames it over `path` only after every write, the flush, the sync and the close have
succeeded; on POSIX the directory is synced after the rename. The temporary is named
`<path>.<pid>.<n>.tmp`, with `n` a counter of the process, and is opened so that an existing name
is never reused: saves to one path from several threads or processes each write a whole file and
the last rename wins. The name used to be `<path>.tmp` for every save, so two concurrent saves
wrote through one file, and the data was not synced before the rename. It returns false when the
temporary cannot be created, when a write, the sync or the close fails (a full disk), and when the
rename fails; the file that was at `path` is then untouched and the temporary is removed. It used to open `path` itself for writing, check nothing
after that and return true, so a save that ran out of space returned true and had already
truncated the previous file.

A load either takes the whole input or none of it. `deserialize` returns false and leaves the
registry with no symbols when the blob is too short for its header, carries another magic or a
version this build does not read, ends before the last entry its count announces (every read is
bounds-checked; a truncated blob used to load entries built from zeros and return true), holds
an entry whose scale `registerSymbol` would refuse, holds two entries with one id, or has bytes
left after the last entry its count announces. `loadFromFile` returns false and leaves the
registry with no symbols when the file does not parse, when an entry of the `symbols` array
lacks its `id`, `exchange` or `symbol` (such an entry used to end the load quietly, with the entries
before it kept and those after it dropped), and when two entries carry one id. A duplicate id used
to load with the later entry in the table and both names resolving to it; trailing bytes used to
be ignored. A file that cannot be opened or is empty returns false
and leaves the registry as it was.

Both loads also fail, registry empty, when two entries carry one exchange and symbol under
different ids (the name resolved to one of them and the other entry could not be found by name)
and when an entry's id is above `kMaxSymbolId`. `loadFromFile` reads the whole document: one JSON
object with a `symbols` array and scalar members beside it. Anything but whitespace after the
object's closing brace fails the load; everything after the `symbols` array used to be ignored.
The `version` member is read: a value above `SymbolRegistry::kFileVersion` (1, the version
`saveToFile` writes) fails the load, because a later format may hold fields this build would
misread, and so does a value that is not a positive integer. A file with no `version` is read as
version 1. The member used to be skipped.

The overloads taking `std::string& why` set it to the reason for a `false`, for example
`registry.json: the file is version 2, newer than the version 1 this build reads and writes`,
`registry.json: two entries are named binance:BTCUSDT (ids 3 and 4)`,
`registry.json: content after the end of the document at byte 181` or
`1 byte(s) follow the last entry`, and to an empty string for a `true`. The exchanges and the equivalence table are not part of either
format and are not touched by a load, failed or not.

Both formats keep `tickSizeKnown`, so a symbol whose tick was declared is checked the same way
after a load as before the save. The JSON file writes `"tick_size_known": true` on such an entry;
the binary form sets bit `0x10` of the per-symbol flags byte. Neither is written for an undeclared
tick, so an undeclared symbol's entry is unchanged.

Data written before the flag was persisted loads with `tickSizeKnown` false on every symbol. An old
entry with the 0.01 tick is therefore not checked after the load, and an old entry with any other
tick is, which is how those symbols were treated before they were saved unless the 0.01 had been
declared with the flag. A registry saved with a declared 0.01 tick before the flag was persisted
loads it unchecked; register it again with `tickSizeKnown`, or save it again, to have it checked.
A reader that predates the flag ignores the bit and the key, and loads the symbol without it.

A declared lot is kept too. The JSON file writes `"lot_size": <raw quantity>` on an entry that
declares one and nothing on one that does not, so an entry without a lot is written exactly as
before and a file from before lots loads with none declared. The binary form writes version 5,
with the lot as eight bytes at the end of each entry whose flags byte has bit `0x20`, only when
some entry declares a lot; a registry without one serializes to the same version-4 bytes as before.
Versions 1 to 4 load with no lot. A build that predates lots refuses version 5 (`deserialize`
returns false) instead of reading the extra bytes as the next entry; its JSON loader ignores the
key.

Both formats also keep the per-symbol `priceScale` and `qtyScale`. The binary form has carried them
since version 4; the JSON file writes `"price_scale"` / `"qty_scale"` only on an entry whose scale is
not the 1e8 default, so a default-scale entry is written exactly as before. A JSON file without the
keys loads every symbol at 1e8, as it always did; a JSON entry whose scale `registerSymbol` would
refuse (not positive, or above 1e18) makes `loadFromFile` return false. The JSON
`contract_multiplier` is written with ten significant digits when that reads back as the same
double, and with seventeen otherwise.

`exchangeId` is in neither format: it is the loading registry's own id for the exchange, resolved
when a symbol is registered, and a loaded symbol carries `InvalidExchangeId`.

## Capacity Limits

| Constant                   | Value | Description                          |
| -------------------------- | ----- | ------------------------------------ |
| `kMaxExchanges`            | 32    | Maximum number of exchanges.         |
| `kMaxSymbols`              | 4096  | Maximum number of symbols.           |
| `kMaxEquivalentsPerSymbol` | 8     | Maximum equivalents per symbol.      |

## Internal Design

* Composite key `"exchange:symbol"` ensures O(1) forward lookups via `_map`.
* `_symbols` is an unordered_map for direct `id` lookups.
* `_symbolToExchange` provides O(1) symbol-to-exchange mapping.
* Equivalence uses flat storage: `[sym * kMaxEquivalentsPerSymbol ... (sym+1) * kMaxEquivalentsPerSymbol)` for O(1) lookup.
* A single shared mutex protects all structures. Registration is rare and takes it exclusively. Lookups are frequent, run concurrently with each other under a shared lock, and wait only while a registration or a `clear()` is in flight. Every accessor goes through it, `getExchange` and `getExchangeForSymbol` and `venueTypeForSymbol` included: reading `_symbolToExchange` while another thread resized it was a read past the end of a freed buffer.
* `registerExchange` fills the entry before the exchange count admits it, so an id a reader is allowed to dereference always carries a populated name and venue type.
* Accessors return values, never addresses into registry storage. `getExchange` used to return `const ExchangeInfo*` and `getEquivalentSymbols` a `std::span`, both pointing at memory the lock protected only until the accessor returned. The caller then read through them with nothing held, and a concurrent `clear()` or registration rewrote the storage underneath: a thread sanitizer run caught exactly that, an unsynchronized `nameView()` against `clear()` zeroing the table. Locking inside the accessor cannot fix it, because the unsafe read happens at the call site. Both copy out under the lock now.

## Notes

* Inherits from `ISubsystem` for lifecycle management integration.
* `InstrumentType` allows immediate filtering (`Spot`, `Future`, `Option`) without extra registry calls in hot paths.
* `SymbolId` remains a compact, contiguous 32-bit value suitable for array indices in event buses and order books.
* Option-specific fields (`strike`, `expiry`, `optionType`) are populated only when `type == InstrumentType::Option`.
* `tickSize` stores the minimum price increment for the instrument (default 0.01). Whether a backtest
  checks order prices against it is decided by `orderTickRaw` in `execution/order_price_rule.h`: a
  tick is checked when `tickSizeKnown` is set or the tick is not the 0.01 default, and never when it
  is zero. See [SimulatedExecutor](../backtest/simulated_executor.md#order-prices-and-the-tick).
* `lotSize` is the venue's order-quantity step. It is zero unless set, and then order quantities
  are not checked; a positive value is declared, and a backtest refuses an order whose quantity is
  not a whole number of it (`orderLotRaw` / `offLotReason` in `execution/order_price_rule.h`). It
  is the step only: `SymbolInfo` has no minimum order size. See
  [SimulatedExecutor](../backtest/simulated_executor.md#order-quantities-and-the-lot).
* `registerSymbol(info)` stores a copy of the whole `SymbolInfo`, flag and lot included. The
  name-only overloads declare no tick and no lot.
* `exchangeId` links symbol directly to its exchange for O(1) access.
* Persistence methods are useful for storing symbol mappings alongside recorded market data.
