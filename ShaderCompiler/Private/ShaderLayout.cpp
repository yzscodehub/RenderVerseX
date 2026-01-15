#include "ShaderCompiler/ShaderLayout.h"
#include "Core/Core.h"
#include <unordered_map>

namespace RVX
{
    namespace
    {
        struct BindingKey
        {
            uint32 set;
            uint32 binding;
        };

        struct BindingKeyHash
        {
            size_t operator()(const BindingKey& key) const
            {
                return (static_cast<size_t>(key.set) << 32) ^ key.binding;
            }
        };

        struct BindingKeyEq
        {
            bool operator()(const BindingKey& a, const BindingKey& b) const
            {
                return a.set == b.set && a.binding == b.binding;
            }
        };
    }

    AutoPipelineLayout BuildAutoPipelineLayout(const std::vector<ReflectedShader>& shaders)
    {
        AutoPipelineLayout result;

        std::unordered_map<BindingKey, RHIBindingLayoutEntry, BindingKeyHash, BindingKeyEq> entries;
        uint32 pushConstantSize = 0;
        RHIShaderStage pushStages = RHIShaderStage::None;

        for (const auto& shader : shaders)
        {
            for (const auto& res : shader.reflection.resources)
            {
                BindingKey key{res.set, res.binding};
                auto it = entries.find(key);
                if (it == entries.end())
                {
                    RHIBindingLayoutEntry entry{};
                    entry.binding = res.binding;
                    entry.type = res.type;
                    entry.visibility = shader.stage;
                    entry.count = res.count;
                    entry.isDynamic = false;
                    entries.emplace(key, entry);
                }
                else
                {
                    if (it->second.type != res.type || it->second.count != res.count)
                    {
                        RVX_CORE_WARN("Descriptor binding conflict at set {}, binding {}", res.set, res.binding);
                    }
                    it->second.visibility = it->second.visibility | shader.stage;
                }
            }

            for (const auto& pc : shader.reflection.pushConstants)
            {
                pushConstantSize = std::max(pushConstantSize, pc.offset + pc.size);
                pushStages = pushStages | shader.stage;
            }
        }

        uint32 setCount = 0;
        for (const auto& entry : entries)
        {
            setCount = std::max(setCount, entry.first.set + 1);
        }
        result.setLayouts.resize(setCount);

        for (const auto& entry : entries)
        {
            result.setLayouts[entry.first.set].entries.push_back(entry.second);
        }

        result.pipelineLayout.pushConstantSize = pushConstantSize;
        result.pipelineLayout.pushConstantStages = pushStages;
        return result;
    }
} // namespace RVX
