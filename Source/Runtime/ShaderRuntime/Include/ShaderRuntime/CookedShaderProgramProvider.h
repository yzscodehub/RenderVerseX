#pragma once

/** @file CookedShaderProgramProvider.h @brief Cooked-only shader provider */

#include "ShaderContracts/ShaderArtifact.h"

#include <filesystem>

namespace RVX
{
    /**
     * @brief Create a provider factory that reads only strict v2 artifacts.
     * @param artifactRoot Root containing backend content-addressed artifacts.
     * @param requiredCompilerAbi ABI that every loaded artifact must match.
     */
    [[nodiscard]] ShaderProgramProviderFactory
    CreateCookedShaderProgramProviderFactory(
        std::filesystem::path artifactRoot,
        uint64 requiredCompilerAbi = RVX_SHADER_COMPILER_ABI_VERSION);
} // namespace RVX
