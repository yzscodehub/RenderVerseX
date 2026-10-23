#pragma once

/**
 * @file EngineRuntimeDependencies.h
 * @brief Move-only services required by the runtime Engine composition.
 */

#include "ResourceRuntime/ResourceManagerDependencies.h"
#include "ShaderContracts/ShaderProgramProvider.h"

#include <optional>

namespace RVX
{
    /**
     * @brief Explicit runtime-only dependency bundle consumed by Engine.
     *
     * Developer and shipping compositions create this value outside Engine.
     * Engine therefore never discovers a source compiler, importer, or other
     * development service through a hidden fallback.
     */
    struct EngineRuntimeDependencies
    {
        std::optional<ResourceManagerDependencies> resourceManager;
        ShaderProgramProviderFactory shaderPrograms;

        EngineRuntimeDependencies() = default;
        ~EngineRuntimeDependencies() = default;
        EngineRuntimeDependencies(const EngineRuntimeDependencies&) = delete;
        EngineRuntimeDependencies& operator=(const EngineRuntimeDependencies&) = delete;
        EngineRuntimeDependencies(EngineRuntimeDependencies&&) noexcept = default;
        EngineRuntimeDependencies& operator=(EngineRuntimeDependencies&&) noexcept = default;
    };

    /**
     * @brief Compose the Engine-owned cooked runtime loaders.
     *
     * ResourceRuntime provides generic cooked resources; Engine owns the
     * feature-level Particle loader registration so ResourceRuntime does not
     * acquire an illegal dependency on a runtime feature module.
     */
    [[nodiscard]] ResourceManagerDependencies
        CreateEngineResourceManagerDependencies();
} // namespace RVX
