#include "MonitorManager.h"
#include "DdcInterface.h"
#include "Logger.h"

#include <dwmapi.h>
#include <powrprof.h>

#include <cmath>
#include <algorithm>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "powrprof.lib")

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
    m_lastMouseActivityTick = GetTickCount64();
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
    m_activeIdleFaded = false;

    for (auto& mon : m_monitors) {
        if (mon.hLogical == hNewActive) {
            mon.isActive = true;
            mon.fadeState = FadeState::FadingIn;
            mon.targetBrightness = mon.normalBrightness;
            mon.targetContrast   = mon.normalContrast;
            mon.fadeStartTime = now;
            mon.fadeBrightnessAtStart = static_cast<double>(mon.currentBrightness);
            mon.fadeContrastAtStart    = static_cast<double>(mon.currentContrast);
            // Why: if hardware drifted (DPMS wake, OSD, dropped DDC write) the
            // cache may already equal normal so the worker would no-op. Force a
            // write so the activated monitor always reaches the ceiling.
            mon.forceWrite = true;
        } else if (mon.hLogical == hPrev) {
            mon.isActive = false;
            mon.fadeState = FadeState::Waiting;
            mon.idleStart = now;
        }
    }

    if (m_workerWakeEvent)
        SetEvent(m_workerWakeEvent.get());
}

void MonitorManager::OnMonitorClicked(HMONITOR hMon) {
    if (!hMon) return;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_config.enabled) return;

        auto now = std::chrono::steady_clock::now();

        for (auto& mon : m_monitors) {
            if (mon.hLogical != hMon) continue;

            // A click is an explicit "make this bright now" gesture. Always
            // queue a forced write — never trust the cached current values,
            // because hardware can drift below the cache (DPMS wake, OSD
            // adjustment, silently-dropped DDC write) and leave the monitor
            // dim while we believe it's at the ceiling.
            mon.fadeState = FadeState::Idle;
            mon.isActive = true;
            mon.targetBrightness = mon.normalBrightness;
            mon.targetContrast   = mon.normalContrast;
            mon.lastCommandTime = std::chrono::steady_clock::time_point{};
            mon.forceWrite = true;

            // If this click landed on a monitor that wasn't the tracked
            // active one, treat it as an activation transition.
            if (m_activeMonitor && m_activeMonitor != hMon) {
                for (auto& prev : m_monitors) {
                    if (prev.hLogical != m_activeMonitor) continue;
                    prev.isActive = false;
                    if (prev.fadeState == FadeState::Idle ||
                        prev.fadeState == FadeState::FadingIn) {
                        prev.fadeState = FadeState::Waiting;
                        prev.idleStart = now;
                    }
                    break;
                }
            }
            m_activeMonitor = hMon;
            m_activeIdleFaded = false;

            Logger::Info("Click on %ls — snapping to ceiling brightness=%lu contrast=%lu",
                         mon.deviceName.c_str(),
                         mon.normalBrightness, mon.normalContrast);
            break;
        }
    }

    if (m_workerWakeEvent) SetEvent(m_workerWakeEvent.get());
}

void MonitorManager::OnMouseActivity() {
    m_lastMouseActivityTick.store(GetTickCount64(), std::memory_order_relaxed);

    // Fast path: nothing faded by mouse-idle, so no lock needed. This runs on
    // the low-level hook thread for every mouse move and must stay cheap.
    if (!m_activeIdleFaded.load(std::memory_order_relaxed)) return;

    bool woke = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_activeIdleFaded = false;
        if (!m_config.enabled) return;

        auto now = std::chrono::steady_clock::now();
        for (auto& mon : m_monitors) {
            if (!mon.isActive) continue;
            if (mon.fadeState == FadeState::FadingOut ||
                mon.fadeState == FadeState::Dimmed) {
                mon.fadeState = FadeState::FadingIn;
                mon.fadeStartTime = now;
                mon.fadeBrightnessAtStart = static_cast<double>(mon.currentBrightness);
                mon.fadeContrastAtStart    = static_cast<double>(mon.currentContrast);
                mon.targetBrightness = mon.normalBrightness;
                mon.targetContrast   = mon.normalContrast;
                mon.forceWrite = true;
                woke = true;
                Logger::Info("Mouse activity — restoring idle-faded active monitor %ls",
                             mon.deviceName.c_str());
            }
            break;
        }
    }

    if (woke && m_workerWakeEvent) SetEvent(m_workerWakeEvent.get());
}

