#pragma once
// UnLeaf v8.0 - Engine Core (Event-Driven Architecture)
//
// v8.0 Architecture Redesign:
// - Replaces polling-based enforcement with event-driven WaitForMultipleObjects
// - ETW events (process/thread creation) are primary enforcement triggers
// - Safety Net is "insurance consistency check" not monitoring (10s interval)
// - AGGRESSIVE: One-shot + deferred verification (not polling loop)
// - STABLE: Purely event-driven (no active polling)
// - PERSISTENT: 5s interval (not 50ms)
// - Config change via OS notification (FindFirstChangeNotification)
// - Merged enforcement + config watcher into single EngineControlThread
// - Idle CPU: Zero between events (WaitForMultipleObjects INFINITE)
//
// Previous versions:
// v7.0: AGGRESSIVE(10ms SET) -> STABLE(500ms GET) -> PERSISTENT(50ms SET)
// v7.4: + Resilience: ETW health check, handle robustness, graceful degradation

#include "../common/types.h"
#include "../common/scoped_handle.h"
#include "../common/config.h"
#include "../common/logger.h"
#include "process_monitor.h"
#include "../common/registry_manager.h"
#include <tlhelp32.h>
#include <map>
#include <set>
#include <atomic>
#include <thread>
#include <queue>

namespace unleaf {

// Process monitoring phase - v8.0 Event-Driven Adaptive Phase Control
enum class ProcessPhase {
    AGGRESSIVE,   // Startup: One-shot SET + deferred verification (3s)
    STABLE,       // Steady-state: Event-driven only (no active polling)
    PERSISTENT    // Stubborn EcoQoS: SET @ 5s interval
};

// v8.0: Enforcement request type (queued from ETW callbacks and timers)
enum class EnforcementRequestType : uint8_t {
    ETW_PROCESS_START,       // New target process detected via ETW
    ETW_THREAD_START,        // Thread created in tracked process (EcoQoS trigger)
    DEFERRED_VERIFICATION,   // Timer-based verification during AGGRESSIVE phase
    PERSISTENT_ENFORCE,      // Periodic enforcement for PERSISTENT phase
    SAFETY_NET               // Insurance consistency check (not monitoring)
};

// v8.0: Enforcement request structure (queued for processing by EngineControlLoop)
struct EnforcementRequest {
    DWORD pid;
    EnforcementRequestType type;
    uint8_t verifyStep;      // For DEFERRED_VERIFICATION: 1=200ms, 2=1s, 3=3s

    EnforcementRequest() : pid(0), type(EnforcementRequestType::ETW_PROCESS_START), verifyStep(0) {}
    EnforcementRequest(DWORD p, EnforcementRequestType t, uint8_t step = 0)
        : pid(p), type(t), verifyStep(step) {}
};

// v8.0: Wait handle indices for WaitForMultipleObjects
enum WaitIndex : DWORD {
    WAIT_STOP = 0,               // stopEvent_ - service stop signal
    WAIT_CONFIG_CHANGE = 1,      // configChangeHandle_ - FindFirstChangeNotification
    WAIT_SAFETY_NET = 2,         // safetyNetTimer_ - Waitable Timer (10s)
    WAIT_ENFORCEMENT_REQUEST = 3, // enforcementRequestEvent_ - queue has items
    WAIT_COUNT = 4
};

// v8.0: Deferred verification timer context
struct DeferredVerifyContext {
    class EngineCore* engine;
    DWORD pid;
    uint8_t step;  // 1, 2, or 3
};

// v7.4: Operation mode for graceful degradation
enum class OperationMode {
    NORMAL,         // ETW + QuickRescan (full functionality)
    DEGRADED_ETW,   // QuickRescan only (ETW failed)
    DEGRADED_CONFIG // Config load failed (limited functionality)
};

// v7.7: Health check info structure
struct HealthInfo {
    bool engineRunning;
    OperationMode mode;
    size_t activeProcesses;
    uint32_t totalViolations;
    bool etwHealthy;
    uint32_t etwEventCount;
    uint64_t uptimeMs;
};

// v7.94: Windows version information for compatibility
struct WindowsVersionInfo {
    DWORD major;
    DWORD minor;
    DWORD build;
    bool isWindows11OrLater;  // Build >= 22000

