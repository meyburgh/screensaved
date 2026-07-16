#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <functional>

#include "Monitor.hpp"

class TrayUI {
public:
    using ToggleCallback   = std::function<void()>;
    using ExitCallback     = std::function<void()>;
    using SettingsCallback = std::function<void(const AppConfig&)>;
    using GetConfigCallback = std::function<AppConfig()>;
    using DisplayWakeCallback   = std::function<void()>;
    using DisplayChangeCallback = std::function<void()>;

    TrayUI();
    ~TrayUI();

    // Initialize the tray icon window. Returns false on failure.
    bool Init(HINSTANCE hInst,
              ToggleCallback onToggle,
              ExitCallback onExit,
              SettingsCallback onSettingsSave,
              GetConfigCallback getConfig,
              DisplayWakeCallback onDisplayWake = nullptr,
              DisplayChangeCallback onDisplayChange = nullptr);

    // Update the icon to reflect enabled/disabled state.
    void SetEnabled(bool enabled);

    // Run the message loop. Blocks until WM_QUIT.
    void RunMessageLoop();

    // Post WM_QUIT to exit the message loop.
    void Quit();

    // Cleanup tray icon.
    void Shutdown();

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    void ShowContextMenu();
    void ShowSettingsDialog();
    void CreateTrayIcons();

    static INT_PTR CALLBACK SettingsDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static bool PersistSettingsFromDialog(HWND hwnd);

    HWND            m_hwnd     = nullptr;
    HINSTANCE       m_hInst    = nullptr;
    HICON           m_iconEnabled  = nullptr;
    HICON           m_iconDisabled = nullptr;
    bool            m_enabled  = true;
    NOTIFYICONDATAW m_nid      = {};

    ToggleCallback        m_onToggle;
    ExitCallback          m_onExit;
    SettingsCallback      m_onSettingsSave;
    GetConfigCallback     m_getConfig;
    DisplayWakeCallback   m_onDisplayWake;
    DisplayChangeCallback m_onDisplayChange;

    HWND              m_settingsDlg = nullptr;
    HPOWERNOTIFY      m_powerNotify = nullptr;

    static TrayUI* s_instance;
};
