#pragma once

#include <cstdio>
#include <mutex>
#include <cstdarg>

class Logging {
public:
    static void init();
    static void info(const char* fmt, ...);
    static void warn(const char* fmt, ...);
    static void error(const char* fmt, ...);

private:
    static void vlog(const char* level, const char* fmt, va_list args);
    static FILE* s_file;
    static std::mutex s_mutex;
};
