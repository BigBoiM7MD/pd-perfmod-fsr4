// fsr4_overlay.cpp
// Standalone FSR4 output-overlay texture helpers (DIAG_SOLID only now).
// See fsr4_overlay.h for the full rationale. The single supported fill path is:
//   DEFAULT-heap texture (COPY_DEST) <-- CopyTextureRegion <-- UPLOAD-heap
//   BUFFER (format-agnostic) <-- Map. No UPLOAD-heap R10G10B10A2 textures,
//   no WriteToSubresource. Validated end-to-end on D3D12 WARP.
//
// COLOR FORMAT NOTE:
//   The output backbuffer format is chosen by the game/REFramework at runtime
//   and handed to us as `ctx.format`. We MUST pack our pixels into that SAME
//   format (see packColor()/formatBpp()) -- packing for the wrong format turns
//   e.g. a dark-gray panel into a solid blue rectangle. All packing below is
//   format-aware.
#include "../include/fsr4_overlay.h"
#include "../include/Logging.h"

#include <d3d12.h>
#include <dxgi1_4.h>
#include <vector>
#include <cstring>
#include <cstdint>
#include <cstdlib>

namespace Fsr4Overlay {

// ---------------------------------------------------------------------------
// Format-aware pixel packing. The DIAG_SOLID texture (and any future overlay)
// is created with the SAME DXGI_FORMAT as the game output, so we must lay out
// each pixel in that format's byte order. Returns bytes-per-pixel for the
// common display formats (everything else is conservatively treated as 4).
// ---------------------------------------------------------------------------
static int formatBpp(DXGI_FORMAT fmt) {
    switch (fmt) {
        case 28: case 27: case 87: case 90: case 24: case 23: // R8G8B8A8(_TYPED/LESS), B8G8R8A8(_TYPED/LESS), R10G10B10A2(_TYPED/LESS)
            return 4;
        case 10: case 9: // R16G16B16A16_FLOAT / _TYPELESS
            return 8;
        default:
            return 4;
    }
}

// Map TYPLESS display enums to their concrete format, mirroring makeResource()
// in FSR4Backend.cpp (23->24 R10G10B10A2, 27->28 R8G8B8A8, 9->10 R16G16B16A16).
static DXGI_FORMAT resolveFormat(DXGI_FORMAT fmt) {
    switch ((int)fmt) {
        case 23: return (DXGI_FORMAT)24;
        case 27: return (DXGI_FORMAT)28;
        case 9:  return (DXGI_FORMAT)10;
        default: return fmt;
    }
}

// Pack (r,g,b,a) in 0..255 into `out` (max 8 bytes) for `fmt`.
static void packColor(DXGI_FORMAT fmt, uint8_t out[8], uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    memset(out, 0, 8);
    switch (resolveFormat(fmt)) {
        case 87: case 90: // B8G8R8A8(_TYPED/LESS): byte order B,G,R,A
            out[0] = b; out[1] = g; out[2] = r; out[3] = a; break;
        case 24: { // R10G10B10A2_UNORM(_TYPED/LESS)
            uint32_t r10 = (uint32_t)((r * 1023 + 128) / 255);
            uint32_t g10 = (uint32_t)((g * 1023 + 128) / 255);
            uint32_t b10 = (uint32_t)((b * 1023 + 128) / 255);
            uint32_t a2  = (uint32_t)((a * 3    + 128) / 255);
            uint32_t u = (a2 << 30) | (b10 << 20) | (g10 << 10) | r10;
            memcpy(out, &u, 4); // little-endian: byte0 = R low bits
            break;
        }
        case 10: case 9: { // R16G16B16A16_FLOAT / _TYPELESS (store as UNORM16)
            uint16_t c[4] = { (uint16_t)(r * 257), (uint16_t)(g * 257),
                              (uint16_t)(b * 257), (uint16_t)(a * 257) };
            memcpy(out, c, 8);
            break;
        }
        default: // R8G8B8A8(_TYPED/LESS) and everything else: R,G,B,A
            out[0] = r; out[1] = g; out[2] = b; out[3] = a; break;
    }
}

// Core: fill a DEFAULT-heap texture from CPU pixels via UPLOAD buffer + copy.
ID3D12Resource* createTexFromPixels(ID3D12Device* dev,
                                     ID3D12CommandQueue* queue,
                                     int w, int h,
                                     DXGI_FORMAT targetFmt,
                                     const std::vector<uint8_t>& px) {
    if (!dev || !queue) {
        Logging::error("Fsr4Overlay: createTexFromPixels null dev/queue");
        return nullptr;
    }
    int bpp = formatBpp(targetFmt);
    if ((int)px.size() < w * h * bpp) {
        Logging::error("Fsr4Overlay: createTexFromPixels px too small (%zu < %d)",
                       (size_t)px.size(), w * h * bpp);
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
    // this works even when targetFmt would be illegal in an UPLOAD *texture*.
    // Size it as RowPitch*Height (256-byte row alignment), NOT the tight size
    // GetCopyableFootprints may report for totalBytes.
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT foot = {};
    UINT rowBytes = 0; UINT64 totalBytes = 0;
    dev->GetCopyableFootprints(&desc, 0, 1, 0, &foot, &rowBytes, &totalBytes, nullptr);
    const UINT footRow = foot.Footprint.RowPitch;
    const UINT tightRow = (UINT)w * bpp;
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
    for (int y = 0; y < h; ++y) {
        memcpy((uint8_t*)mapped + (size_t)y * footRow,
               px.data() + (size_t)y * w * bpp, (size_t)tightRow);
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

// Fill an entire w x h texture with one packed color (used by DIAG_SOLID).
// `colorArgb` is 0xAARRGGBB for readability at the call site; it is repacked
// into the real output format before upload.
ID3D12Resource* createSolidTexture(ID3D12Device* dev, ID3D12CommandQueue* queue,
                                    int w, int h, DXGI_FORMAT targetFmt, uint32_t colorArgb) {
    int bpp = formatBpp(targetFmt);
    uint8_t c[8];
    packColor(targetFmt, c,
              (uint8_t)(colorArgb >> 16), (uint8_t)(colorArgb >> 8),
              (uint8_t)(colorArgb),       (uint8_t)(colorArgb >> 24));
    std::vector<uint8_t> px((size_t)w * h * bpp, 0);
    for (size_t i = 0; i < (size_t)w * h; ++i) memcpy(&px[i * bpp], c, bpp);
    return createTexFromPixels(dev, queue, w, h, targetFmt, px);
}

} // namespace Fsr4Overlay
