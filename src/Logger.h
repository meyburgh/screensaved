#pragma once

#include <string>
#include <cstdarg>

namespace Logger {

void Init(const std::wstring& logFilePath);
void Shutdown();

void Info(const char* fmt, ...);
void Warn(const char* fmt, ...);
void Error(const char* fmt, ...);

} // namespace Logger
