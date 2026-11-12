/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "lrvx/common.h"
#include "lrvx/engine/abstract_subsystem.h"
#include "lrvx/exchange/exchange_info.h"
#include "lrvx/util/base/time.h"

namespace lrvx
{

// Tick a symbol gets when it is registered without one. Books need a positive
// tick, so they use it; order prices are not checked against it.
inline constexpr Price kDefaultTickSize = Price::fromRaw(Price::Scale / 100);

struct SymbolInfo
{
  SymbolId id{0};
  std::string exchange;
  std::string symbol;
  InstrumentType type = InstrumentType::Spot;
  Price tickSize{kDefaultTickSize};
  // True when tickSize is the venue's price increment. A simulated venue
  // refuses order prices off a known tick (see execution/order_price_rule.h).
  // Any tickSize other than the default counts as known without this flag; set
  // it to have an explicit 0.01 checked. saveToFile and serialize keep it; data
  // written before they did loads with it clear.
  bool tickSizeKnown{false};
  ExchangeId exchangeId{InvalidExchangeId};

  std::optional<Price> strike;
  std::optional<TimePoint> expiry;
  std::optional<OptionType> optionType;

  // Contract spec. Defaults preserve spot/perp behavior: a 1.0 multiplier, cash
  // settlement, European exercise. Options override these per the venue
  // (US equity 100x, physical-settled American; crypto 1x, cash, European).
  double contractMultiplier{1.0};
  SettlementType settlementType{SettlementType::Cash};
  ExerciseStyle exerciseStyle{ExerciseStyle::European};
  std::optional<std::string> settlementCcy;

  // Per-symbol fixed-point scale. Default 1e8 reproduces the
  // compile-time Price/Quantity scale exactly, so CEX symbols and existing
  // persisted registries are unchanged. A DEX token whose price (~1e-10) or
  // supply (~1e12) range does not fit 1e8 in int64 sets a different scale
  // here and converts through the scale-aware Decimal overloads.
  int64_t priceScale{Price::Scale};
  int64_t qtyScale{Quantity::Scale};
};

// Validate a symbol's per-symbol scale before registration. A scale must
// be positive and small enough to leave usable integer
// headroom in int64. The default 1e8 always passes, so CEX symbols and old
// registries are never rejected.
inline std::expected<void, std::string> validateSymbolScale(const SymbolInfo& info)
{
  // 1e18 still leaves ~9 integer units of headroom in int64.
  static constexpr int64_t kMaxScale = 1'000'000'000'000'000'000;
  if (info.priceScale <= 0)
  {
    return std::unexpected<std::string>("priceScale must be positive");
  }
  if (info.qtyScale <= 0)
  {
    return std::unexpected<std::string>("qtyScale must be positive");
  }
  if (info.priceScale > kMaxScale)
  {
    return std::unexpected<std::string>("priceScale too large for int64 headroom");
  }
  if (info.qtyScale > kMaxScale)
  {
    return std::unexpected<std::string>("qtyScale too large for int64 headroom");
  }
  return {};
}

// What SymbolRegistry::applyVenueTick did to an existing entry.
enum class VenueTickOutcome : uint8_t
{
  // The entry's tick was not declared, or was one an earlier call applied;
  // it now holds the venue's, known.
  Applied,
  // The entry already declares the venue's tick.
  Unchanged,
  // The entry declares a different tick. It keeps it: a declared tick is the
  // caller's, and only the caller changes it.
  Conflict,
  // No entry has this id.
  UnknownSymbol,
  // The venue tick is not positive; nothing changed.
  InvalidTick,
};

struct VenueTickResult
{
  VenueTickOutcome outcome{VenueTickOutcome::UnknownSymbol};
  // The tick the entry holds after the call (zero for UnknownSymbol).
  Price heldTick{};
};

// The venue ticks applyVenueTick wrote into existing entries, for whoever
// copied an entry before the venue's tick was known. A Strategy sizes each
// symbol's book on the tick it read in its constructor, usually before any
// connector has started; it holds this log, not the registry, so the registry
// does not have to outlive it, and on its next event for a symbol it re-sizes
// that book when the revision moved.
class VenueTickLog
{
 public:
  // Bumped on every applied tick and on clear(). An acquire load: a reader
  // that sees the new revision sees the tick written before it.
  uint64_t revision() const noexcept { return _revision.load(std::memory_order_acquire); }

  // The venue tick applied to this symbol, if one was.
  std::optional<Price> tickFor(SymbolId id) const
  {
    std::lock_guard lock(_mutex);
    auto it = _ticks.find(id);
    if (it == _ticks.end())
    {
      return std::nullopt;
    }
    return it->second;
  }

 private:
  friend class SymbolRegistry;

  void record(SymbolId id, Price tick)
  {
    {
      std::lock_guard lock(_mutex);
      _ticks[id] = tick;
    }
    _revision.fetch_add(1, std::memory_order_release);
  }

  void reset()
  {
    {
      std::lock_guard lock(_mutex);
      _ticks.clear();
    }
    _revision.fetch_add(1, std::memory_order_release);
  }

  std::atomic<uint64_t> _revision{0};
  mutable std::mutex _mutex;
  std::unordered_map<SymbolId, Price> _ticks;
};

class SymbolRegistry : public ISubsystem
{
 public:
  static constexpr size_t kMaxExchanges = 32;
  static constexpr size_t kMaxSymbols = 4096;
  static constexpr size_t kMaxEquivalentsPerSymbol = 8;

