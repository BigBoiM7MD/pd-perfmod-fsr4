#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <Windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define TEST_CHECK(cond)                                                              \
    do {                                                                              \
        if (!(cond)) {                                                                \
            fprintf(stderr, "FAIL %s:%d: %s (gle=%lu)\n", __FILE__, __LINE__, #cond,  \
                    GetLastError());                                                  \
            return 1;                                                                 \
        }                                                                             \
    } while (0)

#define TEST_CHECK_VOID(cond)                                                         \
    do {                                                                              \
        if (!(cond)) {                                                                \
            fprintf(stderr, "FAIL %s:%d: %s (gle=%lu)\n", __FILE__, __LINE__, #cond,  \
                    GetLastError());                                                  \
            return false;                                                             \
        }                                                                             \
    } while (0)

static std::wstring normalizeAbsPathW(const wchar_t* path) {
    if (!path || !*path) return L"";
    const DWORD needed = GetFullPathNameW(path, 0, nullptr, nullptr);
    if (needed == 0) return L"";
    std::wstring out(needed, L'\0');
    const DWORD written = GetFullPathNameW(path, needed, &out[0], nullptr);
    if (written == 0 || written >= needed) return L"";
    out.resize(written);
    return out;
}

static std::wstring toExtendedPathW(std::wstring path) {
    for (wchar_t& ch : path) {
        if (ch == L'/') ch = L'\\';
    }
    if (path.compare(0, 4, L"\\\\?\\") == 0 || path.compare(0, 4, L"\\\\.\\") == 0) {
        return path;
    }
    if (path.size() >= MAX_PATH) {
        if (path.size() >= 3 &&
            ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) &&
            path[1] == L':' && path[2] == L'\\') {
            return L"\\\\?\\" + path;
        }
        if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\') {
            return L"\\\\?\\UNC\\" + path.substr(2);
        }
    }
    return path;
}

static bool createDirRecursiveW(const std::wstring& dir) {
    if (dir.empty()) return false;
    std::wstring clean = dir;
    for (wchar_t& ch : clean) {
        if (ch == L'/') ch = L'\\';
    }
    while (clean.size() > 3 && clean.back() == L'\\') {
        clean.pop_back();
    }

    size_t pos = 0;
    if (clean.size() >= 3 && clean[1] == L':' && clean[2] == L'\\') {
        pos = 3;
    } else if (clean.size() >= 2 && clean[0] == L'\\' && clean[1] == L'\\') {
        pos = 2;
    }

    while (pos <= clean.size()) {
        const size_t next = clean.find(L'\\', pos);
        const std::wstring sub = (next == std::wstring::npos) ? clean : clean.substr(0, next);
        if (!sub.empty()) {
            const std::wstring ext = toExtendedPathW(sub);
            if (!CreateDirectoryW(ext.c_str(), nullptr)) {
                const DWORD err = GetLastError();
                if (err != ERROR_ALREADY_EXISTS) {
                    fprintf(stderr, "CreateDirectoryW failed err=%lu\n", err);
                    return false;
                }
            }
        }
        if (next == std::wstring::npos) break;
        pos = next + 1;
    }
    return true;
}

static void deleteTreeW(const std::wstring& dir) {
    const std::wstring pattern = toExtendedPathW(dir + L"\\*");
    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
                continue;
            }
            const std::wstring child = dir + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                deleteTreeW(child);
            } else {
                DeleteFileW(toExtendedPathW(child).c_str());
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }
    RemoveDirectoryW(toExtendedPathW(dir).c_str());
}

