#include "VulkanPipeline.h"
#include "VulkanDevice.h"
#include "VulkanRayTracing.h"
#include "VulkanResources.h"
#include "RHI/RHIPipelineValidation.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <span>
#include <unordered_map>

namespace RVX
{
    namespace
    {
        bool TryAddUint64(uint64 lhs, uint64 rhs, uint64& result)
        {
            if (lhs > std::numeric_limits<uint64>::max() - rhs)
            {
                return false;
            }

            result = lhs + rhs;
            return true;
        }

        bool TryMultiplyUint64(uint64 lhs, uint64 rhs, uint64& result)
        {
            if (lhs != 0 && rhs > std::numeric_limits<uint64>::max() / lhs)
            {
                return false;
            }

            result = lhs * rhs;
            return true;
        }

        bool TryAlignUpUint64(uint64 value, uint64 alignment, uint64& result)
        {
            if (alignment == 0)
            {
                result = value;
                return true;
            }

            uint64 biasedValue = 0;
            if (!TryAddUint64(value, alignment - 1ull, biasedValue))
            {
                return false;
            }

            result = (biasedValue / alignment) * alignment;
            return true;
        }

        bool IsVulkanRayTracingPipelineStage(RHIShaderStage stage)
        {
            return stage == RHIShaderStage::RayGeneration ||
                   stage == RHIShaderStage::Miss ||
                   stage == RHIShaderStage::ClosestHit ||
                   stage == RHIShaderStage::AnyHit ||
                   stage == RHIShaderStage::Intersection ||
                   stage == RHIShaderStage::Callable;
        }
    }

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

        // Set debug name for RenderDoc/validation layers
        if (desc.debugName)
        {
            m_device->SetObjectName(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, reinterpret_cast<uint64>(m_layout), desc.debugName);
        }
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
        : RHIPipelineLayout(desc)
        , m_device(device)
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

