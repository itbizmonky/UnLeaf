// UnLeaf - Engine Core Implementation (Event-Driven Architecture)

#include "engine_core.h"
#include <algorithm>
#include <functional>
#include <chrono>
#include <cstdio>
#include <cassert>
#include <memory_resource>
#include <unordered_set>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")

namespace unleaf {

// Context structure for wait callback
struct WaitCallbackContext {
    EngineCore* engine;
    DWORD pid;
    std::shared_ptr<TrackedProcess> process;  // prevent premature destruction
};

namespace {

// §9.05: CountingResource — single definition to avoid ODR issues.
// Used in ProcessEnforcementQueue() and HandleSafetyNetCheck() PMR arenas.
class CountingResource : public std::pmr::memory_resource {
public:
    explicit CountingResource(std::pmr::memory_resource* up) : upstream_(up) {}
    size_t count() const { return count_; }
protected:
    void* do_allocate(size_t b, size_t a) override {
        count_++; return upstream_->allocate(b, a);
    }
    void do_deallocate(void* p, size_t b, size_t a) override {
        upstream_->deallocate(p, b, a);
    }
    bool do_is_equal(const std::pmr::memory_resource& o) const noexcept override {
        return this == &o;
    }
private:
    std::pmr::memory_resource* upstream_;
    size_t count_ = 0;
};

#ifdef _DEBUG
// §9.07 修正②: DEBUG-only helper to verify trackedCs_ ownership.
// CriticalSection wraps CRITICAL_SECTION as its sole member — reinterpret_cast is safe on MSVC.
// CRITICAL_SECTION::OwningThread stores the owning TID cast to HANDLE.
static bool IsCSHeldByCurrent(const CriticalSection& cs) noexcept {
    const CRITICAL_SECTION* native = reinterpret_cast<const CRITICAL_SECTION*>(&cs);
    return (DWORD)(ULONG_PTR)native->OwningThread == GetCurrentThreadId();
}
#endif

// §9.07 修正③ + §9.09 修正②③: Multi-candidate eviction — selects up to `count` distinct PIDs.
// Same priority order as SelectEvictionCandidate(): zombies first, then oldest phaseStartTime.
// PRECONDITION: trackedCs_ must be held by caller.
static std::vector<DWORD> SelectEvictionCandidates(
    const std::map<DWORD, std::shared_ptr<TrackedProcess>>& processes,
    size_t count)
{
    // §9.09 修正③: safety cap — prevent requesting more than exists
    count = std::min(count, processes.size());
    if (count == 0) return {};

    // §9.09 修正②: PMR arena for internal temporaries (picked, aged).
    // 16KB handles ~2000-entry aged vector (12 bytes/entry) without fallback under typical load.
    // result uses std::vector (standard allocator) — std::pmr::vector → std::vector 暗黙変換不可.
    CountingResource pmrCountingRes(std::pmr::get_default_resource());
    std::byte buf[16384];
    std::pmr::monotonic_buffer_resource arena(buf, sizeof(buf), &pmrCountingRes);

    std::vector<DWORD> result;
    result.reserve(count);
    // Use pmr::vector instead of unordered_set: monotonic_buffer cannot release rehash memory.
    // picked is typically 0-10 entries (zombie count), so O(N) linear search is acceptable.
    std::pmr::vector<DWORD> picked(&arena);

    // Priority 1: zombies (invalid handle)
    for (const auto& [p, tp] : processes) {
        if (result.size() >= count) break;
        if (!tp->processHandle.get()) {
            result.push_back(p);
            picked.push_back(p);
        }
    }

    // Priority 2: oldest phaseStartTime among non-picked
    if (result.size() < count) {
        std::pmr::vector<std::pair<DWORD, ULONGLONG>> aged(&arena);
        aged.reserve(processes.size());  // §9.08 修正③
        for (const auto& [p, tp] : processes) {
            bool inPicked = false;
            for (DWORD pk : picked) { if (pk == p) { inPicked = true; break; } }
            if (!inPicked) aged.push_back({p, tp->phaseStartTime});
        }
        // §9.08 修正②: partial_sort when we need fewer than all entries (O(N log K) vs O(N log N))
        size_t remain = count - result.size();
        if (aged.size() > remain) {
            std::partial_sort(
                aged.begin(),
                aged.begin() + static_cast<ptrdiff_t>(remain),
                aged.end(),
                [](const auto& a, const auto& b) { return a.second < b.second; });
        } else {
            std::sort(aged.begin(), aged.end(),
                [](const auto& a, const auto& b) { return a.second < b.second; });
        }
        for (const auto& [p, _] : aged) {
            if (result.size() >= count) break;
            result.push_back(p);
        }
    }

    if (pmrCountingRes.count() > 0) {
#ifdef _DEBUG
        wchar_t dbgBuf[128];
        swprintf_s(dbgBuf, L"[PMR] SelectEvictionCandidates fallback count=%zu", pmrCountingRes.count());
        LOG_DEBUG(dbgBuf);
#else
        static std::atomic<int> pmrWarnCount{0};
        if (pmrWarnCount.fetch_add(1, std::memory_order_relaxed) < 10) {
            LOG_INFO(L"[PMR] SelectEvictionCandidates fallback detected");
        }
#endif
    }

    return result;
}

} // anonymous namespace

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
    , lastProcessLivenessCheck_(0)
    , lastSuppressionCleanup_(0)
    , lastMemLogTime_(0)
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
    , configChangePending_(false)
    , hasPathTargets_(false) {
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
        wchar_t osName[16];
        if (winVersion_.major >= 11) {
            // Windows 12 以降で major が繰り上がった場合は major.minor をそのまま表示
            swprintf_s(osName, L"%lu.%lu", winVersion_.major, winVersion_.minor);
        } else if (winVersion_.isWindows11OrLater) {
            // major=10 だが build >= 22000 → Windows 11
            wcscpy_s(osName, L"11");
        } else {
            // build < 22000 → Windows 10
            wcscpy_s(osName, L"10");
        }

        wchar_t verBuf[128];
        swprintf_s(verBuf, L"Engine: Windows %s (Build %lu) - %s",
                   osName, winVersion_.build,
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
    lastDiagLogTime_ = now;
    QueryPerformanceFrequency(&qpcFreq_);
    lastEtwHealthCheck_ = now;
    lastJobQueryTime_ = now;
    lastSafetyNetTime_ = now;
    lastProcessLivenessCheck_ = now;
    lastDegradedScanTime_ = now;
    lastMemLogTime_ = now;
    startTime_ = now;
    ResetEvent(stopEvent_);

    // Start ETW with both process and thread callbacks
    bool etwStarted = processMonitor_.Start(
        [this](DWORD pid, DWORD parentPid, const std::wstring& imageName,
               const std::wstring& imagePath) {
            this->OnProcessStart(pid, parentPid, imageName, imagePath);
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

    // Proactive: apply registry policies from config BEFORE process detection
    ApplyProactivePolicies();

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
    swprintf_s(startBuf, L"EngineCore started: %zu name + %zu path targets, %s mode, Event-Driven, SafetyNet=10s",
               targetNameSet_.size(), targetPathSet_.size(),
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
                waitUnregisterFailures_.fetch_add(1, std::memory_order_relaxed);
            } else {
                waitUnregisterCount_.fetch_add(1, std::memory_order_relaxed);
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
        policyCacheLru_.clear();
        policyCacheMap_.clear();
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

void EngineCore::OnProcessStart(DWORD pid, DWORD parentPid,
                                 const std::wstring& imageName, const std::wstring& imagePath) {
    if (stopRequested_.load()) return;

    // Skip critical processes
    if (IsCriticalProcess(imageName)) {
        return;
    }

    // Case 1: Parent is already tracked -> this is a child process
    if (IsTrackedParent(parentPid)) {
        ApplyOptimization(pid, imageName, true, parentPid, imagePath);
        return;
    }

    // Case 2: This process name is a name-only target
    if (IsTargetName(imageName)) {
        ApplyOptimization(pid, imageName, false, 0, imagePath);
        return;
    }

    // Case 3: Path-based target check (only when path targets are configured)
    if (HasPathTargets()) {
        TryApplyByPath(pid, imageName);
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

    // Spin detection: last line of defense against event misfire or future bugs
    static thread_local uint32_t spinCount = 0;

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

        // Spin detection on hWakeupEvent_ consecutive fires
        if (waitResult == WAIT_OBJECT_0 + WAIT_PROCESS_EXIT) {
            if (++spinCount > 10000) {
                LOG_ALERT(L"[SPIN DETECTED] excessive wakeups");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                spinCount = 0;
            }
        } else {
            spinCount = 0;
        }

        switch (waitResult) {
            case WAIT_OBJECT_0 + WAIT_CONFIG_CHANGE:
                wakeupConfigChange_.fetch_add(1);
                configChangeDetected_.fetch_add(1);
                // Debounce: notification fires for ALL file changes in directory
                // (including log writes). Defer actual check to reduce unnecessary file stats.
                configChangePending_ = true;
                if (configChangeHandle_ != INVALID_HANDLE_VALUE) {
                    if (!FindNextChangeNotification(configChangeHandle_)) {
                        LOG_ALERT(L"[CONFIG] FindNextChangeNotification failed - "
                                  L"config change detection disabled");
                        FindCloseChangeNotification(configChangeHandle_);
                        configChangeHandle_ = INVALID_HANDLE_VALUE;
                    }
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

    // Final drain: process ALL remaining before loop exit (no cap — service stopping)
    for (;;) {
        std::queue<DWORD> pending;
        {
            CSLockGuard lock(pendingRemovalCs_);
            if (pendingRemovalPids_.empty()) break;
            std::swap(pending, pendingRemovalPids_);
        }
        while (!pending.empty()) {
            RemoveTrackedProcess(pending.front());
            pending.pop();
        }
    }

    LOG_INFO(L"Engine: Event-driven control loop ended");
}

// Enqueue enforcement request (called from ETW callbacks and timer callbacks)
// §9.14-A: 2-queue CRITICAL/NON-CRITICAL split with TOTAL_LIMIT absolute guarantee.
// ETW_THREAD_START = NON-CRITICAL (droppable at SOFT_LIMIT).
// All other types = CRITICAL (eviction from nonCritical first, then oldest-CRITICAL rotation).
void EngineCore::EnqueueRequest(const EnforcementRequest& req) {
    const bool isCritical = (req.type != EnforcementRequestType::ETW_THREAD_START);
    bool wasEmpty;
    {
        CSLockGuard lock(queueCs_);
        wasEmpty = criticalQueue_.empty() && nonCriticalQueue_.empty();

        const size_t total = criticalQueue_.size() + nonCriticalQueue_.size();

        if (!isCritical) {
            // NON-CRITICAL: 個別 OR 合計上限でドロップ（高頻度のためログなし）
            if (nonCriticalQueue_.size() >= ENFORCEMENT_QUEUE_SOFT_LIMIT ||
                total >= ENFORCEMENT_QUEUE_TOTAL_LIMIT) {
                enforcementDropCount_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            nonCriticalQueue_.push_back(req);
        } else {
            // CRITICAL: 個別上限チェック
            if (criticalQueue_.size() >= ENFORCEMENT_QUEUE_HARD_LIMIT) {
                uint32_t cnt = criticalDropCount_.fetch_add(1, std::memory_order_relaxed);
                if ((cnt & 0xFF) == 0)
                    LOG_ALERT(L"[QUEUE] CRITICAL HARD drop=" + std::to_wstring(cnt + 1));
                return;
            }

            // CRITICAL: 合計上限超過時の処理（TOTAL_LIMIT 絶対保証）
            if (total >= ENFORCEMENT_QUEUE_TOTAL_LIMIT) {
                if (!nonCriticalQueue_.empty()) {
                    // NON-CRITICAL を追い出して空きを確保 (O(1))
                    nonCriticalQueue_.pop_front();
                    enforcementDropCount_.fetch_add(1, std::memory_order_relaxed);
                } else if (!criticalQueue_.empty()) {
                    // nonCritical 空 + TOTAL_LIMIT 到達 → 最古 CRITICAL を evict して新規を受け入れる
                    // TOTAL_LIMIT は pop_front 1 + push_back 1 で維持（O(1)）
                    // 完全喪失より最古イベントの破棄を優先する設計
                    uint32_t cnt = criticalEvictCount_.fetch_add(1, std::memory_order_relaxed);
                    if ((cnt & 0xFF) == 0)
                        LOG_ALERT(L"[QUEUE] TOTAL-LIMIT evict oldest CRITICAL evict=" + std::to_wstring(cnt + 1));
                    criticalQueue_.pop_front();
                } else {
                    // criticalQueue_ も空 = TOTAL_LIMIT=0 設定など異常構成 → 受け入れ不能
                    criticalDropCount_.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
            }
            criticalQueue_.push_back(req);
        }
    }
    if (wasEmpty) {
        SetEvent(enforcementRequestEvent_);
    }
}

// Process all queued enforcement requests
// §9.14-A: CRITICAL を先に処理（バースト制限付き）、NON-CRITICAL は PMR dedup 適用。
void EngineCore::ProcessEnforcementQueue() {
    std::deque<EnforcementRequest> critical, nonCritical;
    {
        CSLockGuard lock(queueCs_);
        // CRITICAL: バースト制限あり（CPU 安定化のため）
        // 残件は criticalQueue_ に留まり次回呼び出しで処理される（re-enqueue 不要）
        const int toDrain = std::min(static_cast<int>(criticalQueue_.size()), ENFORCEMENT_CRITICAL_PER_TICK);
        for (int i = 0; i < toDrain; i++) {
            critical.push_back(std::move(criticalQueue_.front()));
            criticalQueue_.pop_front();
        }
        // NON-CRITICAL: 全量スワップ
        std::swap(nonCritical, nonCriticalQueue_);
    }

    // CRITICAL を先に処理（フェーズ遷移・プロセス検出を優先）
    for (const auto& req : critical) {
        if (stopRequested_.load()) return;
        DispatchEnforcementRequest(req);
    }

    // NON-CRITICAL: 既存の PMR arena デデュプ処理を適用
    // Deduplicate: merge same-PID ETW_THREAD_START requests to prevent
    // O(N^2) CreateToolhelp32Snapshot storms during thread burst
    //
    // §9.00: Local PMR arena — temporary containers allocated from stack buffer.
    // §9.05: CountingResource defined in anonymous namespace; fallback visible in Debug and Release.
    CountingResource counting(std::pmr::get_default_resource());
    std::byte arenaBuf[8 * 1024];
    std::pmr::monotonic_buffer_resource arena(arenaBuf, sizeof(arenaBuf), &counting);

    std::pmr::vector<EnforcementRequest> deduped(&arena);
    std::pmr::set<DWORD> seenEtwPids(&arena);
    for (auto& req : nonCritical) {
        // All items in nonCritical are ETW_THREAD_START; dedup by PID
        if (seenEtwPids.count(req.pid)) {
            etwThreadDeduped_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        seenEtwPids.insert(req.pid);
        deduped.push_back(std::move(req));
    }

    for (const auto& req : deduped) {
        if (stopRequested_.load()) break;
        DispatchEnforcementRequest(req);
    }

    if (counting.count() > 0) {
#ifdef _DEBUG
        wchar_t dbgBuf[128];
        swprintf_s(dbgBuf, L"[PMR] ProcessEnforcementQueue arena fallback count=%zu", counting.count());
        LOG_DEBUG(dbgBuf);
#else
        static std::atomic<int> pmrWarnCount{0};
        if (pmrWarnCount.fetch_add(1, std::memory_order_relaxed) < 10) {
            LOG_INFO(L"[PMR] ProcessEnforcementQueue fallback detected");
        }
#endif
    }
}

// Dispatch a single enforcement request
void EngineCore::DispatchEnforcementRequest(const EnforcementRequest& req) {
    // Accumulators for timer cleanup: filled inside lock, deleted outside lock
    // (DeleteTimerQueueTimer(INVALID_HANDLE_VALUE) must not be called while holding trackedCs_)
    std::vector<HANDLE>               timersToDelete;
    std::vector<DeferredVerifyContext*> ctxToDelete;

    {
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
                // Rate limit to prevent CPU burst during thread storms
                if (now - tp.lastEtwEnforceTime < ETW_STABLE_RATE_LIMIT) {
                    break;
                }
                bool ecoQoSOn = IsEcoQoSEnabledCached(tp, now);
                if (ecoQoSOn) {
                    // Violation detected via event
                    PulseEnforceV6(tp.processHandle.get(), req.pid, true);
                    tp.ecoQosCached = false;  // Invalidate cache after enforcement
                    tp.violationCount++;
                    totalViolations_.fetch_add(1);
                    tp.lastViolationTime = now;

                    tp.phase = engine_logic::NextPhaseOnViolation(tp.violationCount, policy_);
                    if (tp.phase == ProcessPhase::PERSISTENT) {
                        StartPersistentTimer(req.pid);
                        wchar_t logBuf[256];
                        swprintf_s(logBuf, L"[PERSISTENT] %s (PID:%lu) via thread event (violations=%u)",
                                   tp.name.c_str(), req.pid, tp.violationCount);
                        LOG_DEBUG(logBuf);
                    } else {
                        tp.phaseStartTime = now;
                        ScheduleDeferredVerification(req.pid, 1);
                        wchar_t logBuf[256];
                        swprintf_s(logBuf, L"[VIOLATION] %s (PID:%lu) via thread event -> AGGRESSIVE",
                                   tp.name.c_str(), req.pid);
                        LOG_DEBUG(logBuf);
                    }
                }
                tp.lastCheckTime = now;
                tp.lastEtwEnforceTime = now;
            } else if (tp.phase == ProcessPhase::PERSISTENT) {
                // ETW boost: rate-limited instant response for PERSISTENT phase
                // Provides immediate EcoQoS correction on tab switch without waiting for 5s timer
                if (now - tp.lastEtwEnforceTime >= ETW_BOOST_RATE_LIMIT) {
                    bool ecoQoSOn = IsEcoQoSEnabledCached(tp, now);
                    {
                        wchar_t logBuf[256];
                        swprintf_s(logBuf, L"[ETW_BOOST] %s (PID:%lu) EcoQoS=%s",
                                   tp.name.c_str(), req.pid, ecoQoSOn ? L"ON->enforce" : L"OFF->skip");
                        LOG_DEBUG(logBuf);
                    }
                    if (ecoQoSOn) {
                        PulseEnforceV6(tp.processHandle.get(), req.pid, true);
                        tp.ecoQosCached = false;  // Invalidate cache after enforcement
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
                        CancelProcessTimers(tp, timersToDelete, ctxToDelete);
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

                    tp.phase = engine_logic::NextPhaseOnViolation(tp.violationCount, policy_);
                    if (tp.phase == ProcessPhase::PERSISTENT) {
                        CancelProcessTimers(tp, timersToDelete, ctxToDelete);
                        StartPersistentTimer(req.pid);
                        wchar_t logBuf[256];
                        swprintf_s(logBuf, L"[PERSISTENT] %s (PID:%lu) violations=%u",
                                   tp.name.c_str(), req.pid, tp.violationCount);
                        LOG_DEBUG(logBuf);
                    } else {
                        // Restart AGGRESSIVE with fresh verification sequence
                        tp.phaseStartTime = now;
                        CancelProcessTimers(tp, timersToDelete, ctxToDelete);
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
                    if (engine_logic::ShouldExitPersistent(
                            static_cast<uint64_t>(timeSinceLastViolation),
                            static_cast<uint64_t>(PERSISTENT_CLEAN_THRESHOLD))) {
                        tp.phase = ProcessPhase::STABLE;
                        tp.phaseStartTime = now;
                        CancelProcessTimers(tp, timersToDelete, ctxToDelete);
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

                    tp.phase = engine_logic::NextPhaseOnViolation(tp.violationCount, policy_);
                    if (tp.phase == ProcessPhase::PERSISTENT) {
                        StartPersistentTimer(req.pid);
                    } else {
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
    }  // end CSLockGuard scope

    // Delete accumulated timers outside lock: INVALID_HANDLE_VALUE blocks until
    // any in-flight callback completes, then we free the context.
    for (size_t i = 0; i < timersToDelete.size(); ++i) {
        if (!DeleteTimerQueueTimer(timerQueue_, timersToDelete[i], INVALID_HANDLE_VALUE)) {
            DWORD err = GetLastError();
            if (err != ERROR_IO_PENDING) { shutdownWarnings_.fetch_add(1); }
        }
        delete ctxToDelete[i];
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

    {
        CSLockGuard lock(policySetCs_);
        policyCacheLru_.clear();
        policyCacheMap_.clear();
    }

    RefreshTargetSet();

    // Proactive: sync registry policies with updated config
    ApplyProactivePolicies();

    wchar_t cfgBuf[96];
    swprintf_s(cfgBuf, L"[CONFIG] Reloaded: %zu name + %zu path targets",
               targetNameSet_.size(), targetPathSet_.size());
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

    // §9.00: Local PMR arena for temporary PID snapshot.
    // §9.05: CountingResource defined in anonymous namespace; fallback visible in Debug and Release.
    CountingResource counting(std::pmr::get_default_resource());
    std::byte arenaBuf[8 * 1024];
    std::pmr::monotonic_buffer_resource arena(arenaBuf, sizeof(arenaBuf), &counting);

    std::pmr::vector<DWORD> pidsToCheck(&arena);
    {
        CSLockGuard lock(trackedCs_);
        for (const auto& [pid, tp] : trackedProcesses_) {
            if (tp->processHandle.get() && tp->phase == ProcessPhase::STABLE) {
                pidsToCheck.push_back(pid);
            }
        }
    }

    // §9.14: Policy recovery — retry path resolution for tracked processes with empty fullPath
    struct PolicyRetryInfo {
        DWORD pid;
        HANDLE hProcess;     // borrowed (owned by TrackedProcess::processHandle)
        std::wstring name;
    };
    std::pmr::vector<PolicyRetryInfo> policyRetries(&arena);

    {
        CSLockGuard lock(trackedCs_);
        for (const auto& [pid, tp] : trackedProcesses_) {
            if (tp->needsPolicyRetry && tp->processHandle.get()) {
                policyRetries.push_back({pid, tp->processHandle.get(), tp->name});
            }
        }
    }

    for (auto& info : policyRetries) {
        if (stopRequested_.load()) break;

        std::wstring resolved = ResolveProcessPath(info.hProcess);
        if (!resolved.empty()) {
            resolved = CanonicalizePath(resolved);
        }
        if (resolved.empty()) continue;  // retry on next SafetyNet cycle (10s)

        if (!RegistryPolicyManager::Instance().HasPolicy(resolved)) {
            std::wstring lowerName = ToLower(info.name);
            RegistryPolicyManager::Instance().ApplyPolicy(lowerName, resolved);
            LOG_INFO(L"[REGISTRY] SafetyNet policy recovery: " + lowerName + L" path=" + resolved);
        }

        {
            CSLockGuard lock(trackedCs_);
            auto it = trackedProcesses_.find(info.pid);
            if (it != trackedProcesses_.end()) {
                it->second->fullPath = resolved;
                it->second->needsPolicyRetry = false;
            }
        }
    }

    if (pidsToCheck.empty() && policyRetries.empty()) return;

    // Queue safety net checks for each tracked process
    for (DWORD pid : pidsToCheck) {
        if (stopRequested_.load()) break;
        EnqueueRequest(EnforcementRequest(pid, EnforcementRequestType::SAFETY_NET));
    }

    // Process the queue immediately (we're already in the control loop)
    ProcessEnforcementQueue();

    if (counting.count() > 0) {
#ifdef _DEBUG
        wchar_t dbgBuf[128];
        swprintf_s(dbgBuf, L"[PMR] HandleSafetyNetCheck arena fallback count=%zu", counting.count());
        LOG_DEBUG(dbgBuf);
#else
        static std::atomic<int> pmrWarnCount{0};
        if (pmrWarnCount.fetch_add(1, std::memory_order_relaxed) < 10) {
            LOG_INFO(L"[PMR] HandleSafetyNetCheck fallback detected");
        }
#endif
    }

    // §9.14-B: PendingRemoval overflow → immediate VerifyAndRepair
    // overflow flag は EnqueuePendingRemoval がセット → SafetyNet 10秒以内に即発火
    if (RegistryPolicyManager::Instance().ConsumePendingOverflowFlag()) {
        if (verifyRunning_.exchange(1, std::memory_order_acquire) == 0) {
            ULONGLONG now = GetTickCount64();
            lastPolicyVerifyTime_ = now;
            LOG_INFO(L"[SAFETY] PendingRemoval overflow — VerifyAndRepair immediate");
            RegistryPolicyManager::Instance().VerifyAndRepair();
            verifyRunning_.store(0, std::memory_order_release);
        }
    }

    // Registry policy integrity verification (30min periodic)
    {
        constexpr ULONGLONG POLICY_VERIFY_INTERVAL_MS = 30ULL * 60 * 1000;
        ULONGLONG now = GetTickCount64();
        if (now - lastPolicyVerifyTime_ >= POLICY_VERIFY_INTERVAL_MS) {
            if (verifyRunning_.exchange(1, std::memory_order_acquire) == 0) {
                lastPolicyVerifyTime_ = now;
                RegistryPolicyManager::Instance().VerifyAndRepair();
                verifyRunning_.store(0, std::memory_order_release);
            }
        }
    }

    // §9.14-E: SafetyNet missed-target scan trigger
    // トリガ1: CRITICAL ドロップ差分検出
    // トリガ2: 30 秒バックストップ（ETW silent drop 対策）
    {
        ULONGLONG now = GetTickCount64();
        const uint32_t currentDrops = criticalDropCount_.load(std::memory_order_relaxed);
        const bool hasCriticalDrop = (currentDrops != lastCheckedDropCount_);

        if (hasCriticalDrop) {
            // CRITICAL ドロップ検出 — drop count 基準値を更新してスキャン
            lastCheckedDropCount_ = currentDrops;
            lastSafetyScanTime_   = now;
            ScanRunningProcessesForMissedTargets(MAX_SAFETY_SCAN_PER_TICK);
        } else if ((now - lastSafetyScanTime_) >= SAFETY_SCAN_BACKSTOP_MS) {
            // バックストップ（30 秒周期フェイルセーフ、ETW silent drop 対策）
            // hasCriticalDrop=false のため lastCheckedDropCount_ は更新不要
            lastSafetyScanTime_ = now;
            ScanRunningProcessesForMissedTargets(MAX_SAFETY_SCAN_PER_TICK);
        }
    }
}

// §9.14-E: SafetyNet 2-pass round-robin scan for missed target processes.
// Triggered by CRITICAL drop detection or 30s backstop. Budget = scan count (not apply count).
// lastScannedPid_ is monotonically increasing (std::max) to prevent permanent starvation.
void EngineCore::ScanRunningProcessesForMissedTargets(int maxScan) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    ScopedSnapshot scoped = MakeScopedSnapshot(snap);

    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    if (!Process32FirstW(snap, &pe)) return;

    int scanned = 0;
    // lastScannedPid_ = 0 の場合は先頭からスキャン（初回 or pass2 折り返し後）
    // PID wrap（UINT32_MAX 到達）は実用上発生しないが、仮に起きても
    // 全エントリが <= lastScannedPid_ となり passedOffset 未到達 → pass2 発火で全件カバーされる
    bool passedOffset = (lastScannedPid_ == 0);

    // パス1: lastScannedPid_ 以降のエントリを処理
    do {
        DWORD pid = pe.th32ProcessID;
        if (!passedOffset) {
            if (pid > lastScannedPid_) passedOffset = true;
            else continue;
        }
        // std::max で単調増加を保証。snapshot が降順（高 PID 優先）の場合、
        // lastScannedPid_ = pid だと後退してループが永続するバグを防ぐ
        lastScannedPid_ = std::max(lastScannedPid_, pid);
        bool alreadyTracked;
        { CSLockGuard lk(trackedCs_); alreadyTracked = trackedProcesses_.count(pid) > 0; }
        if (!alreadyTracked && TryApplyIfMissedTarget(pid, pe.szExeFile))
            safetyRecoveredCount_.fetch_add(1, std::memory_order_relaxed);
        scanned++;  // budget はスキャン数で消費（追跡済みを含む全エントリ）
    } while (scanned < maxScan && Process32NextW(snap, &pe));

    // パス2: パス1で offset 未到達 かつ budget 残あり → 先頭から折り返し
    if (!passedOffset && scanned < maxScan) {
        lastScannedPid_ = 0;
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) {
            do {
                DWORD pid = pe.th32ProcessID;
                lastScannedPid_ = pid;
                bool alreadyTracked;
                { CSLockGuard lk(trackedCs_); alreadyTracked = trackedProcesses_.count(pid) > 0; }
                if (!alreadyTracked && TryApplyIfMissedTarget(pid, pe.szExeFile))
                    safetyRecoveredCount_.fetch_add(1, std::memory_order_relaxed);
                scanned++;
            } while (scanned < maxScan && Process32NextW(snap, &pe));
        }
    }
}

// §9.14-E: Check if a process is a missed target; apply optimization if so.
bool EngineCore::TryApplyIfMissedTarget(DWORD pid, const wchar_t* exeName) {
    if (!exeName || !*exeName) return false;
    if (IsCriticalProcess(exeName)) return false;

    std::wstring lowerName = ToLower(exeName);
    bool isNameTarget   = false;
    bool maybePathTarget = false;
    {
        CSLockGuard lock(targetCs_);
        isNameTarget    = (targetNameSet_.count(lowerName) > 0);
        maybePathTarget = hasPathTargets_.load(std::memory_order_relaxed) &&
                          (pathTargetFileNames_.count(lowerName) > 0);
    }

    if (!isNameTarget && !maybePathTarget) return false;

    // Delegate to existing ApplyOptimization — handles IsTracked check + path resolution
    return ApplyOptimization(pid, exeName, false, 0);
}

// Drain pending process removal queue (called from EngineControlLoop thread only)
void EngineCore::ProcessPendingRemovals() {
    constexpr size_t MAX_DRAIN_PER_TICK = 256;

    std::queue<DWORD> pending;
    bool hasRemaining = false;
    {
        CSLockGuard lock(pendingRemovalCs_);

        // §9.09 修正④: Backlog runaway detection
        size_t backlogSize = pendingRemovalPids_.size();
        if (backlogSize > 8192) {
            static std::atomic<int> runawayWarn{0};
            if (runawayWarn.fetch_add(1, std::memory_order_relaxed) < 20) {
                LOG_ALERT(L"[EVICT] pendingRemoval runaway growth");
            }
        }

        // Take up to MAX_DRAIN_PER_TICK items per tick (load leveling)
        size_t toTake = std::min(backlogSize, MAX_DRAIN_PER_TICK);
        for (size_t i = 0; i < toTake; ++i) {
            pending.push(pendingRemovalPids_.front());
            pendingRemovalPids_.pop();
        }

        // Snapshot remaining-items flag under lock.
        // Design: auto-reset event does NOT stay signaled — hasRemaining
        // ensures the next drain is scheduled when items remain after partial drain.
        // Push side (P1/P2/P3) guarantees SetEvent on empty→non-empty
        // transition under the same lock, so no item can be stranded.
        hasRemaining = !pendingRemovalPids_.empty();
    }

    // Signal OUTSIDE lock — minimizes lock hold time for push-side contention.
    // This guarantees: while items remain, the next drain is always scheduled.
    if (hasRemaining) {
        SetEvent(hWakeupEvent_);
    }

    while (!pending.empty()) {
        RemoveTrackedProcess(pending.front());
        pending.pop();
    }
}

// Schedule deferred verification timer (AGGRESSIVE phase)
// §9.14-C: std::exchange pattern ensures old timer/context are cancelled outside lock.
// Previous implementation overwrote deferredTimer/deferredTimerContext without cancellation,
// leaking the old handle and context when the same PID was re-scheduled.
void EngineCore::ScheduleDeferredVerification(DWORD pid, uint8_t step) {
    if (!timerQueue_) return;

    DWORD delayMs = static_cast<DWORD>(engine_logic::DeferredVerifyDelayMs(step, policy_));
    if (delayMs == 0) return;

    HANDLE oldTimer = nullptr;
    DeferredVerifyContext* oldCtx = nullptr;

    {
        CSLockGuard lock(trackedCs_);
        auto it = trackedProcesses_.find(pid);
        if (it == trackedProcesses_.end()) return;

        // Exchange: atomically swap out old timer/context, insert new ones
        // Old handles are deleted outside the lock (DeleteTimerQueueTimer may block)
        oldTimer = std::exchange(it->second->deferredTimer, nullptr);
        oldCtx   = std::exchange(it->second->deferredTimerContext, nullptr);

        auto* context = new DeferredVerifyContext{this, pid, step, it->second};
        HANDLE timer = nullptr;
        if (CreateTimerQueueTimer(&timer, timerQueue_, DeferredVerifyTimerCallback,
                                  context, delayMs, 0, WT_EXECUTEONLYONCE)) {
            it->second->deferredTimer        = timer;
            it->second->deferredTimerContext = context;
        } else {
            delete context;
        }
    }

    // Cancel old timer outside lock — INVALID_HANDLE_VALUE waits for in-flight callbacks
    // DeferredVerifyTimerCallback only calls EnqueueRequest (lock-free), so μs completion
    if (oldTimer) DeleteTimerQueueTimer(timerQueue_, oldTimer, INVALID_HANDLE_VALUE);
    if (oldCtx)   delete oldCtx;
}

// Cancel all timers for a process
// Extracts handles into caller-supplied vectors; caller deletes outside any lock.
void EngineCore::CancelProcessTimers(TrackedProcess& tp,
                                     std::vector<HANDLE>& timersToDelete,
                                     std::vector<DeferredVerifyContext*>& ctxToDelete) {
    if (tp.deferredTimer && timerQueue_) {
        timersToDelete.push_back(tp.deferredTimer);
        ctxToDelete.push_back(tp.deferredTimerContext);
        tp.deferredTimerContext = nullptr;
        tp.deferredTimer        = nullptr;
    }
    if (tp.persistentTimer && timerQueue_) {
        timersToDelete.push_back(tp.persistentTimer);
        ctxToDelete.push_back(tp.persistentTimerContext);
        tp.persistentTimerContext = nullptr;
        tp.persistentTimer        = nullptr;
    }
}

// Start persistent enforcement timer (5s recurring)
void EngineCore::StartPersistentTimer(DWORD pid) {
    if (!timerQueue_) return;

    HANDLE               oldTimerToDelete = nullptr;
    DeferredVerifyContext* oldCtxToDelete = nullptr;

    {
        CSLockGuard lock(trackedCs_);
        auto it = trackedProcesses_.find(pid);
        if (it == trackedProcesses_.end()) {
            return;
        }

        // Cancel existing persistent timer if any (extract handle; delete outside lock)
        if (it->second->persistentTimer) {
            oldTimerToDelete = it->second->persistentTimer;
            oldCtxToDelete   = it->second->persistentTimerContext;
            it->second->persistentTimer        = nullptr;
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

    // INVALID_HANDLE_VALUE: block until callback completes before freeing context
    if (oldTimerToDelete) {
        DeleteTimerQueueTimer(timerQueue_, oldTimerToDelete, INVALID_HANDLE_VALUE);
        delete oldCtxToDelete;
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
                [this](DWORD pid, DWORD parentPid, const std::wstring& imageName,
                       const std::wstring& imagePath) {
                    this->OnProcessStart(pid, parentPid, imageName, imagePath);
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

    // Process liveness check (every 60s) - detect zombie TrackedProcess entries
    // When OpenProcess(SYNCHRONIZE) fails in ApplyOptimization, no exit callback is registered.
    // These entries become zombies when the process exits. This scan detects and removes them.
    if (now - lastProcessLivenessCheck_ >= LIVENESS_CHECK_INTERVAL) {
        std::vector<DWORD> zombiePids;
        {
            CSLockGuard lock(trackedCs_);
            for (const auto& [pid, tp] : trackedProcesses_) {
                // Only check entries without a wait handle (no exit notification registered)
                if (tp->waitHandle != nullptr) continue;

                // Check if process is still alive
                HANDLE hProbe = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                if (hProbe) {
                    DWORD exitCode = 0;
                    bool exited = GetExitCodeProcess(hProbe, &exitCode) && exitCode != STILL_ACTIVE;
                    CloseHandle(hProbe);
                    if (exited) {
                        zombiePids.push_back(pid);
                    }
                } else {
                    // Cannot open process - it has exited
                    zombiePids.push_back(pid);
                }
            }
        }

        if (!zombiePids.empty()) {
            // Use pendingRemovalPids_ to safely remove via the normal path
            bool wasEmpty;
            {
                CSLockGuard lock(pendingRemovalCs_);
                wasEmpty = pendingRemovalPids_.empty();
                for (DWORD pid : zombiePids) {
                    pendingRemovalPids_.push(pid);
                }
            }
            if (wasEmpty) {
                SetEvent(hWakeupEvent_);
            }

            wchar_t logBuf[128];
            swprintf_s(logBuf, L"[LIVENESS] Detected %zu zombie entries - queued for removal",
                       zombiePids.size());
            LOG_DEBUG(logBuf);
        }
        lastProcessLivenessCheck_ = now;
    }

    // errorLogSuppression_ TTL cleanup (every 60s)
    if (now - lastSuppressionCleanup_ >= SUPPRESSION_CLEANUP_INTERVAL) {
        CSLockGuard lock(trackedCs_);
        const size_t before = errorLogSuppression_.size();
        for (auto it = errorLogSuppression_.begin(); it != errorLogSuppression_.end(); ) {
            if (now - it->second > SUPPRESSION_TTL) {
                it = errorLogSuppression_.erase(it);
            } else {
                ++it;
            }
        }
        // Emergency size cap: oldest-first eviction (clear() would cause log storm)
        while (errorLogSuppression_.size() > SUPPRESSION_MAX_SIZE) {
            errorLogSuppression_.erase(errorLogSuppression_.begin());
        }
        if (errorLogSuppression_.size() != before) {
            wchar_t buf[80];
            swprintf_s(buf, L"[MAINT] errSup: %zu -> %zu", before, errorLogSuppression_.size());
            LOG_DEBUG(buf);
        }
        lastSuppressionCleanup_ = now;
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
            uint32_t enfTotal = enforceCount_.load(std::memory_order_relaxed);
            uint32_t enfOk = enforceSuccessCount_.load(std::memory_order_relaxed);
            uint32_t enfFail = enforceFailCount_.load(std::memory_order_relaxed);
            uint32_t enfMaxUs = enforceLatencyMaxUs_.load(std::memory_order_relaxed);
            uint32_t enfAvgUs = (enfTotal > 0)
                ? static_cast<uint32_t>(enforceLatencySumUs_.load(std::memory_order_relaxed) / enfTotal)
                : 0;

            wchar_t statsBuf[512];
            swprintf_s(statsBuf,
                L"Stats: %zu tracked (A:%zu S:%zu P:%zu), %zu jobs, viol=%u, "
                L"wakeup(cfg:%u sn:%u enf:%u exit:%u), persist(apply:%u skip:%u), "
                L"enforce(total:%u ok:%u fail:%u avg:%uus max:%uus)",
                count, aggressiveCount, stableCount, persistentCount,
                jobCount, totalViolations_.load(),
                wakeupConfigChange_.load(), wakeupSafetyNet_.load(),
                wakeupEnforcementRequest_.load(), wakeupProcessExit_.load(),
                persistentEnforceApplied_.load(), persistentEnforceSkipped_.load(),
                enfTotal, enfOk, enfFail, enfAvgUs, enfMaxUs);
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

    // --- Leak diagnostics (interval: DIAG_LOG_INTERVAL_MS — 30s Debug / 120s Release) ---
    if (now - lastDiagLogTime_ >= DIAG_LOG_INTERVAL_MS) {
        size_t trackedSz   = 0;
        size_t watchMapSz  = 0;
        size_t deferCtxCnt = 0;
        size_t errSupSz    = 0;
        {
            CSLockGuard lock(trackedCs_);
            trackedSz  = trackedProcesses_.size();
            watchMapSz = waitContexts_.size();
            for (const auto& [pid, tp] : trackedProcesses_) {
                if (tp->deferredTimerContext != nullptr) ++deferCtxCnt;
            }
            errSupSz = errorLogSuppression_.size();
        }

        DWORD handleCount = 0;
        if (!GetProcessHandleCount(GetCurrentProcess(), &handleCount)) {
            handleCount = 0;
        }

        uint64_t regCnt    = waitRegisterCount_.load(std::memory_order_relaxed);
        uint64_t unregCnt  = waitUnregisterCount_.load(std::memory_order_relaxed);
        uint64_t unregFail = waitUnregisterFailures_.load(std::memory_order_relaxed);
        int64_t  waitDelta = static_cast<int64_t>(regCnt) - static_cast<int64_t>(unregCnt);

        wchar_t diagBuf[320];
        swprintf_s(diagBuf,
            L"[DIAG] wait(reg:%llu unreg:%llu fail:%llu delta:%lld) "
            L"tracked=%zu watchMap=%zu deferCtx=%zu errSup=%zu handles=%u",
            regCnt, unregCnt, unregFail, waitDelta,
            trackedSz, watchMapSz, deferCtxCnt, errSupSz, handleCount);
        LOG_DEBUG(diagBuf);

        lastDiagLogTime_ = now;
    }

    // §9.14-H: Queue + memory counters (60s cadence, piggybacks on DIAG_LOG_INTERVAL_MS)
    if (now - lastStatsLogTime_ < 1000) {  // Only emit if stats log just fired this tick (avoid double-log storm)
        // No-op: combined with stats above
    }
    {
        static ULONGLONG lastQueueDiagTime = 0;
        if (now - lastQueueDiagTime >= 60000) {
            lastQueueDiagTime = now;
            size_t critSz = 0, nonCritSz = 0;
            {
                CSLockGuard lock(queueCs_);
                critSz    = criticalQueue_.size();
                nonCritSz = nonCriticalQueue_.size();
            }
            wchar_t qBuf[320];
            swprintf_s(qBuf,
                L"[DIAG] crit=%zu nc=%zu pending=%d drop=%u critDrop=%u critEvict=%u recovered=%u tracked=%zu",
                critSz, nonCritSz,
                RegistryPolicyManager::Instance().GetPendingQueueSize(),
                enforcementDropCount_.load(std::memory_order_relaxed),
                criticalDropCount_.load(std::memory_order_relaxed),
                criticalEvictCount_.load(std::memory_order_relaxed),
                safetyRecoveredCount_.load(std::memory_order_relaxed),
                GetActiveProcessCount());
            LOG_DEBUG(qBuf);
        }
    }

    // [MEM] memory diagnostics (10s warmup / 60s steady)
    {
        ULONGLONG memInterval = (now - startTime_ < MEM_LOG_WARMUP_MS)
            ? MEM_LOG_INTERVAL_SHORT : MEM_LOG_INTERVAL_LONG;

        if (now - lastMemLogTime_ >= memInterval) {
            PROCESS_MEMORY_COUNTERS_EX pmc = {};
            pmc.cb = sizeof(pmc);
            DWORD handleCount = 0;
            bool pmcOk = GetProcessMemoryInfo(GetCurrentProcess(),
                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)) != FALSE;
            bool hcOk = GetProcessHandleCount(GetCurrentProcess(), &handleCount) != FALSE;
            (void)pmcOk; (void)hcOk;

            // コンテナサイズ（短時間ロック）
            size_t policySz = 0;
            size_t errSupSz = 0;
            {
                CSLockGuard lock(policySetCs_);
                policySz = policyCacheMap_.size();
            }
            {
                CSLockGuard lock(trackedCs_);
                errSupSz = errorLogSuppression_.size();
            }

            wchar_t memBuf[256];
            swprintf_s(memBuf,
                L"[MEM] pid=%u priv=%zu rss=%zu commit=%zu handles=%u policy=%zu errSup=%zu",
                GetCurrentProcessId(),
                pmc.PrivateUsage,
                pmc.WorkingSetSize,
                pmc.PagefileUsage,
                handleCount,
                policySz,
                errSupSz);
            LOG_DEBUG(memBuf);
            lastMemLogTime_ = now;
        }
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

    // Enforcement telemetry: start timing
    LARGE_INTEGER qpcStart, qpcEnd, qpcFreq;
    QueryPerformanceCounter(&qpcStart);
    QueryPerformanceFrequency(&qpcFreq);

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

        // Enforcement telemetry: record latency and failure
        QueryPerformanceCounter(&qpcEnd);
        uint64_t elapsedUs = ((qpcEnd.QuadPart - qpcStart.QuadPart) * 1000000ULL) / qpcFreq.QuadPart;
        enforceLatencySumUs_.fetch_add(elapsedUs, std::memory_order_relaxed);
        enforceCount_.fetch_add(1, std::memory_order_relaxed);
        uint32_t elapsed32 = static_cast<uint32_t>((std::min)(elapsedUs, static_cast<uint64_t>(UINT32_MAX)));
        uint32_t prevMax = enforceLatencyMaxUs_.load(std::memory_order_relaxed);
        while (elapsed32 > prevMax &&
               !enforceLatencyMaxUs_.compare_exchange_weak(prevMax, elapsed32, std::memory_order_relaxed)) {}
        enforceFailCount_.fetch_add(1, std::memory_order_relaxed);

        return false;
    }

    // Enforcement telemetry: record latency and success
    QueryPerformanceCounter(&qpcEnd);
    uint64_t elapsedUs = ((qpcEnd.QuadPart - qpcStart.QuadPart) * 1000000ULL) / qpcFreq.QuadPart;
    enforceLatencySumUs_.fetch_add(elapsedUs, std::memory_order_relaxed);
    enforceCount_.fetch_add(1, std::memory_order_relaxed);
    uint32_t elapsed32 = static_cast<uint32_t>((std::min)(elapsedUs, static_cast<uint64_t>(UINT32_MAX)));
    uint32_t prevMax = enforceLatencyMaxUs_.load(std::memory_order_relaxed);
    while (elapsed32 > prevMax &&
           !enforceLatencyMaxUs_.compare_exchange_weak(prevMax, elapsed32, std::memory_order_relaxed)) {}
    enforceSuccessCount_.fetch_add(1, std::memory_order_relaxed);

    // Record last enforcement timestamp (Unix Epoch milliseconds)
    {
        auto now = std::chrono::system_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        lastEnforceTimeMs_.store(static_cast<uint64_t>(ms), std::memory_order_relaxed);
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

// Cached version: avoids repeated NtQueryInformationProcess during thread burst
bool EngineCore::IsEcoQoSEnabledCached(TrackedProcess& tp, ULONGLONG now) {
    if (engine_logic::IsCacheValid(tp.ecoQosCached,
            static_cast<uint64_t>(now),
            static_cast<uint64_t>(tp.ecoQosCacheTime),
            static_cast<uint64_t>(ECOQOS_CACHE_DURATION))) {
        return tp.ecoQosCachedValue;
    }
    bool result = IsEcoQoSEnabled(tp.processHandle.get());
    tp.ecoQosCached = true;
    tp.ecoQosCachedValue = result;
    tp.ecoQosCacheTime = now;
    return result;
}

// === Registry Policy Application ===

bool EngineCore::ApplyRegistryPolicy(const std::wstring& exePath, const std::wstring& exeName) {
    UNLEAF_ASSERT_CANONICAL(exePath);

    // LRU cache check with registry validation
    {
        CSLockGuard lock(policySetCs_);
        auto it = policyCacheMap_.find(exePath);
        if (it != policyCacheMap_.end()) {
            if (RegistryPolicyManager::Instance().IsPolicyValid(exePath)) {
                policyCacheLru_.splice(policyCacheLru_.begin(), policyCacheLru_, it->second);
                return true;
            } else {
                policyCacheLru_.erase(it->second);
                policyCacheMap_.erase(it);
                LOG_DEBUG(L"[POLICY] LRU invalidated due to external drift: " + exePath);
            }
        }
    }

    bool success = RegistryPolicyManager::Instance().ApplyPolicy(exeName, exePath);

    if (success) {
        CSLockGuard lock(policySetCs_);
        // Evict LRU if at capacity
        if (policyCacheMap_.size() >= POLICY_CACHE_MAX_SIZE) {
            auto& oldest = policyCacheLru_.back();
            policyCacheMap_.erase(oldest);
            policyCacheLru_.pop_back();
        }
        policyCacheLru_.push_front(exePath);
        policyCacheMap_[exePath] = policyCacheLru_.begin();
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
        // §9.14-G: Immediate size cap — oldest-first eviction on insert to bound map growth.
        // errorLogSuppression_ は std::map<pair<DWORD,DWORD>, ULONGLONG>（順序付き）
        // erase(begin()) は最小キー（最古 PID+errorCode ペア）を O(log N) で削除
        while (errorLogSuppression_.size() > SUPPRESSION_MAX_SIZE / 2)
            errorLogSuppression_.erase(errorLogSuppression_.begin());
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
                waitRegisterCount_.fetch_add(1, std::memory_order_relaxed);
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
            waitUnregisterFailures_.fetch_add(1, std::memory_order_relaxed);
        } else {
            waitUnregisterCount_.fetch_add(1, std::memory_order_relaxed);
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

    std::set<std::wstring> localNameTargets;
    std::set<std::wstring> localPathTargets;
    std::set<std::wstring> localPathFileNames;
    bool pathTargetsActive = false;
    {
        CSLockGuard lock(targetCs_);
        localNameTargets   = targetNameSet_;
        localPathTargets   = targetPathSet_;
        localPathFileNames = pathTargetFileNames_;
        pathTargetsActive  = !localPathTargets.empty();
    }

    PROCESSENTRY32W pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32W);

    if (Process32FirstW(scopedSnapshot.get(), &pe32)) {
        do {
            DWORD pid = pe32.th32ProcessID;
            const std::wstring name(pe32.szExeFile);
            DWORD parentPid = pe32.th32ParentProcessID;

            if (IsTracked(pid)) continue;
            if (IsCriticalProcess(name)) continue;

            std::wstring lowerName = ToLower(name);
            bool isNameTarget = (localNameTargets.count(lowerName) > 0);
            bool isChild = IsTrackedParent(parentPid);

            if (isNameTarget || isChild) {
                ApplyOptimization(pid, name, isChild, parentPid);
                continue;
            }

            // Path-based check
            if (pathTargetsActive && localPathFileNames.count(lowerName) > 0) {
                DWORD access = PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_INFORMATION;
                HANDLE hProc = OpenProcess(access, FALSE, pid);
                if (hProc) {
                    ScopedHandle scoped = MakeScopedHandle(hProc);
                    std::wstring fullPath = ResolveProcessPath(scoped.get());
                    if (!fullPath.empty() && localPathTargets.count(fullPath) > 0) {
                        ApplyOptimizationWithHandle(pid, name, false, 0,
                                                     std::move(scoped), fullPath);
                    }
                }
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
    std::set<std::wstring> localNameTargets;
    std::set<std::wstring> localPathTargets;
    std::set<std::wstring> localPathFileNames;
    bool pathTargetsActive = false;
    {
        CSLockGuard lock(targetCs_);
        localNameTargets   = targetNameSet_;
        localPathTargets   = targetPathSet_;
        localPathFileNames = pathTargetFileNames_;
        pathTargetsActive  = !localPathTargets.empty();
    }

    // Phase A: name-based scan (unchanged behavior)
    for (const auto& [pid, info] : processMap) {
        std::wstring lowerName = ToLower(info.name);

        if (localNameTargets.find(lowerName) != localNameTargets.end()) {
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

    // Phase B: path-based scan (only when path targets configured)
    if (pathTargetsActive) {
        for (const auto& [pid, info] : processMap) {
            if (IsTracked(pid)) continue;
            if (IsCriticalProcess(info.name)) continue;
            // Skip processes already matched by name
            if (localNameTargets.count(ToLower(info.name))) continue;
            // Pre-filter: skip if exe name not in any path target
            if (localPathFileNames.count(ToLower(info.name)) == 0) continue;

            // Open with full permissions needed for optimization
            DWORD access = PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_INFORMATION;
            HANDLE hProc = OpenProcess(access, FALSE, pid);
            if (!hProc) continue;
            ScopedHandle scoped = MakeScopedHandle(hProc);

            std::wstring fullPath = ResolveProcessPath(scoped.get());
            if (fullPath.empty()) continue;

            if (localPathTargets.count(fullPath) > 0) {
                // Hand off handle — no second OpenProcess
                ApplyOptimizationWithHandle(pid, info.name, false, 0,
                                             std::move(scoped), fullPath);

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
}

bool EngineCore::ApplyOptimization(DWORD pid, const std::wstring& name, bool isChild,
                                    DWORD parentPid, const std::wstring& preResolvedPath) {
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
        wchar_t logBuf[256];
        swprintf_s(logBuf, L"[SKIP] %s (PID:%lu) OpenProcess failed (error=%lu)",
                   name.c_str(), pid, GetLastError());
        LOG_DEBUG(logBuf);
        return false;
    }

    ScopedHandle scopedHandle = MakeScopedHandle(hProcess);

    // Resolve full path: detect NT device paths from ETW and route appropriately
    auto isNtDevicePath = [](const std::wstring& p) {
        return p.size() > 8 &&
               (p.rfind(L"\\Device\\", 0) == 0 ||
                p.rfind(L"\\??\\", 0) == 0);
    };

    std::wstring resolvedPath;

    if (!preResolvedPath.empty() && !isNtDevicePath(preResolvedPath)) {
        // Safe Win32 path — CanonicalizePath で正規化
        resolvedPath = CanonicalizePath(preResolvedPath);
    } else {
        // NT デバイスパスまたは preResolvedPath 空 → ハンドルベース解決
        resolvedPath = ResolveProcessPath(scopedHandle.get());
        if (!resolvedPath.empty()) {
            resolvedPath = CanonicalizePath(resolvedPath);
        }

        if (resolvedPath.empty()) {
            wchar_t diagBuf[512];
            swprintf_s(diagBuf,
                L"[DIAG] ApplyOptimization: path resolution failed for %s (PID:%lu) hint=%s",
                name.c_str(), pid,
                preResolvedPath.empty() ? L"(empty)" : L"(NT device path)");
            LOG_DEBUG(diagBuf);
        }
    }

    return ApplyOptimizationWithHandle(pid, name, isChild, parentPid,
                                        std::move(scopedHandle), resolvedPath);
}

bool EngineCore::ApplyOptimizationWithHandle(DWORD pid, const std::wstring& name,
                                              bool isChild, DWORD parentPid,
                                              ScopedHandle&& scopedHandle,
                                              const std::wstring& resolvedPath) {
    // Guard: skip if already tracked or critical (may be called directly from TryApplyByPath)
    if (IsTracked(pid)) return false;
    if (IsCriticalProcess(name)) return false;

    ULONGLONG now = GetTickCount64();

    // DESIGN CONTRACT:
    // EngineCore guarantees canonicalized paths via CanonicalizePath().
    // RegistryPolicyManager assumes canonical input.
    // NormalizePath is defensive only and must not be relied upon for primary normalization.
    //
    // DO NOT:
    // - Rely on NormalizePath as primary normalization
    // - Pass raw/unresolved paths into RegistryPolicyManager
    //
    // Registry policy application strategy (§9.14):
    // - name-only target: proactive applied IFEO only (no path); PowerThrottle must be applied
    //   unconditionally on first detection — HasPolicy would always be false but we make intent explicit.
    // - path-based target: proactive applied full policy when file existed; fallback via HasPolicy check.
    {
        std::wstring lowerName = ToLower(name);
        bool isNameOnlyTarget = false;
        {
            CSLockGuard lock(targetCs_);
            isNameOnlyTarget = (targetNameSet_.count(lowerName) > 0);
        }

        if (!resolvedPath.empty()) {
            if (isNameOnlyTarget) {
                // name-only: proactive had no path, apply full policy now (IFEO idempotent + PT new)
                RegistryPolicyManager::Instance().ApplyPolicy(lowerName, resolvedPath);
                LOG_DEBUG(L"[REGISTRY] Name-only policy applied: " + lowerName);
            } else if (!RegistryPolicyManager::Instance().HasPolicy(resolvedPath)) {
                // path-based fallback: file didn't exist at startup, apply now
                RegistryPolicyManager::Instance().ApplyPolicy(lowerName, resolvedPath);
                LOG_DEBUG(L"[REGISTRY] Path-based fallback policy applied: " + lowerName);
            }
        } else {
            wchar_t diagBuf[256];
            swprintf_s(diagBuf,
                L"[DIAG] ApplyOptimizationWithHandle: empty path, policy deferred for %s (PID:%lu)",
                name.c_str(), pid);
            LOG_DEBUG(diagBuf);
        }
    }

    bool success = PulseEnforceV6(scopedHandle.get(), pid, true);

    // Log the optimization with path
    wchar_t optBuf[512];
    swprintf_s(optBuf, L"[TRACK] %s %s (PID: %lu) Child=%d path=%s",
               isChild ? L"[CHILD]" : L"[TARGET]", name.c_str(), pid, isChild ? 1 : 0,
               resolvedPath.empty() ? L"(unresolved)" : resolvedPath.c_str());
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
    tracked->fullPath = resolvedPath;  // cached path (empty if unresolved)
    tracked->needsPolicyRetry = resolvedPath.empty();
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
            waitRegisterCount_.fetch_add(1, std::memory_order_relaxed);
        } else {
            delete context;
            context = nullptr;
            CloseHandle(hWaitProcess);
        }
    }
    // If SYNCHRONIZE fails, process exit will be detected by handle invalidation

    // Add to tracked map
    // §9.02: Candidate selection only; all cleanup delegated to RemoveTrackedProcess().
    // §8.38 compliance: no timer deletion inside lock. No erase of trackedProcesses_ or waitContexts_.
    std::vector<DWORD> evictList;
    {
        CSLockGuard lock(trackedCs_);

        size_t currentSize = trackedProcesses_.size();
        // §9.14-F: Simplified eviction — always select candidates when cap is reached.
        // Evict up to 16 per insertion (+1 accounts for the process being inserted this call).
        // Eliminates the period-skew counter; SelectEvictionCandidates handles N distinct PIDs.
        if (currentSize >= MAX_TRACKED_PROCESSES) {
            size_t excess  = currentSize - MAX_TRACKED_PROCESSES + 1;
            size_t toEvict = std::min(excess, static_cast<size_t>(16));
            evictList = SelectEvictionCandidates(trackedProcesses_, toEvict);
        }

        trackedProcesses_[pid] = std::move(tracked);
        if (context) {
            waitContexts_[pid] = context;
        }
    }

    // Queue evicted PIDs for full cleanup via RemoveTrackedProcess() (outside trackedCs_ — §8.38)
    if (!evictList.empty()) {
        // §9.03: Throttle log to avoid spam under sustained eviction pressure
        static std::atomic<int> evictLogCount{0};
        for (DWORD evictPid : evictList) {
            if (evictLogCount.fetch_add(1, std::memory_order_relaxed) < 50) {
                wchar_t logBuf[128];
                swprintf_s(logBuf, L"[EVICT] cap reached, evicting PID:%lu", evictPid);
                LOG_DEBUG(logBuf);
            }
        }

        bool wasEmpty;
        {
            CSLockGuard lock(pendingRemovalCs_);
            wasEmpty = pendingRemovalPids_.empty();
            for (DWORD evictPid : evictList) {
                size_t pendingSize = pendingRemovalPids_.size();
                if (pendingSize >= MAX_PENDING_REMOVALS) {
                    // §9.09 修正①: Drop 禁止 — eviction 作業を絶対に失わない
                    static std::atomic<int> dropWarnCount{0};
                    if (dropWarnCount.fetch_add(1, std::memory_order_relaxed) < 20) {
                        LOG_ALERT(L"[EVICT] pendingRemoval overflow (no drop)");
                    }
                } else if (pendingSize >= (MAX_PENDING_REMOVALS * 3 / 4)) {
                    // §9.04: Critical backlog — warn only (still push to ensure RemoveTrackedProcess runs)
                    static std::atomic<int> criticalWarnCount{0};
                    if (criticalWarnCount.fetch_add(1, std::memory_order_relaxed) < 20) {
                        LOG_ALERT(L"[EVICT] backlog critical");
                    }
                } else if (pendingSize >= (MAX_PENDING_REMOVALS / 2)) {
                    static std::atomic<int> backlogWarnCount{0};
                    if (backlogWarnCount.fetch_add(1, std::memory_order_relaxed) < 20) {
                        LOG_INFO(L"[EVICT] pendingRemoval backlog high");
                    }
                }
                // §9.06追加: Always push unconditionally — dedup removed (O(N) lock contention risk).
                // RemoveTrackedProcess() is idempotent; duplicate calls are safe.
                pendingRemovalPids_.push(evictPid);
            }
        }
        if (wasEmpty) {
            SetEvent(hWakeupEvent_);
        }
    }

    // Schedule deferred verification
    ScheduleDeferredVerification(pid, 1);

    return success;
}

// §9.00: Select eviction candidate from trackedProcesses_.
// Must be called while holding trackedCs_.
// Priority 1: invalid processHandle (zombie/terminated).
// Priority 2: oldest phaseStartTime (longest-resident process).
DWORD EngineCore::SelectEvictionCandidate() const {
#ifdef _DEBUG
    assert(IsCSHeldByCurrent(trackedCs_) &&
           "SelectEvictionCandidate() must be called with trackedCs_ held");
#endif
    if (trackedProcesses_.empty()) return 0;

    // Priority 1: zombie (handle already closed)
    for (const auto& [pid, tp] : trackedProcesses_) {
        if (!tp->processHandle.get()) return pid;
    }

    // Priority 2: oldest phaseStartTime
    auto it = std::min_element(
        trackedProcesses_.begin(),
        trackedProcesses_.end(),
        [](const auto& a, const auto& b) {
            return a.second->phaseStartTime < b.second->phaseStartTime;
        });

    return (it != trackedProcesses_.end()) ? it->first : 0;
}

void CALLBACK EngineCore::OnProcessExit(PVOID lpParameter, BOOLEAN timerOrWaitFired) {
    (void)timerOrWaitFired;
    auto* context = static_cast<WaitCallbackContext*>(lpParameter);
    if (!context || !context->engine) return;

    EngineCore* engine = context->engine;
    const DWORD pid = context->pid;

    // RAII concurrent guard — entered = callback開始時点の並行度
    struct ConcurrentGuard {
        std::atomic<uint32_t>& c;
        uint32_t entered;
        explicit ConcurrentGuard(std::atomic<uint32_t>& c) : c(c) {
            entered = c.fetch_add(1, std::memory_order_relaxed) + 1;
        }
        ~ConcurrentGuard() { c.fetch_sub(1, std::memory_order_relaxed); }
    } guard(engine->callbackConcurrent_);

    // QPC start — engine->qpcFreq_ を参照: callback内ロックゼロ
    LARGE_INTEGER qpcStart;
    QueryPerformanceCounter(&qpcStart);

    // 本体 — 例外安全性は設計で保証 (CSLockGuard は RAII, queue::push は noexcept 相当)
    bool wasEmpty;
    {
        CSLockGuard lock(engine->pendingRemovalCs_);
        wasEmpty = engine->pendingRemovalPids_.empty();
        engine->pendingRemovalPids_.push(pid);
    }
    if (wasEmpty && !engine->stopRequested_.load(std::memory_order_acquire)) {
        HANDLE h = engine->hWakeupEvent_;
        if (h) SetEvent(h);
    }

    // 閾値超過時のみログ — 正常パスはゼロコスト
    if (engine->qpcFreq_.QuadPart > 0) {
        LARGE_INTEGER qpcEnd;
        QueryPerformanceCounter(&qpcEnd);
        const uint64_t elapsedUs = static_cast<uint64_t>(
            (qpcEnd.QuadPart - qpcStart.QuadPart) * 1000000LL / engine->qpcFreq_.QuadPart);
        if (elapsedUs > CALLBACK_LATENCY_WARN_US) {
            wchar_t buf[128];
            swprintf_s(buf,
                L"[CB_SLOW] pid=%lu elapsed=%lluus concurrent=%u",
                pid, elapsedUs, guard.entered);
            LOG_DEBUG(buf);
        }
    }
}

// Safely remove tracked process with proper wait handle cleanup
void EngineCore::RemoveTrackedProcess(DWORD pid) {
    HANDLE waitHandleToUnregister  = nullptr;
    WaitCallbackContext* contextToDelete   = nullptr;
    DeferredVerifyContext* timerCtxToDelete    = nullptr;
    DeferredVerifyContext* deferredCtxToDelete = nullptr;
    HANDLE deferredTimerToDelete   = nullptr;
    HANDLE persistentTimerToDelete = nullptr;

    {
        CSLockGuard lock(trackedCs_);

        auto it = trackedProcesses_.find(pid);
        if (it != trackedProcesses_.end()) {
            // Extract timer handles for deletion outside lock
            if (it->second->deferredTimer && timerQueue_) {
                deferredTimerToDelete            = it->second->deferredTimer;
                deferredCtxToDelete              = it->second->deferredTimerContext;
                it->second->deferredTimerContext = nullptr;
                it->second->deferredTimer        = nullptr;
            }
            if (it->second->persistentTimer && timerQueue_) {
                persistentTimerToDelete             = it->second->persistentTimer;
                timerCtxToDelete                    = it->second->persistentTimerContext;
                it->second->persistentTimerContext  = nullptr;
                it->second->persistentTimer         = nullptr;
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

    // Delete timers outside lock: INVALID_HANDLE_VALUE blocks until in-flight
    // callback completes, then context is freed — must not hold trackedCs_ here.
    if (deferredTimerToDelete) {
        if (!DeleteTimerQueueTimer(timerQueue_, deferredTimerToDelete, INVALID_HANDLE_VALUE)) {
            DWORD err = GetLastError();
            if (err != ERROR_IO_PENDING) { shutdownWarnings_.fetch_add(1); }
        }
    }
    if (persistentTimerToDelete) {
        if (!DeleteTimerQueueTimer(timerQueue_, persistentTimerToDelete, INVALID_HANDLE_VALUE)) {
            DWORD err = GetLastError();
            if (err != ERROR_IO_PENDING) { shutdownWarnings_.fetch_add(1); }
        }
    }

    // Unregister outside lock with INVALID_HANDLE_VALUE to wait for callback completion
    if (waitHandleToUnregister) {
        if (!UnregisterWaitEx(waitHandleToUnregister, INVALID_HANDLE_VALUE)) {
            shutdownWarnings_.fetch_add(1);
            waitUnregisterFailures_.fetch_add(1, std::memory_order_relaxed);
        } else {
            waitUnregisterCount_.fetch_add(1, std::memory_order_relaxed);
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
    return engine_logic::IsTargetProcess(ToLower(name), targetNameSet_);
}

bool EngineCore::IsTargetPath(const std::wstring& fullPathLower) const {
    CSLockGuard lock(targetCs_);
    return engine_logic::IsTargetByPath(fullPathLower, targetPathSet_, targetNameSet_);
}

bool EngineCore::HasPathTargets() const {
    return hasPathTargets_.load(std::memory_order_acquire);
}

// static — delegates to unleaf::CanonicalizePath (types.h)
std::wstring EngineCore::CanonicalizePath(const std::wstring& rawPath) {
    return unleaf::CanonicalizePath(rawPath);
}

// static
bool EngineCore::FileExistsW(const std::wstring& path) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    return (attrs != INVALID_FILE_ATTRIBUTES);
}

std::wstring EngineCore::ResolveProcessPath(HANDLE hProcess) const {
    wchar_t rawBuf[MAX_PATH + 1] = {};
    DWORD rawSize = MAX_PATH;
    if (!QueryFullProcessImageNameW(hProcess, 0, rawBuf, &rawSize)) {
        LOG_DEBUG(L"[DIAG] ResolveProcessPath: QueryFullProcessImageNameW failed, error="
                  + std::to_wstring(GetLastError()));
        return L"";
    }

    std::wstring rawPath(rawBuf, rawSize);

    // For running processes, use GetFinalPathNameByHandleW for symlink resolution
    HANDLE hFile = CreateFileW(
        rawPath.c_str(), 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr);

    if (hFile == INVALID_HANDLE_VALUE) {
        // Process file inaccessible — fall back to CanonicalizePath
        return CanonicalizePath(rawPath);
    }

    wchar_t buf[MAX_PATH + 4] = {};
    DWORD len = GetFinalPathNameByHandleW(
        hFile, buf, MAX_PATH + 4,
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(hFile);

    if (len == 0 || len > MAX_PATH) {
        return CanonicalizePath(rawPath);
    }

    std::wstring result(buf, len);

    // \\?\UNC\server\share -> \\server\share
    if (result.size() >= 8 &&
        result[0] == L'\\' && result[1] == L'\\' &&
        result[2] == L'?'  && result[3] == L'\\' &&
        (result[4] == L'U' || result[4] == L'u') &&
        (result[5] == L'N' || result[5] == L'n') &&
        (result[6] == L'C' || result[6] == L'c') &&
        result[7] == L'\\') {
        result = L"\\\\" + result.substr(8);
    }
    // \\?\ -> remove prefix
    else if (result.size() >= 4 &&
             result[0] == L'\\' && result[1] == L'\\' &&
             result[2] == L'?'  && result[3] == L'\\') {
        result = result.substr(4);
    }

    // Lowercase
    for (auto& ch : result) ch = towlower(ch);

    return result;
}

void EngineCore::TryApplyByPath(DWORD pid, const std::wstring& name) {
    if (IsTracked(pid)) return;
    if (IsCriticalProcess(name)) return;

    DWORD access = PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_INFORMATION;
    HANDLE hProc = OpenProcess(access, FALSE, pid);
    if (!hProc) {
        LOG_DEBUG(L"[PATH] TryApplyByPath: OpenProcess failed for " + name);
        return;
    }
    ScopedHandle scoped = MakeScopedHandle(hProc);

    std::wstring resolvedPath = ResolveProcessPath(scoped.get());
    if (!resolvedPath.empty()) {
        resolvedPath = CanonicalizePath(resolvedPath);
    }
    if (resolvedPath.empty()) {
        LOG_DEBUG(L"[PATH] TryApplyByPath: path resolution failed for " + name);
        return;
    }

    if (!IsTargetPath(resolvedPath)) {
        // Symlink/junction で CanonicalizePath と GetFinalPathNameByHandleW が
        // 異なるパスを返す場合のフォールバック。
        // ファイル名が pathTargetFileNames_ に含まれていれば続行。
        // HasPolicy fallback in ApplyOptimizationWithHandle が正しいパスで PT を作成する。
        std::wstring lowerName = ToLower(name);
        bool fileNameMatch = false;
        {
            CSLockGuard lock(targetCs_);
            fileNameMatch = (pathTargetFileNames_.count(lowerName) > 0);
        }
        if (!fileNameMatch) return;
        LOG_DEBUG(L"[PATH] TryApplyByPath: exact path mismatch, filename fallback: " + resolvedPath);
    }

    // Delegate with the already-open handle (no second OpenProcess)
    ApplyOptimizationWithHandle(pid, name, false, 0,
                                 std::move(scoped), resolvedPath);
}

bool EngineCore::IsTrackedParent(DWORD parentPid) const {
    CSLockGuard lock(trackedCs_);
    auto it = trackedProcesses_.find(parentPid);
    return it != trackedProcesses_.end();
}

void EngineCore::RefreshTargetSet() {
    // Collect path targets outside the lock
    struct ResolvedEntry {
        std::wstring normalized;
        std::wstring fileName;
    };
    std::vector<ResolvedEntry> pathEntries;
    std::vector<std::wstring> nameEntries;

    {
        const auto& targets = UnLeafConfig::Instance().GetTargets();
        for (const auto& target : targets) {
            if (!target.enabled) continue;
            if (IsPathEntry(target.name)) {
                std::wstring norm = CanonicalizePath(target.name);
                if (norm.empty()) {
                    wchar_t logBuf[512];
                    swprintf_s(logBuf,
                               L"[REGISTRY] Target path canonicalization failed, skipping: %s",
                               target.name.c_str());
                    LOG_ALERT(logBuf);
                } else {
                    ResolvedEntry e;
                    e.normalized = norm;
                    e.fileName   = ExtractFileName(norm);
                    pathEntries.push_back(std::move(e));
                }
            } else {
                nameEntries.push_back(ToLower(target.name));
            }
        }
    }

    // Update sets under lock
    CSLockGuard lock(targetCs_);
    targetNameSet_.clear();
    targetPathSet_.clear();
    pathTargetFileNames_.clear();

    for (auto& name : nameEntries) {
        targetNameSet_.insert(std::move(name));
    }
    for (auto& e : pathEntries) {
        targetPathSet_.insert(e.normalized);
        pathTargetFileNames_.insert(e.fileName);
    }

    hasPathTargets_.store(!targetPathSet_.empty(), std::memory_order_release);
}

void EngineCore::ApplyProactivePolicies() {
    const auto& targets = UnLeafConfig::Instance().GetTargets();

    std::set<std::wstring> desiredNames;
    std::set<std::wstring> desiredPaths;

    for (const auto& target : targets) {
        if (!target.enabled) continue;

        if (IsPathEntry(target.name)) {
            // Path target: canonicalize path (no file handle needed)
            std::wstring resolved = CanonicalizePath(target.name);
            if (resolved.empty()) {
                LOG_ALERT(L"[REGISTRY] Proactive: path canonicalization failed, skipping: " + target.name);
                continue;
            }

            std::wstring exeName = ExtractFileName(resolved);
            std::wstring lowerName = ToLower(exeName);

            if (FileExistsW(resolved)) {
                // File exists: apply full policy (IFEO + PowerThrottle)
                // NOTE: IFEO is exe-name-based and applies globally to ALL instances of this exe name,
                // not just the specific path. This is by design — IFEO operates at the OS level
                // per executable name. Path-specific differentiation is handled by PowerThrottle only.
                RegistryPolicyManager::Instance().ApplyPolicy(lowerName, resolved);
                desiredPaths.insert(resolved);
            } else {
                // File does not exist (not yet installed, portable app not plugged in, etc.)
                // Apply IFEO only — PowerThrottle requires a valid path in registry.
                // Full policy will be applied via ETW fallback (修正③) when the process starts.
                RegistryPolicyManager::Instance().ApplyIFEOOnly(lowerName);
                LOG_ALERT(L"[REGISTRY] Proactive: file not found, IFEO only: " + resolved);
            }
            desiredNames.insert(lowerName);
        } else {
            // Name-only target: apply IFEO only (no path for PowerThrottle)
            std::wstring lowerName = ToLower(target.name);
            RegistryPolicyManager::Instance().ApplyIFEOOnly(lowerName);
            desiredNames.insert(lowerName);
        }
    }

    // Reconcile: remove policies no longer in config
    RegistryPolicyManager::Instance().ReconcileWithConfig(desiredNames, desiredPaths);

    wchar_t buf[128];
    swprintf_s(buf, L"[REGISTRY] Proactive policies applied: %zu names + %zu paths",
               desiredNames.size(), desiredPaths.size());
    LOG_INFO(buf);
}

void EngineCore::CleanupRemovedTargets() {
    // Get current valid targets
    std::set<std::wstring> localNameTargets;
    std::set<std::wstring> localPathTargets;
    bool pathTargetsActive = false;
    {
        CSLockGuard lock(targetCs_);
        localNameTargets  = targetNameSet_;
        localPathTargets  = targetPathSet_;
        pathTargetsActive = !localPathTargets.empty();
    }

    // Collect PIDs of root targets that are no longer in the target list
    std::vector<DWORD> toRemove;

    {
        CSLockGuard lock(trackedCs_);

        // Collect processes to remove
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
                // Root process validity check
                if (!tp->fullPath.empty() && pathTargetsActive) {
                    // Path-resolved process: path match is the sole criterion (path-absolute priority)
                    bool pathMatch = (localPathTargets.count(tp->fullPath) > 0);
                    if (!pathMatch) shouldRemove = true;
                } else {
                    // No resolved path or no path targets: fall back to name match
                    std::wstring lowerName = ToLower(tp->name);
                    if (localNameTargets.find(lowerName) == localNameTargets.end()) {
                        shouldRemove = true;
                    }
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

    // Phase breakdown + active process details
    {
        CSLockGuard lock(trackedCs_);
        for (const auto& [pid, tp] : trackedProcesses_) {
            switch (tp->phase) {
                case ProcessPhase::AGGRESSIVE: info.aggressiveCount++; break;
                case ProcessPhase::STABLE:     info.stableCount++; break;
                case ProcessPhase::PERSISTENT: info.persistentCount++; break;
            }

            ActiveProcessDetail detail;
            detail.pid = tp->pid;
            detail.name = tp->name;
            switch (tp->phase) {
                case ProcessPhase::AGGRESSIVE: detail.phase = "AGGRESSIVE"; break;
                case ProcessPhase::STABLE:     detail.phase = "STABLE"; break;
                case ProcessPhase::PERSISTENT: detail.phase = "PERSISTENT"; break;
            }
            detail.violations = tp->violationCount;
            detail.isChild = tp->isChild;
            info.activeProcessDetails.push_back(std::move(detail));
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

    // Enforcement telemetry
    {
        uint32_t count = enforceCount_.load(std::memory_order_relaxed);
        info.totalEnforcements = count;
        info.enforceSuccessCount = enforceSuccessCount_.load(std::memory_order_relaxed);
        info.enforceFailCount = enforceFailCount_.load(std::memory_order_relaxed);
        info.enforceLatencyMaxUs = enforceLatencyMaxUs_.load(std::memory_order_relaxed);
        info.enforceLatencyAvgUs = (count > 0)
            ? static_cast<uint32_t>(enforceLatencySumUs_.load(std::memory_order_relaxed) / count)
            : 0;
    }

    // Shutdown warnings
    info.shutdownWarnings = shutdownWarnings_.load();

    // Error counters
    info.error5Count = error5Count_.load();
    info.error87Count = error87Count_.load();

    // Config monitoring
    info.configChangeDetected = configChangeDetected_.load();
    info.configReloadCount = configReloadCount_.load();

    // Queue optimization
    info.etwThreadDeduped = etwThreadDeduped_.load(std::memory_order_relaxed);

    // Last enforcement timestamp
    info.lastEnforceTimeMs = lastEnforceTimeMs_.load(std::memory_order_relaxed);

    return info;
}

} // namespace unleaf
