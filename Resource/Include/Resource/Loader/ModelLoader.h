#pragma once

/**
 * @file ModelLoader.h
 * @brief Model resource loader
 * 
 * Loads 3D models from various formats (glTF, GLB, etc.).
 * Creates ModelResource with properly indexed MeshResources and MaterialResources.
 */

#include "Resource/ResourceManager.h"
#include "Resource/Types/ModelResource.h"
#include "Resource/Types/TextureResource.h"
#include "Resource/Importer/GLTFImporter.h"
#include "Resource/Loader/TextureLoader.h"
#include <string>
#include <memory>

namespace RVX::Resource
{
    /**
     * @brief Model resource loader
     * 
     * Features:
     * - Loads models from glTF/GLB formats
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
        bool CanLoad(const std::string& path) const override;

        // =====================================================================
        // Configuration
        // =====================================================================

        /**
         * @brief Set import options for glTF files
         */
        void SetGLTFImportOptions(const GLTFImportOptions& options) { m_gltfOptions = options; }

        /**
         * @brief Get the texture loader used by this model loader
         */
        TextureLoader* GetTextureLoader() { return m_textureLoader.get(); }

    private:
        // ResourceId generation (ensures global uniqueness)
        ResourceId GenerateModelId(const std::string& modelPath);
        ResourceId GenerateMeshId(const std::string& modelPath, int index);
        ResourceId GenerateMaterialId(const std::string& modelPath, int index);

        // Create resources from import result
        ModelResource* CreateModelResource(const std::string& path, GLTFImportResult& importResult);
        MeshResource* CreateMeshResource(const std::string& modelPath, int index, Mesh::Ptr mesh);
        MaterialResource* CreateMaterialResource(const std::string& modelPath, int index,
                                                   Material::Ptr material,
                                                   const std::vector<TextureResource*>& textures,
                                                   const GLTFImportResult& importResult);

        // Load textures from import result
        std::vector<TextureResource*> LoadTextures(const std::string& modelPath,
                                                    const std::vector<TextureReference>& textureRefs);

        ResourceManager* m_manager;
        std::unique_ptr<GLTFImporter> m_gltfImporter;
        std::unique_ptr<TextureLoader> m_textureLoader;
        GLTFImportOptions m_gltfOptions;
    };

} // namespace RVX::Resource
