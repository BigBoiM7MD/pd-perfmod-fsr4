#include "SwapChainHook.h"
#include "Logging.h"
#include "../include/PDPerfPlugin.h"

SwapChainHook SwapChainHook::s_instance;

// FFX API types
using ffxReturnCode_t = uint32_t;
using ffxContext      = void*;
using ffxStructType_t = uint64_t;

// Simplified SwapChainHook - tracks FSR4 output for manual copy to backbuffer
bool SwapChainHook::init(ID3D12Device* device, ID3D12CommandQueue* queue) {
    m_device = device;
    m_queue = queue;
    m_device->AddRef();
    m_queue->AddRef();

    // Create shared copy resources (single allocator/list/fence for all copies)
    if (FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_copyFence)))) {
        Logging::error("SCHook: Failed to create copy fence");
        return false;
    }
    m_copyFenceEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!m_copyFenceEvent) return false;

    if (FAILED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&m_copyAlloc)))) return false;
    if (FAILED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            m_copyAlloc, nullptr, IID_PPV_ARGS(&m_copyCmdList)))) return false;
    m_copyCmdList->Close();

    m_hooked = true;
    Logging::info("SCHook: Initialized (manual copy mode)");
    return true;
}

void SwapChainHook::setPendingCopy(ID3D12Resource* output, ID3D12Fence* fence, UINT64 fenceValue) {
    m_pendingOutput = output;
    m_pendingFence = fence;
    m_pendingFenceValue = fenceValue;
}

void SwapChainHook::doCopy(ID3D12Resource* backbuffer) {
    if (!m_pendingOutput || !backbuffer || !m_device || !m_queue) return;

    // Wait for FSR4 output to be ready
    if (m_pendingFence && m_pendingFenceValue > 0 &&
        m_pendingFence->GetCompletedValue() < m_pendingFenceValue) {
        m_pendingFence->SetEventOnCompletion(m_pendingFenceValue, m_copyFenceEvent);
        WaitForSingleObject(m_copyFenceEvent, INFINITE);
    }

    // Use shared copy command list
    if (!m_copyAlloc || !m_copyCmdList) return;

    if (FAILED(m_copyAlloc->Reset())) return;
    if (FAILED(m_copyCmdList->Reset(m_copyAlloc, nullptr))) return;

    // Transition backbuffer to COPY_DEST
    D3D12_RESOURCE_BARRIER bbBarrier = {};
    bbBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bbBarrier.Transition.pResource = backbuffer;
    bbBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    bbBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    m_copyCmdList->ResourceBarrier(1, &bbBarrier);

    // Transition FSR4 output to COPY_SOURCE
    D3D12_RESOURCE_BARRIER outBarrier = {};
    outBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    outBarrier.Transition.pResource = m_pendingOutput;
    outBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    outBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    m_copyCmdList->ResourceBarrier(1, &outBarrier);

    // Copy FSR4 output to backbuffer
    m_copyCmdList->CopyResource(backbuffer, m_pendingOutput);

    // Transition backbuffer to PRESENT
    bbBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    bbBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    m_copyCmdList->ResourceBarrier(1, &bbBarrier);

    // Transition FSR4 output back to UAV
    outBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    outBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    m_copyCmdList->ResourceBarrier(1, &outBarrier);

    // Execute copy command list
    ID3D12CommandList* lists[] = { m_copyCmdList };
    m_queue->ExecuteCommandLists(1, lists);
    
    // Signal fence for copy completion tracking
    ++m_copyFenceValue;
    if (m_copyFence->GetCompletedValue() < m_copyFenceValue) {
        m_copyFence->SetEventOnCompletion(m_copyFenceValue, m_copyFenceEvent);
        WaitForSingleObject(m_copyFenceEvent, INFINITE);
    }

    backbuffer->Release();

    // Reset pending state
    m_pendingOutput = nullptr;
    m_pendingFence = nullptr;
    m_pendingFenceValue = 0;
}

void SwapChainHook::shutdown() {
    if (m_copyCmdList) { m_copyCmdList->Release(); m_copyCmdList = nullptr; }
    if (m_copyAlloc) { m_copyAlloc->Release(); m_copyAlloc = nullptr; }
    if (m_copyFence) { m_copyFence->Release(); m_copyFence = nullptr; }
    if (m_copyFenceEvent) { CloseHandle(m_copyFenceEvent); m_copyFenceEvent = nullptr; }
    if (m_queue) { m_queue->Release(); m_queue = nullptr; }
    if (m_device) { m_device->Release(); m_device = nullptr; }
}
