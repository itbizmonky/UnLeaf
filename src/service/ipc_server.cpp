// UnLeaf - IPC Server Implementation

#include "ipc_server.h"
#include "engine_core.h"
#include "../common/logger.h"
#include <algorithm>
#include <sstream>

namespace unleaf {

IPCServer& IPCServer::Instance() {
    static IPCServer instance;
    return instance;
}

IPCServer::IPCServer()
    : running_(false)
    , stopRequested_(false)
    , stopEvent_(nullptr) {
}

IPCServer::~IPCServer() {
    Stop();
}

bool IPCServer::Initialize() {
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent_) {
        LOG_ERROR(L"IPC: Failed to create stop event");
        return false;
    }

    LOG_INFO(L"IPC: Initialized");
    return true;
}

void IPCServer::Start() {
    if (running_.load()) return;

    running_ = true;
    stopRequested_ = false;
    ResetEvent(stopEvent_);

    serverThread_ = std::thread(&IPCServer::ServerLoop, this);
    LOG_INFO(L"IPC: Server started");
}

void IPCServer::Stop() {
    if (!running_.load()) return;

    stopRequested_ = true;
    SetEvent(stopEvent_);

    if (serverThread_.joinable()) {
        serverThread_.join();
    }

    if (stopEvent_) {
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
    }

    running_ = false;
    LOG_INFO(L"IPC: Server stopped");
}

void IPCServer::SetCommandHandler(IPCCommand cmd, CommandHandler handler) {
    CSLockGuard lock(handlerCs_);
    handlers_[cmd] = std::move(handler);
}

void IPCServer::ServerLoop() {
    LOG_INFO(L"IPC: Server loop started (secured)");

    // Create security descriptor - REQUIRED for security
    PipeSecurityDescriptor pipeSecurity;
    if (!pipeSecurity.Initialize()) {
        // Do NOT continue without DACL - security risk
        LOG_ERROR(L"IPC: DACL initialization failed - IPC disabled");
        return;
    }
    LOG_INFO(L"IPC: Security descriptor initialized (SYSTEM + Admins only)");

    int consecutiveFailures = 0;

    while (!stopRequested_.load()) {
        // Create named pipe instance with overlapped I/O support
        HANDLE pipeHandle = CreateNamedPipeW(
            PIPE_NAME,
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            static_cast<DWORD>(UNLEAF_MAX_IPC_DATA_SIZE),   // Output buffer size
            static_cast<DWORD>(UNLEAF_MAX_IPC_DATA_SIZE),   // Input buffer size
            0,      // Default timeout
            pipeSecurity.GetSecurityAttributes()
        );

        if (pipeHandle == INVALID_HANDLE_VALUE) {
            consecutiveFailures++;
            if (consecutiveFailures == 10) {
                LOG_ERROR(L"IPC: CreateNamedPipe failed 10 consecutive times");
            }
            DWORD backoff = std::min(1000 * (1 << std::min(consecutiveFailures - 1, 4)), 30000);
            Sleep(backoff);
            continue;
        }

        consecutiveFailures = 0;

        // Check stop flag before waiting for connection
        if (stopRequested_.load()) {
            CloseHandle(pipeHandle);
            break;
        }

        // Wait for client connection (with stop check using overlapped I/O)
        OVERLAPPED overlapped = {};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!overlapped.hEvent) {
            CloseHandle(pipeHandle);
            continue;
        }

        BOOL connected = ConnectNamedPipe(pipeHandle, &overlapped);
        DWORD lastError = GetLastError();

        if (!connected && lastError == ERROR_IO_PENDING) {
            // Wait for connection or stop event
            HANDLE waitHandles[] = { overlapped.hEvent, stopEvent_ };
            DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);

            if (waitResult == WAIT_OBJECT_0 + 1) {
                // Stop requested
                CancelIo(pipeHandle);
                CloseHandle(overlapped.hEvent);
                CloseHandle(pipeHandle);
                break;
            }

            // Check if connection succeeded
            DWORD bytesTransferred;
            if (!GetOverlappedResult(pipeHandle, &overlapped, &bytesTransferred, FALSE)) {
                CloseHandle(overlapped.hEvent);
                CloseHandle(pipeHandle);
                continue;
            }
        }
        else if (!connected && lastError != ERROR_PIPE_CONNECTED) {
            CloseHandle(overlapped.hEvent);
            CloseHandle(pipeHandle);
            continue;
        }

        CloseHandle(overlapped.hEvent);

        // Handle client in this thread (simple approach)
        // For high-concurrency, use thread pool
        HandleClient(pipeHandle);
        CloseHandle(pipeHandle);
    }

    LOG_INFO(L"IPC: Server loop ended");
}

