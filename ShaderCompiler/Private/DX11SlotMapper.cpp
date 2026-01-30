#include "ShaderCompiler/ShaderCompiler.h"
#include "ShaderCompiler/ShaderReflection.h"
#include "Core/Types.h"
#include <vector>
#include <unordered_map>
#include <algorithm>

#ifdef _WIN32

namespace RVX
{
    // =========================================================================
    // DX11 Resource Limits
    // =========================================================================
    constexpr uint32 DX11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT = 14;
    constexpr uint32 DX11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT = 128;
    constexpr uint32 DX11_COMMONSHADER_SAMPLER_SLOT_COUNT = 16;
    constexpr uint32 DX11_PS_CS_UAV_REGISTER_COUNT = 8;

    // =========================================================================
    // DX11 Slot Mapping Result
    // =========================================================================
    struct DX11SlotMapping
    {
        // CBV (Constant Buffer) slots per stage
        std::unordered_map<std::string, uint32> cbvSlots;

        // SRV (Shader Resource View) slots per stage
        std::unordered_map<std::string, uint32> srvSlots;

        // Sampler slots per stage
        std::unordered_map<std::string, uint32> samplerSlots;

        // UAV (Unordered Access View) slots - only valid for CS/PS
        std::unordered_map<std::string, uint32> uavSlots;

        // Mapping from Vulkan-style (set, binding) to DX11 slot
        // Key: (set << 16) | binding
        std::unordered_map<uint32, uint32> setBindingToCBV;
        std::unordered_map<uint32, uint32> setBindingToSRV;
        std::unordered_map<uint32, uint32> setBindingToSampler;
        std::unordered_map<uint32, uint32> setBindingToUAV;

        static uint32 MakeKey(uint32 set, uint32 binding)
        {
            return (set << 16) | (binding & 0xFFFF);
        }
    };

    // =========================================================================
    // DX11 Slot Mapper
    // =========================================================================
    class DX11SlotMapper
    {
    public:
        /** @brief Map from Vulkan-style reflection to DX11 slots */
        static DX11SlotMapping MapFromReflection(const ShaderReflection& reflection)
        {
            DX11SlotMapping mapping;

            // Track next available slot for each type
            uint32 nextCBV = 0;
            uint32 nextSRV = 0;
            uint32 nextSampler = 0;
            uint32 nextUAV = 0;

            // Sort resources by set, then by binding for consistent mapping
            auto sortedResources = reflection.resources;
            std::sort(sortedResources.begin(), sortedResources.end(),
                [](const ShaderReflection::ResourceBinding& a,
                   const ShaderReflection::ResourceBinding& b)
                {
                    if (a.set != b.set) return a.set < b.set;
                    return a.binding < b.binding;
                });

            for (const auto& res : sortedResources)
            {
                uint32 key = DX11SlotMapping::MakeKey(res.set, res.binding);

                switch (res.type)
                {
                    case RHIBindingType::UniformBuffer:
                    {
                        if (nextCBV < DX11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT)
                        {
                            mapping.cbvSlots[res.name] = nextCBV;
                            mapping.setBindingToCBV[key] = nextCBV;
                            nextCBV++;
                        }
                        break;
                    }

                    case RHIBindingType::SampledTexture:
                    case RHIBindingType::CombinedTextureSampler:
                    {
                        if (nextSRV < DX11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT)
                        {
                            mapping.srvSlots[res.name] = nextSRV;
                            mapping.setBindingToSRV[key] = nextSRV;
                            nextSRV++;
                        }

                        // Combined texture samplers also need a sampler slot
                        if (res.type == RHIBindingType::CombinedTextureSampler)
                        {
                            if (nextSampler < DX11_COMMONSHADER_SAMPLER_SLOT_COUNT)
                            {
                                mapping.samplerSlots[res.name] = nextSampler;
                                mapping.setBindingToSampler[key] = nextSampler;
                                nextSampler++;
                            }
                        }
                        break;
                    }

                    case RHIBindingType::Sampler:
                    {
                        if (nextSampler < DX11_COMMONSHADER_SAMPLER_SLOT_COUNT)
                        {
                            mapping.samplerSlots[res.name] = nextSampler;
                            mapping.setBindingToSampler[key] = nextSampler;
                            nextSampler++;
                        }
                        break;
                    }

                    case RHIBindingType::StorageBuffer:
                    case RHIBindingType::StorageTexture:
                    {
                        // UAVs are only available in compute and pixel shaders
                        if (nextUAV < DX11_PS_CS_UAV_REGISTER_COUNT)
                        {
                            mapping.uavSlots[res.name] = nextUAV;
                            mapping.setBindingToUAV[key] = nextUAV;
                            nextUAV++;
                        }
                        break;
                    }

                    default:
                        break;
                }
            }

            return mapping;
        }

