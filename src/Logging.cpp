#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "../include/Logging.h"
#include <utility>
#include <Windows.h>

FILE* Logging::s_file         = nullptr;
SRWLOCK Logging::s_lock       = SRWLOCK_INIT;
bool Logging::s_verbose       = false;   // default: NON-VERBOSE (set by INI)
INIT_ONCE Logging::s_initOnce = INIT_ONCE_STATIC_INIT;

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

static bool isAsciiAlphaW(wchar_t ch) {
    return (ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z');
}

// Normalize slashes and apply extended-length \\?\ or \\?\UNC\ prefix when a
// path reaches MAX_PATH so Win32 wide APIs handle up to 32768 chars cleanly.
static std::wstring normalizeExtendedPathW(std::wstring path) {
    if (path.empty()) {
        return path;
    }

    for (wchar_t& ch : path) {
        if (ch == L'/') {
            ch = L'\\';
        }
    }

    if (path.compare(0, 4, L"\\\\?\\") == 0 || path.compare(0, 4, L"\\\\.\\") == 0) {
        return path;
    }

    if (path.size() >= MAX_PATH) {
        if (path.size() >= 3 && isAsciiAlphaW(path[0]) && path[1] == L':' && path[2] == L'\\') {
            return L"\\\\?\\" + path;
        }
        if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\') {
            return L"\\\\?\\UNC\\" + path.substr(2);
        }
    }

    return path;
}

static std::wstring resolveModuleDirectoryW(LPCWSTR moduleAddr) {
    constexpr DWORD kInitialCap = 512;
    constexpr DWORD kMaxCap     = 32768;

    HMODULE mod = nullptr;
    std::wstring modulePath;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            moduleAddr, &mod) && mod) {
        DWORD cap = kInitialCap;
        while (cap <= kMaxCap) {
            std::wstring buf(cap, L'\0');
            SetLastError(ERROR_SUCCESS);
            const DWORD len = GetModuleFileNameW(mod, &buf[0], cap);
            const DWORD err = GetLastError();
            if (len == 0) {
                break;
            }
            if (len < cap && err != ERROR_INSUFFICIENT_BUFFER) {
                buf.resize(len);
                modulePath = std::move(buf);
                break;
            }
            if (cap == kMaxCap) {
                break;
            }
            cap = (cap > kMaxCap / 2) ? kMaxCap : cap * 2;
        }
    }

    if (!modulePath.empty()) {
        const size_t sep = modulePath.find_last_of(L"\\/");
        if (sep != std::wstring::npos) {
            return normalizeExtendedPathW(modulePath.substr(0, sep));
        }
    }

    const DWORD needed = GetCurrentDirectoryW(0, nullptr);
    if (needed > 0) {
        std::wstring cwd(needed, L'\0');
        const DWORD written = GetCurrentDirectoryW(needed, &cwd[0]);
        if (written > 0 && written < needed) {
            cwd.resize(written);
            while (!cwd.empty() && (cwd.back() == L'\\' || cwd.back() == L'/')) {
                cwd.pop_back();
            }
            return normalizeExtendedPathW(std::move(cwd));
        }
    }

    return L"";
}

