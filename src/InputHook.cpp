#include "InputHook.h"
#include "Logger.h"

InputHook* InputHook::s_instance = nullptr;

InputHook::InputHook(MonitorManager& mgr)
    : m_manager(mgr) {
    s_instance = this;
}

InputHook::~InputHook() {
    Stop();
    s_instance = nullptr;
}

void InputHook::Start() {
    m_running = true;
    m_thread = std::thread(&InputHook::ThreadFunc, this);
    Logger::Info("Input hook thread starting");
}

void InputHook::Stop() {
    if (!m_running) return;
    m_running = false;

    // Post WM_QUIT to break the message loop
    if (m_threadId != 0) {
        PostThreadMessageW(m_threadId, WM_QUIT, 0, 0);
    }

    if (m_thread.joinable())
        m_thread.join();

    Logger::Info("Input hook thread stopped");
}

LRESULT CALLBACK InputHook::MouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && s_instance) {
        auto* mhs = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
        POINT pt = mhs->pt;

        if (wParam == WM_MOUSEMOVE) {
            // Windows re-posts WM_MOUSEMOVE without actual movement (e.g. on
            // window redraws) — only count position changes as activity.
            if (pt.x != s_instance->m_lastPoint.x || pt.y != s_instance->m_lastPoint.y) {
                s_instance->m_lastPoint = pt;
                s_instance->m_manager.OnMouseActivity();

                HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
                if (hMon != s_instance->m_lastMonitor) {
                    s_instance->m_lastMonitor = hMon;
                    s_instance->m_manager.OnMonitorActivated(hMon);
                }
            }
        } else if (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN ||
                   wParam == WM_MBUTTONDOWN || wParam == WM_XBUTTONDOWN) {
            s_instance->m_manager.OnMouseActivity();
            HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
            if (hMon) {
                s_instance->m_lastMonitor = hMon;
                s_instance->m_manager.OnMonitorClicked(hMon);
            }
        } else if (wParam == WM_MOUSEWHEEL || wParam == WM_MOUSEHWHEEL) {
            s_instance->m_manager.OnMouseActivity();
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

LRESULT CALLBACK InputHook::KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && s_instance &&
        (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
        // Throttle: one report per 200ms is plenty, and keeps the low-level
        // hook (which delays keystroke delivery system-wide) cheap.
        ULONGLONG tick = GetTickCount64();
        if (tick - s_instance->m_lastKeyboardTick >= 200) {
            s_instance->m_lastKeyboardTick = tick;
            // Keyboard input belongs to the monitor hosting the focused window.
            HWND fg = GetForegroundWindow();
            if (fg) {
                HMONITOR hMon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
                if (hMon) s_instance->m_manager.OnKeyboardActivity(hMon);
            }
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

void InputHook::ThreadFunc() {
    m_threadId = GetCurrentThreadId();

    // Install low-level mouse hook
    HHOOK hook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc,
                                    GetModuleHandleW(nullptr), 0);
    if (!hook) {
        Logger::Error("Failed to install mouse hook, error=%lu", GetLastError());
        return;
    }

    Logger::Info("Mouse hook installed");

    // Install low-level keyboard hook (keyboard activity keeps the focused
    // window's monitor awake). Non-fatal if it fails.
    HHOOK kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc,
                                     GetModuleHandleW(nullptr), 0);
    if (kbHook) {
        Logger::Info("Keyboard hook installed");
    } else {
        Logger::Error("Failed to install keyboard hook, error=%lu", GetLastError());
    }

    // Message loop required for low-level hooks
    MSG msg;
    while (m_running) {
        BOOL ret = GetMessageW(&msg, nullptr, 0, 0);
        if (ret == 0 || ret == -1) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (kbHook) {
        UnhookWindowsHookEx(kbHook);
        Logger::Info("Keyboard hook uninstalled");
    }
    UnhookWindowsHookEx(hook);
    Logger::Info("Mouse hook uninstalled");
}
