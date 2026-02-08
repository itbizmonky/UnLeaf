#pragma once
// UnLeaf v1.00 - IPC Client
// Named Pipe client for communication with Service

#include "../common/types.h"
#include "../common/scoped_handle.h"
#include <string>
#include <optional>
#include <utility>

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

private:
    HANDLE pipeHandle_;
    CriticalSection cs_;
};

} // namespace unleaf
