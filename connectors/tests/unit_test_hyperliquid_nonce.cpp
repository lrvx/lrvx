/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2026 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * hl::nextNonceMs: the venue rejects a nonce it has already seen for a
 * signer, so every value handed out must be unique and increasing even when
 * many threads ask in the same millisecond, and it must stay a millisecond
 * timestamp the venue accepts.
 */

#include "lrvx-connectors/hyperliquid/hl_signer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

namespace
{

uint64_t wallMs()
{
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count());
}

}  // namespace

TEST(HyperliquidNonce, ConcurrentCallsAreUniqueIncreasingAndNearTheWallClock)
{
  constexpr int kThreads = 4;
  constexpr int kPerThread = 1000;

  const uint64_t before = wallMs();
  std::vector<std::vector<uint64_t>> got(kThreads);
  {
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
    {
      threads.emplace_back(
          [&got, t]
          {
            got[t].reserve(kPerThread);
            for (int i = 0; i < kPerThread; ++i)
            {
              got[t].push_back(lrvx::hl::nextNonceMs());
            }
          });
    }
    for (auto& th : threads)
    {
      th.join();
    }
  }
  const uint64_t after = wallMs();

  std::vector<uint64_t> all;
  for (const auto& v : got)
  {
    // Each thread sees its own values strictly increasing.
    for (size_t i = 1; i < v.size(); ++i)
    {
      ASSERT_LT(v[i - 1], v[i]) << "nonce went backwards or repeated within one thread";
    }
    all.insert(all.end(), v.begin(), v.end());
  }
  ASSERT_EQ(all.size(), static_cast<size_t>(kThreads * kPerThread));

  std::sort(all.begin(), all.end());
  EXPECT_EQ(std::adjacent_find(all.begin(), all.end()), all.end())
      << "two calls got the same nonce";

  // 4000 values may run ahead of the clock by at most 4000 ms; in practice
  // far less. Never behind the wall clock taken before the first call.
  EXPECT_GE(all.front(), before);
  const uint64_t ahead = all.back() > after ? all.back() - after : 0;
  EXPECT_LE(ahead, static_cast<uint64_t>(kThreads * kPerThread))
      << "nonce drifted " << ahead << " ms ahead of the wall clock";
}

TEST(HyperliquidNonce, SequentialCallsNeverRepeat)
{
  uint64_t prev = lrvx::hl::nextNonceMs();
  for (int i = 0; i < 100; ++i)
  {
    const uint64_t next = lrvx::hl::nextNonceMs();
    ASSERT_GT(next, prev);
    prev = next;
  }
}
