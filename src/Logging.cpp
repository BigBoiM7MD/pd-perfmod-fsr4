#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "../include/Logging.h"
#include <cstring>
#include <cwchar>
#include <utility>
#include <Windows.h>

FILE* Logging::s_file         = nullptr;
SRWLOCK Logging::s_lock       = SRWLOCK_INIT;
bool Logging::s_verbose       = false;   // default: NON-VERBOSE (set by INI)
INIT_ONCE Logging::s_initOnce = INIT_ONCE_STATIC_INIT;
std::atomic<Logging::LogDelegateFn> Logging::s_delegate{nullptr};

// ---------------------------------------------------------------------------
// stderr / file / delegate helpers -- SEPARATE FILE-SCOPE FUNCTIONS.
// MSVC/clang-cl forbids mixing __try/__except (SEH) with C++ try/catch in one
// function, and __try cannot appear in functions with C++ objects needing
// unwinding. So stderr/file writes use dedicated SEH helpers, while external
// host delegate invocation uses a dedicated /EHa C++ try/catch helper invoked
// AFTER s_lock is released.
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

static void safeFileLog(FILE* file, const char* level, const char* fmt, va_list args) {
    __try {
        fprintf(file, "[%s] ", level);
        vfprintf(file, fmt, args);
        fprintf(file, "\n");
        fflush(file);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // defend the caller; keep going
    }
}

static void safeInvokeDelegate(Logging::LogDelegateFn fn, char* msg, int len) {
    try {
        fn(msg, len);
    } catch (...) {
        // Never allow an external host log delegate (C++ or SEH under /EHa)
        // to crash or unwind through PDPerfPlugin.
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

static std::wstring stripExtendedDrivePrefixW(std::wstring path) {
    if (path.size() >= 7 && path.compare(0, 4, L"\\\\?\\") == 0 &&
        isAsciiAlphaW(path[4]) && path[5] == L':' && path[6] == L'\\') {
        path.erase(0, 4);
    }
    return path;
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
                shortIni = stripExtendedDrivePrefixW(std::move(shortIni));
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
                std::wstring candidate = stripExtendedDrivePrefixW(shortDir + L"\\" + file);
                if (candidate.size() < MAX_PATH) {
                    return candidate;
                }
            }
        }
    }

    return iniPath;
}

static std::string trimAscii(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || s[start] == '\t' || s[start] == '\r' || s[start] == '\n')) {
        ++start;
    }
    size_t end = s.size();
    while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r' || s[end - 1] == '\n')) {
        --end;
    }
    return s.substr(start, end - start);
}

static std::string toLowerAscii(std::string s) {
    for (char& ch : s) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return s;
}

static std::string wideAsciiLower(const wchar_t* s) {
    std::string out;
    if (!s) return out;
    for (; *s; ++s) {
        wchar_t ch = *s;
        if (ch >= L'A' && ch <= L'Z') {
            ch = static_cast<wchar_t>(ch - L'A' + L'a');
        }
        out.push_back(static_cast<char>(ch & 0xFF));
    }
    return out;
}

