/** @file AnimationLoader.cpp  @brief Deterministic .rvxanim loader. */

#include "Resource/Loader/AnimationLoader.h"

#include "Core/Hash/SHA256.h"
#include "Core/Log.h"
#include "Resource/Cooked/CookedAnimationArtifact.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>

namespace RVX::Resource
{
namespace
{
    constexpr uint64 MaxAnimationArtifactBytes = 256ull * 1024ull * 1024ull;

    bool ReadArtifactBytes(const std::string& path,
                           std::vector<uint8>& outBytes,
                           std::string& outError)
    {
        outBytes.clear();
        outError.clear();
        try
        {
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file.is_open())
            {
                outError = "Cannot open animation artifact: " + path;
                return false;
            }
            const std::streamsize byteCount = file.tellg();
            if (byteCount <= 0 ||
                static_cast<uint64>(byteCount) > MaxAnimationArtifactBytes)
            {
                outError = "Animation artifact byte count is invalid: " + path;
                return false;
            }
            file.seekg(0, std::ios::beg);
            outBytes.resize(static_cast<size_t>(byteCount));
            if (!file.read(reinterpret_cast<char*>(outBytes.data()), byteCount))
            {
                outBytes.clear();
                outError = "Failed to read animation artifact: " + path;
                return false;
            }
            return true;
        }
        catch (const std::exception& exception)
        {
            outBytes.clear();
            outError = "Animation artifact read failed: " + std::string(exception.what());
            return false;
        }
        catch (...)
        {
            outBytes.clear();
            outError = "Animation artifact read failed with an unknown exception.";
            return false;
        }
    }

    AnimationResource* BuildResource(const CookedAnimationArtifact& artifact,
                                     ResourceId resourceId,
                                     const std::string& identityPath,
                                     const std::string& sourcePath,
                                     std::string& outError)
    {
        try
        {
            auto resource = std::make_unique<AnimationResource>();
            resource->SetId(resourceId);
            resource->SetPath(identityPath);
            resource->SetName(std::filesystem::path(sourcePath).stem().string());
            if (!resource->SetData(artifact.skeleton, artifact.clips))
            {
                outError = "Animation artifact could not be admitted by AnimationResource.";
                return nullptr;
            }
            return resource.release();
        }
        catch (const std::exception& exception)
        {
            outError = "Animation resource construction failed: " +
                std::string(exception.what());
            return nullptr;
        }
        catch (...)
        {
            outError = "Animation resource construction failed with an unknown exception.";
            return nullptr;
        }
    }

    ResourceContentIdentity BuildCookedIdentity(const std::vector<uint8>& bytes)
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

std::vector<std::string> AnimationLoader::GetSupportedExtensions() const
{
    return {".rvxanim"};
}

bool AnimationLoader::CanLoad(const std::string& path) const
{
    try
    {
        std::string extension = std::filesystem::path(path).extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char value)
                       {
                           return static_cast<char>(std::tolower(value));
                       });
        return extension == ".rvxanim";
    }
    catch (...)
    {
        return false;
    }
}

IResource* AnimationLoader::Load(const std::string& path)
{
    try
    {
        std::error_code absolutePathError;
        const std::filesystem::path absolutePath =
            std::filesystem::absolute(path, absolutePathError);
        if (absolutePathError)
        {
            RVX_CORE_ERROR("AnimationLoader: Could not resolve '{}': {}",
                           path,
                           absolutePathError.message());
            return nullptr;
        }

        const std::string absolutePathString = absolutePath.string();
        std::vector<uint8> bytes;
        std::string error;
        if (!CanLoad(absolutePathString) ||
            !ReadArtifactBytes(absolutePathString, bytes, error))
        {
            RVX_CORE_ERROR("AnimationLoader: {}", error.empty()
                ? "Unsupported animation artifact path." : error);
            return nullptr;
        }

        CookedAnimationArtifact artifact;
        if (!DeserializeCookedAnimationArtifact(bytes, artifact, error))
        {
            RVX_CORE_ERROR("AnimationLoader: Failed to decode '{}': {}", path, error);
            return nullptr;
        }

        const std::string identityPath = CanonicalizeAssetPath(absolutePathString);
        AnimationResource* resource = BuildResource(
            artifact,
            GenerateResourceId(identityPath),
            identityPath,
            absolutePathString,
            error);
        if (!resource)
        {
            RVX_CORE_ERROR("AnimationLoader: Failed to create '{}': {}", path, error);
            return nullptr;
        }
        resource->NotifyLoaded();
        return resource;
    }
    catch (const std::exception& exception)
    {
        RVX_CORE_ERROR("AnimationLoader: Load of '{}' raised an exception: {}",
                       path,
                       exception.what());
        return nullptr;
    }
    catch (...)
    {
        RVX_CORE_ERROR("AnimationLoader: Load of '{}' raised an unknown exception.", path);
        return nullptr;
    }
}

