#pragma once
// UnLeaf v7.5 - IPC Server
// Named Pipe server for communication with Manager
// v7.5: Added DACL security and client authorization

#include "../common/types.h"
#include "../common/scoped_handle.h"
#include "../common/security.h"
#include <string>
#include <thread>
#include <atomic>
#include <functional>

namespace unleaf {

// IPC Message structure
struct IPCMessage {
    IPCCommand command;
    uint32_t dataLength;
    // Variable length data follows
};

struct IPCResponseMessage {
    IPCResponse response;
    uint32_t dataLength;
    // Variable length data follows
};

class IPCServer {
public:
    // Singleton access
    static IPCServer& Instance();

    // Initialize server
    bool Initialize();

    // Start listening for connections
    void Start();

    // Stop server
    void Stop();

    // Is server running?
    bool IsRunning() const { return running_.load(); }

    // Command handlers
    using CommandHandler = std::function<std::string(const std::string& data)>;
    void SetCommandHandler(IPCCommand cmd, CommandHandler handler);

private:
    IPCServer();
    ~IPCServer();
    IPCServer(const IPCServer&) = delete;
    IPCServer& operator=(const IPCServer&) = delete;

    // Server thread function
    void ServerLoop();

    // Handle a single client connection
    void HandleClient(HANDLE pipeHandle);

    // Process received command
    std::string ProcessCommand(IPCCommand cmd, const std::string& data);

    // Send response to client
    bool SendResponse(HANDLE pipe, IPCResponse response, const std::string& data);

    // Get log entries from specified offset
    std::string GetLogsFromOffset(uint64_t clientOffset);

    // v7.5: Client authorization
    AuthResult AuthorizeClient(HANDLE pipeHandle, IPCCommand cmd);
    static CommandPermission GetCommandPermission(IPCCommand cmd);

    std::atomic<bool> running_;
    std::atomic<bool> stopRequested_;
    std::thread serverThread_;
    HANDLE stopEvent_;

    // Command handlers
    std::map<IPCCommand, CommandHandler> handlers_;
    CriticalSection handlerCs_;
};

} // namespace unleaf
