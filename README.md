# ScreenSaved Specification

## Overview

ScreenSaved is a Windows desktop utility that automatically dims inactive monitors by changing their physical backlight brightness and contrast over DDC/CI instead of applying a visual overlay. The application runs primarily from the system tray, tracks which monitor currently contains the mouse cursor, and restores brightness and contrast on the active monitor while dimming monitors that have become idle.

This project is implemented in C++ with a Win32-based tray UI and a multi-threaded architecture designed to keep the application responsive while interacting with comparatively slow monitor hardware.

## Goals

- Adjust monitor brightness and contrast through DDC/CI using VCP codes `0x10` and `0x12`.
- Preserve color accuracy by avoiding software dimming overlays whenever hardware control is available.
- Detect monitor focus changes from mouse movement without polling.
- Keep CPU usage low during idle periods.
- Prevent UI hangs caused by slow I2C/DDC monitor responses.
- Avoid overwhelming monitor firmware with excessive brightness updates.
- Support configurable idle timeout and separate fade configuration for brightness and contrast.
- Allow the user to enable or disable the application from the tray icon.

## Non-Goals

- Full desktop UI frameworks such as Qt or MFC.
- Per-pixel or shader-based dimming as the primary implementation.
- High-frequency animated brightness fades that spam hardware commands.
- Cross-platform support in the initial version.

## Platform

- Operating system: Windows
- Language: C++
- UI model: Win32 tray application
- Hardware interface: DDC/CI via `dxva2.lib`

## High-Level Behavior

The application discovers all connected desktop monitors, maps logical monitors to physical monitor handles, and tracks each monitor's brightness and contrast state. A global low-level mouse hook detects cursor movement. When the cursor enters a different monitor:

- The newly active monitor is marked for restoration toward its active brightness and contrast targets.
- The previously active monitor starts or resets an idle timer.
- When a monitor remains inactive longer than the configured timeout, which defaults to `15 seconds`, it begins fading brightness and contrast toward idle targets using a logarithmic falloff curve.

Brightness and contrast changes are not written directly from the input path. Instead, the application updates target setpoints and a dedicated worker thread performs throttled DDC/CI writes.

By default, the dim target is `5%` of the monitor's usual brightness and `5%` of the monitor's usual contrast, but brightness fade settings and contrast fade settings must be configurable separately.

## Architecture

The application uses three primary execution contexts.

### 1. Main Thread

Responsibilities:

- Initialize Win32 application state.
- Create and manage the system tray icon using `Shell_NotifyIcon`.
- Load and persist configuration.
- Start and stop worker components.
- Own the message loop for tray commands and application shutdown.
- Handle tray icon double-click to toggle application enabled state.
- Handle tray icon right-click to show a context menu.
- Swap tray icon assets so the enabled or disabled state is visually obvious.

The main thread must never block on hardware I/O.

The settings dialog presented from the tray UI must include a branded header reading `ScreenSaved - by HypeDriven.com` at the top of the window. The `HypeDriven.com` portion must be a clickable hyperlink to `https://www.hypedriven.com`.

### 2. Input Thread

Responsibilities:

- Install a low-level global mouse hook with `SetWindowsHookEx(WH_MOUSE_LL, ...)`.
- Process mouse move events.
- Resolve the current logical monitor using `MonitorFromPoint`.
- Detect transitions between monitors.
- Notify the monitor manager about active monitor changes.

The input thread must not call `SetMonitorBrightness` or `SetMonitorContrast` directly.

### 3. Worker Thread (DDC Queue)

Responsibilities:

- Maintain a queue or setpoint-driven work loop for pending brightness and contrast changes.
- Rate-limit DDC/CI commands.
- Read current monitor state when necessary.
- Advance each monitor's current brightness and contrast toward their targets in controlled steps.
- Apply logarithmic fade-out progression after timeout expiry for both brightness and contrast.
- Log failures for unsupported or unresponsive displays.

This thread isolates slow hardware access from the UI and input paths.

### 4. Topology Watchdog / Validation Timer

Responsibilities:

- Periodically verify that cached monitor bounds still match the current desktop topology.
- Confirm that the current mouse position resolves to the expected monitor even if a hook transition was missed.
- Trigger safe monitor re-enumeration when monitor count, bounds, or mapping appear stale or inconsistent.
- Provide a low-frequency recovery path without replacing the hook-driven design.

