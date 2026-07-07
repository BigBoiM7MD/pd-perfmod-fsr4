#include "FSR4Backend.h"
#include "Logging.h"
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
    if (!m_impl->hLoaderDll) return false;

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
// Verification watermark
// Draws a small solid-color "FSR4" badge into the top-left of the output
// texture via CopyTextureRegion (no shader, cannot disturb the FSR pipeline).
// Its presence on screen proves the output texture is the one REFramework
// presents. Disabled by compiling with -DFSR4_NO_WATERMARK.
// -----------------------------------------------------------------------
#ifndef FSR4_NO_WATERMARK
namespace {
    static const uint8_t g_glyphs[4][5] = {
        {0x7F, 0x40, 0x7C, 0x40, 0x40}, // F
        {0x7C, 0x40, 0x7C, 0x04, 0x7C}, // S
        {0x7E, 0x42, 0x7E, 0x4C, 0x42}, // R
        {0x44, 0x4C, 0x7F, 0x04, 0x04}, // 4
    };
    static const int g_gx[4] = {4, 28, 52, 76};

    // A full-screen solid-color texture used by the PD_FSR4_DIAG_SOLID test.
    // Same format as the output texture so CopyTextureRegion succeeds.
    // Uses an UPLOAD heap + direct Map (no extra command queue) — the same
    // proven pattern as CreateWatermarkTexture. The GPU reads it directly as a
    // copy source, so no GPU submit is needed here.
    // Create a GPU-local (DEFAULT heap) texture of `targetFmt` and upload pixel
    // data via WriteToSubresource. We deliberately avoid the UPLOAD heap: an
    // UPLOAD-heap texture of R10G10B10A2_UNORM (the FSR4 output format, fmt 24)
    // makes CreateCommittedResource fail with E_INVALIDARG (hr=80070057) on many
    // drivers — that was the "watermark never appears" bug. WriteToSubresource
    // handles the CPU->GPU staging internally and has no such format/heap
    // restriction. The texture is left in COPY_SOURCE state so the per-frame
    // CopyTextureRegion into the output (which lives in a DEFAULT heap too)
    // is valid.
    ID3D12Resource* CreateTexFromPixels(ID3D12Device* dev, int w, int h,
                                        DXGI_FORMAT targetFmt,
                                        const std::vector<uint32_t>& px) {
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = (UINT64)w; desc.Height = (UINT)h; desc.DepthOrArraySize = 1;
        desc.MipLevels = 1; desc.Format = targetFmt;
        desc.SampleDesc.Count = 1; desc.SampleDesc.Quality = 0;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;

        ID3D12Resource* tex = nullptr;
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        HRESULT hr = dev->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, IID_PPV_ARGS(&tex));
        if (FAILED(hr)) {
            Logging::error("FSR4Backend: CreateTexFromPixels CreateCommittedResource failed hr=%08x", hr);
            return nullptr;
        }

