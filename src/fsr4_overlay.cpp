// fsr4_overlay.cpp
// Standalone FSR4 output-overlay texture helpers. See fsr4_overlay.h for the
// full rationale. The single supported fill path is:
//   DEFAULT-heap texture (COPY_DEST) <-- CopyTextureRegion <-- UPLOAD-heap
//   BUFFER (format-agnostic) <-- Map. No UPLOAD-heap R10G10B10A2 textures,
//   no WriteToSubresource. Validated end-to-end on D3D12 WARP.
//
// COLOR FORMAT NOTE (the "blue field" bug):
//   The output backbuffer format is chosen by the game/REFramework at runtime
//   and handed to us as `ctx.format`. We MUST pack our badge pixels into that
//   SAME format. The previous code hardcoded an R10G10B10A2 byte layout and a
//   fixed 4-byte/px stride, so under the game's actual R8G8B8A8_UNORM output
//   the panel color 0xEE101010 was read as bytes (R=0x10,G=0x10,B=0xEE) -> a
//   SOLID BLUE rectangle instead of dark gray. All color packing below is now
//   format-aware via packColor()/formatBpp().
#include "..\include\fsr4_overlay.h"
#include "..\include\Logging.h"

#include <d3d12.h>
#include <dxgi1_4.h>
#include <vector>
#include <cstring>
#include <cstdint>
#include <cstdlib>