void IPCServer::HandleClient(HANDLE pipeHandle) {
    // Read message header (Overlapped I/O with 5-second timeout)
    IPCMessage header;
    DWORD bytesRead = 0;

    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return;

    BOOL ok = ReadFile(pipeHandle, &header, sizeof(header), nullptr, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        HANDLE waits[] = { ov.hEvent, stopEvent_ };
        DWORD wr = WaitForMultipleObjects(2, waits, FALSE, 5000);
        if (wr != WAIT_OBJECT_0) {
            CancelIo(pipeHandle);
            CloseHandle(ov.hEvent);
            return;
        }
        if (!GetOverlappedResult(pipeHandle, &ov, &bytesRead, FALSE)) {
            CloseHandle(ov.hEvent);
            return;
        }
    } else if (ok) {
        GetOverlappedResult(pipeHandle, &ov, &bytesRead, FALSE);
    } else {
        CloseHandle(ov.hEvent);
        return;
    }
    CloseHandle(ov.hEvent);

    if (bytesRead != sizeof(header)) return;

    // Authorization check before processing command
    AuthResult auth = AuthorizeClient(pipeHandle, header.command);
    if (auth != AuthResult::AUTHORIZED) {
        LOG_ALERT(L"IPC: Unauthorized command attempt cmd=" +
                  std::to_wstring(static_cast<uint32_t>(header.command)) +
                  L" auth=" + std::to_wstring(static_cast<int>(auth)));
        SendResponse(pipeHandle, IPCResponse::RESP_ERROR_ACCESS_DENIED,
                     "{\"error\": \"Access denied\"}");
        return;
    }

    // Read message data (with size validation)
    std::string data;
    if (header.dataLength > 0) {
        if (header.dataLength >= UNLEAF_MAX_IPC_DATA_SIZE) {
            SendResponse(pipeHandle, IPCResponse::RESP_ERROR_INVALID_INPUT,
                         R"({"error": "Data too large"})");
            return;
        }
        data.resize(header.dataLength);

        OVERLAPPED ov2 = {};
        ov2.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov2.hEvent) return;

        ok = ReadFile(pipeHandle, &data[0], header.dataLength, nullptr, &ov2);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            HANDLE waits[] = { ov2.hEvent, stopEvent_ };
            DWORD wr = WaitForMultipleObjects(2, waits, FALSE, 5000);
            if (wr != WAIT_OBJECT_0) {
                CancelIo(pipeHandle);
                CloseHandle(ov2.hEvent);
                return;
            }
            if (!GetOverlappedResult(pipeHandle, &ov2, &bytesRead, FALSE) ||
                bytesRead != header.dataLength) {
                CloseHandle(ov2.hEvent);
                return;
            }
        } else if (ok) {
            GetOverlappedResult(pipeHandle, &ov2, &bytesRead, FALSE);
        } else {
            CloseHandle(ov2.hEvent);
            return;
        }
        CloseHandle(ov2.hEvent);
    }

    // Process command
    std::string responseData = ProcessCommand(header.command, data);

    // Send response
    SendResponse(pipeHandle, IPCResponse::RESP_SUCCESS, responseData);
}

