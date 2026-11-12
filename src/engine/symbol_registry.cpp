/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "lrvx/engine/symbol_registry.h"
#include "lrvx/util/performance/profile.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <shared_mutex>
#include <span>
#include <stdexcept>

namespace lrvx
{

// =============================================================================
// Exchange management
// =============================================================================

ExchangeId SymbolRegistry::registerExchange(std::string_view name, VenueType type)
{
  std::scoped_lock lock(_mutex);

  // Check if already registered
  auto it = _exchangeNameToId.find(std::string(name));
  if (it != _exchangeNameToId.end())
  {
    return it->second;
  }

  // Check capacity
  if (_numExchanges >= kMaxExchanges)
  {
    return InvalidExchangeId;
  }

  // Fill the entry before the count admits it. Publishing the count first
  // let a reader see an id it was allowed to dereference while the name and
  // the venue type were still blank.
  ExchangeId id = static_cast<ExchangeId>(_numExchanges);
  _exchanges[id].setName(name);
  _exchanges[id].type = type;
  _exchangeNameToId[std::string(name)] = id;
  ++_numExchanges;

  return id;
}

const ExchangeInfo* SymbolRegistry::getExchangeLocked(ExchangeId id) const
{
  if (id >= _numExchanges)
  {
    return nullptr;
  }
  return &_exchanges[id];
}

std::optional<ExchangeInfo> SymbolRegistry::getExchange(ExchangeId id) const
{
  std::shared_lock lock(_mutex);
  const ExchangeInfo* info = getExchangeLocked(id);
  if (info == nullptr)
  {
    return std::nullopt;
  }
  // Copied here, under the lock. Handing back the address instead would move
  // the read to the call site, where nothing holds the lock any more.
  return *info;
}

ExchangeId SymbolRegistry::getExchangeId(std::string_view name) const
{
  std::shared_lock lock(_mutex);
  auto it = _exchangeNameToId.find(std::string(name));
  if (it != _exchangeNameToId.end())
  {
    return it->second;
  }
  return InvalidExchangeId;
}

// =============================================================================
// Symbol registration (ExchangeId-based API)
// =============================================================================

SymbolId SymbolRegistry::registerSymbol(ExchangeId exchange, std::string_view symbol)
{
  std::scoped_lock lock(_mutex);

  // Get exchange name for key
  if (exchange >= _numExchanges)
  {
    return 0;  // Invalid
  }

  std::string exchangeName(_exchanges[exchange].nameView());
  std::string key = exchangeName + ":" + std::string(symbol);

  auto it = _map.find(key);
  if (it != _map.end())
  {
    return it->second;
  }

  SymbolId id = static_cast<SymbolId>(_reverse.size());
  _map[key] = id;
  _reverse.emplace_back(exchangeName, std::string(symbol));

  // Ensure _symbolToExchange has capacity
  if (_symbolToExchange.size() <= id)
  {
    _symbolToExchange.resize(id + 1, InvalidExchangeId);
  }
  _symbolToExchange[id] = exchange;

  // A name-based registration used to fill only the name maps, so
  // getSymbolInfo() returned nullopt for the id it had just handed out and
  // the Strategy constructor threw on it. Store a defaulted SymbolInfo so
  // every registration path answers every getter; a caller that knows tick
  // size and scales uses the SymbolInfo overload.
  SymbolInfo info;
  info.id = id;
  info.exchange = exchangeName;
  info.symbol = std::string(symbol);
  info.exchangeId = exchange;
  _symbols[id] = std::move(info);

  return id;
}

ExchangeId SymbolRegistry::getExchangeForSymbolLocked(SymbolId symbol) const
{
  if (symbol >= _symbolToExchange.size())
  {
    return InvalidExchangeId;
  }
  return _symbolToExchange[symbol];
}

ExchangeId SymbolRegistry::getExchangeForSymbol(SymbolId symbol) const
{
  std::shared_lock lock(_mutex);
  return getExchangeForSymbolLocked(symbol);
}

VenueType SymbolRegistry::venueTypeForSymbol(SymbolId symbol) const
{
  std::shared_lock lock(_mutex);
  const ExchangeInfo* info = getExchangeLocked(getExchangeForSymbolLocked(symbol));
  return info != nullptr ? info->type : VenueType::CentralizedExchange;
}

// =============================================================================
// Symbol equivalence
// =============================================================================

void SymbolRegistry::mapEquivalentSymbols(std::span<const SymbolId> equivalentSymbols)
{
  std::scoped_lock lock(_mutex);

  if (equivalentSymbols.empty())
  {
    return;
  }

  // Ensure capacity
  SymbolId maxSym = 0;
  for (SymbolId s : equivalentSymbols)
  {
    maxSym = std::max(maxSym, s);
  }

  size_t requiredSize = (maxSym + 1) * kMaxEquivalentsPerSymbol;
  if (_equivalents.size() < requiredSize)
  {
    _equivalents.resize(requiredSize, 0);
  }
  if (_equivalentCounts.size() <= maxSym)
  {
    _equivalentCounts.resize(maxSym + 1, 0);
  }

  // Map all symbols to each other
  for (SymbolId sym : equivalentSymbols)
  {
    size_t baseIdx = sym * kMaxEquivalentsPerSymbol;
    uint8_t count = 0;

    for (SymbolId eq : equivalentSymbols)
    {
      if (count >= kMaxEquivalentsPerSymbol)
      {
        break;
      }
      _equivalents[baseIdx + count++] = eq;
    }
    _equivalentCounts[sym] = count;
  }
}

std::span<const SymbolId> SymbolRegistry::getEquivalentSymbolsLocked(SymbolId symbol) const
{
  if (symbol >= _equivalentCounts.size())
  {
    return {};
  }

  uint8_t count = _equivalentCounts[symbol];
  if (count == 0)
  {
    return {};
  }

  size_t baseIdx = symbol * kMaxEquivalentsPerSymbol;
  return {&_equivalents[baseIdx], count};
}

SymbolRegistry::EquivalentSymbols SymbolRegistry::getEquivalentSymbols(SymbolId symbol) const
{
  std::shared_lock lock(_mutex);

  // Same reasoning as getExchange: a span points into a vector that the next
  // mapEquivalentSymbols reallocates and clear() empties, and the caller walks
  // it after the lock is gone. The list is at most kMaxEquivalentsPerSymbol
  // entries, so it is copied out instead.
  EquivalentSymbols out;
  const std::span<const SymbolId> view = getEquivalentSymbolsLocked(symbol);
  out.count = static_cast<uint8_t>(view.size());
  std::copy(view.begin(), view.end(), out.ids.begin());
  return out;
}

SymbolId SymbolRegistry::getEquivalentOnExchange(SymbolId symbol, ExchangeId exchange) const
{
  std::shared_lock lock(_mutex);
  auto equivalents = getEquivalentSymbolsLocked(symbol);
  for (SymbolId eq : equivalents)
  {
    if (getExchangeForSymbolLocked(eq) == exchange)
    {
      return eq;
    }
  }
  return 0;  // Not found
}

// =============================================================================
// Legacy string-based API
// =============================================================================

SymbolId SymbolRegistry::registerSymbol(const std::string& exchange, const std::string& symbol)
{
  std::scoped_lock lock(_mutex);
  std::string key = exchange + ":" + symbol;
  auto it = _map.find(key);
  if (it != _map.end())
  {
    return it->second;
  }

  SymbolId id = static_cast<SymbolId>(_reverse.size());
  _map[key] = id;
  _reverse.emplace_back(exchange, symbol);

  // Same gap as the ExchangeId overload: without a SymbolInfo entry the id
  // this function returns cannot be fed to getSymbolInfo() or a Strategy.
  // The exchange is resolved when it happens to be registered, the same way
  // the SymbolInfo overload resolves it.
  ExchangeId exId = InvalidExchangeId;
  if (auto exIt = _exchangeNameToId.find(exchange); exIt != _exchangeNameToId.end())
  {
    exId = exIt->second;
  }
  SymbolInfo info;
  info.id = id;
  info.exchange = exchange;
  info.symbol = symbol;
  info.exchangeId = exId;
  _symbols[id] = std::move(info);

  if (_symbolToExchange.size() <= id)
  {
    _symbolToExchange.resize(id + 1, InvalidExchangeId);
  }
  _symbolToExchange[id] = exId;

  return id;
}

SymbolId SymbolRegistry::registerSymbol(const SymbolInfo& info)
{
  // Reject an invalid per-symbol scale before any mutation.
  // 0 is the invalid-symbol sentinel. The default 1e8 scale always passes.
  if (!validateSymbolScale(info))
  {
    return 0;
  }

  std::lock_guard lock(_mutex);
  std::string key = info.exchange + ":" + info.symbol;

  auto it = _map.find(key);
  if (it != _map.end())
  {
    return it->second;
  }

  // Use provided ID or find next available ID
  SymbolId newId;
  if (info.id != 0)
  {
    newId = info.id;
  }
  else
  {
    newId = static_cast<SymbolId>(_symbols.size() + 1);
    while (_symbols.find(newId) != _symbols.end())
    {
      ++newId;
    }
  }

  _map[key] = newId;

  ExchangeId exId = InvalidExchangeId;
  auto exIt = _exchangeNameToId.find(info.exchange);
  if (exIt != _exchangeNameToId.end())
  {
    exId = exIt->second;
  }

  SymbolInfo copy = info;
  copy.id = newId;
  copy.exchangeId = exId;
  _symbols[newId] = std::move(copy);

  if (_reverse.size() <= newId)
  {
    _reverse.resize(newId + 1);
  }
  _reverse[newId] = {info.exchange, info.symbol};

  // Record the symbol -> exchange mapping so getExchangeForSymbol and
  // venueTypeForSymbol work for SymbolInfo-registered symbols, the same as the
  // ExchangeId overload. Without this the mapping is only set by the
  // ExchangeId-based registerSymbol, and venue-type lookup silently defaults.
  if (_symbolToExchange.size() <= newId)
  {
    _symbolToExchange.resize(newId + 1, InvalidExchangeId);
  }
  _symbolToExchange[newId] = exId;

  return newId;
}

std::optional<SymbolId> SymbolRegistry::getSymbolId(const std::string& exchange,
                                                    const std::string& symbol) const
{
  LRVX_PROFILE_SCOPE("SymbolRegistry::getSymbolId");

  std::scoped_lock lock(_mutex);
  std::string key = exchange + ":" + symbol;
  auto it = _map.find(key);
  if (it != _map.end())
  {
    return it->second;
  }
  return std::nullopt;
}

std::pair<std::string, std::string> SymbolRegistry::getSymbolName(SymbolId id) const
{
  std::scoped_lock lock(_mutex);
  return _reverse.at(id);
}

std::optional<SymbolInfo> SymbolRegistry::getSymbolInfo(SymbolId id) const
{
  LRVX_PROFILE_SCOPE("SymbolRegistry::getSymbolInfo");

  std::lock_guard lock(_mutex);

  auto it = _symbols.find(id);
  if (it != _symbols.end())
  {
    return it->second;
  }

  return std::nullopt;
}

void SymbolRegistry::clear()
{
  std::lock_guard lock(_mutex);
  _symbols.clear();
  _map.clear();
  _reverse.clear();

  // Clear exchange data
  _exchanges = {};
  _exchangeNameToId.clear();
  _numExchanges = 0;

  // Clear symbol-to-exchange mapping
  _symbolToExchange.clear();

  // Clear equivalence data
  _equivalents.clear();
  _equivalentCounts.clear();
}

std::vector<SymbolInfo> SymbolRegistry::getAllSymbols() const
{
  std::lock_guard lock(_mutex);
  std::vector<SymbolInfo> result;
  result.reserve(_symbols.size());
  for (const auto& [id, info] : _symbols)
  {
    result.push_back(info);
  }
  return result;
}

size_t SymbolRegistry::size() const
{
  std::lock_guard lock(_mutex);
  return _symbols.size();
}

bool SymbolRegistry::saveToFile(const std::filesystem::path& path) const
{
  std::lock_guard lock(_mutex);

  // Binary, both here and in loadFromFile. In text mode Windows turns every
  // "\n" written into two bytes and turns them back on the way in, so the
  // size ftell reports and the count fread returns disagree and the load
  // rejects a file it wrote itself. Nothing on POSIX distinguishes the two
  // modes, so this costs nothing there.
  std::FILE* f = std::fopen(path.string().c_str(), "wb");
  if (!f)
  {
    return false;
  }

  // Simple JSON-like format
  std::fprintf(f, "{\n  \"version\": 1,\n  \"symbols\": [\n");

  size_t count = 0;
  for (const auto& [id, sym] : _symbols)
  {
    std::fprintf(f,
                 "    {\"id\": %u, \"exchange\": \"%s\", \"symbol\": \"%s\", \"type\": %d, "
                 "\"tick_size\": %lld",
                 sym.id, sym.exchange.c_str(), sym.symbol.c_str(), static_cast<int>(sym.type),
                 static_cast<long long>(sym.tickSize.raw()));
    // Written only when set, so an entry with no declared tick reads exactly as
    // a file from before the flag was persisted.
    if (sym.tickSizeKnown)
    {
      std::fprintf(f, ", \"tick_size_known\": true");
    }

    if (sym.strike.has_value())
    {
      std::fprintf(f, ", \"strike\": %lld", static_cast<long long>(sym.strike->raw()));
    }
    if (sym.expiry.has_value())
    {
      auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    sym.expiry->time_since_epoch())
                    .count();
      std::fprintf(f, ", \"expiry\": %lld", static_cast<long long>(ns));
    }
    if (sym.optionType.has_value())
    {
      std::fprintf(f, ", \"option_type\": %d", static_cast<int>(*sym.optionType));
    }

    // The shortest of %.10g (the form files have always carried) and %.17g
    // that reads back as the same double: a multiplier with more than ten
    // significant digits used to come back rounded.
    char mult[32];
    std::snprintf(mult, sizeof mult, "%.10g", sym.contractMultiplier);
    if (std::strtod(mult, nullptr) != sym.contractMultiplier)
    {
      std::snprintf(mult, sizeof mult, "%.17g", sym.contractMultiplier);
    }
    std::fprintf(f,
                 ", \"contract_multiplier\": %s, \"settlement_type\": %d, "
                 "\"exercise_style\": %d",
                 mult, static_cast<int>(sym.settlementType), static_cast<int>(sym.exerciseStyle));
    if (sym.settlementCcy.has_value())
    {
      std::fprintf(f, ", \"settlement_ccy\": \"%s\"", sym.settlementCcy->c_str());
    }
    // Written only when not the 1e8 default, so a default-scale entry reads
    // exactly as a file from before the scales were persisted.
    if (sym.priceScale != Price::Scale)
    {
      std::fprintf(f, ", \"price_scale\": %lld", static_cast<long long>(sym.priceScale));
    }
    if (sym.qtyScale != Quantity::Scale)
    {
      std::fprintf(f, ", \"qty_scale\": %lld", static_cast<long long>(sym.qtyScale));
    }

    std::fprintf(f, "}%s\n", (++count < _symbols.size()) ? "," : "");
  }

