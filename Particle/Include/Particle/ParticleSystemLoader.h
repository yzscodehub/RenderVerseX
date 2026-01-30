#pragma once

/**
 * @file ParticleSystemLoader.h
 * @brief Resource loader for particle systems
 */

#include "Resource/IResource.h"
#include "Resource/ResourceManager.h"
#include "Particle/ParticleSystem.h"
#include <string>
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
        ParticleSystemResource() = default;
        explicit ParticleSystemResource(ParticleSystem::Ptr system) 
            : m_system(system) {}

        Resource::ResourceType GetType() const override { return ResourceType_ParticleSystem; }
        const char* GetTypeName() const override { return "ParticleSystem"; }
        
        ParticleSystem::Ptr GetSystem() const { return m_system; }
        void SetSystem(ParticleSystem::Ptr system) { m_system = system; }

    private:
        ParticleSystem::Ptr m_system;
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
            return { ".particle", ".vfx" };
        }

        Resource::IResource* Load(const std::string& path) override;

        // =====================================================================
        // Serialization
        // =====================================================================

        /// Save to file
        bool Save(const std::string& path, ParticleSystemResource* resource);

        /// Serialize a particle system to JSON
        static std::string SerializeToJson(const ParticleSystem& system);

        /// Deserialize a particle system from JSON
        static ParticleSystem::Ptr DeserializeFromJson(const std::string& json);
    };

    /**
     * @brief Register all particle types for serialization
     */
    void RegisterParticleTypes();

} // namespace RVX::Particle
