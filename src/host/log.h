#pragma once

#include <cstdarg>
#include <cstdio>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace ct2::log {

inline void Info(const char* tag, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
#if defined(__ANDROID__)
    __android_log_vprint(ANDROID_LOG_INFO, tag, fmt, args);
#else
    std::fprintf(stderr, "[I/%s] ", tag);
    std::vfprintf(stderr, fmt, args);
    std::fprintf(stderr, "\n");
#endif
    va_end(args);
}

inline void Warn(const char* tag, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
#if defined(__ANDROID__)
    __android_log_vprint(ANDROID_LOG_WARN, tag, fmt, args);
#else
    std::fprintf(stderr, "[W/%s] ", tag);
    std::vfprintf(stderr, fmt, args);
    std::fprintf(stderr, "\n");
#endif
    va_end(args);
}

inline void Error(const char* tag, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
#if defined(__ANDROID__)
    __android_log_vprint(ANDROID_LOG_ERROR, tag, fmt, args);
#else
    std::fprintf(stderr, "[E/%s] ", tag);
    std::vfprintf(stderr, fmt, args);
    std::fprintf(stderr, "\n");
#endif
    va_end(args);
}

} // namespace ct2::log
