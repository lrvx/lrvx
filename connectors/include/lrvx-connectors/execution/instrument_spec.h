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

namespace lrvx
{

// What a venue enforces on the prices and sizes of one instrument's orders,
// read from the venue's public instrument-info endpoint.
struct InstrumentSpec
{
  // The price increment. On Hyperliquid, which has no fixed tick, the finest
  // increment the venue accepts at any price (see hyperliquidPerpTick).
  Price tickSize{};
  // The size increment. SymbolInfo has no field for it, so it stays here; the
  // connector exposes it through instrumentSpec().
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

// Write the venue's tick into a registry entry and mark it known, so the
// executor and the simulator both check order prices against it.
inline void applyInstrumentSpec(SymbolInfo& info, const InstrumentSpec& spec)
{
  if (spec.tickSize.raw() > 0)
  {
    info.tickSize = spec.tickSize;
    info.tickSizeKnown = true;
  }
}

// The specs one connector loaded, shared between start() and the message
// thread that registers symbols on first sight.
class InstrumentSpecTable
{
 public:
  void set(InstrumentSpecs specs, std::string error)
  {
    std::lock_guard lock(_mutex);
    _specs = std::move(specs);
    _error = std::move(error);
  }

  void add(const std::string& symbol, InstrumentSpec spec)
  {
    std::lock_guard lock(_mutex);
    _specs[symbol] = spec;
  }

  void addError(const std::string& error)
  {
    std::lock_guard lock(_mutex);
    if (!_error.empty())
    {
      _error += "; ";
    }
    _error += error;
  }

  void clear()
  {
    std::lock_guard lock(_mutex);
    _specs.clear();
    _error.clear();
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

  // Empty when every requested instrument loaded.
  std::string error() const
  {
    std::lock_guard lock(_mutex);
    return _error;
  }

 private:
  mutable std::mutex _mutex;
  InstrumentSpecs _specs;
  std::string _error;
};

// Registers a venue symbol, with the venue's tick when the table has one.
// A symbol already in the registry is returned as it is: the registry keeps
// the entry it was first given. When that entry's tick is not the venue's,
// the mismatch is logged as an error, because orders on it are then checked
// and formatted against a tick the venue does not use.
SymbolId registerVenueSymbol(SymbolRegistry& registry, SymbolInfo info,
                             const InstrumentSpecTable& specs, ILogger* logger);

// Run once at start(), after the load: logs, as an error, every instrument
// the venue's metadata did not cover. Such a symbol is registered with its
// tick unknown: no order price on it is checked or rounded by the executor,
// and the venue's own refusal reaches the strategy as an event.
void reportInstrumentLoad(const InstrumentSpecTable& specs, std::string_view venue,
                          ILogger* logger);

}  // namespace lrvx
