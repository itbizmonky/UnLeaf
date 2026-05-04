#pragma once
// UnLeaf - ETW Process Monitor
// Event-driven process creation and thread detection using Event Tracing for Windows

#include "../common/types.h"
#include <evntrace.h>
#include <evntcons.h>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <string>

#pragma comment(lib, "advapi32.lib")

namespace unleaf {

// Callback type for process start events
// imageName: filename only (e.g. "chrome.exe")
// imagePath: full image path from ETW, hint only — may be empty, 8.3, or device-format
using ProcessStartCallback = std::function<void(DWORD pid, DWORD parentPid,
                                                 const std::wstring& imageName,
                                                 const std::wstring& imagePath)>;

// Callback type for thread start events (used to detect EcoQoS re-enablement triggers)
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

    // Health check - returns true if ETW session is healthy
    bool IsHealthy() const;

    // Get last event timestamp (for diagnostics)
    ULONGLONG GetLastEventTime() const { return lastEventTime_.load(); }

    // Get total event count (for diagnostics)
    uint32_t GetEventCount() const { return eventCount_.load(); }

    // Get lost event count (non-zero indicates ETW buffer pressure)
    uint32_t GetLostEventCount() const { return lostEventCount_.load(); }

    bool IsSessionHealthy() const { return sessionHealthy_.load(); }

private:
    // ETW event callback (static for C API compatibility)
    static void WINAPI EventRecordCallback(PEVENT_RECORD pEvent);

    // Process the ETW consumer loop
    void ConsumerThread();

    // Parse process start event data
    static bool ParseProcessStartEvent(PEVENT_RECORD pEvent,
                                        DWORD& pid, DWORD& parentPid,
                                        std::wstring& imageName,
                                        std::wstring& imagePath);

    // ETW handles
    // Protected by stopMtx_ for writes. Reads on the Consumer thread are safe
    // because Start() publishes both handles before the thread is launched, and
    // Stop() joins the thread before the object can be destroyed.
    TRACEHANDLE sessionHandle_;
    TRACEHANDLE traceHandle_;

    // Serializes Stop() teardown vs. IsHealthy()/IsTraceSessionAlive() from the
    // IPC thread. Also protects mutable health-check state below.
    mutable std::mutex stopMtx_;

    // Consumer thread
    std::thread consumerThread_;
    std::atomic<bool> running_;
    std::atomic<bool> stopRequested_;

    std::atomic<ULONGLONG> lastEventTime_;    // Last event received timestamp
    std::atomic<ULONGLONG> startTime_;        // Session start timestamp (for warmup grace period)
    std::atomic<uint32_t> eventCount_;        // Total events received
    std::atomic<uint32_t>  lostEventCount_;    // ETW lost event count (buffer overflow indicator)
    std::atomic<ULONGLONG> lastLostLogTime_;  // Last timestamp a lost-event DEBUG line was emitted (rate limiter)
    std::atomic<bool> sessionHealthy_;        // Session health flag
    // One-shot diagnostic: confirms callback delivery for the CURRENT Start() session.
    // Reset to false in Start() only after EnableTraceEx2 success, so the flag
    // semantically tracks "did this viable session deliver a callback?".
    std::atomic<bool> firstCallbackLogged_{false};

    // Health check state (mutable: updated by const IsHealthy/IsTraceSessionAliveLocked)
    // All fields below are protected by stopMtx_.
    mutable uint32_t  lastCheckedLost_;       // lostEventCount_ snapshot at last health check
    mutable ULONGLONG lastTraceCheckTime_;    // Last IsTraceSessionAliveLocked() call timestamp
    mutable bool      cachedTraceAlive_;      // Cached result of IsTraceSessionAliveLocked()

    // Callbacks
    ProcessStartCallback processCallback_;
    ThreadStartCallback threadCallback_;

    // Session name (unique per instance)
    std::wstring sessionName_;

    // Query whether the underlying ETW trace session is still alive (1s cached).
    // Caller MUST hold stopMtx_.
    bool IsTraceSessionAliveLocked(ULONGLONG now) const;

    // Static instance pointer for callback routing.
    // Atomic as a defensive measure: current synchronization (thread start/join
    // + ETW's "no callback after ProcessTrace returns" contract) already makes
    // non-atomic safe, but atomic hardens against future modifications that
    // might introduce additional readers on other threads.
    static std::atomic<ProcessMonitor*> instance_;

    // ETW session properties buffer
    static constexpr size_t PROPERTIES_BUFFER_SIZE =
        sizeof(EVENT_TRACE_PROPERTIES) + (256 * sizeof(wchar_t)) * 2;

    // ETW session buffer configuration
    // BufferSize must be a multiple of 4KB
    static constexpr ULONG ETW_BUFFER_SIZE_KB = 128;  // KB per buffer (was 64; enlarged for MatchAnyKeyword=0 event volume)
    static constexpr ULONG ETW_MIN_BUFFERS    = 8;    // Minimum buffer count (was 4)
    static constexpr ULONG ETW_MAX_BUFFERS    = 64;   // Maximum buffer count (was 32; max 8MB total)
    static constexpr ULONG ETW_FLUSH_TIMER    = 0;    // 0 = disabled; real-time consumer pulls directly

    // Health check thresholds
    static constexpr ULONGLONG WARMUP_PERIOD_MS   = 120000;  // 120s grace period after session start
    static constexpr ULONGLONG STARVATION_MS      = 60000;   // 60s without events = potential starvation
    static constexpr uint32_t  LOST_EVENT_THRESHOLD = 10;    // Lost events (delta) above this = unhealthy
    static constexpr ULONGLONG TRACE_CHECK_CACHE_MS = 1000;  // IsTraceSessionAlive cache duration
};

} // namespace unleaf
