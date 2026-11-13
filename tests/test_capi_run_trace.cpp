/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2026 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Signal ids through the C API. A signal a runner captures carries its order
// id, (subscriber id << 32) + n, and the trace kept its low 32 bits only.

#include "lrvx/capi/lrvx_capi.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace
{

std::filesystem::path scratch(const std::string& tag)
{
  auto p = std::filesystem::temp_directory_path() / ("lrvx_capi_run_trace_" + tag);
  std::filesystem::remove_all(p);
  return p;
}

void noopSignal(void*, const LrvxSignal*) {}

}  // namespace

TEST(CapiRunTrace, ACapturedSignalReadsBackWithItsWholeOrderId)
{
  const auto dir = scratch("capture");
  LrvxRegistryHandle registry = lrvx_registry_create();
  const uint32_t symbol = lrvx_registry_add_symbol(registry, "test", "BTC", 0.01);
  LrvxStrategyCallbacks scb{};
  uint32_t syms[] = {symbol};
  LrvxStrategyHandle strategy = lrvx_strategy_create(7, syms, 1, registry, scb);
  LrvxRunnerHandle runner = lrvx_runner_create(registry, noopSignal, nullptr);
  lrvx_runner_add_strategy(runner, strategy);
  LrvxRunRecorderHandle recorder = lrvx_run_recorder_create(dir.string().c_str(), "s", "h", 1);
  ASSERT_NE(recorder, nullptr) << lrvx_last_error_message();
  lrvx_runner_attach_trace_recorder(runner, recorder);

  const uint64_t first = lrvx_emit_limit_sell(strategy, symbol, 10100000000LL, 100000000LL);
  const uint64_t second = lrvx_emit_limit_buy(strategy, symbol, 9900000000LL, 100000000LL);
  ASSERT_EQ(first, (uint64_t{7} << 32) + 1);
  ASSERT_EQ(second, (uint64_t{7} << 32) + 2);
  lrvx_emit_cancel(strategy, first);

  lrvx_runner_attach_trace_recorder(runner, nullptr);
  lrvx_run_recorder_close(recorder);
  lrvx_run_recorder_destroy(recorder);

  LrvxRunReaderHandle reader = lrvx_run_reader_open(dir.string().c_str());
  ASSERT_NE(reader, nullptr) << lrvx_last_error_message();
  ASSERT_EQ(lrvx_run_reader_signal_count(reader), 3u);
  EXPECT_EQ(lrvx_run_reader_signal_id64(reader, 0), first);
  EXPECT_EQ(lrvx_run_reader_signal_id64(reader, 1), second);
  EXPECT_EQ(lrvx_run_reader_signal_id64(reader, 2), first) << "the cancel names the order";

  // The 32-bit getter still reports the low half.
  uint32_t low = 0;
  lrvx_run_reader_signal_header(reader, 1, nullptr, nullptr, &low, nullptr, nullptr, nullptr,
                                nullptr, nullptr);
  EXPECT_EQ(low, 2u);

  EXPECT_EQ(lrvx_run_reader_signal_id64(reader, 3), 0u) << "out of range";
  lrvx_run_reader_close(reader);

  lrvx_runner_destroy(runner);
  lrvx_strategy_destroy(strategy);
  lrvx_registry_destroy(registry);
  std::filesystem::remove_all(dir);
}

TEST(CapiRunTrace, SignalId64OfAWrittenRecordAndOfANullHandle)
{
  const auto dir = scratch("written");
  LrvxRunRecorderHandle recorder = lrvx_run_recorder_create(dir.string().c_str(), "s", "h", 1);
  ASSERT_NE(recorder, nullptr) << lrvx_last_error_message();
  const uint32_t syms[] = {1};
  lrvx_run_recorder_write_signal(recorder, 10, 0, /*signal_id=*/42, 0, 0, "x", 1, syms, 1, nullptr,
                                 0);
  lrvx_run_recorder_close(recorder);
  lrvx_run_recorder_destroy(recorder);

  LrvxRunReaderHandle reader = lrvx_run_reader_open(dir.string().c_str());
  ASSERT_NE(reader, nullptr) << lrvx_last_error_message();
  EXPECT_EQ(lrvx_run_reader_signal_id64(reader, 0), 42u);
  lrvx_run_reader_close(reader);

  lrvx_clear_last_error();
  EXPECT_EQ(lrvx_run_reader_signal_id64(nullptr, 0), 0u);
  EXPECT_NE(lrvx_last_error_code(), 0);
  lrvx_clear_last_error();
  std::filesystem::remove_all(dir);
}

