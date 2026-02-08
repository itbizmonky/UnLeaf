#pragma once
// UnLeaf v1.00 - Common Type Definitions
// Windows Native C++ Implementation

// Prevent Windows macro conflicts
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>

// Undefine problematic macros after including windows.h
#ifdef ERROR
#undef ERROR
#endif
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include <string>
#include <vector>
#include <map>
#include <set>
#include <memory>
#include <functional>
#include <atomic>
#include <mutex>
#include <cstdint>

namespace unleaf {

// Version Information
constexpr const wchar_t* VERSION = L"1.00";

// v7.94: Windows version detection constants
constexpr DWORD WINDOWS_11_BUILD_THRESHOLD = 22000;  // Windows 11 starts at build 22000

// v7.6: Log level definitions
enum class LogLevel : uint8_t {
    LOG_ERROR = 0,   // Always output (critical errors)
    LOG_ALERT = 1,   // ALERT and above (warnings)
    LOG_INFO  = 2,   // INFO and above (normal operation) - default
    LOG_DEBUG = 3    // DEBUG only (development details)
};
constexpr const wchar_t* SERVICE_NAME = L"UnLeafService";
constexpr const wchar_t* SERVICE_DISPLAY_NAME = L"UnLeaf Service";
constexpr const wchar_t* SERVICE_DESCRIPTION = L"Optimization Engine (Native C++ Edition)";
constexpr const wchar_t* PIPE_NAME = L"\\\\.\\pipe\\UnLeafServicePipe";

// File Names
constexpr const wchar_t* CONFIG_FILENAME = L"UnLeaf.ini";
constexpr const wchar_t* CONFIG_FILENAME_OLD = L"UnLeaf.json";  // For migration
constexpr const wchar_t* LOG_FILENAME = L"UnLeaf.log";
constexpr const wchar_t* LOG_BACKUP_FILENAME = L"UnLeaf.log.1";

// Default Values
constexpr size_t MAX_LOG_SIZE = 102400;    // 100KB

// v7.5: Input validation constants
constexpr DWORD UNLEAF_MIN_INTERVAL_MS = 10;
constexpr DWORD UNLEAF_MAX_INTERVAL_MS = 60000;
constexpr size_t UNLEAF_MAX_PROCESS_NAME_LEN = 260;

// v7.80: IPC constants
constexpr size_t UNLEAF_MAX_IPC_DATA_SIZE = 65536;    // Maximum IPC message data size
constexpr size_t UNLEAF_MAX_LOG_READ_SIZE = 8192;     // Maximum log read per request

// Process Power Throttling (EcoQoS) - Windows 11 specific
// Use unique names to avoid conflicts with Windows SDK
constexpr ULONG UNLEAF_THROTTLE_VERSION = 1;
constexpr ULONG UNLEAF_THROTTLE_EXECUTION_SPEED = 0x1;
constexpr ULONG UNLEAF_THROTTLE_IGNORE_TIMER = 0x4;

// ProcessInformationClass value for SetProcessInformation
constexpr int UNLEAF_PROCESS_POWER_THROTTLING = 4;

// Priority Class (HIGH_PRIORITY_CLASS to avoid OS auto-EcoQoS)
constexpr DWORD UNLEAF_TARGET_PRIORITY = HIGH_PRIORITY_CLASS;

// Minimum acceptable priority (anything below this triggers re-optimization)
constexpr DWORD UNLEAF_MIN_PRIORITY = NORMAL_PRIORITY_CLASS;

// v5.0: Fixed-size buffer constants
constexpr size_t MAX_TRACKED_PROCESSES = 256;
constexpr size_t MAX_JOB_PIDS = 1024;

// v5.0: Self-healing constants
constexpr uint8_t MAX_RETRY_COUNT = 5;
constexpr DWORD RETRY_BACKOFF_BASE_MS = 50;

// v6.0: NT API type definitions for NtSetInformationProcess
// ProcessInformationClass for power throttling
constexpr ULONG NT_PROCESS_POWER_THROTTLING_STATE = 77;

// NTSTATUS codes
using NTSTATUS = LONG;
constexpr NTSTATUS STATUS_SUCCESS = 0;
constexpr NTSTATUS STATUS_INFO_LENGTH_MISMATCH = 0xC0000004L;
constexpr NTSTATUS STATUS_ACCESS_DENIED = 0xC0000022L;

// v8.0: Old polling-based constants removed (replaced by event-driven architecture)

// Background mode constants (for SetPriorityClass)
#ifndef PROCESS_MODE_BACKGROUND_BEGIN
#define PROCESS_MODE_BACKGROUND_BEGIN 0x00100000
#endif
#ifndef PROCESS_MODE_BACKGROUND_END
#define PROCESS_MODE_BACKGROUND_END   0x00200000
#endif

// Thread Power Throttling constants
constexpr ULONG UNLEAF_THREAD_THROTTLE_VERSION = 1;
constexpr ULONG UNLEAF_THREAD_THROTTLE_EXECUTION_SPEED = 0x1;

// Structure matching Windows THREAD_POWER_THROTTLING_STATE
struct UnleafThreadThrottleState {
    ULONG Version;
    ULONG ControlMask;
    ULONG StateMask;
};

// ThreadInformationClass value for SetThreadInformation
constexpr int UNLEAF_THREAD_POWER_THROTTLING = 4;

// Structure matching Windows PROCESS_POWER_THROTTLING_STATE
struct UnleafThrottleState {
    ULONG Version;
    ULONG ControlMask;
    ULONG StateMask;
};

// v5.0: Pre-allocated check entry (cache-line aligned for performance)
struct alignas(64) CheckInfoEntry {
    HANDLE handle;
    DWORD pid;
    DWORD rootPid;
    uint8_t phase;
    uint8_t retryCount;
    uint16_t flags;           // 0x01=isChild, 0x02=inJob
    ULONGLONG lastEnforceTime;
};

// v5.0: Job Object tracking (uses raw HANDLE, managed by EngineCore)
struct JobObjectInfo {
    HANDLE jobHandle;
    DWORD rootPid;
    bool isOwnJob;            // true if we created it

