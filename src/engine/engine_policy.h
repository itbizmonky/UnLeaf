#pragma once
// engine_policy.h — Engine behaviour parameters for UnLeaf Engine
// NO Windows headers. NO Win32 APIs.

#include <cstdint>

namespace engine_logic {

struct EnginePolicy {
    uint32_t violationThreshold  = 3;     // violations -> PERSISTENT
    uint64_t cacheDurationMs     = 200;   // EcoQoS cache TTL
    uint32_t verifyDelay1Ms      = 200;   // Deferred verify step 1
    uint32_t verifyDelay2Ms      = 1000;  // Deferred verify step 2
    uint32_t verifyDelayFinalMs  = 3000;  // Deferred verify step 3 (final)
};

} // namespace engine_logic
