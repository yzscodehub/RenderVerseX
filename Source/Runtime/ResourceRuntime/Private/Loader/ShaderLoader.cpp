#include "Resource/Loader/ShaderLoader.h"

#include "Core/Log.h"
#include "Core/Hash/SHA256.h"
#include "ShaderContracts/ShaderArtifact.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <utility>

namespace RVX::Resource
{
namespace
{
    bool ReadBytes(
        const std::filesystem::path& path,
        std::vector<uint8>& outBytes)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open())
            return false;
        const std::streamsize size = file.tellg();
        if (size <= 0)
            return false;
        file.seekg(0, std::ios::beg);
        outBytes.resize(static_cast<size_t>(size));
        return file.read(
            reinterpret_cast<char*>(outBytes.data()), size).good();
    }

    ResourceContentIdentity MakeObservedIdentity(
        const std::vector<uint8>& bytes)
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

    ShaderBackendType ToResourceBackend(RHIBackendType backend)
    {
        switch (backend)
        {
            case RHIBackendType::DX11: return ShaderBackendType::DX11;
            case RHIBackendType::DX12: return ShaderBackendType::DX12;
            case RHIBackendType::Vulkan: return ShaderBackendType::Vulkan;
            case RHIBackendType::Metal: return ShaderBackendType::Metal;
            case RHIBackendType::OpenGL: return ShaderBackendType::OpenGL;
            case RHIBackendType::Auto: return ShaderBackendType::Auto;
            default: return ShaderBackendType::None;
        }
    }

    ShaderStage ToResourceStage(RHIShaderStage stage)
    {
        switch (stage)
        {
            case RHIShaderStage::Vertex: return ShaderStage::Vertex;
            case RHIShaderStage::Hull: return ShaderStage::Hull;
            case RHIShaderStage::Domain: return ShaderStage::Domain;
            case RHIShaderStage::Geometry: return ShaderStage::Geometry;
            case RHIShaderStage::Pixel: return ShaderStage::Pixel;
            case RHIShaderStage::Compute: return ShaderStage::Compute;
            case RHIShaderStage::Mesh: return ShaderStage::Mesh;
            case RHIShaderStage::Amplification: return ShaderStage::Amplification;
            case RHIShaderStage::RayGeneration: return ShaderStage::RayGeneration;
            case RHIShaderStage::AnyHit: return ShaderStage::AnyHit;
            case RHIShaderStage::ClosestHit: return ShaderStage::ClosestHit;
            case RHIShaderStage::Miss: return ShaderStage::Miss;
            case RHIShaderStage::Intersection: return ShaderStage::Intersection;
            case RHIShaderStage::Callable: return ShaderStage::Callable;
            default: return ShaderStage::None;
        }
    }
} // namespace

std::vector<std::string> ShaderLoader::GetSupportedExtensions() const
{
    return {".rva", ".rvxshader"};
}

bool ShaderLoader::Supports(
    const ResourceLoaderRequest& request) const noexcept
{
    try
    {
        std::string extension =
            std::filesystem::path(request.resolvedPath).extension().string();
        std::transform(
            extension.begin(), extension.end(), extension.begin(),
            [](unsigned char ch)
            {
                return static_cast<char>(std::tolower(ch));
            });
        return request.type == ResourceType::Shader &&
            (request.domain == ResourceLoadDomain::Cooked ||
             request.domain == ResourceLoadDomain::Package) &&
            (extension == ".rva" || extension == ".rvxshader");
    }
    catch (...)
    {
        return false;
    }
}

bool ShaderLoader::Prepare(
    const ResourceLoadPreparationContext& context,
    PreparedResourceBundle& outBundle,
    ResourceLoadError& outError)
{
    const std::filesystem::path absolutePath =
        std::filesystem::absolute(context.resolvedPath);
    m_lastLoadError.clear();

    std::vector<uint8> consumedBytes;
    if (!ReadBytes(absolutePath, consumedBytes))
    {
        outError = {ResourceLoadErrorCode::LoaderFailure,
                    "ShaderLoader could not read the cooked artifact."};
        return false;
    }

    ShaderArtifact artifact;
    if (!ReadShaderArtifactV2(
            absolutePath, artifact, m_lastLoadError))
    {
        outError = {ResourceLoadErrorCode::LoaderFailure, m_lastLoadError};
        return false;
    }

    ShaderMetadata metadata;
    metadata.sourcePath = artifact.request.logicalAssetId;
    metadata.backend = ToResourceBackend(artifact.request.backend);
    metadata.stage = ToResourceStage(artifact.request.stage);
    metadata.entryPoint = artifact.request.entryPoint;
    metadata.targetProfile = artifact.targetProfile;
    metadata.sourceHash = artifact.contentHash;
    metadata.reflectionResourceCount = static_cast<std::uint32_t>(
        artifact.reflection.resources.size());

    auto resource = std::make_unique<ShaderResource>();
    resource->SetData(
        std::move(artifact.code), {}, {}, metadata);
    resource->SetId(context.rootResourceId);
    resource->SetPath(context.requestedPath);
    resource->SetName(absolutePath.stem().string());
    if (!outBundle.SetRoot(ResourceHandle<IResource>(resource.release())) ||
        !outBundle.SetObservedContentIdentity(
            MakeObservedIdentity(consumedBytes)))
    {
        outError = {ResourceLoadErrorCode::LoaderFailure,
                    "Cooked shader could not form a prepared bundle."};
        return false;
    }
    return true;
}
} // namespace RVX::Resource
