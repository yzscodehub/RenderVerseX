#include "Resource/Loader/ModelLoader.h"

#include "Core/Log.h"

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
} // namespace

ModelLoader::ModelLoader(ResourceManager* manager)
    : m_manager(manager)
{
}

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

bool ModelLoader::CanLoad(const std::string& path) const
{
    std::string extension = std::filesystem::path(path).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
    const std::vector<std::string> extensions = GetSupportedExtensions();
    return std::find(extensions.begin(), extensions.end(), extension) != extensions.end();
}

IResource* ModelLoader::Load(const std::string& path)
{
    // Compatibility-only synchronous path. Asynchronous production loads use
    // Prepare(), which creates a fresh importer/decoder with no manager access.
    const std::filesystem::path absolutePath = std::filesystem::absolute(path);
    GLTFImportOptions options;
    {
        std::lock_guard<std::mutex> lock(m_optionsMutex);
        options = m_gltfOptions;
    }

    const Diagnostics::TraceContext traceContext =
        m_manager ? m_manager->GetStartupTraceContext() : Diagnostics::TraceContext{};
    GLTFImporter importer;
    GLTFImportResult imported = importer.Import(absolutePath.string(), options, traceContext);
    if (!imported.success)
    {
        RVX_CORE_ERROR("ModelLoader: Failed to import '{}': {}", path, imported.errorMessage);
        return nullptr;
    }

    TextureLoader textureLoader(m_manager);
    const std::string resourceIdentityPath = CanonicalizeAssetPath(absolutePath.string());
    ModelResource* model = CreateModelResource(resourceIdentityPath,
                                               absolutePath.string(),
                                               imported,
                                               textureLoader,
                                               traceContext);
    if (!model)
    {
        return nullptr;
    }
    model->SetId(GenerateModelId(resourceIdentityPath));
    model->SetPath(absolutePath.string());
    model->SetName(absolutePath.stem().string());
    for (const ResourceHandle<MeshResource>& mesh : model->GetMeshes())
    {
        if (mesh && !mesh->IsLoaded())
        {
            mesh->NotifyLoaded();
        }
    }
    for (const ResourceHandle<MaterialResource>& material : model->GetMaterials())
    {
        if (material && !material->IsLoaded())
        {
            material->NotifyLoaded();
        }
    }
    model->NotifyLoaded();
    return model;
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

    const auto state =
        std::dynamic_pointer_cast<const ModelLoadPreparationState>(context.loaderState);
    if (!state)
    {
        outError = {ResourceLoadErrorCode::InvalidRequest,
                    "Model load is missing its immutable import-options snapshot."};
        return false;
    }
    const GLTFImportOptions options = state->options;

    GLTFImporter importer;
    Diagnostics::TraceSpan importSpan = Diagnostics::BeginTraceSpan(
        context.traceContext, "ModelIO", {{"path", context.resolvedPath}});
    GLTFImportResult imported = importer.Import(context.resolvedPath,
                                                options,
                                                importSpan.GetChildContext());
    if (!imported.success)
    {
        importSpan.SetAttribute("result", "failed");
        outError = {ResourceLoadErrorCode::LoaderFailure, imported.errorMessage};
        return false;
    }
    importSpan.SetAttribute("result", "prepared");
    importSpan.End();

    Diagnostics::TraceSpan prepareSpan = Diagnostics::BeginTraceSpan(
        context.traceContext,
        "CPUPrepare",
        {{"path", context.resolvedPath}});

    // There is intentionally no ResourceManager on this per-request decoder.
    // This is what prevents a worker from accessing cache, registry, lifecycle
    // notifications, Scene or the render upload gateway.
    TextureLoader textureLoader(nullptr, true);
    ModelResource* model = CreateModelResource(context.resourceIdentityPath,
                                               context.resolvedPath,
                                               imported,
                                               textureLoader,
                                               context.traceContext);
    if (!model)
    {
        prepareSpan.SetAttribute("result", "failed");
        outError = {ResourceLoadErrorCode::LoaderFailure,
                    "Model loader could not prepare the model resource."};
        return false;
    }
    model->SetId(context.rootResourceId);
    model->SetPath(context.requestedPath);
    model->SetName(std::filesystem::path(context.requestedPath).stem().string());
    ResourceHandle<IResource> preparedModel(model);

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
        prepareSpan.SetAttribute("result", "failed");
        outError = {ResourceLoadErrorCode::LoaderFailure,
                    "Model loader could not publish a model root into its prepared bundle."};
        return false;
    }
    prepareSpan.SetAttribute("meshCount", model->GetMeshCount());
    prepareSpan.SetAttribute("materialCount", model->GetMaterialCount());
    prepareSpan.SetAttribute("result", "prepared");
    return true;
}

ModelResource* ModelLoader::CreateModelResource(const std::string& resourceIdentityPath,
                                                const std::string& sourcePath,
                                                GLTFImportResult& importResult,
                                                TextureLoader& textureLoader,
                                                const Diagnostics::TraceContext& traceContext)
{
    auto* model = new ModelResource();
    const std::vector<TextureResource*> textures = LoadTextures(sourcePath,
                                                                 resourceIdentityPath,
                                                                 importResult.textures,
                                                                 textureLoader,
                                                                 traceContext);

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
    if (importResult.model)
    {
        model->SetRootNode(importResult.model->GetRootNode());
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
    const std::vector<TextureResource*>& textures,
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
            resource->SetTexture(slot, ResourceHandle<TextureResource>(textures[info->imageId]));
        }
    };
    associate(material->GetBaseColorTexture(), "albedo");
    associate(material->GetNormalTexture(), "normal");
    associate(material->GetMetallicRoughnessTexture(), "metallic_roughness");
    associate(material->GetOcclusionTexture(), "ao");
    associate(material->GetEmissiveTexture(), "emissive");
    return resource;
}

std::vector<TextureResource*> ModelLoader::LoadTextures(
    const std::string& sourceModelPath,
    const std::string& resourceIdentityPath,
    const std::vector<TextureReference>& references,
    TextureLoader& textureLoader,
    const Diagnostics::TraceContext& traceContext)
{
    std::vector<TextureResource*> textures;
    textures.reserve(references.size());
    for (const TextureReference& reference : references)
    {
        textures.push_back(textureLoader.LoadFromReference(reference,
                                                           sourceModelPath,
                                                           traceContext,
                                                           resourceIdentityPath));
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