        // Set debug name for RenderDoc/validation layers
        if (desc.debugName)
        {
            m_device->SetObjectName(VK_OBJECT_TYPE_PIPELINE_LAYOUT, reinterpret_cast<uint64>(m_layout), desc.debugName);
        }
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
        , m_descriptorPipelineLayout(static_cast<VulkanPipelineLayout*>(desc.pipelineLayout))
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
        , m_descriptorPipelineLayout(static_cast<VulkanPipelineLayout*>(desc.pipelineLayout))
        , m_isCompute(true)
    {
        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }
        CreateComputePipeline(desc);
    }

    VulkanPipeline::VulkanPipeline(VulkanDevice* device, const RHIRayTracingPipelineDesc& desc)
        : m_device(device)
        , m_descriptorPipelineLayout(static_cast<VulkanPipelineLayout*>(desc.pipelineLayout))
        , m_isRayTracing(true)
    {
        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }
        CreateRayTracingPipeline(desc);
    }

    VulkanPipeline::~VulkanPipeline()
    {
        if (m_pipeline)
        {
            vkDestroyPipeline(m_device->GetDevice(), m_pipeline, nullptr);
        }
    }

    uint32 VulkanPipeline::GetRayTracingShaderGroupCount() const
    {
        return static_cast<uint32>(m_rayTracingShaderGroupStages.size());
    }

    RHIShaderStage VulkanPipeline::GetRayTracingShaderGroupStage(uint32 shaderGroupIndex) const
    {
        return shaderGroupIndex < m_rayTracingShaderGroupStages.size()
            ? m_rayTracingShaderGroupStages[shaderGroupIndex]
            : RHIShaderStage::None;
    }

    bool VulkanPipeline::IsRayTracingHitGroup(uint32 shaderGroupIndex) const
    {
        return shaderGroupIndex < m_rayTracingShaderGroupIsHitGroup.size() &&
               m_rayTracingShaderGroupIsHitGroup[shaderGroupIndex] != 0;
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
        if (desc.hullShader)
        {
            auto* vkShader = static_cast<VulkanShader*>(desc.hullShader);
            VkPipelineShaderStageCreateInfo stageInfo = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            stageInfo.stage = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
            stageInfo.module = vkShader->GetShaderModule();
            stageInfo.pName = vkShader->GetEntryPoint();
            shaderStages.push_back(stageInfo);
        }
        if (desc.domainShader)
        {
            auto* vkShader = static_cast<VulkanShader*>(desc.domainShader);
            VkPipelineShaderStageCreateInfo stageInfo = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            stageInfo.stage = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
            stageInfo.module = vkShader->GetShaderModule();
            stageInfo.pName = vkShader->GetEntryPoint();
            shaderStages.push_back(stageInfo);
        }

        // Tessellation state (required when using tessellation shaders)
        VkPipelineTessellationStateCreateInfo tessellationState = {VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO};
        tessellationState.patchControlPoints = desc.tessellationControlPoints;
        bool hasTessellation = desc.hullShader != nullptr && desc.domainShader != nullptr;

        // Vertex input
        std::vector<VkVertexInputBindingDescription> bindingDescs;
        std::vector<VkVertexInputAttributeDescription> attributeDescs;
        const RHIVertexInputTranslation vertexInputTranslation =
            BuildRHIVertexInputTranslation(desc.inputLayout);
        attributeDescs.reserve(
            vertexInputTranslation.attributes.size());
        bindingDescs.reserve(
            vertexInputTranslation.bindings.size());

        for (const RHIVertexInputAttributeTranslation& attribute :
             vertexInputTranslation.attributes)
        {
            const RHIInputElement& elem =
                desc.inputLayout.elements[attribute.elementIndex];

            VkVertexInputAttributeDescription attr = {};
            attr.location = attribute.location;
            attr.binding = attribute.inputSlot;
            attr.format = ToVkFormat(elem.format);
            attr.offset = attribute.alignedByteOffset;
            attributeDescs.push_back(attr);
        }

        for (const RHIVertexInputBindingTranslation& translatedBinding :
             vertexInputTranslation.bindings)
        {
            VkVertexInputBindingDescription binding = {};
            binding.binding = translatedBinding.inputSlot;
            binding.stride = translatedBinding.stride;
            binding.inputRate = translatedBinding.perInstance
                ? VK_VERTEX_INPUT_RATE_INSTANCE
                : VK_VERTEX_INPUT_RATE_VERTEX;
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
        rasterizer.depthBiasEnable = (desc.rasterizerState.depthBias != 0.0f ||
                                      desc.rasterizerState.slopeScaledDepthBias != 0.0f ||
                                      desc.rasterizerState.depthBiasClamp != 0.0f);
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
            const uint32 blendStateIndex =
                SelectVulkanBlendAttachmentStateIndex(
                    desc.blendState.independentBlendEnable,
                    i);
            const auto& rtBlend =
                desc.blendState.renderTargets[blendStateIndex];
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
        // Set stencil format for depth-stencil formats (D24S8, D32S8)
        renderingInfo.stencilAttachmentFormat = IsStencilFormat(desc.depthStencilFormat) ?
            ToVkFormat(desc.depthStencilFormat) : VK_FORMAT_UNDEFINED;

        // Create pipeline
        VkGraphicsPipelineCreateInfo pipelineInfo = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pipelineInfo.pNext = &renderingInfo;
        pipelineInfo.stageCount = static_cast<uint32>(shaderStages.size());
        pipelineInfo.pStages = shaderStages.data();
        pipelineInfo.pVertexInputState = &vertexInputInfo;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pTessellationState = hasTessellation ? &tessellationState : nullptr;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pDepthStencilState = &depthStencil;
        pipelineInfo.pColorBlendState = &colorBlending;
        pipelineInfo.pDynamicState = &dynamicState;
        pipelineInfo.layout = m_pipelineLayout;
        pipelineInfo.renderPass = VK_NULL_HANDLE;  // Using dynamic rendering
        pipelineInfo.subpass = 0;

        VkResult result = vkCreateGraphicsPipelines(m_device->GetDevice(), m_device->GetPipelineCache(), 1,
            &pipelineInfo, nullptr, &m_pipeline);

        if (result != VK_SUCCESS)
        {
            RVX_RHI_ERROR("Failed to create Vulkan graphics pipeline: {}", VkResultToString(result));
        }

        // Set debug name for RenderDoc/validation layers
        if (desc.debugName && m_pipeline != VK_NULL_HANDLE)
        {
            m_device->SetObjectName(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<uint64>(m_pipeline), desc.debugName);
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

        VK_CHECK(vkCreateComputePipelines(m_device->GetDevice(), m_device->GetPipelineCache(), 1,
            &pipelineInfo, nullptr, &m_pipeline));

        // Set debug name for RenderDoc/validation layers
        if (desc.debugName)
        {
            m_device->SetObjectName(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<uint64>(m_pipeline), desc.debugName);
        }
    }

    void VulkanPipeline::CreateRayTracingPipeline(const RHIRayTracingPipelineDesc& desc)
    {
        m_rayTracingShaderGroupStages.clear();
        m_rayTracingShaderGroupIsHitGroup.clear();

        const RHIRayTracingValidationResult validation =
            ValidateRHIRayTracingPipelineDesc(desc);
        if (!validation)
        {
            RVX_RHI_ERROR("Vulkan ray tracing pipeline creation failed: {}", validation.message);
            return;
        }

        const RHICapabilities& capabilities = m_device->GetCapabilities();
        const VulkanRayTracingFunctions& functions = m_device->GetRayTracingFunctions();
        const VkPhysicalDeviceRayTracingPipelinePropertiesKHR& properties =
            m_device->GetRayTracingPipelineProperties();
        if (!capabilities.supportsRaytracingPipeline || !functions.IsComplete())
        {
            RVX_RHI_ERROR(
                "Vulkan ray tracing pipeline creation failed: the selected device has no enabled ray tracing pipeline path");
            return;
        }

        if (capabilities.maxRayRecursionDepth == 0 ||
            desc.maxRecursionDepth > capabilities.maxRayRecursionDepth ||
            desc.maxRecursionDepth > properties.maxRayRecursionDepth)
        {
            RVX_RHI_ERROR(
                "Vulkan ray tracing pipeline creation failed: recursion depth {} exceeds device limit {}",
                desc.maxRecursionDepth,
                capabilities.maxRayRecursionDepth);
            return;
        }

        // VK_KHR_ray_tracing_pipeline exposes an attribute-size limit but no
        // payload-size property. Payload validity is enforced by shader/pipeline
        // validation; do not invent a Vulkan limit from a DXR-only contract.
        if (properties.maxRayHitAttributeSize == 0 ||
            desc.maxAttributeSize > properties.maxRayHitAttributeSize)
        {
            RVX_RHI_ERROR(
                "Vulkan ray tracing pipeline creation failed: payload or attribute size exceeds enabled device limits");
            return;
        }

        m_pipelineLayout = m_descriptorPipelineLayout
            ? m_descriptorPipelineLayout->GetLayout()
            : VK_NULL_HANDLE;
        if (m_pipelineLayout == VK_NULL_HANDLE)
        {
            RVX_RHI_ERROR("Vulkan ray tracing pipeline creation failed: pipeline layout is unavailable");
            return;
        }

        std::vector<VkPipelineShaderStageCreateInfo> shaderStages;
        shaderStages.reserve(desc.shaderGroups.size() * 3u);
        std::unordered_map<RHIShader*, uint32> shaderStageIndices;
        shaderStageIndices.reserve(desc.shaderGroups.size() * 3u);

        auto addShaderStage = [&](RHIShader* shader, uint32& outStageIndex) -> bool
        {
            if (!shader || !IsVulkanRayTracingPipelineStage(shader->GetStage()))
            {
                return false;
            }

            const auto existing = shaderStageIndices.find(shader);
            if (existing != shaderStageIndices.end())
            {
                outStageIndex = existing->second;
                return true;
            }

            auto* vulkanShader = static_cast<VulkanShader*>(shader);
            if (!vulkanShader || vulkanShader->GetShaderModule() == VK_NULL_HANDLE ||
                !vulkanShader->GetEntryPoint() || vulkanShader->GetEntryPoint()[0] == '\0')
            {
                return false;
            }

            VkPipelineShaderStageCreateInfo stageInfo = {
                VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            stageInfo.stage = static_cast<VkShaderStageFlagBits>(
                ToVkShaderStageFlags(shader->GetStage()));
            stageInfo.module = vulkanShader->GetShaderModule();
            stageInfo.pName = vulkanShader->GetEntryPoint();
            if (stageInfo.stage == 0)
            {
                return false;
            }

            outStageIndex = static_cast<uint32>(shaderStages.size());
            shaderStages.push_back(stageInfo);
            shaderStageIndices.emplace(shader, outStageIndex);
            return true;
        };

        constexpr uint32 unusedShader = VK_SHADER_UNUSED_KHR;
        std::vector<VkRayTracingShaderGroupCreateInfoKHR> shaderGroups;
        shaderGroups.reserve(desc.shaderGroups.size());
        std::vector<RHIShaderStage> shaderGroupStages;
        shaderGroupStages.reserve(desc.shaderGroups.size());
        std::vector<uint8> shaderGroupIsHitGroup;
        shaderGroupIsHitGroup.reserve(desc.shaderGroups.size());

        for (uint32 groupIndex = 0;
             groupIndex < static_cast<uint32>(desc.shaderGroups.size());
             ++groupIndex)
        {
            const RHIRayTracingShaderGroupDesc& group = desc.shaderGroups[groupIndex];
            VkRayTracingShaderGroupCreateInfoKHR nativeGroup = {
                VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR};
            nativeGroup.generalShader = unusedShader;
            nativeGroup.closestHitShader = unusedShader;
            nativeGroup.anyHitShader = unusedShader;
            nativeGroup.intersectionShader = unusedShader;

            if (group.type == RHIRayTracingShaderGroupType::General)
            {
                uint32 generalShader = unusedShader;
                if (!addShaderStage(group.generalShader, generalShader))
                {
                    RVX_RHI_ERROR(
                        "Vulkan ray tracing pipeline creation failed: general shader group {} is invalid",
                        groupIndex);
                    return;
                }

                nativeGroup.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
                nativeGroup.generalShader = generalShader;
                shaderGroupStages.push_back(group.generalShader->GetStage());
                shaderGroupIsHitGroup.push_back(0);
            }
            else
            {
                nativeGroup.type = group.type == RHIRayTracingShaderGroupType::TrianglesHitGroup
                    ? VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR
                    : VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR;

                auto addOptionalStage = [&](RHIShader* shader, uint32& destination) -> bool
                {
                    if (!shader)
                    {
                        destination = unusedShader;
                        return true;
                    }
                    return addShaderStage(shader, destination);
                };

                if (!addOptionalStage(group.closestHitShader, nativeGroup.closestHitShader) ||
                    !addOptionalStage(group.anyHitShader, nativeGroup.anyHitShader) ||
                    !addOptionalStage(group.intersectionShader, nativeGroup.intersectionShader))
                {
                    RVX_RHI_ERROR(
                        "Vulkan ray tracing pipeline creation failed: hit shader group {} is invalid",
                        groupIndex);
                    return;
                }

                shaderGroupStages.push_back(RHIShaderStage::None);
                shaderGroupIsHitGroup.push_back(1);
            }

            shaderGroups.push_back(nativeGroup);
        }

        VkRayTracingPipelineCreateInfoKHR pipelineInfo = {
            VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
        pipelineInfo.stageCount = static_cast<uint32>(shaderStages.size());
        pipelineInfo.pStages = shaderStages.data();
        pipelineInfo.groupCount = static_cast<uint32>(shaderGroups.size());
        pipelineInfo.pGroups = shaderGroups.data();
        pipelineInfo.maxPipelineRayRecursionDepth = desc.maxRecursionDepth;
        pipelineInfo.layout = m_pipelineLayout;

        const VkResult result = functions.createRayTracingPipelines(
            m_device->GetDevice(),
            VK_NULL_HANDLE,
            m_device->GetPipelineCache(),
            1,
            &pipelineInfo,
            nullptr,
            &m_pipeline);
        if (result != VK_SUCCESS)
        {
            RVX_RHI_ERROR("Failed to create Vulkan ray tracing pipeline: {}", VkResultToString(result));
            return;
        }

        m_rayTracingShaderGroupStages = std::move(shaderGroupStages);
        m_rayTracingShaderGroupIsHitGroup = std::move(shaderGroupIsHitGroup);

        if (desc.debugName)
        {
            m_device->SetObjectName(
                VK_OBJECT_TYPE_PIPELINE,
                reinterpret_cast<uint64>(m_pipeline),
                desc.debugName);
        }
    }

    // =============================================================================
    // Vulkan Shader Table
    // =============================================================================
    VulkanShaderTable::VulkanShaderTable(
        VulkanDevice* device,
        const RHIShaderTableDesc& desc)
        : m_device(device)
    {
        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }

        Create(desc);
    }

    bool VulkanShaderTable::IsValid() const
    {
        const auto* buffer = static_cast<const VulkanBuffer*>(m_buffer.Get());
        if (!m_device || !m_pipeline || !m_pipeline->IsRayTracing() ||
            !m_pipeline->GetPipeline() || !buffer ||
            buffer->GetBuffer() == VK_NULL_HANDLE)
        {
            return false;
        }

        const VkDeviceAddress bufferAddress = buffer->GetDeviceAddress();
        const VkPhysicalDeviceRayTracingPipelinePropertiesKHR& properties =
            m_device->GetRayTracingPipelineProperties();
        const uint64 recordAlignment = properties.shaderGroupHandleAlignment;
        const uint64 tableAlignment = properties.shaderGroupBaseAlignment;
        if (bufferAddress == 0 || recordAlignment == 0 || tableAlignment == 0 ||
            properties.shaderGroupHandleSize == 0 ||
            properties.maxShaderGroupStride == 0)
        {
            return false;
        }

        const uint64 bufferSize = buffer->GetSize();
        auto sectionRangeValid = [&](const Section& section, bool required) -> bool
        {
            if (section.count == 0)
            {
                return !required && section.offset == 0 && section.size == 0 &&
                       section.stride == 0;
            }

            if (section.size == 0 || section.stride == 0 ||
                section.stride > properties.maxShaderGroupStride ||
                section.stride < properties.shaderGroupHandleSize ||
                section.stride % recordAlignment != 0)
            {
                return false;
            }

            uint64 expectedSize = 0;
            if (!TryMultiplyUint64(section.stride, section.count, expectedSize) ||
                expectedSize != section.size)
            {
                return false;
            }

            uint64 sectionEnd = 0;
            uint64 sectionAddress = 0;
            return TryAddUint64(section.offset, section.size, sectionEnd) &&
                   sectionEnd <= bufferSize &&
                   TryAddUint64(bufferAddress, section.offset, sectionAddress) &&
                   sectionAddress % tableAlignment == 0;
        };

        return sectionRangeValid(m_rayGenerationSection, true) &&
               m_rayGenerationSection.count == 1 &&
               sectionRangeValid(m_missSection, false) &&
               sectionRangeValid(m_hitGroupSection, false) &&
               sectionRangeValid(m_callableSection, false);
    }

    bool VulkanShaderTable::Create(const RHIShaderTableDesc& desc)
    {
        m_pipeline = nullptr;
        m_pipelineOwner.Reset();
        m_buffer.Reset();
        m_rayGenerationSection = {};
        m_missSection = {};
        m_hitGroupSection = {};
        m_callableSection = {};

        const RHIRayTracingValidationResult validation =
            ValidateRHIShaderTableDesc(desc);
        if (!validation)
        {
            RVX_RHI_ERROR("Vulkan shader table creation failed: {}", validation.message);
            return false;
        }

        auto* pipeline = static_cast<VulkanPipeline*>(desc.GetRayTracingPipeline());
        if (!pipeline || !pipeline->IsRayTracing() ||
            pipeline->GetPipeline() == VK_NULL_HANDLE)
        {
            RVX_RHI_ERROR("Vulkan shader table creation failed: invalid ray tracing pipeline");
            return false;
        }

        const VulkanRayTracingFunctions& functions =
            m_device->GetRayTracingFunctions();
        const VkPhysicalDeviceRayTracingPipelinePropertiesKHR& properties =
            m_device->GetRayTracingPipelineProperties();
        const uint64 handleSize = properties.shaderGroupHandleSize;
        const uint64 recordAlignment = properties.shaderGroupHandleAlignment;
        const uint64 tableAlignment = properties.shaderGroupBaseAlignment;
        if (!m_device->GetCapabilities().supportsRaytracingPipeline ||
            !functions.IsComplete() || handleSize == 0 || recordAlignment == 0 ||
            tableAlignment == 0 || properties.maxShaderGroupStride == 0)
        {
            RVX_RHI_ERROR(
                "Vulkan shader table creation failed: enabled ray tracing shader table capabilities are incomplete");
            return false;
        }

        auto computeSection = [&](std::span<const RHIShaderTableRecord> records,
                                  uint64& cursor,
                                  const char* sectionName,
                                  Section& section) -> bool
        {
            section = {};
            if (records.size() > std::numeric_limits<uint32>::max())
            {
                RVX_RHI_ERROR(
                    "Vulkan shader table creation failed: {} record count exceeds the RHI limit",
                    sectionName);
                return false;
            }

            section.count = static_cast<uint32>(records.size());
            if (records.empty())
            {
                return true;
            }

            uint64 maxRecordSize = handleSize;
            for (const RHIShaderTableRecord& record : records)
            {
                uint64 recordSize = 0;
                if (!TryAddUint64(handleSize, record.localRootDataSize, recordSize))
                {
                    RVX_RHI_ERROR(
                        "Vulkan shader table creation failed: {} record size overflowed",
                        sectionName);
                    return false;
                }
                maxRecordSize = std::max(maxRecordSize, recordSize);
            }

            if (!TryAlignUpUint64(cursor, tableAlignment, section.offset) ||
                !TryAlignUpUint64(maxRecordSize, recordAlignment, section.stride))
            {
                RVX_RHI_ERROR(
                    "Vulkan shader table creation failed: {} alignment overflowed",
                    sectionName);
                return false;
            }

            if (section.stride > properties.maxShaderGroupStride)
            {
                RVX_RHI_ERROR(
                    "Vulkan shader table creation failed: {} stride {} exceeds device limit {}",
                    sectionName,
                    section.stride,
                    properties.maxShaderGroupStride);
                return false;
            }

            if (!TryMultiplyUint64(section.stride, section.count, section.size))
            {
                RVX_RHI_ERROR(
                    "Vulkan shader table creation failed: {} section size overflowed",
                    sectionName);
                return false;
            }

            uint64 alignedSectionSize = 0;
            if (!TryAlignUpUint64(section.size, tableAlignment, alignedSectionSize) ||
                !TryAddUint64(section.offset, alignedSectionSize, cursor))
            {
                RVX_RHI_ERROR(
                    "Vulkan shader table creation failed: {} section cursor overflowed",
                    sectionName);
                return false;
            }
            return true;
        };

        uint64 tableSize = 0;
        if (!computeSection(desc.rayGenerationRecords, tableSize, "ray generation", m_rayGenerationSection) ||
            !computeSection(desc.missRecords, tableSize, "miss", m_missSection) ||
            !computeSection(desc.hitGroupRecords, tableSize, "hit group", m_hitGroupSection) ||
            !computeSection(desc.callableRecords, tableSize, "callable", m_callableSection) ||
            !TryAlignUpUint64(tableSize, tableAlignment, tableSize) || tableSize == 0)
        {
            RVX_RHI_ERROR("Vulkan shader table creation failed: invalid shader table layout");
            return false;
        }

        uint64 allocationSize = 0;
        if (!TryAddUint64(tableSize, tableAlignment - 1ull, allocationSize))
        {
            RVX_RHI_ERROR("Vulkan shader table creation failed: shader table allocation size overflowed");
            return false;
        }
        if (allocationSize > std::numeric_limits<size_t>::max())
        {
            RVX_RHI_ERROR("Vulkan shader table creation failed: shader table allocation is too large for host addressing");
            return false;
        }

        RHIBufferDesc bufferDesc;
        bufferDesc.size = allocationSize;
        bufferDesc.usage = RHIBufferUsage::ShaderBindingTable |
                           RHIBufferUsage::DeviceAddress;
        bufferDesc.memoryType = RHIMemoryType::Upload;
        bufferDesc.stride = 1;
        bufferDesc.debugName = desc.debugName ? desc.debugName : "VulkanShaderTable";
        m_buffer = m_device->CreateBuffer(bufferDesc);
        auto* buffer = static_cast<VulkanBuffer*>(m_buffer.Get());
        if (!buffer || buffer->GetBuffer() == VK_NULL_HANDLE ||
            buffer->GetDeviceAddress() == 0)
        {
            RVX_RHI_ERROR(
                "Vulkan shader table creation failed: shader table buffer must be GPU-addressable");
            m_buffer.Reset();
            return false;
        }

        uint64 tableBaseOffset = 0;
        if (!TryAlignUpUint64(buffer->GetDeviceAddress(), tableAlignment, tableBaseOffset))
        {
            RVX_RHI_ERROR("Vulkan shader table creation failed: shader table base address alignment overflowed");
            m_buffer.Reset();
            return false;
        }
        tableBaseOffset -= buffer->GetDeviceAddress();
        if (tableBaseOffset > allocationSize || tableSize > allocationSize - tableBaseOffset)
        {
            RVX_RHI_ERROR("Vulkan shader table creation failed: shader table buffer cannot satisfy base alignment");
            m_buffer.Reset();
            return false;
        }

        auto offsetSection = [tableBaseOffset](Section& section)
        {
            if (section.count != 0)
            {
                section.offset += tableBaseOffset;
            }
        };
        offsetSection(m_rayGenerationSection);
        offsetSection(m_missSection);
        offsetSection(m_hitGroupSection);
        offsetSection(m_callableSection);

        auto* mapped = static_cast<uint8*>(m_buffer->Map());
        if (!mapped)
        {
            RVX_RHI_ERROR("Vulkan shader table creation failed: shader table buffer mapping failed");
            m_buffer.Reset();
            return false;
        }
        std::memset(mapped, 0, static_cast<size_t>(allocationSize));

        std::vector<uint8> shaderGroupHandles;
        uint64 shaderGroupHandleBytes = 0;
        const uint32 shaderGroupCount = pipeline->GetRayTracingShaderGroupCount();
        if (shaderGroupCount == 0 ||
            !TryMultiplyUint64(shaderGroupCount, handleSize, shaderGroupHandleBytes) ||
            shaderGroupHandleBytes > std::numeric_limits<size_t>::max())
        {
            RVX_RHI_ERROR("Vulkan shader table creation failed: shader group handle storage overflowed");
            m_buffer.Reset();
            return false;
        }
        shaderGroupHandles.resize(static_cast<size_t>(shaderGroupHandleBytes));
        const VkResult handlesResult = functions.getRayTracingShaderGroupHandles(
            m_device->GetDevice(),
            pipeline->GetPipeline(),
            0,
            shaderGroupCount,
            static_cast<size_t>(shaderGroupHandleBytes),
            shaderGroupHandles.data());
        if (handlesResult != VK_SUCCESS)
        {
            RVX_RHI_ERROR(
                "Vulkan shader table creation failed: shader group handle query failed: {}",
                VkResultToString(handlesResult));
            m_buffer.Reset();
            return false;
        }

        auto writeSection = [&](const Section& section,
                                std::span<const RHIShaderTableRecord> records) -> bool
        {
            for (uint32 recordIndex = 0; recordIndex < section.count; ++recordIndex)
            {
                const RHIShaderTableRecord& record = records[recordIndex];
                if (record.shaderGroupIndex >= shaderGroupCount)
                {
                    RVX_RHI_ERROR(
                        "Vulkan shader table creation failed: shader group index {} is out of range",
                        record.shaderGroupIndex);
                    return false;
                }

                uint8* destination = mapped + section.offset +
                    static_cast<uint64>(recordIndex) * section.stride;
                const uint8* handle = shaderGroupHandles.data() +
                    static_cast<size_t>(record.shaderGroupIndex) *
                        static_cast<size_t>(handleSize);
                std::memcpy(destination, handle, static_cast<size_t>(handleSize));
                if (record.localRootData && record.localRootDataSize > 0)
                {
                    std::memcpy(
                        destination + handleSize,
                        record.localRootData,
                        record.localRootDataSize);
                }
            }
            return true;
        };

        const bool wroteAll =
            writeSection(m_rayGenerationSection, desc.rayGenerationRecords) &&
            writeSection(m_missSection, desc.missRecords) &&
            writeSection(m_hitGroupSection, desc.hitGroupRecords) &&
            writeSection(m_callableSection, desc.callableRecords);
        if (!wroteAll || !m_buffer->CommitMappedWrite())
        {
            RVX_RHI_ERROR("Vulkan shader table creation failed: shader table upload could not be published");
            m_buffer.Reset();
            return false;
        }

        // The raw pointer form is a legacy call convenience. Retain a real
        // intrusive reference either way so command recording can never outlive
        // the pipeline that produced this table's opaque shader handles.
        m_pipelineOwner = desc.rayTracingPipelineOwner
            ? desc.rayTracingPipelineOwner
            : RHIPipelineRef(desc.GetRayTracingPipeline());
        m_pipeline = pipeline;
        if (!IsValid())
        {
            RVX_RHI_ERROR("Vulkan shader table creation failed: internal layout validation failed");
            m_pipeline = nullptr;
            m_pipelineOwner.Reset();
            m_buffer.Reset();
            return false;
        }
        return true;
    }

    VulkanRayTracingDispatchRegions VulkanShaderTable::BuildDispatchRaysRegions(
        uint32 width,
        uint32 height,
        uint32 depth) const
    {
        VulkanRayTracingDispatchRegions regions;
        if (width == 0 || height == 0 || depth == 0 || !IsValid())
        {
            return regions;
        }

        const auto* buffer = static_cast<const VulkanBuffer*>(m_buffer.Get());
        const VkDeviceAddress baseAddress = buffer->GetDeviceAddress();
        auto makeRegion = [baseAddress](const Section& section)
        {
            VkStridedDeviceAddressRegionKHR region = {};
            if (section.count != 0)
            {
                region.deviceAddress = baseAddress + section.offset;
                region.stride = section.stride;
                region.size = section.size;
            }
            return region;
        };

        regions.rayGeneration = makeRegion(m_rayGenerationSection);
        regions.miss = makeRegion(m_missSection);
        regions.hitGroup = makeRegion(m_hitGroupSection);
        regions.callable = makeRegion(m_callableSection);
        return regions;
    }

    // =============================================================================
    // Vulkan Descriptor Set
    // =============================================================================
    VulkanDescriptorSet::VulkanDescriptorSet(VulkanDevice* device, const RHIDescriptorSetDesc& desc)
        : RHIDescriptorSet(desc)
        , m_device(device)
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

        // Set debug name for RenderDoc/validation layers
        if (desc.debugName)
        {
            m_device->SetObjectName(VK_OBJECT_TYPE_DESCRIPTOR_SET, reinterpret_cast<uint64>(m_descriptorSet), desc.debugName);
        }

        if (!IsReadyForBinding() || !InitializeNativeSnapshot())
        {
            InvalidateDescriptorSnapshot();
        }
    }

    VulkanDescriptorSet::~VulkanDescriptorSet()
    {
        if (m_descriptorSet)
        {
            vkFreeDescriptorSets(m_device->GetDevice(), m_device->GetDescriptorPool(), 1, &m_descriptorSet);
        }
    }

    bool VulkanDescriptorSet::InitializeNativeSnapshot()
    {
        if (!m_layoutWrapper || m_descriptorSet == VK_NULL_HANDLE)
        {
            RVX_RHI_ERROR("VulkanDescriptorSet initialization failed: descriptor set has no valid layout");
            return false;
        }

        const auto& bindings = GetDescriptorSnapshot();

        std::vector<VkWriteDescriptorSet> writes;
        std::vector<VkDescriptorBufferInfo> bufferInfos;
        std::vector<VkDescriptorImageInfo> imageInfos;
        std::vector<VkWriteDescriptorSetAccelerationStructureKHR>
            accelerationStructureInfos;
        std::vector<VkAccelerationStructureKHR> accelerationStructures;

        writes.reserve(bindings.size());
        bufferInfos.reserve(bindings.size());
        imageInfos.reserve(bindings.size());
        // Each RHI descriptor binding has exactly one native AS. Reserving both
        // vectors before creating pNext links keeps Vulkan's borrowed pointers
        // stable until vkUpdateDescriptorSets consumes the complete write list.
        accelerationStructureInfos.reserve(bindings.size());
        accelerationStructures.reserve(bindings.size());

        for (const auto& binding : bindings)
        {
            const RHIBindingLayoutEntry* entry = m_layoutWrapper ? m_layoutWrapper->FindEntry(binding.binding) : nullptr;
            if (!entry)
            {
                RVX_RHI_ERROR("VulkanDescriptorSet: binding {} not found in layout", binding.binding);
                return false;
            }

            VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = m_descriptorSet;
            write.dstBinding = binding.binding;
            write.dstArrayElement = binding.arrayElement;
            write.descriptorCount = 1;

            if (binding.buffer)
            {
                auto* vkBuffer = static_cast<VulkanBuffer*>(binding.buffer);
                if (vkBuffer->GetBuffer() == VK_NULL_HANDLE)
                {
                    RVX_RHI_ERROR("VulkanDescriptorSet: buffer binding {} has no native buffer", binding.binding);
                    return false;
                }

                const uint64 bufferSize = vkBuffer->GetSize();
                if (binding.offset >= bufferSize)
                {
                    RVX_RHI_ERROR(
                        "VulkanDescriptorSet: buffer binding {} offset {} is outside buffer size {}",
                        binding.binding,
                        binding.offset,
                        bufferSize);
                    return false;
                }

                if (binding.range != RVX_WHOLE_SIZE &&
                    (binding.range == 0 ||
                     binding.range > bufferSize - binding.offset))
                {
                    RVX_RHI_ERROR(
                        "VulkanDescriptorSet: buffer binding {} range {} at offset {} exceeds buffer size {}",
                        binding.binding,
                        binding.range,
                        binding.offset,
                        bufferSize);
                    return false;
                }

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
                    case RHIBindingType::ShaderResourceBuffer:
                        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                        break;
                    case RHIBindingType::StorageBuffer:
                        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                        break;
                    case RHIBindingType::DynamicStorageBuffer:
                        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
                        break;
                    default:
                        RVX_RHI_ERROR("VulkanDescriptorSet: binding {} expects non-buffer type", binding.binding);
                        return false;
                }

                write.pBufferInfo = &bufferInfos.back();
            }
            else if (binding.textureView)
            {
                auto* vkView = static_cast<VulkanTextureView*>(binding.textureView);
                if (vkView->GetImageView() == VK_NULL_HANDLE)
                {
                    RVX_RHI_ERROR("VulkanDescriptorSet: texture binding {} has no native image view", binding.binding);
                    return false;
                }

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
                            if (vkSampler->GetSampler() == VK_NULL_HANDLE)
                            {
                                RVX_RHI_ERROR("VulkanDescriptorSet: combined binding {} has no native sampler", binding.binding);
                                return false;
                            }
                            imageInfo.sampler = vkSampler->GetSampler();
                        }
                        else
                        {
                            RVX_RHI_ERROR("VulkanDescriptorSet: combined binding {} missing sampler", binding.binding);
                            return false;
                        }
                        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                        break;
                    }
                    default:
                        RVX_RHI_ERROR("VulkanDescriptorSet: binding {} expects non-texture type", binding.binding);
                        return false;
                }

                imageInfos.push_back(imageInfo);
                write.pImageInfo = &imageInfos.back();
            }
            else if (binding.sampler)
            {
                if (entry->type != RHIBindingType::Sampler)
                {
                    RVX_RHI_ERROR("VulkanDescriptorSet: binding {} expects non-sampler type", binding.binding);
                    return false;
                }

                auto* vkSampler = static_cast<VulkanSampler*>(binding.sampler);
                if (vkSampler->GetSampler() == VK_NULL_HANDLE)
                {
                    RVX_RHI_ERROR("VulkanDescriptorSet: sampler binding {} has no native sampler", binding.binding);
                    return false;
                }

                VkDescriptorImageInfo imageInfo = {};
                imageInfo.sampler = vkSampler->GetSampler();
                imageInfos.push_back(imageInfo);

                write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
                write.pImageInfo = &imageInfos.back();
            }
            else if (binding.accelerationStructure)
            {
                if (entry->type != RHIBindingType::AccelerationStructure)
                {
                    RVX_RHI_ERROR(
                        "VulkanDescriptorSet: binding {} expects a non-acceleration-structure type",
                        binding.binding);
                    return false;
                }

                if (!m_device->GetCapabilities().supportsRaytracingPipeline)
                {
                    RVX_RHI_ERROR(
                        "VulkanDescriptorSet: acceleration structure binding {} requires enabled Vulkan ray tracing support",
                        binding.binding);
                    return false;
                }

                auto* accelerationStructure =
                    dynamic_cast<VulkanAccelerationStructure*>(
                        binding.accelerationStructure);
                if (!accelerationStructure ||
                    accelerationStructure->GetType() !=
                        RHIAccelerationStructureType::TopLevel ||
                    accelerationStructure->GetAccelerationStructure() == VK_NULL_HANDLE ||
                    accelerationStructure->GetDeviceAddress() == 0)
                {
                    RVX_RHI_ERROR(
                        "VulkanDescriptorSet: acceleration structure binding {} requires a live native top-level structure",
                        binding.binding);
                    return false;
                }

                accelerationStructures.push_back(
                    accelerationStructure->GetAccelerationStructure());
                VkWriteDescriptorSetAccelerationStructureKHR info = {
                    VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
                info.accelerationStructureCount = 1;
                info.pAccelerationStructures = &accelerationStructures.back();
                accelerationStructureInfos.push_back(info);

                write.descriptorType =
                    VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
                write.pNext = &accelerationStructureInfos.back();
            }
            else
            {
                RVX_RHI_ERROR("VulkanDescriptorSet: binding {} has no native resource", binding.binding);
                return false;
            }

            writes.push_back(write);
        }

        if (writes.size() != bindings.size())
        {
            RVX_RHI_ERROR("VulkanDescriptorSet: native snapshot is incomplete");
            return false;
        }

        if (!writes.empty())
        {
            vkUpdateDescriptorSets(m_device->GetDevice(), static_cast<uint32>(writes.size()),
                writes.data(), 0, nullptr);
        }
        return true;
    }

    // =============================================================================
    // Factory Functions
    // =============================================================================
    RHIDescriptorSetLayoutRef CreateVulkanDescriptorSetLayout(VulkanDevice* device, const RHIDescriptorSetLayoutDesc& desc)
    {
        auto validation = ValidateRHIDescriptorSetLayoutCapabilities(
            desc,
            device->GetCapabilities());
        if (!validation)
        {
            RVX_RHI_ERROR("Vulkan descriptor set layout creation failed: {} (binding {})",
                          validation.message,
                          validation.binding);
            return nullptr;
        }
        return Ref<VulkanDescriptorSetLayout>(new VulkanDescriptorSetLayout(device, desc));
    }

    RHIPipelineLayoutRef CreateVulkanPipelineLayout(VulkanDevice* device, const RHIPipelineLayoutDesc& desc)
    {
        auto validation = ValidateRHIPipelineLayoutDesc(desc);
        if (!validation)
        {
            RVX_RHI_ERROR("Vulkan pipeline layout creation failed: {}", validation.message);
            return nullptr;
        }
        return Ref<VulkanPipelineLayout>(new VulkanPipelineLayout(device, desc));
    }

    RHIPipelineRef CreateVulkanGraphicsPipeline(VulkanDevice* device, const RHIGraphicsPipelineDesc& desc)
    {
        Ref<VulkanPipeline> pipeline(new VulkanPipeline(device, desc));
        if (pipeline->GetPipeline() == VK_NULL_HANDLE)
        {
            return nullptr;
        }
        return pipeline;
    }

    RHIPipelineRef CreateVulkanComputePipeline(VulkanDevice* device, const RHIComputePipelineDesc& desc)
    {
        Ref<VulkanPipeline> pipeline(new VulkanPipeline(device, desc));
        if (pipeline->GetPipeline() == VK_NULL_HANDLE)
        {
            return nullptr;
        }
        return pipeline;
    }

    RHIPipelineRef CreateVulkanRayTracingPipeline(
        VulkanDevice* device,
        const RHIRayTracingPipelineDesc& desc)
    {
        Ref<VulkanPipeline> pipeline(new VulkanPipeline(device, desc));
        if (pipeline->GetPipeline() == VK_NULL_HANDLE || !pipeline->IsRayTracing())
        {
            RVX_RHI_ERROR("Vulkan ray tracing pipeline creation failed (invalid pipeline state)");
            return nullptr;
        }
        return pipeline;
    }

    RHIShaderTableRef CreateVulkanShaderTable(
        VulkanDevice* device,
        const RHIShaderTableDesc& desc)
    {
        Ref<VulkanShaderTable> shaderTable(new VulkanShaderTable(device, desc));
        if (!shaderTable->IsValid())
        {
            RVX_RHI_ERROR("Vulkan shader table creation failed (invalid shader table)");
            return nullptr;
        }
        return shaderTable;
    }

    RHIDescriptorSetRef CreateVulkanDescriptorSet(VulkanDevice* device, const RHIDescriptorSetDesc& desc)
    {
        auto validation = ValidateRHIDescriptorSetDesc(desc);
        if (!validation)
        {
            RVX_RHI_ERROR("Vulkan descriptor set creation failed: {} (binding {})",
                          validation.message,
                          validation.binding);
            return nullptr;
        }
        auto descriptorSet = Ref<VulkanDescriptorSet>(new VulkanDescriptorSet(device, desc));
        if (!descriptorSet->IsReadyForBinding())
        {
            RVX_RHI_ERROR("Vulkan descriptor set creation failed: native snapshot initialization failed");
            return nullptr;
        }
        return descriptorSet;
    }

} // namespace RVX
