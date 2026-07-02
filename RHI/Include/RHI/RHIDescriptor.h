#pragma once

#include "RHI/RHIRayTracing.h"
#include "RHI/RHIResources.h"

#include <algorithm>
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
            if (type == RHIBindingType::UniformBuffer)
            {
                entry.type = RHIBindingType::DynamicUniformBuffer;
            }
            else if (type == RHIBindingType::StorageBuffer)
            {
                entry.type = RHIBindingType::DynamicStorageBuffer;
            }
            else
            {
                entry.type = type;
            }
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

        virtual const std::vector<RHIBindingLayoutEntry>& GetEntries() const = 0;
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

        // Ray tracing acceleration structure binding
        RHIAccelerationStructure* accelerationStructure = nullptr;

        // Descriptor array element within the layout binding range.
        uint32 arrayElement = 0;
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

        RHIDescriptorSetDesc& BindBuffer(uint32 binding,
                                         RHIBuffer* buffer,
                                         uint64 offset = 0,
                                         uint64 range = RVX_WHOLE_SIZE,
                                         uint32 arrayElement = 0)
        {
            bindings.push_back({binding, buffer, offset, range, nullptr, nullptr, nullptr, arrayElement});
            return *this;
        }

        RHIDescriptorSetDesc& BindTexture(uint32 binding, RHITextureView* view, uint32 arrayElement = 0)
        {
            bindings.push_back({binding, nullptr, 0, 0, view, nullptr, nullptr, arrayElement});
            return *this;
        }

        RHIDescriptorSetDesc& BindSampler(uint32 binding, RHISampler* sampler, uint32 arrayElement = 0)
        {
            bindings.push_back({binding, nullptr, 0, 0, nullptr, sampler, nullptr, arrayElement});
            return *this;
        }

        RHIDescriptorSetDesc& BindCombined(uint32 binding,
                                           RHITextureView* view,
                                           RHISampler* sampler,
                                           uint32 arrayElement = 0)
        {
            bindings.push_back({binding, nullptr, 0, 0, view, sampler, nullptr, arrayElement});
            return *this;
        }

        RHIDescriptorSetDesc& BindAccelerationStructure(uint32 binding,
                                                        RHIAccelerationStructure* accelerationStructure,
                                                        uint32 arrayElement = 0)
        {
            bindings.push_back({binding, nullptr, 0, 0, nullptr, nullptr, accelerationStructure, arrayElement});
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
        virtual bool Update(const std::vector<RHIDescriptorBinding>& bindings) = 0;
    };

    // =============================================================================
    // Descriptor Validation Helpers
    // =============================================================================
    struct RHIDescriptorValidationResult
    {
        bool valid = true;
        const char* message = "";
        uint32 binding = RVX_INVALID_INDEX;

        explicit operator bool() const { return valid; }
    };

    inline RHIDescriptorValidationResult RHIDescriptorValidationPass()
    {
        return {};
    }

    inline RHIDescriptorValidationResult RHIDescriptorValidationFail(
        const char* message,
        uint32 binding = RVX_INVALID_INDEX)
    {
        return {false, message, binding};
    }

    inline bool IsRHIDynamicBindingType(RHIBindingType type)
    {
        return type == RHIBindingType::DynamicUniformBuffer ||
               type == RHIBindingType::DynamicStorageBuffer;
    }

    inline bool IsRHIBufferBindingType(RHIBindingType type)
    {
        return type == RHIBindingType::UniformBuffer ||
               type == RHIBindingType::ShaderResourceBuffer ||
               type == RHIBindingType::StorageBuffer ||
               type == RHIBindingType::DynamicUniformBuffer ||
               type == RHIBindingType::DynamicStorageBuffer;
    }

    inline bool IsRHIAccelerationStructureBindingType(RHIBindingType type)
    {
        return type == RHIBindingType::AccelerationStructure;
    }

    inline const RHIBindingLayoutEntry* FindRHIBindingLayoutEntry(
        const RHIDescriptorSetLayout& layout,
        uint32 binding)
    {
        const auto& entries = layout.GetEntries();
        auto it = std::find_if(entries.begin(), entries.end(),
            [binding](const RHIBindingLayoutEntry& entry)
            {
                return entry.binding == binding;
            });
        return it != entries.end() ? &(*it) : nullptr;
    }

    inline RHIDescriptorValidationResult ValidateRHIDescriptorSetLayoutDesc(
        const RHIDescriptorSetLayoutDesc& desc)
    {
        for (size_t i = 0; i < desc.entries.size(); ++i)
        {
            const RHIBindingLayoutEntry& entry = desc.entries[i];
            if (entry.count == 0)
            {
                return RHIDescriptorValidationFail("descriptor layout binding count must be greater than zero", entry.binding);
            }

            if (entry.isDynamic && !IsRHIDynamicBindingType(entry.type))
            {
                return RHIDescriptorValidationFail("dynamic descriptor binding must use a dynamic buffer binding type", entry.binding);
            }

            if (!entry.isDynamic && IsRHIDynamicBindingType(entry.type))
            {
                return RHIDescriptorValidationFail("dynamic buffer binding type must be marked dynamic", entry.binding);
            }

            for (size_t j = i + 1; j < desc.entries.size(); ++j)
            {
                if (desc.entries[j].binding == entry.binding)
                {
                    return RHIDescriptorValidationFail("duplicate descriptor layout binding", entry.binding);
                }
            }
        }

        return RHIDescriptorValidationPass();
    }

    inline RHIDescriptorValidationResult ValidateRHIPipelineLayoutDesc(
        const RHIPipelineLayoutDesc& desc,
        uint32 maxDescriptorSets = 4)
    {
        if (desc.setLayouts.size() > maxDescriptorSets)
        {
            return RHIDescriptorValidationFail("pipeline layout has too many descriptor sets");
        }

        for (auto* layout : desc.setLayouts)
        {
            if (!layout)
            {
                return RHIDescriptorValidationFail("pipeline layout contains a null descriptor set layout");
            }
        }

        return RHIDescriptorValidationPass();
    }

    inline RHIDescriptorValidationResult ValidateRHIDescriptorBindings(
        const RHIDescriptorSetLayout& layout,
        const std::vector<RHIDescriptorBinding>& bindings)
    {
        for (size_t i = 0; i < bindings.size(); ++i)
        {
            const RHIDescriptorBinding& binding = bindings[i];
            for (size_t j = i + 1; j < bindings.size(); ++j)
            {
                if (bindings[j].binding == binding.binding &&
                    bindings[j].arrayElement == binding.arrayElement)
                {
                    return RHIDescriptorValidationFail("duplicate descriptor binding update", binding.binding);
                }
            }

            const RHIBindingLayoutEntry* entry = FindRHIBindingLayoutEntry(layout, binding.binding);
            if (!entry)
            {
                return RHIDescriptorValidationFail("descriptor binding is not declared in the layout", binding.binding);
            }

            if (binding.arrayElement >= entry->count)
            {
                return RHIDescriptorValidationFail("descriptor binding array element is out of range", binding.binding);
            }

            const bool hasBuffer = binding.buffer != nullptr;
            const bool hasTexture = binding.textureView != nullptr;
            const bool hasSampler = binding.sampler != nullptr;
            const bool hasAccelerationStructure = binding.accelerationStructure != nullptr;

            if (IsRHIBufferBindingType(entry->type))
            {
                if (!hasBuffer || hasTexture || hasSampler || hasAccelerationStructure)
                {
                    return RHIDescriptorValidationFail("descriptor binding must contain exactly one buffer resource", binding.binding);
                }
            }
            else if (entry->type == RHIBindingType::SampledTexture ||
                     entry->type == RHIBindingType::StorageTexture)
            {
                if (!hasTexture || hasBuffer || hasSampler || hasAccelerationStructure)
                {
                    return RHIDescriptorValidationFail("descriptor binding must contain exactly one texture view", binding.binding);
                }
            }
            else if (entry->type == RHIBindingType::Sampler)
            {
                if (!hasSampler || hasBuffer || hasTexture || hasAccelerationStructure)
                {
                    return RHIDescriptorValidationFail("descriptor binding must contain exactly one sampler", binding.binding);
                }
            }
            else if (entry->type == RHIBindingType::CombinedTextureSampler)
            {
                if (!hasTexture || !hasSampler || hasBuffer || hasAccelerationStructure)
                {
                    return RHIDescriptorValidationFail("combined texture-sampler binding requires a texture view and sampler", binding.binding);
                }
            }
            else if (IsRHIAccelerationStructureBindingType(entry->type))
            {
                if (!hasAccelerationStructure || hasBuffer || hasTexture || hasSampler)
                {
                    return RHIDescriptorValidationFail("descriptor binding must contain exactly one acceleration structure", binding.binding);
                }

                if (binding.accelerationStructure->GetType() != RHIAccelerationStructureType::TopLevel)
                {
                    return RHIDescriptorValidationFail(
                        "descriptor acceleration structure binding requires a top-level acceleration structure",
                        binding.binding);
                }

                if (binding.accelerationStructure->GetGPUVirtualAddress() == 0)
                {
                    return RHIDescriptorValidationFail(
                        "descriptor acceleration structure binding requires a non-zero GPU address",
                        binding.binding);
                }
            }
        }

        return RHIDescriptorValidationPass();
    }

    inline RHIDescriptorValidationResult ValidateRHIDescriptorSetDesc(
        const RHIDescriptorSetDesc& desc)
    {
        if (!desc.layout)
        {
            return RHIDescriptorValidationFail("descriptor set layout is null");
        }

        return ValidateRHIDescriptorBindings(*desc.layout, desc.bindings);
    }

} // namespace RVX
