#pragma once

/**
 * @file TextureLoader.h
 * @brief Texture resource loader
 * 
 * Loads textures from files or embedded data.
 * Supports external texture sharing via absolute path caching.
 */

#include "Resource/ResourceManager.h"
#include "Resource/Types/TextureResource.h"
#include "Resource/Loader/TextureReference.h"
#include <string>
#include <memory>

namespace RVX::Resource
{
    /**
     * @brief Texture resource loader
     * 
     * Features:
     * - Loads textures from common image formats (PNG, JPEG, etc.)
     * - Handles both external files and embedded data
     * - Uses absolute paths for external texture caching (enables cross-model sharing)
     * - Provides default textures for missing resources
     */
    class TextureLoader : public IResourceLoader
    {
    public:
        explicit TextureLoader(ResourceManager* manager);
        ~TextureLoader() override = default;

        // =====================================================================
        // IResourceLoader Interface
        // =====================================================================

        ResourceType GetResourceType() const override { return ResourceType::Texture; }
        std::vector<std::string> GetSupportedExtensions() const override;
        IResource* Load(const std::string& path) override;
        bool CanLoad(const std::string& path) const override;

        // =====================================================================
        // Extended Loading API
        // =====================================================================

        /**
         * @brief Load a texture from a TextureReference
         * 
         * For external textures, uses the absolute path as the cache key,
         * enabling cross-model texture sharing.
         * 
         * @param ref The texture reference (external path or embedded data)
         * @param modelPath Path to the model file (used as base for relative paths)
         * @return TextureResource pointer (ownership handled by ResourceManager)
         */
        TextureResource* LoadFromReference(const TextureReference& ref, 
                                            const std::string& modelPath);

        /**
         * @brief Load a texture from file
         * 
         * @param absolutePath Absolute path to the texture file
         * @return TextureResource pointer
         */
        TextureResource* LoadFromFile(const std::string& absolutePath);

        /**
         * @brief Load a texture from memory
         * 
         * @param data Pointer to image data (encoded PNG/JPEG or raw RGBA)
         * @param size Size of data in bytes
         * @param uniqueKey Unique key for caching/ResourceId
         * @param usage Texture usage hint
         * @param isRawRGBA If true, data is raw RGBA pixels (needs width/height)
         * @param width Width of raw RGBA data (only if isRawRGBA)
         * @param height Height of raw RGBA data (only if isRawRGBA)
         * @return TextureResource pointer
         */
        TextureResource* LoadFromMemory(const void* data, size_t size,
                                         const std::string& uniqueKey,
                                         TextureUsage usage = TextureUsage::Color,
                                         bool isRawRGBA = false,
                                         uint32_t width = 0, uint32_t height = 0);

        // =====================================================================
        // Default Textures
        // =====================================================================

        /**
         * @brief Get a default texture for a given usage
         * 
         * @param usage The texture usage type
         * @return Default texture (white for Color, flat normal for Normal, etc.)
         */
        TextureResource* GetDefaultTexture(TextureUsage usage);

        /**
         * @brief Get the white texture (1x1 white)
         */
        TextureResource* GetWhiteTexture();

        /**
         * @brief Get the normal texture (1x1 flat normal)
         */
        TextureResource* GetNormalTexture();

        /**
         * @brief Get the error texture (magenta checkerboard)
         */
        TextureResource* GetErrorTexture();

    private:
        /// Decode image data using stb_image
        bool DecodeImage(const void* data, size_t size,
                         std::vector<uint8_t>& outPixels,
                         uint32_t& outWidth, uint32_t& outHeight,
                         int& outChannels);

        /// Create texture resource from decoded data
        TextureResource* CreateTextureResource(std::vector<uint8_t> pixels,
                                                uint32_t width, uint32_t height,
                                                int channels,
                                                const std::string& uniqueKey,
                                                TextureUsage usage);

        /// Generate ResourceId from unique key
        ResourceId GenerateTextureId(const std::string& uniqueKey);

        ResourceManager* m_manager;

        // Default textures (lazily created)
        TextureResource* m_whiteTexture = nullptr;
        TextureResource* m_normalTexture = nullptr;
        TextureResource* m_errorTexture = nullptr;
    };

} // namespace RVX::Resource
