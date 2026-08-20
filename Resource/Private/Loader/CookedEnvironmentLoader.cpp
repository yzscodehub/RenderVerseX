/** @file CookedEnvironmentLoader.cpp @brief Cooked IBL runtime loader */

#include "Resource/Loader/CookedEnvironmentLoader.h"

#include "Core/Hash/SHA256.h"
#include "Resource/Cooked/CookedEnvironmentArtifact.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <utility>

namespace RVX::Resource
{
namespace
{
    class CookedTextureResource final : public TextureResource
    {
    public:
        void PublishLoaded() { NotifyLoaded(); }
    };

    struct BuiltEnvironment
    {
        EnvironmentHandle root;
        EnvironmentResource* synchronousRoot = nullptr;
        std::array<TextureHandle, 4> textures;
    };

    bool ReadArtifact(
        const std::string& path,
        std::vector<uint8>& outBytes,
        CookedEnvironmentArtifact& outArtifact,
        std::string& outError)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            outError = "Cannot open cooked environment artifact: " + path;
            return false;
        }
        outBytes.assign(std::istreambuf_iterator<char>(file), {});
        if (outBytes.empty())
        {
            outError = "Cooked environment artifact is empty: " + path;
            return false;
        }
        return DeserializeCookedEnvironmentArtifact(
            outBytes, outArtifact, outError);
    }

    const char* RoleSuffix(CookedEnvironmentTextureRole role)
    {
        switch (role)
        {
            case CookedEnvironmentTextureRole::Environment: return "environment";
            case CookedEnvironmentTextureRole::Irradiance: return "irradiance";
            case CookedEnvironmentTextureRole::Prefiltered: return "prefiltered";
            case CookedEnvironmentTextureRole::BrdfLut: return "brdf_lut";
            default: return "unknown";
        }
    }

    std::vector<uint8> MakeTextureBytes(
        const CookedEnvironmentTexture& texture)
    {
        std::vector<uint8> bytes(texture.rgba16f.size() * sizeof(uint16));
        for (size_t index = 0; index < texture.rgba16f.size(); ++index)
        {
            bytes[index * 2u] = static_cast<uint8>(texture.rgba16f[index]);
            bytes[index * 2u + 1u] =
                static_cast<uint8>(texture.rgba16f[index] >> 8u);
        }
        return bytes;
    }

    bool BuildResources(
        const CookedEnvironmentArtifact& artifact,
        const std::string& identity,
        const std::string& requestedPath,
        ResourceId rootId,
        bool markLoaded,
        BuiltEnvironment& outBuilt,
        std::string& outError)
    {
        BuiltEnvironment built;
        for (const CookedEnvironmentTexture& cooked : artifact.textures)
        {
            const size_t roleIndex = static_cast<size_t>(cooked.role);
            if (roleIndex >= built.textures.size() || built.textures[roleIndex])
            {
                outError = "Cooked environment contains an invalid texture role";
                return false;
            }
            TextureMetadata metadata;
            metadata.width = cooked.width;
            metadata.height = cooked.height;
            metadata.depth = 1;
            metadata.mipLevels = cooked.mipLevels;
            metadata.arrayLayers = cooked.arrayLayers;
            metadata.format = TextureFormat::RGBA16F;
            metadata.isCubemap = cooked.cubemap;
            metadata.isArray = cooked.arrayLayers > 1;
            metadata.isSRGB = false;
            metadata.usage = cooked.role ==
                    CookedEnvironmentTextureRole::BrdfLut
                ? TextureUsage::Data
                : TextureUsage::Color;

            auto* cookedTexture = new CookedTextureResource();
            auto texture = TextureHandle(cookedTexture);
            const std::string suffix = RoleSuffix(cooked.role);
            texture->SetId(GenerateResourceId(identity + "#environment_" + suffix));
            texture->SetPath(requestedPath + "#" + suffix);
            texture->SetName(suffix);
            texture->SetData(MakeTextureBytes(cooked), metadata);
            if (markLoaded) cookedTexture->PublishLoaded();
            built.textures[roleIndex] = std::move(texture);
        }

        EnvironmentResourceData data;
        data.sourcePath = artifact.sourcePath;
        data.environment = built.textures[static_cast<size_t>(
            CookedEnvironmentTextureRole::Environment)];
        data.irradiance = built.textures[static_cast<size_t>(
            CookedEnvironmentTextureRole::Irradiance)];
        data.prefiltered = built.textures[static_cast<size_t>(
            CookedEnvironmentTextureRole::Prefiltered)];
        data.brdfLUT = built.textures[static_cast<size_t>(
            CookedEnvironmentTextureRole::BrdfLut)];
        data.environmentResolution = data.environment->GetWidth();
        data.irradianceResolution = data.irradiance->GetWidth();
        data.prefilteredResolution = data.prefiltered->GetWidth();
        data.prefilteredMipLevels = data.prefiltered->GetMipLevels();
        data.brdfLUTResolution = data.brdfLUT->GetWidth();
        data.intensity = artifact.intensity;

        EnvironmentResource* root = new EnvironmentResource();
        root->SetId(rootId);
        root->SetPath(requestedPath);
        root->SetName(std::filesystem::path(requestedPath).stem().string());
        const bool accepted = markLoaded
            ? root->SetData(std::move(data))
            : root->SetPreparedData(std::move(data));
        if (!accepted)
        {
            delete root;
            outError = "Cooked environment resource rejected its texture set";
            return false;
        }
        if (markLoaded)
            built.synchronousRoot = root;
        else
            built.root = EnvironmentHandle(root);
        outBuilt = std::move(built);
        outError.clear();
        return true;
    }

    ResourceContentIdentity MakeObservedIdentity(
        std::span<const uint8> bytes)
    {
        ResourceContentIdentity identity;
        identity.schemaVersion = RVX_RESOURCE_CONTENT_IDENTITY_SCHEMA_VERSION;
        identity.domain = ResourceContentIdentityDomain::CookedArtifact;
        identity.scope = ResourceContentIdentityScope::SelfContainedArtifact;
        identity.algorithm = ResourceContentHashAlgorithm::SHA256;
        identity.digest = Hash::FormatSHA256Digest(
            Hash::ComputeSHA256(bytes.data(), bytes.size()));
        identity.byteCount = bytes.size();
        identity.fileCount = 1;
        return identity;
    }
} // namespace

