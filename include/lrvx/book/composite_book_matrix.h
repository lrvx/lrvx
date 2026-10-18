/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for
 * full license information.
 */

#pragma once

#include "lrvx/book/book_update.h"
#include "lrvx/book/events/book_update_event.h"
#include "lrvx/common.h"
#include "lrvx/engine/abstract_market_data_subscriber.h"
#include "lrvx/strategy/symbol_state_map.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <limits>

namespace lrvx
{

template <size_t MaxExchanges = 4>
class CompositeBookMatrix : public IMarketDataSubscriber
{
 public:
  struct BestQuote
  {
    int64_t priceRaw{0};
    int64_t qtyRaw{0};
    ExchangeId exchange{InvalidExchangeId};
    bool valid{false};
  };

  SubscriberId id() const override { return _subscriberId; }
  void setId(SubscriberId id) { _subscriberId = id; }

  void onBookUpdate(const BookUpdateEvent& ev) override
  {
    ExchangeId ex = ev.sourceExchange;
    if (ex >= MaxExchanges) [[unlikely]]
    {
      return;
    }

    auto& state = _books[ev.update.symbol];
    auto& exState = state.byExchange[ex];

    // Find the best bid/ask level actually carried by this update, if any.
    bool hasBidLevel = false;
    int64_t bestBidPrice = 0;
    int64_t bestBidQty = 0;
    for (const auto& [price, qty] : ev.update.bids)
    {
      if (qty.isZero())
      {
        continue;
      }
      hasBidLevel = true;
      if (price.raw() > bestBidPrice)
      {
        bestBidPrice = price.raw();
        bestBidQty = qty.raw();
      }
    }

    bool hasAskLevel = false;
    int64_t bestAskPrice = std::numeric_limits<int64_t>::max();
    int64_t bestAskQty = 0;
    for (const auto& [price, qty] : ev.update.asks)
    {
      if (qty.isZero())
      {
        continue;
      }
      hasAskLevel = true;
      if (price.raw() < bestAskPrice)
      {
        bestAskPrice = price.raw();
        bestAskQty = qty.raw();
      }
    }
    if (!hasAskLevel)
    {
      bestAskPrice = 0;  // No valid ask
    }

    // A SNAPSHOT replaces both sides wholesale, so a side that is empty in
    // the update really is empty (e.g. no bids left) and must be published
    // as such. TOP_OF_BOOK makes the same promise about the touch: it is
    // the venue's complete word on the best bid/ask, so an empty side means
    // that side of the book is empty. A DELTA only carries the side(s) that
    // changed (a typical Bybit delta touches only bids or only asks); a
    // side absent from the delta must be left exactly as it was, not zeroed
    // out. Zeroing the untouched side used to make one exchange's ask (or
    // bid) vanish from cross-venue comparison on every bid-only (or
    // ask-only) delta.
    const bool replacesSides = ev.update.type != BookUpdateType::DELTA;
    if (replacesSides || hasBidLevel)
    {
      exState.bidPrice.store(bestBidPrice, std::memory_order_release);
      exState.bidQty.store(bestBidQty, std::memory_order_release);
    }
    if (replacesSides || hasAskLevel)
    {
      exState.askPrice.store(bestAskPrice, std::memory_order_release);
      exState.askQty.store(bestAskQty, std::memory_order_release);
    }
    exState.lastUpdateNs.store(static_cast<int64_t>(ev.recvNs.raw()), std::memory_order_release);
    exState.stale.store(false, std::memory_order_release);
  }

  BestQuote bestBid(SymbolId symbol) const
  {
    const auto* state = _books.tryGet(symbol);
    if (!state)
    {
      return BestQuote{};
    }

    BestQuote best{};
    for (size_t ex = 0; ex < MaxExchanges; ++ex)
    {
      const auto& exState = state->byExchange[ex];
      if (exState.stale.load(std::memory_order_acquire))
      {
        continue;
      }

      int64_t price = exState.bidPrice.load(std::memory_order_acquire);
      if (price > best.priceRaw)
      {
        best.priceRaw = price;
        best.qtyRaw = exState.bidQty.load(std::memory_order_acquire);
        best.exchange = static_cast<ExchangeId>(ex);
        best.valid = true;
      }
    }
    return best;
  }

