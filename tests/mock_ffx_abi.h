#pragma once

#include <cstddef>
#include <cstdint>

// ---------------------------------------------------------------------------
// Binary-exact x64 POD mirror structs matching src/FSR4Backend.cpp's FFX ABI
// ---------------------------------------------------------------------------

struct MockFfxApiHeader {
    uint64_t type;
    MockFfxApiHeader* pNext;
};

struct MockFfxDimensions2D {
    uint32_t width;
    uint32_t height;
};

using MockFfxApiMessage = void (*)(uint32_t type, const wchar_t* message);

struct MockFfxCreateContextDescUpscale {
    MockFfxApiHeader header;
    uint32_t flags;
    MockFfxDimensions2D maxRenderSize;
    MockFfxDimensions2D maxUpscaleSize;
    MockFfxApiMessage fpMessage;
};

struct MockFfxOverrideVersion {
    MockFfxApiHeader header;
    uint64_t versionId;
};

struct MockFfxCreateBackendDX12Desc {
    MockFfxApiHeader header;
    void* device;
};

struct MockFfxQueryDescGetVersions {
    MockFfxApiHeader header;
    uint64_t createDescType;
    void* device;
    uint64_t* outputCount;
    uint64_t* versionIds;
    const char** versionNames;
};

struct MockFfxQueryGetProviderVersion {
    MockFfxApiHeader header;
    uint64_t versionId;
    const char* versionName;
};

struct MockFfxQueryDescUpscaleGetJitterPhaseCount {
    MockFfxApiHeader header;
    uint32_t renderWidth;
    uint32_t displayWidth;
    int32_t* pOutPhaseCount;
};

struct MockFfxQueryDescUpscaleGetJitterOffset {
    MockFfxApiHeader header;
    int32_t index;
    int32_t phaseCount;
    float* pOutX;
    float* pOutY;
};

struct MockFfxResourceDescription {
    uint32_t type;
    uint32_t format;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t mipCount;
    uint32_t flags;
    uint32_t usage;
};

struct MockFfxResource {
    void* resource;
    MockFfxResourceDescription description;
    uint32_t state;
};

struct MockFfxFloatCoords2D {
    float x;
    float y;
};

