// UnLeaf v1.00 - IPC Client Implementation

#include "ipc_client.h"
#include "../service/ipc_server.h"  // For message structures

namespace unleaf {

IPCClient::IPCClient()
    : pipeHandle_(INVALID_HANDLE_VALUE) {
}

IPCClient::~IPCClient() {
    Disconnect();
}

bool IPCClient::Connect() {
    CSLockGuard lock(cs_);

    if (pipeHandle_ != INVALID_HANDLE_VALUE) {
        return true;  // Already connected
    }

    // Try to connect to the named pipe
    pipeHandle_ = CreateFileW(
        PIPE_NAME,
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr
    );

    if (pipeHandle_ == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        if (error == ERROR_PIPE_BUSY) {
            // Wait for pipe to become available (short timeout to avoid blocking shutdown)
            if (!WaitNamedPipeW(PIPE_NAME, 100)) {
                return false;
            }

            // Try again
            pipeHandle_ = CreateFileW(
                PIPE_NAME,
                GENERIC_READ | GENERIC_WRITE,
                0,
                nullptr,
                OPEN_EXISTING,
                0,
                nullptr
            );
        }
    }

    if (pipeHandle_ == INVALID_HANDLE_VALUE) {
        return false;
    }

    // Set pipe to message mode
    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(pipeHandle_, &mode, nullptr, nullptr);

    return true;
}

void IPCClient::Disconnect() {
    CSLockGuard lock(cs_);

    if (pipeHandle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(pipeHandle_);
        pipeHandle_ = INVALID_HANDLE_VALUE;
    }
}

bool IPCClient::IsConnected() const {
    return pipeHandle_ != INVALID_HANDLE_VALUE;
}

std::optional<std::string> IPCClient::SendCommand(IPCCommand cmd, const std::string& data) {
    CSLockGuard lock(cs_);

    // Connect if not already
    if (pipeHandle_ == INVALID_HANDLE_VALUE) {
        pipeHandle_ = CreateFileW(
            PIPE_NAME,
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr
        );

        if (pipeHandle_ == INVALID_HANDLE_VALUE) {
            return std::nullopt;
        }

        DWORD mode = PIPE_READMODE_MESSAGE;
        SetNamedPipeHandleState(pipeHandle_, &mode, nullptr, nullptr);
    }

    // Build and send message
    IPCMessage header;
    header.command = cmd;
    header.dataLength = static_cast<uint32_t>(data.size());

    DWORD bytesWritten;
    if (!WriteFile(pipeHandle_, &header, sizeof(header), &bytesWritten, nullptr)) {
        Disconnect();
        return std::nullopt;
    }

    if (!data.empty()) {
        if (!WriteFile(pipeHandle_, data.c_str(),
                       static_cast<DWORD>(data.size()), &bytesWritten, nullptr)) {
            Disconnect();
            return std::nullopt;
        }
    }

    // Read response header
    IPCResponseMessage response;
    DWORD bytesRead;
    if (!ReadFile(pipeHandle_, &response, sizeof(response), &bytesRead, nullptr) ||
        bytesRead != sizeof(response)) {
        Disconnect();
        return std::nullopt;
    }

    // Read response data
    std::string responseData;
    if (response.dataLength > 0 && response.dataLength < 65536) {
        responseData.resize(response.dataLength);
        if (!ReadFile(pipeHandle_, &responseData[0], response.dataLength,
                      &bytesRead, nullptr) ||
            bytesRead != response.dataLength) {
            Disconnect();
            return std::nullopt;
        }
    }

    // Disconnect after each transaction (stateless)
    Disconnect();

    return responseData;
}

std::optional<std::string> IPCClient::GetServiceStatus() {
    return SendCommand(IPCCommand::CMD_GET_STATUS);
}

bool IPCClient::RequestServiceStop() {
    auto result = SendCommand(IPCCommand::CMD_STOP_SERVICE);
    return result.has_value();
}

bool IPCClient::AddTarget(const std::wstring& name) {
    // Convert to UTF-8
    int size = WideCharToMultiByte(CP_UTF8, 0, name.c_str(),
                                   static_cast<int>(name.size()),
                                   nullptr, 0, nullptr, nullptr);
    std::string utf8Name(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, name.c_str(),
                       static_cast<int>(name.size()),
                       &utf8Name[0], size, nullptr, nullptr);

    auto result = SendCommand(IPCCommand::CMD_ADD_TARGET, utf8Name);
    return result.has_value();
}

bool IPCClient::RemoveTarget(const std::wstring& name) {
    // Convert to UTF-8
    int size = WideCharToMultiByte(CP_UTF8, 0, name.c_str(),
                                   static_cast<int>(name.size()),
                                   nullptr, 0, nullptr, nullptr);
    std::string utf8Name(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, name.c_str(),
                       static_cast<int>(name.size()),
                       &utf8Name[0], size, nullptr, nullptr);

    auto result = SendCommand(IPCCommand::CMD_REMOVE_TARGET, utf8Name);
    return result.has_value();
}

std::optional<std::pair<uint64_t, std::string>> IPCClient::GetLogs(uint64_t fromOffset) {
    // Build request with offset
    LogRequest req;
    req.offset = fromOffset;
    std::string reqData(reinterpret_cast<const char*>(&req), sizeof(req));

    auto result = SendCommand(IPCCommand::CMD_GET_LOGS, reqData);
    if (!result.has_value() || result->size() < sizeof(LogResponseHeader)) {
        return std::nullopt;
    }

    // Parse response header
    LogResponseHeader header;
    memcpy(&header, result->data(), sizeof(header));

    // Extract log data
    std::string logData;
    if (header.dataLength > 0 && result->size() >= sizeof(header) + header.dataLength) {
        logData.assign(result->data() + sizeof(header), header.dataLength);
    }

    return std::make_pair(header.newOffset, std::move(logData));
}

} // namespace unleaf
