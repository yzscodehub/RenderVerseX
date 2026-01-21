#pragma once

/**
 * @file TextureReference.h
 * @brief Texture source reference for model loading
 * 
 * Describes the source of a texture (external file or embedded data).
 * Used by ModelLoader to load textures with proper caching.
 */

#include <string>
#include <vector>
#include <cstdint>
#include <filesystem>

namespace RVX::Resource
{
    /**
     * @brief Type of texture source
     */
    enum class TextureSourceType : uint8_t
    {
        External,   ///< External file reference (path)
        Embedded    ///< Embedded data (e.g., in GLB file or base64)
    };

    /**
     * @brief Texture usage hint for proper format selection
     */
    enum class TextureUsage : uint8_t
    {
        Color,      ///< sRGB color texture (albedo, emissive)
        Normal,     ///< Linear normal map
        Data        ///< Linear data texture (metallic-roughness, AO, etc.)
    };

    /**
     * @brief Reference to a texture source
     * 
     * This structure describes where texture data comes from, either as an
     * external file path or embedded binary data. It provides a unique key
     * generation method for proper caching:
     * 
     * - External textures: Use absolute path as key (enables cross-model sharing)
     * - Embedded textures: Use modelPath#texture_index as key (model-specific)
     */
    struct TextureReference
    {
        /// Type of texture source
        TextureSourceType sourceType = TextureSourceType::External;

        /// For External: relative or absolute path to the texture file
        /// For Embedded: internal identifier (optional)
        std::string path;

        /// Embedded texture data (only used when sourceType == Embedded)
        std::vector<uint8_t> embeddedData;

        /// MIME type of the texture data (e.g., "image/png", "image/jpeg")
        std::string mimeType;

        /// glTF image index (used for embedded texture identification)
        int imageIndex = -1;

        /// Whether embeddedData is already decoded (raw RGBA pixels) or encoded (PNG/JPEG)
        bool isRawPixelData = false;

        /// Width of raw pixel data (only valid when isRawPixelData == true)
        uint32_t rawWidth = 0;

        /// Height of raw pixel data (only valid when isRawPixelData == true)
        uint32_t rawHeight = 0;

        /// Texture usage hint for proper format selection
        TextureUsage usage = TextureUsage::Color;

        /// Whether the texture should be treated as sRGB
        bool isSRGB = true;

        // =====================================================================
        // Key Generation
        // =====================================================================

        /**
         * @brief Generate a unique key for caching and ResourceId generation
         * 
         * @param modelPath Path to the model file (used as base for relative paths)
         * @return Unique key string:
         *         - External: Absolute path (enables cross-model sharing)
         *         - Embedded: modelPath#texture_<index> (model-specific)
         */
        std::string GetUniqueKey(const std::string& modelPath) const
        {
            if (sourceType == TextureSourceType::External)
            {
                // External texture: resolve to absolute path for cross-model sharing
                return ResolveAbsolutePath(modelPath, path);
            }
            else
            {
                // Embedded texture: model-specific key
                return modelPath + "#texture_" + std::to_string(imageIndex);
            }
        }

        /**
         * @brief Check if this reference has valid data
         */
        bool IsValid() const
        {
            if (sourceType == TextureSourceType::External)
            {
                return !path.empty();
            }
            else
            {
                return !embeddedData.empty() && imageIndex >= 0;
            }
        }

        /**
         * @brief Check if this is an embedded texture
         */
        bool IsEmbedded() const
        {
            return sourceType == TextureSourceType::Embedded;
        }

        /**
         * @brief Check if this is an external texture
         */
        bool IsExternal() const
        {
            return sourceType == TextureSourceType::External;
        }

        // =====================================================================
        // Static Helpers
        // =====================================================================

        /**
         * @brief Resolve a relative path to an absolute path
         * 
         * @param basePath Base path (typically the model file path)
         * @param relativePath Relative path to resolve
         * @return Absolute path string
         */
        static std::string ResolveAbsolutePath(const std::string& basePath, 
                                                const std::string& relativePath)
        {
            // If already absolute, return as-is
            std::filesystem::path relPath(relativePath);
            if (relPath.is_absolute())
            {
                return std::filesystem::canonical(relPath).string();
            }

            // Get the directory containing the base file
            std::filesystem::path baseDir = std::filesystem::path(basePath).parent_path();
            
            // Combine and normalize
            std::filesystem::path fullPath = baseDir / relPath;
            
            // Try to make canonical (requires file to exist)
            std::error_code ec;
            auto canonical = std::filesystem::canonical(fullPath, ec);
            if (!ec)
            {
                return canonical.string();
            }
            
            // If file doesn't exist yet, just normalize the path
            return std::filesystem::weakly_canonical(fullPath).string();
        }

        /**
         * @brief Create an external texture reference
         */
        static TextureReference CreateExternal(const std::string& path,
                                                TextureUsage usage = TextureUsage::Color,
                                                bool isSRGB = true)
        {
            TextureReference ref;
            ref.sourceType = TextureSourceType::External;
            ref.path = path;
            ref.usage = usage;
            ref.isSRGB = isSRGB;
            return ref;
        }

        /**
         * @brief Create an embedded texture reference (encoded data like PNG/JPEG)
         */
        static TextureReference CreateEmbedded(std::vector<uint8_t> data,
                                                int imageIndex,
                                                const std::string& mimeType = "image/png",
                                                TextureUsage usage = TextureUsage::Color,
                                                bool isSRGB = true)
        {
            TextureReference ref;
            ref.sourceType = TextureSourceType::Embedded;
            ref.embeddedData = std::move(data);
            ref.imageIndex = imageIndex;
            ref.mimeType = mimeType;
            ref.usage = usage;
            ref.isSRGB = isSRGB;
            ref.isRawPixelData = false;
            return ref;
        }

        /**
         * @brief Create an embedded texture reference (already decoded raw RGBA pixels)
         */
        static TextureReference CreateEmbeddedRaw(std::vector<uint8_t> rawPixels,
                                                   int imageIndex,
                                                   uint32_t width,
                                                   uint32_t height,
                                                   TextureUsage usage = TextureUsage::Color,
                                                   bool isSRGB = true)
        {
            TextureReference ref;
            ref.sourceType = TextureSourceType::Embedded;
            ref.embeddedData = std::move(rawPixels);
            ref.imageIndex = imageIndex;
            ref.usage = usage;
            ref.isSRGB = isSRGB;
            ref.isRawPixelData = true;
            ref.rawWidth = width;
            ref.rawHeight = height;
            return ref;
        }
    };

} // namespace RVX::Resource
