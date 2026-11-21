#include "../include/FSR4Backend.h"
#include "../include/Logging.h"
#include "../include/PDPerfPlugin.h"
#include "../include/RbSwapPass.h"
#include "../include/SrwLockGuard.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <unordered_map>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <exception>
#include <string>
#include <utility>
#include <vector>

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

// Output format override (read from pd-perfmod-fsr4.ini [Backend] OutputFormat).
// 0 = auto-detect (Wine SDR defaults to R8G8B8A8_UNORM; HDR keeps container;
// Windows keeps backbuffer format). Non-zero forces a DXGI_FORMAT (e.g.
// 28=R8G8B8A8_UNORM, 87=B8G8R8A8_UNORM, 24=R10G10B10A2_UNORM, 10=R16G16B16A16_FLOAT).
// RBSwap mode ([Backend] RBSwap): -1 = auto (Wine SDR), 0 = force off, 1 = force on.
static int s_outputFormatOverride = 0; // 0 => no override
static int s_rbSwapMode           = -1; // -1 => auto, 0 => off, 1 => on

static void readBackendIni() {
    const std::wstring queryPath = Logging::getIniApiPath();
    wchar_t buf[32] = L"0";
    if (queryPath.size() < MAX_PATH) {
        const DWORD copied = GetPrivateProfileStringW(L"Backend", L"OutputFormat", L"0", buf, 32, queryPath.c_str());
        if (copied == 0) {
            wcscpy_s(buf, L"0");
        }
    } else {
        Logging::readIniFallback(L"Backend", L"OutputFormat", L"0", buf, 32);
    }
    s_outputFormatOverride = _wtoi(buf);
    Logging::info("FSR4Backend: ini Backend.OutputFormat=%d (0=auto-detect)", s_outputFormatOverride);

    wchar_t rbBuf[32] = L"-1";
    if (queryPath.size() < MAX_PATH) {
        const DWORD copied = GetPrivateProfileStringW(L"Backend", L"RBSwap", L"-1", rbBuf, 32, queryPath.c_str());
        if (copied == 0) {
            wcscpy_s(rbBuf, L"-1");
        }
    } else {
        Logging::readIniFallback(L"Backend", L"RBSwap", L"-1", rbBuf, 32);
    }
    const wchar_t* rbStart = rbBuf;
    while (*rbStart == L' ' || *rbStart == L'\t') {
        ++rbStart;
    }
    wchar_t* rbEnd = nullptr;
    errno = 0;
    const long parsedRb = std::wcstol(rbStart, &rbEnd, 10);
    while (rbEnd && (*rbEnd == L' ' || *rbEnd == L'\t')) {
        ++rbEnd;
    }
    if (rbEnd == rbStart || !rbEnd || *rbEnd != L'\0' || errno == ERANGE) {
        Logging::warn("FSR4Backend: invalid Backend.RBSwap='%ls' (expected -1, 0, or 1); using -1 (auto)", rbBuf);
        s_rbSwapMode = -1;
    } else if (parsedRb < 0) {
        s_rbSwapMode = -1;
    } else if (parsedRb == 0) {
        s_rbSwapMode = 0;
    } else {
        s_rbSwapMode = 1;
    }
    Logging::info("FSR4Backend: ini Backend.RBSwap=%d (-1=auto, 0=off, 1=on)", s_rbSwapMode);
}

// True when running under Wine/Proton (Linux) rather than native Windows.
// Detected via the wine_get_version export that Wine/Proton always add to ntdll;
// it is absent on genuine Windows. Cached after first call. This is the standard,
// documented Wine-detection method and needs no special privileges.
static bool isRunningUnderWine() {
    static int cached = -1;
    if (cached >= 0) return cached != 0;
    cached = 0;
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (ntdll && GetProcAddress(ntdll, "wine_get_version"))
        cached = 1;
    return cached != 0;
}

// Upscale sub-IDs
static constexpr uint64_t FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE              = MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_UPSCALE, 0x00);
static constexpr uint64_t FFX_API_DISPATCH_DESC_TYPE_UPSCALE                    = MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_UPSCALE, 0x01);
static constexpr uint64_t FFX_API_QUERY_DESC_TYPE_UPSCALE_GET_JITTER_PHASE_COUNT = MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_UPSCALE, 0x04);
static constexpr uint64_t FFX_API_QUERY_DESC_TYPE_UPSCALE_GET_JITTER_OFFSET      = MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_UPSCALE, 0x05);
static constexpr uint64_t FFX_API_QUERY_DESC_TYPE_GET_VERSIONS                    = 4u;
static constexpr uint64_t FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION            = 6u;
static constexpr uint64_t FFX_API_DESC_TYPE_OVERRIDE_VERSION                      = 5u;

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

// FFX resource types (FfxApiResourceType, ffx_api_types.h).
static constexpr uint32_t FFX_API_RESOURCE_TYPE_BUFFER       = 0;
static constexpr uint32_t FFX_API_RESOURCE_TYPE_TEXTURE1D    = 1;
static constexpr uint32_t FFX_API_RESOURCE_TYPE_TEXTURE2D    = 2;
static constexpr uint32_t FFX_API_RESOURCE_TYPE_TEXTURE_CUBE = 3;
static constexpr uint32_t FFX_API_RESOURCE_TYPE_TEXTURE3D    = 4;

// FFX resource usage flags (FfxApiResourceUsage, ffx_api_types.h) -- BIT FLAGS.
static constexpr uint32_t FFX_API_RESOURCE_USAGE_READ_ONLY     = 0;         // no usage flags
static constexpr uint32_t FFX_API_RESOURCE_USAGE_RENDERTARGET  = (1u << 0); // 1
static constexpr uint32_t FFX_API_RESOURCE_USAGE_UAV           = (1u << 1); // 2
static constexpr uint32_t FFX_API_RESOURCE_USAGE_DEPTHTARGET   = (1u << 2); // 4
static constexpr uint32_t FFX_API_RESOURCE_USAGE_INDIRECT      = (1u << 3); // 8
static constexpr uint32_t FFX_API_RESOURCE_USAGE_ARRAYVIEW     = (1u << 4); // 16
static constexpr uint32_t FFX_API_RESOURCE_USAGE_STENCILTARGET = (1u << 5); // 32
static constexpr uint32_t FFX_API_RESOURCE_USAGE_VALID_MASK    = 0x3Fu;

// FFX surface formats (FfxApiSurfaceFormat, ffx_api_types.h, values 0..39).
static constexpr uint32_t FFX_API_SURFACE_FORMAT_UNKNOWN                 = 0;
static constexpr uint32_t FFX_API_SURFACE_FORMAT_R32G32B32A32_TYPELESS   = 1;
static constexpr uint32_t FFX_API_SURFACE_FORMAT_R32G32B32A32_UINT       = 2;
static constexpr uint32_t FFX_API_SURFACE_FORMAT_R32G32B32A32_FLOAT      = 3;
static constexpr uint32_t FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT      = 4;
static constexpr uint32_t FFX_API_SURFACE_FORMAT_R32G32B32_FLOAT         = 5;
static constexpr uint32_t FFX_API_SURFACE_FORMAT_R32G32_FLOAT            = 6;
static constexpr uint32_t FFX_API_SURFACE_FORMAT_R8_UINT                 = 7;
static constexpr uint32_t FFX_API_SURFACE_FORMAT_R32_UINT                = 8;
static constexpr uint32_t FFX_API_SURFACE_FORMAT_R8G8B8A8_TYPELESS       = 9;
static constexpr uint32_t FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM          = 10;
static constexpr uint32_t FFX_API_SURFACE_FORMAT_R8G8B8A8_SNORM          = 11;
static constexpr uint32_t FFX_API_SURFACE_FORMAT_R8G8B8A8_SRGB           = 12;
static constexpr uint32_t FFX_API_SURFACE_FORMAT_B8G8R8A8_TYPELESS       = 13;
static constexpr uint32_t FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM          = 14;
static constexpr uint32_t FFX_API_SURFACE_FORMAT_B8G8R8A8_SRGB           = 15;
static constexpr uint32_t FFX_API_SURFACE_FORMAT_R11G11B10_FLOAT         = 16;
static constexpr uint32_t FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM       = 17;
static constexpr uint32_t FFX_API_SURFACE_FORMAT_R16G16_FLOAT            = 18;
static constexpr uint32_t FFX_API_SURFACE_FORMAT_R16G16_UINT             = 19;
static constexpr uint32_t FFX_API_SURFACE_FORMAT_R16G16_SINT             = 20;
static constexpr uint32_t FFX_API_SURFACE_FORMAT_R16_FLOAT               = 21;
static constexpr uint32_t FFX_API_SURFACE_FORMAT_R16_UINT                = 22;
static constexpr uint32_t FFX_API_SURFACE_FORMAT_R16_UNORM               = 23;
static constexpr uint32_t FFX_API_SURFACE_FORMAT_R16_SNORM               = 24;
static constexpr uint32_t FFX_API_SURFACE_FORMAT_R8_UNORM                = 25;
static constexpr uint32_t FFX_API_SURFACE_FORMAT_R8G8_UNORM              = 26;
static constexpr uint32_t FFX_API_SURFACE_FORMAT_R8G8_UINT               = 27;
static constexpr uint32_t FFX_API_SURFACE_FORMAT_R32_FLOAT               = 28;
static constexpr uint32_t FFX_API_SURFACE_FORMAT_R9G9B9E5_SHAREDEXP      = 29;
static constexpr uint32_t FFX_API_SURFACE_FORMAT_R16G16B16A16_TYPELESS   = 30;
static constexpr uint32_t FFX_API_SURFACE_FORMAT_R32G32_TYPELESS         = 31;
static constexpr uint32_t FFX_API_SURFACE_FORMAT_R10G10B10A2_TYPELESS    = 32;
static constexpr uint32_t FFX_API_SURFACE_FORMAT_R16G16_TYPELESS         = 33;
static constexpr uint32_t FFX_API_SURFACE_FORMAT_R16_TYPELESS            = 34;
static constexpr uint32_t FFX_API_SURFACE_FORMAT_R8_TYPELESS             = 35;
static constexpr uint32_t FFX_API_SURFACE_FORMAT_R8G8_TYPELESS           = 36;
static constexpr uint32_t FFX_API_SURFACE_FORMAT_R32_TYPELESS            = 37;
static constexpr uint32_t FFX_API_SURFACE_FORMAT_R32G32_UINT             = 38;
static constexpr uint32_t FFX_API_SURFACE_FORMAT_R8_SNORM                = 39;

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

