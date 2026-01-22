#pragma once

#include "DX12Common.h"
#include "DX12DescriptorHeap.h"
#include "RHI/RHIPipeline.h"
#include "RHI/RHIDescriptor.h"
#include <bitset>
#include <map>
#include <unordered_map>

namespace RVX
{
    class DX12Device;

    // =============================================================================
    // DX12 Descriptor Set Layout
    // =============================================================================
    class DX12DescriptorSetLayout : public RHIDescriptorSetLayout
    {
    public:
        DX12DescriptorSetLayout(DX12Device* device, const RHIDescriptorSetLayoutDesc& desc);
        ~DX12DescriptorSetLayout() override;

        const std::vector<RHIBindingLayoutEntry>& GetEntries() const { return m_entries; }
        const RHIBindingLayoutEntry* FindEntry(uint32 binding) const;
        uint32 GetCbvSrvUavCount() const { return m_cbvSrvUavCount; }
        uint32 GetSamplerCount() const { return m_samplerCount; }
        uint32 GetCbvSrvUavIndex(uint32 binding) const;
        uint32 GetSamplerIndex(uint32 binding) const;
        uint32 GetDynamicBindingIndex(uint32 binding) const;

    private:
        DX12Device* m_device = nullptr;
        std::vector<RHIBindingLayoutEntry> m_entries;
        std::unordered_map<uint32, uint32> m_cbvSrvUavIndices;
        std::unordered_map<uint32, uint32> m_samplerIndices;
        std::unordered_map<uint32, uint32> m_dynamicBindingIndices;
        std::unordered_map<uint32, uint32> m_entryIndices;
        uint32 m_cbvSrvUavCount = 0;
        uint32 m_samplerCount = 0;
    };

    // =============================================================================
    // DX12 Pipeline Layout
    // =============================================================================
    class DX12PipelineLayout : public RHIPipelineLayout
    {
    public:
        DX12PipelineLayout(DX12Device* device, const RHIPipelineLayoutDesc& desc);
        ~DX12PipelineLayout() override;

        ID3D12RootSignature* GetRootSignature() const { return m_rootSignature.Get(); }
        uint32 GetPushConstantRootIndex() const { return m_pushConstantRootIndex; }
        
        // Get root parameter index for a CBV binding
        uint32 GetRootCBVIndex(uint32 setIndex, uint32 binding) const 
        { 
            auto it = m_rootCBVIndices.find({setIndex, binding});
            return it != m_rootCBVIndices.end() ? it->second : UINT32_MAX;
        }
        uint32 GetSrvUavTableIndex(uint32 setIndex) const;
        uint32 GetSamplerTableIndex(uint32 setIndex) const;

    private:
        void CreateRootSignature(const RHIPipelineLayoutDesc& desc);

        DX12Device* m_device = nullptr;
        ComPtr<ID3D12RootSignature> m_rootSignature;
        uint32 m_pushConstantRootIndex = UINT32_MAX;
        std::map<std::pair<uint32, uint32>, uint32> m_rootCBVIndices;  // (setIndex, binding) -> root param index
        std::unordered_map<uint32, uint32> m_srvUavTableIndices;
        std::unordered_map<uint32, uint32> m_samplerTableIndices;
    };

    // =============================================================================
    // DX12 Pipeline (Graphics & Compute)
    // =============================================================================
    class DX12Pipeline : public RHIPipeline
    {
    public:
        DX12Pipeline(DX12Device* device, const RHIGraphicsPipelineDesc& desc);
        DX12Pipeline(DX12Device* device, const RHIComputePipelineDesc& desc);
        ~DX12Pipeline() override;

        bool IsCompute() const override { return m_isCompute; }

        ID3D12PipelineState* GetPipelineState() const { return m_pipelineState.Get(); }
        ID3D12RootSignature* GetRootSignature() const { return m_rootSignature.Get(); }
        D3D_PRIMITIVE_TOPOLOGY GetPrimitiveTopology() const { return m_primitiveTopology; }
        DX12PipelineLayout* GetPipelineLayout() const { return m_pipelineLayout; }
        bool IsValid() const { return m_pipelineState != nullptr; }

    private:
        void CreateGraphicsPipeline(const RHIGraphicsPipelineDesc& desc);
        void CreateComputePipeline(const RHIComputePipelineDesc& desc);

        DX12Device* m_device = nullptr;
        ComPtr<ID3D12PipelineState> m_pipelineState;
        ComPtr<ID3D12RootSignature> m_rootSignature;  // Owned or referenced
        D3D_PRIMITIVE_TOPOLOGY m_primitiveTopology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        bool m_isCompute = false;
        RHIPipelineLayoutRef m_ownedLayout;
        DX12PipelineLayout* m_pipelineLayout = nullptr;
    };

    // =============================================================================
    // DX12 Descriptor Set
    // =============================================================================
    class DX12DescriptorSet : public RHIDescriptorSet
    {
    public:
        DX12DescriptorSet(DX12Device* device, const RHIDescriptorSetDesc& desc);
        ~DX12DescriptorSet() override;

        // Update all bindings
        void Update(const std::vector<RHIDescriptorBinding>& bindings) override;
        
        // Update a single binding (optimized path)
        void UpdateSingle(uint32 bindingIndex, const RHIDescriptorBinding& binding);
        
        // Flush any pending descriptor updates
        void FlushUpdates();

        const std::vector<RHIDescriptorBinding>& GetBindings() const { return m_bindings; }
        DX12DescriptorSetLayout* GetLayout() const { return m_layout; }
        bool HasCbvSrvUavTable() const { return m_cbvSrvUavHandle.IsValid(); }
        bool HasSamplerTable() const { return m_samplerHandle.IsValid(); }
        D3D12_GPU_DESCRIPTOR_HANDLE GetCbvSrvUavGpuHandle() const { return m_cbvSrvUavHandle.gpuHandle; }
        D3D12_GPU_DESCRIPTOR_HANDLE GetSamplerGpuHandle() const { return m_samplerHandle.gpuHandle; }

    private:
        void UpdateBindingInternal(const RHIDescriptorBinding& binding);

        DX12Device* m_device = nullptr;
        DX12DescriptorSetLayout* m_layout = nullptr;
        std::vector<RHIDescriptorBinding> m_bindings;
        DX12DescriptorHandle m_cbvSrvUavHandle;
        DX12DescriptorHandle m_samplerHandle;
        uint32 m_cbvSrvUavCount = 0;
        uint32 m_samplerCount = 0;
        
        // Dirty tracking for deferred updates
        std::bitset<64> m_dirtyBindings;
        bool m_hasPendingUpdates = false;
    };

    // =============================================================================
    // Factory Functions
    // =============================================================================
    RHIDescriptorSetLayoutRef CreateDX12DescriptorSetLayout(DX12Device* device, const RHIDescriptorSetLayoutDesc& desc);
    RHIPipelineLayoutRef CreateDX12PipelineLayout(DX12Device* device, const RHIPipelineLayoutDesc& desc);
    RHIPipelineRef CreateDX12GraphicsPipeline(DX12Device* device, const RHIGraphicsPipelineDesc& desc);
    RHIPipelineRef CreateDX12ComputePipeline(DX12Device* device, const RHIComputePipelineDesc& desc);
    RHIDescriptorSetRef CreateDX12DescriptorSet(DX12Device* device, const RHIDescriptorSetDesc& desc);

} // namespace RVX
