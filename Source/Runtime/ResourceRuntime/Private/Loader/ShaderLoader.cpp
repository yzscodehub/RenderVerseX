#include "Resource/Loader/ShaderLoader.h"

#include "Core/Log.h"
#include "ShaderContracts/ShaderArtifact.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <utility>

namespace RVX::Resource
{
namespace
{
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

ShaderLoader::ShaderLoader(ResourceManager* manager)
    : m_manager(manager)
{
}

std::vector<std::string> ShaderLoader::GetSupportedExtensions() const
{
    return {".rva", ".rvxshader"};
}

bool ShaderLoader::CanLoad(const std::string& path) const
{
    std::string extension =
        std::filesystem::path(path).extension().string();
    std::transform(
        extension.begin(), extension.end(), extension.begin(),
        [](unsigned char ch)
        {
            return static_cast<char>(std::tolower(ch));
        });
    const std::vector<std::string> supported = GetSupportedExtensions();
    return std::find(
        supported.begin(), supported.end(), extension) != supported.end();
}

IResource* ShaderLoader::Load(const std::string& path)
{
    (void)m_manager;
    const std::filesystem::path absolutePath =
        std::filesystem::absolute(path);
    m_lastLoadError.clear();

    ShaderArtifact artifact;
    if (!ReadShaderArtifactV2(
            absolutePath, artifact, m_lastLoadError))
    {
        RVX_CORE_ERROR("ShaderLoader: {}", m_lastLoadError);
        return nullptr;
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

    auto* resource = new ShaderResource();
    resource->SetData(
        std::move(artifact.code), {}, {}, metadata);
    resource->SetId(GenerateResourceId(path));
    resource->SetPath(path);
    resource->SetName(absolutePath.stem().string());
    resource->NotifyLoaded();
    return resource;
}
} // namespace RVX::Resource