void MonitorManager::OnKeyboardActivity(HMONITOR hMon) {
    // Runs on the low-level keyboard hook thread — atomic stores only, never
    // take m_mutex here (the worker holds it across slow DDC writes and that
    // would delay keystroke delivery system-wide). The worker picks these up
    // on its next tick.
    if (!hMon) return;
    m_lastKeyboardMonitor.store(hMon, std::memory_order_relaxed);
    m_lastKeyboardActivityTick.store(GetTickCount64(), std::memory_order_relaxed);
}

void MonitorManager::OnDisplayWake() {
    Logger::Info("Display wake event — forcing reconcile on all monitors");
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // Force the next worker tick to validate topology in case monitor
        // handles changed across the sleep/wake cycle.
        m_lastTopologyCheck = std::chrono::steady_clock::time_point{};
        for (auto& mon : m_monitors) {
            mon.forceWrite = true;
            mon.lastCommandTime = std::chrono::steady_clock::time_point{};
        }
    }
    if (m_workerWakeEvent) SetEvent(m_workerWakeEvent.get());
}

void MonitorManager::OnDisplayChanged() {
    Logger::Info("Display change event — scheduling topology validation");
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lastTopologyCheck = std::chrono::steady_clock::time_point{};
    }
    if (m_workerWakeEvent) SetEvent(m_workerWakeEvent.get());
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
        m_activeIdleFaded = false;
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

    // Brightness ceiling — used as the "normal" / restore target.
    DWORD bCeil = mon.brightnessMin +
        static_cast<DWORD>(bRange * m_config.maxBrightnessPercent / 100.0);
    mon.normalBrightness = std::clamp(bCeil, mon.brightnessMin, mon.brightnessMax);

    // Contrast ceiling
    DWORD cCeil = mon.contrastMin +
        static_cast<DWORD>(cRange * m_config.maxContrastPercent / 100.0);
    mon.normalContrast = std::clamp(cCeil, mon.contrastMin, mon.contrastMax);
}

void MonitorManager::SetOnConfigChanged(ConfigSavedCallback cb) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_onConfigChanged = std::move(cb);
}

void MonitorManager::ApplyCeilings() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& mon : m_monitors) {
            RecomputeDimFloors(mon);
            mon.targetBrightness  = mon.normalBrightness;
            mon.targetContrast    = mon.normalContrast;
            mon.currentBrightness = mon.normalBrightness;
            mon.currentContrast   = mon.normalContrast;
            mon.fadeState         = FadeState::Idle;

            if (mon.hPhysical) {
                if (mon.ddcSupported)
                    DdcInterface::SetBrightness(mon.hPhysical, mon.normalBrightness);
                if (mon.contrastSupported)
                    DdcInterface::SetContrast(mon.hPhysical, mon.normalContrast);
                Logger::Info("Applied ceiling to %ls: brightness=%lu contrast=%lu",
                             mon.deviceName.c_str(),
                             mon.normalBrightness, mon.normalContrast);
            }
        }
    }
    if (m_workerWakeEvent) SetEvent(m_workerWakeEvent.get());
}