namespace
{

// The signal frames of a trace: its first segment without the 64-byte
// header, which carries the wall clock at which the segment was opened.
std::string signalFrames(const std::filesystem::path& dir)
{
  std::ifstream in(dir / "signals-000000.bin", std::ios::binary);
  std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return all.size() > 64 ? all.substr(64) : std::string{};
}

}  // namespace

TEST(CapiRunTrace, WriteSignal64KeepsAnIdAbove32Bits)
{
  const auto dir = scratch("wide");
  const uint64_t wide = (uint64_t{7} << 32) + 5;
  LrvxRunRecorderHandle recorder = lrvx_run_recorder_create(dir.string().c_str(), "s", "h", 1);
  ASSERT_NE(recorder, nullptr) << lrvx_last_error_message();
  const uint32_t syms[] = {1};
  lrvx_run_recorder_write_signal64(recorder, 10, 0, wide, 0, 0, "x", 1, syms, 1, nullptr, 0);
  lrvx_run_recorder_close(recorder);
  lrvx_run_recorder_destroy(recorder);

  LrvxRunReaderHandle reader = lrvx_run_reader_open(dir.string().c_str());
  ASSERT_NE(reader, nullptr) << lrvx_last_error_message();
  EXPECT_EQ(lrvx_run_reader_signal_id64(reader, 0), wide);
  int64_t run_ts = 0, feed_ts = 0, strength = 0;
  uint32_t low = 0, flags = 0;
  uint64_t name_len = 0, symbol_count = 0, payload_len = 0;
  lrvx_run_reader_signal_header(reader, 0, &run_ts, &feed_ts, &low, &flags, &strength, &name_len,
                                &symbol_count, &payload_len);
  EXPECT_EQ(low, 5u);
  lrvx_run_reader_close(reader);
  std::filesystem::remove_all(dir);
}

TEST(CapiRunTrace, WriteSignal64OfASmallIdWritesTheBytesWriteSignalDoes)
{
  const auto narrow = scratch("narrow_bytes");
  const auto wide = scratch("wide_bytes");
  const uint32_t syms[] = {1, 2};
  const uint8_t payload[] = {9, 8, 7};

  LrvxRunRecorderHandle a = lrvx_run_recorder_create(narrow.string().c_str(), "s", "h", 1);
  ASSERT_NE(a, nullptr) << lrvx_last_error_message();
  lrvx_run_recorder_write_signal(a, 10, 9, 0xFFFFFFFFu, 3, -4, "name", 4, syms, 2, payload, 3);
  lrvx_run_recorder_close(a);
  lrvx_run_recorder_destroy(a);

  LrvxRunRecorderHandle b = lrvx_run_recorder_create(wide.string().c_str(), "s", "h", 1);
  ASSERT_NE(b, nullptr) << lrvx_last_error_message();
  lrvx_run_recorder_write_signal64(b, 10, 9, 0xFFFFFFFFull, 3, -4, "name", 4, syms, 2, payload, 3);
  lrvx_run_recorder_close(b);
  lrvx_run_recorder_destroy(b);

  const std::string first = signalFrames(narrow);
  ASSERT_FALSE(first.empty());
  EXPECT_EQ(first, signalFrames(wide));
  std::filesystem::remove_all(narrow);
  std::filesystem::remove_all(wide);
}

TEST(CapiRunTrace, WriteSignal64OnANullHandleLeavesAReason)
{
  lrvx_clear_last_error();
  lrvx_run_recorder_write_signal64(nullptr, 1, 0, 1, 0, 0, nullptr, 0, nullptr, 0, nullptr, 0);
  EXPECT_NE(lrvx_last_error_code(), 0);
  lrvx_clear_last_error();
}