This validator exists as a safety net for missed transitions, display hotplug events, and stale monitor geometry.

## Core Components

### Monitor Discovery and Handle Management

The application must enumerate physical monitors associated with the Windows desktop.

Required APIs:

- `EnumDisplayMonitors`
- `GetPhysicalMonitorsFromHMONITOR`
- `DestroyPhysicalMonitors`

Key requirements:

- Distinguish between logical monitors (`HMONITOR`) and physical monitors (`PHYSICAL_MONITOR` / monitor handles used by DDC/CI).
- Capture each monitor's screen bounds for hit testing and mouse-to-monitor mapping.
- Query and cache original brightness and contrast when supported.
- Clean up all physical monitor handles during shutdown or rediscovery.
- Support safe runtime re-enumeration when monitor count, bounds, or monitor mapping changes.

### DDC/CI Communication Layer

This layer wraps Windows monitor configuration APIs and exposes a safe interface to the rest of the application.

Required APIs:

- `GetMonitorBrightness`
- `SetMonitorBrightness`
- `GetMonitorContrast`
- `SetMonitorContrast`

Behavior:

- Use VCP code `0x10` for brightness and `0x12` for contrast through the Windows high-level monitor configuration APIs.
- Expose read/write brightness and contrast operations with structured error handling.
- Detect and report unsupported monitors.
- Normalize brightness and contrast values into application-defined ranges if needed, while respecting hardware min/max values.

### Mouse Tracking Trigger

The application must avoid polling for mouse position.

Required API:

- `SetWindowsHookEx(WH_MOUSE_LL, MouseProc, ...)`

Behavior:

- On mouse move, determine the current monitor with `MonitorFromPoint`.
- If the monitor differs from the previously active monitor, emit a monitor transition event.
- Restore the newly active monitor toward its normal brightness and contrast targets.
- Start or reset idle timing for the previously active monitor.
- If the application is disabled, ignore dimming transition work and leave monitors at their normal brightness and contrast targets.
- The hook is the primary trigger for monitor transitions, but not the only validation mechanism.

### Idle Timing

Idle timing determines when an inactive monitor should dim.

Required API:

- `CreateWaitableTimer`

Behavior:

- Use waitable timers rather than busy loops.
- Keep CPU usage near zero while idle.
- Support per-monitor or centralized idle scheduling.
- Make the idle timeout configurable, with a default of `15 seconds`.
- When the timeout expires, transition the monitor into a fade-out state instead of jumping immediately to the dim target.

### Periodic Validation

The application should include a low-frequency periodic validation pass to guard against missed hook events and stale monitor bounds.

Behavior:

- Run the validation pass on a conservative interval, such as every `1` to `5` seconds.
- Read the current cursor position and resolve the current monitor with `MonitorFromPoint`.
- Verify that all known monitor rectangles still match the current desktop topology.
- Re-enumerate monitors if monitor count, dimensions, or cursor-to-monitor mapping appear inconsistent.
- Reconcile the application's active-monitor state if the cursor is currently on a different monitor than the hook-driven state indicates.

This validation path must remain lightweight and should only trigger heavier monitor re-enumeration when needed.

### Fade Behavior

Fade behavior controls how brightness and contrast change over time after idle begins.

Requirements:

- Fade-out begins only after the configured idle timeout is reached.
- Fade-out follows a logarithmic falloff curve rather than a linear ramp.
- The fade target defaults to `5%` of the monitor's usual brightness and `5%` of the monitor's usual contrast.
- Brightness fade settings and contrast fade settings must be configurable independently.
- Fade-in for a newly active monitor should prioritize responsiveness and defaults to `100ms`, but it must remain throttled through the worker thread for both values.

## Performance Strategy

### Asynchronous Setpoint Model

DDC/CI hardware is slow and often responds on the order of `50ms` to `200ms` per command. Sending one command per small fade step is unsafe and can make some monitors lag or misbehave.

The application must use a setpoint model:

- Each monitor stores `currentBrightness` and `targetBrightness`.
- Each monitor stores `currentContrast` and `targetContrast`.
- Each monitor also stores enough timing state to evaluate where it should be on the fade curve.
- The input and timer logic update only the target.
- The worker thread wakes at a fixed interval, such as every `100ms`.
- If the current brightness or contrast differs from its target, the worker sends at most one bounded update per property toward the target.

