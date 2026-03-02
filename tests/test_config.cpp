// UnLeaf Unit Tests - Config parser/serializer/target management
// Tests: ParseIni, SerializeIni, AddTarget, RemoveTarget, IsTargetEnabled, SetTargetEnabled

#include <gtest/gtest.h>
#include "common/config.h"

using namespace unleaf;

// ============================================================
// ConfigParserTest - friend access to private ParseIni/SerializeIni
// ============================================================

class ConfigParserTest : public ::testing::Test {
protected:
    UnLeafConfig& config() { return UnLeafConfig::Instance(); }

    bool callParseIni(const std::string& content) {
        return config().ParseIni(content);
    }

    std::string callSerializeIni() {
        return config().SerializeIni();
    }

    void SetUp() override {
        auto& cfg = config();
        cfg.targets_.clear();
        cfg.logLevel_ = LogLevel::LOG_INFO;
        cfg.logEnabled_ = true;
    }
};

// --- ParseIni tests ---

TEST_F(ConfigParserTest, EmptyContent) {
    EXPECT_TRUE(callParseIni(""));
    EXPECT_TRUE(config().GetTargets().empty());
    EXPECT_EQ(config().GetLogLevel(), LogLevel::LOG_INFO);
}

TEST_F(ConfigParserTest, SingleTarget) {
    EXPECT_TRUE(callParseIni("[Targets]\nnotepad.exe=1\n"));
    EXPECT_EQ(config().GetTargets().size(), 1u);
    EXPECT_EQ(config().GetTargets()[0].name, L"notepad.exe");
    EXPECT_TRUE(config().GetTargets()[0].enabled);
}

TEST_F(ConfigParserTest, MultipleTargets) {
    EXPECT_TRUE(callParseIni(
        "[Targets]\n"
        "notepad.exe=1\n"
        "calc.exe=1\n"
        "mspaint.exe=0\n"
    ));
    EXPECT_EQ(config().GetTargets().size(), 3u);
}

TEST_F(ConfigParserTest, DisabledTarget) {
    EXPECT_TRUE(callParseIni("[Targets]\napp.exe=0\n"));
    EXPECT_EQ(config().GetTargets().size(), 1u);
    EXPECT_FALSE(config().GetTargets()[0].enabled);
}

TEST_F(ConfigParserTest, LogLevelError) {
    EXPECT_TRUE(callParseIni("[Logging]\nLogLevel=ERROR\n"));
    EXPECT_EQ(config().GetLogLevel(), LogLevel::LOG_ERROR);
}

TEST_F(ConfigParserTest, LogLevelDebug) {
    EXPECT_TRUE(callParseIni("[Logging]\nLogLevel=DEBUG\n"));
    EXPECT_EQ(config().GetLogLevel(), LogLevel::LOG_DEBUG);
}

TEST_F(ConfigParserTest, LogEnabledFalse) {
    EXPECT_TRUE(callParseIni("[Logging]\nLogEnabled=0\n"));
    EXPECT_FALSE(config().IsLogEnabled());
}

TEST_F(ConfigParserTest, InvalidTargetIgnored) {
    EXPECT_TRUE(callParseIni("[Targets]\n..\\evil.exe=1\n"));
    EXPECT_TRUE(config().GetTargets().empty());
}

TEST_F(ConfigParserTest, CriticalProcessBlocked) {
    EXPECT_TRUE(callParseIni("[Targets]\ncsrss.exe=1\n"));
    EXPECT_TRUE(config().GetTargets().empty());
}

TEST_F(ConfigParserTest, CommentAndBlankLines) {
    EXPECT_TRUE(callParseIni(
        "; This is a comment\n"
        "\n"
        "[Targets]\n"
        "notepad.exe=1\n"
    ));
    EXPECT_EQ(config().GetTargets().size(), 1u);
}

TEST_F(ConfigParserTest, UnknownSectionIgnored) {
    EXPECT_TRUE(callParseIni("[Unknown]\nkey=val\n"));
    EXPECT_TRUE(config().GetTargets().empty());
}

TEST_F(ConfigParserTest, InvalidLogLevelDefaultsToInfo) {
    EXPECT_TRUE(callParseIni("[Logging]\nLogLevel=INVALID\n"));
    EXPECT_EQ(config().GetLogLevel(), LogLevel::LOG_INFO);
}

TEST_F(ConfigParserTest, WhitespaceHandling) {
    EXPECT_TRUE(callParseIni("[Targets]\n notepad.exe = 1 \n"));
    EXPECT_EQ(config().GetTargets().size(), 1u);
    EXPECT_EQ(config().GetTargets()[0].name, L"notepad.exe");
}

// --- UTF-8 BOM tests ---

