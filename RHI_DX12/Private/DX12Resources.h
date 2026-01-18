#pragma once

#include "DX12Common.h"
#include "DX12DescriptorHeap.h"
#include "RHI/RHIBuffer.h"
#include "RHI/RHITexture.h"
#include "RHI/RHISampler.h"
#include "RHI/RHIShader.h"
#include "RHI/RHIDescriptor.h"
#include "RHI/RHISynchronization.h"
#include "RHI/RHIHeap.h"

namespace RVX
{
    class DX12Device;

    // =============================================================================
    // DX12 Buffer
    // =============================================================================
    class DX12Buffer : public RHIBuffer
    {
    public:
        DX12Buffer(DX12Device* device, const RHIBufferDesc& desc);
        // Constructor for placed resources (external resource, memory owned by heap)
        DX12Buffer(DX12Device* device, ComPtr<ID3D12Resource> resource, const RHIBufferDesc& desc, bool ownsResource);
        ~DX12Buffer() override;

        // RHIBuffer interface
        uint64 GetSize() const override { return m_desc.size; }
        RHIBufferUsage GetUsage() const override { return m_desc.usage; }
        RHIMemoryType GetMemoryType() const override { return m_desc.memoryType; }
        uint32 GetStride() const override { return m_desc.stride; }
        void* Map() override;
        void Unmap() override;

        // DX12 Specific
        ID3D12Resource* GetResource() const { return m_resource.Get(); }
        D3D12_GPU_VIRTUAL_ADDRESS GetGPUVirtualAddress() const { return m_resource->GetGPUVirtualAddress(); }

        const DX12DescriptorHandle& GetCBVHandle() const { return m_cbvHandle; }
        const DX12DescriptorHandle& GetSRVHandle() const { return m_srvHandle; }
        const DX12DescriptorHandle& GetUAVHandle() const { return m_uavHandle; }

    private:
        void CreateViews();

        DX12Device* m_device = nullptr;
        RHIBufferDesc m_desc;

        ComPtr<ID3D12Resource> m_resource;
        #ifdef RVX_USE_D3D12MA
        ComPtr<D3D12MA::Allocation> m_allocation;
        #endif
        bool m_ownsResource = true;  // False for placed resources (memory owned by heap)

        DX12DescriptorHandle m_cbvHandle;
        DX12DescriptorHandle m_srvHandle;
        DX12DescriptorHandle m_uavHandle;

        void* m_mappedData = nullptr;
    };

    // =============================================================================
    // DX12 Texture
    // =============================================================================
    class DX12Texture : public RHITexture
    {
    public:
        DX12Texture(DX12Device* device, const RHITextureDesc& desc);
        DX12Texture(DX12Device* device, ComPtr<ID3D12Resource> resource, const RHITextureDesc& desc); // For swap chain
        ~DX12Texture() override;

        // RHITexture interface
        uint32 GetWidth() const override { return m_desc.width; }
        uint32 GetHeight() const override { return m_desc.height; }
        uint32 GetDepth() const override { return m_desc.depth; }
        uint32 GetMipLevels() const override { return m_desc.mipLevels; }
        uint32 GetArraySize() const override { return m_desc.arraySize; }
        RHIFormat GetFormat() const override { return m_desc.format; }
        RHITextureUsage GetUsage() const override { return m_desc.usage; }
        RHITextureDimension GetDimension() const override { return m_desc.dimension; }
        RHISampleCount GetSampleCount() const override { return m_desc.sampleCount; }

        // DX12 Specific
        ID3D12Resource* GetResource() const { return m_resource.Get(); }
        DXGI_FORMAT GetDXGIFormat() const { return m_dxgiFormat; }

        const DX12DescriptorHandle& GetSRVHandle() const { return m_srvHandle; }
        const DX12DescriptorHandle& GetUAVHandle() const { return m_uavHandle; }
        const DX12DescriptorHandle& GetRTVHandle(uint32 index = 0) const { return m_rtvHandles[index]; }
        const DX12DescriptorHandle& GetDSVHandle() const { return m_dsvHandle; }

    private:
        void CreateViews();

        DX12Device* m_device = nullptr;
        RHITextureDesc m_desc;
        DXGI_FORMAT m_dxgiFormat = DXGI_FORMAT_UNKNOWN;

        ComPtr<ID3D12Resource> m_resource;
        #ifdef RVX_USE_D3D12MA
        ComPtr<D3D12MA::Allocation> m_allocation;
        #endif
        bool m_ownsResource = true;  // false for swap chain textures

        DX12DescriptorHandle m_srvHandle;
        DX12DescriptorHandle m_uavHandle;
        std::vector<DX12DescriptorHandle> m_rtvHandles;
        DX12DescriptorHandle m_dsvHandle;
    };

    // =============================================================================
    // DX12 Texture View
    // =============================================================================
    class DX12TextureView : public RHITextureView
    {
    public:
        DX12TextureView(DX12Device* device, RHITexture* texture, const RHITextureViewDesc& desc);
        ~DX12TextureView() override;