This design prevents command storms and keeps the application responsive.

### Throttling Rules

- Do not send unbounded fade loops directly to hardware.
- Coalesce repeated target changes.
- Limit outstanding brightness and contrast work per monitor.
- Skip redundant writes when the target has not changed.
- Back off or mark the device degraded after repeated failures.
- Recompute logarithmic fade targets on the worker cadence, not from the hook callback.

## Configuration

The application should expose at least the following configurable values:

- Idle timeout before fade-out starts
- Fade-in duration, defaulting to `100ms`
- Minimum brightness floor, defaulting to `5%` of usual brightness
- Minimum contrast floor, defaulting to `5%` of usual contrast
- Usual or normal brightness and contrast targets per monitor, if the application supports persistent monitor-specific preferences
- Global enabled or disabled state

Configuration changes should be applied without requiring application restart where practical.

### Persistence

User configuration must be stored per user in the Windows AppData folder.

Requirements:

- Load the user's saved configuration on startup.
- Save updated settings when the user confirms changes in the settings dialog.
- Create the configuration file with application defaults if it does not yet exist.
- Fall back to defaults if the persisted configuration is unreadable or invalid.
- Log configuration load and save failures.
- Persist the user's startup-at-logon preference.
- Persist periodic validation preferences if the feature is made user-configurable.

The exact storage format may be JSON, INI, or another simple structured format, but it must be easy to inspect and stable across launches.

## Data Model

Illustrative monitor representation:

```cpp
#include <windows.h>
#include <physicalmonitorenumerationapi.h>
#include <lowlevelmonitorconfigurationapi.h>
#include <vector>

struct MonitorDevice {
    HANDLE hPhysical;
    RECT bounds;
    DWORD originalBrightness;
    DWORD normalBrightness;
    DWORD targetBrightness;
    DWORD currentBrightness;
    DWORD originalContrast;
    DWORD normalContrast;
    DWORD targetContrast;
    DWORD currentContrast;
    bool isDimmed;
    bool isActive;
    bool ddcSupported;
};

std::vector<MonitorDevice> g_Monitors;
```

Recommended additions for production use:

- A stable monitor identifier
- Support/availability flags for DDC/CI
- Brightness min/max range
- Contrast min/max range
- Last command timestamp
- Failure count
- Idle deadline or timer state
- Fade start timestamp
- Fade progression state or curve parameters
- Enabled-state interaction rules
- Synchronization primitives protecting shared state

## Project Structure

| File | Responsibility |
| :--- | :--- |
| `MonitorManager.cpp` | Enumerates monitors, owns monitor state, and coordinates brightness and contrast targets. |
| `DdcInterface.cpp` | Wraps `dxva2.dll`/`dxva2.lib` calls and translates brightness/contrast hardware errors into application-level results. |
| `InputHook.cpp` | Implements `WH_MOUSE_LL` handling and emits monitor transition events. |
| `TrayUI.cpp` | Implements Win32 tray icon behavior, right-click context menu actions, double-click enable/disable toggling, state-specific icons with `Shell_NotifyIcon`, and a settings dialog with branded header text. |
| `ConfigStore.cpp` | Loads and saves user settings in the current user's AppData folder. |
| `Monitor.hpp` | Defines monitor data structures and shared interfaces. |

## Resource Management

The implementation must use RAII for all owned system resources.

Examples:

- Physical monitor handles must be released with `DestroyPhysicalMonitors`.
- Hook handles must be uninstalled automatically.
- Timer handles, thread handles, and synchronization objects must be cleaned up deterministically.

Preferred implementation options:

- Custom RAII wrapper types
- `std::unique_ptr` with custom deleters where appropriate

## Fallback Behavior

Some systems will not support DDC/CI reliably.

The application should detect and handle:

- `GetPhysicalMonitorsFromHMONITOR` returning no physical monitors
- `GetMonitorBrightness`, `SetMonitorBrightness`, `GetMonitorContrast`, or `SetMonitorContrast` failure
- Display adapters or cables that do not pass DDC/CI

Fallback requirement:

- Provide or plan for a software overlay fallback path when hardware brightness or contrast control is unavailable