bool MonitorManager::PollForCeilingBump() {
    AppConfig cfgCopy;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_config.enabled) return false;

        for (auto& mon : m_monitors) {
            if (!mon.hPhysical || !mon.isActive) continue;
            if (mon.fadeState != FadeState::Idle) continue;

            if (mon.ddcSupported) {
                auto info = DdcInterface::GetBrightness(mon.hPhysical);
                if (info.supported) {
                    if (info.current > mon.normalBrightness) {
                        DWORD range = mon.brightnessMax - mon.brightnessMin;
                        if (range > 0) {
                            int pct = static_cast<int>(std::round(
                                100.0 * (info.current - mon.brightnessMin) / range));
                            if (pct > m_config.maxBrightnessPercent) {
                                Logger::Info("User raised brightness on %ls to %lu — bumping ceiling %d%% -> %d%%",
                                             mon.deviceName.c_str(), info.current,
                                             m_config.maxBrightnessPercent, pct);
                                m_config.maxBrightnessPercent = std::clamp(pct, 0, 100);
                                changed = true;
                            }
                        }
                        mon.currentBrightness = info.current;
                    } else if (info.current < mon.normalBrightness) {
                        // Hardware drifted below the ceiling on the active
                        // monitor (DPMS wake, OSD adjustment, silently-dropped
                        // DDC write). Sync the cache and force the worker to
                        // push back up to the target.
                        Logger::Info("Brightness drift on %ls: hw=%lu cache=%lu — forcing re-write",
                                     mon.deviceName.c_str(),
                                     info.current, mon.currentBrightness);
                        mon.currentBrightness = info.current;
                        mon.forceWrite = true;
                    }
                }
            }

            if (mon.contrastSupported) {
                auto info = DdcInterface::GetContrast(mon.hPhysical);
                if (info.supported) {
                    if (info.current > mon.normalContrast) {
                        DWORD range = mon.contrastMax - mon.contrastMin;
                        if (range > 0) {
                            int pct = static_cast<int>(std::round(
                                100.0 * (info.current - mon.contrastMin) / range));
                            if (pct > m_config.maxContrastPercent) {
                                Logger::Info("User raised contrast on %ls to %lu — bumping ceiling %d%% -> %d%%",
                                             mon.deviceName.c_str(), info.current,
                                             m_config.maxContrastPercent, pct);
                                m_config.maxContrastPercent = std::clamp(pct, 0, 100);
                                changed = true;
                            }
                        }
                        mon.currentContrast = info.current;
                    } else if (info.current < mon.normalContrast) {
                        Logger::Info("Contrast drift on %ls: hw=%lu cache=%lu — forcing re-write",
                                     mon.deviceName.c_str(),
                                     info.current, mon.currentContrast);
                        mon.currentContrast = info.current;
                        mon.forceWrite = true;
                    }
                }
            }
        }

        if (changed) {
            for (auto& mon : m_monitors) RecomputeDimFloors(mon);
            cfgCopy = m_config;
        }
    }

    if (changed && m_onConfigChanged) {
        m_onConfigChanged(cfgCopy);
    }
    return changed;
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

    // Set all monitors to their saved ceiling values on exit.
    for (auto& mon : m_monitors) {
        if (mon.hPhysical) {
            if (mon.ddcSupported)
                DdcInterface::SetBrightness(mon.hPhysical, mon.normalBrightness);
            if (mon.contrastSupported)
                DdcInterface::SetContrast(mon.hPhysical, mon.normalContrast);
            Logger::Info("Set monitor %ls to ceiling brightness=%lu contrast=%lu on exit",
                         mon.deviceName.c_str(), mon.normalBrightness, mon.normalContrast);
        }
    }

    DestroyOverlays();
    m_monitors.clear();
    m_physicalHandles.clear();

    Logger::Info("MonitorManager shutdown complete");
}

// ---------------------------------------------------------------------------
// Topology validation (watchdog)
// ---------------------------------------------------------------------------

