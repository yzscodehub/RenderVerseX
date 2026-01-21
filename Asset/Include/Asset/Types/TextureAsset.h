#pragma once

/**
 * @file TextureAsset.h
 * @brief Texture asset type
 */

#include "Asset/Asset.h"
#include <cstdint>
#include <vector>

namespace RVX::Asset
{
    /**
     * @brief Texture format
     */
    enum class TextureFormat : uint8_t
    {
        Unknown,
        RGBA8,
        RGBA16F,
        RGBA32F,
        RGB8,
        RG8,
        R8,
        BC1,    // DXT1
        BC3,    // DXT5
        BC5,    // ATI2
        BC7
    };

    /**
     * @brief Texture metadata
     */
    struct TextureMetadata
    {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t depth = 1;
        uint32_t mipLevels = 1;
        uint32_t arrayLayers = 1;
        TextureFormat format = TextureFormat::RGBA8;
        bool isCubemap = false;
        bool isArray = false;
    };

    /**
     * @brief Texture asset - encapsulates texture data with GPU resource management
     */
    class TextureAsset : public Asset
    {
    public:
        TextureAsset();
        ~TextureAsset() override;

        // =====================================================================
        // Asset Interface
        // =====================================================================

        AssetType GetType() const override { return AssetType::Texture; }
        const char* GetTypeName() const override { return "Texture"; }
        size_t GetMemoryUsage() const override;
        size_t GetGPUMemoryUsage() const override;

        // =====================================================================
        // Metadata
        // =====================================================================

        const TextureMetadata& GetMetadata() const { return m_metadata; }
        uint32_t GetWidth() const { return m_metadata.width; }
        uint32_t GetHeight() const { return m_metadata.height; }
        uint32_t GetMipLevels() const { return m_metadata.mipLevels; }
        TextureFormat GetFormat() const { return m_metadata.format; }
        bool IsCubemap() const { return m_metadata.isCubemap; }

        // =====================================================================
        // Data Access
        // =====================================================================

        const std::vector<uint8_t>& GetData() const { return m_data; }
        void SetData(std::vector<uint8_t> data, const TextureMetadata& metadata);

        // =====================================================================
        // GPU Resources (future)
        // =====================================================================

        // RHI::TextureHandle GetTexture() const;
        // void UploadToGPU(RHI::Device* device);
        // void ReleaseGPUResources();
        // void ReleaseCPUData();

    private:
        TextureMetadata m_metadata;
        std::vector<uint8_t> m_data;

        // GPU resources (future)
        // RHI::TextureHandle m_texture;
    };

} // namespace RVX::Asset
