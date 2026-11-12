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
  std::vector<std::byte> serialize() const;
  bool deserialize(std::span<const std::byte> data);

  // Utilities
  void clear();
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
* `registerSymbol(info)` stores a copy of the whole `SymbolInfo`, flag included. The name-only
  overloads declare no tick.
* `exchangeId` links symbol directly to its exchange for O(1) access.
* Persistence methods are useful for storing symbol mappings alongside recorded market data.
