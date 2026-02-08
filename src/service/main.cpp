// UnLeaf v1.00 - Service Entry Point
// Windows Native C++ Implementation

#include "service_main.h"
#include <iostream>
#include <cstring>

int wmain(int argc, wchar_t* argv[]) {
    // Check for debug mode
    bool debugMode = false;
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"debug") == 0 ||
            _wcsicmp(argv[i], L"-debug") == 0 ||
            _wcsicmp(argv[i], L"--debug") == 0 ||
            _wcsicmp(argv[i], L"/debug") == 0) {
            debugMode = true;
            break;
        }
    }

    if (debugMode) {
        // Run in console/debug mode
        return unleaf::ServiceMain::Instance().RunAsConsole() ? 0 : 1;
    }

    // Try to run as Windows service
    if (!unleaf::ServiceMain::Instance().RunAsService()) {
        // If service dispatch fails, it might mean we're running from console
        // without the debug flag - show help
        std::wcout << L"UnLeaf Service" << std::endl;
        std::wcout << L"====================\n" << std::endl;
        std::wcout << L"Usage:" << std::endl;
        std::wcout << L"  UnLeaf_Service.exe          - Run as Windows Service" << std::endl;
        std::wcout << L"  UnLeaf_Service.exe debug    - Run in debug/console mode\n" << std::endl;
        std::wcout << L"Service Management (Administrator required):" << std::endl;
        std::wcout << L"  sc create UnLeafService binPath= \"<path>\\UnLeaf_Service.exe\" start= auto" << std::endl;
        std::wcout << L"  sc start UnLeafService" << std::endl;
        std::wcout << L"  sc stop UnLeafService" << std::endl;
        std::wcout << L"  sc delete UnLeafService" << std::endl;
        return 1;
    }

    return 0;
}