void MonitorManager::ValidateTopology() {
    // Snapshot what we currently believe.
    struct Snapshot { HMONITOR h; RECT bounds; };
    std::vector<Snapshot> known;
    HMONITOR believedActive = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        believedActive = m_activeMonitor;
        for (auto& m : m_monitors)
            known.push_back({m.hLogical, m.bounds});
    }

    // Re-enumerate desktop monitors and compare against our cache.
    struct EnumItem { HMONITOR h; RECT bounds; };
    std::vector<EnumItem> live;
    EnumDisplayMonitors(nullptr, nullptr,
        [](HMONITOR h, HDC, LPRECT, LPARAM lp) -> BOOL {
            MONITORINFO mi = {}; mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(h, &mi)) {
                reinterpret_cast<std::vector<EnumItem>*>(lp)->push_back({h, mi.rcMonitor});
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&live));

    bool topologyChanged = (live.size() != known.size());
    if (!topologyChanged) {
        for (size_t i = 0; i < live.size() && !topologyChanged; ++i) {
            bool found = false;
            for (auto& k : known) {
                if (k.h == live[i].h &&
                    k.bounds.left   == live[i].bounds.left  &&
                    k.bounds.top    == live[i].bounds.top   &&
                    k.bounds.right  == live[i].bounds.right &&
                    k.bounds.bottom == live[i].bounds.bottom) {
                    found = true;
                    break;
                }
            }
            if (!found) topologyChanged = true;
        }
    }

    if (topologyChanged) {
        Logger::Warn("Topology change detected, re-enumerating monitors");
        EnumerateMonitors();
        ApplyCeilings();
        return;
    }

    // Reconcile active monitor against the current cursor position in case
    // the mouse hook missed a transition.
    POINT pt;
    if (!GetCursorPos(&pt)) return;
    HMONITOR hCursor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    if (hCursor && hCursor != believedActive) {
        Logger::Info("Watchdog detected stale active monitor; reconciling to %p", hCursor);
        OnMonitorActivated(hCursor);
    }
}

// ---------------------------------------------------------------------------
// Full-screen media detection
// ---------------------------------------------------------------------------

namespace {

struct FullscreenScanEntry {
    HMONITOR hLogical;
    RECT     bounds;
    bool     covered;
};

bool IsShellOrOverlayWindow(HWND hwnd) {
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, static_cast<int>(_countof(cls)));
    return _wcsicmp(cls, L"Progman") == 0 ||
           _wcsicmp(cls, L"WorkerW") == 0 ||
           _wcsicmp(cls, kOverlayClass) == 0;
}

BOOL CALLBACK FullscreenEnumProc(HWND hwnd, LPARAM lp) {
    auto* entries = reinterpret_cast<std::vector<FullscreenScanEntry>*>(lp);

    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;
    if (IsShellOrOverlayWindow(hwnd)) return TRUE;

    // Skip DWM-cloaked windows (suspended UWP apps report as visible).
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED,
                                        &cloaked, sizeof(cloaked))) && cloaked)
        return TRUE;

    RECT wr;
    if (!GetWindowRect(hwnd, &wr)) return TRUE;

    for (auto& e : *entries) {
        if (!e.covered &&
            wr.left  <= e.bounds.left  && wr.top    <= e.bounds.top &&
            wr.right >= e.bounds.right && wr.bottom >= e.bounds.bottom) {
            e.covered = true;
        }
    }
    return TRUE;
}

} // namespace

