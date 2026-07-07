#include "FSR4Backend.h"
#include "SwapChainHook.h"
#include "Logging.h"
#include "../include/PDPerfPlugin.h"

#include <d3d12.h>
#include <dxgi1_4.h>
#include <unordered_map>
#include <cstring>
#include <exception>

// -----------------------------------------------------------------------
// FFX API type aliases matching the SDK headers exactly
// -----------------------------------------------------------------------
using ffxReturnCode_t = uint32_t;
using ffxContext      = void*;
using ffxStructType_t = uint64_t;
typedef void (*ffxApiMessage)(uint32_t type, const wchar_t* message);

// Global variable to expose upscaled texture to TemporalUpscaler for backbuffer copy
static ID3D12Resource* g_upscaledTexture = nullptr;

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


