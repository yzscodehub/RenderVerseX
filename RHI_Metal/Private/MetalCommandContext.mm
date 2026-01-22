#include "MetalCommandContext.h"
#include "MetalDevice.h"
#include "MetalResources.h"
#include "MetalPipeline.h"
#include "MetalSwapChain.h"
#include "MetalSynchronization.h"
#include "MetalConversions.h"

namespace RVX
{
    MetalCommandContext::MetalCommandContext(MetalDevice* device, RHICommandQueueType type)
        : m_device(device)
        , m_queueType(type)
    {
    }

    MetalCommandContext::~MetalCommandContext()
    {
        m_commandBuffer = nil;
    }

    // =============================================================================
    // Lifecycle
    // =============================================================================
    void MetalCommandContext::Begin()
    {
        m_commandBuffer = [m_device->GetCommandQueue() commandBuffer];
    }

    void MetalCommandContext::End()
    {
        EndCurrentEncoder();
    }

    void MetalCommandContext::Reset()
    {
        EndCurrentEncoder();
        m_commandBuffer = nil;
    }

    void MetalCommandContext::EndCurrentEncoder()
    {
        if (m_renderEncoder)
        {
            [m_renderEncoder endEncoding];
            m_renderEncoder = nil;
            m_inRenderPass = false;
        }
        if (m_computeEncoder)
        {
            [m_computeEncoder endEncoding];
            m_computeEncoder = nil;
        }
        if (m_blitEncoder)
        {
            [m_blitEncoder endEncoding];
            m_blitEncoder = nil;
        }
    }

    void MetalCommandContext::EnsureBlitEncoder()
    {
        if (!m_blitEncoder)
        {
            EndCurrentEncoder();
            m_blitEncoder = [m_commandBuffer blitCommandEncoder];
        }
    }

    void MetalCommandContext::EnsureComputeEncoder()
    {
        if (!m_computeEncoder)
        {
            EndCurrentEncoder();
            m_computeEncoder = [m_commandBuffer computeCommandEncoder];
        }
    }

    // =============================================================================
    // Debug Markers
    // =============================================================================
    void MetalCommandContext::BeginEvent(const char* name, uint32 color)
    {
        if (m_renderEncoder)
        {
            [m_renderEncoder pushDebugGroup:[NSString stringWithUTF8String:name]];
        }
        else if (m_computeEncoder)
        {
            [m_computeEncoder pushDebugGroup:[NSString stringWithUTF8String:name]];
        }
        else if (m_blitEncoder)
        {
            [m_blitEncoder pushDebugGroup:[NSString stringWithUTF8String:name]];
        }
    }

    void MetalCommandContext::EndEvent()
    {
        if (m_renderEncoder)
        {
            [m_renderEncoder popDebugGroup];
        }
        else if (m_computeEncoder)
        {
            [m_computeEncoder popDebugGroup];
        }
        else if (m_blitEncoder)
        {
            [m_blitEncoder popDebugGroup];
        }
    }

    void MetalCommandContext::SetMarker(const char* name, uint32 color)
    {
        if (m_renderEncoder)
        {
            [m_renderEncoder insertDebugSignpost:[NSString stringWithUTF8String:name]];
        }
        else if (m_computeEncoder)
        {
            [m_computeEncoder insertDebugSignpost:[NSString stringWithUTF8String:name]];
        }
        else if (m_blitEncoder)
        {
            [m_blitEncoder insertDebugSignpost:[NSString stringWithUTF8String:name]];
        }
    }

