/**
 * @file AssetPipeline.cpp
 * @brief Asset pipeline implementation
 */

#include "Tools/AssetPipeline.h"
#include "Core/Log.h"
#include "Resource/Importer/GLTFImporter.h"
#include "Resource/Loader/TextureLoader.h"
#include "Resource/Types/TextureResource.h"
#include "ShaderCompiler/ShaderCompiler.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <system_error>
#include <utility>

namespace RVX::Tools
{
namespace
{
    std::string ToLower(std::string value)
    {
        std::transform(value.begin(),
                       value.end(),
                       value.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return value;
    }

    bool ContainsAny(const std::string& value, std::initializer_list<const char*> needles)
    {
        for (const char* needle : needles)
        {
            if (value.find(needle) != std::string::npos)
            {
                return true;
            }
        }
        return false;
    }

    RHIShaderStage InferShaderStageFromPath(const fs::path& sourcePath)
    {
        const std::string fileName = ToLower(sourcePath.filename().string());
        if (ContainsAny(fileName, {".vs.", "_vs.", ".vert.", "_vert.", "vertex"}))
        {
            return RHIShaderStage::Vertex;
        }
        if (ContainsAny(fileName, {".ps.", "_ps.", ".frag.", "_frag.", "pixel"}))
        {
            return RHIShaderStage::Pixel;
        }
        if (ContainsAny(fileName, {".cs.", "_cs.", ".comp.", "_comp.", "compute"}))
        {
            return RHIShaderStage::Compute;
        }

        return RHIShaderStage::None;
    }

    const char* ToShaderStageString(RHIShaderStage stage)
    {
        switch (stage)
        {
            case RHIShaderStage::Vertex:  return "Vertex";
            case RHIShaderStage::Pixel:   return "Pixel";
            case RHIShaderStage::Compute: return "Compute";
            default:                      return "None";
        }
    }

    const char* ToAssetTypeString(AssetType type)
    {
        switch (type)
        {
            case AssetType::Texture:   return "Texture";
            case AssetType::Mesh:      return "Mesh";
            case AssetType::Material:  return "Material";
            case AssetType::Shader:    return "Shader";
            case AssetType::Animation: return "Animation";
            case AssetType::Audio:     return "Audio";
            case AssetType::Font:      return "Font";
            case AssetType::Prefab:    return "Prefab";
            case AssetType::Scene:     return "Scene";
            case AssetType::Script:    return "Script";
            default:                   return "Unknown";
        }
    }

    const std::vector<uint8>& SelectShaderBinaryPayload(const ShaderCompileResult& compileResult)
    {
        return compileResult.bytecode;
    }

    const char* ToTextureFormatString(Resource::TextureFormat format)
    {
        switch (format)
        {
            case Resource::TextureFormat::RGBA8:   return "RGBA8";
            case Resource::TextureFormat::RGBA16F: return "RGBA16F";
            case Resource::TextureFormat::RGBA32F: return "RGBA32F";
            case Resource::TextureFormat::RGB8:    return "RGB8";
            case Resource::TextureFormat::RG8:     return "RG8";
            case Resource::TextureFormat::R8:      return "R8";
            case Resource::TextureFormat::BC1:     return "BC1";
            case Resource::TextureFormat::BC3:     return "BC3";
            case Resource::TextureFormat::BC5:     return "BC5";
            case Resource::TextureFormat::BC7:     return "BC7";
            default:                               return "Unknown";
        }
    }

    const char* ToTextureUsageString(Resource::TextureUsage usage)
    {
        switch (usage)
        {
            case Resource::TextureUsage::Color:  return "Color";
            case Resource::TextureUsage::Normal: return "Normal";
            case Resource::TextureUsage::Data:   return "Data";
            default:                             return "Unknown";
        }
    }

    const char* ToTextureCompressionModeString(TextureCompressionMode mode)
    {
        switch (mode)
        {
            case TextureCompressionMode::Auto: return "Auto";
            case TextureCompressionMode::None: return "None";
            case TextureCompressionMode::BC1:  return "BC1";
            case TextureCompressionMode::BC3:  return "BC3";
            case TextureCompressionMode::BC5:  return "BC5";
            case TextureCompressionMode::BC7:  return "BC7";
            default:                           return "Unknown";
        }
    }

    uint16_t PackRGB565(uint8_t r, uint8_t g, uint8_t b)
    {
        return static_cast<uint16_t>(((r >> 3u) << 11u) |
                                     ((g >> 2u) << 5u) |
                                     (b >> 3u));
    }

    std::array<uint8_t, 3> UnpackRGB565(uint16_t value)
    {
        const uint8_t r5 = static_cast<uint8_t>((value >> 11u) & 0x1Fu);
        const uint8_t g6 = static_cast<uint8_t>((value >> 5u) & 0x3Fu);
        const uint8_t b5 = static_cast<uint8_t>(value & 0x1Fu);
        return {
            static_cast<uint8_t>((r5 << 3u) | (r5 >> 2u)),
            static_cast<uint8_t>((g6 << 2u) | (g6 >> 4u)),
            static_cast<uint8_t>((b5 << 3u) | (b5 >> 2u))
        };
    }

    size_t RgbaMipOffset(uint32_t width, uint32_t height, uint32_t mipLevel)
    {
        size_t offset = 0;
        for (uint32_t mip = 0; mip < mipLevel; ++mip)
        {
            offset += static_cast<size_t>(std::max(1u, width >> mip)) *
                      std::max(1u, height >> mip) *
                      4u;
        }
        return offset;
    }

    size_t BlockCompressedMipChainSize(uint32_t width,
                                       uint32_t height,
                                       uint32_t mipLevels,
                                       size_t bytesPerBlock)
    {
        size_t totalSize = 0;
        for (uint32_t mip = 0; mip < mipLevels; ++mip)
        {
            const uint32_t mipWidth = std::max(1u, width >> mip);
            const uint32_t mipHeight = std::max(1u, height >> mip);
            const uint32_t blocksX = (mipWidth + 3u) / 4u;
            const uint32_t blocksY = (mipHeight + 3u) / 4u;
            totalSize += static_cast<size_t>(blocksX) * blocksY * bytesPerBlock;
        }
        return totalSize;
    }

    bool IsOpaqueRgbaMipChain(const std::vector<uint8_t>& data)
    {
        for (size_t offset = 3; offset < data.size(); offset += 4)
        {
            if (data[offset] != 255u)
            {
                return false;
            }
        }
        return true;
    }

    void AppendLittleEndian16(std::vector<uint8_t>& output, uint16_t value)
    {
        output.push_back(static_cast<uint8_t>(value & 0xFFu));
        output.push_back(static_cast<uint8_t>((value >> 8u) & 0xFFu));
    }

    void AppendLittleEndian32(std::vector<uint8_t>& output, uint32_t value)
    {
        output.push_back(static_cast<uint8_t>(value & 0xFFu));
        output.push_back(static_cast<uint8_t>((value >> 8u) & 0xFFu));
        output.push_back(static_cast<uint8_t>((value >> 16u) & 0xFFu));
        output.push_back(static_cast<uint8_t>((value >> 24u) & 0xFFu));
    }

    void AppendLittleEndian48(std::vector<uint8_t>& output, uint64_t value)
    {
        for (uint32_t byteIndex = 0; byteIndex < 6; ++byteIndex)
        {
            output.push_back(static_cast<uint8_t>((value >> (byteIndex * 8u)) & 0xFFu));
        }
    }

    void PutBC7Bits(std::array<uint8_t, 16>& block,
                    uint32_t& bitOffset,
                    uint64_t value,
                    uint32_t bitCount)
    {
        for (uint32_t bit = 0; bit < bitCount; ++bit)
        {
            if (((value >> bit) & 1ull) != 0ull)
            {
                block[(bitOffset + bit) / 8u] |=
                    static_cast<uint8_t>(1u << ((bitOffset + bit) % 8u));
            }
        }
        bitOffset += bitCount;
    }

    void BuildBC4Palette(uint8_t endpoint0,
                         uint8_t endpoint1,
                         std::array<uint8_t, 8>& palette)
    {
        palette[0] = endpoint0;
        palette[1] = endpoint1;
        if (endpoint0 > endpoint1)
        {
            palette[2] = static_cast<uint8_t>((6u * endpoint0 + endpoint1) / 7u);
            palette[3] = static_cast<uint8_t>((5u * endpoint0 + 2u * endpoint1) / 7u);
            palette[4] = static_cast<uint8_t>((4u * endpoint0 + 3u * endpoint1) / 7u);
            palette[5] = static_cast<uint8_t>((3u * endpoint0 + 4u * endpoint1) / 7u);
            palette[6] = static_cast<uint8_t>((2u * endpoint0 + 5u * endpoint1) / 7u);
            palette[7] = static_cast<uint8_t>((endpoint0 + 6u * endpoint1) / 7u);
        }
        else
        {
            palette[2] = static_cast<uint8_t>((4u * endpoint0 + endpoint1) / 5u);
            palette[3] = static_cast<uint8_t>((3u * endpoint0 + 2u * endpoint1) / 5u);
            palette[4] = static_cast<uint8_t>((2u * endpoint0 + 3u * endpoint1) / 5u);
            palette[5] = static_cast<uint8_t>((endpoint0 + 4u * endpoint1) / 5u);
            palette[6] = 0;
            palette[7] = 255;
        }
    }

    void AppendBC4Block(const std::vector<uint8_t>& rgba,
                        uint32_t width,
                        uint32_t height,
                        size_t mipOffset,
                        uint32_t blockX,
                        uint32_t blockY,
                        uint32_t channel,
                        std::vector<uint8_t>& output)
    {
        std::array<uint8_t, 16> values{};
        uint8_t minValue = 255;
        uint8_t maxValue = 0;

        for (uint32_t y = 0; y < 4; ++y)
        {
            for (uint32_t x = 0; x < 4; ++x)
            {
                const uint32_t srcX = std::min(width - 1u, blockX * 4u + x);
                const uint32_t srcY = std::min(height - 1u, blockY * 4u + y);
                const size_t src = mipOffset + (static_cast<size_t>(srcY) * width + srcX) * 4u;
                const size_t pixelIndex = static_cast<size_t>(y) * 4u + x;
                values[pixelIndex] = rgba[src + channel];
                minValue = std::min(minValue, values[pixelIndex]);
                maxValue = std::max(maxValue, values[pixelIndex]);
            }
        }

        uint8_t endpoint0 = maxValue;
        uint8_t endpoint1 = minValue;
        if (endpoint0 == endpoint1)
        {
            if (endpoint0 < 255u)
            {
                ++endpoint0;
            }
            else
            {
                --endpoint1;
            }
        }

        std::array<uint8_t, 8> palette{};
        BuildBC4Palette(endpoint0, endpoint1, palette);

        uint64_t indices = 0;
        for (size_t pixelIndex = 0; pixelIndex < values.size(); ++pixelIndex)
        {
            uint64_t bestIndex = 0;
            uint32_t bestDistance = std::numeric_limits<uint32_t>::max();
            for (uint64_t paletteIndex = 0; paletteIndex < palette.size(); ++paletteIndex)
            {
                const int32_t delta = static_cast<int32_t>(values[pixelIndex]) -
                                      static_cast<int32_t>(palette[paletteIndex]);
                const uint32_t distance = static_cast<uint32_t>(delta * delta);
                if (distance < bestDistance)
                {
                    bestDistance = distance;
                    bestIndex = paletteIndex;
                }
            }
            indices |= (bestIndex & 0x7u) << (pixelIndex * 3u);
        }

        output.push_back(endpoint0);
        output.push_back(endpoint1);
        AppendLittleEndian48(output, indices);
    }

    void AppendBC1Block(const std::vector<uint8_t>& rgba,
                        uint32_t width,
                        uint32_t height,
                        size_t mipOffset,
                        uint32_t blockX,
                        uint32_t blockY,
                        std::vector<uint8_t>& output)
    {
        std::array<std::array<uint8_t, 3>, 16> pixels{};
        uint8_t minR = 255;
        uint8_t minG = 255;
        uint8_t minB = 255;
        uint8_t maxR = 0;
        uint8_t maxG = 0;
        uint8_t maxB = 0;

        for (uint32_t y = 0; y < 4; ++y)
        {
            for (uint32_t x = 0; x < 4; ++x)
            {
                const uint32_t srcX = std::min(width - 1u, blockX * 4u + x);
                const uint32_t srcY = std::min(height - 1u, blockY * 4u + y);
                const size_t src = mipOffset + (static_cast<size_t>(srcY) * width + srcX) * 4u;
                const size_t pixelIndex = static_cast<size_t>(y) * 4u + x;
                pixels[pixelIndex] = {rgba[src + 0], rgba[src + 1], rgba[src + 2]};
                minR = std::min(minR, rgba[src + 0]);
                minG = std::min(minG, rgba[src + 1]);
                minB = std::min(minB, rgba[src + 2]);
                maxR = std::max(maxR, rgba[src + 0]);
                maxG = std::max(maxG, rgba[src + 1]);
                maxB = std::max(maxB, rgba[src + 2]);
            }
        }

        uint16_t color0 = PackRGB565(maxR, maxG, maxB);
        uint16_t color1 = PackRGB565(minR, minG, minB);
        if (color0 < color1)
        {
            std::swap(color0, color1);
        }
        if (color0 == color1)
        {
            if (color0 < 0xFFFFu)
            {
                ++color0;
            }
            else
            {
                --color1;
            }
        }

        std::array<std::array<uint8_t, 3>, 4> palette{};
        palette[0] = UnpackRGB565(color0);
        palette[1] = UnpackRGB565(color1);
        for (size_t channel = 0; channel < 3; ++channel)
        {
            palette[2][channel] = static_cast<uint8_t>((2u * palette[0][channel] + palette[1][channel]) / 3u);
            palette[3][channel] = static_cast<uint8_t>((palette[0][channel] + 2u * palette[1][channel]) / 3u);
        }

        uint32_t indices = 0;
        for (size_t pixelIndex = 0; pixelIndex < pixels.size(); ++pixelIndex)
        {
            uint32_t bestIndex = 0;
            uint32_t bestDistance = std::numeric_limits<uint32_t>::max();
            for (uint32_t paletteIndex = 0; paletteIndex < palette.size(); ++paletteIndex)
            {
                const int32_t dr = static_cast<int32_t>(pixels[pixelIndex][0]) - palette[paletteIndex][0];
                const int32_t dg = static_cast<int32_t>(pixels[pixelIndex][1]) - palette[paletteIndex][1];
                const int32_t db = static_cast<int32_t>(pixels[pixelIndex][2]) - palette[paletteIndex][2];
                const uint32_t distance = static_cast<uint32_t>(dr * dr + dg * dg + db * db);
                if (distance < bestDistance)
                {
                    bestDistance = distance;
                    bestIndex = paletteIndex;
                }
            }
            indices |= (bestIndex & 0x3u) << (pixelIndex * 2u);
        }

        AppendLittleEndian16(output, color0);
        AppendLittleEndian16(output, color1);
        AppendLittleEndian32(output, indices);
    }

    std::vector<uint8_t> CompressRgbaMipChainToBC1(const std::vector<uint8_t>& rgba,
                                                    uint32_t width,
                                                    uint32_t height,
                                                    uint32_t mipLevels)
    {
        std::vector<uint8_t> compressed;
        compressed.reserve(BlockCompressedMipChainSize(width, height, mipLevels, 8u));

        for (uint32_t mip = 0; mip < mipLevels; ++mip)
        {
            const uint32_t mipWidth = std::max(1u, width >> mip);
            const uint32_t mipHeight = std::max(1u, height >> mip);
            const size_t mipOffset = RgbaMipOffset(width, height, mip);
            const uint32_t blocksX = (mipWidth + 3u) / 4u;
            const uint32_t blocksY = (mipHeight + 3u) / 4u;
            for (uint32_t blockY = 0; blockY < blocksY; ++blockY)
            {
                for (uint32_t blockX = 0; blockX < blocksX; ++blockX)
                {
                    AppendBC1Block(rgba, mipWidth, mipHeight, mipOffset, blockX, blockY, compressed);
                }
            }
        }

        return compressed;
    }

    std::vector<uint8_t> CompressRgbaMipChainToBC3(const std::vector<uint8_t>& rgba,
                                                    uint32_t width,
                                                    uint32_t height,
                                                    uint32_t mipLevels)
    {
        std::vector<uint8_t> compressed;
        compressed.reserve(BlockCompressedMipChainSize(width, height, mipLevels, 16u));

        for (uint32_t mip = 0; mip < mipLevels; ++mip)
        {
            const uint32_t mipWidth = std::max(1u, width >> mip);
            const uint32_t mipHeight = std::max(1u, height >> mip);
            const size_t mipOffset = RgbaMipOffset(width, height, mip);
            const uint32_t blocksX = (mipWidth + 3u) / 4u;
            const uint32_t blocksY = (mipHeight + 3u) / 4u;
            for (uint32_t blockY = 0; blockY < blocksY; ++blockY)
            {
                for (uint32_t blockX = 0; blockX < blocksX; ++blockX)
                {
                    AppendBC4Block(rgba, mipWidth, mipHeight, mipOffset, blockX, blockY, 3, compressed);
                    AppendBC1Block(rgba, mipWidth, mipHeight, mipOffset, blockX, blockY, compressed);
                }
            }
        }

        return compressed;
    }

    std::vector<uint8_t> CompressRgbaMipChainToBC5(const std::vector<uint8_t>& rgba,
                                                    uint32_t width,
                                                    uint32_t height,
                                                    uint32_t mipLevels)
    {
        std::vector<uint8_t> compressed;
        compressed.reserve(BlockCompressedMipChainSize(width, height, mipLevels, 16u));

        for (uint32_t mip = 0; mip < mipLevels; ++mip)
        {
            const uint32_t mipWidth = std::max(1u, width >> mip);
            const uint32_t mipHeight = std::max(1u, height >> mip);
            const size_t mipOffset = RgbaMipOffset(width, height, mip);
            const uint32_t blocksX = (mipWidth + 3u) / 4u;
            const uint32_t blocksY = (mipHeight + 3u) / 4u;
            for (uint32_t blockY = 0; blockY < blocksY; ++blockY)
            {
                for (uint32_t blockX = 0; blockX < blocksX; ++blockX)
                {
                    AppendBC4Block(rgba, mipWidth, mipHeight, mipOffset, blockX, blockY, 0, compressed);
                    AppendBC4Block(rgba, mipWidth, mipHeight, mipOffset, blockX, blockY, 1, compressed);
                }
            }
        }

        return compressed;
    }

    uint8_t QuantizeBC7Endpoint7(uint8_t value)
    {
        return static_cast<uint8_t>(value >> 1u);
    }

    uint8_t ExpandBC7Endpoint7(uint8_t endpoint, uint8_t pBit)
    {
        return static_cast<uint8_t>((endpoint << 1u) | (pBit & 1u));
    }

    uint8_t InterpolateBC7Mode6(uint8_t endpoint0, uint8_t endpoint1, uint32_t index)
    {
        static constexpr std::array<uint8_t, 16> weights = {
            0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64
        };
        const uint32_t weight = weights[index & 0xFu];
        return static_cast<uint8_t>(((64u - weight) * endpoint0 + weight * endpoint1 + 32u) >> 6u);
    }

    void AppendBC7Mode6Block(const std::vector<uint8_t>& rgba,
                             uint32_t width,
                             uint32_t height,
                             size_t mipOffset,
                             uint32_t blockX,
                             uint32_t blockY,
                             std::vector<uint8_t>& output)
    {
        std::array<std::array<uint8_t, 4>, 16> pixels{};
        std::array<uint8_t, 4> minValue = {255, 255, 255, 255};
        std::array<uint8_t, 4> maxValue = {0, 0, 0, 0};

        for (uint32_t y = 0; y < 4; ++y)
        {
            for (uint32_t x = 0; x < 4; ++x)
            {
                const uint32_t srcX = std::min(width - 1u, blockX * 4u + x);
                const uint32_t srcY = std::min(height - 1u, blockY * 4u + y);
                const size_t src = mipOffset + (static_cast<size_t>(srcY) * width + srcX) * 4u;
                const size_t pixelIndex = static_cast<size_t>(y) * 4u + x;
                for (uint32_t channel = 0; channel < 4; ++channel)
                {
                    pixels[pixelIndex][channel] = rgba[src + channel];
                    minValue[channel] = std::min(minValue[channel], pixels[pixelIndex][channel]);
                    maxValue[channel] = std::max(maxValue[channel], pixels[pixelIndex][channel]);
                }
            }
        }

        std::array<uint8_t, 4> endpoint0 = minValue;
        std::array<uint8_t, 4> endpoint1 = maxValue;

        uint32_t firstToMinDistance = 0;
        uint32_t firstToMaxDistance = 0;
        for (uint32_t channel = 0; channel < 4; ++channel)
        {
            const int32_t minDelta = static_cast<int32_t>(pixels[0][channel]) -
                                     static_cast<int32_t>(minValue[channel]);
            const int32_t maxDelta = static_cast<int32_t>(pixels[0][channel]) -
                                     static_cast<int32_t>(maxValue[channel]);
            firstToMinDistance += static_cast<uint32_t>(minDelta * minDelta);
            firstToMaxDistance += static_cast<uint32_t>(maxDelta * maxDelta);
        }
        if (firstToMaxDistance < firstToMinDistance)
        {
            std::swap(endpoint0, endpoint1);
        }

        std::array<uint8_t, 4> endpoint0Quantized{};
        std::array<uint8_t, 4> endpoint1Quantized{};
        for (uint32_t channel = 0; channel < 4; ++channel)
        {
            endpoint0Quantized[channel] = QuantizeBC7Endpoint7(endpoint0[channel]);
            endpoint1Quantized[channel] = QuantizeBC7Endpoint7(endpoint1[channel]);
        }
        const uint8_t pBit0 = endpoint0[0] & 1u;
        const uint8_t pBit1 = endpoint1[0] & 1u;

        std::array<std::array<uint8_t, 4>, 16> palette{};
        const std::array<uint8_t, 4> endpoint0Expanded = {
            ExpandBC7Endpoint7(endpoint0Quantized[0], pBit0),
            ExpandBC7Endpoint7(endpoint0Quantized[1], pBit0),
            ExpandBC7Endpoint7(endpoint0Quantized[2], pBit0),
            ExpandBC7Endpoint7(endpoint0Quantized[3], pBit0)
        };
        const std::array<uint8_t, 4> endpoint1Expanded = {
            ExpandBC7Endpoint7(endpoint1Quantized[0], pBit1),
            ExpandBC7Endpoint7(endpoint1Quantized[1], pBit1),
            ExpandBC7Endpoint7(endpoint1Quantized[2], pBit1),
            ExpandBC7Endpoint7(endpoint1Quantized[3], pBit1)
        };
        for (uint32_t index = 0; index < palette.size(); ++index)
        {
            for (uint32_t channel = 0; channel < 4; ++channel)
            {
                palette[index][channel] =
                    InterpolateBC7Mode6(endpoint0Expanded[channel], endpoint1Expanded[channel], index);
            }
        }

        std::array<uint8_t, 16> indices{};
        for (size_t pixelIndex = 0; pixelIndex < pixels.size(); ++pixelIndex)
        {
            uint8_t bestIndex = 0;
            uint32_t bestDistance = std::numeric_limits<uint32_t>::max();
            const uint32_t maxIndex = pixelIndex == 0 ? 7u : 15u;
            for (uint32_t paletteIndex = 0; paletteIndex <= maxIndex; ++paletteIndex)
            {
                uint32_t distance = 0;
                for (uint32_t channel = 0; channel < 4; ++channel)
                {
                    const int32_t delta = static_cast<int32_t>(pixels[pixelIndex][channel]) -
                                          static_cast<int32_t>(palette[paletteIndex][channel]);
                    distance += static_cast<uint32_t>(delta * delta);
                }
                if (distance < bestDistance)
                {
                    bestDistance = distance;
                    bestIndex = static_cast<uint8_t>(paletteIndex);
                }
            }
            indices[pixelIndex] = bestIndex;
        }

        std::array<uint8_t, 16> block{};
        uint32_t bitOffset = 0;
        PutBC7Bits(block, bitOffset, 1u << 6u, 7u); // Mode 6 unary selector.
        PutBC7Bits(block, bitOffset, endpoint0Quantized[0], 7u);
        PutBC7Bits(block, bitOffset, endpoint1Quantized[0], 7u);
        PutBC7Bits(block, bitOffset, endpoint0Quantized[1], 7u);
        PutBC7Bits(block, bitOffset, endpoint1Quantized[1], 7u);
        PutBC7Bits(block, bitOffset, endpoint0Quantized[2], 7u);
        PutBC7Bits(block, bitOffset, endpoint1Quantized[2], 7u);
        PutBC7Bits(block, bitOffset, endpoint0Quantized[3], 7u);
        PutBC7Bits(block, bitOffset, endpoint1Quantized[3], 7u);
        PutBC7Bits(block, bitOffset, pBit0, 1u);
        PutBC7Bits(block, bitOffset, pBit1, 1u);
        PutBC7Bits(block, bitOffset, indices[0] & 0x7u, 3u);
        for (size_t pixelIndex = 1; pixelIndex < indices.size(); ++pixelIndex)
        {
            PutBC7Bits(block, bitOffset, indices[pixelIndex], 4u);
        }

        output.insert(output.end(), block.begin(), block.end());
    }

    std::vector<uint8_t> CompressRgbaMipChainToBC7(const std::vector<uint8_t>& rgba,
                                                    uint32_t width,
                                                    uint32_t height,
                                                    uint32_t mipLevels)
    {
        std::vector<uint8_t> compressed;
        compressed.reserve(BlockCompressedMipChainSize(width, height, mipLevels, 16u));

        for (uint32_t mip = 0; mip < mipLevels; ++mip)
        {
            const uint32_t mipWidth = std::max(1u, width >> mip);
            const uint32_t mipHeight = std::max(1u, height >> mip);
            const size_t mipOffset = RgbaMipOffset(width, height, mip);
            const uint32_t blocksX = (mipWidth + 3u) / 4u;
            const uint32_t blocksY = (mipHeight + 3u) / 4u;
            for (uint32_t blockY = 0; blockY < blocksY; ++blockY)
            {
                for (uint32_t blockX = 0; blockX < blocksX; ++blockX)
                {
                    AppendBC7Mode6Block(rgba, mipWidth, mipHeight, mipOffset, blockX, blockY, compressed);
                }
            }
        }

        return compressed;
    }

    const char* ToIndexTypeString(IndexType type)
    {
        switch (type)
        {
            case IndexType::UInt8:  return "UInt8";
            case IndexType::UInt16: return "UInt16";
            case IndexType::UInt32: return "UInt32";
            default:                return "Unknown";
        }
    }

    const char* ToPrimitiveTypeString(PrimitiveType primitive)
    {
        switch (primitive)
        {
            case PrimitiveType::Triangles:     return "Triangles";
            case PrimitiveType::TriangleStrip: return "TriangleStrip";
            case PrimitiveType::TriangleFan:   return "TriangleFan";
            case PrimitiveType::Lines:         return "Lines";
            case PrimitiveType::LineStrip:     return "LineStrip";
            case PrimitiveType::LineLoop:      return "LineLoop";
            case PrimitiveType::Points:        return "Points";
            default:                           return "Unknown";
        }
    }

    std::vector<std::pair<std::string, const VertexAttribute*>>
    GetSortedMeshAttributes(const Mesh& mesh)
    {
        std::vector<std::pair<std::string, const VertexAttribute*>> attributes;
        for (const auto& [name, attribute] : mesh.GetAttributes())
        {
            if (attribute)
            {
                attributes.emplace_back(name, attribute.get());
            }
        }

        std::sort(attributes.begin(),
                  attributes.end(),
                  [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
        return attributes;
    }

    struct CookedAttributePayload
    {
        std::string name;
        size_t components = 0;
        AttributeType type = AttributeType::Float;
        bool normalized = false;
        std::vector<uint8_t> data;
    };

    struct CookedMeshLodPayload
    {
        uint32_t level = 0;
        size_t vertexCount = 0;
        std::vector<uint32_t> indices;
        std::vector<CookedAttributePayload> attributes;
    };

    void WriteVec3Metadata(std::ofstream& file, const char* key, const Vec3& value)
    {
        file << key << "=" << value.x << "," << value.y << "," << value.z << "\n";
    }

    std::vector<uint32_t> SelectTriangleSubset(const std::vector<uint32_t>& sourceIndices,
                                               size_t targetTriangleCount)
    {
        const size_t sourceTriangleCount = sourceIndices.size() / 3;
        if (sourceTriangleCount == 0 || targetTriangleCount == 0)
        {
            return {};
        }

        targetTriangleCount = std::min(targetTriangleCount, sourceTriangleCount);

        std::vector<uint32_t> selected;
        selected.reserve(targetTriangleCount * 3);
        for (size_t triangle = 0; triangle < targetTriangleCount; ++triangle)
        {
            const size_t sourceTriangle = std::min(sourceTriangleCount - 1,
                                                   triangle * sourceTriangleCount / targetTriangleCount);
            selected.push_back(sourceIndices[sourceTriangle * 3 + 0]);
            selected.push_back(sourceIndices[sourceTriangle * 3 + 1]);
            selected.push_back(sourceIndices[sourceTriangle * 3 + 2]);
        }
        return selected;
    }

    std::optional<CookedMeshLodPayload> CreateCompactedLodPayload(const Mesh& mesh,
                                                                  uint32_t level,
                                                                  const std::vector<uint32_t>& selectedIndices)
    {
        const size_t sourceVertexCount = mesh.GetVertexCount();
        if (sourceVertexCount == 0 || selectedIndices.size() < 3)
        {
            return std::nullopt;
        }

        constexpr uint32_t invalidIndex = std::numeric_limits<uint32_t>::max();
        std::vector<uint32_t> remap(sourceVertexCount, invalidIndex);
        std::vector<uint32_t> sourceVertexOrder;
        sourceVertexOrder.reserve(sourceVertexCount);

        CookedMeshLodPayload payload;
        payload.level = level;
        payload.indices.reserve(selectedIndices.size());

        for (uint32_t sourceIndex : selectedIndices)
        {
            if (sourceIndex >= sourceVertexCount)
            {
                return std::nullopt;
            }

            uint32_t& compactIndex = remap[sourceIndex];
            if (compactIndex == invalidIndex)
            {
                compactIndex = static_cast<uint32_t>(sourceVertexOrder.size());
                sourceVertexOrder.push_back(sourceIndex);
            }
            payload.indices.push_back(compactIndex);
        }

        payload.vertexCount = sourceVertexOrder.size();
        for (const auto& [attributeName, attribute] : GetSortedMeshAttributes(mesh))
        {
            if (!attribute || attribute->GetVertexCount() < sourceVertexCount)
            {
                return std::nullopt;
            }

            CookedAttributePayload attributePayload;
            attributePayload.name = attributeName;
            attributePayload.components = attribute->GetComponents();
            attributePayload.type = attribute->GetType();
            attributePayload.normalized = attribute->IsNormalized();

            const uint8_t* sourceData = static_cast<const uint8_t*>(attribute->GetData());
            const size_t stride = attribute->GetStride();
            attributePayload.data.reserve(sourceVertexOrder.size() * stride);
            for (uint32_t sourceIndex : sourceVertexOrder)
            {
                const uint8_t* begin = sourceData + sourceIndex * stride;
                attributePayload.data.insert(attributePayload.data.end(), begin, begin + stride);
            }

            payload.attributes.push_back(std::move(attributePayload));
        }

        return payload;
    }

    std::vector<CookedMeshLodPayload> BuildLowerMeshLods(const Mesh& mesh,
                                                         const MeshImportOptions& importOptions,
                                                         std::vector<std::string>& warnings)
    {
        std::vector<CookedMeshLodPayload> lods;
        if (!importOptions.generateLODs || importOptions.lodCount <= 1)
        {
            return lods;
        }

        if (mesh.GetPrimitiveType() != PrimitiveType::Triangles)
        {
            warnings.push_back("Mesh LOD generation skipped for non-triangle primitive mesh: " + mesh.name);
            return lods;
        }

        if (mesh.GetSubMeshes().size() > 1)
        {
            warnings.push_back("Mesh LOD generation skipped for multi-submesh mesh: " + mesh.name);
            return lods;
        }

        const std::vector<uint32_t> sourceIndices = mesh.GetIndices32();
        const size_t sourceTriangleCount = sourceIndices.size() / 3;
        if (sourceTriangleCount < 2)
        {
            warnings.push_back("Mesh LOD generation skipped because the mesh has fewer than two triangles: " +
                               mesh.name);
            return lods;
        }

        const float reduction = std::clamp(importOptions.lodReductionFactor, 0.05f, 0.95f);
        size_t previousTriangleCount = sourceTriangleCount;
        for (int level = 1; level < importOptions.lodCount; ++level)
        {
            const float ratio = std::pow(reduction, static_cast<float>(level));
            size_t targetTriangleCount = static_cast<size_t>(
                std::floor(static_cast<float>(sourceTriangleCount) * ratio));
            targetTriangleCount = std::max<size_t>(1, targetTriangleCount);
            if (targetTriangleCount >= previousTriangleCount)
            {
                targetTriangleCount = previousTriangleCount > 1 ? previousTriangleCount - 1 : 1;
            }

            const std::vector<uint32_t> selectedIndices =
                SelectTriangleSubset(sourceIndices, targetTriangleCount);
            std::optional<CookedMeshLodPayload> payload =
                CreateCompactedLodPayload(mesh, static_cast<uint32_t>(level), selectedIndices);
            if (!payload)
            {
                warnings.push_back("Mesh LOD generation failed while compacting mesh: " + mesh.name);
                break;
            }

            const size_t producedTriangleCount = payload->indices.size() / 3;
            if (producedTriangleCount == 0 || producedTriangleCount >= previousTriangleCount)
            {
                warnings.push_back("Mesh LOD generation stopped because no further reduction was possible: " +
                                   mesh.name);
                break;
            }

            previousTriangleCount = producedTriangleCount;
            lods.push_back(std::move(*payload));
            if (previousTriangleCount == 1)
            {
                break;
            }
        }

        return lods;
    }

    bool WriteMeshArtifact(const fs::path& outputPath,
                           const fs::path& sourcePath,
                           const MeshImportOptions& importOptions,
                           const std::vector<Mesh::Ptr>& meshes,
                           const std::vector<std::vector<CookedMeshLodPayload>>& meshLods,
                           std::string& outError)
    {
        if (meshes.empty())
        {
            outError = "Mesh import produced no meshes";
            return false;
        }
        if (meshLods.size() != meshes.size())
        {
            outError = "Mesh LOD payload count does not match mesh count";
            return false;
        }

        if (!outputPath.parent_path().empty())
        {
            std::error_code ec;
            fs::create_directories(outputPath.parent_path(), ec);
            if (ec)
            {
                outError = "Failed to create mesh artifact directory: " + ec.message();
                return false;
            }
        }

        std::ofstream file(outputPath, std::ios::binary);
        if (!file.is_open())
        {
            outError = "Failed to open mesh artifact for writing: " + outputPath.string();
            return false;
        }

        size_t maxWrittenLodCount = 1;
        for (const std::vector<CookedMeshLodPayload>& lods : meshLods)
        {
            maxWrittenLodCount = std::max(maxWrittenLodCount, lods.size() + 1);
        }

        file << "RVX_MESH_PREBAKE_V1\n";
        file << "source=" << sourcePath.generic_string() << "\n";
        file << "meshCount=" << meshes.size() << "\n";
        file << "requestedTangents=" << (importOptions.generateTangents ? 1 : 0) << "\n";
        file << "requestedOptimization=" << (importOptions.optimizeMesh ? 1 : 0) << "\n";
        file << "requestedLODs=" << (importOptions.generateLODs ? 1 : 0) << "\n";
        file << "requestedLODCount=" << importOptions.lodCount << "\n";
        file << "requestedLODReduction=" << importOptions.lodReductionFactor << "\n";
        file << "writtenLODCount=" << maxWrittenLodCount << "\n";
        file << "scaleFactor=" << importOptions.scaleFactor << "\n";

        for (size_t meshIndex = 0; meshIndex < meshes.size(); ++meshIndex)
        {
            const Mesh::Ptr& mesh = meshes[meshIndex];
            if (!mesh || !mesh->IsValid())
            {
                outError = "Mesh import produced an invalid mesh at index " + std::to_string(meshIndex);
                return false;
            }

            const std::vector<std::pair<std::string, const VertexAttribute*>> attributes =
                GetSortedMeshAttributes(*mesh);
            const std::vector<uint8_t>& indexData = mesh->GetIndexData();
            const std::vector<CookedMeshLodPayload>& lowerLods = meshLods[meshIndex];

            file << "RVX_MESH_" << meshIndex << "_BEGIN\n";
            file << "mesh." << meshIndex << ".name=" << mesh->name << "\n";
            file << "mesh." << meshIndex << ".vertexCount=" << mesh->GetVertexCount() << "\n";
            file << "mesh." << meshIndex << ".indexCount=" << mesh->GetIndexCount() << "\n";
            file << "mesh." << meshIndex << ".indexType=" << ToIndexTypeString(mesh->GetIndexType()) << "\n";
            file << "mesh." << meshIndex << ".primitive="
                 << ToPrimitiveTypeString(mesh->GetPrimitiveType()) << "\n";
            file << "mesh." << meshIndex << ".attributeCount=" << attributes.size() << "\n";
            file << "mesh." << meshIndex << ".subMeshCount=" << mesh->GetSubMeshes().size() << "\n";
            file << "mesh." << meshIndex << ".hasTangents="
                 << (mesh->HasAttribute(VertexBufferNames::Tangent) ? 1 : 0) << "\n";
            file << "mesh." << meshIndex << ".writtenLODCount=" << (lowerLods.size() + 1) << "\n";
            file << "mesh." << meshIndex << ".lod.0.vertexCount=" << mesh->GetVertexCount() << "\n";
            file << "mesh." << meshIndex << ".lod.0.indexCount=" << mesh->GetIndexCount() << "\n";
            file << "mesh." << meshIndex << ".lod.0.attributeCount=" << attributes.size() << "\n";
            file << "mesh." << meshIndex << ".lod.0.source=ImportedMesh\n";

            if (const auto& bounds = mesh->GetBoundingBox(); bounds && bounds->IsValid())
            {
                WriteVec3Metadata(file,
                                  ("mesh." + std::to_string(meshIndex) + ".boundsMin").c_str(),
                                  bounds->GetMin());
                WriteVec3Metadata(file,
                                  ("mesh." + std::to_string(meshIndex) + ".boundsMax").c_str(),
                                  bounds->GetMax());
            }

            for (size_t subMeshIndex = 0; subMeshIndex < mesh->GetSubMeshes().size(); ++subMeshIndex)
            {
                const SubMesh& subMesh = mesh->GetSubMeshes()[subMeshIndex];
                const std::string key = "mesh." + std::to_string(meshIndex) +
                                        ".subMesh." + std::to_string(subMeshIndex);
                file << key << ".indexOffset=" << subMesh.indexOffset << "\n";
                file << key << ".indexCount=" << subMesh.indexCount << "\n";
                file << key << ".baseVertex=" << subMesh.baseVertex << "\n";
                file << key << ".materialId=" << subMesh.materialId << "\n";
                file << key << ".name=" << subMesh.name << "\n";
            }

            for (size_t attributeIndex = 0; attributeIndex < attributes.size(); ++attributeIndex)
            {
                const auto& [attributeName, attribute] = attributes[attributeIndex];
                const std::string key = "mesh." + std::to_string(meshIndex) +
                                        ".attribute." + std::to_string(attributeIndex);
                file << key << ".name=" << attributeName << "\n";
                file << key << ".components=" << attribute->GetComponents() << "\n";
                file << key << ".type=" << AttributeTypeToString(attribute->GetType()) << "\n";
                file << key << ".normalized=" << (attribute->IsNormalized() ? 1 : 0) << "\n";
                file << key << ".byteSize=" << attribute->GetTotalSize() << "\n";
            }

            file << "mesh." << meshIndex << ".indexDataSize=" << indexData.size() << "\n";
            file << "RVX_MESH_" << meshIndex << "_INDEX_DATA_BEGIN\n";
            if (!indexData.empty())
            {
                file.write(reinterpret_cast<const char*>(indexData.data()),
                           static_cast<std::streamsize>(indexData.size()));
            }
            file << "\nRVX_MESH_" << meshIndex << "_INDEX_DATA_END\n";

            for (size_t attributeIndex = 0; attributeIndex < attributes.size(); ++attributeIndex)
            {
                const auto& [attributeName, attribute] = attributes[attributeIndex];
                file << "RVX_MESH_" << meshIndex << "_ATTRIBUTE_" << attributeIndex
                     << "_DATA_BEGIN name=" << attributeName << "\n";
                file.write(static_cast<const char*>(attribute->GetData()),
                           static_cast<std::streamsize>(attribute->GetTotalSize()));
                file << "\nRVX_MESH_" << meshIndex << "_ATTRIBUTE_" << attributeIndex
                     << "_DATA_END\n";
            }

            for (const CookedMeshLodPayload& lod : lowerLods)
            {
                file << "RVX_MESH_" << meshIndex << "_LOD_" << lod.level << "_BEGIN\n";
                file << "mesh." << meshIndex << ".lod." << lod.level
                     << ".vertexCount=" << lod.vertexCount << "\n";
                file << "mesh." << meshIndex << ".lod." << lod.level
                     << ".indexCount=" << lod.indices.size() << "\n";
                file << "mesh." << meshIndex << ".lod." << lod.level
                     << ".triangleCount=" << (lod.indices.size() / 3) << "\n";
                file << "mesh." << meshIndex << ".lod." << lod.level
                     << ".indexType=UInt32\n";
                file << "mesh." << meshIndex << ".lod." << lod.level
                     << ".attributeCount=" << lod.attributes.size() << "\n";

                for (size_t attributeIndex = 0; attributeIndex < lod.attributes.size(); ++attributeIndex)
                {
                    const CookedAttributePayload& attribute = lod.attributes[attributeIndex];
                    const std::string key = "mesh." + std::to_string(meshIndex) +
                                            ".lod." + std::to_string(lod.level) +
                                            ".attribute." + std::to_string(attributeIndex);
                    file << key << ".name=" << attribute.name << "\n";
                    file << key << ".components=" << attribute.components << "\n";
                    file << key << ".type=" << AttributeTypeToString(attribute.type) << "\n";
                    file << key << ".normalized=" << (attribute.normalized ? 1 : 0) << "\n";
                    file << key << ".byteSize=" << attribute.data.size() << "\n";
                }

                file << "RVX_MESH_" << meshIndex << "_LOD_" << lod.level << "_INDEX_DATA_BEGIN\n";
                if (!lod.indices.empty())
                {
                    file.write(reinterpret_cast<const char*>(lod.indices.data()),
                               static_cast<std::streamsize>(lod.indices.size() * sizeof(uint32_t)));
                }
                file << "\nRVX_MESH_" << meshIndex << "_LOD_" << lod.level << "_INDEX_DATA_END\n";

                for (size_t attributeIndex = 0; attributeIndex < lod.attributes.size(); ++attributeIndex)
                {
                    const CookedAttributePayload& attribute = lod.attributes[attributeIndex];
                    file << "RVX_MESH_" << meshIndex << "_LOD_" << lod.level
                         << "_ATTRIBUTE_" << attributeIndex
                         << "_DATA_BEGIN name=" << attribute.name << "\n";
                    if (!attribute.data.empty())
                    {
                        file.write(reinterpret_cast<const char*>(attribute.data.data()),
                                   static_cast<std::streamsize>(attribute.data.size()));
                    }
                    file << "\nRVX_MESH_" << meshIndex << "_LOD_" << lod.level
                         << "_ATTRIBUTE_" << attributeIndex << "_DATA_END\n";
                }

                file << "RVX_MESH_" << meshIndex << "_LOD_" << lod.level << "_END\n";
            }

            file << "RVX_MESH_" << meshIndex << "_END\n";
        }

        file << "RVX_MESH_PREBAKE_END\n";

        if (!file.good())
        {
            outError = "Failed while writing mesh artifact: " + outputPath.string();
            return false;
        }

        return true;
    }

    bool WriteTextureArtifact(const fs::path& outputPath,
                              const fs::path& sourcePath,
                              const TextureImportOptions& importOptions,
                              const Resource::TextureResource& texture,
                              std::vector<std::string>& warnings,
                              std::string& outError)
    {
        if (!outputPath.parent_path().empty())
        {
            std::error_code ec;
            fs::create_directories(outputPath.parent_path(), ec);
            if (ec)
            {
                outError = "Failed to create texture artifact directory: " + ec.message();
                return false;
            }
        }

        Resource::TextureMetadata metadata = texture.GetMetadata();
        const std::vector<uint8_t>& sourceData = texture.GetData();
        if (metadata.width == 0 || metadata.height == 0 || sourceData.empty())
        {
            outError = "Texture import produced empty texture data";
            return false;
        }

        std::vector<uint8_t> cookedData = sourceData;
        std::string compression = "UncompressedRGBA";
        const bool canEncodeBC =
            metadata.format == Resource::TextureFormat::RGBA8 &&
            metadata.depth == 1 &&
            metadata.arrayLayers == 1;
        const bool isOpaque = canEncodeBC ? IsOpaqueRgbaMipChain(sourceData) : false;
        const bool compressionRequested =
            importOptions.compress &&
            importOptions.compressionMode != TextureCompressionMode::None;

        if (compressionRequested)
        {
            TextureCompressionMode selectedMode = importOptions.compressionMode;
            if (selectedMode == TextureCompressionMode::Auto)
            {
                if (canEncodeBC && metadata.usage == Resource::TextureUsage::Normal)
                {
                    selectedMode = TextureCompressionMode::BC5;
                }
                else if (canEncodeBC && !isOpaque)
                {
                    selectedMode = TextureCompressionMode::BC3;
                }
                else if (canEncodeBC)
                {
                    selectedMode = TextureCompressionMode::BC1;
                }
                else
                {
                    selectedMode = TextureCompressionMode::None;
                    warnings.push_back("Texture BC compression supports only RGBA8 2D textures; writing uncompressed RGBA mip data");
                }
            }

            if (selectedMode == TextureCompressionMode::BC5)
            {
                if (!canEncodeBC)
                {
                    outError = "Texture compression mode BC5 requires RGBA8 2D texture data";
                    return false;
                }
                cookedData = CompressRgbaMipChainToBC5(sourceData,
                                                       metadata.width,
                                                       metadata.height,
                                                       metadata.mipLevels);
                metadata.format = Resource::TextureFormat::BC5;
                compression = "BC5";
            }
            else if (selectedMode == TextureCompressionMode::BC7)
            {
                if (!canEncodeBC)
                {
                    outError = "Texture compression mode BC7 requires RGBA8 2D texture data";
                    return false;
                }
                cookedData = CompressRgbaMipChainToBC7(sourceData,
                                                       metadata.width,
                                                       metadata.height,
                                                       metadata.mipLevels);
                metadata.format = Resource::TextureFormat::BC7;
                compression = "BC7";
            }
            else if (selectedMode == TextureCompressionMode::BC3)
            {
                if (!canEncodeBC)
                {
                    outError = "Texture compression mode BC3 requires RGBA8 2D texture data";
                    return false;
                }
                cookedData = CompressRgbaMipChainToBC3(sourceData,
                                                       metadata.width,
                                                       metadata.height,
                                                       metadata.mipLevels);
                metadata.format = Resource::TextureFormat::BC3;
                compression = "BC3";
            }
            else if (selectedMode == TextureCompressionMode::BC1)
            {
                if (!canEncodeBC)
                {
                    outError = "Texture compression mode BC1 requires RGBA8 2D texture data";
                    return false;
                }
                if (!isOpaque)
                {
                    outError = "Texture compression mode BC1 requires opaque RGBA data";
                    return false;
                }
                cookedData = CompressRgbaMipChainToBC1(sourceData,
                                                       metadata.width,
                                                       metadata.height,
                                                       metadata.mipLevels);
                metadata.format = Resource::TextureFormat::BC1;
                compression = "BC1";
            }
        }

        std::ofstream file(outputPath, std::ios::binary);
        if (!file.is_open())
        {
            outError = "Failed to open texture artifact for writing: " + outputPath.string();
            return false;
        }

        file << "RVX_TEXTURE_PREBAKE_V1\n";
        file << "source=" << sourcePath.generic_string() << "\n";
        file << "width=" << metadata.width << "\n";
        file << "height=" << metadata.height << "\n";
        file << "depth=" << metadata.depth << "\n";
        file << "mipLevels=" << metadata.mipLevels << "\n";
        file << "arrayLayers=" << metadata.arrayLayers << "\n";
        file << "format=" << ToTextureFormatString(metadata.format) << "\n";
        file << "usage=" << ToTextureUsageString(metadata.usage) << "\n";
        file << "srgb=" << (metadata.isSRGB ? 1 : 0) << "\n";
        file << "requestedMipmaps=" << (importOptions.generateMipmaps ? 1 : 0) << "\n";
        file << "requestedCompression=" << (compressionRequested ? 1 : 0) << "\n";
        file << "requestedCompressionMode=" << ToTextureCompressionModeString(importOptions.compressionMode) << "\n";
        file << "compression=" << compression << "\n";
        file << "dataSize=" << cookedData.size() << "\n";
        file << "RVX_TEXTURE_DATA_BEGIN\n";
        file.write(reinterpret_cast<const char*>(cookedData.data()),
                   static_cast<std::streamsize>(cookedData.size()));
        file << "\nRVX_TEXTURE_PREBAKE_END\n";

        if (!file.good())
        {
            outError = "Failed while writing texture artifact: " + outputPath.string();
            return false;
        }

        return true;
    }

    bool WriteShaderArtifact(const fs::path& outputPath,
                             const fs::path& sourcePath,
                             const ShaderImportOptions& importOptions,
                             const ShaderCompileResult& compileResult,
                             std::string& outError)
    {
        if (!outputPath.parent_path().empty())
        {
            std::error_code ec;
            fs::create_directories(outputPath.parent_path(), ec);
            if (ec)
            {
                outError = "Failed to create shader artifact directory: " + ec.message();
                return false;
            }
        }

        std::ofstream file(outputPath, std::ios::binary);
        if (!file.is_open())
        {
            outError = "Failed to open shader artifact for writing: " + outputPath.string();
            return false;
        }

        const std::vector<uint8>& binaryPayload = SelectShaderBinaryPayload(compileResult);
        file << "RVX_SHADER_PREBAKE_V1\n";
        file << "source=" << sourcePath.generic_string() << "\n";
        file << "backend=" << ToString(importOptions.targetBackend) << "\n";
        file << "stage=" << ToShaderStageString(importOptions.stage) << "\n";
        file << "entry=" << importOptions.entryPoint << "\n";
        file << "targetProfile="
             << (importOptions.targetProfile.empty() ? "auto" : importOptions.targetProfile)
             << "\n";
        file << "bytecodeSize=" << binaryPayload.size() << "\n";
        file << "glslSize=" << compileResult.glslSource.size() << "\n";
        file << "mslSize=" << compileResult.mslSource.size() << "\n";
        file << "reflectionResources=" << compileResult.reflection.resources.size() << "\n";
        file << "sourceHash=" << compileResult.sourceInfo.combinedHash << "\n";
        file << "RVX_SHADER_BYTECODE_BEGIN\n";
        if (!binaryPayload.empty())
        {
            file.write(reinterpret_cast<const char*>(binaryPayload.data()),
                       static_cast<std::streamsize>(binaryPayload.size()));
        }
        file << "\nRVX_SHADER_GLSL_BEGIN\n";
        if (!compileResult.glslSource.empty())
        {
            file.write(compileResult.glslSource.data(),
                       static_cast<std::streamsize>(compileResult.glslSource.size()));
        }
        file << "\nRVX_SHADER_MSL_BEGIN\n";
        if (!compileResult.mslSource.empty())
        {
            file.write(compileResult.mslSource.data(),
                       static_cast<std::streamsize>(compileResult.mslSource.size()));
        }
        file << "\nRVX_SHADER_PREBAKE_END\n";

        if (!file.good())
        {
            outError = "Failed while writing shader artifact: " + outputPath.string();
            return false;
        }

        return true;
    }

    uint64 GetFileWriteTimeTicks(const fs::path& path)
    {
        std::error_code ec;
        const auto time = fs::last_write_time(path, ec);
        if (ec)
        {
            return 0;
        }
        return static_cast<uint64>(time.time_since_epoch().count());
    }

    uint64 GetFileSizeBytes(const fs::path& path)
    {
        std::error_code ec;
        const uintmax_t size = fs::file_size(path, ec);
        if (ec)
        {
            return 0;
        }
        return static_cast<uint64>(size);
    }

    std::string ToGenericPathString(const fs::path& path)
    {
        return path.generic_string();
    }

    std::string EscapeManifestValue(const std::string& value)
    {
        std::string escaped;
        escaped.reserve(value.size());
        for (char ch : value)
        {
            switch (ch)
            {
                case '\\': escaped += "\\\\"; break;
                case '\n': escaped += "\\n"; break;
                case '\r': escaped += "\\r"; break;
                case '\t': escaped += "\\t"; break;
                default:   escaped += ch; break;
            }
        }
        return escaped;
    }

    std::vector<fs::path> CollectImportableFiles(const AssetPipeline& pipeline,
                                                 const fs::path& sourceDir,
                                                 bool recursive)
    {
        std::vector<fs::path> files;
        if (!fs::exists(sourceDir) || !fs::is_directory(sourceDir))
        {
            return files;
        }

        auto tryAddFile = [&pipeline, &files](const fs::directory_entry& entry)
        {
            if (!entry.is_regular_file())
            {
                return;
            }

            if (pipeline.GetImporter(entry.path().extension().string()))
            {
                files.push_back(entry.path());
            }
        };

        if (recursive)
        {
            for (const auto& entry : fs::recursive_directory_iterator(sourceDir))
            {
                tryAddFile(entry);
            }
        }
        else
        {
            for (const auto& entry : fs::directory_iterator(sourceDir))
            {
                tryAddFile(entry);
            }
        }

        std::sort(files.begin(), files.end(), [](const fs::path& lhs, const fs::path& rhs)
        {
            return lhs.generic_string() < rhs.generic_string();
        });
        return files;
    }

    void EnforceOutputContract(const fs::path& sourcePath,
                               const fs::path& outputPath,
                               ImportResult& result)
    {
        if (!result.success || fs::exists(outputPath))
        {
            return;
        }

        RVX_CORE_ERROR("Asset import reported success without writing output: source='{}', output='{}'",
                       sourcePath.string(),
                       outputPath.string());
        result.success = false;
        result.error = "Asset import reported success without writing output: " + outputPath.string();
        result.outputPaths.clear();
    }
} // namespace

size_t CookManifest::GetSuccessCount() const
{
    return static_cast<size_t>(std::count_if(entries.begin(),
                                            entries.end(),
                                            [](const CookManifestEntry& entry) { return entry.success; }));
}

size_t CookManifest::GetFailureCount() const
{
    return entries.size() - GetSuccessCount();
}

bool CookManifest::Save(const fs::path& manifestPath, std::string& outError) const
{
    if (manifestPath.empty())
    {
        outError = "Cook manifest path is empty";
        return false;
    }

    std::error_code ec;
    if (!manifestPath.parent_path().empty())
    {
        fs::create_directories(manifestPath.parent_path(), ec);
        if (ec)
        {
            outError = "Failed to create cook manifest directory: " + ec.message();
            return false;
        }
    }

    const fs::path tempPath = manifestPath.string() + ".tmp";
    {
        std::ofstream file(tempPath, std::ios::binary);
        if (!file.is_open())
        {
            outError = "Failed to open cook manifest for writing: " + tempPath.string();
            return false;
        }

        file << "RVX_COOK_MANIFEST_V1\n";
        file << "version=" << Version << "\n";
        file << "sourceRoot=" << EscapeManifestValue(sourceRoot) << "\n";
        file << "outputRoot=" << EscapeManifestValue(outputRoot) << "\n";
        file << "recursive=" << (recursive ? 1 : 0) << "\n";
        file << "entryCount=" << entries.size() << "\n";
        file << "successCount=" << GetSuccessCount() << "\n";
        file << "failureCount=" << GetFailureCount() << "\n";

        for (size_t entryIndex = 0; entryIndex < entries.size(); ++entryIndex)
        {
            const CookManifestEntry& entry = entries[entryIndex];
            const std::string prefix = "entry." + std::to_string(entryIndex) + ".";
            file << prefix << "source=" << EscapeManifestValue(entry.sourcePath) << "\n";
            file << prefix << "output=" << EscapeManifestValue(entry.outputPath) << "\n";
            file << prefix << "type=" << ToAssetTypeString(entry.type) << "\n";
            file << prefix << "success=" << (entry.success ? 1 : 0) << "\n";
            file << prefix << "sourceModTime=" << entry.sourceModTime << "\n";
            file << prefix << "outputModTime=" << entry.outputModTime << "\n";
            file << prefix << "outputSize=" << entry.outputSize << "\n";
            file << prefix << "warningCount=" << entry.warnings.size() << "\n";
            for (size_t warningIndex = 0; warningIndex < entry.warnings.size(); ++warningIndex)
            {
                file << prefix << "warning." << warningIndex << "="
                     << EscapeManifestValue(entry.warnings[warningIndex]) << "\n";
            }
            file << prefix << "error=" << EscapeManifestValue(entry.error) << "\n";
        }

        file << "RVX_COOK_MANIFEST_END\n";
        if (!file.good())
        {
            outError = "Failed while writing cook manifest: " + tempPath.string();
            return false;
        }
    }

    fs::remove(manifestPath, ec);
    ec.clear();
    fs::rename(tempPath, manifestPath, ec);
    if (ec)
    {
        fs::remove(tempPath);
        outError = "Failed to replace cook manifest: " + ec.message();
        return false;
    }

    outError.clear();
    return true;
}

void AssetPipeline::RegisterImporter(std::unique_ptr<IAssetImporter> importer)
{
    if (!importer) return;

    for (const auto& ext : importer->GetSupportedExtensions())
    {
        m_importersByExt[ToLower(ext)] = importer.get();
    }
    m_importers.push_back(std::move(importer));
}

IAssetImporter* AssetPipeline::GetImporter(const std::string& extension) const
{
    auto it = m_importersByExt.find(ToLower(extension));
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

    result = importer->Import(sourcePath, outputPath, options);
    EnforceOutputContract(sourcePath, outputPath, result);
    return result;
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

    std::vector<fs::path> filesToImport = CollectImportableFiles(*this, sourceDir, recursive);

    // Import each file
    size_t processed = 0;
    for (const auto& filePath : filesToImport)
    {
        fs::path relativePath = fs::relative(filePath, sourceDir);
        fs::path outPath = outputDir / relativePath;
        outPath.replace_extension(".rva");  // RenderVerseX Asset

        fs::create_directories(outPath.parent_path());

        ImportResult result = ImportAsset(filePath, outPath);
        results.push_back(result);

        if (callback && !filesToImport.empty())
        {
            float progress = static_cast<float>(++processed) / filesToImport.size();
            callback(progress, filePath.filename().string());
        }
    }

    return results;
}

CookManifest AssetPipeline::CookDirectory(const fs::path& sourceDir,
                                           const fs::path& outputDir,
                                           bool recursive,
                                           const fs::path& manifestPath,
                                           ProgressCallback callback,
                                           ImportOptionsProvider optionsProvider)
{
    CookManifest manifest;
    manifest.sourceRoot = ToGenericPathString(fs::absolute(sourceDir));
    manifest.outputRoot = ToGenericPathString(fs::absolute(outputDir));
    manifest.recursive = recursive;

    if (!fs::exists(sourceDir) || !fs::is_directory(sourceDir))
    {
        manifest.manifestError = "Source directory does not exist: " + sourceDir.string();
        return manifest;
    }

    std::vector<fs::path> filesToImport = CollectImportableFiles(*this, sourceDir, recursive);
    size_t processed = 0;
    for (const fs::path& filePath : filesToImport)
    {
        fs::path relativePath = fs::relative(filePath, sourceDir);
        fs::path outPath = outputDir / relativePath;
        outPath.replace_extension(".rva");
        fs::create_directories(outPath.parent_path());

        const AssetType assetType = GetAssetTypeFromExtension(ToLower(filePath.extension().string()));
        const void* importOptions = optionsProvider ? optionsProvider(filePath, assetType) : nullptr;
        ImportResult result = ImportAsset(filePath, outPath, importOptions);

        CookManifestEntry entry;
        entry.sourcePath = ToGenericPathString(relativePath);
        entry.outputPath = ToGenericPathString(fs::relative(outPath, outputDir));
        entry.type = assetType;
        entry.success = result.success;
        entry.error = result.error;
        entry.warnings = std::move(result.warnings);
        entry.sourceModTime = GetFileWriteTimeTicks(filePath);
        if (fs::exists(outPath))
        {
            entry.outputModTime = GetFileWriteTimeTicks(outPath);
            entry.outputSize = GetFileSizeBytes(outPath);
        }

        manifest.entries.push_back(std::move(entry));

        if (callback && !filesToImport.empty())
        {
            float progress = static_cast<float>(++processed) / filesToImport.size();
            callback(progress, filePath.filename().string());
        }
    }

    if (!manifestPath.empty())
    {
        manifest.manifestWritten = manifest.Save(manifestPath, manifest.manifestError);
    }

    return manifest;
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
    const std::string normalizedExt = ToLower(ext);
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
        {".mat", AssetType::Material},
        {".material", AssetType::Material},
        {".rvxmat", AssetType::Material},
        {".anim", AssetType::Animation},
        {".animation", AssetType::Animation},
        {".rvxanim", AssetType::Animation},
        {".wav", AssetType::Audio},
        {".mp3", AssetType::Audio},
        {".ogg", AssetType::Audio},
        {".flac", AssetType::Audio},
        {".ttf", AssetType::Font},
        {".otf", AssetType::Font},
    };

    auto it = extToType.find(normalizedExt);
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
    const TextureImportOptions importOptions = texOptions ? *texOptions : TextureImportOptions{};

    RVX_CORE_INFO("Importing texture: {}", sourcePath.string());

    Resource::TextureLoader loader(nullptr);
    std::unique_ptr<Resource::TextureResource> texture(
        loader.LoadFromFile(fs::absolute(sourcePath).string()));
    if (!texture || texture->IsDefaultFallback())
    {
        result.error = loader.GetLastLoadError().empty()
            ? "Texture import failed"
            : loader.GetLastLoadError();
        return result;
    }

    if (!importOptions.generateMipmaps && texture->GetMipLevels() > 1)
    {
        result.warnings.push_back("TextureLoader generated mipmaps; no-mipmap cook is not implemented yet");
    }

    if (!WriteTextureArtifact(outputPath,
                              sourcePath,
                              importOptions,
                              *texture,
                              result.warnings,
                              result.error))
    {
        return result;
    }

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

    MeshImportOptions importOptions = options
        ? *static_cast<const MeshImportOptions*>(options)
        : MeshImportOptions{};

    const std::string ext = ToLower(sourcePath.extension().string());
    if (ext != ".gltf" && ext != ".glb")
    {
        result.error = "MeshImporter currently supports only glTF/GLB: " + sourcePath.string();
        return result;
    }

    if (importOptions.optimizeMesh)
    {
        result.warnings.push_back("Mesh vertex/index optimization is not implemented; writing imported order");
    }

    RVX_CORE_INFO("Importing mesh: {}", sourcePath.string());

    Resource::GLTFImportOptions gltfOptions;
    gltfOptions.generateNormals = true;
    gltfOptions.generateTangents = importOptions.generateTangents;
    gltfOptions.mergeMeshes = false;
    gltfOptions.scaleFactor = importOptions.scaleFactor;

    Resource::GLTFImporter importer;
    Resource::GLTFImportResult importResult =
        importer.Import(fs::absolute(sourcePath).string(), gltfOptions);
    if (!importResult.success)
    {
        result.error = importResult.errorMessage.empty()
            ? "Mesh import failed"
            : importResult.errorMessage;
        return result;
    }

    for (const std::string& warning : importResult.warnings)
    {
        result.warnings.push_back(warning);
    }

    std::vector<std::vector<CookedMeshLodPayload>> meshLods;
    meshLods.reserve(importResult.meshes.size());

    for (const Mesh::Ptr& mesh : importResult.meshes)
    {
        if (importOptions.generateTangents &&
            mesh &&
            !mesh->HasAttribute(VertexBufferNames::Tangent))
        {
            result.warnings.push_back("Tangent generation requested but no tangent attribute was produced for mesh: " +
                                      mesh->name);
        }

        meshLods.push_back(mesh
            ? BuildLowerMeshLods(*mesh, importOptions, result.warnings)
            : std::vector<CookedMeshLodPayload>{});
    }

    if (!WriteMeshArtifact(outputPath,
                           sourcePath,
                           importOptions,
                           importResult.meshes,
                           meshLods,
                           result.error))
    {
        return result;
    }

    result.success = true;
    result.outputPaths.push_back(outputPath.string());
    return result;
}

// ============================================================================
// ShaderImporter
// ============================================================================

ShaderImporter::ShaderImporter()
    : ShaderImporter([]() { return CreateShaderCompiler(); })
{
}

ShaderImporter::ShaderImporter(CompilerFactory compilerFactory)
    : m_compilerFactory(std::move(compilerFactory))
{
}

ImportResult ShaderImporter::Import(const fs::path& sourcePath,
                                     const fs::path& outputPath,
                                     const void* options)
{
    ImportResult result;

    ShaderImportOptions importOptions = options
        ? *static_cast<const ShaderImportOptions*>(options)
        : ShaderImportOptions{};
    if (importOptions.stage == RHIShaderStage::None)
    {
        importOptions.stage = InferShaderStageFromPath(sourcePath);
    }
    if (importOptions.stage == RHIShaderStage::None)
    {
        result.error = "ShaderImporter could not infer shader stage from filename: " +
                       sourcePath.filename().string();
        return result;
    }

    std::ifstream sourceFile(sourcePath, std::ios::binary);
    if (!sourceFile.is_open())
    {
        result.error = "Failed to open shader source: " + sourcePath.string();
        return result;
    }

    const std::string sourceCode((std::istreambuf_iterator<char>(sourceFile)),
                                 std::istreambuf_iterator<char>());
    if (sourceCode.empty())
    {
        result.error = "Shader source is empty: " + sourcePath.string();
        return result;
    }

    RVX_CORE_INFO("Compiling shader: {}", sourcePath.string());

    if (!m_compilerFactory)
    {
        result.error = "Shader compiler factory is unavailable";
        return result;
    }

    auto compiler = m_compilerFactory();
    if (!compiler)
    {
        result.error = "Shader compiler is unavailable";
        return result;
    }

    const std::string sourcePathString = sourcePath.string();
    ShaderCompileOptions compileOptions;
    compileOptions.stage = importOptions.stage;
    compileOptions.entryPoint = importOptions.entryPoint.c_str();
    compileOptions.sourceCode = sourceCode.c_str();
    compileOptions.sourcePath = sourcePathString.c_str();
    compileOptions.targetProfile = importOptions.targetProfile.empty()
        ? nullptr
        : importOptions.targetProfile.c_str();
    compileOptions.targetBackend = importOptions.targetBackend;
    compileOptions.enableDebugInfo = importOptions.enableDebugInfo;
    compileOptions.enableOptimization = importOptions.enableOptimization;

    const ShaderCompileSupport support =
        compiler->QuerySupport(compileOptions);
    if (!support.IsSupported())
    {
        result.error = support.reason.empty()
            ? "Shader compiler does not support the requested target"
            : support.reason;
        return result;
    }

    ShaderCompileResult compileResult = compiler->Compile(compileOptions);
    if (!compileResult.success)
    {
        result.error = compileResult.errorMessage.empty()
            ? "Shader compilation failed"
            : compileResult.errorMessage;
        return result;
    }

    if (compileResult.bytecode.empty() &&
        compileResult.glslSource.empty() &&
        compileResult.mslSource.empty())
    {
        result.error = "Shader compilation succeeded without bytecode or translated source";
        return result;
    }

    if (!WriteShaderArtifact(outputPath,
                             sourcePath,
                             importOptions,
                             compileResult,
                             result.error))
    {
        return result;
    }

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

    result.success = false;
    result.error = "AudioImporter is not implemented; no audio artifact was written";
    result.warnings.push_back("Audio import disabled until the real audio pipeline is implemented");
    (void)options;
    (void)outputPath;
    return result;
}

} // namespace RVX::Tools
