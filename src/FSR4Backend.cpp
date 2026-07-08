#include "FSR4Backend.h"
#include "Logging.h"
#include "fsr4_overlay.h"
#include "../include/PDPerfPlugin.h"

#include <d3d12.h>
#include <dxgi1_4.h>
#include <unordered_map>
#include <cstring>
#include <exception>
#include <map>

// -----------------------------------------------------------------------
// FFX API type aliases matching the SDK headers exactly
// -----------------------------------------------------------------------
using ffxReturnCode_t = uint32_t;
using ffxContext      = void*;
using ffxStructType_t = uint64_t;
typedef void (*ffxApiMessage)(uint32_t type, const wchar_t* message);

// -----------------------------------------------------------------------
// Base header (used by all descriptors)
// -----------------------------------------------------------------------
struct ffxApiHeader {
    ffxStructType_t type;
    ffxApiHeader*   pNext;
};
using ffxCreateContextDescHeader = ffxApiHeader;
using ffxDispatchDescHeader      = ffxApiHeader;
using ffxQueryDescHeader         = ffxApiHeader;

// -----------------------------------------------------------------------
// ID / flag constants  (from ffx_api.h  /  ffx_upscale.h)
// -----------------------------------------------------------------------
static constexpr uint64_t FFX_API_EFFECT_ID_UPSCALE  = 0x00010000u;
static constexpr uint64_t FFX_API_EFFECT_MASK        = 0x00ff0000u;
static constexpr uint64_t FFX_API_BACKEND_MASK       = 0xff000000u;

static constexpr uint64_t MAKE_EFFECT_SUB_ID(uint64_t e, uint64_t s) { return (e & FFX_API_EFFECT_MASK) | (s & ~FFX_API_EFFECT_MASK); }
static constexpr uint64_t MAKE_BACKEND_SUB_ID(uint64_t b, uint64_t s) { return (b & FFX_API_BACKEND_MASK) | (s & ~FFX_API_BACKEND_MASK); }

// Backend DX12
static constexpr uint64_t FFX_API_BACKEND_ID_DX12                      = 0x00000000u;
static constexpr uint64_t FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12 = MAKE_BACKEND_SUB_ID(FFX_API_BACKEND_ID_DX12, 2);

// Upscale sub-IDs
static constexpr uint64_t FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE              = MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_UPSCALE, 0x00);
static constexpr uint64_t FFX_API_DISPATCH_DESC_TYPE_UPSCALE                    = MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_UPSCALE, 0x01);
static constexpr uint64_t FFX_API_QUERY_DESC_TYPE_UPSCALE_GET_JITTER_PHASE_COUNT = MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_UPSCALE, 0x04);
static constexpr uint64_t FFX_API_QUERY_DESC_TYPE_UPSCALE_GET_JITTER_OFFSET      = MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_UPSCALE, 0x05);
static constexpr uint64_t FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION       = MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_UPSCALE, 0x0b);

// Create flags
static constexpr uint32_t FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE                 = (1 << 0);
static constexpr uint32_t FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS  = (1 << 1);
static constexpr uint32_t FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION = (1 << 2);
static constexpr uint32_t FFX_UPSCALE_ENABLE_DEPTH_INVERTED                     = (1 << 3);
static constexpr uint32_t FFX_UPSCALE_ENABLE_DEPTH_INFINITE                     = (1 << 4);
static constexpr uint32_t FFX_UPSCALE_ENABLE_AUTO_EXPOSURE                      = (1 << 5);
static constexpr uint32_t FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION                 = (1 << 6);

static constexpr uint32_t FFX_UPSCALER_VERSION = ((4 << 22) | (1 << 12) | 1);

// -----------------------------------------------------------------------
// FFX resource state / usage enums  (FfxApiResourceState / FfxApiResourceUsage
// in the SDK's Kits/FidelityFX/api/include/ffx_api_types.h)
//
// CRITICAL: these are BIT-FLAG enums, NOT the old sequential FfxResourceStates
// values, and NOT raw D3D12_RESOURCE_STATES. Wrong values here compile fine but
// make the loader insert bogus barriers -> FSR reads garbage / black screen.
// They MUST match the version of the loader DLL (FFX_UPSCALER_VERSION below).
// -----------------------------------------------------------------------
static constexpr uint32_t FFX_RESOURCE_STATE_COMMON               = (1u << 0); // 1
static constexpr uint32_t FFX_RESOURCE_STATE_UNORDERED_ACCESS     = (1u << 1); // 2
static constexpr uint32_t FFX_RESOURCE_STATE_COMPUTE_READ         = (1u << 2); // 4
static constexpr uint32_t FFX_RESOURCE_STATE_PIXEL_READ           = (1u << 3); // 8
static constexpr uint32_t FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ   =
    (FFX_RESOURCE_STATE_PIXEL_READ | FFX_RESOURCE_STATE_COMPUTE_READ);          // 0xC
static constexpr uint32_t FFX_RESOURCE_STATE_COPY_SRC             = (1u << 4); // 0x10
static constexpr uint32_t FFX_RESOURCE_STATE_COPY_DEST            = (1u << 5); // 0x20
static constexpr uint32_t FFX_RESOURCE_STATE_PRESENT              = (1u << 7); // 0x80
static constexpr uint32_t FFX_RESOURCE_STATE_RENDER_TARGET        = (1u << 8); // 0x100
static constexpr uint32_t FFX_RESOURCE_STATE_DEPTH_ATTACHMENT     = (1u << 9); // 0x200

// FFX resource usage flags (FfxApiResourceUsage, ffx_api_types.h) -- BIT FLAGS.
static constexpr uint32_t FFX_RESOURCE_USAGE_READ_ONLY    = 0;          // no usage flags
static constexpr uint32_t FFX_RESOURCE_USAGE_RENDERTARGET = (1u << 0);  // 1
static constexpr uint32_t FFX_RESOURCE_USAGE_UAV          = (1u << 1);  // 2
static constexpr uint32_t FFX_RESOURCE_USAGE_DEPTHTARGET  = (1u << 2);  // 4

// -----------------------------------------------------------------------
// Descriptor structs  (exact layout from SDK headers – no manual padding)
// -----------------------------------------------------------------------
struct ffxCreateBackendDX12Desc {
    ffxApiHeader header;
    ID3D12Device* device;
};

struct FfxApiDimensions2D {
    uint32_t width;
    uint32_t height;
};

struct ffxCreateContextDescUpscale {
    ffxApiHeader header;
    uint32_t flags;
    FfxApiDimensions2D maxRenderSize;
    FfxApiDimensions2D maxUpscaleSize;
    ffxApiMessage fpMessage;
};

struct ffxCreateContextDescUpscaleVersion {
    ffxApiHeader header;
    uint32_t version;
};