    // =============================================================================
    // Resource Barriers
    // Metal's hazard tracking handles most synchronization automatically.
    // However, for optimal performance or when using untracked resources,
    // explicit barriers can be beneficial.
    // =============================================================================
    void MetalCommandContext::BufferBarrier(const RHIBufferBarrier& barrier)
    {
        if (!barrier.buffer) return;

        auto* metalBuffer = static_cast<MetalBuffer*>(barrier.buffer);

        // For render encoders, use memoryBarrier for explicit synchronization
        if (m_renderEncoder)
        {
            if (@available(macOS 10.14, iOS 12.0, *))
            {
                // Determine stages based on resource states
                MTLRenderStages afterStages = MTLRenderStageVertex | MTLRenderStageFragment;
                MTLRenderStages beforeStages = MTLRenderStageVertex | MTLRenderStageFragment;

                id<MTLResource> resource = metalBuffer->GetMTLBuffer();
                [m_renderEncoder memoryBarrierWithResources:&resource
                                                      count:1
                                                afterStages:afterStages
                                               beforeStages:beforeStages];
            }
        }
        else if (m_computeEncoder)
        {
            if (@available(macOS 10.14, iOS 12.0, *))
            {
                id<MTLResource> resource = metalBuffer->GetMTLBuffer();
                [m_computeEncoder memoryBarrierWithResources:&resource count:1];
            }
        }
    }

    void MetalCommandContext::TextureBarrier(const RHITextureBarrier& barrier)
    {
        if (!barrier.texture) return;

        auto* metalTexture = static_cast<MetalTexture*>(barrier.texture);

        if (m_renderEncoder)
        {
            if (@available(macOS 10.14, iOS 12.0, *))
            {
                // Determine stages based on resource states
                MTLRenderStages afterStages = MTLRenderStageFragment;
                MTLRenderStages beforeStages = MTLRenderStageVertex | MTLRenderStageFragment;

                // Adjust stages based on transition
                if (barrier.stateBefore == RHIResourceState::RenderTarget ||
                    barrier.stateBefore == RHIResourceState::DepthWrite)
                {
                    afterStages = MTLRenderStageFragment;
                }
                if (barrier.stateAfter == RHIResourceState::ShaderResource)
                {
                    beforeStages = MTLRenderStageVertex | MTLRenderStageFragment;
                }

                id<MTLResource> resource = metalTexture->GetMTLTexture();
                [m_renderEncoder memoryBarrierWithResources:&resource
                                                      count:1
                                                afterStages:afterStages
                                               beforeStages:beforeStages];
            }
        }
        else if (m_computeEncoder)
        {
            if (@available(macOS 10.14, iOS 12.0, *))
            {
                id<MTLResource> resource = metalTexture->GetMTLTexture();
                [m_computeEncoder memoryBarrierWithResources:&resource count:1];
            }
        }
    }

    void MetalCommandContext::Barriers(std::span<const RHIBufferBarrier> bufferBarriers,
                                        std::span<const RHITextureBarrier> textureBarriers)
    {
        // Collect all resources for batch barrier
        if (@available(macOS 10.14, iOS 12.0, *))
        {
            std::vector<id<MTLResource>> resources;
            resources.reserve(bufferBarriers.size() + textureBarriers.size());

            for (const auto& barrier : bufferBarriers)
            {
                if (barrier.buffer)
                {
                    auto* metalBuffer = static_cast<MetalBuffer*>(barrier.buffer);
                    resources.push_back(metalBuffer->GetMTLBuffer());
                }
            }

            for (const auto& barrier : textureBarriers)
            {
                if (barrier.texture)
                {
                    auto* metalTexture = static_cast<MetalTexture*>(barrier.texture);
                    resources.push_back(metalTexture->GetMTLTexture());
                }
            }

            if (resources.empty()) return;

            if (m_renderEncoder)
            {
                [m_renderEncoder memoryBarrierWithResources:resources.data()
                                                      count:resources.size()
                                                afterStages:MTLRenderStageFragment
                                               beforeStages:MTLRenderStageVertex | MTLRenderStageFragment];
            }
            else if (m_computeEncoder)
            {
                [m_computeEncoder memoryBarrierWithResources:resources.data() count:resources.size()];
            }
        }
    }

