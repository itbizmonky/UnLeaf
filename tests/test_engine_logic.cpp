// tests/test_engine_logic.cpp
// Unit tests for engine_logic pure C++ decision functions.
// NO Windows headers. NO Win32 APIs.

#include <gtest/gtest.h>
#include "engine/engine_logic.h"

using engine_logic::EnginePolicy;
using engine_logic::NextPhaseOnViolation;
using engine_logic::DeferredVerifyDelayMs;
using engine_logic::ProcessPhase;
using engine_logic::IsTargetProcess;
using engine_logic::IsCacheValid;
using engine_logic::ShouldExitPersistent;

// ============================================================
// IsTargetProcess
// ============================================================

TEST(IsTargetProcessTest, FoundInSingleElementSet) {
    std::set<std::wstring> targets = {L"notepad.exe"};
    EXPECT_TRUE(IsTargetProcess(L"notepad.exe", targets));
}

TEST(IsTargetProcessTest, NotFoundInSingleElementSet) {
    std::set<std::wstring> targets = {L"notepad.exe"};
    EXPECT_FALSE(IsTargetProcess(L"calc.exe", targets));
}

TEST(IsTargetProcessTest, EmptySet) {
    std::set<std::wstring> targets;
    EXPECT_FALSE(IsTargetProcess(L"notepad.exe", targets));
}

TEST(IsTargetProcessTest, FoundInMultiElementSet) {
    std::set<std::wstring> targets = {L"chrome.exe", L"notepad.exe", L"mspaint.exe"};
    EXPECT_TRUE(IsTargetProcess(L"notepad.exe", targets));
}

TEST(IsTargetProcessTest, NotFoundInMultiElementSet) {
    std::set<std::wstring> targets = {L"chrome.exe", L"notepad.exe", L"mspaint.exe"};
    EXPECT_FALSE(IsTargetProcess(L"calc.exe", targets));
}

TEST(IsTargetProcessTest, EmptyStringKey) {
    std::set<std::wstring> targets = {L"notepad.exe", L""};
    EXPECT_TRUE(IsTargetProcess(L"", targets));
}

// ============================================================
// IsCacheValid
// ============================================================

TEST(IsCacheValidTest, CachedFalseAlwaysInvalid) {
    // cached=false → always miss regardless of timestamps
    EXPECT_FALSE(IsCacheValid(false, 1000, 900, 200));
}

TEST(IsCacheValidTest, WithinTtl) {
    // now=1000, cacheTime=900, duration=200 → elapsed=100 < 200 → valid
    EXPECT_TRUE(IsCacheValid(true, 1000, 900, 200));
}

TEST(IsCacheValidTest, ExactlyAtTtlBoundary) {
    // elapsed = duration-1 → still valid
    EXPECT_TRUE(IsCacheValid(true, 1099, 900, 200));
}

TEST(IsCacheValidTest, ExactlyAtTtlExpiry) {
    // elapsed == duration → NOT valid (condition is < not <=)
    EXPECT_FALSE(IsCacheValid(true, 1100, 900, 200));
}

TEST(IsCacheValidTest, PastTtl) {
    // elapsed > duration → invalid
    EXPECT_FALSE(IsCacheValid(true, 1500, 900, 200));
}

TEST(IsCacheValidTest, NowEqualsCacheTime) {
    // elapsed = 0 < any positive duration → valid
    EXPECT_TRUE(IsCacheValid(true, 1000, 1000, 100));
}

TEST(IsCacheValidTest, ZeroDuration) {
    // duration=0 → 0 < 0 is false → always invalid even if cached=true
    EXPECT_FALSE(IsCacheValid(true, 1000, 1000, 0));
}

// ============================================================
// ShouldExitPersistent
// ============================================================

TEST(ShouldExitPersistentTest, BelowThreshold) {
    EXPECT_FALSE(ShouldExitPersistent(59999, 60000));
}

TEST(ShouldExitPersistentTest, ExactlyAtThreshold) {
    // >= so true
    EXPECT_TRUE(ShouldExitPersistent(60000, 60000));
}

