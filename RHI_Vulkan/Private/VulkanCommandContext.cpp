#include "VulkanCommandContext.h"
#include "VulkanDevice.h"
#include "VulkanResources.h"
#include "VulkanPipeline.h"
#include "VulkanSwapChain.h"
#include "RHI/RHIIndirectExecution.h"
#include "RHI/RHITexture.h"

#include <utility>

namespace RVX
{
    namespace
    {
        bool RequiresScopedBarrier(bool hasScopedAccess,
                                   RHIDependencyKind dependencyKind)
        {
            return hasScopedAccess && dependencyKind != RHIDependencyKind::None;
        }

        uint32 GetQueueFamilyIndex(VulkanDevice* device, GPUQueueDomain domain)
        {
            switch (domain)
            {
                case GPUQueueDomain::Compute: return device->GetComputeQueueFamily();
                case GPUQueueDomain::Copy: return device->GetTransferQueueFamily();
                case GPUQueueDomain::Graphics:
                default: return device->GetGraphicsQueueFamily();
            }
        }

        bool RequiresPairedQueueFamilyTransfer(VulkanDevice* device,
                                               const RHIAccessSnapshot& before,
                                               const RHIAccessSnapshot& after,
                                               RHIDependencyKind dependencyKind)
        {
            return HasDependencyKind(dependencyKind, RHIDependencyKind::Ownership) &&
                   GetQueueFamilyIndex(device, before.domain) !=
                       GetQueueFamilyIndex(device, after.domain);
        }

        uint32 GetContextQueueFamilyIndex(VulkanDevice* device,
                                          RHICommandQueueType queueType)
        {
            switch (queueType)
            {
                case RHICommandQueueType::Compute:
                    return device->GetComputeQueueFamily();
                case RHICommandQueueType::Copy:
                    return device->GetTransferQueueFamily();
                case RHICommandQueueType::Graphics:
                default:
                    return device->GetGraphicsQueueFamily();
            }
        }

        enum class QueueFamilyTransferRole : uint8
        {
            None = 0,
            Release,
            Acquire,
            Invalid,
        };

        QueueFamilyTransferRole GetQueueFamilyTransferRole(
            VulkanDevice* device,
            RHICommandQueueType queueType,
            const RHIAccessSnapshot& before,
            const RHIAccessSnapshot& after,
            RHIDependencyKind dependencyKind)
        {
            if (!RequiresPairedQueueFamilyTransfer(
                    device, before, after, dependencyKind))
            {
                return QueueFamilyTransferRole::None;
            }

            const uint32 currentFamily =
                GetContextQueueFamilyIndex(device, queueType);
            const uint32 sourceFamily =
                GetQueueFamilyIndex(device, before.domain);
            const uint32 destinationFamily =
                GetQueueFamilyIndex(device, after.domain);
            if (currentFamily == sourceFamily)
            {
                return QueueFamilyTransferRole::Release;
            }
            if (currentFamily == destinationFamily)
            {
                return QueueFamilyTransferRole::Acquire;
            }
            return QueueFamilyTransferRole::Invalid;
        }
    } // namespace

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
        if (!barrier.buffer ||
            (barrier.stateBefore == barrier.stateAfter &&
             !RequiresScopedBarrier(barrier.hasScopedAccess, barrier.dependencyKind)))
        {
            return;
        }

        auto* vkBuffer = static_cast<VulkanBuffer*>(barrier.buffer);
        if (!vkBuffer || vkBuffer->GetBuffer() == VK_NULL_HANDLE)
        {
            return;
        }

        const VulkanPipelineStageSupport enabledStages =
            m_device->GetEnabledPipelineStageSupport();
        VkBufferMemoryBarrier2 bufferBarrier = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        bufferBarrier.srcStageMask = barrier.hasScopedAccess
            ? ToVkPipelineStageFlags2(
                barrier.accessBefore.executionScope,
                enabledStages)
            : ToVkPipelineStageFlags(barrier.stateBefore);
        bufferBarrier.srcAccessMask = barrier.hasScopedAccess
            ? ToVkAccessFlags2(barrier.accessBefore.memoryAccess)
            : ToVkAccessFlags(barrier.stateBefore);
        bufferBarrier.dstStageMask = barrier.hasScopedAccess
            ? ToVkPipelineStageFlags2(
                barrier.accessAfter.executionScope,
                enabledStages)
            : ToVkPipelineStageFlags(barrier.stateAfter);
        bufferBarrier.dstAccessMask = barrier.hasScopedAccess
            ? ToVkAccessFlags2(barrier.accessAfter.memoryAccess)
            : ToVkAccessFlags(barrier.stateAfter);
        const QueueFamilyTransferRole transferRole = barrier.hasScopedAccess
            ? GetQueueFamilyTransferRole(m_device,
                                         m_queueType,
                                         barrier.accessBefore,
                                         barrier.accessAfter,
                                         barrier.dependencyKind)
            : QueueFamilyTransferRole::None;
        if (transferRole == QueueFamilyTransferRole::Invalid)
        {
            RVX_RHI_ERROR(
                "Vulkan cross-family buffer barrier was recorded on an unrelated queue family");
            return;
        }
        if (transferRole != QueueFamilyTransferRole::None)
        {
            bufferBarrier.srcQueueFamilyIndex =
                GetQueueFamilyIndex(m_device, barrier.accessBefore.domain);
            bufferBarrier.dstQueueFamilyIndex =
                GetQueueFamilyIndex(m_device, barrier.accessAfter.domain);
            if (transferRole == QueueFamilyTransferRole::Release)
            {
                bufferBarrier.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
                bufferBarrier.dstAccessMask = VK_ACCESS_2_NONE;
            }
            else
            {
                bufferBarrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
                bufferBarrier.srcAccessMask = VK_ACCESS_2_NONE;
            }
        }
        else
        {
            bufferBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bufferBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        }
        bufferBarrier.buffer = vkBuffer->GetBuffer();
        bufferBarrier.offset = barrier.offset;
        bufferBarrier.size = barrier.size == RVX_WHOLE_SIZE ? VK_WHOLE_SIZE : barrier.size;

