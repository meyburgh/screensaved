#include "MonitorManager.h"
#include "DdcInterface.h"
#include "Logger.h"

#include <cmath>
#include <algorithm>

// ---------------------------------------------------------------------------
// Overlay window class name
// ---------------------------------------------------------------------------
static const wchar_t* kOverlayClass = L"ScreenSavedOverlay";

static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// EnumDisplayMonitors callback
// ---------------------------------------------------------------------------
struct EnumContext {
    std::vector<MonitorDevice>*      monitors;
    std::vector<PhysicalMonitorPtr>* handles;
};

static BOOL CALLBACK MonitorEnumProc(HMONITOR hMon, HDC, LPRECT, LPARAM dwData) {
    auto* ctx = reinterpret_cast<EnumContext*>(dwData);

    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hMon, &mi)) return TRUE;

    DWORD numPhysical = 0;
    if (!GetNumberOfPhysicalMonitorsFromHMONITOR(hMon, &numPhysical) || numPhysical == 0) {
        Logger::Warn("No physical monitors for HMONITOR %p", hMon);
        MonitorDevice mon;
        mon.hLogical   = hMon;
        mon.hPhysical  = nullptr;
        mon.bounds     = mi.rcMonitor;
        mon.deviceName = mi.szDevice;
        mon.ddcSupported = false;
        mon.contrastSupported = false;
        mon.useSoftwareFallback = true;
        ctx->monitors->push_back(mon);
        return TRUE;
    }

    auto* physArr = new PHYSICAL_MONITOR[numPhysical];
    if (!GetPhysicalMonitorsFromHMONITOR(hMon, numPhysical, physArr)) {
        Logger::Error("GetPhysicalMonitorsFromHMONITOR failed for %p", hMon);
        delete[] physArr;
        return TRUE;
    }

    PhysicalMonitorPtr ptr(physArr, PhysicalMonitorDeleter{numPhysical});

    for (DWORD i = 0; i < numPhysical; ++i) {
        MonitorDevice mon;
        mon.hLogical   = hMon;
        mon.hPhysical  = physArr[i].hPhysicalMonitor;
        mon.bounds     = mi.rcMonitor;
        mon.deviceName = mi.szDevice;

        // Query brightness
        auto bInfo = DdcInterface::GetBrightness(mon.hPhysical);
        if (bInfo.supported) {
            mon.brightnessMin      = bInfo.minimum;
            mon.brightnessMax      = bInfo.maximum;
            mon.originalBrightness = bInfo.current;
            mon.normalBrightness   = bInfo.current;
            mon.currentBrightness  = bInfo.current;
            mon.targetBrightness   = bInfo.current;
            mon.ddcSupported       = true;
        } else {
            mon.ddcSupported       = false;
            mon.useSoftwareFallback = true;
            Logger::Warn("DDC/CI brightness not supported for %ls", mi.szDevice);
        }

        // Query contrast
        auto cInfo = DdcInterface::GetContrast(mon.hPhysical);
        if (cInfo.supported) {
            mon.contrastMin      = cInfo.minimum;
            mon.contrastMax      = cInfo.maximum;
            mon.originalContrast = cInfo.current;
            mon.normalContrast   = cInfo.current;
            mon.currentContrast  = cInfo.current;
            mon.targetContrast   = cInfo.current;
            mon.contrastSupported = true;
        } else {
            mon.contrastSupported = false;
            Logger::Warn("DDC/CI contrast not supported for %ls", mi.szDevice);
        }

        ctx->monitors->push_back(mon);

        Logger::Info("Discovered monitor: %ls bounds=(%ld,%ld)-(%ld,%ld) ddc=%d contrast=%d "
                     "brightness=%lu/%lu-%lu contrast=%lu/%lu-%lu (saved as original)",
                     mi.szDevice,
                     mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right, mi.rcMonitor.bottom,
                     mon.ddcSupported ? 1 : 0, mon.contrastSupported ? 1 : 0,
                     mon.currentBrightness, mon.brightnessMin, mon.brightnessMax,
                     mon.currentContrast, mon.contrastMin, mon.contrastMax);

        break; // Only use first physical monitor per logical monitor
    }

    ctx->handles->push_back(std::move(ptr));
    return TRUE;
}

// ---------------------------------------------------------------------------
// MonitorManager implementation
// ---------------------------------------------------------------------------

MonitorManager::MonitorManager() {
    m_workerWakeEvent = MakeScopedHandle(
        CreateEventW(nullptr, FALSE, FALSE, nullptr));
}

MonitorManager::~MonitorManager() {
    Shutdown();
}

