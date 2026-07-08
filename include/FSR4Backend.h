#pragma once

#include <cstdint>
#include <Windows.h>

struct UpscaleParams;

class FSR4Backend {
public:
    FSR4Backend();
    ~FSR4Backend();

    bool setup(void* deviceOrQueue, int graphicsAPI);
    bool isAvailable() const;
    bool isReady() const;

    void* createContext(int id, int upscaleMethod, int qualityLevel,
        int displaySizeX, int displaySizeY, bool isContentHDR,
        bool depthInverted, bool yAxisInverted, bool motionVectorsJittered,
        bool enableSharpening, bool enableAutoExposure, int format);

    void evaluate(int id, void* color, void* motionVector, void* depth,
        void* destination, int renderSizeX, int renderSizeY,
        float sharpness, float jitterOffsetX, float jitterOffsetY,
        float motionScaleX, float motionScaleY, bool reset,
        float nearPlane, float farPlane, float verticalFOV,
        bool execute, void* cmdList);
    void evaluate(UpscaleParams* params);
    void release(int id);

    void setMotionScaleX(int id, float scale);
    void setMotionScaleY(int id, float scale);
    int getRenderWidth(int id) const;
    int getRenderHeight(int id) const;
    float getOptimalSharpness(int id) const;
    float getOptimalMipmapBias(int id) const;

    int getJitterPhaseCount(int id) const;
    int getJitterOffset(int id, float* outX, float* outY, int index, int phaseCount) const;
    void logJitterOnce(float x, float y, int index, int phaseCount) const;

    bool isMethodAvailable(int upscaleMethod) const;
    const char* getMethodName(int upscaleMethod) const;

    // Prints "FSR version: X | GPU: Y" once, in BOTH verbosity modes.
    void logSystemInfo();

    // Enumerate FSR versions the GPU supports and pick the newest (so the mod
    // runs on all GPUs: FSR4 4.1.1 if possible, else FSR3.1, ...). Stores the
    // id+name in m_impl. Returns true if at least one version is available.
    bool selectFsrVersion();

private:
    struct Impl;
    Impl* m_impl;
};