  BestQuote bestAsk(SymbolId symbol) const
  {
    const auto* state = _books.tryGet(symbol);
    if (!state)
    {
      return BestQuote{};
    }

    BestQuote best{};
    best.priceRaw = std::numeric_limits<int64_t>::max();

    for (size_t ex = 0; ex < MaxExchanges; ++ex)
    {
      const auto& exState = state->byExchange[ex];
      if (exState.stale.load(std::memory_order_acquire))
      {
        continue;
      }

      int64_t price = exState.askPrice.load(std::memory_order_acquire);
      if (price > 0 && price < best.priceRaw)
      {
        best.priceRaw = price;
        best.qtyRaw = exState.askQty.load(std::memory_order_acquire);
        best.exchange = static_cast<ExchangeId>(ex);
        best.valid = true;
      }
    }

    if (best.priceRaw == std::numeric_limits<int64_t>::max())
    {
      best.valid = false;
      best.priceRaw = 0;
    }
    return best;
  }

  bool hasArbitrageOpportunity(SymbolId symbol) const
  {
    auto bid = bestBid(symbol);
    auto ask = bestAsk(symbol);
    return bid.valid && ask.valid && bid.exchange != ask.exchange && bid.priceRaw > ask.priceRaw;
  }

  int64_t spreadRaw(SymbolId symbol) const
  {
    auto bid = bestBid(symbol);
    auto ask = bestAsk(symbol);
    if (!bid.valid || !ask.valid)
    {
      return 0;
    }
    return ask.priceRaw - bid.priceRaw;
  }

  void markStale(ExchangeId exchange, SymbolId symbol)
  {
    if (auto* state = _books.tryGet(symbol))
    {
      if (exchange < MaxExchanges)
      {
        state->byExchange[exchange].stale.store(true, std::memory_order_release);
      }
    }
  }

  void markExchangeStale(ExchangeId exchange)
  {
    if (exchange >= MaxExchanges)
    {
      return;
    }

    for (auto&& [sym, state] : _books)
    {
      state.byExchange[exchange].stale.store(true, std::memory_order_release);
    }
  }

  void checkStaleness(int64_t nowNs, int64_t thresholdNs)
  {
    for (auto&& [sym, state] : _books)
    {
      for (size_t ex = 0; ex < MaxExchanges; ++ex)
      {
        int64_t lastUpdate = state.byExchange[ex].lastUpdateNs.load(std::memory_order_acquire);
        if (lastUpdate > 0 && nowNs - lastUpdate > thresholdNs)
        {
          state.byExchange[ex].stale.store(true, std::memory_order_release);
        }
      }
    }
  }

  BestQuote bidForExchange(SymbolId symbol, ExchangeId exchange) const
  {
    if (exchange >= MaxExchanges)
    {
      return {};
    }

    const auto* state = _books.tryGet(symbol);
    if (!state)
    {
      return {};
    }

    const auto& exState = state->byExchange[exchange];
    if (exState.stale.load(std::memory_order_acquire))
    {
      return {};
    }

    int64_t price = exState.bidPrice.load(std::memory_order_acquire);
    if (price == 0)
    {
      // Symmetric with askForExchange: a zero price means "no bid known",
      // not "a bid at price zero". Previously only the ask side checked
      // this, so a bid-side price of zero (e.g. after a delete-only delta
      // left no bid level) was reported as a valid $0.00 quote.
      return {};
    }

    BestQuote result{};
    result.priceRaw = price;
    result.qtyRaw = exState.bidQty.load(std::memory_order_acquire);
    result.exchange = exchange;
    result.valid = true;
    return result;
  }

  BestQuote askForExchange(SymbolId symbol, ExchangeId exchange) const
  {
    if (exchange >= MaxExchanges)
    {
      return {};
    }

    const auto* state = _books.tryGet(symbol);
    if (!state)
    {
      return {};
    }

    const auto& exState = state->byExchange[exchange];
    if (exState.stale.load(std::memory_order_acquire))
    {
      return {};
    }

    int64_t price = exState.askPrice.load(std::memory_order_acquire);
    if (price == 0)
    {
      return {};
    }

    BestQuote result{};
    result.priceRaw = price;
    result.qtyRaw = exState.askQty.load(std::memory_order_acquire);
    result.exchange = exchange;
    result.valid = true;
    return result;
  }

 private:
  struct alignas(64) ExchangeBookState
  {
    std::atomic<int64_t> bidPrice{0};
    std::atomic<int64_t> bidQty{0};
    std::atomic<int64_t> askPrice{0};
    std::atomic<int64_t> askQty{0};
    std::atomic<int64_t> lastUpdateNs{0};
    std::atomic<bool> stale{true};
  };

  struct SymbolBooks
  {
    std::array<ExchangeBookState, MaxExchanges> byExchange{};
  };

  SymbolStateMap<SymbolBooks> _books;
  SubscriberId _subscriberId{0};
};

}  // namespace lrvx
