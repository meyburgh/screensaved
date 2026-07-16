#include "Monitor.hpp"
#include "ConfigStore.h"
#include "Logger.h"
#include "MonitorManager.h"
#include "InputHook.h"
#include "TrayUI.h"

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    // Initialize logging (in AppData)
    Logger::Init(ConfigStore::GetLogPath());
    Logger::Info("=== ScreenSaved starting ===");

    // Load configuration from AppData
    AppConfig config = ConfigStore::Load();

    // Verify startup registration matches current exe path
    ConfigStore::VerifyStartupRegistration(config.startWithWindows);

    // Create monitor manager
    MonitorManager monitorMgr;
    monitorMgr.SetConfig(config);
    monitorMgr.EnumerateMonitors();
    monitorMgr.ApplyCeilings();

    // Persist ceiling changes detected at runtime by the worker.
    monitorMgr.SetOnConfigChanged([](const AppConfig& cfg) {
        ConfigStore::Save(cfg);
    });

    monitorMgr.StartWorker();

    // Create input hook
    InputHook inputHook(monitorMgr);
    inputHook.Start();

    // Create tray UI
    TrayUI tray;

    auto onToggle = [&]() {
        bool newState = !monitorMgr.IsEnabled();
        monitorMgr.SetEnabled(newState);
        tray.SetEnabled(newState);

        AppConfig cfg = monitorMgr.GetConfig();
        ConfigStore::Save(cfg);

        Logger::Info("Toggled enabled state to %s", newState ? "enabled" : "disabled");
    };

    auto onExit = [&]() {
        Logger::Info("Exit requested from tray");
        tray.Quit();
    };

    auto onSettingsSave = [&](const AppConfig& cfg) {
        monitorMgr.SetConfig(cfg);
        monitorMgr.SetEnabled(cfg.enabled);
        ConfigStore::Save(cfg);
        monitorMgr.ApplyCeilings();
        Logger::Info("Settings saved from dialog");
    };

    auto getConfig = [&]() -> AppConfig {
        return monitorMgr.GetConfig();
    };

    auto onDisplayWake = [&]() {
        monitorMgr.OnDisplayWake();
    };

    auto onDisplayChange = [&]() {
        monitorMgr.OnDisplayChanged();
    };

    if (!tray.Init(hInstance, onToggle, onExit, onSettingsSave, getConfig,
                   onDisplayWake, onDisplayChange)) {
        Logger::Error("Failed to initialize tray UI");
        monitorMgr.Shutdown();
        Logger::Shutdown();
        return 1;
    }

    tray.SetEnabled(config.enabled);

    // Prompt for startup at logon if not already enabled and not yet prompted
    if (!config.startupPromptShown && !ConfigStore::IsStartWithWindowsEnabled()) {
        int result = MessageBoxW(nullptr,
            L"Would you like ScreenSaved to start automatically when you sign in?",
            L"ScreenSaved",
            MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND);

        config.startupPromptShown = true;
        if (result == IDYES) {
            config.startWithWindows = true;
            ConfigStore::SetStartWithWindows(true);
            Logger::Info("User enabled startup at logon from prompt");
        } else {
            Logger::Info("User declined startup at logon from prompt");
        }
        ConfigStore::Save(config);
        monitorMgr.SetConfig(config);
    }

    Logger::Info("Entering message loop");
    tray.RunMessageLoop();

    // Shutdown
    Logger::Info("Shutting down...");

    // Verify startup registration before exiting
    AppConfig finalConfig = monitorMgr.GetConfig();
    ConfigStore::VerifyStartupRegistration(finalConfig.startWithWindows);

    inputHook.Stop();
    monitorMgr.Shutdown();
    tray.Shutdown();

    Logger::Info("=== ScreenSaved exited ===");
    Logger::Shutdown();

    return 0;
}
