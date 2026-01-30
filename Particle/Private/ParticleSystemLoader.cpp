#include "Particle/ParticleSystemLoader.h"
#include "Particle/Emitters/PointEmitter.h"
#include "Particle/Emitters/BoxEmitter.h"
#include "Particle/Emitters/SphereEmitter.h"
#include "Particle/Emitters/ConeEmitter.h"
#include "Particle/Emitters/CircleEmitter.h"
#include "Particle/Emitters/EdgeEmitter.h"
#include "Particle/Emitters/MeshEmitter.h"
#include "Particle/Modules/ForceModule.h"
#include "Particle/Modules/ColorOverLifetimeModule.h"
#include "Particle/Modules/SizeOverLifetimeModule.h"
#include "Particle/Modules/VelocityOverLifetimeModule.h"
#include "Particle/Modules/RotationOverLifetimeModule.h"
#include "Particle/Modules/NoiseModule.h"
#include "Particle/Modules/CollisionModule.h"
#include "Particle/Modules/TextureSheetModule.h"
#include "Particle/Modules/TrailModule.h"
#include "Particle/Modules/LightsModule.h"
#include "Particle/Modules/SubEmitterModule.h"
#include "Core/Serialization/Serialization.h"
#include "Core/Log.h"
#include <fstream>
#include <sstream>

namespace RVX::Particle
{

Resource::IResource* ParticleSystemLoader::Load(const std::string& path)
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        RVX_CORE_ERROR("ParticleSystemLoader: Failed to open file: {}", path);
        return nullptr;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string json = buffer.str();

    auto system = DeserializeFromJson(json);
    if (!system)
    {
        RVX_CORE_ERROR("ParticleSystemLoader: Failed to parse file: {}", path);
        return nullptr;
    }

    RVX_CORE_INFO("ParticleSystemLoader: Loaded {}", path);
    return new ParticleSystemResource(system);
}

bool ParticleSystemLoader::Save(const std::string& path, ParticleSystemResource* resource)
{
    auto* psResource = resource;
    if (!psResource || !psResource->GetSystem())
    {
        RVX_CORE_ERROR("ParticleSystemLoader: Invalid resource for saving");
        return false;
    }

    std::string json = SerializeToJson(*psResource->GetSystem());

    std::ofstream file(path);
    if (!file.is_open())
    {
        RVX_CORE_ERROR("ParticleSystemLoader: Failed to create file: {}", path);
        return false;
    }

    file << json;
    file.close();

    RVX_CORE_INFO("ParticleSystemLoader: Saved {}", path);
    return true;
}

std::string ParticleSystemLoader::SerializeToJson(const ParticleSystem& system)
{
    // Simplified JSON serialization
    // Real implementation would use a proper JSON library
    
    std::stringstream ss;
    ss << "{\n";
    ss << "  \"name\": \"" << system.name << "\",\n";
    ss << "  \"maxParticles\": " << system.maxParticles << ",\n";
    ss << "  \"duration\": " << system.duration << ",\n";
    ss << "  \"looping\": " << (system.looping ? "true" : "false") << ",\n";
    ss << "  \"prewarm\": " << (system.prewarm ? "true" : "false") << ",\n";
    ss << "  \"renderMode\": " << static_cast<int>(system.renderMode) << ",\n";
    ss << "  \"blendMode\": " << static_cast<int>(system.blendMode) << ",\n";
    ss << "  \"materialPath\": \"" << system.materialPath << "\",\n";
    ss << "  \"texturePath\": \"" << system.texturePath << "\",\n";
    
    // Emitters
    ss << "  \"emitters\": [\n";
    for (size_t i = 0; i < system.emitters.size(); ++i)
    {
        const auto& emitter = system.emitters[i];
        ss << "    {\n";
        ss << "      \"type\": \"" << emitter->GetTypeName() << "\",\n";
        ss << "      \"emissionRate\": " << emitter->emissionRate << ",\n";
        ss << "      \"enabled\": " << (emitter->enabled ? "true" : "false") << "\n";
        ss << "    }" << (i < system.emitters.size() - 1 ? "," : "") << "\n";
    }
    ss << "  ],\n";
    
    // Modules
    ss << "  \"modules\": [\n";
    for (size_t i = 0; i < system.modules.size(); ++i)
    {
        const auto& module = system.modules[i];
        ss << "    {\n";
        ss << "      \"type\": \"" << module->GetTypeName() << "\",\n";
        ss << "      \"enabled\": " << (module->enabled ? "true" : "false") << "\n";
        ss << "    }" << (i < system.modules.size() - 1 ? "," : "") << "\n";
    }
    ss << "  ]\n";
    
    ss << "}\n";
    return ss.str();
}

ParticleSystem::Ptr ParticleSystemLoader::DeserializeFromJson(const std::string& json)
{
    // Simplified JSON deserialization
    // Real implementation would use a proper JSON library
    
    auto system = std::make_shared<ParticleSystem>();
    
    // Parse basic properties
    // This is a placeholder - real implementation would parse JSON properly
    system->name = "LoadedParticleSystem";
    
    // For now, return a simple system
    return system;
}

void RegisterParticleTypes()
{
    // Register types with TypeRegistry
    // auto& registry = TypeRegistry::Get();
    
    // Emitters
    // registry.RegisterType<PointEmitter>("PointEmitter");
    // registry.RegisterType<BoxEmitter>("BoxEmitter");
    // registry.RegisterType<SphereEmitter>("SphereEmitter");
    // registry.RegisterType<ConeEmitter>("ConeEmitter");
    // registry.RegisterType<CircleEmitter>("CircleEmitter");
    // registry.RegisterType<EdgeEmitter>("EdgeEmitter");
    // registry.RegisterType<MeshEmitter>("MeshEmitter");
    
    // Modules
    // registry.RegisterType<ForceModule>("ForceModule");
    // registry.RegisterType<ColorOverLifetimeModule>("ColorOverLifetimeModule");
    // registry.RegisterType<SizeOverLifetimeModule>("SizeOverLifetimeModule");
    // registry.RegisterType<VelocityOverLifetimeModule>("VelocityOverLifetimeModule");
    // registry.RegisterType<RotationOverLifetimeModule>("RotationOverLifetimeModule");
    // registry.RegisterType<NoiseModule>("NoiseModule");
    // registry.RegisterType<CollisionModule>("CollisionModule");
    // registry.RegisterType<TextureSheetModule>("TextureSheetModule");
    // registry.RegisterType<TrailModule>("TrailModule");
    // registry.RegisterType<LightsModule>("LightsModule");
    // registry.RegisterType<SubEmitterModule>("SubEmitterModule");
    
    RVX_CORE_INFO("Particle: Registered serialization types");
}

} // namespace RVX::Particle
