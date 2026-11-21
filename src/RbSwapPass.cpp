#include "../include/RbSwapPass.h"
#include "../include/Logging.h"

#include <Windows.h>

// Precompiled DXBC for the red<->blue swap compute shader (src/shaders/rb_swap.hlsl).
// Embedded so we never rely on d3dcompiler at runtime (unreliable under Proton).
// Regenerate: fxc /nologo /T cs_5_0 /E CSMain /O3 /Fh src/shaders/rb_swap_cs.h /Vn g_rb_swap_cs src/shaders/rb_swap.hlsl
#include "shaders/rb_swap_cs.h"

void RbSwapPipeline::release() {
    if (pso) {
        try {
            pso->Release();
        } catch (...) {}
        pso = nullptr;
    }
    if (rootSig) {
        try {
            rootSig->Release();
        } catch (...) {}
        rootSig = nullptr;
    }
}

bool RbSwapPipeline::ensure(ID3D12Device* device) {
    if (pso && rootSig) {
        return true;
    }
    if (!device) {
        return false;
    }

    release();

    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors                    = 1;
    ranges[0].BaseShaderRegister                = 0; // t0
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors                    = 1;
    ranges[1].BaseShaderRegister                = 0; // u0
    ranges[1].OffsetInDescriptorsFromTableStart = 1;

    D3D12_ROOT_PARAMETER param = {};
    param.ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    param.ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
    param.DescriptorTable.NumDescriptorRanges = 2;
    param.DescriptorTable.pDescriptorRanges   = ranges;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = 1;
    rsDesc.pParameters   = &param;
    rsDesc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    struct RbSwapInitGuard {
        bool loadedD3d12              = false;
        HMODULE d3d12Mod              = nullptr;
        ID3DBlob* sigBlob             = nullptr;
        ID3DBlob* errBlob             = nullptr;
        ID3D12RootSignature** rootSig = nullptr;
        bool committed                = false;

        void releaseTemp() noexcept {
            if (errBlob) {
                try { errBlob->Release(); } catch (...) {}
                errBlob = nullptr;
            }
            if (sigBlob) {
                try { sigBlob->Release(); } catch (...) {}
                sigBlob = nullptr;
            }
            if (loadedD3d12 && d3d12Mod) {
                try { FreeLibrary(d3d12Mod); } catch (...) {}
                d3d12Mod    = nullptr;
                loadedD3d12 = false;
            }
        }

        ~RbSwapInitGuard() {
            if (!committed && rootSig && *rootSig) {
                try { (*rootSig)->Release(); } catch (...) {}
                *rootSig = nullptr;
            }
            releaseTemp();
        }
    } guard;
    guard.rootSig = &rootSig;

    guard.d3d12Mod = GetModuleHandleW(L"d3d12.dll");
    if (!guard.d3d12Mod) {
        guard.d3d12Mod    = LoadLibraryW(L"d3d12.dll");
        guard.loadedD3d12 = (guard.d3d12Mod != nullptr);
    }

    using PFN_D3D12SerializeRootSignature = HRESULT (WINAPI*)(
        const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION,
        ID3DBlob**, ID3DBlob**);
    auto serializeRootSig = guard.d3d12Mod
        ? reinterpret_cast<PFN_D3D12SerializeRootSignature>(
              GetProcAddress(guard.d3d12Mod, "D3D12SerializeRootSignature"))
        : nullptr;
    if (!serializeRootSig) {
        Logging::error("FSR4Backend: rb-swap failed to resolve d3d12.dll!D3D12SerializeRootSignature (err=%lu)",
                       GetLastError());
        return false;
    }

    HRESULT hr = serializeRootSig(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                  &guard.sigBlob, &guard.errBlob);
    if (FAILED(hr) || !guard.sigBlob) {
        Logging::error("FSR4Backend: rb-swap SerializeRootSignature hr=%08x", hr);
        return false;
    }

    hr = device->CreateRootSignature(0, guard.sigBlob->GetBufferPointer(),
                                     guard.sigBlob->GetBufferSize(), IID_PPV_ARGS(&rootSig));
    guard.releaseTemp();
    if (FAILED(hr) || !rootSig) {
        Logging::error("FSR4Backend: rb-swap CreateRootSignature hr=%08x ptr=%p",
                       hr, static_cast<void*>(rootSig));
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature     = rootSig;
    psoDesc.CS.pShaderBytecode = g_rb_swap_cs;
    psoDesc.CS.BytecodeLength  = sizeof(g_rb_swap_cs);
    hr = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pso));
    if (FAILED(hr) || !pso) {
        Logging::error("FSR4Backend: rb-swap CreateComputePipelineState hr=%08x ptr=%p",
                       hr, static_cast<void*>(pso));
        if (pso) {
            try { pso->Release(); } catch (...) {}
            pso = nullptr;
        }
        return false;
    }

    guard.committed = true;
    Logging::info("FSR4Backend: rb-swap compute pipeline created");
    return true;
}

