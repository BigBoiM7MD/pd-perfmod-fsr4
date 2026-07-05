#include "FSR4Backend.h"
#include "Logging.h"
#include "../include/PDPerfPlugin.h"

#include <d3d12.h>
#include <dxgi1_4.h>
#include <unordered_map>
#include <cstring>

using ffxReturnCode_t = int32_t;
using ffxContext = void*;

constexpr uint32_t FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12 = 0x10003;
constexpr uint32_t FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE = 0x30001;
constexpr uint32_t FFX_API_DISPATCH_DESC_TYPE_UPSCALE = 0x3000101;
constexpr uint32_t FFX_API_QUERY_DESC_TYPE_GET_VERSIONS = 0x00001;
constexpr uint32_t FFX_API_QUERY_DESC_TYPE_UPSCALE_GET_JITTER_PHASE_COUNT = 0x30012;
constexpr uint32_t FFX_API_QUERY_DESC_TYPE_UPSCALE_GET_JITTER_OFFSET = 0x30013;

constexpr uint32_t FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE = 0x8;
constexpr uint32_t FFX_UPSCALE_ENABLE_DEPTH_INVERTED = 0x10;
constexpr uint32_t FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTERED = 0x40;
constexpr uint32_t FFX_UPSCALE_ENABLE_SHARPENING = 0x100;
constexpr uint32_t FFX_UPSCALE_ENABLE_AUTO_EXPOSURE = 0x4;

struct ffxCreateContextDescHeader {
    uint32_t type;
    const void* pNext;
};

struct ffxCreateBackendDX12Desc {
    ffxCreateContextDescHeader header;
    ID3D12Device* device;
    ID3D12CommandQueue* commandQueue;
};

struct ffxCreateContextDescUpscale {
    ffxCreateContextDescHeader header;
    uint32_t flags;
    uint32_t maxRenderSize[2];
    uint32_t maxUpscaleSize[2];
};

struct ffxDispatchDescHeader {
    uint32_t type;
    const void* pNext;
};

struct ffxDispatchDescUpscale {
    ffxDispatchDescHeader header;
    void* commandList;
    uint64_t color;
    uint64_t depth;
    uint64_t motionVectors;
    uint64_t output;
    uint32_t renderSize[2];
    uint32_t upscaleSize[2];
    float jitterOffset[2];
    float motionVectorScale[2];
    float cameraNear;
    float cameraFar;
    float cameraFovAngleVertical;
    uint32_t reset;
    uint32_t enableSharpening;
    float sharpness;
    float frameTimeDelta;
    float preExposure;
    uint32_t motionVectorsJittered;
};

struct ffxQueryDescHeader {
    uint32_t type;
    const void* pNext;
};

struct ffxQueryDescGetJitterPhaseCount {
    ffxQueryDescHeader header;
    int32_t jitterPhaseCount;
};

struct ffxQueryDescGetJitterOffset {
    ffxQueryDescHeader header;
    float jitterOffset[2];
    int32_t jitterPhaseCount;
    float jitterPhaseCountFloat;
};

using PFN_ffxCreateContext = ffxReturnCode_t(__cdecl*)(ffxContext*, ffxCreateContextDescHeader*, const void*);
using PFN_ffxDestroyContext = ffxReturnCode_t(__cdecl*)(ffxContext);
using PFN_ffxDispatch = ffxReturnCode_t(__cdecl*)(ffxContext, const ffxDispatchDescHeader*);
using PFN_ffxQuery = ffxReturnCode_t(__cdecl*)(ffxContext, const ffxQueryDescHeader*);

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

    ffxContext ffxCtx = nullptr;
    ID3D12Resource* outputTexture = nullptr;
    ID3D12Device* device = nullptr;

    int renderWidth = 0;
    int renderHeight = 0;
    float motionScaleX = 1.0f;
    float motionScaleY = 1.0f;
};

struct FSR4Backend::Impl {
    HMODULE hLoaderDll = nullptr;
    PFN_ffxCreateContext ffxCreateContextFn = nullptr;
    PFN_ffxDestroyContext ffxDestroyContextFn = nullptr;
    PFN_ffxDispatch ffxDispatchFn = nullptr;
    PFN_ffxQuery ffxQueryFn = nullptr;

    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* commandQueue = nullptr;
    int graphicsAPI = 0;
    bool available = false;

    std::unordered_map<int, UpscaleContext> contexts;
};

FSR4Backend::FSR4Backend() { m_impl = new Impl(); }

