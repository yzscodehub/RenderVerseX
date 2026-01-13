#pragma once

#include "VulkanCommon.h"
#include "RHI/RHIPipeline.h"
#include "RHI/RHIDescriptor.h"

namespace RVX
{
    class VulkanDevice;

    // =============================================================================
    // Vulkan Descriptor Set Layout
    // =============================================================================
    class VulkanDescriptorSetLayout : public RHIDescriptorSetLayout
    {
    public:
        VulkanDescriptorSetLayout(VulkanDevice* device, const RHIDescriptorSetLayoutDesc& desc);
        ~VulkanDescriptorSetLayout() override;

        VkDescriptorSetLayout GetLayout() const { return m_layout; }
        const std::vector<RHIBindingLayoutEntry>& GetEntries() const { return m_entries; }

    private:
        VulkanDevice* m_device;
        VkDescriptorSetLayout m_layout = VK_NULL_HANDLE;
        std::vector<RHIBindingLayoutEntry> m_entries;
    };

    // =============================================================================
    // Vulkan Pipeline Layout
    // =============================================================================
    class VulkanPipelineLayout : public RHIPipelineLayout
    {
    public:
        VulkanPipelineLayout(VulkanDevice* device, const RHIPipelineLayoutDesc& desc);
        ~VulkanPipelineLayout() override;

        VkPipelineLayout GetLayout() const { return m_layout; }

    private:
        VulkanDevice* m_device;
        VkPipelineLayout m_layout = VK_NULL_HANDLE;
        std::vector<VkDescriptorSetLayout> m_setLayouts;
    };

    // =============================================================================
    // Vulkan Pipeline
    // =============================================================================
    class VulkanPipeline : public RHIPipeline
    {
    public:
        VulkanPipeline(VulkanDevice* device, const RHIGraphicsPipelineDesc& desc);
        VulkanPipeline(VulkanDevice* device, const RHIComputePipelineDesc& desc);
        ~VulkanPipeline() override;

        bool IsCompute() const override { return m_isCompute; }

        VkPipeline GetPipeline() const { return m_pipeline; }
        VkPipelineLayout GetPipelineLayout() const { return m_pipelineLayout; }

    private:
        void CreateGraphicsPipeline(const RHIGraphicsPipelineDesc& desc);
        void CreateComputePipeline(const RHIComputePipelineDesc& desc);

        VulkanDevice* m_device;
        VkPipeline m_pipeline = VK_NULL_HANDLE;
        VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
        bool m_isCompute = false;
    };

    // =============================================================================
    // Vulkan Descriptor Set
    // =============================================================================
    class VulkanDescriptorSet : public RHIDescriptorSet
    {
    public:
        VulkanDescriptorSet(VulkanDevice* device, const RHIDescriptorSetDesc& desc);
        ~VulkanDescriptorSet() override;

        void Update(const std::vector<RHIDescriptorBinding>& bindings) override;

        VkDescriptorSet GetDescriptorSet() const { return m_descriptorSet; }

    private:
        VulkanDevice* m_device;
        VkDescriptorSet m_descriptorSet = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_layout = VK_NULL_HANDLE;
    };

    // =============================================================================
    // Factory Functions
    // =============================================================================
    RHIDescriptorSetLayoutRef CreateVulkanDescriptorSetLayout(VulkanDevice* device, const RHIDescriptorSetLayoutDesc& desc);
    RHIPipelineLayoutRef CreateVulkanPipelineLayout(VulkanDevice* device, const RHIPipelineLayoutDesc& desc);
    RHIPipelineRef CreateVulkanGraphicsPipeline(VulkanDevice* device, const RHIGraphicsPipelineDesc& desc);
    RHIPipelineRef CreateVulkanComputePipeline(VulkanDevice* device, const RHIComputePipelineDesc& desc);
    RHIDescriptorSetRef CreateVulkanDescriptorSet(VulkanDevice* device, const RHIDescriptorSetDesc& desc);

} // namespace RVX