// Forced version override. When chained into the create desc pNext, the SDK
// uses EXACTLY this version id (no auto-fallback) -- so we only ever attach it
// with an id we already confirmed the GPU supports (see selectFsrVersion()).
struct ffxOverrideVersion {
    ffxApiHeader header;
    uint64_t versionId;
};

// Null-context query: enumerate FSR versions the GPU actually supports.
struct ffxQueryDescGetVersions {
    ffxQueryDescHeader header;
    uint64_t createDescType;  // FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE
    void*   device;           // ID3D12Device*
    uint64_t* outputCount;    // in/out: capacity -> returned count
    uint64_t* versionIds;     // out: array of version ids
    const char** versionNames;// out: array of version name strings (copy them!)
};

// Context query: which version the created context actually initialized at.
struct ffxQueryGetProviderVersion {
    ffxQueryDescHeader header;
    uint64_t versionId;       // out: running version id
    const char* versionName;  // out: running version name (copy it!)
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

// Verify struct sizes and member offsets match SDK expectations (MSVC x64)
static_assert(sizeof(ffxApiHeader) == 16, "ffxApiHeader size");

static_assert(sizeof(ffxCreateBackendDX12Desc) == 24, "ffxCreateBackendDX12Desc size");
static_assert(offsetof(ffxCreateBackendDX12Desc, device) == 16, "ffxCreateBackendDX12Desc::device offset");

static_assert(sizeof(ffxCreateContextDescUpscale) == 48, "ffxCreateContextDescUpscale size");
static_assert(offsetof(ffxCreateContextDescUpscale, flags) == 16, "ffxCreateContextDescUpscale::flags offset");
static_assert(offsetof(ffxCreateContextDescUpscale, maxRenderSize) == 20, "ffxCreateContextDescUpscale::maxRenderSize offset");
static_assert(offsetof(ffxCreateContextDescUpscale, maxUpscaleSize) == 28, "ffxCreateContextDescUpscale::maxUpscaleSize offset");
static_assert(offsetof(ffxCreateContextDescUpscale, fpMessage) == 40, "ffxCreateContextDescUpscale::fpMessage offset");

static_assert(sizeof(ffxOverrideVersion) == 24, "ffxOverrideVersion size");
static_assert(offsetof(ffxOverrideVersion, versionId) == 16, "ffxOverrideVersion::versionId offset");

static_assert(sizeof(ffxQueryDescGetVersions) == 56, "ffxQueryDescGetVersions size");
static_assert(offsetof(ffxQueryDescGetVersions, versionIds) == 40, "ffxQueryDescGetVersions::versionIds offset");
static_assert(offsetof(ffxQueryDescGetVersions, versionNames) == 48, "ffxQueryDescGetVersions::versionNames offset");

static_assert(sizeof(ffxQueryGetProviderVersion) == 32, "ffxQueryGetProviderVersion size");
static_assert(offsetof(ffxQueryGetProviderVersion, versionId) == 16, "ffxQueryGetProviderVersion::versionId offset");
static_assert(offsetof(ffxQueryGetProviderVersion, versionName) == 24, "ffxQueryGetProviderVersion::versionName offset");

static_assert(sizeof(ffxQueryDescUpscaleGetJitterPhaseCount) == 32, "ffxQueryDescUpscaleGetJitterPhaseCount size");
static_assert(offsetof(ffxQueryDescUpscaleGetJitterPhaseCount, pOutPhaseCount) == 24, "ffxQueryDescUpscaleGetJitterPhaseCount::pOutPhaseCount offset");

static_assert(sizeof(ffxQueryDescUpscaleGetJitterOffset) == 40, "ffxQueryDescUpscaleGetJitterOffset size");
static_assert(offsetof(ffxQueryDescUpscaleGetJitterOffset, pOutX) == 24, "ffxQueryDescUpscaleGetJitterOffset::pOutX offset");
static_assert(offsetof(ffxQueryDescUpscaleGetJitterOffset, pOutY) == 32, "ffxQueryDescUpscaleGetJitterOffset::pOutY offset");

static_assert(sizeof(FfxApiResourceDescription) == 32, "FfxApiResourceDescription size");
static_assert(offsetof(FfxApiResourceDescription, usage) == 28, "FfxApiResourceDescription::usage offset");

static_assert(sizeof(FfxApiResource) == 48, "FfxApiResource size");
static_assert(offsetof(FfxApiResource, description) == 8, "FfxApiResource::description offset");
static_assert(offsetof(FfxApiResource, state) == 40, "FfxApiResource::state offset");

static_assert(sizeof(ffxDispatchDescUpscale) == 432, "ffxDispatchDescUpscale size");
static_assert(offsetof(ffxDispatchDescUpscale, commandList) == 16, "ffxDispatchDescUpscale::commandList offset");
static_assert(offsetof(ffxDispatchDescUpscale, color) == 24, "ffxDispatchDescUpscale::color offset");
static_assert(offsetof(ffxDispatchDescUpscale, output) == 312, "ffxDispatchDescUpscale::output offset");
static_assert(offsetof(ffxDispatchDescUpscale, renderSize) == 376, "ffxDispatchDescUpscale::renderSize offset");
static_assert(offsetof(ffxDispatchDescUpscale, upscaleSize) == 384, "ffxDispatchDescUpscale::upscaleSize offset");
static_assert(offsetof(ffxDispatchDescUpscale, sharpness) == 396, "ffxDispatchDescUpscale::sharpness offset");
static_assert(offsetof(ffxDispatchDescUpscale, flags) == 428, "ffxDispatchDescUpscale::flags offset");

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

    // R/B-swap staging resources (Linux/Proton only). FSR4 writes RGBA into
    // outputTexture; RbSwapPass writes the R<->B-swapped (BGRA-ordered) result
    // into rbSwap.swapTexture, which we hand back to REFramework so its raw
    // copy presents correct colors on the (BGRA-presenting) Proton backbuffer.
    RbSwapResources   rbSwap;
    bool              rbSwapEnabled  = false;

    int           renderWidth  = 0;
    int           renderHeight = 0;
    float         motionScaleX = 1.0f;
    float         motionScaleY = 1.0f;
    LARGE_INTEGER lastEvalQpc  = {};
};

static void decodeFFXVersion(uint32_t v, char* out, size_t outLen);

struct FSR4Backend::Impl {
    mutable SRWLOCK lock = SRWLOCK_INIT;
    HMODULE hLoaderDll = nullptr;
    PFN_ffxCreateContext  ffxCreateContextFn  = nullptr;
    PFN_ffxDestroyContext ffxDestroyContextFn = nullptr;
    PFN_ffxDispatch       ffxDispatchFn       = nullptr;
    PFN_ffxQuery          ffxQueryFn          = nullptr;

    ID3D12Device*        device       = nullptr;
    ID3D12CommandQueue*   commandQueue = nullptr;
    int                   graphicsAPI  = 0;
    bool                  available    = false;
    struct FsrVersionCandidate {
        uint64_t id = 0;
        std::string name;
    };

    // FSR version the GPU actually supports (best/newest first). Chosen at setup()
    // via ffxQueryDescGetVersions so the mod runs on all GPUs (FSR4 4.1.1 if
    // the GPU can do it, else FSR3.1, etc.). 0 = not enumerated yet.
    uint64_t chosenVersionId   = 0;
    char     chosenVersionName[64] = { 0 };
    std::vector<FsrVersionCandidate> candidateVersions;

    static constexpr size_t kNumCmdSlots = 3;
    ID3D12CommandAllocator*    cmdAllocs[kNumCmdSlots] = { nullptr, nullptr, nullptr };
    uint64_t                   slotFenceValues[kNumCmdSlots] = { 0, 0, 0 };
    size_t                     currentSlot = 0;
    bool                       cmdListOpen = false;
    ID3D12GraphicsCommandList* cmdList  = nullptr;
    ID3D12Fence*               fence      = nullptr;
    HANDLE                     fenceEvent = nullptr;
    uint64_t                   fenceValue = 0;

    std::unordered_map<int, UpscaleContext> contexts;

    // Red<->blue swap compute pipeline (Linux/Proton only), shared across
    // contexts; per-context activation lives on UpscaleContext.
    RbSwapPipeline rbSwapPipeline;

    // Per-instance one-time diagnostic log flags (reset in cleanupSetupState).
    bool inputSizesLogged    = false;
    bool rbSwapSkippedWarned = false;

    // Performance counter frequency for per-context frameTimeDelta timing.
    LARGE_INTEGER qpcFreq = {};

    static bool waitForFenceValue(Impl* impl, uint64_t targetValue) {
        if (!impl || !impl->fence || !impl->fenceEvent || targetValue == 0) {
            return true;
        }
        if (impl->fence->GetCompletedValue() >= targetValue) {
            return true;
        }
        ResetEvent(impl->fenceEvent);
        const HRESULT hr = impl->fence->SetEventOnCompletion(targetValue, impl->fenceEvent);
        if (FAILED(hr)) {
            Logging::error("FSR4Backend: SetEventOnCompletion(%llu) failed hr=%08x",
                           (unsigned long long)targetValue, hr);
            return false;
        }
        const DWORD wr = WaitForSingleObject(impl->fenceEvent, 5000);
        if (wr != WAIT_OBJECT_0) {
            const HRESULT removedReason = impl->device ? impl->device->GetDeviceRemovedReason() : S_OK;
            Logging::error("FSR4Backend: WaitForSingleObject(%llu) failed wr=%lu err=%lu removed=%08x",
                           (unsigned long long)targetValue, wr, GetLastError(), removedReason);
            return false;
        }
        return true;
    }

    static bool waitForGpu(Impl* impl) {
        if (!impl || !impl->commandQueue || !impl->fence || !impl->fenceEvent || impl->fenceValue == 0) {
            return true;
        }
        return waitForFenceValue(impl, impl->fenceValue);
    }