struct FfxApiResourceDescription {
    uint32_t type;
    uint32_t format;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t mipCount;
    uint32_t flags;
    uint32_t usage;
};

struct FfxApiResource {
    void* resource;
    FfxApiResourceDescription description;
    uint32_t state;
};

struct FfxApiFloatCoords2D {
    float x;
    float y;
};

struct ffxDispatchDescUpscale {
    ffxDispatchDescHeader header;
    void* commandList;
    FfxApiResource color;
    FfxApiResource depth;
    FfxApiResource motionVectors;
    FfxApiResource exposure;
    FfxApiResource reactive;
    FfxApiResource transparencyAndComposition;
    FfxApiResource output;
    FfxApiFloatCoords2D jitterOffset;
    FfxApiFloatCoords2D motionVectorScale;
    FfxApiDimensions2D renderSize;
    FfxApiDimensions2D upscaleSize;
    bool enableSharpening;
    float sharpness;
    float frameTimeDelta;
    float preExposure;
    bool reset;
    float cameraNear;
    float cameraFar;
    float cameraFovAngleVertical;
    float viewSpaceToMetersFactor;
    uint32_t flags;
};

struct ffxQueryDescUpscaleGetJitterPhaseCount {
    ffxQueryDescHeader header;
    uint32_t renderWidth;
    uint32_t displayWidth;
    int32_t* pOutPhaseCount;
};

struct ffxQueryDescUpscaleGetJitterOffset {
    ffxQueryDescHeader header;
    int32_t index;
    int32_t phaseCount;
    float* pOutX;
    float* pOutY;
};

struct ffxAllocationCallbacks {
    void* pUserData;
    void* (*alloc)(void*, uint64_t);
    void  (*dealloc)(void*, void*);
};

// -----------------------------------------------------------------------
// Function-pointer types matching __cdecl convention
// -----------------------------------------------------------------------
using PFN_ffxCreateContext  = ffxReturnCode_t (__cdecl*)(ffxContext*, ffxCreateContextDescHeader*, const ffxAllocationCallbacks*);
using PFN_ffxDestroyContext = ffxReturnCode_t (__cdecl*)(ffxContext*, const ffxAllocationCallbacks*);
using PFN_ffxDispatch       = ffxReturnCode_t (__cdecl*)(ffxContext*, const ffxDispatchDescHeader*);
using PFN_ffxQuery          = ffxReturnCode_t (__cdecl*)(ffxContext*, ffxQueryDescHeader*);
using PFN_ffxApiGetResourceDX12 = FfxApiResource(__cdecl*)(void*, uint32_t);

// Verify struct sizes match SDK expectations (MSVC x64)
static_assert(sizeof(FfxApiResourceDescription) == 32, "ResDesc size");
static_assert(sizeof(FfxApiResource) == 48, "Resource size");
static_assert(sizeof(ffxDispatchDescUpscale) == 432, "DispatchDesc size");
static_assert(sizeof(ffxCreateContextDescUpscale) == 48, "CreateDesc size");

// -----------------------------------------------------------------------
// Per-context state
// -----------------------------------------------------------------------
struct UpscaleContext {
    int id;
    int upscaleMethod;
    int qualityLevel;
    int displaySizeX;
    int displaySizeY;
    int format;
    bool isContentHDR;
    bool depthInverted;
    bool motionVectorsJittered;
    bool enableSharpening;
    bool enableAutoExposure;

    ffxContext        ffxCtx         = nullptr;
    ID3D12Resource*   outputTexture  = nullptr;

    // Verification watermark: a small badge copied into this output texture so
    // that, if it appears on screen, we know FSR4's output texture is the one
    // REFramework presents. Drawn via CopyTextureRegion (no shader).
    ID3D12Resource*   watermarkTex   = nullptr;
    int               watermarkW     = 0;
    int               watermarkH     = 0;
    ID3D12Resource*   solidTex       = nullptr; // PD_FSR4_DIAG_SOLID test texture
    ID3D12Device*     device         = nullptr;

    int   renderWidth  = 0;
    int   renderHeight = 0;
    float motionScaleX = 1.0f;
    float motionScaleY = 1.0f;
};

struct FSR4Backend::Impl {
    HMODULE hLoaderDll = nullptr;
    HMODULE hInstance  = nullptr; // PDPerfPlugin.dll handle (for DIAG_SOLID sentinel)
    PFN_ffxCreateContext  ffxCreateContextFn  = nullptr;
    PFN_ffxDestroyContext ffxDestroyContextFn = nullptr;
    PFN_ffxDispatch       ffxDispatchFn       = nullptr;
    PFN_ffxQuery          ffxQueryFn          = nullptr;
    PFN_ffxApiGetResourceDX12 ffxGetResourceFn = nullptr;

    ID3D12Device*        device       = nullptr;
    ID3D12CommandQueue*   commandQueue = nullptr;
    int                   graphicsAPI  = 0;
    bool                  available    = false;

    ID3D12CommandAllocator*    cmdAlloc = nullptr;
    ID3D12GraphicsCommandList* cmdList  = nullptr;
    ID3D12Fence*               fence      = nullptr;
    HANDLE                     fenceEvent = nullptr;
    uint64_t                   fenceValue = 0;

    // Cached directory of PDPerfPlugin.dll (resolved once). Sentinel-file
    // toggles (PD_FSR4_DIAG_SOLID / PD_FSR4_NO_WATERMARK) live here. Caching
    // avoids a GetModuleFileNameW + wstring alloc + GetFileAttributesW syscall
    // every frame just to re-check a path that never changes.
    std::wstring dllDir;
    bool         dllDirResolved = false;
    void resolveDllDir() {
        if (dllDirResolved) return;
        dllDirResolved = true;
        HMODULE hmod = hInstance ? hInstance : GetModuleHandleW(L"PDPerfPlugin.dll");
        if (!hmod) hmod = GetModuleHandleW(nullptr);
        wchar_t dllPath[MAX_PATH] = {};
        if (hmod) GetModuleFileNameW(hmod, dllPath, MAX_PATH);
        std::wstring d = dllPath;
        auto pos = d.find_last_of(L"\\/");
        dllDir = (pos == std::wstring::npos) ? L"." : d.substr(0, pos);
    }

    std::unordered_map<int, UpscaleContext> contexts;
};

// -----------------------------------------------------------------------
// Implementation
// -----------------------------------------------------------------------
FSR4Backend::FSR4Backend() { m_impl = new Impl(); }