  std::fprintf(f, "  ]\n}\n");
  std::fclose(f);
  return true;
}

bool SymbolRegistry::loadFromFile(const std::filesystem::path& path)
{
  std::FILE* f = std::fopen(path.string().c_str(), "rb");
  if (!f)
  {
    return false;
  }

  // Read to the end and use what actually arrived, rather than asking ftell
  // how many bytes there should be and rejecting the file when fread returns
  // a different number. That comparison has no right answer on Windows: in
  // text mode the CRT rewrites line endings on the way through, so the two
  // disagree by design, and a file this same class had just written was
  // refused on load. Binary mode above removes the rewriting; reading to EOF
  // removes the dependency on the two numbers agreeing at all.
  std::string content;
  char buf[64 * 1024];
  for (size_t n = std::fread(buf, 1, sizeof buf, f); n > 0;
       n = std::fread(buf, 1, sizeof buf, f))
  {
    content.append(buf, n);
  }
  const bool readFailed = std::ferror(f) != 0;
  std::fclose(f);

  if (readFailed || content.empty())
  {
    return false;
  }

  // Simple parsing - look for symbol entries
  // Format: {"id": N, "exchange": "X", "symbol": "Y", "type": T, ...}

  std::lock_guard lock(_mutex);
  _symbols.clear();
  _map.clear();
  _reverse.clear();

  // A file that is not the shape this expects is a false, not an exception:
  // the signature promises a bool, and a caller loading a registry at startup
  // should not have to guard a bool-returning call with a try. The numbers
  // below are the reason it can happen -- a nanosecond timestamp and a
  // fixed-point price do not fit the 32 bits `long` has on Windows, which is
  // why they are parsed as long long, and a truncated or foreign file can
  // still present something that does not convert.
  try
  {
    size_t pos = 0;
    while ((pos = content.find("\"id\":", pos)) != std::string::npos)
    {
      SymbolInfo info;

      // Parse id
      pos += 5;
      info.id = static_cast<SymbolId>(std::stoull(content.substr(pos)));

      // Parse exchange
      size_t exch_start = content.find("\"exchange\":", pos);
      if (exch_start == std::string::npos)
      {
        break;
      }
      exch_start = content.find('"', exch_start + 11) + 1;
      size_t exch_end = content.find('"', exch_start);
      info.exchange = content.substr(exch_start, exch_end - exch_start);

      // Parse symbol
      size_t sym_start = content.find("\"symbol\":", pos);
      if (sym_start == std::string::npos)
      {
        break;
      }
      sym_start = content.find('"', sym_start + 9) + 1;
      size_t sym_end = content.find('"', sym_start);
      info.symbol = content.substr(sym_start, sym_end - sym_start);

      // Parse type
      size_t type_start = content.find("\"type\":", pos);
      if (type_start != std::string::npos && type_start < content.find('}', pos))
      {
        type_start += 7;
        info.type = static_cast<InstrumentType>(std::stoi(content.substr(type_start)));
      }

      // Parse tick_size
      size_t tick_start = content.find("\"tick_size\":", pos);
      if (tick_start != std::string::npos && tick_start < content.find('}', pos))
      {
        tick_start += 12;
        info.tickSize = Price::fromRaw(std::stoll(content.substr(tick_start)));
      }

      // Optional fields - strike
      size_t strike_start = content.find("\"strike\":", pos);
      size_t entry_end = content.find('}', pos);

      // A file written before the flag was persisted has no key: its ticks
      // load as undeclared, and only a tick other than the 0.01 default is
      // checked, the same as before the load.
      size_t known_start = content.find("\"tick_size_known\":", pos);
      if (known_start != std::string::npos && known_start < entry_end)
      {
        known_start = content.find_first_not_of(' ', known_start + 18);
        info.tickSizeKnown = content.compare(known_start, 4, "true") == 0;
      }
      if (strike_start != std::string::npos && strike_start < entry_end)
      {
        strike_start += 9;
        info.strike = Price::fromRaw(std::stoll(content.substr(strike_start)));
      }

      // Optional - expiry
      size_t expiry_start = content.find("\"expiry\":", pos);
      if (expiry_start != std::string::npos && expiry_start < entry_end)
      {
        expiry_start += 9;
        auto ns = std::stoll(content.substr(expiry_start));
        info.expiry = TimePoint(std::chrono::nanoseconds(ns));
      }

      // Optional - option_type
      size_t opt_start = content.find("\"option_type\":", pos);
      if (opt_start != std::string::npos && opt_start < entry_end)
      {
        opt_start += 14;
        info.optionType = static_cast<OptionType>(std::stoi(content.substr(opt_start)));
      }

      // Contract spec (older files omit these; defaults already on `info`).
      size_t mult_start = content.find("\"contract_multiplier\":", pos);
      if (mult_start != std::string::npos && mult_start < entry_end)
      {
        info.contractMultiplier = std::stod(content.substr(mult_start + 22));
      }
      size_t settle_start = content.find("\"settlement_type\":", pos);
      if (settle_start != std::string::npos && settle_start < entry_end)
      {
        info.settlementType = static_cast<SettlementType>(std::stoi(content.substr(settle_start + 18)));
      }
      size_t ex_start = content.find("\"exercise_style\":", pos);
      if (ex_start != std::string::npos && ex_start < entry_end)
      {
        info.exerciseStyle = static_cast<ExerciseStyle>(std::stoi(content.substr(ex_start + 17)));
      }
      size_t ccy_start = content.find("\"settlement_ccy\": \"", pos);
      if (ccy_start != std::string::npos && ccy_start < entry_end)
      {
        ccy_start += 19;
        size_t ccy_end = content.find('"', ccy_start);
        if (ccy_end != std::string::npos)
        {
          info.settlementCcy = content.substr(ccy_start, ccy_end - ccy_start);
        }
      }

      // Per-symbol fixed-point scale. A file written before the scales were
      // persisted has neither key and loads the 1e8 default, as it always did.
      size_t pscale_start = content.find("\"price_scale\":", pos);
      if (pscale_start != std::string::npos && pscale_start < entry_end)
      {
        info.priceScale = std::stoll(content.substr(pscale_start + 14));
      }
      size_t qscale_start = content.find("\"qty_scale\":", pos);
      if (qscale_start != std::string::npos && qscale_start < entry_end)
      {
        info.qtyScale = std::stoll(content.substr(qscale_start + 12));
      }
      // A scale registerSymbol would refuse makes the file malformed.
      if (!validateSymbolScale(info))
      {
        throw std::invalid_argument("symbol registry file: invalid scale");
      }

      // Add to registry
      std::string key = info.exchange + ":" + info.symbol;
      _map[key] = info.id;
      std::string exchange_copy = info.exchange;
      std::string symbol_copy = info.symbol;
      _symbols[info.id] = std::move(info);
      _reverse.emplace_back(std::move(exchange_copy), std::move(symbol_copy));

      pos = entry_end + 1;
    }
  }
  catch (const std::exception&)
  {
    _symbols.clear();
    _map.clear();
    _reverse.clear();
    return false;
  }

  return !_symbols.empty() || content.find("\"symbols\": []") != std::string::npos;
}