    // =============================================================================
    // Render Pass
    // =============================================================================
    void MetalCommandContext::BeginRenderPass(const RHIRenderPassDesc& desc)
    {
        EndCurrentEncoder();

        MTLRenderPassDescriptor* passDesc = [MTLRenderPassDescriptor renderPassDescriptor];

        // Setup color attachments
        for (uint32 i = 0; i < desc.colorAttachmentCount; ++i)
        {
            const auto& att = desc.colorAttachments[i];
            if (!att.view) continue;

            auto* metalView = static_cast<MetalTextureView*>(att.view);
            passDesc.colorAttachments[i].texture = metalView->GetMTLTexture();
            passDesc.colorAttachments[i].loadAction = ToMTLLoadAction(att.loadOp);
            passDesc.colorAttachments[i].storeAction = ToMTLStoreAction(att.storeOp);
            passDesc.colorAttachments[i].clearColor = MTLClearColorMake(
                att.clearColor.r, att.clearColor.g, att.clearColor.b, att.clearColor.a);
        }

        // Setup depth-stencil attachment
        if (desc.hasDepthStencil && desc.depthStencilAttachment.view)
        {
            const auto& att = desc.depthStencilAttachment;
            auto* metalView = static_cast<MetalTextureView*>(att.view);

            passDesc.depthAttachment.texture = metalView->GetMTLTexture();
            passDesc.depthAttachment.loadAction = ToMTLLoadAction(att.depthLoadOp);
            passDesc.depthAttachment.storeAction = ToMTLStoreAction(att.depthStoreOp);
            passDesc.depthAttachment.clearDepth = att.clearValue.depth;

            // Check if format has stencil
            RHIFormat format = att.view->GetFormat();
            if (format == RHIFormat::D24_UNORM_S8_UINT || format == RHIFormat::D32_FLOAT_S8_UINT)
            {
                passDesc.stencilAttachment.texture = metalView->GetMTLTexture();
                passDesc.stencilAttachment.loadAction = ToMTLLoadAction(att.stencilLoadOp);
                passDesc.stencilAttachment.storeAction = ToMTLStoreAction(att.stencilStoreOp);
                passDesc.stencilAttachment.clearStencil = att.clearValue.stencil;
            }
        }

        m_renderEncoder = [m_commandBuffer renderCommandEncoderWithDescriptor:passDesc];
        m_inRenderPass = true;
    }

    void MetalCommandContext::EndRenderPass()
    {
        if (m_renderEncoder)
        {
            [m_renderEncoder endEncoding];
            m_renderEncoder = nil;
            m_inRenderPass = false;
        }
    }

    // =============================================================================
    // Pipeline Binding
    // =============================================================================
    void MetalCommandContext::SetPipeline(RHIPipeline* pipeline)
    {
        if (auto* graphicsPipeline = dynamic_cast<MetalGraphicsPipeline*>(pipeline))
        {
            m_currentGraphicsPipeline = graphicsPipeline;
            m_currentComputePipeline = nullptr;

            if (m_renderEncoder)
            {
                [m_renderEncoder setRenderPipelineState:graphicsPipeline->GetMTLRenderPipelineState()];

                if (graphicsPipeline->GetMTLDepthStencilState())
                {
                    [m_renderEncoder setDepthStencilState:graphicsPipeline->GetMTLDepthStencilState()];
                }

                [m_renderEncoder setCullMode:graphicsPipeline->GetCullMode()];
                [m_renderEncoder setFrontFacingWinding:graphicsPipeline->GetFrontFace()];
            }
        }
        else if (auto* computePipeline = dynamic_cast<MetalComputePipeline*>(pipeline))
        {
            m_currentComputePipeline = computePipeline;
            m_currentGraphicsPipeline = nullptr;

            EnsureComputeEncoder();
            [m_computeEncoder setComputePipelineState:computePipeline->GetMTLComputePipelineState()];
        }
    }

