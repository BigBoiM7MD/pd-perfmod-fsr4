#pragma once

#include <cstdio>
#include <mutex>
#include <cstdarg>

// INI + log file live beside the DLL, named after the repo.
#define PD_LOG_INI_NAME   "pd-perfmod-fsr4.ini"
#define PD_LOG_FILE_NAME  "pd-perfmod-fsr4.log"

class Logging {
public:
    static void init();
    static void info(const char* fmt, ...);
    static void warn(const char* fmt, ...);
    static void error(const char* fmt, ...);

    // Written in BOTH verbose and non-verbose modes (used for the
    // FSR version + GPU header, and fatal setup errors).
    static void always(const char* fmt, ...);

    static bool isVerbose() { return s_verbose; }

private:
    static void vlog(const char* level, const char* fmt, va_list args);
    static FILE* s_file;
    static std::mutex s_mutex;
    static bool s_verbose;
    static bool s_inited;
};
