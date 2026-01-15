#include "VulkanResources.h"
#include "VulkanDevice.h"
#include <cmath>

namespace RVX
{
    // =============================================================================
    // Vulkan Buffer
    // =============================================================================
    VulkanBuffer::VulkanBuffer(VulkanDevice* device, const RHIBufferDesc& desc)
        : m_device(device)
        , m_desc(desc)
    {
        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }

        VkBufferCreateInfo bufferInfo = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = desc.size;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        // Usage flags
        bufferInfo.usage = 0;
        if (HasFlag(desc.usage, RHIBufferUsage::Vertex))
            bufferInfo.usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        if (HasFlag(desc.usage, RHIBufferUsage::Index))
            bufferInfo.usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        if (HasFlag(desc.usage, RHIBufferUsage::Constant))
            bufferInfo.usage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        if (HasFlag(desc.usage, RHIBufferUsage::ShaderResource))
            bufferInfo.usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        if (HasFlag(desc.usage, RHIBufferUsage::UnorderedAccess))
            bufferInfo.usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        if (HasFlag(desc.usage, RHIBufferUsage::Structured))
            bufferInfo.usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        if (HasFlag(desc.usage, RHIBufferUsage::IndirectArgs))
            bufferInfo.usage |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
        
        // Always allow transfer for buffer updates
        bufferInfo.usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        
        // Enable device address for raytracing/bindless
        bufferInfo.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

        VmaAllocationCreateInfo allocInfo = {};
        switch (desc.memoryType)
        {
            case RHIMemoryType::Default:
                allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
                break;
            case RHIMemoryType::Upload:
                allocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
                allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
                break;
            case RHIMemoryType::Readback:
                allocInfo.usage = VMA_MEMORY_USAGE_GPU_TO_CPU;
                allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
                break;
        }

        VmaAllocationInfo allocationInfo;
        VK_CHECK(vmaCreateBuffer(device->GetAllocator(), &bufferInfo, &allocInfo, 
            &m_buffer, &m_allocation, &allocationInfo));

        // Get persistent mapping for upload/readback buffers
        if (desc.memoryType == RHIMemoryType::Upload || desc.memoryType == RHIMemoryType::Readback)
        {
            m_mappedData = allocationInfo.pMappedData;
        }

        // Get device address
        VkBufferDeviceAddressInfo addressInfo = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        addressInfo.buffer = m_buffer;
        m_deviceAddress = vkGetBufferDeviceAddress(device->GetDevice(), &addressInfo);

        if (desc.debugName)
        {
            VkDebugUtilsObjectNameInfoEXT nameInfo = {VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
            nameInfo.objectType = VK_OBJECT_TYPE_BUFFER;
            nameInfo.objectHandle = reinterpret_cast<uint64>(m_buffer);
            nameInfo.pObjectName = desc.debugName;
            // vkSetDebugUtilsObjectNameEXT would need to be loaded dynamically
        }
    }

    VulkanBuffer::~VulkanBuffer()
    {
        if (m_buffer && m_allocation)
        {
            vmaDestroyBuffer(m_device->GetAllocator(), m_buffer, m_allocation);
        }
    }

    void* VulkanBuffer::Map()
    {
        if (m_mappedData)
            return m_mappedData;

        if (m_desc.memoryType == RHIMemoryType::Default)
        {
            RVX_RHI_ERROR("Cannot map GPU-only buffer");
            return nullptr;
        }

        VK_CHECK(vmaMapMemory(m_device->GetAllocator(), m_allocation, &m_mappedData));
        return m_mappedData;
    }

    void VulkanBuffer::Unmap()
    {
        // Persistently mapped buffers don't need unmapping
        if (m_desc.memoryType == RHIMemoryType::Upload || m_desc.memoryType == RHIMemoryType::Readback)
            return;

        if (m_mappedData)
        {
            vmaUnmapMemory(m_device->GetAllocator(), m_allocation);
            m_mappedData = nullptr;
        }
    }

    // =============================================================================
    // Vulkan Texture
    // =============================================================================
    VulkanTexture::VulkanTexture(VulkanDevice* device, const RHITextureDesc& desc)
        : m_device(device)
        , m_desc(desc)
        , m_ownsImage(true)
    {
        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }

