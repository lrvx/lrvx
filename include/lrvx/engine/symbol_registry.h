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
#include <string_view>
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
  // The venue's order-quantity increment; zero when it is not declared, which
  // is the default. A simulated venue refuses an order whose quantity is not
  // a whole number of a declared lot (see execution/order_price_rule.h). It
  // is the step, not the smallest order the venue takes: a minimum size is a
  // separate venue rule and has no field here. saveToFile and serialize keep
  // it; data written before they did loads with it zero.
  Quantity lotSize{};
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

// What SymbolRegistry::applyVenueLot did, in applyVenueTick's terms:
// InvalidTick is a venue lot that is not positive.
struct VenueLotResult
{
  VenueTickOutcome outcome{VenueTickOutcome::UnknownSymbol};
  // The lot the entry holds after the call (zero when it holds none).
  Quantity heldLot{};
};

// The venue ticks applyVenueTick wrote into existing entries, for whoever
// copied an entry before the venue's tick was known. A Strategy sizes each
// symbol's book on the tick it read in its constructor, usually before any
// connector has started; it holds this log, not the registry, so the registry
// does not have to outlive it, and on its next event for a symbol it re-sizes
// that book when the revision moved.
//
// The lots applyVenueLot wrote are kept beside them, and move the revision
// the same way: a simulated executor caches each symbol's tick and lot and
// drops both caches when the revision moves, and the registry reads the lots
// back to tell one the venue wrote from one the caller declared.
class VenueTickLog
{
 public:
  // Bumped on every applied tick or lot and on clear(). An acquire load: a
  // reader that sees the new revision sees the value written before it.
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

