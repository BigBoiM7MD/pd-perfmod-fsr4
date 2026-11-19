#include <Windows.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "mock_ffx_abi.h"

namespace {

struct MockVersionEntry {
    uint64_t id;
    const char* name;
};

static constexpr MockVersionEntry kMockVersions[] = {
    { 0x00401001ULL, "4.1.1-mock" },
    { 0x00301005ULL, "3.1.5-mock" },
    { 0x00301000ULL, "3.1.0-mock" },
};

static constexpr uint64_t kMockVersionId   = kMockVersions[0].id;
static const char* const kMockVersionName  = kMockVersions[0].name;

struct MockContextEntry {
    void* handle;
    uint64_t versionId;
    const char* versionName;
};

static SRWLOCK g_mockLock = SRWLOCK_INIT;
static std::vector<MockFfxCreateRecord> g_createRecords;
static std::vector<MockFfxDispatchDescUpscale> g_dispatchRecords;
static std::vector<MockContextEntry> g_contexts;
static uint32_t g_createSuccessCount = 0;
static uint32_t g_destroyCount       = 0;
static uint64_t g_lastProviderVersionId = 0;
static uintptr_t g_nextContextSeq    = 1;

struct SrwLockGuard {
    explicit SrwLockGuard(SRWLOCK* lock) noexcept : m_lock(lock) {
        if (m_lock) AcquireSRWLockExclusive(m_lock);
    }
    ~SrwLockGuard() {
        if (m_lock) ReleaseSRWLockExclusive(m_lock);
    }
    SrwLockGuard(const SrwLockGuard&) = delete;
    SrwLockGuard& operator=(const SrwLockGuard&) = delete;
private:
    SRWLOCK* m_lock;
};

static bool readEnvFlagEquals(const char* name, char expected) {
    char buf[32] = {};
    const DWORD len = GetEnvironmentVariableA(name, buf, sizeof(buf));
    return len > 0 && len < sizeof(buf) && buf[0] == expected && buf[1] == '\0';
}

static uint64_t readEnvUint64(const char* name) {
    char buf[64] = {};
    const DWORD len = GetEnvironmentVariableA(name, buf, sizeof(buf));
    if (len == 0 || len >= sizeof(buf)) {
        return 0;
    }
    char* end = nullptr;
    const unsigned long long val = _strtoui64(buf, &end, 0);
    if (end == buf) {
        return 0;
    }
    return static_cast<uint64_t>(val);
}

static uint64_t getConfiguredVersionCount() {
    char buf[32] = {};
    const DWORD len = GetEnvironmentVariableA("PD_MOCK_FFX_VERSIONS", buf, sizeof(buf));
    if (len == 0 || len >= sizeof(buf)) {
        return 1;
    }
    if (buf[0] == '0' && buf[1] == '\0') return 0;
    if (buf[0] == '2' && buf[1] == '\0') return 2;
    if (buf[0] == '3' && buf[1] == '\0') return 3;
    return 1;
}

static const char* findVersionName(uint64_t versionId) {
    for (const auto& entry : kMockVersions) {
        if (entry.id == versionId) {
            return entry.name;
        }
    }
    return kMockVersionName;
}

} // namespace

// ===========================================================================
// Official __cdecl FidelityFX Loader Exports
// ===========================================================================

