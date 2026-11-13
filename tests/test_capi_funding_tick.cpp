/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2026 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// lrvx_funding_schedule_tick asked for a count must leave the settlements
// due, and lrvx_funding_schedule_tick_exact hands the timestamps over as
// integers. The schedule and the expected payments are the ones every
// binding's funding test uses.

#include "lrvx/backtest/funding_schedule.h"
#include "lrvx/capi/lrvx_capi.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace
{

// Not a multiple of 256: a double cannot hold either exactly.
constexpr int64_t kT1 = 1'700'000'000'123'456'789LL;
constexpr int64_t kT2 = 1'700'000'028'800'000'001LL;

struct Schedule
{
  LrvxFundingScheduleHandle h{lrvx_funding_schedule_create()};
  uint32_t symbols[2] = {1, 2};
  double positions[2] = {2.0, -3.0};
  double marks[2] = {50000.0, 2000.0};

  Schedule()
  {
    const int64_t ts[3] = {kT1, kT1, kT2};
    const uint32_t sy[3] = {1, 2, 0};
    const double rates[3] = {0.0001, -0.0002, 0.0003};
    lrvx_funding_schedule_set_tape_by_symbol(h, ts, sy, rates, 3);
  }
  ~Schedule() { lrvx_funding_schedule_destroy(h); }
};

void expectTheFourPayments(const std::vector<int64_t>& ts, const std::vector<double>& buf)
{
  ASSERT_EQ(ts.size(), 4u);
  const int64_t wantTs[4] = {kT1, kT1, kT2, kT2};
  const double wantSymbol[4] = {1, 2, 1, 2};
  const double wantRate[4] = {0.0001, -0.0002, 0.0003, 0.0003};
  const double wantMark[4] = {50000.0, 2000.0, 50000.0, 2000.0};
  const double wantPos[4] = {2.0, -3.0, 2.0, -3.0};
  for (size_t i = 0; i < 4; ++i)
  {
    EXPECT_EQ(ts[i], wantTs[i]) << i;
    EXPECT_EQ(buf[i * 6 + 1], wantSymbol[i]) << i;
    EXPECT_DOUBLE_EQ(buf[i * 6 + 2], wantRate[i]) << i;
    EXPECT_DOUBLE_EQ(buf[i * 6 + 3], wantMark[i]) << i;
    EXPECT_DOUBLE_EQ(buf[i * 6 + 4], wantPos[i]) << i;
    EXPECT_DOUBLE_EQ(buf[i * 6 + 5], -wantPos[i] * wantMark[i] * wantRate[i]) << i;
  }
}

}  // namespace

// The count probe used to settle the events it counted, so the call that
// came for them found none: zero events, or a buffer of zeros.
TEST(CapiFundingTick, ACountProbeLeavesTheSettlementsDue)
{
  Schedule s;
  const uint32_t count =
      lrvx_funding_schedule_tick(s.h, kT2, s.symbols, s.positions, s.marks, 2, nullptr, 0);
  ASSERT_EQ(count, 4u);

  std::vector<double> buf(count * 6, 0.0);
  const uint32_t written = lrvx_funding_schedule_tick(s.h, kT2, s.symbols, s.positions, s.marks, 2,
                                                      buf.data(), count);
  ASSERT_EQ(written, 4u);
  EXPECT_DOUBLE_EQ(buf[2], 0.0001);
  EXPECT_DOUBLE_EQ(buf[5], -2.0 * 50000.0 * 0.0001);
  EXPECT_DOUBLE_EQ(buf[3 * 6 + 5], 3.0 * 2000.0 * 0.0003);

  // Taken once: the next tick at the same instant has nothing left.
  EXPECT_EQ(lrvx_funding_schedule_tick(s.h, kT2, s.symbols, s.positions, s.marks, 2, buf.data(),
                                       count),
            0u);
}

