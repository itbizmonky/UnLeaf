// UnLeaf v1.00 - IPC Client Implementation

#include "ipc_client.h"
#include "../service/ipc_server.h"  // For message structures
#include "../common/config.h"

namespace unleaf {

IPCClient::IPCClient()
    : pipeHandle_(INVALID_HANDLE_VALUE) {
    QueryPerformanceFrequency(&qpcFreq_);
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
    std::optional<std::string> result = std::nullopt;
    bool success = false;
    HANDLE localPipe = INVALID_HANDLE_VALUE;

    // ① エントリ: totalRequests_ インクリメント
    totalRequests_.fetch_add(1, std::memory_order_relaxed);

    // QPC 計測開始（DEBUG ログレベル時のみ）
    const bool measureLatency = (qpcFreq_.QuadPart > 0 &&
        UnLeafConfig::Instance().GetLogLevel() >= LogLevel::LOG_DEBUG);
    LARGE_INTEGER qpcStart{};
    if (measureLatency) QueryPerformanceCounter(&qpcStart);

    // ② pipeHandle_ 取得（ロック内のみ）— I/O はロック外で実行
    {
        CSLockGuard lock(cs_);
        if (pipeHandle_ != INVALID_HANDLE_VALUE) {
            localPipe = pipeHandle_;
            pipeHandle_ = INVALID_HANDLE_VALUE;  // swap: I/O 中は他スレッドから触れない
        }
    }

    // ③ 接続（未接続時のみ）— ロック外
    if (localPipe == INVALID_HANDLE_VALUE) {
        localPipe = CreateFileW(PIPE_NAME,
            GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);

        if (localPipe == INVALID_HANDLE_VALUE) {
            reconnectOpenFail_.fetch_add(1, std::memory_order_relaxed);
            goto cleanup;
        }

        DWORD mode = PIPE_READMODE_MESSAGE;
        SetNamedPipeHandleState(localPipe, &mode, nullptr, nullptr);
        openCount_.fetch_add(1, std::memory_order_relaxed);
    }

    // ④ ヘッダ送信 — ロック外
    {
        IPCMessage header;
        header.command    = cmd;
        header.dataLength = static_cast<uint32_t>(data.size());
        DWORD written;
        if (!WriteFile(localPipe, &header, sizeof(header), &written, nullptr)) {
            DWORD err = GetLastError();
            if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED || err == ERROR_NO_DATA)
                reconnectBroken_.fetch_add(1, std::memory_order_relaxed);
            else if (err == ERROR_SEM_TIMEOUT || err == WAIT_TIMEOUT)
                reconnectTimeout_.fetch_add(1, std::memory_order_relaxed);
            goto cleanup;
        }
    }

    // ⑤ データ送信 — ロック外
    if (!data.empty()) {
        DWORD written;
        if (!WriteFile(localPipe, data.c_str(),
                       static_cast<DWORD>(data.size()), &written, nullptr)) {
            goto cleanup;
        }
    }

    // ⑥ レスポンスヘッダ受信 — ロック外
    {
        IPCResponseMessage response;
        DWORD bytesRead;
        if (!ReadFile(localPipe, &response, sizeof(response), &bytesRead, nullptr) ||
            bytesRead != sizeof(response)) {
            goto cleanup;
        }

        // ⑦ レスポンスデータ受信 — ロック外
        std::string responseData;
        if (response.dataLength > 0 && response.dataLength < 65536) {
            responseData.resize(response.dataLength);
            if (!ReadFile(localPipe, &responseData[0], response.dataLength,
                          &bytesRead, nullptr) ||
                bytesRead != response.dataLength) {
                goto cleanup;
            }
        }

        // ⑧ QPC 計測終了（ReadFile 完了直後）
        if (measureLatency) {
            LARGE_INTEGER qpcEnd;
            QueryPerformanceCounter(&qpcEnd);
            uint64_t us = static_cast<uint64_t>(
                (qpcEnd.QuadPart - qpcStart.QuadPart) * 1000000LL / qpcFreq_.QuadPart);
            latencySumUs_.fetch_add(us, std::memory_order_relaxed);
            latencyCount_.fetch_add(1, std::memory_order_relaxed);
        }

        result  = std::move(responseData);
        success = true;
    }

cleanup:
    // pipeHandle_ が別スレッドに再利用されていない場合のみ INVALID に戻す
    {
        CSLockGuard lock(cs_);
        if (pipeHandle_ == localPipe)
            pipeHandle_ = INVALID_HANDLE_VALUE;
    }
    if (localPipe != INVALID_HANDLE_VALUE)
        CloseHandle(localPipe);

    if (success)
        successfulRequests_.fetch_add(1, std::memory_order_relaxed);
    else
        failedRequests_.fetch_add(1, std::memory_order_relaxed);

    return result;
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

IPCClient::QualityStats IPCClient::SnapAndResetQuality() {
    uint64_t sum   = latencySumUs_.exchange(0, std::memory_order_relaxed);
    uint32_t count = latencyCount_.exchange(0, std::memory_order_relaxed);
    float avgMs = (count > 0)
        ? static_cast<float>(sum) / (1000.f * count)
        : 0.f;
    return { avgMs,
             reconnectTimeout_.exchange(0, std::memory_order_relaxed),
             reconnectBroken_.exchange(0, std::memory_order_relaxed),
             reconnectManual_.exchange(0, std::memory_order_relaxed),
             reconnectOpenFail_.exchange(0, std::memory_order_relaxed) };
}

} // namespace unleaf