TEST(ShouldExitPersistentTest, AboveThreshold) {
    EXPECT_TRUE(ShouldExitPersistent(60001, 60000));
}

TEST(ShouldExitPersistentTest, ZeroTime) {
    EXPECT_FALSE(ShouldExitPersistent(0, 60000));
}

TEST(ShouldExitPersistentTest, BothZero) {
    // 0 >= 0 → true
    EXPECT_TRUE(ShouldExitPersistent(0, 0));
}

// ============================================================
// NextPhaseOnViolation
// ============================================================
// EnginePolicy 定数 (violationThreshold のみ意味を持つ)
static const EnginePolicy kPolicyThresh3{};   // threshold=3 (default)
static const EnginePolicy kPolicyThresh0{0};  // threshold=0

TEST(NextPhaseOnViolationTest, ZeroViolations) {
    EXPECT_EQ(NextPhaseOnViolation(0, kPolicyThresh3), ProcessPhase::AGGRESSIVE);
}

TEST(NextPhaseOnViolationTest, BelowThreshold) {
    EXPECT_EQ(NextPhaseOnViolation(1, kPolicyThresh3), ProcessPhase::AGGRESSIVE);
    EXPECT_EQ(NextPhaseOnViolation(2, kPolicyThresh3), ProcessPhase::AGGRESSIVE);
}

TEST(NextPhaseOnViolationTest, ExactlyAtThreshold) {
    EXPECT_EQ(NextPhaseOnViolation(3, kPolicyThresh3), ProcessPhase::PERSISTENT);
}

TEST(NextPhaseOnViolationTest, AboveThreshold) {
    EXPECT_EQ(NextPhaseOnViolation(4,   kPolicyThresh3), ProcessPhase::PERSISTENT);
    EXPECT_EQ(NextPhaseOnViolation(100, kPolicyThresh3), ProcessPhase::PERSISTENT);
}

TEST(NextPhaseOnViolationTest, ThresholdZero) {
    // count=0 >= threshold=0 → PERSISTENT
    EXPECT_EQ(NextPhaseOnViolation(0, kPolicyThresh0), ProcessPhase::PERSISTENT);
}

// ============================================================
// DeferredVerifyDelayMs
// ============================================================
// Actual game values: v1=200, v2=1000, vFinal=3000 (matches EnginePolicy defaults)

TEST(DeferredVerifyDelayMsTest, Step1) {
    EXPECT_EQ(DeferredVerifyDelayMs(1, EnginePolicy{}), 200u);
}

TEST(DeferredVerifyDelayMsTest, Step2) {
    EXPECT_EQ(DeferredVerifyDelayMs(2, EnginePolicy{}), 800u);  // 1000 - 200
}

TEST(DeferredVerifyDelayMsTest, Step3) {
    EXPECT_EQ(DeferredVerifyDelayMs(3, EnginePolicy{}), 2000u); // 3000 - 1000
}

TEST(DeferredVerifyDelayMsTest, Step0Invalid) {
    EXPECT_EQ(DeferredVerifyDelayMs(0, EnginePolicy{}), 0u);
}

TEST(DeferredVerifyDelayMsTest, Step4Invalid) {
    EXPECT_EQ(DeferredVerifyDelayMs(4, EnginePolicy{}), 0u);
}

TEST(DeferredVerifyDelayMsTest, V1EqualsV2Step2YieldsZero) {
    EXPECT_EQ(DeferredVerifyDelayMs(2, EnginePolicy{3, 200, 500,  500, 3000}), 0u);  // v1==v2
}

TEST(DeferredVerifyDelayMsTest, V2EqualsVFinalStep3YieldsZero) {
    EXPECT_EQ(DeferredVerifyDelayMs(3, EnginePolicy{3, 200, 200, 1000, 1000}), 0u);  // v2==vFinal
}

TEST(DeferredVerifyDelayMsTest, V1ZeroStep1) {
    EXPECT_EQ(DeferredVerifyDelayMs(1, EnginePolicy{3, 200,   0, 1000, 3000}), 0u);  // v1=0
}
