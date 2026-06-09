#include "Resource/Loader/TextureLoader.h"
#include "Resource/ResourceCache.h"
#include "Core/Log.h"

#include <stb_image.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>

namespace RVX::Resource
{
    namespace
    {
        float ByteToUnit(uint8_t value)
        {
            return static_cast<float>(value) / 255.0f;
        }

        uint8_t UnitToByte(float value)
        {
            const float clamped = std::clamp(value, 0.0f, 1.0f);
            return static_cast<uint8_t>(std::round(clamped * 255.0f));
        }

        float SRGBToLinear(uint8_t value)
        {
            const float srgb = ByteToUnit(value);
            if (srgb <= 0.04045f)
            {
                return srgb / 12.92f;
            }

            return std::pow((srgb + 0.055f) / 1.055f, 2.4f);
        }

        uint8_t LinearToSRGBByte(float value)
        {
            const float linear = std::clamp(value, 0.0f, 1.0f);
            const float srgb = linear <= 0.0031308f
                                   ? linear * 12.92f
                                   : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
            return UnitToByte(srgb);
        }

        uint32_t CalculateMipLevelCount(uint32_t width, uint32_t height)
        {
            uint32_t mipLevels = 1;
            while (width > 1 || height > 1)
            {
                width = std::max(1u, width >> 1u);
                height = std::max(1u, height >> 1u);
                ++mipLevels;
            }
            return mipLevels;
        }

        size_t PixelOffset(uint32_t x, uint32_t y, uint32_t width)
        {
            return (static_cast<size_t>(y) * width + x) * 4u;
        }

        std::vector<uint8_t> GenerateNextMip(const std::vector<uint8_t>& source,
                                             uint32_t sourceWidth,
                                             uint32_t sourceHeight,
                                             TextureUsage usage,
                                             bool isSRGB)
        {
            const uint32_t targetWidth = std::max(1u, sourceWidth >> 1u);
            const uint32_t targetHeight = std::max(1u, sourceHeight >> 1u);
            std::vector<uint8_t> target(static_cast<size_t>(targetWidth) * targetHeight * 4u);

            for (uint32_t y = 0; y < targetHeight; ++y)
            {
                const uint32_t sourceY0 = (y * sourceHeight) / targetHeight;
                const uint32_t sourceY1 = std::max(sourceY0 + 1u, ((y + 1u) * sourceHeight + targetHeight - 1u) / targetHeight);
                for (uint32_t x = 0; x < targetWidth; ++x)
                {
                    const uint32_t sourceX0 = (x * sourceWidth) / targetWidth;
                    const uint32_t sourceX1 = std::max(sourceX0 + 1u, ((x + 1u) * sourceWidth + targetWidth - 1u) / targetWidth);

                    float accum[4] = {};
                    uint32_t sampleCount = 0;
                    for (uint32_t sy = sourceY0; sy < sourceY1 && sy < sourceHeight; ++sy)
                    {
                        for (uint32_t sx = sourceX0; sx < sourceX1 && sx < sourceWidth; ++sx)
                        {
                            const size_t src = PixelOffset(sx, sy, sourceWidth);
                            if (usage == TextureUsage::Normal)
                            {
                                accum[0] += ByteToUnit(source[src + 0]) * 2.0f - 1.0f;
                                accum[1] += ByteToUnit(source[src + 1]) * 2.0f - 1.0f;
                                accum[2] += ByteToUnit(source[src + 2]) * 2.0f - 1.0f;
                            }
                            else if (usage == TextureUsage::Color && isSRGB)
                            {
                                accum[0] += SRGBToLinear(source[src + 0]);
                                accum[1] += SRGBToLinear(source[src + 1]);
                                accum[2] += SRGBToLinear(source[src + 2]);
                            }
                            else
                            {
                                accum[0] += ByteToUnit(source[src + 0]);
                                accum[1] += ByteToUnit(source[src + 1]);
                                accum[2] += ByteToUnit(source[src + 2]);
                            }
                            accum[3] += ByteToUnit(source[src + 3]);
                            ++sampleCount;
                        }
                    }

                    const float invSampleCount = sampleCount > 0 ? 1.0f / static_cast<float>(sampleCount) : 0.0f;
                    const size_t dst = PixelOffset(x, y, targetWidth);
                    if (usage == TextureUsage::Normal)
                    {
                        float nx = accum[0] * invSampleCount;
                        float ny = accum[1] * invSampleCount;
                        float nz = accum[2] * invSampleCount;
                        const float lengthSq = nx * nx + ny * ny + nz * nz;
                        if (lengthSq <= 1.0e-8f)
                        {
                            nx = 0.0f;
                            ny = 0.0f;
                            nz = 1.0f;
                        }
                        else
                        {
                            const float invLength = 1.0f / std::sqrt(lengthSq);
                            nx *= invLength;
                            ny *= invLength;
                            nz *= invLength;
                        }
                        target[dst + 0] = UnitToByte(nx * 0.5f + 0.5f);
                        target[dst + 1] = UnitToByte(ny * 0.5f + 0.5f);
                        target[dst + 2] = UnitToByte(nz * 0.5f + 0.5f);
                    }
                    else if (usage == TextureUsage::Color && isSRGB)
                    {
                        target[dst + 0] = LinearToSRGBByte(accum[0] * invSampleCount);
                        target[dst + 1] = LinearToSRGBByte(accum[1] * invSampleCount);
                        target[dst + 2] = LinearToSRGBByte(accum[2] * invSampleCount);
                    }
                    else
                    {
                        target[dst + 0] = UnitToByte(accum[0] * invSampleCount);
                        target[dst + 1] = UnitToByte(accum[1] * invSampleCount);
                        target[dst + 2] = UnitToByte(accum[2] * invSampleCount);
                    }
                    target[dst + 3] = UnitToByte(accum[3] * invSampleCount);
                }
            }

            return target;
        }

