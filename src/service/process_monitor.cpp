// UnLeaf - ETW Process Monitor Implementation

#include "process_monitor.h"
#include "../common/logger.h"
#include <sstream>
#include <tdh.h>

#pragma comment(lib, "tdh.lib")

namespace unleaf {

// Static instance for callback routing
ProcessMonitor* ProcessMonitor::instance_ = nullptr;

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
    , eventCount_(0)
    , sessionHealthy_(false) {

    // Generate unique session name
    std::wstringstream ss;
    ss << L"UnLeafProcessMonitor_" << GetCurrentProcessId();
    sessionName_ = ss.str();
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
    instance_ = this;
    stopRequested_ = false;

    // Allocate properties buffer
    std::vector<BYTE> propertiesBuffer(PROPERTIES_BUFFER_SIZE, 0);
    auto* properties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(propertiesBuffer.data());

    properties->Wnode.BufferSize = static_cast<ULONG>(PROPERTIES_BUFFER_SIZE);
    properties->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    properties->Wnode.ClientContext = 1;  // QPC clock resolution
    properties->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    properties->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    properties->LogFileNameOffset = 0;

    // Stop any existing session with the same name
    ControlTraceW(0, sessionName_.c_str(), properties, EVENT_TRACE_CONTROL_STOP);

    // Start new trace session
    ULONG status = StartTraceW(&sessionHandle_, sessionName_.c_str(), properties);
    if (status != ERROR_SUCCESS) {
        std::wstringstream ss;
        ss << L"Failed to start ETW session: " << status;
        LOG_DEBUG(ss.str());
        return false;
    }

    // Enable the Kernel-Process provider
    // Keywords 0x30 = WINEVENT_KEYWORD_PROCESS (0x10) | WINEVENT_KEYWORD_THREAD (0x20)
    // Thread events allow event-driven detection of EcoQoS re-enablement triggers
    status = EnableTraceEx2(
        sessionHandle_,
        &KernelProcessProviderGuid,
        EVENT_CONTROL_CODE_ENABLE_PROVIDER,
        TRACE_LEVEL_INFORMATION,
        0x30,  // WINEVENT_KEYWORD_PROCESS | WINEVENT_KEYWORD_THREAD
        0,
        0,
        nullptr
    );

    if (status != ERROR_SUCCESS) {
        std::wstringstream ss;
        ss << L"Failed to enable Kernel-Process provider: " << status;
        LOG_DEBUG(ss.str());

        // Cleanup session
        ControlTraceW(sessionHandle_, nullptr, properties, EVENT_TRACE_CONTROL_STOP);
        sessionHandle_ = 0;
        return false;
    }

    // Start consumer thread
    running_ = true;
    consumerThread_ = std::thread(&ProcessMonitor::ConsumerThread, this);

    LOG_INFO(L"ETW: Process Monitor started");
    return true;
}

void ProcessMonitor::Stop() {
    if (!running_.load()) {
        return;
    }

    stopRequested_ = true;

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

        ControlTraceW(sessionHandle_, nullptr, properties, EVENT_TRACE_CONTROL_STOP);
        sessionHandle_ = 0;
    }

    // Wait for consumer thread
    if (consumerThread_.joinable()) {
        consumerThread_.join();
    }

    running_ = false;
    instance_ = nullptr;

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
    if (traceHandle_ == INVALID_PROCESSTRACE_HANDLE) {
        DWORD error = GetLastError();
        std::wstringstream ss;
        ss << L"[ETW] OpenTrace failed (error=" << error << L")";
        LOG_DEBUG(ss.str());
        sessionHealthy_ = false;
        running_ = false;
        return;
    }

    sessionHealthy_ = true;
    lastEventTime_ = GetTickCount64();

    // Process events (blocks until trace is closed)
    ULONG status = ProcessTrace(&traceHandle_, 1, nullptr, nullptr);

    if (status != ERROR_SUCCESS && status != ERROR_CANCELLED && !stopRequested_.load()) {
        std::wstringstream ss;
        ss << L"[ETW] ProcessTrace ended unexpectedly (status=" << status << L")";
        LOG_DEBUG(ss.str());
        sessionHealthy_ = false;
    }

    running_ = false;
}

