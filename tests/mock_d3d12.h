#pragma once

#include <Windows.h>
#include <d3d12.h>
#include <atomic>
#include <cstdint>

class MockD3D12Resource;
class MockD3D12CommandAllocator;
class MockD3D12GraphicsCommandList;
class MockD3D12Fence;
class MockD3D12CommandQueue;

class MockD3D12Device : public ID3D12Device {
public:
    inline static std::atomic<int> liveDevices{0};
    inline static std::atomic<int> liveQueues{0};
    inline static std::atomic<int> liveResources{0};
    inline static std::atomic<int> liveAllocators{0};
    inline static std::atomic<int> liveLists{0};
    inline static std::atomic<int> liveFences{0};

    bool failCreateCommittedResource = false;
    bool failCreateCommandAllocator  = false;
    bool failCreateCommandList       = false;
    bool failCreateFence             = false;

    D3D12_RESOURCE_DESC   lastCreatedDesc{};
    D3D12_RESOURCE_STATES lastInitialState = D3D12_RESOURCE_STATE_COMMON;
    LUID                  adapterLuid{0, 0};

    MockD3D12Device() {
        liveDevices.fetch_add(1, std::memory_order_relaxed);
    }

    virtual ~MockD3D12Device() {
        liveDevices.fetch_sub(1, std::memory_order_relaxed);
    }

    LONG refCount() const {
        return m_refCount.load(std::memory_order_acquire);
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        *ppvObject = nullptr;
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(ID3D12Object) ||
            riid == __uuidof(ID3D12Device)) {
            *ppvObject = static_cast<ID3D12Device*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(m_refCount.fetch_add(1, std::memory_order_relaxed) + 1);
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG next = static_cast<ULONG>(m_refCount.fetch_sub(1, std::memory_order_acq_rel) - 1);
        if (next == 0) {
            delete this;
        }
        return next;
    }

    // ID3D12Object
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override { return S_OK; }

