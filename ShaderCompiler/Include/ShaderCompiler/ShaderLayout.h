#pragma once

#include "ShaderCompiler/ShaderReflection.h"
#include "RHI/RHIDescriptor.h"

namespace RVX
{
    struct ReflectedShader
    {
        ShaderReflection reflection;
        RHIShaderStage stage = RHIShaderStage::None;
    };

    struct AutoPipelineLayout
    {
        std::vector<RHIDescriptorSetLayoutDesc> setLayouts;
        RHIPipelineLayoutDesc pipelineLayout;
    };

    AutoPipelineLayout BuildAutoPipelineLayout(const std::vector<ReflectedShader>& shaders);
} // namespace RVX
