#include "Resource/Loader/TextureLoader.h"
#include "Resource/ResourceCache.h"
#include "Core/Log.h"

#include <stb_image.h>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <functional>

namespace RVX::Resource
{
    // =========================================================================
    // Construction
    // =========================================================================

    TextureLoader::TextureLoader(ResourceManager* manager)
        : m_manager(manager)
    {
    }

    // =========================================================================
    // IResourceLoader Interface
    // =========================================================================

    std::vector<std::string> TextureLoader::GetSupportedExtensions() const
    {
        return { ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".gif", ".hdr" };
    }

    bool TextureLoader::CanLoad(const std::string& path) const
    {
        std::filesystem::path filePath(path);
        std::string ext = filePath.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        auto extensions = GetSupportedExtensions();
        return std::find(extensions.begin(), extensions.end(), ext) != extensions.end();
    }

    IResource* TextureLoader::Load(const std::string& path)
    {
        // Resolve to absolute path
        std::filesystem::path absPath = std::filesystem::absolute(path);
        return LoadFromFile(absPath.string());
    }

    // =========================================================================
    // Extended Loading API
    // =========================================================================

    TextureResource* TextureLoader::LoadFromReference(const TextureReference& ref,
                                                        const std::string& modelPath)
    {
        if (!ref.IsValid())
        {
        RVX_CORE_WARN("TextureLoader: Invalid texture reference");
        return GetDefaultTexture(ref.usage);
        }

        // Generate unique key for caching
        std::string uniqueKey = ref.GetUniqueKey(modelPath);
        ResourceId textureId = GenerateTextureId(uniqueKey);

        // Check cache first
        if (m_manager && m_manager->IsInitialized())
        {
            if (auto* cached = m_manager->GetCache().Get(textureId))
            {
                return static_cast<TextureResource*>(cached);
            }
        }

        TextureResource* texture = nullptr;

        if (ref.IsExternal())
        {
            // External texture - load from file
            texture = LoadFromFile(uniqueKey); // uniqueKey is already the absolute path for external
        }
        else
        {
            // Embedded texture
            if (ref.isRawPixelData)
            {
                // Already decoded raw RGBA pixels (from tinygltf)
                texture = LoadFromMemory(ref.embeddedData.data(), ref.embeddedData.size(),
                                          uniqueKey, ref.usage, 
                                          true, ref.rawWidth, ref.rawHeight);
            }
            else
            {
                // Encoded image data (PNG/JPEG) - needs decoding
                texture = LoadFromMemory(ref.embeddedData.data(), ref.embeddedData.size(),
                                          uniqueKey, ref.usage,
                                          false, 0, 0);
            }
        }

        if (texture)
        {
            texture->SetSRGB(ref.isSRGB);
            texture->SetUsage(ref.usage);
        }
        else
        {
            RVX_CORE_WARN("TextureLoader: Failed to load texture, using default");
            texture = GetDefaultTexture(ref.usage);
        }

        return texture;
    }

    TextureResource* TextureLoader::LoadFromFile(const std::string& absolutePath)
    {
        ResourceId textureId = GenerateTextureId(absolutePath);

        // Check cache
        if (m_manager && m_manager->IsInitialized())
        {
            if (auto* cached = m_manager->GetCache().Get(textureId))
            {
                return static_cast<TextureResource*>(cached);
            }
        }

        // Check if file exists
        if (!std::filesystem::exists(absolutePath))
        {
            RVX_CORE_WARN("TextureLoader: File not found: {}", absolutePath);
            return nullptr;
        }

        // Read file
        std::ifstream file(absolutePath, std::ios::binary | std::ios::ate);
        if (!file.is_open())
        {
            RVX_CORE_WARN("TextureLoader: Cannot open file: {}", absolutePath);
            return nullptr;
        }

        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);

        std::vector<uint8_t> fileData(static_cast<size_t>(size));
        if (!file.read(reinterpret_cast<char*>(fileData.data()), size))
        {
            RVX_CORE_WARN("TextureLoader: Failed to read file: {}", absolutePath);
            return nullptr;
        }

        // Decode image
        std::vector<uint8_t> pixels;
        uint32_t width, height;
        int channels;

        if (!DecodeImage(fileData.data(), fileData.size(), pixels, width, height, channels))
        {
            RVX_CORE_WARN("TextureLoader: Failed to decode image: {}", absolutePath);
            return nullptr;
        }

