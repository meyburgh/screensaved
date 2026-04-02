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

    // Update configuration live.
    void SetConfig(const AppConfig& cfg);
    AppConfig GetConfig() const;

    // Enable / disable dimming. When disabled, all monitors restore to normal.
    void SetEnabled(bool enabled);
    bool IsEnabled() const;

    // Start / stop the worker thread.
    void StartWorker();
    void StopWorker();

    // Shutdown — restores brightness and cleans up.
    void Shutdown();

private:
    // Worker thread entry
    void WorkerLoop();

    // Advance brightness and contrast toward targets for all monitors. Called on worker thread.
    void UpdateMonitors();

    // Recompute dim floors for a monitor based on config percents.
    void RecomputeDimFloors(MonitorDevice& mon);

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
};