        std::vector<uint8_t> BuildMipChain(const std::vector<uint8_t>& basePixels,
                                           uint32_t width,
                                           uint32_t height,
                                           TextureUsage usage,
                                           bool isSRGB,
                                           uint32_t& outMipLevels)
        {
            outMipLevels = CalculateMipLevelCount(width, height);
            if (outMipLevels <= 1)
            {
                return basePixels;
            }

            std::vector<uint8_t> mipChain = basePixels;
            std::vector<uint8_t> previous = basePixels;
            uint32_t previousWidth = width;
            uint32_t previousHeight = height;

            for (uint32_t mipLevel = 1; mipLevel < outMipLevels; ++mipLevel)
            {
                std::vector<uint8_t> next = GenerateNextMip(previous, previousWidth, previousHeight, usage, isSRGB);
                mipChain.insert(mipChain.end(), next.begin(), next.end());
                previous = std::move(next);
                previousWidth = std::max(1u, previousWidth >> 1u);
                previousHeight = std::max(1u, previousHeight >> 1u);
            }

            return mipChain;
        }
    } // namespace

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
        const TextureUsage usage = InferTextureUsageFromPath(absPath.string());
        const bool isSRGB = usage == TextureUsage::Color;
        m_lastLoadStatus = TextureLoadStatus::None;
        m_lastLoadError.clear();
        return LoadFromFileWithPolicy(absPath.string(), usage, isSRGB, absPath.string());
    }

    // =========================================================================
    // Extended Loading API
    // =========================================================================

    TextureResource* TextureLoader::LoadFromReference(const TextureReference& ref,
                                                        const std::string& modelPath)
    {
        m_lastLoadStatus = TextureLoadStatus::None;
        m_lastLoadError.clear();

        if (!ref.IsValid())
        {
            m_lastLoadStatus = TextureLoadStatus::FallbackInvalidReference;
            m_lastLoadError = "Invalid texture reference";
            RVX_CORE_WARN("TextureLoader: Invalid texture reference; using default texture");
            return GetDefaultTexture(ref.usage);
        }

        const std::string sourceKey = ref.GetUniqueKey(modelPath);
        const std::string cacheKey = BuildTexturePolicyCacheKey(sourceKey, ref.usage, ref.isSRGB);
        ResourceId textureId = GenerateTextureId(cacheKey);

        // Check cache first
        if (m_manager && m_manager->IsInitialized())
        {
            if (auto* cached = m_manager->GetCache().Get(textureId))
            {
                m_lastLoadStatus = TextureLoadStatus::Loaded;
                m_lastLoadError.clear();
                return static_cast<TextureResource*>(cached);
            }
        }

        TextureResource* texture = nullptr;

        if (ref.IsExternal())
        {
            texture = LoadFromFileWithPolicy(sourceKey, ref.usage, ref.isSRGB, cacheKey);
        }
        else
        {
            // Embedded texture
            if (ref.isRawPixelData)
            {
                // Already decoded raw RGBA pixels (from tinygltf)
                texture = LoadFromMemoryWithPolicy(ref.embeddedData.data(), ref.embeddedData.size(),
                                                    sourceKey, cacheKey, ref.usage, ref.isSRGB,
                                                    true, ref.rawWidth, ref.rawHeight);
            }
            else
            {
                // Encoded image data (PNG/JPEG) - needs decoding
                texture = LoadFromMemoryWithPolicy(ref.embeddedData.data(), ref.embeddedData.size(),
                                                    sourceKey, cacheKey, ref.usage, ref.isSRGB,
                                                    false, 0, 0);
            }
        }

        if (texture)
        {
            m_lastLoadStatus = TextureLoadStatus::Loaded;
        }
        else
        {
            m_lastLoadStatus = TextureLoadStatus::FallbackLoadFailed;
            m_lastLoadError = "Failed to load texture source; using default texture";
            RVX_CORE_WARN("TextureLoader: Failed to load texture, using default");
            texture = GetDefaultTexture(ref.usage);
        }

        return texture;
    }

    TextureResource* TextureLoader::LoadFromFile(const std::string& absolutePath)
    {
        m_lastLoadStatus = TextureLoadStatus::None;
        m_lastLoadError.clear();

        const TextureUsage usage = InferTextureUsageFromPath(absolutePath);
        const bool isSRGB = usage == TextureUsage::Color;
        const std::string cacheKey = BuildTexturePolicyCacheKey(absolutePath, usage, isSRGB);

        return LoadFromFileWithPolicy(absolutePath, usage, isSRGB, cacheKey);
    }

    TextureResource* TextureLoader::LoadFromFileWithPolicy(const std::string& absolutePath,
                                                            TextureUsage usage,
                                                            bool isSRGB,
                                                            const std::string& cacheKey)
    {
        ResourceId textureId = GenerateTextureId(cacheKey);

        if (m_manager && m_manager->IsInitialized())
        {
            if (auto* cached = m_manager->GetCache().Get(textureId))
            {
                m_lastLoadStatus = TextureLoadStatus::Loaded;
                m_lastLoadError.clear();
                return static_cast<TextureResource*>(cached);
            }
        }

        // Check if file exists
        if (!std::filesystem::exists(absolutePath))
        {
            m_lastLoadStatus = TextureLoadStatus::Failed;
            m_lastLoadError = "Texture file not found: " + absolutePath;
            RVX_CORE_WARN("TextureLoader: File not found: {}", absolutePath);
            return nullptr;
        }

        // Read file
        std::ifstream file(absolutePath, std::ios::binary | std::ios::ate);
        if (!file.is_open())
        {
            m_lastLoadStatus = TextureLoadStatus::Failed;
            m_lastLoadError = "Cannot open texture file: " + absolutePath;
            RVX_CORE_WARN("TextureLoader: Cannot open file: {}", absolutePath);
            return nullptr;
        }

        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);

        std::vector<uint8_t> fileData(static_cast<size_t>(size));
        if (!file.read(reinterpret_cast<char*>(fileData.data()), size))
        {
            m_lastLoadStatus = TextureLoadStatus::Failed;
            m_lastLoadError = "Failed to read texture file: " + absolutePath;
            RVX_CORE_WARN("TextureLoader: Failed to read file: {}", absolutePath);
            return nullptr;
        }

        // Decode image
        std::vector<uint8_t> pixels;
        uint32_t width, height;
        int channels;

        if (!DecodeImage(fileData.data(), fileData.size(), pixels, width, height, channels))
        {
            m_lastLoadStatus = TextureLoadStatus::Failed;
            m_lastLoadError = "Failed to decode texture file: " + absolutePath;
            RVX_CORE_WARN("TextureLoader: Failed to decode image: {}", absolutePath);
            return nullptr;
        }

        TextureResource* texture =
            CreateTextureResource(std::move(pixels), width, height, channels, absolutePath, cacheKey, usage, isSRGB);
        m_lastLoadStatus = texture ? TextureLoadStatus::Loaded : TextureLoadStatus::Failed;
        if (!texture)
        {
            m_lastLoadError = "Failed to create texture resource: " + absolutePath;
        }
        return texture;
    }

    TextureResource* TextureLoader::LoadFromMemory(const void* data, size_t size,
                                                     const std::string& uniqueKey,
                                                     TextureUsage usage,
                                                     bool isRawRGBA,
                                                     uint32_t width, uint32_t height)
    {
        m_lastLoadStatus = TextureLoadStatus::None;
        m_lastLoadError.clear();

        const bool isSRGB = usage == TextureUsage::Color;
        const std::string cacheKey = BuildTexturePolicyCacheKey(uniqueKey, usage, isSRGB);
        return LoadFromMemoryWithPolicy(data, size, uniqueKey, cacheKey, usage, isSRGB, isRawRGBA, width, height);
    }

    TextureResource* TextureLoader::LoadFromMemoryWithPolicy(const void* data, size_t size,
                                                              const std::string& sourceKey,
                                                              const std::string& cacheKey,
                                                              TextureUsage usage,
                                                              bool isSRGB,
                                                              bool isRawRGBA,
                                                              uint32_t width, uint32_t height)
    {
        ResourceId textureId = GenerateTextureId(cacheKey);

        // Check cache
        if (m_manager && m_manager->IsInitialized())
        {
            if (auto* cached = m_manager->GetCache().Get(textureId))
            {
                m_lastLoadStatus = TextureLoadStatus::Loaded;
                m_lastLoadError.clear();
                return static_cast<TextureResource*>(cached);
            }
        }

        TextureResource* texture = nullptr;
        if (isRawRGBA)
        {
            // Raw RGBA data
            std::vector<uint8_t> pixels(static_cast<const uint8_t*>(data),
                                         static_cast<const uint8_t*>(data) + size);
            texture = CreateTextureResource(std::move(pixels), width, height, 4, sourceKey, cacheKey, usage, isSRGB);
        }
        else
        {
            // Encoded image data - decode it
            std::vector<uint8_t> pixels;
            int channels;

            if (!DecodeImage(data, size, pixels, width, height, channels))
            {
                RVX_CORE_WARN("TextureLoader: Failed to decode embedded image: {}", sourceKey);
                m_lastLoadStatus = TextureLoadStatus::Failed;
                m_lastLoadError = "Failed to decode embedded texture: " + sourceKey;
                return nullptr;
            }

            texture = CreateTextureResource(std::move(pixels), width, height, channels, sourceKey, cacheKey, usage, isSRGB);
        }

        m_lastLoadStatus = texture ? TextureLoadStatus::Loaded : TextureLoadStatus::Failed;
        if (!texture)
        {
            m_lastLoadError = "Failed to create texture resource: " + sourceKey;
        }
        return texture;
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
                                                    "__default_white__", "__default_white__",
                                                    TextureUsage::Color, true, false);
            if (m_whiteTexture)
            {
                m_whiteTexture->MarkDefaultFallback("TextureLoader default white fallback");
            }
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
                                                     "__default_normal__", "__default_normal__",
                                                     TextureUsage::Normal, false, false);
            if (m_normalTexture)
            {
                m_normalTexture->SetSRGB(false);
                m_normalTexture->MarkDefaultFallback("TextureLoader default normal fallback");
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
                                                    "__default_error__", "__default_error__",
                                                    TextureUsage::Color, true, false);
            if (m_errorTexture)
            {
                m_errorTexture->MarkDefaultFallback("TextureLoader default error fallback");
            }
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
                                                            const std::string& sourceKey,
                                                            const std::string& cacheKey,
                                                            TextureUsage usage,
                                                            bool isSRGB,
                                                            bool generateMipChain)
    {
        auto* texture = new TextureResource();

        ResourceId textureId = GenerateTextureId(cacheKey);
        texture->SetId(textureId);
        texture->SetPath(sourceKey);

        // Extract name from path
        std::filesystem::path filePath(sourceKey);
        texture->SetName(filePath.stem().string());

        // Set metadata
        TextureMetadata metadata;
        metadata.width = width;
        metadata.height = height;
        metadata.format = (channels == 4) ? TextureFormat::RGBA8 : TextureFormat::RGB8;
        metadata.mipLevels = 1;
        metadata.usage = usage;
        metadata.isSRGB = isSRGB;

        const size_t expectedBaseSize = static_cast<size_t>(width) * height * 4u;
        if (generateMipChain && metadata.format == TextureFormat::RGBA8 && width > 0 && height > 0 &&
            pixels.size() == expectedBaseSize)
        {
            pixels = BuildMipChain(pixels, width, height, usage, isSRGB, metadata.mipLevels);
        }

        texture->SetData(std::move(pixels), metadata);
        texture->NotifyLoaded();

        // Store in cache
        if (m_manager && m_manager->IsInitialized())
        {
            m_manager->GetCache().Store(texture);
        }

        return texture;
    }

    std::string TextureLoader::BuildTexturePolicyCacheKey(const std::string& sourceKey,
                                                          TextureUsage usage,
                                                          bool isSRGB) const
    {
        std::ostringstream stream;
        stream << sourceKey << "|usage=" << static_cast<int>(usage) << "|srgb=" << (isSRGB ? 1 : 0);
        return stream.str();
    }

    TextureUsage TextureLoader::InferTextureUsageFromPath(const std::string& path) const
    {
        TextureUsage usage = TextureUsage::Color;
        std::string lowerPath = path;
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

        return usage;
    }

    ResourceId TextureLoader::GenerateTextureId(const std::string& uniqueKey)
    {
        // Use std::hash for simplicity
        std::hash<std::string> hasher;
        return static_cast<ResourceId>(hasher(uniqueKey));
    }

} // namespace RVX::Resource
