#pragma once

// Which cores share the largest cache at a given level.
//
// Split out from the pinning itself so the rule can be tested against a
// synthetic topology: the interesting layouts -- a part with 3D V-Cache on one
// die only, a part where every core shares one cache -- cannot be produced on
// demand by the machine running the tests, and the rule is the part that decides
// whether the pin helps or does nothing.

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace moderngekko::frontend
{
struct CacheDomain
{
  KAFFINITY mask = 0;
  DWORD size = 0;

  // A zero mask means nothing matched, so there is nothing to pin to.
  explicit operator bool() const { return mask != 0; }
};

// Scans a GetLogicalProcessorInformationEx(RelationCache, ...) buffer.
//
// Ties keep the first match rather than the last, so the answer does not depend
// on the order the OS happens to report caches in.
//
// Multi-group machines would need every group considered; a single group covers
// up to 64 logical processors, which is all this targets.
inline CacheDomain LargestSharedCache(const void* records, std::size_t bytes, BYTE level = 3)
{
  CacheDomain best;
  const auto* base = static_cast<const char*>(records);
  for (std::size_t offset = 0;
       offset + sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX) <= bytes;)
  {
    const auto* entry =
        reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(base + offset);
    // A zero Size would not advance, so this stops rather than spinning on a
    // truncated or malformed buffer.
    if (entry->Size == 0)
      break;
    if (entry->Relationship == RelationCache && entry->Cache.Level == level &&
        entry->Cache.CacheSize > best.size)
    {
      best.size = entry->Cache.CacheSize;
      best.mask = entry->Cache.GroupMask.Mask;
    }
    offset += entry->Size;
  }
  return best;
}

// Whether pinning to the largest cache domain is worth doing at all.
//
// LargestSharedCache always names a domain, because some domain is always the
// largest. That is the wrong question for a default-on pin: on a part whose
// dies are identical, pinning to one of them halves the available cores and
// buys no extra cache. So this returns a domain only when it dominates its
// closest rival by at least half again as much cache -- 96 MB against 32 MB
// pins, 32 against 32 does not, and 48 against 32 is exactly the boundary.
//
// A domain with no rival is dominant by definition: a single L3 across every
// core has nothing to lose to, and its mask covers everything, so applying it
// is a no-op rather than a restriction.
//
// The same physical cache can be reported more than once. A duplicate is not a
// rival -- treating it as one would suppress every pin on hardware that repeats
// itself -- so records matching the leader mask are skipped.
inline CacheDomain DominantSharedCache(const void* records, std::size_t bytes,
                                       BYTE level = 3)
{
  const CacheDomain best = LargestSharedCache(records, bytes, level);
  if (!best)
    return {};

  // Largest cache at this level that is neither the leader nor a duplicate.
  DWORD rival = 0;
  const auto* base = static_cast<const char*>(records);
  for (std::size_t offset = 0;
       offset + sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX) <= bytes;)
  {
    const auto* entry =
        reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(base + offset);
    if (entry->Size == 0)
      break;
    if (entry->Relationship == RelationCache && entry->Cache.Level == level &&
        entry->Cache.GroupMask.Mask != best.mask && entry->Cache.CacheSize > rival)
    {
      rival = entry->Cache.CacheSize;
    }
    offset += entry->Size;
  }

  if (rival == 0)
    return best;

  // best >= 1.5 * rival, in 64-bit: a cache size in bytes times a small factor
  // overflows 32 bits on a part with a gigabyte of L3.
  const unsigned long long scaled_best =
      static_cast<unsigned long long>(best.size) * 2ULL;
  const unsigned long long scaled_rival =
      static_cast<unsigned long long>(rival) * 3ULL;
  return scaled_best >= scaled_rival ? best : CacheDomain{};
}

// Process-wide affinity can interfere with Dolphin's worker threads, so require
// an exact, explicit opt-in instead of changing every launch by default.
inline bool AffinityEnabled(const char* value)
{
  return value != nullptr && value[0] == '1' && value[1] == '\0';
}

struct PinResult
{
  bool affinity_set = false;
};

// Applies a domain to a process. Separate from choosing one so a test can hand
// it a real process handle and read the result back out of the OS.
//
// An empty domain is not an error: there was nothing to pin to, and the
// process's existing affinity is left alone.
inline PinResult ApplyCacheDomain(HANDLE process, const CacheDomain& domain)
{
  PinResult result;
  if (!domain)
    return result;
  result.affinity_set = SetProcessAffinityMask(process, domain.mask) != FALSE;
  return result;
}
}  // namespace moderngekko::frontend
#endif  // _WIN32
