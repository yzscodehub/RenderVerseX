#include "OpenGLPipeline.h"
#include "OpenGLDevice.h"
#include "Core/Log.h"
#include <algorithm>

namespace RVX
{
    // =============================================================================
    // OpenGLPushConstantBuffer Implementation
    // =============================================================================

    OpenGLPushConstantBuffer::OpenGLPushConstantBuffer()
    {
        GL_CHECK(glCreateBuffers(1, &m_buffer));
        
        if (m_buffer != 0)
        {
            GL_CHECK(glNamedBufferStorage(m_buffer, MAX_PUSH_CONSTANT_SIZE, nullptr,
                                         GL_DYNAMIC_STORAGE_BIT | GL_MAP_WRITE_BIT));
            
            OpenGLDebug::Get().SetBufferLabel(m_buffer, "PushConstantBuffer");
            GL_DEBUG_TRACK(m_buffer, GLResourceType::Buffer, "PushConstantBuffer");
            
            RVX_RHI_DEBUG("Created Push Constant Buffer #{}", m_buffer);
        }
    }

    OpenGLPushConstantBuffer::~OpenGLPushConstantBuffer()
    {
        if (m_buffer != 0)
        {
            glDeleteBuffers(1, &m_buffer);
            GL_DEBUG_UNTRACK(m_buffer, GLResourceType::Buffer);
            m_buffer = 0;
        }
    }

    void OpenGLPushConstantBuffer::Update(const void* data, uint32 size, uint32 offset)
    {
        if (offset + size > MAX_PUSH_CONSTANT_SIZE)
        {
            RVX_RHI_ERROR("Push constant update exceeds maximum size: offset={}, size={}, max={}",
                         offset, size, MAX_PUSH_CONSTANT_SIZE);
            return;
        }

        GL_CHECK(glNamedBufferSubData(m_buffer, offset, size, data));
    }

    void OpenGLPushConstantBuffer::Bind()
    {
        GL_CHECK(glBindBufferBase(GL_UNIFORM_BUFFER, PUSH_CONSTANT_BINDING, m_buffer));
    }

    // =============================================================================
    // OpenGLDescriptorSetLayout Implementation
    // =============================================================================

    OpenGLDescriptorSetLayout::OpenGLDescriptorSetLayout(OpenGLDevice* device, const RHIDescriptorSetLayoutDesc& desc)
        : m_device(device)
        , m_desc(desc)
    {
        // Build local binding points per resource type. OpenGLPipelineLayout
        // assigns global set base offsets once the layout order is known.
        for (const auto& entry : desc.entries)
        {
            BindingInfo info;
            info.type = entry.type;
            const uint32 count = std::max(entry.count, 1u);

            switch (entry.type)
            {
                case RHIBindingType::UniformBuffer:
                case RHIBindingType::DynamicUniformBuffer:
                    info.glBinding = m_bindingCounts.uniformBuffer;
                    m_bindingCounts.uniformBuffer += count;
                    break;
                case RHIBindingType::ShaderResourceBuffer:
                case RHIBindingType::StorageBuffer:
                case RHIBindingType::DynamicStorageBuffer:
                    info.glBinding = m_bindingCounts.storageBuffer;
                    m_bindingCounts.storageBuffer += count;
                    break;
                case RHIBindingType::SampledTexture:
                case RHIBindingType::CombinedTextureSampler:
                    info.glBinding = m_bindingCounts.texture;
                    m_bindingCounts.texture += count;
                    break;
                case RHIBindingType::Sampler:
                    info.glBinding = m_bindingCounts.sampler;
                    m_bindingCounts.sampler += count;
                    break;
                case RHIBindingType::StorageTexture:
                    info.glBinding = m_bindingCounts.image;
                    m_bindingCounts.image += count;
                    break;
            }

            m_bindingMap[entry.binding] = info;
        }

        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }

