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
#include "lrvx/strategy/strategy.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace lrvx;

TEST(SymbolRegistryTest, RegisterAndGetSymbolId)
{
  SymbolRegistry registry;

  auto id1 = registry.registerSymbol("bybit", "BTCUSDT");
  auto id2 = registry.registerSymbol("binance", "ETHUSDT");
  auto id3 = registry.registerSymbol("bybit", "BTCUSDT");

  EXPECT_EQ(id1, id3);
  EXPECT_NE(id1, id2);

  auto maybeId = registry.getSymbolId("bybit", "BTCUSDT");
  ASSERT_TRUE(maybeId.has_value());
  EXPECT_EQ(maybeId.value(), id1);

  auto missingId = registry.getSymbolId("bybit", "DOGEUSDT");
  EXPECT_FALSE(missingId.has_value());
}

TEST(SymbolRegistryTest, GetSymbolName)
{
  SymbolRegistry registry;
  auto id = registry.registerSymbol("bybit", "BTCUSDT");

  auto [exchange, symbol] = registry.getSymbolName(id);
  EXPECT_EQ(exchange, "bybit");
  EXPECT_EQ(symbol, "BTCUSDT");
}

TEST(SymbolRegistryTest, StringRegistrationProducesSymbolInfo)
{
  SymbolRegistry registry;
  auto id = registry.registerSymbol("bybit", "BTCUSDT");

  // This returned nullopt: the name-based paths filled only the name maps,
  // and the Strategy constructor threw on any symbol registered through them.
  auto info = registry.getSymbolInfo(id);
  ASSERT_TRUE(info.has_value());
  EXPECT_EQ(info->id, id);
  EXPECT_EQ(info->exchange, "bybit");
  EXPECT_EQ(info->symbol, "BTCUSDT");
  EXPECT_EQ(info->exchangeId, InvalidExchangeId);      // "bybit" was never registered
  EXPECT_EQ(info->tickSize, Price::fromDouble(0.01));  // defaulted
}

TEST(SymbolRegistryTest, ExchangeIdRegistrationProducesSymbolInfo)
{
  SymbolRegistry registry;
  auto ex = registry.registerExchange("hyperliquid");
  auto id = registry.registerSymbol(ex, "BTC");

  auto info = registry.getSymbolInfo(id);
  ASSERT_TRUE(info.has_value());
  EXPECT_EQ(info->id, id);
  EXPECT_EQ(info->exchange, "hyperliquid");
  EXPECT_EQ(info->symbol, "BTC");
  EXPECT_EQ(info->exchangeId, ex);
}

TEST(SymbolRegistryTest, StringRegistrationResolvesAKnownExchange)
{
  SymbolRegistry registry;
  auto ex = registry.registerExchange("binance");
  auto id = registry.registerSymbol("binance", "ETHUSDT");

  auto info = registry.getSymbolInfo(id);
  ASSERT_TRUE(info.has_value());
  EXPECT_EQ(info->exchangeId, ex);
  EXPECT_EQ(registry.getExchangeForSymbol(id), ex);
}

TEST(SymbolRegistryTest, ReregistrationKeepsTheExistingInfo)
{
  SymbolRegistry registry;
  auto id1 = registry.registerSymbol("bybit", "BTCUSDT");
  auto id2 = registry.registerSymbol("bybit", "BTCUSDT");

  EXPECT_EQ(id1, id2);
  auto info = registry.getSymbolInfo(id1);
  ASSERT_TRUE(info.has_value());
  EXPECT_EQ(info->symbol, "BTCUSDT");
}

TEST(SymbolRegistryTest, ThreadSafety)
{
  SymbolRegistry registry;
  constexpr int threadCount = 8;
  constexpr int symbolsPerThread = 1000;

  std::vector<std::thread> threads;

  for (int t = 0; t < threadCount; ++t)
  {
    threads.emplace_back(
        [t, &registry]()
        {
          for (int i = 0; i < symbolsPerThread; ++i)
          {
            std::string exchange = "ex" + std::to_string(t % 3);
            std::string symbol = "SYM_" + std::to_string(t) + "_" + std::to_string(i);
            registry.registerSymbol(exchange, symbol);
          }
        });
  }

  for (auto& th : threads)
  {
    th.join();
  }

  int total = threadCount * symbolsPerThread;
  std::unordered_set<SymbolId> ids;
  for (int t = 0; t < threadCount; ++t)
  {
    for (int i = 0; i < symbolsPerThread; ++i)
    {
      std::string exchange = "ex" + std::to_string(t % 3);
      std::string symbol = "SYM_" + std::to_string(t) + "_" + std::to_string(i);
      auto maybeId = registry.getSymbolId(exchange, symbol);
      ASSERT_TRUE(maybeId.has_value());
      ids.insert(maybeId.value());
    }
  }

  EXPECT_EQ(ids.size(), total);
}

TEST(SymbolRegistryTest, StressTestMassiveSymbols)
{
  SymbolRegistry registry;
  constexpr int count = 100000;

  for (int i = 0; i < count; ++i)
  {
    std::string symbol = "S" + std::to_string(i);
    registry.registerSymbol("stress", symbol);
  }

  for (int i = 0; i < count; ++i)
  {
    std::string symbol = "S" + std::to_string(i);
    auto id = registry.getSymbolId("stress", symbol);
    ASSERT_TRUE(id.has_value());
    auto [ex, sym] = registry.getSymbolName(*id);
    EXPECT_EQ(ex, "stress");
    EXPECT_EQ(sym, symbol);
  }
}

TEST(SymbolRegistryTest, RegisterOptionAndFutureSymbols)
{
  SymbolRegistry registry;

  SymbolInfo option;
  option.exchange = "deribit";
  option.symbol = "BTC-30AUG24-50000-C";
  option.type = InstrumentType::Option;
  option.strike = Price::fromDouble(50000.0);
  option.optionType = OptionType::CALL;

  SymbolInfo future;
  future.exchange = "deribit";
  future.symbol = "BTC-30AUG24";
  future.type = InstrumentType::Future;

  auto optId = registry.registerSymbol(option);
  auto futId = registry.registerSymbol(future);

  EXPECT_NE(optId, futId);

  auto optInfo = registry.getSymbolInfo(optId);
  EXPECT_TRUE(optInfo.has_value());
  EXPECT_EQ(optInfo->type, InstrumentType::Option);
  EXPECT_EQ(optInfo->optionType.value(), OptionType::CALL);
  EXPECT_EQ(optInfo->strike.value(), Price::fromDouble(50000.0));

  auto futOptInfo = registry.getSymbolInfo(futId);
  EXPECT_TRUE(futOptInfo.has_value());
  EXPECT_EQ(futOptInfo->type, InstrumentType::Future);
}

