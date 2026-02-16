#pragma once
// UnLeaf - Engine Core (Event-Driven Architecture)

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
#include <memory>
#include <utility>

// Linker-level guard against accidental timeBeginPeriod usage
#pragma detect_mismatch("UnLeaf_NoHighResTimer", "enforced")

namespace unleaf {

// Process monitoring phase - Event-Driven Adaptive Phase Control
enum class ProcessPhase {
    AGGRESSIVE,   // Startup: One-shot SET + deferred verification (3s)
    STABLE,       // Steady-state: Event-driven only (no active polling)
    PERSISTENT    // Stubborn EcoQoS: SET @ 5s interval
};

// Enforcement request type (queued from ETW callbacks and timers)
enum class EnforcementRequestType : uint8_t {
    ETW_PROCESS_START,       // New target process detected via ETW
    ETW_THREAD_START,        // Thread created in tracked process (EcoQoS trigger)
    DEFERRED_VERIFICATION,   // Timer-based verification during AGGRESSIVE phase
    PERSISTENT_ENFORCE,      // Periodic enforcement for PERSISTENT phase
    SAFETY_NET               // Insurance consistency check (not monitoring)
};

// Enforcement request structure (queued for processing by EngineControlLoop)
struct EnforcementRequest {
    DWORD pid;
    EnforcementRequestType type;
    uint8_t verifyStep;      // For DEFERRED_VERIFICATION: 1=200ms, 2=1s, 3=3s

    EnforcementRequest() : pid(0), type(EnforcementRequestType::ETW_PROCESS_START), verifyStep(0) {}
    EnforcementRequest(DWORD p, EnforcementRequestType t, uint8_t step = 0)
        : pid(p), type(t), verifyStep(step) {}
};

// Wait handle indices for WaitForMultipleObjects
enum WaitIndex : DWORD {
    WAIT_STOP = 0,               // stopEvent_ - service stop signal
    WAIT_CONFIG_CHANGE = 1,      // configChangeHandle_ - FindFirstChangeNotification
    WAIT_SAFETY_NET = 2,         // safetyNetTimer_ - Waitable Timer (10s)
    WAIT_ENFORCEMENT_REQUEST = 3, // enforcementRequestEvent_ - queue has items
    WAIT_PROCESS_EXIT = 4,       // hWakeupEvent_ - process exit pending removal
    WAIT_COUNT = 5
};

// Deferred verification timer context (forward declaration - defined after TrackedProcess)
struct DeferredVerifyContext;

// Operation mode for graceful degradation
enum class OperationMode {
    NORMAL,         // ETW + QuickRescan (full functionality)
    DEGRADED_ETW,   // QuickRescan only (ETW failed)
    DEGRADED_CONFIG // Config load failed (limited functionality)
};

// Health check info structure
struct HealthInfo {
    // Existing
    bool engineRunning;
    OperationMode mode;
    size_t activeProcesses;
    uint32_t totalViolations;
    bool etwHealthy;
    uint32_t etwEventCount;
    uint64_t uptimeMs;

    // Phase breakdown
    uint32_t aggressiveCount;
    uint32_t stableCount;
    uint32_t persistentCount;

    // Wakeup counters
    uint32_t wakeupConfigChange;
    uint32_t wakeupSafetyNet;
    uint32_t wakeupEnforcementRequest;
    uint32_t wakeupProcessExit;

    // PERSISTENT enforce applied/skipped
    uint32_t persistentEnforceApplied;
    uint32_t persistentEnforceSkipped;

    // Shutdown warnings
    uint32_t shutdownWarnings;

    // Error counters
    uint32_t error5Count;
    uint32_t error87Count;

    // Config monitoring
    uint32_t configChangeDetected;
    uint32_t configReloadCount;
};

// Windows version information for compatibility
struct WindowsVersionInfo {
    DWORD major;
    DWORD minor;
    DWORD build;
    bool isWindows11OrLater;  // Build >= 22000

