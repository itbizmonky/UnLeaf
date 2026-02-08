// UnLeaf v1.00 - Windows Service Main Implementation

#include "service_main.h"
#include "engine_core.h"
#include "ipc_server.h"
#include "../common/logger.h"
#include "../common/config.h"
#include <iostream>

namespace unleaf {

// Enable SE_DEBUG_NAME privilege for manipulating other processes' EcoQoS
static bool EnableDebugPrivilege() {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        return false;
    }

    TOKEN_PRIVILEGES tp = {};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    if (!LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &tp.Privileges[0].Luid)) {
        CloseHandle(hToken);
        return false;
    }

    BOOL result = AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    DWORD error = GetLastError();
    CloseHandle(hToken);

    // AdjustTokenPrivileges returns TRUE even if some privileges were not adjusted
    // Check GetLastError for ERROR_NOT_ALL_ASSIGNED
    return result && (error == ERROR_SUCCESS);
}

ServiceMain& ServiceMain::Instance() {
    static ServiceMain instance;
    return instance;
}

ServiceMain::ServiceMain()
    : statusHandle_(nullptr)
    , serviceState_(SERVICE_STOPPED)
    , stopRequested_(false)
    , stopEvent_(nullptr) {
}

bool ServiceMain::RunAsService() {
    SERVICE_TABLE_ENTRYW serviceTable[] = {
        { const_cast<LPWSTR>(SERVICE_NAME), ServiceMainFunc },
        { nullptr, nullptr }
    };

    if (!StartServiceCtrlDispatcherW(serviceTable)) {
        DWORD error = GetLastError();
        if (error == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
            // Not running as service, might be debug mode
            return false;
        }
        return false;
    }

    return true;
}

bool ServiceMain::RunAsConsole() {
    std::wcout << L"--- UnLeaf Debug Mode ---" << std::endl;
    std::wcout << L"Press Ctrl+C to stop" << std::endl;

    // Enable SE_DEBUG_NAME privilege
    if (EnableDebugPrivilege()) {
        std::wcout << L"Debug privilege enabled." << std::endl;
    } else {
        std::wcout << L"Warning: Could not enable debug privilege." << std::endl;
    }

    baseDir_ = GetBaseDirectory();

    // Initialize logger with console output
    LightweightLogger::Instance().Initialize(baseDir_);
    LightweightLogger::Instance().SetConsoleOutput(true);

    // Initialize config
    UnLeafConfig::Instance().Initialize(baseDir_);

    // Initialize and start engine
    if (!EngineCore::Instance().Initialize(baseDir_)) {
        std::wcerr << L"Failed to initialize engine" << std::endl;
        return false;
    }

    // Create stop event
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    // Set console control handler
    SetConsoleCtrlHandler([](DWORD ctrlType) -> BOOL {
        if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT) {
            ServiceMain::Instance().RequestStop();
            return TRUE;
        }
        return FALSE;
    }, TRUE);

    // Start engine
    EngineCore::Instance().Start();

    LOG_DEBUG(L"Debug mode: Engine started, waiting for stop signal");

    // Wait for stop
    WaitForSingleObject(stopEvent_, INFINITE);

    // Cleanup
    EngineCore::Instance().Stop();
    LightweightLogger::Instance().Shutdown();

    std::wcout << L"Debug mode: Stopped" << std::endl;
    return true;
}

void ServiceMain::RequestStop() {
    stopRequested_ = true;
    if (stopEvent_) {
        SetEvent(stopEvent_);
    }
}

