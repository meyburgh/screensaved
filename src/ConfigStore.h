#pragma once

#include "Monitor.hpp"
#include <string>

namespace ConfigStore {

// Returns path to config INI file in %APPDATA%/ScreenSaved/
std::wstring GetConfigPath();

// Returns path to log file in %APPDATA%/ScreenSaved/
std::wstring GetLogPath();

// Load configuration from AppData; fills defaults for missing keys
AppConfig Load();

// Save current configuration to AppData
void Save(const AppConfig& cfg);

// Register/unregister app to start at user logon (HKCU\...\Run)
void SetStartWithWindows(bool enable);

// Check if startup registration currently exists
bool IsStartWithWindowsEnabled();

} // namespace ConfigStore
