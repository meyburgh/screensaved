#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>

namespace DdcInterface {

struct MonitorValueInfo {
    DWORD minimum;
    DWORD current;
    DWORD maximum;
    bool  supported;
};

// Query brightness range and current value. Returns supported=false on failure.
MonitorValueInfo GetBrightness(HANDLE hPhysicalMonitor);

// Set brightness. Returns true on success.
bool SetBrightness(HANDLE hPhysicalMonitor, DWORD value);

// Query contrast range and current value. Returns supported=false on failure.
MonitorValueInfo GetContrast(HANDLE hPhysicalMonitor);

// Set contrast. Returns true on success.
bool SetContrast(HANDLE hPhysicalMonitor, DWORD value);

} // namespace DdcInterface
