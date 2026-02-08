// UnLeaf v7.90 - Main Window Implementation
// Python v1.00 Style Dashboard - Clean, Professional Dark Theme
// v7.90: Added auto-refresh on config file change

#include "main_window.h"
#include "resource.h"
#include "../common/logger.h"
#include <windowsx.h>
#include <commctrl.h>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <tlhelp32.h>
#include <uxtheme.h>

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
    , hwndLogToggle_(nullptr)      // v7.93
    , hFontTitle_(nullptr)
    , hFontNormal_(nullptr)
    , hFontSmall_(nullptr)
    , hFontMono_(nullptr)
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
    , logDisplayDirty_(false)
    , trayCreated_(false)
    , minimizedToTray_(false)
    , hoveredButton_(nullptr) {

    // Python v1.00 inspired dark color scheme
    clrBackground_ = RGB(25, 25, 30);      // Deep dark background
    clrPanel_ = RGB(35, 35, 42);           // Panel/card background
    clrText_ = RGB(220, 220, 225);         // Primary text
    clrTextDim_ = RGB(140, 140, 150);      // Secondary/dim text
    clrAccent_ = RGB(46, 204, 113);        // Green accent (same as v1.00)
    clrLogText_ = RGB(0, 255, 128);        // Bright terminal green for logs
    clrOnline_ = RGB(46, 204, 113);        // Online indicator
    clrOffline_ = RGB(231, 76, 60);        // Offline indicator
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
    if (hFontTitle_) DeleteObject(hFontTitle_);
    if (hFontNormal_) DeleteObject(hFontNormal_);
    if (hFontSmall_) DeleteObject(hFontSmall_);
    if (hFontMono_) DeleteObject(hFontMono_);
    if (hBrushBg_) DeleteObject(hBrushBg_);
    if (hBrushPanel_) DeleteObject(hBrushPanel_);
    if (hBrushButton_) DeleteObject(hBrushButton_);
    if (hBrushButtonHover_) DeleteObject(hBrushButtonHover_);
    if (hBrushLogBg_) DeleteObject(hBrushLogBg_);
}