    static void closeInternalCmdListIfOpen(Impl* impl) {
        if (impl && impl->cmdListOpen) {
            try {
                if (impl->cmdList) {
                    impl->cmdList->Close();
                }
            } catch (...) {}
            impl->cmdListOpen = false;
        }
    }

    static void destroyContextResources(Impl* impl, UpscaleContext& ctx) {
        if (ctx.ffxCtx && impl && impl->ffxDestroyContextFn) {
            try {
                impl->ffxDestroyContextFn(&ctx.ffxCtx, nullptr);
            } catch (...) {
                Logging::warn("FSR4Backend: ffxDestroyContext threw during cleanup (id=%d)", ctx.id);
            }
            ctx.ffxCtx = nullptr;
        }
        if (ctx.outputTexture) {
            try {
                ctx.outputTexture->Release();
            } catch (...) {
                Logging::warn("FSR4Backend: outputTexture->Release threw during cleanup (id=%d)", ctx.id);
            }
            ctx.outputTexture = nullptr;
        }
        ctx.rbSwap.release();
        ctx.rbSwapEnabled = false;
    }

    static void cleanupSetupState(Impl* impl) {
        if (!impl) return;
        closeInternalCmdListIfOpen(impl);
        waitForGpu(impl);

        for (auto& [id, ctx] : impl->contexts) {
            destroyContextResources(impl, ctx);
        }
        impl->contexts.clear();

        auto safeRelease = [](auto*& ptr) {
            if (ptr) {
                try {
                    ptr->Release();
                } catch (...) {}
                ptr = nullptr;
            }
        };

        impl->rbSwapPipeline.release();

        safeRelease(impl->cmdList);
        impl->cmdListOpen = false;
        for (size_t i = 0; i < kNumCmdSlots; ++i) {
            safeRelease(impl->cmdAllocs[i]);
            impl->slotFenceValues[i] = 0;
        }
        impl->currentSlot = 0;
        safeRelease(impl->fence);
        impl->fenceValue = 0;
        if (impl->fenceEvent) {
            CloseHandle(impl->fenceEvent);
            impl->fenceEvent = nullptr;
        }

        safeRelease(impl->commandQueue);
        safeRelease(impl->device);

        impl->ffxCreateContextFn   = nullptr;
        impl->ffxDestroyContextFn  = nullptr;
        impl->ffxDispatchFn        = nullptr;
        impl->ffxQueryFn           = nullptr;
        impl->chosenVersionId      = 0;
        impl->chosenVersionName[0] = '\0';
        impl->candidateVersions.clear();

        if (impl->hLoaderDll) {
            HMODULE mod = impl->hLoaderDll;
            impl->hLoaderDll = nullptr;
            try {
                FreeLibrary(mod);
            } catch (...) {
                Logging::warn("FSR4Backend: FreeLibrary threw during cleanup");
            }
        }

        impl->available           = false;
        impl->graphicsAPI         = 0;
        impl->inputSizesLogged    = false;
        impl->rbSwapSkippedWarned = false;
    }

    static bool selectFsrVersionLocked(Impl* impl) {
        if (!impl) return false;
        impl->chosenVersionId = 0;
        impl->chosenVersionName[0] = '\0';
        impl->candidateVersions.clear();

        if (!impl->ffxQueryFn || !impl->device) {
            Logging::warn("FSR4Backend: cannot enumerate FSR versions (no query fn / device)");
            return false;
        }

        // Pass count=0 -> SDK fills outputCount with the number of available versions.
        uint64_t count = 0;
        ffxQueryDescGetVersions q = {};
        q.header.type       = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        q.createDescType    = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        q.device            = impl->device;
        q.outputCount       = &count;
        q.versionIds        = nullptr;
        q.versionNames      = nullptr;

        if (impl->ffxQueryFn(nullptr, &q.header) != 0 || count == 0) {
            Logging::warn("FSR4Backend: ffxQuery(GetVersions) found 0 supported FSR versions");
            return false;
        }

        std::vector<uint64_t> versionIds(count, 0);
        std::vector<const char*> versionNames(count, nullptr);
        q.versionIds   = versionIds.data();
        q.versionNames = versionNames.data();
        const ffxReturnCode_t rc = impl->ffxQueryFn(nullptr, &q.header);
        if (rc != 0 || count == 0) {
            Logging::warn("FSR4Backend: ffxQuery(GetVersions) failed to fill versions (rc=%u count=%llu)",
                          rc, (unsigned long long)count);
            return false;
        }

        if (count > versionIds.size()) {
            count = versionIds.size();
        }

        // Retain all enumerated candidates in priority order (newest-first). Copy
        // version strings immediately since loader memory may be invalidated later.
        impl->candidateVersions.reserve(static_cast<size_t>(count));
        for (uint64_t i = 0; i < count; ++i) {
            Impl::FsrVersionCandidate cand;
            cand.id   = versionIds[i];
            cand.name = versionNames[i] ? versionNames[i] : "?";
            impl->candidateVersions.push_back(std::move(cand));
        }

        impl->chosenVersionId = impl->candidateVersions[0].id;
        strncpy_s(impl->chosenVersionName, impl->candidateVersions[0].name.c_str(),
                  sizeof(impl->chosenVersionName) - 1);

        if (Logging::isVerbose()) {
            for (const auto& cand : impl->candidateVersions)
                Logging::info("FSR4Backend: GPU supports FSR version %s (id=0x%016llx)",
                              cand.name.c_str(), (unsigned long long)cand.id);
        }
        return true;
    }

    static void logSystemInfoLocked(Impl* impl) {
        if (!impl) return;
        char fsrVer[64] = "<unknown>";
        char gpuName[MAX_PATH] = "<unknown GPU>";

        // --- FSR version: the one this GPU actually supports (newest enumerated) ---
        // This is the real version the mod will run as (FSR4 4.1.1 on capable HW,
        // else FSR3.1, ...), NOT a hardcoded constant. Fall back to the decoded
        // FFX_UPSCALER_VERSION constant only if enumeration didn't run.
        if (impl->chosenVersionName[0])
            strncpy_s(fsrVer, impl->chosenVersionName, sizeof(fsrVer) - 1);
        else
            decodeFFXVersion(FFX_UPSCALER_VERSION, fsrVer, sizeof(fsrVer));

        // --- GPU: match the live device's LUID against enumerated adapters -----
        // We deliberately do NOT use device->QueryInterface(IDXGIDevice) — that
        // returns E_NOINTERFACE in some setups (and gives no way to be sure we
        // picked the real GPU). Instead we enumerate adapters and pick the one
        // whose AdapterLuid equals the device's LUID, so we never confuse the
        // hardware GPU with the WARP/Microsoft Basic Render Driver.
        if (impl->device) {
            // Wine/Proton virtualizes DXGI; EnumAdapters / GetDesc / LUID matching
            // can throw or mismatch. A failure here is cosmetic (we just lose the
            // GPU name), so never let it abort setup() -> FSR4 must still init.
            try {
                struct DxgiScopeGuard {
                    bool loadedDxgi = false;
                    HMODULE dxgiMod = nullptr;
                    IDXGIFactory4* factory = nullptr;
                    IDXGIAdapter* adapter = nullptr;
                    ~DxgiScopeGuard() {
                        if (adapter) {
                            try { adapter->Release(); } catch (...) {}
                            adapter = nullptr;
                        }
                        if (factory) {
                            try { factory->Release(); } catch (...) {}
                            factory = nullptr;
                        }
                        if (loadedDxgi && dxgiMod) {
                            try { FreeLibrary(dxgiMod); } catch (...) {}
                            dxgiMod = nullptr;
                        }
                    }
                } guard;

                using PFN_CreateDXGIFactory1 = HRESULT (WINAPI*)(REFIID, void**);
                guard.dxgiMod = GetModuleHandleW(L"dxgi.dll");
                if (!guard.dxgiMod) {
                    guard.dxgiMod = LoadLibraryW(L"dxgi.dll");
                    guard.loadedDxgi = (guard.dxgiMod != nullptr);
                }
                auto createDXGIFactory1Fn = guard.dxgiMod
                    ? (PFN_CreateDXGIFactory1)GetProcAddress(guard.dxgiMod, "CreateDXGIFactory1")
                    : nullptr;

                if (createDXGIFactory1Fn && SUCCEEDED(createDXGIFactory1Fn(IID_PPV_ARGS(&guard.factory))) && guard.factory) {
                    LUID devLuid = impl->device->GetAdapterLuid();
                    for (UINT i = 0; ; i++) {
                        if (FAILED(guard.factory->EnumAdapters(i, &guard.adapter)) || !guard.adapter) break; // NOT_FOUND ends it
                        DXGI_ADAPTER_DESC desc = {};
                        const HRESULT descHr = guard.adapter->GetDesc(&desc);
                        IDXGIAdapter* ad = guard.adapter;
                        guard.adapter = nullptr;
                        ad->Release();
                        if (SUCCEEDED(descHr) &&
                            desc.AdapterLuid.HighPart == devLuid.HighPart &&
                            desc.AdapterLuid.LowPart  == devLuid.LowPart) {
                            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1,
                                gpuName, MAX_PATH, nullptr, nullptr);
                            break;
                        }
                    }
                }
            } catch (...) {
                strncpy_s(gpuName, "<unknown GPU (DXGI probe failed)>", sizeof(gpuName) - 1);
            }
        }

        Logging::always("FSR version: %s  |  GPU: %s", fsrVer, gpuName);
    }
};

// -----------------------------------------------------------------------
// Implementation
// -----------------------------------------------------------------------
static float computeFrameTimeDeltaMs(LARGE_INTEGER& lastEvalQpc, const LARGE_INTEGER& qpcFreq) {
    constexpr float kDefaultFrameTimeMs = 16.6667f;
    constexpr float kMinFrameTimeMs     = 0.1f;
    constexpr float kMaxFrameTimeMs     = 250.0f;

    LARGE_INTEGER now = {};
    if (!QueryPerformanceCounter(&now)) {
        return kDefaultFrameTimeMs;
    }

    if (lastEvalQpc.QuadPart == 0 || qpcFreq.QuadPart <= 0 || now.QuadPart <= lastEvalQpc.QuadPart) {
        lastEvalQpc = now;
        return kDefaultFrameTimeMs;
    }

    const double elapsedMs = static_cast<double>(now.QuadPart - lastEvalQpc.QuadPart) * 1000.0 /
                             static_cast<double>(qpcFreq.QuadPart);
    lastEvalQpc = now;

    if (elapsedMs < static_cast<double>(kMinFrameTimeMs)) {
        return kMinFrameTimeMs;
    }
    if (elapsedMs > static_cast<double>(kMaxFrameTimeMs)) {
        return kMaxFrameTimeMs;
    }
    return static_cast<float>(elapsedMs);
}

