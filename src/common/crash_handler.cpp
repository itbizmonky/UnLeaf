// UnLeaf - Unhandled exception crash dump writer

#include "crash_handler.h"
#include "logger.h"

#include <windows.h>
#include <dbghelp.h>
#include <cstdio>

#pragma comment(lib, "dbghelp.lib")

namespace unleaf {

namespace {

// Cached crash dump directory, resolved once during InstallCrashHandler().
// Stored as a fixed buffer (no std::wstring) so the exception filter can
// read it without any heap/CRT state that may be corrupted at crash time.
static wchar_t g_crashDir[MAX_PATH] = {0};
static LONG    g_installed = 0;

// Build the full dump file path. Must be async-signal-safe-ish: no heap,
// no CRT locale, no logging. Uses only Win32 file + time APIs.
//
// Filename format: UnLeaf_Service_YYYYMMDD_HHMMSS.sss.dmp
// (millisecond-precision timestamp; no PID — UnLeaf is a single-instance
// service so collisions within the same millisecond are not expected.)
static void BuildDumpFileName(wchar_t* out, size_t outCch) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    swprintf_s(out, outCch,
               L"%s\\UnLeaf_Service_%04u%02u%02u_%02u%02u%02u.%03u.dmp",
               g_crashDir,
               st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond,
               st.wMilliseconds);
}

// SetUnhandledExceptionFilter callback. Writes minidump then chains to
// the default handler so Windows Error Reporting still kicks in.
static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    if (g_crashDir[0] == L'\0') {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    wchar_t dumpPath[MAX_PATH];
    BuildDumpFileName(dumpPath, _countof(dumpPath));

    HANDLE hFile = CreateFileW(
        dumpPath,
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (hFile != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei = {};
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = ep;
        mei.ClientPointers = FALSE;

        // ThreadInfo: per-thread register context and TEB pointers for
        //   every thread (essential to diagnose cross-thread races).
        // IndirectlyReferencedMemory: small slices of heap pointed at by
        //   stack/register values — enough to recover string contents and
        //   small structs without blowing up the dump size.
        const MINIDUMP_TYPE kDumpType = static_cast<MINIDUMP_TYPE>(
            MiniDumpWithThreadInfo |
            MiniDumpWithIndirectlyReferencedMemory);

        MiniDumpWriteDump(
            GetCurrentProcess(),
            GetCurrentProcessId(),
            hFile,
            kDumpType,
            ep ? &mei : nullptr,
            nullptr,
            nullptr);

        CloseHandle(hFile);
    }

    // Chain to the default handler so WER / debugger still fire.
    return EXCEPTION_CONTINUE_SEARCH;
}

} // anonymous namespace

void InstallCrashHandler(const std::wstring& baseDir) {
    // Idempotent: only install once per process.
    if (InterlockedCompareExchange(&g_installed, 1, 0) != 0) {
        return;
    }

    // Build "<baseDir>\crash" into g_crashDir. If baseDir is too long or the
    // directory cannot be created, clear g_crashDir so CrashFilter becomes a
    // pass-through (chain to WER only).
    int written = swprintf_s(g_crashDir, _countof(g_crashDir),
                             L"%s\\crash", baseDir.c_str());
    if (written < 0) {
        g_crashDir[0] = L'\0';
        SetUnhandledExceptionFilter(&CrashFilter);
        return;
    }

    CreateDirectoryW(g_crashDir, nullptr);  // ignores ERROR_ALREADY_EXISTS
    DWORD attrs = GetFileAttributesW(g_crashDir);
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        g_crashDir[0] = L'\0';
    }

    // Capture the previous top-level filter so we can surface cases where
    // another component (CRT, third-party lib) had already installed one.
    // We intentionally do NOT chain to prev from CrashFilter: invoking an
    // unknown filter at crash time risks re-entering corrupted CRT state.
    // EXCEPTION_CONTINUE_SEARCH already delegates to WER / the debugger.
    LPTOP_LEVEL_EXCEPTION_FILTER prev =
        SetUnhandledExceptionFilter(&CrashFilter);
    if (prev != nullptr) {
        wchar_t msg[128];
        swprintf_s(msg, _countof(msg),
                   L"[CRASH] Previous unhandled exception filter was replaced (%p)",
                   reinterpret_cast<void*>(prev));
        LOG_DEBUG(std::wstring(msg));
    }
}

} // namespace unleaf