FSR4Backend::~FSR4Backend() {
    for (auto& [id, ctx] : m_impl->contexts) {
        if (ctx.ffxCtx && m_impl->ffxDestroyContextFn)
            m_impl->ffxDestroyContextFn(&ctx.ffxCtx, nullptr);
        if (ctx.outputTexture) ctx.outputTexture->Release();
        if (ctx.watermarkTex) ctx.watermarkTex->Release();
        if (ctx.solidTex) ctx.solidTex->Release();
    }
    m_impl->contexts.clear();
    if (m_impl->cmdList)  m_impl->cmdList->Release();
    if (m_impl->cmdAlloc) m_impl->cmdAlloc->Release();
    if (m_impl->fence)    m_impl->fence->Release();
    if (m_impl->fenceEvent) CloseHandle(m_impl->fenceEvent);
    if (m_impl->commandQueue) m_impl->commandQueue->Release();
    if (m_impl->device) m_impl->device->Release();
    if (m_impl->hLoaderDll) FreeLibrary(m_impl->hLoaderDll);
    delete m_impl;
}

static void messageCallback(uint32_t type, const wchar_t* message) {
    if (!message) return;
    if (type == 0)
        Logging::error("FSR4: %ls", message);
    else
        Logging::warn("FSR4: %ls", message);
}

bool FSR4Backend::setup(void* deviceOrQueue, int graphicsAPI) {
    Logging::info("FSR4Backend::setup(graphicsAPI=%d)", graphicsAPI);
    m_impl->graphicsAPI = graphicsAPI;
    // Cache our own module handle so the DIAG_SOLID sentinel file can be found
    // next to PDPerfPlugin.dll regardless of the process' current directory.
    if (!m_impl->hInstance) m_impl->hInstance = GetModuleHandleW(L"PDPerfPlugin.dll");
    if (graphicsAPI == 0) {
        Logging::warn("FSR4Backend: D3D11 not supported, need D3D12");
        return false;
    }

    ID3D12CommandQueue* queue = (ID3D12CommandQueue*)deviceOrQueue;
    if (queue) {
        if (FAILED(queue->GetDevice(IID_PPV_ARGS(&m_impl->device)))) {
            Logging::error("FSR4Backend: Failed to get D3D12 device from queue");
            return false;
        }
        Logging::info("FSR4Backend: Got D3D12 device");
        m_impl->commandQueue = queue;
        m_impl->commandQueue->AddRef();
    }

    const char* loaderNames[] = {
        "amd_fidelityfx_loader_dx12.dll",
        "amd_fidelityfx_loader.dll"
    };
    m_impl->hLoaderDll = nullptr;
    for (auto name : loaderNames) {
        Logging::info("FSR4Backend: Loading %s...", name);
        m_impl->hLoaderDll = LoadLibraryA(name);
        if (m_impl->hLoaderDll) {
            Logging::info("FSR4Backend: Loaded %s", name);
            break;
        }
        Logging::warn("FSR4Backend: Failed to load %s (err=%d)", name, GetLastError());
    }
    if (!m_impl->hLoaderDll) {
        // The FFX loader + FSR4 shader/payload files ship alongside the game, NOT
        // inside PDPerfPlugin.dll. If they're missing the upscaler can't init and
        // REFramework presents the (un-upscaled) fallback texture -> black screen.
        // Tell the user exactly what to drop next to the game .exe.
        Logging::error("=============================================================");
        Logging::error("FSR4Backend: FFX loader NOT FOUND. FSR4 will NOT run (black screen).");
        Logging::error("Place these FSR4 files next to the GAME .exe (or in REFramework's");
        Logging::error("upscaler folder): amd_fidelityfx_loader_dx12.dll (and the FSR4");
        Logging::error("shader/payload files it depends on). See the mod's install notes.");
        Logging::error("=============================================================");
        return false;
    }

    m_impl->ffxCreateContextFn  = (PFN_ffxCreateContext)GetProcAddress(m_impl->hLoaderDll, "ffxCreateContext");
    m_impl->ffxDestroyContextFn = (PFN_ffxDestroyContext)GetProcAddress(m_impl->hLoaderDll, "ffxDestroyContext");
    m_impl->ffxDispatchFn       = (PFN_ffxDispatch)GetProcAddress(m_impl->hLoaderDll, "ffxDispatch");
    m_impl->ffxQueryFn          = (PFN_ffxQuery)GetProcAddress(m_impl->hLoaderDll, "ffxQuery");
    m_impl->ffxGetResourceFn    = (PFN_ffxApiGetResourceDX12)GetProcAddress(m_impl->hLoaderDll, "ffxApiGetResourceDX12");

    if (!m_impl->ffxCreateContextFn || !m_impl->ffxDestroyContextFn || !m_impl->ffxDispatchFn) {
        Logging::warn("FSR4Backend: Missing required ffxApi entry points");
        FreeLibrary(m_impl->hLoaderDll);
        m_impl->hLoaderDll = nullptr;
        return false;
    }

    Logging::info("FSR4Backend: ffxApi loaded successfully");

    // Create internal command allocator and list for when REFramework doesn't provide one
    if (SUCCEEDED(m_impl->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&m_impl->cmdAlloc)))) {
        if (FAILED(m_impl->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                m_impl->cmdAlloc, nullptr, IID_PPV_ARGS(&m_impl->cmdList)))) {
            Logging::warn("FSR4Backend: Failed to create command list");
        } else {
            m_impl->cmdList->Close(); // Reset() requires the list to be closed
        }
    } else {
        Logging::warn("FSR4Backend: Failed to create command allocator");
    }

    m_impl->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_impl->fence));
    m_impl->fenceEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);

    m_impl->available = true;
    return true;
}

bool FSR4Backend::isAvailable() const { return m_impl->available; }
bool FSR4Backend::isReady() const { return m_impl->available && !m_impl->contexts.empty(); }

static FfxApiResource makeResource(ID3D12Resource* res, uint32_t state, uint32_t usage) {
    FfxApiResource r = {};
    r.resource = res;
    r.state = state;
    if (res) {
        D3D12_RESOURCE_DESC d = res->GetDesc();
        uint32_t fmt = (uint32_t)d.Format;
        if (fmt == 1)  fmt = 2;   // R32G32B32A32_TYPELESS -> FLOAT
        else if (fmt == 5)  fmt = 6;   // R32G32B32_TYPELESS -> FLOAT
        else if (fmt == 9)  fmt = 10;  // R16G16B16A16_TYPELESS -> FLOAT
        else if (fmt == 15) fmt = 16;  // R32G32_TYPELESS -> FLOAT
        else if (fmt == 19) fmt = 21;  // R32G8X24_TYPELESS -> R32_FLOAT_X8X24_TYPELESS
        else if (fmt == 23) fmt = 24;  // R10G10B10A2_TYPELESS -> UNORM
        else if (fmt == 27) fmt = 28;  // R8G8B8A8_TYPELESS -> UNORM
        else if (fmt == 53) fmt = 54;  // R16G16_TYPELESS -> FLOAT
        else if (fmt == 59) fmt = 41;  // R32_TYPELESS -> R32_FLOAT
        r.description.type    = (uint32_t)d.Dimension;
        r.description.format  = fmt;
        r.description.width   = (uint32_t)d.Width;
        r.description.height  = d.Height;
        r.description.depth   = d.DepthOrArraySize;
        r.description.mipCount = d.MipLevels;
        r.description.flags   = 0;
        r.description.usage   = (uint32_t)d.Flags | usage;
    }
    return r;
}