FSR4Backend::FSR4Backend() {
    m_impl = new Impl();
    QueryPerformanceFrequency(&m_impl->qpcFreq);
}

FSR4Backend::~FSR4Backend() {
    if (m_impl) {
        {
            SrwExclusiveGuard lockGuard(&m_impl->lock);
            Impl::cleanupSetupState(m_impl);
        }
        delete m_impl;
        m_impl = nullptr;
    }
}

static void messageCallback(uint32_t type, const wchar_t* message) {
    if (!message) return;
    if (type == 0)
        Logging::error("FSR4: %ls", message);
    else
        Logging::warn("FSR4: %ls", message);
}

bool FSR4Backend::setup(void* deviceOrQueue, int graphicsAPI) {
    if (!m_impl) return false;
    SrwExclusiveGuard lockGuard(&m_impl->lock);
    // Guard the entire init path (including cleanupSetupState teardown).
    // REFramework calls this from a __stdcall export during on_initialize();
    // any C++ exception or SEH (access violation) inside the D3D12/DXGI/FFX
    // calls must NOT escape into REFramework or the game dies. /EHa (set in
    // CMakeLists.txt) makes catch(...) trap SEH too. On native Windows nothing
    // here throws; under Wine/Proton a virtualized DXGI or an AV inside the
    // AMD loader is caught and FSR4 cleanly disables instead of crashing.
    // (Matches the try/catch already around evaluate().)
    try {
        Impl::cleanupSetupState(m_impl);
        Logging::info("FSR4Backend::setup(graphicsAPI=%d)", graphicsAPI);

        if (graphicsAPI != 1) {
            Logging::warn("FSR4Backend: Unsupported graphicsAPI=%d (D3D12 required)", graphicsAPI);
            return false;
        }
        if (!deviceOrQueue) {
            Logging::error("FSR4Backend: setup() called with null deviceOrQueue");
            return false;
        }

        m_impl->graphicsAPI = graphicsAPI;
        QueryPerformanceFrequency(&m_impl->qpcFreq);
        readBackendIni();

        ID3D12CommandQueue* queue = (ID3D12CommandQueue*)deviceOrQueue;
        if (FAILED(queue->GetDevice(IID_PPV_ARGS(&m_impl->device))) || !m_impl->device) {
            Logging::error("FSR4Backend: Failed to get D3D12 device from queue");
            Impl::cleanupSetupState(m_impl);
            return false;
        }
        Logging::info("FSR4Backend: Got D3D12 device");
        m_impl->commandQueue = queue;
        m_impl->commandQueue->AddRef();

        const std::wstring dllDir = Logging::getDllDir();
        const wchar_t* loaderNames[] = {
            L"amd_fidelityfx_loader_dx12.dll",
            L"amd_fidelityfx_loader.dll"
        };
        m_impl->hLoaderDll = nullptr;
        if (!dllDir.empty()) {
            for (const wchar_t* name : loaderNames) {
                const std::wstring fullPath = Logging::getDllSiblingPath(name);
                if (fullPath.empty()) {
                    continue;
                }
                Logging::info("FSR4Backend: Probing %ls...", fullPath.c_str());
                m_impl->hLoaderDll = LoadLibraryExW(fullPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
                if (m_impl->hLoaderDll) {
                    Logging::info("FSR4Backend: Loaded %ls", fullPath.c_str());
                    break;
                }
                Logging::warn("FSR4Backend: Probe failed for %ls (err=%lu)", fullPath.c_str(), GetLastError());
            }
        }
        if (!m_impl->hLoaderDll) {
            for (const wchar_t* name : loaderNames) {
                Logging::info("FSR4Backend: Loading %ls...", name);
                m_impl->hLoaderDll = LoadLibraryW(name);
                if (m_impl->hLoaderDll) {
                    Logging::info("FSR4Backend: Loaded %ls", name);
                    break;
                }
                Logging::warn("FSR4Backend: Failed to load %ls (err=%lu)", name, GetLastError());
            }
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
            Impl::cleanupSetupState(m_impl);
            return false;
        }

        m_impl->ffxCreateContextFn  = (PFN_ffxCreateContext)GetProcAddress(m_impl->hLoaderDll, "ffxCreateContext");
        m_impl->ffxDestroyContextFn = (PFN_ffxDestroyContext)GetProcAddress(m_impl->hLoaderDll, "ffxDestroyContext");
        m_impl->ffxDispatchFn       = (PFN_ffxDispatch)GetProcAddress(m_impl->hLoaderDll, "ffxDispatch");
        m_impl->ffxQueryFn          = (PFN_ffxQuery)GetProcAddress(m_impl->hLoaderDll, "ffxQuery");

        if (!m_impl->ffxCreateContextFn || !m_impl->ffxDestroyContextFn || !m_impl->ffxDispatchFn) {
            Logging::warn("FSR4Backend: Missing required ffxApi entry points");
            Impl::cleanupSetupState(m_impl);
            return false;
        }

        Logging::info("FSR4Backend: ffxApi loaded successfully");

        // Create 3-slot internal command allocator ring and list for when REFramework doesn't provide one
        m_impl->currentSlot = 0;
        m_impl->fenceValue  = 0;
        m_impl->cmdListOpen = false;
        for (size_t i = 0; i < Impl::kNumCmdSlots; ++i) {
            m_impl->slotFenceValues[i] = 0;
            HRESULT allocHr = m_impl->device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_impl->cmdAllocs[i]));
            if (FAILED(allocHr) || !m_impl->cmdAllocs[i]) {
                Logging::error("FSR4Backend: Failed to create command allocator[%zu] (hr=%08x)", i, allocHr);
                Impl::cleanupSetupState(m_impl);
                return false;
            }
        }

        HRESULT hr = m_impl->device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_impl->cmdAllocs[0], nullptr, IID_PPV_ARGS(&m_impl->cmdList));
        if (FAILED(hr) || !m_impl->cmdList) {
            Logging::error("FSR4Backend: Failed to create command list (hr=%08x)", hr);
            Impl::cleanupSetupState(m_impl);
            return false;
        }

        hr = m_impl->cmdList->Close(); // Reset() requires the list to be closed
        if (FAILED(hr)) {
            Logging::error("FSR4Backend: Failed to close initial command list (hr=%08x)", hr);
            Impl::cleanupSetupState(m_impl);
            return false;
        }
        m_impl->cmdListOpen = false;

        hr = m_impl->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_impl->fence));
        if (FAILED(hr) || !m_impl->fence) {
            Logging::error("FSR4Backend: Failed to create fence (hr=%08x)", hr);
            Impl::cleanupSetupState(m_impl);
            return false;
        }

        m_impl->fenceEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        if (!m_impl->fenceEvent) {
            Logging::error("FSR4Backend: Failed to create fence event (err=%lu)", GetLastError());
            Impl::cleanupSetupState(m_impl);
            return false;
        }

        // Enumerate the FSR versions this GPU actually supports and remember the
        // newest one, so every context we create runs a version the GPU can do
        // (FSR4 4.1.1 if it can, else FSR3.1, ...). Without this, requesting 4.1.1
        // on an unsupported GPU makes ffxCreateContext fail -> no upscaling.
        if (!Impl::selectFsrVersionLocked(m_impl)) {
            Logging::error("FSR4Backend: Failed to enumerate any supported FSR version");
            Impl::cleanupSetupState(m_impl);
            return false;
        }

        // FSR version + GPU: printed ONCE and in BOTH verbosity modes (gated on
        // the preset actually loading, so we never spam before init succeeds).
        Impl::logSystemInfoLocked(m_impl);

        m_impl->available = true;
        return true;
    } catch (const std::exception& e) {
        Logging::error("=============================================================");
        Logging::error("FSR4Backend: setup() threw: %s", e.what());
        Logging::error("FSR4Backend disabled this run; the game will use its native upscaler.");
        Logging::error("=============================================================");
        Impl::cleanupSetupState(m_impl);
        return false;
    } catch (...) {
        Logging::error("=============================================================");
        Logging::error("FSR4Backend: setup() threw an unknown exception (SEH/AV?).");
        Logging::error("FSR4 disabled this run; the game will use its native upscaler.");
        Logging::error("=============================================================");
        Impl::cleanupSetupState(m_impl);
        return false;
    }
}

// Enumerate the FSR versions THIS GPU supports and store the newest (highest
// id) so we always request a version the device can run. The SDK's
// ffxQueryDescGetVersions sorts newest-first into the arrays, so index 0 is
// our pick. Returns false if the query fn is missing or no version is found
// (caller then has no FSR at all).
bool FSR4Backend::selectFsrVersion() {
    if (!m_impl) return false;
    SrwExclusiveGuard lockGuard(&m_impl->lock);
    try {
        return Impl::selectFsrVersionLocked(m_impl);
    } catch (const std::exception& e) {
        Logging::error("FSR4Backend: selectFsrVersion() threw: %s", e.what());
        return false;
    } catch (...) {
        Logging::error("FSR4Backend: selectFsrVersion() threw an unknown exception (SEH/AV?)");
        return false;
    }
}

// Decode FFX version encoding: FFX_MAKE_VERSION(major,minor,patch) =
// (major << 22) | (minor << 12) | patch. Used only as a human-readable
// fallback if the SDK version-name string isn't available.
static void decodeFFXVersion(uint32_t v, char* out, size_t outLen) {
    uint32_t major = (v >> 22) & 0x3FF;
    uint32_t minor = (v >> 12) & 0x3FF;
    uint32_t patch = v & 0xFFF;
    sprintf_s(out, outLen, "%u.%u.%u", major, minor, patch);
}

