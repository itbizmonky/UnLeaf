// UnLeaf - Main Window Implementation
// Python v1.00 Style Dashboard - Clean, Professional Dark Theme

#include "main_window.h"
#include "resource.h"
#include "../common/logger.h"
#include "../common/win_string_utils.h"
#include <windowsx.h>
#include <commctrl.h>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <tlhelp32.h>
#include <uxtheme.h>
#include <richedit.h>
#include <nlohmann/json.hpp>
#include <utility>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")

namespace fs = std::filesystem;

namespace unleaf {

// Window class name
static const wchar_t* WND_CLASS = L"UnLeafDashboard";

// DWM dark mode
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

// Parent-centered MessageBox helper (CBT hook)
namespace {

constexpr int kGapLarge = 6;
constexpr int kGapSmall = 4;

constexpr const wchar_t* kStartLabel  = L"サービス登録・実行 (Register & Run)";
constexpr const wchar_t* kStopLabel   = L"サービス登録解除 (Unregister)";
constexpr const wchar_t* kAddLabel    = L"追加 (Add)";
constexpr const wchar_t* kSelectLabel = L"選択 (Select)";
constexpr const wchar_t* kRemoveLabel = L"削除 (Delete)";

thread_local HWND g_msgBoxParent = nullptr;
thread_local HHOOK g_hCBTHook = nullptr;

LRESULT CALLBACK CenterMsgBoxCBTProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HCBT_ACTIVATE) {
        HWND hwndMsgBox = reinterpret_cast<HWND>(wParam);
        HWND hwndParent = g_msgBoxParent;

        if (hwndParent && IsWindow(hwndParent) && !IsIconic(hwndParent)) {
            RECT parentRect;
            GetWindowRect(hwndParent, &parentRect);
            int parentCX = (parentRect.left + parentRect.right) / 2;
            int parentCY = (parentRect.top + parentRect.bottom) / 2;

            RECT msgRect;
            GetWindowRect(hwndMsgBox, &msgRect);
            int msgW = msgRect.right - msgRect.left;
            int msgH = msgRect.bottom - msgRect.top;

            int newX = parentCX - msgW / 2;
            int newY = parentCY - msgH / 2;

            HMONITOR hMon = MonitorFromWindow(hwndParent, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi = { sizeof(mi) };
            if (GetMonitorInfoW(hMon, &mi)) {
                const RECT& wa = mi.rcWork;
                if (newX < wa.left) newX = wa.left;
                if (newY < wa.top) newY = wa.top;
                if (newX + msgW > wa.right) newX = wa.right - msgW;
                if (newY + msgH > wa.bottom) newY = wa.bottom - msgH;
            }

            SetWindowPos(hwndMsgBox, nullptr, newX, newY, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }

        if (g_hCBTHook) {
            UnhookWindowsHookEx(g_hCBTHook);
            g_hCBTHook = nullptr;
        }
        return 0;
    }
    return CallNextHookEx(g_hCBTHook, nCode, wParam, lParam);
}

int CenteredMessageBoxW(HWND hWndParent, LPCWSTR lpText, LPCWSTR lpCaption, UINT uType) {
    g_msgBoxParent = hWndParent;
    g_hCBTHook = SetWindowsHookExW(WH_CBT, CenterMsgBoxCBTProc,
                                    nullptr, GetCurrentThreadId());
    int result = MessageBoxW(hWndParent, lpText, lpCaption, uType);
    if (g_hCBTHook) {
        UnhookWindowsHookEx(g_hCBTHook);
        g_hCBTHook = nullptr;
    }
    g_msgBoxParent = nullptr;
    return result;
}

} // anonymous namespace

MainWindow::MainWindow()
    : hInstance_(nullptr)
    , hwnd_(nullptr)
    , hwndStatusLabel_(nullptr)
    , hwndBtnStart_(nullptr)
    , hwndBtnStop_(nullptr)
    , hwndTargetList_(nullptr)
    , hwndBtnAdd_(nullptr)
    , hwndBtnSelect_(nullptr)
    , hwndBtnRemove_(nullptr)
    , hwndLogEdit_(nullptr)
    , hwndEngineStatus_(nullptr)
    , hwndObservabilityStatus_(nullptr)
    , hwndLogToggle_(nullptr)
    , hRichEditLib_(nullptr)
    , fonts_{}
    , hBrushBg_(nullptr)
    , hBrushPanel_(nullptr)
    , hBrushButton_(nullptr)
    , hBrushButtonHover_(nullptr)
    , hBrushLogBg_(nullptr)
    , running_(true)
    , currentState_(ServiceState::Unknown)
    , activeProcessCount_(0)
    , shutdownFlag_(false)
    , logWakeEvent_(nullptr)
    , lastLogFileSize_(0)
    , richEditLineCount_(0)
    , autoScroll_(true)
    , trayCreated_(false)
    , minimizedToTray_(false)
    , hoveredButton_(nullptr)
    , fontHeight_(0)
    , smallFontHeight_(0)
    , buttonHeight_(0)
    , labelHeight_(0)
    , smallLabelHeight_(0)
    , itemSpacing_(0)
    , dpi_(96) {

    // Dark-fixed color palette (OS theme independent)
    clrBackground_ = RGB(32, 32, 36);      // Deep dark background
    clrPanel_ = RGB(45, 45, 52);           // Surface / card background
    clrText_ = RGB(230, 230, 235);         // Primary text
    clrTextDim_ = RGB(150, 150, 160);      // Secondary/dim text
    clrAccent_ = RGB(220, 220, 225);       // UI decoration accent
    clrLogText_ = RGB(0, 255, 128);        // Bright terminal green for logs
    clrOnline_ = RGB(46, 204, 113);        // Online indicator
    clrOffline_ = RGB(231, 76, 60);        // Offline indicator
    clrPending_       = RGB(255, 215, 0);  // Gold: visually distinct from green/red states
    clrNotInstalled_  = RGB(140, 140, 150); // Slightly darker than clrTextDim_ (absence vs inactivity)
    clrButton_ = RGB(50, 50, 58);          // Button background
    clrButtonHover_ = RGB(65, 65, 75);     // Button hover
    clrButtonBorder_ = RGB(70, 70, 80);    // Button border

    ZeroMemory(&nid_, sizeof(nid_));
}

MainWindow::~MainWindow() {
    // Ensure shutdown flags are set
    running_.store(false, std::memory_order_release);
    shutdownFlag_.store(true, std::memory_order_release);

    // Signal thread to wake up and exit
    if (logWakeEvent_) {
        SetEvent(logWakeEvent_);
    }

    // Thread may have been detached in WM_CLOSE, only join if still joinable
    if (logThread_.joinable()) {
        logThread_.join();
    }

    if (logWakeEvent_) {
        CloseHandle(logWakeEvent_);
        logWakeEvent_ = nullptr;
    }

    // Cleanup GDI resources
    fonts_.ResetAll();
    if (hBrushBg_) DeleteObject(hBrushBg_);
    if (hBrushPanel_) DeleteObject(hBrushPanel_);
    if (hBrushButton_) DeleteObject(hBrushButton_);
    if (hBrushButtonHover_) DeleteObject(hBrushButtonHover_);
    if (hBrushLogBg_) DeleteObject(hBrushLogBg_);
    if (hRichEditLib_) FreeLibrary(hRichEditLib_);
}

bool MainWindow::Initialize(HINSTANCE hInstance) {
    hInstance_ = hInstance;
    baseDir_ = GetBaseDirectory();

    // Init common controls
    INITCOMMONCONTROLSEX icex = { sizeof(icex), ICC_WIN95_CLASSES | ICC_BAR_CLASSES };
    InitCommonControlsEx(&icex);

    // Load RichEdit library for enhanced log display
    hRichEditLib_ = LoadLibraryW(L"msftedit.dll");  // RICHEDIT50W

    // Init config
    UnLeafConfig::Instance().Initialize(baseDir_);

    // Temporary pre-window fonts (replaced by RecreateFontsForDpi after HWND creation)
    fonts_.normal = { (HFONT)GetStockObject(DEFAULT_GUI_FONT), true };

    // Create brushes
    hBrushBg_          = CreateSolidBrush(clrBackground_);
    hBrushPanel_       = CreateSolidBrush(clrPanel_);
    hBrushButton_      = CreateSolidBrush(clrButton_);
    hBrushButtonHover_ = CreateSolidBrush(clrButtonHover_);
    hBrushLogBg_       = CreateSolidBrush(clrBackground_);
    if (!hBrushBg_ || !hBrushPanel_ || !hBrushButton_ ||
        !hBrushButtonHover_ || !hBrushLogBg_) {
        LOG_ALERT(L"[UI] CreateSolidBrush failed - display may be degraded");
    }

    // Register window class
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    hInstance_ = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;  // OS auto-erase disabled; app handles WM_ERASEBKGND
    wc.lpszClassName = WND_CLASS;
    wc.hIcon   = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));

    if (!RegisterClassExW(&wc)) {
        return false;
    }

    // Center on screen
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int posX = (screenW - WIN_WIDTH) / 2;
    int posY = (screenH - WIN_HEIGHT) / 2;

    // Create window (resizable)
    hwnd_ = CreateWindowExW(
        0, WND_CLASS, L"UnLeaf Manager",
        WS_OVERLAPPEDWINDOW,
        posX, posY, WIN_WIDTH, WIN_HEIGHT,
        nullptr, nullptr, hInstance, this
    );

    if (!hwnd_) return false;

    // Create DPI-aware font set (includes mono font detection)
    RecreateFontsForDpi();
    ComputeLayoutMetrics();
    EnableDarkMode();
    CreateControls();
    CreateTrayIcon();

    // Initial state
    UpdateServiceStatus();
    RefreshTargetList();
    AppendLog(L"UnLeaf dashboard started");
    AppendLog(L"Log monitoring path: " + baseDir_ + L"\\" + LOG_FILENAME);

    // Create wake event for log watcher thread
    logWakeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // Start log watcher
    shutdownFlag_.store(false, std::memory_order_release);
    logThread_ = std::thread(&MainWindow::LogWatcherThread, this);

    // Status update timer (1 second)
    SetTimer(hwnd_, ID_TIMER, 1000, nullptr);

    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    SetFocus(hwndBtnAdd_);

    return true;
}

void MainWindow::EnableDarkMode() {
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
}