TEST(SymbolRegistryTest, ContractSpecDefaultsAndRoundTrip)
{
  SymbolRegistry registry;

  // Plain perp (registered with a bare SymbolInfo) keeps the defaults.
  SymbolInfo perpInfo;
  perpInfo.exchange = "bybit";
  perpInfo.symbol = "BTCUSDT";
  perpInfo.type = InstrumentType::Future;
  auto perpId = registry.registerSymbol(perpInfo);
  auto perp = registry.getSymbolInfo(perpId);
  ASSERT_TRUE(perp.has_value());
  EXPECT_DOUBLE_EQ(perp->contractMultiplier, 1.0);
  EXPECT_EQ(perp->settlementType, SettlementType::Cash);
  EXPECT_EQ(perp->exerciseStyle, ExerciseStyle::European);
  EXPECT_FALSE(perp->settlementCcy.has_value());

  // Equity option overrides the spec.
  SymbolInfo opt;
  opt.exchange = "cboe";
  opt.symbol = "SPY-20240920-450-C";
  opt.type = InstrumentType::Option;
  opt.strike = Price::fromDouble(450.0);
  opt.optionType = OptionType::CALL;
  opt.contractMultiplier = 100.0;
  opt.settlementType = SettlementType::Physical;
  opt.exerciseStyle = ExerciseStyle::American;
  opt.settlementCcy = "USD";
  auto optId = registry.registerSymbol(opt);

  auto got = registry.getSymbolInfo(optId);
  ASSERT_TRUE(got.has_value());
  EXPECT_DOUBLE_EQ(got->contractMultiplier, 100.0);
  EXPECT_EQ(got->settlementType, SettlementType::Physical);
  EXPECT_EQ(got->exerciseStyle, ExerciseStyle::American);
  ASSERT_TRUE(got->settlementCcy.has_value());
  EXPECT_EQ(*got->settlementCcy, "USD");

  // Binary serialize / deserialize preserves the spec.
  auto bytes = registry.serialize();
  SymbolRegistry restored;
  ASSERT_TRUE(restored.deserialize(bytes));
  auto rOpt = restored.getSymbolInfo(optId);
  ASSERT_TRUE(rOpt.has_value());
  EXPECT_DOUBLE_EQ(rOpt->contractMultiplier, 100.0);
  EXPECT_EQ(rOpt->settlementType, SettlementType::Physical);
  EXPECT_EQ(rOpt->exerciseStyle, ExerciseStyle::American);
  ASSERT_TRUE(rOpt->settlementCcy.has_value());
  EXPECT_EQ(*rOpt->settlementCcy, "USD");

  // JSON file save / load preserves the spec.
  auto path = std::filesystem::temp_directory_path() / "test_contract_spec.json";
  std::filesystem::remove(path);
  ASSERT_TRUE(registry.saveToFile(path));
  SymbolRegistry fromFile;
  ASSERT_TRUE(fromFile.loadFromFile(path));
  auto fOpt = fromFile.getSymbolInfo(optId);
  ASSERT_TRUE(fOpt.has_value());
  EXPECT_DOUBLE_EQ(fOpt->contractMultiplier, 100.0);
  EXPECT_EQ(fOpt->settlementType, SettlementType::Physical);
  EXPECT_EQ(fOpt->exerciseStyle, ExerciseStyle::American);
  ASSERT_TRUE(fOpt->settlementCcy.has_value());
  EXPECT_EQ(*fOpt->settlementCcy, "USD");
  std::filesystem::remove(path);
}

TEST(SymbolRegistryTest, SaveAndLoadFromFile)
{
  std::filesystem::path tempPath = std::filesystem::temp_directory_path() / "test_registry.json";

  // Cleanup before test
  std::filesystem::remove(tempPath);

  {
    SymbolRegistry registry;

    // Register simple symbols using SymbolInfo
    SymbolInfo btc;
    btc.exchange = "binance";
    btc.symbol = "BTCUSDT";
    btc.type = InstrumentType::Spot;
    registry.registerSymbol(btc);

    SymbolInfo eth;
    eth.exchange = "binance";
    eth.symbol = "ETHUSDT";
    eth.type = InstrumentType::Spot;
    registry.registerSymbol(eth);

    // Register option with all fields
    SymbolInfo option;
    option.exchange = "deribit";
    option.symbol = "BTC-30AUG24-50000-C";
    option.type = InstrumentType::Option;
    option.strike = Price::fromDouble(50000.0);
    option.expiry = TimePoint(std::chrono::nanoseconds(1724976000000000000LL));  // Aug 30, 2024
    option.optionType = OptionType::CALL;
    registry.registerSymbol(option);

    // Register future
    SymbolInfo future;
    future.exchange = "deribit";
    future.symbol = "BTC-30AUG24";
    future.type = InstrumentType::Future;
    future.expiry = TimePoint(std::chrono::nanoseconds(1724976000000000000LL));
    registry.registerSymbol(future);

    EXPECT_EQ(registry.size(), 4);
    EXPECT_TRUE(registry.saveToFile(tempPath));
  }

  // Verify file exists
  EXPECT_TRUE(std::filesystem::exists(tempPath));

  {
    SymbolRegistry registry;
    EXPECT_TRUE(registry.loadFromFile(tempPath));
    EXPECT_EQ(registry.size(), 4);

    // Verify symbols loaded correctly
    auto symbols = registry.getAllSymbols();
    EXPECT_EQ(symbols.size(), 4);

    // Check option was loaded with all fields
    bool foundOption = false;
    for (const auto& sym : symbols)
    {
      if (sym.symbol == "BTC-30AUG24-50000-C")
      {
        foundOption = true;
        EXPECT_EQ(sym.exchange, "deribit");
        EXPECT_EQ(sym.type, InstrumentType::Option);
        EXPECT_TRUE(sym.strike.has_value());
        EXPECT_EQ(sym.strike->raw(), Price::fromDouble(50000.0).raw());
        EXPECT_TRUE(sym.expiry.has_value());
        EXPECT_TRUE(sym.optionType.has_value());
        EXPECT_EQ(*sym.optionType, OptionType::CALL);
      }
    }
    EXPECT_TRUE(foundOption);
  }

  // Cleanup
  std::filesystem::remove(tempPath);
}

namespace
{
// Every SymbolInfo field a round trip can lose. exchangeId is not here: it is
// resolved from the loading registry's exchange table, not from the data, in
// both formats.
void expectSameSymbol(const SymbolInfo& got, const SymbolInfo& want)
{
  EXPECT_EQ(got.id, want.id);
  EXPECT_EQ(got.exchange, want.exchange);
  EXPECT_EQ(got.symbol, want.symbol);
  EXPECT_EQ(got.type, want.type);
  EXPECT_EQ(got.tickSize.raw(), want.tickSize.raw());
  EXPECT_EQ(got.tickSizeKnown, want.tickSizeKnown);
  EXPECT_EQ(got.strike.has_value(), want.strike.has_value());
  if (got.strike && want.strike)
  {
    EXPECT_EQ(got.strike->raw(), want.strike->raw());
  }
  EXPECT_EQ(got.expiry, want.expiry);
  EXPECT_EQ(got.optionType, want.optionType);
  EXPECT_EQ(got.contractMultiplier, want.contractMultiplier);
  EXPECT_EQ(got.settlementType, want.settlementType);
  EXPECT_EQ(got.exerciseStyle, want.exerciseStyle);
  EXPECT_EQ(got.settlementCcy, want.settlementCcy);
  EXPECT_EQ(got.priceScale, want.priceScale);
  EXPECT_EQ(got.qtyScale, want.qtyScale);
}

std::string readAll(const std::filesystem::path& p)
{
  std::string s;
  if (std::FILE* f = std::fopen(p.string().c_str(), "rb"))
  {
    char buf[4096];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;)
    {
      s.append(buf, n);
    }
    std::fclose(f);
  }
  return s;
}

