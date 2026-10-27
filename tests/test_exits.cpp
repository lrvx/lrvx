/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2026 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// The scenario below is pinned to the same levels in python/tests/test_exits.py,
// which runs lrvx.exits over the same bars: the two implementations are held
// to one answer.

#include "lrvx/strategy/exits.h"

#include <gtest/gtest.h>
#include <cmath>

using namespace lrvx;
using namespace lrvx::exits;

namespace
{

struct Bar
{
  double high, low, atr;
};

// Entry long at 100 with ATR 2. Bars: a drift up to a high of 110, then a
// drop to 97.
constexpr Bar kLongBars[] = {
    {103.0, 99.0, 2.0},
    {106.0, 102.0, 2.0},
    {110.0, 105.0, 2.5},
    {108.0, 104.0, 2.5},
    {105.0, 101.5, 2.5},
    {103.0, 97.0, 3.0},
};

}  // namespace

TEST(AtrStop, FrozenAtEntryMinusMultipleOfAtr)
{
  AtrStop stop(2.0);
  EXPECT_FALSE(stop.armed());
  EXPECT_FALSE(stop.update(101.0, 99.0, 2.0)) << "nothing is watched before arm";

  stop.arm(Side::BUY, 100.0, 2.0);
  EXPECT_TRUE(stop.armed());
  EXPECT_DOUBLE_EQ(stop.stop(), 96.0);
  bool hit = false;
  for (const Bar& b : kLongBars)
  {
    hit = stop.update(b.high, b.low, b.atr);
    EXPECT_DOUBLE_EQ(stop.stop(), 96.0) << "an ATR stop does not move";
    if (hit)
    {
      break;
    }
  }
  EXPECT_FALSE(hit) << "the lowest low, 97, stays above 96";

  stop.arm(Side::SELL, 100.0, 2.0);
  EXPECT_DOUBLE_EQ(stop.stop(), 104.0);
  EXPECT_FALSE(stop.update(103.0, 99.0, 2.0));
  EXPECT_TRUE(stop.update(104.0, 101.0, 2.0)) << "a short is stopped when high >= stop";
  EXPECT_TRUE(stop.update(104.0, 101.0, 2.0)) << "a hit is reported until disarm";
  stop.disarm();
  EXPECT_FALSE(stop.armed());
  EXPECT_TRUE(std::isnan(stop.stop()));
}

TEST(ChandelierExit, HangsBelowTheHighestHighAndOnlyRises)
{
  ChandelierExit exitRule(0, 3.0);
  exitRule.arm(Side::BUY, 100.0, 2.0);
  EXPECT_DOUBLE_EQ(exitRule.stop(), 94.0) << "entry - 3 * 2";

  // Bar 0: tested against 94 (no hit); anchor 103, candidate 97 -> 97.
  EXPECT_FALSE(exitRule.update(103.0, 99.0, 2.0));
  EXPECT_DOUBLE_EQ(exitRule.stop(), 97.0);
  // Bar 1: anchor 106 -> 100.
  EXPECT_FALSE(exitRule.update(106.0, 102.0, 2.0));
  EXPECT_DOUBLE_EQ(exitRule.stop(), 100.0);
  // Bar 2: anchor 110, ATR 2.5 -> 102.5.
  EXPECT_FALSE(exitRule.update(110.0, 105.0, 2.5));
  EXPECT_DOUBLE_EQ(exitRule.stop(), 102.5);
  // Bar 3: high 108 does not raise the anchor; stop stays 102.5, low 104
  // above it.
  EXPECT_FALSE(exitRule.update(108.0, 104.0, 2.5));
  EXPECT_DOUBLE_EQ(exitRule.stop(), 102.5);
  // Bar 4: low 101.5 <= 102.5, the resting stop is reached. The stop still
  // only moves up: candidate 110 - 7.5 = 102.5.
  EXPECT_TRUE(exitRule.update(105.0, 101.5, 2.5));
  EXPECT_DOUBLE_EQ(exitRule.stop(), 102.5);
}

TEST(ChandelierExit, LookbackDropsOldHighsButTheStopNeverFalls)
{
  ChandelierExit exitRule(2, 3.0);
  exitRule.arm(Side::BUY, 100.0, 2.0);
  EXPECT_FALSE(exitRule.update(103.0, 99.0, 2.0));   // window {103} -> 97
  EXPECT_FALSE(exitRule.update(106.0, 102.0, 2.0));  // window {103,106} -> 100
  EXPECT_FALSE(exitRule.update(110.0, 105.0, 2.5));  // window {106,110} -> 102.5
  EXPECT_FALSE(exitRule.update(108.0, 104.0, 2.5));  // window {110,108} -> 102.5
  EXPECT_DOUBLE_EQ(exitRule.stop(), 102.5);
  // window {108,105}: anchor 108, candidate 100.5, below the resting 102.5,
  // so the stop holds; low 101.5 reaches it.
  EXPECT_TRUE(exitRule.update(105.0, 101.5, 2.5));
  EXPECT_DOUBLE_EQ(exitRule.stop(), 102.5);
}