std::vector<std::string> CookedEnvironmentLoader::GetSupportedExtensions() const
{
    return {".rva"};
}

bool CookedEnvironmentLoader::CanLoad(const std::string& path) const
{
    std::string extension = std::filesystem::path(path).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char value)
                   {
                       return static_cast<char>(std::tolower(value));
                   });
    return extension == ".rva";
}

bool CookedEnvironmentLoader::Supports(
    const ResourceLoaderRequest& request) const
{
    return request.type == ResourceType::Environment &&
        (request.domain == ResourceLoadDomain::Cooked ||
         request.domain == ResourceLoadDomain::Package) &&
        CanLoad(request.resolvedPath);
}

IResource* CookedEnvironmentLoader::Load(const std::string& path)
{
    std::vector<uint8> bytes;
    CookedEnvironmentArtifact artifact;
    std::string error;
    if (!ReadArtifact(path, bytes, artifact, error))
        return nullptr;
    const std::string identity = CanonicalizeAssetPath(path);
    BuiltEnvironment built;
    if (identity.empty() ||
        !BuildResources(artifact, identity, path,
                        GenerateResourceId(identity), true, built, error))
    {
        return nullptr;
    }
    return built.synchronousRoot;
}

bool CookedEnvironmentLoader::Prepare(
    const ResourceLoadPreparationContext& context,
    PreparedResourceBundle& outBundle,
    ResourceLoadError& outError)
{
    if (context.IsCancellationRequested())
    {
        outError = {ResourceLoadErrorCode::Cancelled,
                    "Cooked environment load was cancelled."};
        return false;
    }
    std::vector<uint8> bytes;
    CookedEnvironmentArtifact artifact;
    std::string error;
    if (!ReadArtifact(context.resolvedPath, bytes, artifact, error))
    {
        outError = {ResourceLoadErrorCode::LoaderFailure, std::move(error)};
        return false;
    }
    const std::string& identity = context.resourceIdentityPath.empty()
        ? context.assetKey.canonicalPath
        : context.resourceIdentityPath;
    BuiltEnvironment built;
    if (!BuildResources(artifact, identity, context.requestedPath,
                        context.rootResourceId, false, built, error))
    {
        outError = {ResourceLoadErrorCode::LoaderFailure, std::move(error)};
        return false;
    }
    for (const TextureHandle& texture : built.textures)
    {
        if (!outBundle.AddDependency(ResourceHandle<IResource>(texture)))
        {
            outError = {ResourceLoadErrorCode::LoaderFailure,
                        "Cooked environment dependency bundle is invalid."};
            return false;
        }
    }
    if (!outBundle.SetRoot(ResourceHandle<IResource>(built.root)) ||
        !outBundle.SetObservedContentIdentity(MakeObservedIdentity(bytes)))
    {
        outError = {ResourceLoadErrorCode::LoaderFailure,
                    "Cooked environment root bundle is invalid."};
        return false;
    }
    return true;
}
} // namespace RVX::Resource