namespace Fsr4Overlay {

// ---------------------------------------------------------------------------
// 5x7 bitmap font. Each glyph is 7 rows; each row byte has bit0 = LEFT-most
// column, bit4 = RIGHT-most column (the renderer in drawStringScaled() reads
// (bits >> col) & 1 with col 0..4). A '?' placeholder covers any char we
// didn't encode. This is the classic "5x7 dot-matrix" reference font.
// ---------------------------------------------------------------------------
static const uint8_t FONT[][7] = {
/* space */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00},
/* ! */     {0x00,0x00,0x5F,0x00,0x00,0x00,0x00},
/* " */     {0x00,0x07,0x00,0x07,0x00,0x00,0x00},
/* # */     {0x14,0x7F,0x14,0x7F,0x14,0x00,0x00},
/* $ */     {0x24,0x2A,0x7F,0x2A,0x12,0x00,0x00},
/* % */     {0x23,0x13,0x08,0x64,0x62,0x00,0x00},
/* & */     {0x36,0x49,0x55,0x22,0x50,0x00,0x00},
/* ' */     {0x00,0x05,0x03,0x00,0x00,0x00,0x00},
/* ( */     {0x00,0x1C,0x22,0x41,0x00,0x00,0x00},
/* ) */     {0x00,0x41,0x22,0x1C,0x00,0x00,0x00},
/* * */     {0x14,0x08,0x3E,0x08,0x14,0x00,0x00},
/* + */     {0x08,0x08,0x3E,0x08,0x08,0x00,0x00},
/* , */     {0x00,0x00,0x50,0x30,0x00,0x00,0x00},
/* - */     {0x08,0x08,0x08,0x08,0x08,0x00,0x00},
/* . */     {0x00,0x60,0x60,0x00,0x00,0x00,0x00},
/* / */     {0x20,0x10,0x08,0x04,0x02,0x00,0x00},
/* 0 */     {0x3E,0x51,0x49,0x45,0x3E,0x00,0x00},
/* 1 */     {0x00,0x42,0x7F,0x40,0x00,0x00,0x00},
/* 2 */     {0x42,0x61,0x51,0x49,0x46,0x00,0x00},
/* 3 */     {0x21,0x41,0x45,0x4B,0x31,0x00,0x00},
/* 4 */     {0x18,0x14,0x12,0x7F,0x10,0x00,0x00},
/* 5 */     {0x27,0x45,0x45,0x45,0x39,0x00,0x00},
/* 6 */     {0x3C,0x4A,0x49,0x49,0x30,0x00,0x00},
/* 7 */     {0x01,0x71,0x09,0x05,0x03,0x00,0x00},
/* 8 */     {0x36,0x49,0x49,0x49,0x36,0x00,0x00},
/* 9 */     {0x06,0x49,0x49,0x29,0x1E,0x00,0x00},
/* : */     {0x00,0x36,0x36,0x00,0x00,0x00,0x00},
/* ; */     {0x00,0x56,0x36,0x00,0x00,0x00,0x00},
/* < */     {0x08,0x14,0x22,0x41,0x00,0x00,0x00},
/* = */     {0x14,0x14,0x14,0x14,0x14,0x00,0x00},
/* > */     {0x00,0x41,0x22,0x14,0x08,0x00,0x00},
/* ? */     {0x02,0x01,0x51,0x09,0x06,0x00,0x00},
/* @ */     {0x32,0x49,0x79,0x41,0x3E,0x00,0x00},
/* A */     {0x7E,0x11,0x11,0x11,0x7E,0x00,0x00},
/* B */     {0x7F,0x49,0x49,0x49,0x36,0x00,0x00},
/* C */     {0x3E,0x41,0x41,0x41,0x22,0x00,0x00},
/* D */     {0x7F,0x41,0x41,0x22,0x1C,0x00,0x00},
/* E */     {0x7F,0x49,0x49,0x49,0x41,0x00,0x00},
/* F */     {0x7F,0x09,0x09,0x09,0x01,0x00,0x00},
/* G */     {0x3E,0x41,0x49,0x49,0x7A,0x00,0x00},
/* H */     {0x7F,0x08,0x08,0x08,0x7F,0x00,0x00},
/* I */     {0x00,0x41,0x7F,0x41,0x00,0x00,0x00},
/* J */     {0x20,0x40,0x41,0x3F,0x01,0x00,0x00},
/* K */     {0x7F,0x08,0x14,0x22,0x41,0x00,0x00},
/* L */     {0x7F,0x40,0x40,0x40,0x40,0x00,0x00},
/* M */     {0x7F,0x02,0x04,0x02,0x7F,0x00,0x00},
/* N */     {0x7F,0x04,0x08,0x10,0x7F,0x00,0x00},
/* O */     {0x3E,0x41,0x41,0x41,0x3E,0x00,0x00},
/* P */     {0x7F,0x09,0x09,0x09,0x06,0x00,0x00},
/* Q */     {0x3E,0x41,0x41,0x61,0x7E,0x00,0x00},
/* R */     {0x7F,0x09,0x19,0x29,0x46,0x00,0x00},
/* S */     {0x46,0x49,0x49,0x49,0x31,0x00,0x00},
/* T */     {0x01,0x01,0x7F,0x01,0x01,0x00,0x00},
/* U */     {0x3F,0x40,0x40,0x40,0x3F,0x00,0x00},
/* V */     {0x1F,0x20,0x40,0x20,0x1F,0x00,0x00},
/* W */     {0x3F,0x40,0x38,0x40,0x3F,0x00,0x00},
/* X */     {0x63,0x14,0x08,0x14,0x63,0x00,0x00},
/* Y */     {0x07,0x08,0x70,0x08,0x07,0x00,0x00},
/* Z */     {0x61,0x51,0x49,0x45,0x43,0x00,0x00},
/* [ */     {0x00,0x7F,0x41,0x41,0x00,0x00,0x00},
/* \ */     {0x02,0x04,0x08,0x10,0x20,0x00,0x00},
/* ] */     {0x00,0x41,0x41,0x7F,0x00,0x00,0x00},
/* ^ */     {0x04,0x02,0x01,0x02,0x04,0x00,0x00},
/* _ */     {0x40,0x40,0x40,0x40,0x40,0x00,0x00},
/* ` */     {0x00,0x01,0x02,0x04,0x00,0x00,0x00},
/* a */     {0x20,0x54,0x54,0x54,0x78,0x00,0x00},
/* b */     {0x7F,0x48,0x44,0x44,0x38,0x00,0x00},
/* c */     {0x38,0x44,0x44,0x44,0x20,0x00,0x00},
/* d */     {0x38,0x44,0x44,0x48,0x7F,0x00,0x00},
/* e */     {0x38,0x54,0x54,0x54,0x18,0x00,0x00},
/* f */     {0x08,0x7E,0x09,0x01,0x02,0x00,0x00},
/* g */     {0x0C,0x52,0x52,0x52,0x3E,0x00,0x00},
/* h */     {0x7F,0x08,0x04,0x04,0x78,0x00,0x00},
/* i */     {0x00,0x48,0x7A,0x40,0x00,0x00,0x00},
/* j */     {0x20,0x40,0x44,0x3D,0x00,0x00,0x00},
/* k */     {0x7F,0x10,0x28,0x44,0x00,0x00,0x00},
/* l */     {0x00,0x41,0x7F,0x40,0x00,0x00,0x00},
/* m */     {0x7C,0x04,0x18,0x04,0x78,0x00,0x00},
/* n */     {0x7C,0x08,0x04,0x04,0x78,0x00,0x00},
/* o */     {0x38,0x44,0x44,0x44,0x38,0x00,0x00},
/* p */     {0x7C,0x14,0x14,0x14,0x08,0x00,0x00},
/* q */     {0x08,0x14,0x14,0x18,0x7C,0x00,0x00},
/* r */     {0x7C,0x08,0x04,0x04,0x08,0x00,0x00},
/* s */     {0x48,0x54,0x54,0x54,0x20,0x00,0x00},
/* t */     {0x04,0x3F,0x44,0x40,0x20,0x00,0x00},
/* u */     {0x3C,0x40,0x40,0x20,0x7C,0x00,0x00},
/* v */     {0x1C,0x20,0x40,0x20,0x1C,0x00,0x00},
/* w */     {0x3C,0x40,0x30,0x40,0x3C,0x00,0x00},
/* x */     {0x44,0x28,0x10,0x28,0x44,0x00,0x00},
/* y */     {0x0C,0x50,0x50,0x50,0x3C,0x00,0x00},
/* z */     {0x44,0x64,0x54,0x4C,0x44,0x00,0x00},
/* { */     {0x00,0x08,0x36,0x41,0x00,0x00,0x00},
/* | */     {0x00,0x00,0x7F,0x00,0x00,0x00,0x00},
/* } */     {0x00,0x41,0x36,0x08,0x00,0x00,0x00},
/* ~ */     {0x02,0x01,0x02,0x04,0x02,0x00,0x00},
};

