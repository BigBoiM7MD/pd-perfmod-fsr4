#include "OriginalPlugin.h"
#include "Logging.h"

OriginalPlugin::OriginalPlugin() {}

OriginalPlugin::~OriginalPlugin() {
    if (m_dll) { FreeLibrary(m_dll); m_dll = nullptr; }
}

bool OriginalPlugin::load() {
    Logging::info("OriginalPlugin: Loading PDPerfPlugin_original.dll...");
    m_dll = LoadLibraryA("PDPerfPlugin_original.dll");
    if (!m_dll) {
        Logging::warn("OriginalPlugin: Failed to load (err=%d)", GetLastError());
        return false;
    }

    pSetupDirectX = (PFN_SetupDirectX)GetProcAddress(m_dll, "SetupDirectX");
    pSimpleInit = (PFN_SimpleInit)GetProcAddress(m_dll, "SimpleInit");
    pInitUpscaler = (PFN_InitUpscaler)GetProcAddress(m_dll, "InitUpscaler");
    pSimpleEvaluate = (PFN_SimpleEvaluate)GetProcAddress(m_dll, "SimpleEvaluate");
    pEvaluateUpscaler = (PFN_EvaluateUpscaler)GetProcAddress(m_dll, "EvaluateUpscaler");
    pSetMotionScaleX = (PFN_SetMotionScaleX)GetProcAddress(m_dll, "SetMotionScaleX");
    pSetMotionScaleY = (PFN_SetMotionScaleY)GetProcAddress(m_dll, "SetMotionScaleY");
    pGetRenderWidth = (PFN_GetRenderWidth)GetProcAddress(m_dll, "GetRenderWidth");
    pGetRenderHeight = (PFN_GetRenderHeight)GetProcAddress(m_dll, "GetRenderHeight");
    pGetOptimalSharpness = (PFN_GetOptimalSharpness)GetProcAddress(m_dll, "GetOptimalSharpness");
    pGetOptimalMipmapBias = (PFN_GetOptimalMipmapBias)GetProcAddress(m_dll, "GetOptimalMipmapBias");
    pSetDebug = (PFN_SetDebug)GetProcAddress(m_dll, "SetDebug");
    pReleaseUpscaleFeature = (PFN_ReleaseUpscaleFeature)GetProcAddress(m_dll, "ReleaseUpscaleFeature");
    pGetJitterPhaseCount = (PFN_GetJitterPhaseCount)GetProcAddress(m_dll, "GetJitterPhaseCount");
    pGetJitterOffset = (PFN_GetJitterOffset)GetProcAddress(m_dll, "GetJitterOffset");
    pInitLogDelegate = (PFN_InitLogDelegate)GetProcAddress(m_dll, "InitLogDelegate");
    pIsUpscaleMethodAvailable = (PFN_IsUpscaleMethodAvailable)GetProcAddress(m_dll, "IsUpscaleMethodAvailable");
    pGetUpscaleMethodName = (PFN_GetUpscaleMethodName)GetProcAddress(m_dll, "GetUpscaleMethodName");

    Logging::info("OriginalPlugin: Loaded successfully");
    return true;
}

bool OriginalPlugin::isLoaded() const { return m_dll != nullptr; }

#define FORWARD_OR_FAIL(func, ret) do { if (!func) { Logging::error("OriginalPlugin: " #func " null"); return ret; } } while(0)

bool OriginalPlugin::setupDirectX(void* item, int graphicsAPI) { FORWARD_OR_FAIL(pSetupDirectX, false); return pSetupDirectX(item, graphicsAPI); }

void* OriginalPlugin::simpleInit(int id, int upscaleMethod, int qualityLevel, int displaySizeX, int displaySizeY, bool isContentHDR, bool depthInverted, bool YAxisInverted, bool motionVetorsJittered, bool enableSharpening, bool enableAutoExposure, int format) {
    FORWARD_OR_FAIL(pSimpleInit, nullptr);
    return pSimpleInit(id, upscaleMethod, qualityLevel, displaySizeX, displaySizeY, isContentHDR, depthInverted, YAxisInverted, motionVetorsJittered, enableSharpening, enableAutoExposure, format);
}

void* OriginalPlugin::initUpscaler(InitParams* params) { FORWARD_OR_FAIL(pInitUpscaler, nullptr); return pInitUpscaler(params); }

void OriginalPlugin::simpleEvaluate(int id, void* color, void* motionVector, void* depth, void* mask, void* destination, int renderSizeX, int renderSizeY, float sharpness, float jitterOffsetX, float jitterOffsetY, int motionScaleX, int motionScaleY, bool reset, float nearPlane, float farPlane, float verticalFOV, bool execute) {
    if (!pSimpleEvaluate) return;
    pSimpleEvaluate(id, color, motionVector, depth, mask, destination, renderSizeX, renderSizeY, sharpness, jitterOffsetX, jitterOffsetY, motionScaleX, motionScaleY, reset, nearPlane, farPlane, verticalFOV, execute);
}

void OriginalPlugin::evaluateUpscaler(UpscaleParams* params) { if (pEvaluateUpscaler) pEvaluateUpscaler(params); }
void OriginalPlugin::setMotionScaleX(int id, float v) { if (pSetMotionScaleX) pSetMotionScaleX(id, v); }
void OriginalPlugin::setMotionScaleY(int id, float v) { if (pSetMotionScaleY) pSetMotionScaleY(id, v); }
int OriginalPlugin::getRenderWidth(int id) { FORWARD_OR_FAIL(pGetRenderWidth, 0); return pGetRenderWidth(id); }
int OriginalPlugin::getRenderHeight(int id) { FORWARD_OR_FAIL(pGetRenderHeight, 0); return pGetRenderHeight(id); }
float OriginalPlugin::getOptimalSharpness(int id) { FORWARD_OR_FAIL(pGetOptimalSharpness, 0); return pGetOptimalSharpness(id); }
float OriginalPlugin::getOptimalMipmapBias(int id) { FORWARD_OR_FAIL(pGetOptimalMipmapBias, 0); return pGetOptimalMipmapBias(id); }
void OriginalPlugin::setDebug(bool d) { if (pSetDebug) pSetDebug(d); }
void OriginalPlugin::releaseUpscaleFeature(int id) { if (pReleaseUpscaleFeature) pReleaseUpscaleFeature(id); }
int OriginalPlugin::getJitterPhaseCount(int id) { FORWARD_OR_FAIL(pGetJitterPhaseCount, 1); return pGetJitterPhaseCount(id); }
int OriginalPlugin::getJitterOffset(float* ox, float* oy, int idx, int pc) { FORWARD_OR_FAIL(pGetJitterOffset, -1); return pGetJitterOffset(ox, oy, idx, pc); }
void OriginalPlugin::initLogDelegate(void (*fn)(char*, int)) { if (pInitLogDelegate) pInitLogDelegate(fn); }
bool OriginalPlugin::isUpscaleMethodAvailable(int m) { FORWARD_OR_FAIL(pIsUpscaleMethodAvailable, false); return pIsUpscaleMethodAvailable(m); }
char* OriginalPlugin::getUpscaleMethodName(int m) { FORWARD_OR_FAIL(pGetUpscaleMethodName, (char*)""); return pGetUpscaleMethodName(m); }
