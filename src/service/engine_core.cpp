// UnLeaf - Engine Core Implementation (Event-Driven Architecture)

#include "engine_core.h"
#include <algorithm>
#include <functional>
#include <cstdio>

namespace unleaf {

// Context structure for wait callback
struct WaitCallbackContext {
    EngineCore* engine;
    DWORD pid;
    std::shared_ptr<TrackedProcess> process;  // prevent premature destruction
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
    , hWakeupEvent_(nullptr)
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
    , startTime_(0)
    , lastConfigCheckTime_(0)
    , configChangePending_(false) {
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
        CleanupHandles();
        return false;
    }

    // Initialize logger
    if (!LightweightLogger::Instance().Initialize(baseDir)) {
        LOG_ERROR(L"Engine: Failed to initialize logger");
        CleanupHandles();
        return false;
    }

    // Apply log level from config
    LightweightLogger::Instance().SetLogLevel(UnLeafConfig::Instance().GetLogLevel());
    LightweightLogger::Instance().SetEnabled(UnLeafConfig::Instance().IsLogEnabled());

    // Load initial targets
    RefreshTargetSet();

    // Create Timer Queue for deferred verification and persistent enforcement timers
    timerQueue_ = CreateTimerQueue();
    if (!timerQueue_) {
        LOG_ERROR(L"Engine: Failed to create Timer Queue");
        CleanupHandles();
        return false;
    }

    // Create config change notification (event-driven)
    std::wstring configDir = baseDir_;
    configChangeHandle_ = FindFirstChangeNotificationW(
        configDir.c_str(),
        FALSE,  // Do not watch subtree
        FILE_NOTIFY_CHANGE_LAST_WRITE
    );
    if (configChangeHandle_ == INVALID_HANDLE_VALUE) {
        LOG_ALERT(L"Engine: FindFirstChangeNotification failed - config changes require restart");
    }

    // Create Safety Net waitable timer (10s periodic)
    // SAFETY NET: This is an insurance consistency check, NOT monitoring
    safetyNetTimer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    if (!safetyNetTimer_) {
        LOG_ERROR(L"Engine: Failed to create Safety Net timer");
        CleanupHandles();
        return false;
    }

    // Create enforcement request event (auto-reset)
    enforcementRequestEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!enforcementRequestEvent_) {
        LOG_ERROR(L"Engine: Failed to create enforcement request event");
        CleanupHandles();
        return false;
    }

    // Create wakeup event for process exit notifications (auto-reset)
    hWakeupEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!hWakeupEvent_) {
        LOG_ERROR(L"Engine: Failed to create wakeup event");
        CleanupHandles();
        return false;
    }

    // Load NT API functions from ntdll.dll
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

    // Detect Windows version for compatibility
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
        wchar_t verBuf[128];
        swprintf_s(verBuf, L"Engine: Windows %lu.%lu (Build %lu) - %s",
                   winVersion_.major, winVersion_.minor, winVersion_.build,
                   winVersion_.isWindows11OrLater ? L"Full EcoQoS support" : L"Limited EcoQoS (Win10 compatibility mode)");
        LOG_INFO(verBuf);
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

    // Start ETW with both process and thread callbacks
    bool etwStarted = processMonitor_.Start(
        [this](DWORD pid, DWORD parentPid, const std::wstring& imageName) {
            this->OnProcessStart(pid, parentPid, imageName);
        },
        [this](DWORD threadId, DWORD ownerPid) {
            this->OnThreadStart(threadId, ownerPid);
        }
    );

    // Set operation mode based on ETW status
    if (etwStarted) {
        operationMode_ = OperationMode::NORMAL;
    } else {
        operationMode_ = OperationMode::DEGRADED_ETW;
        LOG_ALERT(L"ETW: Monitor failed to start - using DEGRADED mode");
    }

    InitialScan();

    // Set up Safety Net waitable timer (10s periodic)
    // SAFETY NET: Insurance consistency check - NOT monitoring
    LARGE_INTEGER dueTime;
    dueTime.QuadPart = -static_cast<LONGLONG>(SAFETY_NET_INTERVAL) * 10000LL;  // Relative time in 100ns units
    if (!SetWaitableTimer(safetyNetTimer_, &dueTime, static_cast<LONG>(SAFETY_NET_INTERVAL), nullptr, nullptr, FALSE)) {
        LOG_ERROR(L"Engine: Failed to set Safety Net timer");
    }

    // Start single EngineControlThread
    engineControlThread_ = std::thread(&EngineCore::EngineControlLoop, this);

    wchar_t startBuf[128];
    swprintf_s(startBuf, L"EngineCore started: %zu targets, %s mode, Event-Driven, SafetyNet=10s",
               targetSet_.size(),
               (operationMode_ == OperationMode::NORMAL ? L"NORMAL" : L"DEGRADED_ETW"));
    LOG_DEBUG(startBuf);
}