// FSR version + GPU name, printed ONCE in BOTH verbosity modes (gated on the
// preset actually loading, so we never spam before init succeeds).
void FSR4Backend::logSystemInfo() {
    if (!m_impl) return;
    SrwSharedGuard lockGuard(&m_impl->lock);
    Impl::logSystemInfoLocked(m_impl);
}

bool FSR4Backend::isAvailable() const {
    if (!m_impl) return false;
    SrwSharedGuard lockGuard(&m_impl->lock);
    return m_impl->available;
}

bool FSR4Backend::isReady() const {
    if (!m_impl) return false;
    SrwSharedGuard lockGuard(&m_impl->lock);
    return m_impl->available && !m_impl->contexts.empty();
}

static uint32_t dxgiToFfxFormat(DXGI_FORMAT fmt) {
    switch (fmt) {
        case DXGI_FORMAT_UNKNOWN:                  return FFX_API_SURFACE_FORMAT_UNKNOWN;
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:    return FFX_API_SURFACE_FORMAT_R32G32B32A32_TYPELESS;
        case DXGI_FORMAT_R32G32B32A32_UINT:        return FFX_API_SURFACE_FORMAT_R32G32B32A32_UINT;
        case DXGI_FORMAT_R32G32B32A32_FLOAT:       return FFX_API_SURFACE_FORMAT_R32G32B32A32_FLOAT;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:       return FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R32G32B32_FLOAT:          return FFX_API_SURFACE_FORMAT_R32G32B32_FLOAT;
        case DXGI_FORMAT_R32G32_FLOAT:             return FFX_API_SURFACE_FORMAT_R32G32_FLOAT;
        case DXGI_FORMAT_R8_UINT:                  return FFX_API_SURFACE_FORMAT_R8_UINT;
        case DXGI_FORMAT_R32_UINT:                 return FFX_API_SURFACE_FORMAT_R32_UINT;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:        return FFX_API_SURFACE_FORMAT_R8G8B8A8_TYPELESS;
        case DXGI_FORMAT_R8G8B8A8_UNORM:           return FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_R8G8B8A8_SNORM:           return FFX_API_SURFACE_FORMAT_R8G8B8A8_SNORM;
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:      return FFX_API_SURFACE_FORMAT_R8G8B8A8_SRGB;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8X8_TYPELESS:        return FFX_API_SURFACE_FORMAT_B8G8R8A8_TYPELESS;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_UNORM:           return FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:      return FFX_API_SURFACE_FORMAT_B8G8R8A8_SRGB;
        case DXGI_FORMAT_R11G11B10_FLOAT:          return FFX_API_SURFACE_FORMAT_R11G11B10_FLOAT;
        case DXGI_FORMAT_R10G10B10A2_UNORM:        return FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16_FLOAT:             return FFX_API_SURFACE_FORMAT_R16G16_FLOAT;
        case DXGI_FORMAT_R16G16_UINT:              return FFX_API_SURFACE_FORMAT_R16G16_UINT;
        case DXGI_FORMAT_R16G16_SINT:              return FFX_API_SURFACE_FORMAT_R16G16_SINT;
        case DXGI_FORMAT_R16_FLOAT:                return FFX_API_SURFACE_FORMAT_R16_FLOAT;
        case DXGI_FORMAT_R16_UINT:                 return FFX_API_SURFACE_FORMAT_R16_UINT;
        case DXGI_FORMAT_R16_UNORM:
        case DXGI_FORMAT_D16_UNORM:                return FFX_API_SURFACE_FORMAT_R16_UNORM;
        case DXGI_FORMAT_R16_SNORM:                return FFX_API_SURFACE_FORMAT_R16_SNORM;
        case DXGI_FORMAT_R8_UNORM:                 return FFX_API_SURFACE_FORMAT_R8_UNORM;
        case DXGI_FORMAT_R8G8_UNORM:               return FFX_API_SURFACE_FORMAT_R8G8_UNORM;
        case DXGI_FORMAT_R8G8_UINT:                return FFX_API_SURFACE_FORMAT_R8G8_UINT;
        case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R32G8X24_TYPELESS:        return FFX_API_SURFACE_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:       return FFX_API_SURFACE_FORMAT_R9G9B9E5_SHAREDEXP;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:    return FFX_API_SURFACE_FORMAT_R16G16B16A16_TYPELESS;
        case DXGI_FORMAT_R32G32_TYPELESS:          return FFX_API_SURFACE_FORMAT_R32G32_TYPELESS;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:     return FFX_API_SURFACE_FORMAT_R10G10B10A2_TYPELESS;
        case DXGI_FORMAT_R16G16_TYPELESS:          return FFX_API_SURFACE_FORMAT_R16G16_TYPELESS;
        case DXGI_FORMAT_R16_TYPELESS:             return FFX_API_SURFACE_FORMAT_R16_TYPELESS;
        case DXGI_FORMAT_R8_TYPELESS:              return FFX_API_SURFACE_FORMAT_R8_TYPELESS;
        case DXGI_FORMAT_R8G8_TYPELESS:            return FFX_API_SURFACE_FORMAT_R8G8_TYPELESS;
        case DXGI_FORMAT_R32_TYPELESS:             return FFX_API_SURFACE_FORMAT_R32_TYPELESS;
        case DXGI_FORMAT_R32G32_UINT:              return FFX_API_SURFACE_FORMAT_R32G32_UINT;
        case DXGI_FORMAT_R8_SNORM:                 return FFX_API_SURFACE_FORMAT_R8_SNORM;
        default:                                   return FFX_API_SURFACE_FORMAT_UNKNOWN;
    }
}

static uint32_t d3d12DimensionToFfxType(const D3D12_RESOURCE_DESC& d) {
    switch (d.Dimension) {
        case D3D12_RESOURCE_DIMENSION_BUFFER:    return FFX_API_RESOURCE_TYPE_BUFFER;
        case D3D12_RESOURCE_DIMENSION_TEXTURE1D: return FFX_API_RESOURCE_TYPE_TEXTURE1D;
        case D3D12_RESOURCE_DIMENSION_TEXTURE2D:
            return (d.DepthOrArraySize == 6) ? FFX_API_RESOURCE_TYPE_TEXTURE_CUBE
                                             : FFX_API_RESOURCE_TYPE_TEXTURE2D;
        case D3D12_RESOURCE_DIMENSION_TEXTURE3D: return FFX_API_RESOURCE_TYPE_TEXTURE3D;
        default:                                 return FFX_API_RESOURCE_TYPE_TEXTURE2D;
    }
}

static uint32_t d3d12FlagsToFfxUsage(const D3D12_RESOURCE_DESC& d, uint32_t additionalUsage) {
    uint32_t usage = additionalUsage & FFX_API_RESOURCE_USAGE_VALID_MASK;
    if (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)
        usage |= FFX_API_RESOURCE_USAGE_RENDERTARGET;
    if (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
        usage |= FFX_API_RESOURCE_USAGE_DEPTHTARGET;
    if (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
        usage |= FFX_API_RESOURCE_USAGE_UAV;
    const bool isCubemap = (d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.DepthOrArraySize == 6);
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER &&
        d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D &&
        d.DepthOrArraySize > 1 && !isCubemap) {
        usage |= FFX_API_RESOURCE_USAGE_ARRAYVIEW;
    }
    return usage;
}

static FfxApiResource makeResource(ID3D12Resource* res, uint32_t state, uint32_t usage) {
    FfxApiResource r = {};
    r.resource = res;
    r.state = state;
    if (res) {
        D3D12_RESOURCE_DESC d = res->GetDesc();
        r.description.type   = d3d12DimensionToFfxType(d);
        r.description.format = dxgiToFfxFormat(d.Format);
        r.description.flags  = 0;
        r.description.usage  = d3d12FlagsToFfxUsage(d, usage);
        if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            r.description.width    = static_cast<uint32_t>(d.Width);
            r.description.height   = 0;
            r.description.depth    = 0;
            r.description.mipCount = 0;
        } else {
            r.description.width    = static_cast<uint32_t>(d.Width);
            r.description.height   = d.Height;
            r.description.depth    = d.DepthOrArraySize;
            r.description.mipCount = d.MipLevels;
        }
    }
    return r;
}

