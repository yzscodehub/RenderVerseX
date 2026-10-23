#include "AssetImport/ModelLoader.h"

#include "Core/Log.h"
#include "Geometry/Asset/VertexAttribute.h"

#include <algorithm>
#include <bit>
#include <filesystem>
#include <functional>

namespace RVX::Resource
{
namespace
{
    struct ModelLoadPreparationState final : ResourceLoadPreparationState
    {
        explicit ModelLoadPreparationState(GLTFImportOptions value)
            : options(std::move(value))
        {
        }

        GLTFImportOptions options;
    };

    bool IsDefaultImportOptions(const GLTFImportOptions& options)
    {
        const GLTFImportOptions defaults;
        return options.flipUVs == defaults.flipUVs &&
               options.generateNormals == defaults.generateNormals &&
               options.generateTangents == defaults.generateTangents &&
               options.mergeMeshes == defaults.mergeMeshes &&
               std::bit_cast<uint32>(options.scaleFactor) ==
                   std::bit_cast<uint32>(defaults.scaleFactor);
    }

    void HashImportByte(uint64& hash, uint8 value)
    {
        constexpr uint64 FnvPrime = 1099511628211ull;
        hash ^= static_cast<uint64>(value);
        hash *= FnvPrime;
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

    bool ValidateSkinnedNodeMeshes(const ModelResource& model,
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

                std::vector<int> fallbackMeshIndices;
                const std::vector<int>* meshIndices = &node->GetMeshIndices();
                if (meshIndices->empty() && node->GetMeshIndex() >= 0)
                {
                    fallbackMeshIndices.push_back(node->GetMeshIndex());
                    meshIndices = &fallbackMeshIndices;
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
                    for (size_t lod = 0;
                         lod < meshResource->GetLODCount();
                         ++lod)
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
            outError = "Skinned cooked model mesh is missing valid bone_indices or bone_weights data.";
        }
        return valid;
    }
} // namespace

void ModelLoader::SetGLTFImportOptions(const GLTFImportOptions& options)
{
    std::lock_guard<std::mutex> lock(m_optionsMutex);
    m_gltfOptions = options;
}

uint64 ModelLoader::CalculateImportOptionsHash(const GLTFImportOptions& options)
{
    if (IsDefaultImportOptions(options))
        return 0;

    uint64 hash = 14695981039346656037ull;
    HashImportByte(hash, options.flipUVs ? 1u : 0u);
    HashImportByte(hash, options.generateNormals ? 1u : 0u);
    HashImportByte(hash, options.generateTangents ? 1u : 0u);
    HashImportByte(hash, options.mergeMeshes ? 1u : 0u);
    const uint32 scaleBits = std::bit_cast<uint32>(options.scaleFactor);
    for (uint32 shift = 0; shift < 32; shift += 8)
    {
        HashImportByte(hash, static_cast<uint8>((scaleBits >> shift) & 0xffu));
    }
    return hash;
}

bool ModelLoader::CapturePreparationState(
    uint64 requestedImportOptionsHash,
    ResourceLoadPreparationStateRef& outState,
    uint64& outCanonicalImportOptionsHash,
    ResourceLoadError& outError) const
{
    GLTFImportOptions options;
    {
        std::lock_guard<std::mutex> lock(m_optionsMutex);
        options = m_gltfOptions;
    }

    const uint64 actualHash = CalculateImportOptionsHash(options);
    if (requestedImportOptionsHash != 0 && requestedImportOptionsHash != actualHash)
    {
        outError = {ResourceLoadErrorCode::InvalidRequest,
                    "The requested glTF import hash does not match the active loader profile."};
        return false;
    }

    outState = std::make_shared<ModelLoadPreparationState>(options);
    outCanonicalImportOptionsHash = actualHash;
    return true;
}

std::vector<std::string> ModelLoader::GetSupportedExtensions() const
{
    return {".gltf", ".glb"};
}

bool ModelLoader::SupportsPath(const std::string& path) const noexcept
{
    try
    {
        std::string extension = std::filesystem::path(path).extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
        const std::vector<std::string> extensions = GetSupportedExtensions();
        return std::find(
            extensions.begin(), extensions.end(), extension) != extensions.end();
    }
    catch (...)
    {
        return false;
    }
}

bool ModelLoader::Supports(
    const ResourceLoaderRequest& request) const noexcept
{
    return request.type == ResourceType::Model &&
           request.domain == ResourceLoadDomain::Source &&
           SupportsPath(request.resolvedPath);
}

bool ModelLoader::Prepare(const ResourceLoadPreparationContext& context,
                          PreparedResourceBundle& outBundle,
                          ResourceLoadError& outError)
{
    if (context.IsCancellationRequested())
    {
        outError = {ResourceLoadErrorCode::Cancelled, "Model load was cancelled before preparation."};
        return false;
    }

    Diagnostics::TraceSpan importSpan = Diagnostics::BeginTraceSpan(
        context.traceContext, "ModelIO", {{"path", context.resolvedPath}});
    ModelResource* model = nullptr;
    ResourceContentIdentity observedContentIdentity;
    const auto state =
        std::dynamic_pointer_cast<const ModelLoadPreparationState>(context.loaderState);
    if (!state)
    {
        outError = {ResourceLoadErrorCode::InvalidRequest,
                    "Model load is missing its immutable import-options snapshot."};
        return false;
    }
    GLTFImporter importer;
    GLTFImportResult imported = importer.Import(context.resolvedPath,
                                                state->options,
                                                importSpan.GetChildContext());
    if (!imported.success)
    {
        importSpan.SetAttribute("result", "failed");
        outError = {ResourceLoadErrorCode::LoaderFailure, imported.errorMessage};
        return false;
    }
    observedContentIdentity = imported.observedContentIdentity;
    importSpan.SetAttribute("result", "prepared");
    importSpan.End();

    Diagnostics::TraceSpan prepareSpan = Diagnostics::BeginTraceSpan(
        context.traceContext,
        "CPUPrepare",
        {{"path", context.resolvedPath}});
    TextureLoader textureLoader;
    model = CreateModelResource(context.resourceIdentityPath,
                                context.resolvedPath,
                                imported,
                                textureLoader,
                                context.traceContext,
                                true);
    if (model)
    {
        prepareSpan.SetAttribute("meshCount", model->GetMeshCount());
        prepareSpan.SetAttribute("materialCount", model->GetMaterialCount());
        prepareSpan.SetAttribute("result", "prepared");
    }
    if (!model)
    {
        outError = {ResourceLoadErrorCode::LoaderFailure,
                    "Model loader could not prepare the model resource."};
        return false;
    }
    model->SetId(context.rootResourceId);
    model->SetPath(context.requestedPath);
    model->SetName(std::filesystem::path(context.requestedPath).stem().string());
    if (!BuildPreparedBundle(model, outBundle, outError))
    {
        return false;
    }

    if (observedContentIdentity.IsValid() &&
        !outBundle.SetObservedContentIdentity(std::move(observedContentIdentity)))
    {
        outError = {ResourceLoadErrorCode::LoaderFailure,
                    "Model loader could not attach its observed source content identity."};
        return false;
    }
    return true;
}

bool ModelLoader::BuildPreparedBundle(ModelResource* model,
                                      PreparedResourceBundle& outBundle,
                                      ResourceLoadError& outError)
{
    ResourceHandle<IResource> preparedModel(model);
    if (const AnimationHandle animation = model->GetAnimationResource();
        animation && !outBundle.Contains(animation.GetId()) &&
        !outBundle.AddDependency(ResourceHandle<IResource>(animation)))
    {
        outError = {ResourceLoadErrorCode::LoaderFailure,
                    "Model loader produced an invalid or duplicate animation dependency."};
        return false;
    }
    for (const ResourceHandle<MeshResource>& mesh : model->GetMeshes())
    {
        if (!mesh || (!outBundle.Contains(mesh.GetId()) &&
                      !outBundle.AddDependency(ResourceHandle<IResource>(mesh))))
        {
            outError = {ResourceLoadErrorCode::LoaderFailure,
                        "Model loader produced an invalid or duplicate mesh dependency."};
            return false;
        }
    }
    for (const ResourceHandle<TextureResource>& texture : model->GetStreamingTextures())
    {
        if (!texture || (!outBundle.Contains(texture.GetId()) &&
                         !outBundle.AddDependency(ResourceHandle<IResource>(texture))))
        {
            outError = {ResourceLoadErrorCode::LoaderFailure,
                        "Model loader produced an invalid or duplicate streamed texture dependency."};
            return false;
        }
    }
    for (const ResourceHandle<MaterialResource>& material : model->GetMaterials())
    {
        if (!material)
        {
            outError = {ResourceLoadErrorCode::LoaderFailure,
                        "Model loader produced an invalid material dependency."};
            return false;
        }
        for (const auto& [slot, texture] : material->GetTextures())
        {
            (void)slot;
            if (texture && !outBundle.Contains(texture.GetId()) &&
                !outBundle.AddDependency(ResourceHandle<IResource>(texture)))
            {
                outError = {ResourceLoadErrorCode::LoaderFailure,
                            "Model loader produced an invalid or duplicate texture dependency."};
                return false;
            }
        }
        if (!outBundle.Contains(material.GetId()) &&
            !outBundle.AddDependency(ResourceHandle<IResource>(material)))
        {
            outError = {ResourceLoadErrorCode::LoaderFailure,
                        "Model loader produced an invalid or duplicate material dependency."};
            return false;
        }
    }
    if (!outBundle.SetRoot(std::move(preparedModel)))
    {
        outError = {ResourceLoadErrorCode::LoaderFailure,
                    "Model loader could not publish a model root into its prepared bundle."};
        return false;
    }
    return true;
}

ModelResource* ModelLoader::CreateModelResource(const std::string& resourceIdentityPath,
                                                const std::string& sourcePath,
                                                GLTFImportResult& importResult,
                                                TextureLoader& textureLoader,
                                                const Diagnostics::TraceContext& traceContext,
                                                bool streamTextures)
{
    auto* model = new ModelResource();
    std::vector<ModelTextureStreamingSource> streamingSources;
    const std::vector<ResourceHandle<TextureResource>> textures =
        LoadTextures(sourcePath,
                     resourceIdentityPath,
                     importResult.textures,
                     textureLoader,
                     traceContext,
                     streamTextures,
                     streamingSources);

    std::vector<ResourceHandle<MeshResource>> meshes;
    meshes.reserve(importResult.meshes.size());
    for (size_t index = 0; index < importResult.meshes.size(); ++index)
    {
        if (MeshResource* mesh = CreateMeshResource(resourceIdentityPath,
                                                    static_cast<int>(index),
                                                    importResult.meshes[index]))
        {
            meshes.emplace_back(mesh);
        }
    }
    model->SetMeshes(std::move(meshes));

    std::vector<ResourceHandle<MaterialResource>> materials;
    materials.reserve(importResult.materials.size());
    for (size_t index = 0; index < importResult.materials.size(); ++index)
    {
        if (MaterialResource* material = CreateMaterialResource(resourceIdentityPath,
                                                                static_cast<int>(index),
                                                                importResult.materials[index],
                                                                textures,
                                                                importResult))
        {
            materials.emplace_back(material);
        }
    }
    model->SetMaterials(std::move(materials));
    if (streamTextures)
    {
        model->SetTextureStreamingSources(std::move(streamingSources));
    }
    if (importResult.model)
    {
        model->SetRootNode(importResult.model->GetRootNode());
    }
    const bool hasSkeletalPayload = importResult.skeleton ||
                                    !importResult.animationClips.empty();
    if (hasSkeletalPayload)
    {
        if (!importResult.skeleton || importResult.animationClips.empty())
        {
            delete model;
            return nullptr;
        }

        auto animation = std::make_unique<AnimationResource>();
        const std::string animationIdentityPath = resourceIdentityPath + "#animation";
        animation->SetId(GenerateResourceId(animationIdentityPath));
        animation->SetPath(animationIdentityPath);
        animation->SetName(std::filesystem::path(sourcePath).stem().string() + "_Animation");
        if (!animation->SetData(importResult.skeleton,
                                importResult.animationClips))
        {
            delete model;
            return nullptr;
        }
        model->SetAnimationResource(AnimationHandle(animation.release()));
        std::string skinningError;
        if (!ValidateSkinnedNodeMeshes(*model, skinningError))
        {
            delete model;
            return nullptr;
        }
    }
    return model;
}

MeshResource* ModelLoader::CreateMeshResource(const std::string& modelPath,
                                              int index,
                                              Mesh::Ptr mesh)
{
    if (!mesh)
    {
        return nullptr;
    }

    auto* resource = new MeshResource();
    resource->SetId(GenerateMeshId(modelPath, index));
    resource->SetPath(modelPath + "#mesh_" + std::to_string(index));
    resource->SetName(mesh->name.empty() ? "Mesh_" + std::to_string(index) : mesh->name);
    resource->SetMesh(std::move(mesh));
    return resource;
}

MaterialResource* ModelLoader::CreateMaterialResource(
    const std::string& modelPath,
    int index,
    Material::Ptr material,
    const std::vector<ResourceHandle<TextureResource>>& textures,
    const GLTFImportResult& importResult)
{
    (void)importResult;
    if (!material)
    {
        return nullptr;
    }

    auto* resource = new MaterialResource();
    resource->SetId(GenerateMaterialId(modelPath, index));
    resource->SetPath(modelPath + "#material_" + std::to_string(index));
    resource->SetName(material->GetName());
    resource->SetMaterialData(material);

    auto associate = [&](const std::optional<TextureInfo>& info, const std::string& slot)
    {
        if (info && info->imageId >= 0 && info->imageId < static_cast<int>(textures.size()) &&
            textures[info->imageId])
        {
            resource->SetTexture(slot, textures[info->imageId]);
        }
    };
    associate(material->GetBaseColorTexture(), "albedo");
    associate(material->GetNormalTexture(), "normal");
    associate(material->GetMetallicRoughnessTexture(), "metallic_roughness");
    associate(material->GetOcclusionTexture(), "ao");
    associate(material->GetEmissiveTexture(), "emissive");
    return resource;
}

std::vector<ResourceHandle<TextureResource>> ModelLoader::LoadTextures(
    const std::string& sourceModelPath,
    const std::string& resourceIdentityPath,
    std::vector<TextureReference>& references,
    TextureLoader& textureLoader,
    const Diagnostics::TraceContext& traceContext,
    bool streamTextures,
    std::vector<ModelTextureStreamingSource>& outStreamingSources)
{
    std::vector<ResourceHandle<TextureResource>> textures;
    textures.reserve(references.size());
    outStreamingSources.clear();
    outStreamingSources.reserve(streamTextures ? references.size() : 0);
    for (TextureReference& reference : references)
    {
        const bool deferDecode =
            streamTextures &&
            textureLoader.RequiresDeferredDecode(reference);
        TextureResource* texture = deferDecode
            ? textureLoader.CreateStreamingPlaceholder(reference,
                                                       sourceModelPath,
                                                       resourceIdentityPath)
            : textureLoader.LoadFromReference(reference,
                                              sourceModelPath,
                                              traceContext,
                                              resourceIdentityPath);
        textures.emplace_back(texture);
        if (!deferDecode || !texture)
            continue;

        ModelTextureStreamingSource source;
        source.texture = textures.back();
        source.sourceModelPath = sourceModelPath;
        source.resourceIdentityBase = resourceIdentityPath;
        source.traceContext = traceContext;
        if (!textureLoader.EstimateDecodedByteSize(reference,
                                                   sourceModelPath,
                                                   source.estimatedDecodedBytes,
                                                   source.preflightError))
        {
            source.estimatedDecodedBytes = 0;
        }
        source.reference = std::move(reference);
        outStreamingSources.push_back(std::move(source));
    }
    return textures;
}

ResourceId ModelLoader::GenerateModelId(const std::string& modelPath)
{
    return GenerateResourceId(modelPath);
}

ResourceId ModelLoader::GenerateMeshId(const std::string& modelPath, int index)
{
    return GenerateResourceId(modelPath + "#mesh_" + std::to_string(index));
}

ResourceId ModelLoader::GenerateMaterialId(const std::string& modelPath, int index)
{
    return GenerateResourceId(modelPath + "#material_" + std::to_string(index));
}
} // namespace RVX::Resource