        // RHITextureView interface
        RHITexture* GetTexture() const override { return m_texture; }
        RHIFormat GetFormat() const override { return m_format; }
        const RHISubresourceRange& GetSubresourceRange() const override { return m_subresourceRange; }

        // DX12 Specific
        const DX12DescriptorHandle& GetSRVHandle() const { return m_srvHandle; }
        const DX12DescriptorHandle& GetUAVHandle() const { return m_uavHandle; }
        const DX12DescriptorHandle& GetRTVHandle() const { return m_rtvHandle; }
        const DX12DescriptorHandle& GetDSVHandle() const { return m_dsvHandle; }

    private:
        DX12Device* m_device = nullptr;
        RHITexture* m_texture = nullptr;
        RHIFormat m_format = RHIFormat::Unknown;
        RHISubresourceRange m_subresourceRange;

        DX12DescriptorHandle m_srvHandle;
        DX12DescriptorHandle m_uavHandle;
        DX12DescriptorHandle m_rtvHandle;
        DX12DescriptorHandle m_dsvHandle;
    };

    // =============================================================================
    // DX12 Sampler
    // =============================================================================
    class DX12Sampler : public RHISampler
    {
    public:
        DX12Sampler(DX12Device* device, const RHISamplerDesc& desc);
        ~DX12Sampler() override;

        const DX12DescriptorHandle& GetHandle() const { return m_handle; }

    private:
        DX12Device* m_device = nullptr;
        DX12DescriptorHandle m_handle;
    };

    // =============================================================================
    // DX12 Shader
    // =============================================================================
    class DX12Shader : public RHIShader
    {
    public:
        DX12Shader(DX12Device* device, const RHIShaderDesc& desc);
        ~DX12Shader() override;

        // RHIShader interface
        RHIShaderStage GetStage() const override { return m_stage; }
        const std::vector<uint8>& GetBytecode() const override { return m_bytecode; }

        D3D12_SHADER_BYTECODE GetD3D12Bytecode() const
        {
            return { m_bytecode.data(), m_bytecode.size() };
        }

    private:
        RHIShaderStage m_stage = RHIShaderStage::None;
        std::vector<uint8> m_bytecode;
    };

    // =============================================================================
    // DX12 Fence
    // =============================================================================
    class DX12Fence : public RHIFence
    {
    public:
        DX12Fence(DX12Device* device, uint64 initialValue);
        ~DX12Fence() override;

        // RHIFence interface
        uint64 GetCompletedValue() const override;
        void Signal(uint64 value) override;
        void Wait(uint64 value, uint64 timeoutNs) override;

        // DX12 Specific
        ID3D12Fence* GetFence() const { return m_fence.Get(); }

    private:
        DX12Device* m_device = nullptr;
        ComPtr<ID3D12Fence> m_fence;
        HANDLE m_event = nullptr;
    };

    // =============================================================================
    // DX12 Heap (for Memory Aliasing / Placed Resources)
    // =============================================================================
    class DX12Heap : public RHIHeap
    {
    public:
        DX12Heap(DX12Device* device, const RHIHeapDesc& desc);
        ~DX12Heap() override;

        // RHIHeap interface
        uint64 GetSize() const override { return m_size; }
        RHIHeapType GetType() const override { return m_type; }
        RHIHeapFlags GetFlags() const override { return m_flags; }

        // DX12 Specific
        ID3D12Heap* GetHeap() const { return m_heap.Get(); }

    private:
        DX12Device* m_device = nullptr;
        ComPtr<ID3D12Heap> m_heap;
        uint64 m_size = 0;
        RHIHeapType m_type = RHIHeapType::Default;
        RHIHeapFlags m_flags = RHIHeapFlags::AllowAll;
    };

    // =============================================================================
    // Factory Functions
    // =============================================================================
    RHIBufferRef CreateDX12Buffer(DX12Device* device, const RHIBufferDesc& desc);
    RHITextureRef CreateDX12Texture(DX12Device* device, const RHITextureDesc& desc);
    RHITextureRef CreateDX12TextureFromResource(DX12Device* device, ComPtr<ID3D12Resource> resource, const RHITextureDesc& desc);
    RHITextureViewRef CreateDX12TextureView(DX12Device* device, RHITexture* texture, const RHITextureViewDesc& desc);
    RHISamplerRef CreateDX12Sampler(DX12Device* device, const RHISamplerDesc& desc);
    RHIShaderRef CreateDX12Shader(DX12Device* device, const RHIShaderDesc& desc);
    RHIFenceRef CreateDX12Fence(DX12Device* device, uint64 initialValue);
    void WaitForDX12Fence(DX12Device* device, RHIFence* fence, uint64 value);

    // Heap functions (for Memory Aliasing)
    RHIHeapRef CreateDX12Heap(DX12Device* device, const RHIHeapDesc& desc);
    RHITextureRef CreateDX12PlacedTexture(DX12Device* device, RHIHeap* heap, uint64 offset, const RHITextureDesc& desc);
    RHIBufferRef CreateDX12PlacedBuffer(DX12Device* device, RHIHeap* heap, uint64 offset, const RHIBufferDesc& desc);

} // namespace RVX