bool MainWindow::Initialize(HINSTANCE hInstance) {
    hInstance_ = hInstance;
    baseDir_ = GetBaseDirectory();

    // Init common controls
    INITCOMMONCONTROLSEX icex = { sizeof(icex), ICC_WIN95_CLASSES | ICC_BAR_CLASSES };
    InitCommonControlsEx(&icex);

    // Init config
    UnLeafConfig::Instance().Initialize(baseDir_);

    // Create fonts - Yu Gothic UI family (slightly smaller for better fit with bilingual labels)
    hFontTitle_ = CreateFontW(-16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Yu Gothic UI");

    hFontNormal_ = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Yu Gothic UI");

    hFontSmall_ = CreateFontW(-10, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Yu Gothic UI");

    hFontMono_ = CreateFontW(-11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");

    // Create brushes
    hBrushBg_ = CreateSolidBrush(clrBackground_);
    hBrushPanel_ = CreateSolidBrush(clrPanel_);
    hBrushButton_ = CreateSolidBrush(clrButton_);
    hBrushButtonHover_ = CreateSolidBrush(clrButtonHover_);
    hBrushLogBg_ = CreateSolidBrush(RGB(18, 18, 22));

    // Register window class
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    hInstance_ = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = hBrushBg_;
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

    // Create window (fixed size, no resize)
    hwnd_ = CreateWindowExW(
        0, WND_CLASS, L"UnLeaf Manager",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        posX, posY, WIN_WIDTH, WIN_HEIGHT,
        nullptr, nullptr, hInstance, this
    );

    if (!hwnd_) return false;

    EnableDarkMode();
    CreateControls();
    CreateTrayIcon();

    // Initial state
    UpdateServiceStatus();
    RefreshTargetList();
    AppendLog(L"UnLeaf ダッシュボード起動完了");
    AppendLog(L"ログ監視パス: " + baseDir_ + L"\\" + LOG_FILENAME);

    // Create wake event for log watcher thread
    logWakeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // Start log watcher
    shutdownFlag_.store(false, std::memory_order_release);
    logThread_ = std::thread(&MainWindow::LogWatcherThread, this);

    // Status update timer (1 second)
    SetTimer(hwnd_, ID_TIMER, 1000, nullptr);

    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);

    return true;
}

void MainWindow::EnableDarkMode() {
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
}

int MainWindow::Run() {
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
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
        case ID_LOG_TOGGLE:  // v7.93: Log ON/OFF toggle
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
            UpdateEngineStatus();

            // v7.90: Auto-refresh target list on config file change
            if (UnLeafConfig::Instance().HasFileChanged()) {
                UnLeafConfig::Instance().Reload();
                RefreshTargetList();
            }
        }
        break;

/*
    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wParam;
        HWND hwndCtrl = (HWND)lParam;

        // Engine status uses accent color
        if (hwndCtrl == hwndEngineStatus_) {
            SetTextColor(hdc, clrAccent_);
            SetBkColor(hdc, clrPanel_);
            return (LRESULT)hBrushPanel_;
        }

        // Regular static controls
        SetTextColor(hdc, clrText_);
        SetBkMode(hdc, TRANSPARENT);
        return (LRESULT)hBrushBg_;
    }
*/

/*
    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wParam;
        HWND hwndCtrl = (HWND)lParam;

        // Apply dynamic colors to service status label based on current state
        if (hwndCtrl == hwndStatusLabel_) {
            SetBkMode(hdc, TRANSPARENT);
            switch (currentState_) {
                case ServiceState::Running:
                    SetTextColor(hdc, RGB(46, 204, 113)); break; // Spring Green
                case ServiceState::Stopped:
                    SetTextColor(hdc, RGB(231, 76, 60));  break; // Red
                case ServiceState::StartPending:
                case ServiceState::StopPending:
                    SetTextColor(hdc, RGB(255, 215, 0));  break; // Gold/Yellow
                case ServiceState::NotInstalled:
                    SetTextColor(hdc, RGB(140, 140, 150)); break; // Gray
                default:
                    SetTextColor(hdc, RGB(220, 220, 225)); break; // Default White
            }
            return (LRESULT)hBrushBg_;
        }

        // Engine status bar color
        if (hwndCtrl == hwndEngineStatus_) {
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, clrAccent_);
            return (LRESULT)hBrushBg_;
        }

        // Default style for other static labels
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, clrText_);
        return (LRESULT)hBrushBg_;
    }
*/

case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wParam;
        HWND hwndCtrl = (HWND)lParam;

        // Synchronize colors for both top and bottom status displays
        if (hwndCtrl == hwndStatusLabel_ || hwndCtrl == hwndEngineStatus_) {
            SetBkMode(hdc, TRANSPARENT);
            switch (currentState_) {
                case ServiceState::Running:
                    SetTextColor(hdc, RGB(46, 204, 113)); break; // Spring Green
                case ServiceState::Stopped:
                    SetTextColor(hdc, RGB(231, 76, 60));  break; // Red
                case ServiceState::StartPending:
                case ServiceState::StopPending:
                    SetTextColor(hdc, RGB(255, 215, 0));  break; // Gold/Yellow (Starting/Stopping)
                case ServiceState::NotInstalled:
                    SetTextColor(hdc, RGB(140, 140, 150)); break; // Dim Gray
                default:
                    SetTextColor(hdc, RGB(220, 220, 225)); break; // Default Text Color
            }
            return (LRESULT)hBrushBg_;
        }

        // v7.93: Log toggle checkbox - white text for visibility
        if (hwndCtrl == hwndLogToggle_) {
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, clrText_);  // White text
            return (LRESULT)hBrushBg_;
        }

        // Default style for target list and other static labels
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, clrText_);
        return (LRESULT)hBrushBg_;
    }

    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wParam;
        // Log edit - bright green terminal text
        SetTextColor(hdc, clrLogText_);
        SetBkColor(hdc, RGB(18, 18, 22));
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
        // Wait for thread with timeout (max 2 seconds)
        if (logThread_.joinable()) {
            // Detach thread if it takes too long - it will exit on its own
            std::thread::id tid = logThread_.get_id();
            logThread_.detach();
            // Give thread time to notice shutdown flag
            for (int i = 0; i < 40; ++i) {  // 2 seconds max
                Sleep(50);
                // Thread should have exited by now
            }
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
    }

    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