// Binary format:
// [4 bytes] magic: "SREG"
// [4 bytes] version
// [4 bytes] symbol count
// For each symbol:
//   [4 bytes] id
//   [2 bytes] exchange length
//   [N bytes] exchange string
//   [2 bytes] symbol length
//   [N bytes] symbol string
//   [1 byte] instrument type
//   [8 bytes] tick_size (v2+)
//   [1 byte] flags (0x01 strike, 0x02 expiry, 0x04 option_type,
//            0x08 settlement_ccy, 0x10 tick_size_known)
//   [8 bytes] strike (if flag set)
//   [8 bytes] expiry (if flag set)
//   [1 byte] option_type (if flag set)

static constexpr uint32_t kSymbolRegistryMagic = 0x47455253;  // "SREG"
// v3 adds the contract spec (multiplier / settlement_type / exercise_style /
// settlement_ccy). v2 files load with defaults for those.
// v4 adds per-symbol fixed-point scale (price_scale / qty_scale). Older files
// load with the default 1e8 scale, so they are byte-identical in behavior.
static constexpr uint32_t kSymbolRegistryVersion = 4;
// A flag bit rather than a version: a reader that predates it ignores the bit
// and loads the symbol as it always did.
static constexpr uint8_t kFlagTickSizeKnown = 0x10;

