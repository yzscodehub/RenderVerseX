#include "OpenGLDescriptor.h"
#include "OpenGLDevice.h"
#include "OpenGLPipeline.h"
#include "OpenGLResources.h"
#include "OpenGLStateCache.h"
#include "Core/Log.h"

namespace RVX
{
    OpenGLDescriptorSet::OpenGLDescriptorSet(OpenGLDevice* device, const RHIDescriptorSetDesc& desc)
        : m_device(device)
        , m_layout(static_cast<OpenGLDescriptorSetLayout*>(desc.layout))
    {
        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }

        // Resolve initial bindings
        for (const auto& binding : desc.bindings)
        {
            ResolveBinding(binding);
        }

        RVX_RHI_DEBUG("Created DescriptorSet '{}' with {} bindings",
                     GetDebugName(), m_bindings.size());
    }

    void OpenGLDescriptorSet::Update(const std::vector<RHIDescriptorBinding>& bindings)
    {
        // Clear and re-resolve all bindings
        m_bindings.clear();

        for (const auto& binding : bindings)
        {
            ResolveBinding(binding);
        }

        RVX_RHI_DEBUG("Updated DescriptorSet '{}' with {} bindings",
                     GetDebugName(), m_bindings.size());
    }

    void OpenGLDescriptorSet::ResolveBinding(const RHIDescriptorBinding& binding)
    {
        if (!m_layout)
        {
            RVX_RHI_ERROR("DescriptorSet '{}' has no layout", GetDebugName());
            return;
        }

        // Find the layout entry for this binding
        const auto& entries = m_layout->GetEntries();
        const RHIBindingLayoutEntry* layoutEntry = nullptr;

        for (const auto& entry : entries)
        {
            if (entry.binding == binding.binding)
            {
                layoutEntry = &entry;
                break;
            }
        }

        if (!layoutEntry)
        {
            RVX_RHI_ERROR("Binding {} not found in descriptor set layout '{}'",
                         binding.binding, m_layout->GetDebugName());
            return;
        }

        OpenGLBindingEntry glEntry;
        glEntry.type = layoutEntry->type;
        glEntry.glBinding = m_layout->GetGLBinding(binding.binding, layoutEntry->type);

        switch (layoutEntry->type)
        {
            case RHIBindingType::UniformBuffer:
            case RHIBindingType::DynamicUniformBuffer:
            {
                if (!binding.buffer)
                {
                    RVX_RHI_WARN("Binding {}: UBO is null", binding.binding);
                    break;
                }
                auto* glBuffer = static_cast<OpenGLBuffer*>(binding.buffer);
                glEntry.buffer = glBuffer->GetHandle();
                glEntry.offset = static_cast<GLintptr>(binding.offset);
                glEntry.size = (binding.range == RVX_WHOLE_SIZE) ? 
                              static_cast<GLsizeiptr>(glBuffer->GetSize() - binding.offset) :
                              static_cast<GLsizeiptr>(binding.range);
                break;
            }

            case RHIBindingType::StorageBuffer:
            case RHIBindingType::DynamicStorageBuffer:
            {
                if (!binding.buffer)
                {
                    RVX_RHI_WARN("Binding {}: SSBO is null", binding.binding);
                    break;
                }
                auto* glBuffer = static_cast<OpenGLBuffer*>(binding.buffer);
                glEntry.buffer = glBuffer->GetHandle();
                glEntry.offset = static_cast<GLintptr>(binding.offset);
                glEntry.size = (binding.range == RVX_WHOLE_SIZE) ? 
                              static_cast<GLsizeiptr>(glBuffer->GetSize() - binding.offset) :
                              static_cast<GLsizeiptr>(binding.range);
                break;
            }

            case RHIBindingType::SampledTexture:
            {
                if (!binding.textureView)
                {
                    RVX_RHI_WARN("Binding {}: Texture view is null", binding.binding);
                    break;
                }
                auto* glView = static_cast<OpenGLTextureView*>(binding.textureView);
                glEntry.texture = glView->GetHandle();
                glEntry.textureTarget = glView->GetTarget();
                break;
            }

            case RHIBindingType::CombinedTextureSampler:
            {
                if (binding.textureView)
                {
                    auto* glView = static_cast<OpenGLTextureView*>(binding.textureView);
                    glEntry.texture = glView->GetHandle();
                    glEntry.textureTarget = glView->GetTarget();
                }
                if (binding.sampler)
                {
                    auto* glSampler = static_cast<OpenGLSampler*>(binding.sampler);
                    glEntry.sampler = glSampler->GetHandle();
                }
                break;
            }

            case RHIBindingType::Sampler:
            {
                if (!binding.sampler)
                {
                    RVX_RHI_WARN("Binding {}: Sampler is null", binding.binding);
                    break;
                }
                auto* glSampler = static_cast<OpenGLSampler*>(binding.sampler);
                glEntry.sampler = glSampler->GetHandle();
                break;
            }

            case RHIBindingType::StorageTexture:
            {
                if (!binding.textureView)
                {
                    RVX_RHI_WARN("Binding {}: Storage texture view is null", binding.binding);
                    break;
                }
                auto* glView = static_cast<OpenGLTextureView*>(binding.textureView);
                auto* glTexture = static_cast<OpenGLTexture*>(glView->GetTexture());
                
                glEntry.texture = glView->GetHandle();
                glEntry.imageFormat = glTexture->GetGLFormat().internalFormat;
                glEntry.imageAccess = GL_READ_WRITE;  // TODO: Get from desc
                glEntry.imageLevel = 0;
                glEntry.imageLayered = GL_FALSE;
                glEntry.imageLayer = 0;
                break;
            }
        }

        m_bindings.push_back(glEntry);
    }

    void OpenGLDescriptorSet::Bind(OpenGLStateCache& stateCache, uint32 setIndex)
    {
        GL_DEBUG_SCOPE("BindDescriptorSet");

        for (const auto& entry : m_bindings)
        {
            // Calculate global binding point (incorporating set offset)
            // For simplicity, we use a flat binding space
            uint32 globalBinding = entry.glBinding;

            switch (entry.type)
            {
                case RHIBindingType::UniformBuffer:
                case RHIBindingType::DynamicUniformBuffer:
                    if (entry.buffer != 0)
                    {
                        stateCache.BindUniformBuffer(globalBinding, entry.buffer, 
                                                    entry.offset, entry.size);
                    }
                    break;

                case RHIBindingType::StorageBuffer:
                case RHIBindingType::DynamicStorageBuffer:
                    if (entry.buffer != 0)
                    {
                        stateCache.BindStorageBuffer(globalBinding, entry.buffer,
                                                    entry.offset, entry.size);
                    }
                    break;

                case RHIBindingType::SampledTexture:
                    if (entry.texture != 0)
                    {
                        stateCache.BindTexture(globalBinding, entry.textureTarget, entry.texture);
                    }
                    break;

                case RHIBindingType::CombinedTextureSampler:
                    if (entry.texture != 0)
                    {
                        stateCache.BindTexture(globalBinding, entry.textureTarget, entry.texture);
                    }
                    if (entry.sampler != 0)
                    {
                        stateCache.BindSampler(globalBinding, entry.sampler);
                    }
                    break;

                case RHIBindingType::Sampler:
                    if (entry.sampler != 0)
                    {
                        stateCache.BindSampler(globalBinding, entry.sampler);
                    }
                    break;

                case RHIBindingType::StorageTexture:
                    if (entry.texture != 0)
                    {
                        stateCache.BindImageTexture(globalBinding, entry.texture,
                                                   entry.imageLevel, entry.imageLayered,
                                                   entry.imageLayer, entry.imageAccess,
                                                   entry.imageFormat);
                    }
                    break;
            }
        }
    }

} // namespace RVX
