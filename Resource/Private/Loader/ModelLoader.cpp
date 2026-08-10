#include "Resource/Loader/ModelLoader.h"

#include "Core/Log.h"
#include "Resource/Cooked/CookedMeshArtifactReader.h"

#include <algorithm>
#include <bit>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>

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

    bool ReadCookedModel(const std::string& path,
                         CookedModelArtifact& outArtifact,
                         std::string& outError)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open())
        {
            outError = "Cannot open cooked model artifact: " + path;
            return false;
        }
        const std::streamsize byteCount = file.tellg();
        if (byteCount <= 0)
        {
            outError = "Cooked model artifact is empty: " + path;
            return false;
        }
        file.seekg(0, std::ios::beg);
        std::vector<uint8> bytes(static_cast<size_t>(byteCount));
        if (!file.read(reinterpret_cast<char*>(bytes.data()), byteCount))
        {
            outError = "Failed to read cooked model artifact: " + path;
            return false;
        }
        return DeserializeCookedModelArtifact(bytes, outArtifact, outError);
    }

    bool IsCookedModelPath(const std::string& path)
    {
        std::string extension = std::filesystem::path(path).extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
        return extension == ".rva";
    }

    bool ResolveContainedCookedDependency(
        const std::filesystem::path& productDirectory,
        const std::string& relativeValue,
        std::filesystem::path& outPath)
    {
        const std::filesystem::path relativePath(relativeValue);
        if (relativeValue.empty() || relativePath.is_absolute() ||
            relativePath.has_root_path())
        {
            return false;
        }
        for (const std::filesystem::path& component : relativePath)
        {
            if (component == "..")
                return false;
        }

        std::error_code error;
        const std::filesystem::path canonicalRoot =
            std::filesystem::weakly_canonical(productDirectory, error);
        if (error)
            return false;
        const std::filesystem::path canonicalCandidate =
            std::filesystem::weakly_canonical(canonicalRoot / relativePath, error);
        if (error)
            return false;
        const std::filesystem::path relativeCandidate =
            std::filesystem::relative(canonicalCandidate, canonicalRoot, error);
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

        std::string extension = canonicalCandidate.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
        if (extension != ".rva")
            return false;

        outPath = canonicalCandidate;
        return true;
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
    return {".gltf", ".glb", ".rva"};
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
    TextureLoader textureLoader(m_manager);
    const std::string resourceIdentityPath = CanonicalizeAssetPath(absolutePath.string());
    ModelResource* model = nullptr;
    if (IsCookedModelPath(absolutePath.string()))
    {
        CookedModelArtifact artifact;
        std::string cookedError;
        if (!ReadCookedModel(absolutePath.string(), artifact, cookedError))
        {
            RVX_CORE_ERROR("ModelLoader: Failed to read cooked model '{}': {}", path, cookedError);
            return nullptr;
        }
        model = CreateCookedModelResource(resourceIdentityPath,
                                          absolutePath.string(),
                                          artifact,
                                          textureLoader,
                                          traceContext,
                                          cookedError);
        if (!model)
        {
            RVX_CORE_ERROR("ModelLoader: Failed to instantiate cooked model '{}': {}", path, cookedError);
            return nullptr;
        }
    }
    else
    {
        GLTFImporter importer;
        GLTFImportResult imported = importer.Import(absolutePath.string(), options, traceContext);
        if (!imported.success)
        {
            RVX_CORE_ERROR("ModelLoader: Failed to import '{}': {}", path, imported.errorMessage);
            return nullptr;
        }
        model = CreateModelResource(resourceIdentityPath,
                                    absolutePath.string(),
                                    imported,
                                    textureLoader,
                                    traceContext,
                                    false);
    }
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

    Diagnostics::TraceSpan importSpan = Diagnostics::BeginTraceSpan(
        context.traceContext, "ModelIO", {{"path", context.resolvedPath}});
    ModelResource* model = nullptr;
    if (IsCookedModelPath(context.resolvedPath))
    {
        CookedModelArtifact artifact;
        std::string cookedError;
        if (!ReadCookedModel(context.resolvedPath, artifact, cookedError))
        {
            importSpan.SetAttribute("result", "failed");
            outError = {ResourceLoadErrorCode::LoaderFailure, cookedError};
            return false;
        }
        importSpan.SetAttribute("format", "RVX_MODEL_PREBAKE_V1");
        importSpan.SetAttribute("result", "prepared");
        importSpan.End();

        Diagnostics::TraceSpan prepareSpan = Diagnostics::BeginTraceSpan(
            context.traceContext, "CPUPrepare", {{"path", context.resolvedPath}});
        TextureLoader textureLoader(nullptr, true);
        model = CreateCookedModelResource(context.resourceIdentityPath,
                                          context.resolvedPath,
                                          artifact,
                                          textureLoader,
                                          context.traceContext,
                                          cookedError);
        if (!model)
        {
            prepareSpan.SetAttribute("result", "failed");
            outError = {ResourceLoadErrorCode::LoaderFailure, cookedError};
            return false;
        }
        prepareSpan.SetAttribute("meshCount", model->GetMeshCount());
        prepareSpan.SetAttribute("materialCount", model->GetMaterialCount());
        prepareSpan.SetAttribute("result", "prepared");
    }
    else
    {
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
        importSpan.SetAttribute("result", "prepared");
        importSpan.End();

        Diagnostics::TraceSpan prepareSpan = Diagnostics::BeginTraceSpan(
            context.traceContext,
            "CPUPrepare",
            {{"path", context.resolvedPath}});
        TextureLoader textureLoader(nullptr, true);
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
    return BuildPreparedBundle(model, outBundle, outError);
}

bool ModelLoader::BuildPreparedBundle(ModelResource* model,
                                      PreparedResourceBundle& outBundle,
                                      ResourceLoadError& outError)
{
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

ModelResource* ModelLoader::CreateCookedModelResource(
    const std::string& resourceIdentityPath,
    const std::string& sourcePath,
    CookedModelArtifact& artifact,
    TextureLoader& textureLoader,
    const Diagnostics::TraceContext& traceContext,
    std::string& outError)
{
    const std::filesystem::path modelDirectory =
        std::filesystem::path(sourcePath).parent_path();
    std::filesystem::path meshPath;
    if (!ResolveContainedCookedDependency(modelDirectory,
                                          artifact.meshArtifactPath,
                                          meshPath))
    {
        outError = "Cooked model mesh dependency must resolve to a contained .rva product";
        return nullptr;
    }
    std::vector<CookedMeshRecord> cookedMeshes;
    if (!CookedMeshArtifactReader::ReadFile(meshPath.string(), cookedMeshes, outError))
        return nullptr;

    std::vector<ResourceHandle<TextureResource>> textures;
    textures.reserve(artifact.textures.size());
    for (TextureReference& reference : artifact.textures)
    {
        std::filesystem::path resolvedTexturePath;
        if (!ResolveContainedCookedDependency(modelDirectory,
                                              reference.path,
                                              resolvedTexturePath))
        {
            outError = "Cooked model texture dependency must resolve to a contained .rva product";
            return nullptr;
        }
        // Consume the exact canonical path that passed the containment check;
        // do not resolve the serialized path a second time in TextureLoader.
        reference.path = resolvedTexturePath.string();
        TextureResource* texture = textureLoader.LoadFromReference(
            reference,
            sourcePath,
            traceContext,
            resourceIdentityPath);
        if (!texture || textureLoader.WasLastLoadFallback())
        {
            outError = textureLoader.GetLastLoadError().empty()
                ? "Failed to load a cooked model texture dependency"
                : textureLoader.GetLastLoadError();
            return nullptr;
        }
        textures.emplace_back(texture);
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
        auto* mesh = new MeshResource();
        mesh->SetId(GenerateMeshId(resourceIdentityPath, static_cast<int>(index)));
        mesh->SetPath(resourceIdentityPath + "#mesh_" + std::to_string(index));
        mesh->SetName(record.lodMeshes.front()->name.empty()
                          ? "Mesh_" + std::to_string(index)
                          : record.lodMeshes.front()->name);
        mesh->SetLODMeshes(std::move(record.lodMeshes));
        meshes.emplace_back(mesh);
    }
    model->SetMeshes(std::move(meshes));

    std::vector<ResourceHandle<MaterialResource>> materials;
    materials.reserve(artifact.materials.size());
    GLTFImportResult compatibilityImportResult;
    for (size_t index = 0; index < artifact.materials.size(); ++index)
    {
        MaterialResource* material = CreateMaterialResource(
            resourceIdentityPath,
            static_cast<int>(index),
            artifact.materials[index],
            textures,
            compatibilityImportResult);
        if (!material)
        {
            outError = "Cooked model contains an invalid material record";
            return nullptr;
        }
        materials.emplace_back(material);
    }
    model->SetMaterials(std::move(materials));
    model->SetRootNode(std::move(artifact.rootNode));

    bool hierarchyValid = model->GetRootNode() != nullptr;
    if (hierarchyValid)
    {
        model->GetRootNode()->TraverseDepthFirst(
            [&](Node* node)
            {
                if (!node || node->GetMeshIndex() < -1 ||
                    node->GetMeshIndex() >= static_cast<int>(model->GetMeshCount()))
                {
                    hierarchyValid = false;
                    return;
                }
                for (const int materialIndex : node->GetMaterialIndices())
                {
                    if (materialIndex < 0 ||
                        materialIndex >= static_cast<int>(model->GetMaterialCount()))
                    {
                        hierarchyValid = false;
                        return;
                    }
                }
            });
    }
    if (!hierarchyValid)
    {
        outError = "Cooked model hierarchy references an invalid mesh or material index";
        return nullptr;
    }
    return model.release();
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