    WindowsVersionInfo() : major(0), minor(0), build(0), isWindows11OrLater(false) {}
};

// Forward declaration for wait callback context
struct WaitCallbackContext;

// NtSetInformationProcess function pointer type
typedef NTSTATUS(NTAPI* PFN_NtSetInformationProcess)(
    HANDLE ProcessHandle,
    ULONG ProcessInformationClass,
    PVOID ProcessInformation,
    ULONG ProcessInformationLength
);

// NtQueryInformationProcess function pointer type (for diagnostics)
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
    ScopedHandle waitProcessHandle;   // Exit detection handle (SYNCHRONIZE)
    HANDLE waitHandle;                // RegisterWaitForSingleObject handle
    bool isChild;

    // Phase-based enforcement
    ProcessPhase phase;           // Current monitoring phase
    ULONGLONG phaseStartTime;     // When current phase started
    ULONGLONG lastCheckTime;      // Last EcoQoS check time
    ULONGLONG lastPriorityCheck;  // Last priority check time
    uint32_t violationCount;      // EcoQoS re-enablement count

    // Self-healing
    uint8_t consecutiveFailures;
    DWORD lastErrorCode;
    ULONGLONG nextRetryTime;

    // Job Object tracking
    DWORD rootTargetPid;
    bool inJobObject;
    bool jobAssignmentFailed;

    ULONGLONG lastViolationTime;     // Last violation timestamp (0 = never)

    // ETW boost for PERSISTENT phase (rate-limited instant response)
    ULONGLONG lastEtwEnforceTime;    // Last ETW-triggered enforce time (for rate limiting)

    // Timer handles for deferred verification and persistent enforcement
    HANDLE deferredTimer;            // AGGRESSIVE phase deferred verification timer
    HANDLE persistentTimer;          // PERSISTENT phase periodic enforcement timer
    DeferredVerifyContext* persistentTimerContext;  // Recurring timer context (owned pointer)
    DeferredVerifyContext* deferredTimerContext;    // One-shot timer context (owned pointer)

    TrackedProcess()
        : pid(0), parentPid(0), waitHandle(nullptr), isChild(false)
        , phase(ProcessPhase::AGGRESSIVE), phaseStartTime(0)
        , lastCheckTime(0), lastPriorityCheck(0), violationCount(0)
        , consecutiveFailures(0), lastErrorCode(0), nextRetryTime(0)
        , rootTargetPid(0), inJobObject(false), jobAssignmentFailed(false)
        , lastViolationTime(0)
        , lastEtwEnforceTime(0)
        , deferredTimer(nullptr), persistentTimer(nullptr)
        , persistentTimerContext(nullptr)
        , deferredTimerContext(nullptr) {}
};

// Deferred verification timer context (defined after TrackedProcess for shared_ptr)
struct DeferredVerifyContext {
    class EngineCore* engine;
    DWORD pid;
    uint8_t step;  // 1, 2, or 3
    std::shared_ptr<TrackedProcess> process;  // prevent premature destruction
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

    // Get health check info
    HealthInfo GetHealthInfo() const;

private:
    EngineCore();
    ~EngineCore();
    EngineCore(const EngineCore&) = delete;
    EngineCore& operator=(const EngineCore&) = delete;

    // === Event-Driven Architecture ===

    // ETW callback: called immediately when a process starts (~1ms)
    void OnProcessStart(DWORD pid, DWORD parentPid, const std::wstring& imageName);

    // ETW callback for thread creation events (triggers for tracked processes)
    void OnThreadStart(DWORD threadId, DWORD ownerPid);

    // Main event-driven control loop
    // Waits on: stopEvent, configChangeHandle, safetyNetTimer, enforcementRequestEvent
    void EngineControlLoop();

    // Enqueue an enforcement request (called from ETW callbacks and timers)
    void EnqueueRequest(const EnforcementRequest& req);

    // Process all queued enforcement requests
    void ProcessEnforcementQueue();

    // Dispatch a single enforcement request
    void DispatchEnforcementRequest(const EnforcementRequest& req);

    // Handle config file change notification
    void HandleConfigChange();

