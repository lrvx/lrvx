/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "lrvx/common.h"
#include "lrvx/log/log.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <deque>
#include <memory>
#include <type_traits>
#include <utility>

namespace lrvx
{

// Flat per-symbol state table.
//
// THREAD SAFETY: none. This is a container, not a synchronisation
// primitive: concurrent access to the same symbol's slot has to be ordered
// by whoever owns the state. `lrvx::Strategy` does that with a per-symbol
// lock; see the ownership note in lrvx/strategy/strategy.h.
template <typename State, size_t MaxSymbols = 256>
class SymbolStateMap
{
 public:
  static constexpr size_t kMaxSymbols = MaxSymbols;

  SymbolStateMap() = default;

  // Movable, never copyable. A move hands the table over and gives the
  // source a fresh empty one rather than leaving it null: every accessor
  // dereferences `_table`, and a moved-from map that answered `operator[]`
  // through a null pointer would turn a use-after-move into a crash at an
  // unrelated address. Policies are moved into aggregators at wiring time,
  // never on a hot path, so the allocation is not in anyone's way; being
  // noexcept it can only fail by terminating, which is the same outcome as
  // failing to construct the map in the first place.
  SymbolStateMap(const SymbolStateMap&) = delete;
  SymbolStateMap& operator=(const SymbolStateMap&) = delete;

  SymbolStateMap(SymbolStateMap&& other) noexcept
      : _table(std::exchange(other._table, std::make_unique<Table>())),
        _overflowStorage(std::move(other._overflowStorage))
  {
  }

  SymbolStateMap& operator=(SymbolStateMap&& other) noexcept
  {
    if (this != &other)
    {
      _table.swap(other._table);
      _overflowStorage = std::move(other._overflowStorage);
    }
    return *this;
  }

  [[nodiscard]] State& operator[](SymbolId symbol) noexcept
  {
    if (symbol < kMaxSymbols) [[likely]]
    {
      // Read before write. A blind store dirtied a cache line on every
      // single event, and the line is shared by 64 neighbouring symbols,
      // so bus threads working on unrelated symbols bounced it between
      // cores for a flag that is only ever set once.
      if (!_table->initialized[symbol])
      {
        _table->initialized[symbol] = true;
      }
      return _table->flat[symbol];
    }
    // For non-movable types there is no growable overflow storage: even
    // with the deque below (stable addresses, no reallocation), inserting
    // a new entry still move-constructs a State into the pair, and
    // non-movable State holds atomics. Route the write to a dedicated
    // scratch slot instead of
    // `flat[0]`: aliasing symbol 0 silently corrupted a live, unrelated
    // symbol's data (and the assert that was meant to catch this in
    // debug compiles out entirely under NDEBUG, i.e. in every release
    // build). The write is still lost — this map genuinely has no room
    // for it — but it no longer lands on someone else's data.
    if constexpr (!std::is_move_constructible_v<State>)
    {
      assert(false && "SymbolId exceeds MaxSymbols for non-movable type");
      LRVX_LOG_ERROR("SymbolStateMap: symbol "
                     << symbol << " exceeds MaxSymbols (" << kMaxSymbols
                     << ") for a non-movable State; routing to a shared overflow "
                        "scratch slot instead of aliasing symbol 0");
      return _overflowScratch;
    }
    else
    {
      return getOverflow(symbol);
    }
  }

  [[nodiscard]] const State& operator[](SymbolId symbol) const noexcept
  {
    if (symbol < kMaxSymbols) [[likely]]
    {
      return _table->flat[symbol];
    }
    if constexpr (!std::is_move_constructible_v<State>)
    {
      assert(false && "SymbolId exceeds MaxSymbols for non-movable type");
      LRVX_LOG_ERROR("SymbolStateMap: symbol "
                     << symbol << " exceeds MaxSymbols (" << kMaxSymbols
                     << ") for a non-movable State; reading the shared overflow "
                        "scratch slot instead of aliasing symbol 0");
      return _overflowScratch;
    }
    else
    {
      return getOverflowConst(symbol);
    }
  }

  [[nodiscard]] bool contains(SymbolId symbol) const noexcept
  {
    if (symbol < kMaxSymbols)
    {
      return _table->initialized[symbol];
    }
    if constexpr (std::is_move_constructible_v<State>)
    {
      for (const auto& [id, _] : overflow())
      {
        if (id == symbol)
        {
          return true;
        }
      }
    }
    return false;
  }

