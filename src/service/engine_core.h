#pragma once
// UnLeaf - Engine Core (Event-Driven Architecture)

#include "../common/types.h"
#include "../common/scoped_handle.h"
#include "../common/config.h"
#include "../common/logger.h"
#include "process_monitor.h"
#include "../common/registry_manager.h"
#include "../engine/engine_logic.h"
#include <tlhelp32.h>
#include <map>
#include <set>
#include <list>
#include <unordered_map>
#include <vector>
#include <atomic>
#include <thread>
#include <queue>
#include <deque>
#include <memory>
#include <utility>
#include <chrono>
#include <string>

// Linker-level guard against accidental timeBeginPeriod usage
#pragma detect_mismatch("UnLeaf_NoHighResTimer", "enforced")

namespace unleaf {

// Process monitoring phase — imported from engine_logic (pure C++ module)
using ProcessPhase = engine_logic::ProcessPhase;

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
    DEGRADED_ETW    // QuickRescan only (ETW failed)
};

// Active process detail for structured JSON output
struct ActiveProcessDetail {
    DWORD pid;
    std::wstring name;
    std::string phase;       // "AGGRESSIVE", "STABLE", "PERSISTENT"
    uint32_t violations;
    bool isChild;
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

    // Enforcement telemetry
    uint32_t totalEnforcements;      // Total PulseEnforceV6 calls
    uint32_t enforceSuccessCount;    // Successful enforcements
    uint32_t enforceFailCount;       // Failed enforcements
    uint32_t enforceLatencyAvgUs;    // Average latency (microseconds)
    uint32_t enforceLatencyMaxUs;    // Maximum latency (microseconds)

    // Shutdown warnings
    uint32_t shutdownWarnings;

    // Error counters
    uint32_t error5Count;
    uint32_t error87Count;

    // Config monitoring
    uint32_t configChangeDetected;
    uint32_t configReloadCount;

    // Queue optimization
    uint32_t etwThreadDeduped;

    // Last enforcement timestamp (Unix Epoch milliseconds, system_clock based)
    uint64_t lastEnforceTimeMs;

    // Active process details (structured)
    std::vector<ActiveProcessDetail> activeProcessDetails;
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
    std::wstring fullPath;            // Normalized absolute path (GetFinalPathNameByHandleW); empty if unresolved
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

    // EcoQoS state micro-cache (reduces NtQueryInformationProcess calls during burst)
    bool ecoQosCached;               // Cache valid flag
    bool ecoQosCachedValue;          // Cached EcoQoS state
    ULONGLONG ecoQosCacheTime;       // Cache timestamp

    // Timer handles for deferred verification and persistent enforcement
    HANDLE deferredTimer;            // AGGRESSIVE phase deferred verification timer
    HANDLE persistentTimer;          // PERSISTENT phase periodic enforcement timer
    DeferredVerifyContext* persistentTimerContext;  // Recurring timer context (owned pointer)
    DeferredVerifyContext* deferredTimerContext;    // One-shot timer context (owned pointer)

    bool needsPolicyRetry;               // true = fullPath unresolved at tracking time, SafetyNet will retry

    TrackedProcess()
        : pid(0), parentPid(0), fullPath(), waitHandle(nullptr), isChild(false)
        , phase(ProcessPhase::AGGRESSIVE), phaseStartTime(0)
        , lastCheckTime(0), lastPriorityCheck(0), violationCount(0)
        , consecutiveFailures(0), lastErrorCode(0), nextRetryTime(0)
        , rootTargetPid(0), inJobObject(false), jobAssignmentFailed(false)
        , lastViolationTime(0)
        , lastEtwEnforceTime(0)
        , ecoQosCached(false), ecoQosCachedValue(false), ecoQosCacheTime(0)
        , deferredTimer(nullptr), persistentTimer(nullptr)
        , persistentTimerContext(nullptr)
        , deferredTimerContext(nullptr)
        , needsPolicyRetry(false) {}
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
    // imagePath: full path hint from ETW (may be empty/8.3/device-format — hint only)
    void OnProcessStart(DWORD pid, DWORD parentPid,
                        const std::wstring& imageName, const std::wstring& imagePath);

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
    // timersToDelete / ctxToDelete are accumulator vectors; deletion happens outside lock
    void CancelProcessTimers(TrackedProcess& tp,
                             std::vector<HANDLE>& timersToDelete,
                             std::vector<DeferredVerifyContext*>& ctxToDelete);

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
    bool ApplyOptimization(DWORD pid, const std::wstring& name, bool isChild,
                           DWORD parentPid = 0, const std::wstring& preResolvedPath = L"");

