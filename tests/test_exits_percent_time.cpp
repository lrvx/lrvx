/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2026 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// PercentStop, TakeProfit and TimeStop. The levels below are pinned to the
// same numbers in python/tests/test_exits_percent_time.py and in the Node,
// QuickJS, Codon and C API exit tests.

#include "lrvx/strategy/exits.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <stdexcept>

using namespace lrvx;
using namespace lrvx::exits;

TEST(PercentStop, LongStopIsEntryTimesOneMinusPct)
{
  PercentStop stop(5.0);
  EXPECT_FALSE(stop.armed());
  EXPECT_TRUE(std::isnan(stop.stop()));
  EXPECT_FALSE(stop.update(101.0, 1.0)) << "nothing is watched before arm";
  EXPECT_DOUBLE_EQ(stop.pct(), 5.0);

  stop.arm(Side::BUY, 200.0);
  EXPECT_TRUE(stop.armed());
  EXPECT_DOUBLE_EQ(stop.stop(), 190.0);
  EXPECT_FALSE(stop.update(215.0, 195.0));
  EXPECT_FALSE(stop.update(250.0, 190.01)) << "a high far above does not matter to a long";
  EXPECT_DOUBLE_EQ(stop.stop(), 190.0) << "a percent stop does not move";
  EXPECT_TRUE(stop.update(212.0, 190.0)) << "low == stop is a hit";
  EXPECT_TRUE(stop.update(212.0, 190.0)) << "the same bar again is a hit again: a hit does not disarm";
  EXPECT_TRUE(stop.armed());
  // Per bar, like AtrStop: update() says whether this bar reached the
  // resting level, it does not latch an earlier hit.
  EXPECT_FALSE(stop.update(212.0, 191.0)) << "a later bar that does not reach the stop";
  EXPECT_TRUE(stop.armed());
  EXPECT_DOUBLE_EQ(stop.stop(), 190.0);

  stop.disarm();
  EXPECT_FALSE(stop.armed());
  EXPECT_TRUE(std::isnan(stop.stop()));
  EXPECT_FALSE(stop.update(100.0, 1.0)) << "disarmed: nothing is watched";
}

TEST(PercentStop, ShortStopIsEntryTimesOnePlusPct)
{
  PercentStop stop(5.0);
  stop.arm(Side::SELL, 200.0);
  EXPECT_DOUBLE_EQ(stop.stop(), 210.0);
  EXPECT_FALSE(stop.update(209.0, 195.0));
  EXPECT_FALSE(stop.update(209.99, 100.0)) << "a low far below does not matter to a short";
  EXPECT_TRUE(stop.update(210.0, 201.0)) << "high == stop is a hit";
}

TEST(PercentStop, FormulaIsPinnedAcrossBindings)
{
  // 100 * (1 + 2.5 / 100) is 102.49999999999999 in binary floating point; the
  // other bindings compute the same expression and pin the same bits.
  PercentStop stop(2.5);
  stop.arm(Side::SELL, 100.0);
  EXPECT_EQ(stop.stop(), 100.0 * (1.0 + 2.5 / 100.0));
  EXPECT_EQ(stop.stop(), 102.49999999999999);
  stop.arm(Side::BUY, 100.0);
  EXPECT_EQ(stop.stop(), 97.5);
  EXPECT_TRUE(stop.armed()) << "re-arming replaces the level";
}

TEST(PercentStop, RejectsNonPositivePct)
{
  EXPECT_THROW(PercentStop(0.0), std::invalid_argument);
  EXPECT_THROW(PercentStop(-1.0), std::invalid_argument);
  EXPECT_THROW((PercentStop(std::numeric_limits<double>::quiet_NaN())), std::invalid_argument);
  EXPECT_NO_THROW(PercentStop(0.0001));
}