DXGI_FORMAT RbSwapResources::resolveSrvFormat(DXGI_FORMAT fmt) {
    switch (fmt) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:     return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:     return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:  return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        default:                                return fmt;
    }
}

DXGI_FORMAT RbSwapResources::resolveUavFormat(DXGI_FORMAT outFmt) {
    switch (outFmt) {
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
            return DXGI_FORMAT_R10G10B10A2_UNORM;
        default:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

void RbSwapResources::release() {
    if (swapTexture) {
        try {
            swapTexture->Release();
        } catch (...) {
            Logging::warn("FSR4Backend: swapTexture->Release threw during cleanup");
        }
        swapTexture = nullptr;
    }
    if (swapHeap) {
        try {
            swapHeap->Release();
        } catch (...) {
            Logging::warn("FSR4Backend: swapHeap->Release threw during cleanup");
        }
        swapHeap = nullptr;
    }
}

bool RbSwapResources::create(ID3D12Device* device,
                             ID3D12Resource* srcOutput,
                             uint32_t width,
                             uint32_t height,
                             DXGI_FORMAT srcFmt,
                             DXGI_FORMAT dstFmt) {
    release();

    if (!device || !srcOutput || width == 0 || height == 0) {
        return false;
    }

    DXGI_FORMAT srvFmt  = srcFmt;
    DXGI_FORMAT swapFmt = dstFmt;
    if (srvFmt == DXGI_FORMAT_UNKNOWN || swapFmt == DXGI_FORMAT_UNKNOWN) {
        const DXGI_FORMAT outTexFmt = srcOutput->GetDesc().Format;
        if (srvFmt == DXGI_FORMAT_UNKNOWN) {
            srvFmt = resolveSrvFormat(outTexFmt);
        }
        if (swapFmt == DXGI_FORMAT_UNKNOWN) {
            swapFmt = resolveUavFormat(outTexFmt);
        }
    }

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width            = width;
    desc.Height           = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels        = 1;
    desc.Format           = swapFmt;
    desc.SampleDesc.Count = 1;
    desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
        &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
        IID_PPV_ARGS(&swapTexture));
    if (FAILED(hr) || !swapTexture) {
        Logging::error("FSR4Backend: rb-swap texture create hr=%08x (fmt=%d)", hr, static_cast<int>(swapFmt));
        release();
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 2;
    heapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&swapHeap));
    if (FAILED(hr) || !swapHeap) {
        Logging::error("FSR4Backend: rb-swap heap create hr=%08x", hr);
        release();
        return false;
    }

    const UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = swapHeap->GetCPUDescriptorHandleForHeapStart();

    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format                  = srvFmt;
    srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels     = 1;
    device->CreateShaderResourceView(srcOutput, &srv, cpu);

    cpu.ptr += inc;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.Format        = swapFmt;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(swapTexture, nullptr, &uav, cpu);

    return true;
}

void recordRbSwap(const RbSwapPipeline& pipeline,
                  const RbSwapResources& resources,
                  ID3D12Resource* srcOutput,
                  uint32_t width,
                  uint32_t height,
                  ID3D12GraphicsCommandList* cmdList) {
    if (!pipeline.isReady() || !resources.isReady() || !srcOutput || !cmdList || width == 0 || height == 0) {
        return;
    }

    D3D12_RESOURCE_BARRIER toSrv = {};
    toSrv.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toSrv.Transition.pResource   = srcOutput;
    toSrv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toSrv.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    toSrv.Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    cmdList->ResourceBarrier(1, &toSrv);

    ID3D12DescriptorHeap* heaps[] = { resources.swapHeap };
    cmdList->SetDescriptorHeaps(1, heaps);
    cmdList->SetComputeRootSignature(pipeline.rootSig);
    cmdList->SetPipelineState(pipeline.pso);
    cmdList->SetComputeRootDescriptorTable(0, resources.swapHeap->GetGPUDescriptorHandleForHeapStart());

    const UINT gx = (width + 7u) / 8u;
    const UINT gy = (height + 7u) / 8u;
    cmdList->Dispatch(gx, gy, 1);

    D3D12_RESOURCE_BARRIER postBarriers[2] = {};
    postBarriers[0].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    postBarriers[0].Transition.pResource   = srcOutput;
    postBarriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    postBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    postBarriers[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    postBarriers[1].Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    postBarriers[1].UAV.pResource = resources.swapTexture;

    cmdList->ResourceBarrier(2, postBarriers);
}