FSR4Backend::~FSR4Backend() {
    for (auto& [id, ctx] : m_impl->contexts) {
        if (ctx.ffxCtx && m_impl->ffxDestroyContextFn)
            m_impl->ffxDestroyContextFn(ctx.ffxCtx);
        if (ctx.outputTexture) ctx.outputTexture->Release();
    }
    m_impl->contexts.clear();
    if (m_impl->hLoaderDll) {
        FreeLibrary(m_impl->hLoaderDll);
    }
    delete m_impl;
}

bool FSR4Backend::setup(void* deviceOrQueue, int graphicsAPI) {
    Logging::info("FSR4Backend::setup(graphicsAPI=%d)", graphicsAPI);
    m_impl->graphicsAPI = graphicsAPI;

    if (graphicsAPI == 0) {
        Logging::warn("FSR4Backend: D3D11 not supported, need D3D12");
        return false;
    }

    m_impl->commandQueue = (ID3D12CommandQueue*)deviceOrQueue;
    if (m_impl->commandQueue) {
        if (FAILED(m_impl->commandQueue->GetDevice(IID_PPV_ARGS(&m_impl->device)))) {
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
    if (!m_impl->hLoaderDll) {
        return false;
    }

    m_impl->ffxCreateContextFn = (PFN_ffxCreateContext)GetProcAddress(m_impl->hLoaderDll, "ffxCreateContext");
    m_impl->ffxDestroyContextFn = (PFN_ffxDestroyContext)GetProcAddress(m_impl->hLoaderDll, "ffxDestroyContext");
    m_impl->ffxDispatchFn = (PFN_ffxDispatch)GetProcAddress(m_impl->hLoaderDll, "ffxDispatch");
    m_impl->ffxQueryFn = (PFN_ffxQuery)GetProcAddress(m_impl->hLoaderDll, "ffxQuery");

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

void* FSR4Backend::createContext(int id, int upscaleMethod, int qualityLevel,
                                  int displaySizeX, int displaySizeY,
                                  bool isContentHDR, bool depthInverted, bool yAxisInverted,
                                  bool motionVectorsJittered, bool enableSharpening,
                                  bool enableAutoExposure, int format) {
    Logging::info("FSR4Backend::createContext(id=%d, method=%d, qual=%d, %dx%d)",
                  id, upscaleMethod, qualityLevel, displaySizeX, displaySizeY);

    if (!m_impl->available || !m_impl->device) {
        Logging::error("FSR4Backend: Not available");
        return nullptr;
    }

    UpscaleContext ctx;
    ctx.id = id;
    ctx.upscaleMethod = upscaleMethod;
    ctx.qualityLevel = qualityLevel;
    ctx.displaySizeX = displaySizeX;
    ctx.displaySizeY = displaySizeY;
    ctx.format = format;
    ctx.isContentHDR = isContentHDR;
    ctx.depthInverted = depthInverted;
    ctx.motionVectorsJittered = motionVectorsJittered;
    ctx.enableSharpening = enableSharpening;
    ctx.enableAutoExposure = enableAutoExposure;
    ctx.device = m_impl->device;

    float scale = 1.5f;
    switch (qualityLevel) {
        case 0: scale = 2.0f; break;
        case 1: scale = 1.7f; break;
        case 2: scale = 1.5f; break;
        case 3: scale = 3.0f; break;
    }
    ctx.renderWidth = (int)(displaySizeX / scale + 0.5f);
    ctx.renderHeight = (int)(displaySizeY / scale + 0.5f);

    ffxCreateBackendDX12Desc backendDesc = {};
    backendDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backendDesc.header.pNext = nullptr;
    backendDesc.device = m_impl->device;
    backendDesc.commandQueue = m_impl->commandQueue;

    ffxCreateContextDescUpscale upscaleDesc = {};
    upscaleDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    upscaleDesc.header.pNext = &backendDesc.header;
    upscaleDesc.maxRenderSize[0] = (uint32_t)ctx.renderWidth;
    upscaleDesc.maxRenderSize[1] = (uint32_t)ctx.renderHeight;
    upscaleDesc.maxUpscaleSize[0] = (uint32_t)displaySizeX;
    upscaleDesc.maxUpscaleSize[1] = (uint32_t)displaySizeY;
    upscaleDesc.flags = 0;

    if (isContentHDR) upscaleDesc.flags |= FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE;
    if (depthInverted) upscaleDesc.flags |= FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
    if (motionVectorsJittered) upscaleDesc.flags |= FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTERED;
    if (enableAutoExposure) upscaleDesc.flags |= FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
    if (enableSharpening) upscaleDesc.flags |= FFX_UPSCALE_ENABLE_SHARPENING;

    ffxContext ffxCtx = nullptr;
    ffxReturnCode_t ret = m_impl->ffxCreateContextFn(&ffxCtx, &upscaleDesc.header, nullptr);
    if (ret != 0 || ffxCtx == nullptr) {
        Logging::error("FSR4Backend: ffxCreateContext failed code=%d", ret);
        return nullptr;
    }

    ctx.ffxCtx = ffxCtx;
    Logging::info("FSR4Backend: Context created, render=%dx%d upscale=%dx%d",
                  ctx.renderWidth, ctx.renderHeight, displaySizeX, displaySizeY);

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = (UINT64)displaySizeX;
    texDesc.Height = (UINT)displaySizeY;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = (DXGI_FORMAT)format;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    ID3D12Resource* outputTexture = nullptr;
    if (FAILED(m_impl->device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
        &texDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&outputTexture)))) {
        Logging::error("FSR4Backend: Failed to create output texture");
        m_impl->ffxDestroyContextFn(ffxCtx);
        return nullptr;
    }

    ctx.outputTexture = outputTexture;
    m_impl->contexts[id] = ctx;
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

    Logging::info("FSR4Backend::evaluate(id=%d, render=%dx%d)", id, renderSizeX, renderSizeY);

    auto& ctx = it->second;

    ffxDispatchDescUpscale dispatchDesc = {};
    dispatchDesc.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    dispatchDesc.commandList = cmdList;
    dispatchDesc.color = (uint64_t)color;
    dispatchDesc.depth = (uint64_t)depth;
    dispatchDesc.motionVectors = (uint64_t)motionVector;
    dispatchDesc.output = (uint64_t)destination;
    dispatchDesc.renderSize[0] = (uint32_t)renderSizeX;
    dispatchDesc.renderSize[1] = (uint32_t)renderSizeY;
    dispatchDesc.upscaleSize[0] = (uint32_t)ctx.displaySizeX;
    dispatchDesc.upscaleSize[1] = (uint32_t)ctx.displaySizeY;
    dispatchDesc.jitterOffset[0] = jitterOffsetX;
    dispatchDesc.jitterOffset[1] = jitterOffsetY;
    dispatchDesc.motionVectorScale[0] = motionScaleX;
    dispatchDesc.motionVectorScale[1] = motionScaleY;
    dispatchDesc.cameraNear = nearPlane;
    dispatchDesc.cameraFar = farPlane;
    dispatchDesc.cameraFovAngleVertical = verticalFOV;
    dispatchDesc.reset = reset ? 1 : 0;
    dispatchDesc.enableSharpening = ctx.enableSharpening ? 1 : 0;
    dispatchDesc.sharpness = sharpness;
    dispatchDesc.frameTimeDelta = 16.6f;
    dispatchDesc.preExposure = 1.0f;
    dispatchDesc.motionVectorsJittered = ctx.motionVectorsJittered ? 1 : 0;

    ffxReturnCode_t ret = m_impl->ffxDispatchFn(ctx.ffxCtx, &dispatchDesc.header);
    if (ret != 0) {
        Logging::error("FSR4Backend: ffxDispatch failed code=%d", ret);
    }
}

void FSR4Backend::release(int id) {
    auto it = m_impl->contexts.find(id);
    if (it != m_impl->contexts.end()) {
        if (it->second.ffxCtx && m_impl->ffxDestroyContextFn)
            m_impl->ffxDestroyContextFn(it->second.ffxCtx);
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
        ffxQueryDescGetJitterPhaseCount query = {};
        query.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GET_JITTER_PHASE_COUNT;
        m_impl->ffxQueryFn(it->second.ffxCtx, &query.header);
        return query.jitterPhaseCount > 0 ? query.jitterPhaseCount : 1;
    }
    return 1;
}

int FSR4Backend::getJitterOffset(int id, float* outX, float* outY, int index, int phaseCount) const {
    auto it = m_impl->contexts.find(id);
    if (it != m_impl->contexts.end() && it->second.ffxCtx && m_impl->ffxQueryFn) {
        ffxQueryDescGetJitterOffset query = {};
        query.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GET_JITTER_OFFSET;
        query.jitterPhaseCount = phaseCount;
        query.jitterPhaseCountFloat = (float)phaseCount;
        m_impl->ffxQueryFn(it->second.ffxCtx, &query.header);
        if (outX) *outX = query.jitterOffset[0];
        if (outY) *outY = query.jitterOffset[1];
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
