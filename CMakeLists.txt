cmake_minimum_required(VERSION 3.16)
project(ScreenSaved LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

enable_language(RC)

add_executable(ScreenSaved WIN32
    src/main.cpp
    src/MonitorManager.cpp
    src/DdcInterface.cpp
    src/InputHook.cpp
    src/TrayUI.cpp
    src/ConfigStore.cpp
    src/Logger.cpp
    src/app.rc
)

target_include_directories(ScreenSaved PRIVATE src)

target_link_libraries(ScreenSaved PRIVATE
    dxva2
    shlwapi
    shell32
    user32
    gdi32
    comctl32
    ole32
    advapi32
    dwmapi
    powrprof
)

# Manifest is embedded via app.rc — disable CMake's auto-generated one
set_target_properties(ScreenSaved PROPERTIES LINK_FLAGS "/MANIFEST:NO")

target_compile_definitions(ScreenSaved PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
