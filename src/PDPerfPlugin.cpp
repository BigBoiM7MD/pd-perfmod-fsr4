#include "../include/PDPerfPlugin.h"
#include "FSR4Backend.h"
#include "OriginalPlugin.h"
#include "Logging.h"

#include <cstring>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <unordered_map>
#include <map>

static FSR4Backend* g_fsr4 = nullptr;
static OriginalPlugin* g_original = nullptr;
static bool g_useOriginal = false;
static bool g_initialized = false;
static void (*g_logDelegate)(char*, int) = nullptr;

static ID3D12Device* g_d3d12Device = nullptr;
static ID3D12CommandQueue* g_d3d12Queue = nullptr;
static std::unordered_map<int, ID3D12Resource*> g_fallbackTextures;

static void logCallback(char* message, int iSize) {
    if (g_logDelegate) g_logDelegate(message, iSize);
}

// Debug: Log when InitUpscaler is called
static bool s_initUpscalerCalled = false;

static void initialize() {
    if (g_initialized) return;
    g_initialized = true;

    Logging::init();
    Logging::info("pd-perfmod-fsr4 initializing...");

    g_fsr4 = new FSR4Backend();
    g_original = new OriginalPlugin();
}

static ID3D12Resource* createFallbackTexture(int displaySizeX, int displaySizeY, int format) {
    if (!g_d3d12Device) return nullptr;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Alignment = 0;
    desc.Width = (UINT64)displaySizeX;
    desc.Height = (UINT)displaySizeY;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = (DXGI_FORMAT)format;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    ID3D12Resource* resource = nullptr;
    if (SUCCEEDED(g_d3d12Device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
        &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource)))) {
        return resource;
    }
    return nullptr;
}

bool __stdcall SetupDirectX(void* item, int graphicsAPI) {
    initialize();
    Logging::info("SetupDirectX(graphicsAPI=%d)", graphicsAPI);

    if (graphicsAPI == 1) {
        g_d3d12Queue = (ID3D12CommandQueue*)item;
        if (g_d3d12Queue) {
            if (FAILED(g_d3d12Queue->GetDevice(IID_PPV_ARGS(&g_d3d12Device)))) {
                g_d3d12Device = nullptr;
            }
        }
    }

    if (g_fsr4->setup(item, graphicsAPI)) {
        Logging::info("FSR 4 backend ready");
        g_useOriginal = false;
        return true;
    }

    if (g_original->load()) {
        Logging::info("Falling back to original PDPerfPlugin");
        g_useOriginal = true;
        return g_original->setupDirectX(item, graphicsAPI);
    }

    Logging::error("No upscaler backend available");
    return false;
}

void* __stdcall SimpleInit(int id, int upscaleMethod, int qualityLevel,
                            int displaySizeX, int displaySizeY,
                            bool isContentHDR, bool depthInverted, bool YAxisInverted,
                            bool motionVetorsJittered, bool enableSharpening,
                            bool enableAutoExposure, int format) {
    initialize();

    if (!g_useOriginal && g_fsr4 && g_fsr4->isAvailable()) {
        return g_fsr4->createContext(id, upscaleMethod, qualityLevel, displaySizeX, displaySizeY,
                                      isContentHDR, depthInverted, YAxisInverted,
                                      motionVetorsJittered, enableSharpening,
                                      enableAutoExposure, format);
    }

    if (g_useOriginal && g_original && g_original->isLoaded()) {
        return g_original->simpleInit(id, upscaleMethod, qualityLevel, displaySizeX, displaySizeY,
                                       isContentHDR, depthInverted, YAxisInverted,
                                       motionVetorsJittered, enableSharpening,
                                       enableAutoExposure, format);
    }

    ID3D12Resource* fallback = createFallbackTexture(displaySizeX, displaySizeY, format);
    if (fallback) {
        g_fallbackTextures[id] = fallback;
        Logging::info("Created fallback texture id=%d %dx%d", id, displaySizeX, displaySizeY);
    }
    return fallback;
}

