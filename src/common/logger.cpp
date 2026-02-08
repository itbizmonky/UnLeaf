// UnLeaf v7.90 - Lightweight Logger Implementation
// v7.6: Added multi-level logging (ERROR/ALERT/INFO/DEBUG)
// v7.90: Changed log format to 4-char fixed width (ERR /ALRT/INFO/DEBG)

#include "logger.h"
#include <cstdio>
#include <iostream>

namespace unleaf {

LightweightLogger& LightweightLogger::Instance() {
    static LightweightLogger instance;
    return instance;
}

LightweightLogger::LightweightLogger()
    : fileHandle_(INVALID_HANDLE_VALUE)
    , initialized_(false)
    , consoleOutput_(false)
    , currentLevel_(LogLevel::LOG_INFO)  // v7.6: Default to INFO
    , enabled_(true)                     // v7.93: Default to enabled
    , consoleHandle_(nullptr) {
}

LightweightLogger::~LightweightLogger() {
    Shutdown();
}

bool LightweightLogger::Initialize(const std::wstring& baseDir) {
    CSLockGuard lock(cs_);

    if (initialized_) {
        return true;
    }

    logPath_ = baseDir + L"\\" + LOG_FILENAME;
    backupPath_ = baseDir + L"\\" + LOG_BACKUP_FILENAME;

    // Open file for append
    fileHandle_ = CreateFileW(
        logPath_.c_str(),
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (fileHandle_ == INVALID_HANDLE_VALUE) {
        return false;
    }

    initialized_ = true;

#ifdef _DEBUG
    // In debug builds, allocate console for output
    SetConsoleOutput(true);
#endif

    return true;
}

void LightweightLogger::Shutdown() {
    CSLockGuard lock(cs_);

    if (fileHandle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(fileHandle_);
        fileHandle_ = INVALID_HANDLE_VALUE;
    }

    if (consoleHandle_) {
        FreeConsole();
        consoleHandle_ = nullptr;
    }

    initialized_ = false;
}

void LightweightLogger::SetConsoleOutput(bool enabled) {
    CSLockGuard lock(cs_);
    consoleOutput_ = enabled;

    if (enabled && !consoleHandle_) {
        if (AllocConsole()) {
            consoleHandle_ = GetStdHandle(STD_OUTPUT_HANDLE);
            // Redirect stdout to console
            FILE* fp = nullptr;
            freopen_s(&fp, "CONOUT$", "w", stdout);
        }
    }
}

// v7.6: Set minimum log level
void LightweightLogger::SetLogLevel(LogLevel level) {
    CSLockGuard lock(cs_);
    currentLevel_ = level;
}

// v7.6: Internal log method with level check
void LightweightLogger::Log(LogLevel level, const wchar_t* levelStr, const std::wstring& message) {
    if (!initialized_) return;
    // v7.93: Check if logging is enabled
    if (!enabled_.load(std::memory_order_acquire)) return;
    if (static_cast<uint8_t>(level) > static_cast<uint8_t>(currentLevel_)) return;

    std::wstring timestamp = GetTimestamp();
    std::wstring formatted = L"[" + timestamp + L"] [" + levelStr + L"] " + message;

    WriteMessage(formatted);
}

// v7.6: Error level (always output)
// v7.90: Format changed to 4-char fixed width
void LightweightLogger::Error(const std::wstring& message) {
    Log(LogLevel::LOG_ERROR, L"ERR ", message);
}

void LightweightLogger::Error(const std::string& message) {
    Error(Utf8ToWide(message));
}

// v7.6: Alert level (warnings)
// v7.90: Format changed to 4-char fixed width
void LightweightLogger::Alert(const std::wstring& message) {
    Log(LogLevel::LOG_ALERT, L"ALRT", message);
}

void LightweightLogger::Alert(const std::string& message) {
    Alert(Utf8ToWide(message));
}

// Info level (normal operation)
// v7.90: Format changed to 4-char fixed width
void LightweightLogger::Info(const std::wstring& message) {
    Log(LogLevel::LOG_INFO, L"INFO", message);
}

void LightweightLogger::Info(const std::string& message) {
    Info(Utf8ToWide(message));
}

// v7.6: Debug level (development details)
// v7.90: Format changed to 4-char fixed width
void LightweightLogger::Debug(const std::wstring& message) {
    Log(LogLevel::LOG_DEBUG, L"DEBG", message);
}

void LightweightLogger::Debug(const std::string& message) {
    Debug(Utf8ToWide(message));
}

void LightweightLogger::WriteMessage(const std::wstring& formattedMessage) {
    CSLockGuard lock(cs_);

    if (fileHandle_ == INVALID_HANDLE_VALUE) return;

    // Check rotation before writing
    CheckRotation();

    // Convert to UTF-8 for file
    std::string utf8Msg = WideToUtf8(formattedMessage);
    utf8Msg += "\r\n";

    // Write to file
    DWORD bytesWritten = 0;
    WriteFile(fileHandle_, utf8Msg.c_str(),
              static_cast<DWORD>(utf8Msg.size()), &bytesWritten, nullptr);
    FlushFileBuffers(fileHandle_);

    // Console output if enabled
    if (consoleOutput_ && consoleHandle_) {
        std::wcout << formattedMessage << std::endl;
    }
}

void LightweightLogger::CheckRotation() {
    // Get current file size
    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(fileHandle_, &fileSize)) {
        Log(LogLevel::LOG_DEBUG, L"DEBG", L"Logger: CheckRotation - GetFileSizeEx failed");
        return;
    }

    if (static_cast<size_t>(fileSize.QuadPart) >= MAX_LOG_SIZE) {
        // Close current file
        CloseHandle(fileHandle_);
        fileHandle_ = INVALID_HANDLE_VALUE;

        // Delete old backup if exists
        bool deleteFailed = !DeleteFileW(backupPath_.c_str())
                            && GetLastError() != ERROR_FILE_NOT_FOUND;

        // Rename current log to backup
        bool moveFailed = !MoveFileW(logPath_.c_str(), backupPath_.c_str());

        // Open new log file
        fileHandle_ = CreateFileW(
            logPath_.c_str(),
            FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );

        if (fileHandle_ != INVALID_HANDLE_VALUE) {
            if (deleteFailed) {
                Log(LogLevel::LOG_DEBUG, L"DEBG", L"Logger: Rotation - backup delete failed");
            }
            if (moveFailed) {
                Log(LogLevel::LOG_DEBUG, L"DEBG", L"Logger: Rotation - log rename failed");
            }
        }
    }
}

std::wstring LightweightLogger::GetTimestamp() const {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);

