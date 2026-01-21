#include "Resource/Loader/ModelLoader.h"
#include "Resource/ResourceCache.h"
#include "Core/Log.h"

#include <filesystem>
#include <functional>
#include <algorithm>

namespace RVX::Resource
{
    // =========================================================================
    // Construction
    // =========================================================================

    ModelLoader::ModelLoader(ResourceManager* manager)
        : m_manager(manager)
        , m_gltfImporter(std::make_unique<GLTFImporter>())
        , m_textureLoader(std::make_unique<TextureLoader>(manager))
    {
    }

    // =========================================================================
    // IResourceLoader Interface
    // =========================================================================

    std::vector<std::string> ModelLoader::GetSupportedExtensions() const
    {
        return { ".gltf", ".glb" };
    }

    bool ModelLoader::CanLoad(const std::string& path) const
    {
        std::filesystem::path filePath(path);
        std::string ext = filePath.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        auto extensions = GetSupportedExtensions();
        return std::find(extensions.begin(), extensions.end(), ext) != extensions.end();
    }

    IResource* ModelLoader::Load(const std::string& path)
    {
        // Resolve to absolute path
        std::filesystem::path absPath = std::filesystem::absolute(path);
        std::string absolutePath = absPath.string();

        ResourceId modelId = GenerateModelId(absolutePath);

        // Check cache first
        if (m_manager && m_manager->IsInitialized())
        {
            if (auto* cached = m_manager->GetCache().Get(modelId))
            {
                return cached;
            }
        }

        // Import the model
        RVX_CORE_INFO("ModelLoader: Loading model from {}", absolutePath);
        GLTFImportResult importResult = m_gltfImporter->Import(absolutePath, m_gltfOptions);

        if (!importResult.success)
        {
            RVX_CORE_ERROR("ModelLoader: Failed to import model: {}", importResult.errorMessage);
            return nullptr;
        }

        // Log warnings
        for (const auto& warning : importResult.warnings)
        {
            RVX_CORE_WARN("ModelLoader: {}", warning);
        }

        // Create the model resource
        ModelResource* modelResource = CreateModelResource(absolutePath, importResult);
        
        if (modelResource)
        {
            modelResource->SetId(modelId);
            modelResource->SetPath(absolutePath);
            
            std::filesystem::path filePath(absolutePath);
            modelResource->SetName(filePath.stem().string());
            modelResource->NotifyLoaded();

            // Store in cache
            if (m_manager && m_manager->IsInitialized())
            {
                m_manager->GetCache().Store(modelResource);
            }

            RVX_CORE_INFO("ModelLoader: Loaded model '{}' with {} meshes, {} materials, {} textures",
                     modelResource->GetName(),
                     modelResource->GetMeshCount(),
                     modelResource->GetMaterialCount(),
                     importResult.textures.size());
        }

        return modelResource;
    }

    // =========================================================================
    // Resource Creation
    // =========================================================================

    ModelResource* ModelLoader::CreateModelResource(const std::string& path, GLTFImportResult& importResult)
    {
        auto* modelResource = new ModelResource();

        // 1. Load textures first (materials depend on them)
        std::vector<TextureResource*> loadedTextures = LoadTextures(path, importResult.textures);

        // 2. Create MeshResources
        std::vector<ResourceHandle<MeshResource>> meshHandles;
        meshHandles.reserve(importResult.meshes.size());

        for (size_t i = 0; i < importResult.meshes.size(); ++i)
        {
            MeshResource* meshRes = CreateMeshResource(path, static_cast<int>(i), importResult.meshes[i]);
            if (meshRes)
            {
                meshHandles.emplace_back(meshRes);
            }
        }
        modelResource->SetMeshes(std::move(meshHandles));

        // 3. Create MaterialResources
        std::vector<ResourceHandle<MaterialResource>> materialHandles;
        materialHandles.reserve(importResult.materials.size());

        for (size_t i = 0; i < importResult.materials.size(); ++i)
        {
            MaterialResource* matRes = CreateMaterialResource(path, static_cast<int>(i),
                                                                importResult.materials[i],
                                                                loadedTextures,
                                                                importResult);
            if (matRes)
            {
                materialHandles.emplace_back(matRes);
            }
        }
        modelResource->SetMaterials(std::move(materialHandles));

        // 4. Set the root node from the imported model
        if (importResult.model)
        {
            modelResource->SetRootNode(importResult.model->GetRootNode());
        }

        return modelResource;
    }

