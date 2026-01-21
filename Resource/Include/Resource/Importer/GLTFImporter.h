#pragma once

/**
 * @file GLTFImporter.h
 * @brief glTF/GLB format importer
 * 
 * Parses glTF 2.0 files and outputs Scene::Model, Mesh, Material,
 * and TextureReference for loading.
 * 
 * Key features:
 * - Uses Node indices (meshIndex, materialIndices) instead of MeshComponent
 * - Extracts TextureReferences for lazy loading
 * - Supports both .gltf (JSON + external files) and .glb (binary)
 */

#include "Scene/Model.h"
#include "Scene/Mesh.h"
#include "Scene/Material.h"
#include "Resource/Loader/TextureReference.h"
#include <string>
#include <vector>
#include <memory>
#include <functional>

// Forward declaration for tinygltf
namespace tinygltf
{
    class Model;
    struct Mesh;
    struct Material;
    struct Primitive;
    struct Accessor;
    struct BufferView;
    struct Node;
    struct Scene;
}

namespace RVX::Resource
{
    /**
     * @brief Result of a glTF import operation
     */
    struct GLTFImportResult
    {
        /// Whether the import succeeded
        bool success = false;

        /// Error message if import failed
        std::string errorMessage;

        /// Warning messages (non-fatal issues)
        std::vector<std::string> warnings;

        /// The imported model with Node tree
        Model::Ptr model;

        /// All meshes extracted from the file (indexed by glTF mesh index)
        std::vector<Mesh::Ptr> meshes;

        /// All materials extracted from the file (indexed by glTF material index)
        std::vector<Material::Ptr> materials;

        /// All texture references (indexed by glTF texture index)
        /// These are not loaded yet - the ModelLoader will handle that
        std::vector<TextureReference> textures;

        /// Clear all data
        void Clear()
        {
            success = false;
            errorMessage.clear();
            warnings.clear();
            model.reset();
            meshes.clear();
            materials.clear();
            textures.clear();
        }
    };

    /**
     * @brief Import options for glTF files
     */
    struct GLTFImportOptions
    {
        /// Flip UVs vertically (some renderers need this)
        bool flipUVs = false;

        /// Generate normals if not present
        bool generateNormals = true;

        /// Generate tangents if not present (requires normals and UVs)
        bool generateTangents = true;

        /// Merge meshes with the same material
        bool mergeMeshes = false;

        /// Scale factor for positions
        float scaleFactor = 1.0f;
    };

    /**
     * @brief glTF/GLB format importer
     * 
     * This class handles parsing of glTF 2.0 files (both .gltf and .glb formats)
     * and produces Scene module types (Model, Mesh, Material) plus TextureReferences.
     * 
     * The importer outputs Node indices rather than attaching MeshComponent objects,
     * following the Prefab/Instantiate pattern for resource management.
     */
    class GLTFImporter
    {
    public:
        GLTFImporter() = default;
        ~GLTFImporter() = default;

        // Non-copyable
        GLTFImporter(const GLTFImporter&) = delete;
        GLTFImporter& operator=(const GLTFImporter&) = delete;

        // =====================================================================
        // Import Interface
        // =====================================================================

        /**
         * @brief Get supported file extensions
         */
        std::vector<std::string> GetSupportedExtensions() const
        {
            return { ".gltf", ".glb" };
        }

        /**
         * @brief Check if a file can be imported
         */
        bool CanImport(const std::string& path) const;

        /**
         * @brief Import a glTF/GLB file
         * 
         * @param path Absolute path to the file
         * @param options Import options (optional)
         * @return GLTFImportResult containing the imported data
         */
        GLTFImportResult Import(const std::string& path, 
                                 const GLTFImportOptions& options = GLTFImportOptions());

        // =====================================================================
        // Progress Callback
        // =====================================================================

        using ProgressCallback = std::function<void(float progress, const std::string& stage)>;

        void SetProgressCallback(ProgressCallback callback) { m_progressCallback = std::move(callback); }

    private:
        // Parsing methods
        bool LoadFile(const std::string& path, tinygltf::Model& gltfModel, std::string& error, std::string& warning);
        
        void ParseMeshes(const tinygltf::Model& gltf, GLTFImportResult& result, const GLTFImportOptions& options);
        void ParseMaterials(const tinygltf::Model& gltf, GLTFImportResult& result);
        void ExtractTextures(const tinygltf::Model& gltf, const std::string& basePath, GLTFImportResult& result);
        void ParseNodes(const tinygltf::Model& gltf, GLTFImportResult& result);
        void ParseScene(const tinygltf::Model& gltf, GLTFImportResult& result);

        // Conversion helpers
        Mesh::Ptr ConvertPrimitive(const tinygltf::Model& gltf, 
                                    const tinygltf::Primitive& primitive,
                                    const std::string& meshName,
                                    int primitiveIndex,
                                    const GLTFImportOptions& options);

        Material::Ptr ConvertMaterial(const tinygltf::Model& gltf, 
                                       const tinygltf::Material& mat,
                                       int materialIndex);

        Node::Ptr ConvertNode(const tinygltf::Model& gltf, 
                               int nodeIndex,
                               const std::vector<int>& meshToFirstPrimitiveIndex);

        // Data extraction helpers
        template<typename T>
        std::vector<T> ExtractAccessorData(const tinygltf::Model& gltf, int accessorIndex);

        std::vector<uint32_t> ExtractIndices(const tinygltf::Model& gltf, int accessorIndex);

        TextureInfo ExtractTextureInfo(const tinygltf::Model& gltf, int textureIndex, int uvSet = 0);

        // Utility
        PrimitiveType ConvertPrimitiveMode(int mode);
        void ReportProgress(float progress, const std::string& stage);

        // Member variables
        ProgressCallback m_progressCallback;
        std::string m_currentFilePath;
        std::vector<Node::Ptr> m_parsedNodes;  // Temporary storage during parsing
    };

} // namespace RVX::Resource
