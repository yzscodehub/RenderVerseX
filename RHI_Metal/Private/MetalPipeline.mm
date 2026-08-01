#include "MetalPipeline.h"
#include "MetalResources.h"
#include "MetalConversions.h"
#include "RHI/RHIPipelineValidation.h"

namespace RVX
{
    // =============================================================================
    // MetalPipelineLayout
    // =============================================================================
    MetalPipelineLayout::MetalPipelineLayout(const RHIPipelineLayoutDesc& desc)
        : RHIPipelineLayout(desc)
        , m_desc(desc)
    {
        // Metal doesn't have explicit pipeline layouts
        // This is just metadata for descriptor set binding
    }

    // =============================================================================
    // MetalGraphicsPipeline
    // =============================================================================
    MetalGraphicsPipeline::MetalGraphicsPipeline(id<MTLDevice> device, const RHIGraphicsPipelineDesc& desc)
        : m_descriptorPipelineLayout(static_cast<MetalPipelineLayout*>(desc.pipelineLayout))
    {
        MTLRenderPipelineDescriptor* pipelineDesc = [[MTLRenderPipelineDescriptor alloc] init];

        // Set shaders
        if (desc.vertexShader)
        {
            auto* vs = static_cast<MetalShader*>(desc.vertexShader);
            pipelineDesc.vertexFunction = vs->GetMTLFunction();
        }

        if (desc.pixelShader)
        {
            auto* ps = static_cast<MetalShader*>(desc.pixelShader);
            pipelineDesc.fragmentFunction = ps->GetMTLFunction();
        }

        // Setup vertex descriptor from RHIInputLayoutDesc
        if (!desc.inputLayout.elements.empty())
        {
            MTLVertexDescriptor* vertexDesc = [[MTLVertexDescriptor alloc] init];

            // Track slot strides for layout setup
            const RHIVertexInputTranslation vertexInputTranslation =
                BuildRHIVertexInputTranslation(desc.inputLayout);
            for (const RHIVertexInputAttributeTranslation& attribute :
                 vertexInputTranslation.attributes)
            {
                const auto& elem =
                    desc.inputLayout.elements[attribute.elementIndex];

                // Use high buffer index for vertex buffers to avoid conflict with constant buffers
                // Constant buffers use indices 0-29, vertex buffers use 30+
                constexpr uint32 kVertexBufferIndexBase = 30;

                vertexDesc.attributes[attribute.location].format =
                    ToMTLVertexFormat(elem.format);
                vertexDesc.attributes[attribute.location].offset =
                    attribute.alignedByteOffset;
                vertexDesc.attributes[attribute.location].bufferIndex =
                    kVertexBufferIndexBase + attribute.inputSlot;
            }

            // Setup vertex buffer layouts (using offset indices to match attributes)
            constexpr uint32 kLayoutBufferIndexBase = 30;
            for (const RHIVertexInputBindingTranslation& binding :
                 vertexInputTranslation.bindings)
            {
                const uint32 bufferIndex =
                    kLayoutBufferIndexBase + binding.inputSlot;
                vertexDesc.layouts[bufferIndex].stride =
                    binding.stride;
                vertexDesc.layouts[bufferIndex].stepRate =
                    binding.perInstance
                        ? binding.instanceDataStepRate
                        : 1;
                vertexDesc.layouts[bufferIndex].stepFunction =
                    binding.perInstance ?
                    MTLVertexStepFunctionPerInstance : MTLVertexStepFunctionPerVertex;
            }

            pipelineDesc.vertexDescriptor = vertexDesc;
        }

        // Setup color attachments from RHIGraphicsPipelineDesc
        for (uint32 i = 0; i < desc.numRenderTargets; ++i)
        {
            pipelineDesc.colorAttachments[i].pixelFormat = ToMTLPixelFormat(desc.renderTargetFormats[i]);

            // Apply blend state
            const auto& blendRT = desc.blendState.renderTargets[i];
            if (blendRT.blendEnable)
            {
                pipelineDesc.colorAttachments[i].blendingEnabled = YES;
                pipelineDesc.colorAttachments[i].sourceRGBBlendFactor = ToMTLBlendFactor(blendRT.srcColorBlend);
                pipelineDesc.colorAttachments[i].destinationRGBBlendFactor = ToMTLBlendFactor(blendRT.dstColorBlend);
                pipelineDesc.colorAttachments[i].rgbBlendOperation = ToMTLBlendOperation(blendRT.colorBlendOp);
                pipelineDesc.colorAttachments[i].sourceAlphaBlendFactor = ToMTLBlendFactor(blendRT.srcAlphaBlend);
                pipelineDesc.colorAttachments[i].destinationAlphaBlendFactor = ToMTLBlendFactor(blendRT.dstAlphaBlend);
                pipelineDesc.colorAttachments[i].alphaBlendOperation = ToMTLBlendOperation(blendRT.alphaBlendOp);
            }

            // Write mask
            MTLColorWriteMask mask = MTLColorWriteMaskNone;
            if (blendRT.colorWriteMask & 0x1) mask |= MTLColorWriteMaskRed;
            if (blendRT.colorWriteMask & 0x2) mask |= MTLColorWriteMaskGreen;
            if (blendRT.colorWriteMask & 0x4) mask |= MTLColorWriteMaskBlue;
            if (blendRT.colorWriteMask & 0x8) mask |= MTLColorWriteMaskAlpha;
            pipelineDesc.colorAttachments[i].writeMask = mask;
        }

        // Depth-stencil format
        if (desc.depthStencilFormat != RHIFormat::Unknown)
        {
            pipelineDesc.depthAttachmentPixelFormat = ToMTLPixelFormat(desc.depthStencilFormat);

            if (desc.depthStencilFormat == RHIFormat::D24_UNORM_S8_UINT ||
                desc.depthStencilFormat == RHIFormat::D32_FLOAT_S8_UINT)
            {
                pipelineDesc.stencilAttachmentPixelFormat = ToMTLPixelFormat(desc.depthStencilFormat);
            }
        }

        // Sample count
        pipelineDesc.sampleCount = static_cast<NSUInteger>(desc.sampleCount);

        // Create pipeline state
        NSError* error = nil;
        m_pipelineState = [device newRenderPipelineStateWithDescriptor:pipelineDesc error:&error];

        if (error || !m_pipelineState)
        {
            RVX_RHI_ERROR("Failed to create Metal graphics pipeline: {}",
                error ? [[error localizedDescription] UTF8String] : "Unknown error");
            return;
        }

        // Create depth-stencil state if needed
        if (desc.depthStencilState.depthTestEnable || desc.depthStencilState.stencilTestEnable)
        {
            MTLDepthStencilDescriptor* dsDesc = [[MTLDepthStencilDescriptor alloc] init];

            dsDesc.depthCompareFunction = desc.depthStencilState.depthTestEnable ?
                ToMTLCompareFunction(desc.depthStencilState.depthCompareOp) : MTLCompareFunctionAlways;
            dsDesc.depthWriteEnabled = desc.depthStencilState.depthWriteEnable;

            // TODO: Setup stencil state if needed

            m_depthStencilState = [device newDepthStencilStateWithDescriptor:dsDesc];
        }

        // Store rasterization state
        m_cullMode = ToMTLCullMode(desc.rasterizerState.cullMode);
        m_frontFace = desc.rasterizerState.frontFace == RHIFrontFace::CounterClockwise ?
            MTLWindingCounterClockwise : MTLWindingClockwise;
        m_primitiveType = ToMTLPrimitiveType(desc.primitiveTopology);

        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }
    }

    MetalGraphicsPipeline::~MetalGraphicsPipeline()
    {
        m_pipelineState = nil;
        m_depthStencilState = nil;
    }

    // =============================================================================
    // MetalComputePipeline
    // =============================================================================
    MetalComputePipeline::MetalComputePipeline(id<MTLDevice> device, const RHIComputePipelineDesc& desc)
        : m_descriptorPipelineLayout(static_cast<MetalPipelineLayout*>(desc.pipelineLayout))
    {
        if (!desc.computeShader)
        {
            RVX_RHI_ERROR("MetalComputePipeline: No compute shader provided");
            return;
        }

        auto* cs = static_cast<MetalShader*>(desc.computeShader);

        NSError* error = nil;
        m_pipelineState = [device newComputePipelineStateWithFunction:cs->GetMTLFunction() error:&error];

        if (error || !m_pipelineState)
        {
            RVX_RHI_ERROR("Failed to create Metal compute pipeline: {}",
                error ? [[error localizedDescription] UTF8String] : "Unknown error");
            return;
        }

        // Get threadgroup size from pipeline
        m_threadgroupSize = MTLSizeMake(
            [m_pipelineState threadExecutionWidth],
            [m_pipelineState maxTotalThreadsPerThreadgroup] / [m_pipelineState threadExecutionWidth],
            1);

        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }
    }

    MetalComputePipeline::~MetalComputePipeline()
    {
        m_pipelineState = nil;
    }

} // namespace RVX