void* FSR4Backend::createContext(int id, int upscaleMethod, int qualityLevel,
                                  int displaySizeX, int displaySizeY,
                                  bool isContentHDR, bool depthInverted, bool yAxisInverted,
                                  bool motionVectorsJittered, bool enableSharpening,
                                  bool enableAutoExposure, int format) {
    if (!m_impl) return nullptr;
    SrwExclusiveGuard lockGuard(&m_impl->lock);
    UpscaleContext ctx{};
    bool contextInserted = false;
    try {
        Logging::info("FSR4Backend::createContext(id=%d method=%d qual=%d %dx%d fmt=%d)",
                      id, upscaleMethod, qualityLevel, displaySizeX, displaySizeY, format);

        if (!m_impl->available || !m_impl->device) {
            Logging::error("FSR4Backend: not available");
            return nullptr;
        }
        if (displaySizeX <= 0 || displaySizeY <= 0) {
            Logging::error("FSR4Backend: invalid display size %dx%d", displaySizeX, displaySizeY);
            return nullptr;
        }

        auto existing = m_impl->contexts.find(id);
        if (existing != m_impl->contexts.end()) {
            Impl::closeInternalCmdListIfOpen(m_impl);
            Impl::waitForGpu(m_impl);
            Impl::destroyContextResources(m_impl, existing->second);
            m_impl->contexts.erase(existing);
        }

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
            case 3: scale = 1.0f; break; // DLAA / Native resolution
            default: scale = 1.5f; break;
        }
        if (qualityLevel == 3) {
            ctx.renderWidth  = displaySizeX;
            ctx.renderHeight = displaySizeY;
        } else {
            ctx.renderWidth  = (int)(displaySizeX / scale + 0.5f);
            ctx.renderHeight = (int)(displaySizeY / scale + 0.5f);
        }
        if (ctx.renderWidth < 1) ctx.renderWidth = 1;
        if (ctx.renderHeight < 1) ctx.renderHeight = 1;

        Logging::info("FSR4Backend: render=%dx%d upscale=%dx%d scale=%.2f",
                      ctx.renderWidth, ctx.renderHeight, displaySizeX, displaySizeY, scale);

        uint32_t flags = FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION;
        if (isContentHDR)          flags |= FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE;
        if (depthInverted)         flags |= FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
        if (motionVectorsJittered) flags |= FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION;
        if (enableAutoExposure)    flags |= FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;

        Logging::info("FSR4Backend: flags=0x%x HDR=%d depthInv=%d mvJitter=%d autoExp=%d sharp=%d",
                      flags, isContentHDR, depthInverted, motionVectorsJittered,
                      enableAutoExposure, enableSharpening);

        if (!m_impl->ffxCreateContextFn) {
            Logging::error("FSR4Backend: missing ffxCreateContext function");
            return nullptr;
        }

        // pNext chain: upscaleDesc -> [overrideVersion ->] backendDesc
        // Try all enumerated candidateVersions sequentially (newest first) so
        // if FSR4 context creation fails on a given GPU/driver/payload setup,
        // we fall back cleanly to FSR3.1 instead of aborting upscaling.
        ffxCreateBackendDX12Desc backendDesc = {};
        backendDesc.header.type  = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        backendDesc.header.pNext = nullptr;
        backendDesc.device       = m_impl->device;

        ffxCreateContextDescUpscale upscaleDesc = {};
        upscaleDesc.header.type             = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        upscaleDesc.header.pNext            = &backendDesc.header;
        upscaleDesc.flags                   = flags;
        upscaleDesc.maxRenderSize.width     = static_cast<uint32_t>(displaySizeX);
        upscaleDesc.maxRenderSize.height    = static_cast<uint32_t>(displaySizeY);
        upscaleDesc.maxUpscaleSize.width    = static_cast<uint32_t>(displaySizeX);
        upscaleDesc.maxUpscaleSize.height   = static_cast<uint32_t>(displaySizeY);
        upscaleDesc.fpMessage               = messageCallback;

        Logging::info("FSR4Backend: Calling ffxCreateContext (candidates=%zu)...",
                      m_impl->candidateVersions.size());

        ffxContext ffxCtx = nullptr;
        if (!m_impl->candidateVersions.empty()) {
            for (size_t i = 0; i < m_impl->candidateVersions.size(); ++i) {
                const auto& candidate = m_impl->candidateVersions[i];
                ffxOverrideVersion overrideDesc = {};
                overrideDesc.header.type  = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
                overrideDesc.header.pNext = &backendDesc.header;
                overrideDesc.versionId    = candidate.id;
                upscaleDesc.header.pNext  = &overrideDesc.header;

                ffxContext candidateCtx = nullptr;
                ffxReturnCode_t ret = 0;
                bool threw = false;
                try {
                    ret = m_impl->ffxCreateContextFn(&candidateCtx, &upscaleDesc.header, nullptr);
                } catch (const std::exception& e) {
                    threw = true;
                    Logging::warn("FSR4Backend: ffxCreateContext candidate[%zu] '%s' (id=0x%016llx) threw: %s",
                                  i, candidate.name.c_str(), (unsigned long long)candidate.id, e.what());
                } catch (...) {
                    threw = true;
                    Logging::warn("FSR4Backend: ffxCreateContext candidate[%zu] '%s' (id=0x%016llx) threw unknown exception (SEH/AV?)",
                                  i, candidate.name.c_str(), (unsigned long long)candidate.id);
                }

                if (!threw && ret == 0 && candidateCtx != nullptr) {
                    ffxCtx = candidateCtx;
                    m_impl->chosenVersionId = candidate.id;
                    strncpy_s(m_impl->chosenVersionName, candidate.name.c_str(),
                              sizeof(m_impl->chosenVersionName) - 1);
                    if (i > 0) {
                        Logging::always("FSR4Backend: primary FSR version failed; context initialized on fallback version %s (id=0x%016llx)",
                                        m_impl->chosenVersionName, (unsigned long long)candidate.id);
                    }
                    break;
                }

                if (candidateCtx && m_impl->ffxDestroyContextFn) {
                    try {
                        m_impl->ffxDestroyContextFn(&candidateCtx, nullptr);
                    } catch (...) {}
                }
                if (!threw) {
                    Logging::warn("FSR4Backend: ffxCreateContext candidate[%zu] '%s' (id=0x%016llx) failed (ret=%u ctx=%p)",
                                  i, candidate.name.c_str(), (unsigned long long)candidate.id, ret, candidateCtx);
                }
            }
        } else {
            upscaleDesc.header.pNext = &backendDesc.header;
            ffxContext candidateCtx = nullptr;
            ffxReturnCode_t ret = 0;
            bool threw = false;
            try {
                ret = m_impl->ffxCreateContextFn(&candidateCtx, &upscaleDesc.header, nullptr);
            } catch (const std::exception& e) {
                threw = true;
                Logging::error("FSR4Backend: ffxCreateContext threw: %s", e.what());
            } catch (...) {
                threw = true;
                Logging::error("FSR4Backend: ffxCreateContext threw an unknown exception (SEH/AV?)");
            }
            if (!threw) {
                Logging::info("FSR4Backend: ffxCreateContext returned ret=%u ctx=%p", ret, candidateCtx);
                if (ret == 0 && candidateCtx != nullptr) {
                    ffxCtx = candidateCtx;
                } else if (candidateCtx && m_impl->ffxDestroyContextFn) {
                    try {
                        m_impl->ffxDestroyContextFn(&candidateCtx, nullptr);
                    } catch (...) {}
                }
            }
        }

        if (ffxCtx == nullptr) {
            // ffxCreateContext failed for every enumerated candidate version -> upscaler
            // can't run -> PDPerfPlugin allocates the fallback texture instead.
            Logging::error("=============================================================");
            Logging::error("FSR4Backend: ffxCreateContext failed for all %zu candidate version(s). The FSR4",
                           m_impl->candidateVersions.size());
            Logging::error("payload files required by amd_fidelityfx_loader_dx12.dll are missing or");
            Logging::error("the loader version is wrong. Verify the FSR4 files match FFX version");
            Logging::error("0x%x. FSR4 will NOT run (black screen).", (unsigned)FFX_UPSCALER_VERSION);
            Logging::error("=============================================================");
            return nullptr;
        }

        Logging::info("FSR4Backend: ffxCreateContext SUCCEEDED");
        ctx.ffxCtx = ffxCtx;

        // Truth check: ask the context which version it actually initialized at.
        // Normally matches chosenVersionName; if it diverges (driver quirk, etc.)
        // log the real running version so the user isn't misled. The name pointer
        // lives in loader memory, so copy it.
        if (m_impl->ffxQueryFn) {
            ffxQueryGetProviderVersion pv = {};
            pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
            if (m_impl->ffxQueryFn(&ffxCtx, &pv.header) == 0 && pv.versionName) {
                if (m_impl->chosenVersionName[0] == '\0' ||
                    strncmp(m_impl->chosenVersionName, pv.versionName, sizeof(m_impl->chosenVersionName) - 1) != 0) {
                    strncpy_s(m_impl->chosenVersionName, pv.versionName, sizeof(m_impl->chosenVersionName) - 1);
                    Logging::always("FSR running version: %s (id=0x%016llx)",
                                    m_impl->chosenVersionName, (unsigned long long)pv.versionId);
                }
            }
        }

        // Create output texture
        D3D12_RESOURCE_DESC texDesc = {};
        texDesc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width              = (UINT64)displaySizeX;
        texDesc.Height             = (UINT)displaySizeY;
        texDesc.DepthOrArraySize   = 1;
        texDesc.MipLevels          = 1;
        // Output texture format.
        //   * Native Windows: use REFramework's requested backbuffer format as-is.
        //   * Linux/Proton SDR: default to DXGI_FORMAT_R8G8B8A8_UNORM (28), which
        //     maps cleanly to FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM (10), supports
        //     UAV allocation everywhere, and matches the SRV format used by rb_swap.
        //   * Linux/Proton HDR: preserve R10G10B10A2_UNORM / R16G16B16A16_FLOAT.
        //   * [Backend] OutputFormat (if non-zero) overrides the output format,
        //     with automatic retry to R8G8B8A8_UNORM if UAV creation fails.
        uint32_t outFmt;
        const char* how;
        if (isRunningUnderWine()) {
            const bool backbufferHDR =
                (format == (uint32_t)DXGI_FORMAT_R10G10B10A2_UNORM) ||
                (format == (uint32_t)DXGI_FORMAT_R16G16B16A16_FLOAT);
            if (backbufferHDR) {
                outFmt = (uint32_t)format;            // keep HDR container
                how = " (auto: Wine/Proton HDR)";
            } else {
                outFmt = static_cast<uint32_t>(DXGI_FORMAT_R8G8B8A8_UNORM);
                how = " (auto: Wine/Proton SDR R8G8B8A8_UNORM)";
            }
        } else {
            outFmt = (uint32_t)format;                // Windows - backbuffer fmt
            how = " (auto: Windows backbuffer)";
        }
        if (s_outputFormatOverride != 0) {
            outFmt = (uint32_t)s_outputFormatOverride;
            how = " (INI override)";
        }
        Logging::info("FSR4Backend: output format backbuffer=%d -> using=%d%s",
                      (int)format, (int)outFmt, how);
        texDesc.Format             = (DXGI_FORMAT)outFmt;
        texDesc.SampleDesc.Count   = 1;
        texDesc.SampleDesc.Quality = 0;
        texDesc.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        texDesc.Flags              = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        ID3D12Resource* outputTexture = nullptr;
        HRESULT hr = m_impl->device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
            &texDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&outputTexture));
        if ((FAILED(hr) || !outputTexture) && texDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM) {
            Logging::warn("FSR4Backend: output texture format=%d failed hr=%08x, retrying R8G8B8A8_UNORM",
                          (int)texDesc.Format, hr);
            if (outputTexture) {
                outputTexture->Release();
                outputTexture = nullptr;
            }
            texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            outFmt = static_cast<uint32_t>(DXGI_FORMAT_R8G8B8A8_UNORM);
            hr = m_impl->device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
                &texDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&outputTexture));
        }
        if (FAILED(hr) || !outputTexture) {
            Logging::error("FSR4Backend: Failed to create output texture hr=%08x", hr);
            Impl::destroyContextResources(m_impl, ctx);
            return nullptr;
        }

        ctx.outputTexture = outputTexture;
        ctx.format        = static_cast<int>(texDesc.Format);
        ctx.rbSwapEnabled = false;

        // Linux/Proton red<->blue correction, gated by [Backend] RBSwap (-1 auto,
        // 0 force off, 1 force on) and stored per UpscaleContext.
        bool wantRbSwap = false;
        if (s_rbSwapMode == 0) {
            wantRbSwap = false;
        } else if (s_rbSwapMode == 1) {
            wantRbSwap = true;
        } else {
            wantRbSwap = isRunningUnderWine() &&
                (texDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                 texDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                 texDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                 texDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
        }

        void* returnTexture = outputTexture;
        if (wantRbSwap) {
            const DXGI_FORMAT srvFmt = RbSwapResources::resolveSrvFormat(texDesc.Format);
            const DXGI_FORMAT uavFmt = RbSwapResources::resolveUavFormat(texDesc.Format);
            if (m_impl->rbSwapPipeline.ensure(m_impl->device) &&
                ctx.rbSwap.create(m_impl->device, ctx.outputTexture,
                                  static_cast<uint32_t>(displaySizeX),
                                  static_cast<uint32_t>(displaySizeY),
                                  srvFmt, uavFmt)) {
                ctx.rbSwapEnabled = true;
                returnTexture = ctx.rbSwap.swapTexture;
                Logging::info("FSR4Backend: R/B swap pass ENABLED (id=%d outFmt=%d -> swapFmt=%d mode=%d)",
                              id, ctx.format, static_cast<int>(uavFmt), s_rbSwapMode);
            } else {
                Logging::warn("FSR4Backend: R/B swap setup FAILED (id=%d); returning raw output (colors may swap)", id);
            }
        }

        m_impl->contexts[id] = ctx;
        contextInserted = true;

        // REFramework reads the returned texture as m_upscaled_textures[id-1] and
        // raw-copies it to the backbuffer. We return the swap texture (BGRA-ordered
        // bytes) on Linux, or the FSR output on Windows.
        Logging::info("FSR4Backend: context id=%d ready, texture=%p (swap=%d)",
                      id, returnTexture, (int)(returnTexture != outputTexture));
        return returnTexture;
    } catch (const std::exception& e) {
        if (!contextInserted) {
            Impl::destroyContextResources(m_impl, ctx);
        }
        Logging::error("FSR4Backend: createContext threw: %s", e.what());
        return nullptr;
    } catch (...) {
        if (!contextInserted) {
            Impl::destroyContextResources(m_impl, ctx);
        }
        Logging::error("FSR4Backend: createContext threw an unknown exception (SEH/AV?)");
        return nullptr;
    }
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
    if (!m_impl) return;
    SrwExclusiveGuard lockGuard(&m_impl->lock);
    try {
        auto it = m_impl->contexts.find(id);
        if (it == m_impl->contexts.end() || !it->second.ffxCtx || !m_impl->ffxDispatchFn) {
            if (!cmdList) {
                Impl::closeInternalCmdListIfOpen(m_impl);
            }
            static std::atomic<long long> s_last{0};
            const long long now = static_cast<long long>(GetTickCount64());
            long long prev = s_last.load(std::memory_order_relaxed);
            if (now - prev > 2000 && s_last.compare_exchange_strong(prev, now, std::memory_order_acq_rel)) {
                Logging::warn("FSR4Backend::evaluate(id=%d): no context/dispatch fn (map size=%zu, ffxDispatch=%p)",
                              id, m_impl->contexts.size(), (void*)m_impl->ffxDispatchFn);
            }
            return;
        }

        auto& ctx = it->second;

        void* effectiveCmdList = cmdList;
        void* effectiveDst = destination ? destination : ctx.outputTexture;

        if (!color || !depth || !motionVector || !effectiveDst) {
            if (!cmdList) {
                Impl::closeInternalCmdListIfOpen(m_impl);
            }
            static std::atomic<long long> s_lastInvalid{0};
            const long long now = static_cast<long long>(GetTickCount64());
            long long prev = s_lastInvalid.load(std::memory_order_relaxed);
            if (now - prev > 2000 && s_lastInvalid.compare_exchange_strong(prev, now, std::memory_order_acq_rel)) {
                Logging::warn("FSR4Backend::evaluate(id=%d): invalid inputs (color=%p depth=%p mv=%p dst=%p)",
                              id, color, depth, motionVector, effectiveDst);
            }
            return;
        }

        if (!cmdList) {
            ID3D12CommandAllocator* slotAlloc = m_impl->cmdAllocs[m_impl->currentSlot];
            if (!slotAlloc || !m_impl->cmdList) {
                Impl::closeInternalCmdListIfOpen(m_impl);
                return;
            }

            if (!m_impl->cmdListOpen) {
                const uint64_t waitVal = m_impl->slotFenceValues[m_impl->currentSlot];
                if (!Impl::waitForFenceValue(m_impl, waitVal)) {
                    Logging::error("FSR4Backend::evaluate(id=%d): waitForFenceValue(%llu) failed on slot %zu",
                                   id, (unsigned long long)waitVal, m_impl->currentSlot);
                    return;
                }

                const HRESULT allocResetHr = slotAlloc->Reset();
                if (FAILED(allocResetHr)) {
                    Logging::error("FSR4Backend::evaluate(id=%d): cmdAllocs[%zu]->Reset() failed hr=%08x",
                                   id, m_impl->currentSlot, allocResetHr);
                    return;
                }
                const HRESULT listResetHr = m_impl->cmdList->Reset(slotAlloc, nullptr);
                if (FAILED(listResetHr)) {
                    Logging::error("FSR4Backend::evaluate(id=%d): cmdList->Reset() failed hr=%08x",
                                   id, listResetHr);
                    return;
                }
                m_impl->cmdListOpen = true;
            }
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
        dd.color          = makeResource((ID3D12Resource*)color, FFX_RESOURCE_STATE_COMMON, FFX_API_RESOURCE_USAGE_READ_ONLY);
        dd.depth          = makeResource((ID3D12Resource*)depth, FFX_RESOURCE_STATE_COMMON, FFX_API_RESOURCE_USAGE_READ_ONLY);
        dd.motionVectors  = makeResource((ID3D12Resource*)motionVector, FFX_RESOURCE_STATE_COMMON, FFX_API_RESOURCE_USAGE_READ_ONLY);
        dd.exposure       = makeResource(nullptr, 0, 0);
        dd.reactive       = makeResource(nullptr, 0, 0);
        dd.transparencyAndComposition = makeResource(nullptr, 0, 0);
        // Output is written by FSR4 as a UAV. REFramework's copier then copies it
        // from UAV -> backbuffer (PRESENT), so it must be left in UAV state.
        dd.output         = makeResource((ID3D12Resource*)effectiveDst, FFX_RESOURCE_STATE_UNORDERED_ACCESS, FFX_API_RESOURCE_USAGE_UAV);

        int effectiveRenderW = (renderSizeX > 0) ? renderSizeX : ctx.renderWidth;
        int effectiveRenderH = (renderSizeY > 0) ? renderSizeY : ctx.renderHeight;
        if (effectiveRenderW > ctx.displaySizeX) effectiveRenderW = ctx.displaySizeX;
        if (effectiveRenderH > ctx.displaySizeY) effectiveRenderH = ctx.displaySizeY;
        if (effectiveRenderW < 1) effectiveRenderW = 1;
        if (effectiveRenderH < 1) effectiveRenderH = 1;

        dd.jitterOffset.x          = jitterOffsetX;
        dd.jitterOffset.y          = jitterOffsetY;
        dd.motionVectorScale.x     = ctx.motionScaleX;
        dd.motionVectorScale.y     = ctx.motionScaleY;
        dd.renderSize.width        = static_cast<uint32_t>(effectiveRenderW);
        dd.renderSize.height       = static_cast<uint32_t>(effectiveRenderH);
        dd.upscaleSize.width       = (uint32_t)ctx.displaySizeX;
        dd.upscaleSize.height      = (uint32_t)ctx.displaySizeY;
        dd.enableSharpening        = ctx.enableSharpening;
        // REFramework's "Sharpness Amount" slider is authored for FSR3 and ranges
        // 0.0..5.0, defaulting to 0.0. The FFX upscale API (ffx_upscale.h) defines
        // `sharpness` as 0..1 (1 = max RCAS). We LINEARLY normalize the whole
        // 0..5 slider into 0..1 (s /= 5.0 for any value above 0) so the knob tracks
        // RCAS evenly end-to-end. 0 maps to 0 (no sharpening) — the toggle respects
        // the slider, so enabling "Sharpness" with the amount left at 0 does nothing
        // (no auto-fallback). Fix for "the sharpness slider does nothing" — it was
        // being forwarded as 0.0 the whole time.
        {
            float s = sharpness;
            if (s < 0.0f) s = 0.0f;
            // REFramework slider max is 5.0 -> map linearly to FFX max 1.0.
            if (s > 0.0f) s /= 5.0f;
            dd.sharpness = s;
        }
        dd.frameTimeDelta          = computeFrameTimeDeltaMs(ctx.lastEvalQpc, m_impl->qpcFreq);
        dd.preExposure             = 1.0f;
        dd.reset                   = reset;
        dd.cameraNear              = nearPlane;
        dd.cameraFar               = farPlane;
        dd.cameraFovAngleVertical  = verticalFOV;
        dd.viewSpaceToMetersFactor = 1.0f;
        dd.flags                   = 0;

        ffxReturnCode_t ret = m_impl->ffxDispatchFn(&ctx.ffxCtx, &dd.header);
        if (ret != 0) {
            Logging::error("FSR4Backend: ffxDispatch returned code=%d", ret);
            if (!cmdList) {
                Impl::closeInternalCmdListIfOpen(m_impl);
            }
            return;
        }

        // Linux/Proton red<->blue correction. FSR4 just wrote ctx.outputTexture
        // in RGBA byte order (left in UAV state). REFramework will raw-copy the
        // texture we returned from InitUpscaler (ctx.rbSwap.swapTexture) into the
        // BGRA-presenting backbuffer, and vkd3d won't reorder channels on that
        // copy. So we run our compute pass now, on the SAME command list, to
        // write an R/B-swapped (BGRA-ordered) copy into ctx.rbSwap.swapTexture.
        // Only active when the swap pass was set up in createContext.
        if (ctx.rbSwapEnabled && ctx.rbSwap.isReady() &&
            m_impl->rbSwapPipeline.isReady() && effectiveCmdList &&
            effectiveDst == ctx.outputTexture) {
            recordRbSwap(m_impl->rbSwapPipeline, ctx.rbSwap, ctx.outputTexture,
                         static_cast<uint32_t>(ctx.displaySizeX),
                         static_cast<uint32_t>(ctx.displaySizeY),
                         static_cast<ID3D12GraphicsCommandList*>(effectiveCmdList));
        } else if (ctx.rbSwapEnabled && effectiveDst != ctx.outputTexture) {
            // REFramework handed us a destination texture, so FSR4 did NOT write
            // into ctx.outputTexture; the swap source would be wrong. Swap is
            // skipped and colors WILL stay swapped. Log once so a test that shows
            // "still swapped" is explained instead of mysterious.
            if (!m_impl->rbSwapSkippedWarned) {
                m_impl->rbSwapSkippedWarned = true;
                Logging::warn("FSR4Backend: R/B swap SKIPPED - evaluate() got a non-null "
                              "destination (fsr wrote elsewhere); output texture unchanged -> "
                              "colors will still be R/B swapped. REFramework normally passes null.");
            }
        }

        // NOTE: The backbuffer copy is performed by REFramework's TemporalUpscaler
        // (it copies m_upscaled_textures[index] -> backbuffer after EvaluateUpscaler).
        // We just leave the result in ctx.outputTexture, which is what we returned
        // from InitUpscaler. No manual swap-chain copy is needed.

        // Rate-limited success log + one-time input dimensions (aliasing debug).
        {
            static std::atomic<long long> s_last{0};
            const long long now = static_cast<long long>(GetTickCount64());
            if (!m_impl->inputSizesLogged) {
                m_impl->inputSizesLogged = true;
                D3D12_RESOURCE_DESC cd = ((ID3D12Resource*)color)->GetDesc();
                D3D12_RESOURCE_DESC dd2 = ((ID3D12Resource*)depth)->GetDesc();
                D3D12_RESOURCE_DESC md = ((ID3D12Resource*)motionVector)->GetDesc();
                Logging::info("FSR4Backend: inputs color=%ux%u fmt=%d depth=%ux%u fmt=%d mv=%ux%u fmt=%d | out=%ux%u fmt=%d",
                              (unsigned)cd.Width, cd.Height, (int)cd.Format,
                              (unsigned)dd2.Width, dd2.Height, (int)dd2.Format,
                              (unsigned)md.Width, md.Height, (int)md.Format,
                              ctx.displaySizeX, ctx.displaySizeY, ctx.format);
            }
            long long prev = s_last.load(std::memory_order_relaxed);
            if (now - prev > 2000 && s_last.compare_exchange_strong(prev, now, std::memory_order_acq_rel)) {
                Logging::info("FSR4Backend: ffxDispatch SUCCEEDED id=%d (out=%p fmt=%d %ux%u) reset=%d jitter=(%.4f,%.4f) mvScale=(%.4f,%.4f) rcas=%s sharp=%.3f",
                              id, effectiveDst, ctx.format, ctx.displaySizeX, ctx.displaySizeY,
                              (int)reset, jitterOffsetX, jitterOffsetY,
                              ctx.motionScaleX, ctx.motionScaleY,
                              ctx.enableSharpening ? "ON" : "off", dd.sharpness);
            }
        }

        if (!cmdList) {
            if (execute) {
                if (effectiveCmdList && m_impl->commandQueue) {
                    ID3D12GraphicsCommandList* cl = (ID3D12GraphicsCommandList*)effectiveCmdList;
                    const HRESULT closeHr = cl->Close();
                    m_impl->cmdListOpen = false;
                    if (FAILED(closeHr)) {
                        Logging::error("FSR4Backend: cmdList->Close() failed hr=%08x", closeHr);
                        return;
                    }

                    ID3D12CommandList* lists[] = { cl };
                    m_impl->commandQueue->ExecuteCommandLists(1, lists);
                    // Signal the fence so the slot allocator isn't reset while this work is still
                    // in flight (needed for allocator lifetime), rotating across the 3-slot ring
                    // instead of blocking every frame. REFramework copies our output on the SAME
                    // command queue (copier.copy -> backbuffer), so in-order queue execution
                    // already guarantees the upscale finishes before the copy reads it.
                    if (m_impl->fence) {
                        const uint64_t nextFence = m_impl->fenceValue + 1;
                        const HRESULT sigHr = m_impl->commandQueue->Signal(m_impl->fence, nextFence);
                        if (SUCCEEDED(sigHr)) {
                            m_impl->fenceValue = nextFence;
                            m_impl->slotFenceValues[m_impl->currentSlot] = nextFence;
                        } else {
                            Logging::error("FSR4Backend: commandQueue->Signal(%llu) failed hr=%08x",
                                           (unsigned long long)nextFence, sigHr);
                        }
                    }
                    m_impl->currentSlot = (m_impl->currentSlot + 1) % Impl::kNumCmdSlots;
                } else {
                    Impl::closeInternalCmdListIfOpen(m_impl);
                }
            }
        }
    } catch (const std::exception& e) {
        Logging::error("FSR4Backend: ffxDispatch exception: %s", e.what());
        if (!cmdList) {
            Impl::closeInternalCmdListIfOpen(m_impl);
        }
        return;
    } catch (...) {
        Logging::error("FSR4Backend: ffxDispatch CRASHED");
        if (!cmdList) {
            Impl::closeInternalCmdListIfOpen(m_impl);
        }
        return;
    }
}

