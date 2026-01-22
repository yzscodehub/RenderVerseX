#include "VulkanDescriptorAllocator.h"
#include "VulkanDevice.h"

namespace RVX
{
    VulkanDescriptorAllocator::VulkanDescriptorAllocator(VulkanDevice* device)
        : m_device(device)
    {
        // Grab initial pool
        m_currentPool = GrabPool();
    }

    VulkanDescriptorAllocator::~VulkanDescriptorAllocator()
    {
        // Destroy all pools
        for (auto pool : m_pools)
        {
            vkDestroyDescriptorPool(m_device->GetDevice(), pool, nullptr);
        }
        m_pools.clear();

        for (auto pool : m_freePools)
        {
            vkDestroyDescriptorPool(m_device->GetDevice(), pool, nullptr);
        }
        m_freePools.clear();
    }

    VkDescriptorPool VulkanDescriptorAllocator::CreatePool()
    {
        std::vector<VkDescriptorPoolSize> poolSizes = {
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, s_uniformBuffersPerPool},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, s_uniformBuffersPerPool / 2},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, s_storageBuffersPerPool},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, s_storageBuffersPerPool / 2},
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, s_sampledImagesPerPool},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, s_storageImagesPerPool},
            {VK_DESCRIPTOR_TYPE_SAMPLER, s_samplersPerPool},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, s_combinedSamplersPerPool},
        };

        VkDescriptorPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        poolInfo.maxSets = s_setsPerPool;
        poolInfo.poolSizeCount = static_cast<uint32>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();

        VkDescriptorPool pool = VK_NULL_HANDLE;
        VkResult result = vkCreateDescriptorPool(m_device->GetDevice(), &poolInfo, nullptr, &pool);
        if (result != VK_SUCCESS)
        {
            RVX_RHI_ERROR("Failed to create descriptor pool: {}", VkResultToString(result));
            return VK_NULL_HANDLE;
        }

        RVX_RHI_DEBUG("Created new descriptor pool (total: {})", m_pools.size() + 1);
        return pool;
    }

    VkDescriptorPool VulkanDescriptorAllocator::GrabPool()
    {
        // Try to reuse a free pool
        if (!m_freePools.empty())
        {
            VkDescriptorPool pool = m_freePools.back();
            m_freePools.pop_back();
            return pool;
        }

        // Create a new pool
        VkDescriptorPool pool = CreatePool();
        m_pools.push_back(pool);
        return pool;
    }

    VkDescriptorSet VulkanDescriptorAllocator::Allocate(VkDescriptorSetLayout layout)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_currentPool == VK_NULL_HANDLE)
        {
            m_currentPool = GrabPool();
        }

        VkDescriptorSetAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocInfo.descriptorPool = m_currentPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &layout;

        VkDescriptorSet set = VK_NULL_HANDLE;
        VkResult result = vkAllocateDescriptorSets(m_device->GetDevice(), &allocInfo, &set);

        if (result == VK_ERROR_OUT_OF_POOL_MEMORY || result == VK_ERROR_FRAGMENTED_POOL)
        {
            // Current pool is exhausted, get a new one
            m_currentPool = GrabPool();
            allocInfo.descriptorPool = m_currentPool;

            result = vkAllocateDescriptorSets(m_device->GetDevice(), &allocInfo, &set);
            if (result != VK_SUCCESS)
            {
                RVX_RHI_ERROR("Failed to allocate descriptor set from new pool: {}", VkResultToString(result));
                return VK_NULL_HANDLE;
            }
        }
        else if (result != VK_SUCCESS)
        {
            RVX_RHI_ERROR("Failed to allocate descriptor set: {}", VkResultToString(result));
            return VK_NULL_HANDLE;
        }

        m_totalAllocations++;
        return set;
    }

    void VulkanDescriptorAllocator::Free(VkDescriptorSet set)
    {
        // Note: With VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT, individual frees are allowed
        // but can cause fragmentation. For transient resources, prefer ResetPools().
        
        std::lock_guard<std::mutex> lock(m_mutex);
        
        if (set != VK_NULL_HANDLE && m_currentPool != VK_NULL_HANDLE)
        {
            vkFreeDescriptorSets(m_device->GetDevice(), m_currentPool, 1, &set);
        }
    }

    void VulkanDescriptorAllocator::ResetPools()
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        // Reset all used pools
        for (auto pool : m_pools)
        {
            vkResetDescriptorPool(m_device->GetDevice(), pool, 0);
        }

        // Move all pools to free list
        m_freePools.insert(m_freePools.end(), m_pools.begin(), m_pools.end());
        m_pools.clear();

        // Get a fresh pool for new allocations
        m_currentPool = GrabPool();

        m_totalAllocations = 0;
    }

} // namespace RVX