extern "C" __declspec(dllexport) uint32_t __cdecl ffxCreateContext(
    void** context,
    MockFfxApiHeader* desc,
    const void* allocCallbacks) {
    (void)allocCallbacks;

    if (!context) {
        return 1;
    }

    MockFfxCreateRecord rec = {};
    uint32_t hop = 0;
    for (const MockFfxApiHeader* cur = desc; cur != nullptr && hop < 16; cur = cur->pNext, ++hop) {
        if (cur->type == kMockFfxCreateContextUpscale) {
            const auto* up = reinterpret_cast<const MockFfxCreateContextDescUpscale*>(cur);
            rec.flags            = up->flags;
            rec.maxRenderWidth   = up->maxRenderSize.width;
            rec.maxRenderHeight  = up->maxRenderSize.height;
            rec.maxUpscaleWidth  = up->maxUpscaleSize.width;
            rec.maxUpscaleHeight = up->maxUpscaleSize.height;
        } else if (cur->type == kMockFfxOverrideVersion) {
            const auto* ov = reinterpret_cast<const MockFfxOverrideVersion*>(cur);
            rec.overrideVersionId = ov->versionId;
        } else if (cur->type == kMockFfxCreateBackendDX12) {
            const auto* be = reinterpret_cast<const MockFfxCreateBackendDX12Desc*>(cur);
            rec.backendDevice = be->device;
        }
    }

    const bool failAll = readEnvFlagEquals("PD_MOCK_FFX_CREATE_FAIL", '1');
    const uint64_t failVersion = readEnvUint64("PD_MOCK_FFX_FAIL_VERSION");
    const bool failThisVersion = (failVersion != 0 && rec.overrideVersionId == failVersion);

    if (failAll || failThisVersion) {
        rec.resultCode = 1;
        {
            SrwLockGuard guard(&g_mockLock);
            g_createRecords.push_back(rec);
        }
        *context = nullptr;
        return 1;
    }

    const uint64_t winVersionId = (rec.overrideVersionId != 0) ? rec.overrideVersionId : kMockVersionId;
    const char* winVersionName  = findVersionName(winVersionId);

    void* handle = nullptr;
    {
        SrwLockGuard guard(&g_mockLock);
        handle = reinterpret_cast<void*>(static_cast<uintptr_t>(0x46535200ULL + g_nextContextSeq++));
        g_contexts.push_back({ handle, winVersionId, winVersionName });
        ++g_createSuccessCount;
        rec.resultCode = 0;
        g_createRecords.push_back(rec);
    }

    *context = handle;
    return 0;
}

extern "C" __declspec(dllexport) uint32_t __cdecl ffxDestroyContext(
    void** context,
    const void* allocCallbacks) {
    (void)allocCallbacks;
    if (context && *context) {
        void* handle = *context;
        *context = nullptr;
        SrwLockGuard guard(&g_mockLock);
        for (size_t i = 0; i < g_contexts.size(); ++i) {
            if (g_contexts[i].handle == handle) {
                g_contexts.erase(g_contexts.begin() + static_cast<ptrdiff_t>(i));
                break;
            }
        }
        ++g_destroyCount;
    }
    return 0;
}

extern "C" __declspec(dllexport) uint32_t __cdecl ffxDispatch(
    void** context,
    const MockFfxApiHeader* desc) {
    void* ctxHandle = context ? *context : nullptr;

    if (desc && desc->type == kMockFfxDispatchUpscale) {
        const auto* dd = reinterpret_cast<const MockFfxDispatchDescUpscale*>(desc);
        SrwLockGuard guard(&g_mockLock);
        g_dispatchRecords.push_back(*dd);
    }

    // Lock is released before RaiseException so /EHsc DLL frames never leak g_mockLock.
    if (readEnvFlagEquals("PD_MOCK_FFX_CRASH_DISPATCH", '1')) {
        RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    }

    if (readEnvFlagEquals("PD_MOCK_FFX_DISPATCH_FAIL", '1')) {
        return 1;
    }

    if (!ctxHandle) {
        return 1;
    }
    return 0;
}