void MainWindow::CreateControls() {
    RECT clientRect;
    GetClientRect(hwnd_, &clientRect);
    int clientWidth = clientRect.right - clientRect.left;
    int clientHeight = clientRect.bottom - clientRect.top;

    int contentWidth = clientWidth - MARGIN * 2;
    int y = MARGIN;

    // === Header area (title drawn in OnPaint) ===
    y += 28;

    // === Service Status Row ===
    // Status label on the left (wider for bilingual text)
    hwndStatusLabel_ = CreateWindowW(L"STATIC", L"  Service: Checking...",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
        MARGIN, y, 160, 22, hwnd_, nullptr, hInstance_, nullptr);
    SendMessageW(hwndStatusLabel_, WM_SETFONT, (WPARAM)hFontNormal_, TRUE);

    // Service buttons (right-aligned) - wider for bilingual labels
    constexpr int SERVICE_BTN_WIDTH = 190;
    int btnX = MARGIN + contentWidth - SERVICE_BTN_WIDTH;
    hwndBtnStop_ = CreateWindowW(L"BUTTON", L"サービス登録解除 (Unregister)",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        btnX, y - 1, SERVICE_BTN_WIDTH, BTN_HEIGHT,
        hwnd_, (HMENU)(UINT_PTR)ID_BTN_STOP, hInstance_, nullptr);

    btnX -= SERVICE_BTN_WIDTH + 6;
    hwndBtnStart_ = CreateWindowW(L"BUTTON", L"サービス登録・実行 (Register & Run)",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        btnX, y - 1, SERVICE_BTN_WIDTH, BTN_HEIGHT,
        hwnd_, (HMENU)(UINT_PTR)ID_BTN_START, hInstance_, nullptr);

    y += 30;

    // === Target Processes Section ===
    CreateWindowW(L"STATIC", L"対象プロセス (Targets):",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
        MARGIN, y, 150, 28, hwnd_, nullptr, hInstance_, nullptr);

    // Target buttons (right-aligned)
    btnX = MARGIN + contentWidth - SMALL_BTN_WIDTH;
    hwndBtnRemove_ = CreateWindowW(L"BUTTON", L"削除",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        btnX, y - 1, SMALL_BTN_WIDTH, 22,
        hwnd_, (HMENU)(UINT_PTR)ID_BTN_REMOVE, hInstance_, nullptr);

    btnX -= SMALL_BTN_WIDTH + 4;
    hwndBtnSelect_ = CreateWindowW(L"BUTTON", L"選択",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        btnX, y - 1, SMALL_BTN_WIDTH, 22,
        hwnd_, (HMENU)(UINT_PTR)ID_BTN_SELECT, hInstance_, nullptr);

    btnX -= SMALL_BTN_WIDTH + 4;
    hwndBtnAdd_ = CreateWindowW(L"BUTTON", L"追加",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        btnX, y - 1, SMALL_BTN_WIDTH, 22,
        hwnd_, (HMENU)(UINT_PTR)ID_BTN_ADD, hInstance_, nullptr);

    y += 26;

    // Target listbox
    hwndTargetList_ = CreateWindowExW(0, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_BORDER | LBS_NOTIFY,
        MARGIN, y, contentWidth, 100,
        hwnd_, (HMENU)(UINT_PTR)ID_LIST, hInstance_, nullptr);
    SendMessageW(hwndTargetList_, WM_SETFONT, (WPARAM)hFontNormal_, TRUE);

    y += 100;

    // === Live Log Section ===
    CreateWindowW(L"STATIC", L"ライブログ (Live Log):",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        MARGIN, y, 150, 16, hwnd_, nullptr, hInstance_, nullptr);

    // v7.93: Log ON/OFF toggle checkbox (right side of header)
    hwndLogToggle_ = CreateWindowW(L"BUTTON", L"Log Output",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        MARGIN + contentWidth - 90, y - 2, 90, 20,
        hwnd_, (HMENU)(UINT_PTR)ID_LOG_TOGGLE, hInstance_, nullptr);
    SendMessageW(hwndLogToggle_, WM_SETFONT, (WPARAM)hFontSmall_, TRUE);
    SetWindowTheme(hwndLogToggle_, L"", L"");  // Disable visual styles to allow custom text color
    // Set initial checked state based on config
    SendMessageW(hwndLogToggle_, BM_SETCHECK,
                 UnLeafConfig::Instance().IsLogEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);

    y += 20;

    // Log edit area - calculate remaining height minus status bar
    int statusBarHeight = 24;
    int logHeight = clientHeight - y - MARGIN - statusBarHeight - 4;
    if (logHeight < 80) logHeight = 80;

    // v7.93: No horizontal scrollbar (ES_AUTOHSCROLL prevents line wrap)
    hwndLogEdit_ = CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_BORDER | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
        MARGIN, y, contentWidth, logHeight,
        hwnd_, (HMENU)(UINT_PTR)ID_LOG, hInstance_, nullptr);
    SendMessageW(hwndLogEdit_, WM_SETFONT, (WPARAM)hFontMono_, TRUE);

    y += logHeight + 4;

    // === Engine Status Bar (bottom) ===
    hwndEngineStatus_ = CreateWindowW(L"STATIC", L"Active: 0 processes  |  Engine: --",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
        MARGIN, y, contentWidth, statusBarHeight,
        hwnd_, (HMENU)(UINT_PTR)ID_ENGINE_STATUS, hInstance_, nullptr);
    SendMessageW(hwndEngineStatus_, WM_SETFONT, (WPARAM)hFontSmall_, TRUE);

    // Apply fonts to all static controls
    EnumChildWindows(hwnd_, [](HWND hwnd, LPARAM lParam) -> BOOL {
        wchar_t className[32];
        GetClassNameW(hwnd, className, 32);
        if (wcscmp(className, L"Static") == 0) {
            auto self = reinterpret_cast<MainWindow*>(lParam);
            SendMessageW(hwnd, WM_SETFONT, (WPARAM)self->hFontNormal_, TRUE);
        }
        return TRUE;
    }, (LPARAM)this);

    // Override small font for engine status
    SendMessageW(hwndEngineStatus_, WM_SETFONT, (WPARAM)hFontSmall_, TRUE);
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
    SelectObject(hdc, hFontSmall_);
    DrawTextW(hdc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void MainWindow::OnPaint() {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd_, &ps);

    RECT rc;
    GetClientRect(hwnd_, &rc);
    FillRect(hdc, &rc, hBrushBg_);

    // Draw title
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, clrText_);
    SelectObject(hdc, hFontTitle_);
    TextOutW(hdc, MARGIN, MARGIN-1, L"UnLeaf Manager", 14);

    // Version in accent color
    SetTextColor(hdc, clrAccent_);
    SelectObject(hdc, hFontSmall_);
    std::wstring verStr = std::wstring(L"v") + unleaf::VERSION;
    TextOutW(hdc, MARGIN + 132, MARGIN + 2, verStr.c_str(), static_cast<int>(verStr.length()));

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