    // =============================================================================
    // Vertex/Index Buffers
    // =============================================================================
    void MetalCommandContext::SetVertexBuffer(uint32 slot, RHIBuffer* buffer, uint64 offset)
    {
        if (m_renderEncoder && buffer)
        {
            auto* metalBuffer = static_cast<MetalBuffer*>(buffer);
            // Use high index for vertex buffers to avoid conflict with constant buffers
            // Constant buffers use indices 0-9, vertex buffers use 30+
            constexpr uint32 kVertexBufferIndexBase = 30;
            [m_renderEncoder setVertexBuffer:metalBuffer->GetMTLBuffer()
                                      offset:offset
                                     atIndex:kVertexBufferIndexBase + slot];
        }
    }

    void MetalCommandContext::SetVertexBuffers(uint32 startSlot, std::span<RHIBuffer* const> buffers, std::span<const uint64> offsets)
    {
        for (size_t i = 0; i < buffers.size(); ++i)
        {
            uint64 offset = i < offsets.size() ? offsets[i] : 0;
            SetVertexBuffer(startSlot + static_cast<uint32>(i), buffers[i], offset);
        }
    }

    void MetalCommandContext::SetIndexBuffer(RHIBuffer* buffer, RHIFormat format, uint64 offset)
    {
        if (buffer)
        {
            m_currentIndexBuffer = static_cast<MetalBuffer*>(buffer)->GetMTLBuffer();
            m_currentIndexType = ToMTLIndexType(format);
            m_currentIndexBufferOffset = offset;
        }
    }

    // =============================================================================
    // Descriptor Sets
    // =============================================================================
    void MetalCommandContext::SetDescriptorSet(uint32 slot, RHIDescriptorSet* set, std::span<const uint32> dynamicOffsets)
    {
        if (!set) return;

        auto* metalSet = static_cast<MetalDescriptorSet*>(set);
        const auto& bindings = metalSet->GetBindings();

        // Direct binding approach - constant buffers use indices 0-29
        for (size_t i = 0; i < bindings.size(); ++i)
        {
            const auto& binding = bindings[i];
            uint32 bindingIndex = static_cast<uint32>(i);

            if (m_renderEncoder)
            {
                if (binding.buffer)
                {
                    // Bind to vertex stage at index matching shader's [[buffer(N)]]
                    [m_renderEncoder setVertexBuffer:binding.buffer offset:binding.offset atIndex:bindingIndex];
                    [m_renderEncoder setFragmentBuffer:binding.buffer offset:binding.offset atIndex:bindingIndex];
                }
                if (binding.texture)
                {
                    [m_renderEncoder setVertexTexture:binding.texture atIndex:bindingIndex];
                    [m_renderEncoder setFragmentTexture:binding.texture atIndex:bindingIndex];
                }
                if (binding.sampler)
                {
                    [m_renderEncoder setVertexSamplerState:binding.sampler atIndex:bindingIndex];
                    [m_renderEncoder setFragmentSamplerState:binding.sampler atIndex:bindingIndex];
                }
            }
            else if (m_computeEncoder)
            {
                if (binding.buffer)
                {
                    [m_computeEncoder setBuffer:binding.buffer offset:binding.offset atIndex:bindingIndex];
                }
                if (binding.texture)
                {
                    [m_computeEncoder setTexture:binding.texture atIndex:bindingIndex];
                }
                if (binding.sampler)
                {
                    [m_computeEncoder setSamplerState:binding.sampler atIndex:bindingIndex];
                }
            }
        }
    }

    void MetalCommandContext::SetPushConstants(const void* data, uint32 size, uint32 offset)
    {
        // Metal uses setBytes for push constants
        if (m_renderEncoder)
        {
            [m_renderEncoder setVertexBytes:data length:size atIndex:30]; // Use high index for push constants
            [m_renderEncoder setFragmentBytes:data length:size atIndex:30];
        }
        else if (m_computeEncoder)
        {
            [m_computeEncoder setBytes:data length:size atIndex:30];
        }
    }

