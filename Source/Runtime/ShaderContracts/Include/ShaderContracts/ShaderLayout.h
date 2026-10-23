#pragma once

/** @file ShaderLayout.h @brief Runtime pipeline-layout reflection utilities */

#include "RHI/RHIDescriptor.h"
#include "ShaderContracts/ShaderProgramProvider.h"

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

    [[nodiscard]] AutoPipelineLayout BuildAutoPipelineLayout(
        const std::vector<ReflectedShader>& shaders);
} // namespace RVX