    WindowsVersionInfo() : major(0), minor(0), build(0), isWindows11OrLater(false) {}
};

// v7.80: Forward declaration for wait callback context
struct WaitCallbackContext;

// v6.0: NtSetInformationProcess function pointer type
typedef NTSTATUS(NTAPI* PFN_NtSetInformationProcess)(
    HANDLE ProcessHandle,
    ULONG ProcessInformationClass,
    PVOID ProcessInformation,
    ULONG ProcessInformationLength
);

// v6.0: NtQueryInformationProcess function pointer type (for diagnostics)
typedef NTSTATUS(NTAPI* PFN_NtQueryInformationProcess)(
    HANDLE ProcessHandle,
    ULONG ProcessInformationClass,
    PVOID ProcessInformation,
    ULONG ProcessInformationLength,
    PULONG ReturnLength
);

// Tracked process information
struct TrackedProcess {
    DWORD pid;
    DWORD parentPid;
    std::wstring name;
    ScopedHandle processHandle;       // Control handle (0x1200)
    ScopedHandle waitProcessHandle;   // v7.0: Exit detection handle (SYNCHRONIZE)
    HANDLE waitHandle;                // RegisterWaitForSingleObject handle
    bool isChild;

    // v3.0: Phase-based enforcement
    ProcessPhase phase;           // Current monitoring phase
    ULONGLONG phaseStartTime;     // When current phase started
    ULONGLONG lastCheckTime;      // Last EcoQoS check time
    ULONGLONG lastPriorityCheck;  // Last priority check time
    uint32_t violationCount;      // EcoQoS re-enablement count

    // v5.0: Self-healing
    uint8_t consecutiveFailures;
    DWORD lastErrorCode;
    ULONGLONG nextRetryTime;

    // v5.0: Job Object tracking
    DWORD rootTargetPid;
    bool inJobObject;
    bool jobAssignmentFailed;

    // v7.3: STABLE trust-based interval (kept for statistics)
    ULONGLONG stableCheckInterval;   // Not used in v8.0 (event-driven)
    ULONGLONG lastViolationTime;     // Last violation timestamp (0 = never)
    bool isTrustedStable;            // Trusted stable flag

    // v8.0: ETW boost for PERSISTENT phase (rate-limited instant response)
    ULONGLONG lastEtwEnforceTime;    // Last ETW-triggered enforce time (for rate limiting)

    // v8.0: Timer handles for deferred verification and persistent enforcement
    HANDLE deferredTimer;            // AGGRESSIVE phase deferred verification timer
    HANDLE persistentTimer;          // PERSISTENT phase periodic enforcement timer

    TrackedProcess()
        : pid(0), parentPid(0), waitHandle(nullptr), isChild(false)
        , phase(ProcessPhase::AGGRESSIVE), phaseStartTime(0)
        , lastCheckTime(0), lastPriorityCheck(0), violationCount(0)
        , consecutiveFailures(0), lastErrorCode(0), nextRetryTime(0)
        , rootTargetPid(0), inJobObject(false), jobAssignmentFailed(false)
        , stableCheckInterval(500), lastViolationTime(0), isTrustedStable(false)
        , lastEtwEnforceTime(0)
        , deferredTimer(nullptr), persistentTimer(nullptr) {}
};

class EngineCore {
public:
    // Singleton access
    static EngineCore& Instance();

    // Initialize engine with base directory
    bool Initialize(const std::wstring& baseDir);

    // Start all monitoring threads
    void Start();

    // Stop all monitoring
    void Stop();

    // Is engine running?
    bool IsRunning() const { return running_.load(); }

    // Get active tracked process count
    size_t GetActiveProcessCount() const;

    // v7.7: Get health check info
    HealthInfo GetHealthInfo() const;

private:
    EngineCore();
    ~EngineCore();
    EngineCore(const EngineCore&) = delete;
    EngineCore& operator=(const EngineCore&) = delete;

    // === v8.0: Event-Driven Architecture ===

    // ETW callback: called immediately when a process starts (~1ms)
    void OnProcessStart(DWORD pid, DWORD parentPid, const std::wstring& imageName);

    // v8.0: ETW callback for thread creation events (triggers for tracked processes)
    void OnThreadStart(DWORD threadId, DWORD ownerPid);

    // v8.0: Main event-driven control loop (replaces EnforcementLoop + ConfigWatcherLoop)
    // Waits on: stopEvent, configChangeHandle, safetyNetTimer, enforcementRequestEvent
    void EngineControlLoop();

    // v8.0: Enqueue an enforcement request (called from ETW callbacks and timers)
    void EnqueueRequest(const EnforcementRequest& req);

    // v8.0: Process all queued enforcement requests
    void ProcessEnforcementQueue();

    // v8.0: Dispatch a single enforcement request
    void DispatchEnforcementRequest(const EnforcementRequest& req);

    // v8.0: Handle config file change notification
    void HandleConfigChange();

    // v8.0: SAFETY NET - Insurance consistency check (NOT monitoring)
    // Checks only tracked processes for EcoQoS re-enablement
    void HandleSafetyNetCheck();

    // v8.0: Schedule deferred verification timer (AGGRESSIVE phase)
    void ScheduleDeferredVerification(DWORD pid, uint8_t step);

