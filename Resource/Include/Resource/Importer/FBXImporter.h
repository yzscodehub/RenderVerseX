#pragma once

/**
 * @file FBXImporter.h
 * @brief FBX format importer using ufbx library
 * 
 * Parses FBX files and outputs Scene::Model, Mesh, Material,
 * and TextureReference for loading.
 * 
 * Key features:
 * - Supports FBX 2010-2020 formats (binary and ASCII)
 * - Imports meshes, materials, textures
 * - Imports skeleton hierarchy and bone data
 * - Imports animation clips
 * - Uses Node indices for mesh/material references
 */

#include "Scene/Model.h"
#include "Scene/Mesh.h"
#include "Scene/Material.h"
#include "Resource/Loader/TextureReference.h"
#include <string>
#include <vector>
#include <memory>
#include <functional>

// Forward declaration for ufbx (only when available)
#ifdef HAS_UFBX
struct ufbx_scene;
struct ufbx_mesh;
struct ufbx_material;
struct ufbx_node;
struct ufbx_anim_stack;
#else
// Dummy forward declarations when ufbx is not available
struct ufbx_scene {};
struct ufbx_mesh {};
struct ufbx_material {};
struct ufbx_node {};
struct ufbx_anim_stack {};
#endif

namespace RVX::Animation
{
    class AnimationClip;
}

namespace RVX::Resource
{
    /**
     * @brief Skeleton data extracted from FBX
     */
    struct FBXSkeleton
    {
        struct Bone
        {
            std::string name;
            int parentIndex = -1;
            Mat4 bindPose;
            Mat4 inverseBindPose;
        };

        std::vector<Bone> bones;
        std::vector<std::string> boneNames;  // For quick lookup
    };

    /**
     * @brief Animation clip data extracted from FBX
     */
    struct FBXAnimationClip
    {
        std::string name;
        float duration = 0.0f;      // Duration in seconds
        float framesPerSecond = 30.0f;

        struct BoneAnimation
        {
            std::string boneName;
            int boneIndex = -1;

            // Keyframe times (in seconds)
            std::vector<float> positionTimes;
            std::vector<float> rotationTimes;
            std::vector<float> scaleTimes;

            // Keyframe values
            std::vector<Vec3> positions;
            std::vector<Quat> rotations;
            std::vector<Vec3> scales;
        };

        std::vector<BoneAnimation> boneAnimations;
    };

    /**
     * @brief Result of an FBX import operation
     */
    struct FBXImportResult
    {
        /// Whether the import succeeded
        bool success = false;

        /// Error message if import failed
        std::string errorMessage;

        /// Warning messages (non-fatal issues)
        std::vector<std::string> warnings;

        /// The imported model with Node tree
        Model::Ptr model;

        /// All meshes extracted from the file
        std::vector<Mesh::Ptr> meshes;

        /// All materials extracted from the file
        std::vector<Material::Ptr> materials;

        /// All texture references (not loaded yet)
        std::vector<TextureReference> textures;

        /// Skeleton data (if present)
        std::unique_ptr<FBXSkeleton> skeleton;

