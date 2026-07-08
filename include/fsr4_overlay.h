// fsr4_overlay.h
// ---------------------------------------------------------------------------
// Standalone, easily-debuggable helpers for the FSR4 output overlay:
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
// (The on-screen verification watermark was removed; FSR version + resolution
// scaling are reported from the log instead. See FSR4Backend::createContext.)
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
// (Unused by the current mod; kept for potential partial-overlay experiments.)
void blitToOutput(ID3D12GraphicsCommandList* cl,
                  ID3D12Resource* dst, int dstW, int dstH,
                  ID3D12Resource* src, int srcW, int srcH,
                  int destX, int destY, DXGI_FORMAT fmt);

} // namespace Fsr4Overlay