extern "C" __declspec(dllexport) uint32_t __cdecl ffxQuery(
    void* context,
    MockFfxApiHeader* header) {
    if (!header) {
        return 1;
    }

    if (header->type == kMockFfxQueryGetVersions) {
        auto* q = reinterpret_cast<MockFfxQueryDescGetVersions*>(header);
        if (!q->outputCount) {
            return 1;
        }
        const uint64_t count = getConfiguredVersionCount();
        if (count == 0) {
            *q->outputCount = 0;
            return 0;
        }
        if (!q->versionIds || !q->versionNames) {
            *q->outputCount = count;
            return 0;
        }
        if (*q->outputCount < count) {
            *q->outputCount = count;
            return 1;
        }
        *q->outputCount = count;
        for (uint64_t i = 0; i < count; ++i) {
            q->versionIds[i]   = kMockVersions[i].id;
            q->versionNames[i] = kMockVersions[i].name;
        }
        return 0;
    }

    if (header->type == kMockFfxQueryGetProviderVersion) {
        auto* pv = reinterpret_cast<MockFfxQueryGetProviderVersion*>(header);
        void* ctxHandle = context ? *reinterpret_cast<void**>(context) : nullptr;
        uint64_t outId = kMockVersionId;
        const char* outName = kMockVersionName;
        {
            SrwLockGuard guard(&g_mockLock);
            if (ctxHandle) {
                for (const auto& entry : g_contexts) {
                    if (entry.handle == ctxHandle) {
                        outId   = entry.versionId;
                        outName = entry.versionName;
                        break;
                    }
                }
            }
            g_lastProviderVersionId = outId;
        }
        pv->versionId   = outId;
        pv->versionName = outName;
        return 0;
    }

    if (header->type == kMockFfxQueryJitterPhaseCount) {
        auto* q = reinterpret_cast<MockFfxQueryDescUpscaleGetJitterPhaseCount*>(header);
        if (q->pOutPhaseCount) {
            *q->pOutPhaseCount = 8;
        }
        return 0;
    }

    if (header->type == kMockFfxQueryGetJitterOffset) {
        static_assert(kMockFfxQueryGetJitterOffset == 0x00010005u, "jitter offset query id");
        auto* q = reinterpret_cast<MockFfxQueryDescUpscaleGetJitterOffset*>(header);
        if (q->index < 0) {
            return 1;
        }
        if (q->pOutX) *q->pOutX = 0.25f;
        if (q->pOutY) *q->pOutY = -0.25f;
        return 0;
    }

    return 1;
}

// ===========================================================================
// Non-FFX C-ABI test inspection exports
// ===========================================================================

extern "C" __declspec(dllexport) void __cdecl MockFfxResetState() {
    SrwLockGuard guard(&g_mockLock);
    g_createRecords.clear();
    g_dispatchRecords.clear();
    g_contexts.clear();
    g_createSuccessCount    = 0;
    g_destroyCount          = 0;
    g_lastProviderVersionId = 0;
    g_nextContextSeq        = 1;
}

extern "C" __declspec(dllexport) void __cdecl MockFfxGetStats(MockFfxStats* outStats) {
    if (!outStats) return;
    SrwLockGuard guard(&g_mockLock);
    outStats->createAttemptCount    = static_cast<uint32_t>(g_createRecords.size());
    outStats->createSuccessCount    = g_createSuccessCount;
    outStats->destroyCount          = g_destroyCount;
    outStats->dispatchCount         = static_cast<uint32_t>(g_dispatchRecords.size());
    outStats->lastProviderVersionId = g_lastProviderVersionId;
}

extern "C" __declspec(dllexport) int __cdecl MockFfxGetCreateRecord(
    uint32_t index,
    MockFfxCreateRecord* outRecord) {
    if (!outRecord) return 0;
    SrwLockGuard guard(&g_mockLock);
    if (index >= g_createRecords.size()) {
        return 0;
    }
    *outRecord = g_createRecords[index];
    return 1;
}

extern "C" __declspec(dllexport) int __cdecl MockFfxGetDispatchRecord(
    uint32_t index,
    MockFfxDispatchDescUpscale* outRecord) {
    if (!outRecord) return 0;
    SrwLockGuard guard(&g_mockLock);
    if (index >= g_dispatchRecords.size()) {
        return 0;
    }
    *outRecord = g_dispatchRecords[index];
    return 1;
}
