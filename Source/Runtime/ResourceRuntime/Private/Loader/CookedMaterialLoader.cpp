/** @file CookedMaterialLoader.cpp @brief Cooked/package material loader */

#include "Resource/Loader/CookedMaterialLoader.h"

#include "CookedMaterialPreparation.h"

#include "Core/Hash/SHA256.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <span>
#include <utility>

namespace RVX::Resource
{
namespace
{
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
} // namespace

std::vector<std::string> CookedMaterialLoader::GetSupportedExtensions() const
{
    return {".rvmat"};
}

bool CookedMaterialLoader::Supports(
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
        return request.type == ResourceType::Material &&
            (request.domain == ResourceLoadDomain::Cooked ||
             request.domain == ResourceLoadDomain::Package) &&
            extension == ".rvmat";
    }
    catch (...)
    {
        return false;
    }
}

bool CookedMaterialLoader::Prepare(
    const ResourceLoadPreparationContext& context,
    PreparedResourceBundle& outBundle,
    ResourceLoadError& outError)
{
    if (context.IsCancellationRequested())
    {
        outError = {ResourceLoadErrorCode::Cancelled,
                    "Cooked material load was cancelled."};
        return false;
    }

    CookedMaterialPreparedResources prepared;
    CookedMaterialPreparationCache cache;
    std::string error;
    if (!PrepareCookedMaterialResources(
            context.resolvedPath,
            context.resourceIdentityPath,
            context.rootResourceId,
            context.requestedPath,
            std::filesystem::path(context.requestedPath).stem().string(),
            &cache,
            prepared,
            error))
    {
        outError = {ResourceLoadErrorCode::LoaderFailure, std::move(error)};
        return false;
    }

    for (const ResourceHandle<TextureResource>& texture : prepared.textures)
    {
        if (!texture || !outBundle.AddDependency(
                            ResourceHandle<IResource>(texture.Get())))
        {
            outError = {ResourceLoadErrorCode::LoaderFailure,
                        "Cooked material texture dependency is invalid or duplicated."};
            return false;
        }
    }
    if (!prepared.material ||
        !outBundle.SetRoot(ResourceHandle<IResource>(prepared.material.Get())) ||
        !outBundle.SetObservedContentIdentity(
            MakeObservedIdentity(prepared.artifactBytes)))
    {
        outError = {ResourceLoadErrorCode::LoaderFailure,
                    "Cooked material prepared bundle is invalid."};
        return false;
    }
    return true;
}
} // namespace RVX::Resource
