/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Crc32::compute takes the CPU's CRC32 instructions where it has them. These
// pin that path to the table one bit for bit: the format's frames, indexes and
// journals carry CRCs written by whichever path the writer had.

#include "lrvx/util/crc32.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <string>
#include <vector>

using lrvx::util::Crc32;

namespace
{

TEST(Crc32, KnownVectors)
{
  struct Vector
  {
    std::string input;
    uint32_t crc;
  };
  const Vector vectors[] = {
      {"", 0x00000000u},
      {"a", 0xE8B7BE43u},
      {"abc", 0x352441C2u},
      {"hello", 0x3610A686u},
      {"123456789", 0xCBF43926u},
      {"message digest", 0x20159D7Fu},
      {"abcdefghijklmnopqrstuvwxyz", 0x4C2750BDu},
      {"The quick brown fox jumps over the lazy dog", 0x414FA339u},
      {"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789", 0x1FC2E6D2u},
  };
  for (const auto& v : vectors)
  {
    EXPECT_EQ(Crc32::compute(v.input.data(), v.input.size()), v.crc) << v.input;
    EXPECT_EQ(Crc32::computeSoftware(v.input.data(), v.input.size()), v.crc) << v.input;
  }
  const std::vector<uint8_t> zeros(32, 0);
  EXPECT_EQ(Crc32::compute(zeros.data(), zeros.size()), 0x190A55ADu);
  const std::vector<uint8_t> ones(32, 0xFF);
  EXPECT_EQ(Crc32::compute(ones.data(), ones.size()), 0xFF6CAB0Bu);
}

TEST(Crc32, MatchesTableAtEveryLengthAndAlignment)
{
  std::mt19937 rng(20261112);
  std::vector<uint8_t> buf(4096 + 16);
  for (auto& b : buf)
  {
    b = static_cast<uint8_t>(rng());
  }
  for (size_t align = 0; align < 8; ++align)
  {
    for (size_t len = 0; len <= 4096; ++len)
    {
      const uint8_t* p = buf.data() + align;
      ASSERT_EQ(Crc32::compute(p, len), Crc32::computeSoftware(p, len))
          << "len=" << len << " align=" << align;
    }
  }
  // Fresh random content per call, so a lane mix-up cannot hide behind one
  // fixed buffer.
  for (int i = 0; i < 20000; ++i)
  {
    const size_t len = rng() % 4097;
    const size_t align = rng() % 16;
    for (size_t k = 0; k < len; ++k)
    {
      buf[align + k] = static_cast<uint8_t>(rng());
    }
    ASSERT_EQ(Crc32::compute(buf.data() + align, len),
              Crc32::computeSoftware(buf.data() + align, len))
        << "len=" << len << " align=" << align;
  }
}

TEST(Crc32, UsesTheInstructionsOnAArch64)
{
#if defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
  EXPECT_TRUE(Crc32::hardwareAccelerated());
#elif !defined(__aarch64__)
  // The only instruction other targets offer (x86 SSE4.2) computes CRC-32C.
  EXPECT_FALSE(Crc32::hardwareAccelerated());
#else
  GTEST_SKIP() << "AArch64 build without the CRC extension: decided at run time";
#endif
}

}  // namespace