TEST(CapiFundingTick, AProbeDoesNotSeedAnIntervalSchedule)
{
  LrvxFundingScheduleHandle h = lrvx_funding_schedule_create();
  const int64_t hour = 3600LL * 1'000'000'000LL;
  lrvx_funding_schedule_set_constant(h, hour, 0.001);
  const uint32_t sym[1] = {1};
  const double pos[1] = {1.0};
  const double mark[1] = {100.0};
  const int64_t now = 500'000 * hour + 5;

  ASSERT_EQ(lrvx_funding_schedule_tick(h, now, sym, pos, mark, 1, nullptr, 0), 1u);
  double buf[6] = {};
  ASSERT_EQ(lrvx_funding_schedule_tick(h, now, sym, pos, mark, 1, buf, 1), 1u);
  EXPECT_DOUBLE_EQ(buf[2], 0.001);
  EXPECT_DOUBLE_EQ(buf[5], -0.1);
  lrvx_funding_schedule_destroy(h);
}

TEST(CapiFundingTick, ExactHandsOverIntegerTimestampsInOneCall)
{
  Schedule s;
  std::vector<int64_t> ts(8, 0);
  std::vector<double> buf(8 * 6, 0.0);
  const uint32_t count = lrvx_funding_schedule_tick_exact(s.h, kT2, s.symbols, s.positions,
                                                          s.marks, 2, ts.data(), buf.data(), 8);
  ASSERT_EQ(count, 4u);
  ts.resize(count);
  expectTheFourPayments(ts, buf);
  // What slot 0 of the double layout can hold is not the timestamp.
  EXPECT_NE(static_cast<int64_t>(buf[0]), kT1);

  EXPECT_EQ(lrvx_funding_schedule_tick_exact(s.h, kT2, s.symbols, s.positions, s.marks, 2,
                                             ts.data(), buf.data(), 8),
            0u);
}

TEST(CapiFundingTick, ExactWithTooLittleRoomSettlesNothing)
{
  Schedule s;
  std::vector<int64_t> ts(2, -1);
  std::vector<double> buf(2 * 6, -1.0);
  ASSERT_EQ(lrvx_funding_schedule_tick_exact(s.h, kT2, s.symbols, s.positions, s.marks, 2,
                                             ts.data(), buf.data(), 2),
            4u);
  EXPECT_EQ(ts[0], -1) << "nothing is written when the events do not fit";
  EXPECT_EQ(buf[0], -1.0);

  ts.assign(4, 0);
  buf.assign(4 * 6, 0.0);
  ASSERT_EQ(lrvx_funding_schedule_tick_exact(s.h, kT2, s.symbols, s.positions, s.marks, 2,
                                             ts.data(), buf.data(), 4),
            4u);
  expectTheFourPayments(ts, buf);
}

TEST(CapiFundingTick, TheClassAndTheCApiAgree)
{
  Schedule s;
  std::vector<lrvx::FundingTapeEntry> entries(3);
  entries[0] = {lrvx::UnixNanos::fromRaw(kT1), 1, 0.0001};
  entries[1] = {lrvx::UnixNanos::fromRaw(kT1), 2, -0.0002};
  entries[2] = {lrvx::UnixNanos::fromRaw(kT2), 0, 0.0003};
  auto direct = lrvx::FundingSchedule::tapeBySymbol(entries);
  const auto want = direct.tick(kT2, {1, 2}, {2.0, -3.0}, {50000.0, 2000.0});

  std::vector<int64_t> ts(4, 0);
  std::vector<double> buf(4 * 6, 0.0);
  ASSERT_EQ(lrvx_funding_schedule_tick_exact(s.h, kT2, s.symbols, s.positions, s.marks, 2,
                                             ts.data(), buf.data(), 4),
            want.size());
  for (size_t i = 0; i < want.size(); ++i)
  {
    EXPECT_EQ(ts[i], want[i].timestampNs.raw());
    EXPECT_EQ(buf[i * 6 + 1], static_cast<double>(want[i].symbol));
    EXPECT_EQ(buf[i * 6 + 2], want[i].rate);
    EXPECT_EQ(buf[i * 6 + 5], want[i].amount);
  }
}