static bool readAllBytesW(const std::wstring& path, std::vector<uint8_t>& outBytes) {
    const std::wstring ext = toExtendedPathW(path);
    HANDLE hFile = CreateFileW(ext.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    TEST_CHECK_VOID(hFile != INVALID_HANDLE_VALUE);
    LARGE_INTEGER sz{};
    const BOOL gotSize = GetFileSizeEx(hFile, &sz);
    if (!gotSize || sz.QuadPart <= 0 || sz.QuadPart > 64 * 1024 * 1024) {
        CloseHandle(hFile);
        return false;
    }
    outBytes.resize(static_cast<size_t>(sz.QuadPart));
    DWORD totalRead = 0;
    while (totalRead < outBytes.size()) {
        DWORD chunk = 0;
        if (!ReadFile(hFile, outBytes.data() + totalRead,
                      static_cast<DWORD>(outBytes.size() - totalRead), &chunk, nullptr) ||
            chunk == 0) {
            CloseHandle(hFile);
            return false;
        }
        totalRead += chunk;
    }
    CloseHandle(hFile);
    return true;
}

static bool rvaToFileOffset(const IMAGE_NT_HEADERS64* nt,
                            const std::vector<uint8_t>& bytes,
                            uint32_t rva,
                            size_t& outOffset) {
    if (rva < nt->OptionalHeader.SizeOfHeaders && rva < bytes.size()) {
        outOffset = rva;
        return true;
    }
    const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const uint32_t va = sections[i].VirtualAddress;
        const uint32_t rawSize = sections[i].SizeOfRawData;
        if (rva >= va && (rva - va) < rawSize) {
            const size_t offset = static_cast<size_t>(sections[i].PointerToRawData) + (rva - va);
            if (offset < bytes.size()) {
                outOffset = offset;
                return true;
            }
        }
    }
    return false;
}

static const char* const kExpectedExports[18] = {
    "SetupDirectX",
    "SimpleInit",
    "InitUpscaler",
    "SimpleEvaluate",
    "EvaluateUpscaler",
    "SetMotionScaleX",
    "SetMotionScaleY",
    "GetRenderWidth",
    "GetRenderHeight",
    "GetOptimalSharpness",
    "GetOptimalMipmapBias",
    "SetDebug",
    "ReleaseUpscaleFeature",
    "GetJitterPhaseCount",
    "GetJitterOffset",
    "InitLogDelegate",
    "IsUpscaleMethodAvailable",
    "GetUpscaleMethodName",
};

static bool testPeImportsAndExports(const std::wstring& dllPath) {
    printf("[Test A] Verifying PE32+ static imports and 18 exports...\n");
    std::vector<uint8_t> bytes;
    TEST_CHECK_VOID(readAllBytesW(dllPath, bytes));
    TEST_CHECK_VOID(bytes.size() >= sizeof(IMAGE_DOS_HEADER));

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes.data());
    TEST_CHECK_VOID(dos->e_magic == IMAGE_DOS_SIGNATURE);
    TEST_CHECK_VOID(dos->e_lfanew > 0 &&
                    static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) <= bytes.size());

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(bytes.data() + dos->e_lfanew);
    TEST_CHECK_VOID(nt->Signature == IMAGE_NT_SIGNATURE);
    TEST_CHECK_VOID(nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC);

    const IMAGE_DATA_DIRECTORY& impDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    TEST_CHECK_VOID(impDir.VirtualAddress != 0 && impDir.Size >= sizeof(IMAGE_IMPORT_DESCRIPTOR));
    size_t impOff = 0;
    TEST_CHECK_VOID(rvaToFileOffset(nt, bytes, impDir.VirtualAddress, impOff));

    std::vector<std::string> imports;
    for (size_t idx = 0;; ++idx) {
        const size_t entryOff = impOff + idx * sizeof(IMAGE_IMPORT_DESCRIPTOR);
        TEST_CHECK_VOID(entryOff + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= bytes.size());
        const auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(bytes.data() + entryOff);
        if (desc->Name == 0 && desc->FirstThunk == 0 && desc->OriginalFirstThunk == 0) {
            break;
        }
        size_t nameOff = 0;
        TEST_CHECK_VOID(rvaToFileOffset(nt, bytes, desc->Name, nameOff));
        const char* dllName = reinterpret_cast<const char*>(bytes.data() + nameOff);
        imports.emplace_back(dllName);
    }

    TEST_CHECK_VOID(imports.size() == 1);
    TEST_CHECK_VOID(_stricmp(imports[0].c_str(), "KERNEL32.dll") == 0);

    const IMAGE_DATA_DIRECTORY& expDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    TEST_CHECK_VOID(expDir.VirtualAddress != 0 && expDir.Size >= sizeof(IMAGE_EXPORT_DIRECTORY));
    size_t expOff = 0;
    TEST_CHECK_VOID(rvaToFileOffset(nt, bytes, expDir.VirtualAddress, expOff));
    TEST_CHECK_VOID(expOff + sizeof(IMAGE_EXPORT_DIRECTORY) <= bytes.size());
    const auto* exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(bytes.data() + expOff);

    size_t namesOff = 0;
    TEST_CHECK_VOID(rvaToFileOffset(nt, bytes, exp->AddressOfNames, namesOff));
    TEST_CHECK_VOID(namesOff + static_cast<size_t>(exp->NumberOfNames) * sizeof(uint32_t) <= bytes.size());
    const auto* nameRvas = reinterpret_cast<const uint32_t*>(bytes.data() + namesOff);

    std::vector<std::string> exports;
    exports.reserve(exp->NumberOfNames);
    for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
        size_t nameOff = 0;
        TEST_CHECK_VOID(rvaToFileOffset(nt, bytes, nameRvas[i], nameOff));
        exports.emplace_back(reinterpret_cast<const char*>(bytes.data() + nameOff));
    }

    TEST_CHECK_VOID(exports.size() == 18);
    for (const char* expected : kExpectedExports) {
        bool found = false;
        for (const auto& got : exports) {
            if (got == expected) {
                found = true;
                break;
            }
        }
        TEST_CHECK_VOID(found);
    }

    HMODULE mod = LoadLibraryW(dllPath.c_str());
    TEST_CHECK_VOID(mod != nullptr);
    for (const char* expected : kExpectedExports) {
        TEST_CHECK_VOID(GetProcAddress(mod, expected) != nullptr);
    }
    TEST_CHECK_VOID(FreeLibrary(mod) != FALSE);

    printf("[Test A] PASSED (1 import: KERNEL32.dll, 18 exports verified).\n");
    return true;
}

