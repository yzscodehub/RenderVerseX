#pragma once

#include "DX11Common.h"
#include "RHI/RHIPipeline.h"
#include "RHI/RHIDescriptor.h"

#include <span>
#include <vector>

namespace RVX
{
    class DX11Device;

    // =============================================================================
    // DX11 Descriptor Set Layout (Phase 3)
    // =============================================================================
    class DX11DescriptorSetLayout : public RHIDescriptorSetLayout
    {
    public:
        DX11DescriptorSetLayout(DX11Device* device, const RHIDescriptorSetLayoutDesc& desc);
        ~DX11DescriptorSetLayout() override;

        const std::vector<RHIBindingLayoutEntry>& GetEntries() const { return m_entries; }

    private:
        std::vector<RHIBindingLayoutEntry> m_entries;
    };

    // =============================================================================
    // DX11 Pipeline Layout (Phase 3)
    // =============================================================================
    class DX11PipelineLayout : public RHIPipelineLayout
    {
    public:
        DX11PipelineLayout(DX11Device* device, const RHIPipelineLayoutDesc& desc);
        ~DX11PipelineLayout() override;

        const std::vector<RHIDescriptorSetLayout*>& GetSetLayouts() const { return m_setLayouts; }
        uint32 GetPushConstantSize() const { return m_pushConstantSize; }
        ID3D11Buffer* GetPushConstantBuffer() const { return m_pushConstantBuffer.Get(); }

    private:
        std::vector<RHIDescriptorSetLayout*> m_setLayouts;
        uint32 m_pushConstantSize = 0;
        ComPtr<ID3D11Buffer> m_pushConstantBuffer;
    };

    // =============================================================================
    // DX11 Descriptor Set (Phase 3)
    // =============================================================================
    class DX11DescriptorSet : public RHIDescriptorSet
    {
    public:
        DX11DescriptorSet(DX11Device* device, const RHIDescriptorSetDesc& desc);
        ~DX11DescriptorSet() override;

        void Update(const std::vector<RHIDescriptorBinding>& bindings) override;

        // Apply bindings to context
        // setIndex is used for slot remapping when using multiple descriptor sets
        // dynamicOffsets are applied to DynamicUniformBuffer/DynamicStorageBuffer bindings
        void Apply(ID3D11DeviceContext* context, RHIShaderStage stages, uint32 setIndex = 0,
                   std::span<const uint32> dynamicOffsets = {}) const;

    private:
        DX11Device* m_device = nullptr;
        DX11DescriptorSetLayout* m_layout = nullptr;
        std::vector<RHIDescriptorBinding> m_bindings;
    };

    // =============================================================================
    // DX11 Graphics Pipeline (Phase 3)
    // =============================================================================
    class DX11GraphicsPipeline : public RHIPipeline
    {
    public:
        DX11GraphicsPipeline(DX11Device* device, const RHIGraphicsPipelineDesc& desc);
        ~DX11GraphicsPipeline() override;

        bool IsCompute() const override { return false; }

        // Apply pipeline state to context
        void Apply(ID3D11DeviceContext* context) const;

        // Accessors
        DX11PipelineLayout* GetLayout() const { return m_layout; }

    private:
        DX11Device* m_device = nullptr;
        DX11PipelineLayout* m_layout = nullptr;

        // Shaders
        ComPtr<ID3D11VertexShader> m_vertexShader;
        ComPtr<ID3D11PixelShader> m_pixelShader;
        ComPtr<ID3D11GeometryShader> m_geometryShader;
        ComPtr<ID3D11HullShader> m_hullShader;
        ComPtr<ID3D11DomainShader> m_domainShader;

        // State objects (owned by DX11StateCache, raw pointers are safe)
        ID3D11RasterizerState* m_rasterizerState = nullptr;
        ID3D11DepthStencilState* m_depthStencilState = nullptr;
        ID3D11BlendState* m_blendState = nullptr;
        ID3D11InputLayout* m_inputLayout = nullptr;

        D3D11_PRIMITIVE_TOPOLOGY m_topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    };

    // =============================================================================
    // DX11 Compute Pipeline (Phase 3)
    // =============================================================================
    class DX11ComputePipeline : public RHIPipeline
    {
    public:
        DX11ComputePipeline(DX11Device* device, const RHIComputePipelineDesc& desc);
        ~DX11ComputePipeline() override;

        bool IsCompute() const override { return true; }

        void Apply(ID3D11DeviceContext* context) const;

        // Accessors
        DX11PipelineLayout* GetLayout() const { return m_layout; }

    private:
        DX11Device* m_device = nullptr;
        DX11PipelineLayout* m_layout = nullptr;
        ComPtr<ID3D11ComputeShader> m_computeShader;
    };

} // namespace RVX