struct MockFfxDispatchDescUpscale {
    MockFfxApiHeader header;
    void* commandList;
    MockFfxResource color;
    MockFfxResource depth;
    MockFfxResource motionVectors;
    MockFfxResource exposure;
    MockFfxResource reactive;
    MockFfxResource transparencyAndComposition;
    MockFfxResource output;
    MockFfxFloatCoords2D jitterOffset;
    MockFfxFloatCoords2D motionVectorScale;
    MockFfxDimensions2D renderSize;
    MockFfxDimensions2D upscaleSize;
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

// Compile-time x64 ABI layout invariants matching src/FSR4Backend.cpp
static_assert(sizeof(MockFfxApiHeader) == 16, "MockFfxApiHeader size");
static_assert(sizeof(MockFfxResourceDescription) == 32, "MockFfxResourceDescription size");
static_assert(sizeof(MockFfxResource) == 48, "MockFfxResource size");
static_assert(sizeof(MockFfxDispatchDescUpscale) == 432, "MockFfxDispatchDescUpscale size");
static_assert(sizeof(MockFfxCreateContextDescUpscale) == 48, "MockFfxCreateContextDescUpscale size");
static_assert(sizeof(MockFfxOverrideVersion) == 24, "MockFfxOverrideVersion size");
static_assert(sizeof(MockFfxCreateBackendDX12Desc) == 24, "MockFfxCreateBackendDX12Desc size");
static_assert(sizeof(MockFfxQueryDescGetVersions) == 56, "MockFfxQueryDescGetVersions size");
static_assert(sizeof(MockFfxQueryGetProviderVersion) == 32, "MockFfxQueryGetProviderVersion size");
static_assert(sizeof(MockFfxQueryDescUpscaleGetJitterPhaseCount) == 32, "MockFfxQueryDescUpscaleGetJitterPhaseCount size");
static_assert(sizeof(MockFfxQueryDescUpscaleGetJitterOffset) == 40, "MockFfxQueryDescUpscaleGetJitterOffset size");

static_assert(offsetof(MockFfxCreateContextDescUpscale, maxUpscaleSize) == 28, "maxUpscaleSize offset");
static_assert(offsetof(MockFfxResource, description) == 8, "MockFfxResource::description offset");
static_assert(offsetof(MockFfxResource, state) == 40, "MockFfxResource::state offset");
static_assert(offsetof(MockFfxDispatchDescUpscale, commandList) == 16, "commandList offset");
static_assert(offsetof(MockFfxDispatchDescUpscale, color) == 24, "color offset");
static_assert(offsetof(MockFfxDispatchDescUpscale, output) == 312, "output offset");
static_assert(offsetof(MockFfxDispatchDescUpscale, renderSize) == 376, "renderSize offset");
static_assert(offsetof(MockFfxDispatchDescUpscale, upscaleSize) == 384, "upscaleSize offset");
static_assert(offsetof(MockFfxDispatchDescUpscale, flags) == 428, "flags offset");

// FFX descriptor type constants
static constexpr uint64_t kMockFfxCreateContextUpscale    = 0x00010000u;
static constexpr uint64_t kMockFfxDispatchUpscale         = 0x00010001u;
static constexpr uint64_t kMockFfxQueryJitterPhaseCount   = 0x00010004u;
static constexpr uint64_t kMockFfxQueryGetJitterOffset    = 0x00010005u;
static constexpr uint64_t kMockFfxCreateBackendDX12       = 2u;
static constexpr uint64_t kMockFfxQueryGetVersions        = 4u;
static constexpr uint64_t kMockFfxOverrideVersion         = 5u;
static constexpr uint64_t kMockFfxQueryGetProviderVersion = 6u;

// FFX create flags
static constexpr uint32_t kMockFfxUpscaleEnableDynamicResolution = (1u << 6);

// FFX resource state / type / usage / format constants
static constexpr uint32_t kMockFfxResourceStateCommon          = (1u << 0);
static constexpr uint32_t kMockFfxResourceStateUnorderedAccess = (1u << 1);

static constexpr uint32_t kMockFfxResourceTypeBuffer      = 0u;
static constexpr uint32_t kMockFfxResourceTypeTexture1D   = 1u;
static constexpr uint32_t kMockFfxResourceTypeTexture2D   = 2u;
static constexpr uint32_t kMockFfxResourceTypeTextureCube = 3u;
static constexpr uint32_t kMockFfxResourceTypeTexture3D   = 4u;

static constexpr uint32_t kMockFfxResourceUsageReadOnly     = 0u;
static constexpr uint32_t kMockFfxResourceUsageRenderTarget = (1u << 0);
static constexpr uint32_t kMockFfxResourceUsageUav          = (1u << 1);
static constexpr uint32_t kMockFfxResourceUsageDepthTarget  = (1u << 2);

// ---------------------------------------------------------------------------
// Plain C POD inspection structs for GetProcAddress across /MT CRT boundaries
// ---------------------------------------------------------------------------
struct MockFfxCreateRecord {
    uint32_t flags;
    uint32_t maxRenderWidth;
    uint32_t maxRenderHeight;
    uint32_t maxUpscaleWidth;
    uint32_t maxUpscaleHeight;
    uint64_t overrideVersionId;
    void*    backendDevice;
    uint32_t resultCode;
};

struct MockFfxStats {
    uint32_t createAttemptCount;
    uint32_t createSuccessCount;
    uint32_t destroyCount;
    uint32_t dispatchCount;
    uint64_t lastProviderVersionId;
};

using PFN_MockFfxResetState        = void (__cdecl*)();
using PFN_MockFfxGetStats          = void (__cdecl*)(MockFfxStats* outStats);
using PFN_MockFfxGetCreateRecord   = int (__cdecl*)(uint32_t index, MockFfxCreateRecord* outRecord);
using PFN_MockFfxGetDispatchRecord = int (__cdecl*)(uint32_t index, MockFfxDispatchDescUpscale* outRecord);