    // Inner implementation: receives an already-opened process handle + resolved path
    bool ApplyOptimizationWithHandle(DWORD pid, const std::wstring& name, bool isChild,
                                     DWORD parentPid, ScopedHandle&& scopedHandle,
                                     const std::wstring& resolvedPath);

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

    // Cached version: avoids repeated NtQueryInformationProcess during burst
    bool IsEcoQoSEnabledCached(TrackedProcess& tp, ULONGLONG now);

    // Phase-based enforcement handler
    void ProcessPhaseEnforcement(DWORD pid, HANDLE hProcess, ProcessPhase phase, ULONGLONG now);

    // Phase-specific enforcement handlers
    void HandleAggressivePhase(TrackedProcess& tp, DWORD pid, HANDLE hProcess, ULONGLONG now);
    void HandleStablePhase(TrackedProcess& tp, DWORD pid, HANDLE hProcess, ULONGLONG now);
    void HandlePersistentPhase(TrackedProcess& tp, DWORD pid, HANDLE hProcess, ULONGLONG now);

    // Check if PID is being tracked
    bool IsTracked(DWORD pid) const;

    // Check if this is a target process name (name-only set)
    bool IsTargetName(const std::wstring& name) const;

    // Check if fullPathLower matches any path target (with name fallback)
    bool IsTargetPath(const std::wstring& fullPathLower) const;

    // lock-free check: true when any path-based targets are configured
    bool HasPathTargets() const;

    // Resolve process image path via QueryFullProcessImageNameW + GetFinalPathNameByHandleW
    // Returns normalized lowercase absolute path, or empty string on failure (no fallback)
    std::wstring ResolveProcessPath(HANDLE hProcess) const;

    // Normalize a file path via GetFullPathNameW (no file handle needed — works for non-existent files)
    // Returns lowercase absolute path with prefixes stripped, or empty string on failure
    static std::wstring CanonicalizePath(const std::wstring& rawPath);

    // Check if a file exists on disk (GetFileAttributesW)
    static bool FileExistsW(const std::wstring& path);

    // Try to apply optimization by resolving and matching full path (path-target branch)
    // Opens handle, resolves path, checks targetPathSet_, delegates to ApplyOptimizationWithHandle
    void TryApplyByPath(DWORD pid, const std::wstring& name);

    // §9.14-E: SafetyNet 2-pass round-robin scan for missed targets
    void ScanRunningProcessesForMissedTargets(int maxScan);
    // Try to apply optimization for a single process if it matches a target (SafetyNet recovery)
    // Returns true if optimization was applied (new tracking started)
    bool TryApplyIfMissedTarget(DWORD pid, const wchar_t* exeName);

    // Check if parent is being tracked
    bool IsTrackedParent(DWORD parentPid) const;

    // === Configuration ===

    // Release all kernel handles (idempotent - safe for multiple calls)
    void CleanupHandles();

    // Build target set from config
    void RefreshTargetSet();

    // Proactive policy generation from config (IFEO for name targets, IFEO+PT for path targets)
    void ApplyProactivePolicies();

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

