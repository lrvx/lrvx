/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Shared SBE (Simple Binary Encoding) wire primitives for the venue codecs.
 *
 * SBE is little-endian by contract and every message is prefixed with a fixed
 * messageHeader composite: blockLength (u16), templateId (u16), schemaId (u16),
 * version (u16). blockLength is the length of the fixed root block that follows
 * the header (excluding the header itself), which lets a reader skip trailing
 * fields a newer schema version appended -- forward compatibility.
 *
 * These helpers are the hand-written substrate the market-data and order-entry
 * codecs are built on; the per-message schemas are the .xml files in
 * venue/schema/.
 */
#pragma once

#include <cstdint>
#include <vector>

namespace lrvx::venue::sbe
{

inline constexpr size_t kHeaderSize = 8;  // blockLength+templateId+schemaId+version

// ---- little-endian primitive writers ----
inline void putU16(std::vector<uint8_t>& o, uint16_t v)
{
  o.push_back(static_cast<uint8_t>(v & 0xFF));
  o.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}
inline void putU32(std::vector<uint8_t>& o, uint32_t v)
{
  for (int i = 0; i < 4; ++i)
  {
    o.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
  }
}
inline void putU64(std::vector<uint8_t>& o, uint64_t v)
{
  for (int i = 0; i < 8; ++i)
  {
    o.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
  }
}
inline void putI64(std::vector<uint8_t>& o, int64_t v) { putU64(o, static_cast<uint64_t>(v)); }
inline void putU8(std::vector<uint8_t>& o, uint8_t v) { o.push_back(v); }

// ---- little-endian primitive readers (caller bounds-checks) ----
inline uint16_t getU16(const uint8_t* p)
{
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
inline uint32_t getU32(const uint8_t* p)
{
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i)
  {
    v |= static_cast<uint32_t>(p[i]) << (8 * i);
  }
  return v;
}
inline uint64_t getU64(const uint8_t* p)
{
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i)
  {
    v |= static_cast<uint64_t>(p[i]) << (8 * i);
  }
  return v;
}
inline int64_t getI64(const uint8_t* p) { return static_cast<int64_t>(getU64(p)); }

// ---- message header ----
struct Header
{
  uint16_t blockLength{};
  uint16_t templateId{};
  uint16_t schemaId{};
  uint16_t version{};
};

inline void putHeader(std::vector<uint8_t>& o, uint16_t blockLength, uint16_t templateId,
                      uint16_t schemaId, uint16_t version)
{
  putU16(o, blockLength);
  putU16(o, templateId);
  putU16(o, schemaId);
  putU16(o, version);
}

// Read the header from a buffer known to hold at least kHeaderSize bytes.
inline Header readHeader(const uint8_t* p)
{
  return Header{getU16(p + 0), getU16(p + 2), getU16(p + 4), getU16(p + 6)};
}

}  // namespace lrvx::venue::sbe
