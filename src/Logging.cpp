#include "../include/Logging.h"
#include <Windows.h>

FILE* Logging::s_file   = nullptr;
std::mutex Logging::s_mutex;
bool Logging::s_verbose = false;   // default: NON-VERBOSE (set by INI)
bool Logging::s_inited  = false;

void Logging::init() {
    if (s_inited) return;
    s_inited = true;

    // Resolve the DLL directory so the INI + log sit beside PDPerfPlugin.dll.
    char dllDir[MAX_PATH];
    dllDir[0] = '\0';
    HMODULE mod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR)&Logging::init, &mod) && mod) {
        char path[MAX_PATH];
        GetModuleFileNameA(mod, path, MAX_PATH);
        strncpy_s(dllDir, path, MAX_PATH - 1);
        for (int i = (int)strlen(dllDir) - 1; i >= 0; i--) {
            if (dllDir[i] == '\\') { dllDir[i] = '\0'; break; }
        }
    } else {
        GetCurrentDirectoryA(MAX_PATH, dllDir);
    }

    // --- Read/create the INI that selects verbose vs non-verbose ---------
    char iniPath[MAX_PATH];
    sprintf_s(iniPath, "%s\\" PD_LOG_INI_NAME, dllDir);

    char buf[16] = { 0 };
    GetPrivateProfileStringA("Logging", "Verbose", "0", buf, sizeof(buf), iniPath);
    s_verbose = (buf[0] == '1');

    if (GetFileAttributesA(iniPath) == INVALID_FILE_ATTRIBUTES) {
        // File missing -> create it with the default (non-verbose) key.
        WritePrivateProfileStringA("Logging", "Verbose", s_verbose ? "1" : "0", iniPath);
    }

    // --- Open the log file (re)created each load -------------------------
    char logPath[MAX_PATH];
    sprintf_s(logPath, "%s\\" PD_LOG_FILE_NAME, dllDir);
    fopen_s(&s_file, logPath, "w");
    if (s_file) {
        always("pd-perfmod-fsr4 loaded (mode: %s)", s_verbose ? "VERBOSE" : "NON-VERBOSE");
    }
}

void Logging::info(const char* fmt, ...) {
    if (!s_verbose) return;
    va_list args;
    va_start(args, fmt);
    vlog("INFO", fmt, args);
    va_end(args);
}

void Logging::warn(const char* fmt, ...) {
    if (!s_verbose) return;
    va_list args;
    va_start(args, fmt);
    vlog("WARN", fmt, args);
    va_end(args);
}

// Errors are NOT gated by verbose: a failed init (loader missing -> black
// screen, ffxCreateContext failed, etc.) must be visible even in non-verbose.
void Logging::error(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vlog("ERROR", fmt, args);
    va_end(args);
}

// Always printed in both modes (FSR version + GPU header, mode line).
void Logging::always(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vlog("INFO", fmt, args);
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
