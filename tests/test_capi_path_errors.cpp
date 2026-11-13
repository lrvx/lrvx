/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// C API functions that are handed a path or a symbol name: when the file
// cannot be read or the name is not registered, the failure value comes with
// a reason in lrvx_last_error_message.

#include "lrvx/capi/lrvx_capi.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace
{

constexpr int64_t kSec = 1'000'000'000;

class CapiPathErrors : public ::testing::Test
{
 protected:
  void SetUp() override
  {
    _root = std::filesystem::temp_directory_path() /
            (std::string("lrvx_capi_path_errors_") +
             ::testing::UnitTest::GetInstance()->current_test_info()->name() + "_" +
             std::to_string(std::random_device{}()));
    std::filesystem::remove_all(_root);
    std::filesystem::create_directories(_root);
    _missing = (_root / "no_such_thing").string();
    _registry = lrvx_registry_create();
    _symbol = lrvx_registry_add_symbol(_registry, "test", "BTCUSDT", 0.01);
    lrvx_clear_last_error();
  }

  void TearDown() override
  {
    lrvx_registry_destroy(_registry);
    std::filesystem::remove_all(_root);
  }

  std::string writeCsv(const std::string& name, const std::string& body) const
  {
    const auto path = _root / name;
    std::ofstream(path) << body;
    return path.string();
  }

  static std::string lastError() { return lrvx_last_error_message(); }

  static void expectReason(const std::string& needle)
  {
    EXPECT_NE(lrvx_last_error_code(), 0);
    EXPECT_NE(lastError().find(needle), std::string::npos)
        << "the reason should mention '" << needle << "', got '" << lastError() << "'";
  }

  std::filesystem::path _root;
  std::string _missing;
  LrvxRegistryHandle _registry{nullptr};
  uint32_t _symbol{0};
};

int g_factoryCalls = 0;

LrvxStrategyHandle countingFactory(void*, uint64_t)
{
  ++g_factoryCalls;
  return nullptr;
}

LrvxWalkForwardConfig foldConfig()
{
  LrvxWalkForwardConfig cfg{};
  cfg.mode = 1;
  cfg.train_size = 2;
  cfg.test_size = 2;
  cfg.step = 2;
  return cfg;
}

TEST_F(CapiPathErrors, WalkForwardSaysWhichCsvItCouldNotOpen)
{
  const LrvxWalkForwardConfig cfg = foldConfig();
  g_factoryCalls = 0;
  const uint32_t n = lrvx_walk_forward_run_csv(_registry, _missing.c_str(), "BTCUSDT", 0.0, 1000.0,
                                               &cfg, countingFactory, nullptr, nullptr, 0);
  EXPECT_EQ(n, 0u);
  expectReason(_missing);
  EXPECT_EQ(g_factoryCalls, 0);
}

TEST_F(CapiPathErrors, WalkForwardSaysWhichRowItCouldNotParse)
{
  const std::string csv =
      writeCsv("bad.csv", "timestamp,open,high,low,close,volume\n1,1,1,1,100,1\nnot-a-row\n");
  const LrvxWalkForwardConfig cfg = foldConfig();
  const uint32_t n = lrvx_walk_forward_run_csv(_registry, csv.c_str(), "BTCUSDT", 0.0, 1000.0, &cfg,
                                               countingFactory, nullptr, nullptr, 0);
  EXPECT_EQ(n, 0u);
  expectReason("line 3");
  expectReason("not-a-row");
}

TEST_F(CapiPathErrors, WalkForwardTooFewBarsIsNotAnError)
{
  const std::string csv =
      writeCsv("short.csv", "timestamp,open,high,low,close,volume\n1,1,1,1,100,1\n");
  const LrvxWalkForwardConfig cfg = foldConfig();
  const uint32_t n = lrvx_walk_forward_run_csv(_registry, csv.c_str(), "BTCUSDT", 0.0, 1000.0, &cfg,
                                               countingFactory, nullptr, nullptr, 0);
  EXPECT_EQ(n, 0u);
  EXPECT_EQ(lrvx_last_error_code(), 0) << lastError();
}

TEST_F(CapiPathErrors, WalkForwardRefusesAnUnknownSymbol)
{
  std::string body = "timestamp,open,high,low,close,volume\n";
  for (int i = 1; i <= 8; ++i)
  {
    body += std::to_string(i) + ",1,1,1,100,1\n";
  }
  const std::string csv = writeCsv("ok.csv", body);
  const LrvxWalkForwardConfig cfg = foldConfig();
  g_factoryCalls = 0;
  std::vector<LrvxWalkForwardFold> folds(8);
  const uint32_t n = lrvx_walk_forward_run_csv(_registry, csv.c_str(), "NOPE", 0.0, 1000.0, &cfg,
                                               countingFactory, nullptr, folds.data(), 8);
  EXPECT_EQ(n, 0u);
  expectReason("NOPE");
  EXPECT_EQ(g_factoryCalls, 0);
}

TEST_F(CapiPathErrors, WalkForwardNamesAMissingArgument)
{
  const uint32_t n = lrvx_walk_forward_run_csv(_registry, _missing.c_str(), "BTCUSDT", 0.0, 1000.0,
                                               nullptr, countingFactory, nullptr, nullptr, 0);
  EXPECT_EQ(n, 0u);
  expectReason("cfg");
}

TEST_F(CapiPathErrors, RunReaderSaysWhatItCouldNotOpen)
{
  EXPECT_EQ(lrvx_run_reader_open(_missing.c_str()), nullptr);
  expectReason(_missing);
}

TEST_F(CapiPathErrors, RunRecorderSaysWhatItCouldNotCreate)
{
  // A regular file where the trace directory should go.
  const std::string blocker = writeCsv("blocker", "x");
  const std::string under = blocker + "/trace";
  EXPECT_EQ(lrvx_run_recorder_create(under.c_str(), "s", "h", 0), nullptr);
  EXPECT_NE(lrvx_last_error_code(), 0);
  EXPECT_FALSE(lastError().empty());
}

TEST_F(CapiPathErrors, TapeDiffSaysWhichSideIsNotADirectory)
{
  const std::string left = (_root / "left").string();
  std::filesystem::create_directories(left);
  EXPECT_EQ(lrvx_tape_diff_create(left.c_str(), _missing.c_str(), 16, 0), nullptr);
  expectReason(_missing);
}

TEST_F(CapiPathErrors, MergedTapeReaderSaysWhyItDidNotOpen)
{
  const char* paths[] = {_missing.c_str()};
  EXPECT_EQ(lrvx_merged_tape_reader_create(paths, 1, -1, -1, nullptr, 0), nullptr);
  EXPECT_NE(lrvx_last_error_code(), 0);
  EXPECT_FALSE(lastError().empty());

  lrvx_clear_last_error();
  EXPECT_EQ(lrvx_merged_tape_reader_create(nullptr, 0, -1, -1, nullptr, 0), nullptr);
  EXPECT_NE(lrvx_last_error_code(), 0);
  EXPECT_FALSE(lastError().empty());
}

TEST_F(CapiPathErrors, FundingScheduleSaysWhichTapeItCouldNotOpen)
{
  LrvxFundingScheduleHandle fs = lrvx_funding_schedule_create();
  ASSERT_NE(fs, nullptr);
  EXPECT_EQ(lrvx_funding_schedule_load_tape(fs, _missing.c_str()), 0);
  expectReason(_missing);
  lrvx_funding_schedule_destroy(fs);
}

TEST_F(CapiPathErrors, SegmentOpsSayWhyTheyFailed)
{
  const std::string out = (_root / "out").string();
  const LrvxSplitResult split = lrvx_segment_split(_missing.c_str(), out.c_str(), 0, kSec, 0);
  EXPECT_EQ(split.success, 0);
  EXPECT_NE(lrvx_last_error_code(), 0);
  EXPECT_FALSE(lastError().empty());

  lrvx_clear_last_error();
  const LrvxExportResult exported =
      lrvx_segment_export(_missing.c_str(), (out + ".csv").c_str(), 0, 0, 0, nullptr, 0);
  EXPECT_EQ(exported.success, 0);
  EXPECT_NE(lrvx_last_error_code(), 0);
  EXPECT_FALSE(lastError().empty());
}

// recompress and the two extracts returned a bare 0 for an input they could
// not open, the same value an extract that matches nothing returns.
TEST_F(CapiPathErrors, RecompressAndExtractSayWhatTheyCouldNotOpen)
{
  const std::string out = (_root / "out.lrvx").string();

  EXPECT_EQ(lrvx_segment_recompress(_missing.c_str(), out.c_str(), 1), 0);
  expectReason(_missing);
  expectReason("lrvx_segment_recompress");

  lrvx_clear_last_error();
  const uint32_t symbols[1] = {1};
  EXPECT_EQ(lrvx_segment_extract_symbols(_missing.c_str(), out.c_str(), symbols, 1), 0u);
  expectReason(_missing);
  expectReason("lrvx_segment_extract_symbols");

  lrvx_clear_last_error();
  EXPECT_EQ(lrvx_segment_extract_time_range(_missing.c_str(), out.c_str(), 0, kSec), 0u);
  expectReason(_missing);
  expectReason("lrvx_segment_extract_time_range");
}

TEST_F(CapiPathErrors, AnExtractThatMatchesNothingIsNotAnError)
{
  const auto dir = _root / "tape";
  LrvxDataWriterHandle w = lrvx_data_writer_create(dir.string().c_str(), 16, 1);
  ASSERT_NE(w, nullptr);
  for (int i = 0; i < 5; ++i)
  {
    ASSERT_EQ(lrvx_data_writer_write_trade(w, (10 + i) * kSec, (10 + i) * kSec, 100.0 + i, 1.0,
                                           static_cast<uint64_t>(i + 1), 1, 0),
              1);
  }
  lrvx_data_writer_close(w);
  lrvx_data_writer_destroy(w);
  std::string segment;
  for (const auto& entry : std::filesystem::directory_iterator(dir))
  {
    if (entry.path().extension() != ".idx")
    {
      segment = entry.path().string();
    }
  }
  ASSERT_FALSE(segment.empty());
  lrvx_clear_last_error();

  const uint32_t absent[1] = {99};
  EXPECT_EQ(lrvx_segment_extract_symbols(segment.c_str(), (_root / "none.lrvx").string().c_str(),
                                         absent, 1),
            0u);
  EXPECT_EQ(lrvx_last_error_code(), 0) << lastError();
  EXPECT_EQ(lrvx_segment_extract_time_range(segment.c_str(),
                                            (_root / "none2.lrvx").string().c_str(), 500 * kSec,
                                            600 * kSec),
            0u);
  EXPECT_EQ(lrvx_last_error_code(), 0) << lastError();

  const uint32_t present[1] = {1};
  EXPECT_EQ(lrvx_segment_extract_symbols(segment.c_str(), (_root / "all.lrvx").string().c_str(),
                                         present, 1),
            5u);
  EXPECT_EQ(lrvx_segment_recompress(segment.c_str(), (_root / "lz4.lrvx").string().c_str(), 1), 1);
  EXPECT_EQ(lrvx_last_error_code(), 0) << lastError();

  // An output that cannot be written: the path is a directory.
  const auto taken = _root / "taken.lrvx";
  std::filesystem::create_directories(taken);
  EXPECT_EQ(lrvx_segment_recompress(segment.c_str(), taken.string().c_str(), 0), 0);
  expectReason("Cannot write output file");
  expectReason("taken.lrvx");
  lrvx_clear_last_error();
  EXPECT_EQ(lrvx_segment_extract_symbols(segment.c_str(), taken.string().c_str(), present, 1), 0u);
  expectReason("taken.lrvx");
  lrvx_clear_last_error();
  EXPECT_EQ(lrvx_segment_extract_time_range(segment.c_str(), taken.string().c_str(), 0,
                                            100 * kSec),
            0u);
  expectReason("taken.lrvx");
}

// validate and merge returned a bare 0: a file that is not there read the
// same as a segment that failed its checks, and a merge that found nothing to
// merge the same as one that could not write.
TEST_F(CapiPathErrors, ValidateSaysWhenItCouldNotReadTheFile)
{
  EXPECT_EQ(lrvx_segment_validate(_missing.c_str()), 0);
  expectReason(_missing);
  expectReason("lrvx_segment_validate");

  lrvx_clear_last_error();
  const LrvxSegmentValidation full = lrvx_segment_validate_full(_missing.c_str(), 1, 1);
  EXPECT_EQ(full.valid, 0);
  expectReason(_missing);
  expectReason("lrvx_segment_validate_full");
}

TEST_F(CapiPathErrors, AnInvalidSegmentIsAnAnswerNotAnError)
{
  const std::string garbage = writeCsv("garbage.lrvx", std::string(256, 'x'));
  EXPECT_EQ(lrvx_segment_validate(garbage.c_str()), 0);
  EXPECT_EQ(lrvx_last_error_code(), 0) << lastError();
  const LrvxSegmentValidation full = lrvx_segment_validate_full(garbage.c_str(), 1, 1);
  EXPECT_EQ(full.valid, 0);
  EXPECT_EQ(lrvx_last_error_code(), 0) << lastError();
}

TEST_F(CapiPathErrors, MergeSaysWhyNothingWasMerged)
{
  const std::string out = (_root / "merged").string();

  EXPECT_EQ(lrvx_segment_merge(_missing.c_str(), out.c_str()), 0);
  expectReason(_missing);
  expectReason("lrvx_segment_merge");

  lrvx_clear_last_error();
  const auto empty = _root / "empty";
  std::filesystem::create_directories(empty);
  EXPECT_EQ(lrvx_segment_merge(empty.string().c_str(), out.c_str()), 0);
  expectReason(empty.string());
}

TEST_F(CapiPathErrors, AMergeThatWorkedLeavesNoError)
{
  const auto dir = _root / "tape";
  LrvxDataWriterHandle w = lrvx_data_writer_create(dir.string().c_str(), 16, 1);
  ASSERT_NE(w, nullptr);
  for (int i = 0; i < 5; ++i)
  {
    ASSERT_EQ(lrvx_data_writer_write_trade(w, (10 + i) * kSec, (10 + i) * kSec, 100.0 + i, 1.0,
                                           static_cast<uint64_t>(i + 1), 1, 0),
              1);
  }
  lrvx_data_writer_close(w);
  lrvx_data_writer_destroy(w);
  lrvx_clear_last_error();

  EXPECT_EQ(lrvx_segment_merge(dir.string().c_str(), (_root / "merged").string().c_str()), 1);
  EXPECT_EQ(lrvx_last_error_code(), 0) << lastError();
  for (const auto& entry : std::filesystem::directory_iterator(dir))
  {
    if (entry.path().extension() != ".idx")
    {
      EXPECT_EQ(lrvx_segment_validate(entry.path().string().c_str()), 1);
      EXPECT_EQ(lrvx_last_error_code(), 0) << lastError();
    }
  }
}

TEST_F(CapiPathErrors, PartitionerNeedsADirectoryArgument)
{
  EXPECT_EQ(lrvx_partitioner_create(nullptr), nullptr);
  expectReason("data_dir");
}

// An unknown symbol name used to resolve to id 0 and the run went ahead
// against it.
struct Counter
{
  int trades{0};
  int bars{0};
};

void countTrade(void* ud, const LrvxSymbolContext*, const LrvxTradeData*)
{
  ++static_cast<Counter*>(ud)->trades;
}

void countBar(void* ud, const LrvxSymbolContext*, const LrvxBarData*)
{
  ++static_cast<Counter*>(ud)->bars;
}

class CapiRunnerUnknownSymbol : public CapiPathErrors
{
 protected:
  void SetUp() override
  {
    CapiPathErrors::SetUp();
    LrvxStrategyCallbacks cb{};
    cb.on_trade = countTrade;
    cb.on_bar = countBar;
    cb.user_data = &_counter;
    uint32_t syms[] = {_symbol};
    _strategy = lrvx_strategy_create(1, syms, 1, _registry, cb);
    _runner = lrvx_backtest_runner_create(_registry, 0.0, 1000.0);
    lrvx_backtest_runner_set_strategy(_runner, _strategy);
    lrvx_clear_last_error();
  }

  void TearDown() override
  {
    lrvx_backtest_runner_destroy(_runner);
    lrvx_strategy_destroy(_strategy);
    CapiPathErrors::TearDown();
  }

  void expectRefused(int ok) const
  {
    EXPECT_EQ(ok, 0);
    expectReason("NOPE");
    expectReason("not registered");
    EXPECT_EQ(_counter.trades, 0);
    EXPECT_EQ(_counter.bars, 0);
  }

  Counter _counter;
  LrvxStrategyHandle _strategy{nullptr};
  LrvxBacktestRunnerHandle _runner{nullptr};
};

// No name means the first symbol registered, whatever order the registry's
// map walks its entries in.
TEST(CapiRunnerDefaultSymbol, IsTheFirstRegistered)
{
  LrvxRegistryHandle registry = lrvx_registry_create();
  std::vector<uint32_t> ids;
  for (const char* name : {"AAA", "BBB", "CCC", "DDD", "EEE", "FFF", "GGG"})
  {
    ids.push_back(lrvx_registry_add_symbol(registry, "test", name, 0.01));
  }
  struct Seen
  {
    std::vector<uint32_t> symbols;
  } seen;
  LrvxStrategyCallbacks scb{};
  scb.user_data = &seen;
  scb.on_trade = +[](void* ud, const LrvxSymbolContext*, const LrvxTradeData* t)
  {
    static_cast<Seen*>(ud)->symbols.push_back(t->symbol);
  };
  LrvxStrategyHandle strategy = lrvx_strategy_create(
      1, ids.data(), static_cast<uint32_t>(ids.size()), registry, scb);
  LrvxBacktestRunnerHandle runner = lrvx_backtest_runner_create(registry, 0.0, 10000.0);
  lrvx_backtest_runner_set_strategy(runner, strategy);

  const int64_t ts[3] = {kSec, 2 * kSec, 3 * kSec};
  const double close[3] = {100.0, 101.0, 102.0};
  LrvxBacktestStats stats{};
  for (const char* symbol : {static_cast<const char*>(nullptr), ""})
  {
    seen.symbols.clear();
    ASSERT_EQ(lrvx_backtest_runner_run_ohlcv(runner, ts, close, 3, symbol, &stats), 1)
        << lrvx_last_error_message();
    ASSERT_EQ(seen.symbols.size(), 3u);
    for (uint32_t s : seen.symbols)
    {
      EXPECT_EQ(s, ids.front());
    }
  }

  lrvx_backtest_runner_destroy(runner);
  lrvx_strategy_destroy(strategy);
  lrvx_registry_destroy(registry);
}

TEST_F(CapiRunnerUnknownSymbol, RunCsv)
{
  const std::string csv =
      writeCsv("ok.csv", "timestamp,open,high,low,close,volume\n1,1,1,1,100,1\n2,1,1,1,101,1\n");
  LrvxBacktestStats stats{};
  expectRefused(lrvx_backtest_runner_run_csv(_runner, csv.c_str(), "NOPE", &stats));

  lrvx_clear_last_error();
  EXPECT_EQ(lrvx_backtest_runner_run_csv(_runner, csv.c_str(), "BTCUSDT", &stats), 1);
  EXPECT_EQ(lrvx_last_error_code(), 0) << lastError();
  EXPECT_EQ(_counter.trades, 2);
}

TEST_F(CapiRunnerUnknownSymbol, RunOhlcv)
{
  const int64_t ts[] = {1 * kSec, 2 * kSec};
  const double close[] = {100.0, 101.0};
  LrvxBacktestStats stats{};
  expectRefused(lrvx_backtest_runner_run_ohlcv(_runner, ts, close, 2, "NOPE", &stats));

  // No name still means the registry's first symbol.
  lrvx_clear_last_error();
  EXPECT_EQ(lrvx_backtest_runner_run_ohlcv(_runner, ts, close, 2, "", &stats), 1);
  EXPECT_EQ(_counter.trades, 2);
}

TEST_F(CapiRunnerUnknownSymbol, RunBars)
{
  const int64_t start[] = {1 * kSec, 2 * kSec};
  const int64_t end[] = {2 * kSec, 3 * kSec};
  const double px[] = {100.0, 101.0};
  LrvxBacktestStats stats{};
  expectRefused(lrvx_backtest_runner_run_bars(_runner, start, end, px, px, px, px, nullptr, 2,
                                              "NOPE", 0, 0, &stats));

  lrvx_clear_last_error();
  EXPECT_EQ(lrvx_backtest_runner_run_bars(_runner, start, end, px, px, px, px, nullptr, 2,
                                          "BTCUSDT", 0, 0, &stats),
            1);
  EXPECT_EQ(_counter.bars, 2);
}

}  // namespace
