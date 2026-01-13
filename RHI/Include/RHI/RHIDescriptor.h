#pragma once

#include "RHI/RHIResources.h"
#include <vector>

namespace RVX
{
    // =============================================================================
    // Binding Layout Entry
    // =============================================================================
    struct RHIBindingLayoutEntry
    {
        uint32 binding = 0;
        RHIBindingType type = RHIBindingType::UniformBuffer;
        RHIShaderStage visibility = RHIShaderStage::All;
        uint32 count = 1;
        bool isDynamic = false;  // For dynamic uniform/storage buffers
    };

    // =============================================================================
    // Descriptor Set Layout Description
    // =============================================================================
    struct RHIDescriptorSetLayoutDesc
    {
        std::vector<RHIBindingLayoutEntry> entries;
        const char* debugName = nullptr;

        RHIDescriptorSetLayoutDesc& AddBinding(
            uint32 binding,
            RHIBindingType type,
            RHIShaderStage visibility = RHIShaderStage::All,
            uint32 count = 1)
        {
            entries.push_back({binding, type, visibility, count, false});
            return *this;
        }

        RHIDescriptorSetLayoutDesc& AddDynamicBinding(
            uint32 binding,
            RHIBindingType type,
            RHIShaderStage visibility = RHIShaderStage::All)
        {
            RHIBindingLayoutEntry entry;
            entry.binding = binding;
            entry.type = type;
            entry.visibility = visibility;
            entry.isDynamic = true;
            entries.push_back(entry);
            return *this;
        }
    };

    // =============================================================================
    // Descriptor Set Layout Interface
    // =============================================================================
    class RHIDescriptorSetLayout : public RHIResource
    {
    public:
        virtual ~RHIDescriptorSetLayout() = default;
    };

    // =============================================================================
    // Pipeline Layout Description
    // =============================================================================
    struct RHIPipelineLayoutDesc
    {
        std::vector<RHIDescriptorSetLayout*> setLayouts;  // Max 4 sets
        uint32 pushConstantSize = 0;
        RHIShaderStage pushConstantStages = RHIShaderStage::All;
        const char* debugName = nullptr;
    };

    // =============================================================================
    // Pipeline Layout Interface
    // =============================================================================
    class RHIPipelineLayout : public RHIResource
    {
    public:
        virtual ~RHIPipelineLayout() = default;
    };

    // =============================================================================
    // Descriptor Set Entry
    // =============================================================================
    struct RHIDescriptorBinding
    {
        uint32 binding = 0;

        // Buffer binding
        RHIBuffer* buffer = nullptr;
        uint64 offset = 0;
        uint64 range = RVX_WHOLE_SIZE;

        // Texture binding
        RHITextureView* textureView = nullptr;

        // Sampler binding
        RHISampler* sampler = nullptr;
    };

    // =============================================================================
    // Descriptor Set Description
    // =============================================================================
    struct RHIDescriptorSetDesc
    {
        RHIDescriptorSetLayout* layout = nullptr;
        std::vector<RHIDescriptorBinding> bindings;
        const char* debugName = nullptr;

        RHIDescriptorSetDesc& SetLayout(RHIDescriptorSetLayout* l) { layout = l; return *this; }

        RHIDescriptorSetDesc& BindBuffer(uint32 binding, RHIBuffer* buffer, uint64 offset = 0, uint64 range = RVX_WHOLE_SIZE)
        {
            bindings.push_back({binding, buffer, offset, range, nullptr, nullptr});
            return *this;
        }

        RHIDescriptorSetDesc& BindTexture(uint32 binding, RHITextureView* view)
        {
            bindings.push_back({binding, nullptr, 0, 0, view, nullptr});
            return *this;
        }

        RHIDescriptorSetDesc& BindSampler(uint32 binding, RHISampler* sampler)
        {
            bindings.push_back({binding, nullptr, 0, 0, nullptr, sampler});
            return *this;
        }

        RHIDescriptorSetDesc& BindCombined(uint32 binding, RHITextureView* view, RHISampler* sampler)
        {
            bindings.push_back({binding, nullptr, 0, 0, view, sampler});
            return *this;
        }
    };

    // =============================================================================
    // Descriptor Set Interface
    // =============================================================================
    class RHIDescriptorSet : public RHIResource
    {
    public:
        virtual ~RHIDescriptorSet() = default;

        // Update bindings
        virtual void Update(const std::vector<RHIDescriptorBinding>& bindings) = 0;
    };

} // namespace RVX
