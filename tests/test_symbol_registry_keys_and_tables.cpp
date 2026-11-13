/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2026 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// A symbol registry's name key, its JSON entries and its by-id tables: two
// names that joined to one key, an entry member that was an object or an
// array, and tables sized by the largest id.

#include "lrvx/engine/symbol_registry.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

using namespace lrvx;

namespace
{

std::filesystem::path scratchDir(const char* name)
{
  auto dir = std::filesystem::temp_directory_path() / (std::string("lrvx_registry_keys_") + name);
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  return dir;
}

void writeAll(const std::filesystem::path& path, const std::string& text)
{
  std::FILE* f = std::fopen(path.string().c_str(), "wb");
  ASSERT_NE(f, nullptr);
  ASSERT_EQ(std::fwrite(text.data(), 1, text.size(), f), text.size());
  std::fclose(f);
}

std::string document(const std::string& entries)
{
  return "{\n  \"version\": 1,\n  \"symbols\": [\n" + entries + "\n  ]\n}\n";
}

std::string entry(unsigned id, const char* exchange, const char* symbol,
                  const std::string& extra = "")
{
  return "    {\"id\": " + std::to_string(id) + ", \"exchange\": \"" + exchange +
         "\", \"symbol\": \"" + symbol + "\", \"type\": 0, \"tick_size\": 1000000" + extra + "}";
}

}  // namespace

// --- the name key ---

// The key was exchange + ":" + symbol, so ("a:b", "c") and ("a", "b:c") were
// one name: the second registration returned the first one's id.
TEST(SymbolRegistryKeys, TwoNamesThatJoinToTheSameTextAreTwoSymbols)
{
  SymbolRegistry registry;
  const SymbolId first = registry.registerSymbol("a:b", "c");
  const SymbolId second = registry.registerSymbol("a", "b:c");
  ASSERT_NE(first, 0u);
  ASSERT_NE(second, 0u);
  EXPECT_NE(first, second);
  EXPECT_EQ(registry.size(), 2u);

  EXPECT_EQ(registry.getSymbolId("a:b", "c"), first);
  EXPECT_EQ(registry.getSymbolId("a", "b:c"), second);
  EXPECT_FALSE(registry.getSymbolId("a:b:c", "").has_value());
  EXPECT_FALSE(registry.getSymbolId("", "a:b:c").has_value());
  EXPECT_EQ(registry.getSymbolName(first), (std::pair<std::string, std::string>{"a:b", "c"}));
  EXPECT_EQ(registry.getSymbolName(second), (std::pair<std::string, std::string>{"a", "b:c"}));

  // The SymbolInfo overload and a repeated registration agree.
  SymbolInfo info;
  info.exchange = "a";
  info.symbol = "b:c";
  EXPECT_EQ(registry.registerSymbol(info), second);
  EXPECT_EQ(registry.registerSymbol("a:b", "c"), first);

  // A length in front of a name is not a way in either.
  const SymbolId plain = registry.registerSymbol("1", "x");
  const SymbolId tricky = registry.registerSymbol("", "1:1x");
  EXPECT_NE(plain, tricky);
  EXPECT_EQ(registry.getSymbolId("1", "x"), plain);
}

TEST(SymbolRegistryKeys, SuchNamesSurviveAFileAndABlob)
{
  const auto dir = scratchDir("roundtrip");
  SymbolRegistry registry;
  const SymbolId first = registry.registerSymbol("a:b", "c");
  const SymbolId second = registry.registerSymbol("a", "b:c");
  ASSERT_TRUE(registry.saveToFile(dir / "registry.json"));

  SymbolRegistry fromFile;
  std::string why;
  ASSERT_TRUE(fromFile.loadFromFile(dir / "registry.json", why)) << why;
  EXPECT_EQ(fromFile.getSymbolId("a:b", "c"), first);
  EXPECT_EQ(fromFile.getSymbolId("a", "b:c"), second);

  SymbolRegistry fromBlob;
  const std::vector<std::byte> blob = registry.serialize();
  ASSERT_TRUE(fromBlob.deserialize(blob, why)) << why;
  EXPECT_EQ(fromBlob.getSymbolId("a:b", "c"), first);
  EXPECT_EQ(fromBlob.getSymbolId("a", "b:c"), second);

  // One name twice is still refused, and named as written.
  writeAll(dir / "twice.json", document(entry(3, "a:b", "c") + ",\n" + entry(4, "a:b", "c")));
  EXPECT_FALSE(fromFile.loadFromFile(dir / "twice.json", why));
  EXPECT_NE(why.find("two entries are named a:b:c"), std::string::npos) << why;
  std::filesystem::remove_all(dir);
}

// --- a registry entry is flat ---