void EngineCore::Stop() {
    // Atomically claim the right to stop (prevents concurrent Stop() calls)
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) return;

    stopRequested_ = true;
    SetEvent(stopEvent_);
    LOG_DEBUG(L"[STOP] Step 1: Stop signal sent");

    // Stop ETW monitor
    processMonitor_.Stop();
    LOG_DEBUG(L"[STOP] Step 2: ETW monitor stopped");

    // Wait for single control thread
    if (engineControlThread_.joinable()) {
        engineControlThread_.join();
    }
    LOG_DEBUG(L"[STOP] Step 3: Control thread joined");

    // Collect timer contexts before destroying timer queue
    std::vector<DeferredVerifyContext*> timerContextsToDelete;
    size_t deferredCount = 0, persistentCount = 0;
    {
        CSLockGuard lock(trackedCs_);
        for (auto& [pid, tp] : trackedProcesses_) {
            if (tp->persistentTimerContext) {
                timerContextsToDelete.push_back(tp->persistentTimerContext);
                tp->persistentTimerContext = nullptr;
                persistentCount++;
            }
            if (tp->deferredTimerContext) {
                timerContextsToDelete.push_back(tp->deferredTimerContext);
                tp->deferredTimerContext = nullptr;
                deferredCount++;
            }
            tp->persistentTimer = nullptr;
            tp->deferredTimer = nullptr;
        }
    }
    {
        wchar_t stepBuf[128];
        swprintf_s(stepBuf, L"[STOP] Step 4: Timer contexts collected (%zu deferred, %zu persistent)",
                   deferredCount, persistentCount);
        LOG_DEBUG(stepBuf);
    }

    // Delete Timer Queue (waits for all timer callbacks to complete)
    if (timerQueue_) {
        if (!DeleteTimerQueueEx(timerQueue_, INVALID_HANDLE_VALUE)) {
            wchar_t alertBuf[96];
            swprintf_s(alertBuf, L"[STOP] DeleteTimerQueueEx failed (error=%lu)", GetLastError());
            LOG_ALERT(alertBuf);
            shutdownWarnings_.fetch_add(1);
        }
        timerQueue_ = nullptr;
    }
    LOG_DEBUG(L"[STOP] Step 5: Timer Queue deleted");

    // Free persistent timer contexts after timer queue is destroyed
    for (auto* ctx : timerContextsToDelete) {
        delete ctx;
    }

    // Cleanup tracked processes with safe wait handle unregistration
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
            }
            for (auto& [pid, ctx] : waitContexts_) {
                contextsToDelete.push_back(ctx);
            }
            waitContexts_.clear();
            trackedProcesses_.clear();
        }

        for (HANDLE h : waitHandles) {
            if (!UnregisterWaitEx(h, INVALID_HANDLE_VALUE)) {
                shutdownWarnings_.fetch_add(1);
            }
        }

        for (auto* ctx : contextsToDelete) {
            delete ctx;
        }

        {
            wchar_t stepBuf[96];
            swprintf_s(stepBuf, L"[STOP] Step 6: Wait handles unregistered (%zu handles)",
                       waitHandles.size());
            LOG_DEBUG(stepBuf);
        }
    }

    // Cleanup Job Objects
    CleanupJobObjects();
    LOG_DEBUG(L"[STOP] Step 7: Job Objects cleaned up");

    // Cleanup all registry policies (both PowerThrottling + IFEO)
    RegistryPolicyManager::Instance().CleanupAllPolicies();
    {
        CSLockGuard lock(policySetCs_);
        policyAppliedSet_.clear();
    }
    LOG_DEBUG(L"[STOP] Step 8: Registry policies cleaned up");

    CleanupHandles();
    LOG_DEBUG(L"[STOP] Step 9: Event handles closed");

    // running_ already set to false by compare_exchange_strong at entry

    {
        wchar_t stopBuf[256];
        swprintf_s(stopBuf, L"[STOP] Complete: ShutdownWarnings=%u",
                   shutdownWarnings_.load());
        LOG_DEBUG(stopBuf);
    }

    wchar_t stopBuf[256];
    swprintf_s(stopBuf, L"EngineCore stopped. Violations=%u Retries=%u HandleReopen=%u NtApiSuccess=%u NtApiFail=%u PolicyApply=%u",
               totalViolations_.load(), totalRetries_.load(), totalHandleReopen_.load(),
               ntApiSuccessCount_.load(), ntApiFailCount_.load(), policyApplyCount_.load());
    LOG_DEBUG(stopBuf);
}

void EngineCore::CleanupHandles() {
    if (stopEvent_) {
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
    }
    if (timerQueue_) {
        DeleteTimerQueueEx(timerQueue_, nullptr);
        timerQueue_ = nullptr;
    }
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
    if (hWakeupEvent_) {
        CloseHandle(hWakeupEvent_);
        hWakeupEvent_ = nullptr;
    }
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

// ETW callback for thread creation events
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

    // Queue check for STABLE and PERSISTENT phase processes
    // AGGRESSIVE already has active deferred verification
    // PERSISTENT has 5s timer but ETW boost provides instant response on tab switch
    if (currentPhase == ProcessPhase::STABLE || currentPhase == ProcessPhase::PERSISTENT) {
        EnqueueRequest(EnforcementRequest(ownerPid, EnforcementRequestType::ETW_THREAD_START));
    }
}

// === Event-Driven Engine Control Loop ===