CommandPermission IPCServer::GetCommandPermission(IPCCommand cmd) {
    switch (cmd) {
        case IPCCommand::CMD_GET_STATUS:
        case IPCCommand::CMD_GET_LOGS:
        case IPCCommand::CMD_GET_STATS:
        case IPCCommand::CMD_GET_CONFIG:
        case IPCCommand::CMD_HEALTH_CHECK:
            return CommandPermission::PUBLIC;

        case IPCCommand::CMD_ADD_TARGET:
        case IPCCommand::CMD_REMOVE_TARGET:
        case IPCCommand::CMD_SET_INTERVAL:
        case IPCCommand::CMD_SET_LOG_ENABLED:
            return CommandPermission::ADMIN;

        case IPCCommand::CMD_STOP_SERVICE:
            return CommandPermission::SYSTEM_ONLY;

        default:
            return CommandPermission::SYSTEM_ONLY;  // Unknown commands require highest privilege
    }
}

AuthResult IPCServer::AuthorizeClient(HANDLE pipeHandle, IPCCommand cmd) {
    CommandPermission required = GetCommandPermission(cmd);

    // PUBLIC commands are always allowed
    if (required == CommandPermission::PUBLIC) {
        return AuthResult::AUTHORIZED;
    }

    // Impersonate the client to check their credentials
    if (!ImpersonateNamedPipeClient(pipeHandle)) {
        return AuthResult::ERROR_IMPERSONATION;
    }

    // Get the thread token (impersonated)
    HANDLE hToken = nullptr;
    if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, FALSE, &hToken)) {
        RevertToSelf();
        return AuthResult::ERROR_TOKEN;
    }

    bool authorized = false;

    if (required == CommandPermission::SYSTEM_ONLY) {
        // SYSTEM_ONLY: Allow SYSTEM account OR elevated administrators
        authorized = IsTokenSystem(hToken) || IsTokenAdmin(hToken);
    } else if (required == CommandPermission::ADMIN) {
        // ADMIN: Require administrator group membership
        authorized = IsTokenAdmin(hToken);
    }

    CloseHandle(hToken);
    RevertToSelf();

    return authorized ? AuthResult::AUTHORIZED : AuthResult::UNAUTHORIZED;
}

static std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (len <= 0) return L"";
    std::wstring wide(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &wide[0], len);
    return wide;
}



static const char* GetModeString(OperationMode mode) {
    switch (mode) {
        case OperationMode::NORMAL: return "NORMAL";
        case OperationMode::DEGRADED_ETW: return "DEGRADED_ETW";
        case OperationMode::DEGRADED_CONFIG: return "DEGRADED_CONFIG";
        default: return "UNKNOWN";
    }
}

