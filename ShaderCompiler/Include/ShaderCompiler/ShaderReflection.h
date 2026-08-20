#pragma once

#include "ShaderContracts/ShaderProgramProvider.h"

namespace RVX
{
    ShaderReflection ReflectShader(RHIBackendType backend,
                                   RHIShaderStage stage,
                                   const std::vector<uint8>& bytecode);

    /**
     * @brief Convert compiler reflection into the compact owned RHI contract.
     */
    RHIShaderInterface BuildRHIShaderInterface(
        RHIShaderStage stage,
        const ShaderReflection& reflection);
} // namespace RVX
