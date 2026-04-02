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
    if (nCode >= 0 && s_instance && wParam == WM_MOUSEMOVE) {
        auto* mhs = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
        POINT pt = mhs->pt;

        HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        if (hMon != s_instance->m_lastMonitor) {
            s_instance->m_lastMonitor = hMon;
            s_instance->m_manager.OnMonitorActivated(hMon);
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

    // Message loop required for low-level hooks
    MSG msg;
    while (m_running) {
        BOOL ret = GetMessageW(&msg, nullptr, 0, 0);
        if (ret == 0 || ret == -1) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UnhookWindowsHookEx(hook);
    Logger::Info("Mouse hook uninstalled");
}