void writeAll(const std::filesystem::path& p, const std::string& s)
{
  std::FILE* f = std::fopen(p.string().c_str(), "wb");
  ASSERT_NE(f, nullptr);
  std::fwrite(s.data(), 1, s.size(), f);
  std::fclose(f);
}
}  // namespace

// The JSON file keeps every field the binary form keeps: a DEX token's
// per-symbol scale used to come back as the 1e8 default, and a contract
// multiplier with more than ten significant digits came back rounded.
TEST(SymbolRegistryTest, JsonRoundTripKeepsWhatBinaryKeeps)
{
  SymbolInfo dex;
  dex.exchange = "uniswap";
  dex.symbol = "PEPEWETH";
  dex.type = InstrumentType::Option;
  dex.tickSize = Price::fromRaw(7);
  dex.tickSizeKnown = true;
  dex.strike = Price::fromDouble(1.25);
  dex.expiry = TimePoint(std::chrono::nanoseconds(1724976000000000000LL));
  dex.optionType = OptionType::PUT;
  dex.contractMultiplier = 1.0 / 3.0;
  dex.settlementType = SettlementType::Physical;
  dex.exerciseStyle = ExerciseStyle::American;
  dex.settlementCcy = "WETH";
  dex.priceScale = 1'000'000'000'000'000;
  dex.qtyScale = 10'000;

  SymbolInfo plain;
  plain.exchange = "binance";
  plain.symbol = "BTCUSDT";
  plain.qtyScale = 1'000'000;

  SymbolRegistry registry;
  dex.id = registry.registerSymbol(dex);
  plain.id = registry.registerSymbol(plain);
  ASSERT_NE(dex.id, 0u);
  ASSERT_NE(plain.id, 0u);

  SymbolRegistry fromBinary;
  ASSERT_TRUE(fromBinary.deserialize(registry.serialize()));

  const auto path = std::filesystem::temp_directory_path() / "test_registry_scales.json";
  std::filesystem::remove(path);
  ASSERT_TRUE(registry.saveToFile(path));
  SymbolRegistry fromJson;
  ASSERT_TRUE(fromJson.loadFromFile(path));
  std::filesystem::remove(path);

  for (const SymbolInfo* want : {&dex, &plain})
  {
    SCOPED_TRACE(want->symbol);
    auto b = fromBinary.getSymbolInfo(want->id);
    auto j = fromJson.getSymbolInfo(want->id);
    ASSERT_TRUE(b.has_value());
    ASSERT_TRUE(j.has_value());
    expectSameSymbol(*b, *want);
    expectSameSymbol(*j, *want);
  }
}

// A file written before the scales were persisted carries neither key and
// loads every symbol at the 1e8 default, as before. Saving it again writes the
// same bytes: an entry at the default scale gains no key.
TEST(SymbolRegistryTest, JsonWithoutScalesLoadsAsBefore)
{
  const std::string old =
      "{\n  \"version\": 1,\n  \"symbols\": [\n"
      "    {\"id\": 1, \"exchange\": \"binance\", \"symbol\": \"BTCUSDT\", \"type\": 0, "
      "\"tick_size\": 1000000, \"contract_multiplier\": 1, \"settlement_type\": 0, "
      "\"exercise_style\": 0},\n"
      "    {\"id\": 2, \"exchange\": \"cboe\", \"symbol\": \"SPY\", \"type\": 2, "
      "\"tick_size\": 5000000, \"tick_size_known\": true, \"strike\": 45000000000, "
      "\"option_type\": 0, \"contract_multiplier\": 100, \"settlement_type\": 1, "
      "\"exercise_style\": 1, \"settlement_ccy\": \"USD\"}\n"
      "  ]\n}\n";
  const auto path = std::filesystem::temp_directory_path() / "test_registry_old_shape.json";
  writeAll(path, old);

  SymbolRegistry registry;
  ASSERT_TRUE(registry.loadFromFile(path));
  for (SymbolId id : {1u, 2u})
  {
    auto info = registry.getSymbolInfo(id);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->priceScale, Price::Scale);
    EXPECT_EQ(info->qtyScale, Quantity::Scale);
  }
  EXPECT_DOUBLE_EQ(registry.getSymbolInfo(2)->contractMultiplier, 100.0);

  // The registry writes entries in its own map order, so compare the entries
  // without their trailing separators, as a set of lines.
  const auto lines = [](const std::string& s)
  {
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t nl; (nl = s.find('\n', start)) != std::string::npos; start = nl + 1)
    {
      std::string line = s.substr(start, nl - start);
      if (!line.empty() && line.back() == ',')
      {
        line.pop_back();
      }
      out.push_back(line);
    }
    std::sort(out.begin(), out.end());
    return out;
  };
  ASSERT_TRUE(registry.saveToFile(path));
  EXPECT_EQ(lines(readAll(path)), lines(old));
  std::filesystem::remove(path);
}

// A scale registerSymbol would refuse is a malformed file, not a symbol.
TEST(SymbolRegistryTest, JsonWithInvalidScaleIsRefused)
{
  const auto path = std::filesystem::temp_directory_path() / "test_registry_bad_scale.json";
  for (const char* bad : {"\"price_scale\": 0", "\"qty_scale\": -5"})
  {
    SCOPED_TRACE(bad);
    writeAll(path, std::string("{\n  \"version\": 1,\n  \"symbols\": [\n"
                               "    {\"id\": 1, \"exchange\": \"x\", \"symbol\": \"Y\", "
                               "\"type\": 0, \"tick_size\": 1000000, ") +
                       bad + "}\n  ]\n}\n");
    SymbolRegistry registry;
    EXPECT_FALSE(registry.loadFromFile(path));
    EXPECT_EQ(registry.size(), 0u);
  }
  std::filesystem::remove(path);
}

TEST(SymbolRegistryTest, BinarySerializeDeserialize)
{
  SymbolRegistry registry;

  // Register various symbols using SymbolInfo
  SymbolInfo btc;
  btc.exchange = "binance";
  btc.symbol = "BTCUSDT";
  btc.type = InstrumentType::Spot;
  registry.registerSymbol(btc);

  SymbolInfo eth;
  eth.exchange = "bybit";
  eth.symbol = "ETHUSDT";
  eth.type = InstrumentType::Spot;
  registry.registerSymbol(eth);

  SymbolInfo option;
  option.exchange = "deribit";
  option.symbol = "ETH-PUT-2000";
  option.type = InstrumentType::Option;
  option.strike = Price::fromDouble(2000.0);
  option.expiry = TimePoint(std::chrono::nanoseconds(1735689600000000000LL));  // Jan 1, 2025
  option.optionType = OptionType::PUT;
  registry.registerSymbol(option);

  EXPECT_EQ(registry.size(), 3);

  // Serialize
  auto data = registry.serialize();
  EXPECT_GT(data.size(), 12);  // At least header size

  // Deserialize into new registry
  SymbolRegistry registry2;
  EXPECT_TRUE(registry2.deserialize(data));
  EXPECT_EQ(registry2.size(), 3);

  // Verify content
  auto symbols = registry2.getAllSymbols();
  EXPECT_EQ(symbols.size(), 3);

  // Check option preserved correctly
  bool foundOption = false;
  for (const auto& sym : symbols)
  {
    if (sym.symbol == "ETH-PUT-2000")
    {
      foundOption = true;
      EXPECT_EQ(sym.type, InstrumentType::Option);
      EXPECT_TRUE(sym.strike.has_value());
      EXPECT_EQ(sym.strike->raw(), Price::fromDouble(2000.0).raw());
      EXPECT_TRUE(sym.optionType.has_value());
      EXPECT_EQ(*sym.optionType, OptionType::PUT);
    }
  }
  EXPECT_TRUE(foundOption);
}

