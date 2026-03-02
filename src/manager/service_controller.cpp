// UnLeaf v1.00 - Service Controller Implementation

#include "service_controller.h"
#include <shellapi.h>
#include <sstream>

namespace unleaf {

ServiceController::ServiceController() {
}

ServiceState ServiceController::GetServiceState() {
    ScopedSCMHandle scmHandle = MakeScopedSCMHandle(
        OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)
    );

    if (!scmHandle) {
        lastError_ = L"Failed to open SCM";
        return ServiceState::Unknown;
    }

    ScopedSCMHandle serviceHandle = MakeScopedSCMHandle(
        OpenServiceW(scmHandle.get(), SERVICE_NAME, SERVICE_QUERY_STATUS)
    );

    if (!serviceHandle) {
        DWORD error = ::GetLastError();
        if (error == ERROR_SERVICE_DOES_NOT_EXIST) {
            return ServiceState::NotInstalled;
        }
        lastError_ = L"Failed to open service";
        return ServiceState::Unknown;
    }

    SERVICE_STATUS status;
    if (!QueryServiceStatus(serviceHandle.get(), &status)) {
        lastError_ = L"Failed to query status";
        return ServiceState::Unknown;
    }

    switch (status.dwCurrentState) {
        case SERVICE_STOPPED:
            return ServiceState::Stopped;
        case SERVICE_START_PENDING:
            return ServiceState::StartPending;
        case SERVICE_STOP_PENDING:
            return ServiceState::StopPending;
        case SERVICE_RUNNING:
            return ServiceState::Running;
        case SERVICE_CONTINUE_PENDING:
            return ServiceState::ContinuePending;
        case SERVICE_PAUSE_PENDING:
            return ServiceState::PausePending;
        case SERVICE_PAUSED:
            return ServiceState::Paused;
        default:
            return ServiceState::Unknown;
    }
}

bool ServiceController::InstallService(const std::wstring& exePath) {
    ScopedSCMHandle scmHandle = MakeScopedSCMHandle(
        OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE)
    );

    if (!scmHandle) {
        lastError_ = L"Failed to open SCM (requires admin)";
        return false;
    }

    ScopedSCMHandle serviceHandle = MakeScopedSCMHandle(
        CreateServiceW(
            scmHandle.get(),
            SERVICE_NAME,
            SERVICE_DISPLAY_NAME,
            SERVICE_ALL_ACCESS,
            SERVICE_WIN32_OWN_PROCESS,
            SERVICE_AUTO_START,
            SERVICE_ERROR_NORMAL,
            exePath.c_str(),
            nullptr,  // No load ordering group
            nullptr,  // No tag
            nullptr,  // No dependencies
            nullptr,  // LocalSystem account
            nullptr   // No password
        )
    );

    if (!serviceHandle) {
        DWORD error = ::GetLastError();
        if (error == ERROR_SERVICE_EXISTS) {
            // Service already exists, this is OK
            return true;
        }
        std::wostringstream oss;
        oss << L"Failed to create service (error: " << error << L")";
        lastError_ = oss.str();
        return false;
    }

    // Set description
    SERVICE_DESCRIPTIONW desc;
    desc.lpDescription = const_cast<LPWSTR>(SERVICE_DESCRIPTION);
    ChangeServiceConfig2W(serviceHandle.get(), SERVICE_CONFIG_DESCRIPTION, &desc);

    return true;
}

bool ServiceController::UninstallService() {
    ScopedSCMHandle scmHandle = MakeScopedSCMHandle(
        OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS)
    );

    if (!scmHandle) {
        lastError_ = L"Failed to open SCM (requires admin)";
        return false;
    }

    // Open service with all required access rights
    ScopedSCMHandle serviceHandle = MakeScopedSCMHandle(
        OpenServiceW(scmHandle.get(), SERVICE_NAME,
            SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE)
    );

    if (!serviceHandle) {
        DWORD error = ::GetLastError();
        if (error == ERROR_SERVICE_DOES_NOT_EXIST) {
            return true;  // Already uninstalled
        }
        std::wostringstream oss;
        oss << L"Failed to open service (error: " << error << L")";
        lastError_ = oss.str();
        return false;
    }

    // Step 1: Query current status
    SERVICE_STATUS status;
    if (!QueryServiceStatus(serviceHandle.get(), &status)) {
        lastError_ = L"Failed to query service status";
        return false;
    }

    // Step 2: Send stop command if not already stopped
    if (status.dwCurrentState != SERVICE_STOPPED) {
        if (!ControlService(serviceHandle.get(), SERVICE_CONTROL_STOP, &status)) {
            DWORD error = ::GetLastError();
            if (error != ERROR_SERVICE_NOT_ACTIVE) {
                std::wostringstream oss;
                oss << L"Failed to send stop command (error: " << error << L")";
                lastError_ = oss.str();
                return false;
            }
            // ERROR_SERVICE_NOT_ACTIVE means already stopped - continue
        }

        // Step 3: Wait for SERVICE_STOPPED with timeout (max 5 seconds)
        constexpr DWORD TIMEOUT_MS = 5000;
        constexpr DWORD POLL_INTERVAL_MS = 100;
        DWORD waitTime = 0;

        while (waitTime < TIMEOUT_MS) {
            if (!QueryServiceStatus(serviceHandle.get(), &status)) {
                lastError_ = L"Failed to query service status while waiting";
                return false;
            }

            if (status.dwCurrentState == SERVICE_STOPPED) {
                break;  // Successfully stopped
            }

            Sleep(POLL_INTERVAL_MS);
            waitTime += POLL_INTERVAL_MS;
        }

        // Check if stop succeeded
        if (status.dwCurrentState != SERVICE_STOPPED) {
            lastError_ = L"Timeout waiting for service to stop";
            return false;
        }
    }

    // Step 4: Delete the service (now safely stopped)
    if (!DeleteService(serviceHandle.get())) {
        DWORD error = ::GetLastError();
        if (error == ERROR_SERVICE_MARKED_FOR_DELETE) {
            return true;  // Will be deleted when handles close
        }
        std::wostringstream oss;
        oss << L"Failed to delete service (error: " << error << L")";
        lastError_ = oss.str();
        return false;
    }

    return true;
}