using PFN_IsUpscaleMethodAvailable = bool (__stdcall*)(int);
using PFN_GetUpscaleMethodName     = char* (__stdcall*)(int);
using PFN_GetOptimalSharpness      = float (__stdcall*)(int);
using PFN_SetDebug                 = void (__stdcall*)(bool);

static bool testLongNonAsciiPath(const std::wstring& srcDllPath) {
    printf("[Test B] Verifying >260-char non-ASCII path handling...\n");
    wchar_t tempBuf[MAX_PATH] = {};
    const DWORD tempLen = GetTempPathW(MAX_PATH, tempBuf);
    TEST_CHECK_VOID(tempLen > 0 && tempLen < MAX_PATH);

    std::wstring rootDir = std::wstring(tempBuf) + L"pd_p1_b_" + std::to_wstring(GetCurrentProcessId());
    while (!rootDir.empty() && rootDir.back() == L'\\') rootDir.pop_back();
    deleteTreeW(rootDir);

    std::wstring deepDir = rootDir + L"\\日本語_тест_Ümlaut";
    while ((deepDir + L"\\PDPerfPlugin.dll").size() <= 285) {
        deepDir += L"\\sub_segment_日本語_Ümlaut_0123456789";
    }
    TEST_CHECK_VOID(createDirRecursiveW(deepDir));

    const std::wstring dstDll = deepDir + L"\\PDPerfPlugin.dll";
    TEST_CHECK_VOID(dstDll.size() > 280);
    TEST_CHECK_VOID(CopyFileW(toExtendedPathW(srcDllPath).c_str(),
                              toExtendedPathW(dstDll).c_str(), FALSE) != FALSE);

    HMODULE mod = LoadLibraryW(toExtendedPathW(dstDll).c_str());
    TEST_CHECK_VOID(mod != nullptr);

    auto isAvail = reinterpret_cast<PFN_IsUpscaleMethodAvailable>(
        GetProcAddress(mod, "IsUpscaleMethodAvailable"));
    auto getName = reinterpret_cast<PFN_GetUpscaleMethodName>(
        GetProcAddress(mod, "GetUpscaleMethodName"));
    TEST_CHECK_VOID(isAvail != nullptr && getName != nullptr);

    TEST_CHECK_VOID(isAvail(1) == true);
    const char* name = getName(1);
    TEST_CHECK_VOID(name != nullptr && strcmp(name, "AMD FSR 3") == 0);

    const std::wstring iniPath = deepDir + L"\\pd-perfmod-fsr4.ini";
    const std::wstring logPath = deepDir + L"\\pd-perfmod-fsr4.log";
    TEST_CHECK_VOID(GetFileAttributesW(toExtendedPathW(iniPath).c_str()) != INVALID_FILE_ATTRIBUTES);
    TEST_CHECK_VOID(GetFileAttributesW(toExtendedPathW(logPath).c_str()) != INVALID_FILE_ATTRIBUTES);

    TEST_CHECK_VOID(FreeLibrary(mod) != FALSE);

    std::vector<uint8_t> logBytes;
    TEST_CHECK_VOID(readAllBytesW(logPath, logBytes));
    std::string logStr(logBytes.begin(), logBytes.end());
    TEST_CHECK_VOID(logStr.find("pd-perfmod-fsr4 loaded") != std::string::npos);

    deleteTreeW(rootDir);
    printf("[Test B] PASSED (path len=%zu, INI + log verified).\n", dstDll.size());
    return true;
}

