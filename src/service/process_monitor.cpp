// UnLeaf - ETW Process Monitor Implementation

#include "process_monitor.h"
#include "../common/logger.h"
#include <tdh.h>

// Fallback: EVENT_TRACE_TYPE_LOST_EVENT may not be defined in all SDK versions
#ifndef EVENT_TRACE_TYPE_LOST_EVENT
static constexpr UCHAR EVENT_TRACE_TYPE_LOST_EVENT = 0x20;
#endif

#pragma comment(lib, "tdh.lib")

namespace unleaf {

// Static instance for callback routing
std::atomic<ProcessMonitor*> ProcessMonitor::instance_{nullptr};

// Microsoft-Windows-Kernel-Process provider GUID
// {22FB2CD6-0E7B-422B-A0C7-2FAD1FD0E716}
static const GUID KernelProcessProviderGuid = {
    0x22FB2CD6, 0x0E7B, 0x422B,
    {0xA0, 0xC7, 0x2F, 0xAD, 0x1F, 0xD0, 0xE7, 0x16}
};

// Event IDs
static constexpr USHORT EVENT_ID_PROCESS_START = 1;
static constexpr USHORT EVENT_ID_PROCESS_STOP = 2;
static constexpr USHORT EVENT_ID_THREAD_START = 3;

ProcessMonitor::ProcessMonitor()
    : sessionHandle_(0)
    , traceHandle_(INVALID_PROCESSTRACE_HANDLE)
    , running_(false)
    , stopRequested_(false)
    , lastEventTime_(0)
    , startTime_(0)
    , eventCount_(0)
    , lostEventCount_(0)
    , sessionHealthy_(false)
    , lastCheckedLost_(0)
    , lastTraceCheckTime_(0)
    , cachedTraceAlive_(true) {

    // Generate unique session name
    sessionName_ = L"UnLeafProcessMonitor_" + std::to_wstring(GetCurrentProcessId());
}

ProcessMonitor::~ProcessMonitor() {
    Stop();
}

bool ProcessMonitor::Start(ProcessStartCallback processCallback, ThreadStartCallback threadCallback) {
    if (running_.load()) {
        return true;  // Already running
    }

    processCallback_ = processCallback;
    threadCallback_ = threadCallback;
    instance_.store(this, std::memory_order_release);
    stopRequested_ = false;

    // Reset session state for clean start (prevents false starvation after restart)
    eventCount_ = 0;
    lostEventCount_ = 0;
    lastEventTime_ = 0;
    startTime_ = GetTickCount64();
    lastCheckedLost_ = 0;
    lastTraceCheckTime_ = 0;
    cachedTraceAlive_ = true;

    // Allocate properties buffer
    std::vector<BYTE> propertiesBuffer(PROPERTIES_BUFFER_SIZE, 0);
    auto* properties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(propertiesBuffer.data());

    properties->Wnode.BufferSize = static_cast<ULONG>(PROPERTIES_BUFFER_SIZE);
    properties->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    properties->Wnode.ClientContext = 1;  // QPC clock resolution
    properties->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    properties->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    properties->LogFileNameOffset = 0;
    // ETW session buffer configuration (BufferSize must be a multiple of 4KB)
    properties->BufferSize     = ETW_BUFFER_SIZE_KB;
    properties->MinimumBuffers = ETW_MIN_BUFFERS;
    properties->MaximumBuffers = ETW_MAX_BUFFERS;
    properties->FlushTimer     = ETW_FLUSH_TIMER;

    // Stop any existing session with the same name
    ControlTraceW(0, sessionName_.c_str(), properties, EVENT_TRACE_CONTROL_STOP);

    // Start new trace session
    ULONG status = StartTraceW(&sessionHandle_, sessionName_.c_str(), properties);
    if (status != ERROR_SUCCESS) {
        // status=5: ERROR_ACCESS_DENIED (insufficient privilege — typically not LocalSystem)
        // status=183: ERROR_ALREADY_EXISTS (orphan session owned by another SID)
        LOG_ALERT(L"[ETW] StartTraceW failed (status=" + std::to_wstring(status) +
                  L") — likely insufficient privilege or session conflict");
        return false;
    }

    // Enable the Kernel-Process provider.
    // MatchAnyKeyword=0 required: events on Windows 11 26200 carry keyword=0,
    // so any non-zero MatchAnyKeyword (including 0x30 PROCESS|THREAD) excluded all events.
    // EventID filter temporarily disabled.
    // Enabling EVENT_FILTER_TYPE_EVENT_ID caused zero delivered events after provider enable;
    // root cause not yet isolated (provider support vs filter blob format).
    // Accepted trade-off: non-matching EventIDs return from the callback immediately.
    // Buffer overflow persists (~1 lost/sec), but functional impact has not been observed so far.
    status = EnableTraceEx2(
        sessionHandle_,
        &KernelProcessProviderGuid,
        EVENT_CONTROL_CODE_ENABLE_PROVIDER,
        TRACE_LEVEL_INFORMATION,
        0,  // MatchAnyKeyword=0: required (Windows 11 26200 events carry keyword=0)
        0,
        0,
        nullptr
    );

    if (status != ERROR_SUCCESS) {
        // EnableTraceEx2 failure: provider attach rejected. Session was created
        // but Kernel-Process provider subscription not established → no events flow.
        LOG_ALERT(L"[ETW] EnableTraceEx2 failed (status=" + std::to_wstring(status) +
                  L") — provider attach rejected, session will not deliver events");

        // Cleanup session
        ControlTraceW(sessionHandle_, nullptr, properties, EVENT_TRACE_CONTROL_STOP);
        sessionHandle_ = 0;
        return false;
    }

    // Start consumer thread
    running_ = true;
    // Reset per-session diagnostic flag immediately before launching the consumer
    // thread. Visibility to the callback is guaranteed by the atomic release/
    // acquire pairing alone: this release-store synchronizes-with the
    // memory_order_acq_rel CAS in EventRecordCallback, regardless of which
    // thread context dispatches the callback. The placement here is purely for
    // semantic clarity ("did this Start session deliver a callback?"), not
    // because thread-launch ordering is required for correctness.
    firstCallbackLogged_.store(false, std::memory_order_release);
    consumerThread_ = std::thread(&ProcessMonitor::ConsumerThread, this);

    LOG_INFO(L"ETW: Process Monitor started");
    return true;
}

void ProcessMonitor::Stop() {
    // === ETW shutdown contract — DO NOT REORDER ===
    // ProcessTrace() in the consumer thread only returns after CloseTrace()
    // (or ControlTrace(STOP)) is called. Joining the consumer thread BEFORE
    // closing the trace would deadlock permanently. The only safe order is:
    //   1. stopRequested_ = true           (callback early-return guard)
    //   2. CloseTrace()                    (unblock ProcessTrace)
    //   3. ControlTrace(STOP)              (terminate session)
    //   4. consumerThread_.join()          (wait for callback drain & exit)
    //   5. clear instance_ / running_      (only safe after join)
    // After step 4 the ETW runtime guarantees no further callback invocations.
    if (!running_.load()) {
        return;
    }

    {
        std::lock_guard<std::mutex> lk(stopMtx_);

        // Publish stop with release semantics; the callback loads with acquire.
        stopRequested_.store(true, std::memory_order_release);

        // Close trace handle to unblock ProcessTrace
        if (traceHandle_ != INVALID_PROCESSTRACE_HANDLE) {
            CloseTrace(traceHandle_);
            traceHandle_ = INVALID_PROCESSTRACE_HANDLE;
        }

        // Stop the session
        if (sessionHandle_ != 0) {
            std::vector<BYTE> propertiesBuffer(PROPERTIES_BUFFER_SIZE, 0);
            auto* properties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(propertiesBuffer.data());
            properties->Wnode.BufferSize = static_cast<ULONG>(PROPERTIES_BUFFER_SIZE);
            properties->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);

            ULONG stopStatus = ControlTraceW(sessionHandle_, nullptr, properties,
                                             EVENT_TRACE_CONTROL_STOP);
            // ERROR_SUCCESS             = stopped cleanly
            // ERROR_MORE_DATA           = buffer too small for return data (still stopped)
            // ERROR_WMI_INSTANCE_NOT_FOUND = already stopped (e.g. via CloseTrace path)
            // Anything else is worth logging for diagnostics. Next Start() will
            // issue a name-based STOP to reclaim any leftover session, so we do
            // not retry here.
            if (stopStatus != ERROR_SUCCESS &&
                stopStatus != ERROR_MORE_DATA &&
                stopStatus != ERROR_WMI_INSTANCE_NOT_FOUND) {
                LOG_DEBUG(L"[ETW] ControlTrace(STOP) returned unexpected status=" + std::to_wstring(stopStatus));
            }
            sessionHandle_ = 0;
        }

        // Invalidate cached health state so the next IsHealthy() after a
        // potential restart does not see stale "alive" readings.
        cachedTraceAlive_ = false;
        lastTraceCheckTime_ = 0;
    }

