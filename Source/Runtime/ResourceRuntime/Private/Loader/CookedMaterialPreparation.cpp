/** @file CookedMaterialPreparation.cpp @brief Shared cooked material preparation */

#include "CookedMaterialPreparation.h"

#include "Core/Hash/SHA256.h"
#include "Resource/Cooked/CookedMaterialArtifact.h"
#include "Resource/Cooked/CookedTextureArtifact.h"
#include "Resource/ResourceLoadOperation.h"
#include "Resource/Types/MaterialResource.h"
#include "Resource/Types/TextureResource.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <span>
#include <unordered_map>
#include <utility>

namespace RVX::Resource
{
namespace
{
    constexpr uint64 MaxArtifactBytes = 512ull * 1024ull * 1024ull;

    bool ReadBytes(const std::filesystem::path& path,
                   std::vector<uint8>& outBytes,
                   std::string& outError)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open())
        {
            outError = "Cannot open cooked material dependency: " + path.string();
            return false;
        }
        const std::streamsize byteCount = file.tellg();
        if (byteCount <= 0 || static_cast<uint64>(byteCount) > MaxArtifactBytes)
        {
            outError = "Cooked material dependency byte count is invalid: " +
                path.string();
            return false;
        }
        file.seekg(0, std::ios::beg);
        outBytes.resize(static_cast<size_t>(byteCount));
        if (!file.read(reinterpret_cast<char*>(outBytes.data()), byteCount))
        {
            outError = "Failed to read cooked material dependency: " + path.string();
            return false;
        }
        return true;
    }

    bool ResolveContainedDependency(
        const std::filesystem::path& productDirectory,
        const std::string& relativeValue,
        std::string_view expectedExtension,
        std::filesystem::path& outPath)
    {
        const std::filesystem::path relativePath(relativeValue);
        if (relativeValue.empty() || relativeValue.find('\0') != std::string::npos ||
            relativeValue.find('\\') != std::string::npos ||
            relativeValue.front() == '/' ||
            (relativeValue.size() >= 2u && relativeValue[1] == ':' &&
             ((relativeValue[0] >= 'A' && relativeValue[0] <= 'Z') ||
              (relativeValue[0] >= 'a' && relativeValue[0] <= 'z'))) ||
            relativePath.is_absolute() || relativePath.has_root_name() ||
            relativePath.has_root_path() ||
            relativePath.generic_string() != relativeValue)
        {
            return false;
        }
        for (const std::filesystem::path& component : relativePath)
        {
            if (component.empty() || component == "." || component == "..")
                return false;
        }

        std::error_code error;
        const std::filesystem::path canonicalRoot =
            std::filesystem::weakly_canonical(productDirectory, error);
        if (error)
            return false;
        const std::filesystem::path candidate = std::filesystem::weakly_canonical(
            canonicalRoot / relativePath, error);
        if (error)
            return false;
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
                return false;
        }

        std::string extension = candidate.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char value)
                       {
                           return static_cast<char>(std::tolower(value));
                       });
        if (extension != expectedExtension)
            return false;
        outPath = candidate;
        return true;
    }

    const char* GetMaterialTextureSlotName(MaterialTextureSlot slot) noexcept
    {
        switch (slot)
        {
            case MaterialTextureSlot::BaseColor: return "albedo";
            case MaterialTextureSlot::Normal: return "normal";
            case MaterialTextureSlot::MetallicRoughness:
                return "metallic_roughness";
            case MaterialTextureSlot::Occlusion: return "ao";
            case MaterialTextureSlot::Emissive: return "emissive";
            case MaterialTextureSlot::ToonRamp: return "toon_ramp";
            case MaterialTextureSlot::Transmission: return "transmission";
            case MaterialTextureSlot::Thickness: return "thickness";
            default: return nullptr;
        }
    }

    bool IsSupportedRuntimeMaterial(const CookedMaterialArtifact& artifact,
                                    std::string& outError)
    {
        const MaterialStaticDescriptor& descriptor = artifact.material.descriptor;
        const bool opaqueOrMasked =
            descriptor.blendMode == MaterialBlendMode::Opaque ||
            descriptor.blendMode == MaterialBlendMode::Masked;
        const bool translucent = !opaqueOrMasked;
        const bool standardSupported =
            descriptor.shadingModel == MaterialShadingModel::StandardLit &&
            (opaqueOrMasked || descriptor.blendMode == MaterialBlendMode::AlphaBlend ||
             descriptor.blendMode == MaterialBlendMode::Premultiplied);
        const bool unlitSupported =
            descriptor.shadingModel == MaterialShadingModel::Unlit &&
            (opaqueOrMasked || descriptor.blendMode == MaterialBlendMode::AlphaBlend ||
             descriptor.blendMode == MaterialBlendMode::Premultiplied ||
             descriptor.blendMode == MaterialBlendMode::Additive);
        const bool thinSupported =
            descriptor.shadingModel == MaterialShadingModel::ThinTransmission &&
            translucent && (descriptor.blendMode == MaterialBlendMode::AlphaBlend ||
                            descriptor.blendMode == MaterialBlendMode::Premultiplied);
        const bool toonSupported =
            descriptor.shadingModel == MaterialShadingModel::ToonLit && opaqueOrMasked;
        if ((!standardSupported && !unlitSupported && !thinSupported && !toonSupported) ||
            (opaqueOrMasked
                ? descriptor.translucencyPhase != MaterialTranslucencyPhase::Opaque
                : descriptor.translucencyPhase != MaterialTranslucencyPhase::Standard) ||
            (translucent && MaterialShadowPolicyCasts(descriptor.shadowPolicy)))
        {
            outError = "Cooked material is outside the runtime surface support matrix";
            return false;
        }
        for (const CookedMaterialTextureBinding& binding : artifact.textureBindings)
        {
            if (GetMaterialTextureSlotName(binding.definition.slot) == nullptr)
            {
                outError = "Cooked material texture slot is not implemented by the runtime";
                return false;
            }
            if (binding.definition.slot == MaterialTextureSlot::ToonRamp)
            {
                outError = "Cooked ToonRamp is not implemented until the shader samples it";
                return false;
            }
            const bool linearDataSlot =
                binding.definition.slot == MaterialTextureSlot::Transmission ||
                binding.definition.slot == MaterialTextureSlot::Thickness;
            if (linearDataSlot && (binding.usage != TextureUsage::Data || binding.isSRGB))
            {
                outError = "Cooked ThinTransmission texture slots require linear data textures";
                return false;
            }
        }
        return true;
    }

    std::string BuildTextureIdentity(const std::filesystem::path& texturePath,
                                     TextureUsage usage,
                                     bool isSRGB)
    {
        return CanonicalizeAssetPath(texturePath.string()) +
            "|usage=" + std::to_string(static_cast<uint32>(usage)) +
            "|srgb=" + std::to_string(isSRGB ? 1u : 0u);
    }
} // namespace