    MeshResource* ModelLoader::CreateMeshResource(const std::string& modelPath, int index, Mesh::Ptr mesh)
    {
        if (!mesh)
        {
            return nullptr;
        }

        ResourceId meshId = GenerateMeshId(modelPath, index);

        // Check cache
        if (m_manager && m_manager->IsInitialized())
        {
            if (auto* cached = m_manager->GetCache().Get(meshId))
            {
                return static_cast<MeshResource*>(cached);
            }
        }

        auto* meshResource = new MeshResource();
        meshResource->SetId(meshId);
        meshResource->SetPath(modelPath + "#mesh_" + std::to_string(index));
        meshResource->SetName(mesh->name.empty() ? "Mesh_" + std::to_string(index) : mesh->name);
        meshResource->SetMesh(mesh);
        meshResource->NotifyLoaded();

        // Store in cache
        if (m_manager && m_manager->IsInitialized())
        {
            m_manager->GetCache().Store(meshResource);
        }

        return meshResource;
    }

    MaterialResource* ModelLoader::CreateMaterialResource(const std::string& modelPath, int index,
                                                            Material::Ptr material,
                                                            const std::vector<TextureResource*>& textures,
                                                            const GLTFImportResult& importResult)
    {
        if (!material)
        {
            return nullptr;
        }

        ResourceId materialId = GenerateMaterialId(modelPath, index);

        // Check cache
        if (m_manager && m_manager->IsInitialized())
        {
            if (auto* cached = m_manager->GetCache().Get(materialId))
            {
                return static_cast<MaterialResource*>(cached);
            }
        }

        auto* materialResource = new MaterialResource();
        materialResource->SetId(materialId);
        materialResource->SetPath(modelPath + "#material_" + std::to_string(index));
        materialResource->SetName(material->GetName());
        materialResource->SetMaterialData(material);

        // Associate textures
        auto associateTexture = [&](const std::optional<TextureInfo>& info, const std::string& slot) {
            if (info && info->imageId >= 0 && info->imageId < static_cast<int>(textures.size()))
            {
                TextureResource* tex = textures[info->imageId];
                if (tex)
                {
                    materialResource->SetTexture(slot, ResourceHandle<TextureResource>(tex));
                }
            }
        };

        // Map Material textures to MaterialResource texture slots
        associateTexture(material->GetBaseColorTexture(), "albedo");
        associateTexture(material->GetNormalTexture(), "normal");
        associateTexture(material->GetMetallicRoughnessTexture(), "metallic_roughness");
        associateTexture(material->GetOcclusionTexture(), "ao");
        associateTexture(material->GetEmissiveTexture(), "emissive");

        materialResource->NotifyLoaded();

        // Store in cache
        if (m_manager && m_manager->IsInitialized())
        {
            m_manager->GetCache().Store(materialResource);
        }

        return materialResource;
    }

    std::vector<TextureResource*> ModelLoader::LoadTextures(const std::string& modelPath,
                                                              const std::vector<TextureReference>& textureRefs)
    {
        std::vector<TextureResource*> loadedTextures;
        loadedTextures.reserve(textureRefs.size());

        for (const auto& ref : textureRefs)
        {
            TextureResource* tex = m_textureLoader->LoadFromReference(ref, modelPath);
            loadedTextures.push_back(tex);
        }

        return loadedTextures;
    }

    // =========================================================================
    // ResourceId Generation
    // =========================================================================

    ResourceId ModelLoader::GenerateModelId(const std::string& modelPath)
    {
        std::hash<std::string> hasher;
        return static_cast<ResourceId>(hasher(modelPath));
    }

    ResourceId ModelLoader::GenerateMeshId(const std::string& modelPath, int index)
    {
        std::string key = modelPath + "#mesh_" + std::to_string(index);
        std::hash<std::string> hasher;
        return static_cast<ResourceId>(hasher(key));
    }

    ResourceId ModelLoader::GenerateMaterialId(const std::string& modelPath, int index)
    {
        std::string key = modelPath + "#material_" + std::to_string(index);
        std::hash<std::string> hasher;
        return static_cast<ResourceId>(hasher(key));
    }

} // namespace RVX::Resource
