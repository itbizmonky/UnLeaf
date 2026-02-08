// UnLeaf v8.0 - Engine Core Implementation (Event-Driven Architecture)
//
// v8.0 Architecture Redesign:
// - Event-driven WaitForMultipleObjects loop (replaces 10ms polling)
// - ETW process/thread events are primary enforcement triggers
// - SAFETY NET is "insurance consistency check" at 10s interval, NOT monitoring
// - AGGRESSIVE: One-shot SET + deferred verification (not polling loop)
// - STABLE: Purely event-driven (zero active polling)
// - PERSISTENT: 5s interval (not 50ms)
// - Config change via OS notification (FindFirstChangeNotification)
// - Single EngineControlThread (merged enforcement + config watcher)
// - Idle CPU: Zero between events
//
// Previous versions:
// v7.0: AGGRESSIVE(10ms SET) -> STABLE(500ms GET) -> PERSISTENT(50ms SET)
// v7.4: + Resilience: ETW health check, handle robustness, graceful degradation

#include "engine_core.h"
#include <algorithm>
#include <sstream>
#include <functional>

namespace unleaf {

// Helper: ProcessPhase to string (for debug logging)
static const wchar_t* PhaseToString(ProcessPhase p) {
    switch (p) {
        case ProcessPhase::AGGRESSIVE: return L"AGGRESSIVE";
        case ProcessPhase::STABLE:     return L"STABLE";
        case ProcessPhase::PERSISTENT: return L"PERSISTENT";
        default:                       return L"UNKNOWN";
    }
}

// Context structure for wait callback
struct WaitCallbackContext {
    EngineCore* engine;
    DWORD pid;
};

EngineCore& EngineCore::Instance() {
    static EngineCore instance;
    return instance;
}

EngineCore::EngineCore()
    : running_(false)
    , stopRequested_(false)
    , stopEvent_(nullptr)
    , timerQueue_(nullptr)
    , configChangeHandle_(INVALID_HANDLE_VALUE)
    , safetyNetTimer_(nullptr)
    , enforcementRequestEvent_(nullptr)
    , totalViolations_(0)
    , lastStatsLogTime_(0)
    , totalRetries_(0)
    , totalHandleReopen_(0)
    , lastJobQueryTime_(0)
    , lastSafetyNetTime_(0)
    , ntdllHandle_(nullptr)
    , pfnNtSetInformationProcess_(nullptr)
    , pfnNtQueryInformationProcess_(nullptr)
    , ntApiAvailable_(false)
    , ntApiSuccessCount_(0)
    , ntApiFailCount_(0)
    , policyApplyCount_(0)
    , operationMode_(OperationMode::NORMAL)
    , lastEtwHealthCheck_(0)
    , lastDegradedScanTime_(0)
    , startTime_(0) {
}

EngineCore::~EngineCore() {
    Stop();
}

bool EngineCore::Initialize(const std::wstring& baseDir) {
    baseDir_ = baseDir;

    // Create stop event
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent_) {
        LOG_ERROR(L"Engine: Failed to create stop event");
        return false;
    }

    // Initialize config
    if (!UnLeafConfig::Instance().Initialize(baseDir)) {
        LOG_ERROR(L"Engine: Failed to initialize config");
        return false;
    }

    // Initialize logger
    if (!LightweightLogger::Instance().Initialize(baseDir)) {
        LOG_ERROR(L"Engine: Failed to initialize logger");
        return false;
    }

    // v7.6: Apply log level from config
    LightweightLogger::Instance().SetLogLevel(UnLeafConfig::Instance().GetLogLevel());

    // Load initial targets
    RefreshTargetSet();

    // v8.0: Create Timer Queue for deferred verification and persistent enforcement timers
    timerQueue_ = CreateTimerQueue();
    if (!timerQueue_) {
        LOG_ERROR(L"Engine: Failed to create Timer Queue");
        return false;
    }

    // v8.0: Create config change notification (event-driven, replaces polling)
    std::wstring configDir = baseDir_;
    configChangeHandle_ = FindFirstChangeNotificationW(
        configDir.c_str(),
        FALSE,  // Do not watch subtree
        FILE_NOTIFY_CHANGE_LAST_WRITE
    );
    if (configChangeHandle_ == INVALID_HANDLE_VALUE) {
        LOG_ALERT(L"Engine: FindFirstChangeNotification failed - config changes require restart");
    }

    // v8.0: Create Safety Net waitable timer (10s periodic)
    // SAFETY NET: This is an insurance consistency check, NOT monitoring
    safetyNetTimer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    if (!safetyNetTimer_) {
        LOG_ERROR(L"Engine: Failed to create Safety Net timer");
        return false;
    }

    // v8.0: Create enforcement request event (auto-reset)
    enforcementRequestEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!enforcementRequestEvent_) {
        LOG_ERROR(L"Engine: Failed to create enforcement request event");
        return false;
    }

    // v8.0: No longer need 1ms timer precision (event-driven, minimum timer is 200ms)

    // v6.0: Load NT API functions from ntdll.dll
    ntdllHandle_ = GetModuleHandleW(L"ntdll.dll");
    if (ntdllHandle_) {
        pfnNtSetInformationProcess_ = reinterpret_cast<PFN_NtSetInformationProcess>(
            GetProcAddress(ntdllHandle_, "NtSetInformationProcess"));
        pfnNtQueryInformationProcess_ = reinterpret_cast<PFN_NtQueryInformationProcess>(
            GetProcAddress(ntdllHandle_, "NtQueryInformationProcess"));

        ntApiAvailable_ = (pfnNtSetInformationProcess_ != nullptr);
        if (ntApiAvailable_) {
            LOG_DEBUG(L"Engine: NtSetInformationProcess available");
        } else {
            LOG_ALERT(L"Engine: NtSetInformationProcess not found");
        }
    } else {
        LOG_ALERT(L"Engine: ntdll.dll not loaded");
    }

    // v7.94: Detect Windows version for compatibility
    {
        typedef NTSTATUS(WINAPI* RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
        RtlGetVersionPtr RtlGetVersion = nullptr;
        if (ntdllHandle_) {
            RtlGetVersion = reinterpret_cast<RtlGetVersionPtr>(
                GetProcAddress(ntdllHandle_, "RtlGetVersion"));
        }

        if (RtlGetVersion) {
            RTL_OSVERSIONINFOW osvi = {};
            osvi.dwOSVersionInfoSize = sizeof(osvi);
            if (RtlGetVersion(&osvi) == 0) {
                winVersion_.major = osvi.dwMajorVersion;
                winVersion_.minor = osvi.dwMinorVersion;
                winVersion_.build = osvi.dwBuildNumber;
                winVersion_.isWindows11OrLater = (osvi.dwBuildNumber >= WINDOWS_11_BUILD_THRESHOLD);
            }
        }

        // Log detected version
        std::wstringstream ss;
        ss << L"Engine: Windows " << winVersion_.major << L"." << winVersion_.minor
           << L" (Build " << winVersion_.build << L")";
        if (winVersion_.isWindows11OrLater) {
            ss << L" - Full EcoQoS support";
        } else {
            ss << L" - Limited EcoQoS (Win10 compatibility mode)";
        }
        LOG_INFO(ss.str());
    }

    // Initialize registry policy manager (centralized)
    RegistryPolicyManager::Instance().Initialize(baseDir);

    LOG_INFO(L"Engine: Initialized (Event-Driven Architecture)");
    return true;
}

void EngineCore::Start() {
    if (running_.load()) return;

    running_ = true;
    stopRequested_ = false;
    totalViolations_ = 0;
    ULONGLONG now = GetTickCount64();
    lastStatsLogTime_ = now;
    lastEtwHealthCheck_ = now;
    lastJobQueryTime_ = now;
    lastSafetyNetTime_ = now;
    lastDegradedScanTime_ = now;
    startTime_ = now;
    ResetEvent(stopEvent_);

    // v8.0: Start ETW with both process and thread callbacks
    bool etwStarted = processMonitor_.Start(
        [this](DWORD pid, DWORD parentPid, const std::wstring& imageName) {
            this->OnProcessStart(pid, parentPid, imageName);
        },
        [this](DWORD threadId, DWORD ownerPid) {
            this->OnThreadStart(threadId, ownerPid);
        }
    );

    // v7.4: Set operation mode based on ETW status
    if (etwStarted) {
        operationMode_ = OperationMode::NORMAL;
    } else {
        operationMode_ = OperationMode::DEGRADED_ETW;
        LOG_ALERT(L"ETW: Monitor failed to start - using DEGRADED mode");
    }

    // v7.0: InitialScan AFTER ETW is running (no gap)
    InitialScan();

    // v8.0: Set up Safety Net waitable timer (10s periodic)
    // SAFETY NET: Insurance consistency check - NOT monitoring
    LARGE_INTEGER dueTime;
    dueTime.QuadPart = -static_cast<LONGLONG>(SAFETY_NET_INTERVAL) * 10000LL;  // Relative time in 100ns units
    if (!SetWaitableTimer(safetyNetTimer_, &dueTime, static_cast<LONG>(SAFETY_NET_INTERVAL), nullptr, nullptr, FALSE)) {
        LOG_ERROR(L"Engine: Failed to set Safety Net timer");
    }

    // v8.0: Start single EngineControlThread (replaces enforcement + config watcher threads)
    engineControlThread_ = std::thread(&EngineCore::EngineControlLoop, this);

    // v8.0: Updated startup log
    std::wstringstream ss;
    ss << L"EngineCore started: " << targetSet_.size() << L" targets, "
       << (operationMode_ == OperationMode::NORMAL ? L"NORMAL" : L"DEGRADED_ETW")
       << L" mode, Event-Driven, SafetyNet=10s";
    LOG_DEBUG(ss.str());
}

