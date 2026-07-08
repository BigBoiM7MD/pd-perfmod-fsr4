// fsr4_overlay.h
// ---------------------------------------------------------------------------
// Standalone, easily-debuggable helpers for the FSR4 output "overlay":
//   * the verification watermark ("FSR4" badge) -- createWatermarkTexture()
//   * the full-frame solid-green DIAG texture (PD_FSR4_DIAG_SOLID) -- createSolidTexture()
//
// WHY THIS IS A SEPARATE FILE
//   The original code created these R10G10B10A2 textures with WriteToSubresource
//   and with UPLOAD-heap staging textures. BOTH fail on real AMD drivers:
//     - R10G10B10A2 (the FSR4 output format, DXGI fmt 24) CANNOT live in an
//       UPLOAD heap -> CreateCommittedResource returns E_INVALIDARG (hr=80070057).
//     - WriteToSubresource against a DEFAULT COPY_SOURCE texture also returns
//       80070057.
//   The proven-correct path (validated on D3D12 WARP) is:
//     DEFAULT-heap texture (COPY_DEST) <-- CopyTextureRegion <-- UPLOAD-heap
//     BUFFER (format-agnostic, so no R10G10B10A2 restriction) <-- Map.
//   Keeping this in its own TU makes it trivial to unit-test and to re-verify
//   whenever a new REFramework / FidelityFX SDK release changes the output format.
//
// Requires LOGGING via the plugin's Logging::info/error (Logging.h).
// ---------------------------------------------------------------------------
#pragma once
#include <d3d12.h>
#include <vector>
#include <cstdint>

namespace Fsr4Overlay {

// Fill a DEFAULT-heap texture of `targetFmt` from CPU pixels using an
// UPLOAD-heap buffer + CopyTextureRegion. Returns nullptr on failure.
// `queue` must be a DIRECT queue able to execute a one-shot copy + fence wait.
ID3D12Resource* createTexFromPixels(ID3D12Device* dev,
                                     ID3D12CommandQueue* queue,
                                     int w, int h,
                                     DXGI_FORMAT targetFmt,
                                     const std::vector<uint8_t>& px);

// Full-frame solid-color texture (PD_FSR4_DIAG_SOLID). `colorArgb` is
// 0xAARRGGBB (repacked into the real output format before upload).
ID3D12Resource* createSolidTexture(ID3D12Device* dev,
                                    ID3D12CommandQueue* queue,
                                    int w, int h,
                                    DXGI_FORMAT targetFmt,
                                    uint32_t colorArgb);

// Blit `src` (w x h, same format as `dst`) into `dst` at (destX,destY) on the
// given command list. Uses a BUFFER placed-footprint source so the partial,
// offset copy is VALID (texture->texture CopyTextureRegion ignores DstX/Y and
// requires identical sizes -- which would scramble a larger destination).
void blitToOutput(ID3D12GraphicsCommandList* cl,
                  ID3D12Resource* dst, int dstW, int dstH,
                  ID3D12Resource* src, int srcW, int srcH,
                  int destX, int destY, DXGI_FORMAT fmt);

// Verification watermark texture. Renders a two-line badge so the on-screen
// overlay reports what the mod is actually running:
//   line 1: the FSR version string (e.g. "FSR4 V4.1.1")
//   line 2: the upscaling quality level (e.g. "ULTRA PERFORMANCE")
// `fsrVersion` and `qualityLevelName` are plain ASCII passed in by the caller
// (they are formatted from FFX_UPSCALER_VERSION and the quality enum in the
// backend). `dispW`/`dispH` are the OUTPUT backbuffer size; the badge is
// scaled relative to them so it stays a readable fraction of screen height on
// 1080p, 1440p, and 4K. Writes outW/outH.
ID3D12Resource* createWatermarkTexture(ID3D12Device* dev,
                                       ID3D12CommandQueue* queue,
                                       int& outW, int& outH,
                                       DXGI_FORMAT targetFmt,
                                       const char* fsrVersion,
                                       const char* qualityLevelName,
                                       int dispW, int dispH);

} // namespace Fsr4Overlay