The fallback path is not the primary mechanism, but unsupported hardware must not break the app.

## Logging and Diagnostics

The application should log:

- Monitor discovery results
- DDC/CI capability detection
- Brightness or contrast read/write failures
- Repeated timeout or throttling conditions
- Hook installation and shutdown events
- Enabled or disabled state changes from tray interaction
- Configuration load and save results
- Invalid or missing persisted settings handling
- Startup registration enable or disable results
- Watchdog-triggered monitor re-enumeration events
- Detected mismatches between cached monitor bounds and current desktop topology

Logs should be sufficient to diagnose unsupported monitors and hardware response issues.

## Settings Dialog Requirements

The tray-driven settings dialog must include a clear branding header at the top of the window.

Required header text:

- `ScreenSaved - by HypeDriven.com`

Additional guidance:

- The header should appear prominently above the configurable settings controls.
- The branding text is part of the application chrome/content and should be present in the initial implementation, not deferred.
- The `HypeDriven.com` portion of the header must be rendered as a clickable hyperlink that opens `https://www.hypedriven.com`.
- The dialog must display the application's configurable values.
- The dialog controls must initialize from the user's persisted settings.
- Confirming the dialog must update the running configuration and persist the values for future launches.

Minimum settings surface:

- Idle timeout
- Fade-in duration
- Minimum brightness floor
- Minimum contrast floor
- Brightness fade configuration
- Contrast fade configuration
- Application enabled or disabled state
- Start automatically when the user signs in

## Tray Menu Requirements

Right-clicking the tray icon must open a context menu.

Minimum menu items:

- `Settings`
- `Exit`

Behavior:

- Selecting `Settings` opens the settings dialog.
- Selecting `Exit` closes the application cleanly.
- Double-click remains the shortcut for enable or disable toggling.

## Startup Integration Requirements

The settings dialog must include a checkbox that allows the user to enable or disable automatic startup at user logon.

Behavior:

- The startup registration is per user, not machine-wide.
- On application launch, if startup is not already enabled for the current user, the app should prompt the user to choose whether it should run at login.
- The prompt should only appear when startup is currently disabled and should not repeatedly interrupt users after they have made a choice unless that preference is reset.
- Enabling the checkbox registers the application to start automatically for the current user.
- Disabling the checkbox removes that registration.
- The checkbox state should reflect the persisted preference and actual startup registration state where practical.
- The implementation should avoid requiring administrator privileges.

## Security and Privilege Notes

- The application runs in the user session.
- It must have sufficient access to the display driver stack for monitor control APIs to function.
- The design should avoid requiring elevation unless technically necessary on the target system.

## Risks

### 1. Hardware Incompatibility

Some laptops, docks, adapters, and monitor firmware implementations do not support DDC/CI correctly.

Impact:

- Hardware dimming may fail entirely on some displays or only partially support brightness versus contrast control.

Mitigation:

- Capability detection
- Graceful fallback behavior
- Clear logging

### 2. Firmware Stress or Legacy Wear Concerns

Very old displays may respond poorly to frequent brightness or contrast writes.

Impact:

- Laggy transitions
- Potential firmware instability on poorly behaved hardware

Mitigation:

- Aggressive throttling
- Setpoint updates instead of per-step fades

### 3. Slow or Blocking Hardware Responses

DDC/CI writes can stall for tens or hundreds of milliseconds.

Impact:

- UI or input lag if performed on the wrong thread

Mitigation:

- Dedicated worker thread
- No direct hardware access from UI or hook callbacks

### 4. Missed Hook Events or Stale Monitor Geometry

Low-level mouse tracking may occasionally miss a practical transition, and cached monitor bounds may become stale after runtime topology changes.

Impact:

- A monitor may fail to brighten or dim when expected.
- The app may temporarily associate the cursor with the wrong monitor.

Mitigation:

- Low-frequency validation timer
- Runtime monitor re-enumeration
- Active-monitor state reconciliation based on current cursor position

## Functional Requirements