  // A copy of the equivalence list, taken while the registry lock is held.
  // Small and fixed-size, so the copy costs a few words and never allocates.
  struct EquivalentSymbols
  {
    std::array<SymbolId, kMaxEquivalentsPerSymbol> ids{};
    uint8_t count{0};

    const SymbolId* begin() const { return ids.data(); }
    const SymbolId* end() const { return ids.data() + count; }
    size_t size() const { return count; }
    bool empty() const { return count == 0; }
    SymbolId operator[](size_t i) const { return ids[i]; }
  };

  // Exchange management
  ExchangeId registerExchange(std::string_view name,
                              VenueType type = VenueType::CentralizedExchange);

  // Returns a copy of the entry, not a pointer into the registry. A pointer
  // would outlive the lock that made it safe to read: the caller dereferences
  // it after the getter has returned, and by then a clear() or a later
  // registration may already be rewriting that slot. No locking inside the
  // getter can fix that, because the unsafe read happens at the call site.
  std::optional<ExchangeInfo> getExchange(ExchangeId id) const;
  ExchangeId getExchangeId(std::string_view name) const;
  size_t exchangeCount() const
  {
    std::shared_lock lock(_mutex);
    return _numExchanges;
  }

  // Symbol registration (legacy string-based API)
  SymbolId registerSymbol(const std::string& exchange, const std::string& symbol);
  SymbolId registerSymbol(const SymbolInfo& info);
  std::optional<SymbolId> getSymbolId(const std::string& exchange,
                                      const std::string& symbol) const;
  std::optional<SymbolInfo> getSymbolInfo(SymbolId id) const;
  std::pair<std::string, std::string> getSymbolName(SymbolId id) const;

  // Writes a venue's price tick into an entry registered before the venue's
  // instrument info was read (a connector calls it from start()). Only
  // tickSize and tickSizeKnown can change, and only when the entry's tick is
  // not declared (see orderTickRaw in execution/order_price_rule.h: neither
  // tickSizeKnown nor a value other than the 0.01 default). The id, names,
  // type, scales and everything else stay as registered, since consumers
  // index by them. A declared tick is never overwritten: a different venue
  // tick is reported as Conflict and the entry keeps the caller's value. A
  // tick this call applied is not a declaration: the venue may change it
  // again (a connector calls again when the venue moves a market's tick
  // mid-session), and each change is recorded.
  // Atomic with respect to every other registry call; an applied tick is
  // recorded in venueTickLog().
  VenueTickResult applyVenueTick(SymbolId id, Price venueTick);

  // Shared, so a holder may outlive the registry.
  std::shared_ptr<const VenueTickLog> venueTickLog() const noexcept { return _venueTicks; }

  // Symbol registration (ExchangeId-based API)
  SymbolId registerSymbol(ExchangeId exchange, std::string_view symbol);
  ExchangeId getExchangeForSymbol(SymbolId symbol) const;

  // The venue type of the symbol's exchange. Defaults to CentralizedExchange
  // when the symbol or its exchange is unknown. See venue_behavior.h to turn
  // a venue type into routing decisions.
  VenueType venueTypeForSymbol(SymbolId symbol) const;

  // Symbol equivalence (cross-exchange mapping)
  void mapEquivalentSymbols(std::span<const SymbolId> equivalentSymbols);
  EquivalentSymbols getEquivalentSymbols(SymbolId symbol) const;
  SymbolId getEquivalentOnExchange(SymbolId symbol, ExchangeId exchange) const;

  // Persistence
  bool saveToFile(const std::filesystem::path& path) const;
  bool loadFromFile(const std::filesystem::path& path);

  std::vector<std::byte> serialize() const;
  bool deserialize(std::span<const std::byte> data);

  void clear();
  // In registration order (ascending id): front() is the first symbol
  // registered.
  std::vector<SymbolInfo> getAllSymbols() const;
  size_t size() const;

 private:
  // The getters below used to read the exchange table and the symbol-to-
  // exchange vector with no lock at all, while a writer resized that vector
  // under the mutex. The reads are frequent and the writes are rare, so the
  // mutex is a shared one: readers run concurrently with each other and are
  // excluded only while a registration or a clear() is in flight.
  //
  // Public getters take the lock once and go through these helpers, because a
  // getter calling another getter would take a shared lock twice on one
  // thread and can deadlock against a waiting writer. These return references
  // into registry storage and are only ever used while the lock is held.
  const ExchangeInfo* getExchangeLocked(ExchangeId id) const;
  ExchangeId getExchangeForSymbolLocked(SymbolId symbol) const;
  std::span<const SymbolId> getEquivalentSymbolsLocked(SymbolId symbol) const;

  mutable std::shared_mutex _mutex;
  std::unordered_map<SymbolId, SymbolInfo> _symbols;
  std::unordered_map<std::string, SymbolId> _map;
  std::vector<std::pair<std::string, std::string>> _reverse;

  // Exchange storage
  std::array<ExchangeInfo, kMaxExchanges> _exchanges{};
  std::unordered_map<std::string, ExchangeId> _exchangeNameToId;
  size_t _numExchanges{0};

  // Symbol-to-exchange mapping (O(1) lookup)
  std::vector<ExchangeId> _symbolToExchange;

  // Equivalence: flat storage for O(1) lookup
  // [sym * kMaxEquivalentsPerSymbol ... (sym+1) * kMaxEquivalentsPerSymbol)
  std::vector<SymbolId> _equivalents;
  std::vector<uint8_t> _equivalentCounts;

  std::shared_ptr<VenueTickLog> _venueTicks{std::make_shared<VenueTickLog>()};
};

}  // namespace lrvx