    // v8.0: Cancel all timers for a process (cleanup)
    void CancelProcessTimers(TrackedProcess& tp);

    // v8.0: Start persistent enforcement timer
    void StartPersistentTimer(DWORD pid);

    // v8.0: Timer callback for deferred verification (thread pool)
    static void CALLBACK DeferredVerifyTimerCallback(PVOID lpParameter, BOOLEAN timerOrWaitFired);

    // v8.0: Timer callback for persistent enforcement (thread pool)
    static void CALLBACK PersistentEnforceTimerCallback(PVOID lpParameter, BOOLEAN timerOrWaitFired);

    // v8.0: Periodic maintenance (ETW health, job refresh, stats - piggybacks on wakeups)
    void PerformPeriodicMaintenance(ULONGLONG now);

    // === Process management ===

    // Apply optimization to a single process
    bool ApplyOptimization(DWORD pid, const std::wstring& name, bool isChild, DWORD parentPid = 0);

    // Update phase for a tracked process
    void UpdatePhase(DWORD pid, ULONGLONG now);

    // Callback when a tracked process exits
    static void CALLBACK OnProcessExit(PVOID lpParameter, BOOLEAN timerOrWaitFired);

    // Remove tracking for a process
    void RemoveTrackedProcess(DWORD pid);

    // v5.0: Stateless pulse enforcement (Set-only, no Get)
    bool PulseEnforce(HANDLE hProcess, DWORD pid, bool isIntensive);

    // v6.0: Enhanced pulse enforcement with NtSetInformationProcess
    bool PulseEnforceV6(HANDLE hProcess, DWORD pid, bool isIntensive);

    // v6.0: Apply registry policy for a target process
    bool ApplyRegistryPolicy(const std::wstring& exePath, const std::wstring& exeName);

    // v7.80: Consolidated thread throttling (merged Optimized + V6)
    // @param aggressive: true for V6-style aggressive boost, false for conservative
    int DisableThreadThrottling(DWORD pid, bool aggressive);

    // v5.0: Centralized state update after enforcement
    void UpdateEnforceState(DWORD pid, ULONGLONG now, bool success);

    // v5.0: Self-healing error handling
    void HandleEnforceError(HANDLE hProcess, DWORD pid, DWORD error);
    bool ReopenProcessHandle(DWORD pid);

    // v5.0: Job Object management
    bool CreateAndAssignJobObject(DWORD rootPid, HANDLE hProcess);
    void RefreshJobObjectPids();
    void CleanupJobObjects();

    // v5.0: Set process phase externally
    void SetProcessPhase(DWORD pid, ProcessPhase phase);

    // === State checks (v7.0: GET reinstated - Adaptive Phase Control) ===

    // v7.0: Check if EcoQoS (Efficiency Mode) is currently enabled
    bool IsEcoQoSEnabled(HANDLE hProcess) const;

    // v7.0: Phase-based enforcement handler
    void ProcessPhaseEnforcement(DWORD pid, HANDLE hProcess, ProcessPhase phase, ULONGLONG now);

    // v7.80: Phase-specific enforcement handlers (extracted from ProcessPhaseEnforcement)
    void HandleAggressivePhase(TrackedProcess& tp, DWORD pid, HANDLE hProcess, ULONGLONG now);
    void HandleStablePhase(TrackedProcess& tp, DWORD pid, HANDLE hProcess, ULONGLONG now);
    void HandlePersistentPhase(TrackedProcess& tp, DWORD pid, HANDLE hProcess, ULONGLONG now);

    // Check if PID is being tracked
    bool IsTracked(DWORD pid) const;

    // Check if this is a target process name
    bool IsTargetName(const std::wstring& name) const;

    // Check if parent is being tracked
    bool IsTrackedParent(DWORD parentPid) const;

    // === Configuration ===

    // Build target set from config
    void RefreshTargetSet();

    // Remove tracked processes that are no longer targets
    void CleanupRemovedTargets();

    // Initial scan for existing target processes
    void InitialScan();

    // Initial scan for existing target processes (startup only)
    // v8.0: Also used as fallback in DEGRADED_ETW mode
    void InitialScanForDegradedMode();

    // === Members ===

    std::wstring baseDir_;
    std::atomic<bool> running_;
    std::atomic<bool> stopRequested_;

    // v8.0: Event-driven thread management
    ProcessMonitor processMonitor_;       // ETW-based process monitor
    std::thread engineControlThread_;     // Single control thread (replaces enforcement + config watcher)
    HANDLE stopEvent_;                    // Service stop signal (manual reset)