    // Enforcement request queue (thread-safe) — 2-queue CRITICAL/NON-CRITICAL split (§9.14-A)
    // CRITICAL: ETW_PROCESS_START, DEFERRED_VERIFICATION, PERSISTENT_ENFORCE, SAFETY_NET
    // NON-CRITICAL: ETW_THREAD_START (high-frequency, droppable at SOFT_LIMIT)
    std::deque<EnforcementRequest> criticalQueue_;
    std::deque<EnforcementRequest> nonCriticalQueue_;
    mutable CriticalSection queueCs_;
    std::atomic<uint32_t> enforcementDropCount_{0};
    std::atomic<uint32_t> criticalDropCount_{0};    // §9.14-A: HARD_LIMIT 超過によるドロップ数
    std::atomic<uint32_t> criticalEvictCount_{0};   // §9.14-A: TOTAL_LIMIT eviction（rotation）数（drop とは区別）

    // Tracked processes (PID -> TrackedProcess)
    std::map<DWORD, std::shared_ptr<TrackedProcess>> trackedProcesses_;
    mutable CriticalSection trackedCs_;

    // Wait callback context tracking for safe cleanup
    std::map<DWORD, WaitCallbackContext*> waitContexts_;

    // Target process sets (all protected by targetCs_)
    std::set<std::wstring> targetNameSet_;       // lowercase exe names (name-only targets)
    std::set<std::wstring> targetPathSet_;       // GetFinalPathNameByHandleW-normalized full paths
    std::set<std::wstring> pathTargetFileNames_; // exe name part of each targetPathSet_ entry (pre-filter)
    std::atomic<bool>      hasPathTargets_;      // lock-free: true when targetPathSet_ non-empty
    mutable CriticalSection targetCs_;

    // Enforcement statistics
    std::atomic<uint32_t> totalViolations_;
    ULONGLONG lastStatsLogTime_;
    ULONGLONG lastDiagLogTime_;   // tracks DIAG_LOG_INTERVAL_MS cadence (independent of stats)
    LARGE_INTEGER qpcFreq_;       // QueryPerformanceFrequency cache (initialized in Start())

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

    // §9.14-E: SafetyNet 2-pass round-robin scan state
    ULONGLONG lastSafetyScanTime_{0};      // last ScanRunningProcessesForMissedTargets invocation
    DWORD     lastScannedPid_{0};          // round-robin cursor (monotonically increasing via std::max)
    uint32_t  lastCheckedDropCount_{0};    // differential drop detection baseline
    std::atomic<uint32_t> safetyRecoveredCount_{0};  // missed-target recovery counter

    // Last process liveness check time (zombie cleanup)
    ULONGLONG lastProcessLivenessCheck_;

    // Last errorLogSuppression_ cleanup time
    ULONGLONG lastSuppressionCleanup_;

    // Last [MEM] memory diagnostics log time
    ULONGLONG lastMemLogTime_;

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

    // Registry policy LRU cache (canonPath-based)
    std::list<std::wstring> policyCacheLru_;
    std::unordered_map<std::wstring, std::list<std::wstring>::iterator> policyCacheMap_;
    mutable CriticalSection policySetCs_;

    // VerifyAndRepair last execution time + CAS guard
    ULONGLONG lastPolicyVerifyTime_ = 0;
    std::atomic<int> verifyRunning_{0};

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

    // Threadpool wait diagnostics (leak detection)
    // uint64_t: 長時間稼働サービスで uint32_t のオーバーフローを防ぐ。
    // waitDelta = reg - unreg は累計差分。実際のアクティブ待機数は watchMap + deferCtxCnt で読む。
    // ウェイトリーク確定: delta 単調増加 AND watchMap 横ばい AND deferCtx 横ばい AND handles 比例増加 — 全条件同時。
    std::atomic<uint64_t> waitRegisterCount_{0};      // cumulative RegisterWaitForSingleObject successes
    std::atomic<uint64_t> waitUnregisterCount_{0};    // cumulative UnregisterWaitEx calls that returned TRUE
    std::atomic<uint64_t> waitUnregisterFailures_{0}; // cumulative UnregisterWaitEx calls that returned FALSE
    std::atomic<uint32_t> callbackConcurrent_{0};     // OnProcessExit 同時実行数 (RAII管理)

