#pragma once
// UnLeaf - Main Window (Python v1.00 Style Dashboard)

#include "../common/types.h"
#include "../common/scoped_handle.h"
#include "../common/config.h"
#include "../common/registry_manager.h"
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
#include <objidl.h>
#include <gdiplus.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdiplus.lib")

namespace unleaf {

// UI control role for role-based color resolution
enum class ControlRole {
    ServiceState,   // Top-level service status (bold, state-colored)
    Diagnostic,     // Engine status bar (state-colored, keeps Stopped=red)
    Observability,  // SN/Enforcement/Latency (always dim)
    Toggle,         // Log toggle switch (primary text)
    Default         // All other static labels
};

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

    // Toggle switch subclass
    static LRESULT CALLBACK ToggleSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData);
    void DrawToggleSwitch(HDC hdc, RECT rc, bool checked);

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
    void OnToggleLogEnabled();

    // Context menus
    void ShowTargetListContextMenu(int x, int y);
    void ShowLogEditContextMenu(int x, int y);
    void OnToggleTargetEnabled();
    void OnCopyLogSelection();
    void OnSelectAllLog();
    void OnClearLog();
    void OnOpenLogFile();

    // Layout
    struct ButtonWidths {
        int startW, stopW;
        int addW, selectW, removeW;
    };
    ButtonWidths MeasureButtons() const;
    void RepositionControls();
    void UpdateObservabilityStatus();
    static std::wstring SelectBestMonoFont(HWND hwnd);
    int MeasureTextWidth(HDC hdc, HFONT hFont, const wchar_t* text) const;
    void ComputeLayoutMetrics();
    void RecreateFontsForDpi();
    int DpiScale(int value) const;

    // System tray
    void CreateTrayIcon();
    void RemoveTrayIcon();
    void ShowTrayMenu();
    void MinimizeToTray();
    void RestoreFromTray();

    // Background thread
    void LogWatcherThread();

    // Log management (incremental append)
    void LoadInitialLogTail();
    void ProcessNewLogLines(const std::vector<std::wstring>& newLines);
    void RefreshLogDisplay();
    COLORREF GetLogLineColor(const std::wstring& line) const;
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
    HWND hwndEngineStatus_;          // Engine status bar at bottom
    HWND hwndObservabilityStatus_;   // Observability detail bar
    HWND hwndLogToggle_;
    HWND hwndTargetLabel_;      // "対象プロセス (Targets):" ラベル
    HWND hwndLogLabel_;         // "ライブログ (Live Log):" ラベル

    // RichEdit library handle
    HMODULE hRichEditLib_;

    // Managed font entry (DPI-aware, safe cleanup)
    struct FontEntry {
        HFONT handle = nullptr;
        bool isStock = false;
        void Reset() { if (handle && !isStock) DeleteObject(handle); handle = nullptr; isStock = false; }
    };

    // Font set — all entries are recreated together on DPI change
    struct FontSet {
        FontEntry title;    // 16pt semibold — header
        FontEntry normal;   // 12pt normal — labels, buttons
        FontEntry bold;     // 12pt bold — service status emphasis
        FontEntry sm;       // 10pt normal — status bars, toggle
        FontEntry mono;     // 9pt fixed — log edit
        void ResetAll() { title.Reset(); normal.Reset(); bold.Reset(); sm.Reset(); mono.Reset(); }
    } fonts_;

    // Role-based color resolution
    ControlRole GetControlRole(HWND hwndCtrl) const;
    COLORREF ResolveRoleColor(ControlRole role) const;
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
    COLORREF clrPending_;        // Pending transitional state (start/stop in progress).
                                 // Gold chosen to visually distinguish from Running(Green) and Stopped(Red).
    COLORREF clrNotInstalled_;   // Service not installed. Slightly darker than clrTextDim_ to indicate
                                 // absence rather than inactivity.

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

    // Scrollbar visibility
    bool scrollBarVisible_ = false;
    void UpdateScrollBarVisibility();

    // High contrast mode
    bool highContrast_ = false;

    // Log display (incremental append via pending queue)
    CriticalSection pendingLogCs_;
    std::vector<std::wstring> pendingLogLines_;
    size_t richEditLineCount_;
    bool autoScroll_;
    static constexpr size_t MAX_RICHEDIT_LINES = 5000;
    static constexpr int LOG_FONT_PT = 9;                   // ログフォントサイズ (pt)
    static constexpr int LOG_FONT_TWIPS = LOG_FONT_PT * 20;  // RichEdit用 (twips)

    // Tray
    NOTIFYICONDATAW nid_;
    bool trayCreated_;
    bool minimizedToTray_;

    // IPC rate limiting (WM_IPC_REFRESH gate)
    ULONGLONG lastIpcRefreshTime_    = 0;
    bool      ipcRefreshPending_     = false;
#ifdef INTERNAL_DIAG
    ULONGLONG lastIpcStatLog_        = 0;
    uint32_t  prevIpcOpenCount_      = 0;
    uint32_t  prevIpcTotalReqCount_  = 0;
    uint32_t  prevIpcSuccCount_      = 0;
    uint32_t  prevIpcFailCount_      = 0;
#endif

    // Hover tracking
    HWND hoveredButton_;
    bool toggleHovered_ = false;

    // Layout re-entrancy guard
    bool layoutLocked_ = false;

    // Font-metric layout values
    int fontHeight_;
    int smallFontHeight_;
    int buttonHeight_;
    int labelHeight_;
    int smallLabelHeight_;
    int itemSpacing_;
    int dpi_;

    // Control IDs
    static constexpr int ID_BTN_START = 101;
    static constexpr int ID_BTN_STOP = 102;
    static constexpr int ID_BTN_ADD = 103;
    static constexpr int ID_BTN_SELECT = 104;
    static constexpr int ID_BTN_REMOVE = 105;
    static constexpr int ID_LIST = 107;
    static constexpr int ID_LOG = 108;
    static constexpr int ID_ENGINE_STATUS = 109;
    static constexpr int ID_LOG_TOGGLE = 110;
    static constexpr int ID_OBSERVABILITY_STATUS = 111;
    static constexpr int ID_TIMER = 201;

    // Custom messages
    static constexpr int WM_TRAYICON = WM_USER + 100;
    static constexpr int WM_LOG_REFRESH = WM_USER + 101;  // Batch log update
    static constexpr int WM_APP_UPDATE_SCROLLBAR = WM_USER + 102;
    static constexpr int WM_IPC_REFRESH = WM_APP + 1;    // Rate-limited IPC status refresh
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
    static constexpr int WIN_WIDTH = 560;      // 高DPI安全余白確保
    static constexpr int WIN_HEIGHT = 446;
    static constexpr int WIN_MIN_WIDTH = 560;  // 初期サイズと統一
    static constexpr int WIN_MIN_HEIGHT = 446; // 初期サイズと統一
    static constexpr int MARGIN = 16;
    static constexpr int INNER_PADDING = 10;
};

} // namespace unleaf
