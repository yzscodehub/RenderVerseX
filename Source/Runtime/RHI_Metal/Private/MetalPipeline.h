#pragma once

#include "MetalCommon.h"
#include "RHI/RHIPipeline.h"

namespace RVX
{
    class MetalPipelineLayout;

    // =============================================================================
    // MetalPipelineLayout
    // =============================================================================
    class MetalPipelineLayout : public RHIPipelineLayout
    {
    public:
        MetalPipelineLayout(const RHIPipelineLayoutDesc& desc);
        ~MetalPipelineLayout() override = default;

        const RHIPipelineLayoutDesc& GetDesc() const { return m_desc; }

    private:
        RHIPipelineLayoutDesc m_desc;
    };

    // =============================================================================
    // MetalGraphicsPipeline
    // =============================================================================
    class MetalGraphicsPipeline : public RHIPipeline
    {
    public:
        MetalGraphicsPipeline(id<MTLDevice> device, const RHIGraphicsPipelineDesc& desc);
        ~MetalGraphicsPipeline() override;

        bool IsCompute() const override { return false; }

        id<MTLRenderPipelineState> GetMTLRenderPipelineState() const { return m_pipelineState; }
        id<MTLDepthStencilState> GetMTLDepthStencilState() const { return m_depthStencilState; }

        MTLCullMode GetCullMode() const { return m_cullMode; }
        MTLWinding GetFrontFace() const { return m_frontFace; }
        MTLPrimitiveType GetPrimitiveType() const { return m_primitiveType; }
        MetalPipelineLayout* GetDescriptorPipelineLayout() const { return m_descriptorPipelineLayout; }

    private:
        id<MTLRenderPipelineState> m_pipelineState = nil;
        id<MTLDepthStencilState> m_depthStencilState = nil;

        MTLCullMode m_cullMode = MTLCullModeBack;
        MTLWinding m_frontFace = MTLWindingCounterClockwise;
        MTLPrimitiveType m_primitiveType = MTLPrimitiveTypeTriangle;
        MetalPipelineLayout* m_descriptorPipelineLayout = nullptr;
    };

    // =============================================================================
    // MetalComputePipeline
    // =============================================================================
    class MetalComputePipeline : public RHIPipeline
    {
    public:
        MetalComputePipeline(id<MTLDevice> device, const RHIComputePipelineDesc& desc);
        ~MetalComputePipeline() override;

        bool IsCompute() const override { return true; }

        id<MTLComputePipelineState> GetMTLComputePipelineState() const { return m_pipelineState; }
        MTLSize GetThreadgroupSize() const { return m_threadgroupSize; }
        MetalPipelineLayout* GetDescriptorPipelineLayout() const { return m_descriptorPipelineLayout; }

    private:
        id<MTLComputePipelineState> m_pipelineState = nil;
        MTLSize m_threadgroupSize;
        MetalPipelineLayout* m_descriptorPipelineLayout = nullptr;
    };

} // namespace RVX