static void copyDefaultIniValue(const wchar_t* defaultValue, wchar_t* outBuf, DWORD outSize) {
    if (!outBuf || outSize == 0) return;
    if (!defaultValue) {
        outBuf[0] = L'\0';
        return;
    }
    wcsncpy_s(outBuf, outSize, defaultValue, _TRUNCATE);
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

std::wstring Logging::getIniApiPath() {
    return resolveIniApiPathW(getIniPath());
}

std::wstring Logging::getLogPath() {
    return getDllSiblingPath(PD_LOG_FILE_NAME_W);
}

bool Logging::readIniFallback(const wchar_t* section, const wchar_t* key,
                              const wchar_t* defaultValue, wchar_t* outBuf, DWORD outSize) {
    copyDefaultIniValue(defaultValue, outBuf, outSize);
    if (!section || !key || !outBuf || outSize == 0) {
        return false;
    }

    const std::wstring iniPath = getIniPath();
    FILE* f = _wfopen(iniPath.c_str(), L"rb");
    if (!f) {
        return false;
    }

    const std::string targetSection = wideAsciiLower(section);
    const std::string targetKey     = wideAsciiLower(key);
    bool inSection = false;
    bool firstLine = true;
    char lineBuf[1024];

    while (fgets(lineBuf, sizeof(lineBuf), f)) {
        std::string line(lineBuf);
        if (firstLine) {
            firstLine = false;
            if (line.size() >= 3 &&
                static_cast<unsigned char>(line[0]) == 0xEF &&
                static_cast<unsigned char>(line[1]) == 0xBB &&
                static_cast<unsigned char>(line[2]) == 0xBF) {
                line.erase(0, 3);
            }
        }

        const std::string trimmed = trimAscii(line);
        if (trimmed.empty() || trimmed[0] == ';' || trimmed[0] == '#') {
            continue;
        }
        if (trimmed.front() == '[') {
            const size_t closePos = trimmed.find(']');
            if (closePos != std::string::npos) {
                const std::string sec = toLowerAscii(trimAscii(trimmed.substr(1, closePos - 1)));
                inSection = (sec == targetSection);
            } else {
                inSection = false;
            }
            continue;
        }
        if (!inSection) {
            continue;
        }

        const size_t eqPos = trimmed.find('=');
        if (eqPos == std::string::npos) {
            continue;
        }
        const std::string k = toLowerAscii(trimAscii(trimmed.substr(0, eqPos)));
        if (k != targetKey) {
            continue;
        }

        std::string val = trimAscii(trimmed.substr(eqPos + 1));
        if (val.size() >= 2 &&
            ((val.front() == '"' && val.back() == '"') ||
             (val.front() == '\'' && val.back() == '\''))) {
            val = val.substr(1, val.size() - 2);
        }
        if (!val.empty()) {
            const int written = MultiByteToWideChar(CP_UTF8, 0, val.c_str(), -1, outBuf, static_cast<int>(outSize));
            if (written <= 0) {
                outBuf[outSize - 1] = L'\0';
            }
            fclose(f);
            return true;
        }
        fclose(f);
        return false;
    }

    fclose(f);
    return false;
}

BOOL CALLBACK Logging::loggingInitOnce(PINIT_ONCE, PVOID param, PVOID*) {
    auto* didInit = static_cast<bool*>(param);
    // Guard the whole routine. Under Wine/Proton the INI/log file open or the
    // INI read can throw; if it does we must NOT abort the plugin's init (that
    // used to escape through the export and kill REFramework). Fall back to a
    // stderr-only logger so failures are never fully silent. /EHa makes
    // catch(...) trap SEH/AV too. Always return TRUE so callers never wedge.
    try {
        const std::wstring iniPath     = getIniPath();
        const std::wstring profilePath = getIniApiPath();

        wchar_t buf[16] = L"0";
        // Default to NON-VERBOSE (Verbose=0), matching README.md: prints the
        // loaded banner, FSR version + GPU header, and errors while keeping
        // disk I/O quiet. Users can set Verbose=1 for full internal diagnostics.
        // Note: when the key or file is missing, GetPrivateProfileStringW
        // copies L"0" and returns 1. Checking copied == 0 preserves L"0" if the
        // Win32 API fails (e.g. >= MAX_PATH without 8.3 names) or is empty.
        if (profilePath.size() < MAX_PATH) {
            const DWORD copied = GetPrivateProfileStringW(L"Logging", L"Verbose", L"0", buf, 16, profilePath.c_str());
            if (copied == 0) {
                wcscpy_s(buf, L"0");
            }
        } else {
            readIniFallback(L"Logging", L"Verbose", L"0", buf, 16);
        }
        s_verbose = (buf[0] == L'1');

        if (GetFileAttributesW(iniPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            // File missing -> create it with NON-VERBOSE (Verbose=0) default.
            if (profilePath.size() >= MAX_PATH ||
                !WritePrivateProfileStringW(L"Logging", L"Verbose", L"0", profilePath.c_str()) ||
                GetFileAttributesW(iniPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
                FILE* iniFile = _wfopen(iniPath.c_str(), L"w");
                if (iniFile) {
                    fputs("[Logging]\nVerbose=0\n", iniFile);
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
        if (didInit) {
            *didInit = true;
        }
    } catch (...) {
        // Logger setup threw (shouldn't, but under emulation anything goes):
        // keep s_file null; vlog() mirrors to stderr so we're never blind.
        safeStderrWarn("[PDPerfPlugin] Logging::init() threw; continuing without log file.\n");
        s_file = nullptr;
    }
    return TRUE;
}

bool Logging::init() {
    bool firstInit = false;
    InitOnceExecuteOnce(&s_initOnce, loggingInitOnce, &firstInit, nullptr);
    return firstInit;
}

void Logging::setDelegate(LogDelegateFn fn) {
    s_delegate.store(fn, std::memory_order_release);
}

void Logging::info(const char* fmt, ...) {
    if (!s_verbose) return;
    va_list args;
    va_start(args, fmt);
    vlog("INFO", fmt, args, false);
    va_end(args);
}

void Logging::warn(const char* fmt, ...) {
    if (!s_verbose) return;
    va_list args;
    va_start(args, fmt);
    vlog("WARN", fmt, args, false);
    va_end(args);
}

// Errors are NOT gated by verbose: a failed init (loader missing -> black
// screen, ffxCreateContext failed, etc.) must be visible even in non-verbose.
void Logging::error(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vlog("ERROR", fmt, args, true);
    va_end(args);
}

// Always printed in both modes (FSR version + GPU header, mode line).
void Logging::always(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vlog("INFO", fmt, args, true);
    va_end(args);
}

void Logging::vlog(const char* level, const char* fmt, va_list args, bool forwardToDelegate) {
    char delegateBuf[1024] = {};
    int delegateLen = 0;

    if (forwardToDelegate) {
        size_t prefixLen = 0;
        if (level && strcmp(level, "ERROR") == 0) {
            static constexpr char kErrPrefix[] = "[ERROR] ";
            prefixLen = sizeof(kErrPrefix) - 1;
            memcpy(delegateBuf, kErrPrefix, prefixLen);
        }

        va_list aDelegate;
        va_copy(aDelegate, args);
        const size_t avail = sizeof(delegateBuf) - prefixLen;
        const int n = vsnprintf(delegateBuf + prefixLen, avail, fmt, aDelegate);
        va_end(aDelegate);

        if (n < 0) {
            delegateLen = 0;
            delegateBuf[0] = '\0';
        } else {
            size_t total = prefixLen + static_cast<size_t>(n);
            if (total >= sizeof(delegateBuf)) {
                total = sizeof(delegateBuf) - 1;
            }
            delegateBuf[total] = '\0';
            while (total > 0 && (delegateBuf[total - 1] == '\n' || delegateBuf[total - 1] == '\r')) {
                delegateBuf[--total] = '\0';
            }
            delegateLen = static_cast<int>(total);
        }
    }

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

    // File write is SEH-guarded in a separate helper: a stale/invalid FILE*
    // under emulation must never AV back into the caller.
    if (s_file) {
        safeFileLog(s_file, level, fmt, args);
    }

    ReleaseSRWLockExclusive(&s_lock);

    // Invoke external host log delegate strictly AFTER releasing s_lock so
    // re-entrancy or a throwing/crashing callback can never deadlock or leak
    // the lock.
    if (forwardToDelegate) {
        const LogDelegateFn fn = s_delegate.load(std::memory_order_acquire);
        if (fn) {
            safeInvokeDelegate(fn, delegateBuf, delegateLen);
        }
    }
}
