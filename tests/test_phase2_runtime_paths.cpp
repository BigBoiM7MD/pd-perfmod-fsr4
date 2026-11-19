#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <cstring>
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

struct PluginApi {
    HMODULE mod = nullptr;
    bool (__stdcall* SetupDirectX)(void*, int) = nullptr;
    void* (__stdcall* SimpleInit)(int, int, int, int, int, bool, bool, bool, bool, bool, bool, int) = nullptr;
    void* (__stdcall* InitUpscaler)(InitParams*) = nullptr;
    void (__stdcall* SimpleEvaluate)(int, void*, void*, void*, void*, void*, int, int, float,
                                     float, float, int, int, bool, float, float, float, bool) = nullptr;
    void (__stdcall* EvaluateUpscaler)(UpscaleParams*) = nullptr;
    int (__stdcall* GetRenderWidth)(int) = nullptr;
    int (__stdcall* GetRenderHeight)(int) = nullptr;
    void (__stdcall* ReleaseUpscaleFeature)(int) = nullptr;

    bool load(const std::wstring& dllPath) {
        mod = LoadLibraryW(dllPath.c_str());
        TEST_CHECK_BOOL(mod != nullptr);

        SetupDirectX          = reinterpret_cast<decltype(SetupDirectX)>(GetProcAddress(mod, "SetupDirectX"));
        SimpleInit            = reinterpret_cast<decltype(SimpleInit)>(GetProcAddress(mod, "SimpleInit"));
        InitUpscaler          = reinterpret_cast<decltype(InitUpscaler)>(GetProcAddress(mod, "InitUpscaler"));
        SimpleEvaluate        = reinterpret_cast<decltype(SimpleEvaluate)>(GetProcAddress(mod, "SimpleEvaluate"));
        EvaluateUpscaler      = reinterpret_cast<decltype(EvaluateUpscaler)>(GetProcAddress(mod, "EvaluateUpscaler"));
        GetRenderWidth        = reinterpret_cast<decltype(GetRenderWidth)>(GetProcAddress(mod, "GetRenderWidth"));
        GetRenderHeight       = reinterpret_cast<decltype(GetRenderHeight)>(GetProcAddress(mod, "GetRenderHeight"));
        ReleaseUpscaleFeature = reinterpret_cast<decltype(ReleaseUpscaleFeature)>(GetProcAddress(mod, "ReleaseUpscaleFeature"));

        TEST_CHECK_BOOL(SetupDirectX && SimpleInit && InitUpscaler &&
                        SimpleEvaluate && EvaluateUpscaler &&
                        GetRenderWidth && GetRenderHeight &&
                        ReleaseUpscaleFeature);
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

    static bool getCreateRecord(uint32_t index, MockFfxCreateRecord& out) {
        HMODULE m = getLoadedLoader();
        TEST_CHECK_BOOL(m != nullptr);
        auto fn = reinterpret_cast<PFN_MockFfxGetCreateRecord>(GetProcAddress(m, "MockFfxGetCreateRecord"));
        TEST_CHECK_BOOL(fn != nullptr);
        return fn(index, &out) != 0;
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

static bool runEvaluationAndVrMultipassCase(const std::wstring& srcPluginDll,
                                            const std::wstring& srcMockLoaderDll,
                                            const std::wstring& tempRoot) {
    printf("[Case 1] Testing FIX-12/FIX-10/FIX-08 SEH guards, input validation, external cmdList & VR execute=false...\n");
    const std::wstring dir = tempRoot + L"\\case1_eval_vr";
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

    MockD3D12GraphicsCommandList* cmdList = dev->lastCreatedCommandList;
    TEST_CHECK_BOOL(cmdList != nullptr);

    InitParams ip1{};
    ip1.id            = 1;
    ip1.upscaleMethod = 1;
    ip1.qualityLevel  = 2;
    ip1.displaySizeX  = 1920;
    ip1.displaySizeY  = 1080;
    ip1.format        = DXGI_FORMAT_R8G8B8A8_UNORM;
    TEST_CHECK_BOOL(api.InitUpscaler(&ip1) != nullptr);

    InitParams ip2 = ip1;
    ip2.id = 2;
    TEST_CHECK_BOOL(api.InitUpscaler(&ip2) != nullptr);

    auto* colorRes = makeTex2D(dev, 1280, 720, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto* depthRes = makeTex2D(dev, 1280, 720, DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    auto* mvRes    = makeTex2D(dev, 1280, 720, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE);

    // 1) /EHa SEH & null-input guards must not crash or touch cmdList->Reset()
    api.EvaluateUpscaler(nullptr);
    api.EvaluateUpscaler(reinterpret_cast<UpscaleParams*>(0x1234));

    UpscaleParams nullColor = makeEvalParams(1, nullptr, mvRes, depthRes, nullptr, 1280, 720, true, nullptr);
    UpscaleParams nullDepth = makeEvalParams(1, colorRes, mvRes, nullptr, nullptr, 1280, 720, true, nullptr);
    UpscaleParams nullMv    = makeEvalParams(1, colorRes, nullptr, depthRes, nullptr, 1280, 720, true, nullptr);
    api.EvaluateUpscaler(&nullColor);
    api.EvaluateUpscaler(&nullDepth);
    api.EvaluateUpscaler(&nullMv);

    api.SimpleEvaluate(1, nullptr, mvRes, depthRes, nullptr, nullptr, 1280, 720, 0.0f, 0.0f, 0.0f, 1, 1, false, 0.1f, 1000.0f, 1.0f, true);
    api.SimpleEvaluate(1, colorRes, mvRes, nullptr, nullptr, nullptr, 1280, 720, 0.0f, 0.0f, 0.0f, 1, 1, false, 0.1f, 1000.0f, 1.0f, true);
    api.SimpleEvaluate(1, colorRes, nullptr, depthRes, nullptr, nullptr, 1280, 720, 0.0f, 0.0f, 0.0f, 1, 1, false, 0.1f, 1000.0f, 1.0f, true);

    TEST_CHECK_BOOL(cmdList->resetCount == 0);
    TEST_CHECK_BOOL(queue->executeCount.load() == 0);

    // 2) External cmdList isolation (execute=false and execute=true)
    auto* extCmdList = new MockD3D12GraphicsCommandList(dev);
    TEST_CHECK_BOOL(extCmdList->isOpen == true);

    UpscaleParams extFalse = makeEvalParams(1, colorRes, mvRes, depthRes, nullptr, 1280, 720, false, extCmdList);
    UpscaleParams extTrue  = makeEvalParams(1, colorRes, mvRes, depthRes, nullptr, 1280, 720, true, extCmdList);
    api.EvaluateUpscaler(&extFalse);
    api.EvaluateUpscaler(&extTrue);

    MockFfxStats stats{};
    TEST_CHECK_BOOL(MockFfxInspector::getStats(stats));
    TEST_CHECK_BOOL(stats.dispatchCount == 2);

    MockFfxDispatchDescUpscale ddExt0{};
    MockFfxDispatchDescUpscale ddExt1{};
    TEST_CHECK_BOOL(MockFfxInspector::getDispatchRecord(0, ddExt0));
    TEST_CHECK_BOOL(MockFfxInspector::getDispatchRecord(1, ddExt1));
    TEST_CHECK_BOOL(ddExt0.commandList == extCmdList);
    TEST_CHECK_BOOL(ddExt1.commandList == extCmdList);
    TEST_CHECK_BOOL(extCmdList->isOpen == true);
    TEST_CHECK_BOOL(extCmdList->closeCount == 0);
    TEST_CHECK_BOOL(extCmdList->resetCount == 0);
    TEST_CHECK_BOOL(cmdList->resetCount == 0);
    TEST_CHECK_BOOL(queue->executeCount.load() == 0);
    TEST_CHECK_BOOL(queue->signalCount.load() == 0);

    extCmdList->Release();

    // 3) Internal VR multipass (cmdList == nullptr): Eye 0 (execute=false) + Eye 1 (execute=true)
    // Capture baseline closeCount because SetupDirectX() closed the initial open cmdList once (closeCount == 1).
    const int baseCloseCount = cmdList->closeCount;
    TEST_CHECK_BOOL(baseCloseCount == 1);

    UpscaleParams eye0 = makeEvalParams(1, colorRes, mvRes, depthRes, nullptr, 1280, 720, false, nullptr);
    api.EvaluateUpscaler(&eye0);

    TEST_CHECK_BOOL(MockFfxInspector::getStats(stats));
    TEST_CHECK_BOOL(stats.dispatchCount == 3);
    TEST_CHECK_BOOL(cmdList->resetCount == 1);
    TEST_CHECK_BOOL(cmdList->closeCount == baseCloseCount);
    TEST_CHECK_BOOL(cmdList->isOpen == true);
    TEST_CHECK_BOOL(queue->executeCount.load() == 0);

    UpscaleParams eye1 = makeEvalParams(2, colorRes, mvRes, depthRes, nullptr, 1280, 720, true, nullptr);
    api.EvaluateUpscaler(&eye1);

    TEST_CHECK_BOOL(MockFfxInspector::getStats(stats));
    TEST_CHECK_BOOL(stats.dispatchCount == 4);
    TEST_CHECK_BOOL(cmdList->resetCount == 1);
    TEST_CHECK_BOOL(cmdList->closeCount == baseCloseCount + 1);
    TEST_CHECK_BOOL(cmdList->isOpen == false);
    TEST_CHECK_BOOL(queue->executeCount.load() == 1);
    TEST_CHECK_BOOL(queue->signalCount.load() == 1);

    // 4) Open internal cmdList via execute=false followed immediately by ReleaseUpscaleFeature(1)
    api.EvaluateUpscaler(&eye0);
    TEST_CHECK_BOOL(cmdList->isOpen == true);
    TEST_CHECK_BOOL(cmdList->resetCount == 2);
    api.ReleaseUpscaleFeature(1);
    TEST_CHECK_BOOL(cmdList->isOpen == false);
    TEST_CHECK_BOOL(cmdList->closeCount == baseCloseCount + 2);

    // 5) SimpleEvaluate /EHa SEH guard on corrupt D3D12 resource pointer recovers cmdList cleanly
    api.SimpleEvaluate(2, reinterpret_cast<void*>(0x1234), mvRes, depthRes, nullptr, nullptr,
                       1280, 720, 0.0f, 0.0f, 0.0f, 1, 1, false, 0.1f, 1000.0f, 1.0f, true);
    TEST_CHECK_BOOL(cmdList->isOpen == false);
    TEST_CHECK_BOOL(cmdList->invalidResetCount == 0);
    TEST_CHECK_BOOL(cmdList->invalidCloseCount == 0);

    api.ReleaseUpscaleFeature(2);
    colorRes->Release();
    depthRes->Release();
    mvRes->Release();

    TEST_CHECK_BOOL(api.SetupDirectX(nullptr, 1) == false);
    TEST_CHECK_BOOL(MockD3D12Device::liveResources.load() == 0);
    TEST_CHECK_BOOL(MockD3D12Device::liveAllocators.load() == 0);
    TEST_CHECK_BOOL(MockD3D12Device::liveLists.load() == 0);
    TEST_CHECK_BOOL(MockD3D12Device::liveFences.load() == 0);
    TEST_CHECK_BOOL(dev->refCount() == 1);
    TEST_CHECK_BOOL(queue->refCount() == 1);

    queue->Release();
    dev->Release();
    TEST_CHECK_BOOL(MockD3D12Device::liveQueues.load() == 0);
    TEST_CHECK_BOOL(MockD3D12Device::liveDevices.load() == 0);

    api.unload();
    clearMockEnvVars();
    deleteTreeW(dir);
    printf("[Case 1] PASSED.\n");
    return true;
}

static bool runDlaaAndMultiVersionFallbackCase(const std::wstring& srcPluginDll,
                                               const std::wstring& srcMockLoaderDll,
                                               const std::wstring& tempRoot) {
    printf("[Case 2] Testing FIX-10/FIX-13 DLAA dynamic resolution and sequential multi-version fallback...\n");
    const std::wstring dir = tempRoot + L"\\case2_dlaa_versions";
    deleteTreeW(dir);
    TEST_CHECK_BOOL(CreateDirectoryW(dir.c_str(), nullptr) != FALSE);

    const std::wstring dstPlugin = dir + L"\\PDPerfPlugin.dll";
    const std::wstring dstLoader = dir + L"\\amd_fidelityfx_loader_dx12.dll";
    const std::wstring iniPath   = dir + L"\\pd-perfmod-fsr4.ini";
    TEST_CHECK_BOOL(CopyFileW(srcPluginDll.c_str(), dstPlugin.c_str(), FALSE) != FALSE);
    TEST_CHECK_BOOL(CopyFileW(srcMockLoaderDll.c_str(), dstLoader.c_str(), FALSE) != FALSE);
    TEST_CHECK_BOOL(writeBytesW(iniPath, "[Logging]\nVerbose=0\n[Backend]\nRBSwap=0\n"));

    clearMockEnvVars();
    SetEnvironmentVariableA("PD_MOCK_FFX_VERSIONS", "2");
    SetEnvironmentVariableA("PD_MOCK_FFX_FAIL_VERSION", "0x00401001");

    PluginApi api;
    TEST_CHECK_BOOL(api.load(dstPlugin));

    auto* dev   = new MockD3D12Device();
    auto* queue = new MockD3D12CommandQueue(dev);
    TEST_CHECK_BOOL(api.SetupDirectX(queue, 1) == true);
    TEST_CHECK_BOOL(MockFfxInspector::reset());

    InitParams ip{};
    ip.id            = 1;
    ip.upscaleMethod = 1;
    ip.qualityLevel  = 3; // DLAA
    ip.displaySizeX  = 1920;
    ip.displaySizeY  = 1080;
    ip.format        = DXGI_FORMAT_R8G8B8A8_UNORM;

    void* outTex = api.InitUpscaler(&ip);
    TEST_CHECK_BOOL(outTex != nullptr);
    TEST_CHECK_BOOL(api.GetRenderWidth(1) == 1920);
    TEST_CHECK_BOOL(api.GetRenderHeight(1) == 1080);

    MockFfxStats stats{};
    TEST_CHECK_BOOL(MockFfxInspector::getStats(stats));
    TEST_CHECK_BOOL(stats.createAttemptCount == 2);
    TEST_CHECK_BOOL(stats.createSuccessCount == 1);
    TEST_CHECK_BOOL(stats.lastProviderVersionId == 0x00301005ULL);

    MockFfxCreateRecord cr0{};
    MockFfxCreateRecord cr1{};
    TEST_CHECK_BOOL(MockFfxInspector::getCreateRecord(0, cr0));
    TEST_CHECK_BOOL(MockFfxInspector::getCreateRecord(1, cr1));
    TEST_CHECK_BOOL(cr0.overrideVersionId == 0x00401001ULL && cr0.resultCode == 1);
    TEST_CHECK_BOOL(cr1.overrideVersionId == 0x00301005ULL && cr1.resultCode == 0);
    TEST_CHECK_BOOL((cr1.flags & kMockFfxUpscaleEnableDynamicResolution) != 0);
    TEST_CHECK_BOOL(cr1.maxRenderWidth == 1920 && cr1.maxRenderHeight == 1080);
    TEST_CHECK_BOOL(cr1.maxUpscaleWidth == 1920 && cr1.maxUpscaleHeight == 1080);

    auto* colorRes = makeTex2D(dev, 1920, 1080, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto* depthRes = makeTex2D(dev, 1920, 1080, DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    auto* mvRes    = makeTex2D(dev, 1920, 1080, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE);

    // Dynamic resolution: REFramework Native Res DLAA passes displaySize - 1
    UpscaleParams epNativeMinus1 = makeEvalParams(1, colorRes, mvRes, depthRes, nullptr, 1919.0f, 1079.0f, true, nullptr);
    api.EvaluateUpscaler(&epNativeMinus1);

    MockFfxDispatchDescUpscale dd0{};
    TEST_CHECK_BOOL(MockFfxInspector::getDispatchRecord(0, dd0));
    TEST_CHECK_BOOL(dd0.renderSize.width == 1919 && dd0.renderSize.height == 1079);
    TEST_CHECK_BOOL(dd0.upscaleSize.width == 1920 && dd0.upscaleSize.height == 1080);

    // renderSize == 0 falls back to context renderWidth/renderHeight (1920x1080 for DLAA)
    UpscaleParams epZero = makeEvalParams(1, colorRes, mvRes, depthRes, nullptr, 0.0f, 0.0f, true, nullptr);
    api.EvaluateUpscaler(&epZero);

    MockFfxDispatchDescUpscale dd1{};
    TEST_CHECK_BOOL(MockFfxInspector::getDispatchRecord(1, dd1));
    TEST_CHECK_BOOL(dd1.renderSize.width == 1920 && dd1.renderSize.height == 1080);
    TEST_CHECK_BOOL(dd1.upscaleSize.width == 1920 && dd1.upscaleSize.height == 1080);

    // renderSize exceeding displaySize clamps cleanly to maxRenderSize
    UpscaleParams epClamp = makeEvalParams(1, colorRes, mvRes, depthRes, nullptr, 3840.0f, 2160.0f, true, nullptr);
    api.EvaluateUpscaler(&epClamp);

    MockFfxDispatchDescUpscale dd2{};
    TEST_CHECK_BOOL(MockFfxInspector::getDispatchRecord(2, dd2));
    TEST_CHECK_BOOL(dd2.renderSize.width == 1920 && dd2.renderSize.height == 1080);

    // All candidate versions fail -> degrades cleanly to UNORDERED_ACCESS fallback texture
    SetEnvironmentVariableA("PD_MOCK_FFX_FAIL_VERSION", nullptr);
    TEST_CHECK_BOOL(SetEnvironmentVariableA("PD_MOCK_FFX_CREATE_FAIL", "1") != FALSE);

    InitParams ipFb = ip;
    ipFb.id           = 2;
    ipFb.qualityLevel = 2;
    void* fbTex = api.InitUpscaler(&ipFb);
    TEST_CHECK_BOOL(fbTex != nullptr);
    TEST_CHECK_BOOL(dev->lastCreatedDesc.Flags == D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    TEST_CHECK_BOOL(dev->lastInitialState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    TEST_CHECK_BOOL(api.GetRenderWidth(2) == static_cast<int>(1920 * 0.67f));
    TEST_CHECK_BOOL(api.GetRenderHeight(2) == static_cast<int>(1080 * 0.67f));

    TEST_CHECK_BOOL(MockFfxInspector::getStats(stats));
    TEST_CHECK_BOOL(stats.createAttemptCount == 4);
    TEST_CHECK_BOOL(stats.createSuccessCount == 1);

    colorRes->Release();
    depthRes->Release();
    mvRes->Release();

    api.ReleaseUpscaleFeature(1);
    api.ReleaseUpscaleFeature(2);
    TEST_CHECK_BOOL(api.SetupDirectX(nullptr, 1) == false);
    TEST_CHECK_BOOL(MockD3D12Device::liveResources.load() == 0);
    TEST_CHECK_BOOL(MockD3D12Device::liveAllocators.load() == 0);
    TEST_CHECK_BOOL(MockD3D12Device::liveLists.load() == 0);
    TEST_CHECK_BOOL(MockD3D12Device::liveFences.load() == 0);
    TEST_CHECK_BOOL(dev->refCount() == 1);
    TEST_CHECK_BOOL(queue->refCount() == 1);

    queue->Release();
    dev->Release();
    TEST_CHECK_BOOL(MockD3D12Device::liveQueues.load() == 0);
    TEST_CHECK_BOOL(MockD3D12Device::liveDevices.load() == 0);

    api.unload();
    clearMockEnvVars();
    deleteTreeW(dir);
    printf("[Case 2] PASSED.\n");
    return true;
}

static bool verifyRbSwapBarriers(const std::vector<D3D12_RESOURCE_BARRIER>& barriers,
                                 ID3D12Resource* outputTex,
                                 ID3D12Resource* swapTex) {
    bool sawPreSrvTransition = false;
    bool sawPostUavTransition = false;
    bool sawSwapUavBarrier = false;

    for (const auto& b : barriers) {
        if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION &&
            b.Transition.pResource == outputTex) {
            if (b.Transition.StateBefore == D3D12_RESOURCE_STATE_UNORDERED_ACCESS &&
                b.Transition.StateAfter == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) {
                sawPreSrvTransition = true;
            }
            if (b.Transition.StateBefore == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE &&
                b.Transition.StateAfter == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
                sawPostUavTransition = true;
            }
        } else if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV &&
                   b.UAV.pResource == swapTex) {
            sawSwapUavBarrier = true;
        }
    }

    return sawPreSrvTransition && sawPostUavTransition && sawSwapUavBarrier;
}

static bool runRbSwapSubcase(const std::wstring& srcPluginDll,
                             const std::wstring& srcMockLoaderDll,
                             const std::wstring& tempRoot,
                             const wchar_t* tag,
                             const std::string& iniContent,
                             int initFormat,
                             bool isHdr,
                             bool expectSwap,
                             DXGI_FORMAT expectedOutFmt,
                             DXGI_FORMAT expectedSrvFmt,
                             DXGI_FORMAT expectedUavFmt) {
    const std::wstring dir = tempRoot + L"\\" + tag;
    deleteTreeW(dir);
    TEST_CHECK_BOOL(CreateDirectoryW(dir.c_str(), nullptr) != FALSE);

    const std::wstring dstPlugin = dir + L"\\PDPerfPlugin.dll";
    const std::wstring dstLoader = dir + L"\\amd_fidelityfx_loader_dx12.dll";
    const std::wstring iniPath   = dir + L"\\pd-perfmod-fsr4.ini";
    TEST_CHECK_BOOL(CopyFileW(srcPluginDll.c_str(), dstPlugin.c_str(), FALSE) != FALSE);
    TEST_CHECK_BOOL(CopyFileW(srcMockLoaderDll.c_str(), dstLoader.c_str(), FALSE) != FALSE);
    TEST_CHECK_BOOL(writeBytesW(iniPath, iniContent));

    clearMockEnvVars();
    SetEnvironmentVariableA("PD_MOCK_FFX_VERSIONS", "1");

    PluginApi api;
    TEST_CHECK_BOOL(api.load(dstPlugin));

    auto* dev   = new MockD3D12Device();
    auto* queue = new MockD3D12CommandQueue(dev);
    TEST_CHECK_BOOL(api.SetupDirectX(queue, 1) == true);
    TEST_CHECK_BOOL(MockFfxInspector::reset());

    MockD3D12GraphicsCommandList* cmdList = dev->lastCreatedCommandList;
    TEST_CHECK_BOOL(cmdList != nullptr);

    InitParams ip{};
    ip.id            = 1;
    ip.upscaleMethod = 1;
    ip.qualityLevel  = 2;
    ip.displaySizeX  = 1920;
    ip.displaySizeY  = 1080;
    ip.format        = initFormat;
    ip.isContentHDR  = isHdr;

    void* returnedTex = api.InitUpscaler(&ip);
    TEST_CHECK_BOOL(returnedTex != nullptr);
    auto* returnedRes = static_cast<ID3D12Resource*>(returnedTex);

    if (expectSwap) {
        TEST_CHECK_BOOL(MockD3D12Device::liveDescriptorHeaps.load() == 1);
        TEST_CHECK_BOOL(MockD3D12Device::liveRootSignatures.load() == 1);
        TEST_CHECK_BOOL(MockD3D12Device::livePipelineStates.load() == 1);
        TEST_CHECK_BOOL(MockD3D12Device::liveResources.load() == 2);

        TEST_CHECK_BOOL(dev->srvCreateCount == 1 && dev->uavCreateCount == 1);
        TEST_CHECK_BOOL(dev->lastSrvResource != nullptr && dev->lastSrvResource != returnedRes);
        TEST_CHECK_BOOL(dev->lastUavResource == returnedRes);
        TEST_CHECK_BOOL(dev->lastSrvResource->GetDesc().Format == expectedOutFmt);
        TEST_CHECK_BOOL(returnedRes->GetDesc().Format == expectedUavFmt);
        TEST_CHECK_BOOL(dev->lastSrvDesc.Format == expectedSrvFmt);
        TEST_CHECK_BOOL(dev->lastUavDesc.Format == expectedUavFmt);

        auto* colorRes = makeTex2D(dev, 1280, 720, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        auto* depthRes = makeTex2D(dev, 1280, 720, DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        auto* mvRes    = makeTex2D(dev, 1280, 720, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE);

        UpscaleParams ep = makeEvalParams(1, colorRes, mvRes, depthRes, nullptr, 1280.0f, 720.0f, true, nullptr);
        api.EvaluateUpscaler(&ep);

        TEST_CHECK_BOOL(cmdList->dispatchCount == 1);
        TEST_CHECK_BOOL(verifyRbSwapBarriers(cmdList->recordedBarriers, dev->lastSrvResource, returnedRes));

        MockFfxDispatchDescUpscale dd{};
        TEST_CHECK_BOOL(MockFfxInspector::getDispatchRecord(0, dd));
        TEST_CHECK_BOOL(dd.output.resource == dev->lastSrvResource);

        colorRes->Release();
        depthRes->Release();
        mvRes->Release();
    } else {
        TEST_CHECK_BOOL(MockD3D12Device::liveDescriptorHeaps.load() == 0);
        TEST_CHECK_BOOL(MockD3D12Device::liveRootSignatures.load() == 0);
        TEST_CHECK_BOOL(MockD3D12Device::livePipelineStates.load() == 0);
        TEST_CHECK_BOOL(MockD3D12Device::liveResources.load() == 1);
        TEST_CHECK_BOOL(returnedRes->GetDesc().Format == expectedOutFmt);
    }

    api.ReleaseUpscaleFeature(1);
    TEST_CHECK_BOOL(MockD3D12Device::liveResources.load() == 0);
    TEST_CHECK_BOOL(MockD3D12Device::liveDescriptorHeaps.load() == 0);

    TEST_CHECK_BOOL(api.SetupDirectX(nullptr, 1) == false);
    TEST_CHECK_BOOL(MockD3D12Device::liveResources.load() == 0);
    TEST_CHECK_BOOL(MockD3D12Device::liveDescriptorHeaps.load() == 0);
    TEST_CHECK_BOOL(MockD3D12Device::liveRootSignatures.load() == 0);
    TEST_CHECK_BOOL(MockD3D12Device::livePipelineStates.load() == 0);
    TEST_CHECK_BOOL(MockD3D12Device::liveAllocators.load() == 0);
    TEST_CHECK_BOOL(MockD3D12Device::liveLists.load() == 0);
    TEST_CHECK_BOOL(MockD3D12Device::liveFences.load() == 0);
    TEST_CHECK_BOOL(dev->refCount() == 1);
    TEST_CHECK_BOOL(queue->refCount() == 1);

    queue->Release();
    dev->Release();
    TEST_CHECK_BOOL(MockD3D12Device::liveQueues.load() == 0);
    TEST_CHECK_BOOL(MockD3D12Device::liveDevices.load() == 0);

    api.unload();
    clearMockEnvVars();
    deleteTreeW(dir);
    return true;
}

static bool runRbSwapCases(const std::wstring& srcPluginDll,
                           const std::wstring& srcMockLoaderDll,
                           const std::wstring& tempRoot) {
    printf("[Case 3] Testing FIX-11 Wine R/B swap formats, [Backend] RBSwap INI toggle, & barriers...\n");

    // Note: RBSwap=-1 auto-enable relies on isRunningUnderWine() (ntdll!wine_get_version),
    // which returns true under Wine CTest (and false on native Windows).
    TEST_CHECK_BOOL(GetModuleHandleA("ntdll.dll") != nullptr &&
                    GetProcAddress(GetModuleHandleA("ntdll.dll"), "wine_get_version") != nullptr);

    // Test 3a: RBSwap=-1 auto, SDR format=28 (R8G8B8A8_UNORM)
    TEST_CHECK_BOOL(runRbSwapSubcase(
        srcPluginDll, srcMockLoaderDll, tempRoot, L"case3a_auto_sdr",
        "[Logging]\nVerbose=0\n[Backend]\nRBSwap=-1\n",
        DXGI_FORMAT_R8G8B8A8_UNORM, false, true,
        DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM));

    // Test 3b: OutputFormat=87 (B8G8R8A8_UNORM) + RBSwap=1 forced (independent of Wine detection)
    TEST_CHECK_BOOL(runRbSwapSubcase(
        srcPluginDll, srcMockLoaderDll, tempRoot, L"case3b_bgra_forced",
        "[Logging]\nVerbose=0\n[Backend]\nOutputFormat=87\nRBSwap=1\n",
        DXGI_FORMAT_R8G8B8A8_UNORM, false, true,
        DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM));

    // Test 3c: RBSwap=0 A/B disable, SDR format=28
    TEST_CHECK_BOOL(runRbSwapSubcase(
        srcPluginDll, srcMockLoaderDll, tempRoot, L"case3c_off_sdr",
        "[Logging]\nVerbose=0\n[Backend]\nRBSwap=0\n",
        DXGI_FORMAT_R8G8B8A8_UNORM, false, false,
        DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN));

    // Test 3d: RBSwap=-1 auto, HDR format=10 (R16G16B16A16_FLOAT) -> swap skipped
    TEST_CHECK_BOOL(runRbSwapSubcase(
        srcPluginDll, srcMockLoaderDll, tempRoot, L"case3d_auto_hdr",
        "[Logging]\nVerbose=0\n[Backend]\nRBSwap=-1\n",
        DXGI_FORMAT_R16G16B16A16_FLOAT, true, false,
        DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN));

    // Test 3e: RBSwap=1 force on, HDR format=10 -> swap enabled with matched R16G16B16A16_FLOAT SRV & UAV
    TEST_CHECK_BOOL(runRbSwapSubcase(
        srcPluginDll, srcMockLoaderDll, tempRoot, L"case3e_forced_hdr",
        "[Logging]\nVerbose=0\n[Backend]\nRBSwap=1\n",
        DXGI_FORMAT_R16G16B16A16_FLOAT, true, true,
        DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT));

    printf("[Case 3] PASSED (Test 3a..3e all verified).\n");
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
    std::wstring tempRoot = std::wstring(tempBuf) + L"pd_p2_rt_" + std::to_wstring(GetCurrentProcessId());
    while (!tempRoot.empty() && tempRoot.back() == L'\\') tempRoot.pop_back();
    deleteTreeW(tempRoot);
    TEST_CHECK(CreateDirectoryW(tempRoot.c_str(), nullptr) != FALSE);

    const bool ok1 = runEvaluationAndVrMultipassCase(pluginDll, mockLoaderDll, tempRoot);
    const bool ok2 = ok1 && runDlaaAndMultiVersionFallbackCase(pluginDll, mockLoaderDll, tempRoot);
    const bool ok3 = ok2 && runRbSwapCases(pluginDll, mockLoaderDll, tempRoot);

    deleteTreeW(tempRoot);
    TEST_CHECK(ok1 && ok2 && ok3);

    printf("ALL PHASE 2 RUNTIME PATH TESTS PASSED.\n");
    return 0;
}