  [[nodiscard]] State* tryGet(SymbolId symbol) noexcept
  {
    if (symbol < kMaxSymbols)
    {
      return _table->initialized[symbol] ? &_table->flat[symbol] : nullptr;
    }
    if constexpr (std::is_move_constructible_v<State>)
    {
      for (auto& [id, state] : overflow())
      {
        if (id == symbol)
        {
          return &state;
        }
      }
    }
    return nullptr;
  }

  [[nodiscard]] const State* tryGet(SymbolId symbol) const noexcept
  {
    if (symbol < kMaxSymbols)
    {
      return _table->initialized[symbol] ? &_table->flat[symbol] : nullptr;
    }
    if constexpr (std::is_move_constructible_v<State>)
    {
      for (const auto& [id, state] : overflow())
      {
        if (id == symbol)
        {
          return &state;
        }
      }
    }
    return nullptr;
  }

  void clear() noexcept
  {
    // Destroy-and-reconstruct in place rather than `flat = {}`: assigning a
    // freshly-defaulted array requires State to be move- (or copy-)
    // assignable, which the non-movable case (State holding atomics) is
    // not, so that assignment used to be skipped entirely for it -- leaving
    // the old data behind under freshly-cleared `initialized` flags, i.e.
    // the next `operator[]` on that symbol handed back the previous run's
    // state as if it were new. Placement construction only needs State to
    // be default-constructible, which every State here already is (the
    // `Table` and the overflow scratch slot both default-construct it).
    for (State& state : _table->flat)
    {
      std::destroy_at(&state);
      std::construct_at(&state);
    }
    _table->initialized = {};
    _overflowStorage.clear();
    if constexpr (!std::is_move_constructible_v<State>)
    {
      std::destroy_at(&_overflowScratch);
      std::construct_at(&_overflowScratch);
    }
  }

  template <typename Func>
  void forEach(Func&& fn)
  {
    for (size_t i = 0; i < kMaxSymbols; ++i)
    {
      if (_table->initialized[i])
      {
        fn(static_cast<SymbolId>(i), _table->flat[i]);
      }
    }
    if constexpr (std::is_move_constructible_v<State>)
    {
      for (auto& [id, state] : overflow())
      {
        fn(id, state);
      }
    }
  }

  template <typename Func>
  void forEach(Func&& fn) const
  {
    for (size_t i = 0; i < kMaxSymbols; ++i)
    {
      if (_table->initialized[i])
      {
        fn(static_cast<SymbolId>(i), _table->flat[i]);
      }
    }
    if constexpr (std::is_move_constructible_v<State>)
    {
      for (const auto& [id, state] : overflow())
      {
        fn(id, state);
      }
    }
  }

  size_t size() const noexcept
  {
    size_t count = 0;
    for (size_t i = 0; i < kMaxSymbols; ++i)
    {
      if (_table->initialized[i])
      {
        ++count;
      }
    }
    return count + _overflowStorage.size();
  }

  // ── STL-compatible iterator ────────────────────────────────────────────────

  template <bool Const>
  class Iterator
  {
   public:
    using map_type =
        std::conditional_t<Const, const SymbolStateMap, SymbolStateMap>;
    using state_ref = std::conditional_t<Const, const State&, State&>;
    using value_type = std::pair<SymbolId, state_ref>;

    Iterator(map_type* map, size_t flatIdx, size_t overflowIdx)
        : _map(map), _flatIdx(flatIdx), _overflowIdx(overflowIdx)
    {
      advanceToValid();
    }

    value_type operator*() const
    {
      if (_flatIdx < kMaxSymbols)
      {
        return {static_cast<SymbolId>(_flatIdx), _map->_table->flat[_flatIdx]};
      }
      if constexpr (std::is_move_constructible_v<State>)
      {
        auto& entry = _map->overflow()[_overflowIdx];
        return {entry.first, entry.second};
      }
      // Unreachable for non-movable types (no overflow)
#ifdef _MSC_VER
      __assume(false);
#else
      __builtin_unreachable();
#endif
    }

    Iterator& operator++()
    {
      if (_flatIdx < kMaxSymbols)
      {
        ++_flatIdx;
      }
      else
      {
        ++_overflowIdx;
      }
      advanceToValid();
      return *this;
    }

    bool operator!=(const Iterator& other) const
    {
      return _flatIdx != other._flatIdx || _overflowIdx != other._overflowIdx;
    }

    bool operator==(const Iterator& other) const { return !(*this != other); }

