/** @file ShaderLayout.cpp @brief Runtime pipeline-layout reflection utilities */

#include "ShaderContracts/ShaderLayout.h"

#include "Core/Log.h"

#include <algorithm>
#include <unordered_map>

namespace RVX
{
namespace
{
    enum class RegisterSpace : uint32
    {
        SRV = 0,
        Sampler = 1,
        UAV = 2,
        CBV = 3,
    };

    [[nodiscard]] RegisterSpace GetRegisterSpace(RHIBindingType type)
    {
        switch (type)
        {
            case RHIBindingType::Sampler:
                return RegisterSpace::Sampler;
            case RHIBindingType::UniformBuffer:
            case RHIBindingType::DynamicUniformBuffer:
                return RegisterSpace::CBV;
            case RHIBindingType::StorageBuffer:
            case RHIBindingType::DynamicStorageBuffer:
            case RHIBindingType::StorageTexture:
                return RegisterSpace::UAV;
            case RHIBindingType::ShaderResourceBuffer:
            case RHIBindingType::SampledTexture:
            case RHIBindingType::CombinedTextureSampler:
            default:
                return RegisterSpace::SRV;
        }
    }

    struct BindingKey
    {
        uint32 set = 0;
        uint32 binding = 0;
        RegisterSpace registerSpace = RegisterSpace::SRV;

        bool operator==(const BindingKey&) const = default;
    };

    struct BindingKeyHash
    {
        size_t operator()(const BindingKey& key) const noexcept
        {
            return (static_cast<size_t>(key.set) << 32U) ^
                (static_cast<size_t>(key.registerSpace) << 16U) ^
                key.binding;
        }
    };
} // namespace

AutoPipelineLayout BuildAutoPipelineLayout(
    const std::vector<ReflectedShader>& shaders)
{
    AutoPipelineLayout result;
    std::unordered_map<BindingKey, RHIBindingLayoutEntry, BindingKeyHash>
        entries;
    uint32 pushConstantSize = 0;
    RHIShaderStage pushStages = RHIShaderStage::None;

    for (const ReflectedShader& shader : shaders)
    {
        for (const ShaderReflection::ResourceBinding& resource :
             shader.reflection.resources)
        {
            const BindingKey key{
                resource.set,
                resource.binding,
                GetRegisterSpace(resource.type),
            };
            auto [entryIt, inserted] = entries.try_emplace(key);
            RHIBindingLayoutEntry& entry = entryIt->second;
            if (inserted)
            {
                entry.binding = resource.binding;
                entry.type = resource.type;
                entry.visibility = shader.stage;
                entry.count = resource.count;
                entry.isDynamic = false;
            }
            else
            {
                if (entry.type != resource.type ||
                    entry.count != resource.count)
                {
                    RVX_CORE_WARN(
                        "Descriptor binding conflict at set {}, binding {}",
                        resource.set,
                        resource.binding);
                }
                entry.visibility = entry.visibility | shader.stage;
            }
        }

        for (const ShaderReflection::PushConstantRange& range :
             shader.reflection.pushConstants)
        {
            pushConstantSize = std::max(
                pushConstantSize, range.offset + range.size);
            pushStages = pushStages | shader.stage;
        }
    }

    uint32 setCount = 0;
    for (const auto& [key, entry] : entries)
    {
        (void)entry;
        setCount = std::max(setCount, key.set + 1U);
    }
    result.setLayouts.resize(setCount);
    for (const auto& [key, entry] : entries)
    {
        result.setLayouts[key.set].entries.push_back(entry);
    }

    result.pipelineLayout.pushConstantSize = pushConstantSize;
    result.pipelineLayout.pushConstantStages = pushStages;
    return result;
}
} // namespace RVX