void EngineCore::EngineControlLoop() {
    LOG_INFO(L"Engine: Event-driven control loop started (SafetyNet=10s, Event-triggered)");

    // Build wait handle array
    HANDLE waitHandles[WAIT_COUNT];
    waitHandles[WAIT_STOP] = stopEvent_;
    waitHandles[WAIT_CONFIG_CHANGE] = (configChangeHandle_ != INVALID_HANDLE_VALUE) ? configChangeHandle_ : stopEvent_;
    waitHandles[WAIT_SAFETY_NET] = safetyNetTimer_;
    waitHandles[WAIT_ENFORCEMENT_REQUEST] = enforcementRequestEvent_;
    waitHandles[WAIT_PROCESS_EXIT] = hWakeupEvent_;

    while (!stopRequested_.load()) {
        DWORD waitResult = WaitForMultipleObjects(WAIT_COUNT, waitHandles, FALSE, INFINITE);

        if (waitResult == WAIT_OBJECT_0 + WAIT_STOP)
            break;

        if (waitResult == WAIT_FAILED) {
            wchar_t errBuf[96];
            swprintf_s(errBuf, L"Engine: WaitForMultipleObjects failed (error=%lu)", GetLastError());
            LOG_ERROR(errBuf);
            break;
        }

        ULONGLONG now = GetTickCount64();

        switch (waitResult) {
            case WAIT_OBJECT_0 + WAIT_CONFIG_CHANGE:
                wakeupConfigChange_.fetch_add(1);
                configChangeDetected_.fetch_add(1);
                // Debounce: notification fires for ALL file changes in directory
                // (including log writes). Defer actual check to reduce unnecessary file stats.
                configChangePending_ = true;
                if (configChangeHandle_ != INVALID_HANDLE_VALUE) {
                    FindNextChangeNotification(configChangeHandle_);
                }
                break;

            case WAIT_OBJECT_0 + WAIT_SAFETY_NET:
                wakeupSafetyNet_.fetch_add(1);
                HandleSafetyNetCheck();
                lastSafetyNetTime_ = now;
                break;

            case WAIT_OBJECT_0 + WAIT_ENFORCEMENT_REQUEST:
                wakeupEnforcementRequest_.fetch_add(1);
                ProcessEnforcementQueue();
                break;

            case WAIT_OBJECT_0 + WAIT_PROCESS_EXIT:
                wakeupProcessExit_.fetch_add(1);
                ProcessPendingRemovals();
                break;

            default:
                break;
        }

        // Process debounced config change
        if (configChangePending_ && now - lastConfigCheckTime_ >= CONFIG_DEBOUNCE_MS) {
            HandleConfigChange();
            lastConfigCheckTime_ = now;
            configChangePending_ = false;
        }

        PerformPeriodicMaintenance(now);
    }

    // Final drain: process any remaining removals before loop exit
    ProcessPendingRemovals();

    LOG_INFO(L"Engine: Event-driven control loop ended");
}

// Enqueue enforcement request (called from ETW callbacks and timer callbacks)
void EngineCore::EnqueueRequest(const EnforcementRequest& req) {
    bool wasEmpty;
    {
        CSLockGuard lock(queueCs_);
        wasEmpty = enforcementQueue_.empty();
        enforcementQueue_.push(req);
    }
    if (wasEmpty) {
        SetEvent(enforcementRequestEvent_);
    }
}

// Process all queued enforcement requests
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

