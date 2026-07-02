#include "Resource/Types/TextureResource.h"

#include <utility>

namespace RVX::Resource
{
namespace
{
    RenderTextureUploadFormat ToRenderTextureUploadFormat(TextureFormat format)
    {
        switch (format)
        {
            case TextureFormat::RGBA8:
                return RenderTextureUploadFormat::RGBA8;
            case TextureFormat::RGBA16F:
                return RenderTextureUploadFormat::RGBA16F;
            case TextureFormat::RGBA32F:
                return RenderTextureUploadFormat::RGBA32F;
            case TextureFormat::RGB8:
                return RenderTextureUploadFormat::RGB8;
            case TextureFormat::RG8:
                return RenderTextureUploadFormat::RG8;
            case TextureFormat::R8:
                return RenderTextureUploadFormat::R8;
            case TextureFormat::BC1:
                return RenderTextureUploadFormat::BC1;
            case TextureFormat::BC3:
                return RenderTextureUploadFormat::BC3;
            case TextureFormat::BC5:
                return RenderTextureUploadFormat::BC5;
            case TextureFormat::BC7:
                return RenderTextureUploadFormat::BC7;
            case TextureFormat::Unknown:
            default:
                return RenderTextureUploadFormat::Unknown;
        }
    }
} // namespace

TextureResource::TextureResource() = default;
TextureResource::~TextureResource() = default;

void TextureResource::SetData(std::vector<uint8_t> data, const TextureMetadata& metadata)
{
    m_data = std::move(data);
    m_metadata = metadata;
}

void TextureResource::MarkDefaultFallback(std::string reason)
{
    m_isDefaultFallback = true;
    m_fallbackReason = std::move(reason);
}

RenderTextureUploadData TextureResource::GetRenderTextureUploadData() const
{
    RenderTextureUploadData uploadData;
    uploadData.metadata.width = m_metadata.width;
    uploadData.metadata.height = m_metadata.height;
    uploadData.metadata.depth = m_metadata.depth;
    uploadData.metadata.mipLevels = m_metadata.mipLevels;
    uploadData.metadata.arrayLayers = m_metadata.arrayLayers;
    uploadData.metadata.format = ToRenderTextureUploadFormat(m_metadata.format);
    uploadData.metadata.isCubemap = m_metadata.isCubemap;
    uploadData.metadata.isArray = m_metadata.isArray;
    uploadData.metadata.isSRGB = m_metadata.isSRGB;
    uploadData.data = m_data.data();
    uploadData.dataSize = m_data.size();
    return uploadData;
}

size_t TextureResource::GetMemoryUsage() const
{
    return sizeof(*this) + m_data.size();
}

size_t TextureResource::GetGPUMemoryUsage() const
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

} // namespace RVX::Resource