    // =============================================================================
    // Viewport/Scissor
    // =============================================================================
    void MetalCommandContext::SetViewport(const RHIViewport& viewport)
    {
        if (m_renderEncoder)
        {
            MTLViewport mtlViewport;
            mtlViewport.originX = viewport.x;
            mtlViewport.originY = viewport.y;
            mtlViewport.width = viewport.width;
            mtlViewport.height = viewport.height;
            mtlViewport.znear = viewport.minDepth;
            mtlViewport.zfar = viewport.maxDepth;
            [m_renderEncoder setViewport:mtlViewport];
        }
    }

    void MetalCommandContext::SetViewports(std::span<const RHIViewport> viewports)
    {
        if (!viewports.empty())
        {
            SetViewport(viewports[0]);
        }
    }

    void MetalCommandContext::SetScissor(const RHIRect& scissor)
    {
        if (m_renderEncoder)
        {
            MTLScissorRect mtlScissor;
            mtlScissor.x = scissor.x;
            mtlScissor.y = scissor.y;
            mtlScissor.width = scissor.width;
            mtlScissor.height = scissor.height;
            [m_renderEncoder setScissorRect:mtlScissor];
        }
    }

    void MetalCommandContext::SetScissors(std::span<const RHIRect> scissors)
    {
        if (!scissors.empty())
        {
            SetScissor(scissors[0]);
        }
    }

    // =============================================================================
    // Draw Commands
    // =============================================================================
    void MetalCommandContext::Draw(uint32 vertexCount, uint32 instanceCount, uint32 firstVertex, uint32 firstInstance)
    {
        if (m_renderEncoder)
        {
            [m_renderEncoder drawPrimitives:MTLPrimitiveTypeTriangle
                                vertexStart:firstVertex
                                vertexCount:vertexCount
                              instanceCount:instanceCount
                               baseInstance:firstInstance];
        }
    }

    void MetalCommandContext::DrawIndexed(uint32 indexCount, uint32 instanceCount, uint32 firstIndex, int32 vertexOffset, uint32 firstInstance)
    {
        if (m_renderEncoder && m_currentIndexBuffer)
        {
            NSUInteger indexBufferOffset = m_currentIndexBufferOffset +
                firstIndex * (m_currentIndexType == MTLIndexTypeUInt16 ? 2 : 4);

            [m_renderEncoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                        indexCount:indexCount
                                         indexType:m_currentIndexType
                                       indexBuffer:m_currentIndexBuffer
                                 indexBufferOffset:indexBufferOffset
                                     instanceCount:instanceCount
                                        baseVertex:vertexOffset
                                      baseInstance:firstInstance];
        }
    }

    void MetalCommandContext::DrawIndirect(RHIBuffer* buffer, uint64 offset, uint32 drawCount, uint32 stride)
    {
        if (m_renderEncoder && buffer)
        {
            auto* metalBuffer = static_cast<MetalBuffer*>(buffer);
            for (uint32 i = 0; i < drawCount; ++i)
            {
                [m_renderEncoder drawPrimitives:MTLPrimitiveTypeTriangle
                                 indirectBuffer:metalBuffer->GetMTLBuffer()
                           indirectBufferOffset:offset + i * stride];
            }
        }
    }

    void MetalCommandContext::DrawIndexedIndirect(RHIBuffer* buffer, uint64 offset, uint32 drawCount, uint32 stride)
    {
        if (m_renderEncoder && buffer && m_currentIndexBuffer)
        {
            auto* metalBuffer = static_cast<MetalBuffer*>(buffer);
            for (uint32 i = 0; i < drawCount; ++i)
            {
                [m_renderEncoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                             indexType:m_currentIndexType
                                           indexBuffer:m_currentIndexBuffer
                                     indexBufferOffset:m_currentIndexBufferOffset
                                        indirectBuffer:metalBuffer->GetMTLBuffer()
                                  indirectBufferOffset:offset + i * stride];
            }
        }
    }