void EngineCore::Stop() {
    if (!running_.load()) return;

    stopRequested_ = true;
    SetEvent(stopEvent_);

    // Stop ETW monitor
    processMonitor_.Stop();

    // v8.0: Wait for single control thread
    if (engineControlThread_.joinable()) {
        engineControlThread_.join();
    }

    // v8.0: Delete Timer Queue (waits for all timer callbacks to complete)
    if (timerQueue_) {
        DeleteTimerQueueEx(timerQueue_, INVALID_HANDLE_VALUE);
        timerQueue_ = nullptr;
    }

    // v8.0: Cleanup tracked processes with safe wait handle unregistration
    {
        std::vector<HANDLE> waitHandles;
        std::vector<WaitCallbackContext*> contextsToDelete;

        {
            CSLockGuard lock(trackedCs_);
            for (auto& pair : trackedProcesses_) {
                if (pair.second->waitHandle) {
                    waitHandles.push_back(pair.second->waitHandle);
                    pair.second->waitHandle = nullptr;
                }
                // Note: deferredTimer and persistentTimer are managed by timerQueue_
                // which was already deleted above
                pair.second->deferredTimer = nullptr;
                pair.second->persistentTimer = nullptr;
            }
            for (auto& [pid, ctx] : waitContexts_) {
                contextsToDelete.push_back(ctx);
            }
            waitContexts_.clear();
            trackedProcesses_.clear();
        }

        for (HANDLE h : waitHandles) {
            UnregisterWaitEx(h, INVALID_HANDLE_VALUE);
        }

        for (auto* ctx : contextsToDelete) {
            delete ctx;
        }
    }

    // v5.0: Cleanup Job Objects
    CleanupJobObjects();

    // Cleanup all registry policies (both PowerThrottling + IFEO)
    RegistryPolicyManager::Instance().CleanupAllPolicies();
    {
        CSLockGuard lock(policySetCs_);
        policyAppliedSet_.clear();
    }

    // v8.0: Cleanup event-driven handles
    if (configChangeHandle_ != INVALID_HANDLE_VALUE) {
        FindCloseChangeNotification(configChangeHandle_);
        configChangeHandle_ = INVALID_HANDLE_VALUE;
    }
    if (safetyNetTimer_) {
        CloseHandle(safetyNetTimer_);
        safetyNetTimer_ = nullptr;
    }
    if (enforcementRequestEvent_) {
        CloseHandle(enforcementRequestEvent_);
        enforcementRequestEvent_ = nullptr;
    }

    running_ = false;

    std::wstringstream ss;
    ss << L"EngineCore stopped. Violations=" << totalViolations_.load()
       << L" Retries=" << totalRetries_.load()
       << L" HandleReopen=" << totalHandleReopen_.load()
       << L" NtApiSuccess=" << ntApiSuccessCount_.load()
       << L" NtApiFail=" << ntApiFailCount_.load()
       << L" PolicyApply=" << policyApplyCount_.load();
    LOG_DEBUG(ss.str());
}

// === ETW Callbacks ===

void EngineCore::OnProcessStart(DWORD pid, DWORD parentPid, const std::wstring& imageName) {
    if (stopRequested_.load()) return;

    // Skip critical processes
    if (IsCriticalProcess(imageName)) {
        return;
    }

    // Case 1: Parent is already tracked -> this is a child process
    if (IsTrackedParent(parentPid)) {
        ApplyOptimization(pid, imageName, true, parentPid);
        return;
    }

    // Case 2: This process name is a target
    if (IsTargetName(imageName)) {
        ApplyOptimization(pid, imageName, false, 0);
    }
}

// v8.0: ETW callback for thread creation events
// Thread creation is a trigger point where OS may re-apply EcoQoS
void EngineCore::OnThreadStart(DWORD threadId, DWORD ownerPid) {
    (void)threadId;  // Not used, we only care about the owner PID

    if (stopRequested_.load()) return;

    // Quick filter: only process tracked PIDs (O(1) lookup)
    bool isTracked = false;
    ProcessPhase currentPhase = ProcessPhase::STABLE;
    {
        CSLockGuard lock(trackedCs_);
        auto it = trackedProcesses_.find(ownerPid);
        if (it != trackedProcesses_.end()) {
            isTracked = true;
            currentPhase = it->second->phase;
        }
    }

    if (!isTracked) return;

    LOG_DEBUG(L"[ETW] Thread event for PID:" + std::to_wstring(ownerPid) +
              L" phase=" + PhaseToString(currentPhase));

    // Queue check for STABLE and PERSISTENT phase processes
    // AGGRESSIVE already has active deferred verification
    // PERSISTENT has 5s timer but ETW boost provides instant response on tab switch
    if (currentPhase == ProcessPhase::STABLE || currentPhase == ProcessPhase::PERSISTENT) {
        EnqueueRequest(EnforcementRequest(ownerPid, EnforcementRequestType::ETW_THREAD_START));
    }
}

// === v8.0 Event-Driven Engine Control Loop ===

void EngineCore::EngineControlLoop() {
    LOG_INFO(L"Engine: Event-driven control loop started (SafetyNet=10s, Event-triggered)");

    // Build wait handle array
    HANDLE waitHandles[WAIT_COUNT];
    waitHandles[WAIT_STOP] = stopEvent_;
    waitHandles[WAIT_CONFIG_CHANGE] = (configChangeHandle_ != INVALID_HANDLE_VALUE) ? configChangeHandle_ : stopEvent_;
    waitHandles[WAIT_SAFETY_NET] = safetyNetTimer_;
    waitHandles[WAIT_ENFORCEMENT_REQUEST] = enforcementRequestEvent_;

    while (!stopRequested_.load()) {
        DWORD result = WaitForMultipleObjects(WAIT_COUNT, waitHandles, FALSE, INFINITE);

        if (stopRequested_.load()) break;

        ULONGLONG now = GetTickCount64();

        switch (result) {
            case WAIT_OBJECT_0 + WAIT_STOP:
                // Stop requested
                LOG_DEBUG(L"Engine: Stop event received");
                return;

            case WAIT_OBJECT_0 + WAIT_CONFIG_CHANGE:
                // Config file changed (OS notification)
                HandleConfigChange();
                if (configChangeHandle_ != INVALID_HANDLE_VALUE) {
                    FindNextChangeNotification(configChangeHandle_);
                }
                break;

            case WAIT_OBJECT_0 + WAIT_SAFETY_NET:
                // SAFETY NET: Insurance consistency check (NOT monitoring)
                LOG_DEBUG(L"[SAFETY_NET] tick");
                HandleSafetyNetCheck();
                lastSafetyNetTime_ = now;
                break;

            case WAIT_OBJECT_0 + WAIT_ENFORCEMENT_REQUEST:
                // Process queued enforcement requests from ETW callbacks and timers
                ProcessEnforcementQueue();
                break;

            case WAIT_TIMEOUT:
                // Should not happen with INFINITE, but handle gracefully
                break;

            case WAIT_FAILED:
            default:
                // Error - log and continue
                LOG_ERROR(L"Engine: WaitForMultipleObjects failed (error=" + std::to_wstring(GetLastError()) + L")");
                Sleep(1000);  // Avoid tight error loop
                break;
        }

        // v8.0: Periodic maintenance (piggybacks on any wakeup)
        PerformPeriodicMaintenance(now);
    }

    LOG_INFO(L"Engine: Event-driven control loop ended");
}

