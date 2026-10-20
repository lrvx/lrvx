/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * WHEN TO USE: aggregate L2 market-data view (exchange depth feed in, best/top
 * levels out). Flat tick-indexed array with a cached best cursor; no per-order
 * state, no matching. For order-level books see lrvx/book/l3/l3_order_book.h
 * (market data) and lrvx/book/ladder_book.h (venue matching).
 */

#pragma once

#include "lrvx/book/abstract_order_book.h"
#include "lrvx/book/events/book_update_event.h"
#include "lrvx/common.h"
#include "lrvx/util/base/math.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <optional>
#include <stdexcept>
#include <type_traits>

namespace lrvx
{

template <size_t MaxLevels = 8192>
class NLevelOrderBook : public IOrderBook
{
 public:
  static constexpr size_t MAX_LEVELS = MaxLevels;

  // The tick size arrives from instrument configuration and from every binding,
  // so a bad one is bad input rather than a programmer error and is rejected
  // here. A zero tick used to divide by zero while building the reciprocal and
  // leave a book that answered "one tick" for every price and a price of zero
  // for every index, for the rest of its life and without a single diagnostic.
  explicit NLevelOrderBook(Price tickSize)
      : _tickSize(tickSize)
  {
    if (_tickSize.raw() <= 0)
    {
      throw std::invalid_argument("NLevelOrderBook: tick size must be positive");
    }
    _tickSizeDiv = math::make_fastdiv64((uint64_t)_tickSize.raw(), 1);
    clear();
  }

  inline std::optional<size_t> bestAskIndex() const
  {
    if (_bestAskIdx < MAX_LEVELS)
    {
      return _bestAskIdx;
    }

    if (_minAsk >= MAX_LEVELS)
    {
      return std::nullopt;
    }

    for (size_t i = _minAsk; i <= _maxAsk && i < MAX_LEVELS; ++i)
    {
      if (!_asks[i].isZero())
      {
        return i;
      }
    }
    return std::nullopt;
  }

  inline std::optional<size_t> bestBidIndex() const
  {
    if (_bestBidIdx < MAX_LEVELS)
    {
      return _bestBidIdx;
    }

    if (_minBid >= MAX_LEVELS)
    {
      return std::nullopt;
    }

    for (size_t i = normalizeUpperBound(_maxBid) + 1; i-- > _minBid;)
    {
      if (!_bids[i].isZero())
      {
        return i;
      }

      if (i == 0)
      {
        break;
      }
    }
    return std::nullopt;
  }

  void dump(std::ostream& os, size_t levels,
            int pricePrec = 4, int qtyPrec = 3, bool ansi = false) const
  {
    if (levels == 0)
    {
      return;
    }

    if (levels > 512)
    {
      levels = 512;
    }

    auto aBest = bestAsk();
    auto bBest = bestBid();

    const double ts = _tickSize.toDouble();
    os.setf(std::ios::fixed);
    os << "tick=" << std::setprecision(pricePrec) << ts
       << "  baseIndex=" << _baseIndex;

    if (aBest && bBest)
    {
      const double ba = aBest->toDouble(), bb = bBest->toDouble();
      os << "  spread=" << std::setprecision(pricePrec) << (ba - bb)
         << "  mid=" << std::setprecision(pricePrec) << ((ba + bb) * 0.5);
    }
    os << "\n";

    struct Row
    {
      double px{0}, qty{0};
      bool have{false};
    };

    std::array<Row, 512> asks{}, bids{};
    size_t na = 0, nb = 0;

    if (auto aIdxOpt = bestAskIndex())
    {
      for (size_t i = *aIdxOpt; i <= _maxAsk && i < MAX_LEVELS && na < levels; ++i)
      {
        if (_asks[i].isZero())
        {
          continue;
        }

        asks[na].have = true;
        asks[na].px = indexToPrice(i).toDouble();
        asks[na].qty = _asks[i].toDouble();
        ++na;
      }
    }

    if (auto bIdxOpt = bestBidIndex())
    {
      for (size_t i = *bIdxOpt + 1; i-- > _minBid && nb < levels;)
      {
        if (_bids[i].isZero())
        {
          if (i == 0)
          {
            break;
          }
          continue;
        }

        bids[nb].have = true;
        bids[nb].px = indexToPrice(i).toDouble();
        bids[nb].qty = _bids[i].toDouble();
        ++nb;

        if (i == 0)
        {
          break;
        }
      }
    }

    auto num_len = [](double v, int prec) -> int
    {
      char buf[64];
      auto r = std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::fixed, prec);
      return int(r.ptr - buf);
    };

