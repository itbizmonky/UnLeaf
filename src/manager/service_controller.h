#pragma once
// UnLeaf v1.00 - Service Controller
// Windows Service management (SCM wrapper)

#include "../common/types.h"
#include "../common/scoped_handle.h"
#include <string>

namespace unleaf {

enum class ServiceState {
    Unknown,
    NotInstalled,
    Stopped,
    StartPending,
    StopPending,
    Running,
    ContinuePending,
    PausePending,
    Paused
};

class ServiceController {
public:
    ServiceController();
    ~ServiceController() = default;

    // Query service status
    ServiceState GetServiceState();

    // Install service (requires admin)
    bool InstallService(const std::wstring& exePath);

    // Uninstall service (requires admin)
    bool UninstallService();

    // Start service
    bool StartService();

    // Stop service
    bool StopService();

    // Stop service and wait until fully stopped (with timeout)
    bool StopServiceAndWait(int timeoutMs = 10000);

    // Full uninstall sequence: stop, wait, delete
    bool StopAndUninstallService(int timeoutMs = 10000);

    // Check if running as admin
    static bool IsRunningAsAdmin();

    // Restart as admin
    static bool RestartAsAdmin(const std::wstring& args = L"");

    // Get last error message
    const std::wstring& GetLastError() const { return lastError_; }

private:
    std::wstring lastError_;
};

// Convert state to string
std::wstring ServiceStateToString(ServiceState state);

} // namespace unleaf
