#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <cstring>
#include <string>

#include "PDPerfPlugin.h"
#include "mock_d3d12.h"

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

static void deleteTreeW(const std::wstring& dir) {
    const std::wstring pattern = dir + L"\\*";
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
                DeleteFileW(child.c_str());
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }
    RemoveDirectoryW(dir.c_str());
}

struct PluginApi {
    HMODULE mod = nullptr;
    bool (__stdcall* SetupDirectX)(void*, int) = nullptr;
    void* (__stdcall* SimpleInit)(int, int, int, int, int, bool, bool, bool, bool, bool, bool, int) = nullptr;
    void* (__stdcall* InitUpscaler)(InitParams*) = nullptr;
    void (__stdcall* SetMotionScaleX)(int, float) = nullptr;
    void (__stdcall* SetMotionScaleY)(int, float) = nullptr;
    int (__stdcall* GetRenderWidth)(int) = nullptr;
    int (__stdcall* GetRenderHeight)(int) = nullptr;
    float (__stdcall* GetOptimalSharpness)(int) = nullptr;
    float (__stdcall* GetOptimalMipmapBias)(int) = nullptr;
    void (__stdcall* ReleaseUpscaleFeature)(int) = nullptr;
    int (__stdcall* GetJitterPhaseCount)(int) = nullptr;
    int (__stdcall* GetJitterOffset)(float*, float*, int, int) = nullptr;
    bool (__stdcall* IsUpscaleMethodAvailable)(int) = nullptr;
    char* (__stdcall* GetUpscaleMethodName)(int) = nullptr;

    bool load(const std::wstring& dllPath) {
        mod = LoadLibraryW(dllPath.c_str());
        TEST_CHECK_BOOL(mod != nullptr);

        SetupDirectX             = reinterpret_cast<decltype(SetupDirectX)>(GetProcAddress(mod, "SetupDirectX"));
        SimpleInit               = reinterpret_cast<decltype(SimpleInit)>(GetProcAddress(mod, "SimpleInit"));
        InitUpscaler             = reinterpret_cast<decltype(InitUpscaler)>(GetProcAddress(mod, "InitUpscaler"));
        SetMotionScaleX          = reinterpret_cast<decltype(SetMotionScaleX)>(GetProcAddress(mod, "SetMotionScaleX"));
        SetMotionScaleY          = reinterpret_cast<decltype(SetMotionScaleY)>(GetProcAddress(mod, "SetMotionScaleY"));
        GetRenderWidth           = reinterpret_cast<decltype(GetRenderWidth)>(GetProcAddress(mod, "GetRenderWidth"));
        GetRenderHeight          = reinterpret_cast<decltype(GetRenderHeight)>(GetProcAddress(mod, "GetRenderHeight"));
        GetOptimalSharpness      = reinterpret_cast<decltype(GetOptimalSharpness)>(GetProcAddress(mod, "GetOptimalSharpness"));
        GetOptimalMipmapBias     = reinterpret_cast<decltype(GetOptimalMipmapBias)>(GetProcAddress(mod, "GetOptimalMipmapBias"));
        ReleaseUpscaleFeature    = reinterpret_cast<decltype(ReleaseUpscaleFeature)>(GetProcAddress(mod, "ReleaseUpscaleFeature"));
        GetJitterPhaseCount      = reinterpret_cast<decltype(GetJitterPhaseCount)>(GetProcAddress(mod, "GetJitterPhaseCount"));
        GetJitterOffset          = reinterpret_cast<decltype(GetJitterOffset)>(GetProcAddress(mod, "GetJitterOffset"));
        IsUpscaleMethodAvailable = reinterpret_cast<decltype(IsUpscaleMethodAvailable)>(GetProcAddress(mod, "IsUpscaleMethodAvailable"));
        GetUpscaleMethodName     = reinterpret_cast<decltype(GetUpscaleMethodName)>(GetProcAddress(mod, "GetUpscaleMethodName"));

        TEST_CHECK_BOOL(SetupDirectX && SimpleInit && InitUpscaler &&
                        SetMotionScaleX && SetMotionScaleY &&
                        GetRenderWidth && GetRenderHeight &&
                        GetOptimalSharpness && GetOptimalMipmapBias &&
                        ReleaseUpscaleFeature && GetJitterPhaseCount &&
                        GetJitterOffset && IsUpscaleMethodAvailable &&
                        GetUpscaleMethodName);
        return true;
    }

    void unload() {
        if (mod) {
            FreeLibrary(mod);
            mod = nullptr;
        }
    }
};