        // Determine usage based on filename hints
        TextureUsage usage = TextureUsage::Color;
        std::string lowerPath = absolutePath;
        std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(), ::tolower);

        if (lowerPath.find("normal") != std::string::npos ||
            lowerPath.find("_n.") != std::string::npos ||
            lowerPath.find("_norm.") != std::string::npos)
        {
            usage = TextureUsage::Normal;
        }
        else if (lowerPath.find("metallic") != std::string::npos ||
                 lowerPath.find("roughness") != std::string::npos ||
                 lowerPath.find("_mr.") != std::string::npos ||
                 lowerPath.find("_orm.") != std::string::npos ||
                 lowerPath.find("ao") != std::string::npos)
        {
            usage = TextureUsage::Data;
        }

        return CreateTextureResource(std::move(pixels), width, height, channels, absolutePath, usage);
    }

    TextureResource* TextureLoader::LoadFromMemory(const void* data, size_t size,
                                                     const std::string& uniqueKey,
                                                     TextureUsage usage,
                                                     bool isRawRGBA,
                                                     uint32_t width, uint32_t height)
    {
        ResourceId textureId = GenerateTextureId(uniqueKey);

        // Check cache
        if (m_manager && m_manager->IsInitialized())
        {
            if (auto* cached = m_manager->GetCache().Get(textureId))
            {
                return static_cast<TextureResource*>(cached);
            }
        }

        if (isRawRGBA)
        {
            // Raw RGBA data
            std::vector<uint8_t> pixels(static_cast<const uint8_t*>(data),
                                         static_cast<const uint8_t*>(data) + size);
            return CreateTextureResource(std::move(pixels), width, height, 4, uniqueKey, usage);
        }
        else
        {
            // Encoded image data - decode it
            std::vector<uint8_t> pixels;
            int channels;

            if (!DecodeImage(data, size, pixels, width, height, channels))
            {
                RVX_CORE_WARN("TextureLoader: Failed to decode embedded image: {}", uniqueKey);
                return nullptr;
            }

            return CreateTextureResource(std::move(pixels), width, height, channels, uniqueKey, usage);
        }
    }

    // =========================================================================
    // Default Textures
    // =========================================================================

    TextureResource* TextureLoader::GetDefaultTexture(TextureUsage usage)
    {
        switch (usage)
        {
            case TextureUsage::Normal:
                return GetNormalTexture();
            case TextureUsage::Color:
            case TextureUsage::Data:
            default:
                return GetWhiteTexture();
        }
    }

    TextureResource* TextureLoader::GetWhiteTexture()
    {
        if (!m_whiteTexture)
        {
            // Create 1x1 white texture
            std::vector<uint8_t> whitePixel = { 255, 255, 255, 255 };
            m_whiteTexture = CreateTextureResource(std::move(whitePixel), 1, 1, 4,
                                                    "__default_white__", TextureUsage::Color);
        }
        return m_whiteTexture;
    }

    TextureResource* TextureLoader::GetNormalTexture()
    {
        if (!m_normalTexture)
        {
            // Create 1x1 flat normal (pointing up in tangent space)
            // Normal: (0, 0, 1) -> encoded as (128, 128, 255)
            std::vector<uint8_t> normalPixel = { 128, 128, 255, 255 };
            m_normalTexture = CreateTextureResource(std::move(normalPixel), 1, 1, 4,
                                                     "__default_normal__", TextureUsage::Normal);
            if (m_normalTexture)
            {
                m_normalTexture->SetSRGB(false);
            }
        }
        return m_normalTexture;
    }

    TextureResource* TextureLoader::GetErrorTexture()
    {
        if (!m_errorTexture)
        {
            // Create 2x2 magenta checkerboard
            std::vector<uint8_t> errorPixels = {
                255, 0, 255, 255,    0, 0, 0, 255,
                0, 0, 0, 255,        255, 0, 255, 255
            };
            m_errorTexture = CreateTextureResource(std::move(errorPixels), 2, 2, 4,
                                                    "__default_error__", TextureUsage::Color);
        }
        return m_errorTexture;
    }

    // =========================================================================
    // Private Helpers
    // =========================================================================

    bool TextureLoader::DecodeImage(const void* data, size_t size,
                                     std::vector<uint8_t>& outPixels,
                                     uint32_t& outWidth, uint32_t& outHeight,
                                     int& outChannels)
    {
        int width, height, channels;
        
        // Force RGBA output for consistency
        stbi_uc* pixels = stbi_load_from_memory(
            static_cast<const stbi_uc*>(data),
            static_cast<int>(size),
            &width, &height, &channels, STBI_rgb_alpha
        );

        if (!pixels)
        {
            RVX_CORE_WARN("TextureLoader: stb_image decode failed: {}", stbi_failure_reason());
            return false;
        }

        outWidth = static_cast<uint32_t>(width);
        outHeight = static_cast<uint32_t>(height);
        outChannels = 4; // We forced RGBA

        size_t pixelCount = static_cast<size_t>(width) * height * 4;
        outPixels.assign(pixels, pixels + pixelCount);

        stbi_image_free(pixels);
        return true;
    }

    TextureResource* TextureLoader::CreateTextureResource(std::vector<uint8_t> pixels,
                                                            uint32_t width, uint32_t height,
                                                            int channels,
                                                            const std::string& uniqueKey,
                                                            TextureUsage usage)
    {
        auto* texture = new TextureResource();
        
        ResourceId textureId = GenerateTextureId(uniqueKey);
        texture->SetId(textureId);
        texture->SetPath(uniqueKey);
        
        // Extract name from path
        std::filesystem::path filePath(uniqueKey);
        texture->SetName(filePath.stem().string());

        // Set metadata
        TextureMetadata metadata;
        metadata.width = width;
        metadata.height = height;
        metadata.format = (channels == 4) ? TextureFormat::RGBA8 : TextureFormat::RGB8;
        metadata.mipLevels = 1;
        metadata.usage = usage;
        metadata.isSRGB = (usage == TextureUsage::Color);

        texture->SetData(std::move(pixels), metadata);
        texture->NotifyLoaded();

        // Store in cache
        if (m_manager && m_manager->IsInitialized())
        {
            m_manager->GetCache().Store(texture);
        }

        return texture;
    }

    ResourceId TextureLoader::GenerateTextureId(const std::string& uniqueKey)
    {
        // Use std::hash for simplicity
        std::hash<std::string> hasher;
        return static_cast<ResourceId>(hasher(uniqueKey));
    }

} // namespace RVX::Resource
