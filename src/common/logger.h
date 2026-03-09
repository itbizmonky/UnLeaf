#pragma once
// UnLeaf - Lightweight Logger
// Thread-safe rotating file logger with 100KB limit

#include "types.h"
#include "scoped_handle.h"
#include <string>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <chrono>

namespace unleaf {

class LightweightLogger {
public:
    // Singleton access
    static LightweightLogger& Instance();

    // Initialize with base directory
    bool Initialize(const std::wstring& baseDir);

    // Shutdown logger
    void Shutdown();

    void Error(const std::wstring& message);
    void Error(const std::string& message);
    void Alert(const std::wstring& message);
    void Alert(const std::string& message);
    void Info(const std::wstring& message);
    void Info(const std::string& message);
    void Debug(const std::wstring& message);
    void Debug(const std::string& message);

    void SetLogLevel(LogLevel level);
    LogLevel GetLogLevel() const { return currentLevel_; }

    void SetEnabled(bool enabled);
    bool IsEnabled() const;

    // Enable/disable console output (for debug builds)
    void SetConsoleOutput(bool enabled);

    // Get log file path
    const std::wstring& GetLogPath() const { return logPath_; }

private:
    LightweightLogger();
    ~LightweightLogger();
    LightweightLogger(const LightweightLogger&) = delete;
    LightweightLogger& operator=(const LightweightLogger&) = delete;

    // Write message to file with rotation check
    void WriteMessage(const std::wstring& formattedMessage);

    // Check and perform log rotation
    void CheckRotation();

    // Get current timestamp string
    std::wstring GetTimestamp() const;

    // Convert UTF-8 to UTF-16
    std::wstring Utf8ToWide(const std::string& str) const;

    // Convert UTF-16 to UTF-8
    std::string WideToUtf8(const std::wstring& str) const;

    std::wstring logPath_;
    std::wstring backupPath_;
    HANDLE fileHandle_;
    CriticalSection cs_;
    bool initialized_;
    bool consoleOutput_;
    LogLevel currentLevel_;
    std::atomic<bool> enabled_;

    // Console handle for debug output
    HANDLE consoleHandle_;

    void Log(LogLevel level, const wchar_t* levelStr, const std::wstring& message);
};

#define LOG_ERROR(msg) unleaf::LightweightLogger::Instance().Error(msg)
#define LOG_ALERT(msg) unleaf::LightweightLogger::Instance().Alert(msg)
#define LOG_INFO(msg)  unleaf::LightweightLogger::Instance().Info(msg)
#define LOG_DEBUG(msg) unleaf::LightweightLogger::Instance().Debug(msg)

} // namespace unleaf