    // v8.0: Event-driven synchronization handles
    HANDLE timerQueue_;                   // Timer Queue for deferred verification and persistent timers
    HANDLE configChangeHandle_;           // FindFirstChangeNotification handle
    HANDLE safetyNetTimer_;               // Waitable Timer for safety net (10s)
    HANDLE enforcementRequestEvent_;      // Auto-reset event to signal queue has items

    // v8.0: Enforcement request queue (thread-safe)
    std::queue<EnforcementRequest> enforcementQueue_;
    mutable CriticalSection queueCs_;

    // Tracked processes (PID -> TrackedProcess)
    std::map<DWORD, std::unique_ptr<TrackedProcess>> trackedProcesses_;
    mutable CriticalSection trackedCs_;

    // v7.80: Wait callback context tracking for safe cleanup
    std::map<DWORD, WaitCallbackContext*> waitContexts_;

    // Target process names (lowercase for comparison)
    std::set<std::wstring> targetSet_;
    mutable CriticalSection targetCs_;

    // Enforcement statistics
    std::atomic<uint32_t> totalViolations_;
    ULONGLONG lastStatsLogTime_;

    // v5.0: Job Objects (rootPid -> JobObjectInfo)
    std::map<DWORD, std::unique_ptr<JobObjectInfo>> jobObjects_;
    mutable CriticalSection jobCs_;

    // v5.0: Self-healing statistics
    std::atomic<uint32_t> totalRetries_;
    std::atomic<uint32_t> totalHandleReopen_;

    // v5.0: Last job refresh time
    ULONGLONG lastJobQueryTime_;

    // v8.0: Last safety net check time (for logging only, not timing)
    ULONGLONG lastSafetyNetTime_;

    // v7.4: Resilience - operation mode and health tracking
    OperationMode operationMode_;
    ULONGLONG lastEtwHealthCheck_;

    // v8.0: Last degraded mode scan time
    ULONGLONG lastDegradedScanTime_;

    // v7.7: Service start time for uptime calculation
    ULONGLONG startTime_;

    // v6.0: NT API function pointers
    HMODULE ntdllHandle_;
    PFN_NtSetInformationProcess pfnNtSetInformationProcess_;
    PFN_NtQueryInformationProcess pfnNtQueryInformationProcess_;
    bool ntApiAvailable_;

    // v7.94: Windows version for compatibility checks
    WindowsVersionInfo winVersion_;

    // v6.0: Registry policy applied set (exeName -> applied)
    std::set<std::wstring> policyAppliedSet_;
    mutable CriticalSection policySetCs_;

    // v6.0: Statistics
    std::atomic<uint32_t> ntApiSuccessCount_;
    std::atomic<uint32_t> ntApiFailCount_;
    std::atomic<uint32_t> policyApplyCount_;

    // === v8.0: Event-Driven Timing Constants ===

    // AGGRESSIVE Phase: Deferred verification timings (One-shot + verify, not polling)
    static constexpr ULONGLONG AGGRESSIVE_DURATION = 3000;       // Total AGGRESSIVE phase duration
    static constexpr ULONGLONG DEFERRED_VERIFY_1 = 200;          // First verification at 200ms
    static constexpr ULONGLONG DEFERRED_VERIFY_2 = 1000;         // Second verification at 1s
    static constexpr ULONGLONG DEFERRED_VERIFY_FINAL = 3000;     // Final verification at 3s

    // PERSISTENT Phase: Long-interval enforcement (not polling)
    static constexpr ULONGLONG PERSISTENT_ENFORCE_INTERVAL = 5000;  // 5s enforcement interval
    static constexpr ULONGLONG PERSISTENT_CLEAN_THRESHOLD = 60000;  // 60s clean to exit PERSISTENT
    static constexpr ULONGLONG ETW_BOOST_RATE_LIMIT = 1000;         // 1s rate limit for ETW boost in PERSISTENT

    // SAFETY NET: Insurance consistency check (NOT monitoring)
    // This is NOT polling - it's a last-resort check for event misses and OS quirks
    static constexpr ULONGLONG SAFETY_NET_INTERVAL = 10000;      // 10s safety net check

    // Phase transition
    static constexpr uint32_t VIOLATION_THRESHOLD = 3;           // 3 violations -> PERSISTENT

    // Periodic maintenance (piggybacks on other wakeups)
    static constexpr ULONGLONG STATS_LOG_INTERVAL = 60000;       // Stats logging
    static constexpr ULONGLONG JOB_QUERY_INTERVAL = 5000;        // Job Object refresh
    static constexpr ULONGLONG ETW_HEALTH_CHECK_INTERVAL = 30000; // ETW health check

    // DEGRADED_ETW mode: Fallback scan interval
    static constexpr ULONGLONG DEGRADED_SCAN_INTERVAL = 30000;   // 30s fallback scan
};

} // namespace unleaf