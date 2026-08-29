#pragma once

#include "VulkanCommon.h"
#include "RHI/RHICapabilities.h"
#include "RHI/RHIRayTracing.h"

#include <vector>

namespace RVX
{
    class VulkanBuffer;

    /**
     * @brief Device entry points required by the Vulkan ray-tracing RHI path.
     *
     * The backend loads these after creating a logical device with the required
     * extensions.  Keeping the complete set together makes capability reporting
     * fail closed when a loader exposes only a partial extension implementation.
     */
    struct VulkanRayTracingFunctions
    {
        PFN_vkCreateAccelerationStructureKHR createAccelerationStructure = nullptr;
        PFN_vkDestroyAccelerationStructureKHR destroyAccelerationStructure = nullptr;
        PFN_vkGetAccelerationStructureBuildSizesKHR getAccelerationStructureBuildSizes = nullptr;
        PFN_vkGetAccelerationStructureDeviceAddressKHR getAccelerationStructureDeviceAddress = nullptr;
        PFN_vkCmdBuildAccelerationStructuresKHR cmdBuildAccelerationStructures = nullptr;
        PFN_vkCmdWriteAccelerationStructuresPropertiesKHR
            cmdWriteAccelerationStructuresProperties = nullptr;
        PFN_vkCmdCopyAccelerationStructureKHR cmdCopyAccelerationStructure = nullptr;
        PFN_vkCreateRayTracingPipelinesKHR createRayTracingPipelines = nullptr;
        PFN_vkGetRayTracingShaderGroupHandlesKHR getRayTracingShaderGroupHandles = nullptr;
        PFN_vkCmdTraceRaysKHR cmdTraceRays = nullptr;

        bool IsComplete() const
        {
            return createAccelerationStructure != nullptr &&
                   destroyAccelerationStructure != nullptr &&
                   getAccelerationStructureBuildSizes != nullptr &&
                   getAccelerationStructureDeviceAddress != nullptr &&
                   cmdBuildAccelerationStructures != nullptr &&
                   cmdWriteAccelerationStructuresProperties != nullptr &&
                   cmdCopyAccelerationStructure != nullptr &&
                   createRayTracingPipelines != nullptr &&
                   getRayTracingShaderGroupHandles != nullptr &&
                   cmdTraceRays != nullptr;
        }
    };

    VkBuildAccelerationStructureFlagsKHR ToVkAccelerationStructureBuildFlags(
        RHIAccelerationStructureBuildFlags flags,
        bool performUpdate = false);
    VkGeometryFlagsKHR ToVkRayTracingGeometryFlags(
        RHIRayTracingGeometryFlags flags);

    bool ValidateVulkanRayTracingBuildFlags(
        const RHICapabilities& capabilities,
        RHIAccelerationStructureBuildFlags flags,
        const char* operation);

    /**
     * @brief Reject a BLAS request that exceeds the selected device's native limits.
     *
     * The primitive limit applies to the aggregate triangle/AABB count across
     * every geometry in the BLAS, not to each geometry independently.
     */
    bool ValidateVulkanBottomLevelASDeviceLimits(
        const VkPhysicalDeviceAccelerationStructurePropertiesKHR& properties,
        const RHIBottomLevelASDesc& desc,
        const char* operation);

    /** @brief Reject a TLAS request that exceeds the selected device's instance limit. */
    bool ValidateVulkanTopLevelASDeviceLimits(
        const VkPhysicalDeviceAccelerationStructurePropertiesKHR& properties,
        const RHITopLevelASDesc& desc,
        const char* operation);

    /**
     * @brief Fill native geometry and range arrays for a validated BLAS request.
     *
     * `outBuildInfo` borrows `outGeometries`; callers must keep the vectors alive
     * through the Vulkan call that consumes the structure.
     */
    bool BuildVulkanBottomLevelASBuildInputs(
        VkPhysicalDevice physicalDevice,
        const RHIBottomLevelASDesc& desc,
        VkBuildAccelerationStructureFlagsKHR buildFlags,
        std::vector<VkAccelerationStructureGeometryKHR>& outGeometries,
        std::vector<VkAccelerationStructureBuildRangeInfoKHR>& outRanges,
        VkAccelerationStructureBuildGeometryInfoKHR& outBuildInfo);

    /**
     * @brief Fill a native TLAS build input for a validated request.
     *
     * Size queries may use a CPU instance list and therefore have a zero native
     * instance address.  Command recording must require `instanceBuffer`.
     */
    bool BuildVulkanTopLevelASBuildInputs(
        const RHITopLevelASDesc& desc,
        VkBuildAccelerationStructureFlagsKHR buildFlags,
        VkAccelerationStructureGeometryKHR& outGeometry,
        VkAccelerationStructureBuildRangeInfoKHR& outRange,
        VkAccelerationStructureBuildGeometryInfoKHR& outBuildInfo);

    uint64 AlignVulkanAccelerationStructureSize(uint64 size);
}
