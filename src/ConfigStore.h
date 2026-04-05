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

// Verify the startup registry entry points to the current exe path.
// If startWithWindows is true and the entry is missing or stale, re-register.
// If startWithWindows is false and the entry exists, remove it.
void VerifyStartupRegistration(bool startWithWindows);

} // namespace ConfigStore
