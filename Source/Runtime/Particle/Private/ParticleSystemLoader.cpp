/** @file ParticleSystemLoader.cpp @brief Cooked-only CPU billboard particle loader. */

#include "Particle/ParticleSystemLoader.h"

#include "Particle/Emitters/PointEmitter.h"
#include "Particle/Modules/LightsModule.h"
#include "Particle/Modules/TextureSheetModule.h"
#include "Resource/Cooked/CookedParticleEffectArtifact.h"
#include "Resource/Cooked/CookedTextureArtifact.h"
#include "Resource/ResourceLoadOperation.h"

#include "Core/Hash/SHA256.h"
#include "Core/Log.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace RVX::Particle
{
namespace
{
    constexpr uint64 MaxEffectArtifactBytes = 64ull * 1024ull * 1024ull;
    constexpr uint64 MaxAtlasArtifactBytes = 512ull * 1024ull * 1024ull;

    bool ReadBytes(const std::filesystem::path& path,
                   uint64 maxBytes,
                   std::vector<uint8>& outBytes,
                   std::string& outError)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open())
        {
            outError = "Cannot open cooked particle artifact: " + path.string();
            return false;
        }
        const std::streamsize byteCount = file.tellg();
        if (byteCount <= 0 || static_cast<uint64>(byteCount) > maxBytes)
        {
            outError = "Cooked particle artifact byte count is invalid: " + path.string();
            return false;
        }
        file.seekg(0, std::ios::beg);
        outBytes.resize(static_cast<size_t>(byteCount));
        if (!file.read(reinterpret_cast<char*>(outBytes.data()), byteCount))
        {
            outError = "Failed to read cooked particle artifact: " + path.string();
            return false;
        }
        return true;
    }

    bool ResolveContainedDependency(const std::filesystem::path& productDirectory,
                                    const std::string& relativeValue,
                                    std::string_view expectedExtension,
                                    std::filesystem::path& outPath)
    {
        const std::filesystem::path relativePath(relativeValue);
        if (relativeValue.empty() || relativeValue.find('\0') != std::string::npos ||
            relativeValue.find('\\') != std::string::npos || relativeValue.front() == '/' ||
            (relativeValue.size() >= 2u && relativeValue[1] == ':' &&
             ((relativeValue[0] >= 'A' && relativeValue[0] <= 'Z') ||
              (relativeValue[0] >= 'a' && relativeValue[0] <= 'z'))) ||
            relativePath.is_absolute() || relativePath.has_root_name() ||
            relativePath.has_root_path() || relativePath.generic_string() != relativeValue)
        {
            return false;
        }
        for (const std::filesystem::path& component : relativePath)
        {
            if (component.empty() || component == "." || component == "..")
            {
                return false;
            }
        }

        std::error_code error;
        const std::filesystem::path canonicalRoot =
            std::filesystem::weakly_canonical(productDirectory, error);
        if (error)
        {
            return false;
        }
        const std::filesystem::path candidate = std::filesystem::weakly_canonical(
            canonicalRoot / relativePath, error);
        if (error)
        {
            return false;
        }
        const std::filesystem::path relativeCandidate =
            std::filesystem::relative(candidate, canonicalRoot, error);
        if (error || relativeCandidate.empty() || relativeCandidate.is_absolute() ||
            relativeCandidate.has_root_path())
        {
            return false;
        }
        for (const std::filesystem::path& component : relativeCandidate)
        {
            if (component == "..")
            {
                return false;
            }
        }

        std::string extension = candidate.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
            [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        if (extension != expectedExtension)
        {
            return false;
        }
        outPath = candidate;
        return true;
    }

    Resource::ResourceContentIdentity MakeObservedIdentity(
        std::span<const uint8> bytes)
    {
        Resource::ResourceContentIdentity identity;
        identity.schemaVersion = Resource::RVX_RESOURCE_CONTENT_IDENTITY_SCHEMA_VERSION;
        identity.domain = Resource::ResourceContentIdentityDomain::CookedArtifact;
        identity.scope = Resource::ResourceContentIdentityScope::SelfContainedArtifact;
        identity.algorithm = Resource::ResourceContentHashAlgorithm::SHA256;
        identity.digest = Hash::FormatSHA256Digest(
            Hash::ComputeSHA256(bytes.data(), bytes.size()));
        identity.byteCount = bytes.size();
        identity.fileCount = 1;
        return identity;
    }

    std::string BuildAtlasDependencyIdentity(
        const std::filesystem::path& texturePath,
        const Resource::TextureMetadata& metadata,
        const Hash::SHA256Digest& expectedContentHash)
    {
        return Resource::CanonicalizeAssetPath(texturePath.string()) +
            "|sha256=" + Hash::FormatSHA256Digest(expectedContentHash) +
            "|usage=" + std::to_string(static_cast<uint32>(metadata.usage)) +
            "|srgb=" + std::to_string(metadata.isSRGB ? 1u : 0u);
    }

    bool BuildParticleSystem(const Resource::CookedParticleEffectArtifact& artifact,
                             const std::filesystem::path& atlasPath,
                             const Resource::ResourceHandle<Resource::TextureResource>& atlasTexture,
                             ParticleSystem::Ptr& outSystem,
                             std::string& outError)
    {
        if (!atlasTexture)
        {
            outError = "Cooked particle effect atlas dependency is missing";
            return false;
        }
        auto system = ParticleSystem::Create(artifact.logicalAssetId);
        system->id = Resource::GenerateResourceId(artifact.logicalAssetId);
        system->maxParticles = artifact.emitter.maxParticles;
        system->duration = artifact.emitter.durationSeconds;
        system->looping = artifact.emitter.looping;
        system->prewarm = false;
        system->startDelay = 0.0f;
        system->simulationSpeed = 1.0f;
        system->renderMode = ParticleRenderMode::Billboard;
        system->sortMode = ParticleSortMode::ByDistance;
        system->texturePath = atlasPath.generic_string();
        system->softParticleConfig.enabled =
            artifact.softDepth.policy != Resource::CookedParticleEffectSoftDepthPolicy::Disabled;
        system->softParticleConfig.fadeDistance = artifact.softDepth.fadeDistanceMeters;
        system->softParticleConfig.contrastPower = artifact.softDepth.contrastPower;
        ParticleRenderEffectDescriptor& renderEffect = system->renderEffect;
        renderEffect.flipbook.textureAssetId.value = atlasTexture->GetId();
        renderEffect.flipbook.tiles = {
            artifact.flipbook.tilesX, artifact.flipbook.tilesY};
        renderEffect.flipbook.firstFrame = artifact.flipbook.firstFrame;
        renderEffect.flipbook.frameCount = artifact.flipbook.frameCount;
        renderEffect.flipbook.framesPerSecond = artifact.flipbook.framesPerSecond;
        renderEffect.flipbook.uvOffset = artifact.flipbook.uvOffset;
        renderEffect.flipbook.uvScale = artifact.flipbook.uvScale;
        renderEffect.flipbook.required = true;
        switch (artifact.softDepth.policy)
        {
            case Resource::CookedParticleEffectSoftDepthPolicy::Disabled:
                renderEffect.softDepthMode = ParticleRenderSoftDepthMode::Disabled;
                break;
            case Resource::CookedParticleEffectSoftDepthPolicy::Optional:
                renderEffect.softDepthMode = ParticleRenderSoftDepthMode::Optional;
                break;
            case Resource::CookedParticleEffectSoftDepthPolicy::Required:
                renderEffect.softDepthMode = ParticleRenderSoftDepthMode::Required;
                break;
            default:
                outError = "Cooked particle effect contains an unsupported soft-depth policy";
                return false;
        }
        renderEffect.softDepthFadeMeters = artifact.softDepth.fadeDistanceMeters;
        renderEffect.softDepthContrast = artifact.softDepth.contrastPower;
        renderEffect.distortionStrengthPixels = artifact.distortion.distortionStrengthPixels;
        renderEffect.reactiveContribution = artifact.reactiveMask.reactiveContribution;
        renderEffect.lightHint.enabled = artifact.lightHint.enabled;
        renderEffect.lightHint.color = artifact.lightHint.color;
        renderEffect.lightHint.intensityCandela = artifact.lightHint.intensityCandela;
        renderEffect.lightHint.rangeMeters = artifact.lightHint.rangeMeters;
        renderEffect.lightHint.flickerFrequencyHz = artifact.lightHint.flickerFrequencyHz;
        renderEffect.contentHash = renderEffect.ComputeContentHash();
        if (!renderEffect.IsStructurallyValid())
        {
            outError = "Cooked particle effect could not form a valid render-effect descriptor";
            return false;
        }
        switch (artifact.blendMode)
        {
            case Resource::CookedParticleEffectBlendMode::AlphaBlend:
                system->blendMode = ParticleBlendMode::AlphaBlend;
                break;
            case Resource::CookedParticleEffectBlendMode::Premultiplied:
                system->blendMode = ParticleBlendMode::Premultiplied;
                break;
            case Resource::CookedParticleEffectBlendMode::Additive:
                system->blendMode = ParticleBlendMode::Additive;
                break;
            default:
                outError = "Cooked particle effect contains an unsupported blend mode";
                return false;
        }

        PointEmitter* emitter = system->AddEmitter<PointEmitter>();
        emitter->emissionRate = artifact.emitter.emissionRate;
        emitter->initialLifetime = {artifact.emitter.lifetimeMin, artifact.emitter.lifetimeMax};
        emitter->initialSpeed = {artifact.emitter.speedMin, artifact.emitter.speedMax};
        // Cooked Effect V1 intentionally uses PointEmitter's fixed Y-up
        // direction rather than advertising an unconsumed vector range.
        emitter->useShapeVelocity = false;
        emitter->initialColor = {artifact.emitter.initialColor};
        emitter->initialSize = {artifact.emitter.sizeMin, artifact.emitter.sizeMax};
        emitter->initialRotation = {
            artifact.emitter.rotationDegreesMin, artifact.emitter.rotationDegreesMax};
        emitter->rotationSpeed = {
            artifact.emitter.rotationSpeedDegreesMin,
            artifact.emitter.rotationSpeedDegreesMax};
        emitter->position = artifact.emitter.position;

        TextureSheetModule* flipbook = system->AddModule<TextureSheetModule>();
        flipbook->tiles = {artifact.flipbook.tilesX, artifact.flipbook.tilesY};
        flipbook->frameCount = artifact.flipbook.frameCount;
        flipbook->timeMode = TextureSheetTimeMode::Speed;
        flipbook->frameRate = artifact.flipbook.framesPerSecond;
        flipbook->startFrame = artifact.flipbook.firstFrame;
        flipbook->randomStartFrame = false;
        flipbook->cycles = 1.0f;

        if (artifact.lightHint.enabled)
        {
            LightsModule* light = system->AddModule<LightsModule>();
            light->intensity = artifact.lightHint.intensityCandela;
            light->range = artifact.lightHint.rangeMeters;
            light->ratio = 1.0f;
            light->maxLights = 1u;
            light->useParticleColor = false;
            light->lightColor = Vec4(artifact.lightHint.color, 1.0f);
            light->randomDistribution = false;
            light->affectsSpecular = false;
            light->castShadows = false;
        }

        outSystem = std::move(system);
        outError.clear();
        return true;
    }
} // namespace

