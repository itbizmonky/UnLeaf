#pragma once
// engine_logic.h — Pure C++ decision logic for UnLeaf Engine
// NO Windows headers. NO Win32 APIs. NO OS-specific types.

#include <string>
#include <set>
#include <cstdint>
#include "engine_policy.h"

namespace engine_logic {

// Process monitoring phase (mirrors unleaf::ProcessPhase via type alias)
enum class ProcessPhase {
    AGGRESSIVE,   // Startup: One-shot SET + deferred verification (3s)
    STABLE,       // Steady-state: Event-driven only
    PERSISTENT    // Stubborn EcoQoS: SET @ 5s interval
};

// Target process lookup.
// processNameLower must be pre-lowercased by the caller.
bool IsTargetProcess(const std::wstring& processNameLower,
                     const std::set<std::wstring>& targetNames);

// EcoQoS cache validity check.
// Returns true if the cached value is still valid (cache hit).
bool IsCacheValid(bool cached, uint64_t now,
                  uint64_t cacheTime, uint64_t cacheDuration);

// Persistent phase clean-time exit decision.
// Returns true if the process has been clean long enough to exit PERSISTENT.
bool ShouldExitPersistent(uint64_t timeSinceLastViolation,
                           uint64_t cleanThreshold);

// Determine the next phase after a violation.
// Call after incrementing violationCount.
ProcessPhase NextPhaseOnViolation(uint32_t violationCount,
                                  const EnginePolicy& policy) noexcept;

// Deferred verification one-shot timer delay (ms) for the given step.
//   step 1 -> verifyDelay1Ms
//   step 2 -> verifyDelay2Ms - verifyDelay1Ms
//   step 3 -> verifyDelayFinalMs - verifyDelay2Ms
//   other  -> 0  (caller should treat 0 as invalid and return early)
uint32_t DeferredVerifyDelayMs(uint8_t step,
                               const EnginePolicy& policy) noexcept;

} // namespace engine_logic
