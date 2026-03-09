// tests/test_engine_policy.cpp
// Unit tests for EnginePolicy struct.
// NO Windows headers. NO Win32 APIs.

#include <gtest/gtest.h>
#include "engine/engine_policy.h"

using namespace engine_logic;

TEST(EnginePolicyTest, Construction) {
    EnginePolicy policy{3, 200, 200, 1000, 3000};
    EXPECT_EQ(policy.violationThreshold,   3u);
    EXPECT_EQ(policy.cacheDurationMs,    200u);
    EXPECT_EQ(policy.verifyDelay1Ms,     200u);
    EXPECT_EQ(policy.verifyDelay2Ms,    1000u);
    EXPECT_EQ(policy.verifyDelayFinalMs, 3000u);
}

TEST(EnginePolicyTest, DefaultValues) {
    EnginePolicy policy{};
    EXPECT_EQ(policy.violationThreshold,   3u);
    EXPECT_EQ(policy.cacheDurationMs,    200u);
    EXPECT_EQ(policy.verifyDelay1Ms,     200u);
    EXPECT_EQ(policy.verifyDelay2Ms,    1000u);
    EXPECT_EQ(policy.verifyDelayFinalMs, 3000u);
}