// Dispatch a single enforcement request
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
                        wchar_t logBuf[256];
                        swprintf_s(logBuf, L"[PERSISTENT] %s (PID:%lu) via thread event (violations=%u)",
                                   tp.name.c_str(), req.pid, tp.violationCount);
                        LOG_DEBUG(logBuf);
                    } else {
                        tp.phase = ProcessPhase::AGGRESSIVE;
                        tp.phaseStartTime = now;
                        ScheduleDeferredVerification(req.pid, 1);
                        wchar_t logBuf[256];
                        swprintf_s(logBuf, L"[VIOLATION] %s (PID:%lu) via thread event -> AGGRESSIVE",
                                   tp.name.c_str(), req.pid);
                        LOG_DEBUG(logBuf);
                    }
                }
                tp.lastCheckTime = now;
            } else if (tp.phase == ProcessPhase::PERSISTENT) {
                // ETW boost: rate-limited instant response for PERSISTENT phase
                // Provides immediate EcoQoS correction on tab switch without waiting for 5s timer
                if (now - tp.lastEtwEnforceTime >= ETW_BOOST_RATE_LIMIT) {
                    bool ecoQoSOn = IsEcoQoSEnabled(tp.processHandle.get());
                    {
                        wchar_t logBuf[256];
                        swprintf_s(logBuf, L"[ETW_BOOST] %s (PID:%lu) EcoQoS=%s",
                                   tp.name.c_str(), req.pid, ecoQoSOn ? L"ON->enforce" : L"OFF->skip");
                        LOG_DEBUG(logBuf);
                    }
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
                // Clean up fired timer's context
                delete tp.deferredTimerContext;
                tp.deferredTimerContext = nullptr;
                tp.deferredTimer = nullptr;

                bool ecoQoSOn = IsEcoQoSEnabled(tp.processHandle.get());

                if (!ecoQoSOn) {
                    // Clean - check if this is final verification
                    if (req.verifyStep >= 3) {
                        // Final verification passed -> transition to STABLE
                        tp.phase = ProcessPhase::STABLE;
                        tp.phaseStartTime = now;
                        CancelProcessTimers(tp);
                        wchar_t logBuf[256];
                        swprintf_s(logBuf, L"[PHASE] %s (PID:%lu) -> STABLE", tp.name.c_str(), req.pid);
                        LOG_DEBUG(logBuf);
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
                        wchar_t logBuf[256];
                        swprintf_s(logBuf, L"[PERSISTENT] %s (PID:%lu) violations=%u",
                                   tp.name.c_str(), req.pid, tp.violationCount);
                        LOG_DEBUG(logBuf);
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
                bool ecoQoSOn = IsEcoQoSEnabled(tp.processHandle.get());
                if (ecoQoSOn) {
                    // EcoQoS re-enabled -> enforce and mark violation
                    PulseEnforceV6(tp.processHandle.get(), req.pid, true);
                    tp.lastViolationTime = now;
                    persistentEnforceApplied_.fetch_add(1);
                } else {
                    persistentEnforceSkipped_.fetch_add(1);
                }
                tp.lastCheckTime = now;

                // Check if process has been clean long enough to exit PERSISTENT
                // (60 seconds without violation)
                if (!ecoQoSOn) {
                    ULONGLONG timeSinceLastViolation = (tp.lastViolationTime > 0) ?
                        (now - tp.lastViolationTime) : (now - tp.phaseStartTime);
                    if (timeSinceLastViolation >= PERSISTENT_CLEAN_THRESHOLD) {
                        tp.phase = ProcessPhase::STABLE;
                        tp.phaseStartTime = now;
                        CancelProcessTimers(tp);
                        wchar_t logBuf[256];
                        swprintf_s(logBuf, L"[PHASE] %s (PID:%lu) PERSISTENT -> STABLE (clean 60s)",
                                   tp.name.c_str(), req.pid);
                        LOG_DEBUG(logBuf);
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
                    wchar_t logBuf[256];
                    swprintf_s(logBuf, L"[SAFETY_NET] %s (PID:%lu) violation detected",
                               tp.name.c_str(), req.pid);
                    LOG_DEBUG(logBuf);
                }
                tp.lastCheckTime = now;
            }
            break;

        default:
            break;
    }
}

// Handle config file change notification
void EngineCore::HandleConfigChange() {
    // Confirm INI file specifically changed (notification is for any file in directory)
    if (!UnLeafConfig::Instance().HasFileChanged()) {
        return;  // False positive - not our file
    }

    LOG_INFO(L"Config: Reloading (event-driven notification)");

    UnLeafConfig::Instance().Reload();
    configReloadCount_.fetch_add(1, std::memory_order_relaxed);

    // Apply logger settings from reloaded config
    LightweightLogger::Instance().SetLogLevel(UnLeafConfig::Instance().GetLogLevel());
    LightweightLogger::Instance().SetEnabled(UnLeafConfig::Instance().IsLogEnabled());

    RefreshTargetSet();

    wchar_t cfgBuf[96];
    swprintf_s(cfgBuf, L"[CONFIG] Reloaded: %zu targets", targetSet_.size());
    LOG_DEBUG(cfgBuf);

    // Remove tracked processes that are no longer targets
    CleanupRemovedTargets();

    // Re-scan for new targets that might already be running
    InitialScan();

    LOG_INFO(L"Config: Reload complete");
}

// SAFETY NET - Insurance consistency check (NOT monitoring)
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

    if (pidsToCheck.empty()) return;

    // Queue safety net checks for each tracked process
    for (DWORD pid : pidsToCheck) {
        if (stopRequested_.load()) break;
        EnqueueRequest(EnforcementRequest(pid, EnforcementRequestType::SAFETY_NET));
    }

    // Process the queue immediately (we're already in the control loop)
    ProcessEnforcementQueue();
}

// Drain pending process removal queue (called from EngineControlLoop thread only)
void EngineCore::ProcessPendingRemovals() {
    for (;;) {
        std::queue<DWORD> pending;
        {
            CSLockGuard lock(pendingRemovalCs_);
            if (pendingRemovalPids_.empty())
                break;
            std::swap(pending, pendingRemovalPids_);
        }
        while (!pending.empty()) {
            RemoveTrackedProcess(pending.front());
            pending.pop();
        }
    }
}

// Schedule deferred verification timer (AGGRESSIVE phase)
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

    // Get reference to tracked process and create context with shared_ptr
    {
        CSLockGuard lock(trackedCs_);
        auto it = trackedProcesses_.find(pid);
        if (it == trackedProcesses_.end()) {
            return;
        }

        auto* context = new DeferredVerifyContext{this, pid, step, it->second};

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
            it->second->deferredTimerContext = context;
        } else {
            delete context;
        }
    }
}

// Cancel all timers for a process
void EngineCore::CancelProcessTimers(TrackedProcess& tp) {
    if (tp.deferredTimer && timerQueue_) {
        if (!DeleteTimerQueueTimer(timerQueue_, tp.deferredTimer, INVALID_HANDLE_VALUE)) {
            DWORD err = GetLastError();
            if (err != ERROR_IO_PENDING) {
                shutdownWarnings_.fetch_add(1);
            }
        }
        delete tp.deferredTimerContext;
        tp.deferredTimerContext = nullptr;
        tp.deferredTimer = nullptr;
    }
    if (tp.persistentTimer && timerQueue_) {
        if (!DeleteTimerQueueTimer(timerQueue_, tp.persistentTimer, INVALID_HANDLE_VALUE)) {
            DWORD err = GetLastError();
            if (err != ERROR_IO_PENDING) {
                shutdownWarnings_.fetch_add(1);
            }
        }
        delete tp.persistentTimerContext;
        tp.persistentTimerContext = nullptr;
        tp.persistentTimer = nullptr;
    }
}