/*
void MainWindow::UpdateServiceStatus() {
    ServiceState state = serviceCtrl_.GetServiceState();
    currentState_ = state;

    std::wstring statusText = L"  Service: ";
    switch (state) {
    case ServiceState::Running:
        statusText += L"RUNNING";
        break;
    case ServiceState::Stopped:
        statusText += L"STOPPED";
        break;
    case ServiceState::NotInstalled:
        statusText += L"NOT INSTALLED";
        break;
    case ServiceState::StartPending:
        statusText += L"STARTING...";
        break;
    case ServiceState::StopPending:
        statusText += L"STOPPING...";
        break;
    default:
        statusText += L"UNKNOWN";
        break;
    }

    SetWindowTextW(hwndStatusLabel_, statusText.c_str());

    // Start button: disabled when running or pending
    EnableWindow(hwndBtnStart_, state != ServiceState::Running &&
                                 state != ServiceState::StartPending &&
                                 state != ServiceState::StopPending);

    // Stop/Unregister button: enabled when service is installed (any state except NotInstalled/Unknown)
    EnableWindow(hwndBtnStop_, state != ServiceState::NotInstalled &&
                                state != ServiceState::Unknown &&
                                state != ServiceState::StopPending);

    // Repaint status indicator area
    RECT rc = { MARGIN, MARGIN + 28, MARGIN + 20, MARGIN + 48 };
    InvalidateRect(hwnd_, &rc, TRUE);
}
*/

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
    UpdateEngineStatus(); // Explicitly trigger bottom bar update to match the new state

    // Update button states based on service availability
    EnableWindow(hwndBtnStart_, state == ServiceState::Stopped || state == ServiceState::NotInstalled);
    EnableWindow(hwndBtnStop_, state != ServiceState::NotInstalled && state != ServiceState::Unknown);

    // Refresh the status indicator icon (if drawn in OnPaint)
    // RECT rc = { MARGIN, 20, MARGIN + 200, 60 };
    // InvalidateRect(hwnd_, &rc, TRUE);
}