    // SAFETY NET - Insurance consistency check (NOT monitoring)
    // Checks only tracked processes for EcoQoS re-enablement
    void HandleSafetyNetCheck();

    // Process pending removal queue (called from EngineControlLoop only)
    void ProcessPendingRemovals();

    // Schedule deferred verification timer (AGGRESSIVE phase)
    void ScheduleDeferredVerification(DWORD pid, uint8_t step);

    // Cancel all timers for a process (cleanup)
    void CancelProcessTimers(TrackedProcess& tp);

    // Start persistent enforcement timer
    void StartPersistentTimer(DWORD pid);

    // Timer callback for deferred verification (thread pool)
    static void CALLBACK DeferredVerifyTimerCallback(PVOID lpParameter, BOOLEAN timerOrWaitFired);

    // Timer callback for persistent enforcement (thread pool)
    static void CALLBACK PersistentEnforceTimerCallback(PVOID lpParameter, BOOLEAN timerOrWaitFired);

    // Periodic maintenance (ETW health, job refresh, stats - piggybacks on wakeups)
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

    // Stateless pulse enforcement (Set-only, no Get)
    bool PulseEnforce(HANDLE hProcess, DWORD pid, bool isIntensive);

    // Enhanced pulse enforcement with NtSetInformationProcess
    bool PulseEnforceV6(HANDLE hProcess, DWORD pid, bool isIntensive);

    // Apply registry policy for a target process
    bool ApplyRegistryPolicy(const std::wstring& exePath, const std::wstring& exeName);

    // Consolidated thread throttling
    // @param aggressive: true for aggressive boost, false for conservative
    int DisableThreadThrottling(DWORD pid, bool aggressive);

    // Centralized state update after enforcement
    void UpdateEnforceState(DWORD pid, ULONGLONG now, bool success);

    // Self-healing error handling
    void HandleEnforceError(HANDLE hProcess, DWORD pid, DWORD error);
    bool ReopenProcessHandle(DWORD pid);

    // Job Object management
    bool CreateAndAssignJobObject(DWORD rootPid, HANDLE hProcess);
    void RefreshJobObjectPids();
    void CleanupJobObjects();

    // Set process phase externally
    void SetProcessPhase(DWORD pid, ProcessPhase phase);

    // === State checks ===

    // Check if EcoQoS (Efficiency Mode) is currently enabled
    bool IsEcoQoSEnabled(HANDLE hProcess) const;

    // Phase-based enforcement handler
    void ProcessPhaseEnforcement(DWORD pid, HANDLE hProcess, ProcessPhase phase, ULONGLONG now);

    // Phase-specific enforcement handlers
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

    // Release all kernel handles (idempotent - safe for multiple calls)
    void CleanupHandles();

    // Build target set from config
    void RefreshTargetSet();

    // Remove tracked processes that are no longer targets
    void CleanupRemovedTargets();

    // Initial scan for existing target processes
    void InitialScan();

    // Initial scan for existing target processes (startup only)
    // Also used as fallback in DEGRADED_ETW mode
    void InitialScanForDegradedMode();

    // === Members ===

    std::wstring baseDir_;
    std::atomic<bool> running_;
    std::atomic<bool> stopRequested_;

    // Event-driven thread management
    ProcessMonitor processMonitor_;       // ETW-based process monitor
    std::thread engineControlThread_;     // Single control thread
    HANDLE stopEvent_;                    // Service stop signal (manual reset)

    // Event-driven synchronization handles
    HANDLE timerQueue_;                   // Timer Queue for deferred verification and persistent timers
    HANDLE configChangeHandle_;           // FindFirstChangeNotification handle
    HANDLE safetyNetTimer_;               // Waitable Timer for safety net (10s)
    HANDLE enforcementRequestEvent_;      // Auto-reset event to signal queue has items
    HANDLE hWakeupEvent_;                 // Auto-reset event for process exit wakeup

    // Pending process removal queue (populated by OnProcessExit callback, drained by EngineControlLoop)
    std::queue<DWORD> pendingRemovalPids_;
    mutable CriticalSection pendingRemovalCs_;