    std::tm tm_buf;
    localtime_s(&tm_buf, &time);

    wchar_t buffer[32];
    wcsftime(buffer, 32, L"%Y/%m/%d %H:%M:%S", &tm_buf);
    return std::wstring(buffer);
}

std::wstring LightweightLogger::Utf8ToWide(const std::string& str) const {
    if (str.empty()) return L"";

    int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(),
                                   static_cast<int>(str.size()), nullptr, 0);
    if (size <= 0) return L"";

    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(),
                       static_cast<int>(str.size()), &result[0], size);
    return result;
}

std::string LightweightLogger::WideToUtf8(const std::wstring& str) const {
    if (str.empty()) return "";

    int size = WideCharToMultiByte(CP_UTF8, 0, str.c_str(),
                                   static_cast<int>(str.size()),
                                   nullptr, 0, nullptr, nullptr);
    if (size <= 0) return "";

    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, str.c_str(),
                       static_cast<int>(str.size()),
                       &result[0], size, nullptr, nullptr);
    return result;
}

// v7.93: Enable/disable log output
void LightweightLogger::SetEnabled(bool enabled) {
    enabled_.store(enabled, std::memory_order_release);
}

bool LightweightLogger::IsEnabled() const {
    return enabled_.load(std::memory_order_acquire);
}

} // namespace unleaf
