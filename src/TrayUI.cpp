#include "TrayUI.h"
#include "resource.h"
#include "Logger.h"
#include "ConfigStore.h"

#include <shellapi.h>
#include <commctrl.h>

#pragma comment(lib, "comctl32.lib")

// GUID_MONITOR_POWER_ON, defined locally to avoid <initguid.h> include-order
// requirements. Notifies on monitor power transitions: Data byte 0 is
// 0=off, 1=on, 2=dimmed (Win10+).
static const GUID kGuidMonitorPowerOn =
    { 0x02731015, 0x4510, 0x4526, { 0x99, 0xE6, 0xE5, 0xA1, 0x7E, 0xBD, 0x1A, 0xEA } };

TrayUI* TrayUI::s_instance = nullptr;

// ---------------------------------------------------------------------------
// Programmatic icon creation (no .ico resources needed)
// ---------------------------------------------------------------------------

static HICON CreateCircleIcon(COLORREF color) {
    int size = GetSystemMetrics(SM_CXSMICON);
    if (size == 0) size = 16;

    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = size;
    bmi.bmiHeader.biHeight      = -size;
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP colorBmp = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HBITMAP maskBmp  = CreateBitmap(size, size, 1, 1, nullptr);

    SelectObject(memDC, colorBmp);
    HBRUSH bgBrush = CreateSolidBrush(RGB(0, 0, 0));
    RECT rc = {0, 0, size, size};
    FillRect(memDC, &rc, bgBrush);
    DeleteObject(bgBrush);

    HBRUSH circleBrush = CreateSolidBrush(color);
    HBRUSH oldBrush = (HBRUSH)SelectObject(memDC, circleBrush);
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HPEN oldPen = (HPEN)SelectObject(memDC, pen);
    Ellipse(memDC, 1, 1, size - 1, size - 1);
    SelectObject(memDC, oldBrush);
    SelectObject(memDC, oldPen);
    DeleteObject(circleBrush);
    DeleteObject(pen);

    HDC maskDC = CreateCompatibleDC(screenDC);
    SelectObject(maskDC, maskBmp);
    HBRUSH blackBrush = (HBRUSH)GetStockObject(BLACK_BRUSH);
    FillRect(maskDC, &rc, blackBrush);
    DeleteDC(maskDC);

    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);

    ICONINFO ii = {};
    ii.fIcon    = TRUE;
    ii.hbmMask  = maskBmp;
    ii.hbmColor = colorBmp;
    HICON hIcon = CreateIconIndirect(&ii);

    DeleteObject(colorBmp);
    DeleteObject(maskBmp);
    return hIcon;
}

// ---------------------------------------------------------------------------
// Settings dialog — control IDs
// ---------------------------------------------------------------------------
enum {
    IDC_HEADER_LINK         = 2001,
    IDC_IDLE_TIMEOUT        = 2002,
    IDC_BRIGHTNESS_FLOOR    = 2003,
    IDC_CONTRAST_FLOOR      = 2004,
    IDC_ENABLED_CHECK       = 2005,
    IDC_OK_BTN              = 2006,
    IDC_CANCEL_BTN          = 2007,
    IDC_STARTUP_CHECK       = 2008,
    IDC_B_FADE_OUT          = 2020,
    IDC_C_FADE_OUT          = 2022,
    IDC_FADE_IN             = 2024,
    IDC_BRIGHTNESS_CEILING  = 2026,
    IDC_CONTRAST_CEILING    = 2028,
    IDC_MOUSE_IDLE_FADE     = 2030,
};

// ---------------------------------------------------------------------------
// Helper: create a label + edit pair, returns edit HWND
// ---------------------------------------------------------------------------
static HWND CreateLabelEdit(HWND parent, HINSTANCE hInst, HFONT hFont,
                            const wchar_t* label, int ctrlId,
                            int x, int y, int labelW, int editW, int editH) {
    HWND hLabel = CreateWindowExW(0, L"STATIC", label,
        WS_CHILD | WS_VISIBLE,
        x, y + 2, labelW, editH,
        parent, nullptr, hInst, nullptr);
    SendMessageW(hLabel, WM_SETFONT, (WPARAM)hFont, TRUE);

    HWND hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER,
        x + labelW + 8, y, editW, editH,
        parent, (HMENU)(INT_PTR)ctrlId, hInst, nullptr);
    SendMessageW(hEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
    return hEdit;
}

// ---------------------------------------------------------------------------
// Settings dialog proc
// ---------------------------------------------------------------------------

