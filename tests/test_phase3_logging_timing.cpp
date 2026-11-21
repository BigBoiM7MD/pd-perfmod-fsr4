#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "PDPerfPlugin.h"
#include "mock_d3d12.h"
#include "mock_ffx_abi.h"

#define TEST_CHECK(cond)                                                              \
    do {                                                                              \
        if (!(cond)) {                                                                \
            fprintf(stderr, "FAIL %s:%d: %s (gle=%lu)\n", __FILE__, __LINE__, #cond,  \
                    GetLastError());                                                  \
            return 1;                                                                 \
        }                                                                             \
    } while (0)

#define TEST_CHECK_BOOL(cond)                                                         \
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

static bool writeBytesW(const std::wstring& path, const std::string& content) {
    const std::wstring ext = toExtendedPathW(path);
    HANDLE hFile = CreateFileW(ext.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    TEST_CHECK_BOOL(hFile != INVALID_HANDLE_VALUE);
    DWORD written = 0;
    const BOOL ok = content.empty() ||
        (WriteFile(hFile, content.data(), static_cast<DWORD>(content.size()), &written, nullptr) &&
         written == content.size());
    CloseHandle(hFile);
    return ok != FALSE;
}

static bool readAllTextW(const std::wstring& path, std::string& outText) {
    const std::wstring ext = toExtendedPathW(path);
    HANDLE hFile = CreateFileW(ext.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    TEST_CHECK_BOOL(hFile != INVALID_HANDLE_VALUE);
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(hFile, &sz) || sz.QuadPart < 0 || sz.QuadPart > 64 * 1024 * 1024) {
        CloseHandle(hFile);
        return false;
    }
    outText.assign(static_cast<size_t>(sz.QuadPart), '\0');
    DWORD totalRead = 0;
    while (totalRead < outText.size()) {
        DWORD chunk = 0;
        if (!ReadFile(hFile, &outText[totalRead],
                      static_cast<DWORD>(outText.size() - totalRead), &chunk, nullptr) ||
            chunk == 0) {
            CloseHandle(hFile);
            return false;
        }
        totalRead += chunk;
    }
    CloseHandle(hFile);
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

static void clearMockEnvVars() {
    SetEnvironmentVariableA("PD_MOCK_FFX_VERSIONS", nullptr);
    SetEnvironmentVariableA("PD_MOCK_FFX_CREATE_FAIL", nullptr);
    SetEnvironmentVariableA("PD_MOCK_FFX_FAIL_VERSION", nullptr);
    SetEnvironmentVariableA("PD_MOCK_FFX_DISPATCH_FAIL", nullptr);
    SetEnvironmentVariableA("PD_MOCK_FFX_CRASH_DISPATCH", nullptr);
}

static bool assertZeroComLeaks() {
    TEST_CHECK_BOOL(MockD3D12Device::liveDevices.load(std::memory_order_acquire) == 0);
    TEST_CHECK_BOOL(MockD3D12Device::liveQueues.load(std::memory_order_acquire) == 0);
    TEST_CHECK_BOOL(MockD3D12Device::liveResources.load(std::memory_order_acquire) == 0);
    TEST_CHECK_BOOL(MockD3D12Device::liveAllocators.load(std::memory_order_acquire) == 0);
    TEST_CHECK_BOOL(MockD3D12Device::liveLists.load(std::memory_order_acquire) == 0);
    TEST_CHECK_BOOL(MockD3D12Device::liveFences.load(std::memory_order_acquire) == 0);
    TEST_CHECK_BOOL(MockD3D12Device::liveDescriptorHeaps.load(std::memory_order_acquire) == 0);
    TEST_CHECK_BOOL(MockD3D12Device::liveRootSignatures.load(std::memory_order_acquire) == 0);
    TEST_CHECK_BOOL(MockD3D12Device::livePipelineStates.load(std::memory_order_acquire) == 0);
    return true;
}

struct PluginApi {
    HMODULE mod = nullptr;
    bool (__stdcall* SetupDirectX)(void*, int) = nullptr;
    void* (__stdcall* InitUpscaler)(InitParams*) = nullptr;
    void (__stdcall* EvaluateUpscaler)(UpscaleParams*) = nullptr;
    void (__stdcall* ReleaseUpscaleFeature)(int) = nullptr;
    void (__stdcall* InitLogDelegate)(void (*)(char*, int)) = nullptr;
    bool (__stdcall* IsUpscaleMethodAvailable)(int) = nullptr;

    bool load(const std::wstring& dllPath) {
        mod = LoadLibraryW(dllPath.c_str());
        TEST_CHECK_BOOL(mod != nullptr);

        SetupDirectX             = reinterpret_cast<decltype(SetupDirectX)>(GetProcAddress(mod, "SetupDirectX"));
        InitUpscaler             = reinterpret_cast<decltype(InitUpscaler)>(GetProcAddress(mod, "InitUpscaler"));
        EvaluateUpscaler         = reinterpret_cast<decltype(EvaluateUpscaler)>(GetProcAddress(mod, "EvaluateUpscaler"));
        ReleaseUpscaleFeature    = reinterpret_cast<decltype(ReleaseUpscaleFeature)>(GetProcAddress(mod, "ReleaseUpscaleFeature"));
        InitLogDelegate          = reinterpret_cast<decltype(InitLogDelegate)>(GetProcAddress(mod, "InitLogDelegate"));
        IsUpscaleMethodAvailable = reinterpret_cast<decltype(IsUpscaleMethodAvailable)>(GetProcAddress(mod, "IsUpscaleMethodAvailable"));

        TEST_CHECK_BOOL(SetupDirectX && InitUpscaler && EvaluateUpscaler &&
                        ReleaseUpscaleFeature && InitLogDelegate && IsUpscaleMethodAvailable);
        return true;
    }

    void unload() {
        if (mod) {
            FreeLibrary(mod);
            mod = nullptr;
        }
    }
};

struct MockFfxInspector {
    static HMODULE getLoadedLoader() {
        return GetModuleHandleW(L"amd_fidelityfx_loader_dx12.dll");
    }

    static bool reset() {
        HMODULE m = getLoadedLoader();
        TEST_CHECK_BOOL(m != nullptr);
        auto fn = reinterpret_cast<PFN_MockFfxResetState>(GetProcAddress(m, "MockFfxResetState"));
        TEST_CHECK_BOOL(fn != nullptr);
        fn();
        return true;
    }

    static bool getStats(MockFfxStats& out) {
        HMODULE m = getLoadedLoader();
        TEST_CHECK_BOOL(m != nullptr);
        auto fn = reinterpret_cast<PFN_MockFfxGetStats>(GetProcAddress(m, "MockFfxGetStats"));
        TEST_CHECK_BOOL(fn != nullptr);
        fn(&out);
        return true;
    }

    static bool getDispatchRecord(uint32_t index, MockFfxDispatchDescUpscale& out) {
        HMODULE m = getLoadedLoader();
        TEST_CHECK_BOOL(m != nullptr);
        auto fn = reinterpret_cast<PFN_MockFfxGetDispatchRecord>(GetProcAddress(m, "MockFfxGetDispatchRecord"));
        TEST_CHECK_BOOL(fn != nullptr);
        return fn(index, &out) != 0;
    }
};

static MockD3D12Resource* makeTex2D(ID3D12Device* dev, UINT64 width, UINT height,
                                    DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width              = width;
    desc.Height             = height;
    desc.DepthOrArraySize   = 1;
    desc.MipLevels          = 1;
    desc.Format             = format;
    desc.SampleDesc.Count   = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags              = flags;
    return new MockD3D12Resource(dev, desc);
}

static UpscaleParams makeEvalParams(int id, void* color, void* mv, void* depth,
                                    void* dst, float rw, float rh, bool execute, void* cmdList) {
    UpscaleParams p{};
    p.id            = id;
    p.color         = color;
    p.motionVector  = mv;
    p.depth         = depth;
    p.destination   = dst;
    p.renderSizeX   = rw;
    p.renderSizeY   = rh;
    p.sharpness     = 0.0f;
    p.jitterOffsetX = 0.25f;
    p.jitterOffsetY = -0.25f;
    p.motionScaleX  = 1.0f;
    p.motionScaleY  = 1.0f;
    p.reset         = false;
    p.nearPlane     = 0.1f;
    p.farPlane      = 1000.0f;
    p.verticalFOV   = 1.0f;
    p.execute       = execute;
    p.cmdList       = cmdList;
    return p;
}

// ---------------------------------------------------------------------------
// Case 1 delegate capture & fault-injection callbacks (never hold locks across
// throw / RaiseException / plugin re-entry)
// ---------------------------------------------------------------------------
static SRWLOCK g_captureLock = SRWLOCK_INIT;
static std::vector<std::string> g_capturedMessages;
static std::atomic<bool> g_contractViolated{false};

static void resetCapturedMessages() {
    AcquireSRWLockExclusive(&g_captureLock);
    g_capturedMessages.clear();
    ReleaseSRWLockExclusive(&g_captureLock);
    g_contractViolated.store(false, std::memory_order_release);
}

static std::vector<std::string> snapshotCapturedMessages() {
    AcquireSRWLockShared(&g_captureLock);
    std::vector<std::string> copy = g_capturedMessages;
    ReleaseSRWLockShared(&g_captureLock);
    return copy;
}

static void captureDelegate(char* msg, int iSize) {
    if (!msg || iSize <= 0) {
        g_contractViolated.store(true, std::memory_order_release);
        return;
    }
    const size_t len = strlen(msg);
    if (static_cast<size_t>(iSize) != len ||
        msg[len - 1] == '\n' || msg[len - 1] == '\r') {
        g_contractViolated.store(true, std::memory_order_release);
    }

    AcquireSRWLockExclusive(&g_captureLock);
    g_capturedMessages.emplace_back(msg, len);
    ReleaseSRWLockExclusive(&g_captureLock);
}

static std::atomic<bool> g_reenteredOnce{false};
static std::atomic<int>  g_reentrantCalls{0};
static void* (__stdcall* g_reentrantInitUpscaler)(InitParams*) = nullptr;
static void (__stdcall* g_reentrantInitLogDelegate)(void (*)(char*, int)) = nullptr;

static void reentrantDelegate(char* msg, int iSize) {
    (void)msg;
    (void)iSize;
    g_reentrantCalls.fetch_add(1, std::memory_order_acq_rel);
    if (!g_reenteredOnce.exchange(true, std::memory_order_acq_rel)) {
        if (g_reentrantInitUpscaler) {
            g_reentrantInitUpscaler(nullptr);
        }
    } else {
        if (g_reentrantInitLogDelegate) {
            g_reentrantInitLogDelegate(nullptr);
        }
    }
}

static std::atomic<int> g_cppThrowCalls{0};
static void throwingCppDelegate(char* msg, int iSize) {
    (void)msg;
    (void)iSize;
    g_cppThrowCalls.fetch_add(1, std::memory_order_acq_rel);
    throw std::runtime_error("delegate cpp throw");
}

static std::atomic<int> g_sehFaultCalls{0};
static void sehFaultDelegate(char* msg, int iSize) {
    (void)msg;
    (void)iSize;
    g_sehFaultCalls.fetch_add(1, std::memory_order_acq_rel);
    RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
}

static bool containsMessageSubstr(const std::vector<std::string>& msgs, const char* substr) {
    for (const auto& m : msgs) {
        if (m.find(substr) != std::string::npos) {
            return true;
        }
    }
    return false;
}

static bool containsMessageBoth(const std::vector<std::string>& msgs, const char* sub1, const char* sub2) {
    for (const auto& m : msgs) {
        if (m.find(sub1) != std::string::npos && m.find(sub2) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Case 1: FIX-16 InitLogDelegate forwarding, contract, nullptr reset,
//         re-entrancy, C++ throw & SEH fault survival
// ---------------------------------------------------------------------------
static bool runLogDelegateCase(const std::wstring& srcPluginDll,
                               const std::wstring& srcMockLoaderDll,
                               const std::wstring& tempRoot) {
    printf("[Case 1] Testing FIX-16 InitLogDelegate forwarding, re-entrancy & C++/SEH survival...\n");
    const std::wstring dir = tempRoot + L"\\case1_delegate";
    deleteTreeW(dir);
    TEST_CHECK_BOOL(CreateDirectoryW(dir.c_str(), nullptr) != FALSE);

    const std::wstring dstPlugin = dir + L"\\PDPerfPlugin.dll";
    const std::wstring dstLoader = dir + L"\\amd_fidelityfx_loader_dx12.dll";
    const std::wstring iniPath   = dir + L"\\pd-perfmod-fsr4.ini";
    const std::wstring logPath   = dir + L"\\pd-perfmod-fsr4.log";
    TEST_CHECK_BOOL(CopyFileW(srcPluginDll.c_str(), dstPlugin.c_str(), FALSE) != FALSE);
    TEST_CHECK_BOOL(CopyFileW(srcMockLoaderDll.c_str(), dstLoader.c_str(), FALSE) != FALSE);
    TEST_CHECK_BOOL(writeBytesW(iniPath, "[Logging]\nVerbose=1\n[Backend]\nRBSwap=0\n"));

    clearMockEnvVars();
    SetEnvironmentVariableA("PD_MOCK_FFX_VERSIONS", "1");

    resetCapturedMessages();

    PluginApi api;
    TEST_CHECK_BOOL(api.load(dstPlugin));

    // Register delegate BEFORE any other export so initial loaded banner is captured
    api.InitLogDelegate(&captureDelegate);
    {
        const auto msgs = snapshotCapturedMessages();
        TEST_CHECK_BOOL(!g_contractViolated.load(std::memory_order_acquire));
        TEST_CHECK_BOOL(containsMessageSubstr(msgs, "pd-perfmod-fsr4 loaded (mode: VERBOSE)"));
        TEST_CHECK_BOOL(!containsMessageSubstr(msgs, "pd-perfmod-fsr4 initializing..."));
    }

    auto* dev   = new MockD3D12Device();
    auto* queue = new MockD3D12CommandQueue(dev);
    TEST_CHECK_BOOL(api.SetupDirectX(queue, 1) == true);
    TEST_CHECK_BOOL(api.InitUpscaler(nullptr) == nullptr);

    {
        const auto msgs = snapshotCapturedMessages();
        TEST_CHECK_BOOL(!g_contractViolated.load(std::memory_order_acquire));
        TEST_CHECK_BOOL(containsMessageSubstr(msgs, "pd-perfmod-fsr4 loaded"));
        TEST_CHECK_BOOL(containsMessageSubstr(msgs, "FSR version:"));
        TEST_CHECK_BOOL(containsMessageBoth(msgs, "[ERROR] ", "InitUpscaler: NULL params"));
        // Logging::info lines must NEVER be forwarded to REFramework's delegate
        TEST_CHECK_BOOL(!containsMessageSubstr(msgs, "pd-perfmod-fsr4 initializing..."));
        TEST_CHECK_BOOL(!containsMessageSubstr(msgs, "FSR4Backend::setup("));
    }

    // Re-entrancy: delegate triggers nested InitUpscaler(nullptr) and then InitLogDelegate(nullptr)
    g_reenteredOnce.store(false, std::memory_order_release);
    g_reentrantCalls.store(0, std::memory_order_release);
    g_reentrantInitUpscaler    = api.InitUpscaler;
    g_reentrantInitLogDelegate = api.InitLogDelegate;
    api.InitLogDelegate(&reentrantDelegate);
    TEST_CHECK_BOOL(api.InitUpscaler(nullptr) == nullptr);
    TEST_CHECK_BOOL(g_reentrantCalls.load(std::memory_order_acquire) == 2);
    g_reentrantInitUpscaler    = nullptr;
    g_reentrantInitLogDelegate = nullptr;

    // Explicit nullptr reset stops forwarding to captureDelegate
    api.InitLogDelegate(&captureDelegate);
    api.InitLogDelegate(nullptr);
    const size_t countBeforeNull = snapshotCapturedMessages().size();
    TEST_CHECK_BOOL(api.InitUpscaler(nullptr) == nullptr);
    TEST_CHECK_BOOL(snapshotCapturedMessages().size() == countBeforeNull);

    // C++ exception thrown inside host delegate must be caught by PDPerfPlugin (/EHa)
    g_cppThrowCalls.store(0, std::memory_order_release);
    api.InitLogDelegate(&throwingCppDelegate);
    TEST_CHECK_BOOL(api.InitUpscaler(nullptr) == nullptr);
    TEST_CHECK_BOOL(g_cppThrowCalls.load(std::memory_order_acquire) == 1);

    // Win32 SEH fault raised inside host delegate must be caught by PDPerfPlugin (/EHa)
    g_sehFaultCalls.store(0, std::memory_order_release);
    api.InitLogDelegate(&sehFaultDelegate);
    TEST_CHECK_BOOL(api.InitUpscaler(nullptr) == nullptr);
    TEST_CHECK_BOOL(g_sehFaultCalls.load(std::memory_order_acquire) == 1);

    // Re-register captureDelegate and confirm s_lock was never leaked/deadlocked
    api.InitLogDelegate(&captureDelegate);
    const size_t countAfterFaults = snapshotCapturedMessages().size();
    TEST_CHECK_BOOL(api.InitUpscaler(nullptr) == nullptr);
    {
        const auto msgs = snapshotCapturedMessages();
        TEST_CHECK_BOOL(!g_contractViolated.load(std::memory_order_acquire));
        TEST_CHECK_BOOL(msgs.size() == countAfterFaults + 1);
        TEST_CHECK_BOOL(msgs.back().find("[ERROR] ") != std::string::npos);
        TEST_CHECK_BOOL(msgs.back().find("InitUpscaler: NULL params") != std::string::npos);
    }

    // Teardown & zero COM leak verification
    api.InitLogDelegate(nullptr);
    TEST_CHECK_BOOL(api.SetupDirectX(nullptr, 1) == false);
    queue->Release();
    dev->Release();
    TEST_CHECK_BOOL(assertZeroComLeaks());
    TEST_CHECK_BOOL(MockFfxInspector::getLoadedLoader() == nullptr);
    api.unload();

    // Verify Verbose=1 Logging::info lines DID hit disk even though excluded from delegate
    std::string logText;
    TEST_CHECK_BOOL(readAllTextW(logPath, logText));
    TEST_CHECK_BOOL(logText.find("pd-perfmod-fsr4 initializing...") != std::string::npos);
    TEST_CHECK_BOOL(logText.find("FSR4Backend::setup(") != std::string::npos);

    deleteTreeW(dir);
    printf("[Case 1] PASSED (delegate forwarding, contract, re-entrancy, nullptr, C++/SEH survival verified).\n");
    return true;
}

// ---------------------------------------------------------------------------
// Case 2: FIX-18 Real QueryPerformanceCounter dd.frameTimeDelta & per-context
//         independence + clamping [0.1f, 250.0f] ms
// ---------------------------------------------------------------------------
static bool runQpcFrameTimeDeltaCase(const std::wstring& srcPluginDll,
                                     const std::wstring& srcMockLoaderDll,
                                     const std::wstring& tempRoot) {
    printf("[Case 2] Testing FIX-18 per-context QPC frameTimeDelta & [0.1, 250.0] ms clamping...\n");
    const std::wstring dir = tempRoot + L"\\case2_qpc_timing";
    deleteTreeW(dir);
    TEST_CHECK_BOOL(CreateDirectoryW(dir.c_str(), nullptr) != FALSE);

    const std::wstring dstPlugin = dir + L"\\PDPerfPlugin.dll";
    const std::wstring dstLoader = dir + L"\\amd_fidelityfx_loader_dx12.dll";
    const std::wstring iniPath   = dir + L"\\pd-perfmod-fsr4.ini";
    TEST_CHECK_BOOL(CopyFileW(srcPluginDll.c_str(), dstPlugin.c_str(), FALSE) != FALSE);
    TEST_CHECK_BOOL(CopyFileW(srcMockLoaderDll.c_str(), dstLoader.c_str(), FALSE) != FALSE);
    TEST_CHECK_BOOL(writeBytesW(iniPath, "[Logging]\nVerbose=0\n[Backend]\nRBSwap=0\n"));

    clearMockEnvVars();
    SetEnvironmentVariableA("PD_MOCK_FFX_VERSIONS", "1");

    PluginApi api;
    TEST_CHECK_BOOL(api.load(dstPlugin));

    auto* dev   = new MockD3D12Device();
    auto* queue = new MockD3D12CommandQueue(dev);
    TEST_CHECK_BOOL(api.SetupDirectX(queue, 1) == true);
    TEST_CHECK_BOOL(MockFfxInspector::reset());

    InitParams ip1{};
    ip1.id            = 1;
    ip1.upscaleMethod = 1;
    ip1.qualityLevel  = 2;
    ip1.displaySizeX  = 1920;
    ip1.displaySizeY  = 1080;
    ip1.format        = DXGI_FORMAT_R8G8B8A8_UNORM;

    InitParams ip2 = ip1;
    ip2.id = 2;

    void* out1 = api.InitUpscaler(&ip1);
    void* out2 = api.InitUpscaler(&ip2);
    TEST_CHECK_BOOL(out1 != nullptr && out2 != nullptr && out1 != out2);

    auto* colorTex = makeTex2D(dev, 1280, 720, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE);
    auto* depthTex = makeTex2D(dev, 1280, 720, DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    auto* mvTex    = makeTex2D(dev, 1280, 720, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE);

    UpscaleParams eval1 = makeEvalParams(1, colorTex, mvTex, depthTex, nullptr, 1280.0f, 720.0f, true, nullptr);
    UpscaleParams eval2 = makeEvalParams(2, colorTex, mvTex, depthTex, nullptr, 1280.0f, 720.0f, true, nullptr);

    // Record 0: first dispatch of id=1 -> initial default 16.6667f ms
    api.EvaluateUpscaler(&eval1);

    Sleep(35);

    // Record 1: first dispatch of id=2 -> independent context, initial default 16.6667f ms
    api.EvaluateUpscaler(&eval2);

    // Record 2: second dispatch of id=1 -> elapsed ms since record 0 (>= 25 ms, <= 250 ms)
    api.EvaluateUpscaler(&eval1);

    // Record 3: immediate second dispatch of id=2 -> independent short delta (>= 0.1 ms, < 25 ms)
    api.EvaluateUpscaler(&eval2);

    Sleep(300);

    // Record 4: third dispatch of id=1 after 300ms pause -> clamped to 250.0f ms
    api.EvaluateUpscaler(&eval1);

    MockFfxStats stats{};
    TEST_CHECK_BOOL(MockFfxInspector::getStats(stats));
    TEST_CHECK_BOOL(stats.dispatchCount == 5);

    MockFfxDispatchDescUpscale dd0{}, dd1{}, dd2{}, dd3{}, dd4{};
    TEST_CHECK_BOOL(MockFfxInspector::getDispatchRecord(0, dd0));
    TEST_CHECK_BOOL(MockFfxInspector::getDispatchRecord(1, dd1));
    TEST_CHECK_BOOL(MockFfxInspector::getDispatchRecord(2, dd2));
    TEST_CHECK_BOOL(MockFfxInspector::getDispatchRecord(3, dd3));
    TEST_CHECK_BOOL(MockFfxInspector::getDispatchRecord(4, dd4));

    printf("  QPC deltas: dd0=%.3f dd1=%.3f dd2=%.3f dd3=%.3f dd4=%.3f ms\n",
           dd0.frameTimeDelta, dd1.frameTimeDelta, dd2.frameTimeDelta,
           dd3.frameTimeDelta, dd4.frameTimeDelta);

    TEST_CHECK_BOOL(dd0.frameTimeDelta > 16.5f && dd0.frameTimeDelta < 16.8f);
    TEST_CHECK_BOOL(dd1.frameTimeDelta > 16.5f && dd1.frameTimeDelta < 16.8f);
    TEST_CHECK_BOOL(dd2.frameTimeDelta >= 25.0f && dd2.frameTimeDelta <= 250.0f);
    TEST_CHECK_BOOL(dd3.frameTimeDelta >= 0.1f && dd3.frameTimeDelta < 25.0f);
    TEST_CHECK_BOOL(dd4.frameTimeDelta >= 249.9f && dd4.frameTimeDelta <= 250.1f);

    // Clean teardown & COM refcount check
    colorTex->Release();
    depthTex->Release();
    mvTex->Release();
    api.ReleaseUpscaleFeature(1);
    api.ReleaseUpscaleFeature(2);
    TEST_CHECK_BOOL(api.SetupDirectX(nullptr, 1) == false);
    queue->Release();
    dev->Release();
    TEST_CHECK_BOOL(assertZeroComLeaks());
    TEST_CHECK_BOOL(MockFfxInspector::getLoadedLoader() == nullptr);
    api.unload();

    deleteTreeW(dir);
    printf("[Case 2] PASSED (initial 16.67ms, Sleep(35), independent id=1/id=2, Sleep(300) clamp verified).\n");
    return true;
}

// ---------------------------------------------------------------------------
// Case 3: FIX-18 Verbose=0 auto-creation default & Verbose=1 override
// ---------------------------------------------------------------------------
static bool runIniDefaultCase(const std::wstring& srcPluginDll,
                              const std::wstring& tempRoot) {
    printf("[Case 3] Testing FIX-18 Verbose=0 auto-creation default & Verbose=1 override...\n");
    const std::wstring dir = tempRoot + L"\\case3_ini_default";
    deleteTreeW(dir);
    TEST_CHECK_BOOL(CreateDirectoryW(dir.c_str(), nullptr) != FALSE);

    const std::wstring dstPlugin = dir + L"\\PDPerfPlugin.dll";
    const std::wstring iniPath   = dir + L"\\pd-perfmod-fsr4.ini";
    const std::wstring logPath   = dir + L"\\pd-perfmod-fsr4.log";
    TEST_CHECK_BOOL(CopyFileW(srcPluginDll.c_str(), dstPlugin.c_str(), FALSE) != FALSE);

    // Subcase 3a: No INI present -> auto-creates with Verbose=0 and runs in NON-VERBOSE mode
    {
        PluginApi api;
        TEST_CHECK_BOOL(api.load(dstPlugin));
        TEST_CHECK_BOOL(api.IsUpscaleMethodAvailable(1) == true);
        api.unload();

        std::string iniText;
        TEST_CHECK_BOOL(readAllTextW(iniPath, iniText));
        TEST_CHECK_BOOL(iniText.find("Verbose=0") != std::string::npos);

        std::string logText;
        TEST_CHECK_BOOL(readAllTextW(logPath, logText));
        TEST_CHECK_BOOL(logText.find("mode: NON-VERBOSE") != std::string::npos);
        TEST_CHECK_BOOL(logText.find("pd-perfmod-fsr4 initializing...") == std::string::npos);
    }

    // Subcase 3b: Explicit Verbose=1 -> runs in VERBOSE mode and logs info lines
    {
        TEST_CHECK_BOOL(writeBytesW(iniPath, "[Logging]\nVerbose=1\n"));
        PluginApi api;
        TEST_CHECK_BOOL(api.load(dstPlugin));
        TEST_CHECK_BOOL(api.IsUpscaleMethodAvailable(1) == true);
        api.unload();

        std::string logText;
        TEST_CHECK_BOOL(readAllTextW(logPath, logText));
        TEST_CHECK_BOOL(logText.find("mode: VERBOSE") != std::string::npos);
        TEST_CHECK_BOOL(logText.find("pd-perfmod-fsr4 initializing...") != std::string::npos);
    }

    deleteTreeW(dir);
    printf("[Case 3] PASSED (auto-created Verbose=0 NON-VERBOSE + Verbose=1 VERBOSE verified).\n");
    return true;
}

int wmain(int argc, wchar_t** argv) {
    TEST_CHECK(argc >= 3);
    const std::wstring pluginDll     = normalizeAbsPathW(argv[1]);
    const std::wstring mockLoaderDll = normalizeAbsPathW(argv[2]);
    TEST_CHECK(!pluginDll.empty() && !mockLoaderDll.empty());
    TEST_CHECK(GetFileAttributesW(toExtendedPathW(pluginDll).c_str()) != INVALID_FILE_ATTRIBUTES);
    TEST_CHECK(GetFileAttributesW(toExtendedPathW(mockLoaderDll).c_str()) != INVALID_FILE_ATTRIBUTES);

    wchar_t tempBuf[MAX_PATH] = {};
    const DWORD tempLen = GetTempPathW(MAX_PATH, tempBuf);
    TEST_CHECK(tempLen > 0 && tempLen < MAX_PATH);
    std::wstring tempRoot = std::wstring(tempBuf) + L"pd_p3_lt_" + std::to_wstring(GetCurrentProcessId());
    while (!tempRoot.empty() && tempRoot.back() == L'\\') tempRoot.pop_back();
    deleteTreeW(tempRoot);
    TEST_CHECK(CreateDirectoryW(tempRoot.c_str(), nullptr) != FALSE);

    const bool ok1 = runLogDelegateCase(pluginDll, mockLoaderDll, tempRoot);
    const bool ok2 = ok1 && runQpcFrameTimeDeltaCase(pluginDll, mockLoaderDll, tempRoot);
    const bool ok3 = ok2 && runIniDefaultCase(pluginDll, tempRoot);

    deleteTreeW(tempRoot);
    TEST_CHECK(ok1 && ok2 && ok3);

    printf("ALL PHASE 3 LOGGING & TIMING TESTS PASSED.\n");
    return 0;
}
