// Stand-in for Eden's common/logging.h for the offline shader tool. The arguments are not formatted: the message
// is printed as written in the source, and only when EDEN_XLATE_LOG is set; debug and trace output is dropped.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <fmt/format.h>
#include <fmt/ranges.h>

namespace EdenXlate {
inline bool LogOn() { static const bool on = std::getenv("EDEN_XLATE_LOG") != nullptr; return on; }
}
#define EDEN_XLATE_LOG_AT(level, cls, ...) \
    do { if (::EdenXlate::LogOn()) std::fprintf(stderr, "[%s] %s\n", level, #__VA_ARGS__); } while (0)
#define LOG_TRACE(cls, ...) do {} while (0)
#define LOG_DEBUG(cls, ...) do {} while (0)
#define LOG_INFO(cls, ...) EDEN_XLATE_LOG_AT("info", cls, __VA_ARGS__)
#define LOG_WARNING(cls, ...) EDEN_XLATE_LOG_AT("warning", cls, __VA_ARGS__)
#define LOG_ERROR(cls, ...) EDEN_XLATE_LOG_AT("error", cls, __VA_ARGS__)
#define LOG_CRITICAL(cls, ...) EDEN_XLATE_LOG_AT("critical", cls, __VA_ARGS__)
