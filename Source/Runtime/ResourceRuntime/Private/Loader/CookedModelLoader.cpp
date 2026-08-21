/** @file CookedModelLoader.cpp @brief Cooked/package model loader */

#include "Resource/Loader/CookedModelLoader.h"

#include "Geometry/Asset/VertexAttribute.h"
#include "Resource/Cooked/CookedAnimationArtifact.h"
#include "Resource/Cooked/CookedMeshArtifactReader.h"
#include "Resource/Cooked/CookedModelArtifact.h"
#include "Resource/Cooked/CookedTextureArtifact.h"
#include "Resource/Types/AnimationResource.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace RVX::Resource
{
namespace
{
    bool ReadBytes(
        const std::filesystem::path& path,
        std::vector<uint8>& outBytes,
        std::string& outError,
        uint64 maxBytes = 512ull * 1024ull * 1024ull)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open())
        {
            outError = "Cannot open cooked artifact: " + path.string();
            return false;
        }
        const std::streamsize byteCount = file.tellg();
        if (byteCount <= 0 || static_cast<uint64>(byteCount) > maxBytes)
        {
            outError = "Cooked artifact byte count is invalid: " + path.string();
            return false;
        }
        file.seekg(0, std::ios::beg);
        outBytes.resize(static_cast<size_t>(byteCount));
        if (!file.read(
                reinterpret_cast<char*>(outBytes.data()), byteCount))
        {
            outError = "Failed to read cooked artifact: " + path.string();
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
        const std::filesystem::path candidate =
            std::filesystem::weakly_canonical(
                canonicalRoot / relativePath, error);
        if (error)
            return false;
        const std::filesystem::path relativeCandidate =
            std::filesystem::relative(candidate, canonicalRoot, error);
        if (error || relativeCandidate.empty() ||
            relativeCandidate.is_absolute() ||
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

    bool HasValidSkinningAttributes(const Mesh& mesh)
    {
        const VertexAttribute* boneIndices =
            mesh.GetAttribute(VertexBufferNames::BoneIndices);
        const VertexAttribute* boneWeights =
            mesh.GetAttribute(VertexBufferNames::BoneWeights);
        const size_t vertexCount = mesh.GetVertexCount();
        return vertexCount != 0 && boneIndices && boneWeights &&
            boneIndices->GetVertexCount() == vertexCount &&
            boneWeights->GetVertexCount() == vertexCount;
    }

    bool ValidateSkinnedNodeMeshes(
        const ModelResource& model,
        std::string& outError)
    {
        const Node::Ptr root = model.GetRootNode();
        if (!root || !model.GetSkeleton())
        {
            outError = "Skinned cooked model is missing its canonical skeleton.";
            return false;
        }
        bool valid = true;
        root->TraverseDepthFirst(
            [&](Node* node)
            {
                if (!valid || !node || !node->HasSkin())
                    return;
                std::vector<int> fallback;
                const std::vector<int>* meshIndices = &node->GetMeshIndices();
                if (meshIndices->empty() && node->GetMeshIndex() >= 0)
                {
                    fallback.push_back(node->GetMeshIndex());
                    meshIndices = &fallback;
                }
                if (meshIndices->empty())
                {
                    valid = false;
                    return;
                }
                for (const int meshIndex : *meshIndices)
                {
                    if (meshIndex < 0 ||
                        meshIndex >= static_cast<int>(model.GetMeshCount()))
                    {
                        valid = false;
                        return;
                    }
                    const ResourceHandle<MeshResource> meshResource =
                        model.GetMesh(static_cast<size_t>(meshIndex));
                    if (!meshResource)
                    {
                        valid = false;
                        return;
                    }
                    for (size_t lod = 0; lod < meshResource->GetLODCount(); ++lod)
                    {
                        const Mesh::Ptr mesh = meshResource->GetLODMesh(lod);
                        if (!mesh || !HasValidSkinningAttributes(*mesh))
                        {
                            valid = false;
                            return;
                        }
                    }
                }
            });
        if (!valid)
        {
            outError =
                "Skinned cooked model mesh is missing valid bone attributes.";
        }
        return valid;
    }

    MaterialResource* BuildMaterial(
        const std::string& modelIdentity,
        int index,
        const Material::Ptr& material,
        const std::vector<ResourceHandle<TextureResource>>& textures)
    {
        if (!material)
            return nullptr;
        auto resource = std::make_unique<MaterialResource>();
        resource->SetId(GenerateResourceId(
            modelIdentity + "#material_" + std::to_string(index)));
        resource->SetPath(
            modelIdentity + "#material_" + std::to_string(index));
        resource->SetName(material->GetName());
        resource->SetMaterialData(material);
        const auto associate =
            [&](const std::optional<TextureInfo>& info, const char* slot)
            {
                if (info && info->imageId >= 0 &&
                    info->imageId < static_cast<int>(textures.size()) &&
                    textures[static_cast<size_t>(info->imageId)])
                {
                    resource->SetTexture(
                        slot, textures[static_cast<size_t>(info->imageId)]);
                }
            };
        associate(material->GetBaseColorTexture(), "albedo");
        associate(material->GetNormalTexture(), "normal");
        associate(material->GetMetallicRoughnessTexture(), "metallic_roughness");
        associate(material->GetOcclusionTexture(), "ao");
        associate(material->GetEmissiveTexture(), "emissive");
        return resource.release();
    }

    bool BuildPreparedBundle(
        ModelResource* model,
        PreparedResourceBundle& outBundle,
        ResourceLoadError& outError)
    {
        ResourceHandle<IResource> root(model);
        if (const AnimationHandle animation = model->GetAnimationResource();
            animation && !outBundle.Contains(animation.GetId()) &&
            !outBundle.AddDependency(ResourceHandle<IResource>(animation)))
        {
            outError = {ResourceLoadErrorCode::LoaderFailure,
                        "Cooked model animation dependency is invalid."};
            return false;
        }
        for (const ResourceHandle<MeshResource>& mesh : model->GetMeshes())
        {
            if (!mesh || (!outBundle.Contains(mesh.GetId()) &&
                          !outBundle.AddDependency(ResourceHandle<IResource>(mesh))))
            {
                outError = {ResourceLoadErrorCode::LoaderFailure,
                            "Cooked model mesh dependency is invalid."};
                return false;
            }
        }
        for (const ResourceHandle<MaterialResource>& material : model->GetMaterials())
        {
            if (!material)
            {
                outError = {ResourceLoadErrorCode::LoaderFailure,
                            "Cooked model material dependency is invalid."};
                return false;
            }
            for (const auto& [slot, texture] : material->GetTextures())
            {
                (void)slot;
                if (texture && !outBundle.Contains(texture.GetId()) &&
                    !outBundle.AddDependency(ResourceHandle<IResource>(texture)))
                {
                    outError = {ResourceLoadErrorCode::LoaderFailure,
                                "Cooked model texture dependency is invalid."};
                    return false;
                }
            }
            if (!outBundle.Contains(material.GetId()) &&
                !outBundle.AddDependency(ResourceHandle<IResource>(material)))
            {
                outError = {ResourceLoadErrorCode::LoaderFailure,
                            "Cooked model material dependency is duplicated."};
                return false;
            }
        }
        if (!outBundle.SetRoot(std::move(root)))
        {
            outError = {ResourceLoadErrorCode::LoaderFailure,
                        "Cooked model root bundle is invalid."};
            return false;
        }
        return true;
    }

    std::unique_ptr<ModelResource> BuildModel(
        const std::string& sourcePath,
        const std::string& identity,
        std::string& outError)
    {
        std::vector<uint8> modelBytes;
        if (!ReadBytes(sourcePath, modelBytes, outError))
            return nullptr;
        CookedModelArtifact artifact;
        if (!DeserializeCookedModelArtifact(modelBytes, artifact, outError))
            return nullptr;

        const std::filesystem::path modelDirectory =
            std::filesystem::path(sourcePath).parent_path();
        std::filesystem::path meshPath;
        if (!ResolveContainedDependency(
                modelDirectory, artifact.meshArtifactPath, ".rva", meshPath))
        {
            outError =
                "Cooked model mesh dependency must be a contained .rva product";
            return nullptr;
        }
        std::vector<CookedMeshRecord> cookedMeshes;
        if (!CookedMeshArtifactReader::ReadFile(
                meshPath.string(), cookedMeshes, outError))
        {
            return nullptr;
        }

        AnimationHandle animation;
        if (artifact.animationArtifactPath.has_value())
        {
            std::filesystem::path animationPath;
            if (!ResolveContainedDependency(
                    modelDirectory,
                    *artifact.animationArtifactPath,
                    ".rvxanim",
                    animationPath))
            {
                outError =
                    "Cooked model animation dependency must be contained";
                return nullptr;
            }
            std::vector<uint8> animationBytes;
            if (!ReadBytes(
                    animationPath, animationBytes, outError,
                    256ull * 1024ull * 1024ull))
            {
                return nullptr;
            }
            CookedAnimationArtifact animationArtifact;
            if (!DeserializeCookedAnimationArtifact(
                    animationBytes, animationArtifact, outError))
            {
                return nullptr;
            }
            if (animationArtifact.sourcePath != artifact.logicalAssetId)
            {
                outError =
                    "Cooked model and animation source identities differ";
                return nullptr;
            }
            auto resource = std::make_unique<AnimationResource>();
            const std::string animationIdentity = identity + "#animation";
            resource->SetId(GenerateResourceId(animationIdentity));
            resource->SetPath(animationIdentity);
            resource->SetName(animationPath.stem().string());
            if (!resource->SetData(
                    animationArtifact.skeleton, animationArtifact.clips))
            {
                outError = "Cooked animation resource rejected its payload";
                return nullptr;
            }
            animation = AnimationHandle(resource.release());
        }

        std::vector<ResourceHandle<TextureResource>> textures;
        textures.reserve(artifact.textures.size());
        for (size_t index = 0; index < artifact.textures.size(); ++index)
        {
            const TextureReference& reference = artifact.textures[index];
            std::filesystem::path texturePath;
            if (!ResolveContainedDependency(
                    modelDirectory, reference.path, ".rva", texturePath))
            {
                outError =
                    "Cooked model texture dependency must be a contained .rva product";
                return nullptr;
            }
            std::vector<uint8> textureBytes;
            if (!ReadBytes(texturePath, textureBytes, outError))
                return nullptr;
            CookedTextureArtifact textureArtifact;
            if (!DeserializeCookedTextureArtifact(
                    textureBytes, textureArtifact, outError))
            {
                return nullptr;
            }
            if (textureArtifact.metadata.usage != reference.usage ||
                textureArtifact.metadata.isSRGB != reference.isSRGB)
            {
                outError =
                    "Cooked model texture metadata does not match its model contract";
                return nullptr;
            }
            auto texture = std::make_unique<TextureResource>();
            const std::string textureIdentity =
                CanonicalizeAssetPath(texturePath.string()) +
                "|usage=" + std::to_string(static_cast<uint32>(reference.usage)) +
                "|srgb=" + std::to_string(reference.isSRGB ? 1 : 0);
            texture->SetId(GenerateResourceId(textureIdentity));
            texture->SetPath(texturePath.string());
            texture->SetName(texturePath.stem().string());
            texture->SetData(
                std::move(textureArtifact.pixels), textureArtifact.metadata);
            textures.emplace_back(texture.release());
        }

        auto model = std::make_unique<ModelResource>();
        std::vector<ResourceHandle<MeshResource>> meshes;
        meshes.reserve(cookedMeshes.size());
        for (size_t index = 0; index < cookedMeshes.size(); ++index)
        {
            CookedMeshRecord& record = cookedMeshes[index];
            if (record.lodMeshes.empty() || !record.lodMeshes.front())
            {
                outError = "Cooked model contains an empty mesh record";
                return nullptr;
            }
            auto mesh = std::make_unique<MeshResource>();
            mesh->SetId(GenerateResourceId(
                identity + "#mesh_" + std::to_string(index)));
            mesh->SetPath(identity + "#mesh_" + std::to_string(index));
            mesh->SetName(record.lodMeshes.front()->name.empty()
                ? "Mesh_" + std::to_string(index)
                : record.lodMeshes.front()->name);
            mesh->SetLODMeshes(std::move(record.lodMeshes));
            meshes.emplace_back(mesh.release());
        }
        model->SetMeshes(std::move(meshes));

        std::vector<ResourceHandle<MaterialResource>> materials;
        materials.reserve(artifact.materials.size());
        for (size_t index = 0; index < artifact.materials.size(); ++index)
        {
            MaterialResource* material = BuildMaterial(
                identity,
                static_cast<int>(index),
                artifact.materials[index],
                textures);
            if (!material)
            {
                outError = "Cooked model contains an invalid material";
                return nullptr;
            }
            materials.emplace_back(material);
        }
        model->SetMaterials(std::move(materials));
        model->SetRootNode(std::move(artifact.rootNode));
        model->SetAnimationResource(std::move(animation));

        bool hierarchyValid = model->GetRootNode() != nullptr;
        if (hierarchyValid)
        {
            model->GetRootNode()->TraverseDepthFirst(
                [&](Node* node)
                {
                    if (!node || node->GetMeshIndex() < -1 ||
                        node->GetMeshIndex() >=
                            static_cast<int>(model->GetMeshCount()))
                    {
                        hierarchyValid = false;
                        return;
                    }
                    for (const int meshIndex : node->GetMeshIndices())
                    {
                        if (meshIndex < 0 ||
                            meshIndex >= static_cast<int>(model->GetMeshCount()))
                        {
                            hierarchyValid = false;
                            return;
                        }
                    }
                    if (node->GetSkinIndex() < -1 || node->GetSkinIndex() > 0 ||
                        (node->HasSkin() && !model->GetAnimationResource()))
                    {
                        hierarchyValid = false;
                        return;
                    }
                    for (const int materialIndex : node->GetMaterialIndices())
                    {
                        if (materialIndex < 0 || materialIndex >=
                            static_cast<int>(model->GetMaterialCount()))
                        {
                            hierarchyValid = false;
                            return;
                        }
                    }
                });
        }
        if (!hierarchyValid)
        {
            outError = "Cooked model hierarchy contains invalid references";
            return nullptr;
        }
        if (model->GetAnimationResource() &&
            !ValidateSkinnedNodeMeshes(*model, outError))
        {
            return nullptr;
        }
        return model;
    }
} // namespace

std::vector<std::string> CookedModelLoader::GetSupportedExtensions() const
{
    return {".rva"};
}

bool CookedModelLoader::Supports(
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
        return request.type == ResourceType::Model &&
            (request.domain == ResourceLoadDomain::Cooked ||
             request.domain == ResourceLoadDomain::Package) &&
            extension == ".rva";
    }
    catch (...)
    {
        return false;
    }
}

bool CookedModelLoader::Prepare(
    const ResourceLoadPreparationContext& context,
    PreparedResourceBundle& outBundle,
    ResourceLoadError& outError)
{
    if (context.IsCancellationRequested())
    {
        outError = {ResourceLoadErrorCode::Cancelled,
                    "Cooked model load was cancelled."};
        return false;
    }
    std::string error;
    std::unique_ptr<ModelResource> model = BuildModel(
        context.resolvedPath,
        context.resourceIdentityPath,
        error);
    if (!model)
    {
        outError = {ResourceLoadErrorCode::LoaderFailure, std::move(error)};
        return false;
    }
    model->SetId(context.rootResourceId);
    model->SetPath(context.requestedPath);
    model->SetName(
        std::filesystem::path(context.requestedPath).stem().string());
    return BuildPreparedBundle(model.release(), outBundle, outError);
}
} // namespace RVX::Resource
