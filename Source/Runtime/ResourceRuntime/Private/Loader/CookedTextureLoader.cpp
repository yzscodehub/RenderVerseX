/** @file CookedTextureLoader.cpp @brief Cooked/package texture loader */

#include "Resource/Loader/CookedTextureLoader.h"

#include "Core/Hash/SHA256.h"
#include "Resource/Cooked/CookedTextureArtifact.h"
#include "Resource/ResourceCache.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <utility>

namespace RVX::Resource
{
namespace
{
    bool ReadArtifact(
        const std::string& path,
        std::vector<uint8>& outBytes,
        CookedTextureArtifact& outArtifact,
        std::string& outError)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            outError = "Cannot open cooked texture artifact: " + path;
            return false;
        }
        outBytes.assign(std::istreambuf_iterator<char>(file), {});
        if (outBytes.empty())
        {
            outError = "Cooked texture artifact is empty: " + path;
            return false;
        }
        return DeserializeCookedTextureArtifact(
            std::span<const uint8>(outBytes), outArtifact, outError);
    }

    ResourceContentIdentity MakeObservedIdentity(std::span<const uint8> bytes)
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

    std::unique_ptr<TextureResource> BuildTexture(
        CookedTextureArtifact artifact,
        ResourceId id,
        const std::string& path,
        const std::string& name)
    {
        auto texture = std::make_unique<TextureResource>();
        texture->SetId(id);
        texture->SetPath(path);
        texture->SetName(name);
        texture->SetData(
            std::move(artifact.pixels), artifact.metadata);
        return texture;
    }
} // namespace

std::vector<std::string> CookedTextureLoader::GetSupportedExtensions() const
{
    return {".rva"};
}

bool CookedTextureLoader::Supports(
    const ResourceLoaderRequest& request) const noexcept
{
    try
    {
        std::string extension =
            std::filesystem::path(request.resolvedPath).extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char value)
                       {
                           return static_cast<char>(std::tolower(value));
                       });
        return request.type == ResourceType::Texture &&
            (request.domain == ResourceLoadDomain::Cooked ||
             request.domain == ResourceLoadDomain::Package) &&
            extension == ".rva";
    }
    catch (...)
    {
        return false;
    }
}

bool CookedTextureLoader::Prepare(
    const ResourceLoadPreparationContext& context,
    PreparedResourceBundle& outBundle,
    ResourceLoadError& outError)
{
    if (context.IsCancellationRequested())
    {
        outError = {ResourceLoadErrorCode::Cancelled,
                    "Cooked texture load was cancelled."};
        return false;
    }
    std::vector<uint8> bytes;
    CookedTextureArtifact artifact;
    std::string error;
    if (!ReadArtifact(context.resolvedPath, bytes, artifact, error))
    {
        outError = {ResourceLoadErrorCode::LoaderFailure, std::move(error)};
        return false;
    }
    std::unique_ptr<TextureResource> texture = BuildTexture(
        std::move(artifact),
        context.rootResourceId,
        context.requestedPath,
        std::filesystem::path(context.requestedPath).stem().string());
    if (!outBundle.SetRoot(ResourceHandle<IResource>(texture.release())) ||
        !outBundle.SetObservedContentIdentity(MakeObservedIdentity(bytes)))
    {
        outError = {ResourceLoadErrorCode::LoaderFailure,
                    "Cooked texture prepared bundle is invalid."};
        return false;
    }
    return true;
}
} // namespace RVX::Resource