// Start persistent enforcement timer (5s recurring)
void EngineCore::StartPersistentTimer(DWORD pid) {
    if (!timerQueue_) return;

    {
        CSLockGuard lock(trackedCs_);
        auto it = trackedProcesses_.find(pid);
        if (it == trackedProcesses_.end()) {
            return;
        }

        // Cancel existing persistent timer if any
        if (it->second->persistentTimer) {
            // INVALID_HANDLE_VALUE: block until callback completes before freeing context
            DeleteTimerQueueTimer(timerQueue_, it->second->persistentTimer, INVALID_HANDLE_VALUE);
            delete it->second->persistentTimerContext;
            it->second->persistentTimer = nullptr;
            it->second->persistentTimerContext = nullptr;
        }

        auto* context = new DeferredVerifyContext{this, pid, 0, it->second};

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
            it->second->persistentTimerContext = context;
        } else {
            delete context;
        }
    }
}

// Timer callback for deferred verification
// Context lifetime managed by TrackedProcess::deferredTimerContext (not self-deleted)
void CALLBACK EngineCore::DeferredVerifyTimerCallback(PVOID lpParameter, BOOLEAN timerOrWaitFired) {
    (void)timerOrWaitFired;
    auto* context = static_cast<DeferredVerifyContext*>(lpParameter);
    if (context && context->engine && !context->engine->stopRequested_.load()) {
        context->engine->EnqueueRequest(
            EnforcementRequest(context->pid, EnforcementRequestType::DEFERRED_VERIFICATION, context->step));
    }
}

// Timer callback for persistent enforcement
void CALLBACK EngineCore::PersistentEnforceTimerCallback(PVOID lpParameter, BOOLEAN timerOrWaitFired) {
    (void)timerOrWaitFired;
    auto* context = static_cast<DeferredVerifyContext*>(lpParameter);
    if (context && context->engine && !context->engine->stopRequested_.load()) {
        context->engine->EnqueueRequest(
            EnforcementRequest(context->pid, EnforcementRequestType::PERSISTENT_ENFORCE));
    }
    // Note: Do NOT delete context for recurring timer - it's reused
}

// Periodic maintenance (piggybacks on wakeups)
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

    // Job Object refresh (every 5s) - skip if no active Job Objects
    if (now - lastJobQueryTime_ >= JOB_QUERY_INTERVAL) {
        bool hasJobs;
        {
            CSLockGuard lock(jobCs_);
            hasJobs = !jobObjects_.empty();
        }
        if (hasJobs) {
            RefreshJobObjectPids();
        }
        lastJobQueryTime_ = now;
    }

    // DEGRADED_ETW mode fallback scan (every 30s)
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
        if (count > 0 && (aggressiveCount > 0 || persistentCount > 0)) {
            wchar_t statsBuf[512];
            swprintf_s(statsBuf,
                L"Stats: %zu tracked (A:%zu S:%zu P:%zu), %zu jobs, viol=%u, "
                L"wakeup(cfg:%u sn:%u enf:%u exit:%u), persist(apply:%u skip:%u)",
                count, aggressiveCount, stableCount, persistentCount,
                jobCount, totalViolations_.load(),
                wakeupConfigChange_.load(), wakeupSafetyNet_.load(),
                wakeupEnforcementRequest_.load(), wakeupProcessExit_.load(),
                persistentEnforceApplied_.load(), persistentEnforceSkipped_.load());
            LOG_DEBUG(statsBuf);

            if (persistentCount > 0) {
                CSLockGuard lock2(trackedCs_);
                wchar_t pBuf[512];
                int pos = swprintf_s(pBuf, L"PERSISTENT: ");
                bool first = true;
                for (const auto& [p, t] : trackedProcesses_) {
                    if (t->phase == ProcessPhase::PERSISTENT) {
                        int remaining = 512 - pos;
                        if (remaining <= 0) break;
                        int written = swprintf_s(pBuf + pos, remaining,
                                                 first ? L"%s(%lu)" : L", %s(%lu)",
                                                 t->name.c_str(), p);
                        if (written > 0) pos += written;
                        first = false;
                    }
                }
                LOG_DEBUG(pBuf);
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

    // Determine control mask based on Windows version
    // IGNORE_TIMER (0x4) is Windows 11 specific - causes errors on Windows 10
    ULONG controlMask = UNLEAF_THROTTLE_EXECUTION_SPEED;
    if (winVersion_.isWindows11OrLater) {
        controlMask |= UNLEAF_THROTTLE_IGNORE_TIMER;
    }

    // Step 2: Try NtSetInformationProcess first (Windows 11+ only)
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

// === Registry Policy Application ===

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

        wchar_t logBuf[256];
        swprintf_s(logBuf, L"[REGISTRY] Policy applied for: %s", exeName.c_str());
        LOG_DEBUG(logBuf);
    }

    return success;
}

// === Stateless Pulse Enforcement (fallback) ===

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

    // Step 3: Set HIGH priority (unconditional - critical for OS resistance)
    // Even if SetProcessInformation fails (e.g. Chrome sandbox),
    // setting HIGH_PRIORITY_CLASS prevents OS from reapplying EcoQoS
    SetPriorityClass(hProcess, UNLEAF_TARGET_PRIORITY);

    // Step 4: Thread throttling (INTENSIVE phase only - more expensive operation)
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

