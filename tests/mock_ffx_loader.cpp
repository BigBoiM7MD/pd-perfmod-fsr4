#include <Windows.h>
#include <cstdint>

struct MockFfxApiHeader {
    uint64_t type;
    MockFfxApiHeader* pNext;
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

static constexpr uint64_t kMockFfxQueryGetVersions        = 4u;
static constexpr uint64_t kMockFfxQueryGetProviderVersion = 6u;
static constexpr uint64_t kMockVersionId                  = 0x00401001ULL;
static const char* const kMockVersionName                 = "4.1.1-mock";

static int g_dummyContextHandle = 0x46535234;

static bool readEnvFlagEquals(const char* name, char expected) {
    char buf[32] = {};
    const DWORD len = GetEnvironmentVariableA(name, buf, sizeof(buf));
    return len > 0 && len < sizeof(buf) && buf[0] == expected && buf[1] == '\0';
}

extern "C" __declspec(dllexport) uint32_t __cdecl ffxCreateContext(
    void** context,
    MockFfxApiHeader* desc,
    const void* allocCallbacks) {
    (void)desc;
    (void)allocCallbacks;

    if (!context) {
        return 1;
    }

    if (readEnvFlagEquals("PD_MOCK_FFX_CREATE_FAIL", '1')) {
        *context = nullptr;
        return 1;
    }

    *context = &g_dummyContextHandle;
    return 0;
}

extern "C" __declspec(dllexport) uint32_t __cdecl ffxDestroyContext(
    void** context,
    const void* allocCallbacks) {
    (void)allocCallbacks;
    if (context) {
        *context = nullptr;
    }
    return 0;
}

extern "C" __declspec(dllexport) uint32_t __cdecl ffxDispatch(
    void** context,
    const MockFfxApiHeader* desc) {
    (void)context;
    (void)desc;
    return 0;
}

extern "C" __declspec(dllexport) uint32_t __cdecl ffxQuery(
    void* context,
    MockFfxApiHeader* header) {
    (void)context;
    if (!header) {
        return 1;
    }

    if (header->type == kMockFfxQueryGetVersions) {
        auto* q = reinterpret_cast<MockFfxQueryDescGetVersions*>(header);
        if (!q->outputCount) {
            return 1;
        }
        if (readEnvFlagEquals("PD_MOCK_FFX_VERSIONS", '0')) {
            *q->outputCount = 0;
            return 0;
        }
        if (!q->versionIds || !q->versionNames) {
            *q->outputCount = 1;
            return 0;
        }
        if (*q->outputCount < 1) {
            *q->outputCount = 1;
            return 1;
        }
        *q->outputCount = 1;
        q->versionIds[0] = kMockVersionId;
        q->versionNames[0] = kMockVersionName;
        return 0;
    }

    if (header->type == kMockFfxQueryGetProviderVersion) {
        auto* pv = reinterpret_cast<MockFfxQueryGetProviderVersion*>(header);
        pv->versionId = kMockVersionId;
        pv->versionName = kMockVersionName;
        return 0;
    }

    return 1;
}