void MonitorManager::EnumerateMonitors() {
    std::lock_guard<std::mutex> lock(m_mutex);

    for (auto& ov : m_overlays) {
        if (ov.hwnd) DestroyWindow(ov.hwnd);
    }
    m_overlays.clear();
    m_monitors.clear();
    m_physicalHandles.clear();

    EnumContext ctx{&m_monitors, &m_physicalHandles};
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, reinterpret_cast<LPARAM>(&ctx));

    for (auto& mon : m_monitors) {
        RecomputeDimFloors(mon);
    }

    Logger::Info("Enumerated %zu monitors", m_monitors.size());
}

void MonitorManager::OnMonitorActivated(HMONITOR hNewActive) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_config.enabled) return;
    if (hNewActive == m_activeMonitor) return;

    auto now = std::chrono::steady_clock::now();
    HMONITOR hPrev = m_activeMonitor;
    m_activeMonitor = hNewActive;

    for (auto& mon : m_monitors) {
        if (mon.hLogical == hNewActive) {
            mon.isActive = true;
            mon.fadeState = FadeState::FadingIn;
            mon.targetBrightness = mon.normalBrightness;
            mon.targetContrast   = mon.normalContrast;
            mon.fadeStartTime = now;
            mon.fadeBrightnessAtStart = static_cast<double>(mon.currentBrightness);
            mon.fadeContrastAtStart    = static_cast<double>(mon.currentContrast);
        } else if (mon.hLogical == hPrev) {
            mon.isActive = false;
            mon.fadeState = FadeState::Waiting;
            mon.idleStart = now;
        }
    }

    if (m_workerWakeEvent)
        SetEvent(m_workerWakeEvent.get());
}

void MonitorManager::SetConfig(const AppConfig& cfg) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_config = cfg;
    for (auto& mon : m_monitors) {
        RecomputeDimFloors(mon);
    }
}

AppConfig MonitorManager::GetConfig() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_config;
}

void MonitorManager::SetEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(m_mutex);
    bool wasEnabled = m_config.enabled;
    m_config.enabled = enabled;

    Logger::Info("Application %s", enabled ? "enabled" : "disabled");

    if (!enabled && wasEnabled) {
        // Restore all monitors to their original brightness/contrast
        for (auto& mon : m_monitors) {
            mon.fadeState = FadeState::Idle;
            mon.isActive = false;
            mon.targetBrightness = mon.originalBrightness;
            mon.targetContrast   = mon.originalContrast;

            // Immediately write original values via DDC/CI
            if (mon.hPhysical) {
                if (mon.ddcSupported) {
                    if (DdcInterface::SetBrightness(mon.hPhysical, mon.originalBrightness))
                        mon.currentBrightness = mon.originalBrightness;
                }
                if (mon.contrastSupported) {
                    if (DdcInterface::SetContrast(mon.hPhysical, mon.originalContrast))
                        mon.currentContrast = mon.originalContrast;
                }
            }

            // Hide software overlay
            if (mon.useSoftwareFallback) {
                for (auto& ov : m_overlays) {
                    if (ov.hMonitor == mon.hLogical && ov.hwnd)
                        ShowWindow(ov.hwnd, SW_HIDE);
                }
            }

            Logger::Info("Restored monitor %ls to original brightness=%lu contrast=%lu",
                         mon.deviceName.c_str(), mon.originalBrightness, mon.originalContrast);
        }
    }

    if (m_workerWakeEvent)
        SetEvent(m_workerWakeEvent.get());
}

bool MonitorManager::IsEnabled() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_config.enabled;
}

void MonitorManager::RecomputeDimFloors(MonitorDevice& mon) {
    // Brightness floor
    DWORD bRange = mon.brightnessMax - mon.brightnessMin;
    DWORD bFloor = mon.brightnessMin +
        static_cast<DWORD>(bRange * m_config.dimBrightnessPercent / 100.0);
    mon.dimFloorBrightness = std::clamp(bFloor, mon.brightnessMin, mon.brightnessMax);

    // Contrast floor
    DWORD cRange = mon.contrastMax - mon.contrastMin;
    DWORD cFloor = mon.contrastMin +
        static_cast<DWORD>(cRange * m_config.dimContrastPercent / 100.0);
    mon.dimFloorContrast = std::clamp(cFloor, mon.contrastMin, mon.contrastMax);
}

void MonitorManager::StartWorker() {
    m_workerRunning = true;
    m_workerThread = std::thread(&MonitorManager::WorkerLoop, this);
    Logger::Info("Worker thread started");
}

void MonitorManager::StopWorker() {
    m_workerRunning = false;
    if (m_workerWakeEvent)
        SetEvent(m_workerWakeEvent.get());
    if (m_workerThread.joinable())
        m_workerThread.join();
    Logger::Info("Worker thread stopped");
}