// Consolidated thread throttling
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
                            // Boost any thread below ABOVE_NORMAL
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

// === Self-Healing Error Handling ===

void EngineCore::HandleEnforceError(HANDLE hProcess, DWORD pid, DWORD error) {
    totalRetries_.fetch_add(1, std::memory_order_relaxed);

    // Error-code-specific counters (always increment, independent of log suppression)
    if (error == ERROR_ACCESS_DENIED) error5Count_.fetch_add(1);
    else if (error == ERROR_INVALID_PARAMETER) error87Count_.fetch_add(1);

    // Check if process is still alive before retrying
    DWORD exitCode = 0;
    bool processAlive = (hProcess && GetExitCodeProcess(hProcess, &exitCode) && exitCode == STILL_ACTIVE);

    CSLockGuard lock(trackedCs_);
    auto it = trackedProcesses_.find(pid);
    if (it == trackedProcesses_.end()) return;

    auto& tp = it->second;
    tp->consecutiveFailures++;
    tp->lastErrorCode = error;

    // Error log suppression (same PID × error code within 60s window)
    auto suppressKey = std::make_pair(pid, error);
    bool shouldLog = true;
    ULONGLONG now = GetTickCount64();
    auto suppIt = errorLogSuppression_.find(suppressKey);
    if (suppIt != errorLogSuppression_.end() && now - suppIt->second < ERROR_LOG_SUPPRESS_MS) {
        shouldLog = false;
    } else {
        errorLogSuppression_[suppressKey] = now;
    }

    // If process has exited, cleanup immediately (no retry)
    if (!processAlive) {
        if (shouldLog) {
            wchar_t logBuf[256];
            swprintf_s(logBuf, L"[CLEANUP] %s (PID:%lu) process exited (exitCode=%lu)",
                       tp->name.c_str(), pid, exitCode);
            LOG_DEBUG(logBuf);
        }
        tp->processHandle.reset();
        return;
    }

    switch (error) {
        case ERROR_ACCESS_DENIED:
            if (tp->consecutiveFailures <= 2) {
                tp->nextRetryTime = GetTickCount64() + RETRY_BACKOFF_BASE_MS;
            } else if (shouldLog) {
                wchar_t logBuf[256];
                swprintf_s(logBuf, L"[GIVE_UP] %s (PID:%lu) access denied - giving up after 2 retries",
                           tp->name.c_str(), pid);
                LOG_DEBUG(logBuf);
            }
            break;

        case ERROR_INVALID_HANDLE:
            tp->processHandle.reset();
            break;

        case ERROR_INVALID_PARAMETER:  // 87 - PID is invalid (process already exited)
            // No retry needed - process is dead, Safety Net will re-detect if needed
            tp->processHandle.reset();
            break;

        default:
            if (tp->consecutiveFailures <= MAX_RETRY_COUNT) {
                DWORD backoff = RETRY_BACKOFF_BASE_MS * (1 << (tp->consecutiveFailures - 1));
                tp->nextRetryTime = GetTickCount64() + backoff;
            } else if (shouldLog) {
                wchar_t logBuf[256];
                swprintf_s(logBuf, L"[GIVE_UP] %s (PID:%lu) error=%lu - max retries reached",
                           tp->name.c_str(), pid, error);
                LOG_DEBUG(logBuf);
            }
            break;
    }
}

bool EngineCore::ReopenProcessHandle(DWORD pid) {
    totalHandleReopen_.fetch_add(1, std::memory_order_relaxed);

    HANDLE hProcess = OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_INFORMATION,
        FALSE, pid);

    if (!hProcess) return false;

    HANDLE oldWaitHandle = nullptr;
    WaitCallbackContext* oldContext = nullptr;

    {
        CSLockGuard lock(trackedCs_);
        auto it = trackedProcesses_.find(pid);
        if (it == trackedProcesses_.end()) {
            CloseHandle(hProcess);
            return false;
        }

        auto& tp = it->second;

        // Collect old wait handle for out-of-lock cleanup
        if (tp->waitHandle) {
            oldWaitHandle = tp->waitHandle;
            tp->waitHandle = nullptr;
        }
        tp->waitProcessHandle.reset();

        // Free old context
        auto oldCtxIt = waitContexts_.find(pid);
        if (oldCtxIt != waitContexts_.end()) {
            oldContext = oldCtxIt->second;
            waitContexts_.erase(oldCtxIt);
        }

        // Replace handle
        tp->processHandle = MakeScopedHandle(hProcess);
        tp->consecutiveFailures = 0;
        tp->nextRetryTime = 0;

        // Re-register wait callback with separate SYNCHRONIZE handle
        HANDLE hWaitProcess = OpenProcess(SYNCHRONIZE, FALSE, pid);
        if (hWaitProcess) {
            auto context = new WaitCallbackContext{this, pid, it->second};
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
                waitContexts_[pid] = context;
            } else {
                delete context;
                CloseHandle(hWaitProcess);
            }
        }
    }

    // Unregister old wait outside lock (blocks until callback completes)
    if (oldWaitHandle) {
        if (!UnregisterWaitEx(oldWaitHandle, INVALID_HANDLE_VALUE)) {
            shutdownWarnings_.fetch_add(1);
        }
    }
    delete oldContext;

    wchar_t logBuf[64];
    swprintf_s(logBuf, L"Engine: Reopened handle for PID %lu", pid);
    LOG_DEBUG(logBuf);
    return true;
}