// v8.0: Enqueue enforcement request (called from ETW callbacks and timer callbacks)
void EngineCore::EnqueueRequest(const EnforcementRequest& req) {
    {
        CSLockGuard lock(queueCs_);
        enforcementQueue_.push(req);
    }
    SetEvent(enforcementRequestEvent_);  // Wake control loop
}

// v8.0: Process all queued enforcement requests
void EngineCore::ProcessEnforcementQueue() {
    // Copy queue under lock, process outside lock
    std::queue<EnforcementRequest> pending;
    {
        CSLockGuard lock(queueCs_);
        std::swap(pending, enforcementQueue_);
    }

    while (!pending.empty()) {
        if (stopRequested_.load()) break;
        EnforcementRequest req = pending.front();
        pending.pop();
        DispatchEnforcementRequest(req);
    }
}

// v8.0: Dispatch a single enforcement request
void EngineCore::DispatchEnforcementRequest(const EnforcementRequest& req) {
    CSLockGuard lock(trackedCs_);
    auto it = trackedProcesses_.find(req.pid);
    if (it == trackedProcesses_.end()) return;
    auto& tp = *it->second;

    if (!tp.processHandle.get()) return;

    ULONGLONG now = GetTickCount64();

    switch (req.type) {
        case EnforcementRequestType::ETW_THREAD_START:
            // Thread created in tracked process - check for EcoQoS violation
            if (tp.phase == ProcessPhase::STABLE) {
                bool ecoQoSOn = IsEcoQoSEnabled(tp.processHandle.get());
                if (ecoQoSOn) {
                    // Violation detected via event
                    PulseEnforceV6(tp.processHandle.get(), req.pid, true);
                    tp.violationCount++;
                    totalViolations_.fetch_add(1);
                    tp.lastViolationTime = now;

                    if (tp.violationCount >= VIOLATION_THRESHOLD) {
                        tp.phase = ProcessPhase::PERSISTENT;
                        StartPersistentTimer(req.pid);
                        std::wstringstream ss;
                        ss << L"[PERSISTENT] " << tp.name << L" (PID:" << req.pid
                           << L") via thread event (violations=" << tp.violationCount << L")";
                        LOG_DEBUG(ss.str());
                    } else {
                        tp.phase = ProcessPhase::AGGRESSIVE;
                        tp.phaseStartTime = now;
                        ScheduleDeferredVerification(req.pid, 1);
                        std::wstringstream ss;
                        ss << L"[VIOLATION] " << tp.name << L" (PID:" << req.pid
                           << L") via thread event -> AGGRESSIVE";
                        LOG_DEBUG(ss.str());
                    }
                }
                tp.lastCheckTime = now;
            } else if (tp.phase == ProcessPhase::PERSISTENT) {
                // ETW boost: rate-limited instant response for PERSISTENT phase
                // Provides immediate EcoQoS correction on tab switch without waiting for 5s timer
                if (now - tp.lastEtwEnforceTime >= ETW_BOOST_RATE_LIMIT) {
                    bool ecoQoSOn = IsEcoQoSEnabled(tp.processHandle.get());
                    LOG_DEBUG(L"[ETW_BOOST] " + tp.name + L" (PID:" + std::to_wstring(req.pid) +
                              L") EcoQoS=" + (ecoQoSOn ? L"ON->enforce" : L"OFF->skip"));
                    if (ecoQoSOn) {
                        PulseEnforceV6(tp.processHandle.get(), req.pid, true);
                        tp.lastViolationTime = now;
                    }
                    tp.lastEtwEnforceTime = now;
                    tp.lastCheckTime = now;
                }
            }
            break;

        case EnforcementRequestType::DEFERRED_VERIFICATION:
            // Timer-based verification during AGGRESSIVE phase
            if (tp.phase == ProcessPhase::AGGRESSIVE) {
                bool ecoQoSOn = IsEcoQoSEnabled(tp.processHandle.get());

                if (!ecoQoSOn) {
                    // Clean - check if this is final verification
                    if (req.verifyStep >= 3) {
                        // Final verification passed -> transition to STABLE
                        tp.phase = ProcessPhase::STABLE;
                        tp.phaseStartTime = now;
                        tp.isTrustedStable = false;
                        tp.stableCheckInterval = 500;  // Not used in v8.0 but kept for stats
                        CancelProcessTimers(tp);
                        std::wstringstream ss;
                        ss << L"[PHASE] " << tp.name << L" (PID:" << req.pid << L") -> STABLE";
                        LOG_DEBUG(ss.str());
                    } else {
                        // Schedule next verification
                        ScheduleDeferredVerification(req.pid, req.verifyStep + 1);
                    }
                } else {
                    // Violation detected
                    tp.violationCount++;
                    totalViolations_.fetch_add(1);
                    PulseEnforceV6(tp.processHandle.get(), req.pid, true);

                    if (tp.violationCount >= VIOLATION_THRESHOLD) {
                        tp.phase = ProcessPhase::PERSISTENT;
                        CancelProcessTimers(tp);
                        StartPersistentTimer(req.pid);
                        std::wstringstream ss;
                        ss << L"[PERSISTENT] " << tp.name << L" (PID:" << req.pid
                           << L") violations=" << tp.violationCount;
                        LOG_DEBUG(ss.str());
                    } else {
                        // Restart AGGRESSIVE with fresh verification sequence
                        tp.phaseStartTime = now;
                        CancelProcessTimers(tp);
                        ScheduleDeferredVerification(req.pid, 1);
                    }
                }
                tp.lastCheckTime = now;
            }
            break;

        case EnforcementRequestType::PERSISTENT_ENFORCE:
            // Periodic enforcement for PERSISTENT phase (5s interval)
            if (tp.phase == ProcessPhase::PERSISTENT) {
                LOG_DEBUG(L"[PERSISTENT_ENFORCE] " + tp.name + L" (PID:" + std::to_wstring(req.pid) + L")");
                PulseEnforceV6(tp.processHandle.get(), req.pid, true);
                tp.lastCheckTime = now;

                // Check if process has been clean long enough to exit PERSISTENT
                // (60 seconds without violation)
                ULONGLONG timeSinceLastViolation = (tp.lastViolationTime > 0) ?
                    (now - tp.lastViolationTime) : (now - tp.phaseStartTime);
                if (timeSinceLastViolation >= PERSISTENT_CLEAN_THRESHOLD) {
                    bool ecoQoSOn = IsEcoQoSEnabled(tp.processHandle.get());
                    if (!ecoQoSOn) {
                        // Clean for 60s -> transition to STABLE
                        tp.phase = ProcessPhase::STABLE;
                        tp.phaseStartTime = now;
                        CancelProcessTimers(tp);
                        std::wstringstream ss;
                        ss << L"[PHASE] " << tp.name << L" (PID:" << req.pid
                           << L") PERSISTENT -> STABLE (clean 60s)";
                        LOG_DEBUG(ss.str());
                    }
                }
            }
            break;

        case EnforcementRequestType::SAFETY_NET:
            // SAFETY NET: Insurance consistency check for this specific process
            if (tp.processHandle.get()) {
                bool ecoQoSOn = IsEcoQoSEnabled(tp.processHandle.get());
                if (ecoQoSOn && tp.phase == ProcessPhase::STABLE) {
                    // Violation detected via safety net
                    PulseEnforceV6(tp.processHandle.get(), req.pid, true);
                    tp.violationCount++;
                    totalViolations_.fetch_add(1);
                    tp.lastViolationTime = now;

                    if (tp.violationCount >= VIOLATION_THRESHOLD) {
                        tp.phase = ProcessPhase::PERSISTENT;
                        StartPersistentTimer(req.pid);
                    } else {
                        tp.phase = ProcessPhase::AGGRESSIVE;
                        tp.phaseStartTime = now;
                        ScheduleDeferredVerification(req.pid, 1);
                    }
                    std::wstringstream ss;
                    ss << L"[SAFETY_NET] " << tp.name << L" (PID:" << req.pid
                       << L") violation detected";
                    LOG_DEBUG(ss.str());
                }
                tp.lastCheckTime = now;
            }
            break;

        default:
            break;
    }
}

// v8.0: Handle config file change notification
void EngineCore::HandleConfigChange() {
    // Confirm INI file specifically changed (notification is for any file in directory)
    if (!UnLeafConfig::Instance().HasFileChanged()) {
        return;  // False positive - not our file
    }

    LOG_INFO(L"Config: Reloading (event-driven notification)");

    UnLeafConfig::Instance().Reload();
    RefreshTargetSet();

    std::wstringstream ss;
    ss << L"[CONFIG] Reloaded: " << targetSet_.size() << L" targets";
    LOG_DEBUG(ss.str());

    // Remove tracked processes that are no longer targets
    CleanupRemovedTargets();

    // Re-scan for new targets that might already be running
    InitialScan();

    LOG_INFO(L"Config: Reload complete");
}