/*
void MainWindow::UpdateEngineStatus() {
    // Build status string
    std::wstringstream ss;
    ss << L"Active: " << activeProcessCount_.load() << L" processes";

    if (currentState_ == ServiceState::Running) {
        ss << L"  |  Engine: Online (ETW)";
    } else {
        ss << L"  |  Engine: Offline";
    }

    SetWindowTextW(hwndEngineStatus_, ss.str().c_str());
}
*/

void MainWindow::UpdateEngineStatus() {
    // Return to default if service is not running
    if (currentState_ != ServiceState::Running) {
        SetWindowTextW(hwndEngineStatus_, L"Active: 0 processes  |  Engine: Offline");
        InvalidateRect(hwndEngineStatus_, nullptr, TRUE); // Trigger immediate color update
        return;
    }

    // Request actual process count from service via IPC
    auto response = ipcClient_.SendCommand(IPCCommand::CMD_GET_STATS);
    if (response && response->size() >= sizeof(uint32_t)) {
        uint32_t actualCount = *reinterpret_cast<const uint32_t*>(response->data());
        
        std::wstringstream ss;
        ss << L"Active: " << actualCount << L" processes  |  Engine: Online (ETW)";
        SetWindowTextW(hwndEngineStatus_, ss.str().c_str());
    } else {
        SetWindowTextW(hwndEngineStatus_, L"Active: -- processes  |  Engine: Online (Comm Error)");
    }

    // Force redraw to apply color changes in WM_CTLCOLORSTATIC immediately
    InvalidateRect(hwndEngineStatus_, nullptr, TRUE);
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

    // Add to buffer
    logLineBuffer_.push_back(cleanMsg);

    // Remove oldest lines if over limit
    while (logLineBuffer_.size() > MAX_LOG_LINES) {
        logLineBuffer_.pop_front();
    }

    // Mark display as dirty (will be refreshed on next WM_LOG_REFRESH)
    logDisplayDirty_.store(true, std::memory_order_release);
}

void MainWindow::ProcessNewLogLines(const std::vector<std::wstring>& newLines) {
    if (newLines.empty()) return;

    for (const auto& line : newLines) {
        if (!line.empty()) {
            logLineBuffer_.push_back(line);
        }
    }

    // Remove oldest lines if over limit
    while (logLineBuffer_.size() > MAX_LOG_LINES) {
        logLineBuffer_.pop_front();
    }

    logDisplayDirty_.store(true, std::memory_order_release);
}

void MainWindow::RefreshLogDisplay() {
    if (!hwndLogEdit_) return;
    if (!logDisplayDirty_.load(std::memory_order_acquire)) return;

    logDisplayDirty_.store(false, std::memory_order_release);

    // Check if user is near the bottom
    SCROLLINFO si = { sizeof(si), SIF_ALL };
    GetScrollInfo(hwndLogEdit_, SB_VERT, &si);
    bool nearBottom = (si.nMax == 0) || (si.nPos + static_cast<int>(si.nPage) + 2 >= si.nMax);

    // Disable redraw for performance
    SendMessageW(hwndLogEdit_, WM_SETREDRAW, FALSE, 0);

    // Build complete text from buffer
    std::wstring fullText;
    fullText.reserve(logLineBuffer_.size() * 80);  // Estimate ~80 chars per line

    for (const auto& line : logLineBuffer_) {
        fullText += line;
        fullText += L"\r\n";
    }

    // Set entire text at once
    SetWindowTextW(hwndLogEdit_, fullText.c_str());

    // Re-enable redraw
    SendMessageW(hwndLogEdit_, WM_SETREDRAW, TRUE, 0);

    // Auto-scroll if user was near bottom
    if (nearBottom) {
        int len = GetWindowTextLengthW(hwndLogEdit_);
        SendMessageW(hwndLogEdit_, EM_SETSEL, len, len);
        SendMessageW(hwndLogEdit_, EM_SCROLLCARET, 0, 0);
    }

    // Force repaint
    InvalidateRect(hwndLogEdit_, nullptr, TRUE);
}