void MonitorManager::Shutdown() {
    StopWorker();

    std::lock_guard<std::mutex> lock(m_mutex);

    // Restore original brightness and contrast
    for (auto& mon : m_monitors) {
        if (mon.hPhysical) {
            if (mon.ddcSupported)
                DdcInterface::SetBrightness(mon.hPhysical, mon.originalBrightness);
            if (mon.contrastSupported)
                DdcInterface::SetContrast(mon.hPhysical, mon.originalContrast);
            Logger::Info("Restored monitor %ls to original brightness=%lu contrast=%lu on exit",
                         mon.deviceName.c_str(), mon.originalBrightness, mon.originalContrast);
        }
    }

    DestroyOverlays();
    m_monitors.clear();
    m_physicalHandles.clear();

    Logger::Info("MonitorManager shutdown complete");
}

// ---------------------------------------------------------------------------
// Worker loop
// ---------------------------------------------------------------------------

void MonitorManager::WorkerLoop() {
    while (m_workerRunning) {
        DWORD interval;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            interval = static_cast<DWORD>(m_config.workerIntervalMs);
        }

        WaitForSingleObject(m_workerWakeEvent.get(), interval);
        if (!m_workerRunning) break;

        UpdateMonitors();
    }
}

double MonitorManager::LogFade(double startVal, double endVal, double t) {
    t = std::clamp(t, 0.0, 1.0);
    double curve = std::log(1.0 + 9.0 * t) / std::log(10.0);
    return startVal + (endVal - startVal) * curve;
}

void MonitorManager::UpdateMonitors() {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto now = std::chrono::steady_clock::now();

    for (auto& mon : m_monitors) {
        // --- State transitions ---
        if (mon.fadeState == FadeState::Waiting && m_config.enabled) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - mon.idleStart).count();
            if (elapsed >= m_config.idleTimeoutMs) {
                mon.fadeState = FadeState::FadingOut;
                mon.fadeStartTime = now;
                mon.fadeBrightnessAtStart = static_cast<double>(mon.currentBrightness);
                mon.fadeContrastAtStart    = static_cast<double>(mon.currentContrast);
                mon.targetBrightness = mon.dimFloorBrightness;
                mon.targetContrast   = mon.dimFloorContrast;
            }
        }

        // --- Compute desired brightness and contrast ---
        DWORD desiredB = mon.currentBrightness;
        DWORD desiredC = mon.currentContrast;

        if (mon.fadeState == FadeState::FadingOut) {
            double elapsedMs = static_cast<double>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - mon.fadeStartTime).count());

            // Separate fade progress for brightness and contrast
            double tB = (m_config.brightnessFadeOutDurationMs > 0)
                ? elapsedMs / m_config.brightnessFadeOutDurationMs : 1.0;
            double tC = (m_config.contrastFadeOutDurationMs > 0)
                ? elapsedMs / m_config.contrastFadeOutDurationMs : 1.0;

            desiredB = (tB >= 1.0) ? mon.dimFloorBrightness
                : static_cast<DWORD>(std::round(
                    LogFade(mon.fadeBrightnessAtStart,
                            static_cast<double>(mon.dimFloorBrightness), tB)));
            desiredC = (tC >= 1.0) ? mon.dimFloorContrast
                : static_cast<DWORD>(std::round(
                    LogFade(mon.fadeContrastAtStart,
                            static_cast<double>(mon.dimFloorContrast), tC)));

            // Transition to Dimmed when both are complete
            if (tB >= 1.0 && tC >= 1.0) {
                mon.fadeState = FadeState::Dimmed;
            }
        } else if (mon.fadeState == FadeState::FadingIn) {
            double elapsedMs = static_cast<double>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - mon.fadeStartTime).count());

            double t = (m_config.fadeInDurationMs > 0)
                ? elapsedMs / m_config.fadeInDurationMs : 1.0;

            desiredB = (t >= 1.0) ? mon.normalBrightness
                : static_cast<DWORD>(std::round(
                    mon.fadeBrightnessAtStart +
                    (static_cast<double>(mon.normalBrightness) - mon.fadeBrightnessAtStart) * t));
            desiredC = (t >= 1.0) ? mon.normalContrast
                : static_cast<DWORD>(std::round(
                    mon.fadeContrastAtStart +
                    (static_cast<double>(mon.normalContrast) - mon.fadeContrastAtStart) * t));

            if (t >= 1.0) {
                mon.fadeState = FadeState::Idle;
            }
        } else if (mon.fadeState == FadeState::Idle) {
            desiredB = mon.targetBrightness;
            desiredC = mon.targetContrast;
        } else if (mon.fadeState == FadeState::Dimmed) {
            desiredB = mon.dimFloorBrightness;
            desiredC = mon.dimFloorContrast;
        }

        // Clamp
        desiredB = std::clamp(desiredB, mon.brightnessMin, mon.brightnessMax);
        desiredC = std::clamp(desiredC, mon.contrastMin, mon.contrastMax);

        bool brightnessChanged = (desiredB != mon.currentBrightness);
        bool contrastChanged   = (desiredC != mon.currentContrast);

        if (!brightnessChanged && !contrastChanged) continue;

        // Per-monitor throttle: min 50ms between commands
        auto sinceLast = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - mon.lastCommandTime).count();
        if (sinceLast < 50) continue;

        // --- Write via DDC/CI ---
        if (mon.ddcSupported && mon.hPhysical && brightnessChanged) {
            bool ok = DdcInterface::SetBrightness(mon.hPhysical, desiredB);
            if (ok) {
                mon.currentBrightness = desiredB;
                mon.failureCount = 0;
            } else {
                mon.failureCount++;
                if (mon.failureCount >= 5) {
                    Logger::Error("Monitor %ls brightness degraded after %d failures, switching to software fallback",
                                  mon.deviceName.c_str(), mon.failureCount);
                    mon.ddcSupported = false;
                    mon.useSoftwareFallback = true;
                }
            }
            mon.lastCommandTime = now;
        }

        if (mon.contrastSupported && mon.hPhysical && contrastChanged) {
            // Extra throttle check after brightness write
            auto sinceLast2 = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - mon.lastCommandTime).count();
            if (sinceLast2 >= 50) {
                bool ok = DdcInterface::SetContrast(mon.hPhysical, desiredC);
                if (ok) {
                    mon.currentContrast = desiredC;
                } else {
                    Logger::Warn("SetContrast failed for %ls, disabling contrast control",
                                 mon.deviceName.c_str());
                    mon.contrastSupported = false;
                }
                mon.lastCommandTime = now;
            }
        }

        // Software overlay fallback (brightness only — contrast not applicable)
        if (mon.useSoftwareFallback && brightnessChanged) {
            mon.currentBrightness = desiredB;
            bool found = false;
            for (auto& ov : m_overlays) {
                if (ov.hMonitor == mon.hLogical) { found = true; break; }
            }
            if (!found) CreateOverlay(mon);
            UpdateOverlay(mon);
            mon.lastCommandTime = now;
        }
    }
}