    // Join OUTSIDE the mutex: ProcessTrace drains remaining buffered events
    // and returns, then the consumer thread exits. Holding stopMtx_ across
    // join would block IsHealthy() callers unnecessarily (IPC thread).
    if (consumerThread_.joinable()) {
        consumerThread_.join();
    }

    running_ = false;
    instance_.store(nullptr, std::memory_order_release);

    LOG_INFO(L"ETW: Process Monitor stopped");
}

void ProcessMonitor::ConsumerThread() {
    // Setup trace logfile structure
    EVENT_TRACE_LOGFILEW logfile = {};
    logfile.LoggerName = const_cast<LPWSTR>(sessionName_.c_str());
    logfile.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    logfile.EventRecordCallback = EventRecordCallback;

    // Open trace
    traceHandle_ = OpenTraceW(&logfile);
    // 戻り値を常に記録 — INVALID_PROCESSTRACE_HANDLE (64bit: 0xFFFFFFFFFFFFFFFF) か即判定可能にする
    LOG_INFO(L"[ETW] OpenTraceW handle=" + std::to_wstring((uintptr_t)traceHandle_));
    if (traceHandle_ == INVALID_PROCESSTRACE_HANDLE) {
        DWORD error = GetLastError();
        LOG_ALERT(L"[ETW] OpenTraceW failed (error=" + std::to_wstring(error)
                  + L") — ConsumerThread cannot deliver events");
        sessionHealthy_ = false;
        running_ = false;
        return;
    }

    sessionHealthy_ = true;

    // Process events (blocks until trace is closed)
    ULONGLONG enterMs = GetTickCount64();
    LOG_INFO(L"[ETW] ProcessTrace enter");
    ULONG status = ProcessTrace(&traceHandle_, 1, nullptr, nullptr);
    ULONGLONG elapsedMs = GetTickCount64() - enterMs;
    // elapsed で「即 return」(consumer loop 問題) か「長時間ブロック後停止」(provider 側) かを切り分ける
    LOG_INFO(L"[ETW] ProcessTrace exited status=" + std::to_wstring(status)
             + L", elapsed=" + std::to_wstring(elapsedMs) + L"ms");
    if (status != ERROR_SUCCESS && status != ERROR_CANCELLED && !stopRequested_.load()) {
        sessionHealthy_ = false;
    }

    running_ = false;
}