// === Job Object Management ===

bool EngineCore::CreateAndAssignJobObject(DWORD rootPid, HANDLE hProcess) {
    // Check if already in a Job (Chrome sandbox case)
    BOOL inJob = FALSE;
    IsProcessInJob(hProcess, nullptr, &inJob);
    if (inJob) {
        wchar_t logBuf[96];
        swprintf_s(logBuf, L"Job: PID %lu already in Job - pulse-only mode", rootPid);
        LOG_DEBUG(logBuf);
        return false;  // TrackedProcess still created to continue PulseEnforce
    }

    // Create Job Object
    HANDLE hJob = CreateJobObjectW(nullptr, nullptr);
    if (!hJob) {
        wchar_t logBuf[96];
        swprintf_s(logBuf, L"[JOB] Failed to create Job Object (error=%lu)", GetLastError());
        LOG_DEBUG(logBuf);
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
        wchar_t logBuf[128];
        swprintf_s(logBuf, L"[JOB] Failed to assign PID %lu to Job (error=%lu)", rootPid, error);
        LOG_DEBUG(logBuf);
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

    {
        wchar_t logBuf[96];
        swprintf_s(logBuf, L"Job: Created Job Object for root PID %lu", rootPid);
        LOG_DEBUG(logBuf);
    }
    return true;
}

// Optimized 2-pass approach to minimize lock contention
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
}

// === Process Management ===

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

    // Use minimal permissions (0x1200) for Chrome sandbox process compatibility
    DWORD access = PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_INFORMATION;
    HANDLE hProcess = OpenProcess(access, FALSE, pid);
    if (!hProcess) {
        // Log failure for observability
        wchar_t logBuf[256];
        swprintf_s(logBuf, L"[SKIP] %s (PID:%lu) OpenProcess failed (error=%lu)",
                   name.c_str(), pid, GetLastError());
        LOG_DEBUG(logBuf);
        return false;
    }

    ScopedHandle scopedHandle = MakeScopedHandle(hProcess);
    ULONGLONG now = GetTickCount64();

    // Apply registry policy for root target processes (once per executable)
    if (!isChild) {
        // Get full path for registry policy
        wchar_t pathBuffer[MAX_PATH];
        DWORD pathSize = MAX_PATH;
        if (QueryFullProcessImageNameW(scopedHandle.get(), 0, pathBuffer, &pathSize)) {
            std::wstring fullPath(pathBuffer);
            ApplyRegistryPolicy(fullPath, name);
        }
    }

    bool success = PulseEnforceV6(scopedHandle.get(), pid, true);

    // Log the optimization
    wchar_t optBuf[256];
    swprintf_s(optBuf, L"Optimized: %s %s (PID: %lu) Child=%d",
               isChild ? L"[CHILD]" : L"[TARGET]", name.c_str(), pid, isChild ? 1 : 0);
    LOG_DEBUG(optBuf);

    // Job Object assignment for root target processes
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
    auto tracked = std::make_shared<TrackedProcess>();
    tracked->pid = pid;
    tracked->parentPid = parentPid;
    tracked->name = name;
    tracked->processHandle = std::move(scopedHandle);
    tracked->isChild = isChild;
    tracked->phase = ProcessPhase::AGGRESSIVE;
    tracked->phaseStartTime = now;
    tracked->lastCheckTime = now;
    tracked->lastPriorityCheck = now;
    tracked->violationCount = 0;
    tracked->waitHandle = nullptr;

    tracked->consecutiveFailures = 0;
    tracked->lastErrorCode = 0;
    tracked->nextRetryTime = 0;

    tracked->rootTargetPid = rootPid;
    tracked->inJobObject = inJob;
    tracked->jobAssignmentFailed = jobFailed;

    // Register wait for process exit using separate SYNCHRONIZE handle
    // Main handle (0x1200) doesn't have SYNCHRONIZE, so we open another handle
    HANDLE hWaitProcess = OpenProcess(SYNCHRONIZE, FALSE, pid);
    WaitCallbackContext* context = nullptr;
    if (hWaitProcess) {
        context = new WaitCallbackContext{this, pid, tracked};
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
        if (context) {
            waitContexts_[pid] = context;
        }
    }

    // Schedule deferred verification
    ScheduleDeferredVerification(pid, 1);

    return success;
}

void CALLBACK EngineCore::OnProcessExit(PVOID lpParameter, BOOLEAN timerOrWaitFired) {
    (void)timerOrWaitFired;
    auto* context = static_cast<WaitCallbackContext*>(lpParameter);
    if (!context || !context->engine) return;
    EngineCore* engine = context->engine;
    bool wasEmpty;
    {
        CSLockGuard lock(engine->pendingRemovalCs_);
        wasEmpty = engine->pendingRemovalPids_.empty();
        engine->pendingRemovalPids_.push(context->pid);
    }
    if (wasEmpty && !engine->stopRequested_.load(std::memory_order_acquire)) {
        HANDLE h = engine->hWakeupEvent_;
        if (h) SetEvent(h);
    }
}

