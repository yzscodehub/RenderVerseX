#include "VulkanPipeline.h"
#include "VulkanDevice.h"
#include "VulkanResources.h"
#include <unordered_map>

namespace RVX
{
    // =============================================================================
    // Vulkan Descriptor Set Layout
    // =============================================================================
    VulkanDescriptorSetLayout::VulkanDescriptorSetLayout(VulkanDevice* device, const RHIDescriptorSetLayoutDesc& desc)
        : m_device(device)
        , m_entries(desc.entries)
    {
        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }

        std::vector<VkDescriptorSetLayoutBinding> bindings;
        for (const auto& entry : desc.entries)
        {
            VkDescriptorSetLayoutBinding binding = {};
            binding.binding = entry.binding;
            binding.descriptorType = ToVkDescriptorType(entry.type);
            binding.descriptorCount = entry.count;
            binding.stageFlags = ToVkShaderStageFlags(entry.visibility);
            binding.pImmutableSamplers = nullptr;
            bindings.push_back(binding);
        }

        VkDescriptorSetLayoutCreateInfo layoutInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.bindingCount = static_cast<uint32>(bindings.size());
        layoutInfo.pBindings = bindings.data();

        VK_CHECK(vkCreateDescriptorSetLayout(device->GetDevice(), &layoutInfo, nullptr, &m_layout));
    }

    VulkanDescriptorSetLayout::~VulkanDescriptorSetLayout()
    {
        if (m_layout)
        {
            vkDestroyDescriptorSetLayout(m_device->GetDevice(), m_layout, nullptr);
        }
    }

    const RHIBindingLayoutEntry* VulkanDescriptorSetLayout::FindEntry(uint32 binding) const
    {
        for (const auto& entry : m_entries)
        {
            if (entry.binding == binding)
                return &entry;
        }
        return nullptr;
    }

    // =============================================================================
    // Vulkan Pipeline Layout
    // =============================================================================
    VulkanPipelineLayout::VulkanPipelineLayout(VulkanDevice* device, const RHIPipelineLayoutDesc& desc)
        : m_device(device)
    {
        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }

        // Collect descriptor set layouts
        for (auto* setLayout : desc.setLayouts)
        {
            auto* vkSetLayout = static_cast<VulkanDescriptorSetLayout*>(setLayout);
            m_setLayouts.push_back(vkSetLayout->GetLayout());
        }

        VkPipelineLayoutCreateInfo layoutInfo = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.setLayoutCount = static_cast<uint32>(m_setLayouts.size());
        layoutInfo.pSetLayouts = m_setLayouts.data();

        // Push constant range
        VkPushConstantRange pushConstantRange = {};
        if (desc.pushConstantSize > 0)
        {
            pushConstantRange.stageFlags = ToVkShaderStageFlags(desc.pushConstantStages);
            pushConstantRange.offset = 0;
            pushConstantRange.size = desc.pushConstantSize;
            layoutInfo.pushConstantRangeCount = 1;
            layoutInfo.pPushConstantRanges = &pushConstantRange;
        }

        VK_CHECK(vkCreatePipelineLayout(device->GetDevice(), &layoutInfo, nullptr, &m_layout));
    }

    VulkanPipelineLayout::~VulkanPipelineLayout()
    {
        if (m_layout)
        {
            vkDestroyPipelineLayout(m_device->GetDevice(), m_layout, nullptr);
        }
    }

    // =============================================================================
    // Vulkan Pipeline
    // =============================================================================
    VulkanPipeline::VulkanPipeline(VulkanDevice* device, const RHIGraphicsPipelineDesc& desc)
        : m_device(device)
        , m_isCompute(false)
    {
        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }
        CreateGraphicsPipeline(desc);
    }

    VulkanPipeline::VulkanPipeline(VulkanDevice* device, const RHIComputePipelineDesc& desc)
        : m_device(device)
        , m_isCompute(true)
    {
        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }
        CreateComputePipeline(desc);
    }

    VulkanPipeline::~VulkanPipeline()
    {
        if (m_pipeline)
        {
            vkDestroyPipeline(m_device->GetDevice(), m_pipeline, nullptr);
        }
    }

    void VulkanPipeline::CreateGraphicsPipeline(const RHIGraphicsPipelineDesc& desc)
    {
        // Get pipeline layout
        if (desc.pipelineLayout)
        {
            m_pipelineLayout = static_cast<VulkanPipelineLayout*>(desc.pipelineLayout)->GetLayout();
        }

        // Shader stages
        std::vector<VkPipelineShaderStageCreateInfo> shaderStages;
        
        if (desc.vertexShader)
        {
            auto* vkShader = static_cast<VulkanShader*>(desc.vertexShader);
            VkPipelineShaderStageCreateInfo stageInfo = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            stageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
            stageInfo.module = vkShader->GetShaderModule();
            stageInfo.pName = vkShader->GetEntryPoint();
            shaderStages.push_back(stageInfo);
        }
        if (desc.pixelShader)
        {
            auto* vkShader = static_cast<VulkanShader*>(desc.pixelShader);
            VkPipelineShaderStageCreateInfo stageInfo = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            stageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            stageInfo.module = vkShader->GetShaderModule();
            stageInfo.pName = vkShader->GetEntryPoint();
            shaderStages.push_back(stageInfo);
        }
        if (desc.geometryShader)
        {
            auto* vkShader = static_cast<VulkanShader*>(desc.geometryShader);
            VkPipelineShaderStageCreateInfo stageInfo = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            stageInfo.stage = VK_SHADER_STAGE_GEOMETRY_BIT;
            stageInfo.module = vkShader->GetShaderModule();
            stageInfo.pName = vkShader->GetEntryPoint();
            shaderStages.push_back(stageInfo);
        }

        // Vertex input
        std::vector<VkVertexInputBindingDescription> bindingDescs;
        std::vector<VkVertexInputAttributeDescription> attributeDescs;
        
        // Track per-binding offsets and strides for separate vertex buffer slots
        std::unordered_map<uint32, uint32> bindingOffsets;  // Current offset per binding
        std::unordered_map<uint32, uint32> bindingStrides;  // Total stride per binding
        std::unordered_map<uint32, bool> bindingPerInstance; // Per-instance rate per binding

        for (const auto& elem : desc.inputLayout.elements)
        {
            uint32 bindingSlot = elem.inputSlot;
            
            // Initialize binding tracking if this is a new slot
            if (bindingOffsets.find(bindingSlot) == bindingOffsets.end())
            {
                bindingOffsets[bindingSlot] = 0;
                bindingStrides[bindingSlot] = 0;
                bindingPerInstance[bindingSlot] = elem.perInstance;
            }

            VkVertexInputAttributeDescription attr = {};
            attr.location = static_cast<uint32>(attributeDescs.size());
            attr.binding = bindingSlot;
            attr.format = ToVkFormat(elem.format);
            
            // Calculate offset within this binding
            uint32& currentOffset = bindingOffsets[bindingSlot];
            attr.offset = (elem.alignedByteOffset == 0xFFFFFFFF) ? currentOffset : elem.alignedByteOffset;
            attributeDescs.push_back(attr);
            
            // Update stride for this binding
            uint32 elemSize = GetFormatBytesPerPixel(elem.format);
            currentOffset = attr.offset + elemSize;
            bindingStrides[bindingSlot] = currentOffset;
        }

        // Create a binding description for each unique input slot
        for (const auto& [slot, stride] : bindingStrides)
        {
            VkVertexInputBindingDescription binding = {};
            binding.binding = slot;
            binding.stride = stride;
            binding.inputRate = bindingPerInstance[slot] ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX;
            bindingDescs.push_back(binding);
        }

        VkPipelineVertexInputStateCreateInfo vertexInputInfo = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vertexInputInfo.vertexBindingDescriptionCount = static_cast<uint32>(bindingDescs.size());
        vertexInputInfo.pVertexBindingDescriptions = bindingDescs.data();
        vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32>(attributeDescs.size());
        vertexInputInfo.pVertexAttributeDescriptions = attributeDescs.data();

        // Input assembly
        VkPipelineInputAssemblyStateCreateInfo inputAssembly = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        inputAssembly.topology = ToVkPrimitiveTopology(desc.primitiveTopology);
        inputAssembly.primitiveRestartEnable = VK_FALSE;

        // Viewport state (dynamic)
        VkPipelineViewportStateCreateInfo viewportState = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        // Rasterization
        VkPipelineRasterizationStateCreateInfo rasterizer = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rasterizer.depthClampEnable = !desc.rasterizerState.depthClipEnable;
        rasterizer.rasterizerDiscardEnable = VK_FALSE;
        rasterizer.polygonMode = ToVkPolygonMode(desc.rasterizerState.fillMode);
        rasterizer.cullMode = ToVkCullMode(desc.rasterizerState.cullMode);
        rasterizer.frontFace = (desc.rasterizerState.frontFace == RHIFrontFace::CounterClockwise) ?
            VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
        rasterizer.depthBiasEnable = (desc.rasterizerState.depthBias != 0.0f);
        rasterizer.depthBiasConstantFactor = desc.rasterizerState.depthBias;
        rasterizer.depthBiasClamp = desc.rasterizerState.depthBiasClamp;
        rasterizer.depthBiasSlopeFactor = desc.rasterizerState.slopeScaledDepthBias;
        rasterizer.lineWidth = 1.0f;

        // Multisampling
        VkPipelineMultisampleStateCreateInfo multisampling = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisampling.rasterizationSamples = static_cast<VkSampleCountFlagBits>(desc.sampleCount);
        multisampling.sampleShadingEnable = VK_FALSE;

        // Depth stencil
        VkPipelineDepthStencilStateCreateInfo depthStencil = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depthStencil.depthTestEnable = desc.depthStencilState.depthTestEnable;
        depthStencil.depthWriteEnable = desc.depthStencilState.depthWriteEnable;
        depthStencil.depthCompareOp = ToVkCompareOp(desc.depthStencilState.depthCompareOp);
        depthStencil.depthBoundsTestEnable = VK_FALSE;
        depthStencil.stencilTestEnable = desc.depthStencilState.stencilTestEnable;

        depthStencil.front.failOp = ToVkStencilOp(desc.depthStencilState.frontFace.failOp);
        depthStencil.front.passOp = ToVkStencilOp(desc.depthStencilState.frontFace.passOp);
        depthStencil.front.depthFailOp = ToVkStencilOp(desc.depthStencilState.frontFace.depthFailOp);
        depthStencil.front.compareOp = ToVkCompareOp(desc.depthStencilState.frontFace.compareOp);
        depthStencil.front.compareMask = desc.depthStencilState.stencilReadMask;
        depthStencil.front.writeMask = desc.depthStencilState.stencilWriteMask;

        depthStencil.back.failOp = ToVkStencilOp(desc.depthStencilState.backFace.failOp);
        depthStencil.back.passOp = ToVkStencilOp(desc.depthStencilState.backFace.passOp);
        depthStencil.back.depthFailOp = ToVkStencilOp(desc.depthStencilState.backFace.depthFailOp);
        depthStencil.back.compareOp = ToVkCompareOp(desc.depthStencilState.backFace.compareOp);
        depthStencil.back.compareMask = desc.depthStencilState.stencilReadMask;
        depthStencil.back.writeMask = desc.depthStencilState.stencilWriteMask;

        // Color blending
        std::vector<VkPipelineColorBlendAttachmentState> colorBlendAttachments;
        for (uint32 i = 0; i < desc.numRenderTargets; ++i)
        {
            const auto& rtBlend = desc.blendState.renderTargets[i];
            VkPipelineColorBlendAttachmentState attachment = {};
            attachment.blendEnable = rtBlend.blendEnable;
            attachment.srcColorBlendFactor = ToVkBlendFactor(rtBlend.srcColorBlend);
            attachment.dstColorBlendFactor = ToVkBlendFactor(rtBlend.dstColorBlend);
            attachment.colorBlendOp = ToVkBlendOp(rtBlend.colorBlendOp);
            attachment.srcAlphaBlendFactor = ToVkBlendFactor(rtBlend.srcAlphaBlend);
            attachment.dstAlphaBlendFactor = ToVkBlendFactor(rtBlend.dstAlphaBlend);
            attachment.alphaBlendOp = ToVkBlendOp(rtBlend.alphaBlendOp);
            attachment.colorWriteMask = rtBlend.colorWriteMask;
            colorBlendAttachments.push_back(attachment);
        }

        VkPipelineColorBlendStateCreateInfo colorBlending = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        colorBlending.logicOpEnable = VK_FALSE;
        colorBlending.attachmentCount = static_cast<uint32>(colorBlendAttachments.size());
        colorBlending.pAttachments = colorBlendAttachments.data();

        // Dynamic state
        std::vector<VkDynamicState> dynamicStates = {
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR
        };
        VkPipelineDynamicStateCreateInfo dynamicState = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamicState.dynamicStateCount = static_cast<uint32>(dynamicStates.size());
        dynamicState.pDynamicStates = dynamicStates.data();

        // Rendering info for dynamic rendering (Vulkan 1.3)
        std::vector<VkFormat> colorFormats;
        for (uint32 i = 0; i < desc.numRenderTargets; ++i)
        {
            colorFormats.push_back(ToVkFormat(desc.renderTargetFormats[i]));
        }

        VkPipelineRenderingCreateInfo renderingInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        renderingInfo.colorAttachmentCount = desc.numRenderTargets;
        renderingInfo.pColorAttachmentFormats = colorFormats.data();
        renderingInfo.depthAttachmentFormat = ToVkFormat(desc.depthStencilFormat);
        renderingInfo.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;

        // Create pipeline
        VkGraphicsPipelineCreateInfo pipelineInfo = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pipelineInfo.pNext = &renderingInfo;
        pipelineInfo.stageCount = static_cast<uint32>(shaderStages.size());
        pipelineInfo.pStages = shaderStages.data();
        pipelineInfo.pVertexInputState = &vertexInputInfo;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pDepthStencilState = &depthStencil;
        pipelineInfo.pColorBlendState = &colorBlending;
        pipelineInfo.pDynamicState = &dynamicState;
        pipelineInfo.layout = m_pipelineLayout;
        pipelineInfo.renderPass = VK_NULL_HANDLE;  // Using dynamic rendering
        pipelineInfo.subpass = 0;

        VkResult result = vkCreateGraphicsPipelines(m_device->GetDevice(), VK_NULL_HANDLE, 1, 
            &pipelineInfo, nullptr, &m_pipeline);
        
        if (result != VK_SUCCESS)
        {
            RVX_RHI_ERROR("Failed to create Vulkan graphics pipeline: {}", VkResultToString(result));
        }
    }

    void VulkanPipeline::CreateComputePipeline(const RHIComputePipelineDesc& desc)
    {
        if (desc.pipelineLayout)
        {
            m_pipelineLayout = static_cast<VulkanPipelineLayout*>(desc.pipelineLayout)->GetLayout();
        }

        VkPipelineShaderStageCreateInfo stageInfo = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        if (desc.computeShader)
        {
            auto* vkShader = static_cast<VulkanShader*>(desc.computeShader);
            stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            stageInfo.module = vkShader->GetShaderModule();
            stageInfo.pName = vkShader->GetEntryPoint();
        }

        VkComputePipelineCreateInfo pipelineInfo = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipelineInfo.stage = stageInfo;
        pipelineInfo.layout = m_pipelineLayout;

        VK_CHECK(vkCreateComputePipelines(m_device->GetDevice(), VK_NULL_HANDLE, 1,
            &pipelineInfo, nullptr, &m_pipeline));
    }

    // =============================================================================
    // Vulkan Descriptor Set
    // =============================================================================
    VulkanDescriptorSet::VulkanDescriptorSet(VulkanDevice* device, const RHIDescriptorSetDesc& desc)
        : m_device(device)
    {
        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }

        m_layoutWrapper = static_cast<VulkanDescriptorSetLayout*>(desc.layout);
        m_layout = m_layoutWrapper ? m_layoutWrapper->GetLayout() : VK_NULL_HANDLE;

        VkDescriptorSetAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocInfo.descriptorPool = device->GetDescriptorPool();
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &m_layout;

        VK_CHECK(vkAllocateDescriptorSets(device->GetDevice(), &allocInfo, &m_descriptorSet));

        // Initial update
        Update(desc.bindings);
    }

    VulkanDescriptorSet::~VulkanDescriptorSet()
    {
        if (m_descriptorSet)
        {
            vkFreeDescriptorSets(m_device->GetDevice(), m_device->GetDescriptorPool(), 1, &m_descriptorSet);
        }
    }

    void VulkanDescriptorSet::Update(const std::vector<RHIDescriptorBinding>& bindings)
    {
        std::vector<VkWriteDescriptorSet> writes;
        std::vector<VkDescriptorBufferInfo> bufferInfos;
        std::vector<VkDescriptorImageInfo> imageInfos;

        bufferInfos.reserve(bindings.size());
        imageInfos.reserve(bindings.size());

        for (const auto& binding : bindings)
        {
            const RHIBindingLayoutEntry* entry = m_layoutWrapper ? m_layoutWrapper->FindEntry(binding.binding) : nullptr;
            if (!entry)
            {
                RVX_RHI_WARN("VulkanDescriptorSet: binding {} not found in layout", binding.binding);
                continue;
            }

            VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = m_descriptorSet;
            write.dstBinding = binding.binding;
            write.dstArrayElement = 0;
            write.descriptorCount = 1;

            if (binding.buffer)
            {
                auto* vkBuffer = static_cast<VulkanBuffer*>(binding.buffer);
                
                VkDescriptorBufferInfo bufferInfo = {};
                bufferInfo.buffer = vkBuffer->GetBuffer();
                bufferInfo.offset = binding.offset;
                bufferInfo.range = binding.range == RVX_WHOLE_SIZE ? VK_WHOLE_SIZE : binding.range;
                bufferInfos.push_back(bufferInfo);

                switch (entry->type)
                {
                    case RHIBindingType::UniformBuffer:
                        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                        break;
                    case RHIBindingType::DynamicUniformBuffer:
                        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
                        break;
                    case RHIBindingType::StorageBuffer:
                        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                        break;
                    case RHIBindingType::DynamicStorageBuffer:
                        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
                        break;
                    default:
                        RVX_RHI_WARN("VulkanDescriptorSet: binding {} expects non-buffer type", binding.binding);
                        continue;
                }

                write.pBufferInfo = &bufferInfos.back();
            }
            else if (binding.textureView)
            {
                auto* vkView = static_cast<VulkanTextureView*>(binding.textureView);
                
                VkDescriptorImageInfo imageInfo = {};
                imageInfo.imageView = vkView->GetImageView();

                switch (entry->type)
                {
                    case RHIBindingType::SampledTexture:
                        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                        write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                        break;
                    case RHIBindingType::StorageTexture:
                        imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                        break;
                    case RHIBindingType::CombinedTextureSampler:
                    {
                        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                        if (binding.sampler)
                        {
                            auto* vkSampler = static_cast<VulkanSampler*>(binding.sampler);
                            imageInfo.sampler = vkSampler->GetSampler();
                        }
                        else
                        {
                            RVX_RHI_WARN("VulkanDescriptorSet: combined binding {} missing sampler", binding.binding);
                        }
                        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                        break;
                    }
                    default:
                        RVX_RHI_WARN("VulkanDescriptorSet: binding {} expects non-texture type", binding.binding);
                        continue;
                }
                
                imageInfos.push_back(imageInfo);
                write.pImageInfo = &imageInfos.back();
            }
            else if (binding.sampler)
            {
                if (entry->type != RHIBindingType::Sampler)
                {
                    RVX_RHI_WARN("VulkanDescriptorSet: binding {} expects non-sampler type", binding.binding);
                    continue;
                }

                auto* vkSampler = static_cast<VulkanSampler*>(binding.sampler);
                
                VkDescriptorImageInfo imageInfo = {};
                imageInfo.sampler = vkSampler->GetSampler();
                imageInfos.push_back(imageInfo);
                
                write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
                write.pImageInfo = &imageInfos.back();
            }
            else
            {
                continue;  // Skip empty bindings
            }

            writes.push_back(write);
        }

        if (!writes.empty())
        {
            vkUpdateDescriptorSets(m_device->GetDevice(), static_cast<uint32>(writes.size()),
                writes.data(), 0, nullptr);
        }
    }

    // =============================================================================
    // Factory Functions
    // =============================================================================
    RHIDescriptorSetLayoutRef CreateVulkanDescriptorSetLayout(VulkanDevice* device, const RHIDescriptorSetLayoutDesc& desc)
    {
        return Ref<VulkanDescriptorSetLayout>(new VulkanDescriptorSetLayout(device, desc));
    }

    RHIPipelineLayoutRef CreateVulkanPipelineLayout(VulkanDevice* device, const RHIPipelineLayoutDesc& desc)
    {
        return Ref<VulkanPipelineLayout>(new VulkanPipelineLayout(device, desc));
    }

    RHIPipelineRef CreateVulkanGraphicsPipeline(VulkanDevice* device, const RHIGraphicsPipelineDesc& desc)
    {
        return Ref<VulkanPipeline>(new VulkanPipeline(device, desc));
    }

    RHIPipelineRef CreateVulkanComputePipeline(VulkanDevice* device, const RHIComputePipelineDesc& desc)
    {
        return Ref<VulkanPipeline>(new VulkanPipeline(device, desc));
    }

    RHIDescriptorSetRef CreateVulkanDescriptorSet(VulkanDevice* device, const RHIDescriptorSetDesc& desc)
    {
        return Ref<VulkanDescriptorSet>(new VulkanDescriptorSet(device, desc));
    }

} // namespace RVX
