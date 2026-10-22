#include "VulkanCommandContext.h"
#include "VulkanDevice.h"
#include "VulkanResources.h"
#include "VulkanPipeline.h"
#include "VulkanRayTracing.h"
#include "VulkanSwapChain.h"
#include "RHI/RHIDescriptor.h"
#include "RHI/RHIIndirectExecution.h"
#include "RHI/RHITexture.h"

#include <limits>
#include <utility>

namespace RVX
{
    VulkanTextureCopyContract ResolveVulkanTextureCopyContract(
        RHIFormat sourceFormat,
        RHIFormat destinationFormat) noexcept
    {
        if (ToVkFormat(sourceFormat) == VK_FORMAT_UNDEFINED ||
            ToVkFormat(destinationFormat) == VK_FORMAT_UNDEFINED)
        {
            return {VulkanTextureCopyContractStatus::UnknownFormat, 0};
        }

        const bool sourceIsDepth = IsDepthFormat(sourceFormat);
        const bool destinationIsDepth = IsDepthFormat(destinationFormat);
        if (sourceIsDepth != destinationIsDepth)
        {
            return {VulkanTextureCopyContractStatus::AspectMismatch, 0};
        }

        if (sourceFormat != destinationFormat)
        {
            return {VulkanTextureCopyContractStatus::FormatMismatch, 0};
        }

        // RHITextureCopyDesc does not select an individual depth/stencil
        // aspect. Recording a single region would therefore either violate
        // VkImageSubresourceLayers or silently omit part of the resource.
        if (IsStencilFormat(sourceFormat))
        {
            return {
                VulkanTextureCopyContractStatus::UnsupportedStencilAspect,
                0};
        }

        return {
            VulkanTextureCopyContractStatus::Compatible,
            static_cast<VkImageAspectFlags>(
                sourceIsDepth ? VK_IMAGE_ASPECT_DEPTH_BIT
                              : VK_IMAGE_ASPECT_COLOR_BIT)};
    }

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