void MonitorManager::UpdateFullscreenMediaFlags() {
    std::vector<FullscreenScanEntry> entries;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_config.enabled) {
            for (auto& mon : m_monitors) mon.hasFullscreenMedia = false;
            return;
        }
        for (auto& mon : m_monitors)
            entries.push_back({mon.hLogical, mon.bounds, false});
    }
    if (entries.empty()) return;

    // "Media playing" = some app is holding the display awake. Video players
    // and browsers set ES_DISPLAY_REQUIRED during playback; a full-screen
    // window alone (e.g. a paused editor in fullscreen) does not count. If
    // the query fails we err toward not dimming full-screen windows.
    ULONG execState = 0;
    bool displayRequired = true;
    if (CallNtPowerInformation(SystemExecutionState, nullptr, 0,
                               &execState, sizeof(execState)) == 0) {
        displayRequired = (execState & ES_DISPLAY_REQUIRED) != 0;
    }

    if (displayRequired) {
        EnumWindows(FullscreenEnumProc, reinterpret_cast<LPARAM>(&entries));
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& mon : m_monitors) {
        for (auto& e : entries) {
            if (e.hLogical != mon.hLogical) continue;

            bool flag = displayRequired && e.covered;
            if (flag != mon.hasFullscreenMedia) {
                Logger::Info("Full-screen media %s on %ls",
                             flag ? "detected" : "ended", mon.deviceName.c_str());
                if (!flag) {
                    if (mon.isActive) {
                        // Media ended on the cursor monitor — restart the
                        // mouse-idle window instead of fading immediately.
                        m_lastMouseActivityTick.store(GetTickCount64(),
                                                      std::memory_order_relaxed);
                    } else if (mon.fadeState == FadeState::Idle ||
                               mon.fadeState == FadeState::FadingIn) {
                        // Media ended on an inactive monitor: begin the
                        // normal idle countdown.
                        mon.fadeState = FadeState::Waiting;
                        mon.idleStart = std::chrono::steady_clock::now();
                    }
                }
            }
            mon.hasFullscreenMedia = flag;
            break;
        }
    }
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

        // Periodically (~2s) refresh full-screen media flags before advancing
        // fades so a newly started video suppresses dimming promptly.
        {
            auto tick = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(
                    tick - m_lastFullscreenCheck).count() >= 2000) {
                m_lastFullscreenCheck = tick;
                UpdateFullscreenMediaFlags();
            }
        }

        UpdateMonitors();

        // Periodically (~5s) poll active monitor for user-driven ceiling bumps.
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                now - m_lastCeilingPoll).count() >= 5000) {
            m_lastCeilingPoll = now;
            PollForCeilingBump();
        }

        // Periodically (~2s) run the topology / active-monitor watchdog.
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                now - m_lastTopologyCheck).count() >= 2000) {
            m_lastTopologyCheck = now;
            ValidateTopology();
        }
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

    // Pick up keyboard activity handed off by the hook thread. It counts as
    // activity on the monitor hosting the focused window.
    ULONGLONG kbTick = m_lastKeyboardActivityTick.load(std::memory_order_relaxed);
    HMONITOR kbMon = m_lastKeyboardMonitor.load(std::memory_order_relaxed);
    bool kbFresh = (kbTick != 0 && kbTick != m_processedKeyboardTick);
    if (kbFresh) m_processedKeyboardTick = kbTick;

    for (auto& mon : m_monitors) {
        // --- State transitions ---
        if (kbFresh && m_config.enabled && mon.hLogical == kbMon) {
            // Keyboard input in a window on this monitor counts as mouse
            // activity here: keep it awake and reverse any fade in progress.
            if (mon.isActive) {
                m_lastMouseActivityTick.store(kbTick, std::memory_order_relaxed);
            }
            if (mon.fadeState == FadeState::Waiting) {
                mon.idleStart = now;
            } else if (mon.fadeState == FadeState::FadingOut ||
                       mon.fadeState == FadeState::Dimmed) {
                mon.fadeState = FadeState::FadingIn;
                mon.fadeStartTime = now;
                mon.fadeBrightnessAtStart = static_cast<double>(mon.currentBrightness);
                mon.fadeContrastAtStart    = static_cast<double>(mon.currentContrast);
                mon.targetBrightness = mon.normalBrightness;
                mon.targetContrast   = mon.normalContrast;
                mon.forceWrite = true;
                if (mon.isActive) m_activeIdleFaded = false;
                Logger::Info("Keyboard activity on %ls — restoring brightness",
                             mon.deviceName.c_str());
            }
        }

        if (mon.hasFullscreenMedia && m_config.enabled) {
            // Full-screen media playing on this monitor: never begin fading,
            // and restore it if a fade already started.
            if (mon.fadeState == FadeState::Waiting) {
                mon.idleStart = now;
            } else if (mon.fadeState == FadeState::FadingOut ||
                       mon.fadeState == FadeState::Dimmed) {
                mon.fadeState = FadeState::FadingIn;
                mon.fadeStartTime = now;
                mon.fadeBrightnessAtStart = static_cast<double>(mon.currentBrightness);
                mon.fadeContrastAtStart    = static_cast<double>(mon.currentContrast);
                mon.targetBrightness = mon.normalBrightness;
                mon.targetContrast   = mon.normalContrast;
                if (mon.isActive) m_activeIdleFaded = false;
                Logger::Info("Full-screen media on %ls — restoring brightness",
                             mon.deviceName.c_str());
            }
        }

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

        // Mouse-idle fade: the active monitor (the one holding the cursor)
        // also fades once the mouse has been still for the configured time.
        if (m_config.enabled && mon.isActive &&
            mon.fadeState == FadeState::Idle &&
            m_config.mouseIdleFadeMinutes > 0 && !mon.hasFullscreenMedia) {
            ULONGLONG idleMs = GetTickCount64() -
                m_lastMouseActivityTick.load(std::memory_order_relaxed);
            if (idleMs >= static_cast<ULONGLONG>(m_config.mouseIdleFadeMinutes) * 60000ULL) {
                mon.fadeState = FadeState::FadingOut;
                mon.fadeStartTime = now;
                mon.fadeBrightnessAtStart = static_cast<double>(mon.currentBrightness);
                mon.fadeContrastAtStart    = static_cast<double>(mon.currentContrast);
                mon.targetBrightness = mon.dimFloorBrightness;
                mon.targetContrast   = mon.dimFloorContrast;
                m_activeIdleFaded = true;
                Logger::Info("No mouse movement for %d min — fading active monitor %ls",
                             m_config.mouseIdleFadeMinutes, mon.deviceName.c_str());
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
                if (mon.isActive) {
                    mon.fadeState = FadeState::Idle;
                } else {
                    // Restored while inactive (keyboard activity or media on
                    // a non-cursor monitor): resume the idle countdown so it
                    // dims again once the activity stops.
                    mon.fadeState = FadeState::Waiting;
                    mon.idleStart = now;
                }
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
        bool force             = mon.forceWrite;

        if (!brightnessChanged && !contrastChanged && !force) continue;

        // Per-monitor throttle: min 50ms between commands. Force writes
        // bypass the throttle so click / wake / drift recovery is immediate.
        auto sinceLast = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - mon.lastCommandTime).count();
        if (sinceLast < 50 && !force) continue;

        bool brightnessWrite = brightnessChanged || force;
        bool contrastWrite   = contrastChanged   || force;

        // --- Write via DDC/CI ---
        if (mon.ddcSupported && mon.hPhysical && brightnessWrite) {
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

        if (mon.contrastSupported && mon.hPhysical && contrastWrite) {
            // Extra throttle check after brightness write — also bypassed
            // for force writes so a click pushes both values in one tick.
            auto sinceLast2 = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - mon.lastCommandTime).count();
            if (sinceLast2 >= 50 || force) {
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
        if (mon.useSoftwareFallback && brightnessWrite) {
            mon.currentBrightness = desiredB;
            bool found = false;
            for (auto& ov : m_overlays) {
                if (ov.hMonitor == mon.hLogical) { found = true; break; }
            }
            if (!found) CreateOverlay(mon);
            UpdateOverlay(mon);
            mon.lastCommandTime = now;
        }

        mon.forceWrite = false;
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