    // PERSISTENT enforce counters
    std::atomic<uint32_t> persistentEnforceApplied_{0};
    std::atomic<uint32_t> persistentEnforceSkipped_{0};

    // Queue deduplication counter
    std::atomic<uint32_t> etwThreadDeduped_{0};

    // Enforcement telemetry counters
    std::atomic<uint32_t> enforceCount_{0};
    std::atomic<uint32_t> enforceSuccessCount_{0};
    std::atomic<uint32_t> enforceFailCount_{0};
    std::atomic<uint64_t> enforceLatencySumUs_{0};
    std::atomic<uint32_t> enforceLatencyMaxUs_{0};

    // Error counters (independent of log suppression)
    std::atomic<uint32_t> error5Count_{0};   // ERROR_ACCESS_DENIED
    std::atomic<uint32_t> error87Count_{0};  // ERROR_INVALID_PARAMETER

    // Error log suppression map (pid, error) -> lastLogTime
    std::map<std::pair<DWORD, DWORD>, ULONGLONG> errorLogSuppression_;

    // Last enforcement timestamp (Unix Epoch ms, system_clock)
    std::atomic<uint64_t> lastEnforceTimeMs_{0};

    // Config monitoring counters
    std::atomic<uint32_t> configChangeDetected_{0};
    std::atomic<uint32_t> configReloadCount_{0};

    // Engine policy (aggregates timing constants for engine_logic pure functions)
    engine_logic::EnginePolicy policy_{
        VIOLATION_THRESHOLD,
        static_cast<uint64_t>(ECOQOS_CACHE_DURATION),
        static_cast<uint32_t>(DEFERRED_VERIFY_1),
        static_cast<uint32_t>(DEFERRED_VERIFY_2),
        static_cast<uint32_t>(DEFERRED_VERIFY_FINAL)
    };

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
    static constexpr ULONGLONG ETW_STABLE_RATE_LIMIT = 200;          // 200ms rate limit for ETW in STABLE
    static constexpr ULONGLONG ECOQOS_CACHE_DURATION = 100;          // 100ms EcoQoS state cache TTL

    // SAFETY NET: Insurance consistency check (NOT monitoring)
    // This is NOT polling - it's a last-resort check for event misses and OS quirks
    static constexpr ULONGLONG SAFETY_NET_INTERVAL = 10000;      // 10s safety net check

    // Phase transition
    static constexpr uint32_t VIOLATION_THRESHOLD = 3;           // 3 violations -> PERSISTENT

    // Periodic maintenance (piggybacks on other wakeups)
    static constexpr ULONGLONG STATS_LOG_INTERVAL = 60000;       // Stats logging
#ifdef _DEBUG
    static constexpr ULONGLONG DIAG_LOG_INTERVAL_MS = 30000;     // 30s in Debug builds
#else
    static constexpr ULONGLONG DIAG_LOG_INTERVAL_MS = 120000;    // 120s in Release builds
#endif
    static constexpr uint64_t CALLBACK_LATENCY_WARN_US = 1000;   // 1ms: OnProcessExit 遅延警告閾値
    static constexpr ULONGLONG JOB_QUERY_INTERVAL = 5000;        // Job Object refresh
    static constexpr ULONGLONG ETW_HEALTH_CHECK_INTERVAL = 30000; // ETW health check

    // DEGRADED_ETW mode: Fallback scan interval
    static constexpr ULONGLONG DEGRADED_SCAN_INTERVAL = 30000;   // 30s fallback scan

    // Process liveness check interval (zombie TrackedProcess cleanup)
    static constexpr ULONGLONG LIVENESS_CHECK_INTERVAL = 60000;  // 60s

    // Config change debounce (directory-level notification includes log writes)
    static constexpr ULONGLONG CONFIG_DEBOUNCE_MS = 2000;        // 2s debounce

    // Error log suppression window (same PID × error code)
    static constexpr ULONGLONG ERROR_LOG_SUPPRESS_MS = 60000;    // 60s suppression

