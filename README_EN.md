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
* **Extreme memory efficiency:** Built directly on modern C++ and Win32 APIs, it consumes only **~15MB** of memory with zero memory leaks — verified through prolonged stress testing.
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
; Crash minidump writer: 1=enabled, 0=disabled (default)
CrashDump=0

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

### How do I collect crash diagnostics when the service crashes? (CrashDump feature)
UnLeaf is designed to be strictly portable, so crash dump writing is **disabled by default**. To opt in for diagnostics:

1. Stop the service, then add `CrashDump=1` under the `[Logging]` section of `UnLeaf.ini`
2. Restart the service

Once enabled, any unhandled exception inside `UnLeaf_Service.exe` will write a MiniDump to:

```
<install folder>\crash\UnLeaf_Service_YYYYMMDD_HHMMSS.sss.dmp
```

- **Dump type**: `MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory` — includes per-thread registers / TEB pointers plus small slices of heap referenced from stack/register values (enough to diagnose cross-thread races without bloating the dump)
- **Relationship with Windows Error Reporting (WER)**: UnLeaf's MiniDump writer is independent of WER and chains to it via `EXCEPTION_CONTINUE_SEARCH`, so both dumps are produced
- **Debug symbols**: Release builds are compiled with `/Zi /DEBUG /OPT:REF /OPT:ICF` and always emit `UnLeaf_Service.pdb`. Load the PDB together with the dmp in WinDbg or Visual Studio for fully symbolicated analysis
- **Why disabled by default**: The `crash\` subfolder is never created unless you explicitly set `CrashDump=1`. This preserves UnLeaf's portable philosophy of never producing files or folders the user did not ask for

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
│       ├── log_engine.h/cpp     # LogEngine / LogQueue (virtual ListView log infrastructure)
└── tests/                       # Unit tests (104 cases / all PASS)
```

---

## Changelog

### v1.1.5 (2026-05-04)

**ETW Stability Improvements (Windows 11 Build 26200)**
- Fixed `MatchAnyKeyword=0x30` bug: `Microsoft-Windows-Kernel-Process` provider emits events with `keyword=0`; any non-zero `MatchAnyKeyword` (including `0x30` = PROCESS|THREAD) silently blocked all callbacks, causing persistent `DEGRADED_ETW` mode. Changed to `MatchAnyKeyword=0` to restore ETW delivery
- Expanded ETW buffer constants: `ETW_BUFFER_SIZE_KB` 64→128 KB, `ETW_MIN_BUFFERS` 4→8, `ETW_MAX_BUFFERS` 32→64 (accommodates ~1,200 events/sec with `MatchAnyKeyword=0`)
- Added ConsumerThread diagnostic logging: `OpenTraceW` handle value, `ProcessTrace` enter/exit with elapsed ms, first-callback receipt confirmation, first event keyword/eventId — aids post-hoc ETW failure analysis
- `ResolveProcessPath`: suppressed `error=31` (system processes) and `error=87` (already-exited processes) from `QueryFullProcessImageNameW` debug output
- Log level normalization: `OpenTraceW handle=` / `ProcessTrace enter/exit` downgraded ALERT→INFO; `Lost event detected` downgraded ALERT→DEBUG (confirmed structural noise at ~1/sec on this platform)
- Lost event DEBUG log rate-limited: `lastLostLogTime_` (atomic ULONGLONG) 60-second debounce — at most 1 log/minute regardless of event volume; suppresses DEBUG flood from structural noise while maintaining observability
- `etwLost=N` field added to `[DIAG]` log: `lastDiagLostCount_` snapshot tracks lost event count delta per DIAG interval (not cumulative total); aids burst/spike detection

### v1.1.4 (2026-05-01)

**ETW Callback Non-Blocking — Crash Root Fix**
- Fixed ACCESS VIOLATION crash (INVALID_PROCESSTRACE_HANDLE) caused by blocking OS calls (`AssignProcessToJobObject` etc.) executing on the ETW consumer thread inside `OnProcessStart`, which could block `consumerThread_.join()` for up to 9 minutes and freeze `EngineControlLoop`
- `OnProcessStart` now only calls `EnqueueRequest(ETW_PROCESS_START)` and returns immediately; all heavy work executes on `EngineControlLoop`
- `EnforcementRequest` struct extended with `parentPid` / `imageName` / `imagePath` fields