std::string IPCServer::ProcessCommand(IPCCommand cmd, const std::string& data) {
    CSLockGuard lock(handlerCs_);

    // Input validation BEFORE handler dispatch
    switch (cmd) {
        case IPCCommand::CMD_ADD_TARGET:
        case IPCCommand::CMD_REMOVE_TARGET: {
            if (data.empty()) {
                LOG_ALERT(L"IPC: Validation failed - empty process name");
                return "{\"error\": \"Process name required\"}";
            }
            std::wstring processName = Utf8ToWide(data);
            if (!IsValidProcessName(processName)) {
                LOG_ALERT(L"IPC: Validation failed - invalid process name '" + processName + L"'");
                return "{\"error\": \"Invalid process name\"}";
            }
            // Validation passed - call handler
            auto it = handlers_.find(cmd);
            if (it != handlers_.end() && it->second) {
                return it->second(data);
            }
            return R"({"error": "Handler not registered"})";
        }

        case IPCCommand::CMD_SET_INTERVAL: {
            if (data.size() != sizeof(uint32_t)) {
                LOG_ALERT(L"IPC: Validation failed - invalid interval format");
                return R"({"error": "Invalid interval format"})";
            }
            uint32_t interval = *reinterpret_cast<const uint32_t*>(data.data());
            if (interval < UNLEAF_MIN_INTERVAL_MS || interval > UNLEAF_MAX_INTERVAL_MS) {
                LOG_ALERT(L"IPC: Validation failed - interval " + std::to_wstring(interval) +
                          L" out of range (10-60000ms)");
                return "{\"error\": \"Interval out of range (10-60000ms)\"}";
            }
            // Validation passed - call handler
            auto it = handlers_.find(cmd);
            if (it != handlers_.end() && it->second) {
                return it->second(data);
            }
            return R"({"error": "Handler not registered"})";
        }

        default:
            break;
    }

    // Check for registered handler (other commands)
    auto it = handlers_.find(cmd);
    if (it != handlers_.end() && it->second) {
        return it->second(data);
    }

    // Default handlers
    switch (cmd) {
        case IPCCommand::CMD_GET_STATUS: {
            std::ostringstream oss;
            oss << "{\"running\": true, \"version\": \"2.00\"}";
            return oss.str();
        }
        case IPCCommand::CMD_STOP_SERVICE: {
            // Signal stop (handled by service main)
            return "{\"result\": \"stopping\"}";
        }
        case IPCCommand::CMD_GET_LOGS: {
            // Parse offset from request data
            uint64_t clientOffset = 0;
            if (data.size() >= sizeof(LogRequest)) {
                const LogRequest* req = reinterpret_cast<const LogRequest*>(data.data());
                clientOffset = req->offset;
            }
            return GetLogsFromOffset(clientOffset);
        }
        case IPCCommand::CMD_GET_STATS: {
            // Return the current number of tracked processes as a 4-byte binary
            uint32_t count = static_cast<uint32_t>(EngineCore::Instance().GetActiveProcessCount());
            return std::string(reinterpret_cast<const char*>(&count), sizeof(uint32_t));
        }
        case IPCCommand::CMD_HEALTH_CHECK: {
            // Health check API
            auto health = EngineCore::Instance().GetHealthInfo();

            // Determine overall status
            const char* status = "healthy";
            if (!health.engineRunning) {
                status = "unhealthy";
            } else if (!health.etwHealthy || health.mode != OperationMode::NORMAL) {
                status = "degraded";
            }

            // Build JSON response
            std::ostringstream oss;
            oss << "{";
            oss << "\"status\":\"" << status << "\",";
            oss << "\"uptime_seconds\":" << (health.uptimeMs / 1000) << ",";
            oss << "\"engine\":{";
            oss << "\"running\":" << (health.engineRunning ? "true" : "false") << ",";
            oss << "\"mode\":\"" << GetModeString(health.mode) << "\",";
            oss << "\"active_processes\":" << health.activeProcesses << ",";
            oss << "\"total_violations\":" << health.totalViolations << ",";
            oss << "\"phases\":{";
            oss << "\"aggressive\":" << health.aggressiveCount << ",";
            oss << "\"stable\":" << health.stableCount << ",";
            oss << "\"persistent\":" << health.persistentCount;
            oss << "}";
            oss << "},";
            oss << "\"etw\":{";
            oss << "\"healthy\":" << (health.etwHealthy ? "true" : "false") << ",";
            oss << "\"event_count\":" << health.etwEventCount;
            oss << "},";
            oss << "\"wakeups\":{";
            oss << "\"config_change\":" << health.wakeupConfigChange << ",";
            oss << "\"safety_net\":" << health.wakeupSafetyNet << ",";
            oss << "\"enforcement_request\":" << health.wakeupEnforcementRequest << ",";
            oss << "\"process_exit\":" << health.wakeupProcessExit;
            oss << "},";
            oss << "\"enforcement\":{";
            oss << "\"persistent_applied\":" << health.persistentEnforceApplied << ",";
            oss << "\"persistent_skipped\":" << health.persistentEnforceSkipped;
            oss << "},";
            oss << "\"errors\":{";
            oss << "\"access_denied\":" << health.error5Count << ",";
            oss << "\"invalid_parameter\":" << health.error87Count << ",";
            oss << "\"shutdown_warnings\":" << health.shutdownWarnings;
            oss << "},";
            oss << "\"config\":{";
            oss << "\"changes_detected\":" << health.configChangeDetected << ",";
            oss << "\"reloads\":" << health.configReloadCount;
            oss << "},";
            oss << "\"ipc\":{\"healthy\":true}";
            oss << "}";
            return oss.str();
        }
        case IPCCommand::CMD_SET_LOG_ENABLED: {
            if (data.empty()) {
                return R"({"error": "Missing enabled flag"})";
            }

            bool enabled = (data[0] != 0);

            // Update logger
            LightweightLogger::Instance().SetEnabled(enabled);

            // Update config
            UnLeafConfig::Instance().SetLogEnabled(enabled);
            UnLeafConfig::Instance().Save();

            // Log the change
            if (enabled) {
                LOG_INFO(L"Service: Log output enabled via IPC");
            } else {
                LOG_INFO(L"Service: Log output disabled via IPC");
            }

            return R"({"success": true})";
        }
        default:
            return R"({"error": "Unknown command"})";
    }
}