        GPUQueueDomain GetContextQueueDomain(VulkanDevice* device,
                                             RHICommandQueueType queueType)
        {
            GPUQueueDomain domain = GPUQueueDomain::Graphics;
            static_cast<void>(TryGetGPUQueueDomain(
                device->GetCapabilities().queueTopology,
                queueType,
                domain));
            return domain;
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

        bool ValidateVulkanRayTracingCommandState(bool isRecording,
                                                  bool inRenderPass,
                                                  const char* operation)
        {
            if (!isRecording)
            {
                RVX_RHI_ERROR(
                    "VulkanCommandContext: {} requires active command recording",
                    operation);
                return false;
            }

            if (inRenderPass)
            {
                RVX_RHI_ERROR(
                    "VulkanCommandContext: {} cannot run inside a render pass",
                    operation);
                return false;
            }

            return true;
        }

        bool ValidateVulkanBufferAddressRange(const char* operation,
                                              VulkanBuffer* buffer,
                                              uint64 offset,
                                              uint64 requiredBytes,
                                              const char* label)
        {
            if (!buffer || buffer->GetBuffer() == VK_NULL_HANDLE ||
                buffer->GetDeviceAddress() == 0)
            {
                RVX_RHI_ERROR(
                    "VulkanCommandContext: {} requires a GPU-addressable {}",
                    operation,
                    label);
                return false;
            }

            if (!IsRHIRayTracingBufferRangeValid(*buffer, offset, requiredBytes))
            {
                RVX_RHI_ERROR(
                    "VulkanCommandContext: {} {} range exceeds buffer size",
                    operation,
                    label);
                return false;
            }

            if (offset > std::numeric_limits<VkDeviceAddress>::max() -
                             buffer->GetDeviceAddress())
            {
                RVX_RHI_ERROR(
                    "VulkanCommandContext: {} {} device address overflowed",
                    operation,
                    label);
                return false;
            }

            return true;
        }

        bool ValidateVulkanBufferEffectiveAddressAlignment(
            const char* operation,
            VulkanBuffer* buffer,
            uint64 offset,
            VkDeviceSize requiredAlignment,
            const char* label)
        {
            if (!buffer || buffer->GetDeviceAddress() == 0 ||
                requiredAlignment == 0 ||
                offset > std::numeric_limits<VkDeviceAddress>::max() -
                             buffer->GetDeviceAddress())
            {
                RVX_RHI_ERROR(
                    "VulkanCommandContext: {} {} has no valid effective device address",
                    operation,
                    label);
                return false;
            }

            const VkDeviceAddress effectiveAddress =
                buffer->GetDeviceAddress() + offset;
            if (effectiveAddress % requiredAlignment != 0)
            {
                RVX_RHI_ERROR(
                    "VulkanCommandContext: {} {} effective device address does not satisfy Vulkan's {}-byte alignment",
                    operation,
                    label,
                    requiredAlignment);
                return false;
            }

            return true;
        }

        bool ValidateVulkanBLASGeometryInputAddresses(
            const RHIBottomLevelASDesc& desc)
        {
            for (uint32 geometryIndex = 0;
                 geometryIndex < desc.geometries.size();
                 ++geometryIndex)
            {
                const RHIRayTracingGeometryDesc& geometry =
                    desc.geometries[geometryIndex];
                if (geometry.type == RHIRayTracingGeometryType::Triangles)
                {
                    const RHIRayTracingTrianglesDesc& triangles =
                        geometry.triangles;
                    const uint32 vertexFormatSize =
                        GetFormatBytesPerPixel(triangles.vertexFormat);
                    uint64 vertexRangeBytes = 0;
                    if (!TryGetRHIRayTracingStridedRangeSize(
                            triangles.vertexCount,
                            triangles.vertexStride,
                            vertexFormatSize,
                            vertexRangeBytes))
                    {
                        RVX_RHI_ERROR(
                            "VulkanCommandContext: BLAS geometry {} vertex range size overflowed",
                            geometryIndex);
                        return false;
                    }

                    if (!ValidateVulkanBufferAddressRange(
                            "BLAS geometry input",
                            dynamic_cast<VulkanBuffer*>(triangles.vertexBuffer),
                            triangles.vertexOffset,
                            vertexRangeBytes,
                            "vertex buffer") ||
                        !ValidateVulkanBufferEffectiveAddressAlignment(
                            "BLAS geometry input",
                            dynamic_cast<VulkanBuffer*>(triangles.vertexBuffer),
                            triangles.vertexOffset,
                            sizeof(float),
                            "vertex buffer"))
                    {
                        return false;
                    }

                    if (triangles.indexBuffer)
                    {
                        const uint64 indexElementSize =
                            triangles.indexFormat == RHIFormat::R16_UINT
                            ? 2ull
                            : 4ull;
                        uint64 indexRangeBytes = 0;
                        if (!TryMultiplyRHIRayTracingCount(
                                triangles.indexCount,
                                indexElementSize,
                                indexRangeBytes) ||
                            !ValidateVulkanBufferAddressRange(
                                "BLAS geometry input",
                                dynamic_cast<VulkanBuffer*>(
                                    triangles.indexBuffer),
                                triangles.indexOffset,
                                indexRangeBytes,
                                "index buffer"))
                        {
                            RVX_RHI_ERROR(
                                "VulkanCommandContext: BLAS geometry {} index range is invalid",
                                geometryIndex);
                            return false;
                        }

                        if (!ValidateVulkanBufferEffectiveAddressAlignment(
                                "BLAS geometry input",
                                dynamic_cast<VulkanBuffer*>(
                                    triangles.indexBuffer),
                                triangles.indexOffset,
                                indexElementSize,
                                "index buffer"))
                        {
                            return false;
                        }
                    }

                    if (triangles.transformBuffer)
                    {
                        auto* transformBuffer = dynamic_cast<VulkanBuffer*>(
                            triangles.transformBuffer);
                        if (!ValidateVulkanBufferAddressRange(
                                "BLAS geometry input",
                                transformBuffer,
                                triangles.transformOffset,
                                sizeof(float) * 12,
                                "transform buffer") ||
                            !ValidateVulkanBufferEffectiveAddressAlignment(
                                "BLAS geometry input",
                                transformBuffer,
                                triangles.transformOffset,
                                16,
                                "transform buffer"))
                        {
                            return false;
                        }
                    }
                }
                else
                {
                    const RHIRayTracingAABBDesc& aabbs = geometry.aabbs;
                    auto* aabbBuffer =
                        dynamic_cast<VulkanBuffer*>(aabbs.aabbBuffer);
                    uint64 aabbRangeBytes = 0;
                    if (!TryGetRHIRayTracingStridedRangeSize(
                            aabbs.count,
                            aabbs.stride,
                            24,
                            aabbRangeBytes) ||
                        !ValidateVulkanBufferAddressRange(
                            "BLAS geometry input",
                            aabbBuffer,
                            aabbs.offset,
                            aabbRangeBytes,
                            "AABB buffer") ||
                        aabbs.stride % 8u != 0u ||
                        !ValidateVulkanBufferEffectiveAddressAlignment(
                            "BLAS geometry input",
                            aabbBuffer,
                            aabbs.offset,
                            8,
                            "AABB buffer"))
                    {
                        RVX_RHI_ERROR(
                            "VulkanCommandContext: BLAS geometry {} AABB range or native alignment is invalid",
                            geometryIndex);
                        return false;
                    }
                }
            }

            return true;
        }

        uint32 GetVulkanAccelerationStructurePrimitiveCount(
            const RHIRayTracingGeometryDesc& geometry)
        {
            if (geometry.type == RHIRayTracingGeometryType::AABBs)
            {
                return geometry.aabbs.count;
            }

            return geometry.triangles.indexBuffer
                ? geometry.triangles.indexCount / 3u
                : geometry.triangles.vertexCount / 3u;
        }

        VulkanAccelerationStructureBuildSignature
        MakeVulkanBottomLevelASBuildSignature(
            const RHIBottomLevelASDesc& desc)
        {
            VulkanAccelerationStructureBuildSignature signature;
            signature.type = RHIAccelerationStructureType::BottomLevel;
            signature.buildFlags = desc.buildFlags;
            signature.geometries.reserve(desc.geometries.size());

            for (const RHIRayTracingGeometryDesc& geometry : desc.geometries)
            {
                VulkanAccelerationStructureGeometryBuildSignature
                    geometrySignature;
                geometrySignature.flags = geometry.flags;
                geometrySignature.primitiveCount =
                    GetVulkanAccelerationStructurePrimitiveCount(geometry);

                if (geometry.type == RHIRayTracingGeometryType::Triangles)
                {
                    const RHIRayTracingTrianglesDesc& triangles =
                        geometry.triangles;
                    geometrySignature.type =
                        VulkanAccelerationStructureSignatureGeometryType::
                            Triangles;
                    geometrySignature.vertexFormat = triangles.vertexFormat;
                    geometrySignature.maxVertex = triangles.vertexCount - 1u;
                    geometrySignature.indexFormat = triangles.indexBuffer
                        ? triangles.indexFormat
                        : RHIFormat::Unknown;
                    geometrySignature.hasTransform =
                        triangles.transformBuffer != nullptr;
                }
                else
                {
                    geometrySignature.type =
                        VulkanAccelerationStructureSignatureGeometryType::
                            AABBs;
                    geometrySignature.aabbStride = geometry.aabbs.stride;
                }

                signature.geometries.push_back(geometrySignature);
            }

            return signature;
        }

        VulkanAccelerationStructureBuildSignature
        MakeVulkanTopLevelASBuildSignature(
            const RHITopLevelASDesc& desc)
        {
            VulkanAccelerationStructureBuildSignature signature;
            signature.type = RHIAccelerationStructureType::TopLevel;
            signature.buildFlags = desc.buildFlags;

            VulkanAccelerationStructureGeometryBuildSignature
                geometrySignature;
            geometrySignature.type =
                VulkanAccelerationStructureSignatureGeometryType::Instances;
            geometrySignature.primitiveCount = desc.GetInstanceCount();
            // The RHI's GPU-instance-buffer contract is a contiguous array of
            // VkAccelerationStructureInstanceKHR records. The Vulkan build
            // translation uses arrayOfPointers = VK_FALSE.
            geometrySignature.instancesArrayOfPointers = false;
            signature.geometries.push_back(geometrySignature);
            return signature;
        }

        bool ValidateVulkanASBuildResources(
            const char* operation,
            RHIAccelerationStructureType expectedType,
            const RHIAccelerationStructureBuildSizes& sizes,
            bool update,
            VulkanAccelerationStructure* dstAS,
            VulkanAccelerationStructure* srcAS,
            VulkanBuffer* scratch,
            uint64 scratchOffset,
            VkDeviceSize scratchAlignment)
        {
            if (!sizes.IsValid())
            {
                RVX_RHI_ERROR(
                    "VulkanCommandContext: {} requires valid acceleration structure build sizes",
                    operation);
                return false;
            }

            if (update && !sizes.HasValidUpdateScratchSize())
            {
                RVX_RHI_ERROR(
                    "VulkanCommandContext: {} update requires valid update scratch size",
                    operation);
                return false;
            }

            if (!dstAS || dstAS->GetAccelerationStructure() == VK_NULL_HANDLE ||
                dstAS->GetDeviceAddress() == 0 ||
                dstAS->GetType() != expectedType)
            {
                RVX_RHI_ERROR(
                    "VulkanCommandContext: {} requires a live destination AS of the expected type",
                    operation);
                return false;
            }

            const auto isValidStorage = [](VulkanAccelerationStructure* as)
            {
                auto* storage = as
                    ? dynamic_cast<VulkanBuffer*>(as->GetStorageBuffer())
                    : nullptr;
                return storage && storage->GetBuffer() != VK_NULL_HANDLE &&
                       storage->GetDeviceAddress() != 0 &&
                       storage->GetSize() >= as->GetSize() &&
                       HasFlag(storage->GetUsage(),
                               RHIBufferUsage::AccelerationStructureStorage) &&
                       HasFlag(storage->GetUsage(),
                               RHIBufferUsage::DeviceAddress);
            };
            if (!isValidStorage(dstAS))
            {
                RVX_RHI_ERROR(
                    "VulkanCommandContext: {} destination AS storage lacks Vulkan acceleration-structure usage or an address",
                    operation);
                return false;
            }

            if (update &&
                (!srcAS ||
                 srcAS->GetAccelerationStructure() == VK_NULL_HANDLE ||
                 srcAS->GetDeviceAddress() == 0 ||
                 srcAS->GetType() != expectedType))
            {
                RVX_RHI_ERROR(
                    "VulkanCommandContext: {} update requires a live source AS of the expected type",
                    operation);
                return false;
            }

            if (update && !isValidStorage(srcAS))
            {
                RVX_RHI_ERROR(
                    "VulkanCommandContext: {} source AS storage lacks Vulkan acceleration-structure usage or an address",
                    operation);
                return false;
            }

            if (dstAS->GetSize() < sizes.accelerationStructureSize ||
                (update && srcAS->GetSize() < sizes.accelerationStructureSize))
            {
                RVX_RHI_ERROR(
                    "VulkanCommandContext: {} acceleration structure storage is too small",
                    operation);
                return false;
            }

            if (!scratch || scratch->GetBuffer() == VK_NULL_HANDLE ||
                scratch->GetDeviceAddress() == 0 ||
                scratch->GetMemoryType() != RHIMemoryType::Default ||
                !HasFlag(scratch->GetUsage(), RHIBufferUsage::UnorderedAccess) ||
                !HasFlag(scratch->GetUsage(), RHIBufferUsage::DeviceAddress))
            {
                RVX_RHI_ERROR(
                    "VulkanCommandContext: {} requires a default-memory GPU-addressable unordered-access scratch buffer",
                    operation);
                return false;
            }

            if (scratchAlignment == 0 ||
                !ValidateVulkanBufferEffectiveAddressAlignment(
                    operation,
                    scratch,
                    scratchOffset,
                    scratchAlignment,
                    "scratch buffer"))
            {
                RVX_RHI_ERROR(
                    "VulkanCommandContext: {} scratch effective address must satisfy Vulkan's scratch alignment",
                    operation);
                return false;
            }

            const uint64 requiredScratchSize = update
                ? sizes.updateScratchSize
                : sizes.buildScratchSize;
            if (requiredScratchSize == 0 ||
                !IsRHIRayTracingBufferRangeValid(
                    *scratch, scratchOffset, requiredScratchSize) ||
                scratchOffset > std::numeric_limits<VkDeviceAddress>::max() -
                                    scratch->GetDeviceAddress())
            {
                RVX_RHI_ERROR(
                    "VulkanCommandContext: {} scratch range is invalid",
                    operation);
                return false;
            }

            return true;
        }

        bool ValidateVulkanShaderTableRegion(
            const VkStridedDeviceAddressRegionKHR& region,
            const VkPhysicalDeviceRayTracingPipelinePropertiesKHR& properties,
            bool required,
            const char* label)
        {
            if (region.deviceAddress == 0 && region.stride == 0 &&
                region.size == 0)
            {
                if (!required)
                {
                    return true;
                }

                RVX_RHI_ERROR(
                    "VulkanCommandContext: DispatchRays requires a non-empty {} shader table region",
                    label);
                return false;
            }

            if (region.deviceAddress == 0 || region.stride == 0 ||
                region.size == 0 ||
                properties.shaderGroupHandleAlignment == 0 ||
                properties.shaderGroupBaseAlignment == 0 ||
                region.deviceAddress % properties.shaderGroupBaseAlignment != 0 ||
                region.stride % properties.shaderGroupHandleAlignment != 0 ||
                region.stride > properties.maxShaderGroupStride ||
                region.size < region.stride ||
                region.size % region.stride != 0)
            {
                RVX_RHI_ERROR(
                    "VulkanCommandContext: DispatchRays {} shader table region is invalid",
                    label);
                return false;
            }

            if (required && region.size != region.stride)
            {
                RVX_RHI_ERROR(
                    "VulkanCommandContext: DispatchRays ray-generation shader table region must contain exactly one record");
                return false;
            }

            return true;
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
        DiscardPendingAccelerationStructureCompactionQueries();
        DiscardPendingAccelerationStructureBuilds();
        if (m_commandBuffer)
        {
            vkFreeCommandBuffers(m_device->GetDevice(), m_commandPool, 1, &m_commandBuffer);
        }
    }

    void VulkanCommandContext::Begin()
    {
        if (m_isRecording)
            return;

        // vkResetCommandBuffer discards any previous recording. Its pending AS
        // builds must not become globally visible before queue submission.
        DiscardPendingAccelerationStructureCompactionQueries();
        DiscardPendingAccelerationStructureBuilds();
        m_pendingImageBarriers.clear();
        m_pendingBufferBarriers.clear();
        VK_CHECK(vkResetCommandBuffer(m_commandBuffer, 0));

        VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

        VK_CHECK(vkBeginCommandBuffer(m_commandBuffer, &beginInfo));
        m_isRecording = true;
        m_currentPipeline = nullptr;
        m_boundRayTracingDescriptorSetLayouts.clear();
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
        DiscardPendingAccelerationStructureCompactionQueries();
        DiscardPendingAccelerationStructureBuilds();
        m_pendingImageBarriers.clear();
        m_pendingBufferBarriers.clear();
        VK_CHECK(vkResetCommandBuffer(m_commandBuffer, 0));
        m_isRecording = false;
        m_inRenderPass = false;
        m_currentPipeline = nullptr;
        m_boundRayTracingDescriptorSetLayouts.clear();
    }

    bool VulkanCommandContext::HasCompatiblePendingAccelerationStructureBuild(
        const RHIAccelerationStructure* accelerationStructure) const noexcept
    {
        if (!accelerationStructure)
        {
            return false;
        }

        for (const PendingAccelerationStructureBuild& pending :
             m_pendingAccelerationStructureBuilds)
        {
            if (pending.accelerationStructure.Get() == accelerationStructure)
            {
                return true;
            }
        }
        return false;
    }

    bool VulkanCommandContext::HasPendingAccelerationStructureBuild(
        const VulkanAccelerationStructure* accelerationStructure,
        bool requireCompaction) const noexcept
    {
        if (!accelerationStructure)
        {
            return false;
        }

        for (const PendingAccelerationStructureBuild& pending :
             m_pendingAccelerationStructureBuilds)
        {
            if (pending.accelerationStructure.Get() != accelerationStructure)
            {
                continue;
            }

            return !requireCompaction || HasFlag(
                pending.signature.buildFlags,
                RHIAccelerationStructureBuildFlags::AllowCompaction);
        }
        return false;
    }

    bool VulkanCommandContext::ValidateAccelerationStructureUpdateSignature(
        const char* operation,
        VulkanAccelerationStructure* source,
        const VulkanAccelerationStructureBuildSignature& update) const
    {
        if (!source)
        {
            RVX_RHI_ERROR(
                "VulkanCommandContext: {} update requires a Vulkan source AS",
                operation);
            return false;
        }

        VulkanAccelerationStructureUpdateCompatibility compatibility =
            VulkanAccelerationStructureUpdateCompatibility::
                SourceHasNoSubmittedBuild;
        for (auto it = m_pendingAccelerationStructureBuilds.rbegin();
             it != m_pendingAccelerationStructureBuilds.rend();
             ++it)
        {
            if (it->accelerationStructure.Get() != source)
            {
                continue;
            }

            if (!HasFlag(it->signature.buildFlags,
                         RHIAccelerationStructureBuildFlags::AllowUpdate))
            {
                compatibility =
                    VulkanAccelerationStructureUpdateCompatibility::
                        SourceBuildDisallowsUpdate;
            }
            else
            {
                compatibility =
                    GetVulkanAccelerationStructureUpdateCompatibility(
                        it->signature,
                        update);
            }
            break;
        }

        if (compatibility ==
            VulkanAccelerationStructureUpdateCompatibility::
                SourceHasNoSubmittedBuild)
        {
            compatibility = source->GetSubmittedBuildUpdateCompatibility(update);
        }

        if (compatibility ==
            VulkanAccelerationStructureUpdateCompatibility::Compatible)
        {
            return true;
        }

        RVX_RHI_ERROR(
            "VulkanCommandContext: {} update rejected because its source build is incompatible: {}",
            operation,
            GetVulkanAccelerationStructureUpdateCompatibilityName(
                compatibility));
        return false;
    }

    void VulkanCommandContext::RecordPendingAccelerationStructureBuild(
        VulkanAccelerationStructure* accelerationStructure,
        VulkanAccelerationStructureBuildSignature signature)
    {
        if (!accelerationStructure)
        {
            return;
        }

        for (PendingAccelerationStructureBuild& pending :
             m_pendingAccelerationStructureBuilds)
        {
            if (pending.accelerationStructure.Get() == accelerationStructure)
            {
                pending.signature = std::move(signature);
                return;
            }
        }

        PendingAccelerationStructureBuild pending;
        pending.accelerationStructure =
            RHIAccelerationStructureRef(accelerationStructure);
        pending.signature = std::move(signature);
        m_pendingAccelerationStructureBuilds.push_back(std::move(pending));
    }

    void VulkanCommandContext::CommitPendingAccelerationStructureBuilds()
    {
        for (PendingAccelerationStructureBuild& pending :
             m_pendingAccelerationStructureBuilds)
        {
            auto* accelerationStructure =
                dynamic_cast<VulkanAccelerationStructure*>(
                    pending.accelerationStructure.Get());
            if (accelerationStructure)
            {
                accelerationStructure->CommitSubmittedBuild(
                    std::move(pending.signature), m_queueType);
            }
        }
        m_pendingAccelerationStructureBuilds.clear();
    }

    void VulkanCommandContext::DiscardPendingAccelerationStructureBuilds()
    {
        m_pendingAccelerationStructureBuilds.clear();
    }

    void VulkanCommandContext::CommitPendingAccelerationStructureCompactionQueries()
    {
        for (PendingAccelerationStructureCompactionQuery& pending :
             m_pendingAccelerationStructureCompactionQueries)
        {
            auto* query = dynamic_cast<
                VulkanAccelerationStructureCompactedSizeQuery*>(
                pending.query.Get());
            if (query)
            {
                query->Commit();
            }
        }
        m_pendingAccelerationStructureCompactionQueries.clear();
    }

    void VulkanCommandContext::DiscardPendingAccelerationStructureCompactionQueries()
    {
        for (PendingAccelerationStructureCompactionQuery& pending :
             m_pendingAccelerationStructureCompactionQueries)
        {
            auto* query = dynamic_cast<
                VulkanAccelerationStructureCompactedSizeQuery*>(
                pending.query.Get());
            if (query)
            {
                query->Discard();
            }
        }
        m_pendingAccelerationStructureCompactionQueries.clear();
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

        if (barrier.hasScopedAccess &&
            barrier.accessBefore.domain != barrier.accessAfter.domain &&
            GetQueueFamilyIndex(m_device, barrier.accessBefore.domain) ==
                GetQueueFamilyIndex(m_device, barrier.accessAfter.domain))
        {
            const GPUQueueDomain contextDomain =
                GetContextQueueDomain(m_device, m_queueType);
            if (contextDomain == barrier.accessBefore.domain)
            {
                // Same-family transfers need one transition after the queue
                // dependency, not duplicate release/acquire barriers.
                return;
            }
            if (contextDomain != barrier.accessAfter.domain)
            {
                RVX_RHI_ERROR(
                    "Vulkan same-family buffer transfer was recorded on an unrelated queue");
                return;
            }
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

        if (barrier.hasScopedAccess &&
            barrier.accessBefore.domain != barrier.accessAfter.domain &&
            GetQueueFamilyIndex(m_device, barrier.accessBefore.domain) ==
                GetQueueFamilyIndex(m_device, barrier.accessAfter.domain))
        {
            const GPUQueueDomain contextDomain =
                GetContextQueueDomain(m_device, m_queueType);
            if (contextDomain == barrier.accessBefore.domain)
            {
                return;
            }
            if (contextDomain != barrier.accessAfter.domain)
            {
                RVX_RHI_ERROR(
                    "Vulkan same-family texture transfer was recorded on an unrelated queue");
                return;
            }
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
        m_currentPipeline = nullptr;
        m_boundRayTracingDescriptorSetLayouts.clear();

        if (!pipeline)
        {
            RVX_RHI_ERROR("VulkanCommandContext: SetPipeline requires a pipeline");
            return;
        }

        auto* vkPipeline = static_cast<VulkanPipeline*>(pipeline);
        if (vkPipeline->GetPipeline() == VK_NULL_HANDLE)
        {
            RVX_RHI_ERROR("VulkanCommandContext: SetPipeline requires a valid native pipeline");
            return;
        }

        if (vkPipeline->IsRayTracing())
        {
            if (!m_device->GetCapabilities().supportsRaytracingPipeline ||
                !m_device->GetRayTracingFunctions().IsComplete())
            {
                RVX_RHI_ERROR(
                    "VulkanCommandContext: ray tracing pipeline binding requires a complete enabled Vulkan ray tracing implementation");
                return;
            }

            if (m_queueType == RHICommandQueueType::Copy)
            {
                RVX_RHI_ERROR(
                    "VulkanCommandContext: ray tracing pipeline binding cannot run on the copy queue");
                return;
            }

            if (!m_isRecording)
            {
                RVX_RHI_ERROR(
                    "VulkanCommandContext: ray tracing pipeline binding requires active command recording");
                return;
            }

            if (m_inRenderPass)
            {
                RVX_RHI_ERROR(
                    "VulkanCommandContext: ray tracing pipeline binding cannot run inside a render pass");
                return;
            }

            auto* pipelineLayout = vkPipeline->GetDescriptorPipelineLayout();
            if (!pipelineLayout ||
                vkPipeline->GetPipelineLayout() == VK_NULL_HANDLE)
            {
                RVX_RHI_ERROR(
                    "VulkanCommandContext: ray tracing pipeline binding requires a pipeline layout");
                return;
            }

            vkCmdBindPipeline(
                m_commandBuffer,
                VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                vkPipeline->GetPipeline());
            m_boundRayTracingDescriptorSetLayouts.resize(
                pipelineLayout->GetDescriptorSetLayouts().size(), nullptr);
            m_currentPipeline = vkPipeline;
            return;
        }

        const VkPipelineBindPoint bindPoint = vkPipeline->IsCompute()
            ? VK_PIPELINE_BIND_POINT_COMPUTE
            : VK_PIPELINE_BIND_POINT_GRAPHICS;
        vkCmdBindPipeline(m_commandBuffer, bindPoint, vkPipeline->GetPipeline());
        m_currentPipeline = vkPipeline;
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

        const bool bindingRayTracingPipeline = m_currentPipeline->IsRayTracing();
        if (bindingRayTracingPipeline &&
            (!m_isRecording || m_inRenderPass))
        {
            RVX_RHI_ERROR(
                "VulkanCommandContext: ray tracing descriptor set binding requires active command recording outside a render pass");
            return;
        }

        if (bindingRayTracingPipeline)
        {
            for (const RHIDescriptorBinding& binding :
                 set->GetDescriptorSnapshot())
            {
                if (!binding.accelerationStructure)
                {
                    continue;
                }

                auto* accelerationStructure =
                    dynamic_cast<VulkanAccelerationStructure*>(
                        binding.accelerationStructure);
                if (!accelerationStructure ||
                    (!accelerationStructure->HasSubmittedBuild() &&
                     !HasCompatiblePendingAccelerationStructureBuild(
                         accelerationStructure)))
                {
                    RVX_RHI_ERROR(
                        "VulkanCommandContext: ray tracing descriptor set {} references an acceleration structure that is neither submitted nor built by this command context",
                        slot);
                    return;
                }
            }
        }

        auto* vkSet = static_cast<VulkanDescriptorSet*>(set);
        VulkanPipelineLayout* pipelineLayout = m_currentPipeline->GetDescriptorPipelineLayout();
        if (!pipelineLayout)
        {
            RVX_RHI_ERROR("VulkanCommandContext: descriptor binding requires a pipeline layout");
            return;
        }
        const auto& expectedLayouts = pipelineLayout->GetDescriptorSetLayouts();
        const RHIDescriptorSetLayout* const expectedLayout =
            slot < expectedLayouts.size() ? expectedLayouts[slot] : nullptr;
        VulkanDescriptorSetLayout* const actualLayout = vkSet->GetLayout();
        const bool layoutMatches = actualLayout != nullptr &&
            actualLayout == expectedLayout;
        const bool readyForBinding = expectedLayout != nullptr &&
            vkSet->IsReadyForBinding(expectedLayout);
        if (!layoutMatches || !readyForBinding)
        {
            RVX_RHI_ERROR(
                "VulkanCommandContext: descriptor set layout does not match pipeline '{}' slot {} "
                "(actual='{}' {}, expected='{}' {}, ready={})",
                m_currentPipeline->GetDebugName(),
                slot,
                actualLayout ? actualLayout->GetDebugName() : "<null>",
                static_cast<const void*>(actualLayout),
                expectedLayout ? expectedLayout->GetDebugName() : "<null>",
                static_cast<const void*>(expectedLayout),
                readyForBinding);
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

        const VkPipelineBindPoint bindPoint = bindingRayTracingPipeline
            ? VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR
            : (m_currentPipeline->IsCompute()
                   ? VK_PIPELINE_BIND_POINT_COMPUTE
                   : VK_PIPELINE_BIND_POINT_GRAPHICS);

        vkCmdBindDescriptorSets(m_commandBuffer, bindPoint, m_currentPipeline->GetPipelineLayout(),
            slot, 1, &descriptorSet,
            static_cast<uint32>(dynamicOffsets.size()), dynamicOffsets.data());
        if (bindingRayTracingPipeline)
        {
            if (slot >= m_boundRayTracingDescriptorSetLayouts.size())
            {
                m_boundRayTracingDescriptorSetLayouts.resize(slot + 1, nullptr);
            }
            m_boundRayTracingDescriptorSetLayouts[slot] = actualLayout;
        }
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

    // =============================================================================
    // Ray Tracing Commands
    // =============================================================================
    void VulkanCommandContext::BuildBottomLevelAccelerationStructure(
        RHIAccelerationStructure* dst,
        const RHIBottomLevelASDesc& desc,
        RHIBuffer* scratchBuffer,
        uint64 scratchOffset,
        RHIAccelerationStructure* src)
    {
        if (!m_device->GetCapabilities().supportsRaytracing)
        {
            RVX_RHI_WARN(
                "VulkanCommandContext: BLAS build skipped because ray tracing is unsupported");
            return;
        }

        if (m_queueType == RHICommandQueueType::Copy)
        {
            RVX_RHI_ERROR(
                "VulkanCommandContext: BLAS build cannot run on the copy queue");
            return;
        }

        if (!ValidateVulkanRayTracingCommandState(
                m_isRecording, m_inRenderPass, "BLAS build"))
        {
            return;
        }

        const RHIRayTracingValidationResult descValidation =
            ValidateRHIBottomLevelASDesc(desc);
        if (!descValidation)
        {
            RVX_RHI_ERROR(
                "VulkanCommandContext: invalid BLAS build desc: {}",
                descValidation.message);
            return;
        }

        if (!ValidateVulkanRayTracingBuildFlags(
                m_device->GetCapabilities(), desc.buildFlags, "BLAS build"))
        {
            return;
        }

        const VulkanRayTracingFunctions& functions =
            m_device->GetRayTracingFunctions();
        if (!functions.IsComplete())
        {
            RVX_RHI_ERROR(
                "VulkanCommandContext: BLAS build requires complete Vulkan ray tracing entry points");
            return;
        }

        auto* dstAS = dynamic_cast<VulkanAccelerationStructure*>(dst);
        auto* srcAS = dynamic_cast<VulkanAccelerationStructure*>(src);
        auto* scratch = dynamic_cast<VulkanBuffer*>(scratchBuffer);
        const bool update = src != nullptr;
        if (update &&
            !HasFlag(desc.buildFlags,
                     RHIAccelerationStructureBuildFlags::AllowUpdate))
        {
            RVX_RHI_ERROR(
                "VulkanCommandContext: BLAS update requires AllowUpdate build flag");
            return;
        }
        VulkanAccelerationStructureBuildSignature buildSignature =
            MakeVulkanBottomLevelASBuildSignature(desc);
        const RHIAccelerationStructureBuildSizes sizes =
            m_device->GetBottomLevelASBuildSizes(desc);
        if (!ValidateVulkanASBuildResources(
                "BLAS build",
                RHIAccelerationStructureType::BottomLevel,
                sizes,
                update,
                dstAS,
                srcAS,
                scratch,
                scratchOffset,
                m_device->GetAccelerationStructureProperties()
                    .minAccelerationStructureScratchOffsetAlignment) ||
            !ValidateVulkanBLASGeometryInputAddresses(desc))
        {
            return;
        }
        if (update &&
            !ValidateAccelerationStructureUpdateSignature(
                "BLAS build", srcAS, buildSignature))
        {
            return;
        }

        const VkBuildAccelerationStructureFlagsKHR buildFlags =
            ToVkAccelerationStructureBuildFlags(desc.buildFlags, update);
        std::vector<VkAccelerationStructureGeometryKHR> geometries;
        std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
        VkAccelerationStructureBuildGeometryInfoKHR buildInfo = {
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        if (!BuildVulkanBottomLevelASBuildInputs(
                m_device->GetPhysicalDevice(),
                desc,
                buildFlags,
                geometries,
                ranges,
                buildInfo))
        {
            RVX_RHI_ERROR(
                "VulkanCommandContext: BLAS build could not translate validated geometry inputs");
            return;
        }
        buildInfo.mode = update
            ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR
            : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;

        buildInfo.dstAccelerationStructure = dstAS->GetAccelerationStructure();
        buildInfo.srcAccelerationStructure = update
            ? srcAS->GetAccelerationStructure()
            : VK_NULL_HANDLE;
        buildInfo.scratchData.deviceAddress =
            scratch->GetDeviceAddress() + scratchOffset;

        const VkAccelerationStructureBuildRangeInfoKHR* rangePointer =
            ranges.data();
        FlushBarriers();
        functions.cmdBuildAccelerationStructures(
            m_commandBuffer, 1, &buildInfo, &rangePointer);
        RecordPendingAccelerationStructureBuild(
            dstAS, std::move(buildSignature));
        InsertAccelerationStructureBarrier(dstAS);
    }

    void VulkanCommandContext::BuildTopLevelAccelerationStructure(
        RHIAccelerationStructure* dst,
        const RHITopLevelASDesc& desc,
        RHIBuffer* scratchBuffer,
        uint64 scratchOffset,
        RHIAccelerationStructure* src)
    {
        if (!m_device->GetCapabilities().supportsRaytracing)
        {
            RVX_RHI_WARN(
                "VulkanCommandContext: TLAS build skipped because ray tracing is unsupported");
            return;
        }

        if (m_queueType == RHICommandQueueType::Copy)
        {
            RVX_RHI_ERROR(
                "VulkanCommandContext: TLAS build cannot run on the copy queue");
            return;
        }

        if (!ValidateVulkanRayTracingCommandState(
                m_isRecording, m_inRenderPass, "TLAS build"))
        {
            return;
        }

        const RHIRayTracingValidationResult descValidation =
            ValidateRHITopLevelASDesc(desc);
        if (!descValidation)
        {
            RVX_RHI_ERROR(
                "VulkanCommandContext: invalid TLAS build desc: {}",
                descValidation.message);
            return;
        }

        if (!ValidateVulkanRayTracingBuildFlags(
                m_device->GetCapabilities(), desc.buildFlags, "TLAS build"))
        {
            return;
        }

        if (!desc.instanceBuffer)
        {
            RVX_RHI_ERROR(
                "VulkanCommandContext: TLAS build requires a GPU instance buffer");
            return;
        }

        const VulkanRayTracingFunctions& functions =
            m_device->GetRayTracingFunctions();
        if (!functions.IsComplete())
        {
            RVX_RHI_ERROR(
                "VulkanCommandContext: TLAS build requires complete Vulkan ray tracing entry points");
            return;
        }

        auto* dstAS = dynamic_cast<VulkanAccelerationStructure*>(dst);
        auto* srcAS = dynamic_cast<VulkanAccelerationStructure*>(src);
        auto* scratch = dynamic_cast<VulkanBuffer*>(scratchBuffer);
        auto* instances = dynamic_cast<VulkanBuffer*>(desc.instanceBuffer);
        const bool update = src != nullptr;
        if (update &&
            !HasFlag(desc.buildFlags,
                     RHIAccelerationStructureBuildFlags::AllowUpdate))
        {
            RVX_RHI_ERROR(
                "VulkanCommandContext: TLAS update requires AllowUpdate build flag");
            return;
        }
        VulkanAccelerationStructureBuildSignature buildSignature =
            MakeVulkanTopLevelASBuildSignature(desc);
        const RHIAccelerationStructureBuildSizes sizes =
            m_device->GetTopLevelASBuildSizes(desc);
        if (!ValidateVulkanASBuildResources(
                "TLAS build",
                RHIAccelerationStructureType::TopLevel,
                sizes,
                update,
                dstAS,
                srcAS,
                scratch,
                scratchOffset,
                m_device->GetAccelerationStructureProperties()
                    .minAccelerationStructureScratchOffsetAlignment) ||
            !ValidateVulkanBufferAddressRange(
                "TLAS build",
                instances,
                desc.instanceOffset,
                static_cast<uint64>(desc.instanceCount) *
                    sizeof(RHIRayTracingInstanceRecord),
                "instance buffer") ||
            !ValidateVulkanBufferEffectiveAddressAlignment(
                "TLAS build",
                instances,
                desc.instanceOffset,
                16,
                "instance buffer"))
        {
            return;
        }
        if (update &&
            !ValidateAccelerationStructureUpdateSignature(
                "TLAS build", srcAS, buildSignature))
        {
            return;
        }

        const VkBuildAccelerationStructureFlagsKHR buildFlags =
            ToVkAccelerationStructureBuildFlags(desc.buildFlags, update);
        VkAccelerationStructureGeometryKHR geometry = {
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        VkAccelerationStructureBuildRangeInfoKHR range = {};
        VkAccelerationStructureBuildGeometryInfoKHR buildInfo = {
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        if (!BuildVulkanTopLevelASBuildInputs(
                desc, buildFlags, geometry, range, buildInfo))
        {
            RVX_RHI_ERROR(
                "VulkanCommandContext: TLAS build could not translate validated instance input");
            return;
        }
        buildInfo.mode = update
            ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR
            : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;

        buildInfo.dstAccelerationStructure = dstAS->GetAccelerationStructure();
        buildInfo.srcAccelerationStructure = update
            ? srcAS->GetAccelerationStructure()
            : VK_NULL_HANDLE;
        buildInfo.scratchData.deviceAddress =
            scratch->GetDeviceAddress() + scratchOffset;

        const VkAccelerationStructureBuildRangeInfoKHR* rangePointer = &range;
        FlushBarriers();
        functions.cmdBuildAccelerationStructures(
            m_commandBuffer, 1, &buildInfo, &rangePointer);
        RecordPendingAccelerationStructureBuild(
            dstAS, std::move(buildSignature));
        InsertAccelerationStructureBarrier(dstAS);
    }

    void VulkanCommandContext::DispatchRays(const RHIDispatchRaysDesc& desc)
    {
        if (!m_device->GetCapabilities().supportsRaytracingPipeline)
        {
            RVX_RHI_WARN(
                "VulkanCommandContext: DispatchRays skipped because ray tracing pipelines are unsupported");
            return;
        }

        if (m_queueType == RHICommandQueueType::Copy)
        {
            RVX_RHI_ERROR(
                "VulkanCommandContext: DispatchRays cannot run on the copy queue");
            return;
        }

        if (!ValidateVulkanRayTracingCommandState(
                m_isRecording, m_inRenderPass, "DispatchRays"))
        {
            return;
        }

        if (!m_currentPipeline || !m_currentPipeline->IsRayTracing())
        {
            RVX_RHI_ERROR(
                "VulkanCommandContext: DispatchRays requires a bound ray tracing pipeline");
            return;
        }

        const RHIRayTracingValidationResult descValidation =
            ValidateRHIDispatchRaysDesc(desc, m_currentPipeline);
        if (!descValidation)
        {
            RVX_RHI_ERROR(
                "VulkanCommandContext: invalid DispatchRays desc: {}",
                descValidation.message);
            return;
        }

        const VulkanRayTracingFunctions& functions =
            m_device->GetRayTracingFunctions();
        if (!functions.IsComplete() || !functions.cmdTraceRays)
        {
            RVX_RHI_ERROR(
                "VulkanCommandContext: DispatchRays requires a complete Vulkan ray tracing entry point table");
            return;
        }

        auto* shaderTable = dynamic_cast<VulkanShaderTable*>(desc.shaderTable);
        if (!shaderTable || !shaderTable->IsValid())
        {
            RVX_RHI_ERROR(
                "VulkanCommandContext: DispatchRays requires a valid Vulkan shader table");
            return;
        }

        if (shaderTable->GetPipeline() != m_currentPipeline)
        {
            RVX_RHI_ERROR(
                "VulkanCommandContext: DispatchRays shader table does not match the bound ray tracing pipeline");
            return;
        }

        auto* pipelineLayout = m_currentPipeline->GetDescriptorPipelineLayout();
        if (!pipelineLayout)
        {
            RVX_RHI_ERROR(
                "VulkanCommandContext: DispatchRays requires a ray tracing pipeline layout");
            return;
        }

        const auto& expectedLayouts =
            pipelineLayout->GetDescriptorSetLayouts();
        for (uint32 setIndex = 0;
             setIndex < static_cast<uint32>(expectedLayouts.size());
             ++setIndex)
        {
            const auto* expectedLayout = expectedLayouts[setIndex];
            if (!expectedLayout || expectedLayout->GetEntries().empty())
            {
                continue;
            }

            const bool descriptorSetBound =
                setIndex < m_boundRayTracingDescriptorSetLayouts.size() &&
                m_boundRayTracingDescriptorSetLayouts[setIndex] == expectedLayout;
            if (!descriptorSetBound)
            {
                RVX_RHI_ERROR(
                    "VulkanCommandContext: DispatchRays requires descriptor set {} to be bound with the ray tracing pipeline layout",
                    setIndex);
                return;
            }
        }

        const VulkanRayTracingDispatchRegions regions =
            shaderTable->BuildDispatchRaysRegions(
                desc.width, desc.height, desc.depth);
        const VkPhysicalDeviceRayTracingPipelinePropertiesKHR& properties =
            m_device->GetRayTracingPipelineProperties();
        if (!ValidateVulkanShaderTableRegion(
                regions.rayGeneration,
                properties,
                true,
                "ray-generation") ||
            !ValidateVulkanShaderTableRegion(
                regions.miss, properties, false, "miss") ||
            !ValidateVulkanShaderTableRegion(
                regions.hitGroup, properties, false, "hit-group") ||
            !ValidateVulkanShaderTableRegion(
                regions.callable, properties, false, "callable"))
        {
            return;
        }

        FlushBarriers();
        functions.cmdTraceRays(
            m_commandBuffer,
            &regions.rayGeneration,
            &regions.miss,
            &regions.hitGroup,
            &regions.callable,
            desc.width,
            desc.height,
            desc.depth);
    }

    void VulkanCommandContext::WriteAccelerationStructureCompactedSizes(
        RHIAccelerationStructureCompactedSizeQuery* query,
        uint32 firstQuery,
        std::span<RHIAccelerationStructure* const> accelerationStructures)
    {
        if (!m_device->GetCapabilities().supportsAccelerationStructureCompaction ||
            m_queueType == RHICommandQueueType::Copy ||
            !ValidateVulkanRayTracingCommandState(
                m_isRecording, m_inRenderPass, "compacted-size query"))
        {
            return;
        }

        auto* vkQuery = dynamic_cast<
            VulkanAccelerationStructureCompactedSizeQuery*>(query);
        if (!vkQuery || vkQuery->GetQueueType() != m_queueType ||
            accelerationStructures.empty() ||
            accelerationStructures.size() > vkQuery->GetCount() ||
            firstQuery > vkQuery->GetCount() ||
            accelerationStructures.size() > vkQuery->GetCount() - firstQuery ||
            vkQuery->GetQueryPool() == VK_NULL_HANDLE)
        {
            RVX_RHI_ERROR(
                "VulkanCommandContext: compacted-size query arguments are invalid");
            return;
        }

        const VulkanRayTracingFunctions& functions =
            m_device->GetRayTracingFunctions();
        if (!functions.IsComplete() ||
            !functions.cmdWriteAccelerationStructuresProperties)
        {
            RVX_RHI_ERROR(
                "VulkanCommandContext: compacted-size query requires complete Vulkan ray tracing entry points");
            return;
        }

        std::vector<VkAccelerationStructureKHR> nativeStructures;
        nativeStructures.reserve(accelerationStructures.size());
        for (RHIAccelerationStructure* source : accelerationStructures)
        {
            auto* sourceAS = dynamic_cast<VulkanAccelerationStructure*>(source);
            const bool pendingInThisContext =
                sourceAS && HasPendingAccelerationStructureBuild(sourceAS, true);
            const bool submittedOnThisQueue =
                sourceAS && sourceAS->SubmittedBuildAllowsCompaction() &&
                sourceAS->SubmittedBuildWasOnQueue(m_queueType);
            if (!sourceAS || !sourceAS->IsValid() ||
                (!pendingInThisContext && !submittedOnThisQueue))
            {
                RVX_RHI_ERROR(
                    "VulkanCommandContext: compacted-size query requires an AllowCompaction source build recorded in this context or submitted previously on the same queue type");
                return;
            }
            nativeStructures.push_back(sourceAS->GetAccelerationStructure());
        }

        if (!vkQuery->Reserve(
                firstQuery,
                static_cast<uint32>(accelerationStructures.size())))
        {
            RVX_RHI_ERROR(
                "VulkanCommandContext: compacted-size query slots are not available");
            return;
        }

        vkCmdResetQueryPool(
            m_commandBuffer,
            vkQuery->GetQueryPool(),
            firstQuery,
            static_cast<uint32>(nativeStructures.size()));
        FlushBarriers();
        functions.cmdWriteAccelerationStructuresProperties(
            m_commandBuffer,
            static_cast<uint32>(nativeStructures.size()),
            nativeStructures.data(),
            VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR,
            vkQuery->GetQueryPool(),
            firstQuery);

        for (RHIAccelerationStructure* source : accelerationStructures)
        {
            PendingAccelerationStructureCompactionQuery pending;
            pending.query = RHIAccelerationStructureCompactedSizeQueryRef(query);
            pending.source = RHIAccelerationStructureRef(source);
            m_pendingAccelerationStructureCompactionQueries.push_back(
                std::move(pending));
        }
    }

    void VulkanCommandContext::ResolveAccelerationStructureCompactedSizes(
        RHIAccelerationStructureCompactedSizeQuery* query,
        uint32 firstQuery,
        uint32 queryCount,
        RHIBuffer* readbackBuffer,
        uint64 readbackOffset)
    {
        auto* vkQuery = dynamic_cast<
            VulkanAccelerationStructureCompactedSizeQuery*>(query);
        auto* vkReadback = dynamic_cast<VulkanBuffer*>(readbackBuffer);
        if (!m_device->GetCapabilities().supportsAccelerationStructureCompaction ||
            m_queueType == RHICommandQueueType::Copy ||
            !ValidateVulkanRayTracingCommandState(
                m_isRecording, m_inRenderPass, "compacted-size resolve") ||
            !vkQuery || vkQuery->GetQueueType() != m_queueType ||
            vkQuery->GetQueryPool() == VK_NULL_HANDLE ||
            !vkReadback || vkReadback->GetBuffer() == VK_NULL_HANDLE ||
            vkReadback->GetMemoryType() != RHIMemoryType::Readback ||
            (readbackOffset % alignof(uint64)) != 0 || queryCount == 0 ||
            !vkQuery->CanResolve(firstQuery, queryCount))
        {
            RVX_RHI_ERROR(
                "VulkanCommandContext: compacted-size resolve arguments are invalid");
            return;
        }

        bool queryOwnedByContext = false;
        for (const PendingAccelerationStructureCompactionQuery& pending :
             m_pendingAccelerationStructureCompactionQueries)
        {
            if (pending.query.Get() == query)
            {
                queryOwnedByContext = true;
                break;
            }
        }
        const uint64 bytes = static_cast<uint64>(queryCount) * sizeof(uint64);
        if (!queryOwnedByContext ||
            !IsRHIRayTracingBufferRangeValid(
                *vkReadback, readbackOffset, bytes))
        {
            RVX_RHI_ERROR(
                "VulkanCommandContext: compacted-size resolve requires context-owned query slots and a sufficiently large readback range");
            return;
        }

        FlushBarriers();
        // The compacted-size write has no independent availability command.
        // WAIT_BIT makes it part of this copy's synchronization scope while
        // remaining an entirely GPU-side dependency (no CPU fence wait).
        vkCmdCopyQueryPoolResults(
            m_commandBuffer,
            vkQuery->GetQueryPool(),
            firstQuery,
            queryCount,
            vkReadback->GetBuffer(),
            readbackOffset,
            sizeof(uint64),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);

        PendingAccelerationStructureCompactionQuery pending;
        pending.query = RHIAccelerationStructureCompactedSizeQueryRef(query);
        pending.readback = RHIBufferRef(readbackBuffer);
        m_pendingAccelerationStructureCompactionQueries.push_back(
            std::move(pending));
    }

    void VulkanCommandContext::CopyAccelerationStructureCompact(
        RHIAccelerationStructure* dst,
        RHIAccelerationStructure* src,
        uint64 expectedCompactedSize)
    {
        if (!m_device->GetCapabilities().supportsAccelerationStructureCompaction ||
            m_queueType == RHICommandQueueType::Copy ||
            !ValidateVulkanRayTracingCommandState(
                m_isRecording, m_inRenderPass, "acceleration structure compact copy"))
        {
            return;
        }

        auto* dstAS = dynamic_cast<VulkanAccelerationStructure*>(dst);
        auto* srcAS = dynamic_cast<VulkanAccelerationStructure*>(src);
        const bool pendingInThisContext =
            srcAS && HasPendingAccelerationStructureBuild(srcAS, true);
        const bool submittedOnThisQueue =
            srcAS && srcAS->SubmittedBuildAllowsCompaction() &&
            srcAS->SubmittedBuildWasOnQueue(m_queueType);
        if (!dstAS || !srcAS || dstAS == srcAS ||
            !dstAS->IsValid() || !srcAS->IsValid() ||
            dstAS->GetType() != srcAS->GetType() ||
            expectedCompactedSize == 0 ||
            expectedCompactedSize > srcAS->GetSize() ||
            dstAS->GetSize() < expectedCompactedSize ||
            !IsRHIRayTracingAccelerationStructureSizeAligned(dstAS->GetSize()) ||
            !IsRHIRayTracingAccelerationStructureSizeAligned(srcAS->GetSize()) ||
            (!pendingInThisContext && !submittedOnThisQueue))
        {
            RVX_RHI_ERROR(
                "VulkanCommandContext: compact copy requires aligned distinct same-type acceleration structures, an in-range queried size, and an AllowCompaction source build from this context or the same submitted queue type");
            return;
        }

        const VulkanRayTracingFunctions& functions =
            m_device->GetRayTracingFunctions();
        if (!functions.IsComplete() || !functions.cmdCopyAccelerationStructure)
        {
            RVX_RHI_ERROR(
                "VulkanCommandContext: compact copy requires complete Vulkan ray tracing entry points");
            return;
        }

        VkCopyAccelerationStructureInfoKHR copyInfo = {
            VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR};
        copyInfo.src = srcAS->GetAccelerationStructure();
        copyInfo.dst = dstAS->GetAccelerationStructure();
        copyInfo.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;

        FlushBarriers();
        functions.cmdCopyAccelerationStructure(m_commandBuffer, &copyInfo);
        InsertAccelerationStructureBarrier(dstAS);
    }

    void VulkanCommandContext::InsertAccelerationStructureBarrier(
        RHIAccelerationStructure* accelerationStructure)
    {
        auto* vkAS = dynamic_cast<VulkanAccelerationStructure*>(
            accelerationStructure);
        if (!vkAS ||
            vkAS->GetAccelerationStructure() == VK_NULL_HANDLE ||
            !m_device->GetCapabilities().supportsRaytracing)
        {
            return;
        }

        FlushBarriers();
        VkMemoryBarrier2 memoryBarrier = {
            VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        // Without VK_KHR_ray_tracing_maintenance1, AS copy commands execute in
        // the acceleration-structure build stage.  Advertising the dedicated
        // COPY stage bit would itself be invalid on the production base-KHR
        // path, even though vkCmdCopyAccelerationStructureKHR is available.
        memoryBarrier.srcStageMask =
            VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        memoryBarrier.srcAccessMask =
            VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        memoryBarrier.dstStageMask =
            VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
            VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
        memoryBarrier.dstAccessMask =
            VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR |
            VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;

        VkDependencyInfo dependencyInfo = {
            VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependencyInfo.memoryBarrierCount = 1;
        dependencyInfo.pMemoryBarriers = &memoryBarrier;
        vkCmdPipelineBarrier2(m_commandBuffer, &dependencyInfo);
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
        if (src == nullptr || dst == nullptr)
        {
            RVX_RHI_ERROR("Vulkan CopyTexture rejected a null texture");
            return;
        }

        const RHIFormat sourceFormat = src->GetFormat();
        const RHIFormat destinationFormat = dst->GetFormat();
        const VulkanTextureCopyContract copyContract =
            ResolveVulkanTextureCopyContract(sourceFormat, destinationFormat);
        if (!copyContract.IsValid())
        {
            RVX_RHI_ERROR(
                "Vulkan CopyTexture rejected source format {} and destination "
                "format {} (contract status {})",
                static_cast<uint32>(sourceFormat),
                static_cast<uint32>(destinationFormat),
                static_cast<uint32>(copyContract.status));
            return;
        }

        FlushBarriers();

        auto* vkSrc = static_cast<VulkanTexture*>(src);
        auto* vkDst = static_cast<VulkanTexture*>(dst);
        const auto srcSubresource = DecodeTextureSubresource(desc.srcSubresource, src->GetMipLevels());
        const auto dstSubresource = DecodeTextureSubresource(desc.dstSubresource, dst->GetMipLevels());

        VkImageCopy copyRegion = {};
        copyRegion.srcSubresource.aspectMask = copyContract.aspectMask;
        copyRegion.srcSubresource.mipLevel = srcSubresource.mipLevel;
        copyRegion.srcSubresource.baseArrayLayer = srcSubresource.physicalLayer;
        copyRegion.srcSubresource.layerCount = 1;
        copyRegion.srcOffset = {static_cast<int32>(desc.srcX), static_cast<int32>(desc.srcY), static_cast<int32>(desc.srcZ)};
        copyRegion.dstSubresource.aspectMask = copyContract.aspectMask;
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
        copyRegion.imageSubresource.aspectMask = format == RHIFormat::D32_FLOAT
            ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
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
        copyRegion.imageSubresource.aspectMask = format == RHIFormat::D32_FLOAT
            ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
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
                // Normal rendering first consumes the image as a color target.
                // Abort retirement may contain only an image-layout discard,
                // so its acquire wait must cover that all-command transition.
                waitStages.push_back(
                    swapChain->IsAbortAcquireRetirementPending()
                        ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT
                        : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
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
            vkContext->DiscardPendingAccelerationStructureCompactionQueries();
            vkContext->DiscardPendingAccelerationStructureBuilds();
            return 0;
        }
        vkContext->CommitPendingAccelerationStructureBuilds();
        vkContext->CommitPendingAccelerationStructureCompactionQueries();
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
        std::vector<VulkanCommandContext*> graphicsContexts;
        std::vector<VulkanCommandContext*> computeContexts;
        std::vector<VulkanCommandContext*> copyContexts;

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
                    graphicsContexts.push_back(vkContext);
                    break;
                case RHICommandQueueType::Compute:
                    computeCmdBuffers.push_back(cmdBuffer);
                    computeContexts.push_back(vkContext);
                    break;
                case RHICommandQueueType::Copy:
                    copyCmdBuffers.push_back(cmdBuffer);
                    copyContexts.push_back(vkContext);
                    break;
            }
        }

        const auto discardPendingAccelerationStructureState = [&]()
        {
            for (RHICommandContext* context : contexts)
            {
                auto* vkContext = dynamic_cast<VulkanCommandContext*>(context);
                if (!vkContext)
                {
                    continue;
                }
                vkContext->DiscardPendingAccelerationStructureCompactionQueries();
                vkContext->DiscardPendingAccelerationStructureBuilds();
            }
        };

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
            {
                discardPendingAccelerationStructureState();
                return 0;
            }
        }
        if (needCopyToGraphicsSync)
        {
            if (!createSemaphore(copyToGraphicsSemaphore))
            {
                retireCrossQueueSemaphores();
                discardPendingAccelerationStructureState();
                return 0;
            }
        }
        if (needComputeToGraphicsSync)
        {
            if (!createSemaphore(computeToGraphicsSemaphore))
            {
                retireCrossQueueSemaphores();
                discardPendingAccelerationStructureState();
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
                discardPendingAccelerationStructureState();
                return 0;
            }
            submittedAnyBatch = true;
            lastSubmittedQueue = device->GetTransferQueue();
            for (VulkanCommandContext* context : copyContexts)
            {
                context->CommitPendingAccelerationStructureBuilds();
                context->CommitPendingAccelerationStructureCompactionQueries();
            }
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
                discardPendingAccelerationStructureState();
                return 0;
            }
            submittedAnyBatch = true;
            lastSubmittedQueue = device->GetComputeQueue();
            for (VulkanCommandContext* context : computeContexts)
            {
                context->CommitPendingAccelerationStructureBuilds();
                context->CommitPendingAccelerationStructureCompactionQueries();
            }
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
                discardPendingAccelerationStructureState();
                return 0;
            }
            if (frameFence != VK_NULL_HANDLE)
            {
                device->MarkArmedFrameFenceSubmitted(frameFence);
            }
            submittedAnyBatch = true;
            lastSubmittedQueue = device->GetGraphicsQueue();
            for (VulkanCommandContext* context : graphicsContexts)
            {
                context->CommitPendingAccelerationStructureBuilds();
                context->CommitPendingAccelerationStructureCompactionQueries();
            }
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
                discardPendingAccelerationStructureState();
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

    uint64 SubmitVulkanQueuePlan(VulkanDevice* device,
                                 const RHIQueueSubmissionPlan& plan,
                                 RHIFence* terminalFence)
    {
        if (!device)
        {
            return 0;
        }

        const RHIQueueSubmissionPlanValidationResult validation =
            ValidateRHIQueueSubmissionPlan(plan);
        if (!validation)
        {
            RVX_RHI_ERROR("SubmitVulkanQueuePlan rejected invalid plan: {}",
                          validation.message);
            return 0;
        }

        std::lock_guard<std::mutex> lock(device->GetGraphicsQueueMutex());

        struct BatchState
        {
            VkQueue queue = VK_NULL_HANDLE;
            std::vector<VkCommandBufferSubmitInfo> commandBuffers;
            std::vector<VulkanCommandContext*> contexts;
            VkSemaphore completionSemaphore = VK_NULL_HANDLE;
        };

        std::vector<BatchState> batches(plan.batches.size());
        for (uint32 batchIndex = 0;
             batchIndex < static_cast<uint32>(plan.batches.size());
             ++batchIndex)
        {
            const RHIQueueSubmissionBatch& source = plan.batches[batchIndex];
            BatchState& batch = batches[batchIndex];
            batch.queue = GetQueueForType(device, source.queueType);
            if (batch.queue == VK_NULL_HANDLE)
            {
                RVX_RHI_ERROR("SubmitVulkanQueuePlan encountered an unavailable queue");
                return 0;
            }
            batch.commandBuffers.reserve(source.contexts.size());
            batch.contexts.reserve(source.contexts.size());
            for (RHICommandContext* context : source.contexts)
            {
                auto* vkContext = static_cast<VulkanCommandContext*>(context);
                VkCommandBufferSubmitInfo commandInfo = {
                    VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
                commandInfo.commandBuffer = vkContext->GetCommandBuffer();
                commandInfo.deviceMask = 0;
                batch.commandBuffers.push_back(commandInfo);
                batch.contexts.push_back(vkContext);
            }
        }

        std::vector<uint8> needsCrossQueueSignal(plan.batches.size(), 0);
        for (uint32 targetIndex = 0;
             targetIndex < static_cast<uint32>(plan.batches.size());
             ++targetIndex)
        {
            for (uint32 sourceIndex :
                 plan.batches[targetIndex].prerequisiteBatchIndices)
            {
                if (batches[sourceIndex].queue != batches[targetIndex].queue)
                {
                    needsCrossQueueSignal[sourceIndex] = 1;
                }
            }
        }

        const auto destroySemaphores = [device, &batches]()
        {
            for (BatchState& batch : batches)
            {
                if (batch.completionSemaphore != VK_NULL_HANDLE)
                {
                    vkDestroySemaphore(device->GetDevice(),
                                       batch.completionSemaphore,
                                       nullptr);
                    batch.completionSemaphore = VK_NULL_HANDLE;
                }
            }
        };

        const auto discardPendingAccelerationStructureState = [&batches]()
        {
            for (BatchState& batch : batches)
            {
                for (VulkanCommandContext* context : batch.contexts)
                {
                    context->DiscardPendingAccelerationStructureCompactionQueries();
                    context->DiscardPendingAccelerationStructureBuilds();
                }
            }
        };

        for (uint32 batchIndex = 0;
             batchIndex < static_cast<uint32>(batches.size());
             ++batchIndex)
        {
            if (needsCrossQueueSignal[batchIndex] == 0)
            {
                continue;
            }

            VkSemaphoreTypeCreateInfo timelineInfo = {
                VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
            timelineInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
            timelineInfo.initialValue = 0;

            VkSemaphoreCreateInfo semaphoreInfo = {
                VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            semaphoreInfo.pNext = &timelineInfo;
            const VkResult createResult = vkCreateSemaphore(
                device->GetDevice(),
                &semaphoreInfo,
                nullptr,
                &batches[batchIndex].completionSemaphore);
            if (createResult != VK_SUCCESS)
            {
                device->ReportRuntimeFailure(
                    createResult,
                    RHIDeviceFaultOperation::CommandSubmission,
                    "Vulkan queue-plan timeline semaphore creation failed");
                destroySemaphores();
                discardPendingAccelerationStructureState();
                return 0;
            }
        }

        VulkanSwapChain* swapChain = device->GetPrimarySwapChain();
        const bool hasAcquiredImage =
            swapChain && swapChain->HasAcquiredImage();
        uint32 firstGraphicsBatchIndex = RVX_INVALID_INDEX;
        for (uint32 batchIndex = 0;
             batchIndex < static_cast<uint32>(plan.batches.size());
             ++batchIndex)
        {
            if (plan.batches[batchIndex].queueType ==
                RHICommandQueueType::Graphics)
            {
                firstGraphicsBatchIndex = batchIndex;
                break;
            }
        }

        auto* vkTerminalFence = terminalFence
            ? static_cast<VulkanFence*>(terminalFence)
            : nullptr;
        const uint64 terminalFenceValue = vkTerminalFence
            ? vkTerminalFence->AllocateSignalValue()
            : 0;
        bool submittedAnyBatch = false;

        for (uint32 batchIndex = 0;
             batchIndex < static_cast<uint32>(plan.batches.size());
             ++batchIndex)
        {
            const RHIQueueSubmissionBatch& source = plan.batches[batchIndex];
            BatchState& batch = batches[batchIndex];
            std::vector<VkSemaphoreSubmitInfo> waits;
            std::vector<VkSemaphoreSubmitInfo> signals;

            for (uint32 prerequisiteIndex : source.prerequisiteBatchIndices)
            {
                const BatchState& prerequisite = batches[prerequisiteIndex];
                if (prerequisite.queue == batch.queue)
                {
                    continue;
                }
                if (prerequisite.completionSemaphore == VK_NULL_HANDLE)
                {
                    RVX_RHI_ERROR(
                        "SubmitVulkanQueuePlan found a cross-queue dependency without a source signal");
                    if (submittedAnyBatch &&
                        device->QueryRuntimeStatus() == RHIDeviceRuntimeStatus::Ready)
                    {
                        static_cast<void>(vkDeviceWaitIdle(device->GetDevice()));
                    }
                    destroySemaphores();
                    discardPendingAccelerationStructureState();
                    return 0;
                }

                VkSemaphoreSubmitInfo waitInfo = {
                    VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
                waitInfo.semaphore = prerequisite.completionSemaphore;
                waitInfo.value = 1;
                waitInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                waitInfo.deviceIndex = 0;
                waits.push_back(waitInfo);
            }

            if (hasAcquiredImage && batchIndex == firstGraphicsBatchIndex)
            {
                VkSemaphoreSubmitInfo waitInfo = {
                    VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
                waitInfo.semaphore = device->GetImageAvailableSemaphore();
                waitInfo.value = 0;
                waitInfo.stageMask =
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
                waitInfo.deviceIndex = 0;
                waits.push_back(waitInfo);
            }

            if (batch.completionSemaphore != VK_NULL_HANDLE)
            {
                VkSemaphoreSubmitInfo signalInfo = {
                    VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
                signalInfo.semaphore = batch.completionSemaphore;
                signalInfo.value = 1;
                signalInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                signalInfo.deviceIndex = 0;
                signals.push_back(signalInfo);
            }

            const bool isTerminal =
                batchIndex == plan.terminalGraphicsBatchIndex;
            if (isTerminal && hasAcquiredImage)
            {
                VkSemaphoreSubmitInfo signalInfo = {
                    VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
                signalInfo.semaphore = device->GetRenderFinishedSemaphore();
                signalInfo.value = 0;
                signalInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                signalInfo.deviceIndex = 0;
                signals.push_back(signalInfo);
            }
            if (isTerminal && vkTerminalFence)
            {
                VkSemaphoreSubmitInfo signalInfo = {
                    VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
                signalInfo.semaphore = vkTerminalFence->GetSemaphore();
                signalInfo.value = terminalFenceValue;
                signalInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                signalInfo.deviceIndex = 0;
                signals.push_back(signalInfo);
            }

            VkSubmitInfo2 submitInfo = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
            submitInfo.waitSemaphoreInfoCount = static_cast<uint32>(waits.size());
            submitInfo.pWaitSemaphoreInfos = waits.empty() ? nullptr : waits.data();
            submitInfo.commandBufferInfoCount =
                static_cast<uint32>(batch.commandBuffers.size());
            submitInfo.pCommandBufferInfos = batch.commandBuffers.data();
            submitInfo.signalSemaphoreInfoCount =
                static_cast<uint32>(signals.size());
            submitInfo.pSignalSemaphoreInfos =
                signals.empty() ? nullptr : signals.data();

            const VkFence frameFence = isTerminal
                ? device->GetArmedFrameFenceForSubmission()
                : VK_NULL_HANDLE;
            const VkResult submitResult = vkQueueSubmit2(
                batch.queue, 1, &submitInfo, frameFence);
            if (submitResult != VK_SUCCESS)
            {
                device->ReportRuntimeFailure(
                    submitResult,
                    RHIDeviceFaultOperation::CommandSubmission,
                    "Vulkan queue-plan batch submission failed");
                if (submittedAnyBatch &&
                    device->QueryRuntimeStatus() == RHIDeviceRuntimeStatus::Ready)
                {
                    static_cast<void>(vkDeviceWaitIdle(device->GetDevice()));
                }
                destroySemaphores();
                discardPendingAccelerationStructureState();
                return 0;
            }
            submittedAnyBatch = true;
            for (VulkanCommandContext* context : batch.contexts)
            {
                context->CommitPendingAccelerationStructureBuilds();
                context->CommitPendingAccelerationStructureCompactionQueries();
            }
            if (frameFence != VK_NULL_HANDLE)
            {
                device->MarkArmedFrameFenceSubmitted(frameFence);
            }
        }

        std::vector<VkSemaphore> semaphoresToDestroy;
        for (BatchState& batch : batches)
        {
            if (batch.completionSemaphore != VK_NULL_HANDLE)
            {
                semaphoresToDestroy.push_back(batch.completionSemaphore);
                batch.completionSemaphore = VK_NULL_HANDLE;
            }
        }
        device->EnqueueDeferredSemaphoreDestroy(
            std::move(semaphoresToDestroy),
            batches[plan.terminalGraphicsBatchIndex].queue);
        return terminalFenceValue;
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
