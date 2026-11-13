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