// -----------------------------------------------------------------------
// Verification watermark + DIAG solid texture
// The texture-creation logic (incl. the proven DEFAULT-buffer->texture fill)
// now lives in fsr4_overlay.cpp / fsr4_overlay.h so it can be debugged and
// re-verified in isolation. See that file for the full rationale. Disabled by
// compiling with -DFSR4_NO_WATERMARK.
// -----------------------------------------------------------------------

void* FSR4Backend::createContext(int id, int upscaleMethod, int qualityLevel,
                                  int displaySizeX, int displaySizeY,
                                  bool isContentHDR, bool depthInverted, bool yAxisInverted,
                                  bool motionVectorsJittered, bool enableSharpening,
                                  bool enableAutoExposure, int format) {
    Logging::info("FSR4Backend::createContext(id=%d method=%d qual=%d %dx%d fmt=%d)",
                  id, upscaleMethod, qualityLevel, displaySizeX, displaySizeY, format);

    if (!m_impl->available || !m_impl->device) {
        Logging::error("FSR4Backend: not available");
        return nullptr;
    }

    UpscaleContext ctx;
    ctx.id                    = id;
    ctx.upscaleMethod         = upscaleMethod;
    ctx.qualityLevel          = qualityLevel;
    ctx.displaySizeX          = displaySizeX;
    ctx.displaySizeY          = displaySizeY;
    ctx.format                = format;
    ctx.isContentHDR          = isContentHDR;
    ctx.depthInverted         = depthInverted;
    ctx.motionVectorsJittered = motionVectorsJittered;
    ctx.enableSharpening      = enableSharpening;
    ctx.enableAutoExposure    = enableAutoExposure;
    ctx.device                = m_impl->device;

    float scale;
    switch (qualityLevel) {
        case 0: scale = 2.0f; break;
        case 1: scale = 1.7f; break;
        case 2: scale = 1.5f; break;
        case 3: scale = 3.0f; break;
        default: scale = 1.5f;
    }
    ctx.renderWidth  = (int)(displaySizeX / scale + 0.5f);
    ctx.renderHeight = (int)(displaySizeY / scale + 0.5f);

    Logging::info("FSR4Backend: render=%dx%d upscale=%dx%d scale=%.2f",
                  ctx.renderWidth, ctx.renderHeight, displaySizeX, displaySizeY, scale);

    uint32_t flags = 0;
    if (isContentHDR)          flags |= FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE;
    if (depthInverted)         flags |= FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
    if (motionVectorsJittered) flags |= FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION;
    if (enableAutoExposure)    flags |= FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;

    Logging::info("FSR4Backend: flags=0x%x HDR=%d depthInv=%d mvJitter=%d autoExp=%d sharp=%d",
                  flags, isContentHDR, depthInverted, motionVectorsJittered,
                  enableAutoExposure, enableSharpening);

    // pNext chain: upscaleDesc -> versionDesc -> backendDesc
    ffxCreateContextDescUpscaleVersion versionDesc = {};
    versionDesc.header.type  = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    versionDesc.header.pNext = nullptr;
    versionDesc.version      = FFX_UPSCALER_VERSION;

    ffxCreateBackendDX12Desc backendDesc = {};
    backendDesc.header.type  = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backendDesc.header.pNext = nullptr;
    backendDesc.device       = m_impl->device;

    ffxCreateContextDescUpscale upscaleDesc = {};
    upscaleDesc.header.type             = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    upscaleDesc.header.pNext            = &versionDesc.header;
    versionDesc.header.pNext            = &backendDesc.header;
    upscaleDesc.flags                  = flags;
    upscaleDesc.maxRenderSize.width    = (uint32_t)ctx.renderWidth;
    upscaleDesc.maxRenderSize.height   = (uint32_t)ctx.renderHeight;
    upscaleDesc.maxUpscaleSize.width   = (uint32_t)displaySizeX;
    upscaleDesc.maxUpscaleSize.height  = (uint32_t)displaySizeY;
    upscaleDesc.fpMessage             = messageCallback;

    Logging::info("FSR4Backend: Calling ffxCreateContext...");

    ffxContext ffxCtx = nullptr;
    ffxReturnCode_t ret = m_impl->ffxCreateContextFn(&ffxCtx, &upscaleDesc.header, nullptr);

    Logging::info("FSR4Backend: ffxCreateContext returned ret=%d ctx=%p", ret, ffxCtx);

    if (ret != 0 || ffxCtx == nullptr) {
        // ffxCreateContext fails when the loader's FSR4 shader/payload files are
        // missing or version-mismatched -> upscaler can't run -> black screen.
        Logging::error("=============================================================");
        Logging::error("FSR4Backend: ffxCreateContext failed (code=%d). The FSR4 payload", ret);
        Logging::error("files required by amd_fidelityfx_loader_dx12.dll are missing or the");
        Logging::error("loader version is wrong. Verify the FSR4 files match FFX version");
        Logging::error("0x%x. FSR4 will NOT run (black screen).", (unsigned)FFX_UPSCALER_VERSION);
        Logging::error("=============================================================");
        return nullptr;
    }

    Logging::info("FSR4Backend: ffxCreateContext SUCCEEDED");
    ctx.ffxCtx = ffxCtx;

    // Create output texture
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width              = (UINT64)displaySizeX;
    texDesc.Height             = (UINT)displaySizeY;
    texDesc.DepthOrArraySize   = 1;
    texDesc.MipLevels          = 1;
    texDesc.Format             = (DXGI_FORMAT)format;
    texDesc.SampleDesc.Count   = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags              = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    ID3D12Resource* outputTexture = nullptr;
    HRESULT hr = m_impl->device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
        &texDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&outputTexture));
    if (FAILED(hr)) {
        Logging::error("FSR4Backend: Failed to create output texture hr=%08x", hr);
        m_impl->ffxDestroyContextFn(&ffxCtx, nullptr);
        return nullptr;
    }

    ctx.outputTexture = outputTexture;
    m_impl->contexts[id] = ctx;

    // REFramework reads this texture as m_upscaled_textures[id-1] (see
    // TemporalUpscaler.cpp) and copies it to the backbuffer itself, so the
    // return value (outputTexture) is all it needs. No global map required.
    Logging::info("FSR4Backend: context id=%d ready, texture=%p", id, outputTexture);
    return outputTexture;
}

