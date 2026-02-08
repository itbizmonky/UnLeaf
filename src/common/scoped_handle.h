#pragma once
// UnLeaf v1.00 - RAII Handle Management
// Zero-overhead abstraction for Windows HANDLEs

// Prevent Windows macro conflicts
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <winsvc.h>
#include <memory>
#include <type_traits>

namespace unleaf {

// Custom deleter for Windows HANDLEs
struct HandleDeleter {
    using pointer = HANDLE;
    void operator()(HANDLE h) const noexcept {
        if (h && h != INVALID_HANDLE_VALUE) {
            ::CloseHandle(h);
        }
    }
};

// RAII wrapper for HANDLE
using ScopedHandle = std::unique_ptr<void, HandleDeleter>;

// Factory function for creating ScopedHandle
inline ScopedHandle MakeScopedHandle(HANDLE h) noexcept {
    return ScopedHandle((h && h != INVALID_HANDLE_VALUE) ? h : nullptr);
}

// Custom deleter for Toolhelp32 snapshot handles
struct SnapshotDeleter {
    using pointer = HANDLE;
    void operator()(HANDLE h) const noexcept {
        if (h && h != INVALID_HANDLE_VALUE) {
            ::CloseHandle(h);
        }
    }
};

using ScopedSnapshot = std::unique_ptr<void, SnapshotDeleter>;

inline ScopedSnapshot MakeScopedSnapshot(HANDLE h) noexcept {
    return ScopedSnapshot((h && h != INVALID_HANDLE_VALUE) ? h : nullptr);
}

// Custom deleter for Service Control Manager handles
struct SCMHandleDeleter {
    using pointer = SC_HANDLE;
    void operator()(SC_HANDLE h) const noexcept {
        if (h) {
            ::CloseServiceHandle(h);
        }
    }
};

using ScopedSCMHandle = std::unique_ptr<SC_HANDLE__, SCMHandleDeleter>;

inline ScopedSCMHandle MakeScopedSCMHandle(SC_HANDLE h) noexcept {
    return ScopedSCMHandle(h);
}

// RAII wrapper for RegisterWaitForSingleObject
class WaitHandle {
public:
    WaitHandle() noexcept : handle_(nullptr) {}

    explicit WaitHandle(HANDLE h) noexcept : handle_(h) {}

    ~WaitHandle() noexcept {
        reset();
    }

    // Move-only
    WaitHandle(WaitHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }

    WaitHandle& operator=(WaitHandle&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    WaitHandle(const WaitHandle&) = delete;
    WaitHandle& operator=(const WaitHandle&) = delete;

    void reset() noexcept {
        if (handle_) {
            ::UnregisterWait(handle_);
            handle_ = nullptr;
        }
    }

    HANDLE get() const noexcept { return handle_; }
    bool valid() const noexcept { return handle_ != nullptr; }

    HANDLE release() noexcept {
        HANDLE h = handle_;
        handle_ = nullptr;
        return h;
    }

private:
    HANDLE handle_;
};

// RAII wrapper for critical sections
class CriticalSection {
public:
    CriticalSection() noexcept {
        ::InitializeCriticalSection(&cs_);
    }

    ~CriticalSection() noexcept {
        ::DeleteCriticalSection(&cs_);
    }

    CriticalSection(const CriticalSection&) = delete;
    CriticalSection& operator=(const CriticalSection&) = delete;

    void lock() noexcept {
        ::EnterCriticalSection(&cs_);
    }

    void unlock() noexcept {
        ::LeaveCriticalSection(&cs_);
    }

    bool try_lock() noexcept {
        return ::TryEnterCriticalSection(&cs_) != 0;
    }

private:
    CRITICAL_SECTION cs_;
};

// RAII lock guard for CriticalSection
class CSLockGuard {
public:
    explicit CSLockGuard(CriticalSection& cs) noexcept : cs_(cs) {
        cs_.lock();
    }

    ~CSLockGuard() noexcept {
        cs_.unlock();
    }

    CSLockGuard(const CSLockGuard&) = delete;
    CSLockGuard& operator=(const CSLockGuard&) = delete;

private:
    CriticalSection& cs_;
};

} // namespace unleaf