void MainWindow::OnStartService() {
    if (!ServiceController::IsRunningAsAdmin()) {
        int result = MessageBoxW(hwnd_,
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
        AppendLog(L"サービスは既に実行中です");
        MessageBoxW(hwnd_, L"サービスは既に実行中です。",
            L"サービス登録・実行", MB_OK | MB_ICONINFORMATION);
        return;
    }

    // Starting - wait for it
    if (state == ServiceState::StartPending) {
        AppendLog(L"サービスは起動処理中です...");
        return;
    }

    std::wstring exePath = baseDir_ + L"\\UnLeaf_Service.exe";

    // Step 2: Install service if not exists
    if (state == ServiceState::NotInstalled) {
        AppendLog(L"サービスを登録中...");
        if (serviceCtrl_.InstallService(exePath)) {
            AppendLog(L"サービス登録完了");
        } else {
            AppendLog(L"エラー: " + serviceCtrl_.GetLastError());
            MessageBoxW(hwnd_, (L"サービス登録に失敗しました:\n" + serviceCtrl_.GetLastError()).c_str(),
                L"エラー", MB_OK | MB_ICONERROR);
            return;
        }
    }

    // Step 3: Start service if stopped
    AppendLog(L"サービスを開始中...");
    if (serviceCtrl_.StartService()) {
        AppendLog(L"サービス開始コマンド送信完了");
    } else {
        AppendLog(L"エラー: " + serviceCtrl_.GetLastError());
        MessageBoxW(hwnd_, (L"サービス開始に失敗しました:\n" + serviceCtrl_.GetLastError()).c_str(),
            L"エラー", MB_OK | MB_ICONERROR);
    }

    UpdateServiceStatus();
}

void MainWindow::OnStopService() {
    if (!ServiceController::IsRunningAsAdmin()) {
        int result = MessageBoxW(hwnd_,
            L"管理者権限が必要です。\n\n管理者として再起動しますか？",
            L"権限が必要", MB_YESNO | MB_ICONQUESTION);
        if (result == IDYES) {
            ServiceController::RestartAsAdmin();
            PostQuitMessage(0);
        }
        return;
    }

    // Confirm unregistration
    int result = MessageBoxW(hwnd_,
        L"サービスを停止し、登録を解除します。\n\nよろしいですか？",
        L"サービス登録解除", MB_YESNO | MB_ICONQUESTION);
    if (result != IDYES) {
        return;
    }

    AppendLog(L"サービス登録解除を開始...");

    // Use the unified UninstallService which handles stop + wait + delete
    if (serviceCtrl_.UninstallService()) {
        AppendLog(L"サービス登録解除完了");
        MessageBoxW(hwnd_, L"サービスの登録解除が完了しました。",
            L"サービス登録解除", MB_OK | MB_ICONINFORMATION);
    } else {
        AppendLog(L"エラー: " + serviceCtrl_.GetLastError());
        MessageBoxW(hwnd_, (L"サービス登録解除に失敗しました:\n" + serviceCtrl_.GetLastError()).c_str(),
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
    SendMessageW(label, WM_SETFONT, (WPARAM)hFontNormal_, TRUE);

    HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        15, 40, 305, 24, dlg, nullptr, hInstance_, nullptr);
    SendMessageW(edit, WM_SETFONT, (WPARAM)hFontNormal_, TRUE);

    HWND btnOk = CreateWindowW(L"BUTTON", L"追加",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        160, 75, 75, 28, dlg, (HMENU)IDOK, hInstance_, nullptr);
    SendMessageW(btnOk, WM_SETFONT, (WPARAM)hFontNormal_, TRUE);

    HWND btnCancel = CreateWindowW(L"BUTTON", L"キャンセル",
        WS_CHILD | WS_VISIBLE,
        245, 75, 75, 28, dlg, (HMENU)IDCANCEL, hInstance_, nullptr);
    SendMessageW(btnCancel, WM_SETFONT, (WPARAM)hFontNormal_, TRUE);

    SetFocus(edit);
    EnableWindow(hwnd_, FALSE);

    MSG msg;
    bool done = false;
    bool cancelled = false;
    std::wstring result;

    while (!done && GetMessage(&msg, nullptr, 0, 0)) {
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
            MessageBoxW(hwnd_, L"追加に失敗しました（既に存在するか保護されています）",
                L"対象の追加", MB_OK | MB_ICONWARNING);
        }
    }
}

void MainWindow::OnRemoveTarget() {
    int sel = (int)SendMessageW(hwndTargetList_, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR) {
        MessageBoxW(hwnd_, L"対象を選択してください", L"削除", MB_OK | MB_ICONINFORMATION);
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
    SendMessageW(descLabel, WM_SETFONT, (WPARAM)hFontNormal_, TRUE);

    // Create ListBox for process enumeration (moved down for description)
    HWND list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_SORT,
        15, 40, 315, 300, dlg, nullptr, hInstance_, nullptr);
    SendMessageW(list, WM_SETFONT, (WPARAM)hFontNormal_, TRUE);

    // Populate process list
    for (const auto& p : processes) {
        SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)p.c_str());
    }

    // Create "追加" button (matching OnAddTarget style)
    HWND btnOk = CreateWindowW(L"BUTTON", L"追加",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        160, 355, 75, 28, dlg, (HMENU)IDOK, hInstance_, nullptr);
    SendMessageW(btnOk, WM_SETFONT, (WPARAM)hFontNormal_, TRUE);

    // Create "キャンセル" button (matching OnAddTarget style)
    HWND btnCancel = CreateWindowW(L"BUTTON", L"キャンセル",
        WS_CHILD | WS_VISIBLE,
        245, 355, 75, 28, dlg, (HMENU)IDCANCEL, hInstance_, nullptr);
    SendMessageW(btnCancel, WM_SETFONT, (WPARAM)hFontNormal_, TRUE);

    // Disable the main window while the modal dialog is open
    EnableWindow(hwnd_, FALSE);

    MSG msg;
    bool done = false;
    bool cancelled = false;
    std::wstring result;

    // Modal message loop
    while (!done && GetMessage(&msg, nullptr, 0, 0)) {
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

    auto lines = ReadTailLines(logPath, MAX_LOG_LINES);
    logLineBuffer_.clear();

    for (const auto& line : lines) {
        logLineBuffer_.push_back(Utf8ToWide(line));
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

    logDisplayDirty_.store(true, std::memory_order_release);
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
        if (!logLineBuffer_.empty()) {
            requestRefresh();
            initialLoadDone = true;
        }
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
                logLineBuffer_.push_back(L"[ログファイル未検出] サービスを起動してください");
                logDisplayDirty_.store(true, std::memory_order_release);
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

        std::wstring action = newEnabled ? L"有効化" : L"無効化";
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
    // Clear internal buffer
    logLineBuffer_.clear();
    logDisplayDirty_.store(true, std::memory_order_release);

    // Clear UI
    SetWindowTextW(hwndLogEdit_, L"");

    AppendLog(L"ログ表示をクリアしました");
}

void MainWindow::OnOpenLogFile() {
    std::wstring logPath = baseDir_ + L"\\" + LOG_FILENAME;

    // Open explorer with file selected
    std::wstring param = L"/select,\"" + logPath + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe",
        param.c_str(), nullptr, SW_SHOWNORMAL);
}

// v7.93: Toggle log enabled/disabled
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
        AppendLog(L"[Manager] ログ出力: 有効");
    } else {
        AppendLog(L"[Manager] ログ出力: 無効 (Manager UIのみ表示)");
    }
}

} // namespace unleaf