    auto print_num = [](std::ostream& o, double v, int prec, int width)
    {
      char buf[64];
      auto r = std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::fixed, prec);
      int len = int(r.ptr - buf);
      for (int i = 0; i < width - len; ++i)
      {
        o.put(' ');
      }
      o.write(buf, len);
    };

    auto print_dash = [](std::ostream& o, int width)
    {
      for (int i = 0; i < width - 1; ++i)
      {
        o.put(' ');
      }
      o.put('-');
    };

    int wQty = 7;
    int wPx = 6;

    for (size_t i = 0; i < na; ++i)
    {
      wQty = std::max(wQty, num_len(asks[i].qty, qtyPrec));
      wPx = std::max(wPx, num_len(asks[i].px, pricePrec));
    }

    for (size_t i = 0; i < nb; ++i)
    {
      wQty = std::max(wQty, num_len(bids[i].qty, qtyPrec));
      wPx = std::max(wPx, num_len(bids[i].px, pricePrec));
    }

    const char* RED = ansi ? "\033[31m" : "";
    const char* GRN = ansi ? "\033[32m" : "";
    const char* DIM = ansi ? "\033[2m" : "";
    const char* RST = ansi ? "\033[0m" : "";

    os << "  " << std::setw(wQty) << "ASK_QTY"
       << "  " << std::setw(wPx) << "ASK_PX"
       << "  " << DIM << "│" << RST << "  "
       << std::setw(wPx) << "BID_PX"
       << "  " << std::setw(wQty) << "BID_QTY"
       << "\n";

    const size_t rows = std::max(na, nb);