        /// Animation clips (if present)
        std::vector<FBXAnimationClip> animations;

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
            skeleton.reset();
            animations.clear();
        }
    };

    /**
     * @brief Import options for FBX files
     */
    struct FBXImportOptions
    {
        /// Flip UVs vertically
        bool flipUVs = true;

        /// Generate normals if not present
        bool generateNormals = true;

        /// Generate tangents if not present (requires normals and UVs)
        bool generateTangents = true;

        /// Import skeleton/bone data
        bool importSkeleton = true;

        /// Import animations
        bool importAnimations = true;

        /// Scale factor for positions (FBX uses cm, we might want meters)
        float scaleFactor = 0.01f;  // Convert cm to meters

        /// Convert coordinate system (FBX is typically Y-up right-handed)
        bool convertToLeftHanded = false;

        /// Triangulate non-triangle faces
        bool triangulate = true;

        /// Merge meshes with the same material
        bool mergeMeshes = false;

        /// Preserve mesh pivot points
        bool preservePivots = true;

        /// Maximum bones per vertex for skinning
        int maxBonesPerVertex = 4;

        /// Animation sample rate (0 = use original keyframes)
        float animationSampleRate = 0.0f;
    };

    /**
     * @brief FBX format importer using ufbx library
     * 
     * This class handles parsing of FBX files (both binary and ASCII formats)
     * and produces Scene module types (Model, Mesh, Material) plus TextureReferences.
     * 
     * Also extracts skeleton and animation data when present.
     */
    class FBXImporter
    {
    public:
        FBXImporter() = default;
        ~FBXImporter() = default;

        // Non-copyable
        FBXImporter(const FBXImporter&) = delete;
        FBXImporter& operator=(const FBXImporter&) = delete;

        // =====================================================================
        // Import Interface
        // =====================================================================

        /**
         * @brief Get supported file extensions
         */
        std::vector<std::string> GetSupportedExtensions() const
        {
            return { ".fbx" };
        }

        /**
         * @brief Check if a file can be imported
         */
        bool CanImport(const std::string& path) const;

        /**
         * @brief Import an FBX file
         * 
         * @param path Absolute path to the file
         * @param options Import options (optional)
         * @return FBXImportResult containing the imported data
         */
        FBXImportResult Import(const std::string& path, 
                               const FBXImportOptions& options = FBXImportOptions());

        // =====================================================================
        // Progress Callback
        // =====================================================================

        using ProgressCallback = std::function<void(float progress, const std::string& stage)>;

        void SetProgressCallback(ProgressCallback callback) { m_progressCallback = std::move(callback); }

    private:
        // Scene parsing
        void ParseMaterials(ufbx_scene* scene, const std::string& basePath, FBXImportResult& result);
        void ParseMeshes(ufbx_scene* scene, FBXImportResult& result, const FBXImportOptions& options);
        void ParseNodes(ufbx_scene* scene, FBXImportResult& result, const FBXImportOptions& options);
        void ParseSkeleton(ufbx_scene* scene, FBXImportResult& result);
        void ParseAnimations(ufbx_scene* scene, FBXImportResult& result, const FBXImportOptions& options);

        // Conversion helpers
        Mesh::Ptr ConvertMesh(ufbx_scene* scene, ufbx_mesh* mesh, 
                              const FBXImportOptions& options, int meshIndex);
        Material::Ptr ConvertMaterial(ufbx_scene* scene, ufbx_material* mat, 
                                       const std::string& basePath, int materialIndex,
                                       FBXImportResult& result);
        Node::Ptr ConvertNode(ufbx_scene* scene, ufbx_node* node, 
                              const FBXImportOptions& options,
                              const std::unordered_map<ufbx_mesh*, int>& meshIndexMap);

        // Texture extraction
        TextureReference ExtractTexture(ufbx_scene* scene, void* texturePtr,
                                        const std::string& basePath, TextureUsage usage);

        // Animation conversion
        FBXAnimationClip ConvertAnimation(ufbx_scene* scene, ufbx_anim_stack* animStack,
                                          const FBXImportOptions& options,
                                          const FBXSkeleton* skeleton);

        // Coordinate system conversion
        Vec3 ConvertPosition(float x, float y, float z, const FBXImportOptions& options) const;
        Quat ConvertRotation(float x, float y, float z, float w, const FBXImportOptions& options) const;
        Mat4 ConvertMatrix(const float* data, const FBXImportOptions& options) const;

        // Utility
        void ReportProgress(float progress, const std::string& stage);
        PrimitiveType GetPrimitiveType(int faceSize);

        // Member variables
        ProgressCallback m_progressCallback;
        std::string m_currentFilePath;
        std::vector<Node::Ptr> m_parsedNodes;
    };

} // namespace RVX::Resource
