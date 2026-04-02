#include "DdcInterface.h"
#include "Logger.h"

#include <highlevelmonitorconfigurationapi.h>

namespace DdcInterface {

MonitorValueInfo GetBrightness(HANDLE hPhysicalMonitor) {
    MonitorValueInfo info = {0, 0, 100, false};

    DWORD minVal = 0, curVal = 0, maxVal = 100;
    BOOL ok = GetMonitorBrightness(hPhysicalMonitor, &minVal, &curVal, &maxVal);
    if (!ok) {
        DWORD err = GetLastError();
        Logger::Warn("GetMonitorBrightness failed, error=%lu", err);
        return info;
    }

    info.minimum   = minVal;
    info.current   = curVal;
    info.maximum   = maxVal;
    info.supported = true;

    Logger::Info("GetBrightness: min=%lu cur=%lu max=%lu", minVal, curVal, maxVal);
    return info;
}

bool SetBrightness(HANDLE hPhysicalMonitor, DWORD value) {
    BOOL ok = SetMonitorBrightness(hPhysicalMonitor, value);
    if (!ok) {
        DWORD err = GetLastError();
        Logger::Warn("SetMonitorBrightness(%lu) failed, error=%lu", value, err);
        return false;
    }
    return true;
}

MonitorValueInfo GetContrast(HANDLE hPhysicalMonitor) {
    MonitorValueInfo info = {0, 0, 100, false};

    DWORD minVal = 0, curVal = 0, maxVal = 100;
    BOOL ok = GetMonitorContrast(hPhysicalMonitor, &minVal, &curVal, &maxVal);
    if (!ok) {
        DWORD err = GetLastError();
        Logger::Warn("GetMonitorContrast failed, error=%lu", err);
        return info;
    }

    info.minimum   = minVal;
    info.current   = curVal;
    info.maximum   = maxVal;
    info.supported = true;

    Logger::Info("GetContrast: min=%lu cur=%lu max=%lu", minVal, curVal, maxVal);
    return info;
}

bool SetContrast(HANDLE hPhysicalMonitor, DWORD value) {
    BOOL ok = SetMonitorContrast(hPhysicalMonitor, value);
    if (!ok) {
        DWORD err = GetLastError();
        Logger::Warn("SetMonitorContrast(%lu) failed, error=%lu", value, err);
        return false;
    }
    return true;
}

} // namespace DdcInterface
