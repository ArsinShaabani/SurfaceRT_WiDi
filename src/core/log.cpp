// SurfaceRT-WiDi - logging implementation.
// SPDX-License-Identifier: MIT
#include "widi/log.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>

namespace widi {
namespace {

std::mutex g_mutex;
LogLevel g_level = LogLevel::Info;
std::FILE* g_file = nullptr;
bool g_ownsFile = false;

const char* level_name(LogLevel level) {
    switch (level) {
        case LogLevel::Error: return "E";
        case LogLevel::Warn: return "W";
        case LogLevel::Info: return "I";
        default: return "D";
    }
}

std::FILE* target() {
    if (g_file) return g_file;
    return stderr;
}

}  // namespace

void set_log_level(LogLevel level) { g_level = level; }
LogLevel log_level() { return g_level; }

bool set_log_file(const char* path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file && g_ownsFile) {
        std::fclose(g_file);
        g_file = nullptr;
        g_ownsFile = false;
    }
    if (!path) return true;
    std::FILE* f = std::fopen(path, "a");
    if (!f) return false;
    g_file = f;
    g_ownsFile = true;
    return true;
}

void close_log_file() { set_log_file(nullptr); }

void log_message(LogLevel level, const char* fmt, ...) {
    if (static_cast<int>(level) > static_cast<int>(g_level)) return;

    char message[2048];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    const std::time_t now = std::time(nullptr);
    std::tm tm_now;
#if defined(_WIN32)
    localtime_s(&tm_now, &now);
#else
    localtime_r(&now, &tm_now);
#endif

    std::lock_guard<std::mutex> lock(g_mutex);
    std::FILE* out = target();
    std::fprintf(out, "%04d-%02d-%02d %02d:%02d:%02d [%s] %s\n",
                 tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
                 tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec,
                 level_name(level), message);
    std::fflush(out);
}

}  // namespace widi