        RVX_RHI_DEBUG("Created DescriptorSetLayout '{}' with {} bindings",
                     GetDebugName(), desc.entries.size());
    }

    uint32 OpenGLDescriptorSetLayout::GetGLBinding(uint32 rhiBinding, RHIBindingType type) const
    {
        (void)type;

        auto it = m_bindingMap.find(rhiBinding);
        if (it != m_bindingMap.end())
        {
            return it->second.glBinding;
        }
        
        RVX_RHI_WARN("Binding {} not found in descriptor set layout '{}'", rhiBinding, GetDebugName());
        return UINT32_MAX;
    }

    uint32 OpenGLDescriptorSetLayout::GetGLBindingBase(RHIBindingType type) const
    {
        switch (type)
        {
            case RHIBindingType::UniformBuffer:
            case RHIBindingType::DynamicUniformBuffer:
                return m_bindingBaseOffsets.uniformBuffer;
            case RHIBindingType::ShaderResourceBuffer:
            case RHIBindingType::StorageBuffer:
            case RHIBindingType::DynamicStorageBuffer:
                return m_bindingBaseOffsets.storageBuffer;
            case RHIBindingType::SampledTexture:
            case RHIBindingType::CombinedTextureSampler:
                return m_bindingBaseOffsets.texture;
            case RHIBindingType::Sampler:
                return m_bindingBaseOffsets.sampler;
            case RHIBindingType::StorageTexture:
                return m_bindingBaseOffsets.image;
            default:
                return 0;
        }
    }

    // =============================================================================
    // OpenGLPipelineLayout Implementation
    // =============================================================================

    OpenGLPipelineLayout::OpenGLPipelineLayout(OpenGLDevice* device, const RHIPipelineLayoutDesc& desc)
        : m_device(device)
        , m_desc(desc)
    {
        for (auto* layout : desc.setLayouts)
        {
            m_setLayouts.push_back(static_cast<OpenGLDescriptorSetLayout*>(layout));
        }

        OpenGLDescriptorSetLayout::BindingBaseOffsets offsets;
        offsets.uniformBuffer = 1; // Binding 0 is reserved for the push constant UBO.

        for (OpenGLDescriptorSetLayout* layout : m_setLayouts)
        {
            if (!layout)
            {
                continue;
            }

            layout->SetBindingBaseOffsets(offsets);

            const auto& counts = layout->GetBindingCounts();
            offsets.uniformBuffer += counts.uniformBuffer;
            offsets.storageBuffer += counts.storageBuffer;
            offsets.texture += counts.texture;
            offsets.sampler += counts.sampler;
            offsets.image += counts.image;
        }

        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }

        RVX_RHI_DEBUG("Created PipelineLayout '{}' with {} sets, {} bytes push constants",
                     GetDebugName(), m_setLayouts.size(), desc.pushConstantSize);
    }

    // =============================================================================
    // OpenGLGraphicsPipeline Implementation
    // =============================================================================

    OpenGLGraphicsPipeline::OpenGLGraphicsPipeline(OpenGLDevice* device, const RHIGraphicsPipelineDesc& desc)
        : m_device(device)
        , m_rasterizerState(desc.rasterizerState)
        , m_depthStencilState(desc.depthStencilState)
        , m_blendState(desc.blendState)
        , m_inputLayout(desc.inputLayout)
        , m_primitiveMode(ToGLPrimitiveMode(desc.primitiveTopology))
        , m_pipelineLayout(static_cast<OpenGLPipelineLayout*>(desc.pipelineLayout))
    {
        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }

        // Create program
        m_program = std::make_unique<OpenGLProgram>(device, GetDebugName().c_str());

        // Attach shaders
        if (desc.vertexShader)
        {
            m_program->AttachShader(static_cast<OpenGLShader*>(desc.vertexShader));
        }
        if (desc.pixelShader)
        {
            m_program->AttachShader(static_cast<OpenGLShader*>(desc.pixelShader));
        }
        if (desc.geometryShader)
        {
            m_program->AttachShader(static_cast<OpenGLShader*>(desc.geometryShader));
        }
        if (desc.hullShader)
        {
            m_program->AttachShader(static_cast<OpenGLShader*>(desc.hullShader));
        }
        if (desc.domainShader)
        {
            m_program->AttachShader(static_cast<OpenGLShader*>(desc.domainShader));
        }

        // Link program
        if (!m_program->Link())
        {
            RVX_RHI_ERROR("Failed to link graphics pipeline '{}'", GetDebugName());
            return;
        }

        // Compute input layout hash
        ComputeInputLayoutHash();

        RVX_RHI_DEBUG("Created Graphics Pipeline '{}' (program #{})",
                     GetDebugName(), m_program->GetHandle());
    }

    OpenGLGraphicsPipeline::~OpenGLGraphicsPipeline()
    {
        RVX_RHI_DEBUG("Destroyed Graphics Pipeline '{}'", GetDebugName());
    }

    void OpenGLGraphicsPipeline::ComputeInputLayoutHash()
    {
        // Simple hash combining all input element properties
        m_inputLayoutHash = 0;
        
        auto hashCombine = [](uint64& seed, uint64 value) {
            seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
        };

        for (size_t i = 0; i < m_inputLayout.elements.size(); ++i)
        {
            const auto& elem = m_inputLayout.elements[i];
            hashCombine(m_inputLayoutHash, std::hash<std::string>{}(elem.semanticName ? elem.semanticName : ""));
            hashCombine(m_inputLayoutHash, elem.semanticIndex);
            hashCombine(m_inputLayoutHash, static_cast<uint64>(elem.format));
            hashCombine(m_inputLayoutHash, elem.inputSlot);
            hashCombine(m_inputLayoutHash, elem.alignedByteOffset);
            hashCombine(m_inputLayoutHash, elem.perInstance ? 1 : 0);
        }
    }

    // =============================================================================
    // OpenGLComputePipeline Implementation
    // =============================================================================

    OpenGLComputePipeline::OpenGLComputePipeline(OpenGLDevice* device, const RHIComputePipelineDesc& desc)
        : m_device(device)
        , m_pipelineLayout(static_cast<OpenGLPipelineLayout*>(desc.pipelineLayout))
    {
        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }

        // Create program
        m_program = std::make_unique<OpenGLProgram>(device, GetDebugName().c_str());

        // Attach compute shader
        if (desc.computeShader)
        {
            m_program->AttachShader(static_cast<OpenGLShader*>(desc.computeShader));
        }
        else
        {
            RVX_RHI_ERROR("Compute pipeline '{}' requires a compute shader", GetDebugName());
            return;
        }

        // Link program
        if (!m_program->Link())
        {
            RVX_RHI_ERROR("Failed to link compute pipeline '{}'", GetDebugName());
            return;
        }

        RVX_RHI_DEBUG("Created Compute Pipeline '{}' (program #{})",
                     GetDebugName(), m_program->GetHandle());
    }

    OpenGLComputePipeline::~OpenGLComputePipeline()
    {
        RVX_RHI_DEBUG("Destroyed Compute Pipeline '{}'", GetDebugName());
    }

} // namespace RVX