bool TrayUI::PersistSettingsFromDialog(HWND hwnd) {
    if (!s_instance) return false;

    AppConfig cfg = s_instance->m_getConfig ? s_instance->m_getConfig() : AppConfig{};

    BOOL ok = FALSE;
    int val;

    val = GetDlgItemInt(hwnd, IDC_IDLE_TIMEOUT, &ok, FALSE);
    if (ok && val >= 1) cfg.idleTimeoutMs = val * 1000;

    val = GetDlgItemInt(hwnd, IDC_BRIGHTNESS_FLOOR, &ok, FALSE);
    if (ok && val >= 0 && val <= 100) cfg.dimBrightnessPercent = val;

    val = GetDlgItemInt(hwnd, IDC_CONTRAST_FLOOR, &ok, FALSE);
    if (ok && val >= 0 && val <= 100) cfg.dimContrastPercent = val;

    val = GetDlgItemInt(hwnd, IDC_BRIGHTNESS_CEILING, &ok, FALSE);
    if (ok && val >= 0 && val <= 100) cfg.maxBrightnessPercent = val;

    val = GetDlgItemInt(hwnd, IDC_CONTRAST_CEILING, &ok, FALSE);
    if (ok && val >= 0 && val <= 100) cfg.maxContrastPercent = val;

    if (cfg.dimBrightnessPercent > cfg.maxBrightnessPercent) {
        MessageBoxW(hwnd,
                    L"Brightness floor cannot be greater than brightness ceiling.",
                    L"ScreenSaved Settings",
                    MB_OK | MB_ICONWARNING);
        return false;
    }

    if (cfg.dimContrastPercent > cfg.maxContrastPercent) {
        MessageBoxW(hwnd,
                    L"Contrast floor cannot be greater than contrast ceiling.",
                    L"ScreenSaved Settings",
                    MB_OK | MB_ICONWARNING);
        return false;
    }

    val = GetDlgItemInt(hwnd, IDC_B_FADE_OUT, &ok, FALSE);
    if (ok && val >= 0) cfg.brightnessFadeOutDurationMs = val * 1000;

    val = GetDlgItemInt(hwnd, IDC_C_FADE_OUT, &ok, FALSE);
    if (ok && val >= 0) cfg.contrastFadeOutDurationMs = val * 1000;

    val = GetDlgItemInt(hwnd, IDC_FADE_IN, &ok, FALSE);
    if (ok && val >= 0) cfg.fadeInDurationMs = val;

    val = GetDlgItemInt(hwnd, IDC_MOUSE_IDLE_FADE, &ok, FALSE);
    if (ok && val >= 0) cfg.mouseIdleFadeMinutes = val;

    cfg.enabled = (IsDlgButtonChecked(hwnd, IDC_ENABLED_CHECK) == BST_CHECKED);

    bool wantStartup = (IsDlgButtonChecked(hwnd, IDC_STARTUP_CHECK) == BST_CHECKED);
    cfg.startWithWindows = wantStartup;
    ConfigStore::SetStartWithWindows(wantStartup);

    if (s_instance->m_onSettingsSave) {
        s_instance->m_onSettingsSave(cfg);
    }

    s_instance->SetEnabled(cfg.enabled);
    return true;
}

