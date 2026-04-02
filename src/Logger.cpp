#include "Logger.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace Logger {

static FILE* g_logFile = nullptr;
static std::mutex g_mutex;

void Init(const std::wstring& logFilePath) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_logFile) return;
    g_logFile = _wfopen(logFilePath.c_str(), L"a");
}

void Shutdown() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_logFile) {
        fclose(g_logFile);
        g_logFile = nullptr;
    }
}

static void LogV(const char* level, const char* fmt, va_list args) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_logFile) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_logFile, "[%04d-%02d-%02d %02d:%02d:%02d.%03d] [%s] ",
            st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            level);
    vfprintf(g_logFile, fmt, args);
    fprintf(g_logFile, "\n");
    fflush(g_logFile);
}

void Info(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LogV("INFO", fmt, args);
    va_end(args);
}

void Warn(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LogV("WARN", fmt, args);
    va_end(args);
}

void Error(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LogV("ERROR", fmt, args);
    va_end(args);
}

} // namespace Logger
