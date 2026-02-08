// UnLeaf v1.00 - Manager Entry Point
// Windows Native C++ GUI Application

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <objbase.h>
#include <shellscalingapi.h>
#include "main_window.h"

#pragma comment(lib, "shcore.lib")

// Addition: Unique mutex name
const wchar_t* MUTEX_NAME = L"Global\\UnLeaf_Dashboard_Unique_Mutex_v200";

// Enable visual styles
#pragma comment(linker,"\"/manifestdependency:type='win32' \
    name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
    processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

int WINAPI wWinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR lpCmdLine,
    _In_ int nCmdShow)
{
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    // Added: Multiple startup check
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, MUTEX_NAME);
    if (hMutex != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
        // Find existing window and bring it to foreground
        HWND hwndExisting = FindWindowW(L"UnLeafDashboard", nullptr);
        if (hwndExisting) {
            ShowWindow(hwndExisting, SW_RESTORE);
            SetForegroundWindow(hwndExisting);
        }
        CloseHandle(hMutex);
        return 0; // Exit since instance already exists
    }

    // Enable DPI awareness (Windows 8.1+)
    SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);

    // Initialize COM
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // Create and run main window
    unleaf::MainWindow mainWindow;

    if (!mainWindow.Initialize(hInstance)) {
        MessageBoxW(nullptr,
            L"Failed to initialize application.",
            L"UnLeaf Manager",
            MB_OK | MB_ICONERROR);
        CoUninitialize();
        return 1;
    }

    int result = mainWindow.Run();

    // Added: Release resources on exit
    if (hMutex) {
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
    }

    CoUninitialize();
    return result;
}