void FSR4Backend::evaluate(UpscaleParams* params) {
    if (!params) return;
    evaluate(params->id, params->color, params->motionVector, params->depth,
             params->destination, (int)params->renderSizeX, (int)params->renderSizeY,
             params->sharpness, params->jitterOffsetX, params->jitterOffsetY,
             params->motionScaleX, params->motionScaleY, params->reset,
             params->nearPlane, params->farPlane, params->verticalFOV,
             params->execute, params->cmdList);
}

void FSR4Backend::evaluate(int id, void* color, void* motionVector, void* depth,
                            void* destination, int renderSizeX, int renderSizeY,
                            float sharpness, float jitterOffsetX, float jitterOffsetY,
                            float motionScaleX, float motionScaleY, bool reset,
                            float nearPlane, float farPlane, float verticalFOV,
                            bool execute, void* cmdList) {
    // --- Diagnostics (debug builds only) ------------------------------------
    // Gated behind FSR4_DIAGNOSTICS. Define it for a debugging release to get the
    // per-second CALLED counter, the one-time cmdList PROBE, and the true
    // in-function CPU timer. Off in normal builds so Release stays clean.
#ifndef FSR4_DIAGNOSTICS
    (void)cmdList; (void)destination; (void)color; (void)motionVector; (void)depth; (void)execute;
#else
    // Diagnostic: log entry (rate-limited to ~1/sec) so we can see whether
    // REFramework is invoking evaluate() at all, and why it might bail.
    {
        static long long s_last = 0; static int s_count = 0;
        long long now = (long long)GetTickCount64();
        if (now - s_last > 1000) {
            Logging::info("FSR4Backend::evaluate(id=%d) CALLED %d times in last sec | color=%p mv=%p depth=%p dst=%p exec=%d",
                          id, s_count, color, motionVector, depth, destination, (int)execute);
            s_last = now; s_count = 0;
        }
        s_count++;
    }

    // True per-call CPU timing: stamp entry here, measure elapsed at function
    // exit. (The old timer measured the gap BETWEEN calls = 1/fps, not CPU cost,
    // so it always read ~4.4ms at 227fps and was useless for finding hot spots.)
    long long s_entryT = (long long)GetTickCount64();
#endif

    auto it = m_impl->contexts.find(id);
    if (it == m_impl->contexts.end() || !it->second.ffxCtx || !m_impl->ffxDispatchFn) {
        static long long s_last = 0;
        long long now = (long long)GetTickCount64();
        if (now - s_last > 2000) {
            Logging::warn("FSR4Backend::evaluate(id=%d): no context/dispatch fn (map size=%zu, ffxDispatch=%p)",
                          id, m_impl->contexts.size(), (void*)m_impl->ffxDispatchFn);
            s_last = now;
        }
        return;
    }
    if (!execute) {
        static long long s_last = 0;
        long long now = (long long)GetTickCount64();
        if (now - s_last > 2000) {
            Logging::info("FSR4Backend::evaluate(id=%d): skipping (execute=false)", id);
            s_last = now;
        }
        return;
    }

    auto& ctx = it->second;

    void* effectiveCmdList = cmdList;
    void* effectiveDst = destination;
    if (!destination)
        effectiveDst = ctx.outputTexture;

    if (!cmdList) {
        if (!m_impl->cmdAlloc || !m_impl->cmdList) return;

        if (m_impl->fence && m_impl->fenceValue > 0 &&
            m_impl->fence->GetCompletedValue() < m_impl->fenceValue) {
            m_impl->fence->SetEventOnCompletion(m_impl->fenceValue, m_impl->fenceEvent);
            WaitForSingleObject(m_impl->fenceEvent, INFINITE);
        }

        if (FAILED(m_impl->cmdAlloc->Reset())) return;
        if (FAILED(m_impl->cmdList->Reset(m_impl->cmdAlloc, nullptr))) return;
        effectiveCmdList = m_impl->cmdList;
    }

    // One-time diagnostic (FSR4_DIAGNOSTICS only): how does REFramework hand us
    // the command list? If nullptr we spin up our OWN ID3D12GraphicsCommandList
    // and execute+Signal it every frame (see L862). This is parity with every
    // upscaler (REFramework passes a null list to all of them via UpscaleParams),
    // so it is NOT a gap source — but the probe is useful for debugging Release.
#ifndef FSR4_DIAGNOSTICS
    (void)cmdList;
#else
    {
        static bool s_logged = false;
        if (!s_logged) {
            s_logged = true;
            Logging::info("FSR4Backend: PROBE cmdList=%s effectiveDst=%s -> %s submit path",
                          cmdList ? "provided" : "NULL",
                          destination ? "provided" : "internal-output",
                          cmdList ? "record-into-REFramework-list (no extra submit)"
                                  : "OWN internal command list (ExecuteCommandLists+Signal per frame)");
        }
    }
#endif

    ffxDispatchDescUpscale dd = {};
    dd.header.type  = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    dd.header.pNext = nullptr;
    dd.commandList  = effectiveCmdList;
    // Inputs are shader-read resources the engine just rendered into. The FFX
    // loader expects its OWN state enum (not raw D3D12 states). COMMON lets the
    // backend add the correct barriers; UNDEFINED (0) here would make FSR4 fail
    // to read the game's color/depth/motion -> black output.
    dd.color          = makeResource((ID3D12Resource*)color, FFX_RESOURCE_STATE_COMMON, FFX_RESOURCE_USAGE_READ_ONLY);
    dd.depth          = makeResource((ID3D12Resource*)depth, FFX_RESOURCE_STATE_COMMON, FFX_RESOURCE_USAGE_READ_ONLY);
    dd.motionVectors  = makeResource((ID3D12Resource*)motionVector, FFX_RESOURCE_STATE_COMMON, FFX_RESOURCE_USAGE_READ_ONLY);
    dd.exposure       = makeResource(nullptr, 0, 0);
    dd.reactive       = makeResource(nullptr, 0, 0);
    dd.transparencyAndComposition = makeResource(nullptr, 0, 0);
    // Output is written by FSR4 as a UAV. REFramework's copier then copies it
    // from UAV -> backbuffer (PRESENT), so it must be left in UAV state.
    dd.output         = makeResource((ID3D12Resource*)effectiveDst, FFX_RESOURCE_STATE_UNORDERED_ACCESS, FFX_RESOURCE_USAGE_UAV);

    dd.jitterOffset.x          = jitterOffsetX;
    dd.jitterOffset.y          = jitterOffsetY;
    dd.motionVectorScale.x     = ctx.motionScaleX;
    dd.motionVectorScale.y     = ctx.motionScaleY;
    dd.renderSize.width        = (uint32_t)ctx.renderWidth;
    dd.renderSize.height       = (uint32_t)ctx.renderHeight;
    dd.upscaleSize.width       = (uint32_t)ctx.displaySizeX;
    dd.upscaleSize.height      = (uint32_t)ctx.displaySizeY;
    dd.enableSharpening        = ctx.enableSharpening;
    // REFramework's "Sharpness Amount" slider is authored for FSR3 and ranges
    // 0.0..5.0, defaulting to 0.0. The FFX upscale API (ffx_upscale.h) defines
    // `sharpness` as 0..1 (1 = max RCAS). So we (a) clamp the slider's out-of-
    // spec range into 0..1, and (b) if the user enabled the "Sharpness" toggle
    // but left the amount at its 0.0 default, fall back to a sane strength so
    // the toggle isn't a dead switch. This is the fix for "the sharpness slider
    // does nothing" — it was being forwarded as 0.0 the whole time.
    {
        float s = sharpness;
        if (s < 0.0f) s = 0.0f;
        if (s > 1.0f) s = 1.0f;
        if (ctx.enableSharpening && s <= 0.0f) s = 0.8f; // toggle on, amount at default 0 -> apply moderate RCAS
        dd.sharpness = s;
    }
    dd.frameTimeDelta          = 16.6f;
    dd.preExposure             = 1.0f;
    dd.reset                   = reset;
    dd.cameraNear              = nearPlane;
    dd.cameraFar               = farPlane;
    dd.cameraFovAngleVertical  = verticalFOV;
    dd.viewSpaceToMetersFactor = 1.0f;
    dd.flags                   = 0;

    try {
        ffxReturnCode_t ret = m_impl->ffxDispatchFn(&ctx.ffxCtx, &dd.header);
        if (ret != 0) {
            Logging::error("FSR4Backend: ffxDispatch returned code=%d", ret);
            return;
        }
        // NOTE: The backbuffer copy is performed by REFramework's TemporalUpscaler
        // (it copies m_upscaled_textures[index] -> backbuffer after EvaluateUpscaler).
        // We just leave the result in ctx.outputTexture, which is what we returned
        // from InitUpscaler. No manual swap-chain copy is needed.

        // Rate-limited success log + one-time input dimensions (aliasing debug).
        {
            static long long s_last = 0;
            static bool s_sizesLogged = false;
            long long now = (long long)GetTickCount64();
            if (!s_sizesLogged) {
                s_sizesLogged = true;
                D3D12_RESOURCE_DESC cd = ((ID3D12Resource*)color)->GetDesc();
                D3D12_RESOURCE_DESC dd2 = ((ID3D12Resource*)depth)->GetDesc();
                D3D12_RESOURCE_DESC md = ((ID3D12Resource*)motionVector)->GetDesc();
                Logging::info("FSR4Backend: inputs color=%ux%u fmt=%d depth=%ux%u fmt=%d mv=%ux%u fmt=%d | out=%ux%u fmt=%d",
                              (unsigned)cd.Width, cd.Height, (int)cd.Format,
                              (unsigned)dd2.Width, dd2.Height, (int)dd2.Format,
                              (unsigned)md.Width, md.Height, (int)md.Format,
                              ctx.displaySizeX, ctx.displaySizeY, ctx.format);
            }
            if (now - s_last > 2000) {
                s_last = now;
                Logging::info("FSR4Backend: ffxDispatch SUCCEEDED id=%d (out=%p fmt=%d %ux%u) reset=%d jitter=(%.4f,%.4f) mvScale=(%.4f,%.4f) rcas=%s sharp=%.3f",
                              id, effectiveDst, ctx.format, ctx.displaySizeX, ctx.displaySizeY,
                              (int)reset, jitterOffsetX, jitterOffsetY,
                              ctx.motionScaleX, ctx.motionScaleY,
                              ctx.enableSharpening ? "ON" : "off", dd.sharpness);
            }
        }

    // --- Decisive present-chain diagnostic (sentinel FILE, no env-var/quoting) --
    // Drop a file named "PD_FSR4_DIAG_SOLID" next to the DLL (in the game dir,
    // where PDPerfPlugin.dll lives) to engage solid-green mode. NO recompile,
    // NO Steam launch options, NO env-var propagation needed. Delete the file
    // to return to normal FSR4. This is the ground-truth present-chain test:
    // if the screen turns green, our output texture IS what REFramework presents.
    // We log every step so a run with the file but no green is still conclusive.
#ifndef FSR4_NO_WATERMARK
    {
        // Re-check the sentinel only a few times/sec (throttled) so toggling the
        // file still works without a restart, but we don't burn a GetFileAttributesW
        // syscall + wstring alloc every frame. dllDir is cached once at setup.
        static int s_lastMode = -1;
        static long long s_lastPoll = 0;
        long long nowPoll = (long long)GetTickCount64();
        if (nowPoll - s_lastPoll > 250) {
            s_lastPoll = nowPoll;
            m_impl->resolveDllDir();
            std::wstring sentinel = m_impl->dllDir + L"\\PD_FSR4_DIAG_SOLID";
            bool on = (GetFileAttributesW(sentinel.c_str()) != INVALID_FILE_ATTRIBUTES);
            if (on != (s_lastMode == 1)) {
                s_lastMode = on ? 1 : 0;
                Logging::info("FSR4Backend: DIAG_SOLID mode %s (sentinel=%ls)",
                              on ? "ON — painting output GREEN" : "OFF — normal FSR4",
                              sentinel.c_str());
            }
            if (on) {
                if (!ctx.solidTex) {
                    ctx.solidTex = Fsr4Overlay::createSolidTexture(m_impl->device, m_impl->commandQueue, ctx.displaySizeX,
                                                  ctx.displaySizeY, (DXGI_FORMAT)ctx.format,
                                                  0xC00FFC00); // opaque GREEN in R10G10B10A2
                    Logging::info("FSR4Backend: DIAG_SOLID created solidTex=%p", (void*)ctx.solidTex);
                }
                if (ctx.solidTex && effectiveCmdList && effectiveDst) {
                    ID3D12GraphicsCommandList* cl = (ID3D12GraphicsCommandList*)effectiveCmdList;
                    D3D12_RESOURCE_BARRIER b = {};
                    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    b.Transition.pResource = (ID3D12Resource*)effectiveDst;
                    b.Transition.Subresource = 0;
                    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
                    cl->ResourceBarrier(1, &b);
                    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
                    dst.pResource = (ID3D12Resource*)effectiveDst;
                    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    dst.SubresourceIndex = 0;
                    src.pResource = ctx.solidTex;
                    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    src.SubresourceIndex = 0;
                    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                    cl->ResourceBarrier(1, &b);
                    static long long s_last = 0;
                    long long now = (long long)GetTickCount64();
                    if (now - s_last > 2000) {
                        s_last = now;
                        Logging::info("FSR4Backend: DIAG_SOLID painted output GREEN (out=%p cmdList=%p)",
                                      effectiveDst, effectiveCmdList);
                    }
                }
                // Submit the internal command list if REFramework handed us none
                // (otherwise the green copy above is recorded but never executed).
                if (!cmdList && effectiveCmdList && m_impl->commandQueue) {
                    ID3D12GraphicsCommandList* cl2 = (ID3D12GraphicsCommandList*)effectiveCmdList;
                    cl2->Close();
                    ID3D12CommandList* lists[] = { cl2 };
                    m_impl->commandQueue->ExecuteCommandLists(1, lists);
                    // Signal for allocator lifetime only — no per-frame CPU stall
                    // (see normal path below for rationale).
                    m_impl->commandQueue->Signal(m_impl->fence, ++m_impl->fenceValue);
                    return; // skip FSR4 entirely in solid mode
                }
            }
        }
    }
#endif

        // --- Verification watermark (if enabled) -----------------------------
        // Draw a small "FSR4" badge into the output texture's top-left corner.
        // The output is currently in UAV state from the dispatch; transition it
        // to COPY_DEST, copy the badge, then back to UAV so REFramework's copier
        // sees it in the expected state.
        // Runtime kill-switch: drop a file named "PD_FSR4_NO_WATERMARK" next to
        // the DLL to suppress the badge with NO recompile (mirrors the
        // PD_FSR4_DIAG_SOLID sentinel). Useful for A/B perf comparisons.
        {
            // Runtime kill-switch: file "PD_FSR4_NO_WATERMARK" next to the DLL
            // suppresses the badge with NO recompile. Poll a few times/sec only;
            // dllDir is cached once so this is a single cheap GetFileAttributesW
            // every ~250ms, not a GetModuleFileNameW + wstring alloc every frame.
            static int s_lastWm = -1;
            static long long s_lastWmPoll = 0;
            long long nowWm = (long long)GetTickCount64();
            if (nowWm - s_lastWmPoll > 250) {
                s_lastWmPoll = nowWm;
                m_impl->resolveDllDir();
                bool wm_off = (GetFileAttributesW((m_impl->dllDir + L"\\PD_FSR4_NO_WATERMARK").c_str()) != INVALID_FILE_ATTRIBUTES);
                if (wm_off != (s_lastWm == 1)) {
                    s_lastWm = wm_off ? 1 : 0;
                    Logging::info("FSR4Backend: watermark badge %s (sentinel=%ls\\PD_FSR4_NO_WATERMARK)",
                                  wm_off ? "DISABLED via file" : "ENABLED", m_impl->dllDir.c_str());
                }
            }
            bool wm_off = (s_lastWm == 1);
            if (!wm_off && m_impl->device && effectiveDst) {
                if (!ctx.watermarkTex) {
                    ctx.watermarkTex = Fsr4Overlay::createWatermarkTexture(m_impl->device, m_impl->commandQueue,
                                                          ctx.watermarkW, ctx.watermarkH,
                                                          (DXGI_FORMAT)ctx.format);
                    if (ctx.watermarkTex) {
                        Logging::info("FSR4Backend: watermark texture created %dx%d (DEFAULT heap, COPY_SOURCE) — will be copied into output each frame",
                                      ctx.watermarkW, ctx.watermarkH);
                    } else {
                        Logging::error("FSR4Backend: watermark texture creation FAILED (see Fsr4Overlay::createWatermarkTexture log above)");
                    }
                }
                if (ctx.watermarkTex && ctx.watermarkW > 0 && ctx.watermarkH > 0) {
                    ID3D12GraphicsCommandList* cl = (ID3D12GraphicsCommandList*)effectiveCmdList;
                    D3D12_RESOURCE_BARRIER b = {};
                    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    b.Transition.pResource = (ID3D12Resource*)effectiveDst;
                    b.Transition.Subresource = 0;
                    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
                    cl->ResourceBarrier(1, &b);

                    D3D12_TEXTURE_COPY_LOCATION dst = {};
                    dst.pResource = (ID3D12Resource*)effectiveDst;
                    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    dst.SubresourceIndex = 0;
                    D3D12_TEXTURE_COPY_LOCATION src = {};
                    src.pResource = ctx.watermarkTex;
                    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    src.SubresourceIndex = 0;
                    cl->CopyTextureRegion(&dst, 8, 8, 0, &src, nullptr);
                    // CopyTextureRegion returns void; a format mismatch (the old bug)
                    // would have surfaced as a device-removed error. Add a one-time
                    // confirmation that the badge layout is valid.
                    static bool s_badgeLogged = false;
                    if (!s_badgeLogged) {
                        s_badgeLogged = true;
                        Logging::info("FSR4Backend: watermark copied (out fmt=%d badge fmt=%d) — badge should now be visible",
                                      ctx.format, (int)ctx.watermarkTex->GetDesc().Format);
                    }

                    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                    cl->ResourceBarrier(1, &b);
                }
            }
        }

    } catch (const std::exception& e) {
        Logging::error("FSR4Backend: ffxDispatch exception: %s", e.what());
        return;
    } catch (...) {
        Logging::error("FSR4Backend: ffxDispatch CRASHED");
        return;
    }

    if (!cmdList && effectiveCmdList && m_impl->commandQueue) {
        ID3D12GraphicsCommandList* cl = (ID3D12GraphicsCommandList*)effectiveCmdList;
        cl->Close();

        ID3D12CommandList* lists[] = { cl };
        m_impl->commandQueue->ExecuteCommandLists(1, lists);
        // Signal the fence so the allocator isn't reset while this work is still
        // in flight (needed for allocator lifetime), but DO NOT block on it.
        // The old code did WaitForSingleObject(..., INFINITE) here every frame,
        // which forced a full CPU->GPU sync and serialized the pipeline — the
        // cause of the FSR4 perf regression (GPU left ~23% idle, CPU-bound).
        // REFramework copies our output on the SAME command queue (copier.copy
        // -> backbuffer), so in-order queue execution already guarantees the
        // upscale finishes before the copy reads it. Forward progress is bounded
        // by the pre-reset check at 585 via the next frame's WaitForSingleObject.
        m_impl->commandQueue->Signal(m_impl->fence, ++m_impl->fenceValue);
    }

    // --- True per-call CPU timing (FSR4_DIAGNOSTICS only; scoped entry->here) --
    // Measures THIS function's CPU work per frame. Off in normal builds.
#ifndef FSR4_DIAGNOSTICS
    (void)0;
#else
    {
        static long long s_lastLog = 0;
        static double s_msSum = 0; static long long s_n = 0;
        long long t1 = (long long)GetTickCount64();
        s_msSum += (double)(t1 - s_entryT); s_n++;
        if (t1 - s_lastLog > 2000) {
            s_lastLog = t1;
            double avg = s_n ? (s_msSum / (double)s_n) : 0.0;
            Logging::info("FSR4Backend: evaluate() CPU avg=%.4f ms over %lld calls (true in-function cost)",
                          avg, (long long)s_n);
            s_msSum = 0; s_n = 0;
        }
    }
#endif
}