    // ID3D12Device
    UINT STDMETHODCALLTYPE GetNodeCount() override { return 1; }
    HRESULT STDMETHODCALLTYPE CreateCommandQueue(const D3D12_COMMAND_QUEUE_DESC*, REFIID, void** pp) override {
        if (pp) *pp = nullptr;
        return E_NOTIMPL;
    }
    inline HRESULT STDMETHODCALLTYPE CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE type, REFIID riid, void** ppCommandAllocator) override;
    HRESULT STDMETHODCALLTYPE CreateGraphicsPipelineState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void** pp) override {
        if (pp) *pp = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID, void** pp) override {
        if (pp) *pp = nullptr;
        return E_NOTIMPL;
    }
    inline HRESULT STDMETHODCALLTYPE CreateCommandList(
        UINT nodeMask, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator* pCommandAllocator,
        ID3D12PipelineState* pInitialState, REFIID riid, void** ppCommandList) override;
    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(D3D12_FEATURE, void*, UINT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID, void** pp) override {
        if (pp) *pp = nullptr;
        return E_NOTIMPL;
    }
    UINT STDMETHODCALLTYPE GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE) override { return 32; }
    HRESULT STDMETHODCALLTYPE CreateRootSignature(UINT, const void*, SIZE_T, REFIID, void** pp) override {
        if (pp) *pp = nullptr;
        return E_NOTIMPL;
    }
    void STDMETHODCALLTYPE CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE) override {}
    void STDMETHODCALLTYPE CreateShaderResourceView(ID3D12Resource*, const D3D12_SHADER_RESOURCE_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE) override {}
    void STDMETHODCALLTYPE CreateUnorderedAccessView(ID3D12Resource*, ID3D12Resource*, const D3D12_UNORDERED_ACCESS_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE) override {}
    void STDMETHODCALLTYPE CreateRenderTargetView(ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE) override {}
    void STDMETHODCALLTYPE CreateDepthStencilView(ID3D12Resource*, const D3D12_DEPTH_STENCIL_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE) override {}
    void STDMETHODCALLTYPE CreateSampler(const D3D12_SAMPLER_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE) override {}
    void STDMETHODCALLTYPE CopyDescriptors(UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, D3D12_DESCRIPTOR_HEAP_TYPE) override {}
    void STDMETHODCALLTYPE CopyDescriptorsSimple(UINT, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_DESCRIPTOR_HEAP_TYPE) override {}
    D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE GetResourceAllocationInfo(UINT, UINT, const D3D12_RESOURCE_DESC*) override { return {}; }
    D3D12_HEAP_PROPERTIES STDMETHODCALLTYPE GetCustomHeapProperties(UINT, D3D12_HEAP_TYPE) override { return {}; }
    inline HRESULT STDMETHODCALLTYPE CreateCommittedResource(
        const D3D12_HEAP_PROPERTIES* pHeapProperties, D3D12_HEAP_FLAGS HeapFlags,
        const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialResourceState,
        const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riidResource, void** ppvResource) override;
    HRESULT STDMETHODCALLTYPE CreateHeap(const D3D12_HEAP_DESC*, REFIID, void** pp) override {
        if (pp) *pp = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE CreatePlacedResource(ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void** pp) override {
        if (pp) *pp = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE CreateReservedResource(const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void** pp) override {
        if (pp) *pp = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE CreateSharedHandle(ID3D12DeviceChild*, const SECURITY_ATTRIBUTES*, DWORD, LPCWSTR, HANDLE* ph) override {
        if (ph) *ph = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE OpenSharedHandle(HANDLE, REFIID, void** pp) override {
        if (pp) *pp = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE OpenSharedHandleByName(LPCWSTR, DWORD, HANDLE* ph) override {
        if (ph) *ph = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE MakeResident(UINT, ID3D12Pageable* const*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Evict(UINT, ID3D12Pageable* const*) override { return S_OK; }
    inline HRESULT STDMETHODCALLTYPE CreateFence(
        UINT64 InitialValue, D3D12_FENCE_FLAGS Flags, REFIID riid, void** ppFence) override;
    HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() override { return S_OK; }
    void STDMETHODCALLTYPE GetCopyableFootprints(const D3D12_RESOURCE_DESC*, UINT, UINT, UINT64, D3D12_PLACED_SUBRESOURCE_FOOTPRINT*, UINT*, UINT64*, UINT64*) override {}
    HRESULT STDMETHODCALLTYPE CreateQueryHeap(const D3D12_QUERY_HEAP_DESC*, REFIID, void** pp) override {
        if (pp) *pp = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetStablePowerState(BOOL) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE CreateCommandSignature(const D3D12_COMMAND_SIGNATURE_DESC*, ID3D12RootSignature*, REFIID, void** pp) override {
        if (pp) *pp = nullptr;
        return E_NOTIMPL;
    }
    void STDMETHODCALLTYPE GetResourceTiling(ID3D12Resource*, UINT*, D3D12_PACKED_MIP_INFO*, D3D12_TILE_SHAPE*, UINT*, UINT, D3D12_SUBRESOURCE_TILING*) override {}
    LUID STDMETHODCALLTYPE GetAdapterLuid() override { return adapterLuid; }

private:
    std::atomic<LONG> m_refCount{1};
};

class MockD3D12Resource : public ID3D12Resource {
public:
    MockD3D12Resource(ID3D12Device* device, const D3D12_RESOURCE_DESC& desc)
        : m_device(device), m_desc(desc) {
        MockD3D12Device::liveResources.fetch_add(1, std::memory_order_relaxed);
    }

    virtual ~MockD3D12Resource() {
        MockD3D12Device::liveResources.fetch_sub(1, std::memory_order_relaxed);
    }

    LONG refCount() const { return m_refCount.load(std::memory_order_acquire); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        *ppvObject = nullptr;
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(ID3D12Object) ||
            riid == __uuidof(ID3D12DeviceChild) ||
            riid == __uuidof(ID3D12Pageable) ||
            riid == __uuidof(ID3D12Resource)) {
            *ppvObject = static_cast<ID3D12Resource*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(m_refCount.fetch_add(1, std::memory_order_relaxed) + 1);
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG next = static_cast<ULONG>(m_refCount.fetch_sub(1, std::memory_order_acq_rel) - 1);
        if (next == 0) delete this;
        return next;
    }

    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppvDevice) override {
        if (!ppvDevice) return E_POINTER;
        *ppvDevice = nullptr;
        return m_device ? m_device->QueryInterface(riid, ppvDevice) : E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE Map(UINT, const D3D12_RANGE*, void** ppData) override {
        if (ppData) *ppData = nullptr;
        return E_NOTIMPL;
    }
    void STDMETHODCALLTYPE Unmap(UINT, const D3D12_RANGE*) override {}
    D3D12_RESOURCE_DESC STDMETHODCALLTYPE GetDesc() override { return m_desc; }
    D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE GetGPUVirtualAddress() override { return 0; }
    HRESULT STDMETHODCALLTYPE WriteToSubresource(UINT, const D3D12_BOX*, const void*, UINT, UINT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ReadFromSubresource(void*, UINT, UINT, UINT, const D3D12_BOX*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetHeapProperties(D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS*) override { return E_NOTIMPL; }

private:
    std::atomic<LONG> m_refCount{1};
    ID3D12Device* m_device;
    D3D12_RESOURCE_DESC m_desc;
};

class MockD3D12CommandAllocator : public ID3D12CommandAllocator {
public:
    explicit MockD3D12CommandAllocator(ID3D12Device* device) : m_device(device) {
        MockD3D12Device::liveAllocators.fetch_add(1, std::memory_order_relaxed);
    }

    virtual ~MockD3D12CommandAllocator() {
        MockD3D12Device::liveAllocators.fetch_sub(1, std::memory_order_relaxed);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        *ppvObject = nullptr;
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(ID3D12Object) ||
            riid == __uuidof(ID3D12DeviceChild) ||
            riid == __uuidof(ID3D12Pageable) ||
            riid == __uuidof(ID3D12CommandAllocator)) {
            *ppvObject = static_cast<ID3D12CommandAllocator*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(m_refCount.fetch_add(1, std::memory_order_relaxed) + 1);
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG next = static_cast<ULONG>(m_refCount.fetch_sub(1, std::memory_order_acq_rel) - 1);
        if (next == 0) delete this;
        return next;
    }

    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppvDevice) override {
        if (!ppvDevice) return E_POINTER;
        *ppvDevice = nullptr;
        return m_device ? m_device->QueryInterface(riid, ppvDevice) : E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Reset() override { return S_OK; }

private:
    std::atomic<LONG> m_refCount{1};
    ID3D12Device* m_device;
};

class MockD3D12GraphicsCommandList : public ID3D12GraphicsCommandList {
public:
    explicit MockD3D12GraphicsCommandList(ID3D12Device* device) : m_device(device) {
        MockD3D12Device::liveLists.fetch_add(1, std::memory_order_relaxed);
    }

    virtual ~MockD3D12GraphicsCommandList() {
        MockD3D12Device::liveLists.fetch_sub(1, std::memory_order_relaxed);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        *ppvObject = nullptr;
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(ID3D12Object) ||
            riid == __uuidof(ID3D12DeviceChild) ||
            riid == __uuidof(ID3D12CommandList) ||
            riid == __uuidof(ID3D12GraphicsCommandList)) {
            *ppvObject = static_cast<ID3D12GraphicsCommandList*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(m_refCount.fetch_add(1, std::memory_order_relaxed) + 1);
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG next = static_cast<ULONG>(m_refCount.fetch_sub(1, std::memory_order_acq_rel) - 1);
        if (next == 0) delete this;
        return next;
    }

    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppvDevice) override {
        if (!ppvDevice) return E_POINTER;
        *ppvDevice = nullptr;
        return m_device ? m_device->QueryInterface(riid, ppvDevice) : E_NOINTERFACE;
    }

    D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE GetType() override { return D3D12_COMMAND_LIST_TYPE_DIRECT; }
    HRESULT STDMETHODCALLTYPE Close() override { return failClose ? E_FAIL : S_OK; }
    HRESULT STDMETHODCALLTYPE Reset(ID3D12CommandAllocator*, ID3D12PipelineState*) override { return S_OK; }
    void STDMETHODCALLTYPE ClearState(ID3D12PipelineState*) override {}
    void STDMETHODCALLTYPE DrawInstanced(UINT, UINT, UINT, UINT) override {}
    void STDMETHODCALLTYPE DrawIndexedInstanced(UINT, UINT, UINT, INT, UINT) override {}
    void STDMETHODCALLTYPE Dispatch(UINT, UINT, UINT) override {}
    void STDMETHODCALLTYPE CopyBufferRegion(ID3D12Resource*, UINT64, ID3D12Resource*, UINT64, UINT64) override {}
    void STDMETHODCALLTYPE CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT, UINT, const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*) override {}
    void STDMETHODCALLTYPE CopyResource(ID3D12Resource*, ID3D12Resource*) override {}
    void STDMETHODCALLTYPE CopyTiles(ID3D12Resource*, const D3D12_TILED_RESOURCE_COORDINATE*, const D3D12_TILE_REGION_SIZE*, ID3D12Resource*, UINT64, D3D12_TILE_COPY_FLAGS) override {}
    void STDMETHODCALLTYPE ResolveSubresource(ID3D12Resource*, UINT, ID3D12Resource*, UINT, DXGI_FORMAT) override {}
    void STDMETHODCALLTYPE IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY) override {}
    void STDMETHODCALLTYPE RSSetViewports(UINT, const D3D12_VIEWPORT*) override {}
    void STDMETHODCALLTYPE RSSetScissorRects(UINT, const D3D12_RECT*) override {}
    void STDMETHODCALLTYPE OMSetBlendFactor(const FLOAT[4]) override {}
    void STDMETHODCALLTYPE OMSetStencilRef(UINT) override {}
    void STDMETHODCALLTYPE SetPipelineState(ID3D12PipelineState*) override {}
    void STDMETHODCALLTYPE ResourceBarrier(UINT, const D3D12_RESOURCE_BARRIER*) override {}
    void STDMETHODCALLTYPE ExecuteBundle(ID3D12GraphicsCommandList*) override {}
    void STDMETHODCALLTYPE SetDescriptorHeaps(UINT, ID3D12DescriptorHeap* const*) override {}
    void STDMETHODCALLTYPE SetComputeRootSignature(ID3D12RootSignature*) override {}
    void STDMETHODCALLTYPE SetGraphicsRootSignature(ID3D12RootSignature*) override {}
    void STDMETHODCALLTYPE SetComputeRootDescriptorTable(UINT, D3D12_GPU_DESCRIPTOR_HANDLE) override {}
    void STDMETHODCALLTYPE SetGraphicsRootDescriptorTable(UINT, D3D12_GPU_DESCRIPTOR_HANDLE) override {}
    void STDMETHODCALLTYPE SetComputeRoot32BitConstant(UINT, UINT, UINT) override {}
    void STDMETHODCALLTYPE SetGraphicsRoot32BitConstant(UINT, UINT, UINT) override {}
    void STDMETHODCALLTYPE SetComputeRoot32BitConstants(UINT, UINT, const void*, UINT) override {}
    void STDMETHODCALLTYPE SetGraphicsRoot32BitConstants(UINT, UINT, const void*, UINT) override {}
    void STDMETHODCALLTYPE SetComputeRootConstantBufferView(UINT, D3D12_GPU_VIRTUAL_ADDRESS) override {}
    void STDMETHODCALLTYPE SetGraphicsRootConstantBufferView(UINT, D3D12_GPU_VIRTUAL_ADDRESS) override {}
    void STDMETHODCALLTYPE SetComputeRootShaderResourceView(UINT, D3D12_GPU_VIRTUAL_ADDRESS) override {}
    void STDMETHODCALLTYPE SetGraphicsRootShaderResourceView(UINT, D3D12_GPU_VIRTUAL_ADDRESS) override {}
    void STDMETHODCALLTYPE SetComputeRootUnorderedAccessView(UINT, D3D12_GPU_VIRTUAL_ADDRESS) override {}
    void STDMETHODCALLTYPE SetGraphicsRootUnorderedAccessView(UINT, D3D12_GPU_VIRTUAL_ADDRESS) override {}
    void STDMETHODCALLTYPE IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW*) override {}
    void STDMETHODCALLTYPE IASetVertexBuffers(UINT, UINT, const D3D12_VERTEX_BUFFER_VIEW*) override {}
    void STDMETHODCALLTYPE SOSetTargets(UINT, UINT, const D3D12_STREAM_OUTPUT_BUFFER_VIEW*) override {}
    void STDMETHODCALLTYPE OMSetRenderTargets(UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*) override {}
    void STDMETHODCALLTYPE ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS, FLOAT, UINT8, UINT, const D3D12_RECT*) override {}
    void STDMETHODCALLTYPE ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE, const FLOAT[4], UINT, const D3D12_RECT*) override {}
    void STDMETHODCALLTYPE ClearUnorderedAccessViewUint(D3D12_GPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, ID3D12Resource*, const UINT[4], UINT, const D3D12_RECT*) override {}
    void STDMETHODCALLTYPE ClearUnorderedAccessViewFloat(D3D12_GPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, ID3D12Resource*, const FLOAT[4], UINT, const D3D12_RECT*) override {}
    void STDMETHODCALLTYPE DiscardResource(ID3D12Resource*, const D3D12_DISCARD_REGION*) override {}
    void STDMETHODCALLTYPE BeginQuery(ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT) override {}
    void STDMETHODCALLTYPE EndQuery(ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT) override {}
    void STDMETHODCALLTYPE ResolveQueryData(ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT, UINT, ID3D12Resource*, UINT64) override {}
    void STDMETHODCALLTYPE SetPredication(ID3D12Resource*, UINT64, D3D12_PREDICATION_OP) override {}
    void STDMETHODCALLTYPE SetMarker(UINT, const void*, UINT) override {}
    void STDMETHODCALLTYPE BeginEvent(UINT, const void*, UINT) override {}
    void STDMETHODCALLTYPE EndEvent() override {}
    void STDMETHODCALLTYPE ExecuteIndirect(ID3D12CommandSignature*, UINT, ID3D12Resource*, UINT64, ID3D12Resource*, UINT64) override {}

    bool failClose = false;

private:
    std::atomic<LONG> m_refCount{1};
    ID3D12Device* m_device;
};

class MockD3D12Fence : public ID3D12Fence {
public:
    MockD3D12Fence(ID3D12Device* device, UINT64 initialValue)
        : m_device(device), m_value(initialValue) {
        MockD3D12Device::liveFences.fetch_add(1, std::memory_order_relaxed);
    }

    virtual ~MockD3D12Fence() {
        MockD3D12Device::liveFences.fetch_sub(1, std::memory_order_relaxed);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        *ppvObject = nullptr;
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(ID3D12Object) ||
            riid == __uuidof(ID3D12DeviceChild) ||
            riid == __uuidof(ID3D12Pageable) ||
            riid == __uuidof(ID3D12Fence)) {
            *ppvObject = static_cast<ID3D12Fence*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(m_refCount.fetch_add(1, std::memory_order_relaxed) + 1);
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG next = static_cast<ULONG>(m_refCount.fetch_sub(1, std::memory_order_acq_rel) - 1);
        if (next == 0) delete this;
        return next;
    }

    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppvDevice) override {
        if (!ppvDevice) return E_POINTER;
        *ppvDevice = nullptr;
        return m_device ? m_device->QueryInterface(riid, ppvDevice) : E_NOINTERFACE;
    }

    UINT64 STDMETHODCALLTYPE GetCompletedValue() override { return m_value.load(std::memory_order_acquire); }
    HRESULT STDMETHODCALLTYPE SetEventOnCompletion(UINT64, HANDLE hEvent) override {
        if (hEvent) SetEvent(hEvent);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Signal(UINT64 Value) override {
        m_value.store(Value, std::memory_order_release);
        return S_OK;
    }

private:
    std::atomic<LONG> m_refCount{1};
    ID3D12Device* m_device;
    std::atomic<UINT64> m_value{0};
};

class MockD3D12CommandQueue : public ID3D12CommandQueue {
public:
    explicit MockD3D12CommandQueue(ID3D12Device* device) : m_device(device) {
        MockD3D12Device::liveQueues.fetch_add(1, std::memory_order_relaxed);
    }

    virtual ~MockD3D12CommandQueue() {
        MockD3D12Device::liveQueues.fetch_sub(1, std::memory_order_relaxed);
    }

    LONG refCount() const { return m_refCount.load(std::memory_order_acquire); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        *ppvObject = nullptr;
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(ID3D12Object) ||
            riid == __uuidof(ID3D12DeviceChild) ||
            riid == __uuidof(ID3D12Pageable) ||
            riid == __uuidof(ID3D12CommandQueue)) {
            *ppvObject = static_cast<ID3D12CommandQueue*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(m_refCount.fetch_add(1, std::memory_order_relaxed) + 1);
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG next = static_cast<ULONG>(m_refCount.fetch_sub(1, std::memory_order_acq_rel) - 1);
        if (next == 0) delete this;
        return next;
    }

    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppvDevice) override {
        if (!ppvDevice) return E_POINTER;
        *ppvDevice = nullptr;
        return m_device ? m_device->QueryInterface(riid, ppvDevice) : E_NOINTERFACE;
    }

    void STDMETHODCALLTYPE UpdateTileMappings(ID3D12Resource*, UINT, const D3D12_TILED_RESOURCE_COORDINATE*, const D3D12_TILE_REGION_SIZE*, ID3D12Heap*, UINT, const D3D12_TILE_RANGE_FLAGS*, const UINT*, const UINT*, D3D12_TILE_MAPPING_FLAGS) override {}
    void STDMETHODCALLTYPE CopyTileMappings(ID3D12Resource*, const D3D12_TILED_RESOURCE_COORDINATE*, ID3D12Resource*, const D3D12_TILED_RESOURCE_COORDINATE*, const D3D12_TILE_REGION_SIZE*, D3D12_TILE_MAPPING_FLAGS) override {}
    void STDMETHODCALLTYPE ExecuteCommandLists(UINT, ID3D12CommandList* const*) override {}
    void STDMETHODCALLTYPE SetMarker(UINT, const void*, UINT) override {}
    void STDMETHODCALLTYPE BeginEvent(UINT, const void*, UINT) override {}
    void STDMETHODCALLTYPE EndEvent() override {}
    HRESULT STDMETHODCALLTYPE Signal(ID3D12Fence* pFence, UINT64 Value) override {
        return pFence ? pFence->Signal(Value) : S_OK;
    }
    HRESULT STDMETHODCALLTYPE Wait(ID3D12Fence*, UINT64) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetTimestampFrequency(UINT64* pFrequency) override {
        if (pFrequency) *pFrequency = 1000000000ULL;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetClockCalibration(UINT64* pGpu, UINT64* pCpu) override {
        if (pGpu) *pGpu = 0;
        if (pCpu) *pCpu = 0;
        return S_OK;
    }
    D3D12_COMMAND_QUEUE_DESC STDMETHODCALLTYPE GetDesc() override {
        D3D12_COMMAND_QUEUE_DESC d{};
        d.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        return d;
    }

private:
    std::atomic<LONG> m_refCount{1};
    ID3D12Device* m_device;
};

inline HRESULT STDMETHODCALLTYPE MockD3D12Device::CreateCommandAllocator(
    D3D12_COMMAND_LIST_TYPE, REFIID riid, void** ppCommandAllocator) {
    if (!ppCommandAllocator) return E_POINTER;
    *ppCommandAllocator = nullptr;
    if (failCreateCommandAllocator) return E_FAIL;
    auto* obj = new MockD3D12CommandAllocator(this);
    const HRESULT hr = obj->QueryInterface(riid, ppCommandAllocator);
    obj->Release();
    return hr;
}

inline HRESULT STDMETHODCALLTYPE MockD3D12Device::CreateCommandList(
    UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*, ID3D12PipelineState*,
    REFIID riid, void** ppCommandList) {
    if (!ppCommandList) return E_POINTER;
    *ppCommandList = nullptr;
    if (failCreateCommandList) return E_FAIL;
    auto* obj = new MockD3D12GraphicsCommandList(this);
    const HRESULT hr = obj->QueryInterface(riid, ppCommandList);
    obj->Release();
    return hr;
}

inline HRESULT STDMETHODCALLTYPE MockD3D12Device::CreateCommittedResource(
    const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
    const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialResourceState,
    const D3D12_CLEAR_VALUE*, REFIID riidResource, void** ppvResource) {
    if (!ppvResource) return E_POINTER;
    *ppvResource = nullptr;
    if (failCreateCommittedResource) return E_OUTOFMEMORY;
    const D3D12_RESOURCE_DESC desc = pDesc ? *pDesc : D3D12_RESOURCE_DESC{};
    lastCreatedDesc  = desc;
    lastInitialState = InitialResourceState;
    auto* obj = new MockD3D12Resource(this, desc);
    const HRESULT hr = obj->QueryInterface(riidResource, ppvResource);
    obj->Release();
    return hr;
}

inline HRESULT STDMETHODCALLTYPE MockD3D12Device::CreateFence(
    UINT64 InitialValue, D3D12_FENCE_FLAGS, REFIID riid, void** ppFence) {
    if (!ppFence) return E_POINTER;
    *ppFence = nullptr;
    if (failCreateFence) return E_FAIL;
    auto* obj = new MockD3D12Fence(this, InitialValue);
    const HRESULT hr = obj->QueryInterface(riid, ppFence);
    obj->Release();
    return hr;
}