    // =============================================================================
    // Compute Commands
    // =============================================================================
    void MetalCommandContext::Dispatch(uint32 groupCountX, uint32 groupCountY, uint32 groupCountZ)
    {
        if (m_computeEncoder && m_currentComputePipeline)
        {
            MTLSize threadgroups = MTLSizeMake(groupCountX, groupCountY, groupCountZ);
            MTLSize threadsPerGroup = m_currentComputePipeline->GetThreadgroupSize();
            [m_computeEncoder dispatchThreadgroups:threadgroups threadsPerThreadgroup:threadsPerGroup];
        }
        else if (m_computeEncoder)
        {
            // Fallback: use default threadgroup size if no pipeline bound
            RVX_RHI_WARN("Dispatch called without compute pipeline bound, using default threadgroup size");
            MTLSize threadgroups = MTLSizeMake(groupCountX, groupCountY, groupCountZ);
            MTLSize threadsPerGroup = MTLSizeMake(8, 8, 1);
            [m_computeEncoder dispatchThreadgroups:threadgroups threadsPerThreadgroup:threadsPerGroup];
        }
    }

    void MetalCommandContext::DispatchIndirect(RHIBuffer* buffer, uint64 offset)
    {
        if (m_computeEncoder && m_currentComputePipeline && buffer)
        {
            auto* metalBuffer = static_cast<MetalBuffer*>(buffer);
            MTLSize threadsPerGroup = m_currentComputePipeline->GetThreadgroupSize();
            [m_computeEncoder dispatchThreadgroupsWithIndirectBuffer:metalBuffer->GetMTLBuffer()
                                                indirectBufferOffset:offset
                                               threadsPerThreadgroup:threadsPerGroup];
        }
        else if (m_computeEncoder && buffer)
        {
            RVX_RHI_WARN("DispatchIndirect called without compute pipeline bound, using default threadgroup size");
            auto* metalBuffer = static_cast<MetalBuffer*>(buffer);
            MTLSize threadsPerGroup = MTLSizeMake(8, 8, 1);
            [m_computeEncoder dispatchThreadgroupsWithIndirectBuffer:metalBuffer->GetMTLBuffer()
                                                indirectBufferOffset:offset
                                               threadsPerThreadgroup:threadsPerGroup];
        }
    }

    // =============================================================================
    // Copy Commands
    // =============================================================================
    void MetalCommandContext::CopyBuffer(RHIBuffer* src, RHIBuffer* dst, uint64 srcOffset, uint64 dstOffset, uint64 size)
    {
        EnsureBlitEncoder();

        auto* srcBuffer = static_cast<MetalBuffer*>(src);
        auto* dstBuffer = static_cast<MetalBuffer*>(dst);

        [m_blitEncoder copyFromBuffer:srcBuffer->GetMTLBuffer()
                         sourceOffset:srcOffset
                             toBuffer:dstBuffer->GetMTLBuffer()
                    destinationOffset:dstOffset
                                 size:size];
    }

    void MetalCommandContext::CopyTexture(RHITexture* src, RHITexture* dst, const RHITextureCopyDesc& desc)
    {
        EnsureBlitEncoder();

        auto* srcTex = static_cast<MetalTexture*>(src);
        auto* dstTex = static_cast<MetalTexture*>(dst);

        uint32 width = desc.width ? desc.width : src->GetWidth();
        uint32 height = desc.height ? desc.height : src->GetHeight();
        uint32 depth = desc.depth ? desc.depth : src->GetDepth();

        [m_blitEncoder copyFromTexture:srcTex->GetMTLTexture()
                           sourceSlice:0
                           sourceLevel:desc.srcSubresource
                          sourceOrigin:MTLOriginMake(desc.srcX, desc.srcY, desc.srcZ)
                            sourceSize:MTLSizeMake(width, height, depth)
                             toTexture:dstTex->GetMTLTexture()
                      destinationSlice:0
                      destinationLevel:desc.dstSubresource
                     destinationOrigin:MTLOriginMake(desc.dstX, desc.dstY, desc.dstZ)];
    }

