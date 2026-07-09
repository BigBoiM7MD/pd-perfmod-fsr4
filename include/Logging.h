#pragma once

#include <cstdio>
#include <cstdarg>
#include <Windows.h>

// INI + log file live beside the DLL, named after the repo.
#define PD_LOG_INI_NAME   "pd-perfmod-fsr4.ini"
#define PD_LOG_FILE_NAME  "pd-perfmod-fsr4.log"

// NOTE: we intentionally do NOT use std::mutex for the log lock. A static
// std::mutex depends on its C++ dynamic initializer (_Mtx_init) having run;
// under Wine/Proton a late-loaded native DLL can first hit the logger before
// that init has happened, and _Mtx_lock then dereferences a zero _Mtx_t and
// raises an access violation during init() -> the log never opens and FSR4
// silently disables. An SRWLOCK with SRWLOCK_INIT is zero-initialized and
// usable immediately with no dynamic-init dependency -- the Wine-safe choice.
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
    static SRWLOCK s_lock;
    static bool s_verbose;
    static bool s_inited;
};