int MainWindow::Run() {
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
        if (IsDialogMessage(hwnd_, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK MainWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    MainWindow* self = nullptr;

    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<MainWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self) {
        return self->HandleMessage(msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT MainWindow::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND:
        return TRUE;  // OS erase disabled; all painting in WM_PAINT

    case WM_PAINT:
        OnPaint();
        return 0;

    case WM_DRAWITEM: {
        auto lpDIS = reinterpret_cast<LPDRAWITEMSTRUCT>(lParam);
        if (lpDIS->CtlType == ODT_BUTTON) {
            DrawButton(lpDIS);
            return TRUE;
        }
        break;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_BTN_START:
            OnStartService();
            break;
        case ID_BTN_STOP:
            OnStopService();
            break;
        case ID_BTN_ADD:
            OnAddTarget();
            break;
        case ID_BTN_SELECT:
            OnSelectProcess();
            break;
        case ID_BTN_REMOVE:
            OnRemoveTarget();
            break;
        case ID_LOG_TOGGLE:
            OnToggleLogEnabled();
            break;
        case ID_TRAY_OPEN:
            RestoreFromTray();
            break;
        case ID_TRAY_EXIT:
            running_ = false;
            DestroyWindow(hwnd_);
            break;
        // Context menu commands - Target list
        case ID_CTX_TARGET_ENABLE:
        case ID_CTX_TARGET_DISABLE:
            OnToggleTargetEnabled();
            break;
        case ID_CTX_TARGET_DELETE:
            OnRemoveTarget();
            break;
        // Context menu commands - Log edit
        case ID_CTX_LOG_COPY:
            OnCopyLogSelection();
            break;
        case ID_CTX_LOG_SELECT_ALL:
            OnSelectAllLog();
            break;
        case ID_CTX_LOG_CLEAR:
            OnClearLog();
            break;
        case ID_CTX_LOG_OPEN_FILE:
            OnOpenLogFile();
            break;
        }
        break;

    case WM_TIMER:
        if (wParam == ID_TIMER) {
            UpdateServiceStatus();

            ULONGLONG nowMs = GetTickCount64();
            if (!ipcRefreshPending_ && nowMs - lastIpcRefreshTime_ >= 3000ULL) {
                if (PostMessage(hwnd_, WM_IPC_REFRESH, 0, 0))
                    ipcRefreshPending_ = true;
            }

            RefreshLogDisplay();  // Catch-all for pending log lines

            if (UnLeafConfig::Instance().HasFileChanged()) {
                UnLeafConfig::Instance().Reload();
                RefreshTargetList();
            }
        }
        break;

    case WM_IPC_REFRESH: {
        lastIpcRefreshTime_ = GetTickCount64();
        ipcRefreshPending_  = false;

        UpdateEngineStatus();
        UpdateObservabilityStatus();

#ifdef INTERNAL_DIAG
        {
            ULONGLONG now2 = GetTickCount64();
            if (now2 - lastIpcStatLog_ >= 60000ULL) {
                uint32_t curOpen = ipcClient_.GetOpenCount();
                uint32_t curReq  = ipcClient_.GetTotalRequests();
                uint32_t curSucc = ipcClient_.GetSuccessfulRequests();
                uint32_t curFail = ipcClient_.GetFailedRequests();
                prevIpcOpenCount_     = curOpen;
                prevIpcTotalReqCount_ = curReq;
                prevIpcSuccCount_     = curSucc;
                prevIpcFailCount_     = curFail;
                lastIpcStatLog_       = now2;
            }
        }
#endif
        return 0;
    }

case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wParam;
        HWND hwndCtrl = (HWND)lParam;
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, ResolveRoleColor(GetControlRole(hwndCtrl)));
        return (LRESULT)hBrushBg_;
    }

    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, clrText_);
        SetBkColor(hdc, clrBackground_);
        return (LRESULT)hBrushLogBg_;
    }

    case WM_CTLCOLORLISTBOX: {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, clrText_);
        SetBkColor(hdc, clrPanel_);
        return (LRESULT)hBrushPanel_;
    }

    case WM_TRAYICON:
        if (wParam == 1) {
            if (LOWORD(lParam) == WM_LBUTTONDBLCLK) {
                RestoreFromTray();
            } else if (LOWORD(lParam) == WM_RBUTTONUP) {
                ShowTrayMenu();
            }
        }
        break;

    case WM_CONTEXTMENU: {
        HWND hwndClicked = reinterpret_cast<HWND>(wParam);
        int x = GET_X_LPARAM(lParam);
        int y = GET_Y_LPARAM(lParam);

        // Keyboard invocation (Shift+F10) - adjust coordinates
        if (x == -1 && y == -1) {
            RECT rc;
            GetWindowRect(hwndClicked, &rc);
            x = rc.left;
            y = rc.top;
        }

        if (hwndClicked == hwndTargetList_) {
            ShowTargetListContextMenu(x, y);
            return 0;
        }
        if (hwndClicked == hwndLogEdit_) {
            ShowLogEditContextMenu(x, y);
            return 0;
        }
        break;
    }

    case WM_CLOSE:
        // Signal thread to stop before destroying window
        running_.store(false, std::memory_order_release);
        shutdownFlag_.store(true, std::memory_order_release);
        if (logWakeEvent_) {
            SetEvent(logWakeEvent_);
        }
        if (logThread_.joinable()) {
            logThread_.join();  // shutdownFlag_ + logWakeEvent_ ensures prompt exit
        }
        DestroyWindow(hwnd_);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd_, ID_TIMER);
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;

    case WM_LOG_REFRESH:
        // Batch refresh of log display
        RefreshLogDisplay();
        return 0;

    case WM_APP_UPDATE_SCROLLBAR:
        UpdateScrollBarVisibility();
        return 0;

    case WM_SETTINGCHANGE: {
        HIGHCONTRAST hc = { sizeof(hc) };
        if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0)) {
            highContrast_ = (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
        }
        // Rebuild fonts when system font settings change
        if (wParam == SPI_SETNONCLIENTMETRICS || wParam == SPI_SETICONTITLELOGFONT) {
            RecreateFontsForDpi();
            ComputeLayoutMetrics();
            RepositionControls();
        }
        if (hwndLogToggle_) {
            InvalidateRect(hwndLogToggle_, nullptr, TRUE);
        }
        break;
    }

    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            RepositionControls();
            InvalidateRect(hwnd_, nullptr, TRUE);
        }
        if (wParam == SIZE_MINIMIZED) {
            MinimizeToTray();
        }
        return 0;

    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        mmi->ptMinTrackSize.x = DpiScale(WIN_MIN_WIDTH);
        mmi->ptMinTrackSize.y = DpiScale(WIN_MIN_HEIGHT);
        return 0;
    }

    case WM_DPICHANGED: {
        // RAII guard: restores WM_SETREDRAW for children then parent
        struct RedrawGuard {
            HWND parent{};
            HWND child1{};  // logEdit
            HWND child2{};  // targetList

            ~RedrawGuard() noexcept {
                if (child1 && IsWindow(child1))
                    SendMessageW(child1, WM_SETREDRAW, TRUE, 0);
                if (child2 && IsWindow(child2))
                    SendMessageW(child2, WM_SETREDRAW, TRUE, 0);

                if (parent && IsWindow(parent)) {
                    SendMessageW(parent, WM_SETREDRAW, TRUE, 0);
                    RedrawWindow(parent, nullptr, nullptr,
                        RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
                }
            }
        };

        RECT* suggested = reinterpret_cast<RECT*>(lParam);
        SetWindowPos(hwnd_, nullptr,
            suggested->left, suggested->top,
            suggested->right - suggested->left,
            suggested->bottom - suggested->top,
            SWP_NOZORDER | SWP_NOACTIVATE);

        layoutLocked_ = true;

        // Stop drawing during DPI transition
        SendMessageW(hwnd_, WM_SETREDRAW, FALSE, 0);
        SendMessageW(hwndLogEdit_, WM_SETREDRAW, FALSE, 0);
        SendMessageW(hwndTargetList_, WM_SETREDRAW, FALSE, 0);
        RedrawGuard guard{ hwnd_, hwndLogEdit_, hwndTargetList_ };

        RecreateFontsForDpi();
        ComputeLayoutMetrics();
        SendMessageW(hwndTargetList_, LB_SETITEMHEIGHT, 0, fontHeight_ + DpiScale(2));
        RepositionControls();

        layoutLocked_ = false;
        return 0;  // guard destructor restores child1 → child2 → parent + RedrawWindow
    }
    }

    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

// Use Win32 MulDiv for overflow-safe DPI scaling
int MainWindow::DpiScale(int value) const {
    return MulDiv(value, dpi_, 96);
}

// Role-based color system for WM_CTLCOLORSTATIC
ControlRole MainWindow::GetControlRole(HWND hwndCtrl) const {
    if (hwndCtrl == hwndStatusLabel_)        return ControlRole::ServiceState;
    if (hwndCtrl == hwndEngineStatus_)       return ControlRole::Diagnostic;
    if (hwndCtrl == hwndObservabilityStatus_) return ControlRole::Observability;
    if (hwndCtrl == hwndLogToggle_)          return ControlRole::Toggle;
    return ControlRole::Default;
}

COLORREF MainWindow::ResolveRoleColor(ControlRole role) const {
    ServiceState state = currentState_.load(std::memory_order_relaxed);

    switch (role) {
    case ControlRole::ServiceState:
        // Top-level service status: Stopped=grey (low-alarm), Unknown=dim (not red to avoid false alarm)
        switch (state) {
            case ServiceState::Running:      return clrOnline_;
            case ServiceState::Stopped:      return clrTextDim_;
            case ServiceState::StartPending:
            case ServiceState::StopPending:  return clrPending_;
            case ServiceState::NotInstalled: return clrTextDim_;
            default:                         return clrTextDim_;
        }

    case ControlRole::Diagnostic:
        // Engine status bar: keeps original color mapping (Stopped=red for anomaly detection)
        switch (state) {
            case ServiceState::Running:      return clrOnline_;
            case ServiceState::Stopped:      return clrOffline_;
            case ServiceState::StartPending:
            case ServiceState::StopPending:  return clrPending_;
            case ServiceState::NotInstalled: return clrNotInstalled_;
            default:                         return clrText_;
        }

    case ControlRole::Observability:
        return clrTextDim_;

    case ControlRole::Toggle:
        return clrText_;

    default:
        return clrText_;
    }
}

std::wstring MainWindow::SelectBestMonoFont(HWND hwnd) {
    const wchar_t* candidates[] = { L"Cascadia Mono", L"Cascadia Code" };
    HDC hdc = GetDC(hwnd);
    for (const auto* name : candidates) {
        LOGFONTW lf = {};
        wcscpy_s(lf.lfFaceName, name);
        lf.lfCharSet = DEFAULT_CHARSET;
        bool found = false;
        EnumFontFamiliesExW(hdc, &lf,
            [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM p) -> int {
                *reinterpret_cast<bool*>(p) = true;
                return 0;
            }, reinterpret_cast<LPARAM>(&found), 0);
        if (found) { ReleaseDC(hwnd, hdc); return name; }
    }
    ReleaseDC(hwnd, hdc);
    return L"Consolas";
}

int MainWindow::MeasureTextWidth(HDC hdc, HFONT hFont, const wchar_t* text) const {
    HFONT oldFont = (HFONT)SelectObject(hdc, hFont);
    SIZE sz;
    GetTextExtentPoint32W(hdc, text, (int)wcslen(text), &sz);
    SelectObject(hdc, oldFont);
    return sz.cx;
}

void MainWindow::RecreateFontsForDpi() {
    dpi_ = GetDpiForWindow(hwnd_);

    // 1. Release all existing fonts
    fonts_.ResetAll();

    // 2. Recreate full font set with DPI-scaled sizes
    auto safeCreate = [](int height, int weight, DWORD pitch, const wchar_t* face) -> FontEntry {
        HFONT hf = CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, pitch, face);
        if (!hf) return { (HFONT)GetStockObject(DEFAULT_GUI_FONT), true };
        return { hf, false };
    };

    fonts_.title  = safeCreate(-DpiScale(16), FW_SEMIBOLD, DEFAULT_PITCH, L"Yu Gothic UI");
    fonts_.normal = safeCreate(-DpiScale(12), FW_NORMAL,   DEFAULT_PITCH, L"Yu Gothic UI");
    fonts_.bold   = safeCreate(-DpiScale(12), FW_BOLD,     DEFAULT_PITCH, L"Yu Gothic UI");
    fonts_.sm = safeCreate(-DpiScale(10), FW_NORMAL,   DEFAULT_PITCH, L"Yu Gothic UI");

    std::wstring monoFace = SelectBestMonoFont(hwnd_);
    fonts_.mono = safeCreate(-DpiScale(LOG_FONT_PT), FW_NORMAL, FIXED_PITCH, monoFace.c_str());

    // 3. Apply default font to direct child controls
    EnumChildWindows(hwnd_, [](HWND hwnd, LPARAM lParam) -> BOOL {
        auto self = reinterpret_cast<MainWindow*>(lParam);
        if (GetParent(hwnd) != self->hwnd_) return TRUE;
        SendMessageW(hwnd, WM_SETFONT, (WPARAM)self->fonts_.normal.handle, TRUE);
        return TRUE;
    }, (LPARAM)this);

    // Override specific controls with their designated fonts (null-guarded for initial call)
    if (hwndStatusLabel_)
        SendMessageW(hwndStatusLabel_, WM_SETFONT, (WPARAM)fonts_.bold.handle, TRUE);
    if (hwndEngineStatus_)
        SendMessageW(hwndEngineStatus_, WM_SETFONT, (WPARAM)fonts_.sm.handle, TRUE);
    if (hwndObservabilityStatus_)
        SendMessageW(hwndObservabilityStatus_, WM_SETFONT, (WPARAM)fonts_.sm.handle, TRUE);
    if (hwndLogToggle_)
        SendMessageW(hwndLogToggle_, WM_SETFONT, (WPARAM)fonts_.sm.handle, TRUE);
    if (hwndLogEdit_)
        SendMessageW(hwndLogEdit_, WM_SETFONT, (WPARAM)fonts_.mono.handle, TRUE);

    // 4. Update RichEdit CHARFORMAT (LOG_FONT_PT — RichEdit has its own DPI scaling)
    if (hRichEditLib_ && hwndLogEdit_) {
        CHARFORMAT2W cf = {};
        cf.cbSize = sizeof(cf);
        cf.dwMask = CFM_FACE | CFM_SIZE;
        wcscpy_s(cf.szFaceName, monoFace.c_str());
        cf.yHeight = LOG_FONT_TWIPS;
        SendMessageW(hwndLogEdit_, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
    }
}

