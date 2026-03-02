#pragma once
// UnLeaf v1.00 - IPC Client
// Named Pipe client for communication with Service

#include "../common/types.h"
#include "../common/scoped_handle.h"
#include <string>
#include <optional>
#include <utility>
#include <atomic>

namespace unleaf {

class IPCClient {
public:
    IPCClient();
    ~IPCClient();

    // Connect to service pipe
    bool Connect();

    // Disconnect from service
    void Disconnect();

    // Check if connected
    bool IsConnected() const;

    // Send command and receive response
    std::optional<std::string> SendCommand(IPCCommand cmd, const std::string& data = "");

    // Convenience methods
    std::optional<std::string> GetServiceStatus();
    bool RequestServiceStop();
    bool AddTarget(const std::wstring& name);
    bool RemoveTarget(const std::wstring& name);

    // Get log entries from specified offset
    // Returns: pair of (new_offset, log_data), or nullopt on failure
    std::optional<std::pair<uint64_t, std::string>> GetLogs(uint64_t fromOffset);

    // Counters
    uint32_t GetOpenCount()          const { return openCount_.load(std::memory_order_relaxed); }
    uint32_t GetTotalRequests()      const { return totalRequests_.load(std::memory_order_relaxed); }
    uint32_t GetSuccessfulRequests() const { return successfulRequests_.load(std::memory_order_relaxed); }
    uint32_t GetFailedRequests()     const { return failedRequests_.load(std::memory_order_relaxed); }
    uint32_t GetReconnectOpenFail()  const { return reconnectOpenFail_.load(std::memory_order_relaxed); }

    // Quality snapshot (atomically reads and resets interval counters)
    struct QualityStats { float avgLatMs; uint32_t tmo, brk, man, openFail; };
    QualityStats SnapAndResetQuality();

private:
    HANDLE pipeHandle_;
    CriticalSection cs_;

    // Cumulative counters
    std::atomic<uint32_t> openCount_{0};
    std::atomic<uint32_t> totalRequests_{0};
    std::atomic<uint32_t> successfulRequests_{0};
    std::atomic<uint32_t> failedRequests_{0};
    std::atomic<uint32_t> reconnectOpenFail_{0};

    // QPC latency (measured at DEBUG log level)
    LARGE_INTEGER qpcFreq_{};
    std::atomic<uint64_t> latencySumUs_{0};
    std::atomic<uint32_t> latencyCount_{0};

    // Reconnect reason counters (interval, reset by SnapAndResetQuality)
    std::atomic<uint32_t> reconnectTimeout_{0};
    std::atomic<uint32_t> reconnectBroken_{0};
    std::atomic<uint32_t> reconnectManual_{0};
};

} // namespace unleaf
