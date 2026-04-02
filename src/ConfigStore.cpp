#include "ConfigStore.h"
#include "Logger.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>

namespace ConfigStore {

static const wchar_t* kSection = L"ScreenSaved";

static std::wstring GetAppDataDir() {
    wchar_t* appData = nullptr;
    std::wstring dir;

    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) {
        dir = appData;
        CoTaskMemFree(appData);
    } else {
        // Fallback to environment variable
        wchar_t buf[MAX_PATH];
        if (GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH) > 0) {
            dir = buf;
        } else {
            // Last resort: next to executable
            GetModuleFileNameW(nullptr, buf, MAX_PATH);
            PathRemoveFileSpecW(buf);
            dir = buf;
        }
    }

    dir += L"\\ScreenSaved";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring GetConfigPath() {
    return GetAppDataDir() + L"\\screensaved.ini";
}

std::wstring GetLogPath() {
    return GetAppDataDir() + L"\\screensaved.log";
}

static int ReadInt(const std::wstring& file, const wchar_t* key, int def) {
    return GetPrivateProfileIntW(kSection, key, def, file.c_str());
}

static void WriteInt(const std::wstring& file, const wchar_t* key, int val) {
    wchar_t buf[32];
    _snwprintf_s(buf, _countof(buf), L"%d", val);
    WritePrivateProfileStringW(kSection, key, buf, file.c_str());
}

AppConfig Load() {
    std::wstring path = GetConfigPath();
    AppConfig cfg;

    // Check if file exists; if not, create with defaults
    DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        Logger::Info("Config file not found, creating with defaults: %ls",
                     path.c_str());
        Save(cfg);
        return cfg;
    }

    cfg.idleTimeoutMs               = ReadInt(path, L"IdleTimeoutMs",               cfg.idleTimeoutMs);
    cfg.dimBrightnessPercent        = ReadInt(path, L"DimBrightnessPercent",        cfg.dimBrightnessPercent);
    cfg.dimContrastPercent          = ReadInt(path, L"DimContrastPercent",          cfg.dimContrastPercent);
    cfg.brightnessFadeOutDurationMs = ReadInt(path, L"BrightnessFadeOutDurationMs", cfg.brightnessFadeOutDurationMs);
    cfg.contrastFadeOutDurationMs   = ReadInt(path, L"ContrastFadeOutDurationMs",   cfg.contrastFadeOutDurationMs);
    cfg.fadeInDurationMs            = ReadInt(path, L"FadeInDurationMs",            cfg.fadeInDurationMs);
    cfg.workerIntervalMs            = ReadInt(path, L"WorkerIntervalMs",            cfg.workerIntervalMs);
    cfg.enabled                     = ReadInt(path, L"Enabled",                     cfg.enabled ? 1 : 0) != 0;
    cfg.startWithWindows            = ReadInt(path, L"StartWithWindows",            cfg.startWithWindows ? 1 : 0) != 0;
    cfg.startupPromptShown          = ReadInt(path, L"StartupPromptShown",          cfg.startupPromptShown ? 1 : 0) != 0;

    // Clamp values
    if (cfg.dimBrightnessPercent < 0)   cfg.dimBrightnessPercent = 0;
    if (cfg.dimBrightnessPercent > 100) cfg.dimBrightnessPercent = 100;
    if (cfg.dimContrastPercent < 0)     cfg.dimContrastPercent = 0;
    if (cfg.dimContrastPercent > 100)   cfg.dimContrastPercent = 100;
    if (cfg.idleTimeoutMs < 500)        cfg.idleTimeoutMs = 500;
    if (cfg.workerIntervalMs < 50)      cfg.workerIntervalMs = 50;

    Logger::Info("Config loaded from %ls: idle=%dms dimB=%d%% dimC=%d%% enabled=%d startup=%d",
                 path.c_str(),
                 cfg.idleTimeoutMs, cfg.dimBrightnessPercent,
                 cfg.dimContrastPercent, cfg.enabled ? 1 : 0,
                 cfg.startWithWindows ? 1 : 0);
    return cfg;
}

void Save(const AppConfig& cfg) {
    std::wstring path = GetConfigPath();

    WriteInt(path, L"IdleTimeoutMs",               cfg.idleTimeoutMs);
    WriteInt(path, L"DimBrightnessPercent",        cfg.dimBrightnessPercent);
    WriteInt(path, L"DimContrastPercent",           cfg.dimContrastPercent);
    WriteInt(path, L"BrightnessFadeOutDurationMs",  cfg.brightnessFadeOutDurationMs);
    WriteInt(path, L"ContrastFadeOutDurationMs",    cfg.contrastFadeOutDurationMs);
    WriteInt(path, L"FadeInDurationMs",             cfg.fadeInDurationMs);
    WriteInt(path, L"WorkerIntervalMs",             cfg.workerIntervalMs);
    WriteInt(path, L"Enabled",                      cfg.enabled ? 1 : 0);
    WriteInt(path, L"StartWithWindows",             cfg.startWithWindows ? 1 : 0);
    WriteInt(path, L"StartupPromptShown",           cfg.startupPromptShown ? 1 : 0);

    Logger::Info("Config saved to %ls", path.c_str());
}

static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kRunValueName = L"ScreenSaved";

void SetStartWithWindows(bool enable) {
    HKEY hKey = nullptr;
    LONG result = RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &hKey);
    if (result != ERROR_SUCCESS) {
        Logger::Error("Failed to open Run registry key, error=%ld", result);
        return;
    }

    if (enable) {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        result = RegSetValueExW(hKey, kRunValueName, 0, REG_SZ,
                                reinterpret_cast<const BYTE*>(exePath),
                                static_cast<DWORD>((wcslen(exePath) + 1) * sizeof(wchar_t)));
        if (result == ERROR_SUCCESS) {
            Logger::Info("Startup registration enabled: %ls", exePath);
        } else {
            Logger::Error("Failed to set startup registry value, error=%ld", result);
        }
    } else {
        result = RegDeleteValueW(hKey, kRunValueName);
        if (result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND) {
            Logger::Info("Startup registration disabled");
        } else {
            Logger::Error("Failed to delete startup registry value, error=%ld", result);
        }
    }

    RegCloseKey(hKey);
}

bool IsStartWithWindowsEnabled() {
    HKEY hKey = nullptr;
    LONG result = RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &hKey);
    if (result != ERROR_SUCCESS) return false;

    DWORD type = 0, size = 0;
    result = RegQueryValueExW(hKey, kRunValueName, nullptr, &type, nullptr, &size);
    RegCloseKey(hKey);
    return (result == ERROR_SUCCESS && type == REG_SZ);
}

} // namespace ConfigStore
