#include "../include/PDPerfPlugin.h"
#include "../include/FSR4Backend.h"
#include "../include/Logging.h"

#include <cstring>
#include <exception>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <unordered_map>
#include <map>

// Defined in Logging.cpp (SEH-guarded). extern so it links across TUs.
extern void safeStderrWarn(const char* fmt, ...);

static FSR4Backend* g_fsr4 = nullptr;
static INIT_ONCE g_initOnce = INIT_ONCE_STATIC_INIT;
static void (*g_logDelegate)(char*, int) = nullptr;

static ID3D12Device* g_d3d12Device = nullptr;
static ID3D12CommandQueue* g_d3d12Queue = nullptr;
static std::unordered_map<int, ID3D12Resource*> g_fallbackTextures;

static void logCallback(char* message, int iSize) {
    if (g_logDelegate) g_logDelegate(message, iSize);
}

static BOOL CALLBACK initializeOnce(PINIT_ONCE, PVOID, PVOID*) {
    // EXPORT-GUARDED. REFramework calls IsUpscaleMethodAvailable /
    // GetUpscaleMethodName from TemporalUpscaler::on_initialize() *before*
    // SetupDirectX runs, so this runs during load on the game thread. If
    // Logging::init() (file INI/log open) or the constructor throws under
    // Wine/Proton, the exception would escape the __stdcall export and kill
    // REFramework ("Initialization of mods failed. Reason: exception thrown"),
    // with no FSR4Backend log line because setup() was never reached. Guard
    // it. /EHa (CMakeLists.txt) makes catch(...) trap SEH/AV too. If init
    // fails g_fsr4 stays null -> every export already null-guards on g_fsr4
    // and degrades to "no backend available" instead of crashing. Always
    // return TRUE so InitOnceExecuteOnce never wedges subsequent callers.
    try {
        Logging::init();
        Logging::info("pd-perfmod-fsr4 initializing...");
        g_fsr4 = new FSR4Backend();
    } catch (const std::exception& e) {
        // Logging may be unavailable (it threw) -> fallback to stderr so the
        // failure is never fully silent. Routed via safeStderrWarn() so a bad
        // stderr handle under a headless Proton launch can't raise SEH mid-
        // unwind (which would terminate()).
        safeStderrWarn("[PDPerfPlugin] initialize() threw: %s\n", e.what());
        g_fsr4 = nullptr;
    } catch (...) {
        safeStderrWarn("[PDPerfPlugin] initialize() threw unknown exception (SEH/AV?)\n");
        g_fsr4 = nullptr;
    }
    return TRUE;
}

static void initialize() {
    InitOnceExecuteOnce(&g_initOnce, initializeOnce, nullptr, nullptr);
}

static ID3D12Resource* createFallbackTexture(int displaySizeX, int displaySizeY, int format) {
    if (!g_d3d12Device || displaySizeX <= 0 || displaySizeY <= 0) return nullptr;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Alignment = 0;
    desc.Width = (UINT64)displaySizeX;
    desc.Height = (UINT)displaySizeY;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = (format > 0) ? (DXGI_FORMAT)format : DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    ID3D12Resource* resource = nullptr;
    HRESULT hr = g_d3d12Device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
        &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&resource));
    if ((FAILED(hr) || !resource) && desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM) {
        Logging::warn("createFallbackTexture: format=%d failed (hr=%08x), retrying R8G8B8A8_UNORM",
                      (int)desc.Format, hr);
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        resource = nullptr;
        hr = g_d3d12Device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
            &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&resource));
    }
    if (SUCCEEDED(hr) && resource) {
        return resource;
    }
    return nullptr;
}

static void eraseFallbackTexture(int id) {
    auto it = g_fallbackTextures.find(id);
    if (it != g_fallbackTextures.end()) {
        if (it->second) {
            it->second->Release();
        }
        g_fallbackTextures.erase(it);
        Logging::info("Released fallback texture id=%d", id);
    }
}