// v8.0: SAFETY NET - Insurance consistency check (NOT monitoring)
// This is NOT polling - it's a last-resort check for event misses and OS quirks
// Only checks already-tracked processes, never scans all processes
void EngineCore::HandleSafetyNetCheck() {
    // SAFETY NET: Insurance consistency check - NOT monitoring
    // Checks only tracked processes for EcoQoS re-enablement

    std::vector<DWORD> pidsToCheck;
    {
        CSLockGuard lock(trackedCs_);
        for (const auto& [pid, tp] : trackedProcesses_) {
            if (tp->processHandle.get() && tp->phase == ProcessPhase::STABLE) {
                pidsToCheck.push_back(pid);
            }
        }
    }

    // Queue safety net checks for each tracked process
    for (DWORD pid : pidsToCheck) {
        if (stopRequested_.load()) break;
        EnqueueRequest(EnforcementRequest(pid, EnforcementRequestType::SAFETY_NET));
    }

    // Process the queue immediately (we're already in the control loop)
    ProcessEnforcementQueue();
}

// v8.0: Schedule deferred verification timer (AGGRESSIVE phase)
void EngineCore::ScheduleDeferredVerification(DWORD pid, uint8_t step) {
    if (!timerQueue_) return;

    // Determine delay based on step
    DWORD delayMs;
    switch (step) {
        case 1: delayMs = static_cast<DWORD>(DEFERRED_VERIFY_1); break;   // 200ms
        case 2: delayMs = static_cast<DWORD>(DEFERRED_VERIFY_2 - DEFERRED_VERIFY_1); break;  // 800ms more
        case 3: delayMs = static_cast<DWORD>(DEFERRED_VERIFY_FINAL - DEFERRED_VERIFY_2); break;  // 2000ms more
        default: return;
    }

    // Create context for timer callback
    auto* context = new DeferredVerifyContext{this, pid, step};

    // Get reference to tracked process to store timer handle
    {
        CSLockGuard lock(trackedCs_);
        auto it = trackedProcesses_.find(pid);
        if (it == trackedProcesses_.end()) {
            delete context;
            return;
        }

        HANDLE timer = nullptr;
        if (CreateTimerQueueTimer(
                &timer,
                timerQueue_,
                DeferredVerifyTimerCallback,
                context,
                delayMs,
                0,  // One-shot
                WT_EXECUTEONLYONCE)) {
            it->second->deferredTimer = timer;
        } else {
            delete context;
        }
    }
}

// v8.0: Cancel all timers for a process
void EngineCore::CancelProcessTimers(TrackedProcess& tp) {
    // Note: Timer handles are managed by the timer queue
    // When the timer queue is deleted, all timers are automatically cleaned up
    // Here we just clear our references
    tp.deferredTimer = nullptr;
    tp.persistentTimer = nullptr;
}

// v8.0: Start persistent enforcement timer (5s recurring)
void EngineCore::StartPersistentTimer(DWORD pid) {
    if (!timerQueue_) return;

    // Create context
    auto* context = new DeferredVerifyContext{this, pid, 0};

    {
        CSLockGuard lock(trackedCs_);
        auto it = trackedProcesses_.find(pid);
        if (it == trackedProcesses_.end()) {
            delete context;
            return;
        }

        // Cancel existing persistent timer if any
        if (it->second->persistentTimer) {
            DeleteTimerQueueTimer(timerQueue_, it->second->persistentTimer, nullptr);
            it->second->persistentTimer = nullptr;
        }

        HANDLE timer = nullptr;
        if (CreateTimerQueueTimer(
                &timer,
                timerQueue_,
                PersistentEnforceTimerCallback,
                context,
                static_cast<DWORD>(PERSISTENT_ENFORCE_INTERVAL),  // Initial delay
                static_cast<DWORD>(PERSISTENT_ENFORCE_INTERVAL),  // Period (recurring)
                WT_EXECUTEDEFAULT)) {
            it->second->persistentTimer = timer;
        } else {
            delete context;
        }
    }
}

// v8.0: Timer callback for deferred verification
void CALLBACK EngineCore::DeferredVerifyTimerCallback(PVOID lpParameter, BOOLEAN timerOrWaitFired) {
    (void)timerOrWaitFired;
    auto* context = static_cast<DeferredVerifyContext*>(lpParameter);
    if (context && context->engine && !context->engine->stopRequested_.load()) {
        context->engine->EnqueueRequest(
            EnforcementRequest(context->pid, EnforcementRequestType::DEFERRED_VERIFICATION, context->step));
    }
    delete context;
}

// v8.0: Timer callback for persistent enforcement
void CALLBACK EngineCore::PersistentEnforceTimerCallback(PVOID lpParameter, BOOLEAN timerOrWaitFired) {
    (void)timerOrWaitFired;
    auto* context = static_cast<DeferredVerifyContext*>(lpParameter);
    if (context && context->engine && !context->engine->stopRequested_.load()) {
        context->engine->EnqueueRequest(
            EnforcementRequest(context->pid, EnforcementRequestType::PERSISTENT_ENFORCE));
    }
    // Note: Do NOT delete context for recurring timer - it's reused
}

// v8.0: Periodic maintenance (piggybacks on wakeups)
void EngineCore::PerformPeriodicMaintenance(ULONGLONG now) {
    // ETW health check (every 30s)
    if (now - lastEtwHealthCheck_ >= ETW_HEALTH_CHECK_INTERVAL) {
        if (operationMode_ == OperationMode::NORMAL && !processMonitor_.IsHealthy()) {
            LOG_ALERT(L"ETW: Session unhealthy - attempting restart");
            processMonitor_.Stop();

            bool restarted = processMonitor_.Start(
                [this](DWORD pid, DWORD parentPid, const std::wstring& imageName) {
                    this->OnProcessStart(pid, parentPid, imageName);
                },
                [this](DWORD threadId, DWORD ownerPid) {
                    this->OnThreadStart(threadId, ownerPid);
                }
            );

            if (restarted) {
                LOG_INFO(L"ETW: Session restarted successfully");
            } else {
                operationMode_ = OperationMode::DEGRADED_ETW;
                LOG_ERROR(L"ETW: Restart failed - switching to DEGRADED mode");
            }
        }
        lastEtwHealthCheck_ = now;
    }

    // Job Object refresh (every 5s)
    if (now - lastJobQueryTime_ >= JOB_QUERY_INTERVAL) {
        RefreshJobObjectPids();
        lastJobQueryTime_ = now;
    }

    // v8.0: DEGRADED_ETW mode fallback scan (every 30s)
    if (operationMode_ == OperationMode::DEGRADED_ETW) {
        if (now - lastDegradedScanTime_ >= DEGRADED_SCAN_INTERVAL) {
            InitialScanForDegradedMode();
            lastDegradedScanTime_ = now;
        }
    }

    // Stats logging (every 60s)
    if (now - lastStatsLogTime_ >= STATS_LOG_INTERVAL) {
        size_t count = GetActiveProcessCount();
        size_t jobCount = 0;
        size_t aggressiveCount = 0, stableCount = 0, persistentCount = 0;
        {
            CSLockGuard lock(trackedCs_);
            for (const auto& [pid, tp] : trackedProcesses_) {
                switch (tp->phase) {
                    case ProcessPhase::AGGRESSIVE: aggressiveCount++; break;
                    case ProcessPhase::STABLE: stableCount++; break;
                    case ProcessPhase::PERSISTENT: persistentCount++; break;
                }
            }
        }
        {
            CSLockGuard lock(jobCs_);
            jobCount = jobObjects_.size();
        }
        if (count > 0) {
            std::wstringstream ss;
            ss << L"Stats: " << count << L" tracked (A:"
               << aggressiveCount << L" S:" << stableCount << L" P:" << persistentCount
               << L"), " << jobCount << L" jobs, "
               << totalViolations_.load() << L" violations";
            LOG_DEBUG(ss.str());

            if (persistentCount > 0) {
                CSLockGuard lock2(trackedCs_);
                std::wstringstream pss;
                pss << L"  PERSISTENT: ";
                bool first = true;
                for (const auto& [p, t] : trackedProcesses_) {
                    if (t->phase == ProcessPhase::PERSISTENT) {
                        if (!first) pss << L", ";
                        pss << t->name << L"(" << p << L")";
                        first = false;
                    }
                }
                LOG_DEBUG(pss.str());
            }
        }
        lastStatsLogTime_ = now;
    }
}

