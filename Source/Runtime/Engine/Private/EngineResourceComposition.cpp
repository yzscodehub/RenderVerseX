/** @file EngineResourceComposition.cpp @brief Runtime feature loader composition. */

#include "Engine/EngineRuntimeDependencies.h"

#include "Core/Assert.h"
#include "Particle/ParticleSystemLoader.h"
#include "Resource/ResourceManager.h"

#include <memory>
#include <string>

namespace RVX
{
ResourceManagerDependencies CreateEngineResourceManagerDependencies()
{
    ResourceManagerDependencies dependencies =
        Resource::CreateRuntimeResourceManagerDependencies();
    std::string error;
    const bool added = dependencies.loaderCatalog.Add(
        std::make_unique<Particle::ParticleSystemLoader>(), error);
    RVX_ASSERT_MSG(added,
                   "Engine particle resource dependency composition failed: {}",
                   error);
    return dependencies;
}
} // namespace RVX
