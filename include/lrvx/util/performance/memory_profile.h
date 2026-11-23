/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <cerrno>
#include <cstdint>
#include <sstream>
#include <string>
#include <system_error>

#if defined(__linux__) || defined(__APPLE__)
#include <sys/mman.h>
#include <sys/resource.h>
#endif

namespace lrvx::performance
{

// Deployment memory profile. lrvx pre-allocates and touches its hot memory
// (pools and rings are constructed at startup), but a touched page is still
// evictable: under memory pressure it can be paged out, and the next access
// is a major fault in the middle of trading. COLO pins everything with
// mlockall(MCL_CURRENT | MCL_FUTURE). DEFAULT changes nothing -- a hard
// mlockall would break startup on hosts without CAP_IPC_LOCK / memlock
// ulimit, which is most shared VPS deployments (same philosophy as
// BackoffMode::RELAXED).

enum class MemoryProfile
{
  DEFAULT,  ///< No page locking; suitable for shared/VPS hosts
  COLO      ///< mlockall the process; requires memlock privilege
};

inline const char* toString(MemoryProfile p)
{
  return p == MemoryProfile::COLO ? "colo" : "default";
}

inline MemoryProfile memoryProfileFromString(const std::string& s)
{
  return s == "colo" ? MemoryProfile::COLO : MemoryProfile::DEFAULT;
}

struct MemoryReport
{
  MemoryProfile profile{MemoryProfile::DEFAULT};
  bool mlockRequested{false};
  bool mlockApplied{false};
  std::string mlockError;         // errno text when the request failed
  uint64_t memlockLimitBytes{0};  // RLIMIT_MEMLOCK soft limit; UINT64_MAX = unlimited
  bool memlockLimitKnown{false};

  // Filled by the huge-page arena layer when in use (see large_arena.h).
  uint64_t hugeArenaBytes{0};
  std::string hugeArenaMode{"n/a"};

  // Allocations that escaped a pool's inline buffer into the heap
  // (see counting_resource.h). Non-zero means some pool is undersized for
  // the traffic and is holding heap memory a monotonic arena never returns;
  // if it grows after startup, the pool allocated during trading. Fix by
  // sizing the pool via Pool::prewarm() at startup.
  uint64_t poolHeapAllocations{0};
  uint64_t poolHeapBytes{0};

  std::string toString() const
  {
    std::ostringstream os;
    os << "memory profile=" << performance::toString(profile)
       << " mlock=";
    if (!mlockRequested)
    {
      os << "not-requested";
    }
    else if (mlockApplied)
    {
      os << "applied";
    }
    else
    {
      os << "FAILED(" << mlockError << ")";
    }
    os << " memlock-limit=";
    if (!memlockLimitKnown)
    {
      os << "unknown";
    }
    else if (memlockLimitBytes == UINT64_MAX)
    {
      os << "unlimited";
    }
    else
    {
      os << memlockLimitBytes;
    }
    os << " huge-arena=" << hugeArenaMode;
    if (hugeArenaBytes > 0)
    {
      os << "(" << hugeArenaBytes << "B)";
    }
    os << " pool-heap-fallback=";
    if (poolHeapAllocations == 0)
    {
      os << "none";
    }
    else
    {
      os << poolHeapAllocations << "(" << poolHeapBytes << "B)";
    }
    return os.str();
  }
};

// Applies the profile to the current process. Never fatal: a COLO request on
// an unprivileged host degrades to DEFAULT behaviour with the failure recorded
// in the report -- absence of protection must be a visible fact, not a
// surprise.
inline MemoryReport applyMemoryProfile(MemoryProfile profile)
{
  MemoryReport report;
  report.profile = profile;

#if defined(__linux__) || defined(__APPLE__)
  rlimit rl{};
  if (::getrlimit(RLIMIT_MEMLOCK, &rl) == 0)
  {
    report.memlockLimitKnown = true;
    report.memlockLimitBytes =
        (rl.rlim_cur == RLIM_INFINITY) ? UINT64_MAX : static_cast<uint64_t>(rl.rlim_cur);
  }

  if (profile == MemoryProfile::COLO)
  {
    report.mlockRequested = true;
    if (::mlockall(MCL_CURRENT | MCL_FUTURE) == 0)
    {
      report.mlockApplied = true;
    }
    else
    {
      // Not strerror: it hands back a pointer into a buffer every thread
      // shares.
      report.mlockError = std::generic_category().message(errno);
    }
  }
#else
  if (profile == MemoryProfile::COLO)
  {
    report.mlockRequested = true;
    report.mlockError = "mlockall unsupported on this platform";
  }
#endif

  return report;
}

}  // namespace lrvx::performance