// === v6.0 Enhanced Pulse Enforcement with NtSetInformationProcess ===

bool EngineCore::PulseEnforceV6(HANDLE hProcess, DWORD pid, bool isIntensive) {
    // Multi-layer defense strategy:
    // Layer 1: Registry policy (applied once per executable - handled in ApplyOptimization)
    // Layer 2: NtSetInformationProcess (low-level, more resistant to OS override) - Win11 only
    // Layer 3: SetProcessInformation (fallback / Win10 primary)
    // Layer 4: Priority class enforcement
    // Layer 5: Thread-level throttling (INTENSIVE mode only)

    // Step 1: Exit background mode (unconditional)
    SetPriorityClass(hProcess, PROCESS_MODE_BACKGROUND_END);

    bool ecoQoSSuccess = false;

    // v7.94: Determine control mask based on Windows version
    // IGNORE_TIMER (0x4) is Windows 11 specific - causes errors on Windows 10
    ULONG controlMask = UNLEAF_THROTTLE_EXECUTION_SPEED;
    if (winVersion_.isWindows11OrLater) {
        controlMask |= UNLEAF_THROTTLE_IGNORE_TIMER;
    }

    // Step 2: Try NtSetInformationProcess first (Windows 11+ only)
    // v7.94: Skip NT API on Windows 10 - it fails with error=18
    if (winVersion_.isWindows11OrLater && ntApiAvailable_ && pfnNtSetInformationProcess_) {
        UnleafThrottleState state;
        state.Version = UNLEAF_THROTTLE_VERSION;
        state.ControlMask = controlMask;
        state.StateMask = 0;  // Force OFF

        NTSTATUS status = pfnNtSetInformationProcess_(
            hProcess,
            NT_PROCESS_POWER_THROTTLING_STATE,
            &state,
            sizeof(state)
        );

        if (status == STATUS_SUCCESS) {
            ntApiSuccessCount_.fetch_add(1, std::memory_order_relaxed);
            ecoQoSSuccess = true;
        } else {
            ntApiFailCount_.fetch_add(1, std::memory_order_relaxed);
            // Fall through to SetProcessInformation
        }
    }

    // Step 3: SetProcessInformation (primary path for Win10, fallback for Win11)
    if (!ecoQoSSuccess) {
        UnleafThrottleState state;
        state.Version = UNLEAF_THROTTLE_VERSION;
        state.ControlMask = controlMask;
        state.StateMask = 0;  // Force OFF

        BOOL result = SetProcessInformation(hProcess,
            static_cast<PROCESS_INFORMATION_CLASS>(UNLEAF_PROCESS_POWER_THROTTLING),
            &state, sizeof(state));

        ecoQoSSuccess = (result != FALSE);
    }

    // Step 4: Set HIGH priority (unconditional - critical for OS resistance)
    // Even if EcoQoS control failed, priority helps prevent OS auto-EcoQoS
    SetPriorityClass(hProcess, UNLEAF_TARGET_PRIORITY);

    // Step 5: Thread throttling (INTENSIVE phase only)
    // v7.80: Use consolidated function with aggressive=true
    if (isIntensive) {
        DisableThreadThrottling(pid, true);
    }

    // Error handling
    if (!ecoQoSSuccess) {
        HandleEnforceError(hProcess, pid, GetLastError());
        return false;
    }

    return true;
}

// === v7.0 EcoQoS State Check ===

bool EngineCore::IsEcoQoSEnabled(HANDLE hProcess) const {
    UnleafThrottleState state = {};
    state.Version = UNLEAF_THROTTLE_VERSION;

    // Try NtQueryInformationProcess first (if available)
    if (ntApiAvailable_ && pfnNtQueryInformationProcess_) {
        ULONG returnLength = 0;
        NTSTATUS status = pfnNtQueryInformationProcess_(
            hProcess,
            NT_PROCESS_POWER_THROTTLING_STATE,
            &state,
            sizeof(state),
            &returnLength
        );
        if (status == STATUS_SUCCESS) {
            return (state.StateMask & UNLEAF_THROTTLE_EXECUTION_SPEED) != 0;
        }
    }

    // Fallback to GetProcessInformation
    BOOL result = GetProcessInformation(
        hProcess,
        static_cast<PROCESS_INFORMATION_CLASS>(UNLEAF_PROCESS_POWER_THROTTLING),
        &state,
        sizeof(state)
    );
    if (result) {
        return (state.StateMask & UNLEAF_THROTTLE_EXECUTION_SPEED) != 0;
    }

    return false;  // Unable to determine, assume not enabled
}

// v8.0: Old phase handlers removed - enforcement is now event-driven via DispatchEnforcementRequest

// === v6.0 Registry Policy Application ===

bool EngineCore::ApplyRegistryPolicy(const std::wstring& exePath, const std::wstring& exeName) {
    // Check if already applied
    {
        CSLockGuard lock(policySetCs_);
        std::wstring lowerName = ToLower(exeName);
        if (policyAppliedSet_.find(lowerName) != policyAppliedSet_.end()) {
            return true;  // Already applied
        }
    }

    // Apply full registry exclusion
    bool success = RegistryPolicyManager::Instance().ApplyPolicy(exeName, exePath);

    if (success) {
        CSLockGuard lock(policySetCs_);
        policyAppliedSet_.insert(ToLower(exeName));
        policyApplyCount_.fetch_add(1, std::memory_order_relaxed);

        std::wstringstream ss;
        ss << L"[REGISTRY] Policy applied for: " << exeName;
        LOG_DEBUG(ss.str());
    }

    return success;
}

// === v5.6 Stateless Pulse Enforcement (fallback) ===

bool EngineCore::PulseEnforce(HANDLE hProcess, DWORD pid, bool isIntensive) {
    // Zero-Trust: Never check current state - always force desired state

    // Step 1: Exit background mode (unconditional)
    SetPriorityClass(hProcess, PROCESS_MODE_BACKGROUND_END);

    // Step 2: Force EcoQoS OFF (no Get, only Set)
    UnleafThrottleState state;
    state.Version = UNLEAF_THROTTLE_VERSION;
    state.ControlMask = UNLEAF_THROTTLE_EXECUTION_SPEED | UNLEAF_THROTTLE_IGNORE_TIMER;
    state.StateMask = 0;  // Force OFF

    BOOL ecoResult = SetProcessInformation(hProcess,
        static_cast<PROCESS_INFORMATION_CLASS>(UNLEAF_PROCESS_POWER_THROTTLING),
        &state, sizeof(state));

    // v5.6: Always set priority (never skip even if EcoQoS fails)
    // Step 3: Set HIGH priority (unconditional - critical for OS resistance)
    // Even if SetProcessInformation fails (e.g. Chrome sandbox),
    // setting HIGH_PRIORITY_CLASS prevents OS from reapplying EcoQoS
    SetPriorityClass(hProcess, UNLEAF_TARGET_PRIORITY);

    // Step 4: Thread throttling (INTENSIVE phase only - more expensive operation)
    // v7.80: Use consolidated function with aggressive=false
    if (isIntensive) {
        DisableThreadThrottling(pid, false);
    }

    // Handle errors after priority has been set
    if (!ecoResult) {
        HandleEnforceError(hProcess, pid, GetLastError());
        return false;
    }

    return true;
}

// v7.80: Consolidated thread throttling (merged Optimized + V6)
int EngineCore::DisableThreadThrottling(DWORD pid, bool aggressive) {
    if (pid == 0) return 0;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;

    int threadCount = 0;
    THREADENTRY32 te32;
    te32.dwSize = sizeof(THREADENTRY32);

    // Stack-allocated throttle state (no dynamic allocation)
    UnleafThreadThrottleState threadState;
    threadState.Version = UNLEAF_THREAD_THROTTLE_VERSION;
    threadState.ControlMask = UNLEAF_THREAD_THROTTLE_EXECUTION_SPEED;
    threadState.StateMask = 0;  // Disable throttling

    if (Thread32First(snapshot, &te32)) {
        do {
            if (te32.th32OwnerProcessID == pid) {
                HANDLE hThread = OpenThread(
                    THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION,
                    FALSE, te32.th32ThreadID);

                if (hThread) {
                    // Disable thread-level power throttling
                    SetThreadInformation(hThread,
                        static_cast<THREAD_INFORMATION_CLASS>(UNLEAF_THREAD_POWER_THROTTLING),
                        &threadState, sizeof(threadState));

                    // Priority boost logic
                    int currentPriority = GetThreadPriority(hThread);
                    if (currentPriority != THREAD_PRIORITY_ERROR_RETURN) {
                        if (aggressive) {
                            // v6.0 style: boost any thread below ABOVE_NORMAL
                            if (currentPriority < THREAD_PRIORITY_ABOVE_NORMAL) {
                                SetThreadPriority(hThread, THREAD_PRIORITY_ABOVE_NORMAL);
                            }
                        } else {
                            // Conservative: only boost very low priority threads
                            if (currentPriority == THREAD_PRIORITY_IDLE ||
                                currentPriority == THREAD_PRIORITY_LOWEST ||
                                currentPriority == THREAD_PRIORITY_BELOW_NORMAL) {
                                SetThreadPriority(hThread, THREAD_PRIORITY_ABOVE_NORMAL);
                            }
                        }
                    }

                    CloseHandle(hThread);
                    threadCount++;
                }
            }
        } while (Thread32Next(snapshot, &te32));
    }

    CloseHandle(snapshot);
    return threadCount;
}