// A member whose value was an object or an array was read as raw text, or
// ended the entry where the nested value closed.
TEST(SymbolRegistryKeys, AnEntryMemberThatIsAnObjectOrAnArrayIsRefused)
{
  const auto dir = scratchDir("nested");
  const auto path = dir / "registry.json";
  SymbolRegistry registry;
  std::string why;

  writeAll(path, document(entry(1, "ex", "BTC", ", \"meta\": {\"a\": 1}")));
  EXPECT_FALSE(registry.loadFromFile(path, why));
  EXPECT_NE(why.find("\"meta\""), std::string::npos) << why;
  EXPECT_NE(why.find("an object"), std::string::npos) << why;
  EXPECT_NE(why.find("flat"), std::string::npos) << why;
  EXPECT_EQ(registry.size(), 0u);

  writeAll(path, document(entry(1, "ex", "BTC", ", \"tags\": [1, 2]")));
  EXPECT_FALSE(registry.loadFromFile(path, why));
  EXPECT_NE(why.find("\"tags\""), std::string::npos) << why;
  EXPECT_NE(why.find("an array"), std::string::npos) << why;

  writeAll(path, document(entry(1, "ex", "BTC", ", \"tags\": []")));
  EXPECT_FALSE(registry.loadFromFile(path, why));
  EXPECT_NE(why.find("an array"), std::string::npos) << why;

  writeAll(path, document(entry(1, "ex", "BTC", ", \"meta\": {}")));
  EXPECT_FALSE(registry.loadFromFile(path, why));
  EXPECT_NE(why.find("an object"), std::string::npos) << why;

  // The second entry is the one named.
  writeAll(path, document(entry(1, "ex", "BTC") + ",\n" +
                          entry(2, "ex", "ETH", ", \"meta\": {\"a\": 1}")));
  EXPECT_FALSE(registry.loadFromFile(path, why));
  EXPECT_NE(why.find("entry 1"), std::string::npos) << why;

  // A member this version does not know is still skipped when it is flat.
  writeAll(path, document(entry(1, "ex", "BTC", ", \"note\": \"{not an object}\", \"n\": 7")));
  EXPECT_TRUE(registry.loadFromFile(path, why)) << why;
  EXPECT_EQ(registry.size(), 1u);
  std::filesystem::remove_all(dir);
}

// --- the by-id tables ---

// The name, exchange and equivalence tables were arrays indexed by id, so one
// explicit id near the limit sized each of them for every id below it.
TEST(SymbolRegistryTables, OneLargeIdDoesNotSizeTheTablesUpToIt)
{
  SymbolRegistry registry;
  const ExchangeId venue = registry.registerExchange("ex");
  const SymbolId small = registry.registerSymbol(venue, "BTC");

  SymbolInfo info;
  info.exchange = "ex";
  info.symbol = "FAR";
  info.id = SymbolRegistry::kMaxSymbolId - 1;
  const SymbolId far = registry.registerSymbol(info);
  ASSERT_EQ(far, SymbolRegistry::kMaxSymbolId - 1);
  const std::array<SymbolId, 2> both{small, far};
  registry.mapEquivalentSymbols(both);

  EXPECT_LE(registry.denseTableEntries(), 3 * SymbolRegistry::kDenseSymbolIds)
      << "three tables, none of them sized by the largest id";
  EXPECT_LT(registry.denseTableEntries(), 64u) << "two small ids were registered";

  // Lookups answer the same on both sides of the dense bound.
  EXPECT_EQ(registry.getSymbolName(far).second, "FAR");
  EXPECT_EQ(registry.getSymbolName(small).second, "BTC");
  EXPECT_EQ(registry.getExchangeForSymbol(far), venue);
  EXPECT_EQ(registry.getExchangeForSymbol(small), venue);
  EXPECT_EQ(registry.getEquivalentSymbols(far).size(), 2u);
  EXPECT_EQ(registry.getEquivalentSymbols(small).size(), 2u);
  EXPECT_EQ(registry.getEquivalentOnExchange(small, venue), small);

  // Ids nobody registered.
  EXPECT_EQ(registry.getExchangeForSymbol(far - 1), InvalidExchangeId);
  EXPECT_TRUE(registry.getEquivalentSymbols(far - 1).empty());
  EXPECT_THROW(registry.getSymbolName(far + 5), std::out_of_range);
  EXPECT_EQ(registry.getSymbolName(far - 1).second, "") << "below the highest id: no name";

  // The sequence still continues one above the highest id.
  EXPECT_EQ(registry.registerSymbol("ex", "NEXT"), SymbolRegistry::kMaxSymbolId);
  EXPECT_EQ(registry.registerSymbol("ex", "NONE_LEFT"), 0u);
}

TEST(SymbolRegistryTables, IdsOnBothSidesOfTheDenseBoundBehaveAlike)
{
  SymbolRegistry registry;
  const ExchangeId venue = registry.registerExchange("ex");
  std::vector<SymbolId> ids;
  for (SymbolId id : {SymbolRegistry::kDenseSymbolIds - 1, SymbolRegistry::kDenseSymbolIds,
                      SymbolRegistry::kDenseSymbolIds + 1})
  {
    SymbolInfo info;
    info.exchange = "ex";
    info.symbol = "S" + std::to_string(id);
    info.id = id;
    ASSERT_EQ(registry.registerSymbol(info), id);
    ids.push_back(id);
  }
  for (SymbolId id : ids)
  {
    EXPECT_EQ(registry.getSymbolName(id).second, "S" + std::to_string(id));
    EXPECT_EQ(registry.getExchangeForSymbol(id), venue);
    EXPECT_EQ(registry.getSymbolId("ex", "S" + std::to_string(id)), id);
  }

  // A reload rebuilds both halves.
  const std::vector<std::byte> blob = registry.serialize();
  SymbolRegistry again;
  again.registerExchange("ex");
  std::string why;
  ASSERT_TRUE(again.deserialize(blob, why)) << why;
  for (SymbolId id : ids)
  {
    EXPECT_EQ(again.getSymbolName(id).second, "S" + std::to_string(id));
    EXPECT_EQ(again.getExchangeForSymbol(id), venue);
  }
  EXPECT_EQ(again.registerSymbol("ex", "NEXT"), SymbolRegistry::kDenseSymbolIds + 2);

  registry.clear();
  EXPECT_EQ(registry.denseTableEntries(), 0u);
  EXPECT_EQ(registry.registerSymbol("ex", "FIRST"), 1u);
}