TEST(SymbolRegistryTest, ClearRegistry)
{
  SymbolRegistry registry;

  SymbolInfo btc;
  btc.exchange = "binance";
  btc.symbol = "BTCUSDT";
  btc.type = InstrumentType::Spot;
  registry.registerSymbol(btc);

  SymbolInfo eth;
  eth.exchange = "binance";
  eth.symbol = "ETHUSDT";
  eth.type = InstrumentType::Spot;
  registry.registerSymbol(eth);

  EXPECT_EQ(registry.size(), 2);

  registry.clear();
  EXPECT_EQ(registry.size(), 0);
  EXPECT_TRUE(registry.getAllSymbols().empty());
}

TEST(SymbolRegistryTest, EmptyRegistrySerialization)
{
  SymbolRegistry registry;
  EXPECT_EQ(registry.size(), 0);

  auto data = registry.serialize();
  EXPECT_EQ(data.size(), 12);  // Only header: magic(4) + version(4) + count(4)

  SymbolRegistry registry2;
  SymbolInfo test;
  test.exchange = "test";
  test.symbol = "TEST";
  test.type = InstrumentType::Spot;
  registry2.registerSymbol(test);
  EXPECT_EQ(registry2.size(), 1);

  EXPECT_TRUE(registry2.deserialize(data));
  EXPECT_EQ(registry2.size(), 0);  // Should be empty after deserialize
}

TEST(SymbolRegistryTest, LargeRegistrySerialization)
{
  SymbolRegistry registry;
  constexpr int count = 10000;

  for (int i = 0; i < count; ++i)
  {
    SymbolInfo info;
    info.exchange = "exchange" + std::to_string(i % 10);
    info.symbol = "SYMBOL" + std::to_string(i);
    info.type = static_cast<InstrumentType>(i % 4);
    if (i % 3 == 0)
    {
      info.strike = Price::fromDouble(100.0 * i);
    }
    registry.registerSymbol(info);
  }

  EXPECT_EQ(registry.size(), count);

  auto data = registry.serialize();
  EXPECT_GT(data.size(), 100000);  // Should be substantial

  SymbolRegistry registry2;
  EXPECT_TRUE(registry2.deserialize(data));
  EXPECT_EQ(registry2.size(), count);

  // Spot check some symbols using getSymbolInfo (order-independent)
  EXPECT_EQ(registry2.getAllSymbols().size(), count);

  for (int i = 0; i < count; i += 1000)
  {
    std::string exchange = "exchange" + std::to_string(i % 10);
    std::string symbol = "SYMBOL" + std::to_string(i);
    auto id = registry2.getSymbolId(exchange, symbol);
    EXPECT_TRUE(id.has_value());
    auto info = registry2.getSymbolInfo(*id);
    EXPECT_TRUE(info.has_value());
    EXPECT_EQ(info->symbol, symbol);
    EXPECT_EQ(info->type, static_cast<InstrumentType>(i % 4));
  }
}

// The multi-threaded test above runs eight writers and no readers, so the
// getters were never exercised against a concurrent writer at all. These two
// put a reader next to a writer, which is the shape the engine actually runs:
// registration happens on a control path while routing reads the registry.

TEST(SymbolRegistryTest, ConcurrentReadersSeeFullyPopulatedExchanges)
{
  SymbolRegistry registry;

  std::atomic<bool> stop{false};
  std::atomic<bool> readerRunning{false};
  std::atomic<int> reads{0};
  std::atomic<int> halfFilled{0};

  std::thread reader(
      [&]
      {
        readerRunning.store(true, std::memory_order_release);
        while (!stop.load(std::memory_order_relaxed))
        {
          for (ExchangeId id = 0; id < 16; ++id)
          {
            const auto info = registry.getExchange(id);
            if (!info.has_value())
            {
              continue;
            }
            reads.fetch_add(1, std::memory_order_relaxed);
            // An id the registry admits must carry both fields already.
            if (info->nameView().empty() || info->type != VenueType::AmmDex)
            {
              halfFilled.fetch_add(1, std::memory_order_relaxed);
            }
          }
        }
      });

  while (!readerRunning.load(std::memory_order_acquire))
  {
    std::this_thread::yield();
  }

  // Driven by what the reader has actually seen rather than by a fixed round
  // count. A fixed count races thread start-up: the writer finishes in well
  // under a millisecond and can leave the reader nothing to look at, which
  // turns the read counter below into a coin flip.
  constexpr int kMinReads = 1000;
  constexpr int kMaxRounds = 200000;
  int rounds = 0;
  while (rounds < kMaxRounds && reads.load(std::memory_order_relaxed) < kMinReads)
  {
    for (int i = 0; i < 16; ++i)
    {
      registry.registerExchange("venue-" + std::to_string(i), VenueType::AmmDex);
    }
    registry.clear();
    ++rounds;
  }

  stop.store(true, std::memory_order_relaxed);
  reader.join();

  EXPECT_GE(reads.load(), kMinReads);
  EXPECT_EQ(halfFilled.load(), 0);
}

TEST(SymbolRegistryTest, ConcurrentReadersSurviveSymbolTableGrowth)
{
  SymbolRegistry registry;
  const ExchangeId venue = registry.registerExchange("growth-venue", VenueType::AmmDex);
  ASSERT_NE(venue, InvalidExchangeId);

  std::atomic<bool> stop{false};
  std::atomic<bool> readerRunning{false};
  std::atomic<int> reads{0};
  std::atomic<int> wrong{0};

  std::thread reader(
      [&]
      {
        readerRunning.store(true, std::memory_order_release);
        while (!stop.load(std::memory_order_relaxed))
        {
          for (SymbolId sym = 0; sym < 2000; ++sym)
          {
            const ExchangeId got = registry.getExchangeForSymbol(sym);
            reads.fetch_add(1, std::memory_order_relaxed);
            if (got != venue && got != InvalidExchangeId)
            {
              wrong.fetch_add(1, std::memory_order_relaxed);
            }
            if (registry.venueTypeForSymbol(sym) != VenueType::AmmDex &&
                got != InvalidExchangeId)
            {
              wrong.fetch_add(1, std::memory_order_relaxed);
            }
          }
        }
      });

  while (!readerRunning.load(std::memory_order_acquire))
  {
    std::this_thread::yield();
  }

  // Same reason as above: keep growing the table until the reader has done
  // real work, so the read counter is not a race against thread start-up.
  constexpr int kMinReads = 1000;
  int next = 0;
  while (next < 200000 && reads.load(std::memory_order_relaxed) < kMinReads)
  {
    for (int i = 0; i < 2000; ++i)
    {
      registry.registerSymbol(venue, "SYM" + std::to_string(next + i));
    }
    next += 2000;
  }

  stop.store(true, std::memory_order_relaxed);
  reader.join();

  EXPECT_GE(reads.load(), kMinReads);
  EXPECT_EQ(wrong.load(), 0);
}