void MainWindow::ComputeLayoutMetrics() {
    dpi_ = GetDpiForWindow(hwnd_);

    HDC hdc = GetDC(hwnd_);
    if (!hdc) {
        fontHeight_ = DpiScale(12);
        smallFontHeight_ = DpiScale(10);
        buttonHeight_ = DpiScale(24);
        labelHeight_ = DpiScale(18);
        smallLabelHeight_ = DpiScale(14);
        itemSpacing_ = DpiScale(4);
        return;
    }
    HFONT oldFont = (HFONT)SelectObject(hdc, fonts_.normal.handle);
    TEXTMETRICW tm;
    GetTextMetricsW(hdc, &tm);
    fontHeight_ = tm.tmHeight;
    int normalAscent = tm.tmAscent, normalDescent = tm.tmDescent;

    SelectObject(hdc, fonts_.sm.handle);
    GetTextMetricsW(hdc, &tm);
    smallFontHeight_ = tm.tmHeight;

    SelectObject(hdc, oldFont);
    ReleaseDC(hwnd_, hdc);  // GetDC is released on all return paths

    buttonHeight_ = normalAscent + normalDescent + DpiScale(8);
    labelHeight_ = fontHeight_ + DpiScale(6);
    smallLabelHeight_ = smallFontHeight_ + DpiScale(4);
    itemSpacing_ = (std::max)(DpiScale(4), fontHeight_ / 3);
}

// Button widths are "fixed" w.r.t. window size — they do NOT change on resize.
// They ARE recalculated on DPI change / font recreation (RecreateFontsForDpi),
// adapting to system DPI, font metrics, and theme changes.
MainWindow::ButtonWidths MainWindow::MeasureButtons() const {
    ButtonWidths bw{};
    bw.startW  = DpiScale(140);
    bw.stopW   = DpiScale(140);
    bw.addW    = DpiScale(48);
    bw.selectW = DpiScale(48);
    bw.removeW = DpiScale(48);

    if (!hwnd_ || !fonts_.sm.handle)
        return bw;

    HDC hdc = GetDC(hwnd_);
    if (hdc) {
        bw.startW  = (std::max)(DpiScale(140), MeasureTextWidth(hdc, fonts_.sm.handle, kStartLabel) + DpiScale(14) * 2);
        bw.stopW   = (std::max)(DpiScale(140), MeasureTextWidth(hdc, fonts_.sm.handle, kStopLabel) + DpiScale(14) * 2);
        bw.addW    = (std::max)(DpiScale(48), MeasureTextWidth(hdc, fonts_.sm.handle, kAddLabel) + DpiScale(12) * 2);
        bw.selectW = (std::max)(DpiScale(48), MeasureTextWidth(hdc, fonts_.sm.handle, kSelectLabel) + DpiScale(12) * 2);
        bw.removeW = (std::max)(DpiScale(48), MeasureTextWidth(hdc, fonts_.sm.handle, kRemoveLabel) + DpiScale(12) * 2);
        ReleaseDC(hwnd_, hdc);
    }
    return bw;
}

void MainWindow::CreateControls() {
    RECT clientRect;
    GetClientRect(hwnd_, &clientRect);
    int clientWidth = clientRect.right - clientRect.left;
    int clientHeight = clientRect.bottom - clientRect.top;

    int contentWidth = clientWidth - MARGIN * 2;
    int y = MARGIN;

    // ── Unified button metrics (sync with RepositionControls) ──
    // All 5 buttons share the same width and height for visual consistency.
    // Width: max of all measured widths with DpiScale(140) minimum.
    // Height: tgtBtnH formula (list-height divided by 3).
    auto bw = MeasureButtons();
    int unifiedBtnW = (std::max)({DpiScale(140), bw.startW, bw.stopW, bw.addW, bw.selectW, bw.removeW});

    // Compute unified button height from target button formula
    int listHeightBase = fontHeight_ * 6 + DpiScale(6);
    int tgtBtnGap = DpiScale(kGapLarge);
    int inset = DpiScale(1);
    int unifiedBtnH = static_cast<int>((listHeightBase - 2 * inset - tgtBtnGap * 2) / 3.0f + 0.5f);

    // === Header area (title drawn in OnPaint) ===
    y += labelHeight_ + MARGIN / 2;

    // === Service Status + Buttons (same row) ===
    int stopX = MARGIN + contentWidth - unifiedBtnW;
    int startX = stopX - DpiScale(kGapLarge) - unifiedBtnW;
    int statusLabelW = (std::max)(startX - MARGIN - DpiScale(kGapLarge), DpiScale(80));

    hwndStatusLabel_ = CreateWindowW(L"STATIC", L"● Service: Checking...",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
        MARGIN, y, statusLabelW, buttonHeight_, hwnd_, nullptr, hInstance_, nullptr);
    SendMessageW(hwndStatusLabel_, WM_SETFONT, (WPARAM)fonts_.bold.handle, TRUE);

    hwndBtnStart_ = CreateWindowW(L"BUTTON", kStartLabel,
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        startX, y, unifiedBtnW, unifiedBtnH,
        hwnd_, (HMENU)(UINT_PTR)ID_BTN_START, hInstance_, nullptr);

    hwndBtnStop_ = CreateWindowW(L"BUTTON", kStopLabel,
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        stopX, y, unifiedBtnW, unifiedBtnH,
        hwnd_, (HMENU)(UINT_PTR)ID_BTN_STOP, hInstance_, nullptr);

    y += unifiedBtnH + itemSpacing_;

    // === Target Processes Section ===
    // Target label (own row)
    hwndTargetLabel_ = CreateWindowW(L"STATIC", L"対象プロセス (Targets):",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
        MARGIN, y, contentWidth, labelHeight_, hwnd_, nullptr, hInstance_, nullptr);

    y += labelHeight_ + itemSpacing_ / 2;

    // ── Layout alignment block (sync with RepositionControls) ──
    // Alignment: target buttons left edge = Stop button left edge (btnX = stopX)
    // Alignment: list right edge = Start button right edge (listW = btnX - gap - MARGIN)
    int btnX = stopX;
    btnX = (std::max)(btnX, MARGIN);  // safety clamp against extreme font/DPI cases
    int listW = btnX - DpiScale(kGapLarge) - MARGIN;
    listW = (std::max)(listW, DpiScale(120));  // minimum list width guard

    int listHeight = listHeightBase;
    int tgtBtnAreaH = unifiedBtnH * 3 + tgtBtnGap * 2 + 2 * inset;
    listHeight = (std::max)(listHeight, tgtBtnAreaH);
    int btnStartY = y + inset + (listHeight - tgtBtnAreaH) / 2;

    hwndTargetList_ = CreateWindowExW(0, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_BORDER | LBS_NOTIFY,
        MARGIN, y, listW, listHeight,
        hwnd_, (HMENU)(UINT_PTR)ID_LIST, hInstance_, nullptr);
    SendMessageW(hwndTargetList_, WM_SETFONT, (WPARAM)fonts_.normal.handle, TRUE);
    SendMessageW(hwndTargetList_, LB_SETITEMHEIGHT, 0, fontHeight_ + DpiScale(2));

    hwndBtnAdd_ = CreateWindowW(L"BUTTON", kAddLabel,
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
        btnX, btnStartY, unifiedBtnW, unifiedBtnH,
        hwnd_, (HMENU)(UINT_PTR)ID_BTN_ADD, hInstance_, nullptr);

    hwndBtnSelect_ = CreateWindowW(L"BUTTON", kSelectLabel,
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
        btnX, btnStartY + unifiedBtnH + tgtBtnGap, unifiedBtnW, unifiedBtnH,
        hwnd_, (HMENU)(UINT_PTR)ID_BTN_SELECT, hInstance_, nullptr);

    hwndBtnRemove_ = CreateWindowW(L"BUTTON", kRemoveLabel,
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
        btnX, btnStartY + (unifiedBtnH + tgtBtnGap) * 2 - 1, unifiedBtnW, unifiedBtnH,
        hwnd_, (HMENU)(UINT_PTR)ID_BTN_REMOVE, hInstance_, nullptr);

    y += listHeight + itemSpacing_;

    // === Live Log Section ===
    hwndLogLabel_ = CreateWindowW(L"STATIC", L"ライブログ (Live Log):",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
        MARGIN, y, contentWidth / 2, labelHeight_, hwnd_, nullptr, hInstance_, nullptr);

    hwndLogToggle_ = CreateWindowW(L"BUTTON", L"Log Output",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | BS_LEFTTEXT,
        MARGIN + contentWidth - DpiScale(110), y, DpiScale(110), labelHeight_,
        hwnd_, (HMENU)(UINT_PTR)ID_LOG_TOGGLE, hInstance_, nullptr);
    SendMessageW(hwndLogToggle_, WM_SETFONT, (WPARAM)fonts_.sm.handle, TRUE);
    SetWindowTheme(hwndLogToggle_, L"", L"");
    SetWindowSubclass(hwndLogToggle_, ToggleSubclassProc, 0, reinterpret_cast<DWORD_PTR>(this));
    SendMessageW(hwndLogToggle_, BM_SETCHECK,
                 UnLeafConfig::Instance().IsLogEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);

    // Initialize high contrast state
    HIGHCONTRAST hc = { sizeof(hc) };
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0)) {
        highContrast_ = (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
    }

    y += labelHeight_ + itemSpacing_ / 2;

    // Log edit area - calculate remaining height minus status bars
    int logHeight = clientHeight - y - 4 - smallLabelHeight_ - smallLabelHeight_;
    if (logHeight < 60) logHeight = 60;

    // Use RichEdit if available, fallback to EDIT
    const wchar_t* logClass = hRichEditLib_ ? MSFTEDIT_CLASS : L"EDIT";
    hwndLogEdit_ = CreateWindowExW(0, logClass, L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_BORDER | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
        MARGIN, y, contentWidth, logHeight,
        hwnd_, (HMENU)(UINT_PTR)ID_LOG, hInstance_, nullptr);
    SendMessageW(hwndLogEdit_, WM_SETFONT, (WPARAM)fonts_.mono.handle, TRUE);

    // Configure RichEdit if loaded
    if (hRichEditLib_ && hwndLogEdit_) {
        SendMessageW(hwndLogEdit_, EM_SETUNDOLIMIT, 0, 0);
        SendMessageW(hwndLogEdit_, EM_SETBKGNDCOLOR, 0, clrBackground_);

        // Use already-detected mono font name
        std::wstring logFontName = SelectBestMonoFont(hwnd_);

        CHARFORMAT2W cf = {};
        cf.cbSize = sizeof(cf);
        cf.dwMask = CFM_COLOR | CFM_FACE | CFM_SIZE;
        cf.crTextColor = clrText_;
        wcscpy_s(cf.szFaceName, logFontName.c_str());
        cf.yHeight = LOG_FONT_TWIPS;
        SendMessageW(hwndLogEdit_, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);

        // Uniform line spacing
        PARAFORMAT2 pf = {};
        pf.cbSize = sizeof(pf);
        pf.dwMask = PFM_LINESPACING | PFM_SPACEBEFORE | PFM_SPACEAFTER;
        pf.bLineSpacingRule = 0;  // Single spacing
        pf.dySpaceBefore = 0;
        pf.dySpaceAfter = 0;
        SendMessageW(hwndLogEdit_, EM_SETPARAFORMAT, 0, (LPARAM)&pf);
        // Register as default paragraph format for new paragraphs
        SendMessageW(hwndLogEdit_, EM_SETPARAFORMAT, SPF_SETDEFAULT, (LPARAM)&pf);

        // Initial scrollbar hidden (content-based visibility)
        ShowScrollBar(hwndLogEdit_, SB_VERT, FALSE);
    }

    y += logHeight + DpiScale(kGapSmall);

    // === Engine Status Bar (bottom) ===
    hwndEngineStatus_ = CreateWindowW(L"STATIC", L"Processes: 0  |  Engine: --",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
        MARGIN, y, contentWidth, smallLabelHeight_,
        hwnd_, (HMENU)(UINT_PTR)ID_ENGINE_STATUS, hInstance_, nullptr);
    SendMessageW(hwndEngineStatus_, WM_SETFONT, (WPARAM)fonts_.sm.handle, TRUE);

    y += smallLabelHeight_;

    // === Observability Status Bar ===
    hwndObservabilityStatus_ = CreateWindowW(L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
        MARGIN, y, contentWidth, smallLabelHeight_,
        hwnd_, (HMENU)(UINT_PTR)ID_OBSERVABILITY_STATUS, hInstance_, nullptr);
    SendMessageW(hwndObservabilityStatus_, WM_SETFONT, (WPARAM)fonts_.sm.handle, TRUE);

    // Apply default font to direct child controls only
    EnumChildWindows(hwnd_, [](HWND hwnd, LPARAM lParam) -> BOOL {
        auto self = reinterpret_cast<MainWindow*>(lParam);
        if (GetParent(hwnd) != self->hwnd_) return TRUE;
        SendMessageW(hwnd, WM_SETFONT, (WPARAM)self->fonts_.normal.handle, TRUE);
        return TRUE;
    }, (LPARAM)this);

    // Override specific controls with their designated fonts
    SendMessageW(hwndEngineStatus_, WM_SETFONT, (WPARAM)fonts_.sm.handle, TRUE);
    SendMessageW(hwndObservabilityStatus_, WM_SETFONT, (WPARAM)fonts_.sm.handle, TRUE);
    SendMessageW(hwndLogToggle_, WM_SETFONT, (WPARAM)fonts_.sm.handle, TRUE);
    SendMessageW(hwndLogEdit_, WM_SETFONT, (WPARAM)fonts_.mono.handle, TRUE);
}

