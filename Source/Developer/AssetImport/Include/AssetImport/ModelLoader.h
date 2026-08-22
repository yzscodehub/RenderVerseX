#pragma once

/**
 * @file ModelLoader.h
 * @brief Model resource loader
 *
 * Loads source glTF and GLB models for the legacy direct loader path.
 * Creates ModelResource with properly indexed MeshResources and MaterialResources.
 * Canonical RVXCook import dispatch, including feature-enabled static FBX,
 * lives behind ModelImporter and converges on Model Artifact V3.
 */

#include "AssetImport/GLTFImporter.h"
#include "AssetImport/TextureLoader.h"
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
     * - Loads source glTF/GLB model assets only
     * - Creates ModelResource with indexed MeshResources and MaterialResources
     * - Uses modelPath#type_index format for ResourceId generation
     * - Integrates with TextureLoader for texture loading
     */
    class ModelLoader : public IResourceLoader
    {
    public:
        ModelLoader() = default;
        ~ModelLoader() override = default;

        // =====================================================================
        // IResourceLoader Interface
        // =====================================================================

        ResourceType GetResourceType() const override { return ResourceType::Model; }
        std::vector<std::string> GetSupportedExtensions() const override;
        bool Supports(
            const ResourceLoaderRequest& request) const noexcept override;
        bool Prepare(const ResourceLoadPreparationContext& context,
                     PreparedResourceBundle& outBundle,
                     ResourceLoadError& outError) override;
        bool CapturePreparationState(
            uint64 requestedImportOptionsHash,
            ResourceLoadPreparationStateRef& outState,
            uint64& outCanonicalImportOptionsHash,
            ResourceLoadError& outError) const override;

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
        [[nodiscard]] bool SupportsPath(
            const std::string& path) const noexcept;
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

        mutable std::mutex m_optionsMutex;
        GLTFImportOptions m_gltfOptions;
    };

} // namespace RVX::Resource
