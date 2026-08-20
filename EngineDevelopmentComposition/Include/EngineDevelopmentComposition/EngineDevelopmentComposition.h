#pragma once

/** @file EngineDevelopmentComposition.h @brief Development runtime wiring */

#include "Engine/Engine.h"
#include "Resource/ResourceSubsystem.h"

#include <filesystem>

namespace RVX
{
    /**
     * @brief Install the source-compiling shader provider into an engine config.
     *
     * An empty source root enables the provider's bounded workspace search.
     */
    void ConfigureEngineDevelopmentShaders(
        EngineConfig& config,
        std::filesystem::path shaderSourceRoot = {});

    [[nodiscard]] ShaderProgramProviderFactory
        CreateDevelopmentShaderProgramProviderFactory(
            std::filesystem::path shaderSourceRoot = {});

    /** @brief Build the explicit runtime + source dependency bundle. */
    [[nodiscard]] ResourceManagerDependencies
        CreateDevelopmentResourceManagerDependencies();

    /** @brief Configure a ResourceSubsystem with development source support. */
    [[nodiscard]] bool ConfigureEngineDevelopmentResources(
        Resource::ResourceSubsystem& subsystem,
        Resource::ResourceManagerConfig config = {});
} // namespace RVX
