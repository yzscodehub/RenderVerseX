#pragma once

#include "VulkanCommon.h"
#include "RHI/RHIDescriptor.h"
#include "RHI/RHIPipeline.h"
#include "RHI/RHIRayTracing.h"

namespace RVX
{
    class VulkanDevice;

    /**
     * @brief Select the RHI blend-state row used by one Vulkan color target.
     *
     * When independent blending is disabled, the RHI contract applies render
     * target zero's state to every attachment.  Keeping this selection explicit
     * prevents Vulkan from observing accidental per-target differences that the
     * caller did not enable.
     */
    inline uint32 SelectVulkanBlendAttachmentStateIndex(
        bool independentBlendEnable,
        uint32 attachmentIndex) noexcept
    {
        return independentBlendEnable ? attachmentIndex : 0u;
    }

    // =============================================================================
    // Vulkan Descriptor Set Layout
    // =============================================================================
    class VulkanDescriptorSetLayout : public RHIDescriptorSetLayout
    {
    public:
        VulkanDescriptorSetLayout(VulkanDevice* device, const RHIDescriptorSetLayoutDesc& desc);
        ~VulkanDescriptorSetLayout() override;

        VkDescriptorSetLayout GetLayout() const { return m_layout; }
        const std::vector<RHIBindingLayoutEntry>& GetEntries() const override { return m_entries; }
        const RHIBindingLayoutEntry* FindEntry(uint32 binding) const;

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
        VulkanPipeline(VulkanDevice* device, const RHIRayTracingPipelineDesc& desc);
        ~VulkanPipeline() override;

        bool IsCompute() const override { return m_isCompute; }
        bool IsRayTracing() const override { return m_isRayTracing; }
        uint32 GetRayTracingShaderGroupCount() const override;
        RHIShaderStage GetRayTracingShaderGroupStage(uint32 shaderGroupIndex) const override;
        bool IsRayTracingHitGroup(uint32 shaderGroupIndex) const override;

        VkPipeline GetPipeline() const { return m_pipeline; }
        VkPipelineLayout GetPipelineLayout() const { return m_pipelineLayout; }
        VulkanPipelineLayout* GetDescriptorPipelineLayout() const { return m_descriptorPipelineLayout; }

    private:
        void CreateGraphicsPipeline(const RHIGraphicsPipelineDesc& desc);
        void CreateComputePipeline(const RHIComputePipelineDesc& desc);
        void CreateRayTracingPipeline(const RHIRayTracingPipelineDesc& desc);

        VulkanDevice* m_device;
        VkPipeline m_pipeline = VK_NULL_HANDLE;
        VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
        VulkanPipelineLayout* m_descriptorPipelineLayout = nullptr;
        bool m_isCompute = false;
        bool m_isRayTracing = false;
        std::vector<RHIShaderStage> m_rayTracingShaderGroupStages;
        std::vector<uint8> m_rayTracingShaderGroupIsHitGroup;
    };

    /**
     * @brief Native shader binding table regions used by vkCmdTraceRaysKHR.
     *
     * The table owns the backing buffer. These regions borrow that buffer and
     * are consequently valid only while the VulkanShaderTable remains alive.
     */
    struct VulkanRayTracingDispatchRegions
    {
        VkStridedDeviceAddressRegionKHR rayGeneration = {};
        VkStridedDeviceAddressRegionKHR miss = {};
        VkStridedDeviceAddressRegionKHR hitGroup = {};
        VkStridedDeviceAddressRegionKHR callable = {};
    };

    // =============================================================================
    // Vulkan Shader Table
    // =============================================================================
    class VulkanShaderTable : public RHIShaderTable
    {
    public:
        VulkanShaderTable(VulkanDevice* device, const RHIShaderTableDesc& desc);
        ~VulkanShaderTable() override = default;

        uint32 GetRayGenerationRecordCount() const override { return m_rayGenerationSection.count; }
        uint32 GetMissRecordCount() const override { return m_missSection.count; }
        uint32 GetHitGroupRecordCount() const override { return m_hitGroupSection.count; }
        uint32 GetCallableRecordCount() const override { return m_callableSection.count; }
        RHIPipeline* GetRayTracingPipeline() const override { return m_pipeline; }

        bool IsValid() const;
        VulkanPipeline* GetPipeline() const { return m_pipeline; }
        VulkanRayTracingDispatchRegions BuildDispatchRaysRegions(
            uint32 width,
            uint32 height,
            uint32 depth) const;

    private:
        struct Section
        {
            uint64 offset = 0;
            uint64 size = 0;
            uint64 stride = 0;
            uint32 count = 0;
        };

        bool Create(const RHIShaderTableDesc& desc);

        VulkanDevice* m_device = nullptr;
        RHIPipelineRef m_pipelineOwner;
        VulkanPipeline* m_pipeline = nullptr;
        RHIBufferRef m_buffer;
        Section m_rayGenerationSection;
        Section m_missSection;
        Section m_hitGroupSection;
        Section m_callableSection;
    };

    // =============================================================================
    // Vulkan Descriptor Set
    // =============================================================================
    class VulkanDescriptorSet : public RHIDescriptorSet
    {
    public:
        VulkanDescriptorSet(VulkanDevice* device, const RHIDescriptorSetDesc& desc);
        ~VulkanDescriptorSet() override;

        VkDescriptorSet GetDescriptorSet() const { return m_descriptorSet; }
        VulkanDescriptorSetLayout* GetLayout() const { return m_layoutWrapper; }

    private:
        bool InitializeNativeSnapshot();
        VulkanDevice* m_device;
        VkDescriptorSet m_descriptorSet = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_layout = VK_NULL_HANDLE;
        VulkanDescriptorSetLayout* m_layoutWrapper = nullptr;
    };

    // =============================================================================
    // Factory Functions
    // =============================================================================
    RHIDescriptorSetLayoutRef CreateVulkanDescriptorSetLayout(VulkanDevice* device, const RHIDescriptorSetLayoutDesc& desc);
    RHIPipelineLayoutRef CreateVulkanPipelineLayout(VulkanDevice* device, const RHIPipelineLayoutDesc& desc);
    RHIPipelineRef CreateVulkanGraphicsPipeline(VulkanDevice* device, const RHIGraphicsPipelineDesc& desc);
    RHIPipelineRef CreateVulkanComputePipeline(VulkanDevice* device, const RHIComputePipelineDesc& desc);
    RHIPipelineRef CreateVulkanRayTracingPipeline(VulkanDevice* device, const RHIRayTracingPipelineDesc& desc);
    RHIShaderTableRef CreateVulkanShaderTable(VulkanDevice* device, const RHIShaderTableDesc& desc);
    RHIDescriptorSetRef CreateVulkanDescriptorSet(VulkanDevice* device, const RHIDescriptorSetDesc& desc);

} // namespace RVX