// ------------------------------------------------------------------ venue tick

namespace
{

SymbolInfo undeclared(const std::string& exchange, const std::string& symbol)
{
  SymbolInfo info;
  info.exchange = exchange;
  info.symbol = symbol;
  info.type = InstrumentType::Future;
  return info;
}

}  // namespace

// A connector that starts after the symbol was registered writes the venue's
// tick into the entry it finds. Nothing else about the entry moves: the id,
// names, type and scales are what consumers index by.
TEST(SymbolRegistryVenueTick, AnUndeclaredTickTakesTheVenues)
{
  SymbolRegistry registry;
  registry.registerExchange("bybit");
  SymbolInfo info = undeclared("bybit", "BTCUSDT");
  info.priceScale = 1'000'000;
  info.contractMultiplier = 2.0;
  const SymbolId id = registry.registerSymbol(info);
  const auto before = *registry.getSymbolInfo(id);
  const uint64_t rev = registry.venueTickLog()->revision();

  const auto r = registry.applyVenueTick(id, Price::fromDouble(0.1));
  EXPECT_EQ(r.outcome, VenueTickOutcome::Applied);
  EXPECT_EQ(r.heldTick.raw(), Price::fromDouble(0.1).raw());

  const auto after = *registry.getSymbolInfo(id);
  EXPECT_TRUE(after.tickSizeKnown);
  EXPECT_EQ(after.tickSize.raw(), Price::fromDouble(0.1).raw());
  EXPECT_EQ(orderTickRaw(after), Price::fromDouble(0.1).raw());
  EXPECT_EQ(after.id, before.id);
  EXPECT_EQ(after.exchange, before.exchange);
  EXPECT_EQ(after.symbol, before.symbol);
  EXPECT_EQ(after.type, before.type);
  EXPECT_EQ(after.exchangeId, before.exchangeId);
  EXPECT_EQ(after.priceScale, before.priceScale);
  EXPECT_EQ(after.qtyScale, before.qtyScale);
  EXPECT_EQ(after.contractMultiplier, before.contractMultiplier);
  EXPECT_EQ(*registry.getSymbolId("bybit", "BTCUSDT"), id);
  EXPECT_EQ(registry.getExchangeForSymbol(id), before.exchangeId);

  EXPECT_GT(registry.venueTickLog()->revision(), rev);
  ASSERT_TRUE(registry.venueTickLog()->tickFor(id));
  EXPECT_EQ(registry.venueTickLog()->tickFor(id)->raw(), Price::fromDouble(0.1).raw());
}

// A tick the caller declared is the caller's. The same value is a no-op; a
// different one is kept and reported back, never overwritten.
TEST(SymbolRegistryVenueTick, ADeclaredTickIsKept)
{
  SymbolRegistry registry;
  SymbolInfo same = undeclared("bybit", "BTCUSDT");
  same.tickSize = Price::fromDouble(0.1);
  same.tickSizeKnown = true;
  const SymbolId sameId = registry.registerSymbol(same);

  SymbolInfo other = undeclared("bybit", "ETHUSDT");
  other.tickSize = Price::fromDouble(0.05);  // declared by value, flag unset
  const SymbolId otherId = registry.registerSymbol(other);

  SymbolInfo flagged = undeclared("bybit", "SOLUSDT");
  flagged.tickSize = kDefaultTickSize;  // an explicit 0.01, declared by the flag
  flagged.tickSizeKnown = true;
  const SymbolId flaggedId = registry.registerSymbol(flagged);

  const uint64_t rev = registry.venueTickLog()->revision();

  auto r = registry.applyVenueTick(sameId, Price::fromDouble(0.1));
  EXPECT_EQ(r.outcome, VenueTickOutcome::Unchanged);
  r = registry.applyVenueTick(otherId, Price::fromDouble(0.01));
  EXPECT_EQ(r.outcome, VenueTickOutcome::Conflict);
  EXPECT_EQ(r.heldTick.raw(), Price::fromDouble(0.05).raw());
  r = registry.applyVenueTick(flaggedId, Price::fromDouble(0.001));
  EXPECT_EQ(r.outcome, VenueTickOutcome::Conflict);
  EXPECT_EQ(r.heldTick.raw(), kDefaultTickSize.raw());

  EXPECT_EQ(registry.getSymbolInfo(otherId)->tickSize.raw(), Price::fromDouble(0.05).raw());
  EXPECT_EQ(registry.getSymbolInfo(flaggedId)->tickSize.raw(), kDefaultTickSize.raw());
  EXPECT_EQ(registry.venueTickLog()->revision(), rev) << "nothing changed, nothing to follow";
}

// A venue moves a market's tick while it trades. A tick the venue applied is
// not a declaration: the venue's next one replaces it, and each change is
// there for a strategy to follow.
TEST(SymbolRegistryVenueTick, AnAppliedTickFollowsTheVenueAgain)
{
  SymbolRegistry registry;
  const SymbolId id = registry.registerSymbol(undeclared("polymarket", "TOKEN"));
  ASSERT_EQ(registry.applyVenueTick(id, Price::fromDouble(0.01)).outcome,
            VenueTickOutcome::Applied);
  const uint64_t rev = registry.venueTickLog()->revision();

  auto r = registry.applyVenueTick(id, Price::fromDouble(0.001));
  EXPECT_EQ(r.outcome, VenueTickOutcome::Applied);
  EXPECT_EQ(r.heldTick.raw(), Price::fromDouble(0.001).raw());
  EXPECT_TRUE(registry.getSymbolInfo(id)->tickSizeKnown);
  EXPECT_EQ(orderTickRaw(*registry.getSymbolInfo(id)), Price::fromDouble(0.001).raw());
  EXPECT_GT(registry.venueTickLog()->revision(), rev);
  EXPECT_EQ(registry.venueTickLog()->tickFor(id)->raw(), Price::fromDouble(0.001).raw());

  r = registry.applyVenueTick(id, Price::fromDouble(0.001));
  EXPECT_EQ(r.outcome, VenueTickOutcome::Unchanged);
  r = registry.applyVenueTick(id, Price::fromDouble(0.01));
  EXPECT_EQ(r.outcome, VenueTickOutcome::Applied);
  EXPECT_EQ(registry.getSymbolInfo(id)->tickSize.raw(), Price::fromDouble(0.01).raw());
}

// A declared tick that happens to equal the venue's first tick is still the
// caller's when the venue moves on.
TEST(SymbolRegistryVenueTick, ADeclaredTickEqualToTheVenuesStaysDeclared)
{
  SymbolRegistry registry;
  SymbolInfo info = undeclared("polymarket", "TOKEN");
  info.tickSize = Price::fromDouble(0.01);
  info.tickSizeKnown = true;
  const SymbolId id = registry.registerSymbol(info);

  ASSERT_EQ(registry.applyVenueTick(id, Price::fromDouble(0.01)).outcome,
            VenueTickOutcome::Unchanged);
  const auto r = registry.applyVenueTick(id, Price::fromDouble(0.001));
  EXPECT_EQ(r.outcome, VenueTickOutcome::Conflict);
  EXPECT_EQ(r.heldTick.raw(), Price::fromDouble(0.01).raw());
  EXPECT_EQ(registry.getSymbolInfo(id)->tickSize.raw(), Price::fromDouble(0.01).raw());
}