void MainWindow::RepositionControls() {
    if (layoutLocked_) return;

    RECT clientRect;
    GetClientRect(hwnd_, &clientRect);
    int clientWidth = clientRect.right - clientRect.left;
    int clientHeight = clientRect.bottom - clientRect.top;
    int contentWidth = clientWidth - MARGIN * 2;

    // ── Unified button metrics (sync with CreateControls) ──
    // All 5 buttons share the same width and height for visual consistency.
    auto bw = MeasureButtons();
    int unifiedBtnW = (std::max)({DpiScale(140), bw.startW, bw.stopW, bw.addW, bw.selectW, bw.removeW});

    // Compute unified button height from target button formula
    int listHeightBase = fontHeight_ * 6 + DpiScale(6);
    int tgtBtnGap = DpiScale(kGapLarge);
    int inset = DpiScale(1);
    int unifiedBtnH = static_cast<int>((listHeightBase - 2 * inset - tgtBtnGap * 2) / 3.0f + 0.5f);

    // UI overflow detection for future DPI/font changes (one-shot logging)
    int svcBtnTotal = unifiedBtnW * 2 + DpiScale(kGapLarge);
    static bool overflowLogged = false;
    if (svcBtnTotal > contentWidth && !overflowLogged) {
        OutputDebugStringW(L"[UnLeaf] UI overflow detected at current DPI/font\n");
        AppendLog(L"[WARN] UI overflow detected at current DPI/font");
        overflowLogged = true;
    }

    // Header area
    int y = MARGIN + labelHeight_ + MARGIN / 2;

    // Service status + buttons (same row)
    int stopX = MARGIN + contentWidth - unifiedBtnW;
    int startX = stopX - DpiScale(kGapLarge) - unifiedBtnW;
    int statusLabelW = (std::max)(startX - MARGIN - DpiScale(kGapLarge), DpiScale(80));

    if (hwndStatusLabel_)
        MoveWindow(hwndStatusLabel_, MARGIN, y, statusLabelW, buttonHeight_, TRUE);
    if (hwndBtnStart_)
        MoveWindow(hwndBtnStart_, startX, y, unifiedBtnW, unifiedBtnH, TRUE);
    if (hwndBtnStop_)
        MoveWindow(hwndBtnStop_, stopX, y, unifiedBtnW, unifiedBtnH, TRUE);
    y += unifiedBtnH + itemSpacing_;

    // Target label (own row)
    if (hwndTargetLabel_)
        MoveWindow(hwndTargetLabel_, MARGIN, y, contentWidth, labelHeight_, TRUE);
    y += labelHeight_ + itemSpacing_ / 2;

    // ── Layout alignment block (sync with CreateControls) ──
    // Alignment: target buttons left edge = Stop button left edge (btnX = stopX)
    // Alignment: list right edge = Start button right edge (listW = btnX - gap - MARGIN)
    int btnX = stopX;
    btnX = (std::max)(btnX, MARGIN);  // safety clamp against extreme font/DPI cases
    int listW = btnX - DpiScale(kGapLarge) - MARGIN;
    listW = (std::max)(listW, DpiScale(120));  // minimum list width guard

    int listHeight = listHeightBase;
    int tgtBtnAreaH = unifiedBtnH * 3 + tgtBtnGap * 2 + 2 * inset;
    listHeight = (std::max)(listHeight, tgtBtnAreaH);
    int btnStartY = y + inset + (listHeight - tgtBtnAreaH) / 2;

    if (hwndTargetList_)
        MoveWindow(hwndTargetList_, MARGIN, y, listW, listHeight, TRUE);
    if (hwndBtnAdd_)
        MoveWindow(hwndBtnAdd_, btnX, btnStartY, unifiedBtnW, unifiedBtnH, TRUE);
    if (hwndBtnSelect_)
        MoveWindow(hwndBtnSelect_, btnX, btnStartY + unifiedBtnH + tgtBtnGap, unifiedBtnW, unifiedBtnH, TRUE);
    if (hwndBtnRemove_)
        MoveWindow(hwndBtnRemove_, btnX, btnStartY + (unifiedBtnH + tgtBtnGap) * 2 - 1, unifiedBtnW, unifiedBtnH, TRUE);
    y += listHeight + itemSpacing_;

    // Log label + toggle row
    if (hwndLogLabel_)
        MoveWindow(hwndLogLabel_, MARGIN, y, contentWidth / 2, labelHeight_, TRUE);
    if (hwndLogToggle_)
        MoveWindow(hwndLogToggle_, MARGIN + contentWidth - DpiScale(110), y, DpiScale(110), labelHeight_, TRUE);
    y += labelHeight_ + itemSpacing_ / 2;

    // Log edit (fill remaining space minus status bars)
    int logHeight = clientHeight - y - 4 - smallLabelHeight_ - smallLabelHeight_;
    if (logHeight < 60) logHeight = 60;

    if (hwndLogEdit_)
        MoveWindow(hwndLogEdit_, MARGIN, y, contentWidth, logHeight, TRUE);
    y += logHeight + DpiScale(kGapSmall);

    // Engine status bar
    if (hwndEngineStatus_)
        MoveWindow(hwndEngineStatus_, MARGIN, y, contentWidth, smallLabelHeight_, TRUE);
    y += smallLabelHeight_;

    // Observability status bar
    if (hwndObservabilityStatus_)
        MoveWindow(hwndObservabilityStatus_, MARGIN, y, contentWidth, smallLabelHeight_, TRUE);

    PostMessage(hwnd_, WM_APP_UPDATE_SCROLLBAR, 0, 0);
}

void MainWindow::DrawButton(LPDRAWITEMSTRUCT lpDIS) {
    HDC hdc = lpDIS->hDC;
    RECT rc = lpDIS->rcItem;
    UINT state = lpDIS->itemState;
    HWND hwndBtn = lpDIS->hwndItem;

    // Get button text
    wchar_t text[64] = {};
    GetWindowTextW(hwndBtn, text, 64);

    // Determine colors based on state
    COLORREF bgColor = clrButton_;
    COLORREF textColor = clrText_;
    COLORREF borderColor = clrButtonBorder_;

    bool isEnabled = !(state & ODS_DISABLED);
    bool isPressed = (state & ODS_SELECTED);

    if (!isEnabled) {
        bgColor = RGB(40, 40, 45);
        textColor = RGB(100, 100, 110);
        borderColor = RGB(50, 50, 55);
    } else if (isPressed) {
        bgColor = RGB(40, 40, 48);
        borderColor = clrAccent_;
    } else {
        // Check for hover
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwndBtn, &pt);
        RECT btnRect;
        GetClientRect(hwndBtn, &btnRect);
        if (PtInRect(&btnRect, pt)) {
            bgColor = clrButtonHover_;
            borderColor = RGB(90, 90, 100);
        }
    }

    // Fill background
    HBRUSH bgBrush = CreateSolidBrush(bgColor);
    FillRect(hdc, &rc, bgBrush);
    DeleteObject(bgBrush);

    // Draw border (rounded corners)
    HPEN borderPen = CreatePen(PS_SOLID, 1, borderColor);
    HPEN oldPen = (HPEN)SelectObject(hdc, borderPen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    RoundRect(hdc, rc.left, rc.top, rc.right - 1, rc.bottom - 1, 4, 4);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(borderPen);

    // Draw text (use small font for compact buttons)
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, textColor);
    SelectObject(hdc, fonts_.sm.handle);
    DrawTextW(hdc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void MainWindow::OnPaint() {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd_, &ps);

    RECT rc;
    GetClientRect(hwnd_, &rc);

    // Declare all variables before first goto (C++ jump-over-init constraint)
    HDC     hdcMem  = nullptr;
    HBITMAP hbmMem  = nullptr;
    HBITMAP hbmOld  = nullptr;
    HFONT   oldFont = nullptr;
    std::wstring verStr;

    hdcMem = CreateCompatibleDC(hdc);
    if (!hdcMem) goto cleanup;

    {
        int width  = rc.right  - rc.left;
        int height = rc.bottom - rc.top;
        hbmMem = CreateCompatibleBitmap(hdc, width, height);
    }
    if (!hbmMem) goto cleanup;

    hbmOld = (HBITMAP)SelectObject(hdcMem, hbmMem);
    if (!hbmOld) goto cleanup;

    // Background fill
    FillRect(hdcMem, &rc, hBrushBg_);

    // Draw title
    SetBkMode(hdcMem, TRANSPARENT);
    SetTextColor(hdcMem, clrText_);
    oldFont = (HFONT)SelectObject(hdcMem, fonts_.title.handle);
    TextOutW(hdcMem, MARGIN, MARGIN-1, L"UnLeaf Manager", 14);

    // Version in accent color
    SetTextColor(hdcMem, clrAccent_);
    SelectObject(hdcMem, fonts_.sm.handle);
    verStr = std::wstring(L"v") + unleaf::VERSION;
    TextOutW(hdcMem, MARGIN + 132, MARGIN + 2, verStr.c_str(), static_cast<int>(verStr.length()));

    if (!BitBlt(hdc, 0, 0, rc.right - rc.left, rc.bottom - rc.top, hdcMem, 0, 0, SRCCOPY)) {
        OutputDebugStringW(L"OnPaint BitBlt failed\n");
    }

cleanup:
    if (hdcMem && oldFont) SelectObject(hdcMem, oldFont);
    if (hdcMem && hbmOld)  SelectObject(hdcMem, hbmOld);
    if (hbmMem)            DeleteObject(hbmMem);
    if (hdcMem)            DeleteDC(hdcMem);
    EndPaint(hwnd_, &ps);
}