    void MetalCommandContext::CopyBufferToTexture(RHIBuffer* src, RHITexture* dst, const RHIBufferTextureCopyDesc& desc)
    {
        EnsureBlitEncoder();

        auto* srcBuffer = static_cast<MetalBuffer*>(src);
        auto* dstTex = static_cast<MetalTexture*>(dst);

        uint32 width = desc.textureRegion.width ? desc.textureRegion.width : dst->GetWidth();
        uint32 height = desc.textureRegion.height ? desc.textureRegion.height : dst->GetHeight();
        uint32 bytesPerPixel = GetFormatBytesPerPixel(dst->GetFormat());
        uint32 bytesPerRow = desc.bufferRowPitch ? desc.bufferRowPitch : width * bytesPerPixel;
        uint32 bytesPerImage = desc.bufferImageHeight ? desc.bufferImageHeight * bytesPerRow : height * bytesPerRow;

        [m_blitEncoder copyFromBuffer:srcBuffer->GetMTLBuffer()
                         sourceOffset:desc.bufferOffset
                    sourceBytesPerRow:bytesPerRow
                  sourceBytesPerImage:bytesPerImage
                           sourceSize:MTLSizeMake(width, height, 1)
                            toTexture:dstTex->GetMTLTexture()
                     destinationSlice:0
                     destinationLevel:desc.textureSubresource
                    destinationOrigin:MTLOriginMake(desc.textureRegion.x, desc.textureRegion.y, 0)];
    }

    void MetalCommandContext::CopyTextureToBuffer(RHITexture* src, RHIBuffer* dst, const RHIBufferTextureCopyDesc& desc)
    {
        EnsureBlitEncoder();

        auto* srcTex = static_cast<MetalTexture*>(src);
        auto* dstBuffer = static_cast<MetalBuffer*>(dst);

        uint32 width = desc.textureRegion.width ? desc.textureRegion.width : src->GetWidth();
        uint32 height = desc.textureRegion.height ? desc.textureRegion.height : src->GetHeight();
        uint32 bytesPerPixel = GetFormatBytesPerPixel(src->GetFormat());
        uint32 bytesPerRow = desc.bufferRowPitch ? desc.bufferRowPitch : width * bytesPerPixel;
        uint32 bytesPerImage = desc.bufferImageHeight ? desc.bufferImageHeight * bytesPerRow : height * bytesPerRow;

        [m_blitEncoder copyFromTexture:srcTex->GetMTLTexture()
                           sourceSlice:0
                           sourceLevel:desc.textureSubresource
                          sourceOrigin:MTLOriginMake(desc.textureRegion.x, desc.textureRegion.y, 0)
                            sourceSize:MTLSizeMake(width, height, 1)
                              toBuffer:dstBuffer->GetMTLBuffer()
                     destinationOffset:desc.bufferOffset
                destinationBytesPerRow:bytesPerRow
              destinationBytesPerImage:bytesPerImage];
    }

    // =============================================================================
    // Query Commands
    // =============================================================================
    void MetalCommandContext::BeginQuery(RHIQueryPool* /*pool*/, uint32 /*index*/)
    {
        // TODO: Implement Metal query support using visibility result buffer
        RVX_RHI_WARN("MetalCommandContext::BeginQuery not yet implemented");
    }

    void MetalCommandContext::EndQuery(RHIQueryPool* /*pool*/, uint32 /*index*/)
    {
        // TODO: Implement Metal query support
        RVX_RHI_WARN("MetalCommandContext::EndQuery not yet implemented");
    }