INT_PTR CALLBACK TrayUI::SettingsDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        if (!s_instance || !s_instance->m_getConfig) return TRUE;
        AppConfig cfg = s_instance->m_getConfig();

        SetDlgItemInt(hwnd, IDC_IDLE_TIMEOUT, cfg.idleTimeoutMs / 1000, FALSE);
        SetDlgItemInt(hwnd, IDC_BRIGHTNESS_FLOOR, cfg.dimBrightnessPercent, FALSE);
        SetDlgItemInt(hwnd, IDC_CONTRAST_FLOOR, cfg.dimContrastPercent, FALSE);
        SetDlgItemInt(hwnd, IDC_BRIGHTNESS_CEILING, cfg.maxBrightnessPercent, FALSE);
        SetDlgItemInt(hwnd, IDC_CONTRAST_CEILING, cfg.maxContrastPercent, FALSE);
        SetDlgItemInt(hwnd, IDC_B_FADE_OUT, cfg.brightnessFadeOutDurationMs / 1000, FALSE);
        SetDlgItemInt(hwnd, IDC_C_FADE_OUT, cfg.contrastFadeOutDurationMs / 1000, FALSE);
        SetDlgItemInt(hwnd, IDC_FADE_IN, cfg.fadeInDurationMs, FALSE);
        SetDlgItemInt(hwnd, IDC_MOUSE_IDLE_FADE, cfg.mouseIdleFadeMinutes, FALSE);
        CheckDlgButton(hwnd, IDC_ENABLED_CHECK, cfg.enabled ? BST_CHECKED : BST_UNCHECKED);

        // Startup checkbox reflects actual registry state
        bool startupEnabled = ConfigStore::IsStartWithWindowsEnabled();
        CheckDlgButton(hwnd, IDC_STARTUP_CHECK, startupEnabled ? BST_CHECKED : BST_UNCHECKED);
        return TRUE;
    }

    case WM_NOTIFY: {
        auto* nmhdr = reinterpret_cast<NMHDR*>(lp);
        if (nmhdr->idFrom == IDC_HEADER_LINK && nmhdr->code == NM_CLICK) {
            auto* link = reinterpret_cast<NMLINK*>(lp);
            ShellExecuteW(nullptr, L"open", link->item.szUrl, nullptr, nullptr, SW_SHOWNORMAL);
            return TRUE;
        }
        break;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_OK_BTN: {
            if (!PersistSettingsFromDialog(hwnd)) return TRUE;
            DestroyWindow(hwnd);
            s_instance->m_settingsDlg = nullptr;
            Logger::Info("Settings dialog confirmed");
            return TRUE;
        }
        case IDC_CANCEL_BTN:
        case IDCANCEL:
            if (!PersistSettingsFromDialog(hwnd)) return TRUE;
            DestroyWindow(hwnd);
            if (s_instance) s_instance->m_settingsDlg = nullptr;
            Logger::Info("Settings dialog closed");
            return TRUE;
        }
        break;

    case WM_CLOSE:
        if (!PersistSettingsFromDialog(hwnd)) return TRUE;
        DestroyWindow(hwnd);
        if (s_instance) s_instance->m_settingsDlg = nullptr;
        Logger::Info("Settings dialog closed");
        return TRUE;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// Build settings dialog programmatically
// ---------------------------------------------------------------------------

