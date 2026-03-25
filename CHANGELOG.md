# Changelog

All notable changes to UnLeaf will be documented in this file.

---

## [1.0.3] - 2026-03-25

### Fixed
- Log rotation 2nd-cycle crash: `CheckRotation()` called `Log()` internally, causing `Log() → WriteMessage() → CheckRotation() → Log()` infinite recursion → stack overflow when rotation failed. Resolved by making `CheckRotation()` a pure function returning `RotationResult`; all diagnostic output routed through `SafeInternalLog()` which writes directly via `WriteFile` without re-entering `CheckRotation()`
- Log rotation 2nd-cycle rename failure: `MoveFileExW(REPLACE_EXISTING)` fails when Manager holds an open handle to `UnLeaf.log.1` — Windows NTFS marks the destination for deletion but keeps the directory entry occupied until all handles close. Replaced with `SetFileInformationByHandle(FileRenameInfoEx, FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS)` for atomic directory-entry replacement regardless of open handles
- `FlushFileBuffers` failure now aborts rotation and closes `fileHandle_` — prevents infinite retry spin on the next write cycle
- `WriteFile` failure in `WriteMessage()` now disables further logging (`enabled_ = false`) to prevent I/O error loops
- Log sequence reversal after rotation: Manager's stale handle check ran **after** `WriteFile`, so the first write post-rotation landed in the already-renamed `UnLeaf.log.1`, making its timestamp newer than the first line of `UnLeaf.log`. Fixed by moving the stale check to **before** `WriteFile` in `WriteMessage()`; added `Global\UnLeafLogRotated` named event (auto-reset) so Service signals Manager immediately on successful rotation rather than waiting up to 100 writes for the counter fallback

### Added
- `RotationResult` struct: `CheckRotation()` returns `{ triggered, success, mutexFailed, error }` — no logging inside; caller decides action
- `SafeInternalLog()`: writes rotation meta-messages directly to `fileHandle_` without rotation check or callback; acquires `cs_` internally; falls back to `OPEN_ALWAYS` if handle is `INVALID`
- `RotationMutexGuard` RAII: acquires `Global\UnLeafLogRotation` on construction, releases on destruction — eliminates manual `ReleaseMutex` on all return paths
- `ScopedHandle` for rename handle: `hRename` is `ScopedHandle` via `MakeScopedHandle()` — handle leak impossible on any return path
- `GetLastError()` captured immediately after each API failure into a local variable — prevents value corruption by intervening calls
- On rename failure, `fileHandle_` restored immediately in `CheckRotation()` to avoid `OPEN_ALWAYS` churn on every subsequent write
- `hRotationEvent_` (`Global\UnLeafLogRotated`, auto-reset): Service sets on successful rotation; Manager polls with `WaitForSingleObject(..., 0)` before each write to detect stale handles immediately

---

## [1.0.2] - 2026-03-16

### Fixed
- ETW buffer configuration made explicit: `BufferSize=64KB`, `MinimumBuffers=4`, `MaximumBuffers=32`, `FlushTimer=0` (real-time consumer mode)
- ETW zombie session cleanup: stale sessions with the same name are stopped before `StartTraceW` to prevent `ERROR_ALREADY_EXISTS`
- `RefreshLogDisplay`: initial `endPos` now obtained via `EM_GETTEXTLENGTHEX(GTL_NUMCHARS)` instead of `GetWindowTextLengthW` — eliminates per-line position skew caused by `\r\n` expansion

### Added (2026-03-16)
- ETW lost event detection: `EVENT_TRACE_TYPE_LOST_EVENT` opcode handled in callback; LOG_ALERT emitted with cumulative count on buffer overflow
- Window position and size persistence: saved to `[Manager]` section of `UnLeaf.ini` on `WM_DESTROY`, restored on startup with off-screen guard (`MonitorFromPoint(MONITOR_DEFAULTTONULL)`)
- Virtual ListView live log display: RichEdit replaced with a custom virtual ListView + Owner-Draw renderer; eliminates auto-scroll stall and blank-line artifacts under high log volume
- `LogEngine` / `LogQueue`: new log management infrastructure in `src/manager/`; `LogQueue` accumulates log lines with thread-safe wake-up; `LogEngine` drives the log thread and dispatches lines to both the file logger and the UI via `UICallback` registration — resolves [LIVE-2] Manager operation log persistence

---

## [1.0.1] - 2026-03-09

### Fixed
- `DeleteTimerQueueTimer` moved before `trackedCs_` acquisition to eliminate lock-held invocation (all 5 sites)
- `DrawButton` GDI object restore fixed: old font now explicitly restored via `SelectObject`
- `ToggleSubclassProc` null check added
- Windows version log display corrected: Windows 11 now shows `Windows 11 (Build XXXXX)` instead of `Windows 10.0 (Build XXXXX)` — hybrid detection (build threshold `>= 22000` for major=10, major fallback for Windows 12+)

### Added
- Engine decision logic extracted to `src/engine/engine_logic` (pure C++, no Win32 dependency)
- `EnginePolicy` struct introduced to centralize timing constants, decoupled from `engine_core`
- Unit tests expanded: 72 → 104 cases (`test_engine_logic`: 32, `test_engine_policy`: 2 added)
- GitHub Actions CI: automatic build + ctest on push / pull_request
- CI cache path optimized to `build/_deps` (FetchContent dependencies only, improved hit rate)
- `DrawToggleSwitch()` GDI+ initialization replaced with `GdipGuard` RAII struct — ensures `GdiplusShutdown()` on process exit

---

## [1.0.0] - 2026-03-06

Initial release.

### Features
- Event-driven EcoQoS optimizer using ETW (Event Tracing for Windows)
- 3-phase adaptive control: AGGRESSIVE → STABLE → PERSISTENT
- 5-layer EcoQoS policy enforcement (NT API + Win32 API + Registry)
- Named Pipe IPC with DACL (SYSTEM + Administrators only)
- Win32 GUI Manager with dark theme (GDI+)
- 72 unit tests (GoogleTest)
- Zero CPU usage at idle (`WaitForMultipleObjects(INFINITE)`)
- Memory footprint: 3–5 MB
