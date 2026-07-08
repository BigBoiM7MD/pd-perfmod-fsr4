#include "../include/Logging.h"
#include <Windows.h>

FILE* Logging::s_file = nullptr;
std::mutex Logging::s_mutex;

void Logging::init() {
    if (s_file) return;

    char path[MAX_PATH];
    HMODULE mod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)&Logging::init, &mod) && mod) {
        GetModuleFileNameA(mod, path, MAX_PATH);
        for (int i = (int)strlen(path) - 1; i >= 0; i--) {
            if (path[i] == '\\') { path[i] = '\0'; break; }
        }
        strcat_s(path, "\\pdperfmod_fsr4.log");
    } else {
        GetCurrentDirectoryA(MAX_PATH, path);
        strcat_s(path, "\\pdperfmod_fsr4.log");
    }
    fopen_s(&s_file, path, "w");
    if (s_file) {
        info("pd-perfmod-fsr4 loaded");
    }
}

void Logging::info(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vlog("INFO", fmt, args);
    va_end(args);
}

void Logging::warn(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vlog("WARN", fmt, args);
    va_end(args);
}

void Logging::error(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vlog("ERROR", fmt, args);
    va_end(args);
}

void Logging::vlog(const char* level, const char* fmt, va_list args) {
    if (!s_file) return;

    std::lock_guard<std::mutex> lock(s_mutex);
    fprintf(s_file, "[%s] ", level);
    vfprintf(s_file, fmt, args);
    fprintf(s_file, "\n");
    fflush(s_file);
}