  // The venue lot applied to this symbol, if one was.
  std::optional<Quantity> lotFor(SymbolId id) const
  {
    std::lock_guard lock(_mutex);
    auto it = _lots.find(id);
    if (it == _lots.end())
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

  void recordLot(SymbolId id, Quantity lot)
  {
    {
      std::lock_guard lock(_mutex);
      _lots[id] = lot;
    }
    _revision.fetch_add(1, std::memory_order_release);
  }

  void reset()
  {
    {
      std::lock_guard lock(_mutex);
      _ticks.clear();
      _lots.clear();
    }
    _revision.fetch_add(1, std::memory_order_release);
  }

  std::atomic<uint64_t> _revision{0};
  mutable std::mutex _mutex;
  std::unordered_map<SymbolId, Price> _ticks;
  std::unordered_map<SymbolId, Quantity> _lots;
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
  //
  // Every overload draws a new id from one sequence: one above the highest
  // id this registry has handed out or loaded, starting at 1. 0 is never a
  // symbol's id; it is what a registration that was refused returns. A name
  // that is already registered returns the id it has.
  //
  // registerSymbol(info) uses info.id when it is not 0. It returns 0 and
  // changes nothing when that id belongs to a symbol with another name, when
  // it is above kMaxSymbolId, and when the info's scales are not valid.
  //
  // kMaxSymbolId is the largest id a symbol may have, explicit or assigned.
  // Every registerSymbol overload returns 0 for an id above the limit, and a
  // load that holds one fails.
  //
  // The name, exchange and equivalence tables are indexed by id. Below
  // kDenseSymbolIds, where the ids of a registry normally are, each is an
  // array and getSymbolName / getExchangeForSymbol / getEquivalentSymbols an
  // array read; from there up each is a hash map, so one explicit id near
  // the limit is one entry in each table. The tables used to be arrays all
  // the way and that id sized them for every id below it, about 48 MB.
  static constexpr SymbolId kMaxSymbolId = 1'048'575;
  static constexpr SymbolId kDenseSymbolIds = 16'384;
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

  // The same for the venue's quantity step: writes it into an entry that
  // declares no lot (lotSize zero), or whose lot an earlier call applied, and
  // records it in venueTickLog(). A lot the caller declared is never
  // overwritten: a different venue lot is reported as Conflict and the entry
  // keeps the caller's. Only lotSize can change.
  VenueLotResult applyVenueLot(SymbolId id, Quantity venueLot);

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
  //
  // saveToFile writes a temporary file beside `path`, syncs it to the device
  // (fsync; _commit on Windows) and renames it over `path`. The temporary is
  // named `<path>.<pid>.<n>.tmp`, a name no other save uses, so saves to one
  // path from several threads or processes each write a whole file and the
  // last rename wins. It returns false when the temporary cannot be created,
  // a write, the sync or the close fails (a full disk), or the rename fails;
  // the file that was at `path` is then untouched and the temporary is
  // removed.
  //
  // loadFromFile and deserialize return false and leave the registry empty
  // for input that is not a whole registry: malformed or truncated data, an
  // entry with an invalid scale or an id above kMaxSymbolId, two entries
  // with one id, two entries with one exchange and symbol, a file entry with
  // a member that is itself an object or an array (an entry is flat), and
  // anything after the end (bytes after the last entry; for a file, anything
  // but whitespace after the closing brace of the document). A file's "version"
  // is read: one above kFileVersion, the version saveToFile writes, fails
  // the load, since a later format may hold fields this build would misread;
  // a file with no "version" is read as version 1. The overloads taking
  // `why` set it to the reason for a false, and to "" for a true.
  static constexpr uint32_t kFileVersion = 1;
  bool saveToFile(const std::filesystem::path& path) const;
  bool loadFromFile(const std::filesystem::path& path);
  bool loadFromFile(const std::filesystem::path& path, std::string& why);

  std::vector<std::byte> serialize() const;
  bool deserialize(std::span<const std::byte> data);
  bool deserialize(std::span<const std::byte> data, std::string& why);

  void clear();
  // In registration order (ascending id): front() is the first symbol
  // registered.
  std::vector<SymbolInfo> getAllSymbols() const;
  size_t size() const;

#ifdef LRVX_UNIT_TEST
  // Test-only: how many slots the three by-id arrays hold between them, so a
  // test can tell an array sized by the largest id from one that is not.
  size_t denseTableEntries() const
  {
    std::shared_lock lock(_mutex);
    return _reverse.denseSize() + _symbolToExchange.denseSize() + _equivalents.denseSize();
  }
#endif

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

  // The id the next new symbol takes, whichever overload registers it: one
  // above the highest id in use.
  SymbolId nextSymbolIdLocked() const;
  void setSymbolNameLocked(SymbolId id, std::string exchange, std::string symbol);
  // getSymbolName for an id the name array does not cover.
  std::pair<std::string, std::string> symbolNameAboveDenseLocked(SymbolId id) const;

 public:
  // A table indexed by symbol id: an array for the ids below kDenseSymbolIds
  // and a hash map from there up. A lookup of a small id is the bounds check
  // and the index it always was; a large id costs one entry, not an array
  // sized up to it.
  template <typename T>
  class IdTable
  {
   public:
    // nullptr for an id this table has no slot for. An id below the dense
    // size that nobody wrote reads as a default T, as the arrays did.
    const T* find(SymbolId id) const noexcept
    {
      if (id < _dense.size())
      {
        return &_dense[id];
      }
      if (id < kDenseSymbolIds || _sparse.empty())
      {
        return nullptr;
      }
      const auto it = _sparse.find(id);
      return it == _sparse.end() ? nullptr : &it->second;
    }
    // The value at `id`, or `fallback` where find() answers nullptr. The
    // small-id path is the one compare and one index the arrays had; the
    // hash map is kept out of it.
    T valueOr(SymbolId id, const T& fallback) const noexcept
    {
      if (id < _dense.size()) [[likely]]
      {
        return _dense[id];
      }
      return sparseValueOr(id, fallback);
    }
    // The slot of `id`, made if absent; slots made on the way are `fill`.
    T& slot(SymbolId id, const T& fill = T{})
    {
      if (id < kDenseSymbolIds)
      {
        if (_dense.size() <= id)
        {
          _dense.resize(static_cast<size_t>(id) + 1, fill);
        }
        return _dense[id];
      }
      return _sparse.try_emplace(id, fill).first->second;
    }
    void clear()
    {
      _dense.clear();
      _sparse.clear();
    }
    size_t denseSize() const noexcept { return _dense.size(); }
    // The array's slot, for an id below denseSize().
    const T& denseAt(SymbolId id) const noexcept { return _dense[id]; }

   private:
    T sparseValueOr(SymbolId id, const T& fallback) const noexcept
    {
      if (id < kDenseSymbolIds || _sparse.empty())
      {
        return fallback;
      }
      const auto it = _sparse.find(id);
      return it == _sparse.end() ? fallback : it->second;
    }

    std::vector<T> _dense;
    std::unordered_map<SymbolId, T> _sparse;
  };

  // The key a name is found under. An exchange and a symbol joined with ':'
  // gave ("a:b", "c") and ("a", "b:c") one key; the exchange's length in
  // front says where it ends.
  static std::string nameKey(std::string_view exchange, std::string_view symbol);

 private:
  mutable std::shared_mutex _mutex;
  std::unordered_map<SymbolId, SymbolInfo> _symbols;
  std::unordered_map<std::string, SymbolId> _map;
  IdTable<std::pair<std::string, std::string>> _reverse;
  // One above the highest id in use; 1 for an empty registry.
  SymbolId _nextSymbolId{1};

  // Exchange storage
  std::array<ExchangeInfo, kMaxExchanges> _exchanges{};
  std::unordered_map<std::string, ExchangeId> _exchangeNameToId;
  size_t _numExchanges{0};

  // Symbol-to-exchange mapping (O(1) lookup)
  IdTable<ExchangeId> _symbolToExchange;

  // Equivalence: the ids each symbol is mapped to (O(1) lookup)
  IdTable<EquivalentSymbols> _equivalents;

  std::shared_ptr<VenueTickLog> _venueTicks{std::make_shared<VenueTickLog>()};
};

}  // namespace lrvx