bool AnimationLoader::Prepare(const ResourceLoadPreparationContext& context,
                              PreparedResourceBundle& outBundle,
                              ResourceLoadError& outError)
{
    outError = {};
    try
    {
        // The caller-visible bundle is a publication transaction input. Build
        // entirely locally so decoding, validation, cancellation and identity
        // failures cannot expose partial resources to the ResourceManager.
        PreparedResourceBundle candidateBundle;
        if (context.IsCancellationRequested())
        {
            outError = {ResourceLoadErrorCode::Cancelled,
                        "Animation load was cancelled before reading the artifact."};
            return false;
        }
        if (context.assetKey.resourceType != AnimationResource::StaticResourceType ||
            context.rootResourceId == InvalidResourceId || !CanLoad(context.resolvedPath))
        {
            outError = {ResourceLoadErrorCode::InvalidRequest,
                        "Animation preparation requires a valid .rvxanim AssetKey."};
            return false;
        }

        std::vector<uint8> bytes;
        std::string error;
        if (!ReadArtifactBytes(context.resolvedPath, bytes, error))
        {
            outError = {ResourceLoadErrorCode::LoaderFailure, std::move(error)};
            return false;
        }
        if (context.IsCancellationRequested())
        {
            outError = {ResourceLoadErrorCode::Cancelled,
                        "Animation load was cancelled after reading the artifact."};
            return false;
        }

        CookedAnimationArtifact artifact;
        if (!DeserializeCookedAnimationArtifact(bytes, artifact, error))
        {
            outError = {ResourceLoadErrorCode::LoaderFailure, std::move(error)};
            return false;
        }
        if (context.IsCancellationRequested())
        {
            outError = {ResourceLoadErrorCode::Cancelled,
                        "Animation load was cancelled after decoding the artifact."};
            return false;
        }

        const ResourceContentIdentity observedContentIdentity =
            BuildCookedIdentity(bytes);
        if (!observedContentIdentity.IsValid())
        {
            outError = {ResourceLoadErrorCode::LoaderFailure,
                        "Animation loader could not construct its observed content identity."};
            return false;
        }
        if (context.IsCancellationRequested())
        {
            outError = {ResourceLoadErrorCode::Cancelled,
                        "Animation load was cancelled after hashing the artifact."};
            return false;
        }

        std::unique_ptr<AnimationResource> resource(BuildResource(
            artifact,
            context.rootResourceId,
            context.requestedPath,
            context.resolvedPath,
            error));
        if (!resource)
        {
            outError = {ResourceLoadErrorCode::LoaderFailure, std::move(error)};
            return false;
        }
        if (context.IsCancellationRequested())
        {
            outError = {ResourceLoadErrorCode::Cancelled,
                        "Animation load was cancelled after resource validation."};
            return false;
        }

        ResourceHandle<IResource> root(resource.release());
        if (!candidateBundle.SetRoot(std::move(root)) ||
            !candidateBundle.SetObservedContentIdentity(observedContentIdentity))
        {
            outError = {ResourceLoadErrorCode::LoaderFailure,
                        "Animation loader could not construct a verified prepared bundle."};
            return false;
        }
        if (context.IsCancellationRequested())
        {
            outError = {ResourceLoadErrorCode::Cancelled,
                        "Animation load was cancelled before prepared-bundle commit."};
            return false;
        }

        outBundle = std::move(candidateBundle);
        return true;
    }
    catch (const std::exception& exception)
    {
        outError = {ResourceLoadErrorCode::LoaderFailure,
                    "Animation preparation raised an exception: " +
                        std::string(exception.what())};
        return false;
    }
    catch (...)
    {
        outError = {ResourceLoadErrorCode::LoaderFailure,
                    "Animation preparation raised an unknown exception."};
        return false;
    }
}
} // namespace RVX::Resource