    // Enforcement request queue (thread-safe)
    std::queue<EnforcementRequest> enforcementQueue_;
    mutable CriticalSection queueCs_;

    // Tracked processes (PID -> TrackedProcess)
    std::map<DWORD, std::shared_ptr<TrackedProcess>> trackedProcesses_;
    mutable CriticalSection trackedCs_;

    // Wait callback context tracking for safe cleanup
    std::map<DWORD, WaitCallbackContext*> waitContexts_;

    // Target process names (lowercase for comparison)
    std::set<std::wstring> targetSet_;
    mutable CriticalSection targetCs_;

    // Enforcement statistics
    std::atomic<uint32_t> totalViolations_;
    ULONGLONG lastStatsLogTime_;

    // Job Objects (rootPid -> JobObjectInfo)
    std::map<DWORD, std::unique_ptr<JobObjectInfo>> jobObjects_;
    mutable CriticalSection jobCs_;

    // Self-healing statistics
    std::atomic<uint32_t> totalRetries_;
    std::atomic<uint32_t> totalHandleReopen_;

    // Last job refresh time
    ULONGLONG lastJobQueryTime_;

    // Last safety net check time (for logging only, not timing)
    ULONGLONG lastSafetyNetTime_;

    // Config change debounce (FindFirstChangeNotification fires for all directory writes)
    ULONGLONG lastConfigCheckTime_;
    bool configChangePending_;

    // Operation mode and health tracking
    OperationMode operationMode_;
    ULONGLONG lastEtwHealthCheck_;

    // Last degraded mode scan time
    ULONGLONG lastDegradedScanTime_;

    // Service start time for uptime calculation
    ULONGLONG startTime_;

    // NT API function pointers
    HMODULE ntdllHandle_;
    PFN_NtSetInformationProcess pfnNtSetInformationProcess_;
    PFN_NtQueryInformationProcess pfnNtQueryInformationProcess_;
    bool ntApiAvailable_;

    // Windows version for compatibility checks
    WindowsVersionInfo winVersion_;

    // Registry policy applied set (exeName -> applied)
    std::set<std::wstring> policyAppliedSet_;
    mutable CriticalSection policySetCs_;

    // Statistics
    std::atomic<uint32_t> ntApiSuccessCount_;
    std::atomic<uint32_t> ntApiFailCount_;
    std::atomic<uint32_t> policyApplyCount_;

    // Shutdown warning counter
    std::atomic<uint32_t> shutdownWarnings_{0};

    // Event-type wakeup counters
    std::atomic<uint32_t> wakeupConfigChange_{0};
    std::atomic<uint32_t> wakeupSafetyNet_{0};
    std::atomic<uint32_t> wakeupEnforcementRequest_{0};
    std::atomic<uint32_t> wakeupProcessExit_{0};

    // PERSISTENT enforce counters
    std::atomic<uint32_t> persistentEnforceApplied_{0};
    std::atomic<uint32_t> persistentEnforceSkipped_{0};

    // Error counters (independent of log suppression)
    std::atomic<uint32_t> error5Count_{0};   // ERROR_ACCESS_DENIED
    std::atomic<uint32_t> error87Count_{0};  // ERROR_INVALID_PARAMETER

    // Error log suppression map (pid, error) -> lastLogTime
    std::map<std::pair<DWORD, DWORD>, ULONGLONG> errorLogSuppression_;

    // Config monitoring counters
    std::atomic<uint32_t> configChangeDetected_{0};
    std::atomic<uint32_t> configReloadCount_{0};

    // === Event-Driven Timing Constants ===

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

    // Config change debounce (directory-level notification includes log writes)
    static constexpr ULONGLONG CONFIG_DEBOUNCE_MS = 2000;        // 2s debounce

    // Error log suppression window (same PID × error code)
    static constexpr ULONGLONG ERROR_LOG_SUPPRESS_MS = 60000;    // 60s suppression
    
};

} // namespace unleaf