    void MetalCommandContext::WriteTimestamp(RHIQueryPool* /*pool*/, uint32 /*index*/)
    {
        // TODO: Implement Metal GPU timestamps using sampleCountersInBuffer
        RVX_RHI_WARN("MetalCommandContext::WriteTimestamp not yet implemented");
    }

    void MetalCommandContext::ResolveQueries(RHIQueryPool* /*pool*/, uint32 /*firstQuery*/,
                                             uint32 /*queryCount*/, RHIBuffer* /*destBuffer*/,
                                             uint64 /*destOffset*/)
    {
        // TODO: Implement query result resolution for Metal
        RVX_RHI_WARN("MetalCommandContext::ResolveQueries not yet implemented");
    }

    void MetalCommandContext::ResetQueries(RHIQueryPool* /*pool*/, uint32 /*firstQuery*/,
                                           uint32 /*queryCount*/)
    {
        // Metal queries don't require explicit reset
    }

    // =============================================================================
    // Dynamic Render State
    // =============================================================================
    void MetalCommandContext::SetStencilReference(uint32 reference)
    {
        if (m_renderEncoder)
        {
            [m_renderEncoder setStencilReferenceValue:reference];
        }
    }

    void MetalCommandContext::SetBlendConstants(const float constants[4])
    {
        if (m_renderEncoder)
        {
            [m_renderEncoder setBlendColorRed:constants[0]
                                        green:constants[1]
                                         blue:constants[2]
                                        alpha:constants[3]];
        }
    }

    void MetalCommandContext::SetDepthBias(float constantFactor, float slopeFactor, float clamp)
    {
        if (m_renderEncoder)
        {
            [m_renderEncoder setDepthBias:constantFactor
                               slopeScale:slopeFactor
                                    clamp:clamp];
        }
    }

    void MetalCommandContext::SetDepthBounds(float minDepth, float maxDepth)
    {
        // Metal doesn't support depth bounds testing
        (void)minDepth;
        (void)maxDepth;
    }

    void MetalCommandContext::SetStencilReferenceSeparate(uint32 frontRef, uint32 backRef)
    {
        if (m_renderEncoder)
        {
            [m_renderEncoder setStencilFrontReferenceValue:frontRef
                                        backReferenceValue:backRef];
        }
    }

    void MetalCommandContext::SetLineWidth(float width)
    {
        // Metal doesn't support line width (always 1.0)
        (void)width;
    }

    // =============================================================================
    // Split Barriers (no-op for Metal - automatic barriers)
    // =============================================================================
    void MetalCommandContext::BeginBarrier(const RHIBufferBarrier& barrier)
    {
        // Metal handles resource tracking automatically
        (void)barrier;
    }

    void MetalCommandContext::BeginBarrier(const RHITextureBarrier& barrier)
    {
        // Metal handles resource tracking automatically
        (void)barrier;
    }

    void MetalCommandContext::EndBarrier(const RHIBufferBarrier& barrier)
    {
        // Metal handles resource tracking automatically
        (void)barrier;
    }

    void MetalCommandContext::EndBarrier(const RHITextureBarrier& barrier)
    {
        // Metal handles resource tracking automatically
        (void)barrier;
    }

    // =============================================================================
    // Submission
    // =============================================================================
    void MetalCommandContext::Submit(RHIFence* signalFence)
    {
        EndCurrentEncoder();

        // Present drawable through command buffer for optimal scheduling
        // This allows Metal to schedule the present at the optimal time
        if (m_pendingDrawable)
        {
            [m_commandBuffer presentDrawable:m_pendingDrawable];
            m_pendingDrawable = nil;
        }

        if (signalFence)
        {
            auto* metalFence = static_cast<MetalFence*>(signalFence);
            metalFence->SignalFromCommandBuffer(m_commandBuffer);
        }

        [m_commandBuffer commit];
    }

} // namespace RVX
