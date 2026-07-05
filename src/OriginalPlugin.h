#pragma once

#include <Windows.h>
#include "../include/PDPerfPlugin.h"

class OriginalPlugin {
public:
    OriginalPlugin();
    ~OriginalPlugin();

    bool load();
    bool isLoaded() const;

    bool setupDirectX(void* item, int graphicsAPI);
    void* simpleInit(int id, int upscaleMethod, int qualityLevel,
        int displaySizeX, int displaySizeY, bool isContentHDR,
        bool depthInverted, bool YAxisInverted, bool motionVetorsJittered,
        bool enableSharpening, bool enableAutoExposure, int format);
    void* initUpscaler(InitParams* params);
    void simpleEvaluate(int id, void* color, void* motionVector, void* depth,
        void* mask, void* destination, int renderSizeX, int renderSizeY,
        float sharpness, float jitterOffsetX, float jitterOffsetY,
        int motionScaleX, int motionScaleY, bool reset, float nearPlane,
        float farPlane, float verticalFOV, bool execute);
    void evaluateUpscaler(UpscaleParams* params);
    void setMotionScaleX(int id, float motionScaleX);
    void setMotionScaleY(int id, float motionScaleX);
    int getRenderWidth(int id);
    int getRenderHeight(int id);
    float getOptimalSharpness(int id);
    float getOptimalMipmapBias(int id);
    void setDebug(bool debug);
    void releaseUpscaleFeature(int id);
    int getJitterPhaseCount(int id);
    int getJitterOffset(float* outX, float* outY, int index, int phaseCount);
    void initLogDelegate(void (*Log)(char* message, int iSize));
    bool isUpscaleMethodAvailable(int upscaleMethod);
    char* getUpscaleMethodName(int upscaleMethod);

private:
    HMODULE m_dll = nullptr;

    using PFN_SetupDirectX = bool(__stdcall*)(void*, int);
    using PFN_SimpleInit = void*(__stdcall*)(int, int, int, int, int, bool, bool, bool, bool, bool, bool, int);
    using PFN_InitUpscaler = void*(__stdcall*)(InitParams*);
    using PFN_SimpleEvaluate = void(__stdcall*)(int, void*, void*, void*, void*, void*, int, int, float, float, float, int, int, bool, float, float, float, bool);
    using PFN_EvaluateUpscaler = void(__stdcall*)(UpscaleParams*);
    using PFN_SetMotionScaleX = void(__stdcall*)(int, float);
    using PFN_SetMotionScaleY = void(__stdcall*)(int, float);
    using PFN_GetRenderWidth = int(__stdcall*)(int);
    using PFN_GetRenderHeight = int(__stdcall*)(int);
    using PFN_GetOptimalSharpness = float(__stdcall*)(int);
    using PFN_GetOptimalMipmapBias = float(__stdcall*)(int);
    using PFN_SetDebug = void(__stdcall*)(bool);
    using PFN_ReleaseUpscaleFeature = void(__stdcall*)(int);
    using PFN_GetJitterPhaseCount = int(__stdcall*)(int);
    using PFN_GetJitterOffset = int(__stdcall*)(float*, float*, int, int);
    using PFN_InitLogDelegate = void(__stdcall*)(void (*)(char*, int));
    using PFN_IsUpscaleMethodAvailable = bool(__stdcall*)(int);
    using PFN_GetUpscaleMethodName = char*(__stdcall*)(int);

    PFN_SetupDirectX pSetupDirectX = nullptr;
    PFN_SimpleInit pSimpleInit = nullptr;
    PFN_InitUpscaler pInitUpscaler = nullptr;
    PFN_SimpleEvaluate pSimpleEvaluate = nullptr;
    PFN_EvaluateUpscaler pEvaluateUpscaler = nullptr;
    PFN_SetMotionScaleX pSetMotionScaleX = nullptr;
    PFN_SetMotionScaleY pSetMotionScaleY = nullptr;
    PFN_GetRenderWidth pGetRenderWidth = nullptr;
    PFN_GetRenderHeight pGetRenderHeight = nullptr;
    PFN_GetOptimalSharpness pGetOptimalSharpness = nullptr;
    PFN_GetOptimalMipmapBias pGetOptimalMipmapBias = nullptr;
    PFN_SetDebug pSetDebug = nullptr;
    PFN_ReleaseUpscaleFeature pReleaseUpscaleFeature = nullptr;
    PFN_GetJitterPhaseCount pGetJitterPhaseCount = nullptr;
    PFN_GetJitterOffset pGetJitterOffset = nullptr;
    PFN_InitLogDelegate pInitLogDelegate = nullptr;
    PFN_IsUpscaleMethodAvailable pIsUpscaleMethodAvailable = nullptr;
    PFN_GetUpscaleMethodName pGetUpscaleMethodName = nullptr;
};