void TrayUI::ShowSettingsDialog() {
    if (m_settingsDlg) {
        SetForegroundWindow(m_settingsDlg);
        return;
    }

    // Ensure common controls are initialized for SysLink
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LINK_CLASS };
    InitCommonControlsEx(&icc);

    const int dlgW = 380, dlgH = 540;
    const int margin = 12;
    const int labelW = 190, editW = 80, editH = 22;
    const int rowH = 28;
    const int contentW = dlgW - margin * 2 - 16;

    HWND dlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        L"#32770",
        L"ScreenSaved Settings",
        WS_VISIBLE | WS_CAPTION | WS_SYSMENU | DS_CENTER,
        CW_USEDEFAULT, CW_USEDEFAULT, dlgW, dlgH,
        nullptr, nullptr, m_hInst, nullptr);

    if (!dlg) {
        Logger::Error("Failed to create settings dialog");
        return;
    }

    SetWindowLongPtrW(dlg, DWLP_DLGPROC, reinterpret_cast<LONG_PTR>(SettingsDlgProc));

    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    LOGFONTW lf = {};
    GetObjectW(hFont, sizeof(lf), &lf);
    lf.lfWeight = FW_BOLD;
    lf.lfHeight = -16;
    HFONT hBoldFont = CreateFontIndirectW(&lf);

    int y = margin;

    // --- Branded header with clickable hyperlink ---
    HWND hLink = CreateWindowExW(0, WC_LINK,
        L"ScreenSaved - by <a href=\"https://www.hypedriven.com\">HypeDriven.com</a>",
        WS_CHILD | WS_VISIBLE,
        margin, y, contentW, 28,
        dlg, (HMENU)(INT_PTR)IDC_HEADER_LINK, m_hInst, nullptr);
    SendMessageW(hLink, WM_SETFONT, (WPARAM)hBoldFont, TRUE);
    y += 36;

    // Separator
    CreateWindowExW(0, L"STATIC", nullptr,
        WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
        margin, y, contentW, 2,
        dlg, nullptr, m_hInst, nullptr);
    y += 12;

    // --- General settings ---
    CreateLabelEdit(dlg, m_hInst, hFont,
        L"Idle timeout (seconds):", IDC_IDLE_TIMEOUT,
        margin, y, labelW, editW, editH);
    y += rowH;

    CreateLabelEdit(dlg, m_hInst, hFont,
        L"Fade-in duration (ms):", IDC_FADE_IN,
        margin, y, labelW, editW, editH);
    y += rowH;

    CreateLabelEdit(dlg, m_hInst, hFont,
        L"Mouse idle fade (minutes, 0 = off):", IDC_MOUSE_IDLE_FADE,
        margin, y, labelW, editW, editH);
    y += rowH + 4;

    // --- Brightness section ---
    HWND hBrightLabel = CreateWindowExW(0, L"STATIC", L"Brightness",
        WS_CHILD | WS_VISIBLE,
        margin, y, contentW, editH,
        dlg, nullptr, m_hInst, nullptr);
    SendMessageW(hBrightLabel, WM_SETFONT, (WPARAM)hBoldFont, TRUE);
    y += rowH - 4;

    CreateLabelEdit(dlg, m_hInst, hFont,
        L"  Min floor (%):", IDC_BRIGHTNESS_FLOOR,
        margin, y, labelW, editW, editH);
    y += rowH;

    CreateLabelEdit(dlg, m_hInst, hFont,
        L"  Max ceiling (%):", IDC_BRIGHTNESS_CEILING,
        margin, y, labelW, editW, editH);
    y += rowH;

    CreateLabelEdit(dlg, m_hInst, hFont,
        L"  Fade-out duration (seconds):", IDC_B_FADE_OUT,
        margin, y, labelW, editW, editH);
    y += rowH + 4;

    // --- Contrast section ---
    HWND hContrastLabel = CreateWindowExW(0, L"STATIC", L"Contrast",
        WS_CHILD | WS_VISIBLE,
        margin, y, contentW, editH,
        dlg, nullptr, m_hInst, nullptr);
    SendMessageW(hContrastLabel, WM_SETFONT, (WPARAM)hBoldFont, TRUE);
    y += rowH - 4;

    CreateLabelEdit(dlg, m_hInst, hFont,
        L"  Min floor (%):", IDC_CONTRAST_FLOOR,
        margin, y, labelW, editW, editH);
    y += rowH;

    CreateLabelEdit(dlg, m_hInst, hFont,
        L"  Max ceiling (%):", IDC_CONTRAST_CEILING,
        margin, y, labelW, editW, editH);
    y += rowH;

    CreateLabelEdit(dlg, m_hInst, hFont,
        L"  Fade-out duration (seconds):", IDC_C_FADE_OUT,
        margin, y, labelW, editW, editH);
    y += rowH + 8;

    // --- Checkboxes ---
    HWND hCheck = CreateWindowExW(0, L"BUTTON", L"Enabled",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        margin, y, 200, editH,
        dlg, (HMENU)(INT_PTR)IDC_ENABLED_CHECK, m_hInst, nullptr);
    SendMessageW(hCheck, WM_SETFONT, (WPARAM)hFont, TRUE);
    y += rowH;

    HWND hStartup = CreateWindowExW(0, L"BUTTON", L"Start automatically at logon",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        margin, y, 250, editH,
        dlg, (HMENU)(INT_PTR)IDC_STARTUP_CHECK, m_hInst, nullptr);
    SendMessageW(hStartup, WM_SETFONT, (WPARAM)hFont, TRUE);
    y += rowH + 12;

    // --- OK / Cancel ---
    int btnW = 80, btnH = 28;
    int btnX = dlgW - margin - btnW * 2 - 8 - 16;

    HWND hOk = CreateWindowExW(0, L"BUTTON", L"OK",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        btnX, y, btnW, btnH,
        dlg, (HMENU)(INT_PTR)IDC_OK_BTN, m_hInst, nullptr);
    SendMessageW(hOk, WM_SETFONT, (WPARAM)hFont, TRUE);

    HWND hCancel = CreateWindowExW(0, L"BUTTON", L"Cancel",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        btnX + btnW + 8, y, btnW, btnH,
        dlg, (HMENU)(INT_PTR)IDC_CANCEL_BTN, m_hInst, nullptr);
    SendMessageW(hCancel, WM_SETFONT, (WPARAM)hFont, TRUE);

    m_settingsDlg = dlg;
    SendMessageW(dlg, WM_INITDIALOG, 0, 0);
    SetForegroundWindow(dlg);
    Logger::Info("Settings dialog opened");
}

// ---------------------------------------------------------------------------
// TrayUI implementation
// ---------------------------------------------------------------------------

TrayUI::TrayUI() {
    s_instance = this;
}

TrayUI::~TrayUI() {
    Shutdown();
    s_instance = nullptr;
}

void TrayUI::CreateTrayIcons() {
    m_iconEnabled  = CreateCircleIcon(RGB(0, 200, 80));
    m_iconDisabled = CreateCircleIcon(RGB(128, 128, 128));
}

