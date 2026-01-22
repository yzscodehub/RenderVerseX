#pragma once

#include "VulkanCommon.h"
#include <vector>
#include <mutex>

namespace RVX
{
    class VulkanDevice;

    // =============================================================================
    // VulkanDescriptorAllocator - Dynamic Descriptor Pool Management
    // 
    // Handles automatic creation of new descriptor pools when existing ones are
    // exhausted. Supports per-frame reset for transient allocations.
    // =============================================================================
    class VulkanDescriptorAllocator
    {
    public:
        VulkanDescriptorAllocator(VulkanDevice* device);
        ~VulkanDescriptorAllocator();

        // Allocate a descriptor set from the pool
        VkDescriptorSet Allocate(VkDescriptorSetLayout layout);

        // Free a specific descriptor set (optional - can skip if using ResetPools)
        void Free(VkDescriptorSet set);

        // Reset all pools for a new frame (returns all sets to available state)
        void ResetPools();

        // Get statistics
        uint32 GetPoolCount() const { return static_cast<uint32>(m_pools.size()); }
        uint32 GetTotalAllocations() const { return m_totalAllocations; }

    private:
        VkDescriptorPool CreatePool();
        VkDescriptorPool GrabPool();

        VulkanDevice* m_device = nullptr;

        // Active pool for current allocations
        VkDescriptorPool m_currentPool = VK_NULL_HANDLE;

        // All pools (including current)
        std::vector<VkDescriptorPool> m_pools;

        // Pools that are free and can be reused
        std::vector<VkDescriptorPool> m_freePools;

        // Thread safety
        std::mutex m_mutex;

        // Statistics
        uint32 m_totalAllocations = 0;

        // Pool configuration
        static constexpr uint32 s_setsPerPool = 1000;
        static constexpr uint32 s_uniformBuffersPerPool = 2000;
        static constexpr uint32 s_storageBuffersPerPool = 2000;
        static constexpr uint32 s_sampledImagesPerPool = 2000;
        static constexpr uint32 s_storageImagesPerPool = 500;
        static constexpr uint32 s_samplersPerPool = 500;
        static constexpr uint32 s_combinedSamplersPerPool = 2000;
    };

} // namespace RVX
