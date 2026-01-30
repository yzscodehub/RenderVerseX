/**
 * @file AssetPipeline.cpp
 * @brief Asset pipeline implementation
 */

#include "Tools/AssetPipeline.h"
#include "Core/Log.h"

namespace RVX::Tools
{

void AssetPipeline::RegisterImporter(std::unique_ptr<IAssetImporter> importer)
{
    if (!importer) return;

    for (const auto& ext : importer->GetSupportedExtensions())
    {
        m_importersByExt[ext] = importer.get();
    }
    m_importers.push_back(std::move(importer));
}

IAssetImporter* AssetPipeline::GetImporter(const std::string& extension) const
{
    auto it = m_importersByExt.find(extension);
    return (it != m_importersByExt.end()) ? it->second : nullptr;
}

ImportResult AssetPipeline::ImportAsset(const fs::path& sourcePath,
                                         const fs::path& outputPath,
                                         const void* options)
{
    ImportResult result;

    if (!fs::exists(sourcePath))
    {
        result.error = "Source file does not exist: " + sourcePath.string();
        return result;
    }

    std::string ext = sourcePath.extension().string();
    IAssetImporter* importer = GetImporter(ext);
    if (!importer)
    {
        result.error = "No importer found for extension: " + ext;
        return result;
    }

    return importer->Import(sourcePath, outputPath, options);
}

std::vector<ImportResult> AssetPipeline::ImportDirectory(const fs::path& sourceDir,
                                                          const fs::path& outputDir,
                                                          bool recursive,
                                                          ProgressCallback callback)
{
    std::vector<ImportResult> results;

    if (!fs::exists(sourceDir) || !fs::is_directory(sourceDir))
    {
        return results;
    }

    // Collect files to import
    std::vector<fs::path> filesToImport;
    if (recursive)
    {
        for (const auto& entry : fs::recursive_directory_iterator(sourceDir))
        {
            if (entry.is_regular_file())
            {
                filesToImport.push_back(entry.path());
            }
        }
    }
    else
    {
        for (const auto& entry : fs::directory_iterator(sourceDir))
        {
            if (entry.is_regular_file())
            {
                filesToImport.push_back(entry.path());
            }
        }
    }

    // Import each file
    size_t processed = 0;
    for (const auto& filePath : filesToImport)
    {
        std::string ext = filePath.extension().string();
        if (GetImporter(ext))
        {
            fs::path relativePath = fs::relative(filePath, sourceDir);
            fs::path outPath = outputDir / relativePath;
            outPath.replace_extension(".rva");  // RenderVerseX Asset

            fs::create_directories(outPath.parent_path());

            ImportResult result = ImportAsset(filePath, outPath);
            results.push_back(result);

            if (callback)
            {
                float progress = static_cast<float>(++processed) / filesToImport.size();
                callback(progress, filePath.filename().string());
            }
        }
    }

    return results;
}

bool AssetPipeline::NeedsReimport(const fs::path& sourcePath, const fs::path& outputPath) const
{
    if (!fs::exists(outputPath))
    {
        return true;
    }

    auto sourceTime = fs::last_write_time(sourcePath);
    auto outputTime = fs::last_write_time(outputPath);
    return sourceTime > outputTime;
}

AssetType AssetPipeline::GetAssetTypeFromExtension(const std::string& ext)
{
    static const std::unordered_map<std::string, AssetType> extToType = {
        {".png", AssetType::Texture},
        {".jpg", AssetType::Texture},
        {".jpeg", AssetType::Texture},
        {".tga", AssetType::Texture},
        {".bmp", AssetType::Texture},
        {".hdr", AssetType::Texture},
        {".exr", AssetType::Texture},
        {".fbx", AssetType::Mesh},
        {".obj", AssetType::Mesh},
        {".gltf", AssetType::Mesh},
        {".glb", AssetType::Mesh},
        {".dae", AssetType::Mesh},
        {".hlsl", AssetType::Shader},
        {".glsl", AssetType::Shader},
        {".shader", AssetType::Shader},
        {".wav", AssetType::Audio},
        {".mp3", AssetType::Audio},
        {".ogg", AssetType::Audio},
        {".flac", AssetType::Audio},
        {".ttf", AssetType::Font},
        {".otf", AssetType::Font},
    };

    auto it = extToType.find(ext);
    return (it != extToType.end()) ? it->second : AssetType::Unknown;
}

// ============================================================================
// TextureImporter
// ============================================================================

ImportResult TextureImporter::Import(const fs::path& sourcePath,
                                      const fs::path& outputPath,
                                      const void* options)
{
    ImportResult result;

    const TextureImportOptions* texOptions = options 
        ? static_cast<const TextureImportOptions*>(options)
        : nullptr;

    // TODO: Load image using stb_image or similar
    // TODO: Generate mipmaps if requested
    // TODO: Compress to BCn format if requested
    // TODO: Write to custom texture format

    RVX_CORE_INFO("Importing texture: {}", sourcePath.string());

    result.success = true;
    result.outputPaths.push_back(outputPath.string());
    return result;
}

// ============================================================================
// MeshImporter
// ============================================================================

ImportResult MeshImporter::Import(const fs::path& sourcePath,
                                   const fs::path& outputPath,
                                   const void* options)
{
    ImportResult result;

    // TODO: Load mesh using Assimp
    // TODO: Process vertices, generate tangents
    // TODO: Optimize mesh (meshoptimizer)
    // TODO: Generate LODs if requested
    // TODO: Write to custom mesh format

    RVX_CORE_INFO("Importing mesh: {}", sourcePath.string());

    result.success = true;
    result.outputPaths.push_back(outputPath.string());
    return result;
}

// ============================================================================
// ShaderImporter
// ============================================================================

ImportResult ShaderImporter::Import(const fs::path& sourcePath,
                                     const fs::path& outputPath,
                                     const void* options)
{
    ImportResult result;

    // TODO: Compile shader using DXC/glslc
    // TODO: Generate SPIR-V, DXIL, Metal bytecode
    // TODO: Reflect shader bindings
    // TODO: Write to shader pack format

    RVX_CORE_INFO("Compiling shader: {}", sourcePath.string());

    result.success = true;
    result.outputPaths.push_back(outputPath.string());
    return result;
}

// ============================================================================
// AudioImporter
// ============================================================================

ImportResult AudioImporter::Import(const fs::path& sourcePath,
                                    const fs::path& outputPath,
                                    const void* options)
{
    ImportResult result;

    // TODO: Load audio file
    // TODO: Convert to common format
    // TODO: Compress if appropriate

    RVX_CORE_INFO("Importing audio: {}", sourcePath.string());

    result.success = true;
    result.outputPaths.push_back(outputPath.string());
    return result;
}

} // namespace RVX::Tools