        // Accumulate barrier for batch submission (matching DX12 design)
        m_pendingBufferBarriers.push_back(bufferBarrier);
    }

    void VulkanCommandContext::TextureBarrier(const RHITextureBarrier& barrier)
    {
        if (!barrier.texture ||
            (barrier.stateBefore == barrier.stateAfter &&
             !RequiresScopedBarrier(barrier.hasScopedAccess, barrier.dependencyKind)))
        {
            return;
        }

        auto* vkTexture = static_cast<VulkanTexture*>(barrier.texture);
        if (!vkTexture || vkTexture->GetImage() == VK_NULL_HANDLE)
        {
            return;
        }

        const VulkanPipelineStageSupport enabledStages =
            m_device->GetEnabledPipelineStageSupport();
        VkImageMemoryBarrier2 imageBarrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        imageBarrier.srcStageMask = barrier.hasScopedAccess
            ? ToVkPipelineStageFlags2(
                barrier.accessBefore.executionScope,
                enabledStages)
            : ToVkPipelineStageFlags(barrier.stateBefore);
        imageBarrier.srcAccessMask = barrier.hasScopedAccess
            ? ToVkAccessFlags2(barrier.accessBefore.memoryAccess)
            : ToVkAccessFlags(barrier.stateBefore);
        imageBarrier.dstStageMask = barrier.hasScopedAccess
            ? ToVkPipelineStageFlags2(
                barrier.accessAfter.executionScope,
                enabledStages)
            : ToVkPipelineStageFlags(barrier.stateAfter);
        imageBarrier.dstAccessMask = barrier.hasScopedAccess
            ? ToVkAccessFlags2(barrier.accessAfter.memoryAccess)
            : ToVkAccessFlags(barrier.stateAfter);
        imageBarrier.oldLayout = barrier.hasScopedAccess
            ? ToVkImageLayout(barrier.accessBefore.layout)
            : ToVkImageLayout(barrier.stateBefore);
        imageBarrier.newLayout = barrier.hasScopedAccess
            ? ToVkImageLayout(barrier.accessAfter.layout)
            : ToVkImageLayout(barrier.stateAfter);
        const QueueFamilyTransferRole transferRole = barrier.hasScopedAccess
            ? GetQueueFamilyTransferRole(m_device,
                                         m_queueType,
                                         barrier.accessBefore,
                                         barrier.accessAfter,
                                         barrier.dependencyKind)
            : QueueFamilyTransferRole::None;
        if (transferRole == QueueFamilyTransferRole::Invalid)
        {
            RVX_RHI_ERROR(
                "Vulkan cross-family texture barrier was recorded on an unrelated queue family");
            return;
        }
        if (transferRole != QueueFamilyTransferRole::None)
        {
            imageBarrier.srcQueueFamilyIndex =
                GetQueueFamilyIndex(m_device, barrier.accessBefore.domain);
            imageBarrier.dstQueueFamilyIndex =
                GetQueueFamilyIndex(m_device, barrier.accessAfter.domain);
            if (transferRole == QueueFamilyTransferRole::Release)
            {
                imageBarrier.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
                imageBarrier.dstAccessMask = VK_ACCESS_2_NONE;
            }
            else
            {
                imageBarrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
                imageBarrier.srcAccessMask = VK_ACCESS_2_NONE;
            }
        }
        else
        {
            imageBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            imageBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        }
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

        // Use dynamic rendering (Vulkan 1.3)
        std::vector<VkRenderingAttachmentInfo> colorAttachments;
        std::vector<VkExtent2D> attachmentExtents;
        for (uint32 i = 0; i < desc.colorAttachmentCount; ++i)
        {
            const auto& attach = desc.colorAttachments[i];
            if (attach.view == nullptr)
            {
                RVX_RHI_ERROR("Vulkan dynamic rendering rejected null color attachment {}", i);
                return;
            }
            auto* vkView = static_cast<VulkanTextureView*>(attach.view);
            VulkanTexture* texture = vkView->GetVulkanTexture();
            if (texture == nullptr)
            {
                RVX_RHI_ERROR("Vulkan dynamic rendering rejected color attachment {} without a texture", i);
                return;
            }
            attachmentExtents.push_back({texture->GetWidth(), texture->GetHeight()});

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

        // Depth attachment
        VkRenderingAttachmentInfo depthAttachInfo = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        if (desc.depthStencilAttachment.view)
        {
            auto* vkView = static_cast<VulkanTextureView*>(desc.depthStencilAttachment.view);
            VulkanTexture* texture = vkView->GetVulkanTexture();
            if (texture == nullptr)
            {
                RVX_RHI_ERROR("Vulkan dynamic rendering rejected depth attachment without a texture");
                return;
            }
            attachmentExtents.push_back({texture->GetWidth(), texture->GetHeight()});

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
        }

        if (attachmentExtents.empty())
        {
            RVX_RHI_ERROR("Vulkan dynamic rendering rejected a render pass without attachments");
            return;
        }

        VkRect2D renderArea{};
        if (desc.renderArea.width == 0 || desc.renderArea.height == 0)
        {
            // The RHI contract defines a zero extent as the full attachment extent.
            renderArea.extent = attachmentExtents.front();
        }
        else
        {
            renderArea.offset = {desc.renderArea.x, desc.renderArea.y};
            renderArea.extent = {desc.renderArea.width, desc.renderArea.height};
        }

        const bool validOffsets = renderArea.offset.x >= 0 && renderArea.offset.y >= 0;
        const uint64 renderAreaRight = validOffsets
            ? static_cast<uint64>(renderArea.offset.x) + renderArea.extent.width
            : 0;
        const uint64 renderAreaBottom = validOffsets
            ? static_cast<uint64>(renderArea.offset.y) + renderArea.extent.height
            : 0;
        for (const VkExtent2D attachmentExtent : attachmentExtents)
        {
            if (!validOffsets || renderArea.extent.width == 0 || renderArea.extent.height == 0 ||
                renderAreaRight > attachmentExtent.width ||
                renderAreaBottom > attachmentExtent.height)
            {
                RVX_RHI_ERROR(
                    "Vulkan dynamic rendering rejected render area offset=({}, {}), extent={}x{} "
                    "outside attachment extent {}x{}",
                    renderArea.offset.x,
                    renderArea.offset.y,
                    renderArea.extent.width,
                    renderArea.extent.height,
                    attachmentExtent.width,
                    attachmentExtent.height);
                return;
            }
        }

        FlushBarriers();  // Ensure layout transitions are applied before rendering

        VkRenderingInfo renderingInfo = {VK_STRUCTURE_TYPE_RENDERING_INFO};
        renderingInfo.renderArea = renderArea;
        renderingInfo.layerCount = 1;
        renderingInfo.colorAttachmentCount = static_cast<uint32>(colorAttachments.size());
        renderingInfo.pColorAttachments = colorAttachments.empty()
            ? nullptr
            : colorAttachments.data();
        renderingInfo.pDepthAttachment = desc.depthStencilAttachment.view
            ? &depthAttachInfo
            : nullptr;

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
        VulkanPipelineLayout* pipelineLayout = m_currentPipeline->GetDescriptorPipelineLayout();
        if (!pipelineLayout)
        {
            RVX_RHI_ERROR("VulkanCommandContext: descriptor binding requires a pipeline layout");
            return;
        }
        const auto& expectedLayouts = pipelineLayout->GetDescriptorSetLayouts();
        if (slot >= expectedLayouts.size() ||
            !vkSet->IsReadyForBinding(expectedLayouts[slot]) ||
            vkSet->GetLayout() != expectedLayouts[slot])
        {
            RVX_RHI_ERROR("VulkanCommandContext: descriptor set layout does not match pipeline slot {}", slot);
            return;
        }

        if (dynamicOffsets.size() != vkSet->GetRequiredDynamicOffsetCount())
        {
            RVX_RHI_ERROR(
                "VulkanCommandContext: descriptor set {} requires {} dynamic offsets, received {}",
                slot,
                vkSet->GetRequiredDynamicOffsetCount(),
                dynamicOffsets.size());
            return;
        }
        VkDescriptorSet descriptorSet = vkSet->GetDescriptorSet();

        VkPipelineBindPoint bindPoint = m_currentPipeline->IsCompute() ?
            VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS;

        vkCmdBindDescriptorSets(m_commandBuffer, bindPoint, m_currentPipeline->GetPipelineLayout(),
            slot, 1, &descriptorSet,
            static_cast<uint32>(dynamicOffsets.size()), dynamicOffsets.data());
        vkSet->MarkBound();
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
        if (drawCount == 0)
        {
            return;
        }

        const RHICapabilities& capabilities = m_device->GetCapabilities();
        RHIIndexedIndirectExecutionDesc execution;
        execution.mode = RHIIndirectExecutionMode::FixedCount;
        execution.argumentBuffer = buffer;
        execution.argumentOffset = offset;
        execution.commandStride = stride;
        execution.maxDrawCount = drawCount;
        execution.argumentState =
            capabilities.indexedIndirectExecution.requiredArgumentState;
        const RHIIndexedIndirectExecutionValidationResult validation =
            ValidateRHIIndexedIndirectExecutionDesc(capabilities, execution);
        if (!validation)
        {
            RVX_RHI_ERROR(
                "VulkanCommandContext: DrawIndexedIndirect rejected by the RHI contract: {}",
                validation.message);
            return;
        }

        auto* vkBuffer = dynamic_cast<VulkanBuffer*>(buffer);
        if (vkBuffer == nullptr || vkBuffer->GetBuffer() == VK_NULL_HANDLE)
        {
            RVX_RHI_ERROR(
                "VulkanCommandContext: DrawIndexedIndirect rejected because the argument buffer is not a live Vulkan buffer");
            return;
        }

        FlushBarriers();
        vkCmdDrawIndexedIndirect(m_commandBuffer, vkBuffer->GetBuffer(), offset, drawCount, stride);
    }

    void VulkanCommandContext::DrawIndexedIndirectCount(
        RHIBuffer* buffer,
        uint64 offset,
        RHIBuffer* countBuffer,
        uint64 countOffset,
        uint32 maxDrawCount,
        uint32 stride)
    {
        if (maxDrawCount == 0)
        {
            return;
        }

        const RHICapabilities& capabilities = m_device->GetCapabilities();
        RHIIndexedIndirectExecutionDesc execution;
        execution.mode = RHIIndirectExecutionMode::CountBuffer;
        execution.argumentBuffer = buffer;
        execution.argumentOffset = offset;
        execution.commandStride = stride;
        execution.maxDrawCount = maxDrawCount;
        execution.argumentState =
            capabilities.indexedIndirectExecution.requiredArgumentState;
        execution.countBuffer = countBuffer;
        execution.countOffset = countOffset;
        execution.countState =
            capabilities.indexedIndirectExecution.requiredCountState;
        const RHIIndexedIndirectExecutionValidationResult validation =
            ValidateRHIIndexedIndirectExecutionDesc(capabilities, execution);
        if (!validation)
        {
            RVX_RHI_ERROR(
                "VulkanCommandContext: DrawIndexedIndirectCount rejected by the RHI contract: {}",
                validation.message);
            return;
        }

        const VulkanIndexedIndirectCountDispatch dispatch =
            m_device->GetIndexedIndirectCountDispatch();
        if (dispatch == VulkanIndexedIndirectCountDispatch::None)
        {
            RVX_RHI_ERROR(
                "Vulkan indexed indirect-count was requested without an enabled native command path");
            return;
        }

        auto* vkBuffer = dynamic_cast<VulkanBuffer*>(buffer);
        auto* vkCountBuffer = dynamic_cast<VulkanBuffer*>(countBuffer);
        if (vkBuffer == nullptr || vkBuffer->GetBuffer() == VK_NULL_HANDLE ||
            vkCountBuffer == nullptr || vkCountBuffer->GetBuffer() == VK_NULL_HANDLE)
        {
            RVX_RHI_ERROR(
                "VulkanCommandContext: DrawIndexedIndirectCount rejected because a buffer is not a live Vulkan buffer");
            return;
        }

        FlushBarriers();

        switch (dispatch)
        {
            case VulkanIndexedIndirectCountDispatch::Core12:
            {
                const PFN_vkCmdDrawIndexedIndirectCount command =
                    m_device->GetCmdDrawIndexedIndirectCount();
                if (!command)
                {
                    RVX_RHI_ERROR(
                        "Vulkan indexed indirect-count core entry point is unavailable");
                    return;
                }
                command(m_commandBuffer,
                        vkBuffer->GetBuffer(),
                        offset,
                        vkCountBuffer->GetBuffer(),
                        countOffset,
                        maxDrawCount,
                        stride);
                return;
            }
            case VulkanIndexedIndirectCountDispatch::KHR:
            {
                const PFN_vkCmdDrawIndexedIndirectCountKHR command =
                    m_device->GetCmdDrawIndexedIndirectCountKHR();
                if (!command)
                {
                    RVX_RHI_ERROR(
                        "Vulkan indexed indirect-count KHR entry point is unavailable");
                    return;
                }
                command(m_commandBuffer,
                        vkBuffer->GetBuffer(),
                        offset,
                        vkCountBuffer->GetBuffer(),
                        countOffset,
                        maxDrawCount,
                        stride);
                return;
            }
            case VulkanIndexedIndirectCountDispatch::None:
            default:
                RVX_RHI_ERROR(
                    "Vulkan indexed indirect-count dispatch is invalid");
                return;
        }
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
        const auto srcSubresource = DecodeTextureSubresource(desc.srcSubresource, src->GetMipLevels());
        const auto dstSubresource = DecodeTextureSubresource(desc.dstSubresource, dst->GetMipLevels());

        VkImageCopy copyRegion = {};
        copyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.srcSubresource.mipLevel = srcSubresource.mipLevel;
        copyRegion.srcSubresource.baseArrayLayer = srcSubresource.physicalLayer;
        copyRegion.srcSubresource.layerCount = 1;
        copyRegion.srcOffset = {static_cast<int32>(desc.srcX), static_cast<int32>(desc.srcY), static_cast<int32>(desc.srcZ)};
        copyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.dstSubresource.mipLevel = dstSubresource.mipLevel;
        copyRegion.dstSubresource.baseArrayLayer = dstSubresource.physicalLayer;
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
        const auto subresource = DecodeTextureSubresource(desc.textureSubresource, dst->GetMipLevels());
        const RHIFormat format = dst->GetFormat();
        const uint32 formatBytes = GetFormatBytesPerPixel(format);
        const bool compressed = IsCompressedFormat(format);
        const uint32 bufferRowLength = (desc.bufferRowPitch != 0 && formatBytes != 0)
            ? (compressed ? (desc.bufferRowPitch / formatBytes) * 4u : desc.bufferRowPitch / formatBytes)
            : 0u;
        const uint32 bufferImageHeight = (desc.bufferImageHeight != 0 && compressed)
            ? desc.bufferImageHeight * 4u
            : desc.bufferImageHeight;

        VkBufferImageCopy copyRegion = {};
        copyRegion.bufferOffset = desc.bufferOffset;
        copyRegion.bufferRowLength = bufferRowLength;
        copyRegion.bufferImageHeight = bufferImageHeight;
        copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.imageSubresource.mipLevel = subresource.mipLevel;
        copyRegion.imageSubresource.baseArrayLayer = subresource.physicalLayer;
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
        const auto subresource = DecodeTextureSubresource(desc.textureSubresource, src->GetMipLevels());
        const RHIFormat format = src->GetFormat();
        const uint32 formatBytes = GetFormatBytesPerPixel(format);
        const bool compressed = IsCompressedFormat(format);
        const uint32 bufferRowLength = (desc.bufferRowPitch != 0 && formatBytes != 0)
            ? (compressed ? (desc.bufferRowPitch / formatBytes) * 4u : desc.bufferRowPitch / formatBytes)
            : 0u;
        const uint32 bufferImageHeight = (desc.bufferImageHeight != 0 && compressed)
            ? desc.bufferImageHeight * 4u
            : desc.bufferImageHeight;

        VkBufferImageCopy copyRegion = {};
        copyRegion.bufferOffset = desc.bufferOffset;
        copyRegion.bufferRowLength = bufferRowLength;
        copyRegion.bufferImageHeight = bufferImageHeight;
        copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.imageSubresource.mipLevel = subresource.mipLevel;
        copyRegion.imageSubresource.baseArrayLayer = subresource.physicalLayer;
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
    // Query Commands (Vulkan unsupported path)
    // =============================================================================
    void VulkanCommandContext::BeginQuery(RHIQueryPool* /*pool*/, uint32 /*index*/)
    {
        RVX_RHI_ERROR("VulkanCommandContext::BeginQuery called while RHICapabilities reports queries unsupported");
    }

    void VulkanCommandContext::EndQuery(RHIQueryPool* /*pool*/, uint32 /*index*/)
    {
        RVX_RHI_ERROR("VulkanCommandContext::EndQuery called while RHICapabilities reports queries unsupported");
    }

    void VulkanCommandContext::WriteTimestamp(RHIQueryPool* /*pool*/, uint32 /*index*/)
    {
        RVX_RHI_ERROR("VulkanCommandContext::WriteTimestamp called while RHICapabilities reports timestamp queries unsupported");
    }

    void VulkanCommandContext::ResolveQueries(RHIQueryPool* /*pool*/, uint32 /*firstQuery*/,
                                              uint32 /*queryCount*/, RHIBuffer* /*destBuffer*/,
                                              uint64 /*destOffset*/)
    {
        RVX_RHI_ERROR("VulkanCommandContext::ResolveQueries called while RHICapabilities reports queries unsupported");
    }

    void VulkanCommandContext::ResetQueries(RHIQueryPool* /*pool*/, uint32 /*firstQuery*/,
                                            uint32 /*queryCount*/)
    {
        RVX_RHI_ERROR("VulkanCommandContext::ResetQueries called while RHICapabilities reports queries unsupported");
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

    uint64 SubmitVulkanCommandContext(VulkanDevice* device, RHICommandContext* context, RHIFence* signalFence)
    {
        if (!device || !context)
        {
            return 0;
        }

        auto* vkContext = static_cast<VulkanCommandContext*>(context);

        std::lock_guard<std::mutex> lock(device->GetGraphicsQueueMutex());

        VkQueue queue = GetQueueForType(device, vkContext->GetQueueType());
        VkCommandBuffer cmdBuffer = vkContext->GetCommandBuffer();

        // Build semaphore arrays
        std::vector<VkSemaphore> waitSemaphores;
        std::vector<VkPipelineStageFlags> waitStages;
        std::vector<uint64> waitValues;

        std::vector<VkSemaphore> signalSemaphores;
        std::vector<uint64> signalValues;

        VkFence fence = VK_NULL_HANDLE;

        // Swapchain semaphores and the per-frame fence are valid only after an
        // image has been acquired. Graphics setup work submitted before the
        // first frame must not wait on an unsignaled acquire semaphore.
        if (vkContext->GetQueueType() == RHICommandQueueType::Graphics)
        {
            VulkanSwapChain* swapChain = device->GetPrimarySwapChain();
            if (swapChain && swapChain->HasAcquiredImage())
            {
                waitSemaphores.push_back(device->GetImageAvailableSemaphore());
                waitStages.push_back(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
                waitValues.push_back(0);  // Binary semaphore

                signalSemaphores.push_back(device->GetRenderFinishedSemaphore());
                signalValues.push_back(0);  // Binary semaphore
            }

            // The frame fence is owned exclusively by a BeginFrame/EndFrame
            // lifecycle. Raw submissions must never receive a fence that has
            // not first been reset and armed for this frame.
            fence = device->GetArmedFrameFenceForSubmission();
        }

        // Handle timeline semaphore for signalFence
        uint64 signalFenceValue = 0;
        if (signalFence)
        {
            auto* vkFence = static_cast<VulkanFence*>(signalFence);
            signalSemaphores.push_back(vkFence->GetSemaphore());
            signalFenceValue = vkFence->AllocateSignalValue();
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

        const VkResult submitResult =
            vkQueueSubmit(queue, 1, &submitInfo, fence);
        if (submitResult != VK_SUCCESS)
        {
            device->ReportRuntimeFailure(
                submitResult,
                RHIDeviceFaultOperation::CommandSubmission,
                "Vulkan command submission failed");
            return 0;
        }
        if (fence != VK_NULL_HANDLE)
        {
            device->MarkArmedFrameFenceSubmitted(fence);
        }
        return signalFenceValue;
    }

    uint64 SubmitVulkanCommandContexts(VulkanDevice* device, std::span<RHICommandContext* const> contexts,
                                      RHIFence* signalFence)
    {
        if (!device || contexts.empty())
            return 0;

        std::lock_guard<std::mutex> lock(device->GetGraphicsQueueMutex());

        // Group contexts by queue type for batch submission
        std::vector<VkCommandBuffer> graphicsCmdBuffers;
        std::vector<VkCommandBuffer> computeCmdBuffers;
        std::vector<VkCommandBuffer> copyCmdBuffers;

        for (auto* context : contexts)
        {
            if (!context)
            {
                RVX_RHI_ERROR("SubmitVulkanCommandContexts: null command context");
                return 0;
            }

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

        // Same-queue submissions are ordered by Vulkan and need no binary
        // semaphore. Compute and Copy currently alias Graphics, so keep the
        // cross-queue lifetime path dormant until distinct queues are enabled.
        const VkQueue copyQueue = device->GetTransferQueue();
        const VkQueue computeQueue = device->GetComputeQueue();
        const VkQueue graphicsQueue = device->GetGraphicsQueue();
        const bool needCopyToComputeSync =
            !copyCmdBuffers.empty() && !computeCmdBuffers.empty() &&
            copyQueue != computeQueue;
        const bool needCopyToGraphicsSync =
            !copyCmdBuffers.empty() && !graphicsCmdBuffers.empty() &&
            computeCmdBuffers.empty() && copyQueue != graphicsQueue;
        const bool needComputeToGraphicsSync =
            !computeCmdBuffers.empty() && !graphicsCmdBuffers.empty() &&
            computeQueue != graphicsQueue;

        VulkanFence* vkSignalFence = signalFence ? static_cast<VulkanFence*>(signalFence) : nullptr;
        const uint64 signalFenceValue = vkSignalFence ? vkSignalFence->AllocateSignalValue() : 0;

        const auto createSemaphore =
            [device, &semaphoreInfo](VkSemaphore& semaphore) -> bool
        {
            const VkResult result = vkCreateSemaphore(
                device->GetDevice(), &semaphoreInfo, nullptr, &semaphore);
            if (result == VK_SUCCESS)
            {
                return true;
            }
            device->ReportRuntimeFailure(
                result,
                RHIDeviceFaultOperation::CommandSubmission,
                "Vulkan cross-queue semaphore creation failed");
            return false;
        };

        const auto submit =
            [device](VkQueue queue,
                     const VkSubmitInfo& info,
                     VkFence fence,
                     const char* message) -> bool
        {
            const VkResult result = vkQueueSubmit(queue, 1, &info, fence);
            if (result == VK_SUCCESS)
            {
                return true;
            }
            device->ReportRuntimeFailure(
                result,
                RHIDeviceFaultOperation::CommandSubmission,
                message);
            return false;
        };

        bool submittedAnyBatch = false;
        VkQueue lastSubmittedQueue = VK_NULL_HANDLE;
        const auto retireCrossQueueSemaphores = [&]()
        {
            std::vector<VkSemaphore> semaphores;
            if (copyToComputeSemaphore != VK_NULL_HANDLE)
                semaphores.push_back(copyToComputeSemaphore);
            if (copyToGraphicsSemaphore != VK_NULL_HANDLE)
                semaphores.push_back(copyToGraphicsSemaphore);
            if (computeToGraphicsSemaphore != VK_NULL_HANDLE)
                semaphores.push_back(computeToGraphicsSemaphore);
            if (submittedAnyBatch)
            {
                device->EnqueueDeferredSemaphoreDestroy(
                    std::move(semaphores), lastSubmittedQueue);
            }
            else
            {
                for (VkSemaphore semaphore : semaphores)
                {
                    vkDestroySemaphore(device->GetDevice(),
                                       semaphore, nullptr);
                }
            }
            copyToComputeSemaphore = VK_NULL_HANDLE;
            copyToGraphicsSemaphore = VK_NULL_HANDLE;
            computeToGraphicsSemaphore = VK_NULL_HANDLE;
        };

        // Create synchronization semaphores as needed
        if (needCopyToComputeSync)
        {
            if (!createSemaphore(copyToComputeSemaphore))
                return 0;
        }
        if (needCopyToGraphicsSync)
        {
            if (!createSemaphore(copyToGraphicsSemaphore))
            {
                retireCrossQueueSemaphores();
                return 0;
            }
        }
        if (needComputeToGraphicsSync)
        {
            if (!createSemaphore(computeToGraphicsSemaphore))
            {
                retireCrossQueueSemaphores();
                return 0;
            }
        }

        // Submit copy commands first
        if (!copyCmdBuffers.empty())
        {
            std::vector<VkSemaphore> signalSemaphores;
            std::vector<uint64> signalValues;
            if (copyToComputeSemaphore != VK_NULL_HANDLE)
            {
                signalSemaphores.push_back(copyToComputeSemaphore);
                signalValues.push_back(0);
            }
            if (copyToGraphicsSemaphore != VK_NULL_HANDLE)
            {
                signalSemaphores.push_back(copyToGraphicsSemaphore);
                signalValues.push_back(0);
            }
            if (vkSignalFence && computeCmdBuffers.empty() && graphicsCmdBuffers.empty())
            {
                signalSemaphores.push_back(vkSignalFence->GetSemaphore());
                signalValues.push_back(signalFenceValue);
            }

            VkTimelineSemaphoreSubmitInfo timelineInfo = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
            timelineInfo.signalSemaphoreValueCount = static_cast<uint32>(signalValues.size());
            timelineInfo.pSignalSemaphoreValues = signalValues.data();

            VkSubmitInfo submitInfo = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submitInfo.pNext = (vkSignalFence && computeCmdBuffers.empty() && graphicsCmdBuffers.empty()) ? &timelineInfo : nullptr;
            submitInfo.commandBufferCount = static_cast<uint32>(copyCmdBuffers.size());
            submitInfo.pCommandBuffers = copyCmdBuffers.data();
            submitInfo.signalSemaphoreCount = static_cast<uint32>(signalSemaphores.size());
            submitInfo.pSignalSemaphores = signalSemaphores.empty() ? nullptr : signalSemaphores.data();
            if (!submit(device->GetTransferQueue(), submitInfo,
                        VK_NULL_HANDLE,
                        "Vulkan copy batch submission failed"))
            {
                retireCrossQueueSemaphores();
                return 0;
            }
            submittedAnyBatch = true;
            lastSubmittedQueue = device->GetTransferQueue();
        }

        // Submit compute commands (wait for copy if needed)
        if (!computeCmdBuffers.empty())
        {
            std::vector<VkSemaphore> waitSemaphores;
            std::vector<VkPipelineStageFlags> waitStages;
            std::vector<uint64> waitValues;
            std::vector<VkSemaphore> signalSemaphores;
            std::vector<uint64> signalValues;

            if (copyToComputeSemaphore != VK_NULL_HANDLE)
            {
                waitSemaphores.push_back(copyToComputeSemaphore);
                waitStages.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
                waitValues.push_back(0);
            }
            if (computeToGraphicsSemaphore != VK_NULL_HANDLE)
            {
                signalSemaphores.push_back(computeToGraphicsSemaphore);
                signalValues.push_back(0);
            }
            if (vkSignalFence && graphicsCmdBuffers.empty())
            {
                signalSemaphores.push_back(vkSignalFence->GetSemaphore());
                signalValues.push_back(signalFenceValue);
            }

            VkTimelineSemaphoreSubmitInfo timelineInfo = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
            timelineInfo.waitSemaphoreValueCount = static_cast<uint32>(waitValues.size());
            timelineInfo.pWaitSemaphoreValues = waitValues.data();
            timelineInfo.signalSemaphoreValueCount = static_cast<uint32>(signalValues.size());
            timelineInfo.pSignalSemaphoreValues = signalValues.data();

            VkSubmitInfo submitInfo = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submitInfo.pNext = (vkSignalFence && graphicsCmdBuffers.empty()) ? &timelineInfo : nullptr;
            submitInfo.waitSemaphoreCount = static_cast<uint32>(waitSemaphores.size());
            submitInfo.pWaitSemaphores = waitSemaphores.empty() ? nullptr : waitSemaphores.data();
            submitInfo.pWaitDstStageMask = waitStages.empty() ? nullptr : waitStages.data();
            submitInfo.commandBufferCount = static_cast<uint32>(computeCmdBuffers.size());
            submitInfo.pCommandBuffers = computeCmdBuffers.data();
            submitInfo.signalSemaphoreCount = static_cast<uint32>(signalSemaphores.size());
            submitInfo.pSignalSemaphores = signalSemaphores.empty() ? nullptr : signalSemaphores.data();
            if (!submit(device->GetComputeQueue(), submitInfo,
                        VK_NULL_HANDLE,
                        "Vulkan compute batch submission failed"))
            {
                retireCrossQueueSemaphores();
                return 0;
            }
            submittedAnyBatch = true;
            lastSubmittedQueue = device->GetComputeQueue();
        }

        // Submit graphics commands. Swapchain synchronization is attached only
        // when the caller acquired an image for this frame.
        if (!graphicsCmdBuffers.empty())
        {
            std::vector<VkSemaphore> waitSemaphores;
            std::vector<VkPipelineStageFlags> waitStages;
            std::vector<uint64> waitValues;
            VulkanSwapChain* swapChain = device->GetPrimarySwapChain();
            const bool hasAcquiredImage =
                swapChain && swapChain->HasAcquiredImage();

            if (hasAcquiredImage)
            {
                waitSemaphores.push_back(device->GetImageAvailableSemaphore());
                waitStages.push_back(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
                waitValues.push_back(0);  // Binary semaphore
            }

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

            std::vector<VkSemaphore> signalSemaphores;
            std::vector<uint64> signalValues;
            if (hasAcquiredImage)
            {
                signalSemaphores.push_back(device->GetRenderFinishedSemaphore());
                signalValues.push_back(0);  // Binary semaphore
            }

            if (vkSignalFence)
            {
                signalSemaphores.push_back(vkSignalFence->GetSemaphore());
                signalValues.push_back(signalFenceValue);
            }

            VkTimelineSemaphoreSubmitInfo timelineInfo = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
            timelineInfo.waitSemaphoreValueCount = static_cast<uint32>(waitValues.size());
            timelineInfo.pWaitSemaphoreValues = waitValues.data();
            timelineInfo.signalSemaphoreValueCount = static_cast<uint32>(signalValues.size());
            timelineInfo.pSignalSemaphoreValues = signalValues.data();

            VkSubmitInfo submitInfo = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submitInfo.pNext = vkSignalFence ? &timelineInfo : nullptr;
            submitInfo.waitSemaphoreCount = static_cast<uint32>(waitSemaphores.size());
            submitInfo.pWaitSemaphores = waitSemaphores.empty() ? nullptr : waitSemaphores.data();
            submitInfo.pWaitDstStageMask = waitStages.empty() ? nullptr : waitStages.data();
            submitInfo.commandBufferCount = static_cast<uint32>(graphicsCmdBuffers.size());
            submitInfo.pCommandBuffers = graphicsCmdBuffers.data();
            submitInfo.signalSemaphoreCount = static_cast<uint32>(signalSemaphores.size());
            submitInfo.pSignalSemaphores = signalSemaphores.empty() ? nullptr : signalSemaphores.data();

            const VkFence frameFence = device->GetArmedFrameFenceForSubmission();
            if (!submit(device->GetGraphicsQueue(),
                        submitInfo,
                        frameFence,
                        "Vulkan graphics batch submission failed"))
            {
                retireCrossQueueSemaphores();
                return 0;
            }
            if (frameFence != VK_NULL_HANDLE)
            {
                device->MarkArmedFrameFenceSubmitted(frameFence);
            }
            submittedAnyBatch = true;
            lastSubmittedQueue = device->GetGraphicsQueue();
        }
        else if (vkSignalFence && copyCmdBuffers.empty() && computeCmdBuffers.empty())
        {
            // No graphics commands but we need to signal the fence
            uint64 signalValue = signalFenceValue;

            VkTimelineSemaphoreSubmitInfo timelineInfo = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
            timelineInfo.signalSemaphoreValueCount = 1;
            timelineInfo.pSignalSemaphoreValues = &signalValue;

            VkSemaphore signalSemaphore = vkSignalFence->GetSemaphore();
            VkSubmitInfo submitInfo = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submitInfo.pNext = &timelineInfo;
            submitInfo.signalSemaphoreCount = 1;
            submitInfo.pSignalSemaphores = &signalSemaphore;

            if (!submit(device->GetGraphicsQueue(), submitInfo,
                        VK_NULL_HANDLE,
                        "Vulkan empty timeline submission failed"))
            {
                retireCrossQueueSemaphores();
                return 0;
            }
            submittedAnyBatch = true;
            lastSubmittedQueue = device->GetGraphicsQueue();
        }

        if (copyToComputeSemaphore != VK_NULL_HANDLE ||
            copyToGraphicsSemaphore != VK_NULL_HANDLE ||
            computeToGraphicsSemaphore != VK_NULL_HANDLE)
        {
            std::vector<VkSemaphore> semaphoresToDestroy;
            if (copyToComputeSemaphore != VK_NULL_HANDLE)
                semaphoresToDestroy.push_back(copyToComputeSemaphore);
            if (copyToGraphicsSemaphore != VK_NULL_HANDLE)
                semaphoresToDestroy.push_back(copyToGraphicsSemaphore);
            if (computeToGraphicsSemaphore != VK_NULL_HANDLE)
                semaphoresToDestroy.push_back(computeToGraphicsSemaphore);

            VkQueue finalQueue = device->GetTransferQueue();
            if (!graphicsCmdBuffers.empty())
                finalQueue = device->GetGraphicsQueue();
            else if (!computeCmdBuffers.empty())
                finalQueue = device->GetComputeQueue();

            device->EnqueueDeferredSemaphoreDestroy(std::move(semaphoresToDestroy), finalQueue);
        }
        return signalFenceValue;
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
    // Synchronization
    // =============================================================================
    void VulkanCommandContext::SignalFence(RHIFence* fence, uint64 value)
    {
        if (fence)
        {
            fence->SignalOnQueue(value, m_queueType);
        }
    }

    void VulkanCommandContext::WaitFence(RHIFence* fence, uint64 value)
    {
        if (fence)
        {
            RVX_RHI_ERROR(
                "VulkanCommandContext::WaitFence is unsupported in this backend revision; "
                "RHICapabilities::supportsQueueFenceWait is false (requested value {})",
                value);
        }
    }

    // =============================================================================
    // Split Barriers
    // =============================================================================
    void VulkanCommandContext::BeginBarrier(const RHIBufferBarrier& barrier)
    {
        (void)barrier;
        RVX_RHI_WARN("VulkanCommandContext::BeginBarrier is unsupported; RHICapabilities::supportsSplitBarrier is false");
    }

    void VulkanCommandContext::BeginBarrier(const RHITextureBarrier& barrier)
    {
        (void)barrier;
        RVX_RHI_WARN("VulkanCommandContext::BeginBarrier is unsupported; RHICapabilities::supportsSplitBarrier is false");
    }

    void VulkanCommandContext::EndBarrier(const RHIBufferBarrier& barrier)
    {
        BufferBarrier(barrier);
    }

    void VulkanCommandContext::EndBarrier(const RHITextureBarrier& barrier)
    {
        TextureBarrier(barrier);
    }

} // namespace RVX