void FSR4Backend::release(int id) {
    auto it = m_impl->contexts.find(id);
    if (it != m_impl->contexts.end()) {
        if (it->second.ffxCtx && m_impl->ffxDestroyContextFn)
            m_impl->ffxDestroyContextFn(&it->second.ffxCtx, nullptr);
        if (it->second.outputTexture) {
            it->second.outputTexture->Release();
        }
        m_impl->contexts.erase(it);
        Logging::info("FSR4Backend: Released context id=%d", id);
    }
}

void FSR4Backend::setMotionScaleX(int id, float scale) {
    auto it = m_impl->contexts.find(id);
    if (it != m_impl->contexts.end()) it->second.motionScaleX = scale;
}

void FSR4Backend::setMotionScaleY(int id, float scale) {
    auto it = m_impl->contexts.find(id);
    if (it != m_impl->contexts.end()) it->second.motionScaleY = scale;
}

int FSR4Backend::getRenderWidth(int id) const {
    auto it = m_impl->contexts.find(id);
    return it != m_impl->contexts.end() ? it->second.renderWidth : 0;
}

int FSR4Backend::getRenderHeight(int id) const {
    auto it = m_impl->contexts.find(id);
    return it != m_impl->contexts.end() ? it->second.renderHeight : 0;
}

