/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2026 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// What a symbol registry refuses: an id that would size its tables for
// millions of absent symbols, a file or blob in which one name appears under
// two ids, a file of a later version, a file with anything after its
// document; and a save whose temporary name another save could share.

#include "lrvx/engine/symbol_registry.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace lrvx;

namespace
{

std::filesystem::path scratchDir(const char* name)
{
  auto dir = std::filesystem::temp_directory_path() / (std::string("lrvx_registry_rules_") + name);
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

std::string entry(unsigned id, const char* exchange, const char* symbol)
{
  return "    {\"id\": " + std::to_string(id) + ", \"exchange\": \"" + exchange +
         "\", \"symbol\": \"" + symbol + "\", \"type\": 0, \"tick_size\": 1000000}";
}

std::string document(const std::string& head, const std::string& entries,
                     const std::string& tail = "\n")
{
  return "{\n" + head + "  \"symbols\": [\n" + entries + "\n  ]\n}" + tail;
}

constexpr SymbolId kLargestId = 1'048'575;

}  // namespace

// The name table is indexed by id, so an explicit id sized it for every id
// below: registering one symbol under id 4'000'000'000 asked for about 190 GB.
TEST(SymbolRegistryRules, AnExplicitIdAboveTheLimitIsRefused)
{
  SymbolRegistry registry;
  SymbolInfo info;
  info.exchange = "ex";
  info.symbol = "FAR";
  info.id = kLargestId + 1;
  EXPECT_EQ(registry.registerSymbol(info), 0u);
  EXPECT_EQ(registry.size(), 0u);
  EXPECT_FALSE(registry.getSymbolId("ex", "FAR").has_value());

  info.id = kLargestId;
  EXPECT_EQ(registry.registerSymbol(info), kLargestId);
  EXPECT_EQ(registry.getSymbolName(kLargestId).second, "FAR");

  // The sequence is at the limit: a name-based registration has no id left.
  EXPECT_EQ(registry.registerSymbol("ex", "NEXT"), 0u);
  EXPECT_FALSE(registry.getSymbolId("ex", "NEXT").has_value());
  EXPECT_EQ(registry.size(), 1u);
}

TEST(SymbolRegistryRules, ALoadWithAnIdAboveTheLimitFails)
{
  const auto dir = scratchDir("far_id");
  const auto path = dir / "registry.json";
  writeAll(path, document("  \"version\": 1,\n", entry(kLargestId + 1, "ex", "FAR")));
  SymbolRegistry registry;
  EXPECT_FALSE(registry.loadFromFile(path));
  EXPECT_EQ(registry.size(), 0u);
  std::filesystem::remove_all(dir);
}

// One name under two ids loaded, and the name resolved to whichever entry the
// hash map's order visited last.
TEST(SymbolRegistryRules, AFileWithOneNameUnderTwoIdsFails)
{
  const auto dir = scratchDir("dup_name_file");
  const auto path = dir / "registry.json";
  writeAll(path, document("  \"version\": 1,\n",
                          entry(1, "ex", "BTC") + ",\n" + entry(2, "ex", "BTC")));
  SymbolRegistry registry;
  registry.registerSymbol("ex", "BEFORE");
  EXPECT_FALSE(registry.loadFromFile(path));
  EXPECT_EQ(registry.size(), 0u) << "a failed load leaves the registry empty";

  // The same symbol name on two exchanges is two names.
  writeAll(path, document("  \"version\": 1,\n",
                          entry(1, "ex", "BTC") + ",\n" + entry(2, "other", "BTC")));
  EXPECT_TRUE(registry.loadFromFile(path));
  EXPECT_EQ(registry.size(), 2u);
  std::filesystem::remove_all(dir);
}

TEST(SymbolRegistryRules, ABlobWithOneNameUnderTwoIdsFails)
{
  SymbolRegistry source;
  SymbolInfo info;
  info.exchange = "ex";
  info.symbol = "BTCA";
  info.id = 1;
  ASSERT_EQ(source.registerSymbol(info), 1u);
  info.symbol = "BTCB";
  info.id = 2;
  ASSERT_EQ(source.registerSymbol(info), 2u);
  std::vector<std::byte> blob = source.serialize();

  // Rename the second entry to the first's name, in place: same length.
  const std::string from = "BTCB";
  bool renamed = false;
  for (size_t i = 0; i + from.size() <= blob.size(); ++i)
  {
    if (std::string(reinterpret_cast<const char*>(blob.data() + i), from.size()) == from)
    {
      blob[i + 3] = std::byte{'A'};
      renamed = true;
    }
  }
  ASSERT_TRUE(renamed);

  SymbolRegistry registry;
  EXPECT_FALSE(registry.deserialize(blob));
  EXPECT_EQ(registry.size(), 0u);
}

// "version" was never read: a file of any later format loaded as if it were
// this one.
TEST(SymbolRegistryRules, AFileOfALaterVersionFails)
{
  const auto dir = scratchDir("version");
  const auto path = dir / "registry.json";
  SymbolRegistry registry;

  writeAll(path, document("  \"version\": 2,\n", entry(1, "ex", "BTC")));
  EXPECT_FALSE(registry.loadFromFile(path));
  EXPECT_EQ(registry.size(), 0u);

  // Wherever the member sits in the object.
  writeAll(path, "{\n  \"symbols\": [\n" + entry(1, "ex", "BTC") + "\n  ],\n  \"version\": 2\n}\n");
  EXPECT_FALSE(registry.loadFromFile(path));

  writeAll(path, document("  \"version\": \"one\",\n", entry(1, "ex", "BTC")));
  EXPECT_FALSE(registry.loadFromFile(path));

  // The version this build writes, and a file with none, load.
  writeAll(path, document("  \"version\": 1,\n", entry(1, "ex", "BTC")));
  EXPECT_TRUE(registry.loadFromFile(path));
  writeAll(path, document("", entry(1, "ex", "BTC")));
  EXPECT_TRUE(registry.loadFromFile(path));
  EXPECT_EQ(registry.size(), 1u);
  std::filesystem::remove_all(dir);
}

// Everything after the "symbols" array was ignored.
TEST(SymbolRegistryRules, AFileWithContentAfterItsDocumentFails)
{
  const auto dir = scratchDir("trailing");
  const auto path = dir / "registry.json";
  SymbolRegistry registry;
  const std::string one = entry(1, "ex", "BTC");

  for (const char* tail : {"garbage", "\n{}", "\n]", "}"})
  {
    writeAll(path, document("  \"version\": 1,\n", one, tail));
    EXPECT_FALSE(registry.loadFromFile(path)) << "tail: " << tail;
    EXPECT_EQ(registry.size(), 0u);
  }
  // Content between the array and the closing brace that is not a member.
  writeAll(path, "{\n  \"version\": 1,\n  \"symbols\": [\n" + one + "\n  ] garbage\n}\n");
  EXPECT_FALSE(registry.loadFromFile(path));
  // The object left open.
  writeAll(path, "{\n  \"version\": 1,\n  \"symbols\": [\n" + one + "\n  ]\n");
  EXPECT_FALSE(registry.loadFromFile(path));

  // Whitespace after the document is not content.
  writeAll(path, document("  \"version\": 1,\n", one, "\n\n  \t\r\n"));
  EXPECT_TRUE(registry.loadFromFile(path));
  EXPECT_EQ(registry.size(), 1u);
  std::filesystem::remove_all(dir);
}

// The temporary was always "<path>.tmp": two saves to one path wrote through
// one name, and anything already at that name stopped every save.
TEST(SymbolRegistryRules, ASaveDoesNotDependOnAFixedTemporaryName)
{
  const auto dir = scratchDir("unique_tmp");
  const auto path = dir / "registry.json";
  // Something else holds the old fixed name.
  std::filesystem::create_directories(dir / "registry.json.tmp");

  SymbolRegistry registry;
  registry.registerSymbol("ex", "BTC");
  ASSERT_TRUE(registry.saveToFile(path));
  ASSERT_TRUE(registry.saveToFile(path)) << "and again, over the file just written";

  SymbolRegistry loaded;
  EXPECT_TRUE(loaded.loadFromFile(path));
  EXPECT_EQ(loaded.size(), 1u);

  size_t entries = 0;
  for (const auto& e : std::filesystem::directory_iterator(dir))
  {
    (void)e;
    ++entries;
  }
  EXPECT_EQ(entries, 2u) << "the file and the unrelated directory; no temporary is left";
  std::filesystem::remove_all(dir);
}

// --- reasons ---

TEST(SymbolRegistryRules, AFailedFileLoadSaysWhy)
{
  const auto dir = scratchDir("reasons");
  const auto path = dir / "registry.json";
  SymbolRegistry registry;
  std::string why = "stale";

  writeAll(path, document("  \"version\": 7,\n", entry(1, "ex", "BTC")));
  EXPECT_FALSE(registry.loadFromFile(path, why));
  EXPECT_NE(why.find("version 7"), std::string::npos) << why;
  EXPECT_NE(why.find("newer than the version 1"), std::string::npos) << why;
  EXPECT_NE(why.find(path.string()), std::string::npos) << why;

  writeAll(path, document("  \"version\": 1,\n", entry(1, "ex", "BTC"), "garbage"));
  EXPECT_FALSE(registry.loadFromFile(path, why));
  EXPECT_NE(why.find("content after the end of the document"), std::string::npos) << why;

  writeAll(path, document("  \"version\": 1,\n",
                          entry(3, "ex", "BTC") + ",\n" + entry(4, "ex", "BTC")));
  EXPECT_FALSE(registry.loadFromFile(path, why));
  EXPECT_NE(why.find("two entries are named ex:BTC"), std::string::npos) << why;
  EXPECT_NE(why.find("3"), std::string::npos) << why;
  EXPECT_NE(why.find("4"), std::string::npos) << why;

  writeAll(path, document("  \"version\": 1,\n", entry(SymbolRegistry::kMaxSymbolId + 1, "ex", "X")));
  EXPECT_FALSE(registry.loadFromFile(path, why));
  EXPECT_NE(why.find("above the largest id 1048575"), std::string::npos) << why;

  EXPECT_FALSE(registry.loadFromFile(dir / "absent.json", why));
  EXPECT_NE(why.find("cannot open"), std::string::npos) << why;

  writeAll(path, document("  \"version\": 1,\n", entry(1, "ex", "BTC")));
  EXPECT_TRUE(registry.loadFromFile(path, why));
  EXPECT_EQ(why, "");
  std::filesystem::remove_all(dir);
}

TEST(SymbolRegistryRules, AFailedDeserializeSaysWhy)
{
  SymbolRegistry source;
  source.registerSymbol("ex", "BTC");
  std::vector<std::byte> blob = source.serialize();
  SymbolRegistry registry;
  std::string why = "stale";

  EXPECT_TRUE(registry.deserialize(blob, why));
  EXPECT_EQ(why, "");

  std::vector<std::byte> longer = blob;
  longer.push_back(std::byte{0});
  EXPECT_FALSE(registry.deserialize(longer, why));
  EXPECT_NE(why.find("1 byte(s) follow the last entry"), std::string::npos) << why;

  std::vector<std::byte> newer = blob;
  newer[4] = std::byte{99};
  EXPECT_FALSE(registry.deserialize(newer, why));
  EXPECT_NE(why.find("version 99"), std::string::npos) << why;

  blob.resize(blob.size() - 3);
  EXPECT_FALSE(registry.deserialize(blob, why));
  EXPECT_NE(why.find("ends inside entry 0"), std::string::npos) << why;
}
