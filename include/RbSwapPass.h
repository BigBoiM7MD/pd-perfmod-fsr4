#pragma once

#include <cstdint>
#include <d3d12.h>

// ---------------------------------------------------------------------------
// Red<->blue swap compute pass (Linux/Proton only)
// ---------------------------------------------------------------------------
// The Proton swapchain backbuffer PRESENTS as B8G8R8A8 (BGRA) even though
// REFramework's backbuffer GetDesc() reports R8G8B8A8 (28). REFramework copies
// our output with a raw CopyResource that vkd3d implements as vkCmdCopyImage2
// (no channel reorder). FSR4 always writes RGBA byte order, so a straight copy
// into the BGRA-presenting backbuffer swaps red/blue. This pass reads the RGBA
// FSR output via SRV and stores (b,g,r,a) into an RGBA-typed texture, i.e. the
// stored bytes are BGRA-ordered -> the raw copy then presents correct colors.
// SRV read + RGBA UAV store are baseline D3D12 (no Typed UAV Load cap needed).
//
// Pure C++17 + D3D12 header declarations; D3D12SerializeRootSignature is
// resolved dynamically from d3d12.dll so PDPerfPlugin.dll never static-links
// d3d12.lib or dxgi.lib.

struct RbSwapPipeline {
    ID3D12RootSignature* rootSig = nullptr;
    ID3D12PipelineState* pso     = nullptr;

    bool ensure(ID3D12Device* device);
    bool isReady() const { return rootSig != nullptr && pso != nullptr; }
    void release();
};

struct RbSwapResources {
    ID3D12Resource*       swapTexture = nullptr;
    ID3D12DescriptorHeap* swapHeap    = nullptr;

    static DXGI_FORMAT resolveSrvFormat(DXGI_FORMAT fmt);
    static DXGI_FORMAT resolveUavFormat(DXGI_FORMAT outFmt);

    bool create(ID3D12Device* device,
                ID3D12Resource* srcOutput,
                uint32_t width,
                uint32_t height,
                DXGI_FORMAT srcFmt = DXGI_FORMAT_UNKNOWN,
                DXGI_FORMAT dstFmt = DXGI_FORMAT_UNKNOWN);
    bool isReady() const { return swapTexture != nullptr && swapHeap != nullptr; }
    void release();
};

void recordRbSwap(const RbSwapPipeline& pipeline,
                  const RbSwapResources& resources,
                  ID3D12Resource* srcOutput,
                  uint32_t width,
                  uint32_t height,
                  ID3D12GraphicsCommandList* cmdList);
