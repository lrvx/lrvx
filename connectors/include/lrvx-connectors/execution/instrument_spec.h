/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2026 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <lrvx/common.h>
#include <lrvx/engine/symbol_registry.h>
#include <lrvx/log/abstract_logger.h>

#include <expected>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lrvx
{

// What a venue enforces on the prices and sizes of one instrument's orders,
// read from the venue's public instrument-info endpoint.
struct InstrumentSpec
{
  // The price increment. On Hyperliquid, which has no fixed tick, the finest
  // increment the venue accepts at any price (see hyperliquidPerpTick).
  Price tickSize{};
  // The size increment; zero when the venue's response does not carry one
  // (Polymarket's tick-size answer has no size field). It reaches the
  // registry entry as SymbolInfo::lotSize the way the tick does.
  Quantity lotSize{};
};

// Keyed by the venue's own symbol name ("BTCUSDT", "BTC", a token id).
using InstrumentSpecs = std::unordered_map<std::string, InstrumentSpec>;

// One request to a public endpoint: a GET when body is empty, a POST of the
// JSON body otherwise. Returns the response body, or why there is none. A
// connector takes one so a test can answer with a recorded response.
using InstrumentFetch = std::function<std::expected<std::string, std::string>(
    const std::string& url, const std::string& body)>;

// Blocking libcurl request with a total timeout. An error is a request that
// got no answer (connection, TLS, timeout) or an empty non-2xx answer; a
// non-2xx answer with a body returns the body, so the venue's own refusal
// text reaches the parser. A connector stops requesting after the first
// error, so an unreachable venue costs start() one timeout, not one per
// symbol.
InstrumentFetch defaultInstrumentFetch(int timeoutMs = 10000);

namespace instrument_info
{

// Bybit v5 GET /v5/market/instruments-info: result.list[].priceFilter.tickSize
// and lotSizeFilter.qtyStep (lotSizeFilter.basePrecision for spot, which has
// no qtyStep). A retCode other than 0 is an error carrying retMsg.
std::expected<InstrumentSpecs, std::string> parseBybit(std::string_view body);

// Bitget v2 GET /api/v2/mix/market/contracts: tick = priceEndStep x
// 10^-pricePlace, lot = sizeMultiplier. A code other than "00000" is an error
// carrying msg.
std::expected<InstrumentSpecs, std::string> parseBitgetContracts(std::string_view body);

// Bitget v2 GET /api/v2/spot/public/symbols: tick = 10^-pricePrecision, lot =
// 10^-quantityPrecision.
std::expected<InstrumentSpecs, std::string> parseBitgetSpot(std::string_view body);

// Hyperliquid POST /info {"type":"meta"}: universe[].szDecimals. Lot is
// 10^-szDecimals; tick is hyperliquidPerpTick(szDecimals).
std::expected<InstrumentSpecs, std::string> parseHyperliquidMeta(std::string_view body);

// Polymarket CLOB GET /tick-size?token_id=...: minimum_tick_size, a JSON
// number read from its text so no binary rounding enters. Lot is left 0: the
// CLOB states no share increment there.
std::expected<InstrumentSpec, std::string> parsePolymarketTickSize(std::string_view body);

// The finest price increment Hyperliquid accepts on a perp with this
// szDecimals: a price has at most 6 - szDecimals decimals (and at most five
// significant figures, which no single tick can express; the executor checks
// that half of the rule itself).
Price hyperliquidPerpTick(int szDecimals);

}  // namespace instrument_info

// The specs one connector loaded, shared between start() and the message
// thread that registers symbols on first sight, and what is wrong with them
// now. The report is kept per symbol: a symbol whose tick could not be read
// has one entry, which goes when the venue supplies the tick later; a symbol
// whose declared tick disagrees with the venue's has one entry, which is
// rewritten when the venue's tick changes and goes when the two agree.
class InstrumentSpecTable
{
 public:
  void set(InstrumentSpecs specs, std::string error)
  {
    std::lock_guard lock(_mutex);
    _specs = std::move(specs);
    _issues.clear();
    if (!error.empty())
    {
      _issues.push_back({Issue::Kind::General, {}, std::move(error)});
    }
  }

  // The venue's spec for a symbol. Whatever said its tick could not be read
  // no longer holds.
  void add(const std::string& symbol, InstrumentSpec spec)
  {
    std::lock_guard lock(_mutex);
    _specs[symbol] = spec;
    eraseIssue(Issue::Kind::NotLoaded, symbol);
  }

  // A failure that belongs to no one symbol (the whole response was
  // unusable). Stays until the next load.
  void addError(const std::string& error)
  {
    std::lock_guard lock(_mutex);
    _issues.push_back({Issue::Kind::General, {}, error});
  }

  // Why this symbol's spec could not be read. One entry per symbol: a later
  // reason replaces the earlier one.
  void addError(const std::string& symbol, const std::string& reason)
  {
    std::lock_guard lock(_mutex);
    putIssue(Issue::Kind::NotLoaded, symbol, reason);
  }

  // The registry declares a tick or a lot for this symbol that is not the
  // venue's.
  void setConflict(const std::string& symbol, const std::string& text)
  {
    std::lock_guard lock(_mutex);
    putIssue(Issue::Kind::Conflict, symbol, text);
  }

  void clearConflict(const std::string& symbol)
  {
    std::lock_guard lock(_mutex);
    eraseIssue(Issue::Kind::Conflict, symbol);
  }

  void clear()
  {
    std::lock_guard lock(_mutex);
    _specs.clear();
    _issues.clear();
  }

  std::optional<InstrumentSpec> find(std::string_view symbol) const
  {
    std::lock_guard lock(_mutex);
    auto it = _specs.find(std::string(symbol));
    if (it == _specs.end())
    {
      return std::nullopt;
    }
    return it->second;
  }

  // Empty when every requested instrument is loaded and reached the
  // registry. Otherwise what is wrong now, "; "-separated, a symbol's entry
  // as "<symbol>: <text>", in the order the entries first appeared.
  std::string error() const
  {
    std::lock_guard lock(_mutex);
    std::string out;
    for (const auto& issue : _issues)
    {
      if (!out.empty())
      {
        out += "; ";
      }
      if (issue.kind != Issue::Kind::General)
      {
        out += issue.symbol + ": ";
      }
      out += issue.text;
    }
    return out;
  }

 private:
  struct Issue
  {
    enum class Kind : uint8_t
    {
      General,
      NotLoaded,
      Conflict
    };
    Kind kind;
    std::string symbol;
    std::string text;
  };

  // Callers hold _mutex.
  void putIssue(Issue::Kind kind, const std::string& symbol, const std::string& text)
  {
    for (auto& issue : _issues)
    {
      if (issue.kind == kind && issue.symbol == symbol)
      {
        issue.text = text;
        return;
      }
    }
    _issues.push_back({kind, symbol, text});
  }

  void eraseIssue(Issue::Kind kind, const std::string& symbol)
  {
    std::erase_if(_issues,
                  [&](const Issue& issue)
                  {
                    return issue.kind == kind && issue.symbol == symbol;
                  });
  }

  mutable std::mutex _mutex;
  InstrumentSpecs _specs;
  std::vector<Issue> _issues;
};

// Registers a venue symbol, with the venue's tick and lot when the table has
// them. What follows about the tick holds for the lot, which goes in through
// SymbolRegistry::applyVenueLot: an entry that declares no lot takes the
// venue's, one that declares a different lot keeps it and the disagreement is
// reported the same way.
// The tick goes into the entry through SymbolRegistry::applyVenueTick, for a
// symbol registered here and for one already in the registry, which keeps
// its id and entry: an entry whose tick was not declared takes the venue's,
// marked known, and takes the venue's next one when the table's tick has
// changed since (a connector calls again after the venue moved the tick). An entry that
// declares a different tick keeps it -- the caller may know better (a
// product the venue lists differently, a test of the refusal path) and a
// declaration is not silently replaced -- and the disagreement is logged as
// an error and kept in the table's error(), naming both ticks, for as long
// as the two differ.
SymbolId registerVenueSymbol(SymbolRegistry& registry, SymbolInfo info, InstrumentSpecTable& specs,
                             ILogger* logger);

// Run once at start(), after the load: logs, as an error, every instrument
// the venue's metadata did not cover. Such a symbol is registered with its
// tick unknown: no order price on it is checked or rounded by the executor,
// and the venue's own refusal reaches the strategy as an event.
void reportInstrumentLoad(const InstrumentSpecTable& specs, std::string_view venue,
                          ILogger* logger);

}  // namespace lrvx