static void clearFallbackTextures() {
    for (auto& [id, res] : g_fallbackTextures) {
        if (res) {
            try {
                res->Release();
            } catch (...) {
                safeStderrWarn("[PDPerfPlugin] clearFallbackTextures: Release threw exception for id=%d\n", id);
            }
        }
    }
    g_fallbackTextures.clear();
}

static ID3D12Resource* storeFallbackTexture(int id, int displaySizeX, int displaySizeY, int format) {
    eraseFallbackTexture(id);
    ID3D12Resource* fallback = createFallbackTexture(displaySizeX, displaySizeY, format);
    if (fallback) {
        g_fallbackTextures[id] = fallback;
        Logging::info("Created fallback texture id=%d %dx%d", id, displaySizeX, displaySizeY);
    }
    return fallback;
}

bool __stdcall SetupDirectX(void* item, int graphicsAPI) {
    initialize();
    Logging::info("SetupDirectX(graphicsAPI=%d)", graphicsAPI);

    clearFallbackTextures();

    if (g_d3d12Device) {
        try {
            g_d3d12Device->Release();
        } catch (...) {
            safeStderrWarn("[PDPerfPlugin] SetupDirectX: Release threw exception\n");
        }
        g_d3d12Device = nullptr;
    }
    g_d3d12Queue = nullptr;

    if (graphicsAPI == 1 && item != nullptr) {
        try {
            g_d3d12Queue = (ID3D12CommandQueue*)item;
            if (FAILED(g_d3d12Queue->GetDevice(IID_PPV_ARGS(&g_d3d12Device))) || !g_d3d12Device) {
                g_d3d12Device = nullptr;
            }
        } catch (...) {
            safeStderrWarn("[PDPerfPlugin] SetupDirectX: GetDevice threw exception\n");
            g_d3d12Device = nullptr;
        }
    }

    if (g_fsr4 && g_fsr4->setup(item, graphicsAPI)) {
        Logging::info("FSR 4 backend ready");
        return true;
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

    if (g_fsr4 && g_fsr4->isAvailable()) {
        void* tex = g_fsr4->createContext(id, upscaleMethod, qualityLevel, displaySizeX, displaySizeY,
                                          isContentHDR, depthInverted, YAxisInverted,
                                          motionVetorsJittered, enableSharpening,
                                          enableAutoExposure, format);
        if (tex) {
            eraseFallbackTexture(id);
            return tex;
        }
        Logging::error("SimpleInit: FSR4 createContext failed for id=%d, using fallback texture", id);
    }

    return storeFallbackTexture(id, displaySizeX, displaySizeY, format);
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

    if (g_fsr4 && g_fsr4->isAvailable()) {
        Logging::info("InitUpscaler: Using FSR4 backend");
        void* tex = g_fsr4->createContext(params->id, params->upscaleMethod, params->qualityLevel,
                                          params->displaySizeX, params->displaySizeY,
                                          params->isContentHDR, params->depthInverted,
                                          params->YAxisInverted, params->motionVetorsJittered,
                                          params->enableSharpening, params->enableAutoExposure,
                                          params->format);
        if (tex) {
            eraseFallbackTexture(params->id);
            return tex;
        }
        Logging::error("InitUpscaler: FSR4 createContext failed for id=%d, using fallback texture", params->id);
        return storeFallbackTexture(params->id, params->displaySizeX, params->displaySizeY, params->format);
    }

    Logging::error("InitUpscaler: No backend available!");
    return storeFallbackTexture(params->id, params->displaySizeX, params->displaySizeY, params->format);
}

void __stdcall SimpleEvaluate(int id, void* color, void* motionVector, void* depth, void* mask,
                               void* destination, int renderSizeX, int renderSizeY, float sharpness,
                               float jitterOffsetX, float jitterOffsetY, int motionScaleX, int motionScaleY,
                               bool reset, float nearPlane, float farPlane, float verticalFOV, bool execute) {
    initialize();
    if (g_fsr4 && g_fsr4->isReady()) {
        g_fsr4->evaluate(id, color, motionVector, depth, destination, renderSizeX, renderSizeY,
                          sharpness, jitterOffsetX, jitterOffsetY, (float)motionScaleX, (float)motionScaleY,
                          reset, nearPlane, farPlane, verticalFOV, execute, nullptr);
        return;
    }
}

void __stdcall EvaluateUpscaler(UpscaleParams* params) {
    if (!params) { return; }
    initialize();

    static bool s_loggedOnce = false;
    if (!s_loggedOnce) {
        Logging::info("EvaluateUpscaler: fsr4=%p ready=%d",
                      (void*)g_fsr4, g_fsr4 ? (int)g_fsr4->isReady() : -1);
        s_loggedOnce = true;
    }

    if (g_fsr4 && g_fsr4->isReady()) {
        g_fsr4->evaluate(params);
        return;
    }
}

void __stdcall SetMotionScaleX(int id, float motionScaleX) {
    initialize();
    if (g_fsr4 && g_fsr4->isAvailable()) { g_fsr4->setMotionScaleX(id, motionScaleX); return; }
}

void __stdcall SetMotionScaleY(int id, float motionScaleX) {
    initialize();
    if (g_fsr4 && g_fsr4->isAvailable()) { g_fsr4->setMotionScaleY(id, motionScaleX); return; }
}

int __stdcall GetRenderWidth(int id) {
    initialize();
    if (g_fsr4 && g_fsr4->isAvailable()) {
        const int w = g_fsr4->getRenderWidth(id);
        if (w > 0) return w;
    }
    auto it = g_fallbackTextures.find(id);
    if (it != g_fallbackTextures.end() && it->second) {
        D3D12_RESOURCE_DESC desc = it->second->GetDesc();
        if (desc.Width > 0) {
            const int w = (int)(desc.Width * 0.67f);
            return w > 0 ? w : 1;
        }
    }
    return 0;
}

int __stdcall GetRenderHeight(int id) {
    initialize();
    if (g_fsr4 && g_fsr4->isAvailable()) {
        const int h = g_fsr4->getRenderHeight(id);
        if (h > 0) return h;
    }
    auto it = g_fallbackTextures.find(id);
    if (it != g_fallbackTextures.end() && it->second) {
        D3D12_RESOURCE_DESC desc = it->second->GetDesc();
        if (desc.Height > 0) {
            const int h = (int)(desc.Height * 0.67f);
            return h > 0 ? h : 1;
        }
    }
    return 0;
}

float __stdcall GetOptimalSharpness(int id) {
    initialize();
    if (g_fsr4 && g_fsr4->isAvailable()) return g_fsr4->getOptimalSharpness(id);
    return 0.5f;
}

float __stdcall GetOptimalMipmapBias(int id) {
    initialize();
    if (g_fsr4 && g_fsr4->isAvailable()) return g_fsr4->getOptimalMipmapBias(id);
    return 0.0f;
}

void __stdcall SetDebug(bool debug) {
    initialize();
    (void)debug;
}

void __stdcall ReleaseUpscaleFeature(int id) {
    initialize();
    if (g_fsr4 && g_fsr4->isAvailable()) {
        g_fsr4->release(id);
    }
    eraseFallbackTexture(id);
}

int __stdcall GetJitterPhaseCount(int id) {
    initialize();
    if (g_fsr4 && g_fsr4->isAvailable()) return g_fsr4->getJitterPhaseCount(id);
    return 1;
}

int __stdcall GetJitterOffset(float* outX, float* outY, int index, int phaseCount) {
    initialize();
    if (g_fsr4 && g_fsr4->isAvailable()) return g_fsr4->getJitterOffset(0, outX, outY, index, phaseCount);
    return -1;
}

void __stdcall InitLogDelegate(void (*Log)(char* message, int iSize)) {
    initialize();
    g_logDelegate = Log;
}

bool __stdcall IsUpscaleMethodAvailable(int upscaleMethod) {
    initialize();
    if (g_fsr4) {
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
    return false;
}

char* __stdcall GetUpscaleMethodName(int upscaleMethod) {
    static thread_local char nameBuf[64];
    initialize();

    if (g_fsr4) {
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

    nameBuf[0] = '\0';
    return nameBuf;
}
