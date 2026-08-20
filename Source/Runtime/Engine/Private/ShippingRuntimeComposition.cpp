/**
 * @file ShippingRuntimeComposition.cpp
 * @brief Cooked-only dependency composition for shipping products.
 */

#include "Engine/ShippingRuntimeComposition.h"

#include "Resource/ResourceManager.h"
#include "ShaderRuntime/CookedShaderProgramProvider.h"

#include <utility>

namespace RVX
{
    EngineRuntimeDependencies CreateShippingEngineRuntimeDependencies(
        std::filesystem::path cookedShaderRoot)
    {
        EngineRuntimeDependencies dependencies;
        dependencies.resourceManager.emplace(
            Resource::CreateRuntimeResourceManagerDependencies(
                Resource::ResourceManager::Get()));
        dependencies.shaderPrograms =
            CreateCookedShaderProgramProviderFactory(
                std::move(cookedShaderRoot));
        return dependencies;
    }

    Resource::ResourceManagerConfig CreateShippingResourceManagerConfig(
        std::filesystem::path cookedResourceRoot)
    {
        Resource::ResourceManagerConfig config =
            Resource::MakeResourceManagerConfigForAppMode(AppMode::Runtime);
        const std::string root = cookedResourceRoot.lexically_normal().generic_string();
        config.basePath = root;
        config.runtimePolicy.cookedRoot = root;
        config.enableHotReload = false;
        return config;
    }
} // namespace RVX
