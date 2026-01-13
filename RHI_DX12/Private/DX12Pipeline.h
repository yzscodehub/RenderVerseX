#pragma once

#include "DX12Common.h"
#include "RHI/RHIPipeline.h"
#include "RHI/RHIDescriptor.h"
#include <map>

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

    private:
        DX12Device* m_device = nullptr;
        std::vector<RHIBindingLayoutEntry> m_entries;
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

    private:
        void CreateRootSignature(const RHIPipelineLayoutDesc& desc);

        DX12Device* m_device = nullptr;
        ComPtr<ID3D12RootSignature> m_rootSignature;
        uint32 m_pushConstantRootIndex = UINT32_MAX;
        std::map<std::pair<uint32, uint32>, uint32> m_rootCBVIndices;  // (setIndex, binding) -> root param index
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

    private:
        void CreateGraphicsPipeline(const RHIGraphicsPipelineDesc& desc);
        void CreateComputePipeline(const RHIComputePipelineDesc& desc);

        DX12Device* m_device = nullptr;
        ComPtr<ID3D12PipelineState> m_pipelineState;
        ComPtr<ID3D12RootSignature> m_rootSignature;  // Owned or referenced
        D3D_PRIMITIVE_TOPOLOGY m_primitiveTopology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        bool m_isCompute = false;
    };

    // =============================================================================
    // DX12 Descriptor Set
    // =============================================================================
    class DX12DescriptorSet : public RHIDescriptorSet
    {
    public:
        DX12DescriptorSet(DX12Device* device, const RHIDescriptorSetDesc& desc);
        ~DX12DescriptorSet() override;

        void Update(const std::vector<RHIDescriptorBinding>& bindings) override;

        const std::vector<RHIDescriptorBinding>& GetBindings() const { return m_bindings; }

    private:
        DX12Device* m_device = nullptr;
        std::vector<RHIDescriptorBinding> m_bindings;
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
