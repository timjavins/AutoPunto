#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <commctrl.h>
#include <cstdio>
#include <ctime>
#include <string>
#include <sstream>
#include <random>
#include <atomic>
#include <mutex>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")

// --------------------------------------------------
// CONFIGURATION
// --------------------------------------------------
static std::atomic<int>  g_CPS{12};
static double g_RandomFactor  = 0.20;

static std::atomic<bool> g_ToggleMode{false}; // F13
static std::atomic<bool> g_HoldMode{false};   // F14

static HWND   g_hMainWnd      = nullptr; // GUI with slider
static HWND   g_hTrayWnd      = nullptr; // same as main
static HWND   g_hOverlayWnd   = nullptr; // [ / ] overlay

static HINSTANCE g_hInst      = nullptr;

static std::mt19937_64 g_Rng((unsigned)time(nullptr));

static LARGE_INTEGER g_Freq   = {0};
static double        g_NextClickTime = 0.0; // in seconds

// For overlay char
static wchar_t g_OverlayChar = L'[';

// Worker thread control
static HANDLE g_hClickThread = nullptr;
static std::atomic<bool> g_Running{true};

// Tray icon ID
#define WM_TRAYICON (WM_APP + 1)
#define ID_TRAYICON 1001

// Hotkey IDs
#define ID_HOTKEY_F13  2001
#define ID_HOTKEY_F14  2002

// Slider control ID
#define ID_SLIDER_CPS  3001
#define ID_LABEL_CPS   3002
#define ID_LABEL_STATUS 3003

// Label handles
static HWND g_hLabelCPS    = nullptr;
static HWND g_hLabelStatus = nullptr;

// Log mutex
static std::mutex g_LogMutex;

// --------------------------------------------------
// Logging helper
// --------------------------------------------------
void Log(const std::string& msg) {
    std::lock_guard<std::mutex> lock(g_LogMutex);
    
    char timeBuf[64];
    std::time_t t = std::time(nullptr);
    std::tm tm;
    localtime_s(&tm, &t);
    std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &tm);

    std::ostringstream oss;
    oss << timeBuf << " - " << msg << "\r\n";

    HANDLE hFile = CreateFileA("debug_log.txt", GENERIC_WRITE, FILE_SHARE_READ,
                               nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
        return;

    SetFilePointer(hFile, 0, nullptr, FILE_END);
    DWORD written = 0;
    std::string out = oss.str();
    WriteFile(hFile, out.c_str(), (DWORD)out.size(), &written, nullptr);
    CloseHandle(hFile);
}

// --------------------------------------------------
// Time helpers
// --------------------------------------------------
double NowSeconds() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)g_Freq.QuadPart;
}

// --------------------------------------------------
// Fullscreen detection (approximate, like your AHK logic)
// --------------------------------------------------
bool IsFullscreenForeground() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;

    RECT r;
    if (!GetWindowRect(fg, &r))
        return false;

    HMONITOR hMon = MonitorFromWindow(fg, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfo(hMon, &mi))
        return false;

    int w = r.right - r.left;
    int h = r.bottom - r.top;
    int mw = mi.rcMonitor.right - mi.rcMonitor.left;
    int mh = mi.rcMonitor.bottom - mi.rcMonitor.top;

    const int tolerance = 4;
    bool fullscreen = (w >= mw - tolerance && h >= mh - tolerance);

    return fullscreen;
}

// --------------------------------------------------
// Overlay window: shows [ or ] at top center
// --------------------------------------------------
LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        SetBkMode(hdc, TRANSPARENT);
        HBRUSH hBrush = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(hdc, &rc, hBrush);
        DeleteObject(hBrush);
        SetTextColor(hdc, RGB(0, 255, 0));
        HFONT hFont = CreateFontW(20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                  CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                                  DEFAULT_PITCH | FF_DONTCARE, L"Consolas");
        HFONT hOld = (HFONT)SelectObject(hdc, hFont);
        wchar_t buf[2] = { g_OverlayChar, 0 };
        DrawTextW(hdc, buf, 1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, hOld);
        DeleteObject(hFont);
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

void CreateOverlayWindow(HWND hParent) {
    if (g_hOverlayWnd) return;

    if (!g_OverlayClassRegistered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc   = OverlayWndProc;
        wc.hInstance     = g_hInst;
        wc.lpszClassName = L"ClickOverlayClass";
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        if (RegisterClassW(&wc)) {
            g_OverlayClassRegistered = true;
        }
    }

    int width  = 40;
    int height = 40;

    // Top-center of primary monitor
    RECT rc;
    SystemParametersInfo(SPI_GETWORKAREA, 0, &rc, 0);
    int x = (rc.right - rc.left) / 2 - width / 2;
    int y = rc.top + 40;

    g_hOverlayWnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        wc.lpszClassName,
        L"",
        WS_POPUP,
        x, y, width, height,
        hParent,
        nullptr,
        g_hInst,
        nullptr
    );

    // Don't show initially - only show when clicking
}

