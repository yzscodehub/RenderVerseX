#pragma once

/**
 * @file ParticleSystemLoader.h
 * @brief Resource loader for particle systems
 */

#include "Particle/ParticleSystem.h"
#include "Resource/IResource.h"
#include "Resource/ResourceManager.h"

#include <string>
#include <utility>
#include <vector>

namespace RVX::Particle
{
    /// Custom resource type for particle systems
    constexpr Resource::ResourceType ResourceType_ParticleSystem = 
        static_cast<Resource::ResourceType>(static_cast<uint32>(Resource::ResourceType::Custom) + 1);

    /**
     * @brief Resource wrapper for particle systems
     */
    class ParticleSystemResource : public Resource::IResource
    {
    public:
        static constexpr Resource::ResourceType StaticResourceType =
            ResourceType_ParticleSystem;
        ParticleSystemResource() = default;
        explicit ParticleSystemResource(
            ParticleSystem::Ptr system,
            Resource::ResourceId atlasResourceId = Resource::InvalidResourceId,
            std::string atlasDependencyIdentity = {})
            : m_system(std::move(system))
            , m_atlasResourceId(atlasResourceId)
            , m_atlasDependencyIdentity(std::move(atlasDependencyIdentity)) {}

        Resource::ResourceType GetType() const override { return ResourceType_ParticleSystem; }
        const char* GetTypeName() const override { return "ParticleSystem"; }
        
        ParticleSystem::Ptr GetSystem() const { return m_system; }
        void SetSystem(ParticleSystem::Ptr system) { m_system = system; }
        [[nodiscard]] Resource::ResourceId GetAtlasTextureResourceId() const noexcept
        {
            return m_atlasResourceId;
        }
        [[nodiscard]] const std::string& GetAtlasDependencyIdentity() const noexcept
        {
            return m_atlasDependencyIdentity;
        }
        [[nodiscard]] std::vector<Resource::ResourceId> GetRequiredDependencies() const override;

    private:
        ParticleSystem::Ptr m_system;
        // The ResourceManager dependency graph owns the canonical atlas object.
        // Keeping only its stable identity prevents a second effect preparation
        // from retaining an unpublished duplicate handle after cache deduplication.
        Resource::ResourceId m_atlasResourceId = Resource::InvalidResourceId;
        std::string m_atlasDependencyIdentity;
    };

    /**
     * @brief Loader for particle system assets
     */
    class ParticleSystemLoader : public Resource::IResourceLoader
    {
    public:
        ParticleSystemLoader() = default;
        ~ParticleSystemLoader() override = default;

        // =====================================================================
        // IResourceLoader Interface
        // =====================================================================

        Resource::ResourceType GetResourceType() const override 
        { 
            return ResourceType_ParticleSystem; 
        }

        std::vector<std::string> GetSupportedExtensions() const override
        {
            return { ".rvxvfx" };
        }

        bool Supports(
            const ResourceLoaderRequest& request) const noexcept override;
        bool Prepare(
            const Resource::ResourceLoadPreparationContext& context,
            Resource::PreparedResourceBundle& outBundle,
            Resource::ResourceLoadError& outError) override;
    };

    /**
     * @brief Register all particle types for serialization
     */
    void RegisterParticleTypes();

} // namespace RVX::Particle
