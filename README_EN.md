[English](README_EN.md) | [日本語](README.md)

[![Build](https://github.com/itbizmonky/UnLeaf/actions/workflows/build.yml/badge.svg)](https://github.com/itbizmonky/UnLeaf/actions/workflows/build.yml)

# 🍃 UnLeaf - The Zero-Overhead EcoQoS Optimizer

**UnLeaf** is the ultimate background optimization tool for Windows 11 / 10 that completely disables **EcoQoS (Efficiency Mode)** and **Power Throttling** for your specified applications at the deepest OS level.

Originally created by kbn.

---

## 🔥 Why UnLeaf? (The Decisive Technical Advantage)

Traditional tools in the PC tuning space (like Process Lasso) rely on an outdated **"polling"** design — they scan the entire system every few seconds, constantly burning CPU cycles and wasting 15–20 MB of memory. The irony: a tool meant to boost your game's framerate is actually slowing your system down.

**UnLeaf is fundamentally different.**
It uses a fully **event-driven architecture** integrated directly with the Windows Kernel via **ETW (Event Tracing for Windows)**.

* **0.00% CPU while idle:** The engine wakes up for just a few milliseconds when a process is created or destroyed, then returns to a complete sleep (0% CPU).
* **Extreme memory efficiency:** Built directly on modern C++ and Win32 APIs, it consumes only **~3MB–5MB** of memory with zero memory leaks — verified through prolonged stress testing.
* **Millisecond assassin:** Detects child process spawns at the OS level the instant they occur, and strips EcoQoS with no lag.

---

## Design Philosophy: "Set and Forget"

- **Fully automatic after installation.** No manual intervention required.
- **Event-driven**: Process launches and thread creation are detected via ETW (Event Tracing for Windows) and handled immediately. Zero CPU is used when no events occur.
- **3-phase adaptive control**: Automatically switches between AGGRESSIVE → STABLE → PERSISTENT based on process behavior, maintaining maximum effect with minimal resources.
- **Safety first**: System-critical processes (csrss.exe, lsass.exe, svchost.exe, etc.) are protected by an allow-list and will never be targeted.

---

## What It Does

- Disables EcoQoS (Efficiency Mode) for target processes
- Sets process priority to `HIGH_PRIORITY_CLASS`
- Disables Power Throttling for threads
- Suppresses EcoQoS application via registry policy (Image File Execution Options)

## What It Does NOT Do

- Overclock or modify hardware settings
- Cheat or modify games
- Make network connections (fully offline operation)
- Collect or transmit personal data
- Interfere with any process outside the target list

---

## System Requirements

| Item | Requirement |
|------|-------------|
| OS | Windows 10 (1709 or later) / Windows 11 |
| Privileges | Administrator |
| Memory | ~2 MB |
| Disk | ~1.2 MB |

> **Note**: On Windows 11, `NtSetInformationProcess` (NT API) is used preferentially. On Windows 10, equivalent control is achieved via `SetProcessInformation` (Win32 API). The OS version is detected automatically at startup.

---

## Installation (Binary ZIP)

No source build required. Get started immediately with the intuitive UI.

1. Go to the Releases page.
2. Download the latest `UnLeaf_v1.x.x.zip` and extract it to any folder.
3. Run `UnLeaf_Manager.exe` (a UAC prompt will appear on first launch for service registration).
4. Add the apps you want to optimize (e.g., discord.exe) to the list and press "Start Service".
5. You can close the Manager — the invisible engine will continue protecting your PC indefinitely.

> **Note**: ⚠️ Security warning during installation<br>UnLeaf is a free, independently developed tool and does not have an expensive code signing certificate (digital signature). As a result, Windows Defender SmartScreen (the blue "Windows protected your PC" screen) may appear when downloading or running it for the first time.<br>This is a standard warning for unrecognized files. The safety of this software is guaranteed by the publicly available source code. If the warning appears, click "More info" and then "Run anyway" to proceed.

---

## Uninstallation

### Recommended: Uninstall via Manager UI (Complete)

1. Launch `UnLeaf_Manager.exe` as Administrator
2. Click "Unregister Service"
3. Delete the UnLeaf folder

> Unregistering via the Manager UI guarantees a complete registry cleanup.
> `RemoveAllPolicies` is designed to be idempotent — it completes safely even if the manifest is missing, registry keys are absent, or internal state is empty.

---

## Registry Changes

UnLeaf writes to **only 2 registry locations**. No other registry keys are touched.

| # | Path | Value | Purpose |
|---|------|-------|---------|
| 1 | `HKLM\SYSTEM\CurrentControlSet\Control\Power\PowerThrottling\<FullExePath>` | DWORD `1` | Persistent EcoQoS exclusion |
| 2 | `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\<ExeName>\PerfOptions\CpuPriorityClass` | DWORD `3` | CPU priority High |

> **Guarantee**: All entries are automatically deleted when the service is stopped or unregistered.

### Registry Lifecycle

| Action | Registry | Description |
|--------|----------|-------------|
| Add / Remove / Disable process | No change | Manager UI operations do not modify the registry |
| Stop service | **Fully deleted** | All entries applied in that session are deleted |
| Unregister service | **Fully deleted** | All past entries are deleted via the manifest file (safe even after a crash) |

---

## Typical Use Cases

| Application | Benefit |
|-------------|---------|
| Chrome / Edge | Prevents throttling of background tabs |
| Games (all) | Prevents performance degradation after Alt-Tab |
| DAW (Cubase, FL Studio, etc.) | Prevents CPU throttle during rendering |
| Video editing (DaVinci Resolve, etc.) | Prevents slowdowns during encoding |

---

## FAQ

### What is EcoQoS?
A power-saving API introduced in Windows 10 (1709) as Power Throttling. The OS reduces a process's CPU frequency and scheduling priority to lower power consumption. In Windows 11, it appears visually in Task Manager as "Efficiency Mode" (leaf icon).

### Will CPU usage increase?
**No.** UnLeaf is event-driven and sleeps completely via `WaitForMultipleObjects(INFINITE)` when there are no events to handle. Idle CPU usage is 0%.

### Are there any security risks?
The following security measures are implemented:
- **DACL**: IPC communication is accessible only to SYSTEM + Administrators
- **Input validation**: Path traversal checks and length limits on process names
- **Protection list**: System-critical processes (csrss.exe, lsass.exe, svchost.exe, etc.) are excluded from targeting
- **Privilege separation**: Each command has a privilege level of PUBLIC / ADMIN / SYSTEM_ONLY

### Does it work on Windows 10?
**Yes.** On Windows 10 (1709 or later), Power Throttling is controlled via `SetProcessInformation`. On Windows 11, `NtSetInformationProcess` is preferred with `SetProcessInformation` as a fallback. The OS version is detected automatically at startup — no user configuration required.

### Where is the configuration file?
`UnLeaf.ini` is generated in the same folder as `UnLeaf_Service.exe`. It can be edited directly in any text editor; changes are reloaded automatically on save (no service restart required).

```ini
; UnLeaf Configuration
; Auto-generated - Do not edit while service is running

[Logging]
; Log level: ERROR, ALERT, INFO, DEBUG
LogLevel=INFO
; Log output: 1=enabled, 0=disabled
LogEnabled=1

[Manager]
; Window position and size (auto-saved by Manager UI)
WindowX=100
WindowY=100
WindowWidth=800
WindowHeight=600
Maximized=0

[Targets]
chrome.exe=1
```

### How do I check if the service is running?
Launch `UnLeaf_Manager.exe` and check the service status from the UI. You can also run `sc query UnLeafService` from the command line.

---

## Technical Architecture

### Event-Driven Control Loop (WFMO)

The engine control thread monitors 5 handles via `WaitForMultipleObjects(INFINITE)` and consumes zero CPU when idle.

| Index | Handle | Trigger | Action |
|-------|--------|---------|--------|
| 0 | `stopEvent_` | Service stop request | Exit loop |
| 1 | `configChangeHandle_` | `FindFirstChangeNotification` (INI change) | Reload config + rebuild targets after debounce |
| 2 | `safetyNetTimer_` | Waitable Timer (10-second interval) | Re-check EcoQoS for STABLE-phase processes only |
| 3 | `enforcementRequestEvent_` | `EnqueueRequest()` from ETW callback / timer | Swap queue → sequential processing via `DispatchEnforcementRequest()` |
| 4 | `hWakeupEvent_` | Wakeup from `OnProcessExit` callback | Exclusive removal on the control thread via `ProcessPendingRemovals()` |

### No Direct Deletion from Callbacks

Timer callbacks (`DeferredVerifyTimerCallback`, `PersistentEnforceTimerCallback`) only call `EnqueueRequest()` — no blocking operations or self-deletion of context.

- **Deferred context** (one-shot): The control loop `delete`s it after processing the request via `DispatchEnforcementRequest()`
- **Persistent context** (recurring): `Stop()` waits for all callbacks to complete via `DeleteTimerQueueEx(INVALID_HANDLE_VALUE)` before `delete`

### Pending Removal Queue

The process-exit callback `OnProcessExit` runs on the OS thread pool and cannot call `RemoveTrackedProcess()` directly.

1. `OnProcessExit` → push PID to `pendingRemovalPids_` + signal `hWakeupEvent_`
2. Engine control loop wakes on `WAIT_PROCESS_EXIT`
3. `ProcessPendingRemovals()` drains the queue exclusively using `CriticalSection` + `swap`, then calls `RemoveTrackedProcess()` on the control thread

### EcoQoS Policy: 5-Layer Defense

`PulseEnforceV6()` disables EcoQoS across 5 layers:

| Layer | API | Description |
|-------|-----|-------------|
| 1 | `SetPriorityClass(PROCESS_MODE_BACKGROUND_END)` | Exit Background Mode |
| 2 | `NtSetInformationProcess` (Win11) | Low-level NT API — Power Throttling OFF |
| 3 | `SetProcessInformation` (Win10 / fallback) | Win32 API — Power Throttling OFF |
| 4 | `SetPriorityClass(HIGH_PRIORITY_CLASS)` | Set priority to HIGH to prevent OS from auto-applying EcoQoS |
| 5 | `DisableThreadThrottling` (INTENSIVE phase only) | Disable Power Throttling per-thread |

Additionally, registry policy (`PowerThrottling` + `Image File Execution Options`) ensures EcoQoS exclusion persists across OS reboots.

### 3-Phase Adaptive Control

| Phase | Behavior | Transition |
|-------|----------|------------|
| **AGGRESSIVE** (0–3 s) | Immediate enforce + 3 deferred verifications (200ms / 1s / 3s) | All 3 clean → STABLE; 3+ violations → PERSISTENT |
| **STABLE** | Event-driven only (zero CPU) | Violation detected via ETW thread event or Safety Net → AGGRESSIVE (< 3 violations) or PERSISTENT (≥ 3) |
| **PERSISTENT** | 5-second interval enforce + immediate ETW boost response | 60 seconds clean → STABLE |

### Safety Guarantees

- **Timer callback = enqueue only**: Blocking operations, `delete`, and `RemoveTrackedProcess` are prohibited inside callbacks
- **RemoveTrackedProcess = control thread only**: OS thread pool access is exclusively via `pendingRemovalPids_`
- **Stop() = 9-step barrier order**: ETW stop → thread join → Timer Queue teardown → Wait handle release → Job Object release → registry cleanup
- **UAF prevention**: `shared_ptr<TrackedProcess>` reference counting prevents early destruction of process context while a callback is executing

---

## Thread Model

UnLeaf operates with a fixed number of threads. Thread count does not grow over time.

| Component | Thread | Description |
|-----------|--------|-------------|
| **Service** | EngineControlThread (1) | Handles all events via WaitForMultipleObjects |
| | ETW Consumer Thread (1) | OS event tracing session |
| | IPC Server Thread (1) | Named Pipe connection acceptor |
| **Manager** | UI Thread (1) | Win32 message loop |
| | Log Watcher Thread (1) | Log file diff monitor |

Timer Queue callbacks execute on the OS thread pool but are not threads created by UnLeaf.

---

## 24/7 Resident Design

UnLeaf is designed as a 24/7/365 resident service following these principles:

- **Idle is King**: No CPU usage when there are no events (`WaitForMultipleObjects(INFINITE)`)
- **Fixed resources**: Thread count, handle count, and memory usage are constant after startup — repeated process additions and removals do not increase resource consumption
- **RAII + explicit release**: All timer contexts and wait callback contexts have managed lifetimes and are reliably released on process exit or timer recreation
- **Blocking release**: `UnregisterWaitEx(INVALID_HANDLE_VALUE)` and `DeleteTimerQueueTimer(INVALID_HANDLE_VALUE)` guarantee callback completion before context is released
- **IPC timeout**: Client `ReadFile` is implemented with Overlapped I/O + 5-second timeout — a frozen client cannot block the server thread

---

## License

The source code published in this repository (core engine) is released under the MIT License.

---

## Developer Information

### Build Instructions

**Prerequisites**: Visual Studio 2022 or later (MSVC), CMake 3.20+

```cmd
# Configure
cmake -B build -G "Visual Studio 17 2022" -A x64

# Release build
git clone https://github.com/itbizmonky/UnLeaf.git
cd UnLeaf
mkdir build
cd build
cmake ..
cmake --build . --config Release
```

The built `UnLeaf_Service.exe` can be registered and run as a standalone Windows service. Refer to the specification document for detailed specs and the IPC communication interface.

### Source Code Structure

```
UnLeaf/
├── .github/
│   └── workflows/
│       └── build.yml            # GitHub Actions CI
├── CHANGELOG.md                 # Changelog
├── CMakeLists.txt               # OSS dynamic build script
├── LICENSE                      # MIT License
├── README.md / README_EN.md
├── docs/
│   └── Engine_Specification.md  # Detailed engine technical specification
├── resources/
│   └── service.rc               # Service resource file
├── src/
│   ├── common/                  # Shared utilities (logger, config, registry management)
│   ├── engine/                  # Engine decision logic (Win32-independent, pure C++)
│   │   ├── engine_logic.*       # Phase transitions & EcoQoS enforcement (5 functions)
│   │   └── engine_policy.h      # Timing constants (EnginePolicy struct)
│   ├── service/                 # Core engine (ETW monitoring, service control)
│   └── manager/                 # Manager UI (closed-source, not built by OSS CMake)
└── tests/                       # Unit tests (104 cases / all PASS)
```

---

## Changelog

### v1.0.2 (2026-03-16)

**ETW Reliability**
- Explicit ETW buffer configuration: `BufferSize=64KB`, `MinimumBuffers=4`, `MaximumBuffers=32`, `FlushTimer=0`
- ETW zombie session auto-cleanup: stops any lingering session with the same name before `StartTraceW` (prevents `ERROR_ALREADY_EXISTS`)
- ETW lost event detection: detects `EVENT_TRACE_TYPE_LOST_EVENT` and records `LOG_ALERT` + cumulative count

**Manager Improvements**
- Window position and size persistence: saved to `UnLeaf.ini [Manager]` section with off-screen restore guard
- Live log color bug fix: replaced `GetWindowTextLengthW` with `EM_GETTEXTLENGTHEX(GTL_NUMCHARS)`
- **Live log overhaul**: RichEdit replaced with a virtual ListView + Owner-Draw renderer — eliminates auto-scroll stall, blank-line artifacts, and double-display issues at the root
- **Manager operation log persistence**: `LogEngine` / `LogQueue` infrastructure with `UICallback` registration routes Manager UI actions (toggle, errors) to the log file

### v1.0.1 (2026-03-09)

**Bug Fixes & Safety**
- Moved `DeleteTimerQueueTimer` before `trackedCs_` acquisition to eliminate all lock-held calls (5 sites)
- Fixed GDI object restore in `DrawButton` (explicitly restore old font via `SelectObject`)
- Added null check to `ToggleSubclassProc`
- Windows version display fix: corrected mis-display of `Windows 10.0` on Windows 11. Now correctly identifies Windows 11 via build threshold (Build >= 22000); future Windows 12 (major >= 11) displays major.minor as-is

**Architecture**
- Extracted engine decision logic to `src/engine/engine_logic` (Win32-independent pure C++ module)
- Introduced `EnginePolicy` struct to centralize timing constants, decoupled from `engine_core`
- Expanded unit tests: 72 → 104 cases (`test_engine_logic`: +32, `test_engine_policy`: +2)

**Resource Management**
- RAII-ified GDI+ initialization in `DrawToggleSwitch()` — `GdiplusShutdown()` is now guaranteed to be called on process exit

**CI/CD**
- Added GitHub Actions CI (automatic build + ctest on `push` / `pull_request`)
- Optimized CI cache path to `build/_deps` (FetchContent deps only, improves cache hit rate)

### v1.0.0 (2026-03-06)

Initial release.