bool ServiceController::StartService() {
    ScopedSCMHandle scmHandle = MakeScopedSCMHandle(
        OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)
    );

    if (!scmHandle) {
        lastError_ = L"Failed to open SCM";
        return false;
    }

    ScopedSCMHandle serviceHandle = MakeScopedSCMHandle(
        OpenServiceW(scmHandle.get(), SERVICE_NAME, SERVICE_START)
    );

    if (!serviceHandle) {
        lastError_ = L"Failed to open service";
        return false;
    }

    if (!::StartServiceW(serviceHandle.get(), 0, nullptr)) {
        DWORD error = ::GetLastError();
        if (error == ERROR_SERVICE_ALREADY_RUNNING) {
            return true;
        }
        std::wostringstream oss;
        oss << L"Failed to start service (error: " << error << L")";
        lastError_ = oss.str();
        return false;
    }

    return true;
}

bool ServiceController::StopService() {
    ScopedSCMHandle scmHandle = MakeScopedSCMHandle(
        OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)
    );

    if (!scmHandle) {
        lastError_ = L"Failed to open SCM";
        return false;
    }

    ScopedSCMHandle serviceHandle = MakeScopedSCMHandle(
        OpenServiceW(scmHandle.get(), SERVICE_NAME, SERVICE_STOP | SERVICE_QUERY_STATUS)
    );

    if (!serviceHandle) {
        DWORD error = ::GetLastError();
        if (error == ERROR_SERVICE_DOES_NOT_EXIST) {
            return true;
        }
        std::wostringstream oss;
        oss << L"Failed to open service (error: " << error << L")";
        lastError_ = oss.str();
        return false;
    }

    SERVICE_STATUS status;
    if (!ControlService(serviceHandle.get(), SERVICE_CONTROL_STOP, &status)) {
        DWORD error = ::GetLastError();
        if (error == ERROR_SERVICE_NOT_ACTIVE) {
            return true;  // Already stopped
        }
        std::wostringstream oss;
        oss << L"Failed to stop service (error: " << error << L")";
        lastError_ = oss.str();
        return false;
    }

    // Wait for SERVICE_STOPPED with timeout (max 5 seconds)
    constexpr DWORD TIMEOUT_MS = 5000;
    constexpr DWORD POLL_INTERVAL_MS = 100;
    DWORD waitTime = 0;

    while (waitTime < TIMEOUT_MS) {
        if (!QueryServiceStatus(serviceHandle.get(), &status)) {
            lastError_ = L"Failed to query service status";
            return false;
        }

        if (status.dwCurrentState == SERVICE_STOPPED) {
            return true;
        }

        Sleep(POLL_INTERVAL_MS);
        waitTime += POLL_INTERVAL_MS;
    }

    lastError_ = L"Timeout waiting for service to stop";
    return false;
}

bool ServiceController::IsRunningAsAdmin() {
    BOOL isAdmin = FALSE;
    PSID adminGroup = nullptr;

    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(
            &ntAuthority, 2,
            SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
            0, 0, 0, 0, 0, 0,
            &adminGroup)) {
        CheckTokenMembership(nullptr, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }

    return isAdmin != FALSE;
}

bool ServiceController::RestartAsAdmin(const std::wstring& args) {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);

    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.lpVerb = L"runas";
    sei.lpFile = path;
    sei.lpParameters = args.c_str();
    sei.nShow = SW_SHOWNORMAL;

    return ShellExecuteExW(&sei) != FALSE;
}

std::wstring ServiceStateToString(ServiceState state) {
    switch (state) {
        case ServiceState::NotInstalled:
            return L"NOT INSTALLED";
        case ServiceState::Stopped:
            return L"STOPPED";
        case ServiceState::StartPending:
            return L"STARTING...";
        case ServiceState::StopPending:
            return L"STOPPING...";
        case ServiceState::Running:
            return L"RUNNING";
        case ServiceState::Paused:
            return L"PAUSED";
        default:
            return L"UNKNOWN";
    }
}

} // namespace unleaf
