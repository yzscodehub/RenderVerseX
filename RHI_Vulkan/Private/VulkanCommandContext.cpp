#include "VulkanCommandContext.h"
#include "VulkanDevice.h"
#include "VulkanResources.h"
#include "VulkanPipeline.h"
#include "VulkanSwapChain.h"

namespace RVX
{
    VulkanCommandContext::VulkanCommandContext(VulkanDevice* device, RHICommandQueueType type)
        : m_device(device)
        , m_queueType(type)
    {
        m_commandPool = device->GetCommandPool(type);

        VkCommandBufferAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocInfo.commandPool = m_commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;

        VK_CHECK(vkAllocateCommandBuffers(device->GetDevice(), &allocInfo, &m_commandBuffer));
    }

    VulkanCommandContext::~VulkanCommandContext()
    {
        if (m_commandBuffer)
        {
            vkFreeCommandBuffers(m_device->GetDevice(), m_commandPool, 1, &m_commandBuffer);
        }
    }

    void VulkanCommandContext::Begin()
    {
        if (m_isRecording)
            return;

        VK_CHECK(vkResetCommandBuffer(m_commandBuffer, 0));

        VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

        VK_CHECK(vkBeginCommandBuffer(m_commandBuffer, &beginInfo));
        m_isRecording = true;
        m_currentPipeline = nullptr;
    }

    void VulkanCommandContext::End()
    {
        if (!m_isRecording)
            return;

        if (m_inRenderPass)
        {
            EndRenderPass();
        }

        FlushBarriers();  // Ensure all pending barriers are submitted

        VK_CHECK(vkEndCommandBuffer(m_commandBuffer));
        m_isRecording = false;
    }

    void VulkanCommandContext::Reset()
    {
        VK_CHECK(vkResetCommandBuffer(m_commandBuffer, 0));
        m_isRecording = false;
        m_inRenderPass = false;
        m_currentPipeline = nullptr;
    }

