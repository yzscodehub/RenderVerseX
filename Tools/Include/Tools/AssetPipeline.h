/**
 * @file AssetPipeline.h
 * @brief Asset import and processing pipeline
 */

#pragma once

#include "Core/Types.h"
#include "RHI/RHIDefinitions.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace RVX
{
    class IShaderCompiler;
} // namespace RVX

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
 * @brief One entry in a cooked asset manifest
 */
struct CookManifestEntry
{
    std::string sourcePath;
    std::string outputPath;
    AssetType type = AssetType::Unknown;
    bool success = false;
    std::string error;
    std::vector<std::string> warnings;
    uint64 sourceModTime = 0;
    uint64 outputModTime = 0;
    uint64 outputSize = 0;
};

/**
 * @brief Deterministic manifest produced by a directory cook
 */
struct CookManifest
{
    static constexpr uint32 Version = 1;

    std::string sourceRoot;
    std::string outputRoot;
    bool recursive = true;
    bool manifestWritten = false;
    std::string manifestError;
    std::vector<CookManifestEntry> entries;

    size_t GetSuccessCount() const;
    size_t GetFailureCount() const;
    bool Save(const fs::path& manifestPath, std::string& outError) const;
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
 * @brief Texture compression mode selection
 */
enum class TextureCompressionMode : uint8
{
    Auto,
    None,
    BC1,
    BC3,
    BC5,
    BC7
};

/**
 * @brief Texture import options
 */
struct TextureImportOptions
{
    bool generateMipmaps = true;
    bool sRGB = true;
    bool compress = true;
    TextureCompressionMode compressionMode = TextureCompressionMode::Auto;
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
 * @brief Shader import options
 */
struct ShaderImportOptions
{
    RHIShaderStage stage = RHIShaderStage::None;
    RHIBackendType targetBackend = RHIBackendType::DX12;
    std::string entryPoint = "main";
    std::string targetProfile;
    bool enableDebugInfo = false;
    bool enableOptimization = true;
};

/**
 * @brief Asset pipeline for batch processing
 */
class AssetPipeline
{
public:
    using ProgressCallback = std::function<void(float progress, const std::string& status)>;
    using ImportOptionsProvider = std::function<const void*(const fs::path& sourcePath, AssetType assetType)>;

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
     * @brief Import a directory and optionally write a deterministic cook manifest
     */
    CookManifest CookDirectory(const fs::path& sourceDir,
                               const fs::path& outputDir,
                               bool recursive = true,
                               const fs::path& manifestPath = {},
                               ProgressCallback callback = nullptr,
                               ImportOptionsProvider optionsProvider = nullptr);

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
 * @brief Mesh importer
 */
class MeshImporter : public IAssetImporter
{
public:
    const char* GetName() const override { return "MeshImporter"; }

    std::vector<std::string> GetSupportedExtensions() const override
    {
        return {".gltf", ".glb"};
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
    using CompilerFactory =
        std::function<std::unique_ptr<RVX::IShaderCompiler>()>;

    ShaderImporter();
    explicit ShaderImporter(CompilerFactory compilerFactory);

    const char* GetName() const override { return "ShaderImporter"; }

    std::vector<std::string> GetSupportedExtensions() const override
    {
        return {".hlsl", ".glsl", ".shader"};
    }

    AssetType GetAssetType() const override { return AssetType::Shader; }

    ImportResult Import(const fs::path& sourcePath,
                        const fs::path& outputPath,
                        const void* options = nullptr) override;

private:
    CompilerFactory m_compilerFactory;
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
