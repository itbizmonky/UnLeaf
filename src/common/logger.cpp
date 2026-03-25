// UnLeaf - Lightweight Logger Implementation

#include "logger.h"
#include <cassert>
#include <iostream>
#include <vector>

// POSIX rename constants — available since Windows 10 1607 (Build 14393).
// UnLeaf minimum requirement: Windows 1709 (Build 16299). Safe to use unconditionally.
#ifndef FILE_RENAME_FLAG_REPLACE_IF_EXISTS
#   define FILE_RENAME_FLAG_REPLACE_IF_EXISTS 0x00000001U
#   define FILE_RENAME_FLAG_POSIX_SEMANTICS   0x00000002U
#endif

// FileRenameInfoEx in FILE_INFO_BY_HANDLE_CLASS (SetFileInformationByHandle) = 22.
// Added in Windows SDK 10.0.17134.0 (RS4 / 1803). UnLeaf requires 1709, so guard
// with #ifndef to use the SDK definition when available.
#ifndef FileRenameInfoEx
#   define FileRenameInfoEx ((FILE_INFO_BY_HANDLE_CLASS)22)
#endif

namespace unleaf {

// RAII guard for the inter-process rotation mutex.
// Acquires on construction (INFINITE wait); releases on destruction if acquired.
// Eliminates manual ReleaseMutex on every return path in CheckRotation().
class RotationMutexGuard {
public:
    explicit RotationMutexGuard(HANDLE h) : h_(h), acquired_(false), error_(0) {
        if (h_) {
            DWORD wait = WaitForSingleObject(h_, INFINITE);
            acquired_ = (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED);
            if (!acquired_) {
                error_ = GetLastError();
            }
        }
    }

    ~RotationMutexGuard() {
        if (acquired_ && h_) {
            ReleaseMutex(h_);
        }
    }

    RotationMutexGuard(const RotationMutexGuard&) = delete;
    RotationMutexGuard& operator=(const RotationMutexGuard&) = delete;

