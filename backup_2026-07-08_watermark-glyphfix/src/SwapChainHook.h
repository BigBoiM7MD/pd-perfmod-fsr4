#pragma once
#include <d3d12.h>
#include <dxgi1_4.h>
#include <Windows.h>

class SwapChainHook {
public:
    bool init(ID3D12Device* device, ID3D12CommandQueue* queue);
    void setPendingCopy(ID3D12Resource* output, ID3D12Fence* fence, UINT64 fenceValue);
    void doCopy(ID3D12Resource* backbuffer);  // Call after upscaling completes with backbuffer
    void shutdown();

private:
    bool m_hooked = false;
    
    // Game resources
    ID3D12Device* m_device = nullptr;
    ID3D12CommandQueue* m_queue = nullptr;
    IDXGISwapChain* m_swapchain = nullptr;  // Game's actual swapchain
    
    // Copy resources (shared across all copies)
    ID3D12CommandAllocator* m_copyAlloc = nullptr;
    ID3D12GraphicsCommandList* m_copyCmdList = nullptr;
    ID3D12Fence* m_copyFence = nullptr;
    HANDLE m_copyFenceEvent = nullptr;
    UINT64 m_copyFenceValue = 0;
    
    // Pending copy state
    ID3D12Resource* m_pendingOutput = nullptr;
    ID3D12Fence* m_pendingFence = nullptr;
    UINT64 m_pendingFenceValue = 0;

    static SwapChainHook s_instance;
};