    void VulkanCommandContext::BeginEvent(const char* name, uint32 color)
    {
        if (!m_device->HasDebugUtils())
            return;

        VkDebugUtilsLabelEXT label = {VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
        label.pLabelName = name;
        // Convert color from ARGB to float RGBA
        label.color[0] = ((color >> 16) & 0xFF) / 255.0f;  // R
        label.color[1] = ((color >> 8) & 0xFF) / 255.0f;   // G
        label.color[2] = (color & 0xFF) / 255.0f;          // B
        label.color[3] = ((color >> 24) & 0xFF) / 255.0f;  // A

        m_device->vkCmdBeginDebugUtilsLabel(m_commandBuffer, &label);
    }

    void VulkanCommandContext::EndEvent()
    {
        if (!m_device->HasDebugUtils())
            return;

        m_device->vkCmdEndDebugUtilsLabel(m_commandBuffer);
    }

    void VulkanCommandContext::SetMarker(const char* name, uint32 color)
    {
        if (!m_device->HasDebugUtils())
            return;

        VkDebugUtilsLabelEXT label = {VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
        label.pLabelName = name;
        label.color[0] = ((color >> 16) & 0xFF) / 255.0f;
        label.color[1] = ((color >> 8) & 0xFF) / 255.0f;
        label.color[2] = (color & 0xFF) / 255.0f;
        label.color[3] = ((color >> 24) & 0xFF) / 255.0f;

        m_device->vkCmdInsertDebugUtilsLabel(m_commandBuffer, &label);
    }

    void VulkanCommandContext::BufferBarrier(const RHIBufferBarrier& barrier)
    {
        auto* vkBuffer = static_cast<VulkanBuffer*>(barrier.buffer);

        VkBufferMemoryBarrier2 bufferBarrier = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        bufferBarrier.srcStageMask = ToVkPipelineStageFlags(barrier.stateBefore);
        bufferBarrier.srcAccessMask = ToVkAccessFlags(barrier.stateBefore);
        bufferBarrier.dstStageMask = ToVkPipelineStageFlags(barrier.stateAfter);
        bufferBarrier.dstAccessMask = ToVkAccessFlags(barrier.stateAfter);
        bufferBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bufferBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bufferBarrier.buffer = vkBuffer->GetBuffer();
        bufferBarrier.offset = barrier.offset;
        bufferBarrier.size = barrier.size == RVX_WHOLE_SIZE ? VK_WHOLE_SIZE : barrier.size;

        // Accumulate barrier for batch submission (matching DX12 design)
        m_pendingBufferBarriers.push_back(bufferBarrier);
    }

    void VulkanCommandContext::TextureBarrier(const RHITextureBarrier& barrier)
    {
        auto* vkTexture = static_cast<VulkanTexture*>(barrier.texture);

        VkImageMemoryBarrier2 imageBarrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        imageBarrier.srcStageMask = ToVkPipelineStageFlags(barrier.stateBefore);
        imageBarrier.srcAccessMask = ToVkAccessFlags(barrier.stateBefore);
        imageBarrier.dstStageMask = ToVkPipelineStageFlags(barrier.stateAfter);
        imageBarrier.dstAccessMask = ToVkAccessFlags(barrier.stateAfter);
        imageBarrier.oldLayout = ToVkImageLayout(barrier.stateBefore);
        imageBarrier.newLayout = ToVkImageLayout(barrier.stateAfter);
        imageBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        imageBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        imageBarrier.image = vkTexture->GetImage();

        // Subresource range
        imageBarrier.subresourceRange.baseMipLevel = barrier.subresourceRange.baseMipLevel;
        imageBarrier.subresourceRange.levelCount = (barrier.subresourceRange.mipLevelCount == 0 || barrier.subresourceRange.mipLevelCount == RVX_ALL_MIPS) ?
            VK_REMAINING_MIP_LEVELS : barrier.subresourceRange.mipLevelCount;
        imageBarrier.subresourceRange.baseArrayLayer = barrier.subresourceRange.baseArrayLayer;
        imageBarrier.subresourceRange.layerCount = (barrier.subresourceRange.arrayLayerCount == 0 || barrier.subresourceRange.arrayLayerCount == RVX_ALL_LAYERS) ?
            VK_REMAINING_ARRAY_LAYERS : barrier.subresourceRange.arrayLayerCount;

        // Determine aspect mask - must match VulkanTextureView logic for depth/stencil formats
        if (HasFlag(barrier.texture->GetUsage(), RHITextureUsage::DepthStencil))
        {
            RHIFormat format = barrier.texture->GetFormat();
            // Use IsStencilFormat() for maintainability - handles D24_UNORM_S8_UINT and D32_FLOAT_S8_UINT
            if (IsStencilFormat(format))
            {
                imageBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
            }
            else
            {
                // Pure depth formats: D16_UNORM, D32_FLOAT
                imageBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            }
        }
        else
        {
            imageBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        }

        // Accumulate barrier for batch submission (matching DX12 design)
        m_pendingImageBarriers.push_back(imageBarrier);

        // Update tracked layout
        vkTexture->SetCurrentLayout(imageBarrier.newLayout);
    }

    void VulkanCommandContext::Barriers(std::span<const RHIBufferBarrier> bufferBarriers,
                                         std::span<const RHITextureBarrier> textureBarriers)
    {
        for (const auto& barrier : bufferBarriers)
            BufferBarrier(barrier);
        for (const auto& barrier : textureBarriers)
            TextureBarrier(barrier);
    }

    void VulkanCommandContext::FlushBarriers()
    {
        if (m_pendingImageBarriers.empty() && m_pendingBufferBarriers.empty())
            return;

        VkDependencyInfo dependencyInfo = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependencyInfo.imageMemoryBarrierCount = static_cast<uint32>(m_pendingImageBarriers.size());
        dependencyInfo.pImageMemoryBarriers = m_pendingImageBarriers.data();
        dependencyInfo.bufferMemoryBarrierCount = static_cast<uint32>(m_pendingBufferBarriers.size());
        dependencyInfo.pBufferMemoryBarriers = m_pendingBufferBarriers.data();

        vkCmdPipelineBarrier2(m_commandBuffer, &dependencyInfo);

        m_pendingImageBarriers.clear();
        m_pendingBufferBarriers.clear();
    }

    void VulkanCommandContext::BeginRenderPass(const RHIRenderPassDesc& desc)
    {
        if (m_inRenderPass)
            return;

        FlushBarriers();  // Ensure layout transitions are applied before rendering

        // Use dynamic rendering (Vulkan 1.3)
        std::vector<VkRenderingAttachmentInfo> colorAttachments;
        for (uint32 i = 0; i < desc.colorAttachmentCount; ++i)
        {
            const auto& attach = desc.colorAttachments[i];
            auto* vkView = static_cast<VulkanTextureView*>(attach.view);

            VkRenderingAttachmentInfo attachInfo = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            attachInfo.imageView = vkView->GetImageView();
            attachInfo.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            
            switch (attach.loadOp)
            {
                case RHILoadOp::Load:    attachInfo.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; break;
                case RHILoadOp::Clear:   attachInfo.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; break;
                case RHILoadOp::DontCare: attachInfo.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; break;
            }

            switch (attach.storeOp)
            {
                case RHIStoreOp::Store:    attachInfo.storeOp = VK_ATTACHMENT_STORE_OP_STORE; break;
                case RHIStoreOp::DontCare: attachInfo.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; break;
            }

            attachInfo.clearValue.color = {{attach.clearColor.r, attach.clearColor.g, 
                                            attach.clearColor.b, attach.clearColor.a}};

            colorAttachments.push_back(attachInfo);
        }

        VkRenderingInfo renderingInfo = {VK_STRUCTURE_TYPE_RENDERING_INFO};
        
        // Get render area from first color attachment
        if (desc.colorAttachmentCount > 0 && desc.colorAttachments[0].view)
        {
            auto* view = static_cast<VulkanTextureView*>(desc.colorAttachments[0].view);
            renderingInfo.renderArea.extent.width = view->GetVulkanTexture()->GetWidth();
            renderingInfo.renderArea.extent.height = view->GetVulkanTexture()->GetHeight();
        }
        renderingInfo.layerCount = 1;
        renderingInfo.colorAttachmentCount = static_cast<uint32>(colorAttachments.size());
        renderingInfo.pColorAttachments = colorAttachments.data();

        // Depth attachment
        VkRenderingAttachmentInfo depthAttachInfo = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        if (desc.depthStencilAttachment.view)
        {
            auto* vkView = static_cast<VulkanTextureView*>(desc.depthStencilAttachment.view);
            
            depthAttachInfo.imageView = vkView->GetImageView();
            depthAttachInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            
            switch (desc.depthStencilAttachment.depthLoadOp)
            {
                case RHILoadOp::Load:    depthAttachInfo.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; break;
                case RHILoadOp::Clear:   depthAttachInfo.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; break;
                case RHILoadOp::DontCare: depthAttachInfo.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; break;
            }

            switch (desc.depthStencilAttachment.depthStoreOp)
            {
                case RHIStoreOp::Store:    depthAttachInfo.storeOp = VK_ATTACHMENT_STORE_OP_STORE; break;
                case RHIStoreOp::DontCare: depthAttachInfo.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; break;
            }

            depthAttachInfo.clearValue.depthStencil = {desc.depthStencilAttachment.clearValue.depth, 
                                                        desc.depthStencilAttachment.clearValue.stencil};

            renderingInfo.pDepthAttachment = &depthAttachInfo;
        }

        vkCmdBeginRendering(m_commandBuffer, &renderingInfo);
        m_inRenderPass = true;
    }

    void VulkanCommandContext::EndRenderPass()
    {
        if (!m_inRenderPass)
            return;

        vkCmdEndRendering(m_commandBuffer);
        m_inRenderPass = false;
    }

    void VulkanCommandContext::SetPipeline(RHIPipeline* pipeline)
    {
        m_currentPipeline = static_cast<VulkanPipeline*>(pipeline);
        
        VkPipelineBindPoint bindPoint = m_currentPipeline->IsCompute() ?
            VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS;

        vkCmdBindPipeline(m_commandBuffer, bindPoint, m_currentPipeline->GetPipeline());
    }

    void VulkanCommandContext::SetVertexBuffer(uint32 slot, RHIBuffer* buffer, uint64 offset)
    {
        auto* vkBuffer = static_cast<VulkanBuffer*>(buffer);
        VkBuffer buffers[] = {vkBuffer->GetBuffer()};
        VkDeviceSize offsets[] = {offset};
        vkCmdBindVertexBuffers(m_commandBuffer, slot, 1, buffers, offsets);
    }

    void VulkanCommandContext::SetVertexBuffers(uint32 startSlot, std::span<RHIBuffer* const> buffers, 
                                                 std::span<const uint64> offsets)
    {
        std::vector<VkBuffer> vkBuffers(buffers.size());
        std::vector<VkDeviceSize> vkOffsets(buffers.size());

        for (size_t i = 0; i < buffers.size(); ++i)
        {
            vkBuffers[i] = static_cast<VulkanBuffer*>(buffers[i])->GetBuffer();
            vkOffsets[i] = offsets.empty() ? 0 : offsets[i];
        }

        vkCmdBindVertexBuffers(m_commandBuffer, startSlot, static_cast<uint32>(buffers.size()),
            vkBuffers.data(), vkOffsets.data());
    }

    void VulkanCommandContext::SetIndexBuffer(RHIBuffer* buffer, RHIFormat format, uint64 offset)
    {
        auto* vkBuffer = static_cast<VulkanBuffer*>(buffer);
        VkIndexType indexType = (format == RHIFormat::R16_UINT) ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
        vkCmdBindIndexBuffer(m_commandBuffer, vkBuffer->GetBuffer(), offset, indexType);
    }

    void VulkanCommandContext::SetDescriptorSet(uint32 slot, RHIDescriptorSet* set, 
                                                 std::span<const uint32> dynamicOffsets)
    {
        if (!m_currentPipeline || !set)
            return;

        auto* vkSet = static_cast<VulkanDescriptorSet*>(set);
        VkDescriptorSet descriptorSet = vkSet->GetDescriptorSet();

        VkPipelineBindPoint bindPoint = m_currentPipeline->IsCompute() ?
            VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS;

        vkCmdBindDescriptorSets(m_commandBuffer, bindPoint, m_currentPipeline->GetPipelineLayout(),
            slot, 1, &descriptorSet, 
            static_cast<uint32>(dynamicOffsets.size()), dynamicOffsets.data());
    }

    void VulkanCommandContext::SetPushConstants(const void* data, uint32 size, uint32 offset)
    {
        if (!m_currentPipeline)
            return;

        vkCmdPushConstants(m_commandBuffer, m_currentPipeline->GetPipelineLayout(),
            VK_SHADER_STAGE_ALL, offset, size, data);
    }

    void VulkanCommandContext::SetViewport(const RHIViewport& viewport)
    {
        VkViewport vp = {};
        vp.x = viewport.x;
        vp.y = viewport.y;
        vp.width = viewport.width;
        vp.height = viewport.height;
        vp.minDepth = viewport.minDepth;
        vp.maxDepth = viewport.maxDepth;
        vkCmdSetViewport(m_commandBuffer, 0, 1, &vp);
    }

    void VulkanCommandContext::SetViewports(std::span<const RHIViewport> viewports)
    {
        std::vector<VkViewport> vps(viewports.size());
        for (size_t i = 0; i < viewports.size(); ++i)
        {
            vps[i].x = viewports[i].x;
            vps[i].y = viewports[i].y;
            vps[i].width = viewports[i].width;
            vps[i].height = viewports[i].height;
            vps[i].minDepth = viewports[i].minDepth;
            vps[i].maxDepth = viewports[i].maxDepth;
        }
        vkCmdSetViewport(m_commandBuffer, 0, static_cast<uint32>(vps.size()), vps.data());
    }

    void VulkanCommandContext::SetScissor(const RHIRect& scissor)
    {
        VkRect2D rect = {};
        rect.offset.x = scissor.x;
        rect.offset.y = scissor.y;
        rect.extent.width = scissor.width;
        rect.extent.height = scissor.height;
        vkCmdSetScissor(m_commandBuffer, 0, 1, &rect);
    }

    void VulkanCommandContext::SetScissors(std::span<const RHIRect> scissors)
    {
        std::vector<VkRect2D> rects(scissors.size());
        for (size_t i = 0; i < scissors.size(); ++i)
        {
            rects[i].offset.x = scissors[i].x;
            rects[i].offset.y = scissors[i].y;
            rects[i].extent.width = scissors[i].width;
            rects[i].extent.height = scissors[i].height;
        }
        vkCmdSetScissor(m_commandBuffer, 0, static_cast<uint32>(rects.size()), rects.data());
    }

    void VulkanCommandContext::Draw(uint32 vertexCount, uint32 instanceCount, 
                                     uint32 firstVertex, uint32 firstInstance)
    {
        FlushBarriers();
        vkCmdDraw(m_commandBuffer, vertexCount, instanceCount, firstVertex, firstInstance);
    }

    void VulkanCommandContext::DrawIndexed(uint32 indexCount, uint32 instanceCount, 
                                            uint32 firstIndex, int32 vertexOffset, uint32 firstInstance)
    {
        FlushBarriers();
        vkCmdDrawIndexed(m_commandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
    }

    void VulkanCommandContext::DrawIndirect(RHIBuffer* buffer, uint64 offset, uint32 drawCount, uint32 stride)
    {
        FlushBarriers();
        auto* vkBuffer = static_cast<VulkanBuffer*>(buffer);
        vkCmdDrawIndirect(m_commandBuffer, vkBuffer->GetBuffer(), offset, drawCount, stride);
    }

    void VulkanCommandContext::DrawIndexedIndirect(RHIBuffer* buffer, uint64 offset, uint32 drawCount, uint32 stride)
    {
        FlushBarriers();
        auto* vkBuffer = static_cast<VulkanBuffer*>(buffer);
        vkCmdDrawIndexedIndirect(m_commandBuffer, vkBuffer->GetBuffer(), offset, drawCount, stride);
    }

    void VulkanCommandContext::Dispatch(uint32 groupCountX, uint32 groupCountY, uint32 groupCountZ)
    {
        FlushBarriers();
        vkCmdDispatch(m_commandBuffer, groupCountX, groupCountY, groupCountZ);
    }

    void VulkanCommandContext::DispatchIndirect(RHIBuffer* buffer, uint64 offset)
    {
        FlushBarriers();
        auto* vkBuffer = static_cast<VulkanBuffer*>(buffer);
        vkCmdDispatchIndirect(m_commandBuffer, vkBuffer->GetBuffer(), offset);
    }

    void VulkanCommandContext::CopyBuffer(RHIBuffer* src, RHIBuffer* dst, 
                                           uint64 srcOffset, uint64 dstOffset, uint64 size)
    {
        FlushBarriers();

        auto* vkSrc = static_cast<VulkanBuffer*>(src);
        auto* vkDst = static_cast<VulkanBuffer*>(dst);

        VkBufferCopy copyRegion = {};
        copyRegion.srcOffset = srcOffset;
        copyRegion.dstOffset = dstOffset;
        copyRegion.size = size;

        vkCmdCopyBuffer(m_commandBuffer, vkSrc->GetBuffer(), vkDst->GetBuffer(), 1, &copyRegion);
    }

    void VulkanCommandContext::CopyTexture(RHITexture* src, RHITexture* dst, const RHITextureCopyDesc& desc)
    {
        FlushBarriers();

        auto* vkSrc = static_cast<VulkanTexture*>(src);
        auto* vkDst = static_cast<VulkanTexture*>(dst);

        VkImageCopy copyRegion = {};
        copyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.srcSubresource.mipLevel = desc.srcSubresource;
        copyRegion.srcSubresource.baseArrayLayer = 0;
        copyRegion.srcSubresource.layerCount = 1;
        copyRegion.srcOffset = {static_cast<int32>(desc.srcX), static_cast<int32>(desc.srcY), static_cast<int32>(desc.srcZ)};
        copyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.dstSubresource.mipLevel = desc.dstSubresource;
        copyRegion.dstSubresource.baseArrayLayer = 0;
        copyRegion.dstSubresource.layerCount = 1;
        copyRegion.dstOffset = {static_cast<int32>(desc.dstX), static_cast<int32>(desc.dstY), static_cast<int32>(desc.dstZ)};
        copyRegion.extent = {desc.width ? desc.width : src->GetWidth(), 
                             desc.height ? desc.height : src->GetHeight(), 
                             desc.depth ? desc.depth : src->GetDepth()};

        vkCmdCopyImage(m_commandBuffer, 
            vkSrc->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            vkDst->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &copyRegion);
    }

    void VulkanCommandContext::CopyBufferToTexture(RHIBuffer* src, RHITexture* dst, 
                                                    const RHIBufferTextureCopyDesc& desc)
    {
        FlushBarriers();

        auto* vkSrc = static_cast<VulkanBuffer*>(src);
        auto* vkDst = static_cast<VulkanTexture*>(dst);

        VkBufferImageCopy copyRegion = {};
        copyRegion.bufferOffset = desc.bufferOffset;
        copyRegion.bufferRowLength = desc.bufferRowPitch;
        copyRegion.bufferImageHeight = desc.bufferImageHeight;
        copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.imageSubresource.mipLevel = desc.textureSubresource;
        copyRegion.imageSubresource.baseArrayLayer = 0;
        copyRegion.imageSubresource.layerCount = 1;
        copyRegion.imageOffset = {static_cast<int32>(desc.textureRegion.x), 
                                  static_cast<int32>(desc.textureRegion.y), 
                                  static_cast<int32>(desc.textureDepthSlice)};
        copyRegion.imageExtent = {desc.textureRegion.width ? desc.textureRegion.width : dst->GetWidth(),
                                  desc.textureRegion.height ? desc.textureRegion.height : dst->GetHeight(),
                                  1};

        vkCmdCopyBufferToImage(m_commandBuffer, vkSrc->GetBuffer(), vkDst->GetImage(),
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);
    }

    void VulkanCommandContext::CopyTextureToBuffer(RHITexture* src, RHIBuffer* dst,
                                                    const RHIBufferTextureCopyDesc& desc)
    {
        FlushBarriers();

        auto* vkSrc = static_cast<VulkanTexture*>(src);
        auto* vkDst = static_cast<VulkanBuffer*>(dst);

        VkBufferImageCopy copyRegion = {};
        copyRegion.bufferOffset = desc.bufferOffset;
        copyRegion.bufferRowLength = desc.bufferRowPitch;
        copyRegion.bufferImageHeight = desc.bufferImageHeight;
        copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.imageSubresource.mipLevel = desc.textureSubresource;
        copyRegion.imageSubresource.baseArrayLayer = 0;
        copyRegion.imageSubresource.layerCount = 1;
        copyRegion.imageOffset = {static_cast<int32>(desc.textureRegion.x), 
                                  static_cast<int32>(desc.textureRegion.y), 
                                  static_cast<int32>(desc.textureDepthSlice)};
        copyRegion.imageExtent = {desc.textureRegion.width ? desc.textureRegion.width : src->GetWidth(),
                                  desc.textureRegion.height ? desc.textureRegion.height : src->GetHeight(),
                                  1};

        vkCmdCopyImageToBuffer(m_commandBuffer, vkSrc->GetImage(),
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vkDst->GetBuffer(), 1, &copyRegion);
    }

    // =============================================================================
    // Query Commands (Vulkan stubs - queries not yet implemented)
    // =============================================================================
    void VulkanCommandContext::BeginQuery(RHIQueryPool* /*pool*/, uint32 /*index*/)
    {
        // TODO: Implement Vulkan query support using vkCmdBeginQuery
        RVX_RHI_WARN("VulkanCommandContext::BeginQuery not yet implemented");
    }

    void VulkanCommandContext::EndQuery(RHIQueryPool* /*pool*/, uint32 /*index*/)
    {
        // TODO: Implement Vulkan query support using vkCmdEndQuery
        RVX_RHI_WARN("VulkanCommandContext::EndQuery not yet implemented");
    }

    void VulkanCommandContext::WriteTimestamp(RHIQueryPool* /*pool*/, uint32 /*index*/)
    {
        // TODO: Implement Vulkan timestamp queries using vkCmdWriteTimestamp
        RVX_RHI_WARN("VulkanCommandContext::WriteTimestamp not yet implemented");
    }

    void VulkanCommandContext::ResolveQueries(RHIQueryPool* /*pool*/, uint32 /*firstQuery*/,
                                              uint32 /*queryCount*/, RHIBuffer* /*destBuffer*/,
                                              uint64 /*destOffset*/)
    {
        // TODO: Implement query result retrieval using vkCmdCopyQueryPoolResults
        RVX_RHI_WARN("VulkanCommandContext::ResolveQueries not yet implemented");
    }

    void VulkanCommandContext::ResetQueries(RHIQueryPool* /*pool*/, uint32 /*firstQuery*/,
                                            uint32 /*queryCount*/)
    {
        // TODO: Implement query reset using vkCmdResetQueryPool
        RVX_RHI_WARN("VulkanCommandContext::ResetQueries not yet implemented");
    }

    // =============================================================================
    // Factory and Submit
    // =============================================================================
    RHICommandContextRef CreateVulkanCommandContext(VulkanDevice* device, RHICommandQueueType type)
    {
        return Ref<VulkanCommandContext>(new VulkanCommandContext(device, type));
    }

    static VkQueue GetQueueForType(VulkanDevice* device, RHICommandQueueType type)
    {
        switch (type)
        {
            case RHICommandQueueType::Graphics: return device->GetGraphicsQueue();
            case RHICommandQueueType::Compute:  return device->GetComputeQueue();
            case RHICommandQueueType::Copy:     return device->GetTransferQueue();
            default: return device->GetGraphicsQueue();
        }
    }

    void SubmitVulkanCommandContext(VulkanDevice* device, RHICommandContext* context, RHIFence* signalFence)
    {
        auto* vkContext = static_cast<VulkanCommandContext*>(context);

        std::lock_guard<std::mutex> lock(device->GetSubmitMutex());

        VkQueue queue = GetQueueForType(device, vkContext->GetQueueType());
        VkCommandBuffer cmdBuffer = vkContext->GetCommandBuffer();

        // Build semaphore arrays
        std::vector<VkSemaphore> waitSemaphores;
        std::vector<VkPipelineStageFlags> waitStages;
        std::vector<uint64> waitValues;

        std::vector<VkSemaphore> signalSemaphores;
        std::vector<uint64> signalValues;

        VkFence fence = VK_NULL_HANDLE;

        // Graphics queue needs swapchain synchronization
        if (vkContext->GetQueueType() == RHICommandQueueType::Graphics)
        {
            waitSemaphores.push_back(device->GetImageAvailableSemaphore());
            waitStages.push_back(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
            waitValues.push_back(0);  // Binary semaphore

            signalSemaphores.push_back(device->GetRenderFinishedSemaphore());
            signalValues.push_back(0);  // Binary semaphore

            fence = device->GetCurrentFrameFence();
        }

        // Handle timeline semaphore for signalFence
        uint64 signalFenceValue = 0;
        if (signalFence)
        {
            auto* vkFence = static_cast<VulkanFence*>(signalFence);
            signalSemaphores.push_back(vkFence->GetSemaphore());
            signalFenceValue = vkFence->GetCompletedValue() + 1;
            signalValues.push_back(signalFenceValue);
        }

        // Use timeline semaphore submit info
        VkTimelineSemaphoreSubmitInfo timelineInfo = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        timelineInfo.waitSemaphoreValueCount = static_cast<uint32>(waitValues.size());
        timelineInfo.pWaitSemaphoreValues = waitValues.data();
        timelineInfo.signalSemaphoreValueCount = static_cast<uint32>(signalValues.size());
        timelineInfo.pSignalSemaphoreValues = signalValues.data();

        VkSubmitInfo submitInfo = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submitInfo.pNext = (signalFence || !waitValues.empty()) ? &timelineInfo : nullptr;
        submitInfo.waitSemaphoreCount = static_cast<uint32>(waitSemaphores.size());
        submitInfo.pWaitSemaphores = waitSemaphores.empty() ? nullptr : waitSemaphores.data();
        submitInfo.pWaitDstStageMask = waitStages.empty() ? nullptr : waitStages.data();
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmdBuffer;
        submitInfo.signalSemaphoreCount = static_cast<uint32>(signalSemaphores.size());
        submitInfo.pSignalSemaphores = signalSemaphores.empty() ? nullptr : signalSemaphores.data();

        VK_CHECK(vkQueueSubmit(queue, 1, &submitInfo, fence));
    }

    void SubmitVulkanCommandContexts(VulkanDevice* device, std::span<RHICommandContext* const> contexts, 
                                      RHIFence* signalFence)
    {
        if (contexts.empty())
            return;

        std::lock_guard<std::mutex> lock(device->GetSubmitMutex());

        // Group contexts by queue type for batch submission
        std::vector<VkCommandBuffer> graphicsCmdBuffers;
        std::vector<VkCommandBuffer> computeCmdBuffers;
        std::vector<VkCommandBuffer> copyCmdBuffers;

        for (auto* context : contexts)
        {
            auto* vkContext = static_cast<VulkanCommandContext*>(context);
            VkCommandBuffer cmdBuffer = vkContext->GetCommandBuffer();

            switch (vkContext->GetQueueType())
            {
                case RHICommandQueueType::Graphics:
                    graphicsCmdBuffers.push_back(cmdBuffer);
                    break;
                case RHICommandQueueType::Compute:
                    computeCmdBuffers.push_back(cmdBuffer);
                    break;
                case RHICommandQueueType::Copy:
                    copyCmdBuffers.push_back(cmdBuffer);
                    break;
            }
        }

        // Cross-queue synchronization semaphores (created on-demand)
        VkSemaphore copyToComputeSemaphore = VK_NULL_HANDLE;
        VkSemaphore copyToGraphicsSemaphore = VK_NULL_HANDLE;
        VkSemaphore computeToGraphicsSemaphore = VK_NULL_HANDLE;

        VkSemaphoreCreateInfo semaphoreInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};

        // Determine if we need cross-queue sync
        bool needCopyToComputeSync = !copyCmdBuffers.empty() && !computeCmdBuffers.empty();
        bool needCopyToGraphicsSync = !copyCmdBuffers.empty() && !computeCmdBuffers.empty() && graphicsCmdBuffers.empty();
        bool needComputeToGraphicsSync = !computeCmdBuffers.empty() && !graphicsCmdBuffers.empty();

        // Create synchronization semaphores as needed
        if (needCopyToComputeSync)
        {
            vkCreateSemaphore(device->GetDevice(), &semaphoreInfo, nullptr, &copyToComputeSemaphore);
        }
        if (needCopyToGraphicsSync)
        {
            vkCreateSemaphore(device->GetDevice(), &semaphoreInfo, nullptr, &copyToGraphicsSemaphore);
        }
        if (needComputeToGraphicsSync)
        {
            vkCreateSemaphore(device->GetDevice(), &semaphoreInfo, nullptr, &computeToGraphicsSemaphore);
        }

        // Submit copy commands first
        if (!copyCmdBuffers.empty())
        {
            std::vector<VkSemaphore> signalSemaphores;
            if (copyToComputeSemaphore != VK_NULL_HANDLE)
                signalSemaphores.push_back(copyToComputeSemaphore);
            if (copyToGraphicsSemaphore != VK_NULL_HANDLE)
                signalSemaphores.push_back(copyToGraphicsSemaphore);

            VkSubmitInfo submitInfo = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submitInfo.commandBufferCount = static_cast<uint32>(copyCmdBuffers.size());
            submitInfo.pCommandBuffers = copyCmdBuffers.data();
            submitInfo.signalSemaphoreCount = static_cast<uint32>(signalSemaphores.size());
            submitInfo.pSignalSemaphores = signalSemaphores.empty() ? nullptr : signalSemaphores.data();
            VK_CHECK(vkQueueSubmit(device->GetTransferQueue(), 1, &submitInfo, VK_NULL_HANDLE));
        }

        // Submit compute commands (wait for copy if needed)
        if (!computeCmdBuffers.empty())
        {
            std::vector<VkSemaphore> waitSemaphores;
            std::vector<VkPipelineStageFlags> waitStages;
            std::vector<VkSemaphore> signalSemaphores;

            if (copyToComputeSemaphore != VK_NULL_HANDLE)
            {
                waitSemaphores.push_back(copyToComputeSemaphore);
                waitStages.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
            }
            if (computeToGraphicsSemaphore != VK_NULL_HANDLE)
            {
                signalSemaphores.push_back(computeToGraphicsSemaphore);
            }

            VkSubmitInfo submitInfo = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submitInfo.waitSemaphoreCount = static_cast<uint32>(waitSemaphores.size());
            submitInfo.pWaitSemaphores = waitSemaphores.empty() ? nullptr : waitSemaphores.data();
            submitInfo.pWaitDstStageMask = waitStages.empty() ? nullptr : waitStages.data();
            submitInfo.commandBufferCount = static_cast<uint32>(computeCmdBuffers.size());
            submitInfo.pCommandBuffers = computeCmdBuffers.data();
            submitInfo.signalSemaphoreCount = static_cast<uint32>(signalSemaphores.size());
            submitInfo.pSignalSemaphores = signalSemaphores.empty() ? nullptr : signalSemaphores.data();
            VK_CHECK(vkQueueSubmit(device->GetComputeQueue(), 1, &submitInfo, VK_NULL_HANDLE));
        }

        // Submit graphics commands with swapchain sync
        if (!graphicsCmdBuffers.empty())
        {
            std::vector<VkSemaphore> waitSemaphores;
            std::vector<VkPipelineStageFlags> waitStages;
            std::vector<uint64> waitValues;

            // Wait for swapchain image
            waitSemaphores.push_back(device->GetImageAvailableSemaphore());
            waitStages.push_back(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
            waitValues.push_back(0);  // Binary semaphore

            // Wait for compute if needed
            if (computeToGraphicsSemaphore != VK_NULL_HANDLE)
            {
                waitSemaphores.push_back(computeToGraphicsSemaphore);
                waitStages.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
                waitValues.push_back(0);  // Binary semaphore
            }

            // Wait for copy if needed (only if no compute)
            if (copyToGraphicsSemaphore != VK_NULL_HANDLE)
            {
                waitSemaphores.push_back(copyToGraphicsSemaphore);
                waitStages.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
                waitValues.push_back(0);  // Binary semaphore
            }

            std::vector<VkSemaphore> signalSemaphores = {device->GetRenderFinishedSemaphore()};
            std::vector<uint64> signalValues = {0};  // Binary semaphore

            if (signalFence)
            {
                auto* vkFence = static_cast<VulkanFence*>(signalFence);
                signalSemaphores.push_back(vkFence->GetSemaphore());
                signalValues.push_back(vkFence->GetCompletedValue() + 1);
            }

            VkTimelineSemaphoreSubmitInfo timelineInfo = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
            timelineInfo.waitSemaphoreValueCount = static_cast<uint32>(waitValues.size());
            timelineInfo.pWaitSemaphoreValues = waitValues.data();
            timelineInfo.signalSemaphoreValueCount = static_cast<uint32>(signalValues.size());
            timelineInfo.pSignalSemaphoreValues = signalValues.data();

            VkSubmitInfo submitInfo = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submitInfo.pNext = signalFence ? &timelineInfo : nullptr;
            submitInfo.waitSemaphoreCount = static_cast<uint32>(waitSemaphores.size());
            submitInfo.pWaitSemaphores = waitSemaphores.data();
            submitInfo.pWaitDstStageMask = waitStages.data();
            submitInfo.commandBufferCount = static_cast<uint32>(graphicsCmdBuffers.size());
            submitInfo.pCommandBuffers = graphicsCmdBuffers.data();
            submitInfo.signalSemaphoreCount = static_cast<uint32>(signalSemaphores.size());
            submitInfo.pSignalSemaphores = signalSemaphores.data();

            VK_CHECK(vkQueueSubmit(device->GetGraphicsQueue(), 1, &submitInfo, device->GetCurrentFrameFence()));
        }
        else if (signalFence)
        {
            // No graphics commands but we need to signal the fence
            auto* vkFence = static_cast<VulkanFence*>(signalFence);
            uint64 signalValue = vkFence->GetCompletedValue() + 1;

            VkTimelineSemaphoreSubmitInfo timelineInfo = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
            timelineInfo.signalSemaphoreValueCount = 1;
            timelineInfo.pSignalSemaphoreValues = &signalValue;

            VkSemaphore signalSemaphore = vkFence->GetSemaphore();
            VkSubmitInfo submitInfo = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submitInfo.pNext = &timelineInfo;
            submitInfo.signalSemaphoreCount = 1;
            submitInfo.pSignalSemaphores = &signalSemaphore;

            VK_CHECK(vkQueueSubmit(device->GetGraphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE));
        }

        // Clean up temporary semaphores (will be destroyed after GPU is done with them)
        // Note: In a production engine, these would be pooled and reused
        // For now, we wait for idle before destroying (safe but not optimal)
        if (copyToComputeSemaphore != VK_NULL_HANDLE || 
            copyToGraphicsSemaphore != VK_NULL_HANDLE || 
            computeToGraphicsSemaphore != VK_NULL_HANDLE)
        {
            // Queue these for deferred deletion (after fence signals)
            // For simplicity, we'll rely on the frame sync to clean up
            // A proper implementation would use a deletion queue
            
            // Immediate cleanup (safe because we're in a locked mutex and 
            // the semaphores were signaled before queue submit returned)
            device->WaitIdle();
            if (copyToComputeSemaphore != VK_NULL_HANDLE)
                vkDestroySemaphore(device->GetDevice(), copyToComputeSemaphore, nullptr);
            if (copyToGraphicsSemaphore != VK_NULL_HANDLE)
                vkDestroySemaphore(device->GetDevice(), copyToGraphicsSemaphore, nullptr);
            if (computeToGraphicsSemaphore != VK_NULL_HANDLE)
                vkDestroySemaphore(device->GetDevice(), computeToGraphicsSemaphore, nullptr);
        }
    }

    // =============================================================================
    // Dynamic Render State
    // =============================================================================
    void VulkanCommandContext::SetStencilReference(uint32 reference)
    {
        vkCmdSetStencilReference(m_commandBuffer, VK_STENCIL_FACE_FRONT_AND_BACK, reference);
    }

    void VulkanCommandContext::SetBlendConstants(const float constants[4])
    {
        vkCmdSetBlendConstants(m_commandBuffer, constants);
    }

    void VulkanCommandContext::SetDepthBias(float constantFactor, float slopeFactor, float clamp)
    {
        vkCmdSetDepthBias(m_commandBuffer, constantFactor, clamp, slopeFactor);
    }

    void VulkanCommandContext::SetDepthBounds(float minDepth, float maxDepth)
    {
        vkCmdSetDepthBounds(m_commandBuffer, minDepth, maxDepth);
    }

    void VulkanCommandContext::SetStencilReferenceSeparate(uint32 frontRef, uint32 backRef)
    {
        vkCmdSetStencilReference(m_commandBuffer, VK_STENCIL_FACE_FRONT_BIT, frontRef);
        vkCmdSetStencilReference(m_commandBuffer, VK_STENCIL_FACE_BACK_BIT, backRef);
    }

    void VulkanCommandContext::SetLineWidth(float width)
    {
        vkCmdSetLineWidth(m_commandBuffer, width);
    }

    // =============================================================================
    // Split Barriers
    // =============================================================================
    void VulkanCommandContext::BeginBarrier(const RHIBufferBarrier& barrier)
    {
        // Vulkan split barriers work by specifying srcStageMask in the begin
        // and dstStageMask in the end. For simplicity, we add to pending barriers
        // and they will be flushed together. The actual split is handled by
        // the barrier batching system.
        // TODO: Implement proper split barrier with separate dependency tracking
        BufferBarrier(barrier);
    }

    void VulkanCommandContext::BeginBarrier(const RHITextureBarrier& barrier)
    {
        // Same as buffer - add to pending barriers
        TextureBarrier(barrier);
    }

    void VulkanCommandContext::EndBarrier(const RHIBufferBarrier& barrier)
    {
        // For now, this is a no-op since we did full barrier in BeginBarrier
        // TODO: Implement proper split barrier with END_ONLY semantics
        (void)barrier;
    }

    void VulkanCommandContext::EndBarrier(const RHITextureBarrier& barrier)
    {
        // No-op - barrier was done in BeginBarrier
        (void)barrier;
    }

} // namespace RVX
