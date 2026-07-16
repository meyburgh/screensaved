#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <physicalmonitorenumerationapi.h>
#include <highlevelmonitorconfigurationapi.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// RAII wrappers
// ---------------------------------------------------------------------------

struct PhysicalMonitorDeleter {
    DWORD count = 0;
    void operator()(PHYSICAL_MONITOR* p) const {
        if (p) {
            DestroyPhysicalMonitors(count, p);
            delete[] p;
        }
    }
};
using PhysicalMonitorPtr = std::unique_ptr<PHYSICAL_MONITOR[], PhysicalMonitorDeleter>;

struct HandleDeleter {
    void operator()(void* h) const {
        if (h && h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
    }
};
using ScopedHandle = std::unique_ptr<void, HandleDeleter>;

inline ScopedHandle MakeScopedHandle(HANDLE h) {
    if (h == nullptr || h == INVALID_HANDLE_VALUE) return nullptr;
    return ScopedHandle(h);
}

struct HookDeleter {
    void operator()(void* h) const {
        if (h) UnhookWindowsHookEx(static_cast<HHOOK>(h));
    }
};
using ScopedHook = std::unique_ptr<void, HookDeleter>;

// ---------------------------------------------------------------------------
// Monitor state
// ---------------------------------------------------------------------------

enum class FadeState {
    Idle,       // Monitor is active — no fading
    Waiting,    // Inactive, waiting for idle timeout
    FadingOut,  // Logarithmic fade-out in progress
    Dimmed,     // Reached dim target
    FadingIn    // Restoring brightness
};

struct MonitorDevice {
    // Identity
    HMONITOR        hLogical    = nullptr;
    HANDLE          hPhysical   = nullptr;   // owned by physicalHandles below
    RECT            bounds      = {};
    std::wstring    deviceName;

    // DDC/CI brightness range
    DWORD           brightnessMin   = 0;
    DWORD           brightnessMax   = 100;

    // DDC/CI contrast range
    DWORD           contrastMin     = 0;
    DWORD           contrastMax     = 100;

    // Brightness state (protected by MonitorManager mutex)
    DWORD           originalBrightness = 100;
    DWORD           normalBrightness   = 100;   // "usual" brightness
    DWORD           targetBrightness   = 100;
    DWORD           currentBrightness  = 100;
    DWORD           dimFloorBrightness = 0;     // computed: 0% of normal by default

    // Contrast state (protected by MonitorManager mutex)
    DWORD           originalContrast   = 50;
    DWORD           normalContrast     = 50;    // "usual" contrast
    DWORD           targetContrast     = 50;
    DWORD           currentContrast    = 50;
    DWORD           dimFloorContrast   = 0;     // computed: 0% of normal by default
    bool            contrastSupported  = true;

    // Fade / timing
    FadeState       fadeState       = FadeState::Idle;
    std::chrono::steady_clock::time_point idleStart;
    std::chrono::steady_clock::time_point fadeStartTime;
    double          fadeBrightnessAtStart = 100.0;
    double          fadeContrastAtStart   = 50.0;

    // Flags
    bool            isActive        = false;
    bool            ddcSupported    = true;
    bool            useSoftwareFallback = false;
    int             failureCount    = 0;

    // True while a full-screen window covers this monitor and an application
    // is holding the display awake (ES_DISPLAY_REQUIRED) — i.e. full-screen
    // media playback. Fading is suppressed while set.
    bool            hasFullscreenMedia = false;

    // Last DDC command time (for per-monitor throttling)
    std::chrono::steady_clock::time_point lastCommandTime;

    // When true, the worker pushes targetBrightness/targetContrast to hardware
    // even if the cached current values already match. Used by clicks,
    // activations, monitor-wake events, and downward-drift reconciliation —
    // any path where the cache might lie about the real hardware state.
    bool            forceWrite      = false;
};

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

struct AppConfig {
    int     idleTimeoutMs               = 15000;    // 15 seconds default
    int     dimBrightnessPercent        = 5;        // dim to 5% of normal brightness
    int     dimContrastPercent          = 5;        // dim to 5% of normal contrast
    int     maxBrightnessPercent        = 100;      // ceiling: % of monitor max brightness
    int     maxContrastPercent          = 100;      // ceiling: % of monitor max contrast
    int     brightnessFadeOutDurationMs = 3000;     // brightness: 3 second fade-out
    int     contrastFadeOutDurationMs   = 3000;     // contrast: 3 second fade-out
    int     fadeInDurationMs            = 100;      // shared fade-in: 100ms default
    int     mouseIdleFadeMinutes        = 5;        // fade active monitor after N minutes without mouse movement (0 = off)
    int     workerIntervalMs            = 100;      // worker wake interval
    bool    enabled                     = true;
    bool    startWithWindows            = false;
    bool    startupPromptShown          = false;    // track whether startup prompt was shown
};

// ---------------------------------------------------------------------------
// Software overlay fallback handle (window per monitor)
// ---------------------------------------------------------------------------

struct OverlayWindow {
    HWND hwnd = nullptr;
    HMONITOR hMonitor = nullptr;
};
