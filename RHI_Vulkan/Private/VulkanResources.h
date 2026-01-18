#pragma once

#include "VulkanCommon.h"
#include "RHI/RHIBuffer.h"
#include "RHI/RHITexture.h"
#include "RHI/RHISampler.h"
#include "RHI/RHIShader.h"
#include "RHI/RHISynchronization.h"
#include "RHI/RHIHeap.h"

namespace RVX
{
    class VulkanDevice;

    // =============================================================================
    // Vulkan Buffer
    // =============================================================================
    class VulkanBuffer : public RHIBuffer
    {
    public:
        VulkanBuffer(VulkanDevice* device, const RHIBufferDesc& desc);
        // Constructor for placed resources (external buffer, memory owned by heap)
        VulkanBuffer(VulkanDevice* device, VkBuffer buffer, VkDeviceMemory memory, 
                     uint64 memoryOffset, const RHIBufferDesc& desc, bool ownsBuffer);
        ~VulkanBuffer() override;

        uint64 GetSize() const override { return m_desc.size; }
        RHIBufferUsage GetUsage() const override { return m_desc.usage; }
        RHIMemoryType GetMemoryType() const override { return m_desc.memoryType; }
        uint32 GetStride() const override { return m_desc.stride; }

        void* Map() override;
        void Unmap() override;

        VkBuffer GetBuffer() const { return m_buffer; }
        VkDeviceAddress GetDeviceAddress() const { return m_deviceAddress; }

    private:
        VulkanDevice* m_device;
        RHIBufferDesc m_desc;
        VkBuffer m_buffer = VK_NULL_HANDLE;
        VmaAllocation m_allocation = VK_NULL_HANDLE;
        VkDeviceAddress m_deviceAddress = 0;
        void* m_mappedData = nullptr;
        bool m_ownsBuffer = true;           // False for placed resources
        VkDeviceMemory m_boundMemory = VK_NULL_HANDLE;  // For placed resources (memory owned by heap)
        uint64 m_memoryOffset = 0;          // Offset within the heap memory
    };

    // =============================================================================
    // Vulkan Texture
    // =============================================================================
    class VulkanTexture : public RHITexture
    {
    public:
        VulkanTexture(VulkanDevice* device, const RHITextureDesc& desc);
        VulkanTexture(VulkanDevice* device, VkImage image, const RHITextureDesc& desc, bool ownsImage = false);  // For swapchain/placed
        ~VulkanTexture() override;

        uint32 GetWidth() const override { return m_desc.width; }
        uint32 GetHeight() const override { return m_desc.height; }
        uint32 GetDepth() const override { return m_desc.depth; }
        uint32 GetMipLevels() const override { return m_desc.mipLevels; }
        uint32 GetArraySize() const override { return m_desc.arraySize; }
        RHIFormat GetFormat() const override { return m_desc.format; }
        RHITextureDimension GetDimension() const override { return m_desc.dimension; }
        RHITextureUsage GetUsage() const override { return m_desc.usage; }
        RHISampleCount GetSampleCount() const override { return m_desc.sampleCount; }

        VkImage GetImage() const { return m_image; }
        VkImageLayout GetCurrentLayout() const { return m_currentLayout; }
        void SetCurrentLayout(VkImageLayout layout) { m_currentLayout = layout; }

    private:
        VulkanDevice* m_device;
        RHITextureDesc m_desc;
        VkImage m_image = VK_NULL_HANDLE;
        VmaAllocation m_allocation = VK_NULL_HANDLE;
        VkImageLayout m_currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        bool m_ownsImage = true;  // True for VMA-allocated and placed resources
        bool m_ownsAllocation = true;  // False for placed resources (memory owned by heap)
    };

    // =============================================================================
    // Vulkan Texture View
    // =============================================================================
    class VulkanTextureView : public RHITextureView
    {
    public:
        VulkanTextureView(VulkanDevice* device, RHITexture* texture, const RHITextureViewDesc& desc);
        ~VulkanTextureView() override;

        RHITexture* GetTexture() const override { return m_texture; }
        RHIFormat GetFormat() const override { return m_format; }
        const RHISubresourceRange& GetSubresourceRange() const override { return m_subresourceRange; }

        VkImageView GetImageView() const { return m_imageView; }
        VulkanTexture* GetVulkanTexture() const { return m_texture; }