TEST(SymbolRegistryVenueTick, UnknownIdsAndNonPositiveTicksAreRefused)
{
  SymbolRegistry registry;
  const SymbolId id = registry.registerSymbol(undeclared("bybit", "BTCUSDT"));
  EXPECT_EQ(registry.applyVenueTick(id + 100, Price::fromDouble(0.1)).outcome,
            VenueTickOutcome::UnknownSymbol);
  EXPECT_EQ(registry.applyVenueTick(id, Price{}).outcome, VenueTickOutcome::InvalidTick);
  EXPECT_FALSE(registry.getSymbolInfo(id)->tickSizeKnown);
}

// The log is shared, so a holder that outlives the registry reads no freed
// memory.
TEST(SymbolRegistryVenueTick, TheLogOutlivesTheRegistry)
{
  std::shared_ptr<const VenueTickLog> log;
  SymbolId id = 0;
  {
    SymbolRegistry registry;
    id = registry.registerSymbol(undeclared("bybit", "BTCUSDT"));
    log = registry.venueTickLog();
    registry.applyVenueTick(id, Price::fromDouble(0.5));
  }
  ASSERT_TRUE(log->tickFor(id));
  EXPECT_EQ(log->tickFor(id)->raw(), Price::fromDouble(0.5).raw());
}

// Readers on other threads see the old entry or the updated one, never half
// of each.
TEST(SymbolRegistryVenueTick, ConcurrentReadersSeeAWholeEntry)
{
  SymbolRegistry registry;
  std::vector<SymbolId> ids;
  for (int i = 0; i < 64; ++i)
  {
    ids.push_back(registry.registerSymbol(undeclared("bybit", "S" + std::to_string(i))));
  }
  std::atomic<bool> stop{false};
  std::atomic<int> wrong{0};
  std::thread reader(
      [&]
      {
        while (!stop.load(std::memory_order_relaxed))
        {
          for (SymbolId id : ids)
          {
            const auto info = registry.getSymbolInfo(id);
            const bool old = !info->tickSizeKnown && info->tickSize == kDefaultTickSize;
            const bool updated =
                info->tickSizeKnown && info->tickSize.raw() == Price::fromDouble(0.5).raw();
            if (!old && !updated)
            {
              wrong.fetch_add(1);
            }
          }
        }
      });
  for (SymbolId id : ids)
  {
    registry.applyVenueTick(id, Price::fromDouble(0.5));
  }
  stop.store(true);
  reader.join();
  EXPECT_EQ(wrong.load(), 0);
}

namespace
{

class TickReader final : public Strategy
{
 public:
  TickReader(std::vector<SymbolId> syms, const SymbolRegistry& registry)
      : Strategy(3, std::move(syms), registry)
  {
  }
  Price bookTick(SymbolId sym) const { return ctx(sym).book.tickSize(); }
  std::optional<Price> tickInTrade;

 protected:
  void onSymbolTrade(SymbolContext& c, const TradeEvent&) override
  {
    tickInTrade = c.book.tickSize();
  }
};

void trade(TickReader& s, SymbolId sym)
{
  TradeEvent ev{};
  ev.trade.symbol = sym;
  ev.trade.price = Price::fromDouble(100);
  ev.trade.quantity = Quantity::fromDouble(1);
  s.onTrade(ev);
}

}  // namespace

// A strategy built before the venue's tick is applied reads it from the
// symbol's first event on, in whatever callback reads it. Other symbols keep
// their books.
TEST(SymbolRegistryVenueTick, AStrategyBuiltEarlierFollowsTheAppliedTick)
{
  SymbolRegistry registry;
  const SymbolId a = registry.registerSymbol(undeclared("bybit", "AUSDT"));
  const SymbolId b = registry.registerSymbol(undeclared("bybit", "BUSDT"));
  TickReader strategy({a, b}, registry);
  EXPECT_EQ(strategy.bookTick(a).raw(), kDefaultTickSize.raw());

  registry.applyVenueTick(a, Price::fromDouble(0.0001));
  trade(strategy, a);
  ASSERT_TRUE(strategy.tickInTrade);
  EXPECT_EQ(strategy.tickInTrade->raw(), Price::fromDouble(0.0001).raw());
  EXPECT_EQ(strategy.bookTick(a).raw(), Price::fromDouble(0.0001).raw());

  trade(strategy, b);
  EXPECT_EQ(strategy.bookTick(b).raw(), kDefaultTickSize.raw());
  // symbolInfos() follows the applied tick too; the other symbol's entry is
  // as it was copied.
  const auto infos = strategy.symbolInfos();
  EXPECT_TRUE(infos[0].tickSizeKnown);
  EXPECT_EQ(infos[0].tickSize.raw(), Price::fromDouble(0.0001).raw());
  EXPECT_FALSE(infos[1].tickSizeKnown);
}

// The venue's second tick reaches the strategy the way its first did.
TEST(SymbolRegistryVenueTick, AStrategyFollowsEachChangeOfTheVenueTick)
{
  SymbolRegistry registry;
  const SymbolId a = registry.registerSymbol(undeclared("polymarket", "TOKEN"));
  TickReader strategy({a}, registry);

  registry.applyVenueTick(a, Price::fromDouble(0.01));
  trade(strategy, a);
  EXPECT_EQ(strategy.bookTick(a).raw(), Price::fromDouble(0.01).raw());

  registry.applyVenueTick(a, Price::fromDouble(0.001));
  EXPECT_EQ(strategy.bookTick(a).raw(), Price::fromDouble(0.01).raw()) << "until its next event";
  trade(strategy, a);
  ASSERT_TRUE(strategy.tickInTrade);
  EXPECT_EQ(strategy.tickInTrade->raw(), Price::fromDouble(0.001).raw());
  EXPECT_EQ(strategy.bookTick(a).raw(), Price::fromDouble(0.001).raw());
}

// The strategy holds the log, not the registry: a registry gone before the
// strategy's next event is not touched.
TEST(SymbolRegistryVenueTick, AStrategyMayOutliveTheRegistry)
{
  std::unique_ptr<TickReader> strategy;
  SymbolId a = 0;
  {
    SymbolRegistry registry;
    a = registry.registerSymbol(undeclared("bybit", "AUSDT"));
    strategy = std::make_unique<TickReader>(std::vector<SymbolId>{a}, registry);
    registry.applyVenueTick(a, Price::fromDouble(0.5));
  }
  trade(*strategy, a);
  EXPECT_EQ(strategy->bookTick(a).raw(), Price::fromDouble(0.5).raw());
}

