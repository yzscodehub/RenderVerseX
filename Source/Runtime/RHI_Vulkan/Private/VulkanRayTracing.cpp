#include "VulkanRayTracing.h"

#include "VulkanResources.h"

#include "Core/Log.h"

#include <limits>

namespace RVX
{
    namespace
    {
        VkDeviceAddress GetVulkanRayTracingBufferAddress(
            RHIBuffer* buffer,
            uint64 offset)
        {
            auto* vulkanBuffer = dynamic_cast<VulkanBuffer*>(buffer);
            if (!vulkanBuffer || vulkanBuffer->GetDeviceAddress() == 0 ||
                offset > std::numeric_limits<VkDeviceAddress>::max() -
                             vulkanBuffer->GetDeviceAddress())
            {
                return 0;
            }

            return vulkanBuffer->GetDeviceAddress() + offset;
        }

        uint32 GetVulkanRayTracingPrimitiveCount(
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
    }

    VkBuildAccelerationStructureFlagsKHR ToVkAccelerationStructureBuildFlags(
        RHIAccelerationStructureBuildFlags flags,
        bool performUpdate)
    {
        VkBuildAccelerationStructureFlagsKHR result = 0;
        if (HasFlag(flags, RHIAccelerationStructureBuildFlags::AllowUpdate))
        {
            result |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
        }
        if (HasFlag(flags, RHIAccelerationStructureBuildFlags::AllowCompaction))
        {
            result |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
        }
        if (HasFlag(flags, RHIAccelerationStructureBuildFlags::PreferFastTrace))
        {
            result |= VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        }
        if (HasFlag(flags, RHIAccelerationStructureBuildFlags::PreferFastBuild))
        {
            result |= VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
        }
        if (HasFlag(flags, RHIAccelerationStructureBuildFlags::MinimizeMemory))
        {
            result |= VK_BUILD_ACCELERATION_STRUCTURE_LOW_MEMORY_BIT_KHR;
        }
        // Vulkan expresses build versus update through
        // VkAccelerationStructureBuildGeometryInfoKHR::mode, not a build flag.
        (void)performUpdate;
        return result;
    }

    VkGeometryFlagsKHR ToVkRayTracingGeometryFlags(
        RHIRayTracingGeometryFlags flags)
    {
        VkGeometryFlagsKHR result = 0;
        if (HasFlag(flags, RHIRayTracingGeometryFlags::Opaque))
        {
            result |= VK_GEOMETRY_OPAQUE_BIT_KHR;
        }
        if (HasFlag(flags, RHIRayTracingGeometryFlags::NoDuplicateAnyHitInvocation))
        {
            result |= VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR;
        }
        return result;
    }

    bool ValidateVulkanRayTracingBuildFlags(
        const RHICapabilities& capabilities,
        RHIAccelerationStructureBuildFlags flags,
        const char* operation)
    {
        if (HasFlag(flags, RHIAccelerationStructureBuildFlags::AllowUpdate) &&
            !capabilities.supportsAccelerationStructureUpdate)
        {
            RVX_RHI_ERROR(
                "Vulkan {} rejected: AllowUpdate requires acceleration structure update support",
                operation ? operation : "ray tracing operation");
            return false;
        }

        if (HasFlag(flags, RHIAccelerationStructureBuildFlags::AllowCompaction) &&
            !capabilities.supportsAccelerationStructureCompaction)
        {
            RVX_RHI_ERROR(
                "Vulkan {} rejected: AllowCompaction requires acceleration structure compaction support",
                operation ? operation : "ray tracing operation");
            return false;
        }

        return true;
    }

    bool ValidateVulkanBottomLevelASDeviceLimits(
        const VkPhysicalDeviceAccelerationStructurePropertiesKHR& properties,
        const RHIBottomLevelASDesc& desc,
        const char* operation)
    {
        const char* const operationName = operation
            ? operation
            : "BLAS operation";
        const uint64 geometryCount = static_cast<uint64>(desc.geometries.size());
        if (geometryCount > properties.maxGeometryCount)
        {
            RVX_RHI_ERROR(
                "Vulkan {} rejected: {} geometries exceed the device limit {}",
                operationName,
                geometryCount,
                properties.maxGeometryCount);
            return false;
        }

        uint64 totalPrimitiveCount = 0;
        for (const RHIRayTracingGeometryDesc& geometry : desc.geometries)
        {
            const uint64 primitiveCount =
                GetVulkanRayTracingPrimitiveCount(geometry);
            if (primitiveCount >
                properties.maxPrimitiveCount - totalPrimitiveCount)
            {
                RVX_RHI_ERROR(
                    "Vulkan {} rejected: {} existing BLAS primitives plus {} additional primitives exceed the device limit {}",
                    operationName,
                    totalPrimitiveCount,
                    primitiveCount,
                    properties.maxPrimitiveCount);
                return false;
            }
            totalPrimitiveCount += primitiveCount;
        }

        return true;
    }