void MainWindow::DrawStatusIndicator(HDC hdc, int x, int y, bool online) {
    COLORREF color = online ? clrOnline_ : clrOffline_;
    HBRUSH brush = CreateSolidBrush(color);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, brush);
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);

    Ellipse(hdc, x - 5, y - 5, x + 5, y + 5);

    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}

void MainWindow::UpdateServiceStatus() {
    ServiceState state = serviceCtrl_.GetServiceState();
    currentState_ = state; // Update the global state shared by UI components

    std::wstring statusText;
    // New bold format: Symbol + Status Text
    switch (state) {
    case ServiceState::Running:
        statusText = L"● SERVICE RUNNING"; break;
    case ServiceState::Stopped:
        statusText = L"■ SERVICE STOPPED"; break;
    case ServiceState::NotInstalled:
        statusText = L"○ SERVICE NOT INSTALLED"; break;
    case ServiceState::StartPending:
        statusText = L"▶ SERVICE STARTING..."; break;
    case ServiceState::StopPending:
        statusText = L"⏳ SERVICE STOPPING..."; break;
    default:
        statusText = L"? SERVICE UNKNOWN"; break;
    }

    SetWindowTextW(hwndStatusLabel_, statusText.c_str());
    
    // Ensure both upper and lower status areas are repainted with the new state color
    InvalidateRect(hwndStatusLabel_, nullptr, TRUE);

    // Update button states based on service availability
    EnableWindow(hwndBtnStart_, state == ServiceState::Stopped || state == ServiceState::NotInstalled);
    EnableWindow(hwndBtnStop_, state != ServiceState::NotInstalled && state != ServiceState::Unknown);

}

void MainWindow::UpdateEngineStatus() {
    // Return to default if service is not running
    if (currentState_ != ServiceState::Running) {
        SetWindowTextW(hwndEngineStatus_, L"Processes: 0  |  Engine: Offline");
        InvalidateRect(hwndEngineStatus_, nullptr, TRUE); // Trigger immediate color update
        return;
    }

    // Request actual process count from service via IPC
    auto response = ipcClient_.SendCommand(IPCCommand::CMD_GET_STATS);
    if (response && response->size() >= sizeof(uint32_t)) {
        uint32_t actualCount = *reinterpret_cast<const uint32_t*>(response->data());
        
        std::wstringstream ss;
        ss << L"Processes: " << actualCount << L"  |  Engine: Online (ETW)";
        SetWindowTextW(hwndEngineStatus_, ss.str().c_str());
    } else {
        SetWindowTextW(hwndEngineStatus_, L"Processes: --  |  Engine: Online (Comm Error)");
    }

    // Force redraw to apply color changes in WM_CTLCOLORSTATIC immediately
    InvalidateRect(hwndEngineStatus_, nullptr, TRUE);
}

void MainWindow::UpdateObservabilityStatus() {
    if (!hwndObservabilityStatus_) return;

    if (currentState_ != ServiceState::Running) {
        SetWindowTextW(hwndObservabilityStatus_, L"");
        return;
    }

    // Request health check from service via IPC
    auto response = ipcClient_.SendCommand(IPCCommand::CMD_HEALTH_CHECK);
    if (!response || response->empty()) {
        SetWindowTextW(hwndObservabilityStatus_, L"Health: N/A");
        return;
    }

    // Parse JSON response with fallback
    try {
        auto j = nlohmann::json::parse(*response);
        std::wstringstream ss;

        uint32_t wakeupSn = j.value("/wakeups/safety_net"_json_pointer, 0u);
        uint32_t enfTotal = j.value("/enforcement/total"_json_pointer, 0u);
        uint32_t enfFail = j.value("/enforcement/fail"_json_pointer, 0u);
        uint32_t avgUs = j.value("/enforcement/avg_latency_us"_json_pointer, 0u);

        ss << L"SN: " << wakeupSn
           << L" | Enforcement: " << enfTotal
           << L" (" << enfFail << L" fail)"
           << L" | Latency: ";
        // avgUs == 0: no enforcement executed yet (unmeasured)
        if (avgUs == 0)
            ss << L"-";
        else if (avgUs < 1000)
            ss << L"<1ms";
        else
            ss << (avgUs / 1000) << L"ms";

        SetWindowTextW(hwndObservabilityStatus_, ss.str().c_str());
    } catch (const std::exception& e) {
        LOG_ERROR(std::wstring(L"[EXC] UpdateObservabilityStatus: ") +
                  unleaf::Utf8ToWide(e.what()));
        SetWindowTextW(hwndObservabilityStatus_, L"Health: parse error");
    } catch (...) {
        LOG_ERROR(L"[EXC] UpdateObservabilityStatus: unknown exception");
        SetWindowTextW(hwndObservabilityStatus_, L"Health: parse error");
    }
}

void MainWindow::RefreshTargetList() {
    SendMessageW(hwndTargetList_, LB_RESETCONTENT, 0, 0);

    if (UnLeafConfig::Instance().HasFileChanged()) {
        UnLeafConfig::Instance().Reload();
    }

    const auto& targets = UnLeafConfig::Instance().GetTargets();
    for (const auto& t : targets) {
        std::wstring item = t.name;
        if (!t.enabled) item += L" (disabled)";
        SendMessageW(hwndTargetList_, LB_ADDSTRING, 0, (LPARAM)item.c_str());
    }
}

void MainWindow::AppendLog(const std::wstring& message) {
    if (message.empty()) return;

    // Trim trailing whitespace
    std::wstring cleanMsg = message;
    while (!cleanMsg.empty() && (cleanMsg.back() == L'\0' || cleanMsg.back() == L'\r' || cleanMsg.back() == L'\n')) {
        cleanMsg.pop_back();
    }
    if (cleanMsg.empty()) return;

    // Tag UI messages and push to pending queue (consumed by RefreshLogDisplay on UI thread)
    std::wstring tagged = L"[ UI ] " + cleanMsg;
    {
        CSLockGuard lock(pendingLogCs_);
        pendingLogLines_.push_back(tagged);
    }
}

void MainWindow::ProcessNewLogLines(const std::vector<std::wstring>& newLines) {
    if (newLines.empty()) return;

    CSLockGuard lock(pendingLogCs_);
    for (const auto& line : newLines) {
        if (!line.empty()) {
            pendingLogLines_.push_back(line);
        }
    }
}

COLORREF MainWindow::GetLogLineColor(const std::wstring& line) const {
    if (line.find(L"[ERR ]") != std::wstring::npos)
        return RGB(255, 80, 80);
    if (line.find(L"[ALRT]") != std::wstring::npos)
        return RGB(255, 170, 0);
    if (line.find(L"[ UI ]") != std::wstring::npos)
        return RGB(80, 220, 220);
    if (line.find(L"[INFO]") != std::wstring::npos)
        return clrText_;
    if (line.find(L"[DEBG]") != std::wstring::npos)
        return RGB(100, 100, 115);
    return clrText_;
}

