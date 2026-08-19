#pragma once

#include "DX12Common.h"
#include "DX12DescriptorHeap.h"
#include "RHI/RHIBuffer.h"
#include "RHI/RHIPipeline.h"
#include "RHI/RHIDescriptor.h"
#include "RHI/RHIRayTracing.h"
#include <array>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

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

        const std::vector<RHIBindingLayoutEntry>& GetEntries() const override { return m_entries; }
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
        uint32 GetSetLayoutCount() const { return static_cast<uint32>(m_setLayouts.size()); }
        DX12DescriptorSetLayout* GetSetLayout(uint32 setIndex) const;

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
        std::vector<DX12DescriptorSetLayout*> m_setLayouts;
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
        DX12Pipeline(DX12Device* device, const RHIRayTracingPipelineDesc& desc);
        ~DX12Pipeline() override;

        bool IsCompute() const override { return m_isCompute; }
        bool IsRayTracing() const override { return m_isRayTracing; }

        ID3D12PipelineState* GetPipelineState() const { return m_pipelineState.Get(); }
        ID3D12StateObject* GetStateObject() const { return m_stateObject.Get(); }
        ID3D12RootSignature* GetRootSignature() const { return m_rootSignature.Get(); }
        D3D_PRIMITIVE_TOPOLOGY GetPrimitiveTopology() const { return m_primitiveTopology; }
        DX12PipelineLayout* GetPipelineLayout() const { return m_pipelineLayout; }
        bool UsesComputeRootSignature() const { return m_isCompute || m_isRayTracing; }
        bool IsValid() const { return m_isRayTracing ? m_stateObject != nullptr : m_pipelineState != nullptr; }
        uint32 GetRenderTargetCount() const { return m_renderTargetCount; }
        RHIFormat GetRenderTargetFormat(uint32 index) const
        {
            return index < m_renderTargetFormats.size()
                ? m_renderTargetFormats[index]
                : RHIFormat::Unknown;
        }
        RHIFormat GetDepthStencilFormat() const { return m_depthStencilFormat; }
        RHISampleCount GetSampleCount() const { return m_sampleCount; }

        uint32 GetRayTracingShaderGroupCount() const override { return static_cast<uint32>(m_shaderGroupExports.size()); }
        RHIShaderStage GetRayTracingShaderGroupStage(uint32 shaderGroupIndex) const override;
        bool IsRayTracingHitGroup(uint32 shaderGroupIndex) const override;
        uint32 GetShaderGroupCount() const { return GetRayTracingShaderGroupCount(); }
        const void* GetShaderIdentifier(uint32 shaderGroupIndex) const;

    private:
        void CreateGraphicsPipeline(const RHIGraphicsPipelineDesc& desc);
        void CreateComputePipeline(const RHIComputePipelineDesc& desc);
        void CreateRayTracingPipeline(const RHIRayTracingPipelineDesc& desc);

        DX12Device* m_device = nullptr;
        ComPtr<ID3D12PipelineState> m_pipelineState;
        ComPtr<ID3D12StateObject> m_stateObject;
        ComPtr<ID3D12StateObjectProperties> m_stateObjectProperties;
        ComPtr<ID3D12RootSignature> m_rootSignature;  // Owned or referenced
        D3D_PRIMITIVE_TOPOLOGY m_primitiveTopology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        bool m_isCompute = false;
        bool m_isRayTracing = false;
        uint32 m_renderTargetCount = 0;
        std::array<RHIFormat, RVX_MAX_RENDER_TARGETS> m_renderTargetFormats{};
        RHIFormat m_depthStencilFormat = RHIFormat::Unknown;
        RHISampleCount m_sampleCount = RHISampleCount::Count1;
        RHIPipelineLayoutRef m_ownedLayout;
        DX12PipelineLayout* m_pipelineLayout = nullptr;
        std::vector<std::wstring> m_shaderGroupExports;
        std::vector<RHIShaderStage> m_shaderGroupStages;
        std::vector<uint8> m_shaderGroupIsHitGroup;
    };

    // =============================================================================
    // DX12 Shader Table
    // =============================================================================
    class DX12ShaderTable : public RHIShaderTable
    {
    public:
        DX12ShaderTable(DX12Device* device, const RHIShaderTableDesc& desc);
        ~DX12ShaderTable() override = default;

        uint32 GetRayGenerationRecordCount() const override { return m_rayGenerationSection.count; }
        uint32 GetMissRecordCount() const override { return m_missSection.count; }
        uint32 GetHitGroupRecordCount() const override { return m_hitGroupSection.count; }
        uint32 GetCallableRecordCount() const override { return m_callableSection.count; }

        bool IsValid() const;
        RHIPipeline* GetRayTracingPipeline() const override { return m_pipeline; }
        DX12Pipeline* GetPipeline() const { return m_pipeline; }
        D3D12_DISPATCH_RAYS_DESC BuildDispatchRaysDesc(uint32 width, uint32 height, uint32 depth) const;

    private:
        struct Section
        {
            uint64 offset = 0;
            uint64 size = 0;
            uint64 stride = 0;
            uint32 count = 0;
        };

        bool Create(const RHIShaderTableDesc& desc);

        DX12Device* m_device = nullptr;
        RHIPipelineRef m_pipelineOwner;
        DX12Pipeline* m_pipeline = nullptr;
        RHIBufferRef m_buffer;
        Section m_rayGenerationSection;
        Section m_missSection;
        Section m_hitGroupSection;
        Section m_callableSection;
    };

    // =============================================================================
    // DX12 Descriptor Set
    // =============================================================================
    class DX12DescriptorSet : public RHIDescriptorSet
    {
    public:
        DX12DescriptorSet(DX12Device* device, const RHIDescriptorSetDesc& desc);
        ~DX12DescriptorSet() override;

        const std::vector<RHIDescriptorBinding>& GetBindings() const { return m_bindings; }
        DX12DescriptorSetLayout* GetLayout() const { return m_layout; }
        bool IsValid() const { return m_isValid; }
        bool HasCbvSrvUavTable() const { return m_cbvSrvUavHandle.IsValid(); }
        bool HasSamplerTable() const { return m_samplerHandle.IsValid(); }
        D3D12_GPU_DESCRIPTOR_HANDLE GetCbvSrvUavGpuHandle() const { return m_cbvSrvUavHandle.gpuHandle; }
        D3D12_GPU_DESCRIPTOR_HANDLE GetSamplerGpuHandle() const { return m_samplerHandle.gpuHandle; }

    private:
        bool InitializeNativeSnapshot();
        bool UpdateBindingInternal(const RHIDescriptorBinding& binding);

        DX12Device* m_device = nullptr;
        DX12DescriptorSetLayout* m_layout = nullptr;
        std::vector<RHIDescriptorBinding> m_bindings;
        DX12DescriptorHandle m_cbvSrvUavHandle;
        DX12DescriptorHandle m_samplerHandle;
        uint32 m_cbvSrvUavCount = 0;
        uint32 m_samplerCount = 0;
        bool m_isValid = true;

    };

    // =============================================================================
    // Factory Functions
    // =============================================================================
    RHIDescriptorSetLayoutRef CreateDX12DescriptorSetLayout(DX12Device* device, const RHIDescriptorSetLayoutDesc& desc);
    RHIPipelineLayoutRef CreateDX12PipelineLayout(DX12Device* device, const RHIPipelineLayoutDesc& desc);
    RHIPipelineRef CreateDX12GraphicsPipeline(DX12Device* device, const RHIGraphicsPipelineDesc& desc);
    RHIPipelineRef CreateDX12ComputePipeline(DX12Device* device, const RHIComputePipelineDesc& desc);
    RHIPipelineRef CreateDX12RayTracingPipeline(DX12Device* device, const RHIRayTracingPipelineDesc& desc);
    RHIShaderTableRef CreateDX12ShaderTable(DX12Device* device, const RHIShaderTableDesc& desc);
    RHIDescriptorSetRef CreateDX12DescriptorSet(DX12Device* device, const RHIDescriptorSetDesc& desc);

} // namespace RVX