### v1.1.3 (2026-04-28)

**ETW Monitoring Extended — EcoQoS Guarantee Regardless of ETW State (§9.18 expanded)**
- `ScanRunningProcessesForMissedTargets`: force-reset `lastScannedPid_=0` on full-scan completion. Prevents the round-robin from pinning at high-PID range and permanently missing early-launched chrome.exe
- `PERIODIC_FULL_SCAN_INTERVAL` 60 s → **20 s**: Root-requirement fix — new Chrome processes are detected within ≤ 20 s regardless of ETW state, including during cold-dead window
- `DEGRADED_SCAN_INTERVAL` 30 s → **20 s**: Reduces detection delay in `DEGRADED_ETW` mode
- **ETW cold-dead detection added**: if eventCount remains 0 for 240 s after service start or last restart (and targets are running), `ProcessMonitor` is force-restarted. Compensates for `IsHealthy()` blind spot where `ControlTrace(QUERY)` succeeds but no events are delivered
- **`EtwState` 3-state machine** (`HEALTHY` / `VERIFYING_RECOVERY` / `DEGRADED`): after restart, confirms eventCount increase within 30 s. Up to 2 retries before `DEGRADED_ETW` transition
- Extended `[DIAG]` diagnostic log with `etwEvents=N` field for post-hoc ETW delivery health verification

### v1.1.2 (2026-04-11)

**Memory Leak Fix & Heap Fragmentation Reduction**
- Fixed `jobObjects_` handle accumulation: `jobObjects_.erase(pid)` added at the end of `RemoveTrackedProcess()` under `jobCs_`. Job Object handles are now released on every process exit, stopping the ~9 MB / 41 hours Private Bytes growth
- `ParseProcessStartEvent`: replaced per-call `std::vector<BYTE>` allocations with `thread_local` reusable buffers. Eliminates heap alloc/free on every ETW event
- `ProcessMonitor` log sites (4 locations): replaced `std::wstringstream` with `std::to_wstring()` to reduce heap fragmentation
- `engineControlThreadId_` changed to `std::atomic<DWORD>` for cross-thread visibility. DEBUG asserts in `RemoveTrackedProcess`, `RefreshJobObjectPids`, `ProcessPendingRemovals` use `relaxed` load for deterministic misuse detection
- Added `HeapSetInformation(GetProcessHeap(), HeapOptimizeResources, ...)` to `EngineCore::Start()`. Instructs the process heap to decommit idle pages aggressively (Windows 8.1+, best-effort). 6.5-hour test confirmed ~5x improvement in memory growth rate (3.78→6.26 MB / 6.5h vs 3→15.5 MB)

### v1.1.1 (2026-04-09)

**ProcessMonitor Hardening (§9.15)**
- Redesigned `IsHealthy()` with 3-stage evaluation: warmup grace period (120s) + lost event delta threshold + `ControlTraceW(QUERY)` session liveness check (1s cached). Correctly distinguishes idle (no events, session alive) from ETW session death
- Changed `instance_` to `std::atomic<ProcessMonitor*>`. `EventRecordCallback` snapshots into local `self` to eliminate TOCTOU. acquire/release pairing
- Protected `Stop()` with `stopMtx_`, enforcing ETW shutdown contract (5-step order: `stopRequested_` → `CloseTrace` → `ControlTrace(STOP)` → `join` → `instance_ clear`). Eliminates race with IPC thread's `IsHealthy()`
- Replaced string property reads in `ParseProcessStartEvent` with bounded copies capped by `propSize`. Prevents out-of-bounds reads from unterminated TDH payloads (AV risk). Unknown `InType` is now skipped
- Explicit state reset in `Start()` (`eventCount_`, `lostEventCount_`, `lastEventTime_`, `startTime_`, `lastCheckedLost_`, `cachedTraceAlive_`) to prevent stale state contamination after service restart

**CrashDump opt-in feature (Phase 2)**
- Added `src/common/crash_handler.{h,cpp}`: `SetUnhandledExceptionFilter` + `MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory` writer. **Disabled by default**; opt-in via `[Logging] CrashDump=1`
- Output: `<install dir>\crash\UnLeaf_Service_YYYYMMDD_HHMMSS.sss.dmp` (portable philosophy — no `%ProgramData%` writes)
- Chains to WER / debugger via `EXCEPTION_CONTINUE_SEARCH`; both UnLeaf MiniDump and WER are captured
- Release builds compile with `/Zi /DEBUG /OPT:REF /OPT:ICF`, always emitting PDB for full symbolication

