// SurfaceRT-WiDi - tiny logging facility (stderr on POSIX, file on Windows RT).
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdarg>

namespace widi {

enum class LogLevel { Error = 0, Warn = 1, Info = 2, Debug = 3 };

void set_log_level(LogLevel level);
LogLevel log_level();

// path == nullptr restores stderr. On Windows RT the service points this at
// %ProgramData%\SurfaceRT-WiDi\widi.log.
bool set_log_file(const char* path);
void close_log_file();

void log_message(LogLevel level, const char* fmt, ...);

}  // namespace widi

#define WIDI_LOGE(...) ::widi::log_message(::widi::LogLevel::Error, __VA_ARGS__)
#define WIDI_LOGW(...) ::widi::log_message(::widi::LogLevel::Warn, __VA_ARGS__)
#define WIDI_LOGI(...) ::widi::log_message(::widi::LogLevel::Info, __VA_ARGS__)
#define WIDI_LOGD(...) ::widi::log_message(::widi::LogLevel::Debug, __VA_ARGS__)
