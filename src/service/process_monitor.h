#pragma once
// UnLeaf v8.0 - ETW Process Monitor
// Event-driven process creation and thread detection using Event Tracing for Windows
// v7.4: Added health check, session recovery, event statistics
// v8.0: Added thread start event monitoring for event-driven enforcement

#include "../common/types.h"
#include <evntrace.h>
#include <evntcons.h>
#include <functional>
#include <thread>
#include <atomic>
#include <string>

#pragma comment(lib, "advapi32.lib")

namespace unleaf {

// Callback type for process start events
using ProcessStartCallback = std::function<void(DWORD pid, DWORD parentPid,
                                                 const std::wstring& imageName)>;

// v8.0: Callback type for thread start events (used to detect EcoQoS re-enablement triggers)
using ThreadStartCallback = std::function<void(DWORD threadId, DWORD ownerPid)>;

// ETW-based process creation monitor
class ProcessMonitor {
public:
    ProcessMonitor();
    ~ProcessMonitor();

    // Start monitoring with callbacks
    // processCallback: Called on process creation events
    // threadCallback: Optional, called on thread creation events for tracked processes
    bool Start(ProcessStartCallback processCallback, ThreadStartCallback threadCallback = nullptr);

    // Stop monitoring
    void Stop();

    // Is monitoring active?
    bool IsRunning() const { return running_.load(); }

    // v7.4: Health check - returns true if ETW session is healthy
    bool IsHealthy() const;

    // v7.4: Get last event timestamp (for diagnostics)
    ULONGLONG GetLastEventTime() const { return lastEventTime_.load(); }

    // v7.4: Get total event count (for diagnostics)
    uint32_t GetEventCount() const { return eventCount_.load(); }

    // v7.4: Check if session is marked healthy
    bool IsSessionHealthy() const { return sessionHealthy_.load(); }

private:
    // ETW event callback (static for C API compatibility)
    static void WINAPI EventRecordCallback(PEVENT_RECORD pEvent);

    // Process the ETW consumer loop
    void ConsumerThread();

    // Parse process start event data
    static bool ParseProcessStartEvent(PEVENT_RECORD pEvent,
                                        DWORD& pid, DWORD& parentPid,
                                        std::wstring& imageName);

    // ETW handles
    TRACEHANDLE sessionHandle_;
    TRACEHANDLE traceHandle_;

    // Consumer thread
    std::thread consumerThread_;
    std::atomic<bool> running_;
    std::atomic<bool> stopRequested_;

    // v7.4: Health monitoring
    std::atomic<ULONGLONG> lastEventTime_;    // Last event received timestamp
    std::atomic<uint32_t> eventCount_;        // Total events received
    std::atomic<bool> sessionHealthy_;        // Session health flag

    // Callbacks
    ProcessStartCallback processCallback_;
    ThreadStartCallback threadCallback_;  // v8.0: Thread event callback

    // Session name (unique per instance)
    std::wstring sessionName_;

    // Static instance pointer for callback routing
    static ProcessMonitor* instance_;

    // ETW session properties buffer
    static constexpr size_t PROPERTIES_BUFFER_SIZE =
        sizeof(EVENT_TRACE_PROPERTIES) + (256 * sizeof(wchar_t)) * 2;
};

} // namespace unleaf