        // WriteToSubresource requires the source row pitch to match the
        // texture's footprint pitch EXACTLY. D3D12 always aligns a texture row
        // pitch up to 256 bytes, so for a 200px-wide R10G10B10A2 texture the
        // footprint is 1024 bytes even though our pixels are only 800 bytes/row.
        // Passing a tight 800-byte row pitch makes WriteToSubresource return
        // E_INVALIDARG (hr=80070057). So we lay the pixels out with the aligned
        // pitch (256-aligned) in a staging buffer before uploading.
        const UINT bytesPerPixel = 4; // R10G10B10A2_UNORM = 4 bytes
        const UINT tightRow = (UINT)w * bytesPerPixel;
        const UINT alignedRow =
            (tightRow + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
            ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1); // 256-byte align
        std::vector<uint8_t> staged((size_t)alignedRow * h);
        for (int y = 0; y < h; ++y) {
            memcpy(staged.data() + (size_t)y * alignedRow,
                   px.data() + (size_t)y * w, (size_t)tightRow);
        }
        D3D12_SUBRESOURCE_DATA sub = {};
        sub.pData = staged.data();
        sub.RowPitch = (LONG_PTR)alignedRow;
        sub.SlicePitch = (LONG_PTR)((size_t)alignedRow * h);
        hr = tex->WriteToSubresource(0, nullptr, sub.pData, (UINT)sub.RowPitch, (UINT)sub.SlicePitch);
        if (FAILED(hr)) {
            Logging::error("FSR4Backend: CreateTexFromPixels WriteToSubresource failed hr=%08x", hr);
            tex->Release();
            return nullptr;
        }
        return tex;
    }

    ID3D12Resource* CreateSolidTexture(ID3D12Device* dev, int w, int h,
                                       DXGI_FORMAT targetFmt, uint32_t color) {
        std::vector<uint32_t> px((size_t)w * h, color);
        return CreateTexFromPixels(dev, w, h, targetFmt, px);
    }

    ID3D12Resource* CreateWatermarkTexture(ID3D12Device* dev, int& outW, int& outH,
                                            DXGI_FORMAT targetFmt) {
        // IMPORTANT: the destination is the output texture (R10G10B10A2_UNORM,
        // fmt 24). CopyTextureRegion REQUIRES the source format to match the
        // destination format exactly, otherwise it returns E_INVALIDARG and the
        // badge silently never appears. So we always create the badge in the
        // same format as the output texture.
        const int w = 200, h = 40;
        const uint32_t green = 0xC00FFC00; // ABGR opaque GREEN (R10G10B10A2)
        const uint32_t black = 0xFF000000;

        std::vector<uint32_t> px((size_t)w * h, black);
        for (int g = 0; g < 4; ++g) {
            for (int row = 0; row < 5; ++row) {
                uint8_t bits = g_glyphs[g][row];
                for (int col = 0; col < 7; ++col) {
                    if (bits & (0x40 >> col)) {
                        int x = g_gx[g] + col;
                        int y = 8 + row;
                        if (x >= 0 && x < w && y >= 0 && y < h)
                            px[(size_t)y * w + x] = green;
                    }
                }
            }
        }

        ID3D12Resource* tex = CreateTexFromPixels(dev, w, h, targetFmt, px);
        if (tex) { outW = w; outH = h; }
        return tex;
    }
}
#endif

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
        Logging::error("FSR4Backend: ffxCreateContext failed code=%d", ret);
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
    dd.sharpness               = sharpness;
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
                Logging::info("FSR4Backend: ffxDispatch SUCCEEDED id=%d (out=%p fmt=%d %ux%u) reset=%d jitter=(%.4f,%.4f) mvScale=(%.4f,%.4f) sharp=%.3f",
                              id, effectiveDst, ctx.format, ctx.displaySizeX, ctx.displaySizeY,
                              (int)reset, jitterOffsetX, jitterOffsetY,
                              ctx.motionScaleX, ctx.motionScaleY, sharpness);
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
        // Re-check the sentinel EVERY frame so toggling the file works without
        // a restart, and resolve the DLL dir robustly (fall back to the process
        // module if our cached hInstance is null).
        bool on = false;
        wchar_t dllPath[MAX_PATH] = {};
        HMODULE hmod = (HMODULE)m_impl->hInstance;
        if (!hmod) hmod = GetModuleHandleW(nullptr);
        if (hmod) GetModuleFileNameW(hmod, dllPath, MAX_PATH);
        std::wstring d = dllPath; auto pos = d.find_last_of(L"\\/");
        std::wstring dir = (pos == std::wstring::npos) ? L"." : d.substr(0, pos);
        std::wstring sentinel = dir + L"\\PD_FSR4_DIAG_SOLID";
        on = (GetFileAttributesW(sentinel.c_str()) != INVALID_FILE_ATTRIBUTES);
        static int s_lastMode = -1;
        if (on != (s_lastMode == 1)) {
            s_lastMode = on ? 1 : 0;
            Logging::info("FSR4Backend: DIAG_SOLID mode %s (sentinel=%ls)",
                          on ? "ON — painting output GREEN" : "OFF — normal FSR4",
                          sentinel.c_str());
        }
        if (on) {
            if (!ctx.solidTex) {
                ctx.solidTex = CreateSolidTexture(m_impl->device, ctx.displaySizeX,
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
                m_impl->commandQueue->Signal(m_impl->fence, ++m_impl->fenceValue);
                if (m_impl->fence->GetCompletedValue() < m_impl->fenceValue) {
                    m_impl->fence->SetEventOnCompletion(m_impl->fenceValue, m_impl->fenceEvent);
                    WaitForSingleObject(m_impl->fenceEvent, INFINITE);
                }
            }
            return; // skip FSR4 entirely in solid mode
        }
    }
#endif

        // --- Verification watermark (if enabled) -----------------------------
        // Draw a small "FSR4" badge into the output texture's top-left corner.
        // The output is currently in UAV state from the dispatch; transition it
        // to COPY_DEST, copy the badge, then back to UAV so REFramework's copier
        // sees it in the expected state.
#ifndef FSR4_NO_WATERMARK
        if (m_impl->device && effectiveDst) {
            if (!ctx.watermarkTex) {
                ctx.watermarkTex = CreateWatermarkTexture(m_impl->device,
                                                          ctx.watermarkW, ctx.watermarkH,
                                                          (DXGI_FORMAT)ctx.format);
                if (ctx.watermarkTex) {
                    Logging::info("FSR4Backend: watermark texture created %dx%d (DEFAULT heap, COPY_SOURCE) — will be copied into output each frame",
                                  ctx.watermarkW, ctx.watermarkH);
                } else {
                    Logging::error("FSR4Backend: watermark texture creation FAILED (see CreateTexFromPixels error above)");
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
#endif

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
        m_impl->commandQueue->Signal(m_impl->fence, ++m_impl->fenceValue);

        if (m_impl->fence->GetCompletedValue() < m_impl->fenceValue) {
            m_impl->fence->SetEventOnCompletion(m_impl->fenceValue, m_impl->fenceEvent);
            WaitForSingleObject(m_impl->fenceEvent, INFINITE);
        }
    }
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
    // This backend only implements FSR4. REFramework's TemporalUpscaler probes
    // method ids via IsUpscaleMethodAvailable and only lets you pick one that
    // reports true. The pd-upscaler branch adds FSR4=3; to be safe we also
    // report the standard ids available so the UI has a selectable entry (FSR4
    // ignores the method id at context-creation time).
    return upscaleMethod == 3 || upscaleMethod == 0 || upscaleMethod == 1 || upscaleMethod == 2;
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
