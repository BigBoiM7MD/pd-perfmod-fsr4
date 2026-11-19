#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <atomic>
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
    void (__stdcall* SetMotionScaleX)(int, float) = nullptr;
    void (__stdcall* SetMotionScaleY)(int, float) = nullptr;
    int (__stdcall* GetRenderWidth)(int) = nullptr;
    int (__stdcall* GetRenderHeight)(int) = nullptr;
    void (__stdcall* ReleaseUpscaleFeature)(int) = nullptr;
    int (__stdcall* GetJitterPhaseCount)(int) = nullptr;
    int (__stdcall* GetJitterOffset)(float*, float*, int, int) = nullptr;

    bool load(const std::wstring& dllPath) {
        mod = LoadLibraryW(dllPath.c_str());
        TEST_CHECK_BOOL(mod != nullptr);

        SetupDirectX          = reinterpret_cast<decltype(SetupDirectX)>(GetProcAddress(mod, "SetupDirectX"));
        SimpleInit            = reinterpret_cast<decltype(SimpleInit)>(GetProcAddress(mod, "SimpleInit"));
        InitUpscaler          = reinterpret_cast<decltype(InitUpscaler)>(GetProcAddress(mod, "InitUpscaler"));
        SimpleEvaluate        = reinterpret_cast<decltype(SimpleEvaluate)>(GetProcAddress(mod, "SimpleEvaluate"));
        EvaluateUpscaler      = reinterpret_cast<decltype(EvaluateUpscaler)>(GetProcAddress(mod, "EvaluateUpscaler"));
        SetMotionScaleX       = reinterpret_cast<decltype(SetMotionScaleX)>(GetProcAddress(mod, "SetMotionScaleX"));
        SetMotionScaleY       = reinterpret_cast<decltype(SetMotionScaleY)>(GetProcAddress(mod, "SetMotionScaleY"));
        GetRenderWidth        = reinterpret_cast<decltype(GetRenderWidth)>(GetProcAddress(mod, "GetRenderWidth"));
        GetRenderHeight       = reinterpret_cast<decltype(GetRenderHeight)>(GetProcAddress(mod, "GetRenderHeight"));
        ReleaseUpscaleFeature = reinterpret_cast<decltype(ReleaseUpscaleFeature)>(GetProcAddress(mod, "ReleaseUpscaleFeature"));
        GetJitterPhaseCount   = reinterpret_cast<decltype(GetJitterPhaseCount)>(GetProcAddress(mod, "GetJitterPhaseCount"));
        GetJitterOffset       = reinterpret_cast<decltype(GetJitterOffset)>(GetProcAddress(mod, "GetJitterOffset"));

        TEST_CHECK_BOOL(SetupDirectX && SimpleInit && InitUpscaler &&
                        SimpleEvaluate && EvaluateUpscaler &&
                        SetMotionScaleX && SetMotionScaleY &&
                        GetRenderWidth && GetRenderHeight &&
                        ReleaseUpscaleFeature && GetJitterPhaseCount &&
                        GetJitterOffset);
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

static MockD3D12Resource* makeMockResource(ID3D12Device* dev,
                                           D3D12_RESOURCE_DIMENSION dim,
                                           UINT64 width,
                                           UINT height,
                                           UINT16 depthOrArray,
                                           UINT16 mipLevels,
                                           DXGI_FORMAT format,
                                           D3D12_RESOURCE_FLAGS flags) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension          = dim;
    desc.Width              = width;
    desc.Height             = height;
    desc.DepthOrArraySize   = depthOrArray;
    desc.MipLevels          = mipLevels;
    desc.Format             = format;
    desc.SampleDesc.Count   = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout             = (dim == D3D12_RESOURCE_DIMENSION_BUFFER)
                                  ? D3D12_TEXTURE_LAYOUT_ROW_MAJOR
                                  : D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags              = flags;
    return new MockD3D12Resource(dev, desc);
}

static UpscaleParams makeEvalParams(int id, void* color, void* mv, void* depth, void* dst, bool execute) {
    UpscaleParams p{};
    p.id            = id;
    p.color         = color;
    p.motionVector  = mv;
    p.depth         = depth;
    p.destination   = dst;
    p.renderSizeX   = 1280.0f;
    p.renderSizeY   = 720.0f;
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
    p.cmdList       = nullptr;
    return p;
}