void FSR4Backend::discardOpenCommandList() {
    if (!m_impl) return;
    SrwExclusiveGuard lockGuard(&m_impl->lock);
    try {
        Impl::closeInternalCmdListIfOpen(m_impl);
    } catch (...) {
        Logging::warn("FSR4Backend: discardOpenCommandList threw exception");
    }
}

void FSR4Backend::release(int id) {
    if (!m_impl) return;
    SrwExclusiveGuard lockGuard(&m_impl->lock);
    auto it = m_impl->contexts.find(id);
    if (it != m_impl->contexts.end()) {
        Impl::closeInternalCmdListIfOpen(m_impl);
        Impl::waitForGpu(m_impl);
        Impl::destroyContextResources(m_impl, it->second);
        m_impl->contexts.erase(it);
        Logging::info("FSR4Backend: Released context id=%d", id);
    }
}

void FSR4Backend::setMotionScaleX(int id, float scale) {
    if (!m_impl) return;
    SrwExclusiveGuard lockGuard(&m_impl->lock);
    auto it = m_impl->contexts.find(id);
    if (it != m_impl->contexts.end()) it->second.motionScaleX = scale;
}

void FSR4Backend::setMotionScaleY(int id, float scale) {
    if (!m_impl) return;
    SrwExclusiveGuard lockGuard(&m_impl->lock);
    auto it = m_impl->contexts.find(id);
    if (it != m_impl->contexts.end()) it->second.motionScaleY = scale;
}

