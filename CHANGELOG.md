# Changelog

All notable changes to UnLeaf will be documented in this file.

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