// Map an ASCII code to a FONT[] index (covering 0x20..0x7E).
static int fontIndexFor(char c) {
    unsigned u = (unsigned char)c;
    if (u < 0x20 || u > 0x7E) return 0x3F - 0x20; // '?' glyph
    return (int)(u - 0x20);
}

// ---------------------------------------------------------------------------
// Format-aware pixel packing. The badge textures are created with the SAME
// DXGI_FORMAT as the game output, so we must lay out each pixel in that
// format's byte order. Returns bytes-per-pixel for the common display formats
// (everything else is conservatively treated as 4).
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

// Stamp `str` into px (a w x h buffer of `bpp`-byte pixels) starting at (x0,y0),
// in `color` (already packed for the target format, `bpp` bytes). Each 5x7
// glyph pixel is expanded to `scale`x`scale` blocks so the badge stays legible
// when copied 1:1 into a full-resolution backbuffer. Bit0 of a row byte is
// column 0 (left), so we test (bits >> col) & 1. Out-of-range writes clipped.
static void drawStringScaled(std::vector<uint8_t>& px, int w, int h, int x0, int y0,
                             const char* str, const uint8_t* color, int bpp, int scale) {
    if (!str || scale < 1) return;
    int x = x0;
    for (const char* p = str; *p; ++p) {
        const uint8_t* g = FONT[fontIndexFor(*p)];
        for (int row = 0; row < 7; ++row) {
            uint8_t bits = g[row];
            for (int col = 0; col < 5; ++col) {
                if ((bits >> col) & 1) {
                    for (int dy = 0; dy < scale; ++dy) {
                        int gy = y0 + row * scale + dy;
                        for (int dx = 0; dx < scale; ++dx) {
                            int gx = x + col * scale + dx;
                            if (gx >= 0 && gx < w && gy >= 0 && gy < h) {
                                size_t off = ((size_t)gy * w + gx) * bpp;
                                memcpy(&px[off], color, bpp);
                            }
                        }
                    }
                }
            }
        }
        x += 6 * scale; // 5px glyph + 1px gap, scaled
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

ID3D12Resource* createWatermarkTexture(ID3D12Device* dev, ID3D12CommandQueue* queue,
                                        int& outW, int& outH, DXGI_FORMAT targetFmt,
                                        const char* fsrVersion, const char* qualityLevelName) {
    // Two-line badge: line 1 = FSR version, line 2 = upscaling quality level.
    // Glyphs are scaled up (SCALE x) so the badge stays readable when copied
    // 1:1 into a full-res backbuffer (5px text is invisible on 1080p+). A solid
    // dark panel sits behind the text for contrast over any scene. Colors are
    // packed for the game's ACTUAL output format (see packColor) so the panel
    // really is dark gray, not a blue field.
    const int SCALE = 3;
    const int GLYPH_W = 5 * SCALE;        // 15px per glyph cell (incl. 1px gap)
    const int GLYPH_H = 7 * SCALE;        // 21px per line
    const int PAD     = 8 * SCALE;        // 24px inner padding
    const int LINE_GAP = 6 * SCALE;       // 18px between the two lines
    const int bpp = formatBpp(targetFmt);
    uint8_t PANEL[8], TEXT[8];
    packColor(targetFmt, PANEL, 18, 18, 18, 235); // near-opaque dark gray
    packColor(targetFmt, TEXT,   0, 255, 0, 255); // opaque green

    auto lineWidth = [GLYPH_W](const char* s) -> int {
        int n = s ? (int)strlen(s) : 0;
        return n > 0 ? n * GLYPH_W : 0;
    };
    int wText = lineWidth(fsrVersion);
    int qw = lineWidth(qualityLevelName);
    if (qw > wText) wText = qw;
    int w = wText + PAD * 2;
    int h = PAD * 2 + GLYPH_H * 2 + LINE_GAP;

    std::vector<uint8_t> px((size_t)w * h * bpp, 0);
    for (size_t i = 0; i < (size_t)w * h; ++i) memcpy(&px[i * bpp], PANEL, bpp);

    // Line 1 (FSR version) and line 2 (quality preset), top-aligned in padding.
    int y1 = PAD;
    int y2 = PAD + GLYPH_H + LINE_GAP;
    drawStringScaled(px, w, h, PAD, y1, fsrVersion,       TEXT, bpp, SCALE);
    drawStringScaled(px, w, h, PAD, y2, qualityLevelName, TEXT, bpp, SCALE);

    ID3D12Resource* tex = createTexFromPixels(dev, queue, w, h, targetFmt, px);
    if (tex) { outW = w; outH = h; }
    return tex;
}

} // namespace Fsr4Overlay
