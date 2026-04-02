#pragma once

#include "MonitorManager.h"

class InputHook {
public:
    explicit InputHook(MonitorManager& mgr);
    ~InputHook();

    // Start the input thread with the mouse hook. Non-blocking.
    void Start();

    // Stop the input thread and uninstall the hook.
    void Stop();

private:
    static LRESULT CALLBACK MouseProc(int nCode, WPARAM wParam, LPARAM lParam);
    void ThreadFunc();

    MonitorManager& m_manager;
    std::thread     m_thread;
    DWORD           m_threadId = 0;
    std::atomic<bool> m_running{false};
    HMONITOR        m_lastMonitor = nullptr;

    // Static instance pointer for the hook callback
    static InputHook* s_instance;
};