bool ParticleSystemLoader::Supports(
    const Resource::ResourceLoaderRequest& request) const noexcept
{
    try
    {
        std::string extension =
            std::filesystem::path(request.resolvedPath).extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
            [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        return request.type == ResourceType_ParticleSystem &&
            (request.domain == Resource::ResourceLoadDomain::Cooked ||
             request.domain == Resource::ResourceLoadDomain::Package) &&
            extension == ".rvxvfx";
    }
    catch (...)
    {
        return false;
    }
}

bool ParticleSystemLoader::Prepare(
    const Resource::ResourceLoadPreparationContext& context,
    Resource::PreparedResourceBundle& outBundle,
    Resource::ResourceLoadError& outError)
{
    if (context.IsCancellationRequested())
    {
        outError = {Resource::ResourceLoadErrorCode::Cancelled,
                    "Cooked particle effect load was cancelled."};
        return false;
    }

    std::vector<uint8> effectBytes;
    Resource::CookedParticleEffectArtifact artifact;
    std::string error;
    const std::filesystem::path effectPath(context.resolvedPath);
    if (!ReadBytes(effectPath, MaxEffectArtifactBytes, effectBytes, error) ||
        !Resource::DeserializeCookedParticleEffectArtifact(effectBytes, artifact, error))
    {
        outError = {Resource::ResourceLoadErrorCode::LoaderFailure, std::move(error)};
        return false;
    }
    if (context.IsCancellationRequested())
    {
        outError = {Resource::ResourceLoadErrorCode::Cancelled,
                    "Cooked particle effect load was cancelled."};
        return false;
    }

    std::filesystem::path atlasPath;
    if (!ResolveContainedDependency(effectPath.parent_path(),
                                   artifact.flipbook.textureArtifactPath,
                                   ".rva", atlasPath))
    {
        outError = {Resource::ResourceLoadErrorCode::LoaderFailure,
                    "Cooked particle effect atlas must be a contained package-relative .rva product."};
        return false;
    }
    std::vector<uint8> atlasBytes;
    Resource::CookedTextureArtifact atlasArtifact;
    if (!ReadBytes(atlasPath, MaxAtlasArtifactBytes, atlasBytes, error))
    {
        outError = {Resource::ResourceLoadErrorCode::LoaderFailure, std::move(error)};
        return false;
    }
    if (context.IsCancellationRequested())
    {
        outError = {Resource::ResourceLoadErrorCode::Cancelled,
                    "Cooked particle effect load was cancelled while reading its atlas."};
        return false;
    }
    if (Hash::ComputeSHA256(atlasBytes.data(), atlasBytes.size()) !=
        artifact.flipbook.expectedContentHash)
    {
        outError = {Resource::ResourceLoadErrorCode::LoaderFailure,
                    "Cooked particle effect atlas content hash does not match its bound dependency."};
        return false;
    }
    if (context.IsCancellationRequested())
    {
        outError = {Resource::ResourceLoadErrorCode::Cancelled,
                    "Cooked particle effect load was cancelled while hashing its atlas."};
        return false;
    }
    if (!Resource::DeserializeCookedTextureArtifact(atlasBytes, atlasArtifact, error))
    {
        outError = {Resource::ResourceLoadErrorCode::LoaderFailure, std::move(error)};
        return false;
    }
    if (context.IsCancellationRequested())
    {
        outError = {Resource::ResourceLoadErrorCode::Cancelled,
                    "Cooked particle effect load was cancelled while parsing its atlas."};
        return false;
    }
    if (atlasArtifact.metadata.usage != Resource::TextureUsage::Color)
    {
        outError = {Resource::ResourceLoadErrorCode::LoaderFailure,
                    "Cooked particle effect atlas must be a color texture."};
        return false;
    }
    const uint64 tilesX = static_cast<uint64>(artifact.flipbook.tilesX);
    const uint64 tilesY = static_cast<uint64>(artifact.flipbook.tilesY);
    if (atlasArtifact.metadata.width < tilesX ||
        atlasArtifact.metadata.height < tilesY ||
        atlasArtifact.metadata.width % tilesX != 0u ||
        atlasArtifact.metadata.height % tilesY != 0u)
    {
        outError = {Resource::ResourceLoadErrorCode::LoaderFailure,
                    "Cooked particle effect atlas dimensions must contain an integral flipbook tile grid."};
        return false;
    }
    if (context.IsCancellationRequested())
    {
        outError = {Resource::ResourceLoadErrorCode::Cancelled,
                    "Cooked particle effect load was cancelled."};
        return false;
    }

    const std::string atlasIdentity = BuildAtlasDependencyIdentity(
        atlasPath, atlasArtifact.metadata, artifact.flipbook.expectedContentHash);
    const Resource::ResourceId atlasId = Resource::GenerateResourceId(atlasIdentity);
    auto atlasTexture = Resource::ResourceHandle<Resource::TextureResource>(
        new Resource::TextureResource());
    atlasTexture->SetId(atlasId);
    atlasTexture->SetPath(atlasPath.generic_string());
    atlasTexture->SetName(atlasPath.stem().string());
    atlasTexture->SetData(std::move(atlasArtifact.pixels), atlasArtifact.metadata);

    ParticleSystem::Ptr system;
    if (!BuildParticleSystem(artifact, atlasPath, atlasTexture, system, error))
    {
        outError = {Resource::ResourceLoadErrorCode::LoaderFailure, std::move(error)};
        return false;
    }
    auto root = Resource::ResourceHandle<ParticleSystemResource>(
        new ParticleSystemResource(std::move(system), atlasId, atlasIdentity));
    root->SetId(context.rootResourceId);
    root->SetPath(context.requestedPath);
    root->SetName(std::filesystem::path(context.requestedPath).stem().string());
    if (!outBundle.AddDependency(Resource::ResourceHandle<Resource::IResource>(atlasTexture)) ||
        !outBundle.SetRoot(Resource::ResourceHandle<Resource::IResource>(root)) ||
        !outBundle.SetObservedContentIdentity(MakeObservedIdentity(effectBytes)))
    {
        outError = {Resource::ResourceLoadErrorCode::LoaderFailure,
                    "Cooked particle effect could not form a prepared dependency bundle."};
        return false;
    }
    return true;
}

std::vector<Resource::ResourceId> ParticleSystemResource::GetRequiredDependencies() const
{
    const Resource::ResourceId atlasId = GetAtlasTextureResourceId();
    return atlasId == Resource::InvalidResourceId
        ? std::vector<Resource::ResourceId>{}
        : std::vector<Resource::ResourceId>{atlasId};
}

void RegisterParticleTypes()
{
    RVX_CORE_INFO("Particle: cooked-only particle effect resource type registered");
}
} // namespace RVX::Particle
