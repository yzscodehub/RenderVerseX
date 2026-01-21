#include "Asset/Types/TextureAsset.h"

namespace RVX::Asset
{

TextureAsset::TextureAsset() = default;
TextureAsset::~TextureAsset() = default;

void TextureAsset::SetData(std::vector<uint8_t> data, const TextureMetadata& metadata)
{
    m_data = std::move(data);
    m_metadata = metadata;
}

size_t TextureAsset::GetMemoryUsage() const
{
    return sizeof(*this) + m_data.size();
}

size_t TextureAsset::GetGPUMemoryUsage() const
{
    // Estimate GPU memory based on metadata
    size_t bytesPerPixel = 4; // Assume RGBA8
    
    switch (m_metadata.format)
    {
        case TextureFormat::RGBA8:
        case TextureFormat::RGB8:
            bytesPerPixel = 4;
            break;
        case TextureFormat::RGBA16F:
            bytesPerPixel = 8;
            break;
        case TextureFormat::RGBA32F:
            bytesPerPixel = 16;
            break;
        case TextureFormat::RG8:
            bytesPerPixel = 2;
            break;
        case TextureFormat::R8:
            bytesPerPixel = 1;
            break;
        case TextureFormat::BC1:
            bytesPerPixel = 1; // 0.5 bytes per pixel, but we round up
            break;
        case TextureFormat::BC3:
        case TextureFormat::BC5:
        case TextureFormat::BC7:
            bytesPerPixel = 1;
            break;
        default:
            break;
    }
    
    size_t size = m_metadata.width * m_metadata.height * bytesPerPixel;
    
    // Account for mipmaps
    if (m_metadata.mipLevels > 1)
    {
        size = static_cast<size_t>(size * 1.34f); // Approximate mipmap overhead
    }
    
    return size;
}

} // namespace RVX::Asset