// Safely remove tracked process with proper wait handle cleanup
void EngineCore::RemoveTrackedProcess(DWORD pid) {
    HANDLE waitHandleToUnregister = nullptr;
    WaitCallbackContext* contextToDelete = nullptr;
    DeferredVerifyContext* timerCtxToDelete = nullptr;
    DeferredVerifyContext* deferredCtxToDelete = nullptr;

    {
        CSLockGuard lock(trackedCs_);

        auto it = trackedProcesses_.find(pid);
        if (it != trackedProcesses_.end()) {
            // Cancel timers and recover contexts
            if (it->second->deferredTimer && timerQueue_) {
                if (!DeleteTimerQueueTimer(timerQueue_, it->second->deferredTimer, INVALID_HANDLE_VALUE)) {
                    DWORD err = GetLastError();
                    if (err != ERROR_IO_PENDING) {
                        shutdownWarnings_.fetch_add(1);
                    }
                }
                deferredCtxToDelete = it->second->deferredTimerContext;
                it->second->deferredTimerContext = nullptr;
                it->second->deferredTimer = nullptr;
            }
            if (it->second->persistentTimer && timerQueue_) {
                if (!DeleteTimerQueueTimer(timerQueue_, it->second->persistentTimer, INVALID_HANDLE_VALUE)) {
                    DWORD err = GetLastError();
                    if (err != ERROR_IO_PENDING) {
                        shutdownWarnings_.fetch_add(1);
                    }
                }
                timerCtxToDelete = it->second->persistentTimerContext;
                it->second->persistentTimerContext = nullptr;
                it->second->persistentTimer = nullptr;
            }

            waitHandleToUnregister = it->second->waitHandle;
            it->second->waitHandle = nullptr;
            trackedProcesses_.erase(it);
        }

        auto ctxIt = waitContexts_.find(pid);
        if (ctxIt != waitContexts_.end()) {
            contextToDelete = ctxIt->second;
            waitContexts_.erase(ctxIt);
        }
    }

    // Unregister outside lock with INVALID_HANDLE_VALUE to wait for callback completion
    if (waitHandleToUnregister) {
        if (!UnregisterWaitEx(waitHandleToUnregister, INVALID_HANDLE_VALUE)) {
            shutdownWarnings_.fetch_add(1);
        }
    }

    // Clean up error suppression entries for this PID
    {
        CSLockGuard lock(trackedCs_);
        for (auto it = errorLogSuppression_.begin(); it != errorLogSuppression_.end(); ) {
            if (it->first.first == pid) {
                it = errorLogSuppression_.erase(it);
            } else {
                ++it;
            }
        }
    }

    delete contextToDelete;
    delete timerCtxToDelete;
    delete deferredCtxToDelete;
}

bool EngineCore::IsTracked(DWORD pid) const {
    CSLockGuard lock(trackedCs_);
    return trackedProcesses_.find(pid) != trackedProcesses_.end();
}

bool EngineCore::IsTargetName(const std::wstring& name) const {
    CSLockGuard lock(targetCs_);
    return targetSet_.find(ToLower(name)) != targetSet_.end();
}

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

    // Remove collected processes (delegates to RemoveTrackedProcess for proper cleanup)
    if (!toRemove.empty()) {
        for (DWORD pid : toRemove) {
            RemoveTrackedProcess(pid);
        }
        wchar_t logBuf[128];
        swprintf_s(logBuf, L"Removed %zu tracked processes (targets changed)",
                   toRemove.size());
        LOG_DEBUG(logBuf);
    }
}

size_t EngineCore::GetActiveProcessCount() const {
    CSLockGuard lock(trackedCs_);
    return trackedProcesses_.size();
}

// Health check info
HealthInfo EngineCore::GetHealthInfo() const {
    HealthInfo info = {};  // zero-initialize all fields
    info.engineRunning = running_.load();
    info.mode = operationMode_;
    info.activeProcesses = GetActiveProcessCount();
    info.totalViolations = totalViolations_.load();
    info.etwHealthy = processMonitor_.IsHealthy();
    info.etwEventCount = processMonitor_.GetEventCount();
    info.uptimeMs = (startTime_ > 0) ? (GetTickCount64() - startTime_) : 0;

    // Phase breakdown
    {
        CSLockGuard lock(trackedCs_);
        for (const auto& [pid, tp] : trackedProcesses_) {
            switch (tp->phase) {
                case ProcessPhase::AGGRESSIVE: info.aggressiveCount++; break;
                case ProcessPhase::STABLE:     info.stableCount++; break;
                case ProcessPhase::PERSISTENT: info.persistentCount++; break;
            }
        }
    }

    // Wakeup counters
    info.wakeupConfigChange = wakeupConfigChange_.load();
    info.wakeupSafetyNet = wakeupSafetyNet_.load();
    info.wakeupEnforcementRequest = wakeupEnforcementRequest_.load();
    info.wakeupProcessExit = wakeupProcessExit_.load();

    // PERSISTENT enforce counters
    info.persistentEnforceApplied = persistentEnforceApplied_.load();
    info.persistentEnforceSkipped = persistentEnforceSkipped_.load();

    // Shutdown warnings
    info.shutdownWarnings = shutdownWarnings_.load();

    // Error counters
    info.error5Count = error5Count_.load();
    info.error87Count = error87Count_.load();

    // Config monitoring
    info.configChangeDetected = configChangeDetected_.load();
    info.configReloadCount = configReloadCount_.load();

    return info;
}

} // namespace unleaf
