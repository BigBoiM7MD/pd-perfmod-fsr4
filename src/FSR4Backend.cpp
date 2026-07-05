#include "FSR4Backend.h"
#include "Logging.h"
#include "../include/PDPerfPlugin.h"

#include <d3d12.h>
#include <dxgi1_4.h>
#include <unordered_map>
#include <cstring>

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
using ffxDispatchDescHeader     = ffxApiHeader;
using ffxQueryDescHeader        = ffxApiHeader;

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
static constexpr uint64_t FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12 = MAKE_BACKEND_SUB_ID(FFX_API_BACKEND_ID_DX12, 2); // 0x00000002

// Upscale sub-IDs
static constexpr uint64_t FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE              = MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_UPSCALE, 0x00); // 0x00010000
static constexpr uint64_t FFX_API_DISPATCH_DESC_TYPE_UPSCALE                    = MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_UPSCALE, 0x01); // 0x00010001
static constexpr uint64_t FFX_API_QUERY_DESC_TYPE_UPSCALE_GET_JITTER_PHASE_COUNT = MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_UPSCALE, 0x04); // 0x00010004
static constexpr uint64_t FFX_API_QUERY_DESC_TYPE_UPSCALE_GET_JITTER_OFFSET      = MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_UPSCALE, 0x05); // 0x00010005
static constexpr uint64_t FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION       = MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_UPSCALE, 0x0b); // 0x0001000b

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
// Descriptor structs  (exact layout from SDK headers – no manual padding)
// -----------------------------------------------------------------------
struct ffxCreateBackendDX12Desc {
    ffxCreateContextDescHeader header;
    ID3D12Device*             device;
};

struct FfxApiDimensions2D {
    uint32_t width;
    uint32_t height;
};

struct ffxCreateContextDescUpscale {
    ffxCreateContextDescHeader header;
    uint32_t                   flags;
    FfxApiDimensions2D         maxRenderSize;
    FfxApiDimensions2D         maxUpscaleSize;
    ffxApiMessage              fpMessage;
};

struct ffxCreateContextDescUpscaleVersion {
    ffxCreateContextDescHeader header;
    uint32_t                   version;
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
    void*                       resource;
    FfxApiResourceDescription   description;
    uint32_t                    state;
};

struct FfxApiFloatCoords2D {
    float x;
    float y;
};

struct ffxDispatchDescUpscale {
    ffxDispatchDescHeader header;
    void*                 commandList;
    FfxApiResource        color;
    FfxApiResource        depth;
    FfxApiResource        motionVectors;
    FfxApiResource        exposure;
    FfxApiResource        reactive;
    FfxApiResource        transparencyAndComposition;
    FfxApiResource        output;
    FfxApiFloatCoords2D   jitterOffset;
    FfxApiFloatCoords2D   motionVectorScale;
    FfxApiDimensions2D    renderSize;
    FfxApiDimensions2D    upscaleSize;
    bool                  enableSharpening;
    float                 sharpness;
    float                 frameTimeDelta;
    float                 preExposure;
    bool                  reset;
    float                 cameraNear;
    float                 cameraFar;
    float                 cameraFovAngleVertical;
    float                 viewSpaceToMetersFactor;
    uint32_t              flags;
};

struct ffxQueryDescUpscaleGetJitterPhaseCount {
    ffxQueryDescHeader header;
    uint32_t           renderWidth;
    uint32_t           displayWidth;
    int32_t*           pOutPhaseCount;
};

struct ffxQueryDescUpscaleGetJitterOffset {
    ffxQueryDescHeader header;
    int32_t            index;
    int32_t            phaseCount;
    float*             pOutX;
    float*             pOutY;
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
using PFN_ffxDispatch       = ffxReturnCode_t (__cdecl*)(ffxContext, const ffxDispatchDescHeader*);
using PFN_ffxQuery          = ffxReturnCode_t (__cdecl*)(ffxContext, ffxQueryDescHeader*);

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
    ID3D12Device*     device         = nullptr;

    int   renderWidth  = 0;
    int   renderHeight = 0;
    float motionScaleX = 1.0f;
    float motionScaleY = 1.0f;
};

struct FSR4Backend::Impl {
    HMODULE hLoaderDll = nullptr;
    PFN_ffxCreateContext  ffxCreateContextFn  = nullptr;
    PFN_ffxDestroyContext ffxDestroyContextFn = nullptr;
    PFN_ffxDispatch       ffxDispatchFn       = nullptr;
    PFN_ffxQuery          ffxQueryFn          = nullptr;

    ID3D12Device* device       = nullptr;
    int           graphicsAPI  = 0;
    bool          available    = false;

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
    }
    m_impl->contexts.clear();
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

    if (!m_impl->ffxCreateContextFn || !m_impl->ffxDestroyContextFn || !m_impl->ffxDispatchFn) {
        Logging::warn("FSR4Backend: Missing required ffxApi entry points");
        FreeLibrary(m_impl->hLoaderDll);
        m_impl->hLoaderDll = nullptr;
        return false;
    }

    Logging::info("FSR4Backend: ffxApi loaded successfully");
    m_impl->available = true;
    return true;
}

bool FSR4Backend::isAvailable() const { return m_impl->available; }
bool FSR4Backend::isReady() const { return m_impl->available && !m_impl->contexts.empty(); }