float FSR4Backend::getOptimalSharpness(int id) const { return 0.5f; }
float FSR4Backend::getOptimalMipmapBias(int id) const { return 0.0f; }

int FSR4Backend::getJitterPhaseCount(int id) const {
    auto it = m_impl->contexts.find(id);
    // The pd-upscaler contract's GetJitterPhaseCount export DOES receive the
    // real evaluate id (REFramework passes get_evaluate_id() = 1 for the left
    // eye), so this usually hits. Fall back to the first live context only if
    // the id somehow doesn't resolve, so we never return a useless 1 silently.
    if (it == m_impl->contexts.end() && !m_impl->contexts.empty())
        it = m_impl->contexts.begin();
    if (it != m_impl->contexts.end() && it->second.ffxCtx && m_impl->ffxQueryFn) {
        int32_t out = 0;
        ffxQueryDescUpscaleGetJitterPhaseCount q = {};
        q.header.type    = FFX_API_QUERY_DESC_TYPE_UPSCALE_GET_JITTER_PHASE_COUNT;
        q.renderWidth    = (uint32_t)it->second.renderWidth;
        q.displayWidth   = (uint32_t)it->second.displaySizeX;
        q.pOutPhaseCount = &out;
        m_impl->ffxQueryFn(&it->second.ffxCtx, &q.header);
        return out > 0 ? out : 1;
    }
    return 1;
}