TEST(TakeProfit, LongTargetIsEntryTimesOnePlusPct)
{
  TakeProfit tp(25.0);
  EXPECT_FALSE(tp.armed());
  EXPECT_TRUE(std::isnan(tp.target()));
  EXPECT_FALSE(tp.update(1000.0, 1.0)) << "nothing is watched before arm";
  EXPECT_DOUBLE_EQ(tp.pct(), 25.0);

  tp.arm(Side::BUY, 200.0);
  EXPECT_TRUE(tp.armed());
  EXPECT_DOUBLE_EQ(tp.target(), 250.0);
  EXPECT_FALSE(tp.update(249.0, 190.0));
  EXPECT_FALSE(tp.update(249.99, 1.0)) << "a low far below does not matter to a long target";
  EXPECT_TRUE(tp.update(250.0, 205.0)) << "high == target is a hit";
  EXPECT_TRUE(tp.update(250.0, 205.0)) << "the same bar again is a hit again: a hit does not disarm";
  EXPECT_FALSE(tp.update(240.0, 205.0)) << "per bar: a later bar below the target is no hit";
  EXPECT_TRUE(tp.armed());
  EXPECT_DOUBLE_EQ(tp.target(), 250.0);

  tp.disarm();
  EXPECT_FALSE(tp.armed());
  EXPECT_TRUE(std::isnan(tp.target()));
  EXPECT_FALSE(tp.update(1000.0, 1.0));
}

TEST(TakeProfit, ShortTargetIsEntryTimesOneMinusPct)
{
  TakeProfit tp(25.0);
  tp.arm(Side::SELL, 200.0);
  EXPECT_DOUBLE_EQ(tp.target(), 150.0);
  EXPECT_FALSE(tp.update(195.0, 151.0));
  EXPECT_FALSE(tp.update(1000.0, 150.01)) << "a high far above does not matter to a short target";
  EXPECT_TRUE(tp.update(190.0, 150.0)) << "low == target is a hit";
}

TEST(TakeProfit, RejectsNonPositivePct)
{
  EXPECT_THROW(TakeProfit(0.0), std::invalid_argument);
  EXPECT_THROW(TakeProfit(-25.0), std::invalid_argument);
  EXPECT_THROW((TakeProfit(std::numeric_limits<double>::quiet_NaN())), std::invalid_argument);
}

TEST(TimeStop, FiresOnTheMaxBarsThUpdate)
{
  TimeStop ts(3);
  EXPECT_EQ(ts.maxBars(), 3u);
  EXPECT_FALSE(ts.armed());
  EXPECT_FALSE(ts.update()) << "nothing is counted before arm";
  EXPECT_EQ(ts.bars(), 0u);

  ts.arm();
  EXPECT_TRUE(ts.armed());
  EXPECT_EQ(ts.bars(), 0u);
  EXPECT_FALSE(ts.update());
  EXPECT_EQ(ts.bars(), 1u);
  EXPECT_FALSE(ts.update());
  EXPECT_EQ(ts.bars(), 2u);
  EXPECT_TRUE(ts.update()) << "the third update is the third bar";
  EXPECT_EQ(ts.bars(), 3u);
  EXPECT_TRUE(ts.update()) << "a hit does not disarm";
  EXPECT_EQ(ts.bars(), 4u);
  EXPECT_TRUE(ts.armed());

  ts.disarm();
  EXPECT_FALSE(ts.armed());
  EXPECT_EQ(ts.bars(), 0u);
  EXPECT_FALSE(ts.update());

  ts.arm();
  EXPECT_FALSE(ts.update()) << "re-arming restarts the count";
  EXPECT_FALSE(ts.update());
  EXPECT_TRUE(ts.update());
}

TEST(TimeStop, OneBarFiresOnTheFirstUpdate)
{
  TimeStop ts(1);
  ts.arm();
  EXPECT_TRUE(ts.update());
}

TEST(TimeStop, ArmWhileArmedRestartsTheCount)
{
  TimeStop ts(2);
  ts.arm();
  EXPECT_FALSE(ts.update());
  ts.arm();
  EXPECT_EQ(ts.bars(), 0u);
  EXPECT_FALSE(ts.update());
  EXPECT_TRUE(ts.update());
}

TEST(TimeStop, RejectsZeroBars) { EXPECT_THROW(TimeStop(0), std::invalid_argument); }
