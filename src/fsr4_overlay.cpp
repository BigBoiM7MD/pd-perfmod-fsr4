// fsr4_overlay.cpp
// Standalone FSR4 output-overlay texture helpers. See fsr4_overlay.h for the
// full rationale. The single supported fill path is:
//   DEFAULT-heap texture (COPY_DEST) <-- CopyTextureRegion <-- UPLOAD-heap
//   BUFFER (format-agnostic) <-- Map. No UPLOAD-heap R10G10B10A2 textures,
//   no WriteToSubresource. Validated end-to-end on D3D12 WARP.
#include "fsr4_overlay.h"
#include "Logging.h"

#include <d3d12.h>
#include <dxgi1_4.h>
#include <vector>
#include <cstring>
#include <windows.h>

namespace Fsr4Overlay {

// 5x7 glyphs for "FSR4" (identical to the original watermark layout).
static const uint8_t g_glyphs[4][5] = {
    {0x7C, 0x44, 0x44, 0x44, 0x7C}, // F
    {0x7C, 0x40, 0x7C, 0x40, 0x7C}, // S
    {0x7C, 0x40, 0x40, 0x40, 0x40}, // R
    {0x7E, 0x42, 0x7E, 0x4C, 0x42}, // 4
};
static const int g_gx[4] = {4, 28, 52, 76};

// Core: fill a DEFAULT-heap texture from CPU pixels via UPLOAD buffer + copy.
ID3D12Resource* createTexFromPixels(ID3D12Device* dev,
                                     ID3D12CommandQueue* queue,
                                     int w, int h,
                                     DXGI_FORMAT targetFmt,
                                     const std::vector<uint32_t>& px) {
    if (!dev || !queue) {
        Logging::error("Fsr4Overlay: createTexFromPixels null dev/queue");
        return nullptr;
    }
    if ((int)px.size() < w * h) {
        Logging::error("Fsr4Overlay: createTexFromPixels px too small (%zu < %d)",
                       (size_t)px.size(), w * h);
        return nullptr;
    }

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = (UINT64)w; desc.Height = (UINT)h; desc.DepthOrArraySize = 1;
    desc.MipLevels = 1; desc.Format = targetFmt;
    desc.SampleDesc.Count = 1; desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    // Destination: GPU-local DEFAULT heap in COPY_DEST (we fill it via copy).
    ID3D12Resource* tex = nullptr;
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    HRESULT hr = dev->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex));
    if (FAILED(hr)) {
        Logging::error("Fsr4Overlay: createTexFromPixels dst CreateCommittedResource failed hr=%08x", hr);
        return nullptr;
    }

    // Staging BUFFER in an UPLOAD heap. Buffers have NO format restriction, so
    // this works even when targetFmt (R10G10B10A2) would be illegal in an
    // UPLOAD *texture*. Size it as RowPitch*Height (256-byte row alignment),
    // NOT the tight size GetCopyableFootprints may report for totalBytes.
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT foot = {};
    UINT rowBytes = 0; UINT64 totalBytes = 0;
    dev->GetCopyableFootprints(&desc, 0, 1, 0, &foot, &rowBytes, &totalBytes, nullptr);
    const UINT footRow = foot.Footprint.RowPitch;
    const UINT64 bufSize = (UINT64)footRow * h;

    D3D12_RESOURCE_DESC bdesc = {};
    bdesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bdesc.Width = bufSize; bdesc.Height = 1; bdesc.DepthOrArraySize = 1;
    bdesc.MipLevels = 1; bdesc.SampleDesc.Count = 1;
    bdesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* staging = nullptr;
    D3D12_HEAP_PROPERTIES bhp = {};
    bhp.Type = D3D12_HEAP_TYPE_UPLOAD;
    hr = dev->CreateCommittedResource(
        &bhp, D3D12_HEAP_FLAG_NONE, &bdesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&staging));
    if (FAILED(hr)) {
        Logging::error("Fsr4Overlay: createTexFromPixels staging buffer CreateCommittedResource failed hr=%08x", hr);
        tex->Release();
        return nullptr;
    }

    void* mapped = nullptr;
    D3D12_RANGE read = {0, 0};
    hr = staging->Map(0, &read, &mapped);
    if (FAILED(hr)) {
        Logging::error("Fsr4Overlay: createTexFromPixels staging Map failed hr=%08x", hr);
        staging->Release(); tex->Release();
        return nullptr;
    }
    const UINT tightRow = (UINT)w * 4; // R10G10B10A2 = 4 bytes/px
    for (int y = 0; y < h; ++y) {
        memcpy((uint8_t*)mapped + (size_t)y * footRow,
               px.data() + (size_t)y * w, (size_t)tightRow);
    }
    staging->Unmap(0, nullptr);

    // One-shot copy + transition to COPY_SOURCE (badge is a copy source
    // afterwards, so the per-frame badge->output copy is valid).
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* cl = nullptr;
    if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
        FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&cl)))) {
        Logging::error("Fsr4Overlay: createTexFromPixels create cmdlist failed");
        if (alloc) alloc->Release();
        staging->Release(); tex->Release();
        return nullptr;
    }
    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = 0; dstLoc.pResource = tex;
    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.SubresourceIndex = 0; srcLoc.pResource = staging; srcLoc.PlacedFootprint = foot;
    cl->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = tex; b.Transition.Subresource = 0;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cl->ResourceBarrier(1, &b);
    cl->Close();

    ID3D12CommandList* lists[] = { cl };
    queue->ExecuteCommandLists(1, lists);
    ID3D12Fence* fence = nullptr;
    if (SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        queue->Signal(fence, 1);
        if (fence->GetCompletedValue() < 1) {
            HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
            if (ev) {
                fence->SetEventOnCompletion(1, ev);
                WaitForSingleObject(ev, INFINITE);
                CloseHandle(ev);
            }
        }
        fence->Release();
    }
    alloc->Release();
    cl->Release();
    staging->Release();
    return tex;
}

ID3D12Resource* createSolidTexture(ID3D12Device* dev, ID3D12CommandQueue* queue,
                                    int w, int h, DXGI_FORMAT targetFmt, uint32_t color) {
    std::vector<uint32_t> px((size_t)w * h, color);
    return createTexFromPixels(dev, queue, w, h, targetFmt, px);
}

ID3D12Resource* createWatermarkTexture(ID3D12Device* dev, ID3D12CommandQueue* queue,
                                        int& outW, int& outH, DXGI_FORMAT targetFmt) {
    const int w = 200, h = 40;
    const uint32_t green = 0xC00FFC00; // ABGR opaque GREEN (R10G10B10A2)
    const uint32_t black = 0xFF000000;

    std::vector<uint32_t> px((size_t)w * h, black);
    for (int g = 0; g < 4; ++g) {
        for (int row = 0; row < 5; ++row) {
            uint8_t bits = g_glyphs[g][row];
            for (int col = 0; col < 7; ++col) {
                if (bits & (0x40 >> col)) {
                    int x = g_gx[g] + col;
                    int y = 8 + row;
                    if (x >= 0 && x < w && y >= 0 && y < h)
                        px[(size_t)y * w + x] = green;
                }
            }
        }
    }

    ID3D12Resource* tex = createTexFromPixels(dev, queue, w, h, targetFmt, px);
    if (tex) { outW = w; outH = h; }
    return tex;
}

} // namespace Fsr4Overlay