std::vector<std::byte> SymbolRegistry::serialize() const
{
  std::lock_guard lock(_mutex);

  std::vector<std::byte> result;
  result.reserve(1024 + _symbols.size() * 64);  // Estimate

  auto write_u32 = [&result](uint32_t v)
  {
    result.push_back(static_cast<std::byte>(v & 0xFF));
    result.push_back(static_cast<std::byte>((v >> 8) & 0xFF));
    result.push_back(static_cast<std::byte>((v >> 16) & 0xFF));
    result.push_back(static_cast<std::byte>((v >> 24) & 0xFF));
  };

  auto write_u16 = [&result](uint16_t v)
  {
    result.push_back(static_cast<std::byte>(v & 0xFF));
    result.push_back(static_cast<std::byte>((v >> 8) & 0xFF));
  };

  auto write_u8 = [&result](uint8_t v)
  { result.push_back(static_cast<std::byte>(v)); };

  auto write_i64 = [&result](int64_t v)
  {
    for (int i = 0; i < 8; ++i)
    {
      result.push_back(static_cast<std::byte>((v >> (i * 8)) & 0xFF));
    }
  };

  auto write_f64 = [&result](double d)
  {
    int64_t bits;
    std::memcpy(&bits, &d, sizeof(bits));
    for (int i = 0; i < 8; ++i)
    {
      result.push_back(static_cast<std::byte>((bits >> (i * 8)) & 0xFF));
    }
  };

  auto write_string = [&](const std::string& s)
  {
    write_u16(static_cast<uint16_t>(s.size()));
    for (char c : s)
    {
      result.push_back(static_cast<std::byte>(c));
    }
  };

  // Header
  write_u32(kSymbolRegistryMagic);
  write_u32(kSymbolRegistryVersion);
  write_u32(static_cast<uint32_t>(_symbols.size()));

  // Symbols
  for (const auto& [id, sym] : _symbols)
  {
    write_u32(sym.id);
    write_string(sym.exchange);
    write_string(sym.symbol);
    write_u8(static_cast<uint8_t>(sym.type));
    write_i64(sym.tickSize.raw());

    uint8_t flags = 0;
    if (sym.strike.has_value())
    {
      flags |= 0x01;
    }
    if (sym.expiry.has_value())
    {
      flags |= 0x02;
    }
    if (sym.optionType.has_value())
    {
      flags |= 0x04;
    }
    if (sym.settlementCcy.has_value())
    {
      flags |= 0x08;
    }
    if (sym.tickSizeKnown)
    {
      flags |= kFlagTickSizeKnown;
    }
    write_u8(flags);

    if (sym.strike.has_value())
    {
      write_i64(sym.strike->raw());
    }
    if (sym.expiry.has_value())
    {
      auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    sym.expiry->time_since_epoch())
                    .count();
      write_i64(ns);
    }
    if (sym.optionType.has_value())
    {
      write_u8(static_cast<uint8_t>(*sym.optionType));
    }

    // v3 contract spec.
    write_f64(sym.contractMultiplier);
    write_u8(static_cast<uint8_t>(sym.settlementType));
    write_u8(static_cast<uint8_t>(sym.exerciseStyle));
    if (sym.settlementCcy.has_value())
    {
      write_string(*sym.settlementCcy);
    }

    // v4 per-symbol fixed-point scale.
    write_i64(sym.priceScale);
    write_i64(sym.qtyScale);
  }

  return result;
}

