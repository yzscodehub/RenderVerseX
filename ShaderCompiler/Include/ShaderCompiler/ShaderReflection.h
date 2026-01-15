#pragma once

#include "RHI/RHIDefinitions.h"
#include <string>
#include <vector>

namespace RVX
{
    struct ShaderReflection
    {
        struct ResourceBinding
        {
            std::string name;
            uint32 set = 0;
            uint32 binding = 0;
            RHIBindingType type = RHIBindingType::UniformBuffer;
            uint32 count = 1;
        };

        struct PushConstantRange
        {
            uint32 offset = 0;
            uint32 size = 0;
        };

        struct InputAttribute
        {
            std::string semantic;
            uint32 location = 0;
            RHIFormat format = RHIFormat::Unknown;
        };

        std::vector<ResourceBinding> resources;
        std::vector<PushConstantRange> pushConstants;
        std::vector<InputAttribute> inputs;
    };

    ShaderReflection ReflectShader(RHIBackendType backend,
                                   RHIShaderStage stage,
                                   const std::vector<uint8>& bytecode);
} // namespace RVX
