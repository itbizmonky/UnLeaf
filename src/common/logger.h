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
#include <functional>
#include <vector>

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
    void Manager(const std::wstring& message);

    void SetLogLevel(LogLevel level);
    LogLevel GetLogLevel() const { return currentLevel_; }

    void SetEnabled(bool enabled);
    bool IsEnabled() const;

    // Enable log rotation (Service process only; Manager keeps default false)
    void SetRotationEnabled(bool enabled);

    // Register UI callback (called after each log write; use PostMessage inside, not direct UI calls)
    void SetUICallback(std::function<void(const std::wstring&)> cb);

    // Enable/disable console output (for debug builds)
    void SetConsoleOutput(bool enabled);

    // Get log file path
    const std::wstring& GetLogPath() const { return logPath_; }

private:
    LightweightLogger();
    ~LightweightLogger();
    LightweightLogger(const LightweightLogger&) = delete;
    LightweightLogger& operator=(const LightweightLogger&) = delete;

    // Rotation outcome returned by CheckRotation(). Never calls Log().
    struct RotationResult {
        bool  triggered   = false;  // true if rotation was attempted (size threshold reached)
        bool  success     = false;  // true if rename succeeded
        bool  mutexFailed = false;  // true if mutex acquisition failed
        DWORD error       = 0;      // GetLastError() on failure
    };

    // Write message to file with rotation check
    void WriteMessage(const std::wstring& formattedMessage);

    // Check and perform log rotation (Service only; returns immediately when rotationEnabled_=false).
    // NEVER calls Log() or SafeInternalLog(). Returns result for the caller to act on.
    RotationResult CheckRotation();

    // Detect and reopen stale file handle (Manager convergence after Service rotation)
    void CheckStaleHandle();

    // Write a pre-formatted line directly to fileHandle_ without rotation check or callback.
    // If fileHandle_ is INVALID, temporarily opens OPEN_ALWAYS, writes, then closes.
    // Called under cs_ lock (from WriteMessage()). Used only for rotation meta-messages.
    void SafeInternalLog(const std::wstring& msg);

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

    // UI callback: called after each log write (Manager process only; nullptr in Service)
    std::function<void(const std::wstring&)> uiCallback_;

    bool     rotationEnabled_;   // true = Service only; Manager keeps false (default)
    HANDLE   hRotationMutex_;    // inter-process rotation mutex (Global\UnLeafLogRotation)
    HANDLE   hRotationEvent_;    // inter-process rotation signal (Global\UnLeafLogRotated)
    uint32_t staleCheckCounter_; // periodic stale handle detection counter (Manager)

    void Log(LogLevel level, const wchar_t* levelStr, const std::wstring& message);
};

#define LOG_ERROR(msg)   unleaf::LightweightLogger::Instance().Error(msg)
#define LOG_ALERT(msg)   unleaf::LightweightLogger::Instance().Alert(msg)
#define LOG_INFO(msg)    unleaf::LightweightLogger::Instance().Info(msg)
#define LOG_DEBUG(msg)   unleaf::LightweightLogger::Instance().Debug(msg)
#define LOG_MANAGER(msg) unleaf::LightweightLogger::Instance().Manager(msg)

} // namespace unleaf