void MainWindow::RefreshLogDisplay() {
    if (!hwndLogEdit_) return;

    // Re-entrancy guard (prevents WM_SIZE cascades and update loops)
    static bool updating = false;
    if (updating) return;
    updating = true;

    // Move pending lines to local vector (minimize lock scope)
    std::vector<std::wstring> lines;
    {
        CSLockGuard lock(pendingLogCs_);
        if (pendingLogLines_.empty()) { updating = false; return; }
        lines.swap(pendingLogLines_);
    }

    // Update autoScroll_ based on current scroll position BEFORE modifying content
    {
        SCROLLINFO si = { sizeof(si), SIF_ALL };
        GetScrollInfo(hwndLogEdit_, SB_VERT, &si);
        if (si.nMax <= 0) {
            autoScroll_ = true;
        } else {
            autoScroll_ = (si.nPos + static_cast<int>(si.nPage) + 2 >= si.nMax);
        }
    }

    // Batch all modifications inside WM_SETREDRAW block
    SendMessageW(hwndLogEdit_, WM_SETREDRAW, FALSE, 0);

    bool isRichEdit = (hRichEditLib_ != nullptr);

    // Prepare paragraph format for uniform line spacing (reused across loop iterations)
    PARAFORMAT2 pf = {};
    pf.cbSize = sizeof(pf);
    pf.dwMask = PFM_LINESPACING | PFM_SPACEBEFORE | PFM_SPACEAFTER;
    pf.bLineSpacingRule = 0;
    pf.dySpaceBefore = 0;
    pf.dySpaceAfter = 0;

    // Get text length once before the loop to avoid O(N²) GetWindowTextLengthW calls
    LONG endPos = GetWindowTextLengthW(hwndLogEdit_);

    for (const auto& line : lines) {
        // Move cursor to end of text
        SendMessageW(hwndLogEdit_, EM_SETSEL, endPos, endPos);

        // Apply paragraph format to insertion point (uniform line spacing)
        if (isRichEdit) {
            SendMessageW(hwndLogEdit_, EM_SETPARAFORMAT, 0, (LPARAM)&pf);
        }

        // Set per-line color (RichEdit only)
        if (isRichEdit) {
            CHARFORMAT2W cf = {};
            cf.cbSize = sizeof(cf);
            cf.dwMask = CFM_COLOR;
            cf.crTextColor = GetLogLineColor(line);
            SendMessageW(hwndLogEdit_, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
        }

        // Append line with newline
        std::wstring text = line + L"\r\n";
        SendMessageW(hwndLogEdit_, EM_REPLACESEL, FALSE, (LPARAM)text.c_str());
        endPos += static_cast<LONG>(text.length());
        richEditLineCount_++;
    }

    // Trim excess lines from top (batch: delete all excess at once)
    if (richEditLineCount_ > MAX_RICHEDIT_LINES) {
        size_t excess = richEditLineCount_ - MAX_RICHEDIT_LINES;
        LONG trimEnd = static_cast<LONG>(
            SendMessageW(hwndLogEdit_, EM_LINEINDEX, static_cast<WPARAM>(excess), 0));
        if (trimEnd > 0 && trimEnd != -1) {
            SendMessageW(hwndLogEdit_, EM_SETSEL, 0, trimEnd);
            SendMessageW(hwndLogEdit_, EM_REPLACESEL, FALSE, (LPARAM)L"");
            richEditLineCount_ -= excess;
        }
    }

    SendMessageW(hwndLogEdit_, WM_SETREDRAW, TRUE, 0);

    // Auto-scroll to bottom if user was at the bottom
    if (autoScroll_) {
        SendMessageW(hwndLogEdit_, WM_VSCROLL, SB_BOTTOM, 0);
    }

    InvalidateRect(hwndLogEdit_, nullptr, TRUE);

    // Deferred scrollbar visibility update
    PostMessage(hwnd_, WM_APP_UPDATE_SCROLLBAR, 0, 0);

    updating = false;
}

void MainWindow::UpdateScrollBarVisibility() {
    if (!hwndLogEdit_) return;

    SCROLLINFO si = {};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE;
    GetScrollInfo(hwndLogEdit_, SB_VERT, &si);

    bool needScroll = ((si.nMax - si.nMin) >= static_cast<int>(si.nPage));
    if (needScroll != scrollBarVisible_) {
        scrollBarVisible_ = needScroll;
        ShowScrollBar(hwndLogEdit_, SB_VERT, needScroll ? TRUE : FALSE);
        // Force non-client area repaint after scrollbar visibility change
        RedrawWindow(hwndLogEdit_, NULL, NULL, RDW_FRAME | RDW_INVALIDATE);
    }
}

LRESULT CALLBACK MainWindow::ToggleSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR /*uIdSubclass*/, DWORD_PTR dwRefData) {
    auto* self = reinterpret_cast<MainWindow*>(dwRefData);

    switch (msg) {
    case WM_PAINT: {
        if (self->highContrast_) {
            return DefSubclassProc(hwnd, msg, wParam, lParam);
        }

        PAINTSTRUCT ps;
        HDC hdcPaint = BeginPaint(hwnd, &ps);

        RECT rc;
        GetClientRect(hwnd, &rc);

        // Double-buffer drawing
        HDC hdcMem = CreateCompatibleDC(hdcPaint);
        HBITMAP hbmMem = CreateCompatibleBitmap(hdcPaint, rc.right, rc.bottom);
        HBITMAP hbmOld = (HBITMAP)SelectObject(hdcMem, hbmMem);

        bool checked = (SendMessageW(hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED);
        self->DrawToggleSwitch(hdcMem, rc, checked);

        BitBlt(hdcPaint, 0, 0, rc.right, rc.bottom, hdcMem, 0, 0, SRCCOPY);

        SelectObject(hdcMem, hbmOld);
        DeleteObject(hbmMem);
        DeleteDC(hdcMem);

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP: {
        // Only respond to clicks on the toggle track area
        RECT rc;
        GetClientRect(hwnd, &rc);
        int trackH = static_cast<int>(self->smallFontHeight_ * 0.85f);
        if (trackH < 8) trackH = 8;
        int trackW = static_cast<int>(trackH * 1.8f);
        int trackX = rc.right - trackW - self->DpiScale(2);
        int clickX = GET_X_LPARAM(lParam);
        if (clickX < trackX)
            return 0;  // Ignore clicks on the text area
        break;
    }

    case WM_MOUSEMOVE: {
        if (!self->toggleHovered_) {
            self->toggleHovered_ = true;
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;
    }
    case WM_MOUSELEAVE:
        self->toggleHovered_ = false;
        InvalidateRect(hwnd, nullptr, FALSE);
        break;

    case WM_ERASEBKGND:
        return TRUE;

    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, ToggleSubclassProc, 0);
        return DefSubclassProc(hwnd, msg, wParam, lParam);
    }

    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

void MainWindow::DrawToggleSwitch(HDC hdc, RECT rc, bool checked) {
    // One-time GDI+ initialization (process lifetime)
    static ULONG_PTR s_gdipToken = []() -> ULONG_PTR {
        ULONG_PTR tok = 0;
        Gdiplus::GdiplusStartupInput inp;
        Gdiplus::GdiplusStartup(&tok, &inp, nullptr);
        return tok;
    }();
    (void)s_gdipToken;

    // --- All drawing via GDI+ (no GDI mixing) ---
    Gdiplus::Graphics gfx(hdc);
    gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    gfx.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

    // Background
    gfx.Clear(Gdiplus::Color(255, GetRValue(clrBackground_),
                              GetGValue(clrBackground_),
                              GetBValue(clrBackground_)));

    // Toggle dimensions derived from font metrics (compute first for text layout)
    int trackH = static_cast<int>(smallFontHeight_ * 0.85f);
    if (trackH < 8) trackH = 8;                       // safety floor
    int trackW = static_cast<int>(trackH * 1.8f);
    int thumbR = (trackH - DpiScale(4)) / 2;          // inset 2dp each side
    if (thumbR < 3) thumbR = 3;

    int trackX = rc.right - trackW - DpiScale(2);
    int trackY = (rc.bottom + rc.top - trackH) / 2;

    // "Log Output" text — gap to toggle matches button spacing (kGapSmall)
    SetTextColor(hdc, clrText_);
    SetBkMode(hdc, TRANSPARENT);
    HFONT oldFont = (HFONT)SelectObject(hdc, fonts_.sm.handle);

    RECT textRect = rc;
    textRect.right = trackX - DpiScale(kGapSmall);
    DrawTextW(hdc, L"Log Output", -1, &textRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    SelectObject(hdc, oldFont);
    float cornerF = trackH / 2.0f;

    // Track colors — Cyan Resonance theme
    Gdiplus::Color trackClr;
    if (checked) {
        trackClr = toggleHovered_
            ? Gdiplus::Color(255, 100, 235, 235)   // Cyan hover
            : Gdiplus::Color(255, 80, 220, 220);    // Cyan #50DCDC
    } else {
        trackClr = toggleHovered_
            ? Gdiplus::Color(255, 75, 75, 85)       // Dark steel hover
            : Gdiplus::Color(255, 61, 61, 70);      // Dark steel #3D3D46
    }
    Gdiplus::SolidBrush trackBrush(trackClr);

    // Build rounded-rect path
    Gdiplus::GraphicsPath path;
    float tx = (float)trackX, ty = (float)trackY;
    float tw = (float)trackW, th = (float)trackH;
    float d = cornerF * 2.0f;
    path.AddArc(tx, ty, d, d, 180.0f, 90.0f);
    path.AddArc(tx + tw - d, ty, d, d, 270.0f, 90.0f);
    path.AddArc(tx + tw - d, ty + th - d, d, d, 0.0f, 90.0f);
    path.AddArc(tx, ty + th - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
    gfx.FillPath(&trackBrush, &path);

    // Thumb
    float thumbCY = trackY + trackH / 2.0f;
    float thumbCX = checked
        ? trackX + trackW - cornerF
        : trackX + cornerF;
    Gdiplus::SolidBrush thumbBrush(Gdiplus::Color(255, 240, 240, 245));
    gfx.FillEllipse(&thumbBrush,
        thumbCX - thumbR, thumbCY - thumbR,
        (float)(thumbR * 2), (float)(thumbR * 2));
}

void MainWindow::OnStartService() {
    if (!ServiceController::IsRunningAsAdmin()) {
        int result = CenteredMessageBoxW(hwnd_,
            L"管理者権限が必要です。\n\n管理者として再起動しますか？",
            L"権限が必要", MB_YESNO | MB_ICONQUESTION);
        if (result == IDYES) {
            ServiceController::RestartAsAdmin();
            PostQuitMessage(0);
        }
        return;
    }

    // Step 1: Check current state
    ServiceState state = serviceCtrl_.GetServiceState();

    // Already running - notify user and do nothing
    if (state == ServiceState::Running) {
        AppendLog(L"Service is already running");
        CenteredMessageBoxW(hwnd_, L"サービスは既に実行中です。",
            L"サービス登録・実行", MB_OK | MB_ICONINFORMATION);
        return;
    }

    // Starting - wait for it
    if (state == ServiceState::StartPending) {
        AppendLog(L"Service is starting...");
        return;
    }

    std::wstring exePath = baseDir_ + L"\\UnLeaf_Service.exe";

    // Step 2: Install service if not exists
    if (state == ServiceState::NotInstalled) {
        AppendLog(L"Registering service...");
        if (serviceCtrl_.InstallService(exePath)) {
            AppendLog(L"Service registered");
        } else {
            AppendLog(L"Error: " + serviceCtrl_.GetLastError());
            CenteredMessageBoxW(hwnd_, (L"サービス登録に失敗しました:\n" + serviceCtrl_.GetLastError()).c_str(),
                L"エラー", MB_OK | MB_ICONERROR);
            return;
        }
    }

    // Step 3: Start service if stopped
    AppendLog(L"Starting service...");
    if (serviceCtrl_.StartService()) {
        AppendLog(L"Service start command sent");
    } else {
        AppendLog(L"Error: " + serviceCtrl_.GetLastError());
        CenteredMessageBoxW(hwnd_, (L"サービス開始に失敗しました:\n" + serviceCtrl_.GetLastError()).c_str(),
            L"エラー", MB_OK | MB_ICONERROR);
    }

    UpdateServiceStatus();
}

void MainWindow::OnStopService() {
    if (!ServiceController::IsRunningAsAdmin()) {
        int result = CenteredMessageBoxW(hwnd_,
            L"管理者権限が必要です。\n\n管理者として再起動しますか？",
            L"権限が必要", MB_YESNO | MB_ICONQUESTION);
        if (result == IDYES) {
            ServiceController::RestartAsAdmin();
            PostQuitMessage(0);
        }
        return;
    }

    // Confirm unregistration
    int result = CenteredMessageBoxW(hwnd_,
        L"サービスを停止し、登録を解除します。\n\nよろしいですか？",
        L"サービス登録解除", MB_YESNO | MB_ICONQUESTION);
    if (result != IDYES) {
        return;
    }

    AppendLog(L"Unregistering service...");

    // Use the unified UninstallService which handles stop + wait + delete
    if (serviceCtrl_.UninstallService()) {
        AppendLog(L"Service unregistered");

        // Safety net: Remove any remaining registry policies via manifest
        RegistryPolicyManager::Instance().Initialize(baseDir_);
        RegistryPolicyManager::Instance().RemoveAllPolicies();
        AppendLog(L"Registry policies removed");

        CenteredMessageBoxW(hwnd_, L"サービスの登録解除が完了しました。\n\nレジストリは初期状態に復元されました。",
            L"サービス登録解除", MB_OK | MB_ICONINFORMATION);
    } else {
        AppendLog(L"Error: " + serviceCtrl_.GetLastError());
        CenteredMessageBoxW(hwnd_, (L"サービス登録解除に失敗しました:\n" + serviceCtrl_.GetLastError()).c_str(),
            L"エラー", MB_OK | MB_ICONERROR);
    }

    UpdateServiceStatus();
}

// Dialog subclass context
struct DialogCloseContext {
    bool closeRequested;
    WNDPROC originalProc;
};

// Subclass proc to intercept WM_CLOSE and WM_SYSCOMMAND SC_CLOSE
static LRESULT CALLBACK DialogCloseSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ctx = reinterpret_cast<DialogCloseContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!ctx) return DefWindowProcW(hwnd, msg, wParam, lParam);

    if (msg == WM_CLOSE) {
        ctx->closeRequested = true;
        return 0;  // Don't let default proc handle it
    }
    if (msg == WM_SYSCOMMAND && (wParam & 0xFFF0) == SC_CLOSE) {
        ctx->closeRequested = true;
        return 0;  // Don't let default proc handle it
    }

    return CallWindowProcW(ctx->originalProc, hwnd, msg, wParam, lParam);
}

void MainWindow::OnAddTarget() {
    wchar_t name[256] = {};

    // Dialog dimensions adjusted for content
    constexpr int DLG_WIDTH = 350;
    constexpr int DLG_HEIGHT = 150;

    // Center dialog on parent window (not screen)
    RECT parentRect;
    GetWindowRect(hwnd_, &parentRect);
    int parentCenterX = (parentRect.left + parentRect.right) / 2;
    int parentCenterY = (parentRect.top + parentRect.bottom) / 2;
    int dlgX = parentCenterX - DLG_WIDTH / 2;
    int dlgY = parentCenterY - DLG_HEIGHT / 2;

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        L"#32770", L"対象の追加 (Add Target)",
        WS_POPUP | WS_VISIBLE | WS_CAPTION | WS_SYSMENU,
        dlgX, dlgY,
        DLG_WIDTH, DLG_HEIGHT, hwnd_, nullptr, hInstance_, nullptr);

    if (!dlg) return;

    // Subclass dialog to intercept close messages
    DialogCloseContext closeCtx = { false, nullptr };
    closeCtx.originalProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(dlg, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(DialogCloseSubclassProc)));
    SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&closeCtx));

    HWND label = CreateWindowW(L"STATIC", L"プロセス名 (例: game.exe):",
        WS_CHILD | WS_VISIBLE, 15, 15, 300, 20, dlg, nullptr, hInstance_, nullptr);
    SendMessageW(label, WM_SETFONT, (WPARAM)fonts_.normal.handle, TRUE);

    HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        15, 40, 305, 24, dlg, nullptr, hInstance_, nullptr);
    SendMessageW(edit, WM_SETFONT, (WPARAM)fonts_.normal.handle, TRUE);

    HWND btnOk = CreateWindowW(L"BUTTON", L"追加",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        160, 75, 75, 28, dlg, (HMENU)IDOK, hInstance_, nullptr);
    SendMessageW(btnOk, WM_SETFONT, (WPARAM)fonts_.normal.handle, TRUE);

    HWND btnCancel = CreateWindowW(L"BUTTON", L"キャンセル",
        WS_CHILD | WS_VISIBLE,
        245, 75, 75, 28, dlg, (HMENU)IDCANCEL, hInstance_, nullptr);
    SendMessageW(btnCancel, WM_SETFONT, (WPARAM)fonts_.normal.handle, TRUE);

    SetFocus(edit);
    EnableWindow(hwnd_, FALSE);

    MSG msg;
    bool done = false;
    bool cancelled = false;
    std::wstring result;

    while (!done && GetMessage(&msg, nullptr, 0, 0) > 0) {
        // Check if close was requested via subclass proc
        if (closeCtx.closeRequested) {
            done = true;
            cancelled = true;
            continue;
        }

        // Handle keyboard
        if (msg.message == WM_KEYDOWN) {
            if (msg.wParam == VK_RETURN) {
                GetWindowTextW(edit, name, 256);
                result = name;
                done = true;
                continue;
            } else if (msg.wParam == VK_ESCAPE) {
                done = true;
                cancelled = true;
                continue;
            }
        }

        // Handle button clicks
        if (msg.message == WM_LBUTTONUP) {
            if (msg.hwnd == btnOk) {
                GetWindowTextW(edit, name, 256);
                result = name;
                done = true;
                continue;
            }
            if (msg.hwnd == btnCancel) {
                done = true;
                cancelled = true;
                continue;
            }
        }

        TranslateMessage(&msg);
        DispatchMessage(&msg);

        // Check again after dispatch
        if (closeCtx.closeRequested || !IsWindow(dlg)) {
            done = true;
            cancelled = true;
        }
    }

    EnableWindow(hwnd_, TRUE);
    if (IsWindow(dlg)) {
        DestroyWindow(dlg);
    }
    SetForegroundWindow(hwnd_);
    SetFocus(hwnd_);

    if (!cancelled && !result.empty()) {
        // Auto-append .exe if not present
        if (result.length() < 4 || _wcsicmp(result.c_str() + result.length() - 4, L".exe") != 0) {
            result += L".exe";
        }

        if (UnLeafConfig::Instance().AddTarget(result)) {
            UnLeafConfig::Instance().Save();
            RefreshTargetList();
            AppendLog(L"[+] REGISTERED : " + result);
        } else {
            CenteredMessageBoxW(hwnd_, L"追加に失敗しました（既に存在するか保護されています）",
                L"対象の追加", MB_OK | MB_ICONWARNING);
        }
    }
}

