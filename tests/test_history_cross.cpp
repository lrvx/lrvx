/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2026 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// History: a fixed-capacity ring of the last values, indexed newest first.
// crossedAbove / crossedBelow: whether a series crossed another (or a level)
// between the previous step and this one.

#include "lrvx/strategy/history.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <stdexcept>

using namespace lrvx;

namespace
{
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
}

TEST(History, NewestIsIndexZero)
{
  History h(3);
  EXPECT_EQ(h.capacity(), 3u);
  EXPECT_EQ(h.size(), 0u);
  EXPECT_FALSE(h.full());
  EXPECT_THROW(h.at(0), std::out_of_range) << "an empty history has no index 0";

  h.push(10.0);
  EXPECT_EQ(h.size(), 1u);
  EXPECT_DOUBLE_EQ(h[0], 10.0);
  EXPECT_DOUBLE_EQ(h.at(0), 10.0);
  EXPECT_THROW(h.at(1), std::out_of_range);

  h.push(20.0);
  EXPECT_DOUBLE_EQ(h[0], 20.0) << "0 is the newest";
  EXPECT_DOUBLE_EQ(h[1], 10.0) << "1 is the previous";
  EXPECT_FALSE(h.full());

  h.push(30.0);
  EXPECT_TRUE(h.full());
  EXPECT_EQ(h.size(), 3u);
  EXPECT_DOUBLE_EQ(h.at(2), 10.0);

  h.push(40.0);
  EXPECT_EQ(h.size(), 3u) << "size stops at capacity";
  EXPECT_DOUBLE_EQ(h.at(0), 40.0);
  EXPECT_DOUBLE_EQ(h.at(1), 30.0);
  EXPECT_DOUBLE_EQ(h.at(2), 20.0) << "the oldest, 10, dropped out";
  EXPECT_THROW(h.at(3), std::out_of_range) << "index == size is out of range";

  for (double v : {50.0, 60.0, 70.0, 80.0})
  {
    h.push(v);
  }
  EXPECT_DOUBLE_EQ(h.at(0), 80.0) << "wraps around the ring more than once";
  EXPECT_DOUBLE_EQ(h.at(1), 70.0);
  EXPECT_DOUBLE_EQ(h.at(2), 60.0);
}

TEST(History, CapacityOneKeepsOnlyTheNewest)
{
  History h(1);
  h.push(1.0);
  EXPECT_TRUE(h.full());
  h.push(2.0);
  EXPECT_EQ(h.size(), 1u);
  EXPECT_DOUBLE_EQ(h.at(0), 2.0);
  EXPECT_THROW(h.at(1), std::out_of_range);
}

TEST(History, NaNIsStoredAsGiven)
{
  History h(2);
  h.push(kNaN);
  h.push(5.0);
  EXPECT_EQ(h.size(), 2u) << "a NaN takes a slot";
  EXPECT_DOUBLE_EQ(h.at(0), 5.0);
  EXPECT_TRUE(std::isnan(h.at(1)));
}

TEST(History, RejectsZeroCapacity) { EXPECT_THROW(History(0), std::invalid_argument); }

TEST(Crossing, AboveNeedsAtOrBelowThenStrictlyAbove)
{
  EXPECT_TRUE(crossedAbove(1.0, 3.0, 2.0, 2.0)) << "from below to above a level";
  EXPECT_FALSE(crossedAbove(3.0, 4.0, 2.0, 2.0)) << "already above";
  EXPECT_FALSE(crossedAbove(1.0, 2.0, 2.0, 2.0)) << "touching the level is not a cross";
  EXPECT_TRUE(crossedAbove(2.0, 2.5, 2.0, 2.0)) << "leaving upward from equal is a cross";
  EXPECT_FALSE(crossedAbove(2.0, 2.0, 2.0, 2.0)) << "equal both steps";
  EXPECT_FALSE(crossedAbove(3.0, 1.0, 2.0, 2.0)) << "a cross below is not above";
  // Two moving series: a was under b, now over it.
  EXPECT_TRUE(crossedAbove(10.0, 12.0, 11.0, 11.5));
  // a rose, but b rose more.
  EXPECT_FALSE(crossedAbove(10.0, 12.0, 11.0, 13.0));
  // b fell through a still a.
  EXPECT_TRUE(crossedAbove(10.0, 10.0, 10.5, 9.5));
  // a was above b before: already above, no cross.
  EXPECT_FALSE(crossedAbove(12.0, 13.0, 11.0, 11.5));
}

TEST(Crossing, BelowNeedsAtOrAboveThenStrictlyBelow)
{
  EXPECT_TRUE(crossedBelow(3.0, 1.0, 2.0, 2.0));
  EXPECT_FALSE(crossedBelow(1.0, 0.5, 2.0, 2.0)) << "already below";
  EXPECT_FALSE(crossedBelow(3.0, 2.0, 2.0, 2.0)) << "touching the level is not a cross";
  EXPECT_TRUE(crossedBelow(2.0, 1.5, 2.0, 2.0)) << "leaving downward from equal is a cross";
  EXPECT_FALSE(crossedBelow(2.0, 2.0, 2.0, 2.0));
  EXPECT_FALSE(crossedBelow(1.0, 3.0, 2.0, 2.0)) << "a cross above is not below";
  EXPECT_TRUE(crossedBelow(12.0, 10.0, 11.5, 11.0));
  EXPECT_FALSE(crossedBelow(12.0, 10.0, 11.0, 9.0)) << "b fell further";
}

TEST(Crossing, AnyNaNIsNoCross)
{
  for (int i = 0; i < 4; ++i)
  {
    double v[4] = {1.0, 3.0, 2.0, 2.0};
    v[i] = kNaN;
    EXPECT_FALSE(crossedAbove(v[0], v[1], v[2], v[3])) << "NaN at " << i;
    double w[4] = {3.0, 1.0, 2.0, 2.0};
    w[i] = kNaN;
    EXPECT_FALSE(crossedBelow(w[0], w[1], w[2], w[3])) << "NaN at " << i;
  }
}