    // errorLogSuppression_ periodic cleanup
    static constexpr ULONGLONG SUPPRESSION_CLEANUP_INTERVAL = 60000;  // 60s cleanup cadence
    static constexpr ULONGLONG SUPPRESSION_TTL = 300000;               // 5min TTL (> suppress window to prevent re-register churn)
    static constexpr size_t    SUPPRESSION_MAX_SIZE = 2000;            // emergency cap (oldest-first eviction)

    // policyCacheMap_ size cap
    static constexpr size_t    POLICY_CACHE_MAX_SIZE = 1024;

    // [MEM] memory diagnostics log intervals
    static constexpr ULONGLONG MEM_LOG_INTERVAL_SHORT = 10000;    // 起動後 30 分: 10 秒
    static constexpr ULONGLONG MEM_LOG_INTERVAL_LONG  = 60000;    // 30 分経過後: 60 秒
    static constexpr ULONGLONG MEM_LOG_WARMUP_MS      = 1800000ULL; // 30 分

    // trackedProcesses_ hard cap (§9.00 Eviction)
    static constexpr size_t MAX_TRACKED_PROCESSES = 2000;

    // pendingRemovalPids_ saturation guard (§9.01)
    static constexpr size_t MAX_PENDING_REMOVALS = 4096;

    // §9.14-A: Enforcement queue limits (2-queue CRITICAL/NON-CRITICAL)
    // SOFT_LIMIT: NON-CRITICAL individual cap
    // HARD_LIMIT: CRITICAL individual cap
    // TOTAL_LIMIT: absolute combined cap — nonCritical eviction first, then oldest-CRITICAL eviction
    // 設定上の関係: TOTAL_LIMIT <= HARD_LIMIT が推奨。
    // TOTAL_LIMIT < HARD_LIMIT の場合、nonCritical 空でも TOTAL_LIMIT が先に踏まれ
    // CRITICAL eviction が発生する（HARD_LIMIT は事実上機能しない設定となる）。
    static constexpr size_t ENFORCEMENT_QUEUE_SOFT_LIMIT  = 4096;
    static constexpr size_t ENFORCEMENT_QUEUE_HARD_LIMIT  = 8192;
    static constexpr size_t ENFORCEMENT_QUEUE_TOTAL_LIMIT = 8192;
    // 設計制約: TOTAL_LIMIT >= HARD_LIMIT を compile-time で強制。
    // TOTAL_LIMIT < HARD_LIMIT 設定は HARD_LIMIT を事実上無効化し、nonCritical 空時に
    // CRITICAL eviction が予期せず発生する（R6 バグシナリオ）。ビルドレベルで根絶する。
    static_assert(ENFORCEMENT_QUEUE_TOTAL_LIMIT >= ENFORCEMENT_QUEUE_HARD_LIMIT,
        "TOTAL_LIMIT must be >= HARD_LIMIT; otherwise HARD_LIMIT is bypassed by TOTAL_LIMIT eviction");
    // MAX_CRITICAL_PER_TICK: 1 tick あたりの CRITICAL 処理上限（CPU バースト防止）
    // HARD_LIMIT=8192 の全量を 1 tick で処理すると kernel 呼び出し連続で CPU スパイクが起きうる
    static constexpr int    ENFORCEMENT_CRITICAL_PER_TICK = 512;

    // §9.14-E: SafetyNet missed-target scan constants
    static constexpr int    MAX_SAFETY_SCAN_PER_TICK   = 64;
    // 30 秒 periodic バックストップ: ETW silent drop（kernel レベルのイベントロス）で
    // hasCriticalDrop が不発の場合でも最大 30 秒以内に ScanRunningProcessesForMissedTargets を発火。
    static constexpr ULONGLONG SAFETY_SCAN_BACKSTOP_MS = 30ULL * 1000;

    // Select eviction candidate from trackedProcesses_ (call while holding trackedCs_)
    // Priority: 1) invalid handle (zombie), 2) oldest phaseStartTime
    // Returns PID of victim, or 0 if map is empty.
    DWORD SelectEvictionCandidate() const;

};

} // namespace unleaf