void WINAPI ServiceMain::ServiceMainFunc(DWORD argc, LPWSTR* argv) {
    (void)argc;
    (void)argv;

    ServiceMain& service = ServiceMain::Instance();

    // Register service control handler
    service.statusHandle_ = RegisterServiceCtrlHandlerExW(
        SERVICE_NAME,
        ServiceCtrlHandler,
        nullptr
    );

    if (!service.statusHandle_) {
        return;
    }

    // Report starting status
    service.ReportStatus(SERVICE_START_PENDING, 0, 3000);

    // Create stop event
    service.stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!service.stopEvent_) {
        service.ReportStatus(SERVICE_STOPPED, GetLastError());
        return;
    }

    // Enable SE_DEBUG_NAME privilege for EcoQoS manipulation on other processes
    if (EnableDebugPrivilege()) {
        // Privilege enabled successfully
    } else {
        // Continue without debug privilege - some processes may not be accessible
    }

    // Get base directory
    service.baseDir_ = service.GetBaseDirectory();

    // Initialize components
    if (!LightweightLogger::Instance().Initialize(service.baseDir_)) {
        service.ReportStatus(SERVICE_STOPPED, 1);
        return;
    }

    if (!UnLeafConfig::Instance().Initialize(service.baseDir_)) {
        service.ReportStatus(SERVICE_STOPPED, 2);
        return;
    }

    if (!EngineCore::Instance().Initialize(service.baseDir_)) {
        service.ReportStatus(SERVICE_STOPPED, 3);
        return;
    }

    if (!IPCServer::Instance().Initialize()) {
        service.ReportStatus(SERVICE_STOPPED, 4);
        return;
    }

    // Report running status
    service.ReportStatus(SERVICE_RUNNING);

    LOG_INFO(L"Service started");

    // Start IPC server
    IPCServer::Instance().Start();

    // Run main loop
    service.Run();

    // Cleanup
    IPCServer::Instance().Stop();
    EngineCore::Instance().Stop();
    LightweightLogger::Instance().Shutdown();

    if (service.stopEvent_) {
        CloseHandle(service.stopEvent_);
        service.stopEvent_ = nullptr;
    }

    service.ReportStatus(SERVICE_STOPPED);
}

DWORD WINAPI ServiceMain::ServiceCtrlHandler(
    DWORD dwControl,
    DWORD dwEventType,
    LPVOID lpEventData,
    LPVOID lpContext)
{
    (void)dwEventType;
    (void)lpEventData;
    (void)lpContext;

    ServiceMain& service = ServiceMain::Instance();

    switch (dwControl) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            service.ReportStatus(SERVICE_STOP_PENDING, 0, 3000);
            service.RequestStop();
            return NO_ERROR;

        case SERVICE_CONTROL_INTERROGATE:
            return NO_ERROR;

        default:
            return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

void ServiceMain::ReportStatus(DWORD state, DWORD exitCode, DWORD waitHint) {
    static DWORD checkPoint = 1;

    SERVICE_STATUS status = {};
    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState = state;
    status.dwWin32ExitCode = exitCode;
    status.dwWaitHint = waitHint;

    if (state == SERVICE_START_PENDING) {
        status.dwControlsAccepted = 0;
    } else {
        status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    }

    if (state == SERVICE_RUNNING || state == SERVICE_STOPPED) {
        status.dwCheckPoint = 0;
    } else {
        status.dwCheckPoint = checkPoint++;
    }

    serviceState_ = state;
    SetServiceStatus(statusHandle_, &status);
}

void ServiceMain::Run() {
    // Start engine
    EngineCore::Instance().Start();

    // Wait for stop signal
    while (!stopRequested_.load()) {
        DWORD result = WaitForSingleObject(stopEvent_, 1000);
        if (result == WAIT_OBJECT_0) {
            break;
        }
    }

    LOG_INFO(L"Service stopping");
}

std::wstring ServiceMain::GetBaseDirectory() {
    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);

    if (len == 0 || len >= MAX_PATH) {
        return L".";
    }

    std::wstring fullPath(path);
    size_t pos = fullPath.find_last_of(L"\\/");
    if (pos != std::wstring::npos) {
        return fullPath.substr(0, pos);
    }

    return L".";
}

} // namespace unleaf