bool SymbolRegistry::deserialize(std::span<const std::byte> data)
{
  if (data.size() < 12)
  {
    return false;  // Minimum header size
  }

  size_t pos = 0;

  auto read_u32 = [&data, &pos]() -> uint32_t
  {
    if (pos + 4 > data.size())
    {
      return 0;
    }
    uint32_t v = static_cast<uint32_t>(data[pos]) | (static_cast<uint32_t>(data[pos + 1]) << 8) |
                 (static_cast<uint32_t>(data[pos + 2]) << 16) |
                 (static_cast<uint32_t>(data[pos + 3]) << 24);
    pos += 4;
    return v;
  };

  auto read_u16 = [&data, &pos]() -> uint16_t
  {
    if (pos + 2 > data.size())
    {
      return 0;
    }
    uint16_t v = static_cast<uint16_t>(data[pos]) | (static_cast<uint16_t>(data[pos + 1]) << 8);
    pos += 2;
    return v;
  };

  auto read_u8 = [&data, &pos]() -> uint8_t
  {
    if (pos >= data.size())
    {
      return 0;
    }
    return static_cast<uint8_t>(data[pos++]);
  };

  auto read_i64 = [&data, &pos]() -> int64_t
  {
    if (pos + 8 > data.size())
    {
      return 0;
    }
    int64_t v = 0;
    for (int i = 0; i < 8; ++i)
    {
      v |= static_cast<int64_t>(static_cast<uint8_t>(data[pos + i])) << (i * 8);
    }
    pos += 8;
    return v;
  };

  auto read_f64 = [&read_i64]() -> double
  {
    int64_t bits = read_i64();
    double d;
    std::memcpy(&d, &bits, sizeof(d));
    return d;
  };

  auto read_string = [&data, &pos, &read_u16]() -> std::string
  {
    uint16_t len = read_u16();
    if (pos + len > data.size())
    {
      return "";
    }
    std::string s(len, '\0');
    for (uint16_t i = 0; i < len; ++i)
    {
      s[i] = static_cast<char>(data[pos + i]);
    }
    pos += len;
    return s;
  };

  // Read header
  uint32_t magic = read_u32();
  if (magic != kSymbolRegistryMagic)
  {
    return false;
  }

  uint32_t version = read_u32();
  if (version != 1 && version != 2 && version != 3 && version != 4)
  {
    return false;
  }

  uint32_t count = read_u32();

  std::lock_guard lock(_mutex);
  _symbols.clear();
  _map.clear();
  _reverse.clear();

  for (uint32_t i = 0; i < count; ++i)
  {
    SymbolInfo info;
    info.id = read_u32();
    info.exchange = read_string();
    info.symbol = read_string();
    info.type = static_cast<InstrumentType>(read_u8());

    if (version >= 2)
    {
      info.tickSize = Price::fromRaw(read_i64());
    }

    uint8_t flags = read_u8();
    // Clear in data written before the bit existed, at any version: those
    // ticks load as undeclared, as they were.
    info.tickSizeKnown = (flags & kFlagTickSizeKnown) != 0;

    if (flags & 0x01)
    {
      info.strike = Price::fromRaw(read_i64());
    }
    if (flags & 0x02)
    {
      info.expiry = TimePoint(std::chrono::nanoseconds(read_i64()));
    }
    if (flags & 0x04)
    {
      info.optionType = static_cast<OptionType>(read_u8());
    }

    if (version >= 3)
    {
      info.contractMultiplier = read_f64();
      info.settlementType = static_cast<SettlementType>(read_u8());
      info.exerciseStyle = static_cast<ExerciseStyle>(read_u8());
      if (flags & 0x08)
      {
        info.settlementCcy = read_string();
      }
    }

    if (version >= 4)
    {
      info.priceScale = read_i64();
      info.qtyScale = read_i64();
    }

    std::string key = info.exchange + ":" + info.symbol;
    _map[key] = info.id;
    std::string exchange_copy = info.exchange;
    std::string symbol_copy = info.symbol;
    _symbols[info.id] = std::move(info);
    _reverse.emplace_back(std::move(exchange_copy), std::move(symbol_copy));
  }

  return true;
}

}  // namespace lrvx
