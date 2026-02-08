#pragma once
// UnLeaf v1.00 - Windows Service Main
// Service control handler and entry point

#include "../common/types.h"
#include "../common/scoped_handle.h"
#include <string>
#include <atomic>

namespace unleaf {

class ServiceMain {
public:
    // Singleton access
    static ServiceMain& Instance();

    // Run as Windows service
    bool RunAsService();

    // Run in debug/console mode
    bool RunAsConsole();

    // Stop the service
    void RequestStop();

    // Get service status
    DWORD GetServiceState() const { return serviceState_.load(); }

    // Service control handler
    static DWORD WINAPI ServiceCtrlHandler(
        DWORD dwControl,
        DWORD dwEventType,
        LPVOID lpEventData,
        LPVOID lpContext
    );

    // Service main function
    static void WINAPI ServiceMainFunc(DWORD argc, LPWSTR* argv);

private:
    ServiceMain();
    ~ServiceMain() = default;
    ServiceMain(const ServiceMain&) = delete;
    ServiceMain& operator=(const ServiceMain&) = delete;

    // Report service status to SCM
    void ReportStatus(DWORD state, DWORD exitCode = 0, DWORD waitHint = 0);

    // Main service loop
    void Run();

    // Get executable directory
    std::wstring GetBaseDirectory();

    SERVICE_STATUS_HANDLE statusHandle_;
    std::atomic<DWORD> serviceState_;
    std::atomic<bool> stopRequested_;
    HANDLE stopEvent_;

    std::wstring baseDir_;
};

} // namespace unleaf