struct ThreadTestContext {
    HANDLE barrier = nullptr;
    PFN_IsUpscaleMethodAvailable isAvail = nullptr;
    PFN_GetUpscaleMethodName getName     = nullptr;
    PFN_GetOptimalSharpness getSharp     = nullptr;
    PFN_SetDebug setDebug                = nullptr;
    bool ok = false;
};

static DWORD WINAPI concurrentWorkerProc(LPVOID param) {
    auto* ctx = static_cast<ThreadTestContext*>(param);
    if (WaitForSingleObject(ctx->barrier, 10000) != WAIT_OBJECT_0) {
        return 1;
    }
    const bool avail = ctx->isAvail(1);
    const char* name = ctx->getName(1);
    const float sharp = ctx->getSharp(1);
    ctx->setDebug(false);

    ctx->ok = avail && name && strcmp(name, "AMD FSR 3") == 0 && sharp == 0.5f;
    return ctx->ok ? 0 : 1;
}

static bool testConcurrentInit(const std::wstring& srcDllPath) {
    printf("[Test C] Verifying 16-thread concurrent InitOnceExecuteOnce...\n");
    wchar_t tempBuf[MAX_PATH] = {};
    const DWORD tempLen = GetTempPathW(MAX_PATH, tempBuf);
    TEST_CHECK_VOID(tempLen > 0 && tempLen < MAX_PATH);

    std::wstring rootDir = std::wstring(tempBuf) + L"pd_p1_c_" + std::to_wstring(GetCurrentProcessId());
    while (!rootDir.empty() && rootDir.back() == L'\\') rootDir.pop_back();
    deleteTreeW(rootDir);
    TEST_CHECK_VOID(createDirRecursiveW(rootDir));

    const std::wstring dstDll = rootDir + L"\\PDPerfPlugin.dll";
    TEST_CHECK_VOID(CopyFileW(toExtendedPathW(srcDllPath).c_str(),
                              toExtendedPathW(dstDll).c_str(), FALSE) != FALSE);

    HMODULE mod = LoadLibraryW(toExtendedPathW(dstDll).c_str());
    TEST_CHECK_VOID(mod != nullptr);

    auto isAvail  = reinterpret_cast<PFN_IsUpscaleMethodAvailable>(GetProcAddress(mod, "IsUpscaleMethodAvailable"));
    auto getName  = reinterpret_cast<PFN_GetUpscaleMethodName>(GetProcAddress(mod, "GetUpscaleMethodName"));
    auto getSharp = reinterpret_cast<PFN_GetOptimalSharpness>(GetProcAddress(mod, "GetOptimalSharpness"));
    auto setDebug = reinterpret_cast<PFN_SetDebug>(GetProcAddress(mod, "SetDebug"));
    TEST_CHECK_VOID(isAvail && getName && getSharp && setDebug);

    HANDLE barrier = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    TEST_CHECK_VOID(barrier != nullptr);

    constexpr int kThreads = 16;
    ThreadTestContext ctx[kThreads];
    HANDLE threads[kThreads] = {};
    for (int i = 0; i < kThreads; ++i) {
        ctx[i].barrier  = barrier;
        ctx[i].isAvail  = isAvail;
        ctx[i].getName  = getName;
        ctx[i].getSharp = getSharp;
        ctx[i].setDebug = setDebug;
        threads[i] = CreateThread(nullptr, 0, concurrentWorkerProc, &ctx[i], 0, nullptr);
        TEST_CHECK_VOID(threads[i] != nullptr);
    }

    Sleep(20);
    TEST_CHECK_VOID(SetEvent(barrier) != FALSE);
    TEST_CHECK_VOID(WaitForMultipleObjects(kThreads, threads, TRUE, 15000) == WAIT_OBJECT_0);
    for (int i = 0; i < kThreads; ++i) {
        CloseHandle(threads[i]);
        TEST_CHECK_VOID(ctx[i].ok);
    }
    CloseHandle(barrier);

    TEST_CHECK_VOID(FreeLibrary(mod) != FALSE);

    const std::wstring logPath = rootDir + L"\\pd-perfmod-fsr4.log";
    std::vector<uint8_t> logBytes;
    TEST_CHECK_VOID(readAllBytesW(logPath, logBytes));
    std::string logStr(logBytes.begin(), logBytes.end());

    int loadedCount = 0;
    size_t pos = 0;
    const std::string needle = "pd-perfmod-fsr4 loaded";
    while ((pos = logStr.find(needle, pos)) != std::string::npos) {
        ++loadedCount;
        pos += needle.size();
    }
    TEST_CHECK_VOID(loadedCount == 1);

    deleteTreeW(rootDir);
    printf("[Test C] PASSED (16 concurrent threads, 1 init banner in log).\n");
    return true;
}