void EngineCore::UpdateEnforceState(DWORD pid, ULONGLONG now, bool success) {
    CSLockGuard lock(trackedCs_);
    auto it = trackedProcesses_.find(pid);
    if (it == trackedProcesses_.end()) return;

    auto& tp = it->second;
    tp->lastCheckTime = now;

    if (success) {
        tp->consecutiveFailures = 0;
        tp->nextRetryTime = 0;
        // v7.0: Phase transition handled in ProcessPhaseEnforcement
    }
}

void EngineCore::SetProcessPhase(DWORD pid, ProcessPhase phase) {
    CSLockGuard lock(trackedCs_);
    auto it = trackedProcesses_.find(pid);
    if (it != trackedProcesses_.end()) {
        it->second->phase = phase;
        it->second->phaseStartTime = GetTickCount64();
    }
}

// === v5.0 Self-Healing Error Handling ===

void EngineCore::HandleEnforceError(HANDLE hProcess, DWORD pid, DWORD error) {
    totalRetries_.fetch_add(1, std::memory_order_relaxed);

    // v7.4: Check if process is still alive before retrying
    DWORD exitCode = 0;
    bool processAlive = (hProcess && GetExitCodeProcess(hProcess, &exitCode) && exitCode == STILL_ACTIVE);

    CSLockGuard lock(trackedCs_);
    auto it = trackedProcesses_.find(pid);
    if (it == trackedProcesses_.end()) return;

    auto& tp = it->second;
    tp->consecutiveFailures++;
    tp->lastErrorCode = error;

    // v7.4: If process has exited, cleanup immediately (no retry)
    if (!processAlive) {
        std::wstringstream ss;
        ss << L"[CLEANUP] " << tp->name << L" (PID:" << pid
           << L") process exited (exitCode=" << exitCode << L")";
        LOG_DEBUG(ss.str());
        tp->processHandle.reset();
        return;
    }

    switch (error) {
        case ERROR_ACCESS_DENIED:
            // v7.4: Max 2 retries, then give up
            if (tp->consecutiveFailures <= 2) {
                tp->nextRetryTime = GetTickCount64() + RETRY_BACKOFF_BASE_MS;
            } else {
                std::wstringstream ss;
                ss << L"[GIVE_UP] " << tp->name << L" (PID:" << pid
                   << L") access denied - giving up after 2 retries";
                LOG_DEBUG(ss.str());
            }
            break;

        case ERROR_INVALID_HANDLE:
            tp->processHandle.reset();
            break;

        case ERROR_INVALID_PARAMETER:  // 87 - PID is invalid (process already exited)
            // v7.2: No retry needed - process is dead, Safety Net will re-detect if needed
            tp->processHandle.reset();
            break;

        default:
            // v7.4: Exponential backoff with max retries and give-up logging
            if (tp->consecutiveFailures <= MAX_RETRY_COUNT) {
                DWORD backoff = RETRY_BACKOFF_BASE_MS * (1 << (tp->consecutiveFailures - 1));
                tp->nextRetryTime = GetTickCount64() + backoff;
            } else {
                std::wstringstream ss;
                ss << L"[GIVE_UP] " << tp->name << L" (PID:" << pid
                   << L") error=" << error << L" - max retries reached";
                LOG_DEBUG(ss.str());
            }
            break;
    }
}

bool EngineCore::ReopenProcessHandle(DWORD pid) {
    totalHandleReopen_.fetch_add(1, std::memory_order_relaxed);

    // v7.0: Use minimal permissions matching Python v1.00 (0x1200)
    HANDLE hProcess = OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_INFORMATION,
        FALSE, pid);

    if (!hProcess) return false;

    CSLockGuard lock(trackedCs_);
    auto it = trackedProcesses_.find(pid);
    if (it == trackedProcesses_.end()) {
        CloseHandle(hProcess);
        return false;
    }

    auto& tp = it->second;

    // Unregister old wait
    if (tp->waitHandle) {
        UnregisterWait(tp->waitHandle);
        tp->waitHandle = nullptr;
    }
    tp->waitProcessHandle.reset();

    // Replace handle
    tp->processHandle = MakeScopedHandle(hProcess);
    tp->consecutiveFailures = 0;
    tp->nextRetryTime = 0;

    // v7.0: Re-register wait callback with separate SYNCHRONIZE handle
    // v7.80: Track context in waitContexts_ for safe cleanup
    HANDLE hWaitProcess = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (hWaitProcess) {
        auto context = new WaitCallbackContext{this, pid};
        HANDLE waitHandle = nullptr;

        if (RegisterWaitForSingleObject(
                &waitHandle,
                hWaitProcess,
                OnProcessExit,
                context,
                INFINITE,
                WT_EXECUTEONLYONCE)) {
            tp->waitHandle = waitHandle;
            tp->waitProcessHandle = MakeScopedHandle(hWaitProcess);
            // v7.80: Track context (still under trackedCs_ lock)
            waitContexts_[pid] = context;
        } else {
            delete context;
            CloseHandle(hWaitProcess);
        }
    }

    LOG_DEBUG(L"Engine: Reopened handle for PID " + std::to_wstring(pid));
    return true;
}

// === v5.0 Job Object Management ===

bool EngineCore::CreateAndAssignJobObject(DWORD rootPid, HANDLE hProcess) {
    // Check if already in a Job (Chrome sandbox case)
    BOOL inJob = FALSE;
    IsProcessInJob(hProcess, nullptr, &inJob);
    if (inJob) {
        LOG_DEBUG(L"Job: PID " + std::to_wstring(rootPid) + L" already in Job - pulse-only mode");
        return false;  // TrackedProcess still created to continue PulseEnforce
    }

    // Create Job Object
    HANDLE hJob = CreateJobObjectW(nullptr, nullptr);
    if (!hJob) {
        DWORD error = GetLastError();
        std::wstringstream ss;
        ss << L"[JOB] Failed to create Job Object (error=" << error << L")";
        LOG_DEBUG(ss.str());
        return false;
    }

    // Configure: allow breakaway for nested processes
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobInfo = {};
    jobInfo.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_BREAKAWAY_OK | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK;
    SetInformationJobObject(hJob, JobObjectExtendedLimitInformation,
        &jobInfo, sizeof(jobInfo));

    // Assign process to Job
    if (!AssignProcessToJobObject(hJob, hProcess)) {
        DWORD error = GetLastError();
        CloseHandle(hJob);
        std::wstringstream ss;
        ss << L"[JOB] Failed to assign PID " << rootPid << L" to Job (error=" << error << L")";
        LOG_DEBUG(ss.str());
        return false;
    }

    // Store Job Object
    auto info = std::make_unique<JobObjectInfo>();
    info->jobHandle = hJob;
    info->rootPid = rootPid;
    info->isOwnJob = true;

    {
        CSLockGuard lock(jobCs_);
        jobObjects_[rootPid] = std::move(info);
    }

    LOG_DEBUG(L"Job: Created Job Object for root PID " + std::to_wstring(rootPid));
    return true;
}