void ShowClickOverlay() {
    if (!g_hOverlayWnd && g_hMainWnd)
        CreateOverlayWindow(g_hMainWnd);

    g_OverlayChar = (g_OverlayChar == L'[') ? L']' : L'[';
    if (g_hOverlayWnd) {
        InvalidateRect(g_hOverlayWnd, nullptr, TRUE);
        UpdateWindow(g_hOverlayWnd);
    }
}

// --------------------------------------------------
// Mouse click using SendInput
// --------------------------------------------------
void SendLeftClick() {
    INPUT inputs[2] = {};
    inputs[0].type = INPUT_MOUSE;
    inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;

    inputs[1].type = INPUT_MOUSE;
    inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;

    SendInput(2, inputs, sizeof(INPUT));
}

// --------------------------------------------------
// Click logic with CPS + jitter
// --------------------------------------------------
void MaybeClick() {
    double now = NowSeconds();
    if (now < g_NextClickTime)
        return;

    // Schedule next click
    int cps = g_CPS.load();
    double baseInterval = 1.0 / (double)cps;
    double jitterRange  = baseInterval * g_RandomFactor;

    std::uniform_real_distribution<double> dist(-jitterRange, jitterRange);
    double jitter = dist(g_Rng);
    double interval = baseInterval + jitter;
    if (interval < 0.0) interval = 0.0;

    g_NextClickTime = now + interval;

    // Perform click & overlay
    SendLeftClick();
    ShowClickOverlay();
}

// --------------------------------------------------
// Worker thread: runs high-frequency loop
// --------------------------------------------------
DWORD WINAPI ClickThreadProc(LPVOID) {
    Log("Click thread started");
    while (g_Running) {
        bool isFullscreen = IsFullscreenForeground();
        SHORT lmbState    = GetAsyncKeyState(VK_LBUTTON);

        if (g_ToggleMode) {
            if (isFullscreen && !(lmbState & 0x8000)) {
                MaybeClick();
            }
        }

        if (g_HoldMode) {
            if (isFullscreen && (lmbState & 0x8000)) {
                MaybeClick();
            }
        }

        Sleep(1); // high-frequency loop
    }
    Log("Click thread exiting");
    return 0;
}

// --------------------------------------------------
// Tray icon helpers
// --------------------------------------------------
void AddTrayIcon(HWND hwnd) {
    NOTIFYICONDATA nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = ID_TRAYICON;
    nid.uVersion = NOTIFYICON_VERSION_4;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wcscpy_s(nid.szTip, L"AutoClicker C++");
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    Shell_NotifyIcon(NIM_ADD, &nid);
}

void RemoveTrayIcon(HWND hwnd) {
    NOTIFYICONDATA nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = ID_TRAYICON;
    Shell_NotifyIcon(NIM_DELETE, &nid);
}

// --------------------------------------------------
// Helper to update CPS label
// --------------------------------------------------
void UpdateCPSLabel() {
    if (g_hLabelCPS) {
        wchar_t buf[32];
        swprintf_s(buf, L"CPS: %d", g_CPS);
        SetWindowTextW(g_hLabelCPS, buf);
    }
}

// --------------------------------------------------
// Helper to update status label
// --------------------------------------------------
void UpdateStatusLabel() {
    if (g_hLabelStatus) {
        const wchar_t* status = L"Mode: Idle";
        if (g_ToggleMode) status = L"Mode: Toggle (ON)";
        else if (g_HoldMode) status = L"Mode: Hold (ON)";
        SetWindowTextW(g_hLabelStatus, status);
    }

    // Show/hide overlay based on mode
    if (g_hOverlayWnd) {
        if (g_ToggleMode || g_HoldMode) {
            ShowWindow(g_hOverlayWnd, SW_SHOW);
        } else {
            ShowWindow(g_hOverlayWnd, SW_HIDE);
        }
    }
}