    for (size_t r = 0; r < rows; ++r)
    {
      os << "  ";
      if (r < na && asks[r].have)
      {
        os << RED;
        print_num(os, asks[r].qty, qtyPrec, wQty);
        os << RST;
      }
      else
      {
        print_dash(os, wQty);
      }

      os << "  ";
      if (r < na && asks[r].have)
      {
        os << RED;
        print_num(os, asks[r].px, pricePrec, wPx);
        os << RST;
      }
      else
      {
        print_dash(os, wPx);
      }

      os << "  " << DIM << "│" << RST << "  ";

      if (r < nb && bids[r].have)
      {
        os << GRN;
        print_num(os, bids[r].px, pricePrec, wPx);
        os << RST;
      }
      else
      {
        print_dash(os, wPx);
      }

      os << "  ";
      if (r < nb && bids[r].have)
      {
        os << GRN;
        print_num(os, bids[r].qty, qtyPrec, wQty);
        os << RST;
      }
      else
      {
        print_dash(os, wQty);
      }

      os << "\n";
    }
  }

  void applyBookUpdate(const BookUpdateEvent& ev) override
  {
    const auto& up = ev.update;

    // The touch, not the book (see BookUpdateType). Levels this book holds
    // behind the touch are still valid, so there is nothing a depth book
    // can safely do with one-level frames; it keeps tracking the depth feed.
    if (up.type == BookUpdateType::TOP_OF_BOOK)
    {
      return;
    }

    if (up.type == BookUpdateType::SNAPSHOT)
    {
      int64_t minIdx = std::numeric_limits<int64_t>::max();
      int64_t maxIdx = std::numeric_limits<int64_t>::min();

      auto acc = [&](const auto& vec, Side side)
      {
        for (const auto& [p, _] : vec)
        {
          const int64_t t = ticks(p, side);
          if (t < minIdx)
          {
            minIdx = t;
          }
          if (t > maxIdx)
          {
            maxIdx = t;
          }
        }
      };

      acc(up.bids, Side::BUY);
      acc(up.asks, Side::SELL);

      if (minIdx == std::numeric_limits<int64_t>::max())
      {
        clear();
      }
      else
      {
        reanchor(minIdx, maxIdx);
      }

      _bids.fill({});
      _asks.fill({});
      _minBid = _minAsk = MAX_LEVELS;
      _maxBid = _maxAsk = 0;
      _bestBidIdx = _bestAskIdx = MAX_LEVELS;
    }
    else
    {
      reanchorForDelta(up);
    }

    for (const auto& [p, q] : up.bids)
    {
      const size_t i = localIndex(p, Side::BUY);
      if (i >= MAX_LEVELS)
      {
        continue;
      }

      const bool had = !_bids[i].isZero();
      if (_bids[i].raw() == q.raw())
      {
        continue;
      }

      _bids[i] = q;

      if (!q.isZero())
      {
        if (i < _minBid)
        {
          _minBid = i;
        }
        if (i > _maxBid)
        {
          _maxBid = i;
        }
        if (_bestBidIdx >= MAX_LEVELS || i > _bestBidIdx)
        {
          _bestBidIdx = i;
        }
      }
      else if (had)
      {
        if (i == _bestBidIdx)
        {
          _bestBidIdx = prevNonZeroBid(i);
        }
        if (i == _minBid)
        {
          _minBid = nextNonZeroBid(_minBid);
        }
        if (i == _maxBid)
        {
          _maxBid = normalizeUpperBound(prevNonZeroBid(_maxBid));
        }
      }
    }

    for (const auto& [p, q] : up.asks)
    {
      const size_t i = localIndex(p, Side::SELL);
      if (i >= MAX_LEVELS)
      {
        continue;
      }

      const bool had = !_asks[i].isZero();
      if (_asks[i].raw() == q.raw())
      {
        continue;
      }

      _asks[i] = q;

      if (!q.isZero())
      {
        if (i < _minAsk)
        {
          _minAsk = i;
        }
        if (i > _maxAsk)
        {
          _maxAsk = i;
        }
        if (_bestAskIdx >= MAX_LEVELS || i < _bestAskIdx)
        {
          _bestAskIdx = i;
        }
      }
      else if (had)
      {
        if (i == _bestAskIdx)
        {
          _bestAskIdx = nextNonZeroAsk(i);
        }
        if (i == _minAsk)
        {
          _minAsk = nextNonZeroAsk(_minAsk);
        }
        if (i == _maxAsk)
        {
          _maxAsk = normalizeUpperBound(prevNonZeroAsk(_maxAsk));
        }
      }
    }
  }

  inline std::optional<Price> bestBid() const override
  {
    const auto t = bestBidTick();
    if (!t)
    {
      return std::nullopt;
    }
    return std::optional<Price>{Price::fromRaw(_tickSize.raw() * *t)};
  }

  inline std::optional<Price> bestAsk() const override
  {
    const auto t = bestAskTick();
    if (!t)
    {
      return std::nullopt;
    }
    return std::optional<Price>{Price::fromRaw(_tickSize.raw() * *t)};
  }

  inline Quantity bidAtPrice(Price p) const override
  {
    const size_t i = localIndex(p, Side::BUY);
    return i < MAX_LEVELS ? _bids[i] : Quantity{};
  }

  inline Quantity askAtPrice(Price p) const override
  {
    const size_t i = localIndex(p, Side::SELL);
    return i < MAX_LEVELS ? _asks[i] : Quantity{};
  }

  inline std::pair<Quantity, Volume> consumeAsks(Quantity needQty) const
  {
    if (_bestAskIdx >= MAX_LEVELS)
    {
      return {Quantity{}, Volume{}};
    }

#if LRVX_HAS_NATIVE_INT128
    // Fast path: 128-bit accumulator with single division at end (GCC/Clang)
    int64_t remRaw = needQty.raw();
    __int128_t notionalRaw2 = 0;

    size_t i = _bestAskIdx;
    const size_t hi = normalizeUpperBound(_maxAsk);
    const int64_t ts = _tickSize.raw();
    int64_t tickNum = _baseIndex + static_cast<int64_t>(i);

    for (; i <= hi && remRaw > 0; ++i, ++tickNum)
    {
      const int64_t qRaw = _asks[i].raw();
      if (qRaw == 0)
      {
        continue;
      }

      const int64_t takeRaw = (qRaw < remRaw) ? qRaw : remRaw;
      notionalRaw2 += static_cast<__int128_t>(takeRaw) * static_cast<__int128_t>(ts * tickNum);
      remRaw -= takeRaw;
    }

    // The quotient is narrowed through the same checked cast the rest of the
    // fixed-point arithmetic uses. A deep consume accumulates past the int64
    // range, and the bare static_cast that stood here wrapped it into a
    // negative Volume -- a notional handed to risk with the wrong sign.
    return {Quantity::fromRaw(needQty.raw() - remRaw),
            Volume::fromRaw(checkedNarrowI64(notionalRaw2 / Volume::Scale))};
#else
    // Fallback: use type-safe operators (slower but portable)
    Quantity remaining = needQty;
    Volume notional{};

    size_t i = _bestAskIdx;
    const size_t hi = normalizeUpperBound(_maxAsk);
    int64_t tickNum = _baseIndex + static_cast<int64_t>(i);

    for (; i <= hi && remaining.raw() > 0; ++i, ++tickNum)
    {
      const Quantity available = _asks[i];
      if (available.isZero())
      {
        continue;
      }

      const Quantity take = available.raw() < remaining.raw() ? available : remaining;
      const Price price = Price::fromRaw(_tickSize.raw() * tickNum);
      notional = notional + (take * price);
      remaining = remaining - take;
    }

    return {needQty - remaining, notional};
#endif
  }

  inline std::pair<Quantity, Volume> consumeBids(Quantity needQty) const
  {
    if (_bestBidIdx >= MAX_LEVELS)
    {
      return {Quantity{}, Volume{}};
    }

#if LRVX_HAS_NATIVE_INT128
    // Fast path: 128-bit accumulator with single division at end (GCC/Clang)
    int64_t remRaw = needQty.raw();
    __int128_t notionalRaw2 = 0;

    size_t i = _bestBidIdx;
    const size_t lo = normalizeLowerBound(_minBid);
    const int64_t ts = _tickSize.raw();
    int64_t tickNum = _baseIndex + static_cast<int64_t>(i);

    for (;;)
    {
      if (remRaw <= 0)
      {
        break;
      }

      const int64_t qRaw = _bids[i].raw();
      if (qRaw != 0)
      {
        const int64_t takeRaw = (qRaw < remRaw) ? qRaw : remRaw;
        notionalRaw2 += static_cast<__int128_t>(takeRaw) * static_cast<__int128_t>(ts * tickNum);
        remRaw -= takeRaw;
      }

      if (i == lo)
      {
        break;
      }
      --i;
      --tickNum;
    }

    // The quotient is narrowed through the same checked cast the rest of the
    // fixed-point arithmetic uses. A deep consume accumulates past the int64
    // range, and the bare static_cast that stood here wrapped it into a
    // negative Volume -- a notional handed to risk with the wrong sign.
    return {Quantity::fromRaw(needQty.raw() - remRaw),
            Volume::fromRaw(checkedNarrowI64(notionalRaw2 / Volume::Scale))};
#else
    // Fallback: use type-safe operators (slower but portable)
    Quantity remaining = needQty;
    Volume notional{};

    size_t i = _bestBidIdx;
    const size_t lo = normalizeLowerBound(_minBid);
    int64_t tickNum = _baseIndex + static_cast<int64_t>(i);

    for (;;)
    {
      if (remaining.raw() <= 0)
      {
        break;
      }

      const Quantity available = _bids[i];
      if (!available.isZero())
      {
        const Quantity take = available.raw() < remaining.raw() ? available : remaining;
        const Price price = Price::fromRaw(_tickSize.raw() * tickNum);
        notional = notional + (take * price);
        remaining = remaining - take;
      }

      if (i == lo)
      {
        break;
      }
      --i;
      --tickNum;
    }

    return {needQty - remaining, notional};
#endif
  }

  Price tickSize() const { return _tickSize; }

  inline bool isCrossed() const
  {
    const auto bid = bestBidTick();
    const auto ask = bestAskTick();
    if (!bid || !ask)
    {
      return false;
    }
    return *bid >= *ask;
  }

  inline std::optional<Price> spread() const
  {
    const auto bid = bestBidTick();
    const auto ask = bestAskTick();
    if (!bid || !ask)
    {
      return std::nullopt;
    }
    return Price::fromRaw(_tickSize.raw() * (*ask - *bid));
  }

  inline std::optional<Price> mid() const
  {
    const auto bidTick = bestBidTick();
    const auto askTick = bestAskTick();
    if (!bidTick || !askTick)
    {
      return std::nullopt;
    }
    // midPrice = tickSize * (bidTick + askTick) / 2. Halving the tick first
    // threw away half a tick on every odd raw tick size and then multiplied the
    // loss by the tick index: 20% low at a raw tick of 5, 33% at 3, zero at 1.
    // Halve the tick *sum* instead, so the division runs once and only an odd
    // sum can cost anything, and at most half a raw unit.
    const int64_t tick = _tickSize.raw();
    const int64_t tickSum = *bidTick + *askTick;
    const int64_t wholeTicks = tickSum / 2;
    const int64_t oddTick = tickSum - wholeTicks * 2;
    return Price::fromRaw(tick * wholeTicks + (tick * oddTick) / 2);
  }

  struct PriceLevel
  {
    Price price;
    Quantity quantity;
  };

  std::vector<PriceLevel> getBidLevels(size_t maxLevels) const
  {
    std::vector<PriceLevel> levels;
    levels.reserve(maxLevels);

    if (auto bidIdx = bestBidIndex())
    {
      for (size_t i = *bidIdx + 1; levels.size() < maxLevels;)
      {
        if (i-- == 0)
        {
          break;
        }
        if (_bids[i].isZero())
        {
          continue;
        }
        levels.push_back({indexToPrice(i), _bids[i]});
      }
    }
    return levels;
  }

  std::vector<PriceLevel> getAskLevels(size_t maxLevels) const
  {
    std::vector<PriceLevel> levels;
    levels.reserve(maxLevels);

    if (auto askIdx = bestAskIndex())
    {
      for (size_t i = *askIdx; levels.size() < maxLevels && i < MAX_LEVELS; i++)
      {
        if (_asks[i].isZero())
        {
          continue;
        }
        levels.push_back({indexToPrice(i), _asks[i]});
      }
    }
    return levels;
  }

  void clear()
  {
    _bids.fill({});
    _asks.fill({});
    _minBid = _minAsk = MAX_LEVELS;
    _maxBid = _maxAsk = 0;
    _baseIndex = 0;
    _bestBidIdx = _bestAskIdx = MAX_LEVELS;
  }

 private:
  // prevNonZeroBid / prevNonZeroAsk report "nothing found" as MAX_LEVELS, which
  // is the empty marker for the *lower* bounds but a one-past-the-end index for
  // the upper ones. Storing it unchanged in _maxAsk let the next consume walk to
  // _asks[MAX_LEVELS]; clear() uses 0 for an empty upper bound, so match that.
  static constexpr size_t normalizeUpperBound(size_t idx)
  {
    return idx < MAX_LEVELS ? idx : 0;
  }

  static constexpr size_t normalizeLowerBound(size_t idx)
  {
    return idx < MAX_LEVELS ? idx : 0;
  }

  // Which way an off-tick quote is snapped. Rounding to the nearest tick
  // stored an ask up to half a tick BELOW what the venue quoted (and a bid the
  // same distance above it), so bestAsk() handed the strategy a price nobody
  // was offering and it sized against liquidity that is not there. A quote is
  // snapped away from the mid instead: the stored tick is never better than
  // the quote, only equal to it or one tick worse. A price already on a tick
  // is unchanged either way.
  int64_t ticks(Price p, Side side) const
  {
    const int64_t pr = p.raw();
    return (side == Side::SELL) ? math::sdiv_ceil(pr, _tickSizeDiv)
                                : math::sdiv_floor(pr, _tickSizeDiv);
  }

  Price indexToPrice(size_t i) const
  {
    const int64_t ts = _tickSize.raw();
    const int64_t tick = _baseIndex + static_cast<int64_t>(i);
    return Price::fromRaw(ts * tick);
  }

  // The tick of the best quote on each side, and the single place where "this
  // side is empty" is decided. Emptiness is a property of the ladder index --
  // _bestBidIdx / _bestAskIdx outside [0, MAX_LEVELS) -- never of the tick:
  // a tick below zero is an ordinary price. WTI settled at -37.63 in April
  // 2020, day-ahead power clears below zero on a windy afternoon, and a
  // calendar spread is negative whenever the market is in contango. Reading a
  // negative tick as an empty side reported no quote at all on those books,
  // while getBidLevels and bidAtPrice -- which walk the index -- reported the
  // levels that were there. Deriving the tick from the index keeps every
  // accessor on one answer.
  std::optional<int64_t> bestBidTick() const
  {
    const auto i = bestBidIndex();
    if (!i)
    {
      return std::nullopt;
    }
    return _baseIndex + static_cast<int64_t>(*i);
  }

  std::optional<int64_t> bestAskTick() const
  {
    const auto i = bestAskIndex();
    if (!i)
    {
      return std::nullopt;
    }
    return _baseIndex + static_cast<int64_t>(*i);
  }

  // Side matters here for the same reason it matters in ticks(): a price is
  // looked up at the tick it would have been stored at, so the query has to
  // snap the way the write did.
  size_t localIndex(Price p, Side side) const
  {
    const int64_t t = ticks(p, side) - _baseIndex;
    return (static_cast<uint64_t>(t) < static_cast<uint64_t>(MAX_LEVELS))
               ? static_cast<size_t>(t)
               : MAX_LEVELS;
  }

  void reanchor(int64_t minIdx, int64_t maxIdx)
  {
    constexpr int64_t HYST = 8;
    const int64_t span = maxIdx - minIdx + 1;
    const int64_t curLo = _baseIndex;
    const int64_t curHi = _baseIndex + static_cast<int64_t>(MAX_LEVELS) - 1;

    if (curLo + HYST <= minIdx && maxIdx <= curHi - HYST)
    {
      return;
    }

    if (span >= static_cast<int64_t>(MAX_LEVELS))
    {
      _baseIndex = minIdx;
    }
    else
    {
      const int64_t mid = (minIdx + maxIdx) / 2;
      _baseIndex = mid - static_cast<int64_t>(MAX_LEVELS / 2);
    }
  }

  // A delta feed re-sends a snapshot only on connect and on a sequence gap, so
  // between the two the tick window has to follow the market itself. Levels
  // landing outside it used to be dropped with no signal at all: about $25 of
  // BTC movement emptied one side of a 512-level book and $30 emptied both,
  // with no recovery short of a reconnect. The usable range is half the level
  // count, not all of it, because re-anchoring centres the window on the market.
  //
  // The window moves only when everything already in the book still fits
  // alongside the new level. That keeps one far-off quote -- a stale price, a
  // fat finger, a venue placeholder -- from evicting live depth, which is the
  // one thing the old silent drop did get right.
  void reanchorForDelta(const BookUpdate& up)
  {
    int64_t lo = std::numeric_limits<int64_t>::max();
    int64_t hi = std::numeric_limits<int64_t>::min();

    auto scan = [&](const auto& vec, Side side)
    {
      for (const auto& [p, q] : vec)
      {
        if (q.isZero())
        {
          // A removal names a price the book cannot be holding out here.
          continue;
        }
        const int64_t t = ticks(p, side);
        const int64_t local = t - _baseIndex;
        if (local >= 0 && local < static_cast<int64_t>(MAX_LEVELS))
        {
          continue;
        }
        lo = std::min(lo, t);
        hi = std::max(hi, t);
      }
    };

    scan(up.bids, Side::BUY);
    scan(up.asks, Side::SELL);

    if (lo > hi)
    {
      return;
    }

    if (_minBid < MAX_LEVELS)
    {
      lo = std::min(lo, _baseIndex + static_cast<int64_t>(_minBid));
      hi = std::max(hi, _baseIndex + static_cast<int64_t>(_maxBid));
    }
    if (_minAsk < MAX_LEVELS)
    {
      lo = std::min(lo, _baseIndex + static_cast<int64_t>(_minAsk));
      hi = std::max(hi, _baseIndex + static_cast<int64_t>(_maxAsk));
    }

    if (hi - lo + 1 > static_cast<int64_t>(MAX_LEVELS))
    {
      return;
    }

    reanchorWithData(lo, hi);
  }

  void reanchorWithData(int64_t minIdx, int64_t maxIdx)
  {
    const int64_t oldBase = _baseIndex;

    // Calculate new base index
    const int64_t span = maxIdx - minIdx + 1;
    int64_t newBase;
    if (span >= static_cast<int64_t>(MAX_LEVELS))
    {
      newBase = minIdx;
    }
    else
    {
      const int64_t mid = (minIdx + maxIdx) / 2;
      newBase = mid - static_cast<int64_t>(MAX_LEVELS / 2);
    }

    if (newBase == oldBase)
    {
      return;
    }

    // Shift both ladders in place. A pair of MAX_LEVELS scratch arrays would be
    // a quarter of a megabyte of stack at the default level count, which is more
    // than a worker thread has.
    const int64_t shift = oldBase - newBase;
    shiftLadder(_bids, shift);
    shiftLadder(_asks, shift);
    _baseIndex = newBase;

    // Recalculate min/max/best indices
    _minBid = _minAsk = MAX_LEVELS;
    _maxBid = _maxAsk = 0;
    _bestBidIdx = _bestAskIdx = MAX_LEVELS;

    for (size_t i = 0; i < MAX_LEVELS; ++i)
    {
      if (!_bids[i].isZero())
      {
        if (i < _minBid)
        {
          _minBid = i;
        }
        if (i > _maxBid)
        {
          _maxBid = i;
        }
        if (_bestBidIdx >= MAX_LEVELS || i > _bestBidIdx)
        {
          _bestBidIdx = i;
        }
      }
      if (!_asks[i].isZero())
      {
        if (i < _minAsk)
        {
          _minAsk = i;
        }
        if (i > _maxAsk)
        {
          _maxAsk = i;
        }
        if (_bestAskIdx >= MAX_LEVELS || i < _bestAskIdx)
        {
          _bestAskIdx = i;
        }
      }
    }
  }

  // Move every level by `shift` slots, dropping whatever falls off either end
  // and zeroing the slots that open up.
  static void shiftLadder(std::array<Quantity, MAX_LEVELS>& ladder, int64_t shift)
  {
    if (shift == 0)
    {
      return;
    }

    const size_t magnitude = static_cast<size_t>(shift < 0 ? -shift : shift);
    if (magnitude >= MAX_LEVELS)
    {
      ladder.fill({});
      return;
    }

    if (shift > 0)
    {
      std::copy_backward(ladder.begin(), ladder.end() - magnitude, ladder.end());
      std::fill(ladder.begin(), ladder.begin() + magnitude, Quantity{});
    }
    else
    {
      std::copy(ladder.begin() + magnitude, ladder.end(), ladder.begin());
      std::fill(ladder.end() - magnitude, ladder.end(), Quantity{});
    }
  }

  size_t nextNonZeroAsk(size_t from) const
  {
    // Limit search to known ask range
    const size_t limit = (_maxAsk < MAX_LEVELS) ? _maxAsk + 1 : MAX_LEVELS;
    for (size_t i = from; i < limit; ++i)
    {
      if (!_asks[i].isZero())
      {
        return i;
      }
    }
    return MAX_LEVELS;
  }

  size_t prevNonZeroAsk(size_t from) const
  {
    // Limit search to known ask range
    const size_t limit = (_minAsk < MAX_LEVELS) ? _minAsk : 0;
    if (from < limit)
    {
      return MAX_LEVELS;
    }
    for (size_t i = from + 1; i-- > limit;)
    {
      if (!_asks[i].isZero())
      {
        return i;
      }
    }
    return MAX_LEVELS;
  }

  size_t nextNonZeroBid(size_t from) const
  {
    // Limit search to known bid range
    const size_t limit = (_maxBid < MAX_LEVELS) ? _maxBid + 1 : MAX_LEVELS;
    for (size_t i = from; i < limit; ++i)
    {
      if (!_bids[i].isZero())
      {
        return i;
      }
    }
    return MAX_LEVELS;
  }

  size_t prevNonZeroBid(size_t from) const
  {
    // Limit search to known bid range
    const size_t limit = (_minBid < MAX_LEVELS) ? _minBid : 0;
    if (from < limit)
    {
      return MAX_LEVELS;
    }
    for (size_t i = from + 1; i-- > limit;)
    {
      if (!_bids[i].isZero())
      {
        return i;
      }
    }
    return MAX_LEVELS;
  }

 private:
  Price _tickSize;
  math::FastDiv64 _tickSizeDiv;

  int64_t _baseIndex{0};

  alignas(64) std::array<Quantity, MAX_LEVELS> _bids{};
  alignas(64) std::array<Quantity, MAX_LEVELS> _asks{};

  size_t _minBid{MAX_LEVELS}, _maxBid{0}, _minAsk{MAX_LEVELS}, _maxAsk{0};

  size_t _bestBidIdx{MAX_LEVELS}, _bestAskIdx{MAX_LEVELS};
};

}  // namespace lrvx