        VkImageCreateInfo imageInfo = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        
        switch (desc.dimension)
        {
            case RHITextureDimension::Texture1D:
                imageInfo.imageType = VK_IMAGE_TYPE_1D;
                break;
            case RHITextureDimension::Texture2D:
                imageInfo.imageType = VK_IMAGE_TYPE_2D;
                break;
            case RHITextureDimension::Texture3D:
                imageInfo.imageType = VK_IMAGE_TYPE_3D;
                break;
            case RHITextureDimension::TextureCube:
                imageInfo.imageType = VK_IMAGE_TYPE_2D;
                imageInfo.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
                break;
        }

        imageInfo.extent.width = desc.width;
        imageInfo.extent.height = desc.height;
        imageInfo.extent.depth = desc.depth;
        imageInfo.mipLevels = desc.mipLevels;
        imageInfo.arrayLayers = desc.arraySize;
        imageInfo.format = ToVkFormat(desc.format);
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.samples = static_cast<VkSampleCountFlagBits>(desc.sampleCount);

        // Usage flags
        imageInfo.usage = 0;
        if (HasFlag(desc.usage, RHITextureUsage::ShaderResource))
            imageInfo.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
        if (HasFlag(desc.usage, RHITextureUsage::UnorderedAccess))
            imageInfo.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        if (HasFlag(desc.usage, RHITextureUsage::RenderTarget))
            imageInfo.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (HasFlag(desc.usage, RHITextureUsage::DepthStencil))
            imageInfo.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        
        imageInfo.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

        VmaAllocationCreateInfo allocInfo = {};
        allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

        VK_CHECK(vmaCreateImage(device->GetAllocator(), &imageInfo, &allocInfo,
            &m_image, &m_allocation, nullptr));