int FSR4Backend::getJitterOffset(int id, float* outX, float* outY, int index, int phaseCount) const {
    auto it = m_impl->contexts.find(id);
    // CRITICAL: the pd-upscaler contract's GetJitterOffset export carries NO id
    // (signature int(__stdcall*)(float*,float*,int,int)); our wrapper hardcodes
    // id=0. But REFramework creates the FSR4 context with evaluate_id = 1, so
    // find(0) FAILS and we'd return -1 without ever writing outX/outY -> jitter
    // stays zero everywhere (matches "Jitter ON but jitter=(0,0)"). Fall back to
    // the actual live context so the FFX jitter query runs and writes real offsets.
    if (it == m_impl->contexts.end() && !m_impl->contexts.empty())
        it = m_impl->contexts.begin();
    if (it != m_impl->contexts.end() && it->second.ffxCtx && m_impl->ffxQueryFn) {
        ffxQueryDescUpscaleGetJitterOffset q = {};
        q.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GET_JITTER_OFFSET;
        q.index       = index;
        q.phaseCount  = phaseCount;
        q.pOutX       = outX;
        q.pOutY       = outY;
        ffxReturnCode_t rc = m_impl->ffxQueryFn(&it->second.ffxCtx, &q.header);
        if (rc != 0) {
            static long long s_last = 0;
            long long now = (long long)GetTickCount64();
            if (now - s_last > 2000) {
                s_last = now;
                Logging::error("FSR4Backend: ffxQuery jitter offset FAILED rc=%d (outX=%p outY=%p)", rc, (void*)outX, (void*)outY);
            }
            return -1;
        }
        if (outX && outY) logJitterOnce(*outX, *outY, index, phaseCount);
        return 0;
    }
    return -1;
}

// One-time diagnostic: prove the FFX jitter query actually returns non-zero
// sub-pixel offsets (and that the id-0 fallback resolved a real context).
namespace { bool s_jitterLogged = false; }
void FSR4Backend::logJitterOnce(float x, float y, int index, int phaseCount) const {
    if (s_jitterLogged) return;
    s_jitterLogged = true;
    Logging::info("FSR4Backend: JITTER_QUERY idx=%d phase=%d -> offset=(%.5f,%.5f) %s",
                  index, phaseCount, x, y,
                  (x != 0.0f || y != 0.0f) ? "(NON-ZERO: jitter active)" : "(ZERO: FSR4 jitter seq returned 0!)");
}

bool FSR4Backend::isMethodAvailable(int upscaleMethod) const {
    if (!m_impl->available) return false;
    // This mod replaces the game's upscaler with FSR4 (built on the FSR3
    // backend). REFramework's TemporalUpscaler probes method ids via
    // IsUpscaleMethodAvailable and only lets you pick one that reports true,
    // then drives it through the FSR3 back-end contract. So we advertise ONLY
    // FSR3 (id 1) as selectable — hiding DLSS/XeSS/FSR4 from the dropdown keeps
    // the menu honest about what this mod actually provides. FSR4 itself is
    // what runs under the hood, so exposing a separate "FSR4" entry is
    // redundant and just confuses the menu.
    return upscaleMethod == 1; // AMD FSR 3 is the only selectable back-end
}

const char* FSR4Backend::getMethodName(int upscaleMethod) const {
    switch (upscaleMethod) {
        case 0: return "NVIDIA DLSS";
        case 1: return "AMD FSR 3";
        case 2: return "Intel XeSS";
        case 3: return "AMD FSR 4";
        default: return "";
    }
}