std::string IPCServer::GetLogsFromOffset(uint64_t clientOffset) {
    const std::wstring& logPath = LightweightLogger::Instance().GetLogPath();

    // Prepare empty response header
    LogResponseHeader header = { 0, 0 };

    if (logPath.empty()) {
        std::string result(sizeof(header), '\0');
        memcpy(&result[0], &header, sizeof(header));
        return result;
    }

    // Open log file for reading
    HANDLE hFile = CreateFileW(
        logPath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (hFile == INVALID_HANDLE_VALUE) {
        std::string result(sizeof(header), '\0');
        memcpy(&result[0], &header, sizeof(header));
        return result;
    }

    // Get file size
    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart == 0) {
        CloseHandle(hFile);
        std::string result(sizeof(header), '\0');
        memcpy(&result[0], &header, sizeof(header));
        return result;
    }

    uint64_t currentSize = static_cast<uint64_t>(fileSize.QuadPart);

    // If file was truncated (rotated), reset offset
    if (clientOffset > currentSize) {
        clientOffset = 0;
    }

    // If no new data, return header only
    if (clientOffset >= currentSize) {
        header.newOffset = currentSize;
        header.dataLength = 0;
        CloseHandle(hFile);
        std::string result(sizeof(header), '\0');
        memcpy(&result[0], &header, sizeof(header));
        return result;
    }

    const uint64_t maxRead = UNLEAF_MAX_LOG_READ_SIZE;
    uint64_t available = currentSize - clientOffset;
    uint64_t toRead = (available > maxRead) ? maxRead : available;

    // Seek to client's offset
    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(clientOffset);
    SetFilePointerEx(hFile, li, nullptr, FILE_BEGIN);

    // Read data
    std::string logData(static_cast<size_t>(toRead), '\0');
    DWORD bytesRead = 0;
    if (!ReadFile(hFile, &logData[0], static_cast<DWORD>(toRead), &bytesRead, nullptr)) {
        CloseHandle(hFile);
        std::string result(sizeof(header), '\0');
        memcpy(&result[0], &header, sizeof(header));
        return result;
    }

    CloseHandle(hFile);
    logData.resize(bytesRead);

    // Set new offset
    header.newOffset = clientOffset + bytesRead;
    header.dataLength = bytesRead;

    // Build response: header + log data
    std::string result(sizeof(header) + logData.size(), '\0');
    memcpy(&result[0], &header, sizeof(header));
    if (!logData.empty()) {
        memcpy(&result[sizeof(header)], logData.data(), logData.size());
    }

    return result;
}

bool IPCServer::SendResponse(HANDLE pipe, IPCResponse response, const std::string& data) {
    IPCResponseMessage header;
    header.response = response;
    header.dataLength = static_cast<uint32_t>(data.size());

    DWORD bytesWritten = 0;

    // Write header
    if (!WriteFile(pipe, &header, sizeof(header), &bytesWritten, nullptr)) {
        return false;
    }

    // Write data
    if (!data.empty()) {
        if (!WriteFile(pipe, data.c_str(),
                       static_cast<DWORD>(data.size()), &bytesWritten, nullptr)) {
            return false;
        }
    }

    FlushFileBuffers(pipe);
    return true;
}

} // namespace unleaf