void WINAPI ProcessMonitor::EventRecordCallback(PEVENT_RECORD pEvent) {
    // Load instance_ once into a local to avoid repeated atomic reads and to
    // prevent a theoretical TOCTOU where instance_ could be re-checked at
    // different points. Acquire pairs with the release store in Start()/Stop().
    ProcessMonitor* self = instance_.load(std::memory_order_acquire);
    if (!self) return;

    // Once Stop() has set stopRequested_, any events delivered by ETW during
    // the ProcessTrace drain phase are ignored, so no further callback side
    // effects occur before consumerThread_.join() completes.
    if (self->stopRequested_.load(std::memory_order_acquire)) {
        return;
    }

    // Updated once per callback (after stopRequested drain exclusion) to record
    // the last time the ETW session delivered any event, including overflow notices.
    self->lastEventTime_ = GetTickCount64();

    // One-shot per-session diagnostic: confirm callback delivery for the CURRENT
    // Start() session. Member flag (not static local) so RestartETW() rearmaments
    // re-enable the diagnostic for each new session. Placed after stopRequested
    // check to avoid logging from the prior session's teardown drain. Placed
    // before LOST_EVENT / provider filter so any callback type counts as proof
    // of delivery.
    bool expected = false;
    if (self->firstCallbackLogged_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        LOG_INFO(L"[ETW] First callback received — session is delivering events");
        // Log keyword bits of first event to confirm whether MatchAnyKeyword=0 is
        // permanently required (keyword=0 events are excluded by any non-zero mask).
        wchar_t kwBuf[24];
        swprintf_s(kwBuf, L"0x%016llX", pEvent->EventHeader.EventDescriptor.Keyword);
        LOG_DEBUG(L"[ETW] First event: keyword=" + std::wstring(kwBuf)
                  + L" eventId=" + std::to_wstring(pEvent->EventHeader.EventDescriptor.Id));
    }

    // Detect ETW buffer overflow: lost event notification
    // ETW uses Opcode EVENT_TRACE_TYPE_LOST_EVENT (0x20) to signal dropped events
    if (pEvent->EventHeader.EventDescriptor.Opcode == EVENT_TRACE_TYPE_LOST_EVENT) {
        uint32_t count = ++self->lostEventCount_;
        LOG_DEBUG(L"[ETW] Lost event detected (buffer overflow). Total lost: " + std::to_wstring(count));
        return;
    }

    self->eventCount_.fetch_add(1, std::memory_order_relaxed);

    // Filter by provider
    if (!IsEqualGUID(pEvent->EventHeader.ProviderId, KernelProcessProviderGuid)) {
        return;
    }

    const USHORT eventId = pEvent->EventHeader.EventDescriptor.Id;

    // Handle process start event
    if (eventId == EVENT_ID_PROCESS_START) {
        DWORD pid = 0;
        DWORD parentPid = 0;
        std::wstring imageName;
        std::wstring imagePath;

        if (ParseProcessStartEvent(pEvent, pid, parentPid, imageName, imagePath)) {
            if (self->processCallback_) {
                self->processCallback_(pid, parentPid, imageName, imagePath);
            }
        }
        return;
    }

    // Handle thread start event
    // Thread creation is a trigger point where OS may re-apply EcoQoS
    if (eventId == EVENT_ID_THREAD_START) {
        if (self->threadCallback_) {
            // The owner PID is in the event header (the process that owns the new thread)
            DWORD ownerPid = pEvent->EventHeader.ProcessId;
            // Thread ID can be extracted from user data, but we don't need it for our use case
            // We only care about which process is creating threads
            DWORD threadId = pEvent->EventHeader.ThreadId;
            self->threadCallback_(threadId, ownerPid);
        }
        return;
    }
}