static bool writeBytesW(const std::wstring& path, const std::string& content) {
    const std::wstring ext = toExtendedPathW(path);
    HANDLE hFile = CreateFileW(ext.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    TEST_CHECK_VOID(hFile != INVALID_HANDLE_VALUE);
    DWORD written = 0;
    const BOOL ok = content.empty() ||
        (WriteFile(hFile, content.data(), static_cast<DWORD>(content.size()), &written, nullptr) &&
         written == content.size());
    CloseHandle(hFile);
    return ok != FALSE;
}

static std::wstring buildExactLenShortSegDirW(const std::wstring& baseDir, size_t targetLen) {
    std::wstring dir = baseDir;
    int idx = 0;
    while (dir.size() + 10 < targetLen) {
        wchar_t seg[16];
        swprintf(seg, 16, L"\\d%06d", idx++);
        dir += seg;
    }
    size_t rem = (targetLen > dir.size()) ? (targetLen - dir.size()) : 0;
    if (rem == 10) {
        dir += L"\\a123";
        rem -= 5;
    }
    if (rem >= 2 && rem <= 9) {
        dir.push_back(L'\\');
        dir.append(rem - 1, L'x');
    }
    return dir;
}

using PFN_InitUpscaler = void* (__stdcall*)(void*);

static bool testBoundary241AndInitOrder(const std::wstring& srcDllPath) {
    printf("[Test D] Verifying 229..259-char dllDir INI defaults + InitUpscaler(nullptr) log order...\n");
    wchar_t tempBuf[MAX_PATH] = {};
    const DWORD tempLen = GetTempPathW(MAX_PATH, tempBuf);
    TEST_CHECK_VOID(tempLen > 0 && tempLen < MAX_PATH);

    std::wstring rootDir = std::wstring(tempBuf) + L"pd_p1_d_" + std::to_wstring(GetCurrentProcessId());
    while (!rootDir.empty() && rootDir.back() == L'\\') rootDir.pop_back();
    deleteTreeW(rootDir);
    TEST_CHECK_VOID(rootDir.size() < 220);

    const std::wstring deepDir = buildExactLenShortSegDirW(rootDir, 241);
    TEST_CHECK_VOID(deepDir.size() >= 229 && deepDir.size() <= 259);
    TEST_CHECK_VOID(createDirRecursiveW(deepDir));

    const std::wstring dstDll  = deepDir + L"\\PDPerfPlugin.dll";
    const std::wstring iniPath = deepDir + L"\\pd-perfmod-fsr4.ini";
    const std::wstring logPath = deepDir + L"\\pd-perfmod-fsr4.log";
    TEST_CHECK_VOID(dstDll.size() < MAX_PATH);
    TEST_CHECK_VOID(iniPath.size() >= MAX_PATH);
    TEST_CHECK_VOID(logPath.size() >= MAX_PATH);

    TEST_CHECK_VOID(CopyFileW(srcDllPath.c_str(), dstDll.c_str(), FALSE) != FALSE);
    TEST_CHECK_VOID(writeBytesW(iniPath, ""));

    // 1) Empty INI in 241-char dir + InitUpscaler(nullptr) first -> defaults to NON-VERBOSE while Logging::error still writes
    HMODULE mod = LoadLibraryW(dstDll.c_str());
    TEST_CHECK_VOID(mod != nullptr);
    auto initUpscaler = reinterpret_cast<PFN_InitUpscaler>(GetProcAddress(mod, "InitUpscaler"));
    TEST_CHECK_VOID(initUpscaler != nullptr);
    TEST_CHECK_VOID(initUpscaler(nullptr) == nullptr);
    TEST_CHECK_VOID(FreeLibrary(mod) != FALSE);

    std::vector<uint8_t> logBytes;
    TEST_CHECK_VOID(readAllBytesW(logPath, logBytes));
    std::string logStr(logBytes.begin(), logBytes.end());
    TEST_CHECK_VOID(logStr.find("mode: NON-VERBOSE") != std::string::npos);
    TEST_CHECK_VOID(logStr.find("InitUpscaler: NULL params") != std::string::npos);

    // 2) Explicit Verbose=1 in 241-char dir -> mode: VERBOSE
    TEST_CHECK_VOID(writeBytesW(iniPath, "[Logging]\nVerbose=1\n"));
    mod = LoadLibraryW(dstDll.c_str());
    TEST_CHECK_VOID(mod != nullptr);
    auto isAvail = reinterpret_cast<PFN_IsUpscaleMethodAvailable>(GetProcAddress(mod, "IsUpscaleMethodAvailable"));
    TEST_CHECK_VOID(isAvail != nullptr);
    TEST_CHECK_VOID(isAvail(1) == true);
    TEST_CHECK_VOID(FreeLibrary(mod) != FALSE);

    logBytes.clear();
    TEST_CHECK_VOID(readAllBytesW(logPath, logBytes));
    logStr.assign(logBytes.begin(), logBytes.end());
    TEST_CHECK_VOID(logStr.find("mode: VERBOSE") != std::string::npos);

    deleteTreeW(rootDir);
    printf("[Test D] PASSED (dir=%zu, ini=%zu, empty-INI NON-VERBOSE + Verbose=1 + InitUpscaler(nullptr)).\n",
           deepDir.size(), iniPath.size());
    return true;
}

int wmain(int argc, wchar_t** argv) {
    TEST_CHECK(argc >= 2);
    const std::wstring dllPath = normalizeAbsPathW(argv[1]);
    TEST_CHECK(!dllPath.empty());
    TEST_CHECK(GetFileAttributesW(toExtendedPathW(dllPath).c_str()) != INVALID_FILE_ATTRIBUTES);

    TEST_CHECK(testPeImportsAndExports(dllPath));
    TEST_CHECK(testLongNonAsciiPath(dllPath));
    TEST_CHECK(testConcurrentInit(dllPath));
    TEST_CHECK(testBoundary241AndInitOrder(dllPath));

    printf("ALL PHASE 1 STARTUP TESTS PASSED.\n");
    return 0;
}