    JobObjectInfo() : jobHandle(nullptr), rootPid(0), isOwnJob(false) {}
    ~JobObjectInfo() {
        if (jobHandle && jobHandle != INVALID_HANDLE_VALUE) {
            CloseHandle(jobHandle);
        }
    }
    // Non-copyable
    JobObjectInfo(const JobObjectInfo&) = delete;
    JobObjectInfo& operator=(const JobObjectInfo&) = delete;
    // Movable
    JobObjectInfo(JobObjectInfo&& other) noexcept
        : jobHandle(other.jobHandle), rootPid(other.rootPid), isOwnJob(other.isOwnJob) {
        other.jobHandle = nullptr;
    }
    JobObjectInfo& operator=(JobObjectInfo&& other) noexcept {
        if (this != &other) {
            if (jobHandle && jobHandle != INVALID_HANDLE_VALUE) {
                CloseHandle(jobHandle);
            }
            jobHandle = other.jobHandle;
            rootPid = other.rootPid;
            isOwnJob = other.isOwnJob;
            other.jobHandle = nullptr;
        }
        return *this;
    }
};

// Target Process Configuration
struct TargetProcess {
    std::wstring name;
    bool enabled;

    TargetProcess() : enabled(true) {}
    TargetProcess(const std::wstring& n, bool e = true) : name(n), enabled(e) {}
};

// IPC Commands
enum class IPCCommand : uint32_t {
    CMD_ADD_TARGET = 1,
    CMD_REMOVE_TARGET = 2,
    CMD_GET_STATUS = 3,
    CMD_STOP_SERVICE = 4,
    CMD_GET_CONFIG = 5,
    CMD_SET_INTERVAL = 6,
    CMD_GET_LOGS = 7,
    CMD_GET_STATS = 8,           // Performance statistics and active count
    CMD_HEALTH_CHECK = 9,        // v7.7: Health check API
    CMD_SET_LOG_ENABLED = 10     // v7.93: Enable/disable log output
};

// IPC Response Types
enum class IPCResponse : uint32_t {
    RESP_SUCCESS = 0,
    RESP_ERROR_GENERAL = 1,
    RESP_ERROR_NOT_FOUND = 2,
    RESP_ERROR_ACCESS_DENIED = 3,     // v7.5: Authorization error
    RESP_ERROR_INVALID_INPUT = 4,     // v7.5: Input validation error
    RESP_STATUS_UPDATE = 10,
    RESP_LOG_STREAM = 11
};

// Log request structure (sent with CMD_GET_LOGS)
// Packed to ensure binary compatibility between Service and Manager
#pragma pack(push, 1)
struct LogRequest {
    uint64_t offset;  // Client's current read position (0 = from beginning)
};

// Log response header (returned before log data)
struct LogResponseHeader {
    uint64_t newOffset;   // New offset after this data
    uint32_t dataLength;  // Length of log data following this header
};
#pragma pack(pop)

// System Critical Processes (Protection List)
inline const std::set<std::wstring>& GetCriticalProcesses() {
static const std::set<std::wstring> critical = {
        L"ntoskrnl.exe", L"smss.exe", L"csrss.exe", L"wininit.exe", 
        L"services.exe", L"lsass.exe", L"winlogon.exe", L"svchost.exe",
        L"explorer.exe", L"dwm.exe", L"ctfmon.exe", L"unleaf_service.exe", 
        L"unleaf_manager.exe"
    };
    return critical;
}

// Utility: Convert string to lowercase
inline std::wstring ToLower(const std::wstring& str) {
    std::wstring result = str;
    for (auto& ch : result) {
        ch = towlower(ch);
    }
    return result;
}

// v7.80: Enhanced process name validation
// Allowed: alphanumeric, underscore, dot, hyphen; must end with .exe
// Blocked: path traversal, absolute paths, directory separators
inline bool IsValidProcessName(const std::wstring& name) {
    if (name.empty() || name.length() > UNLEAF_MAX_PROCESS_NAME_LEN) {
        return false;
    }

    // v7.80: Block path traversal attempts
    if (name.find(L"..") != std::wstring::npos) {
        return false;
    }

    // v7.80: Block absolute paths (drive letters or UNC)
    if (name.length() >= 2) {
        if (name[1] == L':') return false;  // C:\path
        if (name[0] == L'\\' || name[0] == L'/') return false;  // \\server or /path
    }

    // v7.80: Block directory separators
    if (name.find(L'\\') != std::wstring::npos ||
        name.find(L'/') != std::wstring::npos) {
        return false;
    }

    // Check for valid characters only
    for (wchar_t c : name) {
        if (!iswalnum(c) && c != L'_' && c != L'.' && c != L'-') {
            return false;
        }
    }

    // Must end with .exe
    if (name.length() < 4) return false;
    std::wstring ext = name.substr(name.length() - 4);
    for (auto& ch : ext) ch = towlower(ch);
    return ext == L".exe";
}

// Utility: Check if process is critical
inline bool IsCriticalProcess(const std::wstring& name) {
    return GetCriticalProcesses().count(ToLower(name)) > 0;
}

} // namespace unleaf