int FSR4Backend::getRenderWidth(int id) const {
    if (!m_impl) return 0;
    SrwSharedGuard lockGuard(&m_impl->lock);
    auto it = m_impl->contexts.find(id);
    return it != m_impl->contexts.end() ? it->second.renderWidth : 0;
}

int FSR4Backend::getRenderHeight(int id) const {
    if (!m_impl) return 0;
    SrwSharedGuard lockGuard(&m_impl->lock);
    auto it = m_impl->contexts.find(id);
    return it != m_impl->contexts.end() ? it->second.renderHeight : 0;
}

float FSR4Backend::getOptimalSharpness(int id) const { return 0.5f; }
float FSR4Backend::getOptimalMipmapBias(int id) const { return 0.0f; }

int FSR4Backend::getJitterPhaseCount(int id) const {
    if (!m_impl) return 1;
    SrwSharedGuard lockGuard(&m_impl->lock);
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
    if (!m_impl) return -1;
    SrwSharedGuard lockGuard(&m_impl->lock);
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
            static std::atomic<long long> s_last{0};
            long long now = (long long)GetTickCount64();
            long long prev = s_last.load(std::memory_order_relaxed);
            if (now - prev > 2000 && s_last.compare_exchange_strong(prev, now, std::memory_order_acq_rel)) {
                Logging::error("FSR4Backend: ffxQuery jitter offset FAILED rc=%d (outX=%p outY=%p)", rc, (void*)outX, (void*)outY);
            }
            return -1;
        }
        return 0;
    }
    return -1;
}

bool FSR4Backend::isMethodAvailable(int upscaleMethod) const {
    if (!m_impl) return false;
    SrwSharedGuard lockGuard(&m_impl->lock);
    if (!m_impl->available) return false;
    // REFramework's TemporalUpscaler enumerates exactly three method ids via
    // PDUpscaleType { DLSS=0, FSR2=1, XeSS=2 } (TemporalUpscaler.cpp loops
    // for i = 0..XESS) and only lists one whose IsUpscaleMethodAvailable
    // returns true. We advertise ONLY id 1 as selectable; getMethodName relabels
    // it "AMD FSR 3" because this mod runs FSR4 under the FSR back-end contract.
    // DLSS (0) and XeSS (2) report false and are hidden. There is no FSR4 id in
    // REFramework's dropdown, so no separate entry needs hiding.
    return upscaleMethod == 1; // only the FSR back-end (id 1) is selectable
}

const char* FSR4Backend::getMethodName(int upscaleMethod) const {
    // REFramework only queries ids 0..2 (DLSS, FSR2, XeSS). We relabel id 1 as
    // "AMD FSR 3" since this mod drives FSR4 through the FSR back-end.
    switch (upscaleMethod) {
        case 0: return "NVIDIA DLSS";
        case 1: return "AMD FSR 3";
        case 2: return "Intel XeSS";
        default: return "";
    }
}