    bool ValidateVulkanTopLevelASDeviceLimits(
        const VkPhysicalDeviceAccelerationStructurePropertiesKHR& properties,
        const RHITopLevelASDesc& desc,
        const char* operation)
    {
        const uint64 instanceCount = static_cast<uint64>(desc.GetInstanceCount());
        if (instanceCount > properties.maxInstanceCount)
        {
            RVX_RHI_ERROR(
                "Vulkan {} rejected: {} instances exceed the device limit {}",
                operation ? operation : "TLAS operation",
                instanceCount,
                properties.maxInstanceCount);
            return false;
        }

        return true;
    }

    bool BuildVulkanBottomLevelASBuildInputs(
        VkPhysicalDevice physicalDevice,
        const RHIBottomLevelASDesc& desc,
        VkBuildAccelerationStructureFlagsKHR buildFlags,
        std::vector<VkAccelerationStructureGeometryKHR>& outGeometries,
        std::vector<VkAccelerationStructureBuildRangeInfoKHR>& outRanges,
        VkAccelerationStructureBuildGeometryInfoKHR& outBuildInfo)
    {
        outGeometries.clear();
        outRanges.clear();
        outGeometries.reserve(desc.geometries.size());
        outRanges.reserve(desc.geometries.size());

        for (const RHIRayTracingGeometryDesc& geometry : desc.geometries)
        {
            VkAccelerationStructureGeometryKHR nativeGeometry = {
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
            nativeGeometry.flags = ToVkRayTracingGeometryFlags(geometry.flags);

            VkAccelerationStructureBuildRangeInfoKHR nativeRange = {};
            nativeRange.primitiveCount = GetVulkanRayTracingPrimitiveCount(geometry);
            if (nativeRange.primitiveCount == 0)
            {
                RVX_RHI_ERROR(
                    "Vulkan BLAS geometry has no complete primitives after RHI validation");
                return false;
            }

            if (geometry.type == RHIRayTracingGeometryType::Triangles)
            {
                const RHIRayTracingTrianglesDesc& triangles = geometry.triangles;
                if (physicalDevice == VK_NULL_HANDLE ||
                    triangles.vertexFormat != RHIFormat::RGB32_FLOAT ||
                    triangles.vertexStride % sizeof(float) != 0)
                {
                    RVX_RHI_ERROR(
                        "Vulkan BLAS triangle geometry currently requires an RGB32_FLOAT vertex format and float-aligned stride");
                    return false;
                }

                nativeGeometry.geometryType =
                    VK_GEOMETRY_TYPE_TRIANGLES_KHR;
                nativeGeometry.geometry.triangles = {
                    VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
                nativeGeometry.geometry.triangles.vertexFormat =
                    ToVkFormat(triangles.vertexFormat);
                if (nativeGeometry.geometry.triangles.vertexFormat ==
                    VK_FORMAT_UNDEFINED)
                {
                    RVX_RHI_ERROR(
                        "Vulkan BLAS geometry uses an RHI vertex format without a Vulkan mapping");
                    return false;
                }

                VkFormatProperties formatProperties{};
                vkGetPhysicalDeviceFormatProperties(
                    physicalDevice,
                    nativeGeometry.geometry.triangles.vertexFormat,
                    &formatProperties);
                if ((formatProperties.bufferFeatures &
                     VK_FORMAT_FEATURE_ACCELERATION_STRUCTURE_VERTEX_BUFFER_BIT_KHR) ==
                    0)
                {
                    RVX_RHI_ERROR(
                        "Vulkan BLAS triangle vertex format lacks acceleration-structure vertex-buffer support on the selected device");
                    return false;
                }
                nativeGeometry.geometry.triangles.vertexData.deviceAddress =
                    GetVulkanRayTracingBufferAddress(
                        triangles.vertexBuffer, triangles.vertexOffset);
                nativeGeometry.geometry.triangles.vertexStride =
                    triangles.vertexStride;
                nativeGeometry.geometry.triangles.maxVertex =
                    triangles.vertexCount - 1u;
                nativeGeometry.geometry.triangles.indexType =
                    triangles.indexBuffer
                    ? (triangles.indexFormat == RHIFormat::R16_UINT
                           ? VK_INDEX_TYPE_UINT16
                           : VK_INDEX_TYPE_UINT32)
                    : VK_INDEX_TYPE_NONE_KHR;
                nativeGeometry.geometry.triangles.indexData.deviceAddress =
                    triangles.indexBuffer
                    ? GetVulkanRayTracingBufferAddress(
                          triangles.indexBuffer, triangles.indexOffset)
                    : 0;
                nativeGeometry.geometry.triangles.transformData.deviceAddress =
                    triangles.transformBuffer
                    ? GetVulkanRayTracingBufferAddress(
                          triangles.transformBuffer, triangles.transformOffset)
                    : 0;
            }
            else
            {
                const RHIRayTracingAABBDesc& aabbs = geometry.aabbs;
                nativeGeometry.geometryType = VK_GEOMETRY_TYPE_AABBS_KHR;
                nativeGeometry.geometry.aabbs = {
                    VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR};
                nativeGeometry.geometry.aabbs.data.deviceAddress =
                    GetVulkanRayTracingBufferAddress(aabbs.aabbBuffer,
                                                      aabbs.offset);
                nativeGeometry.geometry.aabbs.stride = aabbs.stride;
            }

            if ((geometry.type == RHIRayTracingGeometryType::Triangles &&
                 (nativeGeometry.geometry.triangles.vertexData.deviceAddress == 0 ||
                  (geometry.triangles.indexBuffer &&
                   nativeGeometry.geometry.triangles.indexData.deviceAddress == 0) ||
                  (geometry.triangles.transformBuffer &&
                   nativeGeometry.geometry.triangles.transformData.deviceAddress == 0))) ||
                (geometry.type == RHIRayTracingGeometryType::AABBs &&
                 nativeGeometry.geometry.aabbs.data.deviceAddress == 0))
            {
                RVX_RHI_ERROR(
                    "Vulkan BLAS geometry requires backend-owned device-address buffers");
                return false;
            }

            outGeometries.push_back(nativeGeometry);
            outRanges.push_back(nativeRange);
        }

        outBuildInfo = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        outBuildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        outBuildInfo.flags = buildFlags;
        outBuildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        outBuildInfo.geometryCount = static_cast<uint32>(outGeometries.size());
        outBuildInfo.pGeometries = outGeometries.data();
        return true;
    }

    bool BuildVulkanTopLevelASBuildInputs(
        const RHITopLevelASDesc& desc,
        VkBuildAccelerationStructureFlagsKHR buildFlags,
        VkAccelerationStructureGeometryKHR& outGeometry,
        VkAccelerationStructureBuildRangeInfoKHR& outRange,
        VkAccelerationStructureBuildGeometryInfoKHR& outBuildInfo)
    {
        const uint32 instanceCount = desc.GetInstanceCount();
        VkDeviceAddress instanceAddress = 0;
        if (desc.instanceBuffer)
        {
            instanceAddress = GetVulkanRayTracingBufferAddress(
                desc.instanceBuffer, desc.instanceOffset);
            if (instanceAddress == 0)
            {
                RVX_RHI_ERROR(
                    "Vulkan TLAS requires a backend-owned device-address instance buffer");
                return false;
            }
        }

        outGeometry = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        outGeometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        outGeometry.geometry.instances = {
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
        outGeometry.geometry.instances.arrayOfPointers = VK_FALSE;
        outGeometry.geometry.instances.data.deviceAddress = instanceAddress;

        outRange = {};
        outRange.primitiveCount = instanceCount;

        outBuildInfo = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        outBuildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        outBuildInfo.flags = buildFlags;
        outBuildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        outBuildInfo.geometryCount = 1;
        outBuildInfo.pGeometries = &outGeometry;
        return instanceCount > 0;
    }

    uint64 AlignVulkanAccelerationStructureSize(uint64 size)
    {
        constexpr uint64 alignment =
            RVX_RAY_TRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT;
        if (size == 0 || size > std::numeric_limits<uint64>::max() -
                                 (alignment - 1u))
        {
            return 0;
        }
        return (size + alignment - 1u) & ~(alignment - 1u);
    }
}
