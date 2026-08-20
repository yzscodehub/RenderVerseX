#pragma once

/** @file EngineDevelopmentComposition.h @brief Development runtime wiring */

#include "Engine/EngineRuntimeDependencies.h"

#include <filesystem>

namespace RVX
{
    [[nodiscard]] ShaderProgramProviderFactory
        CreateDevelopmentShaderProgramProviderFactory(
            std::filesystem::path shaderSourceRoot = {});

    /** @brief Build the explicit runtime + source dependency bundle. */
    [[nodiscard]] ResourceManagerDependencies
        CreateDevelopmentResourceManagerDependencies();

    /** @brief Build the complete source-enabled Engine runtime composition. */
    [[nodiscard]] EngineRuntimeDependencies
        CreateDevelopmentEngineRuntimeDependencies(
            std::filesystem::path shaderSourceRoot = {});
} // namespace RVX
