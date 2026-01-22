#include "OpenGLDescriptor.h"
#include "OpenGLDevice.h"
#include "OpenGLPipeline.h"
#include "OpenGLResources.h"
#include "OpenGLStateCache.h"
#include "Core/Log.h"
#include <algorithm>

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
                
                // Infer access mode from texture usage flags
                // If texture has both UnorderedAccess and ShaderResource, it supports read-write
                // If only UnorderedAccess, default to read-write for maximum compatibility
                const auto usage = glTexture->GetUsage();
                bool canRead = HasFlag(usage, RHITextureUsage::ShaderResource) || 
                               HasFlag(usage, RHITextureUsage::UnorderedAccess);
                bool canWrite = HasFlag(usage, RHITextureUsage::UnorderedAccess);
                
                if (canRead && canWrite)
                    glEntry.imageAccess = GL_READ_WRITE;
                else if (canWrite)
                    glEntry.imageAccess = GL_WRITE_ONLY;
                else if (canRead)
                    glEntry.imageAccess = GL_READ_ONLY;
                else
                    glEntry.imageAccess = GL_READ_WRITE;  // Fallback
                
                // Get subresource info from view
                const auto& sr = glView->GetSubresourceRange();
                glEntry.imageLevel = static_cast<GLint>(sr.baseMipLevel);
                glEntry.imageLayered = (sr.arrayLayerCount > 1 || sr.arrayLayerCount == RVX_ALL_LAYERS) 
                                       ? GL_TRUE : GL_FALSE;
                glEntry.imageLayer = static_cast<GLint>(sr.baseArrayLayer);
                break;
            }
        }

        m_bindings.push_back(glEntry);
    }

    void OpenGLDescriptorSet::Bind(OpenGLStateCache& stateCache, uint32 setIndex,
                                   std::span<const uint32> dynamicOffsets)
    {
        GL_DEBUG_SCOPE("BindDescriptorSet");

        // Track which dynamic offset to use next
        size_t dynamicOffsetIndex = 0;

        // Check if multi-bind is available for batch optimizations
        bool useMultiBind = m_device && m_device->GetCapabilities().opengl.hasMultiBind;

        // Collect texture and sampler bindings for potential batch binding
        std::vector<std::pair<uint32, GLuint>> textureBindings;  // <slot, texture>
        std::vector<std::pair<uint32, GLuint>> samplerBindings;  // <slot, sampler>

        for (const auto& entry : m_bindings)
        {
            uint32 globalBinding = entry.glBinding;

            switch (entry.type)
            {
                case RHIBindingType::UniformBuffer:
                    if (entry.buffer != 0)
                    {
                        stateCache.BindUniformBuffer(globalBinding, entry.buffer, 
                                                    entry.offset, entry.size);
                    }
                    break;

                case RHIBindingType::DynamicUniformBuffer:
                    if (entry.buffer != 0)
                    {
                        GLintptr offset = entry.offset;
                        if (dynamicOffsetIndex < dynamicOffsets.size())
                        {
                            offset += static_cast<GLintptr>(dynamicOffsets[dynamicOffsetIndex++]);
                        }
                        stateCache.BindUniformBuffer(globalBinding, entry.buffer, offset, entry.size);
                    }
                    break;

                case RHIBindingType::StorageBuffer:
                    if (entry.buffer != 0)
                    {
                        stateCache.BindStorageBuffer(globalBinding, entry.buffer,
                                                    entry.offset, entry.size);
                    }
                    break;

                case RHIBindingType::DynamicStorageBuffer:
                    if (entry.buffer != 0)
                    {
                        GLintptr offset = entry.offset;
                        if (dynamicOffsetIndex < dynamicOffsets.size())
                        {
                            offset += static_cast<GLintptr>(dynamicOffsets[dynamicOffsetIndex++]);
                        }
                        stateCache.BindStorageBuffer(globalBinding, entry.buffer, offset, entry.size);
                    }
                    break;

                case RHIBindingType::SampledTexture:
                    if (entry.texture != 0)
                    {
                        if (useMultiBind)
                        {
                            textureBindings.emplace_back(globalBinding, entry.texture);
                        }
                        else
                        {
                            stateCache.BindTexture(globalBinding, entry.textureTarget, entry.texture);
                        }
                    }
                    break;

                case RHIBindingType::CombinedTextureSampler:
                    if (entry.texture != 0)
                    {
                        if (useMultiBind)
                        {
                            textureBindings.emplace_back(globalBinding, entry.texture);
                        }
                        else
                        {
                            stateCache.BindTexture(globalBinding, entry.textureTarget, entry.texture);
                        }
                    }
                    if (entry.sampler != 0)
                    {
                        if (useMultiBind)
                        {
                            samplerBindings.emplace_back(globalBinding, entry.sampler);
                        }
                        else
                        {
                            stateCache.BindSampler(globalBinding, entry.sampler);
                        }
                    }
                    break;

                case RHIBindingType::Sampler:
                    if (entry.sampler != 0)
                    {
                        if (useMultiBind)
                        {
                            samplerBindings.emplace_back(globalBinding, entry.sampler);
                        }
                        else
                        {
                            stateCache.BindSampler(globalBinding, entry.sampler);
                        }
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

        // Batch bind textures if using multi-bind and we have multiple textures
        if (useMultiBind && textureBindings.size() > 1)
        {
            // Sort by binding slot for contiguous binding
            std::sort(textureBindings.begin(), textureBindings.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });

            // Find contiguous ranges and batch bind them
            size_t i = 0;
            while (i < textureBindings.size())
            {
                uint32 startSlot = textureBindings[i].first;
                size_t rangeStart = i;
                
                // Find contiguous range
                while (i + 1 < textureBindings.size() && 
                       textureBindings[i + 1].first == textureBindings[i].first + 1)
                {
                    ++i;
                }
                
                size_t count = i - rangeStart + 1;
                
                if (count > 1)
                {
                    // Batch bind contiguous textures
                    std::vector<GLuint> textures(count);
                    for (size_t j = 0; j < count; ++j)
                    {
                        textures[j] = textureBindings[rangeStart + j].second;
                    }
                    GL_CHECK(glBindTextures(startSlot, static_cast<GLsizei>(count), textures.data()));
                }
                else
                {
                    // Single texture, use normal bind
                    stateCache.BindTexture(startSlot, GL_TEXTURE_2D, textureBindings[rangeStart].second);
                }
                ++i;
            }
        }
        else if (useMultiBind)
        {
            // Single texture or none - use normal path
            for (const auto& [slot, texture] : textureBindings)
            {
                stateCache.BindTexture(slot, GL_TEXTURE_2D, texture);
            }
        }

        // Batch bind samplers if using multi-bind and we have multiple samplers
        if (useMultiBind && samplerBindings.size() > 1)
        {
            std::sort(samplerBindings.begin(), samplerBindings.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });

            size_t i = 0;
            while (i < samplerBindings.size())
            {
                uint32 startSlot = samplerBindings[i].first;
                size_t rangeStart = i;
                
                while (i + 1 < samplerBindings.size() && 
                       samplerBindings[i + 1].first == samplerBindings[i].first + 1)
                {
                    ++i;
                }
                
                size_t count = i - rangeStart + 1;
                
                if (count > 1)
                {
                    std::vector<GLuint> samplers(count);
                    for (size_t j = 0; j < count; ++j)
                    {
                        samplers[j] = samplerBindings[rangeStart + j].second;
                    }
                    GL_CHECK(glBindSamplers(startSlot, static_cast<GLsizei>(count), samplers.data()));
                }
                else
                {
                    stateCache.BindSampler(startSlot, samplerBindings[rangeStart].second);
                }
                ++i;
            }
        }
        else if (useMultiBind)
        {
            for (const auto& [slot, sampler] : samplerBindings)
            {
                stateCache.BindSampler(slot, sampler);
            }
        }
    }

} // namespace RVX
