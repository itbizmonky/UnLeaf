// engine_logic.cpp — Pure C++ decision logic for UnLeaf Engine
// NO Windows headers. NO Win32 APIs.

#include "engine_logic.h"

namespace engine_logic {

bool IsTargetProcess(const std::wstring& processNameLower,
                     const std::set<std::wstring>& targetNames) {
    return targetNames.count(processNameLower) > 0;
}

bool IsCacheValid(bool cached, uint64_t now,
                  uint64_t cacheTime, uint64_t cacheDuration) {
    return cached && (now - cacheTime < cacheDuration);
}

bool ShouldExitPersistent(uint64_t timeSinceLastViolation,
                           uint64_t cleanThreshold) {
    return timeSinceLastViolation >= cleanThreshold;
}

ProcessPhase NextPhaseOnViolation(uint32_t violationCount,
                                  const EnginePolicy& policy) noexcept {
    return (violationCount >= policy.violationThreshold)
        ? ProcessPhase::PERSISTENT
        : ProcessPhase::AGGRESSIVE;
}

uint32_t DeferredVerifyDelayMs(uint8_t step,
                               const EnginePolicy& policy) noexcept {
    switch (step) {
        case 1: return policy.verifyDelay1Ms;
        case 2:
            return (policy.verifyDelay2Ms > policy.verifyDelay1Ms)
                ? policy.verifyDelay2Ms - policy.verifyDelay1Ms
                : 0;
        case 3:
            return (policy.verifyDelayFinalMs > policy.verifyDelay2Ms)
                ? policy.verifyDelayFinalMs - policy.verifyDelay2Ms
                : 0;
        default:
            // invalid step or misconfigured policy
            return 0;
    }
}

} // namespace engine_logic