bool ProcessMonitor::ParseProcessStartEvent(PEVENT_RECORD pEvent,
                                             DWORD& pid, DWORD& parentPid,
                                             std::wstring& imageName,
                                             std::wstring& imagePath) {
    // The process ID is in the event header
    pid = pEvent->EventHeader.ProcessId;

    // For Kernel-Process events, we need to parse the user data
    // Event structure varies by OS version, so we use TDH to parse it

    static constexpr DWORD MAX_TDH_BUFFER = 512 * 1024;  // 512KB上限
    DWORD bufferSize = 0;
    TDHSTATUS status = TdhGetEventInformation(pEvent, 0, nullptr, nullptr, &bufferSize);
    if (status != ERROR_INSUFFICIENT_BUFFER) {
        return false;
    }
    if (bufferSize == 0 || bufferSize > MAX_TDH_BUFFER) {
        return false;
    }

    // thread_local: ConsumerThread 専用。capacity は縮小しない（反復ヒープ確保を排除）
    thread_local std::vector<BYTE> s_tdhBuffer;
    if (s_tdhBuffer.capacity() < bufferSize)
        s_tdhBuffer.reserve(bufferSize);
    s_tdhBuffer.resize(bufferSize);

#ifdef _DEBUG
    // Debug: MAX_TDH_BUFFER を超える成長は現在の設計では発生しない。
    // 将来の cap 変更・スキーマ拡張を検知するためのセーフティネット。
    if (s_tdhBuffer.capacity() > MAX_TDH_BUFFER * 2) {
        LOG_DEBUG(L"[ETW] TDH buffer abnormal growth: "
                  + std::to_wstring(s_tdhBuffer.capacity()) + L" bytes");
    }
#endif

    auto* eventInfo = reinterpret_cast<TRACE_EVENT_INFO*>(s_tdhBuffer.data());

    status = TdhGetEventInformation(pEvent, 0, nullptr, eventInfo, &bufferSize);
    if (status != ERROR_SUCCESS) {
        return false;
    }

    // Parse properties
    BYTE* userData = static_cast<BYTE*>(pEvent->UserData);
    USHORT userDataLength = pEvent->UserDataLength;

    for (DWORD i = 0; i < eventInfo->TopLevelPropertyCount && userData < (static_cast<BYTE*>(pEvent->UserData) + userDataLength); i++) {
        EVENT_PROPERTY_INFO& propInfo = eventInfo->EventPropertyInfoArray[i];
        LPCWSTR propName = reinterpret_cast<LPCWSTR>(
            reinterpret_cast<BYTE*>(eventInfo) + propInfo.NameOffset);

        PROPERTY_DATA_DESCRIPTOR dataDesc = {};
        dataDesc.PropertyName = reinterpret_cast<ULONGLONG>(propName);
        dataDesc.ArrayIndex = ULONG_MAX;

        DWORD propSize = 0;
        status = TdhGetPropertySize(pEvent, 0, nullptr, 1, &dataDesc, &propSize);
        if (status != ERROR_SUCCESS) {
            continue;
        }

        thread_local std::vector<BYTE> s_propBuffer;
        if (s_propBuffer.capacity() < propSize)
            s_propBuffer.reserve(propSize);
        s_propBuffer.resize(propSize);

        status = TdhGetProperty(pEvent, 0, nullptr, 1, &dataDesc, propSize, s_propBuffer.data());
        if (status != ERROR_SUCCESS) {
            continue;
        }

        // Extract relevant properties
        if (_wcsicmp(propName, L"ProcessID") == 0 && propSize >= sizeof(DWORD)) {
            pid = *reinterpret_cast<DWORD*>(s_propBuffer.data());
        }
        else if (_wcsicmp(propName, L"ParentProcessID") == 0 && propSize >= sizeof(DWORD)) {
            parentPid = *reinterpret_cast<DWORD*>(s_propBuffer.data());
        }
        else if (_wcsicmp(propName, L"ImageName") == 0 || _wcsicmp(propName, L"ImageFileName") == 0) {
            // Image name is a null-terminated string. TDH usually returns a
            // terminated payload, but truncated events or malformed providers
            // can hand us an unterminated buffer. Always bound the read by
            // propSize to avoid reading past the heap allocation (AV risk).
            if (propSize > 0) {
                if (propInfo.nonStructType.InType == TDH_INTYPE_UNICODESTRING) {
                    // Bounded wide-string copy: stop at first L'\0' or propSize.
                    const wchar_t* wp = reinterpret_cast<const wchar_t*>(s_propBuffer.data());
                    size_t maxChars = propSize / sizeof(wchar_t);
                    size_t wlen = 0;
                    while (wlen < maxChars && wp[wlen] != L'\0') ++wlen;
                    imageName.assign(wp, wlen);
                } else if (propInfo.nonStructType.InType == TDH_INTYPE_ANSISTRING) {
                    // Bounded ANSI copy, then convert to wide.
                    const char* cp = reinterpret_cast<const char*>(s_propBuffer.data());
                    size_t alen = 0;
                    while (alen < propSize && cp[alen] != '\0') ++alen;
                    if (alen > 0) {
                        int wideLen = MultiByteToWideChar(CP_ACP, 0, cp,
                                                          static_cast<int>(alen),
                                                          nullptr, 0);
                        if (wideLen > 0) {
                            imageName.resize(wideLen);
                            int converted = MultiByteToWideChar(CP_ACP, 0, cp,
                                                                static_cast<int>(alen),
                                                                &imageName[0], wideLen);
                            if (converted == 0) imageName.clear();
                        }
                    }
                } else {
                    // Unknown InType — do NOT reinterpret as a string. Skip this
                    // property to avoid reading arbitrary bytes as wchar_t.
                    continue;
                }

                // Preserve full path before extracting filename
                size_t lastSlash = imageName.find_last_of(L"\\/");
                if (lastSlash != std::wstring::npos) {
                    imagePath = imageName;  // full path hint (may be 8.3/device format)
                    imageName = imageName.substr(lastSlash + 1);
                }
                // If no separator found, imagePath stays empty (filename only from ETW)
            }
        }
    }

    return pid != 0;
}