    bool  Acquired() const { return acquired_; }
    DWORD Error()    const { return error_; }

private:
    HANDLE h_;
    bool   acquired_;
    DWORD  error_;
};

LightweightLogger& LightweightLogger::Instance() {
    static LightweightLogger instance;
    return instance;
}

LightweightLogger::LightweightLogger()
    : fileHandle_(INVALID_HANDLE_VALUE)
    , initialized_(false)
    , consoleOutput_(false)
    , currentLevel_(LogLevel::LOG_INFO)
    , enabled_(true)
    , consoleHandle_(nullptr)
    , rotationEnabled_(false)
    , hRotationMutex_(nullptr)
    , hRotationEvent_(nullptr)
    , staleCheckCounter_(0) {
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

    // Open file for append.
    // FILE_SHARE_DELETE is required so that the POSIX rename (SetFileInformationByHandle)
    // can succeed while any other process has this file open.
    fileHandle_ = CreateFileW(
        logPath_.c_str(),
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (fileHandle_ == INVALID_HANDLE_VALUE) {
        return false;
    }

    SetFilePointer(fileHandle_, 0, nullptr, FILE_END);

    // Inter-process rotation mutex: serializes rotation so only one writer rotates at a time.
    // Service acquires this during rotation; Manager never calls SetRotationEnabled(true).
    hRotationMutex_ = CreateMutexW(nullptr, FALSE, L"Global\\UnLeafLogRotation");

    // Inter-process rotation event: Service sets this immediately after a successful rotation.
    // Manager polls it (WaitForSingleObject with 0 timeout) before each write to detect stale
    // handles without waiting for the 100-write counter. auto-reset: consumed by first waiter.
    hRotationEvent_ = CreateEventW(nullptr, FALSE, FALSE, L"Global\\UnLeafLogRotated");
    // Failure is non-fatal; staleCheckCounter_ fallback continues to operate.

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

    if (hRotationMutex_) {
        CloseHandle(hRotationMutex_);
        hRotationMutex_ = nullptr;
    }

    if (hRotationEvent_) {
        CloseHandle(hRotationEvent_);
        hRotationEvent_ = nullptr;
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
            if (consoleHandle_ == INVALID_HANDLE_VALUE) consoleHandle_ = nullptr;
            // Redirect stdout to console
            FILE* fp = nullptr;
            freopen_s(&fp, "CONOUT$", "w", stdout);
        }
    }
}

void LightweightLogger::SetLogLevel(LogLevel level) {
    CSLockGuard lock(cs_);
    currentLevel_ = level;
}

void LightweightLogger::Log(LogLevel level, const wchar_t* levelStr, const std::wstring& message) {
    if (!initialized_) return;
    if (!enabled_.load(std::memory_order_acquire)) return;
    if (static_cast<uint8_t>(level) > static_cast<uint8_t>(currentLevel_)) return;

    std::wstring timestamp = GetTimestamp();
    std::wstring formatted = timestamp + L" " + levelStr + L" " + message;

    WriteMessage(formatted);

    // Notify UI callback (Manager process only; copy under lock to avoid races with SetUICallback)
    std::function<void(const std::wstring&)> cbCopy;
    {
        CSLockGuard lock(cs_);
        cbCopy = uiCallback_;
    }
    if (cbCopy) cbCopy(formatted);
}

void LightweightLogger::SetUICallback(std::function<void(const std::wstring&)> cb) {
    CSLockGuard lock(cs_);
    uiCallback_ = std::move(cb);
}

void LightweightLogger::Error(const std::wstring& message) {
    Log(LogLevel::LOG_ERROR, L"E", message);
}

void LightweightLogger::Error(const std::string& message) {
    Error(Utf8ToWide(message));
}

void LightweightLogger::Alert(const std::wstring& message) {
    Log(LogLevel::LOG_ALERT, L"A", message);
}

void LightweightLogger::Alert(const std::string& message) {
    Alert(Utf8ToWide(message));
}

void LightweightLogger::Info(const std::wstring& message) {
    Log(LogLevel::LOG_INFO, L"I", message);
}

void LightweightLogger::Info(const std::string& message) {
    Info(Utf8ToWide(message));
}

void LightweightLogger::Debug(const std::wstring& message) {
    Log(LogLevel::LOG_DEBUG, L"D", message);
}

void LightweightLogger::Debug(const std::string& message) {
    Debug(Utf8ToWide(message));
}

void LightweightLogger::Manager(const std::wstring& message) {
    Log(LogLevel::LOG_INFO, L"M", message);
}

void LightweightLogger::WriteMessage(const std::wstring& formattedMessage) {
    CSLockGuard lock(cs_);

    if (!initialized_) return;

    // Reopen if handle was invalidated (e.g., by a previous rotation attempt).
    if (fileHandle_ == INVALID_HANDLE_VALUE) {
        fileHandle_ = CreateFileW(
            logPath_.c_str(),
            FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (fileHandle_ != INVALID_HANDLE_VALUE)
            SetFilePointer(fileHandle_, 0, nullptr, FILE_END);
    }
    if (fileHandle_ == INVALID_HANDLE_VALUE) return;

    // Service: attempt rotation if size threshold reached.
    // CheckRotation() NEVER calls Log() or SafeInternalLog() — no recursion possible.
    // FlushFileBuffers is performed only inside CheckRotation(), immediately before rename.
    RotationResult rotResult = CheckRotation();

    // After rotation, fileHandle_ may be INVALID (closed for rename); reopen before write.
    if (fileHandle_ == INVALID_HANDLE_VALUE) {
        fileHandle_ = CreateFileW(
            logPath_.c_str(),
            FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (fileHandle_ != INVALID_HANDLE_VALUE)
            SetFilePointer(fileHandle_, 0, nullptr, FILE_END);
        if (fileHandle_ == INVALID_HANDLE_VALUE) return;
    }

    // Manager: stale handle detection BEFORE the write, ensuring this write lands in the
    // new file rather than the already-renamed UnLeaf.log.1.
    // rotationSignaled: fires immediately when Service sets Global\UnLeafLogRotated.
    // Counter fallback: catches cases where hRotationEvent_ is null or signal was lost.
    if (!rotationEnabled_) {
        const bool rotationSignaled =
            hRotationEvent_ &&
            WaitForSingleObject(hRotationEvent_, 0) == WAIT_OBJECT_0;
        if (rotationSignaled) {
            staleCheckCounter_ = 0;
            CheckStaleHandle();
        } else if (++staleCheckCounter_ >= 100) {
            staleCheckCounter_ = 0;
            CheckStaleHandle();
        }
        if (fileHandle_ == INVALID_HANDLE_VALUE) return;  // guard: stale reopen failed
    }

    // Write the caller's message (no FlushFileBuffers — rotation handles its own flush).
    std::string utf8Msg = WideToUtf8(formattedMessage);
    utf8Msg += "\r\n";
    DWORD bytesWritten = 0;
    if (!WriteFile(fileHandle_, utf8Msg.c_str(),
                   static_cast<DWORD>(utf8Msg.size()), &bytesWritten, nullptr)) {
        // FAIL-SAFE: disable further logging to prevent I/O error loops.
        // Service remains running; rotation and other operations are unaffected.
        enabled_.store(false, std::memory_order_release);
    }

    // Report rotation outcome only when rotation was actually attempted.
    // SafeInternalLog() writes directly to fileHandle_ — CheckRotation() is never called
    // from within it, so this path cannot recurse back into WriteMessage().
    if (rotResult.triggered) {
        const std::wstring ts = GetTimestamp();
        if (rotResult.mutexFailed) {
            SafeInternalLog(ts + L" A [LOGGER] Rotation mutex wait failed (err=" +
                            std::to_wstring(rotResult.error) + L")");
        } else if (!rotResult.success) {
            SafeInternalLog(ts + L" A [LOGGER] Rotation FAILED (err=" +
                            std::to_wstring(rotResult.error) +
                            L") - original log preserved; will retry on next write");
        } else {
            SafeInternalLog(ts + L" I [LOGGER] Log rotated successfully");
        }
    }

    if (consoleOutput_ && consoleHandle_) {
        std::wcout << formattedMessage << std::endl;
    }
}

LightweightLogger::RotationResult LightweightLogger::CheckRotation() {
    // Called from WriteMessage() under cs_ lock.
    // NEVER calls Log() or SafeInternalLog() — structurally incapable of recursion.
    // Returns RotationResult; all diagnostics are handled by the caller (WriteMessage).
    //
    // FlushFileBuffers is called ONLY here (rotation boundary).
    // Normal WriteFile operations intentionally do NOT flush for performance reasons.

    RotationResult result;
    if (!rotationEnabled_) return result;  // Manager: always fast-exits here

    // Fast-path size check (no mutex — avoids kernel call on every non-rotating write).
    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(fileHandle_, &fileSize)) return result;
    if (static_cast<size_t>(fileSize.QuadPart) < MAX_LOG_SIZE) return result;

#ifdef _DEBUG
    {
        wchar_t exePath[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        assert(std::wstring(exePath).find(L"_Service") != std::wstring::npos &&
               "CheckRotation: rotation must only occur in the Service process");
    }
#endif

    result.triggered = true;

    // RAII mutex guard: acquires on construction, releases on destruction (all paths).
    // WAIT_ABANDONED: prior Service instance died mid-rotation; acquire and proceed.
    RotationMutexGuard mutexGuard(hRotationMutex_);
    if (hRotationMutex_ && !mutexGuard.Acquired()) {
        result.mutexFailed = true;
        result.error       = mutexGuard.Error();
        return result;
    }

    // === Mutex held (released by mutexGuard destructor) ===

    // Flush with error check — abort rotation if flush fails (rename must not proceed
    // on unflushed data). GetLastError captured immediately after failure.
    // CloseHandle + INVALID on failure: prevents infinite retry spin on the next write
    // (WriteMessage reopens via OPEN_ALWAYS and rotation retries on subsequent writes).
    if (!FlushFileBuffers(fileHandle_)) {
        DWORD err = GetLastError();
        result.error = err;
        CloseHandle(fileHandle_);
        fileHandle_ = INVALID_HANDLE_VALUE;
        return result;  // mutexGuard destructor releases mutex
    }
    CloseHandle(fileHandle_);
    fileHandle_ = INVALID_HANDLE_VALUE;

    // ===== §8.64 Final Stable Rename Block =====

    size_t pos = logPath_.rfind(L'\\');
    if (pos == std::wstring::npos) {
        result.success = false;
        result.error   = ERROR_INVALID_NAME;
        return result;
    }

    const std::wstring dirPath = logPath_.substr(0, pos);

    ScopedHandle hDir = MakeScopedHandle(CreateFileW(
        dirPath.c_str(),
        FILE_TRAVERSE | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        nullptr));

    if (!hDir) {
        result.success = false;
        result.error   = GetLastError();
        return result;
    }

    ScopedHandle hRename = MakeScopedHandle(CreateFileW(
        logPath_.c_str(),
        DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));

    if (!hRename) {
        result.success = false;
        result.error   = GetLastError();
        return result;
    }

    // filename 抽出
    size_t bpos = backupPath_.rfind(L'\\');
    if (bpos == std::wstring::npos || bpos + 1 >= backupPath_.size()) {
        result.success = false;
        result.error   = ERROR_INVALID_NAME;
        return result;
    }

    const std::wstring fileNameOnly = backupPath_.substr(bpos + 1);

    // 最低限バリデーション（軽量）
    if (fileNameOnly.empty() ||
        fileNameOnly == L"." ||
        fileNameOnly == L".." ||
        fileNameOnly.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) {
        result.success = false;
        result.error   = ERROR_INVALID_NAME;
        return result;
    }

    // オーバーフロー防止（必須）
    if (fileNameOnly.size() > (MAXDWORD / sizeof(wchar_t))) {
        result.success = false;
        result.error   = ERROR_FILENAME_EXCED_RANGE;
        return result;
    }

    const DWORD nameChars = static_cast<DWORD>(fileNameOnly.size());
    const DWORD nameBytes = nameChars * sizeof(wchar_t);

    const DWORD bufSize =
        FIELD_OFFSET(FILE_RENAME_INFO, FileName) + nameBytes + sizeof(wchar_t);

    std::vector<BYTE> buf;
    try {
        buf.assign(bufSize, 0);
    } catch (...) {
        result.success = false;
        result.error   = ERROR_OUTOFMEMORY;
        return result;
    }

    FILE_RENAME_INFO* ri = reinterpret_cast<FILE_RENAME_INFO*>(buf.data());

    ri->RootDirectory  = hDir.get();
    ri->FileNameLength = nameBytes;
    wmemcpy(ri->FileName, fileNameOnly.c_str(), nameChars);
    ri->FileName[nameChars] = L'\0';

    // ---- Step1: 安定ルート ----
    ri->ReplaceIfExists = TRUE;

    BOOL ok = SetFileInformationByHandle(
        hRename.get(),
        FileRenameInfo,
        ri,
        bufSize);

    // ---- Step2: フォールバック（絶対パス + POSIX_SEMANTICS）----
    // FileRenameInfoEx + RootDirectory の組み合わせは Windows カーネルの
    // 実装によって err=87 を返すケースがある。Step2 では RootDirectory=NULL +
    // 絶対パスを使用してこのカーネルバグを回避する。
#if defined(FILE_RENAME_FLAG_POSIX_SEMANTICS)
    if (!ok) {
        DWORD err = GetLastError();

        if (err == ERROR_SHARING_VIOLATION ||
            err == ERROR_ACCESS_DENIED      ||
            err == ERROR_INVALID_PARAMETER) {

            const std::wstring& full = backupPath_;

            if (full.size() >= 32767 ||
                full.size() > (MAXDWORD / sizeof(wchar_t))) {
                result.success = false;
                result.error   = ERROR_FILENAME_EXCED_RANGE;
                return result;
            }

            const DWORD fullChars = static_cast<DWORD>(full.size());
            const DWORD fullBytes = fullChars * sizeof(wchar_t);

            const DWORD bufSize2 =
                FIELD_OFFSET(FILE_RENAME_INFO, FileName) +
                fullBytes + sizeof(wchar_t);

            std::vector<BYTE> buf2;
            try {
                buf2.assign(bufSize2, 0);
            } catch (...) {
                result.success = false;
                result.error   = ERROR_OUTOFMEMORY;
                return result;
            }

            FILE_RENAME_INFO* ri2 =
                reinterpret_cast<FILE_RENAME_INFO*>(buf2.data());

            ri2->RootDirectory  = NULL;  // 絶対パス使用時は NULL
            ri2->FileNameLength = fullBytes;
            wmemcpy(ri2->FileName, full.c_str(), fullChars);
            ri2->FileName[fullChars] = L'\0';

            ri2->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS
                       | FILE_RENAME_FLAG_POSIX_SEMANTICS;

            ok = SetFileInformationByHandle(
                hRename.get(),
                FileRenameInfoEx,
                ri2,
                bufSize2);
        }
    }
#endif

    // ---- 結果処理 ----
    if (!ok) {
        result.success = false;
        result.error   = GetLastError();

        // ★ 重要：ログ停止防止
        fileHandle_ = CreateFileW(
            logPath_.c_str(),
            FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);

        if (fileHandle_ != INVALID_HANDLE_VALUE)
            SetFilePointer(fileHandle_, 0, nullptr, FILE_END);

        return result;
    }

    // Signal Manager processes that rotation just completed so they can detect the stale
    // handle before their next write (rather than waiting up to 100 writes).
    if (hRotationEvent_) {
        SetEvent(hRotationEvent_);
    }

    result.success = true;
    result.error   = 0;
    return result;
}

void LightweightLogger::SafeInternalLog(const std::wstring& msg) {
    // STRICTLY for rotation internal diagnostics. DO NOT use for general logging.
    // This function MUST remain recursion-free and must never call Log() or CheckRotation().
    //
    // Acquires cs_ internally (CriticalSection is re-entrant for the same thread,
    // so calling from WriteMessage() — which already holds cs_ — is safe on Windows).
    // Writes directly to fileHandle_ via WriteFile only.
    // If fileHandle_ is INVALID, opens OPEN_ALWAYS temporarily, writes, then closes.
    // No FlushFileBuffers — consistent with the normal-write no-flush policy.

    CSLockGuard lock(cs_);

    HANDLE local = fileHandle_;
    bool tempOpened = false;

    if (local == INVALID_HANDLE_VALUE) {
        local = CreateFileW(
            logPath_.c_str(),
            FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (local != INVALID_HANDLE_VALUE) {
            SetFilePointer(local, 0, nullptr, FILE_END);
            tempOpened = true;
        }
    }
    if (local == INVALID_HANDLE_VALUE) return;

    std::string utf8 = WideToUtf8(msg);
    utf8 += "\r\n";
    DWORD written = 0;
    if (!WriteFile(local, utf8.c_str(), static_cast<DWORD>(utf8.size()), &written, nullptr)) {
        // FAIL-SAFE: rotation diagnostic loss is acceptable; never crash or recurse.
        (void)GetLastError();  // clear error state; no further action taken
    }

    if (tempOpened) {
        CloseHandle(local);
        // fileHandle_ is intentionally NOT updated — remains INVALID_HANDLE_VALUE.
        // WriteMessage() will reopen it on the next call via OPEN_ALWAYS.
    }
}

void LightweightLogger::CheckStaleHandle() {
    // Called from WriteMessage() under cs_ lock, every 100 writes, for Manager only.
    // Uses NTFS file identity (volume serial + file index) to detect whether our open
    // handle has been silently renamed to UnLeaf.log.1 by the Service rotation.
    // If stale, reopen to the current UnLeaf.log so Manager converges to the fresh file.

    if (fileHandle_ == INVALID_HANDLE_VALUE) return;

    BY_HANDLE_FILE_INFORMATION currentInfo = {};
    if (!GetFileInformationByHandle(fileHandle_, &currentInfo)) return;

    // Open logPath_ for identity query only (no data access required)
    HANDLE queryHandle = CreateFileW(
        logPath_.c_str(),
        0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (queryHandle == INVALID_HANDLE_VALUE) return;  // new log not yet visible

    BY_HANDLE_FILE_INFORMATION logPathInfo = {};
    const bool gotInfo = (GetFileInformationByHandle(queryHandle, &logPathInfo) != 0);
    CloseHandle(queryHandle);

    if (!gotInfo) return;

    const bool sameFile =
        (currentInfo.dwVolumeSerialNumber == logPathInfo.dwVolumeSerialNumber &&
         currentInfo.nFileIndexHigh       == logPathInfo.nFileIndexHigh       &&
         currentInfo.nFileIndexLow        == logPathInfo.nFileIndexLow);

    if (!sameFile) {
        // Handle is stale (points to renamed UnLeaf.log.1); reopen to current UnLeaf.log
        CloseHandle(fileHandle_);
        fileHandle_ = CreateFileW(
            logPath_.c_str(),
            FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );
        if (fileHandle_ != INVALID_HANDLE_VALUE) {
            SetFilePointer(fileHandle_, 0, nullptr, FILE_END);
        }
    }
}

void LightweightLogger::SetRotationEnabled(bool enabled) {
    CSLockGuard lock(cs_);
    rotationEnabled_ = enabled;
}

std::wstring LightweightLogger::GetTimestamp() const {
    auto now = std::chrono::system_clock::now();
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                   now.time_since_epoch()) % 1000;
    auto time = std::chrono::system_clock::to_time_t(now);

    std::tm tm_buf;
    localtime_s(&tm_buf, &time);

    wchar_t buffer[32];
    wcsftime(buffer, 32, L"%Y-%m-%d %H:%M:%S", &tm_buf);

    wchar_t full[40];
    swprintf_s(full, L"%s.%03d", buffer, static_cast<int>(ms.count()));
    return std::wstring(full);
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

void LightweightLogger::SetEnabled(bool enabled) {
    enabled_.store(enabled, std::memory_order_release);
}

bool LightweightLogger::IsEnabled() const {
    return enabled_.load(std::memory_order_acquire);
}

} // namespace unleaf