void MainWindow::OnRemoveTarget() {
    int sel = (int)SendMessageW(hwndTargetList_, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR) {
        CenteredMessageBoxW(hwnd_, L"対象を選択してください", L"削除", MB_OK | MB_ICONINFORMATION);
        return;
    }

    wchar_t text[256] = {};
    SendMessageW(hwndTargetList_, LB_GETTEXT, sel, (LPARAM)text);

    std::wstring name = text;
    size_t pos = name.find(L" (disabled)");
    if (pos != std::wstring::npos) {
        name = name.substr(0, pos);
    }

    if (UnLeafConfig::Instance().RemoveTarget(name)) {
        UnLeafConfig::Instance().Save();
        RefreshTargetList();
        AppendLog(L"[-] UNREGISTERED : " + name);
    }
}

void MainWindow::OnSelectProcess() {
    auto processes = GetRunningProcesses();

    constexpr int DLG_WIDTH = 360;
    constexpr int DLG_HEIGHT = 440;

    // Center dialog on parent window (not screen)
    RECT parentRect;
    GetWindowRect(hwnd_, &parentRect);
    int parentCenterX = (parentRect.left + parentRect.right) / 2;
    int parentCenterY = (parentRect.top + parentRect.bottom) / 2;
    int dlgX = parentCenterX - DLG_WIDTH / 2;
    int dlgY = parentCenterY - DLG_HEIGHT / 2;

    // Create the process selection dialog window
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        L"#32770", L"プロセスの選択 (Select Process)",
        WS_POPUP | WS_VISIBLE | WS_CAPTION | WS_SYSMENU,
        dlgX, dlgY,
        DLG_WIDTH, DLG_HEIGHT, hwnd_, nullptr, hInstance_, nullptr);

    if (!dlg) return;

    // Subclass the dialog to handle the "X" (Close) button properly
    DialogCloseContext closeCtx = { false, nullptr };
    closeCtx.originalProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(dlg, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(DialogCloseSubclassProc)));
    SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&closeCtx));

    // Description label at top
    HWND descLabel = CreateWindowW(L"STATIC",
        L"実行中のプロセスから対象を選択してください:",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        15, 15, 320, 20, dlg, nullptr, hInstance_, nullptr);
    SendMessageW(descLabel, WM_SETFONT, (WPARAM)fonts_.normal.handle, TRUE);

    // Create ListBox for process enumeration (moved down for description)
    HWND list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_SORT,
        15, 40, 315, 300, dlg, nullptr, hInstance_, nullptr);
    SendMessageW(list, WM_SETFONT, (WPARAM)fonts_.normal.handle, TRUE);

    // Populate process list
    for (const auto& p : processes) {
        SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)p.c_str());
    }

    // Create "追加" button (matching OnAddTarget style)
    HWND btnOk = CreateWindowW(L"BUTTON", L"追加",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        160, 355, 75, 28, dlg, (HMENU)IDOK, hInstance_, nullptr);
    SendMessageW(btnOk, WM_SETFONT, (WPARAM)fonts_.normal.handle, TRUE);

    // Create "キャンセル" button (matching OnAddTarget style)
    HWND btnCancel = CreateWindowW(L"BUTTON", L"キャンセル",
        WS_CHILD | WS_VISIBLE,
        245, 355, 75, 28, dlg, (HMENU)IDCANCEL, hInstance_, nullptr);
    SendMessageW(btnCancel, WM_SETFONT, (WPARAM)fonts_.normal.handle, TRUE);

    // Disable the main window while the modal dialog is open
    EnableWindow(hwnd_, FALSE);

    MSG msg;
    bool done = false;
    bool cancelled = false;
    std::wstring result;

    // Modal message loop
    while (!done && GetMessage(&msg, nullptr, 0, 0) > 0) {
        // Exit loop if the "X" button or Alt+F4 was triggered via subclass
        if (closeCtx.closeRequested) {
            done = true;
            cancelled = true;
            continue;
        }

        // Handle Escape key for cancellation
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            done = true;
            cancelled = true;
            continue;
        }

        // Handle ListBox double-click for selection
        if (msg.hwnd == list && msg.message == WM_LBUTTONDBLCLK) {
            int sel = (int)SendMessageW(list, LB_GETCURSEL, 0, 0);
            if (sel != LB_ERR) {
                wchar_t buf[256] = {};
                SendMessageW(list, LB_GETTEXT, sel, (LPARAM)buf);
                result = buf;
            }
            done = true;
            continue;
        }

        // Handle button clicks
        if (msg.message == WM_LBUTTONUP) {
            if (msg.hwnd == btnOk) {
                int sel = (int)SendMessageW(list, LB_GETCURSEL, 0, 0);
                if (sel != LB_ERR) {
                    wchar_t buf[256] = {};
                    SendMessageW(list, LB_GETTEXT, sel, (LPARAM)buf);
                    result = buf;
                }
                done = true;
                continue;
            }
            if (msg.hwnd == btnCancel) {
                done = true;
                cancelled = true;
                continue;
            }
        }

        TranslateMessage(&msg);
        DispatchMessage(&msg);

        // Safety check to exit if the window was destroyed
        if (!IsWindow(dlg)) {
            done = true;
            cancelled = true;
        }
    }

    // Re-enable main window and cleanup
    EnableWindow(hwnd_, TRUE);
    if (IsWindow(dlg)) {
        DestroyWindow(dlg);
    }
    SetForegroundWindow(hwnd_);
    SetFocus(hwnd_);

    // Register process if selection was valid
    if (!cancelled && !result.empty()) {
        if (UnLeafConfig::Instance().AddTarget(result)) {
            UnLeafConfig::Instance().Save();
            RefreshTargetList();
            AppendLog(L"[+] REGISTERED : " + result);
        }
    }
}

std::vector<std::wstring> MainWindow::GetRunningProcesses() {
    std::vector<std::wstring> result;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return result;

    PROCESSENTRY32W pe = { sizeof(pe) };
    if (Process32FirstW(snap, &pe)) {
        do {
            std::wstring name = pe.szExeFile;
            if (!IsCriticalProcess(name)) {
                bool found = false;
                for (const auto& p : result) {
                    if (_wcsicmp(p.c_str(), name.c_str()) == 0) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    result.push_back(name);
                }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    std::sort(result.begin(), result.end(),
        [](const std::wstring& a, const std::wstring& b) {
            return _wcsicmp(a.c_str(), b.c_str()) < 0;
        });

    return result;
}

void MainWindow::CreateTrayIcon() {
    nid_.cbSize = sizeof(nid_);
    nid_.hWnd = hwnd_;
    nid_.uID = 1;
    nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid_.uCallbackMessage = WM_TRAYICON;
    nid_.hIcon = (HICON)LoadImageW(hInstance_,MAKEINTRESOURCEW(IDI_APP_ICON),IMAGE_ICON,16, 16,LR_DEFAULTCOLOR | LR_SHARED);
    wcscpy_s(nid_.szTip, L"UnLeaf Manager");

    Shell_NotifyIconW(NIM_ADD, &nid_);
    trayCreated_ = true;
}

void MainWindow::RemoveTrayIcon() {
    if (trayCreated_) {
        Shell_NotifyIconW(NIM_DELETE, &nid_);
        trayCreated_ = false;
    }
}

void MainWindow::ShowTrayMenu() {
    POINT pt;
    GetCursorPos(&pt);

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_TRAY_OPEN, L"開く (Open)");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, L"終了 (Exit)");

    SetForegroundWindow(hwnd_);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
}

void MainWindow::MinimizeToTray() {
    ShowWindow(hwnd_, SW_HIDE);
    minimizedToTray_ = true;
}

void MainWindow::RestoreFromTray() {
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    minimizedToTray_ = false;
}

// Helper: Convert UTF-8 to wide string
static std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return L"";
    int wideSize = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
        static_cast<int>(utf8.size()), nullptr, 0);
    if (wideSize <= 0) return L"";
    std::wstring wide(wideSize, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
        static_cast<int>(utf8.size()), &wide[0], wideSize);
    return wide;
}