bool PrepareCookedMaterialResources(
    const std::string& materialArtifactPath,
    const std::string& materialIdentityPath,
    ResourceId materialId,
    const std::string& materialResourcePath,
    const std::string& materialName,
    CookedMaterialPreparationCache* preparationCache,
    CookedMaterialPreparedResources& outResources,
    std::string& outError)
{
    outResources = {};
    if (materialId == InvalidResourceId || materialArtifactPath.empty() ||
        materialIdentityPath.empty())
    {
        outError = "Cooked material preparation requires a concrete artifact identity";
        return false;
    }

    CookedMaterialArtifact artifact;
    if (!ReadBytes(materialArtifactPath, outResources.artifactBytes, outError) ||
        !DeserializeCookedMaterialArtifact(
            outResources.artifactBytes, artifact, outError) ||
        !IsSupportedRuntimeMaterial(artifact, outError))
    {
        outResources = {};
        return false;
    }

    ResourceHandle<MaterialResource> material(new MaterialResource());
    material->SetId(materialId);
    material->SetPath(materialResourcePath);
    material->SetName(materialName.empty()
        ? std::filesystem::path(materialArtifactPath).stem().string()
        : materialName);
    if (!material->SetMaterialDefinition(artifact.material))
    {
        outError = "Cooked material resource rejected its canonical definition";
        outResources = {};
        return false;
    }

    const std::filesystem::path productDirectory =
        std::filesystem::path(materialArtifactPath).parent_path();
    CookedMaterialPreparationCache localCache;
    CookedMaterialPreparationCache& cache = preparationCache != nullptr
        ? *preparationCache
        : localCache;
    cache.textures.reserve(cache.textures.size() + artifact.textureBindings.size());
    cache.textureContentHashes.reserve(
        cache.textureContentHashes.size() + artifact.textureBindings.size());
    outResources.textures.reserve(artifact.textureBindings.size());
    for (const CookedMaterialTextureBinding& binding : artifact.textureBindings)
    {
        std::filesystem::path texturePath;
        if (!ResolveContainedDependency(
                productDirectory, binding.artifactPath, ".rva", texturePath))
        {
            outError = "Cooked material texture dependency must be a contained .rva product";
            outResources = {};
            return false;
        }

        const ResourceId textureId = GenerateResourceId(BuildTextureIdentity(
            texturePath, binding.usage, binding.isSRGB));
        ResourceHandle<TextureResource> texture;
        const auto existingTexture = cache.textures.find(textureId);
        const auto existingHash = cache.textureContentHashes.find(textureId);
        if ((existingTexture == cache.textures.end()) !=
            (existingHash == cache.textureContentHashes.end()))
        {
            outError =
                "Cooked material texture preparation cache is inconsistent";
            outResources = {};
            return false;
        }
        if (existingTexture != cache.textures.end())
        {
            if (!existingTexture->second ||
                existingHash->second != binding.expectedContentHash)
            {
                outError =
                    "Cooked material texture dependency content hash conflicts with the prepared cache";
                outResources = {};
                return false;
            }
            texture = existingTexture->second;
        }
        else
        {
            std::vector<uint8> textureBytes;
            CookedTextureArtifact textureArtifact;
            if (!ReadBytes(texturePath, textureBytes, outError))
            {
                outResources = {};
                return false;
            }
            const Hash::SHA256Digest observedContentHash = Hash::ComputeSHA256(
                textureBytes.data(), textureBytes.size());
            if (observedContentHash != binding.expectedContentHash)
            {
                outError =
                    "Cooked material texture dependency content hash mismatch";
                outResources = {};
                return false;
            }
            if (!DeserializeCookedTextureArtifact(
                    textureBytes, textureArtifact, outError))
            {
                outResources = {};
                return false;
            }
            if (textureArtifact.metadata.usage != binding.usage ||
                textureArtifact.metadata.isSRGB != binding.isSRGB)
            {
                outError = "Cooked material texture metadata does not match its dependency contract";
                outResources = {};
                return false;
            }

            texture = ResourceHandle<TextureResource>(new TextureResource());
            TextureResource* prepared = texture.Get();
            prepared->SetId(textureId);
            prepared->SetPath(texturePath.string());
            prepared->SetName(texturePath.stem().string());
            prepared->SetData(
                std::move(textureArtifact.pixels), textureArtifact.metadata);
            cache.textures.emplace(textureId, texture);
            cache.textureContentHashes.emplace(
                textureId, observedContentHash);
        }

        const char* textureSlotName = GetMaterialTextureSlotName(binding.definition.slot);
        if (textureSlotName == nullptr ||
            !material->SetTexture(textureSlotName, texture))
        {
            outError = "Cooked material resource rejected a prepared texture dependency";
            outResources = {};
            return false;
        }
        const bool alreadyPrepared = std::any_of(
            outResources.textures.begin(), outResources.textures.end(),
            [textureId](const ResourceHandle<TextureResource>& candidate)
            {
                return candidate && candidate->GetId() == textureId;
            });
        if (!alreadyPrepared)
            outResources.textures.push_back(texture);
    }

    outResources.material = material;
    outError.clear();
    return true;
}
} // namespace RVX::Resource