### v1.1.0 (2026-03-30)

**Memory Stability & Long-Run Hardening**
- `trackedProcesses_` hard cap (MAX=2,000): eviction candidates selected zombie-first → oldest phaseStartTime, delegated to `RemoveTrackedProcess()` — zero Timer/WaitContext leaks
- Burst eviction: `SelectEvictionCandidates()` selects all excess PIDs in one pass with `partial_sort` O(N log K) + PMR arena (16 KB stack buffer) to minimize heap allocation
- `pendingRemovalPids_` overflow no longer drops (`pop()` removed) — always push, emit `LOG_ALERT` only. Guarantees no eviction work is lost
- `ProcessPendingRemovals()` capped at 256 items/tick — eliminates processing spikes; remaining items auto-rescheduled via `SetEvent`; runaway backlog (> 8,192) detected with `LOG_ALERT`
- `Logger::WriteMessage()` migrated to stack buffer (2,048 bytes) — zero heap allocation on normal path

**RegistryPolicyManager v5 + CPU Spin Fix**
- Full redesign of RegistryPolicyManager: IFEO (exe-name-based) and PowerThrottle (path-based) managed independently. Multiple paths for the same exe name (Chrome + Canary) tracked separately
- Fixed CPU 96.9% spin: `SetEvent` responsibility separation (wasEmpty / hasRemaining pattern) + spin detection safety net
- Proactive policy generation: policies applied to all config entries at service startup. IFEO is effective even before the process launches
- Replaced `ResolvePathByHandle` with `CanonicalizePath` (GetFullPathNameW-based) — long path support, no file existence required
- ETW fallback: if proactive application failed, policy is recovered when the process is detected via ETW

**PowerThrottle Deferred-Apply Bug Fix**
- Fixed PowerThrottle policy not applied to name-only targets (e.g., `chrome.exe=1`) started after service registration: `ApplyOptimizationWithHandle` now separates name-only targets (always call `ApplyPolicy` when path is available) from path-based targets (guarded by `HasPolicy`)
- Added SafetyNet (10s) policy recovery: processes whose image path could not be resolved at ETW callback time (`QueryFullProcessImageNameW` returned before image mapping completed) are retried on the next SafetyNet cycle

**ServiceEngine Memory Growth Fix (§9.14 Rev.17)**
- Split `enforcementQueue_` into CRITICAL / NON-CRITICAL dual-queue with absolute TOTAL_LIMIT guarantee + 512-item/tick burst cap — eliminates linear memory growth under sustained ETW load
- PendingRemoval: CAS-based size cap (MAX=512) + RAII NodeGuard. Re-enqueue loop removed, eliminating unbounded queue growth
- Fixed `ScheduleDeferredVerification` timer handle leak (`std::exchange` + `INVALID_HANDLE_VALUE` sync-wait)
- SafetyNet: 2-pass round-robin scan + 30s backstop. Missed-target recovery guaranteed within ≤30s even under ETW silent drops

### v1.0.3 (2026-03-24)

**Log Rotation Stability**
- Fixed 2nd-cycle rotation crash: infinite recursion (`Log() → WriteMessage() → CheckRotation() → Log()` → stack overflow) caused by `Log()` calls inside `CheckRotation()`. Resolved by redesigning `CheckRotation()` as a pure result-returning function; diagnostics routed through `SafeInternalLog()` which writes directly via `WriteFile`
- Fixed 2nd-cycle rename failure: replaced `MoveFileExW(REPLACE_EXISTING)` with `SetFileInformationByHandle(FileRenameInfoEx, POSIX_SEMANTICS)` — atomically replaces the destination directory entry even when Manager holds an open handle to `UnLeaf.log.1`
- `FlushFileBuffers` failure now aborts rotation and closes `fileHandle_` — self-recovers on next write cycle
- `WriteFile` failure disables logging (`enabled_ = false`) to prevent I/O error loops
- `RotationMutexGuard` RAII eliminates `ReleaseMutex` leaks on all return paths
- `ScopedHandle` for rename handle — leak-free on every code path
- `GetLastError()` captured immediately into a local variable after each API failure

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