// v7.80: Optimized 2-pass approach to minimize lock contention
void EngineCore::RefreshJobObjectPids() {
    // Pass 1: Collect job info and PIDs under lock
    struct JobQueryResult {
        DWORD rootPid;
        std::vector<DWORD> newPids;
    };
    std::vector<JobQueryResult> jobResults;

    {
        CSLockGuard jobLock(jobCs_);
        CSLockGuard trackedLock(trackedCs_);

        for (auto& [rootPid, jobInfo] : jobObjects_) {
            if (!jobInfo->isOwnJob || !jobInfo->jobHandle) continue;

            // Fixed-size stack buffer for PID list (no dynamic allocation)
            BYTE buffer[sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST) +
                        (MAX_JOB_PIDS - 1) * sizeof(ULONG_PTR)];
            auto* pidList = reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST*>(buffer);
            pidList->NumberOfAssignedProcesses = MAX_JOB_PIDS;
            pidList->NumberOfProcessIdsInList = 0;

            if (!QueryInformationJobObject(jobInfo->jobHandle,
                    JobObjectBasicProcessIdList, pidList, sizeof(buffer), nullptr)) {
                continue;
            }

            JobQueryResult result;
            result.rootPid = rootPid;

            // Collect only new (untracked) PIDs
            for (DWORD i = 0; i < pidList->NumberOfProcessIdsInList; i++) {
                DWORD pid = static_cast<DWORD>(pidList->ProcessIdList[i]);
                if (trackedProcesses_.find(pid) == trackedProcesses_.end()) {
                    result.newPids.push_back(pid);
                }
            }

            if (!result.newPids.empty()) {
                jobResults.push_back(std::move(result));
            }
        }
    }

    // Pass 2: Process new PIDs without holding locks (expensive operations)
    for (const auto& result : jobResults) {
        for (DWORD pid : result.newPids) {
            // Double-check not tracked (may have been added between passes)
            if (IsTracked(pid)) continue;

            // Get process name (no lock needed)
            HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (!hProcess) continue;

            wchar_t nameBuffer[MAX_PATH];
            DWORD nameSize = MAX_PATH;
            std::wstring name;

            if (QueryFullProcessImageNameW(hProcess, 0, nameBuffer, &nameSize)) {
                // Extract filename from path
                std::wstring fullPath(nameBuffer);
                size_t pos = fullPath.find_last_of(L"\\/");
                name = (pos != std::wstring::npos) ? fullPath.substr(pos + 1) : fullPath;
            }
            CloseHandle(hProcess);

            if (name.empty()) continue;

            // Skip critical processes
            if (!IsCriticalProcess(name)) {
                ApplyOptimization(pid, name, true, result.rootPid);
            }
        }
    }
}

void EngineCore::CleanupJobObjects() {
    CSLockGuard lock(jobCs_);
    jobObjects_.clear();  // JobObjectInfo destructor handles CloseHandle
    LOG_DEBUG(L"Job: Cleaned up all Job Objects");
}

void EngineCore::UpdatePhase(DWORD pid, ULONGLONG now) {
    CSLockGuard lock(trackedCs_);
    auto it = trackedProcesses_.find(pid);
    if (it == trackedProcesses_.end()) return;

    auto& tp = it->second;
    tp->lastCheckTime = now;
    // v7.0: Phase transition handled in ProcessPhaseEnforcement
}

// v8.0: ConfigWatcherLoop removed - config changes detected via FindFirstChangeNotification

// === Process Management ===

// v8.0: QuickRescan removed - replaced by event-driven ETW and Safety Net
// InitialScanForDegradedMode is used only in DEGRADED_ETW mode as fallback

void EngineCore::InitialScanForDegradedMode() {
    // DEGRADED_ETW mode fallback: scan all processes
    // This is only called when ETW is unavailable
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;

    ScopedSnapshot scopedSnapshot = MakeScopedSnapshot(snapshot);

    std::set<std::wstring> localTargets;
    {
        CSLockGuard lock(targetCs_);
        localTargets = targetSet_;
    }

    PROCESSENTRY32W pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32W);

    if (Process32FirstW(scopedSnapshot.get(), &pe32)) {
        do {
            DWORD pid = pe32.th32ProcessID;
            const std::wstring& name = pe32.szExeFile;
            DWORD parentPid = pe32.th32ParentProcessID;

            if (IsTracked(pid)) continue;
            if (IsCriticalProcess(name)) continue;

            std::wstring lowerName = ToLower(name);
            bool isTarget = (localTargets.count(lowerName) > 0);
            bool isChild = IsTrackedParent(parentPid);

            if (isTarget || isChild) {
                ApplyOptimization(pid, name, isChild, parentPid);
            }
        } while (Process32NextW(scopedSnapshot.get(), &pe32));
    }
}

void EngineCore::InitialScan() {
    // Scan for existing target processes at startup
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;

    ScopedSnapshot scopedSnapshot = MakeScopedSnapshot(snapshot);

    // Build process map
    struct ProcessInfo {
        std::wstring name;
        DWORD parentPid;
    };
    std::map<DWORD, ProcessInfo> processMap;

    PROCESSENTRY32W pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32W);

    if (Process32FirstW(scopedSnapshot.get(), &pe32)) {
        do {
            processMap[pe32.th32ProcessID] = { pe32.szExeFile, pe32.th32ParentProcessID };
        } while (Process32NextW(scopedSnapshot.get(), &pe32));
    }

    // Collect descendants recursively
    std::function<void(DWORD, std::vector<std::pair<DWORD, std::wstring>>&)> collectDescendants;
    collectDescendants = [&](DWORD parentPid, std::vector<std::pair<DWORD, std::wstring>>& out) {
        for (const auto& [pid, info] : processMap) {
            if (info.parentPid == parentPid && !IsCriticalProcess(info.name)) {
                out.emplace_back(pid, info.name);
                collectDescendants(pid, out);
            }
        }
    };

    // Find and optimize target processes
    std::set<std::wstring> localTargets;
    {
        CSLockGuard lock(targetCs_);
        localTargets = targetSet_;
    }

    for (const auto& [pid, info] : processMap) {
        std::wstring lowerName = ToLower(info.name);

        if (localTargets.find(lowerName) != localTargets.end()) {
            if (!IsCriticalProcess(info.name) && !IsTracked(pid)) {
                ApplyOptimization(pid, info.name, false, 0);
            }

            // Collect and optimize descendants
            std::vector<std::pair<DWORD, std::wstring>> descendants;
            collectDescendants(pid, descendants);

            for (const auto& [childPid, childName] : descendants) {
                if (!IsTracked(childPid)) {
                    ApplyOptimization(childPid, childName, true, pid);
                }
            }
        }
    }
}

bool EngineCore::ApplyOptimization(DWORD pid, const std::wstring& name, bool isChild, DWORD parentPid) {
    // Skip if already tracked
    if (IsTracked(pid)) {
        return false;
    }

    // Skip critical processes
    if (IsCriticalProcess(name)) {
        return false;
    }

    // v7.0: Use minimal permissions matching Python v1.00 (0x1200)
    // This allows access to Chrome sandbox processes that reject SYNCHRONIZE
    DWORD access = PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_INFORMATION;
    HANDLE hProcess = OpenProcess(access, FALSE, pid);
    if (!hProcess) {
        // Log failure for observability
        DWORD error = GetLastError();
        std::wstringstream ss;
        ss << L"[SKIP] " << name << L" (PID:" << pid
           << L") OpenProcess failed (error=" << error << L")";
        LOG_DEBUG(ss.str());
        return false;
    }

    ScopedHandle scopedHandle = MakeScopedHandle(hProcess);
    ULONGLONG now = GetTickCount64();

    // v6.0: Apply registry policy for root target processes (once per executable)
    if (!isChild) {
        // Get full path for registry policy
        wchar_t pathBuffer[MAX_PATH];
        DWORD pathSize = MAX_PATH;
        if (QueryFullProcessImageNameW(scopedHandle.get(), 0, pathBuffer, &pathSize)) {
            std::wstring fullPath(pathBuffer);
            ApplyRegistryPolicy(fullPath, name);
        }
    }

    // v6.0: Use PulseEnforceV6 with NtSetInformationProcess
    // v7.0: Initial enforcement in AGGRESSIVE phase
    bool success = PulseEnforceV6(scopedHandle.get(), pid, true);

    // Log the optimization
    std::wstring prefix = isChild ? L"[CHILD]" : L"[TARGET]";
    std::wstringstream ss;
    ss << L"Optimized: " << prefix << L" " << name << L" (PID: " << pid << L") Child=" << isChild;
    LOG_DEBUG(ss.str());

    // v5.0: Job Object assignment for root target processes
    bool inJob = false;
    bool jobFailed = false;
    DWORD rootPid = isChild ? parentPid : pid;

    if (!isChild) {
        // Root process - try to create and assign to Job Object
        if (CreateAndAssignJobObject(pid, scopedHandle.get())) {
            inJob = true;
        } else {
            jobFailed = true;  // Chrome sandbox case or failure
        }
    } else {
        // Child process - check if parent is in our Job
        CSLockGuard lock(jobCs_);
        auto jobIt = jobObjects_.find(parentPid);
        if (jobIt != jobObjects_.end() && jobIt->second->isOwnJob) {
            inJob = true;  // Already in parent's Job
        }
    }

    // Create tracked process entry
    auto tracked = std::make_unique<TrackedProcess>();
    tracked->pid = pid;
    tracked->parentPid = parentPid;
    tracked->name = name;
    tracked->processHandle = std::move(scopedHandle);
    tracked->isChild = isChild;
    tracked->phase = ProcessPhase::AGGRESSIVE;  // v7.0: Start in AGGRESSIVE phase
    tracked->phaseStartTime = now;
    tracked->lastCheckTime = now;
    tracked->lastPriorityCheck = now;
    tracked->violationCount = 0;
    tracked->waitHandle = nullptr;

    // v5.0: Self-healing fields
    tracked->consecutiveFailures = 0;
    tracked->lastErrorCode = 0;
    tracked->nextRetryTime = 0;

    // v5.0: Job Object tracking
    tracked->rootTargetPid = rootPid;
    tracked->inJobObject = inJob;
    tracked->jobAssignmentFailed = jobFailed;

    // v7.0: Register wait for process exit using separate SYNCHRONIZE handle
    // Main handle (0x1200) doesn't have SYNCHRONIZE, so we open another handle
    // v7.80: Track context in waitContexts_ for safe cleanup
    HANDLE hWaitProcess = OpenProcess(SYNCHRONIZE, FALSE, pid);
    WaitCallbackContext* context = nullptr;
    if (hWaitProcess) {
        context = new WaitCallbackContext{this, pid};
        HANDLE waitHandle = nullptr;

        if (RegisterWaitForSingleObject(
                &waitHandle,
                hWaitProcess,
                OnProcessExit,
                context,
                INFINITE,
                WT_EXECUTEONLYONCE)) {
            tracked->waitHandle = waitHandle;
            tracked->waitProcessHandle = MakeScopedHandle(hWaitProcess);
        } else {
            delete context;
            context = nullptr;
            CloseHandle(hWaitProcess);
        }
    }
    // If SYNCHRONIZE fails, process exit will be detected by handle invalidation

    // Add to tracked map
    {
        CSLockGuard lock(trackedCs_);
        trackedProcesses_[pid] = std::move(tracked);
        // v7.80: Track context for safe cleanup
        if (context) {
            waitContexts_[pid] = context;
        }
    }

    // v8.0: Schedule deferred verification (replaces polling-based AGGRESSIVE phase)
    ScheduleDeferredVerification(pid, 1);

    return success;
}

