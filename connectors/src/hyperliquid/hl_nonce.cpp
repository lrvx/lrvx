/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2026 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Kept out of hl_signer.cpp so that a test which replaces hl_sign_with_sdk at
// link time does not also drag in, and collide with, the real signer.

#include "lrvx-connectors/hyperliquid/hl_signer.h"

#include <algorithm>
#include <atomic>
#include <chrono>

namespace lrvx::hl
{

uint64_t nextNonceMs()
{
  static std::atomic<uint64_t> last{0};

  const auto now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                             std::chrono::system_clock::now().time_since_epoch())
                                             .count());
  uint64_t prev = last.load(std::memory_order_relaxed);
  for (;;)
  {
    const uint64_t next = std::max(now, prev + 1);
    if (last.compare_exchange_weak(prev, next, std::memory_order_relaxed))
    {
      return next;
    }
  }
}

}  // namespace lrvx::hl
