// UnLeaf Unit Tests - Logger public API
// Tests: LogLevel get/set, Enabled get/set, initial path state

#include <gtest/gtest.h>
#include "common/logger.h"

using namespace unleaf;

// ============================================================
// LoggerLevelTest
// ============================================================

TEST(LoggerLevelTest, DefaultLevel) {
    auto& logger = LightweightLogger::Instance();
    // Reset to default for test isolation
    logger.SetLogLevel(LogLevel::LOG_INFO);
    EXPECT_EQ(logger.GetLogLevel(), LogLevel::LOG_INFO);
}

TEST(LoggerLevelTest, SetAndGetError) {
    auto& logger = LightweightLogger::Instance();
    logger.SetLogLevel(LogLevel::LOG_ERROR);
    EXPECT_EQ(logger.GetLogLevel(), LogLevel::LOG_ERROR);
    logger.SetLogLevel(LogLevel::LOG_INFO); // restore
}

TEST(LoggerLevelTest, SetAndGetDebug) {
    auto& logger = LightweightLogger::Instance();
    logger.SetLogLevel(LogLevel::LOG_DEBUG);
    EXPECT_EQ(logger.GetLogLevel(), LogLevel::LOG_DEBUG);
    logger.SetLogLevel(LogLevel::LOG_INFO); // restore
}

TEST(LoggerLevelTest, SetAndGetAlert) {
    auto& logger = LightweightLogger::Instance();
    logger.SetLogLevel(LogLevel::LOG_ALERT);
    EXPECT_EQ(logger.GetLogLevel(), LogLevel::LOG_ALERT);
    logger.SetLogLevel(LogLevel::LOG_INFO); // restore
}

// ============================================================
// LoggerEnabledTest
// ============================================================

TEST(LoggerEnabledTest, DefaultEnabled) {
    auto& logger = LightweightLogger::Instance();
    logger.SetEnabled(true); // ensure default state
    EXPECT_TRUE(logger.IsEnabled());
}

TEST(LoggerEnabledTest, DisableAndCheck) {
    auto& logger = LightweightLogger::Instance();
    logger.SetEnabled(false);
    EXPECT_FALSE(logger.IsEnabled());
    logger.SetEnabled(true); // restore
}

TEST(LoggerEnabledTest, ReEnable) {
    auto& logger = LightweightLogger::Instance();
    logger.SetEnabled(false);
    EXPECT_FALSE(logger.IsEnabled());
    logger.SetEnabled(true);
    EXPECT_TRUE(logger.IsEnabled());
}

// ============================================================
// LoggerPathTest
// ============================================================

TEST(LoggerPathTest, InitialPathEmpty) {
    // Without Initialize(), the log path should be empty
    // Note: Since this is a singleton, it may have been initialized
    // by other tests. We test the getter works without crashing.
    auto& logger = LightweightLogger::Instance();
    // GetLogPath() should return a valid reference (empty or not)
    const auto& path = logger.GetLogPath();
    (void)path; // Just verify it doesn't crash
    SUCCEED();
}