- Discover all desktop-attached monitors on startup.
- Retrieve physical monitor handles for DDC-capable displays.
- Read brightness and contrast capabilities and current values where supported.
- Track mouse monitor transitions using a low-level mouse hook.
- Periodically validate cursor-to-monitor mapping and monitor dimensions as a recovery mechanism.
- Restore the active monitor and dim inactive monitors only after a configurable idle timeout.
- Use a default fade-in duration of `100ms` when restoring the active monitor.
- Apply a logarithmic fade-out curve for brightness and contrast after timeout expiry.
- Respect separate configurable fade settings for brightness and contrast, including minimum floors with defaults of `5%`.
- Throttle brightness and contrast writes to protect monitor firmware and preserve responsiveness.
- Restore or preserve original brightness values as defined by configuration or shutdown policy.
- Restore or preserve original contrast values as defined by configuration or shutdown policy.
- Continue operating when one or more monitors do not support DDC/CI.
- Toggle the application enabled state via tray icon double-click.
- Change the tray icon visual state to indicate whether the application is enabled or disabled.
- Expose a settings dialog from the tray UI that includes the header text `ScreenSaved - by HypeDriven.com`.
- Expose a `Settings` option when the tray icon is right-clicked.
- Display the configurable values in the settings dialog and remember the user's choices.
- Persist user settings in the current user's AppData folder.
- Expose a checkbox in the settings dialog to enable or disable startup at user logon for the current user.
- Prompt on launch to ask whether the app should run at login for the current user when startup is not already enabled.
- Render `HypeDriven.com` in the settings header as a hyperlink to `https://www.hypedriven.com`.

## Non-Functional Requirements

- The tray UI must remain responsive during monitor I/O.
- Idle CPU usage should remain near zero.
- Brightness and contrast update logic must not busy-wait.
- Periodic validation must remain lightweight and low-frequency.
- Shared monitor state must be thread-safe.
- Resource cleanup must be deterministic and leak-free.
- Disabling the app should promptly stop dimming behavior and restore normal brightness and contrast targets without requiring restart.

## Acceptance Criteria

- Moving the mouse between monitors causes the newly active monitor to begin restoring brightness and contrast without freezing the tray UI.
- Inactive monitors do not begin fading until the configured idle timeout has elapsed.
- The idle timeout defaults to `15 seconds` and can be changed through settings.
- Once the timeout expires, inactive monitors fade brightness and contrast down using a logarithmic falloff instead of a linear step pattern.
- Fade-in defaults to `100ms` and can be changed through settings.
- Brightness fade settings and contrast fade settings are exposed separately in the settings dialog.
- The minimum brightness floor defaults to `5%` of usual brightness and the minimum contrast floor defaults to `5%` of usual contrast, and both can be changed independently.
- Rapid mouse movement across screens does not produce a burst of unbounded DDC/CI commands.
- If the mouse hook misses a transition, the periodic validation pass corrects the active-monitor state within its configured interval.
- If monitor layout changes at runtime, the app re-detects monitor dimensions and continues tracking the correct screens.
- Double-clicking the tray icon toggles the app between enabled and disabled states.
- The tray icon visibly changes to reflect whether the app is enabled or disabled.
- Right-clicking the tray icon opens a context menu containing a `Settings` option.
- Opening the settings dialog shows the branding header `ScreenSaved - by HypeDriven.com` at the top of the window.
- The `HypeDriven.com` text in the settings header is clickable and opens `https://www.hypedriven.com`.
- The settings dialog displays the configurable values and initializes from the user's last saved choices.
- The settings dialog includes a startup checkbox and preserves the user's startup preference across launches.
- When the app starts and startup is not already enabled, the user is prompted to enable run-at-login for the current user.
- Confirming changes in the settings dialog persists them to the current user's AppData folder and restores them on next launch.
- Unsupported monitors are detected and reported without crashing the application.
- Closing the application releases monitor handles, hooks, timers, and worker resources cleanly.

## Suggested Next Implementation Steps

1. Define `MonitorDevice` and RAII wrappers in `Monitor.hpp`.
2. Implement monitor enumeration and physical handle acquisition in `MonitorManager.cpp`.
3. Build the `DdcInterface` wrapper around `GetMonitorBrightness`, `SetMonitorBrightness`, `GetMonitorContrast`, and `SetMonitorContrast`.
4. Add the worker-thread setpoint loop and throttling policy.
5. Add `WH_MOUSE_LL` tracking in `InputHook.cpp`.
6. Add the tray icon and configuration commands in `TrayUI.cpp`.
7. Add fallback behavior for unsupported hardware.
