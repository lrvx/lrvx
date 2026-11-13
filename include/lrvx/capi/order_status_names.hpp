/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2026 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// The names of LrvxOrderEventData.status, indexed by the lrvx::OrderEventStatus
// code. One table for every binding that hands the status to user code as a
// string or takes it back as one: each binding used to keep its own switch,
// and two of them named code 1 "ACCEPTED" and code 2 "PENDING_NEW".

namespace lrvx::capi
{

inline constexpr const char* kOrderEventStatusNames[] = {
    "NEW",
    "SUBMITTED",
    "ACCEPTED",
    "PARTIALLY_FILLED",
    "FILLED",
    "PENDING_CANCEL",
    "CANCELED",
    "EXPIRED",
    "REJECTED",
    "REPLACED",
    "PENDING_TRIGGER",
    "TRIGGERED",
    "TRAILING_UPDATED",
    "QUEUE_POSITION_UPDATED",
    "MARKET_POSITION_CHANGED",
    "REPLACE_SUBMITTED",
    "REPLACE_ACCEPTED",
    "REPLACE_REJECTED",
    "REJECTED_RATE_LIMIT",
    "PENDING_ONCHAIN",
    "REVERTED",
    "REPLACED_GAS",
};
inline constexpr std::size_t kOrderEventStatusNameCount =
    sizeof(kOrderEventStatusNames) / sizeof(kOrderEventStatusNames[0]);

inline const char* orderEventStatusName(uint8_t code)
{
  return code < kOrderEventStatusNameCount ? kOrderEventStatusNames[code] : "UNKNOWN";
}

// Returns false and leaves *out untouched for an unrecognized name.
inline bool orderEventStatusCodeFromName(const std::string& name, uint8_t* out)
{
  for (std::size_t i = 0; i < kOrderEventStatusNameCount; ++i)
  {
    if (name == kOrderEventStatusNames[i])
    {
      *out = static_cast<uint8_t>(i);
      return true;
    }
  }
  return false;
}

inline std::string unknownOrderEventStatusMessage(const std::string& name)
{
  std::string out = "unknown status '" + name + "'; expected one of: ";
  for (std::size_t i = 0; i < kOrderEventStatusNameCount; ++i)
  {
    out += i == 0 ? "" : ", ";
    out += kOrderEventStatusNames[i];
  }
  return out;
}

}  // namespace lrvx::capi