static bool runAbiMappingCase(const std::wstring& srcPluginDll,
                              const std::wstring& srcMockLoaderDll,
                              const std::wstring& tempRoot) {
    printf("[Case 1] Testing FIX-07 FFX ABI resource type/usage/format mappings...\n");
    const std::wstring dir = tempRoot + L"\\case1_abi";
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

    InitParams ip{};
    ip.id            = 1;
    ip.upscaleMethod = 1;
    ip.qualityLevel  = 2;
    ip.displaySizeX  = 1920;
    ip.displaySizeY  = 1080;
    ip.format        = DXGI_FORMAT_R8G8B8A8_UNORM;

    void* outTex = api.InitUpscaler(&ip);
    TEST_CHECK_BOOL(outTex != nullptr);
    TEST_CHECK_BOOL(MockD3D12Device::liveResources.load() == 1);

    auto* colorRes = makeMockResource(dev, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 1280, 720, 1, 1,
                                      DXGI_FORMAT_R16G16B16A16_FLOAT,
                                      D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto* depthRes = makeMockResource(dev, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 1280, 720, 1, 1,
                                      DXGI_FORMAT_D32_FLOAT,
                                      D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
    auto* mvRes    = makeMockResource(dev, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 1280, 720, 1, 1,
                                      DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE);

    UpscaleParams ep = makeEvalParams(1, colorRes, mvRes, depthRes, nullptr, true);
    api.EvaluateUpscaler(&ep);

    MockFfxStats stats{};
    TEST_CHECK_BOOL(MockFfxInspector::getStats(stats));
    TEST_CHECK_BOOL(stats.dispatchCount == 1);

    MockFfxDispatchDescUpscale dd{};
    TEST_CHECK_BOOL(MockFfxInspector::getDispatchRecord(0, dd));
    TEST_CHECK_BOOL(dd.color.resource == colorRes);
    TEST_CHECK_BOOL(dd.color.description.type == 2u);
    TEST_CHECK_BOOL(dd.color.description.format == 4u);
    TEST_CHECK_BOOL(dd.color.description.usage == 3u);

    TEST_CHECK_BOOL(dd.depth.resource == depthRes);
    TEST_CHECK_BOOL(dd.depth.description.type == 2u);
    TEST_CHECK_BOOL(dd.depth.description.format == 28u);
    TEST_CHECK_BOOL(dd.depth.description.usage == 4u);

    TEST_CHECK_BOOL(dd.motionVectors.resource == mvRes);
    TEST_CHECK_BOOL(dd.motionVectors.description.type == 2u);
    TEST_CHECK_BOOL(dd.motionVectors.description.format == 18u);
    TEST_CHECK_BOOL(dd.motionVectors.description.usage == 0u);

    TEST_CHECK_BOOL(dd.output.resource == outTex);
    TEST_CHECK_BOOL(dd.output.description.type == 2u);
    TEST_CHECK_BOOL(dd.output.description.format == 10u);
    TEST_CHECK_BOOL(dd.output.description.usage == 2u);

    // Additional format and dimension checks
    struct VariantCheck {
        D3D12_RESOURCE_DIMENSION dim;
        UINT64 width;
        UINT height;
        UINT16 depthOrArray;
        UINT16 mipLevels;
        DXGI_FORMAT dxgiFmt;
        uint32_t expectedType;
        uint32_t expectedFmt;
    };
    const VariantCheck variants[] = {
        { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 1280, 720, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM,     2u, 14u },
        { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 1280, 720, 1, 1, DXGI_FORMAT_R11G11B10_FLOAT,    2u, 16u },
        { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 1280, 720, 1, 1, DXGI_FORMAT_R32_TYPELESS,       2u, 37u },
        { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 1280, 720, 1, 1, DXGI_FORMAT_D16_UNORM,          2u, 23u },
        { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 1280, 720, 1, 1, DXGI_FORMAT_D32_FLOAT_S8X24_UINT, 2u, 28u },
        { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 512,  512, 6, 1, DXGI_FORMAT_R8G8B8A8_UNORM,     3u, 10u },
        { D3D12_RESOURCE_DIMENSION_TEXTURE1D, 256,  1,   1, 1, DXGI_FORMAT_R8G8B8A8_UNORM,     1u, 10u },
        { D3D12_RESOURCE_DIMENSION_TEXTURE3D, 64,   64,  8, 1, DXGI_FORMAT_R8G8B8A8_UNORM,     4u, 10u },
        { D3D12_RESOURCE_DIMENSION_BUFFER,    256,  1,   1, 1, DXGI_FORMAT_UNKNOWN,            0u,  0u },
    };

    for (size_t i = 0; i < sizeof(variants) / sizeof(variants[0]); ++i) {
        const auto& v = variants[i];
        auto* probe = makeMockResource(dev, v.dim, v.width, v.height, v.depthOrArray,
                                       v.mipLevels, v.dxgiFmt, D3D12_RESOURCE_FLAG_NONE);
        UpscaleParams vp = makeEvalParams(1, probe, mvRes, depthRes, nullptr, true);
        api.EvaluateUpscaler(&vp);

        MockFfxDispatchDescUpscale vdd{};
        TEST_CHECK_BOOL(MockFfxInspector::getDispatchRecord(static_cast<uint32_t>(i + 1), vdd));
        TEST_CHECK_BOOL(vdd.color.description.type == v.expectedType);
        TEST_CHECK_BOOL(vdd.color.description.format == v.expectedFmt);
        if (v.dim == D3D12_RESOURCE_DIMENSION_BUFFER) {
            TEST_CHECK_BOOL(vdd.color.description.width == 256u);
            TEST_CHECK_BOOL(vdd.color.description.height == 0u);
            TEST_CHECK_BOOL(vdd.color.description.depth == 0u);
            TEST_CHECK_BOOL(vdd.color.description.mipCount == 0u);
        }
        probe->Release();
    }

    mvRes->Release();
    depthRes->Release();
    colorRes->Release();

    api.ReleaseUpscaleFeature(1);
    TEST_CHECK_BOOL(MockD3D12Device::liveResources.load() == 0);
    TEST_CHECK_BOOL(api.SetupDirectX(nullptr, 1) == false);
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

struct FenceCompletionHelper {
    MockD3D12Fence* fence = nullptr;
    UINT64 targetValue = 0;
    int expectedWaitCount = 1;
    bool checkDestroyBeforeComplete = false;
    uint32_t baselineDestroyCount = 0;
    bool sawNotDestroyedBeforeComplete = true;
    bool sawWaiterRegistered = false;
};

static DWORD WINAPI delayedFenceCompleteProc(LPVOID param) {
    auto* ctx = static_cast<FenceCompletionHelper*>(param);
    for (int i = 0; i < 2000; ++i) {
        if (ctx->fence->setEventCallCount.load(std::memory_order_acquire) >= ctx->expectedWaitCount) {
            ctx->sawWaiterRegistered = true;
            break;
        }
        Sleep(1);
    }
    Sleep(5);
    if (ctx->checkDestroyBeforeComplete) {
        MockFfxStats s{};
        if (!MockFfxInspector::getStats(s) || s.destroyCount != ctx->baselineDestroyCount) {
            ctx->sawNotDestroyedBeforeComplete = false;
        }
    }
    ctx->fence->completeUpTo(ctx->targetValue);
    return 0;
}

static bool runRingAndLifecycleCase(const std::wstring& srcPluginDll,
                                    const std::wstring& srcMockLoaderDll,
                                    const std::wstring& tempRoot) {
    printf("[Case 2] Testing FIX-09/FIX-15 3-slot ring, manual fence wait, overwrite drain & zero-leak teardown...\n");
    const std::wstring dir = tempRoot + L"\\case2_ring";
    deleteTreeW(dir);
    TEST_CHECK_BOOL(CreateDirectoryW(dir.c_str(), nullptr) != FALSE);

    const std::wstring dstPlugin = dir + L"\\PDPerfPlugin.dll";
    const std::wstring dstLoader = dir + L"\\amd_fidelityfx_loader_dx12.dll";
    const std::wstring iniPath   = dir + L"\\pd-perfmod-fsr4.ini";
    TEST_CHECK_BOOL(CopyFileW(srcPluginDll.c_str(), dstPlugin.c_str(), FALSE) != FALSE);
    TEST_CHECK_BOOL(CopyFileW(srcMockLoaderDll.c_str(), dstLoader.c_str(), FALSE) != FALSE);
    TEST_CHECK_BOOL(writeBytesW(iniPath, "[Logging]\nVerbose=0\n"));

    clearMockEnvVars();
    SetEnvironmentVariableA("PD_MOCK_FFX_VERSIONS", "1");

    PluginApi api;
    TEST_CHECK_BOOL(api.load(dstPlugin));

    auto* dev   = new MockD3D12Device();
    auto* queue = new MockD3D12CommandQueue(dev);
    TEST_CHECK_BOOL(api.SetupDirectX(queue, 1) == true);
    TEST_CHECK_BOOL(MockFfxInspector::reset());

    MockD3D12Fence* fence = dev->lastCreatedFence;
    MockD3D12GraphicsCommandList* cmdList = dev->lastCreatedCommandList;
    TEST_CHECK_BOOL(fence != nullptr && cmdList != nullptr);
    TEST_CHECK_BOOL(dev->createdAllocators.size() == 3);

    fence->manualCompletion = true;

    InitParams ip{};
    ip.id            = 1;
    ip.upscaleMethod = 1;
    ip.qualityLevel  = 2;
    ip.displaySizeX  = 1920;
    ip.displaySizeY  = 1080;
    ip.format        = DXGI_FORMAT_R8G8B8A8_UNORM;

    void* tex1 = api.InitUpscaler(&ip);
    TEST_CHECK_BOOL(tex1 != nullptr);
    const int ctxResCount = MockD3D12Device::liveResources.load();
    TEST_CHECK_BOOL(ctxResCount == 1 || ctxResCount == 2);

    auto* colorRes = makeMockResource(dev, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 1280, 720, 1, 1,
                                      DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto* depthRes = makeMockResource(dev, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 1280, 720, 1, 1,
                                      DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    auto* mvRes    = makeMockResource(dev, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 1280, 720, 1, 1,
                                      DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE);

    // Frames 1..3: fill all 3 allocator slots with zero fence waits
    for (int frame = 0; frame < 3; ++frame) {
        UpscaleParams ep = makeEvalParams(1, colorRes, mvRes, depthRes, nullptr, true);
        api.EvaluateUpscaler(&ep);
    }

    TEST_CHECK_BOOL(cmdList->resetAllocators.size() == 3);
    TEST_CHECK_BOOL(cmdList->resetAllocators[0] != cmdList->resetAllocators[1]);
    TEST_CHECK_BOOL(cmdList->resetAllocators[1] != cmdList->resetAllocators[2]);
    TEST_CHECK_BOOL(cmdList->resetAllocators[0] != cmdList->resetAllocators[2]);
    TEST_CHECK_BOOL(fence->setEventCallCount.load() == 0);
    TEST_CHECK_BOOL(queue->executeCount.load() == 3);
    TEST_CHECK_BOOL(fence->lastSignaledValue.load() == 3);

    // Frame 4: slot 0 wraps around and must wait for fence value 1
    FenceCompletionHelper f4Helper{};
    f4Helper.fence = fence;
    f4Helper.targetValue = 1;
    f4Helper.expectedWaitCount = fence->setEventCallCount.load(std::memory_order_acquire) + 1;
    HANDLE hF4 = CreateThread(nullptr, 0, delayedFenceCompleteProc, &f4Helper, 0, nullptr);
    TEST_CHECK_BOOL(hF4 != nullptr);

    UpscaleParams ep4 = makeEvalParams(1, colorRes, mvRes, depthRes, nullptr, true);
    api.EvaluateUpscaler(&ep4);

    TEST_CHECK_BOOL(WaitForSingleObject(hF4, 10000) == WAIT_OBJECT_0);
    CloseHandle(hF4);

    TEST_CHECK_BOOL(f4Helper.sawWaiterRegistered);
    TEST_CHECK_BOOL(fence->setEventCallCount.load() == 1);
    TEST_CHECK_BOOL(fence->waitTargets.size() == 1 && fence->waitTargets[0] == 1);
    TEST_CHECK_BOOL(queue->executeCount.load() == 4);
    TEST_CHECK_BOOL(fence->lastSignaledValue.load() == 4);
    TEST_CHECK_BOOL(cmdList->resetAllocators.size() == 4);
    TEST_CHECK_BOOL(cmdList->resetAllocators[3] == cmdList->resetAllocators[0]);

    // Overwrite context id=1 while fenceValue=4 is incomplete; must drain before ffxDestroyContext
    MockFfxStats statsBeforeOverwrite{};
    TEST_CHECK_BOOL(MockFfxInspector::getStats(statsBeforeOverwrite));
    const int liveResBeforeOverwrite = MockD3D12Device::liveResources.load();

    FenceCompletionHelper owHelper{};
    owHelper.fence = fence;
    owHelper.targetValue = 4;
    owHelper.expectedWaitCount = fence->setEventCallCount.load(std::memory_order_acquire) + 1;
    owHelper.checkDestroyBeforeComplete = true;
    owHelper.baselineDestroyCount = statsBeforeOverwrite.destroyCount;
    HANDLE hOw = CreateThread(nullptr, 0, delayedFenceCompleteProc, &owHelper, 0, nullptr);
    TEST_CHECK_BOOL(hOw != nullptr);

    void* tex1b = api.InitUpscaler(&ip);
    TEST_CHECK_BOOL(tex1b != nullptr);
    TEST_CHECK_BOOL(WaitForSingleObject(hOw, 10000) == WAIT_OBJECT_0);
    CloseHandle(hOw);

    TEST_CHECK_BOOL(owHelper.sawWaiterRegistered);
    TEST_CHECK_BOOL(owHelper.sawNotDestroyedBeforeComplete);
    MockFfxStats statsAfterOverwrite{};
    TEST_CHECK_BOOL(MockFfxInspector::getStats(statsAfterOverwrite));
    TEST_CHECK_BOOL(statsAfterOverwrite.destroyCount == statsBeforeOverwrite.destroyCount + 1);
    TEST_CHECK_BOOL(MockD3D12Device::liveResources.load() == liveResBeforeOverwrite);

    colorRes->Release();
    depthRes->Release();
    mvRes->Release();

    api.ReleaseUpscaleFeature(1);
    TEST_CHECK_BOOL(MockD3D12Device::liveResources.load() == 0);
    TEST_CHECK_BOOL(MockD3D12Device::liveDescriptorHeaps.load() == 0);

    TEST_CHECK_BOOL(api.SetupDirectX(nullptr, 1) == false);
    TEST_CHECK_BOOL(MockD3D12Device::liveAllocators.load() == 0);
    TEST_CHECK_BOOL(MockD3D12Device::liveLists.load() == 0);
    TEST_CHECK_BOOL(MockD3D12Device::liveFences.load() == 0);
    TEST_CHECK_BOOL(MockD3D12Device::liveResources.load() == 0);
    TEST_CHECK_BOOL(MockD3D12Device::liveDescriptorHeaps.load() == 0);
    TEST_CHECK_BOOL(MockD3D12Device::liveRootSignatures.load() == 0);
    TEST_CHECK_BOOL(MockD3D12Device::livePipelineStates.load() == 0);
    TEST_CHECK_BOOL(dev->refCount() == 1);
    TEST_CHECK_BOOL(queue->refCount() == 1);
    TEST_CHECK_BOOL(GetModuleHandleW(L"amd_fidelityfx_loader_dx12.dll") == nullptr);

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

struct BackendStressThreadCtx {
    int threadIdx = 0;
    HANDLE barrier = nullptr;
    PluginApi* api = nullptr;
    ID3D12Resource* color = nullptr;
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* mv = nullptr;
    bool ok = false;
};

static DWORD WINAPI backendStressWorkerProc(LPVOID param) {
    auto* ctx = static_cast<BackendStressThreadCtx*>(param);
    if (WaitForSingleObject(ctx->barrier, 15000) != WAIT_OBJECT_0) {
        return 1;
    }

    for (int iter = 0; iter < 25; ++iter) {
        const int id = ((ctx->threadIdx + iter) % 4) + 1;

        InitParams ip{};
        ip.id            = id;
        ip.upscaleMethod = 1;
        ip.qualityLevel  = (iter % 4);
        ip.displaySizeX  = 1280;
        ip.displaySizeY  = 720;
        ip.format        = DXGI_FORMAT_R8G8B8A8_UNORM;

        void* out = ctx->api->InitUpscaler(&ip);
        if (!out) {
            return 1;
        }

        UpscaleParams ep = makeEvalParams(id, ctx->color, ctx->mv, ctx->depth, nullptr, (iter % 2) == 1);
        ctx->api->EvaluateUpscaler(&ep);

        const int rw = ctx->api->GetRenderWidth(id);
        if (rw < 0) {
            return 1;
        }

        float jx = 0.0f, jy = 0.0f;
        const int jOk = ctx->api->GetJitterOffset(&jx, &jy, 0, 8);
        if (jOk != 0 && jOk != -1) {
            return 1;
        }
        if (jOk == 0 && (jx != 0.25f || jy != -0.25f)) {
            return 1;
        }

        const int jErr = ctx->api->GetJitterOffset(&jx, &jy, -1, 8);
        if (jErr != -1) {
            return 1;
        }

        ctx->api->SetMotionScaleX(id, 1.0f + 0.01f * static_cast<float>(iter));

        if (iter % 5 == 4) {
            ctx->api->ReleaseUpscaleFeature(id);
        }
    }

    ctx->ok = true;
    return 0;
}

struct FallbackStressThreadCtx {
    int threadIdx = 0;
    HANDLE barrier = nullptr;
    PluginApi* api = nullptr;
    MockD3D12CommandQueue* queue = nullptr;
    bool ok = false;
};

static DWORD WINAPI fallbackStressWorkerProc(LPVOID param) {
    auto* ctx = static_cast<FallbackStressThreadCtx*>(param);
    if (WaitForSingleObject(ctx->barrier, 15000) != WAIT_OBJECT_0) {
        return 1;
    }

    const int expectedW = static_cast<int>(1920 * 0.67f);
    const int expectedH = static_cast<int>(1080 * 0.67f);

    for (int iter = 0; iter < 25; ++iter) {
        const int id = ((ctx->threadIdx + iter) % 3) + 1;

        InitParams ip{};
        ip.id            = id;
        ip.upscaleMethod = 1;
        ip.qualityLevel  = 2;
        ip.displaySizeX  = 1920;
        ip.displaySizeY  = 1080;
        ip.format        = DXGI_FORMAT_R8G8B8A8_UNORM;

        if ((iter % 2) == 0) {
            ctx->api->InitUpscaler(&ip);
        } else {
            ctx->api->SimpleInit(id, 1, 2, 1920, 1080, false, false, false, false, false, false,
                                 DXGI_FORMAT_R8G8B8A8_UNORM);
        }

        const int rw = ctx->api->GetRenderWidth(id);
        const int rh = ctx->api->GetRenderHeight(id);
        if ((rw != 0 && rw != expectedW) || (rh != 0 && rh != expectedH)) {
            return 1;
        }

        if (iter % 3 == 2) {
            ctx->api->ReleaseUpscaleFeature(id);
        }

        if (ctx->threadIdx == 0 && (iter % 7 == 6)) {
            ctx->api->SetupDirectX(ctx->queue, 1);
        }
    }

    ctx->ok = true;
    return 0;
}

static bool runRecoveryAndConcurrencyCase(const std::wstring& srcPluginDll,
                                          const std::wstring& srcMockLoaderDll,
                                          const std::wstring& tempRoot) {
    printf("[Case 3] Testing FIX-08/FIX-09 cmdListOpen error/SEH recovery and SRWLOCK multi-thread stress...\n");
    const std::wstring dir = tempRoot + L"\\case3_concurrency";
    deleteTreeW(dir);
    TEST_CHECK_BOOL(CreateDirectoryW(dir.c_str(), nullptr) != FALSE);

    const std::wstring dstPlugin = dir + L"\\PDPerfPlugin.dll";
    const std::wstring dstLoader = dir + L"\\amd_fidelityfx_loader_dx12.dll";
    const std::wstring iniPath   = dir + L"\\pd-perfmod-fsr4.ini";
    TEST_CHECK_BOOL(CopyFileW(srcPluginDll.c_str(), dstPlugin.c_str(), FALSE) != FALSE);
    TEST_CHECK_BOOL(CopyFileW(srcMockLoaderDll.c_str(), dstLoader.c_str(), FALSE) != FALSE);
    TEST_CHECK_BOOL(writeBytesW(iniPath, "[Logging]\nVerbose=0\n"));

    clearMockEnvVars();
    SetEnvironmentVariableA("PD_MOCK_FFX_VERSIONS", "1");

    PluginApi api;
    TEST_CHECK_BOOL(api.load(dstPlugin));

    auto* dev   = new MockD3D12Device();
    auto* queue = new MockD3D12CommandQueue(dev);
    TEST_CHECK_BOOL(api.SetupDirectX(queue, 1) == true);

    MockD3D12GraphicsCommandList* cmdList = dev->lastCreatedCommandList;
    TEST_CHECK_BOOL(cmdList != nullptr);
    TEST_CHECK_BOOL(cmdList->isOpen == false);

    InitParams ip{};
    ip.id            = 1;
    ip.upscaleMethod = 1;
    ip.qualityLevel  = 2;
    ip.displaySizeX  = 1280;
    ip.displaySizeY  = 720;
    ip.format        = DXGI_FORMAT_R8G8B8A8_UNORM;
    TEST_CHECK_BOOL(api.InitUpscaler(&ip) != nullptr);

    auto* colorRes = makeMockResource(dev, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 853, 480, 1, 1,
                                      DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto* depthRes = makeMockResource(dev, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 853, 480, 1, 1,
                                      DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    auto* mvRes    = makeMockResource(dev, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 853, 480, 1, 1,
                                      DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE);

    UpscaleParams ep = makeEvalParams(1, colorRes, mvRes, depthRes, nullptr, true);

    // 1) Non-zero dispatch return closes internal command list cleanly
    TEST_CHECK_BOOL(SetEnvironmentVariableA("PD_MOCK_FFX_DISPATCH_FAIL", "1") != FALSE);
    api.EvaluateUpscaler(&ep);
    TEST_CHECK_BOOL(cmdList->isOpen == false);
    TEST_CHECK_BOOL(queue->executeCount.load() == 0);
    SetEnvironmentVariableA("PD_MOCK_FFX_DISPATCH_FAIL", nullptr);

    // 2) SEH RaiseException inside ffxDispatch closes internal command list cleanly
    TEST_CHECK_BOOL(SetEnvironmentVariableA("PD_MOCK_FFX_CRASH_DISPATCH", "1") != FALSE);
    api.EvaluateUpscaler(&ep);
    TEST_CHECK_BOOL(cmdList->isOpen == false);
    TEST_CHECK_BOOL(queue->executeCount.load() == 0);
    SetEnvironmentVariableA("PD_MOCK_FFX_CRASH_DISPATCH", nullptr);

    // 3) Normal dispatch succeeds afterwards with zero invalid Reset/Close
    api.EvaluateUpscaler(&ep);
    TEST_CHECK_BOOL(cmdList->isOpen == false);
    TEST_CHECK_BOOL(queue->executeCount.load() == 1);
    TEST_CHECK_BOOL(cmdList->invalidResetCount == 0);
    TEST_CHECK_BOOL(cmdList->invalidCloseCount == 0);

    // 4) 8-thread SRWLOCK stress across FSR4Backend (including GetJitterOffset 0 and -1)
    {
        HANDLE barrier = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        TEST_CHECK_BOOL(barrier != nullptr);

        constexpr int kThreads = 8;
        BackendStressThreadCtx tctx[kThreads];
        HANDLE threads[kThreads] = {};
        for (int i = 0; i < kThreads; ++i) {
            tctx[i].threadIdx = i;
            tctx[i].barrier   = barrier;
            tctx[i].api       = &api;
            tctx[i].color     = colorRes;
            tctx[i].depth     = depthRes;
            tctx[i].mv        = mvRes;
            threads[i] = CreateThread(nullptr, 0, backendStressWorkerProc, &tctx[i], 0, nullptr);
            TEST_CHECK_BOOL(threads[i] != nullptr);
        }

        Sleep(20);
        TEST_CHECK_BOOL(SetEvent(barrier) != FALSE);
        TEST_CHECK_BOOL(WaitForMultipleObjects(kThreads, threads, TRUE, 30000) == WAIT_OBJECT_0);
        for (int i = 0; i < kThreads; ++i) {
            CloseHandle(threads[i]);
            TEST_CHECK_BOOL(tctx[i].ok);
        }
        CloseHandle(barrier);
    }

    TEST_CHECK_BOOL(cmdList->invalidResetCount == 0);
    TEST_CHECK_BOOL(cmdList->invalidCloseCount == 0);
    TEST_CHECK_BOOL(cmdList->recordWhileClosedCount == 0);

    colorRes->Release();
    depthRes->Release();
    mvRes->Release();

    TEST_CHECK_BOOL(api.SetupDirectX(nullptr, 1) == false);
    TEST_CHECK_BOOL(MockD3D12Device::liveResources.load() == 0);
    TEST_CHECK_BOOL(MockD3D12Device::liveAllocators.load() == 0);
    TEST_CHECK_BOOL(MockD3D12Device::liveLists.load() == 0);
    TEST_CHECK_BOOL(MockD3D12Device::liveFences.load() == 0);
    TEST_CHECK_BOOL(MockD3D12Device::liveDescriptorHeaps.load() == 0);
    TEST_CHECK_BOOL(MockD3D12Device::liveRootSignatures.load() == 0);
    TEST_CHECK_BOOL(MockD3D12Device::livePipelineStates.load() == 0);

    // 5) 6-thread g_fallbackTextures / g_fallbackLock stress with interleaved SetupDirectX
    TEST_CHECK_BOOL(SetEnvironmentVariableA("PD_MOCK_FFX_CREATE_FAIL", "1") != FALSE);
    TEST_CHECK_BOOL(api.SetupDirectX(queue, 1) == true);
    {
        HANDLE barrier = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        TEST_CHECK_BOOL(barrier != nullptr);

        constexpr int kFbThreads = 6;
        FallbackStressThreadCtx fctx[kFbThreads];
        HANDLE fthreads[kFbThreads] = {};
        for (int i = 0; i < kFbThreads; ++i) {
            fctx[i].threadIdx = i;
            fctx[i].barrier   = barrier;
            fctx[i].api       = &api;
            fctx[i].queue     = queue;
            fthreads[i] = CreateThread(nullptr, 0, fallbackStressWorkerProc, &fctx[i], 0, nullptr);
            TEST_CHECK_BOOL(fthreads[i] != nullptr);
        }

        Sleep(20);
        TEST_CHECK_BOOL(SetEvent(barrier) != FALSE);
        TEST_CHECK_BOOL(WaitForMultipleObjects(kFbThreads, fthreads, TRUE, 30000) == WAIT_OBJECT_0);
        for (int i = 0; i < kFbThreads; ++i) {
            CloseHandle(fthreads[i]);
            TEST_CHECK_BOOL(fctx[i].ok);
        }
        CloseHandle(barrier);
    }

    TEST_CHECK_BOOL(api.SetupDirectX(nullptr, 1) == false);
    TEST_CHECK_BOOL(MockD3D12Device::liveResources.load() == 0);
    TEST_CHECK_BOOL(MockD3D12Device::liveAllocators.load() == 0);
    TEST_CHECK_BOOL(MockD3D12Device::liveLists.load() == 0);
    TEST_CHECK_BOOL(MockD3D12Device::liveFences.load() == 0);
    TEST_CHECK_BOOL(MockD3D12Device::liveDescriptorHeaps.load() == 0);
    TEST_CHECK_BOOL(MockD3D12Device::liveRootSignatures.load() == 0);
    TEST_CHECK_BOOL(MockD3D12Device::livePipelineStates.load() == 0);
    TEST_CHECK_BOOL(dev->refCount() == 1);
    TEST_CHECK_BOOL(queue->refCount() == 1);

    queue->Release();
    dev->Release();
    TEST_CHECK_BOOL(MockD3D12Device::liveQueues.load() == 0);
    TEST_CHECK_BOOL(MockD3D12Device::liveDevices.load() == 0);

    api.unload();
    clearMockEnvVars();
    deleteTreeW(dir);
    printf("[Case 3] PASSED.\n");
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
    std::wstring tempRoot = std::wstring(tempBuf) + L"pd_p2_abi_" + std::to_wstring(GetCurrentProcessId());
    while (!tempRoot.empty() && tempRoot.back() == L'\\') tempRoot.pop_back();
    deleteTreeW(tempRoot);
    TEST_CHECK(CreateDirectoryW(tempRoot.c_str(), nullptr) != FALSE);

    const bool ok1 = runAbiMappingCase(pluginDll, mockLoaderDll, tempRoot);
    const bool ok2 = ok1 && runRingAndLifecycleCase(pluginDll, mockLoaderDll, tempRoot);
    const bool ok3 = ok2 && runRecoveryAndConcurrencyCase(pluginDll, mockLoaderDll, tempRoot);

    deleteTreeW(tempRoot);
    TEST_CHECK(ok1 && ok2 && ok3);

    printf("ALL PHASE 2 ABI/LIFECYCLE TESTS PASSED.\n");
    return 0;
}