TEST(ChandelierExit, ShortMirrorsTheLong)
{
  ChandelierExit exitRule(0, 3.0);
  exitRule.arm(Side::SELL, 100.0, 2.0);
  EXPECT_DOUBLE_EQ(exitRule.stop(), 106.0);
  EXPECT_FALSE(exitRule.update(101.0, 97.0, 2.0));  // anchor 97 -> 103
  EXPECT_DOUBLE_EQ(exitRule.stop(), 103.0);
  EXPECT_FALSE(exitRule.update(99.0, 94.0, 2.0));  // anchor 94 -> 100
  EXPECT_DOUBLE_EQ(exitRule.stop(), 100.0);
  EXPECT_TRUE(exitRule.update(100.5, 96.0, 2.0)) << "high 100.5 >= 100";
}

TEST(BreakEvenTrail, MovesToEntryOnTheTriggerThenTrails)
{
  BreakEvenTrail exitRule(2.0, 3.0, 1.5);
  exitRule.arm(Side::BUY, 100.0, 2.0);
  EXPECT_DOUBLE_EQ(exitRule.stop(), 96.0) << "hard stop at entry - 2 ATR";
  EXPECT_FALSE(exitRule.atBreakEven());

  // Bar 0: best 103, 3 above entry, the trigger is 3 * 2 = 6: not yet.
  EXPECT_FALSE(exitRule.update(103.0, 99.0, 2.0));
  EXPECT_DOUBLE_EQ(exitRule.stop(), 96.0);
  EXPECT_FALSE(exitRule.atBreakEven());
  // Bar 1: best 106 = entry + 6: break-even; trail 106 - 1.5 * 2 = 103 is
  // above the entry, so the stop goes straight to 103.
  EXPECT_FALSE(exitRule.update(106.0, 102.0, 2.0));
  EXPECT_TRUE(exitRule.atBreakEven());
  EXPECT_DOUBLE_EQ(exitRule.stop(), 103.0);
  // Bar 2: best 110, ATR 2.5 -> 106.25.
  EXPECT_FALSE(exitRule.update(110.0, 105.0, 2.5));
  EXPECT_DOUBLE_EQ(exitRule.stop(), 106.25);
  // Bar 3: best unchanged, candidate 106.25; low 104 <= 106.25: hit.
  EXPECT_TRUE(exitRule.update(108.0, 104.0, 2.5));
  EXPECT_DOUBLE_EQ(exitRule.stop(), 106.25);
}

TEST(BreakEvenTrail, StopsAtEntryWhenTheTrailWouldBeBelowIt)
{
  BreakEvenTrail exitRule(2.0, 1.0, 4.0);
  exitRule.arm(Side::BUY, 100.0, 2.0);
  // Trigger after 2 points; the trail, 4 ATR = 8 below the best, would sit
  // below the entry, so the stop is the entry itself.
  EXPECT_FALSE(exitRule.update(102.5, 99.5, 2.0));
  EXPECT_TRUE(exitRule.atBreakEven());
  EXPECT_DOUBLE_EQ(exitRule.stop(), 100.0);
  EXPECT_TRUE(exitRule.update(101.0, 100.0, 2.0)) << "low 100 <= stop 100";
}

TEST(BreakEvenTrail, ShortMirrorsTheLong)
{
  BreakEvenTrail exitRule(2.0, 3.0, 1.5);
  exitRule.arm(Side::SELL, 100.0, 2.0);
  EXPECT_DOUBLE_EQ(exitRule.stop(), 104.0);
  EXPECT_FALSE(exitRule.update(101.0, 97.0, 2.0));
  EXPECT_FALSE(exitRule.atBreakEven());
  EXPECT_FALSE(exitRule.update(98.0, 94.0, 2.0));  // best 94 = entry - 6
  EXPECT_TRUE(exitRule.atBreakEven());
  EXPECT_DOUBLE_EQ(exitRule.stop(), 97.0);  // 94 + 3
  EXPECT_TRUE(exitRule.update(97.5, 95.0, 2.0)) << "high 97.5 >= 97";
}