// ---------------------------------------------------------------------------
// Software overlay fallback
// ---------------------------------------------------------------------------

void MonitorManager::CreateOverlay(MonitorDevice& mon) {
    if (!m_overlayClassRegistered) {
        WNDCLASSEXW wc = {};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = OverlayWndProc;
        wc.hInstance      = GetModuleHandleW(nullptr);
        wc.lpszClassName  = kOverlayClass;
        RegisterClassExW(&wc);
        m_overlayClassRegistered = true;
    }

    int x = mon.bounds.left;
    int y = mon.bounds.top;
    int w = mon.bounds.right - mon.bounds.left;
    int h = mon.bounds.bottom - mon.bounds.top;

    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        kOverlayClass, L"",
        WS_POPUP,
        x, y, w, h,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

    if (hwnd) {
        SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);

        OverlayWindow ov;
        ov.hwnd = hwnd;
        ov.hMonitor = mon.hLogical;
        m_overlays.push_back(ov);

        Logger::Info("Created software overlay for %ls", mon.deviceName.c_str());
    }
}

void MonitorManager::UpdateOverlay(MonitorDevice& mon) {
    for (auto& ov : m_overlays) {
        if (ov.hMonitor != mon.hLogical || !ov.hwnd) continue;

        double range = static_cast<double>(mon.normalBrightness - mon.dimFloorBrightness);
        double dimAmount = 0.0;
        if (range > 0 && mon.currentBrightness < mon.normalBrightness) {
            dimAmount = static_cast<double>(mon.normalBrightness - mon.currentBrightness) / range;
        }
        dimAmount = std::clamp(dimAmount, 0.0, 1.0);

        BYTE alpha = static_cast<BYTE>(dimAmount * 200);
        SetLayeredWindowAttributes(ov.hwnd, 0, alpha, LWA_ALPHA);

        if (alpha > 0) {
            ShowWindow(ov.hwnd, SW_SHOWNOACTIVATE);
        } else {
            ShowWindow(ov.hwnd, SW_HIDE);
        }
        break;
    }
}

void MonitorManager::DestroyOverlays() {
    for (auto& ov : m_overlays) {
        if (ov.hwnd) DestroyWindow(ov.hwnd);
    }
    m_overlays.clear();
}