static FfxApiResource makeResource(ID3D12Resource* res, uint32_t state, uint32_t usage) {
    FfxApiResource r = {};
    r.resource = res;
    if (res) {
        D3D12_RESOURCE_DESC d = res->GetDesc();
        r.description.type    = 2; // TEXTURE2D
        r.description.format  = (uint32_t)d.Format;
        r.description.width   = (uint32_t)d.Width;
        r.description.height  = d.Height;
        r.description.depth   = d.DepthOrArraySize;
        r.description.mipCount = d.MipLevels;
        r.description.usage   = usage;
    }
    r.state = state;
    return r;
}

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

    // pNext chain: upscaleDesc → versionDesc → backendDesc
    ffxCreateBackendDX12Desc backendDesc = {};
    backendDesc.header.type  = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backendDesc.header.pNext = nullptr;
    backendDesc.device       = m_impl->device;

    ffxCreateContextDescUpscaleVersion versionDesc = {};
    versionDesc.header.type  = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    versionDesc.header.pNext = &backendDesc.header;
    versionDesc.version      = FFX_UPSCALER_VERSION;

    ffxCreateContextDescUpscale upscaleDesc = {};
    upscaleDesc.header.type             = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    upscaleDesc.header.pNext            = &versionDesc.header;
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
    texDesc.Flags              = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    ID3D12Resource* outputTexture = nullptr;
    HRESULT hr = m_impl->device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
        &texDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&outputTexture));
    if (FAILED(hr)) {
        Logging::error("FSR4Backend: Failed to create output texture hr=%08x", hr);
        m_impl->ffxDestroyContextFn(&ffxCtx, nullptr);
        return nullptr;
    }

    ctx.outputTexture = outputTexture;
    m_impl->contexts[id] = ctx;
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
    auto it = m_impl->contexts.find(id);
    if (it == m_impl->contexts.end() || !it->second.ffxCtx || !m_impl->ffxDispatchFn) return;
    if (!execute) return;

    auto& ctx = it->second;

    ffxDispatchDescUpscale dd = {};
    dd.header.type  = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    dd.header.pNext = nullptr;
    dd.commandList  = cmdList;

    dd.color          = makeResource((ID3D12Resource*)color, 1, 0);
    dd.depth          = makeResource((ID3D12Resource*)depth, 1, 0);
    dd.motionVectors  = makeResource((ID3D12Resource*)motionVector, 1, 0);
    dd.exposure       = makeResource(nullptr, 1, 0);
    dd.reactive       = makeResource(nullptr, 1, 0);
    dd.transparencyAndComposition = makeResource(nullptr, 1, 0);
    dd.output         = makeResource((ID3D12Resource*)destination, 1, 2);

    dd.jitterOffset.x          = jitterOffsetX;
    dd.jitterOffset.y          = jitterOffsetY;
    dd.motionVectorScale.x     = motionScaleX;
    dd.motionVectorScale.y     = motionScaleY;
    dd.renderSize.width        = (uint32_t)renderSizeX;
    dd.renderSize.height       = (uint32_t)renderSizeY;
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

    ffxReturnCode_t ret = m_impl->ffxDispatchFn(ctx.ffxCtx, &dd.header);
    if (ret != 0) {
        Logging::error("FSR4Backend: ffxDispatch failed code=%d", ret);
    }
}

void FSR4Backend::release(int id) {
    auto it = m_impl->contexts.find(id);
    if (it != m_impl->contexts.end()) {
        if (it->second.ffxCtx && m_impl->ffxDestroyContextFn)
            m_impl->ffxDestroyContextFn(&it->second.ffxCtx, nullptr);
        if (it->second.outputTexture) it->second.outputTexture->Release();
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
    if (it != m_impl->contexts.end() && it->second.ffxCtx && m_impl->ffxQueryFn) {
        int32_t out = 0;
        ffxQueryDescUpscaleGetJitterPhaseCount q = {};
        q.header.type    = FFX_API_QUERY_DESC_TYPE_UPSCALE_GET_JITTER_PHASE_COUNT;
        q.renderWidth    = (uint32_t)it->second.renderWidth;
        q.displayWidth   = (uint32_t)it->second.displaySizeX;
        q.pOutPhaseCount = &out;
        m_impl->ffxQueryFn(it->second.ffxCtx, &q.header);
        return out > 0 ? out : 1;
    }
    return 1;
}

int FSR4Backend::getJitterOffset(int id, float* outX, float* outY, int index, int phaseCount) const {
    auto it = m_impl->contexts.find(id);
    if (it != m_impl->contexts.end() && it->second.ffxCtx && m_impl->ffxQueryFn) {
        ffxQueryDescUpscaleGetJitterOffset q = {};
        q.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GET_JITTER_OFFSET;
        q.index       = index;
        q.phaseCount  = phaseCount;
        q.pOutX       = outX;
        q.pOutY       = outY;
        m_impl->ffxQueryFn(it->second.ffxCtx, &q.header);
        return 0;
    }
    return -1;
}

bool FSR4Backend::isMethodAvailable(int upscaleMethod) const {
    if (!m_impl->available) return false;
    return upscaleMethod == 3;
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
