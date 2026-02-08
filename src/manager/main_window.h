#pragma once
// UnLeaf v2.00 - Main Window (Python v1.00 Style Dashboard)

#include "../common/types.h"
#include "../common/config.h"
#include "service_controller.h"
#include "ipc_client.h"
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <deque>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>

#pragma comment(lib, "dwmapi.lib")

namespace unleaf {

class MainWindow {
public:
    MainWindow();
    ~MainWindow();

    bool Initialize(HINSTANCE hInstance);
    int Run();

private:
    // Window procedure
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    // UI setup
    void CreateControls();
    void EnableDarkMode();

    // Drawing
    void OnPaint();
    void DrawButton(LPDRAWITEMSTRUCT lpDIS);
    void DrawStatusIndicator(HDC hdc, int x, int y, bool online);

    // State updates
    void UpdateServiceStatus();
    void UpdateEngineStatus();
    void RefreshTargetList();
    void AppendLog(const std::wstring& message);

    // Event handlers
    void OnStartService();
    void OnStopService();
    void OnAddTarget();
    void OnRemoveTarget();
    void OnSelectProcess();
    void OnToggleLogEnabled();  // v7.93: Log ON/OFF toggle

    // Context menus
    void ShowTargetListContextMenu(int x, int y);
    void ShowLogEditContextMenu(int x, int y);
    void OnToggleTargetEnabled();
    void OnCopyLogSelection();
    void OnSelectAllLog();
    void OnClearLog();
    void OnOpenLogFile();

    // System tray
    void CreateTrayIcon();
    void RemoveTrayIcon();
    void ShowTrayMenu();
    void MinimizeToTray();
    void RestoreFromTray();

    // Background thread
    void LogWatcherThread();

    // Log management (line-based buffer)
    void LoadInitialLogTail();
    void ProcessNewLogLines(const std::vector<std::wstring>& newLines);
    void RefreshLogDisplay();
    std::vector<std::string> ReadTailLines(const std::wstring& path, size_t maxLines);
    std::vector<std::string> ReadNewLines(HANDLE hFile, uint64_t& offset);

    // Utilities
    std::wstring GetBaseDirectory();
    std::vector<std::wstring> GetRunningProcesses();

    // Instance
    HINSTANCE hInstance_;
    HWND hwnd_;
    std::wstring baseDir_;

    // Controls
    HWND hwndStatusLabel_;
    HWND hwndBtnStart_;
    HWND hwndBtnStop_;
    HWND hwndTargetList_;
    HWND hwndBtnAdd_;
    HWND hwndBtnSelect_;
    HWND hwndBtnRemove_;
    HWND hwndLogEdit_;
    HWND hwndEngineStatus_;     // Engine status bar at bottom
    HWND hwndLogToggle_;        // v7.93: Log ON/OFF toggle checkbox

    // Fonts & Brushes
    HFONT hFontTitle_;
    HFONT hFontNormal_;
    HFONT hFontSmall_;
    HFONT hFontMono_;
    HBRUSH hBrushBg_;
    HBRUSH hBrushPanel_;
    HBRUSH hBrushButton_;
    HBRUSH hBrushButtonHover_;
    HBRUSH hBrushLogBg_;

    // Colors - Python v1.00 inspired dark theme
    COLORREF clrBackground_;
    COLORREF clrPanel_;
    COLORREF clrText_;
    COLORREF clrTextDim_;
    COLORREF clrAccent_;
    COLORREF clrLogText_;       // Bright green for logs
    COLORREF clrOnline_;
    COLORREF clrOffline_;
    COLORREF clrButton_;
    COLORREF clrButtonHover_;
    COLORREF clrButtonBorder_;

    // State
    ServiceController serviceCtrl_;
    IPCClient ipcClient_;
    std::atomic<bool> running_;
    std::atomic<ServiceState> currentState_;
    std::atomic<int> activeProcessCount_;

    // Log watcher
    std::thread logThread_;
    std::atomic<bool> shutdownFlag_;
    HANDLE logWakeEvent_;
    uint64_t lastLogFileSize_;
    std::string partialLine_;

    // Log line buffer (max 1000 lines)
    std::deque<std::wstring> logLineBuffer_;
    std::atomic<bool> logDisplayDirty_;
    static constexpr size_t MAX_LOG_LINES = 1000;

    // Tray
    NOTIFYICONDATAW nid_;
    bool trayCreated_;
    bool minimizedToTray_;

    // Hover tracking
    HWND hoveredButton_;

    // Control IDs
    static constexpr int ID_BTN_START = 101;
    static constexpr int ID_BTN_STOP = 102;
    static constexpr int ID_BTN_ADD = 103;
    static constexpr int ID_BTN_SELECT = 104;
    static constexpr int ID_BTN_REMOVE = 105;
    static constexpr int ID_LIST = 107;
    static constexpr int ID_LOG = 108;
    static constexpr int ID_ENGINE_STATUS = 109;
    static constexpr int ID_LOG_TOGGLE = 110;     // v7.93: Log toggle checkbox
    static constexpr int ID_TIMER = 201;

    // Custom messages
    static constexpr int WM_TRAYICON = WM_USER + 100;
    static constexpr int WM_LOG_REFRESH = WM_USER + 101;  // Batch log update
    static constexpr int ID_TRAY_OPEN = 5001;
    static constexpr int ID_TRAY_EXIT = 5002;

    // Context menu command IDs
    static constexpr int ID_CTX_TARGET_ENABLE  = 6001;
    static constexpr int ID_CTX_TARGET_DISABLE = 6002;
    static constexpr int ID_CTX_TARGET_DELETE  = 6003;
    static constexpr int ID_CTX_LOG_COPY       = 6011;
    static constexpr int ID_CTX_LOG_SELECT_ALL = 6012;
    static constexpr int ID_CTX_LOG_CLEAR      = 6013;
    static constexpr int ID_CTX_LOG_OPEN_FILE  = 6014;

    // Fixed dimensions - Compact Python v1.00 style (widened for bilingual labels)
    // static constexpr int WIN_WIDTH = 640;
    // static constexpr int WIN_HEIGHT = 520;
    static constexpr int WIN_WIDTH = 600;
    static constexpr int WIN_HEIGHT = 450;
    static constexpr int MARGIN = 16;
    static constexpr int INNER_PADDING = 10;
    static constexpr int BTN_HEIGHT = 26;
    static constexpr int BTN_WIDTH = 160;  // Service buttons
    static constexpr int SMALL_BTN_WIDTH = 60;  // Target action buttons
};

} // namespace unleaf