        /** @brief Merge mappings from multiple shader stages */
        static DX11SlotMapping MergeStages(
            const std::vector<std::pair<RHIShaderStage, ShaderReflection>>& stages)
        {
            // For DX11, we need consistent slot assignments across all stages
            // that share resources. This is typically handled by the HLSL compiler
            // using register annotations, but we can also merge here.

            DX11SlotMapping merged;

            // Track next available slot for each type
            uint32 nextCBV = 0;
            uint32 nextSRV = 0;
            uint32 nextSampler = 0;
            uint32 nextUAV = 0;

            // Collect all unique resources by name
            std::unordered_map<std::string, ShaderReflection::ResourceBinding> allResources;

            for (const auto& [stage, reflection] : stages)
            {
                for (const auto& res : reflection.resources)
                {
                    auto it = allResources.find(res.name);
                    if (it == allResources.end())
                    {
                        allResources[res.name] = res;
                    }
                    // If already exists, verify type matches
                }
            }

            // Sort by set/binding
            std::vector<ShaderReflection::ResourceBinding> sortedResources;
            for (const auto& [name, res] : allResources)
            {
                sortedResources.push_back(res);
            }

            std::sort(sortedResources.begin(), sortedResources.end(),
                [](const ShaderReflection::ResourceBinding& a,
                   const ShaderReflection::ResourceBinding& b)
                {
                    if (a.set != b.set) return a.set < b.set;
                    return a.binding < b.binding;
                });

            // Map each resource
            for (const auto& res : sortedResources)
            {
                uint32 key = DX11SlotMapping::MakeKey(res.set, res.binding);

                switch (res.type)
                {
                    case RHIBindingType::UniformBuffer:
                        merged.cbvSlots[res.name] = nextCBV;
                        merged.setBindingToCBV[key] = nextCBV++;
                        break;

                    case RHIBindingType::SampledTexture:
                    case RHIBindingType::CombinedTextureSampler:
                        merged.srvSlots[res.name] = nextSRV;
                        merged.setBindingToSRV[key] = nextSRV++;
                        if (res.type == RHIBindingType::CombinedTextureSampler)
                        {
                            merged.samplerSlots[res.name] = nextSampler;
                            merged.setBindingToSampler[key] = nextSampler++;
                        }
                        break;

                    case RHIBindingType::Sampler:
                        merged.samplerSlots[res.name] = nextSampler;
                        merged.setBindingToSampler[key] = nextSampler++;
                        break;

                    case RHIBindingType::StorageBuffer:
                    case RHIBindingType::StorageTexture:
                        merged.uavSlots[res.name] = nextUAV;
                        merged.setBindingToUAV[key] = nextUAV++;
                        break;

                    default:
                        break;
                }
            }

            return merged;
        }

        /** @brief Get DX11 slot for a resource */
        static uint32 GetSlot(
            const DX11SlotMapping& mapping,
            RHIBindingType type,
            uint32 set,
            uint32 binding)
        {
            uint32 key = DX11SlotMapping::MakeKey(set, binding);

            switch (type)
            {
                case RHIBindingType::UniformBuffer:
                {
                    auto it = mapping.setBindingToCBV.find(key);
                    return it != mapping.setBindingToCBV.end() ? it->second : RVX_INVALID_INDEX;
                }

                case RHIBindingType::SampledTexture:
                case RHIBindingType::CombinedTextureSampler:
                {
                    auto it = mapping.setBindingToSRV.find(key);
                    return it != mapping.setBindingToSRV.end() ? it->second : RVX_INVALID_INDEX;
                }

                case RHIBindingType::Sampler:
                {
                    auto it = mapping.setBindingToSampler.find(key);
                    return it != mapping.setBindingToSampler.end() ? it->second : RVX_INVALID_INDEX;
                }

                case RHIBindingType::StorageBuffer:
                case RHIBindingType::StorageTexture:
                {
                    auto it = mapping.setBindingToUAV.find(key);
                    return it != mapping.setBindingToUAV.end() ? it->second : RVX_INVALID_INDEX;
                }

                default:
                    return RVX_INVALID_INDEX;
            }
        }

        /** @brief Get DX11 slot for a resource by name */
        static uint32 GetSlotByName(
            const DX11SlotMapping& mapping,
            RHIBindingType type,
            const std::string& name)
        {
            switch (type)
            {
                case RHIBindingType::UniformBuffer:
                {
                    auto it = mapping.cbvSlots.find(name);
                    return it != mapping.cbvSlots.end() ? it->second : RVX_INVALID_INDEX;
                }

                case RHIBindingType::SampledTexture:
                case RHIBindingType::CombinedTextureSampler:
                {
                    auto it = mapping.srvSlots.find(name);
                    return it != mapping.srvSlots.end() ? it->second : RVX_INVALID_INDEX;
                }

                case RHIBindingType::Sampler:
                {
                    auto it = mapping.samplerSlots.find(name);
                    return it != mapping.samplerSlots.end() ? it->second : RVX_INVALID_INDEX;
                }

                case RHIBindingType::StorageBuffer:
                case RHIBindingType::StorageTexture:
                {
                    auto it = mapping.uavSlots.find(name);
                    return it != mapping.uavSlots.end() ? it->second : RVX_INVALID_INDEX;
                }

                default:
                    return RVX_INVALID_INDEX;
            }
        }
    };

} // namespace RVX

#endif // _WIN32
