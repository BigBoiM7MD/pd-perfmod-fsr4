#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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
                    return false;
                }
            }
        }
        if (next == std::wstring::npos) break;
        pos = next + 1;
    }
    return true;
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

static bool readAllBytesW(const std::wstring& path, std::vector<uint8_t>& outBytes) {
    const std::wstring ext = toExtendedPathW(path);
    HANDLE hFile = CreateFileW(ext.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    TEST_CHECK_BOOL(hFile != INVALID_HANDLE_VALUE);
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

    // sRGB UAV failure retries with R8G8B8A8_UNORM and SetupDirectX clears g_fallbackTextures
    dev->failUavOnSrgbFormat = true;
    void* texSrgb = api.SimpleInit(3, 1, 2, 1280, 720, false, false, false, false, false, false, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    TEST_CHECK_BOOL(texSrgb != nullptr);
    TEST_CHECK_BOOL(MockD3D12Device::liveResources.load() == 1);
    TEST_CHECK_BOOL(dev->lastCreatedDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM);
    TEST_CHECK_BOOL(dev->lastCreatedDesc.Flags == D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    TEST_CHECK_BOOL(dev->lastInitialState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    TEST_CHECK_BOOL(api.GetRenderWidth(3) == static_cast<int>(1280 * 0.67f));
    dev->failUavOnSrgbFormat = false;

    // Device re-setup clears g_fallbackTextures even without ReleaseUpscaleFeature
    TEST_CHECK_BOOL(api.SetupDirectX(queue, 1) == false);
    TEST_CHECK_BOOL(MockD3D12Device::liveResources.load() == 0);
    TEST_CHECK_BOOL(api.GetRenderWidth(3) == 0);
    TEST_CHECK_BOOL(api.GetRenderHeight(3) == 0);
    TEST_CHECK_BOOL(dev->refCount() == 2);

    // Recreate fallback texture, then verify SetupDirectX(nullptr, 1) teardown clears it too
    void* texTeardown = api.SimpleInit(4, 1, 2, 800, 600, false, false, false, false, false, false, DXGI_FORMAT_R8G8B8A8_UNORM);
    TEST_CHECK_BOOL(texTeardown != nullptr);
    TEST_CHECK_BOOL(MockD3D12Device::liveResources.load() == 1);

    // SetupDirectX(nullptr, 1) releases g_d3d12Device back to baseline 1 and clears fallback textures
    TEST_CHECK_BOOL(api.SetupDirectX(nullptr, 1) == false);
    TEST_CHECK_BOOL(MockD3D12Device::liveResources.load() == 0);
    TEST_CHECK_BOOL(api.GetRenderWidth(4) == 0);
    TEST_CHECK_BOOL(api.GetRenderHeight(4) == 0);
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
    TEST_CHECK_BOOL(dev->refCount() == 2);
    TEST_CHECK_BOOL(queue->refCount() == 1);
    dev->failCreateCommandAllocator = false;

    // 2) CommandList failure rolls back CommandAllocator too
    dev->failCreateCommandList = true;
    TEST_CHECK_BOOL(api.SetupDirectX(queue, 1) == false);
    TEST_CHECK_BOOL(MockD3D12Device::liveAllocators.load() == 0);
    TEST_CHECK_BOOL(MockD3D12Device::liveLists.load() == 0);
    TEST_CHECK_BOOL(MockD3D12Device::liveFences.load() == 0);
    TEST_CHECK_BOOL(dev->refCount() == 2);
    TEST_CHECK_BOOL(queue->refCount() == 1);
    dev->failCreateCommandList = false;

    // 3) Fence failure rolls back allocator and list
    dev->failCreateFence = true;
    TEST_CHECK_BOOL(api.SetupDirectX(queue, 1) == false);
    TEST_CHECK_BOOL(MockD3D12Device::liveAllocators.load() == 0);
    TEST_CHECK_BOOL(MockD3D12Device::liveLists.load() == 0);
    TEST_CHECK_BOOL(MockD3D12Device::liveFences.load() == 0);
    TEST_CHECK_BOOL(dev->refCount() == 2);
    TEST_CHECK_BOOL(queue->refCount() == 1);
    dev->failCreateFence = false;

    // 4) Version enumeration failure (0 versions) rolls back everything
    TEST_CHECK_BOOL(SetEnvironmentVariableA("PD_MOCK_FFX_VERSIONS", "0") != FALSE);
    TEST_CHECK_BOOL(api.SetupDirectX(queue, 1) == false);
    TEST_CHECK_BOOL(MockD3D12Device::liveAllocators.load() == 0);
    TEST_CHECK_BOOL(MockD3D12Device::liveLists.load() == 0);
    TEST_CHECK_BOOL(MockD3D12Device::liveFences.load() == 0);
    TEST_CHECK_BOOL(dev->refCount() == 2);
    TEST_CHECK_BOOL(queue->refCount() == 1);

    // 5) Setup succeeds, then ffxCreateContext fails -> InitUpscaler & SimpleInit return fallback textures
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

    void* simpleFallback = api.SimpleInit(2, 1, 2, 1600, 900, false, false, false, false, false, false, DXGI_FORMAT_R8G8B8A8_UNORM);
    TEST_CHECK_BOOL(simpleFallback != nullptr);
    TEST_CHECK_BOOL(MockD3D12Device::liveResources.load() == 2);
    TEST_CHECK_BOOL(dev->lastCreatedDesc.Flags == D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    TEST_CHECK_BOOL(dev->lastInitialState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    TEST_CHECK_BOOL(api.GetRenderWidth(2) == static_cast<int>(1600 * 0.67f));
    TEST_CHECK_BOOL(api.GetRenderHeight(2) == static_cast<int>(900 * 0.67f));

    api.ReleaseUpscaleFeature(2);
    TEST_CHECK_BOOL(MockD3D12Device::liveResources.load() == 1);
    TEST_CHECK_BOOL(api.GetRenderWidth(2) == 0);
    TEST_CHECK_BOOL(api.GetRenderHeight(2) == 0);

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

static std::wstring buildExactLenShortSegDirW(const std::wstring& baseDir, size_t targetLen) {
    std::wstring dir = baseDir;
    int idx = 0;
    while (dir.size() + 10 < targetLen) {
        wchar_t seg[16];
        swprintf(seg, 16, L"\\s%06d", idx++);
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

static bool runBoundaryDllDirCase(const std::wstring& srcPluginDll,
                                  const std::wstring& srcMockLoaderDll,
                                  const std::wstring& tempRoot) {
    printf("[Case 3] Testing 229..259-char dllDir loader probe + readBackendIni...\n");
    const std::wstring caseRoot = tempRoot + L"\\c3";
    deleteTreeW(caseRoot);
    TEST_CHECK_BOOL(caseRoot.size() < 220);

    const std::wstring dir = buildExactLenShortSegDirW(caseRoot, 241);
    TEST_CHECK_BOOL(dir.size() >= 229 && dir.size() <= 259);
    TEST_CHECK_BOOL(createDirRecursiveW(dir));

    const std::wstring dstPlugin = dir + L"\\PDPerfPlugin.dll";
    const std::wstring dstLoader = dir + L"\\amd_fidelityfx_loader_dx12.dll";
    const std::wstring iniPath   = dir + L"\\pd-perfmod-fsr4.ini";
    const std::wstring logPath   = dir + L"\\pd-perfmod-fsr4.log";
    TEST_CHECK_BOOL(dstPlugin.size() < MAX_PATH);
    TEST_CHECK_BOOL(dstLoader.size() >= MAX_PATH);
    TEST_CHECK_BOOL(iniPath.size() >= MAX_PATH);

    TEST_CHECK_BOOL(CopyFileW(toExtendedPathW(srcPluginDll).c_str(),
                              toExtendedPathW(dstPlugin).c_str(), FALSE) != FALSE);
    TEST_CHECK_BOOL(CopyFileW(toExtendedPathW(srcMockLoaderDll).c_str(),
                              toExtendedPathW(dstLoader).c_str(), FALSE) != FALSE);
    TEST_CHECK_BOOL(writeBytesW(iniPath, "[Logging]\nVerbose=0\n[Backend]\nOutputFormat=10\n"));

    SetEnvironmentVariableA("PD_MOCK_FFX_VERSIONS", "1");
    SetEnvironmentVariableA("PD_MOCK_FFX_CREATE_FAIL", nullptr);

    PluginApi api;
    TEST_CHECK_BOOL(api.load(dstPlugin));

    auto* dev   = new MockD3D12Device();
    auto* queue = new MockD3D12CommandQueue(dev);

    // SetupDirectX probes dir + L"\\amd_fidelityfx_loader_dx12.dll" (len >= MAX_PATH) via \\?\
    TEST_CHECK_BOOL(api.SetupDirectX(queue, 1) == true);

    InitParams params{};
    params.id            = 1;
    params.upscaleMethod = 1;
    params.qualityLevel  = 2;
    params.displaySizeX  = 1920;
    params.displaySizeY  = 1080;
    params.format        = DXGI_FORMAT_R8G8B8A8_UNORM;

    void* tex = api.InitUpscaler(&params);
    TEST_CHECK_BOOL(tex != nullptr);
    TEST_CHECK_BOOL(MockD3D12Device::liveResources.load() == 1);
    // OutputFormat=10 (R16G16B16A16_FLOAT) from >= MAX_PATH INI without 8.3 shrinkage
    TEST_CHECK_BOOL(static_cast<ID3D12Resource*>(tex)->GetDesc().Format == DXGI_FORMAT_R16G16B16A16_FLOAT);

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

    SetEnvironmentVariableA("PD_MOCK_FFX_VERSIONS", nullptr);
    api.unload();

    std::vector<uint8_t> logBytes;
    TEST_CHECK_BOOL(readAllBytesW(logPath, logBytes));
    std::string logStr(logBytes.begin(), logBytes.end());
    TEST_CHECK_BOOL(logStr.find("mode: NON-VERBOSE") != std::string::npos);

    deleteTreeW(caseRoot);
    printf("[Case 3] PASSED (dir=%zu, loader=%zu).\n", dir.size(), dstLoader.size());
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
    std::wstring tempRoot = std::wstring(tempBuf) + L"pd_p1_com_" + std::to_wstring(GetCurrentProcessId());
    while (!tempRoot.empty() && tempRoot.back() == L'\\') tempRoot.pop_back();
    deleteTreeW(tempRoot);
    TEST_CHECK(CreateDirectoryW(tempRoot.c_str(), nullptr) != FALSE);

    const bool ok1 = runNoLoaderCase(pluginDll, tempRoot);
    const bool ok2 = ok1 && runMockLoaderCase(pluginDll, mockLoaderDll, tempRoot);
    const bool ok3 = ok2 && runBoundaryDllDirCase(pluginDll, mockLoaderDll, tempRoot);

    deleteTreeW(tempRoot);
    TEST_CHECK(ok1 && ok2 && ok3);

    printf("ALL PHASE 1 FALLBACK/COM TESTS PASSED.\n");
    return 0;
}
