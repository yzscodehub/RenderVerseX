#pragma once

/**
 * @file FBXImporter.h
 * @brief Verified static-model FBX import contract.
 *
 * The public contract intentionally contains no parser-library types. FBX is
 * a development-only source format and the supported subset is limited to
 * static meshes, materials, external textures, and node transforms.
 */

#include "Geometry/Asset/Material.h"
#include "Geometry/Asset/Mesh.h"
#include "Geometry/Asset/Model.h"
#include "Resource/Loader/TextureReference.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace RVX::Resource
{
    /** @brief Result of a static FBX import operation. */
    struct FBXImportResult
    {
        bool success = false;
        std::string errorMessage;
        std::vector<std::string> warnings;
        Model::Ptr model;
        std::vector<Mesh::Ptr> meshes;
        std::vector<Material::Ptr> materials;
        std::vector<TextureReference> textures;
    };

    /**
     * @brief Options for the supported static FBX subset.
     *
     * Triangulation, node-transform preservation, right-handed Y-up space,
     * and one engine unit per meter are mandatory and are not configurable.
     */
    struct FBXImportOptions
    {
        bool generateNormals = true;
        bool generateTangents = true;
        bool flipUVs = false;
    };

    /** @brief Development-only importer for the verified static FBX subset. */
    class FBXImporter
    {
    public:
        using ProgressCallback =
            std::function<void(float progress, const std::string& stage)>;

        FBXImporter() = default;
        ~FBXImporter() = default;

        FBXImporter(const FBXImporter&) = delete;
        FBXImporter& operator=(const FBXImporter&) = delete;

        [[nodiscard]] std::vector<std::string> GetSupportedExtensions() const
        {
            return {".fbx"};
        }

        [[nodiscard]] bool CanImport(const std::string& path) const;

        [[nodiscard]] FBXImportResult Import(
            const std::string& path,
            const FBXImportOptions& options = FBXImportOptions{});

        void SetProgressCallback(ProgressCallback callback)
        {
            m_progressCallback = std::move(callback);
        }

    private:
        void ReportProgress(float progress, const std::string& stage) const;

        ProgressCallback m_progressCallback;
    };
} // namespace RVX::Resource
