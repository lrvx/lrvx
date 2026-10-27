/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace lrvx::hl
{

struct HlSig
{
  std::string r, s;
  int v;
};

struct HlSignParams
{
  std::string actionJson;
  long long nonceMs;
  std::string privateKeyHex;
  bool isMainnet{true};

  std::optional<std::string> activePoolJson;
  std::optional<long long> expiresAfterMs;
};

std::optional<HlSig> hl_sign_with_sdk(const HlSignParams& p);

// Nonce for an L1 action: the wall clock in milliseconds, bumped past the
// last value this process handed out. The venue rejects a nonce it has
// already seen for the signer, so two actions signed in the same millisecond
// (from two threads, or from two signers in one process) need distinct
// values. Process-wide and lock-free; never returns the same value twice and
// never goes backwards.
uint64_t nextNonceMs();

}  // namespace lrvx::hl