// Names are written as JSON strings: quotes, backslashes and control
// characters are escaped, and they read back as the same bytes. A brace, a
// comma or a key-like fragment inside a name does not end the entry early.
TEST(SymbolRegistryTest, JsonEscapesNamesAndReadsThemBack)
{
  SymbolInfo odd;
  odd.exchange = "ex\"q\\b";
  odd.symbol = std::string("A\"B\\C\n}{, \"id\": 9, \"tick_size\": 1\t") + '\x01' + "\xc3\xa9";
  odd.tickSize = Price::fromRaw(5);
  odd.tickSizeKnown = true;
  odd.settlementCcy = "U\"SD\\";
  odd.qtyScale = 1'000;
  SymbolInfo next;
  next.exchange = "binance";
  next.symbol = "ETHUSDT";

  SymbolRegistry registry;
  odd.id = registry.registerSymbol(odd);
  next.id = registry.registerSymbol(next);
  ASSERT_NE(odd.id, 0u);

  const auto path = std::filesystem::temp_directory_path() / "test_registry_escapes.json";
  std::filesystem::remove(path);
  ASSERT_TRUE(registry.saveToFile(path));
  std::string text;
  {
    std::FILE* f = std::fopen(path.string().c_str(), "rb");
    ASSERT_NE(f, nullptr);
    char buf[4096];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;)
    {
      text.append(buf, n);
    }
    std::fclose(f);
  }
  for (unsigned char c : text)
  {
    EXPECT_TRUE(c >= 0x20 || c == '\n') << "raw control byte " << int(c) << " in the file";
  }
  EXPECT_NE(text.find(R"("exchange": "ex\"q\\b")"), std::string::npos) << text;
  EXPECT_NE(text.find(R"(A\"B\\C\n}{, \"id\": 9, \"tick_size\": 1\t\u0001)"), std::string::npos)
      << text;
  EXPECT_NE(text.find(R"("settlement_ccy": "U\"SD\\")"), std::string::npos) << text;

  SymbolRegistry loaded;
  ASSERT_TRUE(loaded.loadFromFile(path));
  std::filesystem::remove(path);
  EXPECT_EQ(loaded.size(), 2u);
  for (const SymbolInfo* want : {&odd, &next})
  {
    SCOPED_TRACE(want->symbol);
    auto got = loaded.getSymbolInfo(want->id);
    ASSERT_TRUE(got.has_value());
    expectSameSymbol(*got, *want);
    EXPECT_EQ(loaded.getSymbolId(want->exchange, want->symbol), want->id);
  }
}

// A file from before names were escaped, plain names and a raw backslash the
// old writer left as it was, loads as it did.
TEST(SymbolRegistryTest, JsonFromBeforeEscapingLoadsAsBefore)
{
  const auto path = std::filesystem::temp_directory_path() / "test_registry_unescaped.json";
  {
    std::FILE* f = std::fopen(path.string().c_str(), "wb");
    ASSERT_NE(f, nullptr);
    std::fputs(
        "{\n  \"version\": 1,\n  \"symbols\": [\n"
        "    {\"id\": 3, \"exchange\": \"binance\", \"symbol\": \"BTC\\USDT\", \"type\": 0, "
        "\"tick_size\": 1000000, \"contract_multiplier\": 1, \"settlement_type\": 0, "
        "\"exercise_style\": 0},\n"
        "    {\"id\": 4, \"exchange\": \"bybit\", \"symbol\": \"ETHUSDT\", \"type\": 2, "
        "\"tick_size\": 1000000, \"tick_size_known\": true}\n"
        "  ]\n}\n",
        f);
    std::fclose(f);
  }
  SymbolRegistry loaded;
  ASSERT_TRUE(loaded.loadFromFile(path));
  std::filesystem::remove(path);
  ASSERT_EQ(loaded.size(), 2u);
  EXPECT_EQ(loaded.getSymbolInfo(3)->symbol, "BTC\\USDT");
  EXPECT_EQ(loaded.getSymbolInfo(4)->exchange, "bybit");
  EXPECT_EQ(loaded.getSymbolInfo(4)->type, InstrumentType::Inverse);
  EXPECT_TRUE(loaded.getSymbolInfo(4)->tickSizeKnown);
  EXPECT_EQ(loaded.getSymbolId("bybit", "ETHUSDT"), SymbolId{4});
}

// A load rebuilds what registration builds: the symbol -> exchange mapping
// (when the loading registry knows the exchange), the info's exchange id and
// the id -> name table.
TEST(SymbolRegistryTest, LoadRebuildsTheSymbolToExchangeMapping)
{
  SymbolRegistry src;
  src.registerExchange("uniswap", VenueType::AmmDex);
  SymbolInfo pool;
  pool.exchange = "uniswap";
  pool.symbol = "WETHUSDC";
  SymbolInfo cex;
  cex.exchange = "binance";
  cex.symbol = "BTCUSDT";
  SymbolInfo stray;
  stray.exchange = "nowhere";
  stray.symbol = "XYZ";
  stray.id = 9;
  const SymbolId poolId = src.registerSymbol(pool);
  const SymbolId cexId = src.registerSymbol(cex);
  const SymbolId strayId = src.registerSymbol(stray);

  const auto path = std::filesystem::temp_directory_path() / "test_registry_exchanges.json";
  ASSERT_TRUE(src.saveToFile(path));
  const auto bytes = src.serialize();

  for (int viaJson = 0; viaJson < 2; ++viaJson)
  {
    SCOPED_TRACE(viaJson ? "json" : "binary");
    SymbolRegistry dst;
    // Registered in another order, so the ids differ from the source's.
    const ExchangeId binance = dst.registerExchange("binance", VenueType::CentralizedExchange);
    const ExchangeId uniswap = dst.registerExchange("uniswap", VenueType::AmmDex);
    ASSERT_TRUE(viaJson ? dst.loadFromFile(path) : dst.deserialize(bytes));

    EXPECT_EQ(dst.getExchangeForSymbol(poolId), uniswap);
    EXPECT_EQ(dst.venueTypeForSymbol(poolId), VenueType::AmmDex);
    EXPECT_EQ(dst.getSymbolInfo(poolId)->exchangeId, uniswap);
    EXPECT_EQ(dst.getExchangeForSymbol(cexId), binance);
    EXPECT_EQ(dst.getExchangeForSymbol(strayId), InvalidExchangeId);
    EXPECT_EQ(dst.venueTypeForSymbol(strayId), VenueType::CentralizedExchange);
    EXPECT_EQ(dst.getSymbolName(poolId), (std::pair<std::string, std::string>{"uniswap", "WETHUSDC"}));
    EXPECT_EQ(dst.getSymbolName(strayId), (std::pair<std::string, std::string>{"nowhere", "XYZ"}));
  }
  std::filesystem::remove(path);
}

namespace
{

SymbolRegistry* fillThreeSymbols(SymbolRegistry& reg)
{
  SymbolInfo a;
  a.exchange = "bybit";
  a.symbol = "BTCUSDT";
  a.tickSize = Price::fromDouble(0.5);
  a.lotSize = Quantity::fromDouble(0.001);
  SymbolInfo b;
  b.exchange = "bybit";
  b.symbol = "ETH-28MAR-3000-C";
  b.type = InstrumentType::Option;
  b.strike = Price::fromDouble(3000.0);
  b.optionType = OptionType::CALL;
  b.settlementCcy = "USDC";
  SymbolInfo c;
  c.exchange = "bitget";
  c.symbol = "SOLUSDT";
  reg.registerSymbol(a);
  reg.registerSymbol(b);
  reg.registerSymbol(c);
  return &reg;
}

std::string readWholeFile(const std::filesystem::path& path)
{
  std::string out;
  std::FILE* f = std::fopen(path.string().c_str(), "rb");
  if (f == nullptr)
  {
    return out;
  }
  char buf[4096];
  for (size_t n = std::fread(buf, 1, sizeof buf, f); n > 0; n = std::fread(buf, 1, sizeof buf, f))
  {
    out.append(buf, n);
  }
  std::fclose(f);
  return out;
}

}  // namespace

// Every proper prefix of a blob is a truncated blob: it must not load, and it
// must leave the registry empty rather than holding entries built from zeros.
TEST(SymbolRegistryTest, DeserializeRefusesEveryTruncatedBlob)
{
  SymbolRegistry src;
  const auto bytes = fillThreeSymbols(src)->serialize();
  ASSERT_GT(bytes.size(), 12u);

  for (size_t len = 0; len < bytes.size(); ++len)
  {
    SCOPED_TRACE("prefix of " + std::to_string(len) + " bytes");
    SymbolRegistry dst;
    dst.registerSymbol("held", "BEFORE");
    EXPECT_FALSE(dst.deserialize(std::span<const std::byte>(bytes.data(), len)));
    EXPECT_EQ(dst.size(), 0u);
    EXPECT_FALSE(dst.getSymbolId("held", "BEFORE").has_value());
  }

  SymbolRegistry whole;
  ASSERT_TRUE(whole.deserialize(bytes));
  EXPECT_EQ(whole.size(), 3u);
}

// A count larger than the entries that follow is the same truncation.
TEST(SymbolRegistryTest, DeserializeRefusesACountBeyondTheData)
{
  SymbolRegistry src;
  auto bytes = fillThreeSymbols(src)->serialize();
  bytes[8] = std::byte{200};
  SymbolRegistry dst;
  EXPECT_FALSE(dst.deserialize(bytes));
  EXPECT_EQ(dst.size(), 0u);
}

TEST(SymbolRegistryTest, DeserializeRefusesAnInvalidScale)
{
  SymbolRegistry src;
  SymbolInfo info;
  info.exchange = "x";
  info.symbol = "Y";
  src.registerSymbol(info);
  auto bytes = src.serialize();
  // The entry ends with price_scale and qty_scale, eight bytes each.
  ASSERT_GE(bytes.size(), 16u);
  std::fill(bytes.end() - 16, bytes.end(), std::byte{0});
  SymbolRegistry dst;
  EXPECT_FALSE(dst.deserialize(bytes));
  EXPECT_EQ(dst.size(), 0u);
}

// An entry without its id or names used to end the list quietly: the load
// returned true with the entries before it and none of those after.
TEST(SymbolRegistryTest, LoadRefusesAnEntryWithoutItsNames)
{
  const auto path = std::filesystem::temp_directory_path() / "test_registry_nameless_entry.json";
  const char* bodies[] = {
      // no "symbol"
      "    {\"id\": 1, \"exchange\": \"bybit\", \"symbol\": \"BTCUSDT\", \"type\": 0},\n"
      "    {\"id\": 2, \"exchange\": \"bybit\", \"type\": 0},\n"
      "    {\"id\": 3, \"exchange\": \"bybit\", \"symbol\": \"ETHUSDT\", \"type\": 0}\n",
      // no "exchange"
      "    {\"id\": 1, \"symbol\": \"BTCUSDT\", \"type\": 0},\n"
      "    {\"id\": 3, \"exchange\": \"bybit\", \"symbol\": \"ETHUSDT\", \"type\": 0}\n",
      // no "id"
      "    {\"id\": 1, \"exchange\": \"bybit\", \"symbol\": \"BTCUSDT\", \"type\": 0},\n"
      "    {\"exchange\": \"bybit\", \"symbol\": \"ETHUSDT\", \"type\": 0}\n",
  };
  for (const char* body : bodies)
  {
    SCOPED_TRACE(body);
    {
      std::FILE* f = std::fopen(path.string().c_str(), "wb");
      ASSERT_NE(f, nullptr);
      std::fputs("{\n  \"version\": 1,\n  \"symbols\": [\n", f);
      std::fputs(body, f);
      std::fputs("  ]\n}\n", f);
      std::fclose(f);
    }
    SymbolRegistry dst;
    dst.registerSymbol("held", "BEFORE");
    EXPECT_FALSE(dst.loadFromFile(path));
    EXPECT_EQ(dst.size(), 0u);
  }
  std::filesystem::remove(path);
}

// The table is a hash map; what is written from it is in id order, so two
// registries holding the same entries write the same bytes whatever order the
// entries went in and whatever the map's bucket layout is.
TEST(SymbolRegistryTest, SaveAndSerializeWriteInIdOrder)
{
  constexpr uint32_t kCount = 300;
  const auto fill = [](SymbolRegistry& reg, bool reversed)
  {
    for (uint32_t n = 0; n < kCount; ++n)
    {
      const uint32_t i = reversed ? kCount - n : n + 1;
      SymbolInfo info;
      info.id = i;
      info.exchange = "ex" + std::to_string(i % 7);
      info.symbol = "SYM" + std::to_string(i);
      info.tickSize = Price::fromRaw(static_cast<int64_t>(i) * 1000);
      reg.registerSymbol(info);
    }
  };
  SymbolRegistry forward;
  SymbolRegistry backward;
  fill(forward, false);
  fill(backward, true);

  const auto bytes = forward.serialize();
  EXPECT_EQ(bytes, backward.serialize());

  // Each entry starts with its id; they ascend through the blob.
  size_t pos = 12;
  for (uint32_t i = 1; i <= kCount; ++i)
  {
    ASSERT_LE(pos + 4, bytes.size());
    uint32_t id = 0;
    std::memcpy(&id, bytes.data() + pos, 4);
    ASSERT_EQ(id, i) << "entry " << i << " of the blob";
    const std::string ex = "ex" + std::to_string(i % 7);
    const std::string sym = "SYM" + std::to_string(i);
    // id, two length-prefixed names, type, tick, flags, v3 spec, v4 scales
    pos += 4 + 2 + ex.size() + 2 + sym.size() + 1 + 8 + 1 + 8 + 1 + 1 + 16;
  }
  EXPECT_EQ(pos, bytes.size());

  const auto dir = std::filesystem::temp_directory_path();
  const auto pathA = dir / "test_registry_id_order_a.json";
  const auto pathB = dir / "test_registry_id_order_b.json";
  ASSERT_TRUE(forward.saveToFile(pathA));
  ASSERT_TRUE(backward.saveToFile(pathB));
  const std::string textA = readWholeFile(pathA);
  const std::string textB = readWholeFile(pathB);
  std::filesystem::remove(pathA);
  std::filesystem::remove(pathB);
  EXPECT_EQ(textA, textB);

  size_t at = 0;
  for (uint32_t i = 1; i <= kCount; ++i)
  {
    const std::string key = "{\"id\": " + std::to_string(i) + ",";
    const size_t found = textA.find(key, at);
    ASSERT_NE(found, std::string::npos) << "id " << i << " is not after id " << (i - 1);
    at = found + key.size();
  }
}
