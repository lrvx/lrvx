/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "lrvx/engine/symbol_registry.h"
#include "lrvx/execution/order_price_rule.h"
#include "lrvx/util/performance/profile.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <shared_mutex>
#include <span>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#include <process.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

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

SymbolId SymbolRegistry::nextSymbolIdLocked() const
{
  return _nextSymbolId;
}

void SymbolRegistry::setSymbolNameLocked(SymbolId id, std::string exchange, std::string symbol)
{
  _reverse.slot(id) = {std::move(exchange), std::move(symbol)};
  if (id >= _nextSymbolId)
  {
    _nextSymbolId = id + 1;
  }
}

std::string SymbolRegistry::nameKey(std::string_view exchange, std::string_view symbol)
{
  std::string key = std::to_string(exchange.size());
  key.reserve(key.size() + 1 + exchange.size() + symbol.size());
  key += ':';
  key += exchange;
  key += symbol;
  return key;
}

SymbolId SymbolRegistry::registerSymbol(ExchangeId exchange, std::string_view symbol)
{
  std::scoped_lock lock(_mutex);

  // Get exchange name for key
  if (exchange >= _numExchanges)
  {
    return 0;  // Invalid
  }

  std::string exchangeName(_exchanges[exchange].nameView());
  std::string key = nameKey(exchangeName, symbol);

  auto it = _map.find(key);
  if (it != _map.end())
  {
    return it->second;
  }

  const SymbolId id = nextSymbolIdLocked();
  if (id > kMaxSymbolId)
  {
    return 0;
  }
  _map[key] = id;
  setSymbolNameLocked(id, exchangeName, std::string(symbol));

  _symbolToExchange.slot(id, InvalidExchangeId) = exchange;

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
  return _symbolToExchange.valueOr(symbol, InvalidExchangeId);
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

  // Map all symbols to each other
  EquivalentSymbols all;
  for (SymbolId eq : equivalentSymbols)
  {
    if (all.count >= kMaxEquivalentsPerSymbol)
    {
      break;
    }
    all.ids[all.count++] = eq;
  }
  for (SymbolId sym : equivalentSymbols)
  {
    _equivalents.slot(sym) = all;
  }
}

std::span<const SymbolId> SymbolRegistry::getEquivalentSymbolsLocked(SymbolId symbol) const
{
  const EquivalentSymbols* entry = _equivalents.find(symbol);
  if (entry == nullptr || entry->count == 0)
  {
    return {};
  }
  return {entry->ids.data(), entry->count};
}