// --------------------------------------------------
// GUI (main window) with CPS slider
// --------------------------------------------------
LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        InitCommonControls();

        // CPS Label (above slider)
        g_hLabelCPS = CreateWindowExW(
            0, L"STATIC", L"CPS: 12",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            10, 10, 80, 20,
            hwnd, (HMENU)ID_LABEL_CPS, g_hInst, nullptr
        );

        HWND hSlider = CreateWindowExW(
            0, TRACKBAR_CLASSW, L"",
            WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
            10, 30, 260, 40,
            hwnd, (HMENU)ID_SLIDER_CPS, g_hInst, nullptr
        );

        SendMessage(hSlider, TBM_SETRANGE, TRUE, MAKELPARAM(1, 200));
        SendMessage(hSlider, TBM_SETPOS, TRUE, g_CPS);
        SendMessage(hSlider, TBM_SETTICFREQ, 10, 0);

        // Status Label (below slider)
        g_hLabelStatus = CreateWindowExW(
            0, L"STATIC", L"Mode: Idle",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            10, 75, 260, 20,
            hwnd, (HMENU)ID_LABEL_STATUS, g_hInst, nullptr
        );

        UpdateCPSLabel();
        UpdateStatusLabel();

        return 0;
    }

    case WM_HSCROLL: {
        if ((HWND)lParam && GetDlgCtrlID((HWND)lParam) == ID_SLIDER_CPS) {
            int pos = (int)SendMessage((HWND)lParam, TBM_GETPOS, 0, 0);
            if (pos < 1) pos = 1;
            g_CPS = pos;
            UpdateCPSLabel();
            std::ostringstream oss;
            oss << "CPS set to " << g_CPS;
            Log(oss.str());
        }
        return 0;
    }

    case WM_TRAYICON: {
        if (LOWORD(lParam) == WM_LBUTTONDBLCLK) {
            // Double-click tray icon -> show GUI
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
        }
        return 0;
    }

    case WM_HOTKEY: {
        if (wParam == ID_HOTKEY_F13) {
            g_ToggleMode = !g_ToggleMode;
            g_HoldMode = false;
            UpdateStatusLabel();
            std::string s = std::string("F13 toggle mode -> ") + (g_ToggleMode ? "ON" : "OFF");
            Log(s);
        } else if (wParam == ID_HOTKEY_F14) {
            g_HoldMode = !g_HoldMode;
            g_ToggleMode = false;
            UpdateStatusLabel();
            std::string s = std::string("F14 hold mode -> ") + (g_HoldMode ? "ON" : "OFF");
            Log(s);
        }
        return 0;
    }

    case WM_CLOSE:
        // X button closes the GUI and exits autoclicker
        PostQuitMessage(0);
        return 0;

    case WM_DESTROY:
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// --------------------------------------------------
// WinMain
// --------------------------------------------------
int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    g_hInst = hInstance;
    QueryPerformanceFrequency(&g_Freq);
    g_NextClickTime = NowSeconds();

    // Main window (hidden initially, shows GUI when tray icon dbl-clicked)
    const wchar_t CLASS_NAME[] = L"AutoClickerMainWndClass";

    WNDCLASSW wc = {};
    wc.lpfnWndProc   = MainWndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon         = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);

    RegisterClassW(&wc);

    g_hMainWnd = CreateWindowExW(
        0,
        CLASS_NAME,
        L"AutoClicker Control Panel",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 300, 150,
        nullptr,
        nullptr,
        hInstance,
        nullptr
    );

    if (!g_hMainWnd)
        return 0;

    g_hTrayWnd = g_hMainWnd;

    AddTrayIcon(g_hTrayWnd);

    // Register global hotkeys: F13 and F14
    // VK_F13 = 0x7C, VK_F14 = 0x7D
    if (!RegisterHotKey(g_hMainWnd, ID_HOTKEY_F13, 0, VK_F13)) {
        Log("Failed to register F13 hotkey");
    }
    if (!RegisterHotKey(g_hMainWnd, ID_HOTKEY_F14, 0, VK_F14)) {
        Log("Failed to register F14 hotkey");
    }

    // Create overlay (hidden logic handled inside)
    CreateOverlayWindow(g_hMainWnd);

    // Start click thread
    g_hClickThread = CreateThread(nullptr, 0, ClickThreadProc, nullptr, 0, nullptr);

    // Initially hide GUI; user opens it by double-clicking tray
    ShowWindow(g_hMainWnd, SW_HIDE);

    // Message loop
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // Cleanup
    g_Running = false;
    if (g_hClickThread) {
        WaitForSingleObject(g_hClickThread, 2000);
        CloseHandle(g_hClickThread);
    }

    UnregisterHotKey(g_hMainWnd, ID_HOTKEY_F13);
    UnregisterHotKey(g_hMainWnd, ID_HOTKEY_F14);
    RemoveTrayIcon(g_hTrayWnd);

    if (g_hOverlayWnd) {
        DestroyWindow(g_hOverlayWnd);
        g_hOverlayWnd = nullptr;
    }




}    return 0;    DestroyWindow(g_hMainWnd);
    return 0;
}
