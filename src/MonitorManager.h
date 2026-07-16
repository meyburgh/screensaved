#pragma once

#include "Monitor.hpp"
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

class MonitorManager {
public:
    MonitorManager();
    ~MonitorManager();

    // Discover / rediscover monitors. Thread-safe.
    void EnumerateMonitors();

    // Called from input hook when the active monitor changes.
    void OnMonitorActivated(HMONITOR hNewActive);

    // Called from input hook when a mouse button is pressed on a monitor.
    // Snaps brightness and contrast to ceiling without fade interpolation.
    void OnMonitorClicked(HMONITOR hMon);

    // Called from input hook on any real mouse activity (movement, buttons,
    // wheel). Refreshes the mouse-idle timestamp and, if the active monitor
    // was faded by mouse-idle, restores it.
    void OnMouseActivity();

    // Called from input hook on keyboard activity, with the monitor hosting
    // the focused (foreground) window. Counts as activity on that monitor:
    // postpones or reverses its fade on the next worker tick. Lock-free —
    // safe to call from the low-level keyboard hook.
    void OnKeyboardActivity(HMONITOR hMon);

    // Update configuration live.
    void SetConfig(const AppConfig& cfg);
    AppConfig GetConfig() const;

    // Enable / disable dimming. When disabled, all monitors restore to normal.
    void SetEnabled(bool enabled);
    bool IsEnabled() const;

    // Start / stop the worker thread.
    void StartWorker();
    void StopWorker();

    // Apply saved ceiling values to all monitors immediately. Used on startup.
    void ApplyCeilings();

    // Called when the system reports a monitor power-on transition
    // (WM_POWERBROADCAST). Forces the worker to re-issue current targets so
    // hardware comes back in sync with our cached state after DPMS wake.
    void OnDisplayWake();

    // Called on WM_DISPLAYCHANGE. Triggers an immediate topology validation
    // pass on the next worker tick so monitor handles / bounds get refreshed.
    void OnDisplayChanged();

    // Set callback invoked when MonitorManager updates config (e.g. ceiling bump).
    using ConfigSavedCallback = std::function<void(const AppConfig&)>;
    void SetOnConfigChanged(ConfigSavedCallback cb);

    // Shutdown — restores brightness and cleans up.
    void Shutdown();

private:
    // Worker thread entry
    void WorkerLoop();

    // Advance brightness and contrast toward targets for all monitors. Called on worker thread.
    void UpdateMonitors();

    // Recompute dim floors and ceilings for a monitor based on config percents.
    void RecomputeDimFloors(MonitorDevice& mon);

    // Poll active monitor for user-driven brightness/contrast bumps above the
    // saved ceiling, and persist a new ceiling if found. Returns true if changed.
    bool PollForCeilingBump();

    // Periodic validation pass: reconciles active monitor with current cursor
    // position and triggers re-enumeration if the desktop topology changed.
    void ValidateTopology();

    // Periodic scan of top-level windows: flags monitors that are covered by
    // a full-screen window while an app holds the display awake (media
    // playback). Fading is suppressed on flagged monitors.
    void UpdateFullscreenMediaFlags();

    // Apply logarithmic fade-out curve: returns value for a given elapsed fraction [0,1].
    double LogFade(double startVal, double endVal, double t);

    // Software overlay fallback
    void CreateOverlay(MonitorDevice& mon);
    void UpdateOverlay(MonitorDevice& mon);
    void DestroyOverlays();

    mutable std::mutex              m_mutex;
    std::vector<MonitorDevice>      m_monitors;
    std::vector<PhysicalMonitorPtr> m_physicalHandles;  // ownership
    std::vector<OverlayWindow>      m_overlays;

    AppConfig                       m_config;
    HMONITOR                        m_activeMonitor = nullptr;

    // Worker
    std::thread                     m_workerThread;
    ScopedHandle                    m_workerWakeEvent;
    std::atomic<bool>               m_workerRunning{false};

    // Overlay window class
    bool                            m_overlayClassRegistered = false;

    // Callback used when ceiling bump updates m_config and we want to persist.
    ConfigSavedCallback             m_onConfigChanged;

    // Last time we polled hardware for user ceiling bumps.
    std::chrono::steady_clock::time_point m_lastCeilingPoll;

    // Last time we ran the topology validation pass.
    std::chrono::steady_clock::time_point m_lastTopologyCheck;

    // Last time we scanned for full-screen media windows.
    std::chrono::steady_clock::time_point m_lastFullscreenCheck;

    // Mouse-idle tracking for the active-monitor fade.
    std::atomic<ULONGLONG>          m_lastMouseActivityTick{0};
    std::atomic<bool>               m_activeIdleFaded{false};

    // Keyboard activity handoff from the hook thread to the worker.
    std::atomic<ULONGLONG>          m_lastKeyboardActivityTick{0};
    std::atomic<HMONITOR>           m_lastKeyboardMonitor{nullptr};
    ULONGLONG                       m_processedKeyboardTick = 0;  // worker-only
};