void* __stdcall InitUpscaler(InitParams* params) {
    if (!params) { 
        Logging::error("InitUpscaler: NULL params");
        return nullptr; 
    }
    
    initialize();
    Logging::info("InitUpscaler called: id=%d method=%d qual=%d %dx%d fmt=%d",
                  params->id, params->upscaleMethod, params->qualityLevel,
                  params->displaySizeX, params->displaySizeY, params->format);

    if (!g_useOriginal && g_fsr4 && g_fsr4->isAvailable()) {
        Logging::info("InitUpscaler: Using FSR4 backend");
        return g_fsr4->createContext(params->id, params->upscaleMethod, params->qualityLevel,
                                      params->displaySizeX, params->displaySizeY,
                                      params->isContentHDR, params->depthInverted, 
                                      params->YAxisInverted, params->motionVetorsJittered,
                                      params->enableSharpening, params->enableAutoExposure,
                                      params->format);
    }

    if (g_useOriginal && g_original && g_original->isLoaded()) {
        Logging::info("InitUpscaler: Using original PDPerfPlugin");
        return g_original->simpleInit(params->id, params->upscaleMethod, params->qualityLevel,
                                       params->displaySizeX, params->displaySizeY,
                                       params->isContentHDR, params->depthInverted, 
                                       params->YAxisInverted, params->motionVetorsJittered,
                                       params->enableSharpening, params->enableAutoExposure,
                                       params->format);
    }

    Logging::error("InitUpscaler: No backend available!");
    ID3D12Resource* fallback = createFallbackTexture(params->displaySizeX, params->displaySizeY, params->format);
    if (fallback) {
        g_fallbackTextures[params->id] = fallback;
        Logging::info("Created fallback texture id=%d %dx%d", params->id, params->displaySizeX, params->displaySizeY);
    }
    return fallback;
}

void __stdcall SimpleEvaluate(int id, void* color, void* motionVector, void* depth, void* mask,
                               void* destination, int renderSizeX, int renderSizeY, float sharpness,
                               float jitterOffsetX, float jitterOffsetY, int motionScaleX, int motionScaleY,
                               bool reset, float nearPlane, float farPlane, float verticalFOV, bool execute) {
    initialize();
    if (!g_useOriginal && g_fsr4 && g_fsr4->isReady()) {
        g_fsr4->evaluate(id, color, motionVector, depth, destination, renderSizeX, renderSizeY,
                          sharpness, jitterOffsetX, jitterOffsetY, (float)motionScaleX, (float)motionScaleY,
                          reset, nearPlane, farPlane, verticalFOV, execute, nullptr);
        return;
    }

    if (g_useOriginal && g_original && g_original->isLoaded()) {
        g_original->simpleEvaluate(id, color, motionVector, depth, mask, destination,
                                    renderSizeX, renderSizeY, sharpness,
                                    jitterOffsetX, jitterOffsetY, motionScaleX, motionScaleY,
                                    reset, nearPlane, farPlane, verticalFOV, execute);
    }
}

void __stdcall EvaluateUpscaler(UpscaleParams* params) {
    if (!params) { return; }
    initialize();

    static bool s_loggedOnce = false;
    if (!s_loggedOnce) {
        Logging::info("EvaluateUpscaler: useOriginal=%d fsr4=%p ready=%d",
                      (int)g_useOriginal, (void*)g_fsr4, g_fsr4 ? (int)g_fsr4->isReady() : -1);
        s_loggedOnce = true;
    }

    if (!g_useOriginal && g_fsr4 && g_fsr4->isReady()) {
        g_fsr4->evaluate(params);
        return;
    }

    if (g_useOriginal && g_original && g_original->isLoaded()) {
        g_original->evaluateUpscaler(params);
    }
}

void __stdcall SetMotionScaleX(int id, float motionScaleX) {
    initialize();
    if (!g_useOriginal && g_fsr4) { g_fsr4->setMotionScaleX(id, motionScaleX); return; }
    if (g_useOriginal && g_original) g_original->setMotionScaleX(id, motionScaleX);
}

void __stdcall SetMotionScaleY(int id, float motionScaleX) {
    initialize();
    if (!g_useOriginal && g_fsr4) { g_fsr4->setMotionScaleY(id, motionScaleX); return; }
    if (g_useOriginal && g_original) g_original->setMotionScaleY(id, motionScaleX);
}

int __stdcall GetRenderWidth(int id) {
    initialize();
    if (!g_useOriginal && g_fsr4) return g_fsr4->getRenderWidth(id);
    if (g_useOriginal && g_original) return g_original->getRenderWidth(id);
    auto it = g_fallbackTextures.find(id);
    if (it != g_fallbackTextures.end()) {
        D3D12_RESOURCE_DESC desc = it->second->GetDesc();
        return (int)(desc.Width * 0.67f);
    }
    return 0;
}

