/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320) -- integrity checksums for
 * on-disk records and wire frames. The table is built at compile time; on
 * AArch64 the CRC32 instructions are used instead when the CPU has them.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

// The format's polynomial is the IEEE one (0xEDB88320 reflected), which is
// what the ARMv8 CRC32 instructions compute. The x86 SSE4.2 instruction
// computes CRC-32C, a different polynomial, so x86 stays on the table.
#if defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
#if defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#define LRVX_CRC32_ARM_STATIC 1
#elif defined(__linux__)
#include <arm_acle.h>
#include <sys/auxv.h>
#ifndef HWCAP_CRC32
#define HWCAP_CRC32 (1 << 7)
#endif
#define LRVX_CRC32_ARM_RUNTIME 1
#endif
#endif

namespace lrvx::util
{

namespace detail
{
// Built at compile time. The table used to be built on first use, behind a
// plain bool: every thread that had not seen the flag set built the table
// again, over the copy another thread was reading -- a data race, benign in
// practice only because each writer wrote the same bytes, and undefined
// behaviour regardless. Nothing about the table needs a runtime decision, so
// there is nothing to synchronise and no first-use branch left on the
// checksum path.
constexpr std::array<uint32_t, 256> makeCrc32Table() noexcept
{
  std::array<uint32_t, 256> t{};
  for (uint32_t i = 0; i < 256; ++i)
  {
    uint32_t c = i;
    for (int j = 0; j < 8; ++j)
    {
      c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    }
    t[i] = c;
  }
  return t;
}

inline constexpr std::array<uint32_t, 256> kCrc32Table = makeCrc32Table();

// Pre- and post-inversion are the caller's: these take and return the raw
// register so the table and the instruction paths can be compared directly.
inline uint32_t crc32Table(uint32_t crc, const uint8_t* p, size_t size) noexcept
{
  for (size_t i = 0; i < size; ++i)
  {
    crc = kCrc32Table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  }
  return crc;
}

#if defined(LRVX_CRC32_ARM_STATIC) || defined(LRVX_CRC32_ARM_RUNTIME)
#if defined(LRVX_CRC32_ARM_RUNTIME)
#if defined(__clang__)
#define LRVX_CRC32_ARM_TARGET __attribute__((target("crc")))
#else
#define LRVX_CRC32_ARM_TARGET __attribute__((target("+crc")))
#endif
#else
#define LRVX_CRC32_ARM_TARGET
#endif

LRVX_CRC32_ARM_TARGET inline uint32_t crc32Arm(uint32_t crc, const uint8_t* p,
                                               size_t size) noexcept
{
  while (size >= 32)
  {
    uint64_t a, b, c, d;
    std::memcpy(&a, p, 8);
    std::memcpy(&b, p + 8, 8);
    std::memcpy(&c, p + 16, 8);
    std::memcpy(&d, p + 24, 8);
    crc = __crc32d(crc, a);
    crc = __crc32d(crc, b);
    crc = __crc32d(crc, c);
    crc = __crc32d(crc, d);
    p += 32;
    size -= 32;
  }
  while (size >= 8)
  {
    uint64_t v;
    std::memcpy(&v, p, 8);
    crc = __crc32d(crc, v);
    p += 8;
    size -= 8;
  }
  if (size >= 4)
  {
    uint32_t v;
    std::memcpy(&v, p, 4);
    crc = __crc32w(crc, v);
    p += 4;
    size -= 4;
  }
  while (size > 0)
  {
    crc = __crc32b(crc, *p++);
    --size;
  }
  return crc;
}
#undef LRVX_CRC32_ARM_TARGET
#endif

inline bool crc32HardwareAvailable() noexcept
{
#if defined(LRVX_CRC32_ARM_STATIC)
  return true;
#elif defined(LRVX_CRC32_ARM_RUNTIME)
  static const bool available = (getauxval(AT_HWCAP) & HWCAP_CRC32) != 0;
  return available;
#else
  return false;
#endif
}
}  // namespace detail

class Crc32
{
 public:
  static uint32_t compute(const void* data, size_t size) noexcept
  {
    const auto* p = static_cast<const uint8_t*>(data);
#if defined(LRVX_CRC32_ARM_STATIC)
    return detail::crc32Arm(0xFFFFFFFFu, p, size) ^ 0xFFFFFFFFu;
#elif defined(LRVX_CRC32_ARM_RUNTIME)
    if (detail::crc32HardwareAvailable())
    {
      return detail::crc32Arm(0xFFFFFFFFu, p, size) ^ 0xFFFFFFFFu;
    }
    return detail::crc32Table(0xFFFFFFFFu, p, size) ^ 0xFFFFFFFFu;
#else
    return detail::crc32Table(0xFFFFFFFFu, p, size) ^ 0xFFFFFFFFu;
#endif
  }

  static uint32_t compute(std::span<const std::byte> data) noexcept
  {
    return compute(data.data(), data.size());
  }

  // The table path, whatever the target: the reference the instruction path
  // is checked against.
  static uint32_t computeSoftware(const void* data, size_t size) noexcept
  {
    return detail::crc32Table(0xFFFFFFFFu, static_cast<const uint8_t*>(data), size) ^
           0xFFFFFFFFu;
  }

  // True when compute() runs on the CPU's CRC32 instructions.
  static bool hardwareAccelerated() noexcept { return detail::crc32HardwareAvailable(); }
};

}  // namespace lrvx::util
