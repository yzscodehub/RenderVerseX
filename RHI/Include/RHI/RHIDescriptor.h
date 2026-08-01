#pragma once

#include "RHI/RHICapabilities.h"
#include "RHI/RHIRayTracing.h"
#include "RHI/RHIResources.h"

#include <algorithm>
#include <atomic>
#include <vector>

namespace RVX
{
    /** @brief Describes whether the resource contents referenced by a descriptor may change. */
    enum class RHIResourceDataVolatility : uint8
    {
        Mutable = 0,
        Immutable
    };

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
        RHIResourceDataVolatility resourceDataVolatility = RHIResourceDataVolatility::Mutable;
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
            uint32 count = 1,
            RHIResourceDataVolatility resourceDataVolatility = RHIResourceDataVolatility::Mutable)
        {
            entries.push_back({binding, type, visibility, count, false, resourceDataVolatility});
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
            entry.resourceDataVolatility = RHIResourceDataVolatility::Mutable;
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
        RHIPipelineLayout() = default;
        explicit RHIPipelineLayout(const RHIPipelineLayoutDesc& desc)
            : m_setLayouts(desc.setLayouts)
            , m_pushConstantSize(desc.pushConstantSize)
            , m_pushConstantStages(desc.pushConstantStages)
            , m_hasDescription(true)
        {
            if (desc.debugName)
            {
                SetDebugName(desc.debugName);
            }
        }
        virtual ~RHIPipelineLayout() = default;

        [[nodiscard]] bool HasDescription() const
        {
            return m_hasDescription;
        }

        [[nodiscard]] const std::vector<RHIDescriptorSetLayout*>&
        GetDescriptorSetLayouts() const
        {
            return m_setLayouts;
        }

        [[nodiscard]] uint32 GetDeclaredPushConstantSize() const
        {
            return m_pushConstantSize;
        }

        [[nodiscard]] RHIShaderStage GetDeclaredPushConstantStages() const
        {
            return m_pushConstantStages;
        }

    private:
        std::vector<RHIDescriptorSetLayout*> m_setLayouts;
        uint32 m_pushConstantSize = 0;
        RHIShaderStage m_pushConstantStages = RHIShaderStage::None;
        bool m_hasDescription = false;
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
        RHIDescriptorSet() = default;
        explicit RHIDescriptorSet(const RHIDescriptorSetDesc& desc);
        virtual ~RHIDescriptorSet() = default;

        /**
         * @brief Descriptor sets are immutable snapshots after creation.
         * @return true only for an empty no-op update on a ready set. Create a replacement set for changes.
         */
        virtual bool Update(const std::vector<RHIDescriptorBinding>& bindings)
        {
            return bindings.empty() && m_readyForBinding;
        }

        [[nodiscard]] bool IsReadyForBinding(
            const RHIDescriptorSetLayout* expectedLayout = nullptr) const
        {
            return m_readyForBinding &&
                   (!expectedLayout || expectedLayout == m_layoutIdentity);
        }

        [[nodiscard]] RHIDescriptorSetLayout* GetLayoutIdentity() const
        {
            return m_layoutIdentity;
        }

        [[nodiscard]] const std::vector<RHIDescriptorBinding>&
        GetDescriptorSnapshot() const
        {
            return m_descriptorSnapshot;
        }

        [[nodiscard]] uint32 GetRequiredDynamicOffsetCount() const
        {
            return m_requiredDynamicOffsetCount;
        }

        [[nodiscard]] bool HasBeenBound() const
        {
            return m_hasBeenBound.load(std::memory_order_relaxed);
        }

        void MarkBound()
        {
            m_hasBeenBound.store(true, std::memory_order_relaxed);
        }

    protected:
        void InitializeDescriptorSnapshot(const RHIDescriptorSetDesc& desc);
        void InvalidateDescriptorSnapshot() { m_readyForBinding = false; }

    private:
        RHIDescriptorSetLayout* m_layoutIdentity = nullptr;
        std::vector<RHIDescriptorBinding> m_descriptorSnapshot;
        uint32 m_requiredDynamicOffsetCount = 0;
        bool m_readyForBinding = false;
        std::atomic<bool> m_hasBeenBound = false;
    };

    // =============================================================================
    // Descriptor Validation Helpers
    // =============================================================================
    enum class RHIDescriptorValidationCode : uint8
    {
        None = 0,
        NullLayout,
        InvalidLayout,
        DuplicateBinding,
        UndeclaredBinding,
        ArrayElementOutOfRange,
        InvalidResource,
        IncompleteSnapshot
    };

    struct RHIDescriptorValidationResult
    {
        bool valid = true;
        RHIDescriptorValidationCode code = RHIDescriptorValidationCode::None;
        const char* message = "";
        uint32 binding = RVX_INVALID_INDEX;
        uint32 arrayElement = RVX_INVALID_INDEX;

        explicit operator bool() const { return valid; }
    };

    inline RHIDescriptorValidationResult RHIDescriptorValidationPass()
    {
        return {};
    }

    inline RHIDescriptorValidationResult RHIDescriptorValidationFail(
        const char* message,
        uint32 binding = RVX_INVALID_INDEX,
        uint32 arrayElement = RVX_INVALID_INDEX,
        RHIDescriptorValidationCode code = RHIDescriptorValidationCode::InvalidResource)
    {
        return {false, code, message, binding, arrayElement};
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
                return RHIDescriptorValidationFail(
                    "descriptor layout binding count must be greater than zero",
                    entry.binding,
                    RVX_INVALID_INDEX,
                    RHIDescriptorValidationCode::InvalidLayout);
            }

            if (entry.isDynamic && !IsRHIDynamicBindingType(entry.type))
            {
                return RHIDescriptorValidationFail(
                    "dynamic descriptor binding must use a dynamic buffer binding type",
                    entry.binding,
                    RVX_INVALID_INDEX,
                    RHIDescriptorValidationCode::InvalidLayout);
            }

            if (!entry.isDynamic && IsRHIDynamicBindingType(entry.type))
            {
                return RHIDescriptorValidationFail(
                    "dynamic buffer binding type must be marked dynamic",
                    entry.binding,
                    RVX_INVALID_INDEX,
                    RHIDescriptorValidationCode::InvalidLayout);
            }

            if ((entry.isDynamic ||
                 entry.type == RHIBindingType::StorageBuffer ||
                 entry.type == RHIBindingType::DynamicStorageBuffer ||
                 entry.type == RHIBindingType::StorageTexture) &&
                entry.resourceDataVolatility == RHIResourceDataVolatility::Immutable)
            {
                return RHIDescriptorValidationFail(
                    "dynamic and writable descriptor resources must use mutable data volatility",
                    entry.binding,
                    RVX_INVALID_INDEX,
                    RHIDescriptorValidationCode::InvalidLayout);
            }

            for (size_t j = i + 1; j < desc.entries.size(); ++j)
            {
                if (desc.entries[j].binding == entry.binding)
                {
                    return RHIDescriptorValidationFail(
                        "duplicate descriptor layout binding",
                        entry.binding,
                        RVX_INVALID_INDEX,
                        RHIDescriptorValidationCode::DuplicateBinding);
                }
            }
        }

        return RHIDescriptorValidationPass();
    }

    inline RHIDescriptorValidationResult ValidateRHIDescriptorSetLayoutCapabilities(
        const RHIDescriptorSetLayoutDesc& desc,
        const RHICapabilities& capabilities)
    {
        const RHIDescriptorValidationResult layoutValidation =
            ValidateRHIDescriptorSetLayoutDesc(desc);
        if (!layoutValidation)
        {
            return layoutValidation;
        }

        if (!capabilities.supportsDescriptorSets || capabilities.maxDescriptorSets == 0)
        {
            return RHIDescriptorValidationFail(
                "device does not support the descriptor-set contract",
                RVX_INVALID_INDEX,
                RVX_INVALID_INDEX,
                RHIDescriptorValidationCode::InvalidLayout);
        }

        for (const RHIBindingLayoutEntry& entry : desc.entries)
        {
            if (entry.isDynamic && !capabilities.supportsDynamicDescriptorOffsets)
            {
                return RHIDescriptorValidationFail(
                    "device does not support dynamic descriptor offsets",
                    entry.binding,
                    RVX_INVALID_INDEX,
                    RHIDescriptorValidationCode::InvalidLayout);
            }

            if (IsRHIAccelerationStructureBindingType(entry.type) &&
                !capabilities.supportsRaytracing)
            {
                return RHIDescriptorValidationFail(
                    "device does not support acceleration-structure descriptors",
                    entry.binding,
                    RVX_INVALID_INDEX,
                    RHIDescriptorValidationCode::InvalidLayout);
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
            return RHIDescriptorValidationFail(
                "pipeline layout has too many descriptor sets",
                RVX_INVALID_INDEX,
                RVX_INVALID_INDEX,
                RHIDescriptorValidationCode::InvalidLayout);
        }

        for (auto* layout : desc.setLayouts)
        {
            if (!layout)
            {
                return RHIDescriptorValidationFail(
                    "pipeline layout contains a null descriptor set layout",
                    RVX_INVALID_INDEX,
                    RVX_INVALID_INDEX,
                    RHIDescriptorValidationCode::NullLayout);
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
                    return RHIDescriptorValidationFail(
                        "duplicate descriptor binding element",
                        binding.binding,
                        binding.arrayElement,
                        RHIDescriptorValidationCode::DuplicateBinding);
                }
            }

            const RHIBindingLayoutEntry* entry = FindRHIBindingLayoutEntry(layout, binding.binding);
            if (!entry)
            {
                return RHIDescriptorValidationFail(
                    "descriptor binding is not declared in the layout",
                    binding.binding,
                    binding.arrayElement,
                    RHIDescriptorValidationCode::UndeclaredBinding);
            }

            if (binding.arrayElement >= entry->count)
            {
                return RHIDescriptorValidationFail(
                    "descriptor binding array element is out of range",
                    binding.binding,
                    binding.arrayElement,
                    RHIDescriptorValidationCode::ArrayElementOutOfRange);
            }

            const bool hasBuffer = binding.buffer != nullptr;
            const bool hasTexture = binding.textureView != nullptr;
            const bool hasSampler = binding.sampler != nullptr;
            const bool hasAccelerationStructure = binding.accelerationStructure != nullptr;

            if (IsRHIBufferBindingType(entry->type))
            {
                if (!hasBuffer || hasTexture || hasSampler || hasAccelerationStructure)
                {
                    return RHIDescriptorValidationFail(
                        "descriptor binding must contain exactly one buffer resource",
                        binding.binding,
                        binding.arrayElement);
                }
            }
            else if (entry->type == RHIBindingType::SampledTexture ||
                     entry->type == RHIBindingType::StorageTexture)
            {
                if (!hasTexture || hasBuffer || hasSampler || hasAccelerationStructure)
                {
                    return RHIDescriptorValidationFail(
                        "descriptor binding must contain exactly one texture view",
                        binding.binding,
                        binding.arrayElement);
                }
            }
            else if (entry->type == RHIBindingType::Sampler)
            {
                if (!hasSampler || hasBuffer || hasTexture || hasAccelerationStructure)
                {
                    return RHIDescriptorValidationFail(
                        "descriptor binding must contain exactly one sampler",
                        binding.binding,
                        binding.arrayElement);
                }
            }
            else if (entry->type == RHIBindingType::CombinedTextureSampler)
            {
                if (!hasTexture || !hasSampler || hasBuffer || hasAccelerationStructure)
                {
                    return RHIDescriptorValidationFail(
                        "combined texture-sampler binding requires a texture view and sampler",
                        binding.binding,
                        binding.arrayElement);
                }
            }
            else if (IsRHIAccelerationStructureBindingType(entry->type))
            {
                if (!hasAccelerationStructure || hasBuffer || hasTexture || hasSampler)
                {
                    return RHIDescriptorValidationFail(
                        "descriptor binding must contain exactly one acceleration structure",
                        binding.binding,
                        binding.arrayElement);
                }

                if (binding.accelerationStructure->GetType() != RHIAccelerationStructureType::TopLevel)
                {
                    return RHIDescriptorValidationFail(
                        "descriptor acceleration structure binding requires a top-level acceleration structure",
                        binding.binding,
                        binding.arrayElement);
                }

                if (binding.accelerationStructure->GetGPUVirtualAddress() == 0)
                {
                    return RHIDescriptorValidationFail(
                        "descriptor acceleration structure binding requires a non-zero GPU address",
                        binding.binding,
                        binding.arrayElement);
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
            return RHIDescriptorValidationFail(
                "descriptor set layout is null",
                RVX_INVALID_INDEX,
                RVX_INVALID_INDEX,
                RHIDescriptorValidationCode::NullLayout);
        }

        const RHIDescriptorValidationResult bindingValidation =
            ValidateRHIDescriptorBindings(*desc.layout, desc.bindings);
        if (!bindingValidation)
        {
            return bindingValidation;
        }

        for (const RHIBindingLayoutEntry& entry : desc.layout->GetEntries())
        {
            for (uint32 arrayElement = 0; arrayElement < entry.count; ++arrayElement)
            {
                const auto found = std::find_if(
                    desc.bindings.begin(),
                    desc.bindings.end(),
                    [&entry, arrayElement](const RHIDescriptorBinding& binding)
                    {
                        return binding.binding == entry.binding &&
                               binding.arrayElement == arrayElement;
                    });
                if (found == desc.bindings.end())
                {
                    return RHIDescriptorValidationFail(
                        "descriptor set snapshot is missing a required binding element",
                        entry.binding,
                        arrayElement,
                        RHIDescriptorValidationCode::IncompleteSnapshot);
                }
            }
        }

        return RHIDescriptorValidationPass();
    }

    inline RHIDescriptorSet::RHIDescriptorSet(const RHIDescriptorSetDesc& desc)
    {
        InitializeDescriptorSnapshot(desc);
    }

    inline void RHIDescriptorSet::InitializeDescriptorSnapshot(
        const RHIDescriptorSetDesc& desc)
    {
        m_layoutIdentity = desc.layout;
        m_descriptorSnapshot.clear();
        m_requiredDynamicOffsetCount = 0;
        m_readyForBinding = false;

        if (!ValidateRHIDescriptorSetDesc(desc))
        {
            return;
        }

        m_descriptorSnapshot = desc.bindings;
        std::sort(
            m_descriptorSnapshot.begin(),
            m_descriptorSnapshot.end(),
            [](const RHIDescriptorBinding& lhs, const RHIDescriptorBinding& rhs)
            {
                if (lhs.binding != rhs.binding)
                {
                    return lhs.binding < rhs.binding;
                }
                return lhs.arrayElement < rhs.arrayElement;
            });

        for (const RHIBindingLayoutEntry& entry : desc.layout->GetEntries())
        {
            if (entry.isDynamic)
            {
                m_requiredDynamicOffsetCount += entry.count;
            }
        }

        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }
        m_readyForBinding = true;
    }

} // namespace RVX
