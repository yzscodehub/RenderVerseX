#pragma once

#include "ShaderContracts/ShaderProgramProvider.h"

namespace RVX
{
    ShaderReflection ReflectShader(RHIBackendType backend,
                                   RHIShaderStage stage,
                                   const std::vector<uint8>& bytecode,
                                   const char* entryPoint = nullptr);

} // namespace RVX
