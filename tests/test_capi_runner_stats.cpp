/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// What the backtest runner's run_* calls hand back through the C API: every
// field of LrvxBacktestStats on success, and a reason in
// lrvx_last_error_message on failure.

#include "lrvx/capi/lrvx_capi.h"
#include "lrvx/replay/binary_format_v1.h"
#include "lrvx/replay/recording_metadata.h"
#include "lrvx/replay/writers/binary_log_writer.h"

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace
{

constexpr int64_t kSec = 1'000'000'000;
constexpr int64_t kT0 = 1'767'225'600LL * kSec;
constexpr int kTrades = 20;

// Buys on the first trade, sells on the tenth: one closed trade nine seconds
// long on a rising tape.
struct RoundTrip
{
  LrvxStrategyHandle strategy{nullptr};
  uint32_t symbol{0};
  int seen{0};
};

void onTrade(void* ud, const LrvxSymbolContext*, const LrvxTradeData*)
{
  auto* s = static_cast<RoundTrip*>(ud);
  ++s->seen;
  if (s->seen == 1)
  {
    lrvx_emit_market_buy(s->strategy, s->symbol, 100'000'000);
  }
  else if (s->seen == 10)
  {
    lrvx_emit_market_sell(s->strategy, s->symbol, 100'000'000);
  }
}

class CapiRunnerStats : public ::testing::Test
{
 protected:
  void SetUp() override
  {
    _root = std::filesystem::temp_directory_path() /
            (std::string("lrvx_capi_runner_stats_") +
             ::testing::UnitTest::GetInstance()->current_test_info()->name() + "_" +
             std::to_string(std::random_device{}()));
    std::filesystem::remove_all(_root);
    _tape = _root / "tape";
    std::filesystem::create_directories(_tape);

    lrvx::replay::WriterConfig cfg;
    cfg.output_dir = _tape;
    lrvx::replay::BinaryLogWriter writer(cfg);
    for (int i = 0; i < kTrades; ++i)
    {
      lrvx::replay::TradeRecord r{};
      r.exchange_ts_ns = kT0 + i * kSec;
      r.recv_ts_ns = r.exchange_ts_ns;
      r.price_raw = (100 + i) * 100'000'000LL;
      r.qty_raw = 100'000'000;
      r.symbol_id = 1;
      r.side = 0;
      r.trade_id = static_cast<uint64_t>(i + 1);
      writer.writeTrade(r);
    }
    writer.close();

    // run_tapes keys symbols by (exchange, name), which it reads here.
    lrvx::replay::RecordingMetadata meta{};
    meta.recording_id = "runner_stats";
    meta.exchange = "test";
    meta.has_trades = true;
    lrvx::replay::SymbolInfo sym{};
    sym.symbol_id = 1;
    sym.name = "AAA";
    meta.symbols.push_back(sym);
    meta.save(lrvx::replay::RecordingMetadata::metadataPath(_tape));

    _reg = lrvx_registry_create();
    ASSERT_EQ(lrvx_registry_add_symbol(_reg, "test", "AAA", 0.01), 1u);
    _btr = lrvx_backtest_runner_create(_reg, 0.0, 10'000.0);
    LrvxStrategyCallbacks cb{};
    cb.on_trade = onTrade;
    cb.user_data = &_state;
    uint32_t syms[] = {1};
    _state.symbol = 1;
    _state.strategy = lrvx_strategy_create(1, syms, 1, _reg, cb);
    lrvx_backtest_runner_set_strategy(_btr, _state.strategy);
    lrvx_clear_last_error();
  }

  void TearDown() override
  {
    lrvx_backtest_runner_destroy(_btr);
    lrvx_strategy_destroy(_state.strategy);
    lrvx_registry_destroy(_reg);
    std::filesystem::remove_all(_root);
  }

  // Every byte set, so a field the call never writes cannot pass for a zero.
  static LrvxBacktestStats poisoned()
  {
    LrvxBacktestStats s;
    std::memset(&s, 0xAA, sizeof(s));
    return s;
  }

  // The runner's stats against the stats of the result the same run left
  // behind, which is the complete reference.
  void expectCompleteStats(const LrvxBacktestStats& got)
  {
    LrvxBacktestResultHandle res = lrvx_backtest_runner_take_result(_btr);
    ASSERT_NE(res, nullptr);
    LrvxBacktestStats want{};
    lrvx_backtest_result_stats(res, &want);
    lrvx_backtest_result_destroy(res);

    ASSERT_EQ(want.totalTrades, 1u);
    EXPECT_EQ(got.totalTrades, want.totalTrades);
    EXPECT_EQ(got.netPnl, want.netPnl);

    EXPECT_EQ(want.avgTradeDurationNs, 9.0 * kSec);
    EXPECT_EQ(got.avgTradeDurationNs, want.avgTradeDurationNs);
    EXPECT_EQ(got.medianTradeDurationNs, want.medianTradeDurationNs);
    EXPECT_EQ(got.maxTradeDurationNs, want.maxTradeDurationNs);
    EXPECT_NE(want.timeWeightedReturn, 0.0);
    EXPECT_EQ(got.timeWeightedReturn, want.timeWeightedReturn);
    EXPECT_GT(want.startTimeNs, 0);
    EXPECT_GT(want.endTimeNs, want.startTimeNs);
    EXPECT_EQ(got.startTimeNs, want.startTimeNs);
    EXPECT_EQ(got.endTimeNs, want.endTimeNs);
    EXPECT_EQ(got.optionQuoteCount, 0u);
    EXPECT_EQ(got.poolStateCount, 0u);
  }

  std::filesystem::path _root;
  std::filesystem::path _tape;
  LrvxRegistryHandle _reg{nullptr};
  LrvxBacktestRunnerHandle _btr{nullptr};
  RoundTrip _state;
};

}  // namespace

TEST_F(CapiRunnerStats, RunTapeWritesEveryField)
{
  LrvxBacktestStats stats = poisoned();
  const std::string tape = _tape.string();
  ASSERT_EQ(lrvx_backtest_runner_run_tape(_btr, tape.c_str(), &stats), 1)
      << lrvx_last_error_message();
  expectCompleteStats(stats);
}

TEST_F(CapiRunnerStats, RunTapesWritesEveryField)
{
  LrvxBacktestStats stats = poisoned();
  const std::string tape = _tape.string();
  const char* dirs[] = {tape.c_str()};
  ASSERT_EQ(lrvx_backtest_runner_run_tapes(_btr, dirs, 1, &stats), 1)
      << lrvx_last_error_message();
  expectCompleteStats(stats);
}

TEST_F(CapiRunnerStats, ResultStatsWritesEveryField)
{
  const std::string tape = _tape.string();
  ASSERT_EQ(lrvx_backtest_runner_run_tape(_btr, tape.c_str(), nullptr), 1);
  LrvxBacktestResultHandle res = lrvx_backtest_runner_take_result(_btr);
  ASSERT_NE(res, nullptr);
  LrvxBacktestStats stats = poisoned();
  lrvx_backtest_result_stats(res, &stats);
  lrvx_backtest_result_destroy(res);
  EXPECT_EQ(stats.totalTrades, 1u);
  EXPECT_EQ(stats.optionQuoteCount, 0u);
  EXPECT_EQ(stats.poolStateCount, 0u);
}

TEST_F(CapiRunnerStats, RunTapeFailureSaysWhy)
{
  const std::string missing = (_root / "no_such_tape").string();
  LrvxBacktestStats stats{};
  EXPECT_EQ(lrvx_backtest_runner_run_tape(_btr, missing.c_str(), &stats), 0);
  EXPECT_EQ(lrvx_last_error_code(), 2);
  const std::string msg = lrvx_last_error_message();
  EXPECT_NE(msg.find("lrvx_backtest_runner_run_tape"), std::string::npos) << msg;
  EXPECT_NE(msg.find("does not exist"), std::string::npos) << msg;
  EXPECT_NE(msg.find(missing), std::string::npos) << msg;

  lrvx_clear_last_error();
  EXPECT_EQ(lrvx_backtest_runner_run_tape(_btr, nullptr, &stats), 0);
  EXPECT_EQ(lrvx_last_error_code(), 1);
  EXPECT_NE(std::string(lrvx_last_error_message()).find("tape_dir"), std::string::npos)
      << lrvx_last_error_message();
}

TEST_F(CapiRunnerStats, RunTapesFailureSaysWhy)
{
  const std::string tape = _tape.string();
  const std::string missing = (_root / "no_such_tape").string();
  LrvxBacktestStats stats{};

  const char* dirs[] = {tape.c_str(), missing.c_str()};
  EXPECT_EQ(lrvx_backtest_runner_run_tapes(_btr, dirs, 2, &stats), 0);
  EXPECT_EQ(lrvx_last_error_code(), 2);
  std::string msg = lrvx_last_error_message();
  EXPECT_NE(msg.find("lrvx_backtest_runner_run_tapes"), std::string::npos) << msg;
  EXPECT_NE(msg.find("not a directory"), std::string::npos) << msg;
  EXPECT_NE(msg.find(missing), std::string::npos) << msg;

  lrvx_clear_last_error();
  EXPECT_EQ(lrvx_backtest_runner_run_tapes(_btr, dirs, 0, &stats), 0);
  EXPECT_EQ(lrvx_last_error_code(), 2);
  msg = lrvx_last_error_message();
  EXPECT_NE(msg.find("empty"), std::string::npos) << msg;

  lrvx_clear_last_error();
  EXPECT_EQ(lrvx_backtest_runner_run_tapes(_btr, nullptr, 1, &stats), 0);
  EXPECT_EQ(lrvx_last_error_code(), 1);
  EXPECT_NE(std::string(lrvx_last_error_message()).find("tape_dirs"), std::string::npos)
      << lrvx_last_error_message();

  // A NULL entry is a path that is not there, not a crash.
  lrvx_clear_last_error();
  const char* withNull[] = {tape.c_str(), nullptr};
  EXPECT_EQ(lrvx_backtest_runner_run_tapes(_btr, withNull, 2, &stats), 0);
  EXPECT_EQ(lrvx_last_error_code(), 2);
}

TEST_F(CapiRunnerStats, RunCsvFailureSaysWhy)
{
  const std::string missing = (_root / "no_such.csv").string();
  LrvxBacktestStats stats{};
  EXPECT_EQ(lrvx_backtest_runner_run_csv(_btr, missing.c_str(), "AAA", &stats), 0);
  EXPECT_EQ(lrvx_last_error_code(), 2);
  std::string msg = lrvx_last_error_message();
  EXPECT_NE(msg.find("lrvx_backtest_runner_run_csv"), std::string::npos) << msg;
  EXPECT_NE(msg.find("cannot open"), std::string::npos) << msg;
  EXPECT_NE(msg.find(missing), std::string::npos) << msg;

  // A row that is not a bar names the file and the line it is on.
  const std::string bad = (_root / "bad.csv").string();
  {
    std::ofstream f(bad);
    f << "timestamp,open,high,low,close,volume\n"
      << "1767225600,100,101,99,100,1\n"
      << "not-a-timestamp,100,101,99,100,1\n";
  }
  lrvx_clear_last_error();
  EXPECT_EQ(lrvx_backtest_runner_run_csv(_btr, bad.c_str(), "AAA", &stats), 0);
  EXPECT_EQ(lrvx_last_error_code(), 2);
  msg = lrvx_last_error_message();
  EXPECT_NE(msg.find(bad), std::string::npos) << msg;
  EXPECT_NE(msg.find("line 3"), std::string::npos) << msg;

  // A readable file still runs.
  const std::string good = (_root / "good.csv").string();
  {
    std::ofstream f(good);
    f << "timestamp,open,high,low,close,volume\n"
      << "1767225600,100,101,99,100,1\n"
      << "1767225660,100,101,99,101,1\n";
  }
  lrvx_clear_last_error();
  EXPECT_EQ(lrvx_backtest_runner_run_csv(_btr, good.c_str(), "AAA", &stats), 1)
      << lrvx_last_error_message();
  EXPECT_EQ(lrvx_last_error_code(), 0);
}

TEST_F(CapiRunnerStats, RunCsvNullPathSaysWhy)
{
  LrvxBacktestStats stats{};
  EXPECT_EQ(lrvx_backtest_runner_run_csv(_btr, nullptr, "AAA", &stats), 0);
  EXPECT_EQ(lrvx_last_error_code(), 1);
  EXPECT_NE(std::string(lrvx_last_error_message()).find("'path'"), std::string::npos)
      << lrvx_last_error_message();
}

TEST_F(CapiRunnerStats, RunReplaySourceFailureSaysWhy)
{
  LrvxBacktestStats stats{};
  EXPECT_EQ(lrvx_backtest_runner_run_replay_source(_btr, nullptr, &stats), 0);
  EXPECT_EQ(lrvx_last_error_code(), 1);
  const std::string msg = lrvx_last_error_message();
  EXPECT_NE(msg.find("lrvx_backtest_runner_run_replay_source"), std::string::npos) << msg;
  EXPECT_NE(msg.find("'source'"), std::string::npos) << msg;
}

TEST_F(CapiRunnerStats, RunOhlcvFailureSaysWhy)
{
  LrvxBacktestStats stats{};
  const int64_t ts[] = {kT0, kT0 + kSec};
  const double closes[] = {100.0, 101.0};

  EXPECT_EQ(lrvx_backtest_runner_run_ohlcv(_btr, nullptr, closes, 2, "AAA", &stats), 0);
  EXPECT_EQ(lrvx_last_error_code(), 1);
  EXPECT_NE(std::string(lrvx_last_error_message()).find("'timestamps_ns'"), std::string::npos)
      << lrvx_last_error_message();

  lrvx_clear_last_error();
  EXPECT_EQ(lrvx_backtest_runner_run_ohlcv(_btr, ts, nullptr, 2, "AAA", &stats), 0);
  EXPECT_EQ(lrvx_last_error_code(), 1);
  EXPECT_NE(std::string(lrvx_last_error_message()).find("'close_prices'"), std::string::npos)
      << lrvx_last_error_message();

  // No bars need no arrays.
  lrvx_clear_last_error();
  EXPECT_EQ(lrvx_backtest_runner_run_ohlcv(_btr, nullptr, nullptr, 0, "AAA", &stats), 1)
      << lrvx_last_error_message();
  EXPECT_EQ(lrvx_last_error_code(), 0);
}

TEST_F(CapiRunnerStats, RunBarsFailureSaysWhy)
{
  LrvxBacktestStats stats{};
  const int64_t starts[] = {kT0};
  const int64_t ends[] = {kT0 + kSec};
  const double px[] = {100.0};

  struct Case
  {
    const int64_t* start;
    const int64_t* end;
    const double* open;
    const double* high;
    const double* low;
    const double* close;
    const char* name;
  };
  const Case cases[] = {
      {nullptr, ends, px, px, px, px, "start_time_ns"},
      {starts, nullptr, px, px, px, px, "end_time_ns"},
      {starts, ends, nullptr, px, px, px, "open"},
      {starts, ends, px, nullptr, px, px, "high"},
      {starts, ends, px, px, nullptr, px, "low"},
      {starts, ends, px, px, px, nullptr, "close"},
  };
  for (const Case& c : cases)
  {
    lrvx_clear_last_error();
    EXPECT_EQ(lrvx_backtest_runner_run_bars(_btr, c.start, c.end, c.open, c.high, c.low, c.close,
                                            px, 1, "AAA", 0, 0, &stats),
              0)
        << c.name;
    EXPECT_EQ(lrvx_last_error_code(), 1) << c.name;
    EXPECT_NE(std::string(lrvx_last_error_message()).find(std::string("'") + c.name + "'"),
              std::string::npos)
        << lrvx_last_error_message();
  }

  // The volume column is optional.
  lrvx_clear_last_error();
  EXPECT_EQ(lrvx_backtest_runner_run_bars(_btr, starts, ends, px, px, px, px, nullptr, 1, "AAA",
                                          0, 0, &stats),
            1)
      << lrvx_last_error_message();
  EXPECT_EQ(lrvx_last_error_code(), 0);
}