SymbolRegistry::EquivalentSymbols SymbolRegistry::getEquivalentSymbols(SymbolId symbol) const
{
  std::shared_lock lock(_mutex);

  // Same reasoning as getExchange: a span points into storage that the next
  // mapEquivalentSymbols may move and clear() empties, and the caller walks
  // it after the lock is gone. The list is at most kMaxEquivalentsPerSymbol
  // entries, so the table's entry is copied out instead.
  return _equivalents.valueOr(symbol, EquivalentSymbols{});
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
  std::string key = nameKey(exchange, symbol);
  auto it = _map.find(key);
  if (it != _map.end())
  {
    return it->second;
  }

  const SymbolId id = nextSymbolIdLocked();
  if (id > kMaxSymbolId)
  {
    return 0;
  }
  _map[key] = id;
  setSymbolNameLocked(id, exchange, symbol);

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

  _symbolToExchange.slot(id, InvalidExchangeId) = exId;

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
  std::string key = nameKey(info.exchange, info.symbol);

  auto it = _map.find(key);
  if (it != _map.end())
  {
    return it->second;
  }

  // The id the caller names, or the next one. An id another symbol holds is
  // refused: taking it would replace that symbol's entry while its name still
  // resolved to the id.
  if (info.id != 0 && _symbols.find(info.id) != _symbols.end())
  {
    return 0;
  }
  const SymbolId newId = info.id != 0 ? info.id : nextSymbolIdLocked();
  // The tables beside the symbol table are indexed by id: an id above the
  // limit would size them for every id below it.
  if (newId > kMaxSymbolId)
  {
    return 0;
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

  setSymbolNameLocked(newId, info.exchange, info.symbol);

  // Record the symbol -> exchange mapping so getExchangeForSymbol and
  // venueTypeForSymbol work for SymbolInfo-registered symbols, the same as the
  // ExchangeId overload. Without this the mapping is only set by the
  // ExchangeId-based registerSymbol, and venue-type lookup silently defaults.
  _symbolToExchange.slot(newId, InvalidExchangeId) = exId;

  return newId;
}

std::optional<SymbolId> SymbolRegistry::getSymbolId(const std::string& exchange,
                                                    const std::string& symbol) const
{
  LRVX_PROFILE_SCOPE("SymbolRegistry::getSymbolId");

  std::scoped_lock lock(_mutex);
  std::string key = nameKey(exchange, symbol);
  auto it = _map.find(key);
  if (it != _map.end())
  {
    return it->second;
  }
  return std::nullopt;
}

namespace
{
[[noreturn]] void throwNoSuchSymbol(SymbolId id)
{
  throw std::out_of_range("SymbolRegistry::getSymbolName: no symbol has the id " +
                          std::to_string(id));
}
}  // namespace

std::pair<std::string, std::string> SymbolRegistry::getSymbolName(SymbolId id) const
{
  std::scoped_lock lock(_mutex);
  // The array read it always was for the ids a registry normally holds.
  if (id < _reverse.denseSize()) [[likely]]
  {
    return _reverse.denseAt(id);
  }
  return symbolNameAboveDenseLocked(id);
}

std::pair<std::string, std::string> SymbolRegistry::symbolNameAboveDenseLocked(SymbolId id) const
{
  // An id at or above the next one to hand out names nothing; one below it
  // that was skipped has an empty name, as the array this used to index had.
  if (id >= _nextSymbolId)
  {
    throwNoSuchSymbol(id);
  }
  const auto* name = _reverse.find(id);
  return name != nullptr ? *name : std::pair<std::string, std::string>{};
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

VenueTickResult SymbolRegistry::applyVenueTick(SymbolId id, Price venueTick)
{
  std::lock_guard lock(_mutex);
  auto it = _symbols.find(id);
  if (it == _symbols.end())
  {
    return {VenueTickOutcome::UnknownSymbol, Price{}};
  }
  SymbolInfo& info = it->second;
  if (venueTick.raw() <= 0)
  {
    return {VenueTickOutcome::InvalidTick, info.tickSize};
  }
  const int64_t declared = orderTickRaw(info);
  if (declared == venueTick.raw())
  {
    return {VenueTickOutcome::Unchanged, info.tickSize};
  }
  if (declared != 0)
  {
    // A tick the venue wrote here is the venue's to change again; any other
    // was declared by the caller.
    const auto applied = _venueTicks->tickFor(id);
    if (!applied || applied->raw() != info.tickSize.raw())
    {
      return {VenueTickOutcome::Conflict, info.tickSize};
    }
  }
  info.tickSize = venueTick;
  info.tickSizeKnown = true;
  _venueTicks->record(id, venueTick);
  return {VenueTickOutcome::Applied, info.tickSize};
}

VenueLotResult SymbolRegistry::applyVenueLot(SymbolId id, Quantity venueLot)
{
  std::lock_guard lock(_mutex);
  auto it = _symbols.find(id);
  if (it == _symbols.end())
  {
    return {VenueTickOutcome::UnknownSymbol, Quantity{}};
  }
  SymbolInfo& info = it->second;
  if (venueLot.raw() <= 0)
  {
    return {VenueTickOutcome::InvalidTick, info.lotSize};
  }
  const int64_t declared = orderLotRaw(info);
  if (declared == venueLot.raw())
  {
    return {VenueTickOutcome::Unchanged, info.lotSize};
  }
  if (declared != 0)
  {
    // A lot the venue wrote here is the venue's to change again; any other
    // was declared by the caller.
    const auto applied = _venueTicks->lotFor(id);
    if (!applied || applied->raw() != info.lotSize.raw())
    {
      return {VenueTickOutcome::Conflict, info.lotSize};
    }
  }
  info.lotSize = venueLot;
  _venueTicks->recordLot(id, venueLot);
  return {VenueTickOutcome::Applied, info.lotSize};
}

void SymbolRegistry::clear()
{
  std::lock_guard lock(_mutex);
  _venueTicks->reset();
  _symbols.clear();
  _map.clear();
  _reverse.clear();
  _nextSymbolId = 1;

  // Clear exchange data
  _exchanges = {};
  _exchangeNameToId.clear();
  _numExchanges = 0;

  // Clear symbol-to-exchange mapping
  _symbolToExchange.clear();

  // Clear equivalence data
  _equivalents.clear();
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
  // Ids are handed out in registration order; the map's own order is the
  // standard library's, and "the first symbol" must not depend on it.
  std::sort(result.begin(), result.end(),
            [](const SymbolInfo& a, const SymbolInfo& b)
            { return a.id < b.id; });
  return result;
}

size_t SymbolRegistry::size() const
{
  std::lock_guard lock(_mutex);
  return _symbols.size();
}

namespace
{

// `s` as a JSON string literal, quotes included. Quotes, backslashes and
// control characters are escaped; every other byte, UTF-8 included, is
// written as it is.
std::string jsonString(std::string_view s)
{
  std::string out;
  out.reserve(s.size() + 2);
  out += '"';
  for (const char ch : s)
  {
    const auto c = static_cast<unsigned char>(ch);
    switch (c)
    {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      default:
        if (c < 0x20)
        {
          char esc[8];
          std::snprintf(esc, sizeof esc, "\\u%04x", c);
          out += esc;
        }
        else
        {
          out += ch;
        }
    }
  }
  out += '"';
  return out;
}

void appendUtf8(std::string& out, uint32_t cp)
{
  if (cp < 0x80)
  {
    out += static_cast<char>(cp);
  }
  else if (cp < 0x800)
  {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
  else if (cp < 0x10000)
  {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
  else
  {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

// The reader of the registry's JSON file: the object of every entry in the
// "symbols" array, as key -> raw value (strings unescaped, numbers and
// literals as their text). It walks the text token by token, so a name
// holding a brace, a comma or something that looks like a key is read as the
// string it is.
class RegistryJsonReader
{
 public:
  explicit RegistryJsonReader(std::string_view text) : _t(text) {}

  using Entry = std::unordered_map<std::string, std::string>;

  // The whole document: one object holding a "symbols" array of flat objects
  // and, beside it, scalar members such as "version". False, with the reason
  // in `why`, for anything else, and for anything but whitespace after the
  // object's closing brace. `version` is left alone when the document has
  // none.
  bool document(std::vector<Entry>& out, std::optional<std::string>& version, std::string& why)
  {
    const auto bad = [this, &why](const char* what)
    {
      why = std::string(what) + " at byte " + std::to_string(_p);
      return false;
    };
    ws();
    if (!eat('{'))
    {
      return bad("the document is not a JSON object: expected '{'");
    }
    bool haveSymbols = false;
    for (bool first = true;; first = false)
    {
      ws();
      if (eat('}'))
      {
        break;
      }
      if (!first && !eat(','))
      {
        return bad("expected ',' or '}'");
      }
      ws();
      std::string k;
      if (!string(k))
      {
        return bad("expected a member name");
      }
      ws();
      if (!eat(':'))
      {
        return bad("expected ':'");
      }
      ws();
      if (k == "symbols")
      {
        if (haveSymbols)
        {
          return bad("a second \"symbols\" member");
        }
        if (!entries(out))
        {
          return bad(_reason.empty() ? "\"symbols\" is not an array of objects: malformed"
                                     : _reason.c_str());
        }
        haveSymbols = true;
        continue;
      }
      std::string v;
      if (!scalar(v))
      {
        return bad("expected a string, a number or a literal");
      }
      if (k == "version")
      {
        version = std::move(v);
      }
    }
    ws();
    if (_p != _t.size())
    {
      return bad("content after the end of the document");
    }
    if (!haveSymbols)
    {
      why = "the document has no \"symbols\" array";
      return false;
    }
    return true;
  }

 private:
  // The array the cursor is at.
  bool entries(std::vector<Entry>& out)
  {
    if (!eat('['))
    {
      return false;
    }
    for (;;)
    {
      ws();
      if (eat(']'))
      {
        return true;
      }
      if (!out.empty() && !eat(','))
      {
        return false;
      }
      ws();
      Entry e;
      if (!object(e, out.size()))
      {
        return false;
      }
      out.push_back(std::move(e));
    }
  }

  // A string, or the text of a number or literal.
  bool scalar(std::string& v)
  {
    if (peek() == '"')
    {
      return string(v);
    }
    const size_t start = _p;
    while (_p < _t.size() && _t[_p] != ',' && _t[_p] != '}' && _t[_p] != ']' && _t[_p] != '{' &&
           _t[_p] != '[' && !isSpace(_t[_p]))
    {
      ++_p;
    }
    v = std::string(_t.substr(start, _p - start));
    return !v.empty();
  }

  // One entry: a flat object. A member whose value is itself an object or an
  // array is refused with the reason in _reason. It used to be taken as raw
  // text, or to end the entry where the nested value closed.
  bool object(Entry& e, size_t index)
  {
    if (!eat('{'))
    {
      return false;
    }
    for (bool first = true;; first = false)
    {
      ws();
      if (eat('}'))
      {
        return true;
      }
      if (!first && !eat(','))
      {
        return false;
      }
      ws();
      std::string k;
      if (!string(k))
      {
        return false;
      }
      ws();
      if (!eat(':'))
      {
        return false;
      }
      ws();
      std::string v;
      if (peek() == '{' || peek() == '[')
      {
        _reason = "entry " + std::to_string(index) + ": member \"" + k + "\" is " +
                  (peek() == '{' ? "an object" : "an array") +
                  "; a registry entry is flat, every member a string, a number or a literal";
        return false;
      }
      if (peek() == '"')
      {
        if (!string(v))
        {
          return false;
        }
      }
      else
      {
        const size_t start = _p;
        while (_p < _t.size() && _t[_p] != ',' && _t[_p] != '}' && !isSpace(_t[_p]))
        {
          ++_p;
        }
        v = std::string(_t.substr(start, _p - start));
        if (v.empty())
        {
          return false;
        }
      }
      e[std::move(k)] = std::move(v);
    }
  }

  // A JSON string. An escape JSON does not define is kept as written: the
  // writer before this one put names into the file unescaped, and a
  // backslash in such a name reads back as the backslash it was.
  bool string(std::string& out)
  {
    if (!eat('"'))
    {
      return false;
    }
    while (_p < _t.size())
    {
      const char c = _t[_p++];
      if (c == '"')
      {
        return true;
      }
      if (c != '\\' || _p >= _t.size())
      {
        out += c;
        continue;
      }
      const char e = _t[_p++];
      switch (e)
      {
        case '"':
        case '\\':
        case '/':
          out += e;
          break;
        case 'n':
          out += '\n';
          break;
        case 'r':
          out += '\r';
          break;
        case 't':
          out += '\t';
          break;
        case 'b':
          out += '\b';
          break;
        case 'f':
          out += '\f';
          break;
        case 'u':
        {
          uint32_t cp = 0;
          if (!hex4(cp))
          {
            out += "\\u";
            break;
          }
          if (cp >= 0xD800 && cp < 0xDC00 && _p + 1 < _t.size() && _t[_p] == '\\' &&
              _t[_p + 1] == 'u')
          {
            const size_t save = _p;
            _p += 2;
            uint32_t lo = 0;
            if (hex4(lo) && lo >= 0xDC00 && lo < 0xE000)
            {
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            }
            else
            {
              _p = save;
            }
          }
          appendUtf8(out, cp);
          break;
        }
        default:
          out += '\\';
          out += e;
      }
    }
    return false;
  }

  bool hex4(uint32_t& cp)
  {
    if (_p + 4 > _t.size())
    {
      return false;
    }
    uint32_t v = 0;
    for (size_t i = 0; i < 4; ++i)
    {
      const char h = _t[_p + i];
      v <<= 4;
      if (h >= '0' && h <= '9')
      {
        v |= static_cast<uint32_t>(h - '0');
      }
      else if (h >= 'a' && h <= 'f')
      {
        v |= static_cast<uint32_t>(h - 'a' + 10);
      }
      else if (h >= 'A' && h <= 'F')
      {
        v |= static_cast<uint32_t>(h - 'A' + 10);
      }
      else
      {
        return false;
      }
    }
    _p += 4;
    cp = v;
    return true;
  }

  static bool isSpace(char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; }
  void ws()
  {
    while (_p < _t.size() && isSpace(_t[_p]))
    {
      ++_p;
    }
  }
  char peek() const { return _p < _t.size() ? _t[_p] : '\0'; }
  bool eat(char c)
  {
    if (peek() != c)
    {
      return false;
    }
    ++_p;
    return true;
  }

  std::string_view _t;
  size_t _p{0};
  // Why an entry was refused, when there is more to say than "malformed".
  std::string _reason;
};

// What registration builds next to the symbol table, rebuilt for symbols a
// load put there: each info's exchange id and the symbol -> exchange mapping,
// resolved through the exchanges this registry knows (unknown ones stay
// InvalidExchangeId, as registerSymbol leaves them), and the id -> name table,
// indexed by id.
void rebuildLoadedIndexes(std::unordered_map<SymbolId, SymbolInfo>& symbols,
                          const std::unordered_map<std::string, ExchangeId>& exchangeNameToId,
                          SymbolRegistry::IdTable<std::pair<std::string, std::string>>& reverse,
                          SymbolRegistry::IdTable<ExchangeId>& symbolToExchange,
                          SymbolId& nextSymbolId)
{
  reverse.clear();
  symbolToExchange.clear();
  nextSymbolId = 1;
  for (auto& [id, info] : symbols)
  {
    ExchangeId exId = InvalidExchangeId;
    if (auto it = exchangeNameToId.find(info.exchange); it != exchangeNameToId.end())
    {
      exId = it->second;
    }
    info.exchangeId = exId;
    reverse.slot(id) = {info.exchange, info.symbol};
    symbolToExchange.slot(id, InvalidExchangeId) = exId;
    nextSymbolId = std::max<SymbolId>(nextSymbolId, id + 1);
  }
}

// The entries in ascending id order. The table is a hash map, and what is
// written from it must not depend on the map's iteration order.
std::vector<const SymbolInfo*> entriesById(const std::unordered_map<SymbolId, SymbolInfo>& symbols)
{
  std::vector<const SymbolInfo*> out;
  out.reserve(symbols.size());
  // order: not observable -- sorted below
  for (const auto& [id, info] : symbols)
  {
    out.push_back(&info);
  }
  std::sort(out.begin(), out.end(),
            [](const SymbolInfo* a, const SymbolInfo* b)
            { return a->id < b->id; });
  return out;
}

// A file of a name nothing else has, beside `path`, opened for writing: the
// path, this process, a counter and ".tmp". The "x" mode refuses a name that
// exists, so two saves never write through one name, in this process or
// across processes.
std::FILE* openUniqueTemp(const std::filesystem::path& path, std::filesystem::path& tmp)
{
  static std::atomic<uint64_t> counter{0};
#if defined(_WIN32)
  const auto pid = static_cast<unsigned long long>(_getpid());
#else
  const auto pid = static_cast<unsigned long long>(::getpid());
#endif
  for (int attempt = 0; attempt < 64; ++attempt)
  {
    tmp = path;
    tmp += "." + std::to_string(pid) + "." +
           std::to_string(counter.fetch_add(1, std::memory_order_relaxed)) + ".tmp";
    errno = 0;
    if (std::FILE* f = std::fopen(tmp.string().c_str(), "wbx"))
    {
      return f;
    }
    if (errno != EEXIST)
    {
      return nullptr;
    }
  }
  return nullptr;
}

// The stream's bytes, already flushed to the OS, forced to the device.
bool syncToDevice(std::FILE* f)
{
#if defined(_WIN32)
  return _commit(_fileno(f)) == 0;
#else
  return ::fsync(fileno(f)) == 0;
#endif
}

// The rename is durable once the directory holding the name is synced. Where
// a directory cannot be opened or synced the rename has still happened.
void syncDirectoryOf(const std::filesystem::path& path)
{
#if !defined(_WIN32)
  std::filesystem::path dir = path.parent_path();
  if (dir.empty())
  {
    dir = ".";
  }
  const int fd = ::open(dir.string().c_str(), O_RDONLY);
  if (fd >= 0)
  {
    (void)::fsync(fd);
    ::close(fd);
  }
#else
  (void)path;
#endif
}

}  // namespace

bool SymbolRegistry::saveToFile(const std::filesystem::path& path) const
{
  std::lock_guard lock(_mutex);

  // Binary, both here and in loadFromFile. In text mode Windows turns every
  // "\n" written into two bytes and turns them back on the way in, so the
  // size ftell reports and the count fread returns disagree and the load
  // rejects a file it wrote itself. Nothing on POSIX distinguishes the two
  // modes, so this costs nothing there.
  //
  // Written beside the target and renamed over it at the end, so a save that
  // fails part-way leaves the file that was there. The temporary's name is
  // this save's alone: two saves to one path each write a whole file and the
  // later rename wins.
  std::filesystem::path tmp;
  std::FILE* f = openUniqueTemp(path, tmp);
  if (!f)
  {
    return false;
  }

  std::fprintf(f, "{\n  \"version\": %u,\n  \"symbols\": [\n", kFileVersion);

  size_t count = 0;
  for (const SymbolInfo* entry : entriesById(_symbols))
  {
    const SymbolInfo& sym = *entry;
    std::fprintf(f,
                 "    {\"id\": %u, \"exchange\": %s, \"symbol\": %s, \"type\": %d, "
                 "\"tick_size\": %lld",
                 sym.id, jsonString(sym.exchange).c_str(), jsonString(sym.symbol).c_str(),
                 static_cast<int>(sym.type), static_cast<long long>(sym.tickSize.raw()));
    // Written only when set, so an entry with no declared tick reads exactly as
    // a file from before the flag was persisted.
    if (sym.tickSizeKnown)
    {
      std::fprintf(f, ", \"tick_size_known\": true");
    }
    // Likewise only when a lot is declared.
    if (sym.lotSize.raw() > 0)
    {
      std::fprintf(f, ", \"lot_size\": %lld", static_cast<long long>(sym.lotSize.raw()));
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
      std::fprintf(f, ", \"settlement_ccy\": %s", jsonString(*sym.settlementCcy).c_str());
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

  // A write that failed (a full disk) sets the stream's error flag, and the
  // bytes still buffered are written by the flush and the close: all of them
  // have to succeed before the file may replace the previous one. The sync
  // comes before the rename: renamed first, a crash could leave the new name
  // on a file whose bytes never reached the device.
  const bool flushed = std::fflush(f) == 0;
  const bool written = std::ferror(f) == 0;
  const bool synced = flushed && written && syncToDevice(f);
  const bool closed = std::fclose(f) == 0;
  std::error_code ec;
  if (synced && closed)
  {
    std::filesystem::rename(tmp, path, ec);
    if (!ec)
    {
      syncDirectoryOf(path);
      return true;
    }
  }
  std::filesystem::remove(tmp, ec);
  return false;
}

bool SymbolRegistry::loadFromFile(const std::filesystem::path& path)
{
  std::string why;
  return loadFromFile(path, why);
}

bool SymbolRegistry::loadFromFile(const std::filesystem::path& path, std::string& why)
{
  why.clear();
  std::FILE* f = std::fopen(path.string().c_str(), "rb");
  if (!f)
  {
    why = "cannot open " + path.string() + ": " + std::strerror(errno);
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
    why = path.string() + (readFailed ? ": the read failed" : ": the file is empty");
    return false;
  }

  // A file that does not parse leaves the registry empty, as a failed load
  // always has.
  const auto fail = [this, &why, &path](const std::string& reason)
  {
    why = path.string() + ": " + reason;
    std::lock_guard lock(_mutex);
    _venueTicks->reset();
    _symbols.clear();
    _map.clear();
    _reverse.clear();
    _nextSymbolId = 1;
    _symbolToExchange.clear();
    return false;
  };

  std::vector<RegistryJsonReader::Entry> entries;
  std::optional<std::string> version;
  std::string malformed;
  if (!RegistryJsonReader(content).document(entries, version, malformed))
  {
    return fail(malformed);
  }
  // A file with no "version" is read as the one version there has been. A
  // version this build does not write may mean fields it would misread.
  if (version)
  {
    unsigned long long v = 0;
    size_t used = 0;
    try
    {
      v = std::stoull(*version, &used);
    }
    catch (const std::exception&)
    {
      used = 0;
    }
    if (used != version->size() || version->front() == '-' || v == 0)
    {
      return fail("\"version\" is " + *version + ", not a positive integer");
    }
    if (v > kFileVersion)
    {
      return fail("the file is version " + *version + ", newer than the version " +
                  std::to_string(kFileVersion) + " this build reads and writes");
    }
  }

  // A file that is not the shape this expects is a false, not an exception:
  // the signature promises a bool, and a caller loading a registry at startup
  // should not have to guard a bool-returning call with a try. The numbers
  // below are the reason it can happen -- a nanosecond timestamp and a
  // fixed-point price do not fit the 32 bits `long` has on Windows, which is
  // why they are parsed as long long, and a truncated or foreign file can
  // still present something that does not convert.
  std::unordered_map<SymbolId, SymbolInfo> loaded;
  std::unordered_map<std::string, SymbolId> names;
  try
  {
    for (const auto& e : entries)
    {
      const auto field = [&e](const char* key) -> const std::string*
      {
        auto it = e.find(key);
        return it == e.end() ? nullptr : &it->second;
      };
      const std::string* id = field("id");
      const std::string* exchange = field("exchange");
      const std::string* symbol = field("symbol");
      if (id == nullptr || exchange == nullptr || symbol == nullptr)
      {
        // An entry without its id or names makes the file malformed. Ending
        // the list here instead dropped every entry after it.
        return fail("entry " + std::to_string(loaded.size()) +
                    " lacks its \"id\", \"exchange\" or \"symbol\"");
      }
      SymbolInfo info;
      const unsigned long long rawId = std::stoull(*id);
      if (rawId > kMaxSymbolId)
      {
        return fail("id " + *id + " of " + *exchange + ":" + *symbol + " is above the largest id " +
                    std::to_string(kMaxSymbolId));
      }
      info.id = static_cast<SymbolId>(rawId);
      info.exchange = *exchange;
      info.symbol = *symbol;
      if (const auto* v = field("type"))
      {
        info.type = static_cast<InstrumentType>(std::stoi(*v));
      }
      if (const auto* v = field("tick_size"))
      {
        info.tickSize = Price::fromRaw(std::stoll(*v));
      }
      // A file written before the flag was persisted has no key: its ticks
      // load as undeclared, and only a tick other than the 0.01 default is
      // checked, the same as before the load.
      if (const auto* v = field("tick_size_known"))
      {
        info.tickSizeKnown = *v == "true";
      }
      // Absent from a file written before lots were persisted, and from an
      // entry that declares none: the quantity is not checked.
      if (const auto* v = field("lot_size"))
      {
        info.lotSize = Quantity::fromRaw(std::stoll(*v));
      }
      if (const auto* v = field("strike"))
      {
        info.strike = Price::fromRaw(std::stoll(*v));
      }
      if (const auto* v = field("expiry"))
      {
        info.expiry = TimePoint(std::chrono::nanoseconds(std::stoll(*v)));
      }
      if (const auto* v = field("option_type"))
      {
        info.optionType = static_cast<OptionType>(std::stoi(*v));
      }
      // Contract spec (older files omit these; defaults already on `info`).
      if (const auto* v = field("contract_multiplier"))
      {
        info.contractMultiplier = std::stod(*v);
      }
      if (const auto* v = field("settlement_type"))
      {
        info.settlementType = static_cast<SettlementType>(std::stoi(*v));
      }
      if (const auto* v = field("exercise_style"))
      {
        info.exerciseStyle = static_cast<ExerciseStyle>(std::stoi(*v));
      }
      if (const auto* v = field("settlement_ccy"))
      {
        info.settlementCcy = *v;
      }
      // Per-symbol fixed-point scale. A file written before the scales were
      // persisted has neither key and loads the 1e8 default, as it always did.
      if (const auto* v = field("price_scale"))
      {
        info.priceScale = std::stoll(*v);
      }
      if (const auto* v = field("qty_scale"))
      {
        info.qtyScale = std::stoll(*v);
      }
      // A scale registerSymbol would refuse makes the file malformed.
      if (!validateSymbolScale(info))
      {
        throw std::invalid_argument("symbol registry file: invalid scale");
      }
      // Two entries with one id make the file malformed: the second would
      // replace the first while both names resolved to the id. So do two
      // entries with one name: the name would resolve to one of the ids and
      // the other entry could not be found by it.
      const SymbolId sid = info.id;
      const std::string name = info.exchange + ":" + info.symbol;
      if (const auto [it, fresh] = names.emplace(nameKey(info.exchange, info.symbol), sid);
          !fresh)
      {
        return fail("two entries are named " + name + " (ids " + std::to_string(it->second) +
                    " and " + std::to_string(sid) + ")");
      }
      if (!loaded.emplace(sid, std::move(info)).second)
      {
        return fail("two entries have the id " + std::to_string(sid));
      }
    }
  }
  catch (const std::exception& e)
  {
    return fail(std::string("an entry does not convert: ") + e.what());
  }

  std::lock_guard lock(_mutex);
  _venueTicks->reset();
  _symbols = std::move(loaded);
  _map = std::move(names);
  rebuildLoadedIndexes(_symbols, _exchangeNameToId, _reverse, _symbolToExchange,
                       _nextSymbolId);
  return true;
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
//            0x08 settlement_ccy, 0x10 tick_size_known, 0x20 lot_size)
//   [8 bytes] strike (if flag set)
//   [8 bytes] expiry (if flag set)
//   [1 byte] option_type (if flag set)
//   ... v3 contract spec, v4 scales ...
//   [8 bytes] lot_size (v5, if flag set)

static constexpr uint32_t kSymbolRegistryMagic = 0x47455253;  // "SREG"
// v3 adds the contract spec (multiplier / settlement_type / exercise_style /
// settlement_ccy). v2 files load with defaults for those.
// v4 adds per-symbol fixed-point scale (price_scale / qty_scale). Older files
// load with the default 1e8 scale, so they are byte-identical in behavior.
static constexpr uint32_t kSymbolRegistryVersion = 4;
// v5 adds the declared lot (lot_size), eight bytes at the end of an entry
// whose flags carry kFlagLotSize. It is written only when some entry declares
// a lot, so a registry without one serializes to the same v4 bytes as before
// and a build that predates lots still reads it; such a build refuses v5
// rather than misreading the extra bytes.
static constexpr uint32_t kSymbolRegistryVersionWithLots = 5;
static constexpr uint8_t kFlagLotSize = 0x20;
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

  bool anyLot = false;
  // order: not observable -- any declared lot decides the version
  for (const auto& [id, sym] : _symbols)
  {
    anyLot = anyLot || sym.lotSize.raw() > 0;
  }

  // Header
  write_u32(kSymbolRegistryMagic);
  write_u32(anyLot ? kSymbolRegistryVersionWithLots : kSymbolRegistryVersion);
  write_u32(static_cast<uint32_t>(_symbols.size()));

  // Symbols
  for (const SymbolInfo* entry : entriesById(_symbols))
  {
    const SymbolInfo& sym = *entry;
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
    if (sym.lotSize.raw() > 0)
    {
      flags |= kFlagLotSize;
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

    // v5 declared lot.
    if (sym.lotSize.raw() > 0)
    {
      write_i64(sym.lotSize.raw());
    }
  }

  return result;
}

bool SymbolRegistry::deserialize(std::span<const std::byte> data)
{
  std::string why;
  return deserialize(data, why);
}

bool SymbolRegistry::deserialize(std::span<const std::byte> data, std::string& why)
{
  why.clear();
  // A blob that does not parse leaves the registry empty, as a file that does
  // not parse does in loadFromFile.
  const auto fail = [this, &why](const std::string& reason)
  {
    why = reason;
    std::lock_guard lock(_mutex);
    _venueTicks->reset();
    _symbols.clear();
    _map.clear();
    _reverse.clear();
    _nextSymbolId = 1;
    _symbolToExchange.clear();
    return false;
  };

  if (data.size() < 12)
  {
    return fail("the data is shorter than the 12-byte header");
  }

  size_t pos = 0;
  // Set by a read that would run past the end. The read returns zero so the
  // caller needs no check of its own; the load is then refused.
  bool truncated = false;

  auto read_u32 = [&data, &pos, &truncated]() -> uint32_t
  {
    if (data.size() - pos < 4)
    {
      truncated = true;
      return 0;
    }
    uint32_t v = static_cast<uint32_t>(data[pos]) | (static_cast<uint32_t>(data[pos + 1]) << 8) |
                 (static_cast<uint32_t>(data[pos + 2]) << 16) |
                 (static_cast<uint32_t>(data[pos + 3]) << 24);
    pos += 4;
    return v;
  };

  auto read_u16 = [&data, &pos, &truncated]() -> uint16_t
  {
    if (data.size() - pos < 2)
    {
      truncated = true;
      return 0;
    }
    uint16_t v = static_cast<uint16_t>(data[pos]) | (static_cast<uint16_t>(data[pos + 1]) << 8);
    pos += 2;
    return v;
  };

  auto read_u8 = [&data, &pos, &truncated]() -> uint8_t
  {
    if (pos >= data.size())
    {
      truncated = true;
      return 0;
    }
    return static_cast<uint8_t>(data[pos++]);
  };

  auto read_i64 = [&data, &pos, &truncated]() -> int64_t
  {
    if (data.size() - pos < 8)
    {
      truncated = true;
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

  auto read_string = [&data, &pos, &truncated, &read_u16]() -> std::string
  {
    uint16_t len = read_u16();
    if (data.size() - pos < len)
    {
      truncated = true;
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
    return fail("the data does not start with the registry magic \"SREG\"");
  }

  uint32_t version = read_u32();
  if (version < 1 || version > kSymbolRegistryVersionWithLots)
  {
    return fail("the data is version " + std::to_string(version) +
                "; this build reads versions 1 to " +
                std::to_string(kSymbolRegistryVersionWithLots));
  }

  uint32_t count = read_u32();

  // Parsed into a local table first: the registry is replaced only by a blob
  // that read to the end of its last entry.
  std::unordered_map<SymbolId, SymbolInfo> loaded;
  std::unordered_map<std::string, SymbolId> names;
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

    // Before v5 the bit meant nothing and no bytes follow it.
    if (version >= kSymbolRegistryVersionWithLots && (flags & kFlagLotSize) != 0)
    {
      info.lotSize = Quantity::fromRaw(read_i64());
    }

    // A scale registerSymbol would refuse makes the blob malformed, as it
    // does a file.
    if (truncated)
    {
      return fail("the data ends inside entry " + std::to_string(i) + " of " +
                  std::to_string(count));
    }
    if (!validateSymbolScale(info))
    {
      return fail("entry " + std::to_string(i) + " has an invalid scale");
    }
    const SymbolId sid = info.id;
    if (sid > kMaxSymbolId)
    {
      return fail("id " + std::to_string(sid) + " of " + info.exchange + ":" + info.symbol +
                  " is above the largest id " + std::to_string(kMaxSymbolId));
    }
    // So do a name and an id that an earlier entry carries.
    const std::string name = info.exchange + ":" + info.symbol;
    if (const auto [it, fresh] = names.emplace(nameKey(info.exchange, info.symbol), sid); !fresh)
    {
      return fail("two entries are named " + name + " (ids " + std::to_string(it->second) +
                  " and " + std::to_string(sid) + ")");
    }
    if (!loaded.emplace(sid, std::move(info)).second)
    {
      return fail("two entries have the id " + std::to_string(sid));
    }
  }

  // The blob is its header and its entries. Bytes after the last entry mean
  // the count is wrong or the data is something else.
  if (pos != data.size())
  {
    return fail(std::to_string(data.size() - pos) + " byte(s) follow the last entry");
  }

  std::lock_guard lock(_mutex);
  _venueTicks->reset();
  _symbols = std::move(loaded);
  _map = std::move(names);
  rebuildLoadedIndexes(_symbols, _exchangeNameToId, _reverse, _symbolToExchange,
                       _nextSymbolId);

  return true;
}

}  // namespace lrvx