bool ProcessMonitor::IsHealthy() const {
    // Atomic fast paths — no lock needed
    if (!running_.load()) {
        return false;
    }
    if (!sessionHealthy_.load()) {
        return false;
    }

    ULONGLONG now = GetTickCount64();

    // Warmup grace period: unconditionally healthy for 120s after session start
    // Prevents false positives during boot storm → quiet transition
    if ((now - startTime_.load()) < WARMUP_PERIOD_MS) {
        return true;
    }

    ULONGLONG lastEvent = lastEventTime_.load();

    // Acquire stopMtx_ to serialize with Stop() and protect mutable health state.
    // Note: IsHealthy() callers (IPC thread) may briefly block if Stop() is in
    // progress — this is intentional, it prevents data races on sessionHandle_.
    std::lock_guard<std::mutex> lk(stopMtx_);

    // Re-check running_ under the lock — Stop() may have completed while we
    // were computing `now`, in which case there is nothing left to query.
    if (!running_.load()) {
        return false;
    }

    if (lastEvent == 0) {
        // No events ever received after warmup - check session liveness only
        return IsTraceSessionAliveLocked(now);
    }

    if ((now - lastEvent) > STARVATION_MS) {
        // Event starvation detected - check delta of lost events since last check
        uint32_t currentLost = lostEventCount_.load();
        uint32_t deltaLost = currentLost - lastCheckedLost_;
        lastCheckedLost_ = currentLost;

        if (deltaLost > LOST_EVENT_THRESHOLD) {
            return false;
        }
        // No significant lost event burst - check if ETW session is alive
        if (!IsTraceSessionAliveLocked(now)) {
            return false;
        }
    }

    return true;
}

bool ProcessMonitor::IsTraceSessionAliveLocked(ULONGLONG now) const {
    // Caller must hold stopMtx_. Safe to read sessionHandle_ under the lock.
    if (lastTraceCheckTime_ > 0 && (now - lastTraceCheckTime_) < TRACE_CHECK_CACHE_MS) {
        return cachedTraceAlive_;
    }

    lastTraceCheckTime_ = now;

    if (sessionHandle_ == 0) {
        cachedTraceAlive_ = false;
        return false;
    }

    std::vector<BYTE> buf(PROPERTIES_BUFFER_SIZE, 0);
    auto* props = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buf.data());
    props->Wnode.BufferSize = static_cast<ULONG>(PROPERTIES_BUFFER_SIZE);
    props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);

    ULONG status = ControlTraceW(sessionHandle_, nullptr, props, EVENT_TRACE_CONTROL_QUERY);
    cachedTraceAlive_ = (status == ERROR_SUCCESS);
    return cachedTraceAlive_;
}

} // namespace unleaf
