/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2026 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "lrvx-connectors/execution/instrument_spec.h"
#include "lrvx-connectors/util/safe_parse.h"

#include <lrvx/log/log.h>

#include <curl/curl.h>
#include <simdjson.h>

#include <memory>
#include <mutex>

namespace lrvx
{

namespace
{

size_t appendBody(char* ptr, size_t size, size_t nmemb, void* userdata)
{
  static_cast<std::string*>(userdata)->append(ptr, size * nmemb);
  return size * nmemb;
}

// 10^-digits as a Price; 0 when the fixed-point scale cannot carry it.
Price powerOfTenTick(int64_t digits)
{
  if (digits < 0)
  {
    return Price{};
  }
  int64_t raw = Price::Scale;
  for (int64_t i = 0; i < digits; ++i)
  {
    if (raw % 10 != 0)
    {
      return Price{};
    }
    raw /= 10;
  }
  return Price::fromRaw(raw);
}

Quantity powerOfTenLot(int64_t digits) { return Quantity::fromRaw(powerOfTenTick(digits).raw()); }

std::string_view stringField(simdjson::dom::element el, std::string_view key)
{
  auto f = el[key];
  if (f.error())
  {
    return {};
  }
  auto s = f.get_string();
  return s.error() ? std::string_view{} : s.value_unsafe();
}

std::optional<int64_t> intFromStringField(simdjson::dom::element el, std::string_view key)
{
  const std::string_view s = stringField(el, key);
  if (s.empty())
  {
    return std::nullopt;
  }
  return util::parseInt64(s);
}

}  // namespace

InstrumentFetch defaultInstrumentFetch(int timeoutMs)
{
  return [timeoutMs](const std::string& url,
                     const std::string& body) -> std::expected<std::string, std::string>
  {
    static std::once_flag initOnce;
    std::call_once(initOnce,
                   []
                   {
                     curl_global_init(CURL_GLOBAL_ALL);
                   });

    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> h(curl_easy_init(), &curl_easy_cleanup);
    if (!h)
    {
      return std::unexpected<std::string>("curl_easy_init failed");
    }
    std::string response;
    curl_easy_setopt(h.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(h.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h.get(), CURLOPT_TIMEOUT_MS, static_cast<long>(timeoutMs));
    curl_easy_setopt(h.get(), CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(timeoutMs));
    curl_easy_setopt(h.get(), CURLOPT_WRITEFUNCTION, appendBody);
    curl_easy_setopt(h.get(), CURLOPT_WRITEDATA, &response);
    struct curl_slist* hdrs = nullptr;
    if (!body.empty())
    {
      hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
      curl_easy_setopt(h.get(), CURLOPT_HTTPHEADER, hdrs);
      curl_easy_setopt(h.get(), CURLOPT_POST, 1L);
      curl_easy_setopt(h.get(), CURLOPT_POSTFIELDS, body.c_str());
      curl_easy_setopt(h.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    }
    const CURLcode res = curl_easy_perform(h.get());
    long httpCode = 0;
    curl_easy_getinfo(h.get(), CURLINFO_RESPONSE_CODE, &httpCode);
    curl_slist_free_all(hdrs);
    if (res != CURLE_OK)
    {
      return std::unexpected<std::string>(std::string(curl_easy_strerror(res)));
    }
    // A venue refusing a symbol (Bitget answers 400, Polymarket 404) says why
    // in a JSON body; that body goes to the parser, which reports the venue's
    // own message. Only an answer with nothing in it is an error here.
    if ((httpCode < 200 || httpCode >= 300) && response.empty())
    {
      return std::unexpected<std::string>("HTTP " + std::to_string(httpCode) + ": " +
                                          response.substr(0, 512));
    }
    return response;
  };
}

namespace instrument_info
{

Price hyperliquidPerpTick(int szDecimals)
{
  constexpr int kPerpMaxDecimals = 6;
  if (szDecimals < 0 || szDecimals > kPerpMaxDecimals)
  {
    return Price{};
  }
  return powerOfTenTick(kPerpMaxDecimals - szDecimals);
}

std::expected<InstrumentSpecs, std::string> parseBybit(std::string_view body)
{
  simdjson::dom::parser parser;
  simdjson::padded_string padded(body);
  auto parsed = parser.parse(padded);
  if (parsed.error())
  {
    return std::unexpected<std::string>("unparseable instruments-info response");
  }
  auto doc = parsed.value();
  auto retCode = doc["retCode"].get_int64();
  if (retCode.error())
  {
    return std::unexpected<std::string>("instruments-info response has no retCode");
  }
  if (retCode.value_unsafe() != 0)
  {
    return std::unexpected<std::string>(
        "instruments-info retCode=" + std::to_string(retCode.value_unsafe()) +
        " retMsg=" + std::string(stringField(doc, "retMsg")));
  }
  auto list = doc["result"]["list"].get_array();
  if (list.error())
  {
    return std::unexpected<std::string>("instruments-info response has no result.list");
  }

  InstrumentSpecs out;
  for (auto item : list.value_unsafe())
  {
    const std::string_view symbol = stringField(item, "symbol");
    if (symbol.empty())
    {
      continue;
    }
    InstrumentSpec spec;
    auto priceFilter = item["priceFilter"];
    if (!priceFilter.error())
    {
      if (auto tick = util::parsePrice(stringField(priceFilter.value_unsafe(), "tickSize")))
      {
        spec.tickSize = *tick;
      }
    }
    auto lot = item["lotSizeFilter"];
    if (!lot.error())
    {
      std::string_view step = stringField(lot.value_unsafe(), "qtyStep");
      if (step.empty())
      {
        step = stringField(lot.value_unsafe(), "basePrecision");
      }
      if (auto q = util::parseQty(step))
      {
        spec.lotSize = *q;
      }
    }
    if (spec.tickSize.raw() <= 0)
    {
      return std::unexpected<std::string>("instruments-info entry " + std::string(symbol) +
                                          " has no usable priceFilter.tickSize");
    }
    out[std::string(symbol)] = spec;
  }
  return out;
}

namespace
{

// Bitget answers {"code":"00000","msg":"success","data":[...]}; anything else
// is the venue's refusal.
std::expected<simdjson::dom::array, std::string> bitgetData(simdjson::dom::parser& parser,
                                                            const simdjson::padded_string& padded)
{
  auto parsed = parser.parse(padded);
  if (parsed.error())
  {
    return std::unexpected<std::string>("unparseable symbol-info response");
  }
  auto doc = parsed.value();
  const std::string_view code = stringField(doc, "code");
  if (code != "00000")
  {
    return std::unexpected<std::string>("symbol-info code=" + std::string(code) +
                                        " msg=" + std::string(stringField(doc, "msg")));
  }
  auto data = doc["data"].get_array();
  if (data.error())
  {
    return std::unexpected<std::string>("symbol-info response has no data array");
  }
  return data.value_unsafe();
}

}  // namespace

std::expected<InstrumentSpecs, std::string> parseBitgetContracts(std::string_view body)
{
  simdjson::dom::parser parser;
  simdjson::padded_string padded(body);
  auto data = bitgetData(parser, padded);
  if (!data)
  {
    return std::unexpected(data.error());
  }

  InstrumentSpecs out;
  for (auto item : *data)
  {
    const std::string_view symbol = stringField(item, "symbol");
    if (symbol.empty())
    {
      continue;
    }
    const auto place = intFromStringField(item, "pricePlace");
    const auto endStep = intFromStringField(item, "priceEndStep");
    if (!place || !endStep || *endStep <= 0)
    {
      return std::unexpected<std::string>("contract " + std::string(symbol) +
                                          " has no usable pricePlace / priceEndStep");
    }
    const Price unit = powerOfTenTick(*place);
    if (unit.raw() <= 0)
    {
      return std::unexpected<std::string>("contract " + std::string(symbol) + " pricePlace " +
                                          std::to_string(*place) +
                                          " is finer than the fixed-point price scale");
    }
    InstrumentSpec spec;
    spec.tickSize = Price::fromRaw(unit.raw() * *endStep);
    if (auto lot = util::parseQty(stringField(item, "sizeMultiplier")))
    {
      spec.lotSize = *lot;
    }
    out[std::string(symbol)] = spec;
  }
  return out;
}

std::expected<InstrumentSpecs, std::string> parseBitgetSpot(std::string_view body)
{
  simdjson::dom::parser parser;
  simdjson::padded_string padded(body);
  auto data = bitgetData(parser, padded);
  if (!data)
  {
    return std::unexpected(data.error());
  }

  InstrumentSpecs out;
  for (auto item : *data)
  {
    const std::string_view symbol = stringField(item, "symbol");
    if (symbol.empty())
    {
      continue;
    }
    const auto pricePrecision = intFromStringField(item, "pricePrecision");
    const Price tick = pricePrecision ? powerOfTenTick(*pricePrecision) : Price{};
    if (tick.raw() <= 0)
    {
      return std::unexpected<std::string>("spot symbol " + std::string(symbol) +
                                          " has no usable pricePrecision");
    }
    InstrumentSpec spec;
    spec.tickSize = tick;
    if (const auto qp = intFromStringField(item, "quantityPrecision"))
    {
      spec.lotSize = powerOfTenLot(*qp);
    }
    out[std::string(symbol)] = spec;
  }
  return out;
}

std::expected<InstrumentSpecs, std::string> parseHyperliquidMeta(std::string_view body)
{
  simdjson::dom::parser parser;
  simdjson::padded_string padded(body);
  auto parsed = parser.parse(padded);
  if (parsed.error())
  {
    return std::unexpected<std::string>("unparseable meta response");
  }
  auto universe = parsed.value()["universe"].get_array();
  if (universe.error())
  {
    return std::unexpected<std::string>("meta response has no universe");
  }

  InstrumentSpecs out;
  for (auto coin : universe.value_unsafe())
  {
    const std::string_view name = stringField(coin, "name");
    auto szDecimals = coin["szDecimals"].get_int64();
    if (name.empty() || szDecimals.error())
    {
      continue;
    }
    const int sz = static_cast<int>(szDecimals.value_unsafe());
    InstrumentSpec spec;
    spec.tickSize = hyperliquidPerpTick(sz);
    spec.lotSize = powerOfTenLot(sz);
    if (spec.tickSize.raw() <= 0)
    {
      continue;
    }
    out[std::string(name)] = spec;
  }
  if (out.empty())
  {
    return std::unexpected<std::string>("meta response listed no coins");
  }
  return out;
}

std::expected<InstrumentSpec, std::string> parsePolymarketTickSize(std::string_view body)
{
  simdjson::dom::parser parser;
  simdjson::padded_string padded(body);
  auto parsed = parser.parse(padded);
  if (parsed.error())
  {
    return std::unexpected<std::string>("unparseable tick-size response");
  }
  auto doc = parsed.value();
  auto tickField = doc["minimum_tick_size"];
  if (tickField.error())
  {
    const std::string_view err = stringField(doc, "error");
    return std::unexpected<std::string>(err.empty() ? std::string("tick-size response has no "
                                                                  "minimum_tick_size")
                                                    : "tick-size: " + std::string(err));
  }
  // The text of the number, not its double: 0.001 must become exactly
  // 100000 raw.
  std::string text = simdjson::minify(tickField.value_unsafe());
  if (!text.empty() && text.front() == '"')
  {
    text = text.substr(1, text.size() >= 2 ? text.size() - 2 : 0);
  }
  auto tick = util::parsePrice(text);
  if (!tick || tick->raw() <= 0)
  {
    return std::unexpected<std::string>("tick-size minimum_tick_size " + text +
                                        " is not a positive decimal");
  }
  InstrumentSpec spec;
  spec.tickSize = *tick;
  return spec;
}

}  // namespace instrument_info

SymbolId registerVenueSymbol(SymbolRegistry& registry, SymbolInfo info, InstrumentSpecTable& specs,
                             ILogger* logger)
{
  const auto spec = specs.find(info.symbol);
  if (auto existing = registry.getSymbolId(info.exchange, info.symbol))
  {
    if (spec)
    {
      const auto report = [&](const std::string& msg)
      {
        if (logger)
        {
          logger->error(msg);
        }
        LRVX_LOG_ERROR(msg);
      };
      // One entry per symbol, so a tick and a lot that both disagree share it.
      std::string conflict;
      const auto r = registry.applyVenueTick(*existing, spec->tickSize);
      if (r.outcome == VenueTickOutcome::Conflict)
      {
        conflict = "the registry declares tick " + r.heldTick.toString() + ", the venue uses " +
                   spec->tickSize.toString() + "; the declared tick is kept";
        report("[" + info.exchange + "] " + info.symbol + " was registered with tick " +
               r.heldTick.toString() + " and the venue uses " + spec->tickSize.toString() +
               ". The declared tick is kept: orders on it are checked against " +
               r.heldTick.toString() + " and the venue may refuse prices that pass.");
      }
      if (spec->lotSize.raw() > 0)
      {
        const auto l = registry.applyVenueLot(*existing, spec->lotSize);
        if (l.outcome == VenueTickOutcome::Conflict)
        {
          if (!conflict.empty())
          {
            conflict += "; ";
          }
          conflict += "the registry declares lot " + l.heldLot.toString() + ", the venue uses " +
                      spec->lotSize.toString() + "; the declared lot is kept";
          report("[" + info.exchange + "] " + info.symbol + " was registered with lot " +
                 l.heldLot.toString() + " and the venue uses " + spec->lotSize.toString() +
                 ". The declared lot is kept: order quantities on it are checked against " +
                 l.heldLot.toString() + " and the venue may refuse quantities that pass.");
        }
      }
      if (conflict.empty())
      {
        specs.clearConflict(info.symbol);
      }
      else
      {
        specs.setConflict(info.symbol, conflict);
      }
    }
    return *existing;
  }
  // Registered first and given the venue's tick through applyVenueTick, as a
  // symbol registered earlier is: the registry then knows the tick is the
  // venue's, and lets the venue change it later. The lot likewise.
  const SymbolId id = registry.registerSymbol(info);
  if (spec)
  {
    registry.applyVenueTick(id, spec->tickSize);
    if (spec->lotSize.raw() > 0)
    {
      registry.applyVenueLot(id, spec->lotSize);
    }
  }
  return id;
}

void reportInstrumentLoad(const InstrumentSpecTable& specs, std::string_view venue, ILogger* logger)
{
  const std::string error = specs.error();
  if (error.empty())
  {
    return;
  }
  const std::string msg =
      "[" + std::string(venue) + "] instrument info not loaded: " + error +
      ". These symbols are registered with their tick unknown: order prices on them are sent "
      "as given, neither checked nor rounded.";
  if (logger)
  {
    logger->error(msg);
  }
  LRVX_LOG_ERROR(msg);
}

}  // namespace lrvx