// v5.0: ForceOptimize and DisableThreadThrottling removed
// Replaced by PulseEnforce and DisableThreadThrottlingOptimized

// v7.80: OnProcessExit no longer deletes context - RemoveTrackedProcess handles it
void CALLBACK EngineCore::OnProcessExit(PVOID lpParameter, BOOLEAN timerOrWaitFired) {
    (void)timerOrWaitFired;

    auto context = static_cast<WaitCallbackContext*>(lpParameter);
    if (context && context->engine) {
        // v7.80: Just trigger removal; context cleanup is handled by RemoveTrackedProcess
        // The context will be deleted after UnregisterWaitEx completes
        context->engine->RemoveTrackedProcess(context->pid);
    }
    // v7.80: Do NOT delete context here - it's managed by waitContexts_
}

// v7.80: Safely remove tracked process with proper wait handle cleanup
void EngineCore::RemoveTrackedProcess(DWORD pid) {
    HANDLE waitHandleToUnregister = nullptr;
    WaitCallbackContext* contextToDelete = nullptr;

    {
        CSLockGuard lock(trackedCs_);

        auto it = trackedProcesses_.find(pid);
        if (it != trackedProcesses_.end()) {
            waitHandleToUnregister = it->second->waitHandle;
            it->second->waitHandle = nullptr;
            trackedProcesses_.erase(it);
        }

        // v7.80: Get context for deletion
        auto ctxIt = waitContexts_.find(pid);
        if (ctxIt != waitContexts_.end()) {
            contextToDelete = ctxIt->second;
            waitContexts_.erase(ctxIt);
        }
    }

    // v7.80: Unregister outside lock with INVALID_HANDLE_VALUE to wait for callback completion
    if (waitHandleToUnregister) {
        UnregisterWaitEx(waitHandleToUnregister, INVALID_HANDLE_VALUE);
    }

    // v7.80: Safe to delete context after UnregisterWaitEx completes
    delete contextToDelete;
}

// === State Checks (v5.0: Get functions removed - using Set-only Zero-Trust model) ===
// CheckEcoQoSState, IsEcoQoSEnabled, NeedsPriorityBoost, IsInEfficiencyMode removed
// Replaced by unconditional PulseEnforce

bool EngineCore::IsTracked(DWORD pid) const {
    CSLockGuard lock(trackedCs_);
    return trackedProcesses_.find(pid) != trackedProcesses_.end();
}

bool EngineCore::IsTargetName(const std::wstring& name) const {
    CSLockGuard lock(targetCs_);
    return targetSet_.find(ToLower(name)) != targetSet_.end();
}
/*
bool EngineCore::IsTrackedParent(DWORD parentPid) const {
    CSLockGuard lock(trackedCs_);
    auto it = trackedProcesses_.find(parentPid);
    if (it != trackedProcesses_.end()) {
        // Only consider non-child processes as valid parents
        return !it->second->isChild;
    }
    return false;
}
*/

bool EngineCore::IsTrackedParent(DWORD parentPid) const {
    CSLockGuard lock(trackedCs_);
    auto it = trackedProcesses_.find(parentPid);
    return it != trackedProcesses_.end();
}

void EngineCore::RefreshTargetSet() {
    CSLockGuard lock(targetCs_);
    targetSet_.clear();

    const auto& targets = UnLeafConfig::Instance().GetTargets();
    for (const auto& target : targets) {
        if (target.enabled) {
            targetSet_.insert(ToLower(target.name));
        }
    }
}

void EngineCore::CleanupRemovedTargets() {
    // Get current valid targets
    std::set<std::wstring> localTargets;
    {
        CSLockGuard lock(targetCs_);
        localTargets = targetSet_;
    }

    // Collect PIDs of root targets that are no longer in the target list
    std::vector<DWORD> toRemove;
    std::set<DWORD> validRootPids;

    {
        CSLockGuard lock(trackedCs_);

        // First pass: identify valid root processes
        for (const auto& [pid, tp] : trackedProcesses_) {
            if (!tp->isChild) {
                std::wstring lowerName = ToLower(tp->name);
                if (localTargets.find(lowerName) != localTargets.end()) {
                    validRootPids.insert(pid);
                }
            }
        }

        // Second pass: collect processes to remove
        for (const auto& [pid, tp] : trackedProcesses_) {
            bool shouldRemove = false;

            if (tp->isChild) {
                // Parent still alive -> do not remove
                bool parentAlive = trackedProcesses_.find(tp->parentPid) != trackedProcesses_.end();
                // Process itself is a target name -> never remove
                bool selfIsTarget = IsTargetName(tp->name);

                if (!parentAlive && !selfIsTarget) {
                    shouldRemove = true;
                }
            } else {
                // Root process: remove if not in target list
                std::wstring lowerName = ToLower(tp->name);
                if (localTargets.find(lowerName) == localTargets.end()) {
                    shouldRemove = true;
                }
            }

            if (shouldRemove) {
                toRemove.push_back(pid);
            }
        }
    }

    // Remove collected processes
    if (!toRemove.empty()) {
        CSLockGuard lock(trackedCs_);
        for (DWORD pid : toRemove) {
            auto it = trackedProcesses_.find(pid);
            if (it != trackedProcesses_.end()) {
                if (it->second->waitHandle) {
                    UnregisterWait(it->second->waitHandle);
                    it->second->waitHandle = nullptr;
                }
                trackedProcesses_.erase(it);
            }
        }

        std::wstringstream ss;
        ss << L"Removed " << toRemove.size() << L" tracked processes (targets changed)";
        LOG_DEBUG(ss.str());
    }
}

size_t EngineCore::GetActiveProcessCount() const {
    CSLockGuard lock(trackedCs_);
    return trackedProcesses_.size();
}

// v7.7: Health check info
HealthInfo EngineCore::GetHealthInfo() const {
    HealthInfo info;
    info.engineRunning = running_.load();
    info.mode = operationMode_;
    info.activeProcesses = GetActiveProcessCount();
    info.totalViolations = totalViolations_.load();
    info.etwHealthy = processMonitor_.IsHealthy();
    info.etwEventCount = processMonitor_.GetEventCount();
    info.uptimeMs = (startTime_ > 0) ? (GetTickCount64() - startTime_) : 0;
    return info;
}

} // namespace unleaf