    private:
        VulkanDevice* m_device;
        VulkanTexture* m_texture;
        VkImageView m_imageView = VK_NULL_HANDLE;
        RHIFormat m_format;
        RHISubresourceRange m_subresourceRange;
    };

    // =============================================================================
    // Vulkan Sampler
    // =============================================================================
    class VulkanSampler : public RHISampler
    {
    public:
        VulkanSampler(VulkanDevice* device, const RHISamplerDesc& desc);
        ~VulkanSampler() override;

        VkSampler GetSampler() const { return m_sampler; }

    private:
        VulkanDevice* m_device;
        VkSampler m_sampler = VK_NULL_HANDLE;
    };

    // =============================================================================
    // Vulkan Shader
    // =============================================================================
    class VulkanShader : public RHIShader
    {
    public:
        VulkanShader(VulkanDevice* device, const RHIShaderDesc& desc);
        ~VulkanShader() override;

        RHIShaderStage GetStage() const override { return m_stage; }
        const std::vector<uint8>& GetBytecode() const override { return m_bytecode; }

        VkShaderModule GetShaderModule() const { return m_shaderModule; }
        const char* GetEntryPoint() const { return m_entryPoint.c_str(); }

    private:
        VulkanDevice* m_device;
        RHIShaderStage m_stage;
        std::vector<uint8> m_bytecode;
        std::string m_entryPoint;
        VkShaderModule m_shaderModule = VK_NULL_HANDLE;
    };

    // =============================================================================
    // Vulkan Fence (Timeline Semaphore)
    // =============================================================================
    class VulkanFence : public RHIFence
    {
    public:
        VulkanFence(VulkanDevice* device, uint64 initialValue);
        ~VulkanFence() override;

        uint64 GetCompletedValue() const override;
        void Signal(uint64 value) override;
        void Wait(uint64 value, uint64 timeoutNs = UINT64_MAX) override;

        VkSemaphore GetSemaphore() const { return m_semaphore; }

    private:
        VulkanDevice* m_device;
        VkSemaphore m_semaphore = VK_NULL_HANDLE;
    };

    // =============================================================================
    // Vulkan Heap (for Memory Aliasing / Placed Resources)
    // =============================================================================
    class VulkanHeap : public RHIHeap
    {
    public:
        VulkanHeap(VulkanDevice* device, const RHIHeapDesc& desc);
        ~VulkanHeap() override;

        // RHIHeap interface
        uint64 GetSize() const override { return m_size; }
        RHIHeapType GetType() const override { return m_type; }
        RHIHeapFlags GetFlags() const override { return m_flags; }

        // Vulkan Specific
        VkDeviceMemory GetMemory() const { return m_memory; }
        uint32 GetMemoryTypeIndex() const { return m_memoryTypeIndex; }

    private:
        VulkanDevice* m_device = nullptr;
        VkDeviceMemory m_memory = VK_NULL_HANDLE;
        uint64 m_size = 0;
        RHIHeapType m_type = RHIHeapType::Default;
        RHIHeapFlags m_flags = RHIHeapFlags::AllowAll;
        uint32 m_memoryTypeIndex = 0;
    };

    // =============================================================================
    // Factory Functions
    // =============================================================================
    RHIBufferRef CreateVulkanBuffer(VulkanDevice* device, const RHIBufferDesc& desc);
    RHITextureRef CreateVulkanTexture(VulkanDevice* device, const RHITextureDesc& desc);
    RHITextureViewRef CreateVulkanTextureView(VulkanDevice* device, RHITexture* texture, const RHITextureViewDesc& desc);
    RHISamplerRef CreateVulkanSampler(VulkanDevice* device, const RHISamplerDesc& desc);
    RHIShaderRef CreateVulkanShader(VulkanDevice* device, const RHIShaderDesc& desc);
    RHIFenceRef CreateVulkanFence(VulkanDevice* device, uint64 initialValue);
    void WaitForVulkanFence(VulkanDevice* device, RHIFence* fence, uint64 value);

    // Heap functions (for Memory Aliasing)
    RHIHeapRef CreateVulkanHeap(VulkanDevice* device, const RHIHeapDesc& desc);
    RHITextureRef CreateVulkanPlacedTexture(VulkanDevice* device, RHIHeap* heap, uint64 offset, const RHITextureDesc& desc);
    RHIBufferRef CreateVulkanPlacedBuffer(VulkanDevice* device, RHIHeap* heap, uint64 offset, const RHIBufferDesc& desc);

} // namespace RVX
