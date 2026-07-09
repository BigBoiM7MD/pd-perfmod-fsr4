#include "../include/Logging.h"
#include <Windows.h>

FILE* Logging::s_file   = nullptr;
SRWLOCK Logging::s_lock = SRWLOCK_INIT;
bool Logging::s_verbose = false;   // default: NON-VERBOSE (set by INI)
bool Logging::s_inited  = false;

// ---------------------------------------------------------------------------
// stderr helpers -- SEPARATE FILE-SCOPE FUNCTIONS.
// MSVC forbids mixing __try/__except (SEH) with C++ try/catch in one function,
// and __try cannot appear in functions with C++ objects needing unwinding. So
// every stderr write is routed here, where we use SEH to swallow a bad/invalid
// stderr handle. Under a headless Proton launch stderr is often invalid and
// fprintf() to it raises an access violation; with /EHa that SEH would be
// caught by an export's C++ try/catch and silently disable FSR4. Guarding it
// here ensures a dead stderr can NEVER disable the backend.
// ---------------------------------------------------------------------------
static void safeStderr(const char* level, const char* fmt, va_list args) {
    __try {
        fprintf(stderr, "[%s] ", level);
        vfprintf(stderr, fmt, args);
        fprintf(stderr, "\n");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // stderr unusable (headless launch) -- file log (if any) still runs.
    }
}

// NOT static: referenced from PDPerfPlugin.cpp (declared extern there).
void safeStderrWarn(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    __try {
        vfprintf(stderr, fmt, args);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    va_end(args);
}

void Logging::init() {
    if (s_inited) return;
    s_inited = true;

    // Guard the whole routine. Under Wine/Proton the INI/log file open or the
    // INI read can throw; if it does we must NOT abort the plugin's init (that
    // used to escape through the export and kill REFramework). Fall back to a
    // stderr-only logger so failures are never fully silent. /EHa makes
    // catch(...) trap SEH/AV too.
    try {
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
        // Default to VERBOSE. The non-verbose path hides almost every
        // FSR4Backend: diagnostic line, and on a new platform (Linux/Proton)
        // you WANT to see them. User can set Verbose=0 to quiet it down.
        GetPrivateProfileStringA("Logging", "Verbose", "1", buf, sizeof(buf), iniPath);
        s_verbose = (buf[0] == '1');

        if (GetFileAttributesA(iniPath) == INVALID_FILE_ATTRIBUTES) {
            // File missing -> create it with VERBOSE default so diagnostics show.
            WritePrivateProfileStringA("Logging", "Verbose", "1", iniPath);
        }

        // --- Open the log file (re)created each load -------------------------
        char logPath[MAX_PATH];
        sprintf_s(logPath, "%s\\" PD_LOG_FILE_NAME, dllDir);
        // Plain fopen, NOT _fsopen(..., _SH_DENYWR): under Wine/Proton the file-
        // sharing flag makes msvcrt return NULL, which silently killed the log
        // (empty file, no output). We don't need shared-read for the log to work;
        // a file that never opens is worse than one you can't tail. Unbuffered
        // (_IONBF) so every line hits disk immediately. vlog() ALSO mirrors to
        // stderr, so output is still visible under Proton even if this fails.
        s_file = fopen(logPath, "w");
        if (s_file) {
            setvbuf(s_file, nullptr, _IONBF, 0);
        } else {
            safeStderrWarn("[PDPerfPlugin] WARNING: could not open log file %s (continuing; output -> stderr only)\n", logPath);
        }
        always("pd-perfmod-fsr4 loaded (mode: %s)", s_verbose ? "VERBOSE" : "NON-VERBOSE");
    } catch (...) {
        // Logger setup threw (shouldn't, but under emulation anything goes):
        // keep s_file null; vlog() mirrors to stderr so we're never blind.
        safeStderrWarn("[PDPerfPlugin] Logging::init() threw; continuing without log file.\n");
        s_file = nullptr;
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
    // SRWLOCK (not std::mutex): zero-init via SRWLOCK_INIT, usable before any
    // C++ dynamic init has run. Critical under Wine/Proton where this DLL is
    // loaded late and can first log during its own init().
    AcquireSRWLockExclusive(&s_lock);

    // ALWAYS mirror to stderr first. On native Windows this is discarded (no
    // console attached), so behavior is unchanged. Under Wine/Proton stderr is
    // captured to the game terminal / PROTON_LOG, which is the reliable channel
    // when the log file can't be opened. Routed through safeStderr() so a
    // bad/invalid stderr handle under a headless Proton launch cannot raise SEH
    // that disables FSR4.
    {
        va_list a2;
        va_copy(a2, args);
        safeStderr(level, fmt, a2);
        va_end(a2);
    }

    // File write is SEH-guarded: a stale/invalid FILE* under emulation must
    // never AV back into the caller (that used to disable FSR4 during init).
    if (s_file) {
        __try {
            fprintf(s_file, "[%s] ", level);
            vfprintf(s_file, fmt, args);
            fprintf(s_file, "\n");
            fflush(s_file);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // defend the caller; keep going
        }
    }

    ReleaseSRWLockExclusive(&s_lock);
}