int __stdcall GetRenderHeight(int id) {
    initialize();
    if (!g_useOriginal && g_fsr4) return g_fsr4->getRenderHeight(id);
    if (g_useOriginal && g_original) return g_original->getRenderHeight(id);
    auto it = g_fallbackTextures.find(id);
    if (it != g_fallbackTextures.end()) {
        D3D12_RESOURCE_DESC desc = it->second->GetDesc();
        return (int)(desc.Height * 0.67f);
    }
    return 0;
}

float __stdcall GetOptimalSharpness(int id) {
    initialize();
    if (!g_useOriginal && g_fsr4) return g_fsr4->getOptimalSharpness(id);
    if (g_useOriginal && g_original) return g_original->getOptimalSharpness(id);
    return 0.5f;
}

float __stdcall GetOptimalMipmapBias(int id) {
    initialize();
    if (!g_useOriginal && g_fsr4) return g_fsr4->getOptimalMipmapBias(id);
    if (g_useOriginal && g_original) return g_original->getOptimalMipmapBias(id);
    return 0.0f;
}

void __stdcall SetDebug(bool debug) {
    initialize();
    if (g_original) g_original->setDebug(debug);
}

void __stdcall ReleaseUpscaleFeature(int id) {
    initialize();
    if (!g_useOriginal && g_fsr4) { g_fsr4->release(id); return; }
    if (g_useOriginal && g_original) { g_original->releaseUpscaleFeature(id); return; }
    auto it = g_fallbackTextures.find(id);
    if (it != g_fallbackTextures.end()) {
        it->second->Release();
        g_fallbackTextures.erase(it);
        Logging::info("Released fallback texture id=%d", id);
    }
}

int __stdcall GetJitterPhaseCount(int id) {
    initialize();
    if (!g_useOriginal && g_fsr4) return g_fsr4->getJitterPhaseCount(id);
    if (g_useOriginal && g_original) return g_original->getJitterPhaseCount(id);
    return 1;
}

int __stdcall GetJitterOffset(float* outX, float* outY, int index, int phaseCount) {
    initialize();
    if (!g_useOriginal && g_fsr4) return g_fsr4->getJitterOffset(0, outX, outY, index, phaseCount);
    if (g_useOriginal && g_original) return g_original->getJitterOffset(outX, outY, index, phaseCount);
    return -1;
}

void __stdcall InitLogDelegate(void (*Log)(char* message, int iSize)) {
    initialize();
    g_logDelegate = Log;
    if (g_original && g_original->isLoaded())
        g_original->initLogDelegate(Log);
}

bool __stdcall IsUpscaleMethodAvailable(int upscaleMethod) {
    initialize();
    if (!g_useOriginal && g_fsr4) {
        // REFramework's TemporalUpscaler enumerates the dropdown once, during
        // on_initialize -- which runs BEFORE the game calls SetupDirectX, so
        // g_fsr4->isAvailable() is frequently still false at that point. Both
        // this fallback and FSR4Backend::isMethodAvailable must therefore
        // report ONLY FSR3 (id 1); anything else just confuses the menu of an
        // FSR4 mod. FSR4 runs under the FSR3 back-end contract.
        if (g_fsr4->isAvailable())
            return g_fsr4->isMethodAvailable(upscaleMethod);
        return upscaleMethod == 1; // AMD FSR 3 only, even before backend is up
    }
    if (g_useOriginal && g_original) return g_original->isUpscaleMethodAvailable(upscaleMethod);
    return false;
}

char* __stdcall GetUpscaleMethodName(int upscaleMethod) {
    static thread_local char nameBuf[64];
    initialize();

    if (!g_useOriginal && g_fsr4) {
        if (g_fsr4->isAvailable()) {
            const char* name = g_fsr4->getMethodName(upscaleMethod);
            strncpy_s(nameBuf, name, sizeof(nameBuf) - 1);
            return nameBuf;
        }
        if (upscaleMethod >= 0 && upscaleMethod <= 2) {
            const char* names[] = {"NVIDIA DLSS", "AMD FSR 3", "Intel XeSS"};
            strcpy_s(nameBuf, names[upscaleMethod]);
            return nameBuf;
        }
        nameBuf[0] = '\0';
        return nameBuf;
    }
    if (g_useOriginal && g_original) return g_original->getUpscaleMethodName(upscaleMethod);

    nameBuf[0] = '\0';
    return nameBuf;
}