        m_currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    }

    VulkanTexture::VulkanTexture(VulkanDevice* device, VkImage image, const RHITextureDesc& desc)
        : m_device(device)
        , m_desc(desc)
        , m_image(image)
        , m_ownsImage(false)
        , m_currentLayout(VK_IMAGE_LAYOUT_UNDEFINED)
    {
        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }
    }

    VulkanTexture::~VulkanTexture()
    {
        if (m_ownsImage && m_image && m_allocation)
        {
            vmaDestroyImage(m_device->GetAllocator(), m_image, m_allocation);
        }
    }

    // =============================================================================
    // Vulkan Texture View
    // =============================================================================
    VulkanTextureView::VulkanTextureView(VulkanDevice* device, RHITexture* texture, const RHITextureViewDesc& desc)
        : m_device(device)
        , m_texture(static_cast<VulkanTexture*>(texture))
        , m_format(desc.format == RHIFormat::Unknown ? texture->GetFormat() : desc.format)
        , m_subresourceRange(desc.subresourceRange)
    {
        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }

        VkImageViewCreateInfo viewInfo = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = m_texture->GetImage();
        viewInfo.format = ToVkFormat(m_format);

        // View type based on texture dimension
        switch (texture->GetDimension())
        {
            case RHITextureDimension::Texture1D:
                viewInfo.viewType = (desc.subresourceRange.arrayLayerCount > 1) ? 
                    VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
                break;
            case RHITextureDimension::Texture2D:
                viewInfo.viewType = (desc.subresourceRange.arrayLayerCount > 1) ?
                    VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
                break;
            case RHITextureDimension::Texture3D:
                viewInfo.viewType = VK_IMAGE_VIEW_TYPE_3D;
                break;
            case RHITextureDimension::TextureCube:
                viewInfo.viewType = (desc.subresourceRange.arrayLayerCount > 6) ?
                    VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE;
                break;
        }

        // Subresource range
        viewInfo.subresourceRange.baseMipLevel = desc.subresourceRange.baseMipLevel;
        viewInfo.subresourceRange.levelCount = (desc.subresourceRange.mipLevelCount == 0 || desc.subresourceRange.mipLevelCount == RVX_ALL_MIPS) ? 
            VK_REMAINING_MIP_LEVELS : desc.subresourceRange.mipLevelCount;
        viewInfo.subresourceRange.baseArrayLayer = desc.subresourceRange.baseArrayLayer;
        viewInfo.subresourceRange.layerCount = (desc.subresourceRange.arrayLayerCount == 0 || desc.subresourceRange.arrayLayerCount == RVX_ALL_LAYERS) ?
            VK_REMAINING_ARRAY_LAYERS : desc.subresourceRange.arrayLayerCount;

        // Aspect mask
        if (HasFlag(texture->GetUsage(), RHITextureUsage::DepthStencil))
        {
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            // Add stencil aspect for formats that have it
            VkFormat vkFormat = ToVkFormat(m_format);
            if (vkFormat == VK_FORMAT_D24_UNORM_S8_UINT || vkFormat == VK_FORMAT_D32_SFLOAT_S8_UINT)
            {
                viewInfo.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
            }
        }
        else
        {
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        }

        viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;

        VK_CHECK(vkCreateImageView(device->GetDevice(), &viewInfo, nullptr, &m_imageView));
    }

    VulkanTextureView::~VulkanTextureView()
    {
        if (m_imageView)
        {
            vkDestroyImageView(m_device->GetDevice(), m_imageView, nullptr);
        }
    }

    // =============================================================================
    // Vulkan Sampler
    // =============================================================================
    static VkBorderColor ToVkBorderColor(const float borderColor[4])
    {
        auto closeTo = [](float value, float target) {
            return std::abs(value - target) < 1e-6f;
        };

        const bool isZeroRgb = closeTo(borderColor[0], 0.0f) &&
                               closeTo(borderColor[1], 0.0f) &&
                               closeTo(borderColor[2], 0.0f);
        const bool isOneRgb = closeTo(borderColor[0], 1.0f) &&
                              closeTo(borderColor[1], 1.0f) &&
                              closeTo(borderColor[2], 1.0f);
        const bool isAlphaZero = closeTo(borderColor[3], 0.0f);
        const bool isAlphaOne = closeTo(borderColor[3], 1.0f);

        if (isZeroRgb && isAlphaZero)
            return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        if (isZeroRgb && isAlphaOne)
            return VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        if (isOneRgb && isAlphaOne)
            return VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;

        RVX_RHI_WARN("Unsupported Vulkan border color (only transparent/opaque black/white supported). Using transparent black.");
        return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    }

    VulkanSampler::VulkanSampler(VulkanDevice* device, const RHISamplerDesc& desc)
        : m_device(device)
    {
        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }

        VkSamplerCreateInfo samplerInfo = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samplerInfo.magFilter = ToVkFilter(desc.magFilter);
        samplerInfo.minFilter = ToVkFilter(desc.minFilter);
        samplerInfo.mipmapMode = ToVkMipmapMode(desc.mipFilter);
        samplerInfo.addressModeU = ToVkSamplerAddressMode(desc.addressU);
        samplerInfo.addressModeV = ToVkSamplerAddressMode(desc.addressV);
        samplerInfo.addressModeW = ToVkSamplerAddressMode(desc.addressW);
        samplerInfo.mipLodBias = desc.mipLodBias;
        samplerInfo.anisotropyEnable = desc.anisotropyEnable;
        samplerInfo.maxAnisotropy = desc.maxAnisotropy;
        samplerInfo.compareEnable = desc.compareEnable;
        samplerInfo.compareOp = ToVkCompareOp(desc.compareOp);
        samplerInfo.minLod = desc.minLod;
        samplerInfo.maxLod = desc.maxLod;
        samplerInfo.borderColor = ToVkBorderColor(desc.borderColor);

        VK_CHECK(vkCreateSampler(device->GetDevice(), &samplerInfo, nullptr, &m_sampler));
    }

    VulkanSampler::~VulkanSampler()
    {
        if (m_sampler)
        {
            vkDestroySampler(m_device->GetDevice(), m_sampler, nullptr);
        }
    }

    // =============================================================================
    // Vulkan Shader
    // =============================================================================
    VulkanShader::VulkanShader(VulkanDevice* device, const RHIShaderDesc& desc)
        : m_device(device)
        , m_stage(desc.stage)
        , m_entryPoint(desc.entryPoint ? desc.entryPoint : "main")
    {
        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }

        // Copy bytecode
        m_bytecode.resize(desc.bytecodeSize);
        std::memcpy(m_bytecode.data(), desc.bytecode, desc.bytecodeSize);

        VkShaderModuleCreateInfo createInfo = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        createInfo.codeSize = desc.bytecodeSize;
        createInfo.pCode = reinterpret_cast<const uint32*>(desc.bytecode);

        VK_CHECK(vkCreateShaderModule(device->GetDevice(), &createInfo, nullptr, &m_shaderModule));
    }

    VulkanShader::~VulkanShader()
    {
        if (m_shaderModule)
        {
            vkDestroyShaderModule(m_device->GetDevice(), m_shaderModule, nullptr);
        }
    }

    // =============================================================================
    // Vulkan Fence (Timeline Semaphore)
    // =============================================================================
    VulkanFence::VulkanFence(VulkanDevice* device, uint64 initialValue)
        : m_device(device)
    {
        VkSemaphoreTypeCreateInfo timelineInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        timelineInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        timelineInfo.initialValue = initialValue;

        VkSemaphoreCreateInfo semaphoreInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        semaphoreInfo.pNext = &timelineInfo;

        VK_CHECK(vkCreateSemaphore(device->GetDevice(), &semaphoreInfo, nullptr, &m_semaphore));
    }

    VulkanFence::~VulkanFence()
    {
        if (m_semaphore)
        {
            vkDestroySemaphore(m_device->GetDevice(), m_semaphore, nullptr);
        }
    }

    uint64 VulkanFence::GetCompletedValue() const
    {
        uint64 value = 0;
        VK_CHECK(vkGetSemaphoreCounterValue(m_device->GetDevice(), m_semaphore, &value));
        return value;
    }

    void VulkanFence::Signal(uint64 value)
    {
        VkSemaphoreSignalInfo signalInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
        signalInfo.semaphore = m_semaphore;
        signalInfo.value = value;
        VK_CHECK(vkSignalSemaphore(m_device->GetDevice(), &signalInfo));
    }

    void VulkanFence::Wait(uint64 value, uint64 timeoutNs)
    {
        VkSemaphoreWaitInfo waitInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        waitInfo.semaphoreCount = 1;
        waitInfo.pSemaphores = &m_semaphore;
        waitInfo.pValues = &value;
        VK_CHECK(vkWaitSemaphores(m_device->GetDevice(), &waitInfo, timeoutNs));
    }

    // =============================================================================
    // Factory Functions
    // =============================================================================
    RHIBufferRef CreateVulkanBuffer(VulkanDevice* device, const RHIBufferDesc& desc)
    {
        return Ref<VulkanBuffer>(new VulkanBuffer(device, desc));
    }

    RHITextureRef CreateVulkanTexture(VulkanDevice* device, const RHITextureDesc& desc)
    {
        return Ref<VulkanTexture>(new VulkanTexture(device, desc));
    }

    RHITextureViewRef CreateVulkanTextureView(VulkanDevice* device, RHITexture* texture, const RHITextureViewDesc& desc)
    {
        return Ref<VulkanTextureView>(new VulkanTextureView(device, texture, desc));
    }

    RHISamplerRef CreateVulkanSampler(VulkanDevice* device, const RHISamplerDesc& desc)
    {
        return Ref<VulkanSampler>(new VulkanSampler(device, desc));
    }

    RHIShaderRef CreateVulkanShader(VulkanDevice* device, const RHIShaderDesc& desc)
    {
        return Ref<VulkanShader>(new VulkanShader(device, desc));
    }

    RHIFenceRef CreateVulkanFence(VulkanDevice* device, uint64 initialValue)
    {
        return Ref<VulkanFence>(new VulkanFence(device, initialValue));
    }

    void WaitForVulkanFence(VulkanDevice* device, RHIFence* fence, uint64 value)
    {
        if (fence)
        {
            static_cast<VulkanFence*>(fence)->Wait(value);
        }
    }

} // namespace RVX