bool TrayUI::Init(HINSTANCE hInst,
                   ToggleCallback onToggle,
                   ExitCallback onExit,
                   SettingsCallback onSettingsSave,
                   GetConfigCallback getConfig,
                   DisplayWakeCallback onDisplayWake,
                   DisplayChangeCallback onDisplayChange) {
    m_hInst            = hInst;
    m_onToggle         = onToggle;
    m_onExit           = onExit;
    m_onSettingsSave   = onSettingsSave;
    m_getConfig        = getConfig;
    m_onDisplayWake    = onDisplayWake;
    m_onDisplayChange  = onDisplayChange;

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance      = hInst;
    wc.lpszClassName  = L"ScreenSavedTray";
    RegisterClassExW(&wc);

    m_hwnd = CreateWindowExW(0, L"ScreenSavedTray", L"ScreenSaved",
                              0, 0, 0, 0, 0,
                              HWND_MESSAGE, nullptr, hInst, nullptr);
    if (!m_hwnd) {
        Logger::Error("Failed to create tray message window");
        return false;
    }

    CreateTrayIcons();

    m_nid.cbSize           = sizeof(m_nid);
    m_nid.hWnd             = m_hwnd;
    m_nid.uID              = 1;
    m_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    m_nid.uCallbackMessage = WM_TRAYICON;
    m_nid.hIcon            = m_iconEnabled;
    wcscpy_s(m_nid.szTip, L"ScreenSaved - Enabled");

    Shell_NotifyIconW(NIM_ADD, &m_nid);
    Logger::Info("Tray icon added");

    // Subscribe to monitor power transitions so we can re-push ceilings to
    // hardware after DPMS wake (otherwise the cached state and HW desync).
    m_powerNotify = RegisterPowerSettingNotification(
        m_hwnd, &kGuidMonitorPowerOn, DEVICE_NOTIFY_WINDOW_HANDLE);
    if (!m_powerNotify) {
        Logger::Warn("RegisterPowerSettingNotification failed, error=%lu",
                     GetLastError());
    }

    return true;
}

void TrayUI::SetEnabled(bool enabled) {
    m_enabled = enabled;
    m_nid.hIcon = enabled ? m_iconEnabled : m_iconDisabled;
    wcscpy_s(m_nid.szTip, enabled ? L"ScreenSaved - Enabled" : L"ScreenSaved - Disabled");
    m_nid.uFlags = NIF_ICON | NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &m_nid);

    Logger::Info("Tray icon updated: %s", enabled ? "enabled" : "disabled");
}

void TrayUI::RunMessageLoop() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (m_settingsDlg && IsDialogMessageW(m_settingsDlg, &msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void TrayUI::Quit() {
    PostQuitMessage(0);
}

void TrayUI::Shutdown() {
    if (m_powerNotify) {
        UnregisterPowerSettingNotification(m_powerNotify);
        m_powerNotify = nullptr;
    }
    if (m_settingsDlg) {
        DestroyWindow(m_settingsDlg);
        m_settingsDlg = nullptr;
    }
    if (m_nid.hWnd) {
        Shell_NotifyIconW(NIM_DELETE, &m_nid);
        m_nid.hWnd = nullptr;
    }
    if (m_hwnd) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    if (m_iconEnabled)  { DestroyIcon(m_iconEnabled);  m_iconEnabled = nullptr; }
    if (m_iconDisabled) { DestroyIcon(m_iconDisabled); m_iconDisabled = nullptr; }

    Logger::Info("TrayUI shutdown");
}

void TrayUI::ShowContextMenu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_SETTINGS, L"Settings");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(m_hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, m_hwnd, nullptr);
    PostMessageW(m_hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

LRESULT CALLBACK TrayUI::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (!s_instance) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_TRAYICON:
        if (lp == WM_LBUTTONDBLCLK) {
            if (s_instance->m_onToggle) s_instance->m_onToggle();
        } else if (lp == WM_RBUTTONUP) {
            s_instance->ShowContextMenu();
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_SETTINGS:
            s_instance->ShowSettingsDialog();
            break;
        case IDM_EXIT:
            if (s_instance->m_onExit) s_instance->m_onExit();
            break;
        }
        return 0;

    case WM_POWERBROADCAST:
        if (wp == PBT_POWERSETTINGCHANGE && lp) {
            auto* pbs = reinterpret_cast<POWERBROADCAST_SETTING*>(lp);
            if (pbs->PowerSetting == kGuidMonitorPowerOn &&
                pbs->DataLength >= sizeof(DWORD)) {
                DWORD state = *reinterpret_cast<const DWORD*>(pbs->Data);
                if (state == 1 && s_instance->m_onDisplayWake) {
                    s_instance->m_onDisplayWake();
                }
            }
        }
        return TRUE;

    case WM_DISPLAYCHANGE:
        if (s_instance->m_onDisplayChange) {
            s_instance->m_onDisplayChange();
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}