static bool runNoLoaderCase(const std::wstring& srcPluginDll, const std::wstring& tempRoot) {
    printf("[Case 1] Testing SetupDirectX rejections, refcounts, and fallback textures without loader DLL...\n");
    const std::wstring dir = tempRoot + L"\\case1_no_loader";
    deleteTreeW(dir);
    TEST_CHECK_BOOL(CreateDirectoryW(dir.c_str(), nullptr) != FALSE);

    const std::wstring dstPlugin = dir + L"\\PDPerfPlugin.dll";
    TEST_CHECK_BOOL(CopyFileW(srcPluginDll.c_str(), dstPlugin.c_str(), FALSE) != FALSE);

    PluginApi api;
    TEST_CHECK_BOOL(api.load(dstPlugin));

    auto* dev   = new MockD3D12Device();
    auto* queue = new MockD3D12CommandQueue(dev);
    TEST_CHECK_BOOL(dev->refCount() == 1);
    TEST_CHECK_BOOL(queue->refCount() == 1);

    // Rejections leave device/queue refcounts unchanged
    TEST_CHECK_BOOL(api.SetupDirectX(nullptr, 1) == false);
    TEST_CHECK_BOOL(dev->refCount() == 1 && queue->refCount() == 1);
    TEST_CHECK_BOOL(api.SetupDirectX(queue, 0) == false);
    TEST_CHECK_BOOL(dev->refCount() == 1 && queue->refCount() == 1);
    TEST_CHECK_BOOL(api.SetupDirectX(queue, 2) == false);
    TEST_CHECK_BOOL(dev->refCount() == 1 && queue->refCount() == 1);

    // Valid queue/API but absent loader DLL -> false, queue ref unchanged, g_d3d12Device held once
    TEST_CHECK_BOOL(api.SetupDirectX(queue, 1) == false);
    TEST_CHECK_BOOL(queue->refCount() == 1);
    TEST_CHECK_BOOL(dev->refCount() == 2);

    // Re-setup releases prior g_d3d12Device before GetDevice -> refcount does not grow
    TEST_CHECK_BOOL(api.SetupDirectX(queue, 1) == false);
    TEST_CHECK_BOOL(queue->refCount() == 1);
    TEST_CHECK_BOOL(dev->refCount() == 2);

    // InitUpscaler allocates UNORDERED_ACCESS fallback texture
    InitParams params{};
    params.id           = 1;
    params.upscaleMethod = 1;
    params.qualityLevel = 2;
    params.displaySizeX = 1920;
    params.displaySizeY = 1080;
    params.format       = DXGI_FORMAT_R8G8B8A8_UNORM;

    void* tex1 = api.InitUpscaler(&params);
    TEST_CHECK_BOOL(tex1 != nullptr);
    TEST_CHECK_BOOL(MockD3D12Device::liveResources.load() == 1);
    TEST_CHECK_BOOL(dev->lastCreatedDesc.Flags == D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    TEST_CHECK_BOOL(dev->lastInitialState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    TEST_CHECK_BOOL(api.GetRenderWidth(1) == static_cast<int>(1920 * 0.67f));
    TEST_CHECK_BOOL(api.GetRenderHeight(1) == static_cast<int>(1080 * 0.67f));

    // Query/setter gating when !isAvailable()
    api.SetMotionScaleX(1, 2.0f);
    api.SetMotionScaleY(1, 2.0f);
    TEST_CHECK_BOOL(api.GetOptimalSharpness(1) == 0.5f);
    TEST_CHECK_BOOL(api.GetOptimalMipmapBias(1) == 0.0f);
    TEST_CHECK_BOOL(api.GetJitterPhaseCount(1) == 1);
    float jx = 123.0f, jy = 456.0f;
    TEST_CHECK_BOOL(api.GetJitterOffset(&jx, &jy, 0, 1) == -1);
    TEST_CHECK_BOOL(jx == 123.0f && jy == 456.0f);

    // SimpleInit for id=2 increases liveResources to 2
    void* tex2 = api.SimpleInit(2, 1, 2, 1600, 900, false, false, false, false, false, false, DXGI_FORMAT_R8G8B8A8_UNORM);
    TEST_CHECK_BOOL(tex2 != nullptr);
    TEST_CHECK_BOOL(MockD3D12Device::liveResources.load() == 2);
    TEST_CHECK_BOOL(dev->lastCreatedDesc.Flags == D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    TEST_CHECK_BOOL(dev->lastInitialState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // Overwriting id=1 releases the first fallback resource -> liveResources stays 2 (not 3)
    void* tex1b = api.SimpleInit(1, 1, 2, 1280, 720, false, false, false, false, false, false, DXGI_FORMAT_R8G8B8A8_UNORM);
    TEST_CHECK_BOOL(tex1b != nullptr);
    TEST_CHECK_BOOL(MockD3D12Device::liveResources.load() == 2);
    TEST_CHECK_BOOL(api.GetRenderWidth(1) == static_cast<int>(1280 * 0.67f));
    TEST_CHECK_BOOL(api.GetRenderHeight(1) == static_cast<int>(720 * 0.67f));

    api.ReleaseUpscaleFeature(2);
    TEST_CHECK_BOOL(MockD3D12Device::liveResources.load() == 1);
    TEST_CHECK_BOOL(api.GetRenderWidth(2) == 0);
    TEST_CHECK_BOOL(api.GetRenderHeight(2) == 0);

    api.ReleaseUpscaleFeature(1);
    TEST_CHECK_BOOL(MockD3D12Device::liveResources.load() == 0);
    TEST_CHECK_BOOL(api.GetRenderWidth(1) == 0);
    TEST_CHECK_BOOL(api.GetRenderHeight(1) == 0);

    // SetupDirectX(nullptr, 1) releases g_d3d12Device back to baseline 1
    TEST_CHECK_BOOL(api.SetupDirectX(nullptr, 1) == false);
    TEST_CHECK_BOOL(dev->refCount() == 1);
    TEST_CHECK_BOOL(queue->refCount() == 1);

    queue->Release();
    dev->Release();
    TEST_CHECK_BOOL(MockD3D12Device::liveQueues.load() == 0);
    TEST_CHECK_BOOL(MockD3D12Device::liveDevices.load() == 0);

    api.unload();
    deleteTreeW(dir);
    printf("[Case 1] PASSED.\n");
    return true;
}

static bool runMockLoaderCase(const std::wstring& srcPluginDll,
                              const std::wstring& srcMockLoaderDll,
                              const std::wstring& tempRoot) {
    printf("[Case 2] Testing D3D12/version failure rollbacks and createContext fallback with mock loader DLL...\n");
    const std::wstring dir = tempRoot + L"\\case2_mock_loader";
    deleteTreeW(dir);
    TEST_CHECK_BOOL(CreateDirectoryW(dir.c_str(), nullptr) != FALSE);

    const std::wstring dstPlugin = dir + L"\\PDPerfPlugin.dll";
    const std::wstring dstLoader = dir + L"\\amd_fidelityfx_loader_dx12.dll";
    TEST_CHECK_BOOL(CopyFileW(srcPluginDll.c_str(), dstPlugin.c_str(), FALSE) != FALSE);
    TEST_CHECK_BOOL(CopyFileW(srcMockLoaderDll.c_str(), dstLoader.c_str(), FALSE) != FALSE);

    SetEnvironmentVariableA("PD_MOCK_FFX_VERSIONS", nullptr);
    SetEnvironmentVariableA("PD_MOCK_FFX_CREATE_FAIL", nullptr);

    PluginApi api;
    TEST_CHECK_BOOL(api.load(dstPlugin));

    auto* dev   = new MockD3D12Device();
    auto* queue = new MockD3D12CommandQueue(dev);

    // 1) CommandAllocator failure rolls back cleanly
    dev->failCreateCommandAllocator = true;
    TEST_CHECK_BOOL(api.SetupDirectX(queue, 1) == false);
    TEST_CHECK_BOOL(MockD3D12Device::liveAllocators.load() == 0);
    TEST_CHECK_BOOL(MockD3D12Device::liveLists.load() == 0);
    TEST_CHECK_BOOL(MockD3D12Device::liveFences.load() == 0);
    TEST_CHECK_BOOL(queue->refCount() == 1);
    dev->failCreateCommandAllocator = false;

    // 2) CommandList failure rolls back CommandAllocator too
    dev->failCreateCommandList = true;
    TEST_CHECK_BOOL(api.SetupDirectX(queue, 1) == false);
    TEST_CHECK_BOOL(MockD3D12Device::liveAllocators.load() == 0);
    TEST_CHECK_BOOL(MockD3D12Device::liveLists.load() == 0);
    TEST_CHECK_BOOL(MockD3D12Device::liveFences.load() == 0);
    TEST_CHECK_BOOL(queue->refCount() == 1);
    dev->failCreateCommandList = false;

    // 3) Fence failure rolls back allocator and list
    dev->failCreateFence = true;
    TEST_CHECK_BOOL(api.SetupDirectX(queue, 1) == false);
    TEST_CHECK_BOOL(MockD3D12Device::liveAllocators.load() == 0);
    TEST_CHECK_BOOL(MockD3D12Device::liveLists.load() == 0);
    TEST_CHECK_BOOL(MockD3D12Device::liveFences.load() == 0);
    TEST_CHECK_BOOL(queue->refCount() == 1);
    dev->failCreateFence = false;

    // 4) Version enumeration failure (0 versions) rolls back everything
    TEST_CHECK_BOOL(SetEnvironmentVariableA("PD_MOCK_FFX_VERSIONS", "0") != FALSE);
    TEST_CHECK_BOOL(api.SetupDirectX(queue, 1) == false);
    TEST_CHECK_BOOL(MockD3D12Device::liveAllocators.load() == 0);
    TEST_CHECK_BOOL(MockD3D12Device::liveLists.load() == 0);
    TEST_CHECK_BOOL(MockD3D12Device::liveFences.load() == 0);
    TEST_CHECK_BOOL(queue->refCount() == 1);

    // 5) Setup succeeds, then ffxCreateContext fails -> fallback texture is returned and reachable
    TEST_CHECK_BOOL(SetEnvironmentVariableA("PD_MOCK_FFX_VERSIONS", "1") != FALSE);
    TEST_CHECK_BOOL(SetEnvironmentVariableA("PD_MOCK_FFX_CREATE_FAIL", "1") != FALSE);
    TEST_CHECK_BOOL(api.SetupDirectX(queue, 1) == true);
    TEST_CHECK_BOOL(api.IsUpscaleMethodAvailable(1) == true);

    InitParams params{};
    params.id            = 1;
    params.upscaleMethod = 1;
    params.qualityLevel  = 2;
    params.displaySizeX  = 1920;
    params.displaySizeY  = 1080;
    params.format        = DXGI_FORMAT_R8G8B8A8_UNORM;

    void* fallback = api.InitUpscaler(&params);
    TEST_CHECK_BOOL(fallback != nullptr);
    TEST_CHECK_BOOL(MockD3D12Device::liveResources.load() == 1);
    TEST_CHECK_BOOL(dev->lastCreatedDesc.Flags == D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    TEST_CHECK_BOOL(dev->lastInitialState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    TEST_CHECK_BOOL(api.GetRenderWidth(1) == static_cast<int>(1920 * 0.67f));
    TEST_CHECK_BOOL(api.GetRenderHeight(1) == static_cast<int>(1080 * 0.67f));

    api.ReleaseUpscaleFeature(1);
    TEST_CHECK_BOOL(MockD3D12Device::liveResources.load() == 0);
    TEST_CHECK_BOOL(api.GetRenderWidth(1) == 0);
    TEST_CHECK_BOOL(api.GetRenderHeight(1) == 0);

    // Tear down backend state via SetupDirectX(nullptr, 1)
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

    SetEnvironmentVariableA("PD_MOCK_FFX_VERSIONS", nullptr);
    SetEnvironmentVariableA("PD_MOCK_FFX_CREATE_FAIL", nullptr);

    api.unload();
    deleteTreeW(dir);
    printf("[Case 2] PASSED.\n");
    return true;
}

int wmain(int argc, wchar_t** argv) {
    TEST_CHECK(argc >= 3);
    const std::wstring pluginDll     = normalizeAbsPathW(argv[1]);
    const std::wstring mockLoaderDll = normalizeAbsPathW(argv[2]);
    TEST_CHECK(!pluginDll.empty() && !mockLoaderDll.empty());
    TEST_CHECK(GetFileAttributesW(pluginDll.c_str()) != INVALID_FILE_ATTRIBUTES);
    TEST_CHECK(GetFileAttributesW(mockLoaderDll.c_str()) != INVALID_FILE_ATTRIBUTES);

    wchar_t tempBuf[MAX_PATH] = {};
    const DWORD tempLen = GetTempPathW(MAX_PATH, tempBuf);
    TEST_CHECK(tempLen > 0 && tempLen < MAX_PATH);
    std::wstring tempRoot = std::wstring(tempBuf) + L"pd_p1_com_" + std::to_wstring(GetCurrentProcessId());
    while (!tempRoot.empty() && tempRoot.back() == L'\\') tempRoot.pop_back();
    deleteTreeW(tempRoot);
    TEST_CHECK(CreateDirectoryW(tempRoot.c_str(), nullptr) != FALSE);

    const bool ok1 = runNoLoaderCase(pluginDll, tempRoot);
    const bool ok2 = ok1 && runMockLoaderCase(pluginDll, mockLoaderDll, tempRoot);

    deleteTreeW(tempRoot);
    TEST_CHECK(ok1 && ok2);

    printf("ALL PHASE 1 FALLBACK/COM TESTS PASSED.\n");
    return 0;
}