   private:
    void advanceToValid()
    {
      while (_flatIdx < kMaxSymbols && !_map->_table->initialized[_flatIdx])
      {
        ++_flatIdx;
      }
    }

    map_type* _map;
    size_t _flatIdx;
    size_t _overflowIdx;
  };

  using iterator = Iterator<false>;
  using const_iterator = Iterator<true>;

  iterator begin() noexcept
  {
    return iterator(this, 0, 0);
  }

  iterator end() noexcept
  {
    size_t overflowSize = 0;
    if constexpr (std::is_move_constructible_v<State>)
    {
      overflowSize = overflow().size();
    }
    return iterator(this, kMaxSymbols, overflowSize);
  }

  const_iterator begin() const noexcept
  {
    return const_iterator(this, 0, 0);
  }

  const_iterator end() const noexcept
  {
    size_t overflowSize = 0;
    if constexpr (std::is_move_constructible_v<State>)
    {
      overflowSize = overflow().size();
    }
    return const_iterator(this, kMaxSymbols, overflowSize);
  }

 private:
  // Grows `overflow()` by one entry. `State&` results handed out by earlier
  // calls (and by tryGet/iterators) must stay valid across this -- see the
  // std::deque choice on OverflowStorage below.
  State& getOverflow(SymbolId symbol)
    requires std::is_move_constructible_v<State>
  {
    for (auto& [id, state] : overflow())
    {
      if (id == symbol)
      {
        return state;
      }
    }
    overflow().emplace_back(symbol, State{});
    return overflow().back().second;
  }

  const State& getOverflowConst(SymbolId symbol) const
    requires std::is_move_constructible_v<State>
  {
    for (const auto& [id, state] : overflow())
    {
      if (id == symbol)
      {
        return state;
      }
    }
    static const State empty{};
    return empty;
  }

  // Helper to conditionally include overflow storage.
  //
  // std::deque, not std::vector: growing a vector past its capacity
  // reallocates and moves every existing element, which invalidates every
  // reference `operator[]`/`tryGet`/an iterator has already handed out --
  // `State& a = map[300]; State& b = map[301];` could leave `a` pointing at
  // freed memory. A deque's push/emplace at either end never relocates
  // existing elements (only its internal map of chunks grows), so
  // references and pointers into it survive later insertions; only
  // iterators are invalidated, and this class does not cache those across
  // a mutation.
  template <typename T, bool Enable>
  struct OverflowStorage
  {
    std::deque<std::pair<SymbolId, T>> data;
    void clear() { data.clear(); }
    size_t size() const { return data.size(); }
  };

  template <typename T>
  struct OverflowStorage<T, false>
  {
    void clear() {}
    size_t size() const { return 0; }
  };

  // The flat table dominates this type's footprint, and it used to sit
  // inside whatever object held the map. With a 512-level order book as the
  // State that is 8,384 bytes per slot in a release build and 16,640 with
  // LRVX_SCALE_CHECKS on (the checks widen Price and Quantity from 8 to 16
  // bytes), so 256 slots came to roughly 2 MB release and 4 MB checked --
  // by value. A Strategy holding one therefore could not go on the stack:
  // two of them in a single frame overran a default 8 MB stack in a checked
  // build, and the overrun landed in the constructor prologue, before a
  // line of the strategy had run.
  //
  // One heap block, allocated once at construction, owned by the map. The
  // map itself is then a pointer plus the overflow bookkeeping, so holding
  // it by value costs nothing worth counting. The hot path pays one extra
  // dependent load, which is in L1 after the first event for the object.
  struct Table
  {
    alignas(64) std::array<State, kMaxSymbols> flat{};
    std::array<bool, kMaxSymbols> initialized{};
  };

  std::unique_ptr<Table> _table{std::make_unique<Table>()};
  OverflowStorage<State, std::is_move_constructible_v<State>> _overflowStorage;

  // Single shared landing slot for out-of-range writes when State is not
  // move-constructible (see operator[] above). Never aliases `flat[0]` or
  // any other real symbol; not reachable through tryGet/contains/forEach,
  // so it never masquerades as a real symbol's data either.
  std::conditional_t<std::is_move_constructible_v<State>, char, State> _overflowScratch{};

  // Accessor for overflow (only valid for movable types)
  auto& overflow()
    requires std::is_move_constructible_v<State>
  {
    return _overflowStorage.data;
  }

  const auto& overflow() const
    requires std::is_move_constructible_v<State>
  {
    return _overflowStorage.data;
  }
};

}  // namespace lrvx
