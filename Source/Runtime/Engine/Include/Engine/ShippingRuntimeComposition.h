#pragma once

/**
 * @file ShippingRuntimeComposition.h
 * @brief Cooked-only dependency composition for shipping products.
 */

#include "Engine/EngineRuntimeDependencies.h"
#include "Resource/ResourceManager.h"

#include <filesystem>

namespace RVX
{
    /**
     * @brief Create the runtime dependency bundle used by shipping products.
     * @param cookedShaderRoot Root containing strict shader artifact v2 files.
     * @return Move-only dependencies with cooked resource and shader providers.
     */
    [[nodiscard]] EngineRuntimeDependencies
    CreateShippingEngineRuntimeDependencies(
        std::filesystem::path cookedShaderRoot);

    /**
     * @brief Create the fail-closed Resource configuration used by shipping products.
     * @param cookedResourceRoot Root containing cooked Resource artifacts.
     * @return Runtime-only configuration that forbids source reads and hot reload.
     */
    [[nodiscard]] Resource::ResourceManagerConfig
    CreateShippingResourceManagerConfig(
        std::filesystem::path cookedResourceRoot);
} // namespace RVX