TEST_F(ConfigParserTest, Utf8BomStripped) {
    std::string bom = "\xEF\xBB\xBF";
    EXPECT_TRUE(callParseIni(bom + "[Targets]\nnotepad.exe=1\n"));
    EXPECT_EQ(config().GetTargets().size(), 1u);
}

TEST_F(ConfigParserTest, Utf8BomWithLoggingSection) {
    std::string bom = "\xEF\xBB\xBF";
    EXPECT_TRUE(callParseIni(bom + "[Logging]\nLogLevel=DEBUG\n[Targets]\napp.exe=1\n"));
    EXPECT_EQ(config().GetLogLevel(), LogLevel::LOG_DEBUG);
    EXPECT_EQ(config().GetTargets().size(), 1u);
}

TEST_F(ConfigParserTest, NoBomStillWorks) {
    EXPECT_TRUE(callParseIni("[Targets]\nnotepad.exe=1\n"));
    EXPECT_EQ(config().GetTargets().size(), 1u);
}

// --- SerializeIni tests ---

TEST_F(ConfigParserTest, SerializeEmptyTargets) {
    std::string ini = callSerializeIni();
    EXPECT_NE(ini.find("[Logging]"), std::string::npos);
    EXPECT_NE(ini.find("[Targets]"), std::string::npos);
    EXPECT_NE(ini.find("LogLevel=INFO"), std::string::npos);
}

TEST_F(ConfigParserTest, RoundTrip) {
    std::string input =
        "[Logging]\n"
        "LogLevel=DEBUG\n"
        "LogEnabled=0\n"
        "\n"
        "[Targets]\n"
        "notepad.exe=1\n"
        "calc.exe=0\n";

    EXPECT_TRUE(callParseIni(input));
    EXPECT_EQ(config().GetLogLevel(), LogLevel::LOG_DEBUG);
    EXPECT_FALSE(config().IsLogEnabled());
    EXPECT_EQ(config().GetTargets().size(), 2u);

    std::string serialized = callSerializeIni();

    // Re-parse the serialized output
    EXPECT_TRUE(callParseIni(serialized));
    EXPECT_EQ(config().GetLogLevel(), LogLevel::LOG_DEBUG);
    EXPECT_FALSE(config().IsLogEnabled());
    EXPECT_EQ(config().GetTargets().size(), 2u);

    auto& targets = config().GetTargets();
    EXPECT_EQ(targets[0].name, L"notepad.exe");
    EXPECT_TRUE(targets[0].enabled);
    EXPECT_EQ(targets[1].name, L"calc.exe");
    EXPECT_FALSE(targets[1].enabled);
}

// ============================================================
// ConfigTargetTest - target management via public API
// (inherits ConfigParserTest for SetUp cleanup via friend access)
// ============================================================

class ConfigTargetTest : public ConfigParserTest {};

TEST_F(ConfigTargetTest, AddAndQuery) {
    EXPECT_TRUE(config().AddTarget(L"notepad.exe"));
    EXPECT_TRUE(config().IsTargetEnabled(L"notepad.exe"));
}

TEST_F(ConfigTargetTest, AddDuplicate) {
    EXPECT_TRUE(config().AddTarget(L"notepad.exe"));
    EXPECT_FALSE(config().AddTarget(L"notepad.exe"));
}

TEST_F(ConfigTargetTest, AddCritical) {
    EXPECT_FALSE(config().AddTarget(L"csrss.exe"));
}

TEST_F(ConfigTargetTest, RemoveExisting) {
    config().AddTarget(L"notepad.exe");
    EXPECT_TRUE(config().RemoveTarget(L"notepad.exe"));
    EXPECT_FALSE(config().IsTargetEnabled(L"notepad.exe"));
}

TEST_F(ConfigTargetTest, RemoveNonExistent) {
    EXPECT_FALSE(config().RemoveTarget(L"nonexistent.exe"));
}

TEST_F(ConfigTargetTest, CaseInsensitiveQuery) {
    config().AddTarget(L"notepad.exe");
    EXPECT_TRUE(config().IsTargetEnabled(L"NOTEPAD.EXE"));
}

TEST_F(ConfigTargetTest, SetTargetEnabled) {
    config().AddTarget(L"notepad.exe");
    EXPECT_TRUE(config().IsTargetEnabled(L"notepad.exe"));
    EXPECT_TRUE(config().SetTargetEnabled(L"notepad.exe", false));
    EXPECT_FALSE(config().IsTargetEnabled(L"notepad.exe"));
}

TEST_F(ConfigTargetTest, SetTargetEnabledNotFound) {
    EXPECT_FALSE(config().SetTargetEnabled(L"nonexistent.exe", true));
}
