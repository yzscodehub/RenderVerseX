#include "RHI/RHIDefinitions.h"

namespace RVX
{
    // =============================================================================
    // Format Utilities
    // =============================================================================

    uint32 GetFormatBytesPerPixel(RHIFormat format)
    {
        switch (format)
        {
            case RHIFormat::R8_UNORM:
            case RHIFormat::R8_SNORM:
            case RHIFormat::R8_UINT:
            case RHIFormat::R8_SINT:
                return 1;

            case RHIFormat::R16_FLOAT:
            case RHIFormat::R16_UNORM:
            case RHIFormat::R16_UINT:
            case RHIFormat::R16_SINT:
            case RHIFormat::RG8_UNORM:
            case RHIFormat::RG8_SNORM:
            case RHIFormat::RG8_UINT:
            case RHIFormat::RG8_SINT:
            case RHIFormat::D16_UNORM:
                return 2;

            case RHIFormat::R32_FLOAT:
            case RHIFormat::R32_UINT:
            case RHIFormat::R32_SINT:
            case RHIFormat::RG16_FLOAT:
            case RHIFormat::RG16_UNORM:
            case RHIFormat::RG16_UINT:
            case RHIFormat::RG16_SINT:
            case RHIFormat::RGBA8_UNORM:
            case RHIFormat::RGBA8_UNORM_SRGB:
            case RHIFormat::RGBA8_SNORM:
            case RHIFormat::RGBA8_UINT:
            case RHIFormat::RGBA8_SINT:
            case RHIFormat::BGRA8_UNORM:
            case RHIFormat::BGRA8_UNORM_SRGB:
            case RHIFormat::RGB10A2_UNORM:
            case RHIFormat::RGB10A2_UINT:
            case RHIFormat::RG11B10_FLOAT:
            case RHIFormat::D24_UNORM_S8_UINT:
            case RHIFormat::D32_FLOAT:
                return 4;

            case RHIFormat::RG32_FLOAT:
            case RHIFormat::RG32_UINT:
            case RHIFormat::RG32_SINT:
            case RHIFormat::RGBA16_FLOAT:
            case RHIFormat::RGBA16_UNORM:
            case RHIFormat::RGBA16_UINT:
            case RHIFormat::RGBA16_SINT:
            case RHIFormat::D32_FLOAT_S8_UINT:
                return 8;

            case RHIFormat::RGB32_FLOAT:
            case RHIFormat::RGB32_UINT:
            case RHIFormat::RGB32_SINT:
                return 12;

            case RHIFormat::RGBA32_FLOAT:
            case RHIFormat::RGBA32_UINT:
            case RHIFormat::RGBA32_SINT:
                return 16;

            // Compressed formats (bytes per block)
            case RHIFormat::BC1_UNORM:
            case RHIFormat::BC1_UNORM_SRGB:
            case RHIFormat::BC4_UNORM:
            case RHIFormat::BC4_SNORM:
                return 8;  // 8 bytes per 4x4 block

            case RHIFormat::BC2_UNORM:
            case RHIFormat::BC2_UNORM_SRGB:
            case RHIFormat::BC3_UNORM:
            case RHIFormat::BC3_UNORM_SRGB:
            case RHIFormat::BC5_UNORM:
            case RHIFormat::BC5_SNORM:
            case RHIFormat::BC6H_UF16:
            case RHIFormat::BC6H_SF16:
            case RHIFormat::BC7_UNORM:
            case RHIFormat::BC7_UNORM_SRGB:
                return 16;  // 16 bytes per 4x4 block

            default:
                return 0;
        }
    }

    bool IsDepthFormat(RHIFormat format)
    {
        switch (format)
        {
            case RHIFormat::D16_UNORM:
            case RHIFormat::D24_UNORM_S8_UINT:
            case RHIFormat::D32_FLOAT:
            case RHIFormat::D32_FLOAT_S8_UINT:
                return true;
            default:
                return false;
        }
    }

    bool IsStencilFormat(RHIFormat format)
    {
        switch (format)
        {
            case RHIFormat::D24_UNORM_S8_UINT:
            case RHIFormat::D32_FLOAT_S8_UINT:
                return true;
            default:
                return false;
        }
    }

    bool IsCompressedFormat(RHIFormat format)
    {
        switch (format)
        {
            case RHIFormat::BC1_UNORM:
            case RHIFormat::BC1_UNORM_SRGB:
            case RHIFormat::BC2_UNORM:
            case RHIFormat::BC2_UNORM_SRGB:
            case RHIFormat::BC3_UNORM:
            case RHIFormat::BC3_UNORM_SRGB:
            case RHIFormat::BC4_UNORM:
            case RHIFormat::BC4_SNORM:
            case RHIFormat::BC5_UNORM:
            case RHIFormat::BC5_SNORM:
            case RHIFormat::BC6H_UF16:
            case RHIFormat::BC6H_SF16:
            case RHIFormat::BC7_UNORM:
            case RHIFormat::BC7_UNORM_SRGB:
                return true;
            default:
                return false;
        }
    }

    bool IsSRGBFormat(RHIFormat format)
    {
        switch (format)
        {
            case RHIFormat::RGBA8_UNORM_SRGB:
            case RHIFormat::BGRA8_UNORM_SRGB:
            case RHIFormat::BC1_UNORM_SRGB:
            case RHIFormat::BC2_UNORM_SRGB:
            case RHIFormat::BC3_UNORM_SRGB:
            case RHIFormat::BC7_UNORM_SRGB:
                return true;
            default:
                return false;
        }
    }

} // namespace RVX