std::vector<std::string> MainWindow::ReadTailLines(const std::wstring& path, size_t maxLines) {
    std::vector<std::string> result;

    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (hFile == INVALID_HANDLE_VALUE) {
        return result;
    }

    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart == 0) {
        CloseHandle(hFile);
        return result;
    }

    uint64_t size = static_cast<uint64_t>(fileSize.QuadPart);

    // Read from end in chunks to find lines
    constexpr size_t CHUNK_SIZE = 65536;  // 64KB chunks
    std::string accumulated;
    uint64_t readPos = size;
    size_t lineCount = 0;

    while (readPos > 0 && lineCount < maxLines) {
        uint64_t chunkStart = (readPos > CHUNK_SIZE) ? readPos - CHUNK_SIZE : 0;
        size_t chunkSize = static_cast<size_t>(readPos - chunkStart);

        LARGE_INTEGER seekPos;
        seekPos.QuadPart = static_cast<LONGLONG>(chunkStart);
        SetFilePointerEx(hFile, seekPos, nullptr, FILE_BEGIN);

        std::string chunk(chunkSize, '\0');
        DWORD bytesRead = 0;
        if (!ReadFile(hFile, &chunk[0], static_cast<DWORD>(chunkSize), &bytesRead, nullptr)) {
            break;
        }
        chunk.resize(bytesRead);

        // Prepend to accumulated data
        accumulated = chunk + accumulated;
        readPos = chunkStart;

        // Count newlines in accumulated data
        lineCount = 0;
        for (char c : accumulated) {
            if (c == '\n') lineCount++;
        }
    }

    CloseHandle(hFile);

    // Parse lines from accumulated data
    std::vector<std::string> allLines;
    size_t start = 0;
    size_t pos;
    while ((pos = accumulated.find('\n', start)) != std::string::npos) {
        std::string line = accumulated.substr(start, pos - start);
        // Trim trailing CR
        while (!line.empty() && (line.back() == '\r' || line.back() == '\0')) {
            line.pop_back();
        }
        if (!line.empty()) {
            allLines.push_back(line);
        }
        start = pos + 1;
    }
    // Handle last line without newline
    if (start < accumulated.size()) {
        std::string line = accumulated.substr(start);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == '\0')) {
            line.pop_back();
        }
        if (!line.empty()) {
            allLines.push_back(line);
        }
    }

    // Return only the last maxLines
    if (allLines.size() > maxLines) {
        result.assign(allLines.end() - maxLines, allLines.end());
    } else {
        result = std::move(allLines);
    }

    return result;
}

std::vector<std::string> MainWindow::ReadNewLines(HANDLE hFile, uint64_t& offset) {
    std::vector<std::string> result;

    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(hFile, &fileSize)) {
        return result;
    }

    uint64_t size = static_cast<uint64_t>(fileSize.QuadPart);

    // Handle log rotation (file got smaller)
    if (size < offset) {
        offset = 0;
        partialLine_.clear();
    }

    // Read new data
    if (size <= offset) {
        return result;
    }

    LARGE_INTEGER seekPos;
    seekPos.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(hFile, seekPos, nullptr, FILE_BEGIN)) {
        return result;
    }

    uint64_t available = size - offset;
    constexpr uint64_t MAX_READ = 65536;  // 64KB max per read
    uint64_t toRead = (available > MAX_READ) ? MAX_READ : available;

    std::string buffer(static_cast<size_t>(toRead), '\0');
    DWORD bytesRead = 0;
    if (!ReadFile(hFile, &buffer[0], static_cast<DWORD>(toRead), &bytesRead, nullptr)) {
        return result;
    }

    if (bytesRead == 0) {
        return result;
    }

    buffer.resize(bytesRead);
    offset += bytesRead;

    // Process with partial line handling
    std::string data = partialLine_ + buffer;
    partialLine_.clear();

    size_t start = 0;
    size_t pos;
    while ((pos = data.find('\n', start)) != std::string::npos) {
        std::string line = data.substr(start, pos - start);
        // Trim trailing CR
        while (!line.empty() && (line.back() == '\r' || line.back() == '\0')) {
            line.pop_back();
        }
        if (!line.empty()) {
            result.push_back(line);
        }
        start = pos + 1;
    }

    // Save partial line for next read
    if (start < data.size()) {
        partialLine_ = data.substr(start);
    }

    return result;
}

void MainWindow::LoadInitialLogTail() {
    std::wstring logPath = baseDir_ + L"\\" + LOG_FILENAME;

    auto lines = ReadTailLines(logPath, MAX_RICHEDIT_LINES);

    {
        CSLockGuard lock(pendingLogCs_);
        for (const auto& line : lines) {
            pendingLogLines_.push_back(Utf8ToWide(line));
        }
    }

    // Get current file size for incremental reading
    HANDLE hFile = CreateFileW(logPath.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (hFile != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER fileSize;
        if (GetFileSizeEx(hFile, &fileSize)) {
            lastLogFileSize_ = static_cast<uint64_t>(fileSize.QuadPart);
        }
        CloseHandle(hFile);
    }
}

void MainWindow::LogWatcherThread() {
    std::wstring logPath = baseDir_ + L"\\" + LOG_FILENAME;
    bool fileNotFoundReported = false;
    bool initialLoadDone = false;

    // Helper: Check if we should exit
    auto shouldExit = [this]() -> bool {
        return shutdownFlag_.load(std::memory_order_acquire) ||
               !running_.load(std::memory_order_acquire);
    };

    // Helper: Request UI refresh
    auto requestRefresh = [this, &shouldExit]() {
        if (shouldExit()) return;
        if (hwnd_ && IsWindow(hwnd_)) {
            PostMessageW(hwnd_, WM_LOG_REFRESH, 0, 0);
        }
    };

    // Initial load of tail lines
    if (!shouldExit()) {
        LoadInitialLogTail();
        requestRefresh();
        initialLoadDone = true;
    }

    // Main loop - Incremental reading
    while (!shouldExit()) {
        HANDLE hFile = CreateFileW(logPath.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

        if (hFile != INVALID_HANDLE_VALUE) {
            if (!initialLoadDone) {
                // First successful open after file was missing
                LoadInitialLogTail();
                requestRefresh();
                initialLoadDone = true;
            }

            fileNotFoundReported = false;

            // Read new lines incrementally
            auto newLines = ReadNewLines(hFile, lastLogFileSize_);
            CloseHandle(hFile);

            if (!newLines.empty()) {
                // Convert and add to buffer
                std::vector<std::wstring> wideLines;
                wideLines.reserve(newLines.size());
                for (const auto& line : newLines) {
                    wideLines.push_back(Utf8ToWide(line));
                }
                ProcessNewLogLines(wideLines);
                requestRefresh();
            }
        } else {
            // File not found
            if (!fileNotFoundReported) {
                {
                    CSLockGuard lock(pendingLogCs_);
                    pendingLogLines_.push_back(L"[Log file not found] Please start the service");
                }
                requestRefresh();
                fileNotFoundReported = true;
                initialLoadDone = false;  // Reset for re-load when file appears
            }
        }

        // Wait ~500ms with responsive shutdown checks
        for (int i = 0; i < 10 && !shouldExit(); ++i) {
            if (logWakeEvent_) {
                DWORD result = WaitForSingleObject(logWakeEvent_, 50);
                if (result == WAIT_OBJECT_0) break;
            } else {
                Sleep(50);
            }
        }
    }
}

std::wstring MainWindow::GetBaseDirectory() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring fullPath(path);
    size_t pos = fullPath.find_last_of(L"\\/");
    return (pos != std::wstring::npos) ? fullPath.substr(0, pos) : L".";
}

// ============================================================================
// Context Menu Implementation
// ============================================================================

void MainWindow::ShowTargetListContextMenu(int x, int y) {
    int sel = static_cast<int>(SendMessageW(hwndTargetList_, LB_GETCURSEL, 0, 0));

    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    if (sel != LB_ERR) {
        // Get selected item text
        wchar_t text[256] = {};
        SendMessageW(hwndTargetList_, LB_GETTEXT, sel, reinterpret_cast<LPARAM>(text));

        // Check if currently disabled
        bool isDisabled = (wcsstr(text, L"(disabled)") != nullptr);

        if (isDisabled) {
            AppendMenuW(menu, MF_STRING, ID_CTX_TARGET_ENABLE,
                L"有効にする (Enable)");
        } else {
            AppendMenuW(menu, MF_STRING, ID_CTX_TARGET_DISABLE,
                L"無効にする (Disable)");
        }

        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, ID_CTX_TARGET_DELETE,
            L"削除 (Delete)");
    } else {
        // No selection - show grayed out menu
        AppendMenuW(menu, MF_STRING | MF_GRAYED, ID_CTX_TARGET_ENABLE,
            L"有効にする (Enable)");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | MF_GRAYED, ID_CTX_TARGET_DELETE,
            L"削除 (Delete)");
    }

    SetForegroundWindow(hwnd_);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, x, y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
}

void MainWindow::ShowLogEditContextMenu(int x, int y) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    // Check if there's a selection
    DWORD selStart = 0, selEnd = 0;
    SendMessageW(hwndLogEdit_, EM_GETSEL,
        reinterpret_cast<WPARAM>(&selStart),
        reinterpret_cast<LPARAM>(&selEnd));
    bool hasSelection = (selStart != selEnd);

    AppendMenuW(menu, MF_STRING | (hasSelection ? 0 : MF_GRAYED),
        ID_CTX_LOG_COPY, L"コピー (Copy)");
    AppendMenuW(menu, MF_STRING, ID_CTX_LOG_SELECT_ALL,
        L"すべて選択 (Select All)");
    AppendMenuW(menu, MF_STRING, ID_CTX_LOG_CLEAR,
        L"クリア (Clear)");

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_CTX_LOG_OPEN_FILE,
        L"ログファイルを開く (Open Log File)");

    SetForegroundWindow(hwnd_);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, x, y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
}

void MainWindow::OnToggleTargetEnabled() {
    int sel = static_cast<int>(SendMessageW(hwndTargetList_, LB_GETCURSEL, 0, 0));
    if (sel == LB_ERR) return;

    wchar_t text[256] = {};
    SendMessageW(hwndTargetList_, LB_GETTEXT, sel, reinterpret_cast<LPARAM>(text));

    // Extract process name (remove " (disabled)" suffix if present)
    std::wstring name = text;
    size_t pos = name.find(L" (disabled)");
    if (pos != std::wstring::npos) {
        name = name.substr(0, pos);
    }

    // Get current state and toggle
    bool currentlyEnabled = UnLeafConfig::Instance().IsTargetEnabled(name);
    bool newEnabled = !currentlyEnabled;

    if (UnLeafConfig::Instance().SetTargetEnabled(name, newEnabled)) {
        UnLeafConfig::Instance().Save();
        RefreshTargetList();

        // Restore selection
        SendMessageW(hwndTargetList_, LB_SETCURSEL, sel, 0);

        std::wstring action = newEnabled ? L"Enabled" : L"Disabled";
        AppendLog(L"[*] " + action + L": " + name);
    }
}

void MainWindow::OnCopyLogSelection() {
    SendMessageW(hwndLogEdit_, WM_COPY, 0, 0);
}

void MainWindow::OnSelectAllLog() {
    SendMessageW(hwndLogEdit_, EM_SETSEL, 0, -1);
}

void MainWindow::OnClearLog() {
    // Discard any pending lines
    {
        CSLockGuard lock(pendingLogCs_);
        pendingLogLines_.clear();
    }

    // Clear RichEdit and reset state
    SetWindowTextW(hwndLogEdit_, L"");
    richEditLineCount_ = 0;
    autoScroll_ = true;

    AppendLog(L"Log display cleared");

    PostMessage(hwnd_, WM_APP_UPDATE_SCROLLBAR, 0, 0);
}

void MainWindow::OnOpenLogFile() {
    std::wstring logPath = baseDir_ + L"\\" + LOG_FILENAME;

    // Open explorer with file selected
    std::wstring param = L"/select,\"" + logPath + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe",
        param.c_str(), nullptr, SW_SHOWNORMAL);
}

void MainWindow::OnToggleLogEnabled() {
    // Get current checkbox state
    LRESULT checkState = SendMessageW(hwndLogToggle_, BM_GETCHECK, 0, 0);
    bool enabled = (checkState == BST_CHECKED);

    // Update config
    UnLeafConfig::Instance().SetLogEnabled(enabled);
    UnLeafConfig::Instance().Save();

    // Update logger
    LightweightLogger::Instance().SetEnabled(enabled);

    // Notify service via IPC
    if (currentState_ == ServiceState::Running) {
        char enabledByte = enabled ? '\x01' : '\x00';
        std::string data(1, enabledByte);
        ipcClient_.SendCommand(IPCCommand::CMD_SET_LOG_ENABLED, data);
    }

    // Log the change (this log will be displayed in Manager UI only if logging is enabled)
    if (enabled) {
        AppendLog(L"[Manager] Log output: Enabled");
    } else {
        AppendLog(L"[Manager] Log output: Disabled (Manager UI only)");
    }
}

} // namespace unleaf
