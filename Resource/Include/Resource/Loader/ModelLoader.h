#pragma once

/**
 * @file ModelLoader.h
 * @brief Model resource loader
 * 
 * Loads 3D models from various formats (glTF, GLB, etc.).
 * Creates ModelResource with properly indexed MeshResources and MaterialResources.
 */

#include "Resource/Cooked/CookedModelArtifact.h"
#include "Resource/Importer/GLTFImporter.h"
#include "Resource/Loader/TextureLoader.h"
#include "Resource/ResourceManager.h"
#include "Resource/Types/ModelResource.h"
#include "Resource/Types/TextureResource.h"
#include <mutex>
#include <string>

namespace RVX::Resource
{
    /**
     * @brief Model resource loader
     * 
     * Features:
     * - Loads source glTF/GLB and cooked RVX model products
     * - Creates ModelResource with indexed MeshResources and MaterialResources
     * - Uses modelPath#type_index format for ResourceId generation
     * - Integrates with TextureLoader for texture loading
     */
    class ModelLoader : public IResourceLoader
    {
    public:
        explicit ModelLoader(ResourceManager* manager);
        ~ModelLoader() override = default;

        // =====================================================================
        // IResourceLoader Interface
        // =====================================================================

        ResourceType GetResourceType() const override { return ResourceType::Model; }
        std::vector<std::string> GetSupportedExtensions() const override;
        IResource* Load(const std::string& path) override;
        bool Prepare(const ResourceLoadPreparationContext& context,
                     PreparedResourceBundle& outBundle,
                     ResourceLoadError& outError) override;
        bool SupportsPreparedLoading() const override { return true; }
        bool CapturePreparationState(
            uint64 requestedImportOptionsHash,
            ResourceLoadPreparationStateRef& outState,
            uint64& outCanonicalImportOptionsHash,
            ResourceLoadError& outError) const override;
        bool CanLoad(const std::string& path) const override;

        // =====================================================================
        // Configuration
        // =====================================================================

        /**
         * @brief Set import options for glTF files
         */
        void SetGLTFImportOptions(const GLTFImportOptions& options);

        /** @brief Deterministic AssetKey hash for a concrete glTF import profile. */
        static uint64 CalculateImportOptionsHash(const GLTFImportOptions& options);

    private:
        // ResourceId generation (ensures global uniqueness)
        ResourceId GenerateModelId(const std::string& modelPath);
        ResourceId GenerateMeshId(const std::string& modelPath, int index);
        ResourceId GenerateMaterialId(const std::string& modelPath, int index);

        // Create resources from import result
        ModelResource* CreateModelResource(const std::string& resourceIdentityPath,
                                           const std::string& sourcePath,
                                           GLTFImportResult& importResult,
                                           TextureLoader& textureLoader,
                                           const Diagnostics::TraceContext& traceContext,
                                           bool streamTextures);
        ModelResource* CreateCookedModelResource(
            const std::string& resourceIdentityPath,
            const std::string& sourcePath,
            CookedModelArtifact& artifact,
            TextureLoader& textureLoader,
            const Diagnostics::TraceContext& traceContext,
            std::string& outError);
        bool BuildPreparedBundle(ModelResource* model,
                                 PreparedResourceBundle& outBundle,
                                 ResourceLoadError& outError);
        MeshResource* CreateMeshResource(const std::string& modelPath, int index, Mesh::Ptr mesh);
        MaterialResource* CreateMaterialResource(const std::string& modelPath, int index,
                                                   Material::Ptr material,
                                                   const std::vector<ResourceHandle<TextureResource>>& textures,
                                                   const GLTFImportResult& importResult);

        // Load textures from import result
        std::vector<ResourceHandle<TextureResource>> LoadTextures(
            const std::string& sourceModelPath,
            const std::string& resourceIdentityPath,
            std::vector<TextureReference>& textureRefs,
            TextureLoader& textureLoader,
            const Diagnostics::TraceContext& traceContext,
            bool streamTextures,
            std::vector<ModelTextureStreamingSource>& outStreamingSources);

        ResourceManager* m_manager = nullptr;
        mutable std::mutex m_optionsMutex;
        GLTFImportOptions m_gltfOptions;
    };

} // namespace RVX::Resource
