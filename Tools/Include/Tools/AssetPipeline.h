/**
 * @file AssetPipeline.h
 * @brief Asset import and processing pipeline
 */

#pragma once

#include "Core/Types.h"
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <filesystem>

namespace RVX::Tools
{

namespace fs = std::filesystem;

/**
 * @brief Asset type enumeration
 */
enum class AssetType : uint8
{
    Unknown,
    Texture,
    Mesh,
    Material,
    Shader,
    Animation,
    Audio,
    Font,
    Prefab,
    Scene,
    Script
};

/**
 * @brief Asset import result
 */
struct ImportResult
{
    bool success = false;
    std::string error;
    std::vector<std::string> outputPaths;
    std::vector<std::string> warnings;
};

/**
 * @brief Base class for asset importers
 */
class IAssetImporter
{
public:
    virtual ~IAssetImporter() = default;

    virtual const char* GetName() const = 0;
    virtual std::vector<std::string> GetSupportedExtensions() const = 0;
    virtual AssetType GetAssetType() const = 0;

    virtual ImportResult Import(const fs::path& sourcePath,
                                 const fs::path& outputPath,
                                 const void* options = nullptr) = 0;
};

/**
 * @brief Texture import options
 */
struct TextureImportOptions
{
    bool generateMipmaps = true;
    bool sRGB = true;
    bool compress = true;
    int maxSize = 4096;
    bool flipY = true;
};

/**
 * @brief Mesh import options
 */
struct MeshImportOptions
{
    bool generateTangents = true;
    bool optimizeMesh = true;
    bool generateLODs = false;
    int lodCount = 3;
    float lodReductionFactor = 0.5f;
    float scaleFactor = 1.0f;
    bool importAnimations = true;
    bool importMaterials = true;
};

/**
 * @brief Asset pipeline for batch processing
 */
class AssetPipeline
{
public:
    using ProgressCallback = std::function<void(float progress, const std::string& status)>;

    AssetPipeline() = default;

    /**
     * @brief Register an importer
     */
    void RegisterImporter(std::unique_ptr<IAssetImporter> importer);

    /**
     * @brief Get importer for file extension
     */
    IAssetImporter* GetImporter(const std::string& extension) const;

    /**
     * @brief Import a single asset
     */
    ImportResult ImportAsset(const fs::path& sourcePath,
                              const fs::path& outputPath,
                              const void* options = nullptr);

    /**
     * @brief Import directory recursively
     */
    std::vector<ImportResult> ImportDirectory(const fs::path& sourceDir,
                                               const fs::path& outputDir,
                                               bool recursive = true,
                                               ProgressCallback callback = nullptr);

    /**
     * @brief Check if file needs reimport
     */
    bool NeedsReimport(const fs::path& sourcePath, const fs::path& outputPath) const;

    /**
     * @brief Get asset type from extension
     */
    static AssetType GetAssetTypeFromExtension(const std::string& ext);

private:
    std::vector<std::unique_ptr<IAssetImporter>> m_importers;
    std::unordered_map<std::string, IAssetImporter*> m_importersByExt;
};

/**
 * @brief Texture importer
 */
class TextureImporter : public IAssetImporter
{
public:
    const char* GetName() const override { return "TextureImporter"; }

    std::vector<std::string> GetSupportedExtensions() const override
    {
        return {".png", ".jpg", ".jpeg", ".tga", ".bmp", ".hdr", ".exr"};
    }

    AssetType GetAssetType() const override { return AssetType::Texture; }

    ImportResult Import(const fs::path& sourcePath,
                        const fs::path& outputPath,
                        const void* options = nullptr) override;
};

/**
 * @brief Mesh importer (FBX, OBJ, GLTF)
 */
class MeshImporter : public IAssetImporter
{
public:
    const char* GetName() const override { return "MeshImporter"; }

    std::vector<std::string> GetSupportedExtensions() const override
    {
        return {".fbx", ".obj", ".gltf", ".glb", ".dae"};
    }

    AssetType GetAssetType() const override { return AssetType::Mesh; }

    ImportResult Import(const fs::path& sourcePath,
                        const fs::path& outputPath,
                        const void* options = nullptr) override;
};

/**
 * @brief Shader importer/compiler
 */
class ShaderImporter : public IAssetImporter
{
public:
    const char* GetName() const override { return "ShaderImporter"; }

    std::vector<std::string> GetSupportedExtensions() const override
    {
        return {".hlsl", ".glsl", ".shader"};
    }

    AssetType GetAssetType() const override { return AssetType::Shader; }

    ImportResult Import(const fs::path& sourcePath,
                        const fs::path& outputPath,
                        const void* options = nullptr) override;
};

/**
 * @brief Audio importer
 */
class AudioImporter : public IAssetImporter
{
public:
    const char* GetName() const override { return "AudioImporter"; }

    std::vector<std::string> GetSupportedExtensions() const override
    {
        return {".wav", ".mp3", ".ogg", ".flac"};
    }

    AssetType GetAssetType() const override { return AssetType::Audio; }

    ImportResult Import(const fs::path& sourcePath,
                        const fs::path& outputPath,
                        const void* options = nullptr) override;
};

} // namespace RVX::Tools