void WINAPI ProcessMonitor::EventRecordCallback(PEVENT_RECORD pEvent) {
    if (!instance_ || instance_->stopRequested_.load()) {
        return;
    }

    instance_->lastEventTime_ = GetTickCount64();
    instance_->eventCount_.fetch_add(1, std::memory_order_relaxed);

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

        if (ParseProcessStartEvent(pEvent, pid, parentPid, imageName)) {
            if (instance_->processCallback_) {
                instance_->processCallback_(pid, parentPid, imageName);
            }
        }
        return;
    }

    // Handle thread start event
    // Thread creation is a trigger point where OS may re-apply EcoQoS
    if (eventId == EVENT_ID_THREAD_START) {
        if (instance_->threadCallback_) {
            // The owner PID is in the event header (the process that owns the new thread)
            DWORD ownerPid = pEvent->EventHeader.ProcessId;
            // Thread ID can be extracted from user data, but we don't need it for our use case
            // We only care about which process is creating threads
            DWORD threadId = pEvent->EventHeader.ThreadId;
            instance_->threadCallback_(threadId, ownerPid);
        }
        return;
    }
}

bool ProcessMonitor::ParseProcessStartEvent(PEVENT_RECORD pEvent,
                                             DWORD& pid, DWORD& parentPid,
                                             std::wstring& imageName) {
    // The process ID is in the event header
    pid = pEvent->EventHeader.ProcessId;

    // For Kernel-Process events, we need to parse the user data
    // Event structure varies by OS version, so we use TDH to parse it

    DWORD bufferSize = 0;
    TDHSTATUS status = TdhGetEventInformation(pEvent, 0, nullptr, nullptr, &bufferSize);
    if (status != ERROR_INSUFFICIENT_BUFFER) {
        return false;
    }

    std::vector<BYTE> buffer(bufferSize);
    auto* eventInfo = reinterpret_cast<TRACE_EVENT_INFO*>(buffer.data());

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

        std::vector<BYTE> propData(propSize);
        status = TdhGetProperty(pEvent, 0, nullptr, 1, &dataDesc, propSize, propData.data());
        if (status != ERROR_SUCCESS) {
            continue;
        }

        // Extract relevant properties
        if (_wcsicmp(propName, L"ProcessID") == 0 && propSize >= sizeof(DWORD)) {
            pid = *reinterpret_cast<DWORD*>(propData.data());
        }
        else if (_wcsicmp(propName, L"ParentProcessID") == 0 && propSize >= sizeof(DWORD)) {
            parentPid = *reinterpret_cast<DWORD*>(propData.data());
        }
        else if (_wcsicmp(propName, L"ImageName") == 0 || _wcsicmp(propName, L"ImageFileName") == 0) {
            // Image name is a null-terminated string
            if (propSize > 0) {
                // Check if it's Unicode or ANSI
                if (propInfo.nonStructType.InType == TDH_INTYPE_UNICODESTRING) {
                    imageName = reinterpret_cast<LPCWSTR>(propData.data());
                } else if (propInfo.nonStructType.InType == TDH_INTYPE_ANSISTRING) {
                    // Convert ANSI to Unicode
                    std::string ansiName(reinterpret_cast<char*>(propData.data()));
                    int wideLen = MultiByteToWideChar(CP_ACP, 0, ansiName.c_str(), -1, nullptr, 0);
                    if (wideLen > 0) {
                        imageName.resize(wideLen - 1);
                        MultiByteToWideChar(CP_ACP, 0, ansiName.c_str(), -1, &imageName[0], wideLen);
                    }
                } else {
                    // Try as raw string
                    imageName = reinterpret_cast<LPCWSTR>(propData.data());
                }

                // Extract just the filename from path
                size_t lastSlash = imageName.find_last_of(L"\\/");
                if (lastSlash != std::wstring::npos) {
                    imageName = imageName.substr(lastSlash + 1);
                }
            }
        }
    }

    return pid != 0;
}

bool ProcessMonitor::IsHealthy() const {
    // Not running = not healthy
    if (!running_.load()) {
        return false;
    }

    // Session marked unhealthy (e.g., OpenTrace/ProcessTrace failed)
    if (!sessionHealthy_.load()) {
        return false;
    }

    // Check for event starvation (no events for 60 seconds)
    // Note: This is a soft check - system might just be idle
    // We use 60 seconds to avoid false positives
    ULONGLONG now = GetTickCount64();
    ULONGLONG lastEvent = lastEventTime_.load();
    if (lastEvent > 0 && (now - lastEvent) > 60000) {
        // No events for 60 seconds - might be unhealthy
        // But only flag as unhealthy if we've received events before
        // (to handle initial startup period)
        if (eventCount_.load() > 0) {
            return false;
        }
    }

    return true;
}

} // namespace unleaf