// Win32 PrivateProfile APIs enforce MAX_PATH internally in Wine and older
// Windows builds. When >= MAX_PATH, shorten via 8.3 names when available so
// GetPrivateProfileStringW / WritePrivateProfileStringW operate on the real file.
static std::wstring resolveIniApiPathW(const std::wstring& iniPath) {
    if (iniPath.size() < MAX_PATH) {
        return iniPath;
    }

    if (GetFileAttributesW(iniPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        const DWORD needed = GetShortPathNameW(iniPath.c_str(), nullptr, 0);
        if (needed > 0) {
            std::wstring shortIni(needed, L'\0');
            const DWORD written = GetShortPathNameW(iniPath.c_str(), &shortIni[0], needed);
            if (written > 0 && written < needed) {
                shortIni.resize(written);
                if (shortIni.size() < MAX_PATH) {
                    return shortIni;
                }
            }
        }
    }

    const size_t sep = iniPath.find_last_of(L"\\/");
    if (sep != std::wstring::npos) {
        const std::wstring dir  = iniPath.substr(0, sep);
        const std::wstring file = iniPath.substr(sep + 1);
        const DWORD needed = GetShortPathNameW(dir.c_str(), nullptr, 0);
        if (needed > 0) {
            std::wstring shortDir(needed, L'\0');
            const DWORD written = GetShortPathNameW(dir.c_str(), &shortDir[0], needed);
            if (written > 0 && written < needed) {
                shortDir.resize(written);
                while (!shortDir.empty() && shortDir.back() == L'\\') {
                    shortDir.pop_back();
                }
                std::wstring candidate = shortDir + L"\\" + file;
                if (candidate.size() < MAX_PATH) {
                    return candidate;
                }
            }
        }
    }

    return iniPath;
}

std::wstring Logging::getDllDir() {
    return resolveModuleDirectoryW(reinterpret_cast<LPCWSTR>(&Logging::getDllDir));
}

std::wstring Logging::getDllSiblingPath(const wchar_t* filename) {
    if (!filename || !*filename) {
        return L"";
    }
    const std::wstring dir = getDllDir();
    if (dir.empty()) {
        return filename;
    }
    if (dir.back() == L'\\' || dir.back() == L'/') {
        return normalizeExtendedPathW(dir + filename);
    }
    return normalizeExtendedPathW(dir + L"\\" + filename);
}

std::wstring Logging::getIniPath() {
    return getDllSiblingPath(PD_LOG_INI_NAME_W);
}

std::wstring Logging::getLogPath() {
    return getDllSiblingPath(PD_LOG_FILE_NAME_W);
}

BOOL CALLBACK Logging::loggingInitOnce(PINIT_ONCE, PVOID, PVOID*) {
    // Guard the whole routine. Under Wine/Proton the INI/log file open or the
    // INI read can throw; if it does we must NOT abort the plugin's init (that
    // used to escape through the export and kill REFramework). Fall back to a
    // stderr-only logger so failures are never fully silent. /EHa makes
    // catch(...) trap SEH/AV too. Always return TRUE so callers never wedge.
    try {
        const std::wstring iniPath     = getIniPath();
        const std::wstring profilePath = resolveIniApiPathW(iniPath);

        wchar_t buf[16] = { 0 };
        // Default to VERBOSE. The non-verbose path hides almost every
        // FSR4Backend: diagnostic line, and on a new platform (Linux/Proton)
        // you WANT to see them. User can set Verbose=0 to quiet it down.
        GetPrivateProfileStringW(L"Logging", L"Verbose", L"1", buf, 16, profilePath.c_str());
        s_verbose = (buf[0] == L'1');

        if (GetFileAttributesW(iniPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            // File missing -> create it with VERBOSE default so diagnostics show.
            if (!WritePrivateProfileStringW(L"Logging", L"Verbose", L"1", profilePath.c_str()) ||
                GetFileAttributesW(iniPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
                FILE* iniFile = _wfopen(iniPath.c_str(), L"w");
                if (iniFile) {
                    fputs("[Logging]\nVerbose=1\n", iniFile);
                    fclose(iniFile);
                }
            }
        }

        // --- Open the log file (re)created each load -------------------------
        const std::wstring logPath = getLogPath();
        // Plain _wfopen, NOT _wfsopen with _SH_DENYWR: under Wine/Proton the
        // file-sharing flag makes msvcrt return NULL, which silently killed the
        // log (empty file, no output). Unbuffered (_IONBF) so every line hits
        // disk immediately. vlog() ALSO mirrors to stderr.
        s_file = _wfopen(logPath.c_str(), L"w");
        if (s_file) {
            setvbuf(s_file, nullptr, _IONBF, 0);
        } else {
            safeStderrWarn("[PDPerfPlugin] WARNING: could not open log file %ls (continuing; output -> stderr only)\n",
                           logPath.c_str());
        }
        always("pd-perfmod-fsr4 loaded (mode: %s)", s_verbose ? "VERBOSE" : "NON-VERBOSE");
    } catch (...) {
        // Logger setup threw (shouldn't, but under emulation anything goes):
        // keep s_file null; vlog() mirrors to stderr so we're never blind.
        safeStderrWarn("[PDPerfPlugin] Logging::init() threw; continuing without log file.\n");
        s_file = nullptr;
    }
    return TRUE;
}

void Logging::init() {
    InitOnceExecuteOnce(&s_initOnce, loggingInitOnce, nullptr, nullptr);
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
