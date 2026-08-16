/**
 * @file AssetPipeline.cpp
 * @brief Asset pipeline implementation
 */

#include "Tools/AssetPipeline.h"
#include "Core/Diagnostics/ContentHash.h"
#include "Core/Log.h"
#include "Resource/Cooked/CookedAnimationArtifact.h"
#include "Resource/Cooked/CookedModelArtifact.h"
#include "Resource/Importer/GLTFImporter.h"
#include "Resource/Loader/TextureLoader.h"
#include "Resource/Types/TextureResource.h"
#include "ShaderCompiler/ShaderCompiler.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <exception>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <locale>
#include <mutex>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_set>
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
            case AssetType::Model:     return "Model";
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

    class Sha256Accumulator
    {
    public:
        Sha256Accumulator()
            : m_state{
                0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u }
        {
        }

        void Update(const uint8* data, size_t byteCount)
        {
            m_bitCount += static_cast<uint64>(byteCount) * 8ull;
            while (byteCount > 0)
            {
                const size_t copyCount = std::min(byteCount, m_buffer.size() - m_bufferSize);
                std::copy_n(data, copyCount, m_buffer.data() + m_bufferSize);
                m_bufferSize += copyCount;
                data += copyCount;
                byteCount -= copyCount;
                if (m_bufferSize == m_buffer.size())
                {
                    Transform(m_buffer.data());
                    m_bufferSize = 0;
                }
            }
        }

        std::string Finalize()
        {
            const uint64 messageBitCount = m_bitCount;
            const uint8 paddingStart = 0x80u;
            Update(&paddingStart, 1);

            const uint8 zero = 0;
            while (m_bufferSize != 56u)
            {
                Update(&zero, 1);
            }

            std::array<uint8, 8> lengthBytes{};
            for (uint32 byteIndex = 0; byteIndex < lengthBytes.size(); ++byteIndex)
            {
                lengthBytes[byteIndex] = static_cast<uint8>(
                    (messageBitCount >> ((lengthBytes.size() - 1u - byteIndex) * 8u)) & 0xFFu);
            }
            Update(lengthBytes.data(), lengthBytes.size());

            constexpr char Hex[] = "0123456789abcdef";
            std::string result;
            result.reserve(64);
            for (const uint32 value : m_state)
            {
                for (int32 byteIndex = 3; byteIndex >= 0; --byteIndex)
                {
                    const uint8 byte = static_cast<uint8>((value >> (byteIndex * 8)) & 0xFFu);
                    result.push_back(Hex[byte >> 4u]);
                    result.push_back(Hex[byte & 0x0Fu]);
                }
            }
            return result;
        }

    private:
        static uint32 RotateRight(uint32 value, uint32 amount)
        {
            return (value >> amount) | (value << (32u - amount));
        }

        void Transform(const uint8* block)
        {
            static constexpr std::array<uint32, 64> Constants = {
                0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
                0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
                0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
                0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
                0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
                0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
                0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
                0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
                0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
                0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
                0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
                0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
                0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
                0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
                0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
                0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u };

            std::array<uint32, 64> words{};
            for (uint32 wordIndex = 0; wordIndex < 16u; ++wordIndex)
            {
                const uint32 byteIndex = wordIndex * 4u;
                words[wordIndex] = (static_cast<uint32>(block[byteIndex]) << 24u) |
                                   (static_cast<uint32>(block[byteIndex + 1u]) << 16u) |
                                   (static_cast<uint32>(block[byteIndex + 2u]) << 8u) |
                                   static_cast<uint32>(block[byteIndex + 3u]);
            }
            for (uint32 wordIndex = 16u; wordIndex < words.size(); ++wordIndex)
            {
                const uint32 sigma0 = RotateRight(words[wordIndex - 15u], 7u) ^
                                       RotateRight(words[wordIndex - 15u], 18u) ^
                                       (words[wordIndex - 15u] >> 3u);
                const uint32 sigma1 = RotateRight(words[wordIndex - 2u], 17u) ^
                                       RotateRight(words[wordIndex - 2u], 19u) ^
                                       (words[wordIndex - 2u] >> 10u);
                words[wordIndex] = words[wordIndex - 16u] + sigma0 +
                                   words[wordIndex - 7u] + sigma1;
            }

            uint32 a = m_state[0];
            uint32 b = m_state[1];
            uint32 c = m_state[2];
            uint32 d = m_state[3];
            uint32 e = m_state[4];
            uint32 f = m_state[5];
            uint32 g = m_state[6];
            uint32 h = m_state[7];
            for (uint32 round = 0; round < words.size(); ++round)
            {
                const uint32 sum1 = RotateRight(e, 6u) ^ RotateRight(e, 11u) ^ RotateRight(e, 25u);
                const uint32 choose = (e & f) ^ ((~e) & g);
                const uint32 temporary1 = h + sum1 + choose + Constants[round] + words[round];
                const uint32 sum0 = RotateRight(a, 2u) ^ RotateRight(a, 13u) ^ RotateRight(a, 22u);
                const uint32 majority = (a & b) ^ (a & c) ^ (b & c);
                const uint32 temporary2 = sum0 + majority;

                h = g;
                g = f;
                f = e;
                e = d + temporary1;
                d = c;
                c = b;
                b = a;
                a = temporary1 + temporary2;
            }

            m_state[0] += a;
            m_state[1] += b;
            m_state[2] += c;
            m_state[3] += d;
            m_state[4] += e;
            m_state[5] += f;
            m_state[6] += g;
            m_state[7] += h;
        }

        std::array<uint32, 8> m_state;
        std::array<uint8, 64> m_buffer{};
        size_t m_bufferSize = 0;
        uint64 m_bitCount = 0;
    };

    bool ComputeFileSha256(const fs::path& path,
                           std::string& outHash,
                           uint64& outByteCount,
                           std::string& outError)
    {
        std::error_code fileError;
        if (!fs::is_regular_file(path, fileError) || fileError)
        {
            outError = "Path is not a readable regular file: " + path.string();
            return false;
        }

        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            outError = "Failed to open file for SHA-256: " + path.string();
            return false;
        }

        Sha256Accumulator hasher;
        uint64 readByteCount = 0;
        std::array<char, 8192> buffer{};
        while (file)
        {
            file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize readCount = file.gcount();
            if (readCount > 0)
            {
                const uint64 bytesRead = static_cast<uint64>(readCount);
                if (bytesRead > std::numeric_limits<uint64>::max() - readByteCount)
                {
                    outError = "File byte count exceeds cook manifest range: " + path.string();
                    return false;
                }
                readByteCount += bytesRead;
                hasher.Update(reinterpret_cast<const uint8*>(buffer.data()),
                              static_cast<size_t>(readCount));
            }
        }

        if (file.bad())
        {
            outError = "Failed while reading file for SHA-256: " + path.string();
            return false;
        }

        outHash = hasher.Finalize();
        outByteCount = readByteCount;
        outError.clear();
        return true;
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

    bool ResolveDirectoryRoot(const fs::path& path, fs::path& outRoot, std::string& outError)
    {
        std::error_code pathError;
        const fs::path absolutePath = fs::absolute(path, pathError);
        if (pathError)
        {
            outError = "Failed to make directory path absolute: " + pathError.message();
            return false;
        }

        outRoot = fs::weakly_canonical(absolutePath, pathError);
        if (pathError || !fs::is_directory(outRoot, pathError) || pathError)
        {
            outError = "Path is not an accessible directory root: " + path.string();
            return false;
        }

        outError.clear();
        return true;
    }

    bool PathComponentsEqual(const fs::path& lhs, const fs::path& rhs)
    {
#if defined(_WIN32)
        return ToLower(lhs.generic_string()) == ToLower(rhs.generic_string());
#else
        return lhs == rhs;
#endif
    }

    bool IsPathContainedByRoot(const fs::path& path, const fs::path& root)
    {
        auto pathIt = path.begin();
        auto rootIt = root.begin();
        for (; rootIt != root.end(); ++rootIt, ++pathIt)
        {
            if (pathIt == path.end() || !PathComponentsEqual(*pathIt, *rootIt))
            {
                return false;
            }
        }
        return true;
    }

    bool IsCanonicalRelativePath(const std::string& relativePath)
    {
        if (relativePath.empty())
        {
            return false;
        }

        const fs::path path(relativePath);
        if (path.is_absolute() || path.has_root_name() || path.generic_string() != relativePath)
        {
            return false;
        }

        for (const fs::path& component : path)
        {
            if (component.empty() || component == "." || component == "..")
            {
                return false;
            }
        }
        return true;
    }

    bool CaptureContentIdentity(const fs::path& root,
                                const fs::path& path,
                                CookContentIdentity& outIdentity,
                                std::string& outError)
    {
        std::error_code pathError;
        const fs::path absolutePath = fs::absolute(path, pathError);
        if (pathError)
        {
            outError = "Failed to make content path absolute: " + pathError.message();
            return false;
        }

        const fs::path canonicalPath = fs::weakly_canonical(absolutePath, pathError);
        if (pathError)
        {
            outError = "Failed to canonicalize content path: " + path.string() + ": " +
                       pathError.message();
            return false;
        }
        if (!IsPathContainedByRoot(canonicalPath, root))
        {
            outError = "Content path escapes its declared root: " + canonicalPath.string();
            return false;
        }
        if (!fs::is_regular_file(canonicalPath, pathError) || pathError)
        {
            outError = "Content path is not a regular file: " + canonicalPath.string();
            return false;
        }

        const fs::path relativePath = fs::relative(canonicalPath, root, pathError).lexically_normal();
        if (pathError || !IsCanonicalRelativePath(relativePath.generic_string()))
        {
            outError = "Failed to form canonical relative content path: " + canonicalPath.string();
            return false;
        }

        const uintmax_t byteCount = fs::file_size(canonicalPath, pathError);
        if (pathError)
        {
            outError = "Failed to read content byte count: " + canonicalPath.string() + ": " +
                       pathError.message();
            return false;
        }
        if (byteCount > static_cast<uintmax_t>(std::numeric_limits<uint64>::max()))
        {
            outError = "Content byte count exceeds cook manifest range: " + canonicalPath.string();
            return false;
        }

        CookContentIdentity identity;
        identity.relativePath = relativePath.generic_string();
        uint64 readByteCount = 0;
        if (!ComputeFileSha256(canonicalPath, identity.sha256, readByteCount, outError))
        {
            return false;
        }
        if (readByteCount != static_cast<uint64>(byteCount))
        {
            outError = "Content changed while its identity was being read: " + canonicalPath.string();
            return false;
        }
        identity.byteCount = readByteCount;

        outIdentity = std::move(identity);
        outError.clear();
        return true;
    }

    bool IsSha256(const std::string& value)
    {
        if (value.size() != 64u)
        {
            return false;
        }

        return std::all_of(value.begin(), value.end(), [](char character)
        {
            return (character >= '0' && character <= '9') ||
                   (character >= 'a' && character <= 'f');
        });
    }

    bool ContentIdentitiesEqual(const CookContentIdentity& lhs,
                                const CookContentIdentity& rhs)
    {
        return lhs.relativePath == rhs.relativePath &&
               lhs.byteCount == rhs.byteCount &&
               lhs.sha256 == rhs.sha256;
    }

    bool AddCapturedContentIdentity(std::vector<CookContentIdentity>& identities,
                                    const fs::path& root,
                                    const fs::path& path,
                                    std::string& outError)
    {
        CookContentIdentity identity;
        if (!CaptureContentIdentity(root, path, identity, outError))
        {
            return false;
        }

        const auto existing = std::find_if(identities.begin(), identities.end(),
                                           [&identity](const CookContentIdentity& current)
        {
            return current.relativePath == identity.relativePath;
        });
        if (existing == identities.end())
        {
            identities.push_back(std::move(identity));
            return true;
        }
        if (!ContentIdentitiesEqual(*existing, identity))
        {
            outError = "A cook path resolved to conflicting content identities: " +
                       identity.relativePath;
            return false;
        }
        return true;
    }

    void SortContentIdentities(std::vector<CookContentIdentity>& identities)
    {
        std::sort(identities.begin(), identities.end(),
                  [](const CookContentIdentity& lhs, const CookContentIdentity& rhs)
        {
            return lhs.relativePath < rhs.relativePath;
        });
    }

    bool SourceClosuresEqual(const CookContentIdentity& lhsRoot,
                             const std::vector<CookContentIdentity>& lhsDependencies,
                             const CookContentIdentity& rhsRoot,
                             const std::vector<CookContentIdentity>& rhsDependencies)
    {
        if (!ContentIdentitiesEqual(lhsRoot, rhsRoot) ||
            lhsDependencies.size() != rhsDependencies.size())
        {
            return false;
        }
        for (size_t index = 0; index < lhsDependencies.size(); ++index)
        {
            if (!ContentIdentitiesEqual(lhsDependencies[index], rhsDependencies[index]))
            {
                return false;
            }
        }
        return true;
    }

    bool IsDataUri(const std::string_view uri)
    {
        return uri.size() >= 5u &&
               ToLower(std::string(uri.substr(0, 5u))) == "data:";
    }

    bool HasUriScheme(const std::string_view uri)
    {
        const size_t separator = uri.find(':');
        if (separator == std::string_view::npos || separator == 0u)
        {
            return false;
        }
        for (size_t index = 0; index < separator; ++index)
        {
            const unsigned char character = static_cast<unsigned char>(uri[index]);
            if (!(std::isalpha(character) ||
                  (index > 0u && (std::isdigit(character) || character == '+' ||
                                  character == '-' || character == '.'))))
            {
                return false;
            }
        }
        return true;
    }

    bool DecodeUriPath(const std::string_view uri,
                       std::string& outPath,
                       std::string& outError)
    {
        const auto decodeHex = [](const char character) -> int
        {
            if (character >= '0' && character <= '9') return character - '0';
            if (character >= 'a' && character <= 'f') return character - 'a' + 10;
            if (character >= 'A' && character <= 'F') return character - 'A' + 10;
            return -1;
        };

        outPath.clear();
        outPath.reserve(uri.size());
        for (size_t index = 0; index < uri.size(); ++index)
        {
            const char character = uri[index];
            if (character != '%')
            {
                outPath += character;
                continue;
            }
            if (index + 2u >= uri.size())
            {
                outError = "glTF external URI contains a truncated percent escape.";
                return false;
            }
            const int high = decodeHex(uri[index + 1u]);
            const int low = decodeHex(uri[index + 2u]);
            if (high < 0 || low < 0)
            {
                outError = "glTF external URI contains an invalid percent escape.";
                return false;
            }
            outPath += static_cast<char>((high << 4) | low);
            index += 2u;
        }
        if (outPath.find('\0') != std::string::npos)
        {
            outError = "glTF external URI contains a NUL byte.";
            return false;
        }
        return true;
    }

    bool ResolveGltfExternalUri(const fs::path& canonicalSourceDirectory,
                                const std::string_view uri,
                                fs::path& outPath,
                                std::string& outError)
    {
        if (uri.empty())
        {
            outError = "glTF external URI is empty.";
            return false;
        }
        if (IsDataUri(uri))
        {
            outPath.clear();
            return true;
        }
        if (uri.starts_with("//") || uri.starts_with("\\\\") || HasUriScheme(uri) ||
            uri.find('?') != std::string_view::npos || uri.find('#') != std::string_view::npos ||
            uri.find('\\') != std::string_view::npos)
        {
            outError = "glTF external URI must be a contained offline relative path: " +
                       std::string(uri);
            return false;
        }

        std::string decodedUri;
        if (!DecodeUriPath(uri, decodedUri, outError))
        {
            return false;
        }
        const fs::path relativePath(decodedUri);
        if (relativePath.empty() || relativePath.is_absolute() || relativePath.has_root_name() ||
            relativePath.generic_string() != decodedUri)
        {
            outError = "glTF external URI must normalize to a relative POSIX path: " +
                       std::string(uri);
            return false;
        }
        for (const fs::path& component : relativePath)
        {
            if (component.empty() || component == "." || component == "..")
            {
                outError = "glTF external URI contains a prohibited path component: " +
                           std::string(uri);
                return false;
            }
        }

        std::error_code pathError;
        const fs::path candidate = canonicalSourceDirectory / relativePath;
        const fs::path canonicalPath = fs::weakly_canonical(candidate, pathError);
        if (pathError || !IsPathContainedByRoot(canonicalPath, canonicalSourceDirectory))
        {
            outError = "glTF external URI escapes the source document directory: " +
                       std::string(uri);
            return false;
        }
        outPath = canonicalPath;
        return true;
    }

    bool CollectGltfSourceDependencyClosure(
        const fs::path& canonicalSourceRoot,
        const fs::path& sourcePath,
        CookContentIdentity& outSourceContent,
        std::vector<CookContentIdentity>& outSourceDependencies,
        std::string& outError)
    {
        outSourceContent = {};
        outSourceDependencies.clear();

        std::error_code pathError;
        const fs::path absoluteSourcePath = fs::absolute(sourcePath, pathError);
        const fs::path canonicalSourcePath = pathError
            ? fs::path{}
            : fs::weakly_canonical(absoluteSourcePath, pathError);
        if (pathError || canonicalSourcePath.empty() ||
            !IsPathContainedByRoot(canonicalSourcePath, canonicalSourceRoot))
        {
            outError = "glTF source path is not contained by the cook source root: " +
                       sourcePath.string();
            return false;
        }

        CookContentIdentity initialRootIdentity;
        if (!CaptureContentIdentity(canonicalSourceRoot,
                                    canonicalSourcePath,
                                    initialRootIdentity,
                                    outError))
        {
            return false;
        }

        std::ifstream input(canonicalSourcePath, std::ios::binary);
        if (!input.is_open())
        {
            outError = "Failed to open glTF source closure root: " + canonicalSourcePath.string();
            return false;
        }
        const std::string jsonText((std::istreambuf_iterator<char>(input)),
                                   std::istreambuf_iterator<char>());
        if (!input.good() && !input.eof())
        {
            outError = "Failed while reading glTF source closure root: " + canonicalSourcePath.string();
            return false;
        }

        const nlohmann::json document = nlohmann::json::parse(jsonText, nullptr, false, true);
        if (document.is_discarded() || !document.is_object())
        {
            outError = "glTF source closure root is not a valid JSON object: " +
                       canonicalSourcePath.string();
            return false;
        }

        const fs::path canonicalSourceDirectory = canonicalSourcePath.parent_path();
        const auto collectUris = [&](const char* collectionName) -> bool
        {
            const auto found = document.find(collectionName);
            if (found == document.end())
            {
                return true;
            }
            if (!found->is_array())
            {
                outError = std::string("glTF ") + collectionName + " member must be an array.";
                return false;
            }
            for (const nlohmann::json& record : *found)
            {
                if (!record.is_object())
                {
                    outError = std::string("glTF ") + collectionName +
                               " entries must be objects.";
                    return false;
                }
                const auto uri = record.find("uri");
                if (uri == record.end())
                {
                    continue;
                }
                if (!uri->is_string())
                {
                    outError = std::string("glTF ") + collectionName +
                               " URI must be a string.";
                    return false;
                }
                const std::string uriValue = uri->get<std::string>();
                fs::path externalPath;
                if (!ResolveGltfExternalUri(canonicalSourceDirectory,
                                            uriValue,
                                            externalPath,
                                            outError))
                {
                    return false;
                }
                if (externalPath.empty())
                {
                    // Data URIs are bytes embedded in the already-hashed root document.
                    continue;
                }

                CookContentIdentity dependency;
                if (!CaptureContentIdentity(canonicalSourceRoot,
                                            externalPath,
                                            dependency,
                                            outError))
                {
                    outError = "Failed to capture glTF external " +
                               std::string(collectionName) + " dependency: " + outError;
                    return false;
                }
                if (dependency.relativePath == initialRootIdentity.relativePath)
                {
                    outError = "glTF external URI resolves to its root document.";
                    return false;
                }
                const auto existing = std::find_if(outSourceDependencies.begin(),
                                                   outSourceDependencies.end(),
                                                   [&dependency](const CookContentIdentity& current)
                {
                    return current.relativePath == dependency.relativePath;
                });
                if (existing == outSourceDependencies.end())
                {
                    outSourceDependencies.push_back(std::move(dependency));
                }
                else if (!ContentIdentitiesEqual(*existing, dependency))
                {
                    outError = "glTF source closure path resolved to inconsistent bytes: " +
                               dependency.relativePath;
                    return false;
                }
            }
            return true;
        };

        if (!collectUris("buffers") || !collectUris("images"))
        {
            return false;
        }

        CookContentIdentity finalRootIdentity;
        if (!CaptureContentIdentity(canonicalSourceRoot,
                                    canonicalSourcePath,
                                    finalRootIdentity,
                                    outError))
        {
            return false;
        }
        if (!ContentIdentitiesEqual(initialRootIdentity, finalRootIdentity))
        {
            outError = "glTF source root changed while its dependency closure was captured.";
            return false;
        }
        SortContentIdentities(outSourceDependencies);
        outSourceContent = std::move(finalRootIdentity);
        outError.clear();
        return true;
    }

    std::string ComputeStringSha256(const std::string& value)
    {
        Sha256Accumulator hasher;
        hasher.Update(reinterpret_cast<const uint8*>(value.data()), value.size());
        return hasher.Finalize();
    }

    void AppendCanonicalBool(std::string& settings, const char* key, bool value)
    {
        settings += key;
        settings += '=';
        settings += value ? '1' : '0';
        settings += '\n';
    }

    void AppendCanonicalInteger(std::string& settings, const char* key, int value)
    {
        settings += key;
        settings += '=';
        settings += std::to_string(value);
        settings += '\n';
    }

    void AppendCanonicalFloat(std::string& settings, const char* key, float value)
    {
        std::ostringstream stream;
        stream.imbue(std::locale::classic());
        stream << std::setprecision(std::numeric_limits<float>::max_digits10) << value;
        settings += key;
        settings += '=';
        settings += stream.str();
        settings += '\n';
    }

    void AppendCanonicalMeshSettings(std::string& settings,
                                     const std::string& prefix,
                                     const MeshImportOptions& options)
    {
        AppendCanonicalBool(settings, (prefix + "generateTangents").c_str(), options.generateTangents);
        AppendCanonicalBool(settings, (prefix + "optimizeMesh").c_str(), options.optimizeMesh);
        AppendCanonicalBool(settings, (prefix + "generateLODs").c_str(), options.generateLODs);
        AppendCanonicalInteger(settings, (prefix + "lodCount").c_str(), options.lodCount);
        AppendCanonicalFloat(settings, (prefix + "lodReductionFactor").c_str(), options.lodReductionFactor);
        AppendCanonicalFloat(settings, (prefix + "scaleFactor").c_str(), options.scaleFactor);
        AppendCanonicalBool(settings, (prefix + "importAnimations").c_str(), options.importAnimations);
        AppendCanonicalBool(settings, (prefix + "importMaterials").c_str(), options.importMaterials);
    }

    std::string BuildCanonicalCookSettings(AssetType type, const void* options)
    {
        std::string settings = "settingsSchema=RVX_COOK_SETTINGS_V1\n";
        settings += "assetType=";
        settings += ToAssetTypeString(type);
        settings += '\n';

        switch (type)
        {
            case AssetType::Texture:
            {
                const TextureImportOptions resolved = options
                    ? *static_cast<const TextureImportOptions*>(options)
                    : TextureImportOptions{};
                AppendCanonicalBool(settings, "texture.generateMipmaps", resolved.generateMipmaps);
                AppendCanonicalBool(settings, "texture.sRGB", resolved.sRGB);
                AppendCanonicalBool(settings, "texture.compress", resolved.compress);
                settings += "texture.compressionMode=";
                settings += ToTextureCompressionModeString(resolved.compressionMode);
                settings += '\n';
                AppendCanonicalInteger(settings, "texture.maxSize", resolved.maxSize);
                AppendCanonicalBool(settings, "texture.flipY", resolved.flipY);
                break;
            }
            case AssetType::Mesh:
            {
                const MeshImportOptions resolved = options
                    ? *static_cast<const MeshImportOptions*>(options)
                    : MeshImportOptions{};
                AppendCanonicalMeshSettings(settings, "mesh.", resolved);
                break;
            }
            case AssetType::Model:
            {
                const ModelImportOptions resolved = options
                    ? *static_cast<const ModelImportOptions*>(options)
                    : ModelImportOptions{};
                AppendCanonicalMeshSettings(settings, "model.mesh.", resolved.mesh);
                break;
            }
            case AssetType::Shader:
            {
                const ShaderImportOptions resolved = options
                    ? *static_cast<const ShaderImportOptions*>(options)
                    : ShaderImportOptions{};
                settings += "shader.stage=";
                settings += ToShaderStageString(resolved.stage);
                settings += '\n';
                settings += "shader.targetBackend=" +
                            std::to_string(static_cast<uint32>(resolved.targetBackend)) + '\n';
                settings += "shader.entryPoint=" + resolved.entryPoint + '\n';
                settings += "shader.targetProfile=" + resolved.targetProfile + '\n';
                AppendCanonicalBool(settings, "shader.enableDebugInfo", resolved.enableDebugInfo);
                AppendCanonicalBool(settings, "shader.enableOptimization", resolved.enableOptimization);
                break;
            }
            default:
                settings += "options=none\n";
                break;
        }

        return settings;
    }

    std::string BuildRecipeHash(const CookManifestEntry& entry)
    {
        std::string recipe = "recipeSchema=RVX_COOK_RECIPE_V1\n";
        recipe += "toolName=";
        recipe += CookManifest::ToolName;
        recipe += '\n';
        recipe += "toolVersion=";
        recipe += CookManifest::ToolVersion;
        recipe += '\n';
        recipe += "importer=" + entry.importerName + '\n';
        recipe += "assetType=";
        recipe += ToAssetTypeString(entry.type);
        recipe += '\n';
        recipe += "output.path=" + entry.outputPath + '\n';
        recipe += "source.path=" + entry.sourceContent.relativePath + '\n';
        recipe += "source.byteCount=" + std::to_string(entry.sourceContent.byteCount) + '\n';
        recipe += "source.sha256=" + entry.sourceContent.sha256 + '\n';
        if (entry.sourceDependencyClosureRecorded)
        {
            recipe += "sourceDependencyCount=" +
                      std::to_string(entry.sourceDependencies.size()) + '\n';
            for (const CookContentIdentity& dependency : entry.sourceDependencies)
            {
                recipe += "sourceDependency.path=" + dependency.relativePath + '\n';
                recipe += "sourceDependency.byteCount=" +
                          std::to_string(dependency.byteCount) + '\n';
                recipe += "sourceDependency.sha256=" + dependency.sha256 + '\n';
            }
        }
        recipe += "cookSettingsHash=" + entry.cookSettingsHash + '\n';
        recipe += "dependencyCount=" + std::to_string(entry.dependencies.size()) + '\n';
        for (const CookContentIdentity& dependency : entry.dependencies)
        {
            recipe += "dependency.path=" + dependency.relativePath + '\n';
            recipe += "dependency.byteCount=" + std::to_string(dependency.byteCount) + '\n';
            recipe += "dependency.sha256=" + dependency.sha256 + '\n';
        }
        return ComputeStringSha256(recipe);
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

    bool IsStrictlySortedByRelativePath(const std::vector<CookContentIdentity>& identities)
    {
        return std::adjacent_find(identities.begin(), identities.end(),
                                  [](const CookContentIdentity& lhs,
                                     const CookContentIdentity& rhs)
        {
            return lhs.relativePath >= rhs.relativePath;
        }) == identities.end();
    }

    bool ValidateCapturedContentIdentity(const CookContentIdentity& expected,
                                         const fs::path& root,
                                         const char* label,
                                         std::string& outError)
    {
        if (!IsCanonicalRelativePath(expected.relativePath) || !IsSha256(expected.sha256))
        {
            outError = std::string("Cook manifest has an invalid ") + label + " identity";
            return false;
        }

        CookContentIdentity actual;
        if (!CaptureContentIdentity(root, root / expected.relativePath, actual, outError))
        {
            outError = std::string("Failed to validate cook ") + label + ": " + outError;
            return false;
        }
        if (!ContentIdentitiesEqual(expected, actual))
        {
            outError = std::string("Cook ") + label + " no longer matches its recorded identity: " +
                       expected.relativePath;
            return false;
        }
        return true;
    }

    bool ValidateSuccessfulCookManifestEntry(const CookManifestEntry& entry,
                                             const fs::path& sourceRoot,
                                             const fs::path& outputRoot,
                                             std::string& outError)
    {
        if (entry.importerName.empty() || entry.canonicalCookSettings.empty() ||
            !entry.sourceDependencyClosureRecorded ||
            !IsCanonicalRelativePath(entry.sourcePath) ||
            !IsCanonicalRelativePath(entry.outputPath) ||
            entry.sourcePath != entry.sourceContent.relativePath)
        {
            outError = "Cook manifest success entry is missing required v2 identity fields";
            return false;
        }
        if (!IsStrictlySortedByRelativePath(entry.sourceDependencies) ||
            !IsStrictlySortedByRelativePath(entry.dependencies) ||
            !IsStrictlySortedByRelativePath(entry.artifacts) || entry.artifacts.empty())
        {
            outError = "Cook manifest source dependency, cooked dependency, or artifact paths are not canonical and sorted";
            return false;
        }
        if (entry.cookSettingsHash != ComputeStringSha256(entry.canonicalCookSettings))
        {
            outError = "Cook manifest settings hash does not match canonical settings";
            return false;
        }
        if (entry.recipeHash != BuildRecipeHash(entry))
        {
            outError = "Cook manifest recipe hash does not match entry inputs";
            return false;
        }
        if (!ValidateCapturedContentIdentity(entry.sourceContent, sourceRoot, "source", outError))
        {
            return false;
        }
        if (!entry.sourceDependencyClosureRecorded)
        {
            if (!entry.sourceDependencies.empty())
            {
                outError = "Cook manifest has source dependencies without a source closure marker";
                return false;
            }
        }
        else
        {
            for (const CookContentIdentity& dependency : entry.sourceDependencies)
            {
                if (!ValidateCapturedContentIdentity(dependency,
                                                     sourceRoot,
                                                     "source dependency",
                                                     outError))
                {
                    return false;
                }
            }
        }

        bool hasPrimaryArtifact = false;
        for (const CookContentIdentity& artifact : entry.artifacts)
        {
            if (!ValidateCapturedContentIdentity(artifact, outputRoot, "artifact", outError))
            {
                return false;
            }
            hasPrimaryArtifact = hasPrimaryArtifact || artifact.relativePath == entry.outputPath;
        }
        if (!hasPrimaryArtifact)
        {
            outError = "Cook manifest success entry does not record its primary output artifact";
            return false;
        }

        std::vector<std::string> dependencyPaths;
        dependencyPaths.reserve(entry.dependencies.size());
        for (const CookContentIdentity& dependency : entry.dependencies)
        {
            if (!ValidateCapturedContentIdentity(dependency, outputRoot, "dependency", outError))
            {
                return false;
            }
            dependencyPaths.push_back(dependency.relativePath);
        }
        if (entry.dependencyOutputs != dependencyPaths)
        {
            outError = "Cook manifest legacy dependency paths do not match v2 dependency identities";
            return false;
        }

        outError.clear();
        return true;
    }

    struct CookPublicationPaths
    {
        fs::path finalOutputRoot;
        fs::path outputParent;
        fs::path stagingOutputRoot;
        fs::path manifestPath;
        fs::path stagingManifestPath;
        bool publishManifest = false;
        bool manifestInsideOutput = false;
    };

    std::atomic<uint64> s_cookTransactionSequence{0};

    fs::path MakeUniqueTransactionSibling(const fs::path& parent,
                                          const std::string& stem,
                                          const char* role)
    {
        const uint64 sequence = ++s_cookTransactionSequence;
        const uint64 ticks = static_cast<uint64>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        return parent / ("." + stem + ".rvx-cook-" + role + "-" +
                         std::to_string(ticks) + "-" + std::to_string(sequence));
    }

    bool PathExists(const fs::path& path, bool& outExists, std::string& outError)
    {
        std::error_code ec;
        outExists = fs::exists(path, ec);
        if (ec)
        {
            outError = "Failed to query transaction path '" + path.string() + "': " +
                       ec.message();
            return false;
        }
        outError.clear();
        return true;
    }

    bool RemoveTransactionPath(const fs::path& path, std::string& outError)
    {
        bool exists = false;
        if (!PathExists(path, exists, outError) || !exists)
        {
            return outError.empty();
        }

        std::error_code ec;
        fs::remove_all(path, ec);
        if (ec)
        {
            outError = "Failed to remove cook transaction path '" + path.string() + "': " +
                       ec.message();
            return false;
        }
        outError.clear();
        return true;
    }

    bool RenameTransactionPath(const fs::path& from,
                               const fs::path& to,
                               const char* operation,
                               std::string& outError)
    {
        std::error_code ec;
        fs::rename(from, to, ec);
        if (ec)
        {
            outError = std::string(operation) + " ('" + from.string() + "' -> '" +
                       to.string() + "') failed: " + ec.message();
            return false;
        }
        outError.clear();
        return true;
    }

    bool ResolveFutureOutputRoot(const fs::path& outputDir,
                                 fs::path& outOutputRoot,
                                 fs::path& outOutputParent,
                                 std::string& outError)
    {
        std::error_code ec;
        const fs::path absoluteOutput = fs::absolute(outputDir, ec).lexically_normal();
        if (ec || absoluteOutput.empty() || absoluteOutput.filename().empty())
        {
            outError = "Cook output root must name a concrete directory, not a filesystem root: " +
                       outputDir.string();
            return false;
        }

        const fs::path rawParent = absoluteOutput.parent_path();
        const fs::path canonicalParent = fs::weakly_canonical(rawParent, ec);
        if (ec || !fs::is_directory(canonicalParent, ec) || ec)
        {
            outError = "Cook output parent is inaccessible: " + rawParent.string();
            return false;
        }

        const fs::path outputRoot = canonicalParent / absoluteOutput.filename();
        if (PathComponentsEqual(outputRoot, canonicalParent))
        {
            outError = "Cook output root must not equal its parent directory";
            return false;
        }

        const fs::file_status outputStatus = fs::symlink_status(outputRoot, ec);
        if (ec && ec != std::errc::no_such_file_or_directory)
        {
            outError = "Failed to inspect cook output root: " + ec.message();
            return false;
        }
        ec.clear();
        if (fs::exists(outputStatus))
        {
            if (fs::is_symlink(outputStatus) || !fs::is_directory(outputStatus))
            {
                outError = "Cook output root must be a non-symlink directory when it exists: " +
                           outputRoot.string();
                return false;
            }
            if (fs::exists(outputRoot / ".git", ec) && !ec)
            {
                outError = "Refusing to replace a repository root as a cooked package: " +
                           outputRoot.string();
                return false;
            }
        }

        outOutputRoot = outputRoot;
        outOutputParent = canonicalParent;
        outError.clear();
        return true;
    }

    bool ResolveExplicitManifestPath(const fs::path& manifestPath,
                                     fs::path& outManifestPath,
                                     std::string& outError)
    {
        std::error_code ec;
        const fs::path absoluteManifest = fs::absolute(manifestPath, ec).lexically_normal();
        if (ec || absoluteManifest.empty() || absoluteManifest.filename().empty())
        {
            outError = "Cook manifest path must name a concrete file: " + manifestPath.string();
            return false;
        }

        const fs::path canonicalParent = fs::weakly_canonical(absoluteManifest.parent_path(), ec);
        if (ec || !fs::is_directory(canonicalParent, ec) || ec)
        {
            outError = "Explicit cook manifest parent must already exist and be accessible: " +
                       absoluteManifest.parent_path().string();
            return false;
        }

        const fs::path resolved = canonicalParent / absoluteManifest.filename();
        const fs::file_status status = fs::symlink_status(resolved, ec);
        if (ec && ec != std::errc::no_such_file_or_directory)
        {
            outError = "Failed to inspect explicit cook manifest path: " + ec.message();
            return false;
        }
        ec.clear();
        if (fs::exists(status) && (fs::is_symlink(status) || !fs::is_regular_file(status)))
        {
            outError = "Explicit cook manifest path must be a non-symlink regular file when it exists: " +
                       resolved.string();
            return false;
        }

        outManifestPath = resolved;
        outError.clear();
        return true;
    }

    bool PathsShareTransactionVolume(const fs::path& lhs, const fs::path& rhs)
    {
        // Cook publication relies on rename for both package and explicit
        // manifest replacement.  Root names are volume names on Windows; the
        // resolved parents above remove junction/symlink ambiguity before this
        // comparison.
        return PathComponentsEqual(lhs.root_name(), rhs.root_name());
    }

    bool PrepareCookPublicationPaths(const fs::path& canonicalSourceRoot,
                                     const fs::path& requestedOutputRoot,
                                     const fs::path& requestedManifestPath,
                                     CookPublicationPaths& outPaths,
                                     std::string& outError)
    {
        if (!ResolveFutureOutputRoot(requestedOutputRoot,
                                     outPaths.finalOutputRoot,
                                     outPaths.outputParent,
                                     outError))
        {
            return false;
        }

        if (IsPathContainedByRoot(outPaths.finalOutputRoot, canonicalSourceRoot) ||
            IsPathContainedByRoot(canonicalSourceRoot, outPaths.finalOutputRoot))
        {
            outError = "Cook output root must not overlap the source root";
            return false;
        }

        outPaths.stagingOutputRoot = MakeUniqueTransactionSibling(
            outPaths.outputParent, outPaths.finalOutputRoot.filename().string(), "staging");
        bool stagingExists = false;
        if (!PathExists(outPaths.stagingOutputRoot, stagingExists, outError))
        {
            return false;
        }
        if (stagingExists)
        {
            outError = "Cook transaction staging path already exists: " +
                       outPaths.stagingOutputRoot.string();
            return false;
        }

        if (requestedManifestPath.empty())
        {
            outError.clear();
            return true;
        }

        outPaths.publishManifest = true;
        std::error_code manifestPathError;
        const fs::path absoluteManifest =
            fs::absolute(requestedManifestPath, manifestPathError).lexically_normal();
        if (manifestPathError || absoluteManifest.empty() || absoluteManifest.filename().empty())
        {
            outError = "Cook manifest path must name a concrete file: " +
                       requestedManifestPath.string();
            return false;
        }

        if (IsPathContainedByRoot(absoluteManifest, outPaths.finalOutputRoot))
        {
            // The final package root may not exist yet.  Containment was
            // established above from normalized absolute paths, so keep this
            // purely lexical and avoid filesystem::relative(), which attempts
            // to canonicalize the future manifest path on Windows.
            const fs::path relativeManifest =
                absoluteManifest.lexically_relative(outPaths.finalOutputRoot).lexically_normal();
            if (!IsCanonicalRelativePath(relativeManifest.generic_string()))
            {
                outError = "Cook manifest path inside the output package is not canonical";
                return false;
            }
            outPaths.manifestInsideOutput = true;
            outPaths.manifestPath = absoluteManifest;
            outPaths.stagingManifestPath = outPaths.stagingOutputRoot / relativeManifest;
            outError.clear();
            return true;
        }

        if (!ResolveExplicitManifestPath(absoluteManifest, outPaths.manifestPath, outError))
        {
            return false;
        }
        if (IsPathContainedByRoot(outPaths.manifestPath, canonicalSourceRoot))
        {
            outError = "Cook manifest path must not overwrite a source file";
            return false;
        }

        if (!PathsShareTransactionVolume(outPaths.finalOutputRoot, outPaths.manifestPath))
        {
            outError = "Explicit cook manifest is on a different filesystem volume; "
                       "cross-volume package publication is not transactional";
            return false;
        }

        outPaths.manifestInsideOutput = false;
        outPaths.stagingManifestPath = MakeUniqueTransactionSibling(
            outPaths.manifestPath.parent_path(), outPaths.manifestPath.filename().string(), "manifest");
        bool manifestStageExists = false;
        if (!PathExists(outPaths.stagingManifestPath, manifestStageExists, outError))
        {
            return false;
        }
        if (manifestStageExists)
        {
            outError = "Cook transaction manifest staging path already exists: " +
                       outPaths.stagingManifestPath.string();
            return false;
        }

        outError.clear();
        return true;
    }

    void AppendCleanupFailure(std::string& target, const std::string& cleanupError)
    {
        if (cleanupError.empty())
        {
            return;
        }
        if (!target.empty())
        {
            target += "; ";
        }
        target += cleanupError;
    }

    bool PublishStagedCookPackage(const CookPublicationPaths& paths, std::string& outError)
    {
        bool hadOutput = false;
        if (!PathExists(paths.finalOutputRoot, hadOutput, outError))
        {
            return false;
        }

        bool hadManifest = false;
        if (paths.publishManifest && !paths.manifestInsideOutput &&
            !PathExists(paths.manifestPath, hadManifest, outError))
        {
            return false;
        }

        const fs::path outputBackup = MakeUniqueTransactionSibling(
            paths.outputParent, paths.finalOutputRoot.filename().string(), "backup");
        const fs::path manifestBackup = paths.publishManifest && !paths.manifestInsideOutput
            ? MakeUniqueTransactionSibling(paths.manifestPath.parent_path(),
                                           paths.manifestPath.filename().string(),
                                           "backup")
            : fs::path{};
        bool outputMovedToBackup = false;
        bool stagedOutputPublished = false;
        bool manifestMovedToBackup = false;
        bool stagedManifestPublished = false;

        const auto rollback = [&](std::string primaryError)
        {
            std::string rollbackErrors;
            if (stagedManifestPublished)
            {
                std::string removeError;
                if (!RemoveTransactionPath(paths.manifestPath, removeError))
                {
                    AppendCleanupFailure(rollbackErrors, removeError);
                }
            }
            if (manifestMovedToBackup)
            {
                std::string restoreError;
                if (!RenameTransactionPath(manifestBackup,
                                           paths.manifestPath,
                                           "Failed to restore previous external cook manifest",
                                           restoreError))
                {
                    AppendCleanupFailure(rollbackErrors, restoreError);
                }
            }
            if (stagedOutputPublished)
            {
                std::string removeError;
                if (!RemoveTransactionPath(paths.finalOutputRoot, removeError))
                {
                    AppendCleanupFailure(rollbackErrors, removeError);
                }
            }
            if (outputMovedToBackup)
            {
                std::string restoreError;
                if (!RenameTransactionPath(outputBackup,
                                           paths.finalOutputRoot,
                                           "Failed to restore previous cooked package",
                                           restoreError))
                {
                    AppendCleanupFailure(rollbackErrors, restoreError);
                }
            }
            std::string stageCleanupError;
            if (!RemoveTransactionPath(paths.stagingOutputRoot, stageCleanupError))
            {
                AppendCleanupFailure(rollbackErrors, stageCleanupError);
            }
            if (paths.publishManifest && !paths.manifestInsideOutput)
            {
                std::string manifestStageCleanupError;
                if (!RemoveTransactionPath(paths.stagingManifestPath, manifestStageCleanupError))
                {
                    AppendCleanupFailure(rollbackErrors, manifestStageCleanupError);
                }
            }
            outError = std::move(primaryError);
            if (!rollbackErrors.empty())
            {
                outError += "; rollback incomplete: " + rollbackErrors;
            }
            return false;
        };

        if (hadOutput && !RenameTransactionPath(paths.finalOutputRoot,
                                                outputBackup,
                                                "Failed to backup previous cooked package",
                                                outError))
        {
            return rollback(outError);
        }
        outputMovedToBackup = hadOutput;

        if (!RenameTransactionPath(paths.stagingOutputRoot,
                                   paths.finalOutputRoot,
                                   "Failed to publish staged cooked package",
                                   outError))
        {
            return rollback(outError);
        }
        stagedOutputPublished = true;

        if (paths.publishManifest && !paths.manifestInsideOutput)
        {
            if (hadManifest && !RenameTransactionPath(paths.manifestPath,
                                                       manifestBackup,
                                                       "Failed to backup previous external cook manifest",
                                                       outError))
            {
                return rollback(outError);
            }
            manifestMovedToBackup = hadManifest;

            if (!RenameTransactionPath(paths.stagingManifestPath,
                                       paths.manifestPath,
                                       "Failed to publish staged external cook manifest",
                                       outError))
            {
                return rollback(outError);
            }
            stagedManifestPublished = true;
        }

        std::string cleanupError;
        if (outputMovedToBackup && !RemoveTransactionPath(outputBackup, cleanupError))
        {
            return rollback("Cook package publication succeeded but previous package cleanup failed: " +
                            cleanupError);
        }
        if (manifestMovedToBackup && !RemoveTransactionPath(manifestBackup, cleanupError))
        {
            return rollback("Cook package publication succeeded but previous manifest cleanup failed: " +
                            cleanupError);
        }

        outError.clear();
        return true;
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

bool CookManifest::PopulateSuccessfulEntry(CookManifestEntry& entry,
                                           const fs::path& sourceRoot,
                                           const fs::path& outputRoot,
                                           const fs::path& sourcePath,
                                           const fs::path& primaryOutputPath,
                                           const std::vector<fs::path>& productPaths,
                                           const std::vector<fs::path>& dependencyPaths,
                                           const std::string& canonicalCookSettings,
                                           const std::string& importerName,
                                           std::string& outError)
{
    return PopulateSuccessfulEntry(entry,
                                   sourceRoot,
                                   outputRoot,
                                   sourcePath,
                                   primaryOutputPath,
                                   productPaths,
                                   dependencyPaths,
                                   {},
                                   canonicalCookSettings,
                                   importerName,
                                   outError);
}

bool CookManifest::PopulateSuccessfulEntry(CookManifestEntry& entry,
                                           const fs::path& sourceRoot,
                                           const fs::path& outputRoot,
                                           const fs::path& sourcePath,
                                           const fs::path& primaryOutputPath,
                                           const std::vector<fs::path>& productPaths,
                                           const std::vector<fs::path>& dependencyPaths,
                                           const std::vector<fs::path>& sourceDependencyPaths,
                                           const std::string& canonicalCookSettings,
                                           const std::string& importerName,
                                           std::string& outError)
{
    if (!entry.success)
    {
        outError = "Cannot populate cook manifest identity for a failed entry";
        return false;
    }
    if (canonicalCookSettings.empty() || importerName.empty())
    {
        outError = "Cook manifest requires canonical settings and importer name";
        return false;
    }

    fs::path canonicalSourceRoot;
    if (!ResolveDirectoryRoot(sourceRoot, canonicalSourceRoot, outError))
    {
        return false;
    }
    fs::path canonicalOutputRoot;
    if (!ResolveDirectoryRoot(outputRoot, canonicalOutputRoot, outError))
    {
        return false;
    }

    CookManifestEntry populated = entry;
    if (ToLower(sourcePath.extension().string()) == ".gltf")
    {
        if (!CollectGltfSourceDependencyClosure(canonicalSourceRoot,
                                                sourcePath,
                                                populated.sourceContent,
                                                populated.sourceDependencies,
                                                outError))
        {
            return false;
        }
    }
    else if (!CaptureContentIdentity(canonicalSourceRoot,
                                     sourcePath,
                                     populated.sourceContent,
                                     outError))
    {
        return false;
    }
    populated.sourceDependencyClosureRecorded = true;
    if (ToLower(sourcePath.extension().string()) != ".gltf")
    {
        populated.sourceDependencies.clear();
        for (const fs::path& dependencyPath : sourceDependencyPaths)
        {
            if (!AddCapturedContentIdentity(populated.sourceDependencies,
                                            canonicalSourceRoot,
                                            dependencyPath,
                                            outError))
            {
                return false;
            }
        }
        SortContentIdentities(populated.sourceDependencies);
    }
    populated.sourcePath = populated.sourceContent.relativePath;

    CookContentIdentity primaryArtifact;
    if (!CaptureContentIdentity(canonicalOutputRoot,
                                primaryOutputPath,
                                primaryArtifact,
                                outError))
    {
        return false;
    }
    populated.outputPath = primaryArtifact.relativePath;

    populated.artifacts.clear();
    if (!AddCapturedContentIdentity(populated.artifacts,
                                    canonicalOutputRoot,
                                    primaryOutputPath,
                                    outError))
    {
        return false;
    }
    for (const fs::path& productPath : productPaths)
    {
        if (!AddCapturedContentIdentity(populated.artifacts,
                                        canonicalOutputRoot,
                                        productPath,
                                        outError))
        {
            return false;
        }
    }
    SortContentIdentities(populated.artifacts);

    populated.dependencies.clear();
    for (const fs::path& dependencyPath : dependencyPaths)
    {
        CookContentIdentity dependency;
        if (!CaptureContentIdentity(canonicalOutputRoot, dependencyPath, dependency, outError))
        {
            return false;
        }
        if (dependency.relativePath == populated.outputPath)
        {
            continue;
        }
        const auto existing = std::find_if(populated.dependencies.begin(),
                                           populated.dependencies.end(),
                                           [&dependency](const CookContentIdentity& current)
        {
            return current.relativePath == dependency.relativePath;
        });
        if (existing == populated.dependencies.end())
        {
            populated.dependencies.push_back(std::move(dependency));
        }
        else if (!ContentIdentitiesEqual(*existing, dependency))
        {
            outError = "A cook dependency path resolved to conflicting content identities: " +
                       dependency.relativePath;
            return false;
        }
    }
    SortContentIdentities(populated.dependencies);
    populated.dependencyOutputs.clear();
    populated.dependencyOutputs.reserve(populated.dependencies.size());
    for (const CookContentIdentity& dependency : populated.dependencies)
    {
        populated.dependencyOutputs.push_back(dependency.relativePath);
    }

    populated.importerName = importerName;
    populated.canonicalCookSettings = canonicalCookSettings;
    populated.cookSettingsHash = ComputeStringSha256(populated.canonicalCookSettings);
    populated.recipeHash = BuildRecipeHash(populated);

    entry = std::move(populated);
    outError.clear();
    return true;
}

bool CookManifest::Save(const fs::path& manifestPath,
                        std::string& outError,
                        const fs::path& validationOutputRoot) const
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

    fs::path canonicalSourceRoot;
    if (!ResolveDirectoryRoot(sourceRoot, canonicalSourceRoot, outError))
    {
        outError = "Failed to validate cook manifest source root: " + outError;
        return false;
    }
    fs::path canonicalOutputRoot;
    const fs::path& rootForValidation =
        validationOutputRoot.empty() ? fs::path(outputRoot) : validationOutputRoot;
    if (!ResolveDirectoryRoot(rootForValidation, canonicalOutputRoot, outError))
    {
        outError = "Failed to validate cook manifest output root: " + outError;
        return false;
    }
    for (const CookManifestEntry& entry : entries)
    {
        if (entry.success &&
            !ValidateSuccessfulCookManifestEntry(entry,
                                                 canonicalSourceRoot,
                                                 canonicalOutputRoot,
                                                 outError))
        {
            outError = "Cook manifest v2 validation failed: " + outError;
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

        file << "RVX_COOK_MANIFEST_V2\n";
        file << "schema=" << SchemaName << "\n";
        file << "version=" << Version << "\n";
        file << "toolName=" << ToolName << "\n";
        file << "toolVersion=" << ToolVersion << "\n";
        // Persisted v2 roots are observations, never machine-specific authority.
        // Mounted roots are supplied independently at admission time.
        file << "sourceRoot=.\n";
        file << "outputRoot=.\n";
        file << "recursive=" << (recursive ? 1 : 0) << "\n";
        file << "entryCount=" << entries.size() << "\n";
        file << "successCount=" << GetSuccessCount() << "\n";
        file << "failureCount=" << GetFailureCount() << "\n";

        std::vector<const CookManifestEntry*> sortedEntries;
        sortedEntries.reserve(entries.size());
        for (const CookManifestEntry& entry : entries)
        {
            sortedEntries.push_back(&entry);
        }
        std::sort(sortedEntries.begin(), sortedEntries.end(),
                  [](const CookManifestEntry* lhs, const CookManifestEntry* rhs)
        {
            if (lhs->sourcePath != rhs->sourcePath)
            {
                return lhs->sourcePath < rhs->sourcePath;
            }
            if (lhs->outputPath != rhs->outputPath)
            {
                return lhs->outputPath < rhs->outputPath;
            }
            return static_cast<uint32>(lhs->type) < static_cast<uint32>(rhs->type);
        });

        for (size_t entryIndex = 0; entryIndex < sortedEntries.size(); ++entryIndex)
        {
            const CookManifestEntry& entry = *sortedEntries[entryIndex];
            const std::string prefix = "entry." + std::to_string(entryIndex) + ".";
            file << prefix << "source=" << EscapeManifestValue(entry.sourcePath) << "\n";
            file << prefix << "output=" << EscapeManifestValue(entry.outputPath) << "\n";
            file << prefix << "type=" << ToAssetTypeString(entry.type) << "\n";
            file << prefix << "success=" << (entry.success ? 1 : 0) << "\n";
            file << prefix << "importer=" << EscapeManifestValue(entry.importerName) << "\n";
            file << prefix << "source.byteCount=" << entry.sourceContent.byteCount << "\n";
            file << prefix << "source.sha256=" << entry.sourceContent.sha256 << "\n";
            if (entry.sourceDependencyClosureRecorded)
            {
                file << prefix << "sourceDependencyCount="
                     << entry.sourceDependencies.size() << "\n";
                for (size_t sourceDependencyIndex = 0;
                     sourceDependencyIndex < entry.sourceDependencies.size();
                     ++sourceDependencyIndex)
                {
                    const CookContentIdentity& dependency =
                        entry.sourceDependencies[sourceDependencyIndex];
                    const std::string dependencyPrefix = prefix + "sourceDependency." +
                        std::to_string(sourceDependencyIndex);
                    file << dependencyPrefix << ".path="
                         << EscapeManifestValue(dependency.relativePath) << "\n";
                    file << dependencyPrefix << ".byteCount=" << dependency.byteCount << "\n";
                    file << dependencyPrefix << ".sha256=" << dependency.sha256 << "\n";
                }
            }
            file << prefix << "cookSettings="
                 << EscapeManifestValue(entry.canonicalCookSettings) << "\n";
            file << prefix << "cookSettingsHash=" << entry.cookSettingsHash << "\n";
            file << prefix << "recipeHash=" << entry.recipeHash << "\n";

            std::vector<std::string> warnings = entry.warnings;
            std::sort(warnings.begin(), warnings.end());
            file << prefix << "warningCount=" << warnings.size() << "\n";
            for (size_t warningIndex = 0; warningIndex < warnings.size(); ++warningIndex)
            {
                file << prefix << "warning." << warningIndex << "="
                     << EscapeManifestValue(warnings[warningIndex]) << "\n";
            }
            file << prefix << "dependencyCount=" << entry.dependencies.size() << "\n";
            for (size_t dependencyIndex = 0; dependencyIndex < entry.dependencies.size(); ++dependencyIndex)
            {
                const CookContentIdentity& dependency = entry.dependencies[dependencyIndex];
                file << prefix << "dependency." << dependencyIndex << ".path="
                     << EscapeManifestValue(dependency.relativePath) << "\n";
                file << prefix << "dependency." << dependencyIndex << ".byteCount="
                     << dependency.byteCount << "\n";
                file << prefix << "dependency." << dependencyIndex << ".sha256="
                     << dependency.sha256 << "\n";
            }
            file << prefix << "artifactCount=" << entry.artifacts.size() << "\n";
            for (size_t artifactIndex = 0; artifactIndex < entry.artifacts.size(); ++artifactIndex)
            {
                const CookContentIdentity& artifact = entry.artifacts[artifactIndex];
                file << prefix << "artifact." << artifactIndex << ".path="
                     << EscapeManifestValue(artifact.relativePath) << "\n";
                file << prefix << "artifact." << artifactIndex << ".byteCount="
                     << artifact.byteCount << "\n";
                file << prefix << "artifact." << artifactIndex << ".sha256="
                     << artifact.sha256 << "\n";
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
                                           ImportOptionsProvider optionsProvider,
                                           StagingMutator stagingMutator)
{
    CookManifest manifest;
    manifest.recursive = recursive;

    fs::path canonicalSourceRoot;
    if (!ResolveDirectoryRoot(sourceDir, canonicalSourceRoot, manifest.manifestError))
    {
        manifest.manifestError = "Source directory does not exist or is inaccessible: " +
                                 sourceDir.string();
        return manifest;
    }

    manifest.sourceRoot = ToGenericPathString(canonicalSourceRoot);
    CookPublicationPaths publication;
    if (!PrepareCookPublicationPaths(canonicalSourceRoot,
                                     outputDir,
                                     manifestPath,
                                     publication,
                                     manifest.manifestError))
    {
        return manifest;
    }
    manifest.outputRoot = ToGenericPathString(publication.finalOutputRoot);

    std::error_code stagingError;
    fs::create_directory(publication.stagingOutputRoot, stagingError);
    if (stagingError)
    {
        manifest.manifestError = "Failed to create cook transaction staging root: " +
                                 stagingError.message();
        return manifest;
    }

    const auto abortTransaction = [&](std::string error)
    {
        std::string cleanupError;
        if (!RemoveTransactionPath(publication.stagingOutputRoot, cleanupError))
        {
            error += "; staging cleanup failed: " + cleanupError;
        }
        if (publication.publishManifest && !publication.manifestInsideOutput)
        {
            std::string manifestCleanupError;
            if (!RemoveTransactionPath(publication.stagingManifestPath, manifestCleanupError))
            {
                error += "; manifest staging cleanup failed: " + manifestCleanupError;
            }
        }
        manifest.manifestWritten = false;
        manifest.manifestError = std::move(error);
        return manifest;
    };

    try
    {
        std::vector<fs::path> filesToImport =
            CollectImportableFiles(*this, canonicalSourceRoot, recursive);
        size_t processed = 0;
        for (const fs::path& filePath : filesToImport)
        {
            CookManifestEntry entry;
            const AssetType assetType =
                GetAssetTypeFromExtension(ToLower(filePath.extension().string()));
            entry.type = assetType;

            std::error_code relativeError;
            const fs::path relativePath =
                fs::relative(filePath, canonicalSourceRoot, relativeError).lexically_normal();
            if (relativeError || !IsCanonicalRelativePath(relativePath.generic_string()))
            {
                entry.success = false;
                entry.error = "Failed to make cook source path canonical and root-contained: " +
                              filePath.string();
            }
            else
            {
                entry.sourcePath = relativePath.generic_string();
                fs::path outPath = publication.stagingOutputRoot / relativePath;
                outPath.replace_extension(".rva");
                fs::path relativeOutputPath = relativePath;
                relativeOutputPath.replace_extension(".rva");
                entry.outputPath = ToGenericPathString(relativeOutputPath);
                if (!IsCanonicalRelativePath(entry.outputPath))
                {
                    entry.success = false;
                    entry.error = "Failed to make cook output path canonical and root-contained: " +
                                  outPath.string();
                }
                else
                {
                    std::error_code outputError;
                    fs::create_directories(outPath.parent_path(), outputError);
                    if (outputError)
                    {
                        entry.success = false;
                        entry.error = "Failed to create cook staging output directory: " +
                                      outputError.message();
                    }
                    else
                    {
                        const void* requestedImportOptions =
                            optionsProvider ? optionsProvider(filePath, assetType) : nullptr;
                        std::optional<ModelImportOptions> canonicalModelOptions;
                        const void* importOptions = requestedImportOptions;
                        if (assetType == AssetType::Model)
                        {
                            canonicalModelOptions = requestedImportOptions
                                ? *static_cast<const ModelImportOptions*>(requestedImportOptions)
                                : ModelImportOptions{};
                            canonicalModelOptions->artifactSourcePath =
                                relativePath.generic_string();
                            importOptions = &*canonicalModelOptions;
                        }
                        const bool isExternalGltf =
                            ToLower(filePath.extension().string()) == ".gltf";
                        CookContentIdentity sourceBeforeImport;
                        std::vector<CookContentIdentity> sourceDependenciesBeforeImport;
                        std::string sourceClosureError;
                        ImportResult result;
                        if (isExternalGltf &&
                            !CollectGltfSourceDependencyClosure(
                                canonicalSourceRoot,
                                filePath,
                                sourceBeforeImport,
                                sourceDependenciesBeforeImport,
                                sourceClosureError))
                        {
                            result.error = "Cook manifest source dependency closure capture failed: " +
                                           sourceClosureError;
                        }
                        else
                        {
                            result = ImportAsset(filePath, outPath, importOptions);
                        }

                        entry.success = result.success;
                        entry.error = result.error;
                        entry.warnings = std::move(result.warnings);
                        entry.sourceModTime = GetFileWriteTimeTicks(filePath);
                        if (entry.success)
                        {
                            IAssetImporter* importer = GetImporter(filePath.extension().string());
                            std::vector<fs::path> productPaths;
                            productPaths.reserve(result.outputPaths.size() + 1u);
                            productPaths.push_back(outPath);
                            for (const std::string& productPath : result.outputPaths)
                            {
                                productPaths.emplace_back(productPath);
                            }

                            std::vector<fs::path> dependencyPaths;
                            dependencyPaths.reserve(result.outputPaths.size());
                            for (const std::string& dependencyPath : result.outputPaths)
                            {
                                dependencyPaths.emplace_back(dependencyPath);
                            }

                            std::string identityError;
                            if (!importer || !CookManifest::PopulateSuccessfulEntry(
                                                 entry,
                                                 canonicalSourceRoot,
                                                 publication.stagingOutputRoot,
                                                 filePath,
                                                 outPath,
                                                 productPaths,
                                                 dependencyPaths,
                                                 BuildCanonicalCookSettings(assetType, importOptions),
                                                 importer ? std::string(importer->GetName()) : std::string{},
                                                 identityError))
                            {
                                entry.success = false;
                                entry.error = "Cook manifest v2 identity capture failed: " + identityError;
                                entry.dependencyOutputs.clear();
                                entry.dependencies.clear();
                                entry.artifacts.clear();
                                entry.sourceContent = {};
                                entry.canonicalCookSettings.clear();
                                entry.cookSettingsHash.clear();
                                entry.recipeHash.clear();
                            }
                            else
                            {
                                if (isExternalGltf &&
                                    !SourceClosuresEqual(sourceBeforeImport,
                                                         sourceDependenciesBeforeImport,
                                                         entry.sourceContent,
                                                         entry.sourceDependencies))
                                {
                                    entry.success = false;
                                    entry.error =
                                        "glTF source dependency closure changed while the cooked product was produced";
                                    entry.dependencyOutputs.clear();
                                    entry.dependencies.clear();
                                    entry.artifacts.clear();
                                    entry.sourceContent = {};
                                    entry.sourceDependencies.clear();
                                    entry.sourceDependencyClosureRecorded = false;
                                    entry.canonicalCookSettings.clear();
                                    entry.cookSettingsHash.clear();
                                    entry.recipeHash.clear();
                                }
                                else
                                {
                                    entry.outputModTime = GetFileWriteTimeTicks(outPath);
                                    entry.outputSize = GetFileSizeBytes(outPath);
                                }
                            }
                        }
                    }
                }
            }

            manifest.entries.push_back(std::move(entry));
            if (callback && !filesToImport.empty())
            {
                const float progress = static_cast<float>(++processed) / filesToImport.size();
                callback(progress, filePath.filename().string());
            }
        }

        if (manifest.GetFailureCount() != 0u)
        {
            return abortTransaction("Cook transaction aborted because one or more entries failed; "
                                    "the previous package was preserved");
        }

        if (stagingMutator)
        {
            std::string mutatorError;
            if (!stagingMutator(manifest, publication.stagingOutputRoot, mutatorError))
            {
                return abortTransaction("Cook transaction staging finalization failed: " + mutatorError);
            }
            if (manifest.GetFailureCount() != 0u)
            {
                return abortTransaction("Cook transaction staging finalization produced failed entries; "
                                        "the previous package was preserved");
            }
        }

        const fs::path validationManifestPath = publication.publishManifest
            ? publication.stagingManifestPath
            : publication.stagingOutputRoot / ".rvx-cook-validation.manifest";
        std::string manifestSaveError;
        if (!manifest.Save(validationManifestPath,
                           manifestSaveError,
                           publication.stagingOutputRoot))
        {
            return abortTransaction("Cook transaction manifest validation failed: " + manifestSaveError);
        }

        if (!publication.publishManifest)
        {
            std::string validationCleanupError;
            if (!RemoveTransactionPath(validationManifestPath, validationCleanupError))
            {
                return abortTransaction("Cook transaction could not remove its validation manifest: " +
                                        validationCleanupError);
            }
        }

        std::string publishError;
        if (!PublishStagedCookPackage(publication, publishError))
        {
            manifest.manifestWritten = false;
            manifest.manifestError = "Cook transaction publication failed: " + publishError;
            return manifest;
        }

        manifest.manifestWritten = publication.publishManifest;
        manifest.manifestError.clear();
        return manifest;
    }
    catch (const std::exception& exception)
    {
        return abortTransaction("Cook transaction threw an exception before publication: " +
                                std::string(exception.what()));
    }
    catch (...)
    {
        return abortTransaction("Cook transaction threw an unknown exception before publication");
    }
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
        {".gltf", AssetType::Model},
        {".glb", AssetType::Model},
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
// ModelImporter
// ============================================================================

ImportResult ModelImporter::Import(const fs::path& sourcePath,
                                   const fs::path& outputPath,
                                   const void* options)
{
    ImportResult result;
    const ModelImportOptions importOptions = options
        ? *static_cast<const ModelImportOptions*>(options)
        : ModelImportOptions{};
    fs::path artifactSourcePath = sourcePath;
    if (!importOptions.artifactSourcePath.empty())
    {
        if (!IsCanonicalRelativePath(importOptions.artifactSourcePath))
        {
            result.error = "ModelImporter artifact source identity must be a canonical relative path.";
            return result;
        }
        artifactSourcePath = fs::path(importOptions.artifactSourcePath);
    }
    const std::string extension = ToLower(sourcePath.extension().string());
    if (extension != ".gltf" && extension != ".glb")
    {
        result.error = "ModelImporter currently supports only glTF/GLB: " +
                       sourcePath.string();
        return result;
    }

    Resource::GLTFImportOptions gltfOptions;
    gltfOptions.generateNormals = true;
    gltfOptions.generateTangents = importOptions.mesh.generateTangents;
    gltfOptions.mergeMeshes = false;
    gltfOptions.scaleFactor = importOptions.mesh.scaleFactor;

    Resource::GLTFImporter importer;
    Resource::GLTFImportResult imported =
        importer.Import(fs::absolute(sourcePath).string(), gltfOptions);
    if (!imported.success)
    {
        result.error = imported.errorMessage.empty()
            ? "Model import failed"
            : imported.errorMessage;
        return result;
    }
    if (imported.hasMorphTargets)
    {
        result.error = "Cooked model products do not support glTF morph targets.";
        return result;
    }
    const bool sourceDeclaresSkeletalPayload = imported.hasSkins || imported.hasAnimations;
    const bool importerProducedSkeletalPayload = imported.skeleton ||
                                                 !imported.animationClips.empty();
    if (sourceDeclaresSkeletalPayload)
    {
        if (!importOptions.mesh.importAnimations)
        {
            result.error = "Cooked skeletal models require MeshImportOptions::importAnimations.";
            return result;
        }
        if (!imported.hasSkins || !imported.hasAnimations || !imported.skeleton ||
            imported.animationClips.empty())
        {
            result.error = "Cooked skeletal models require one valid glTF skin and at least one parsed clip.";
            return result;
        }
    }
    else if (importerProducedSkeletalPayload)
    {
        result.error = "glTF skeletal feature metadata and parsed payload disagree.";
        return result;
    }

    // These material extensions are intentionally accepted because the current
    // source runtime imports the same core metallic/roughness subset. Keeping
    // them source-compatible preserves Source/Cooked parity for the Porsche
    // qualification asset. Geometry, compression and required runtime
    // extensions remain fail-closed.
    const std::unordered_set<std::string> sourceCompatibleExtensions = {
        "KHR_materials_specular",
        "KHR_materials_clearcoat",
        "KHR_materials_transmission",
    };
    for (const std::string& used : imported.extensionsUsed)
    {
        if (!sourceCompatibleExtensions.contains(used))
        {
            result.error = "Cooked model products do not support glTF extension: " + used;
            return result;
        }
        result.warnings.push_back(
            "Cooked model preserves current source-runtime core PBR semantics and does not add extension-specific shading for " +
            used);
    }
    for (const std::string& required : imported.extensionsRequired)
    {
        if (!sourceCompatibleExtensions.contains(required))
        {
            result.error = "Cooked model products cannot satisfy required glTF extension: " + required;
            return result;
        }
    }
    for (const std::string& warning : imported.warnings)
        result.warnings.push_back(warning);

    std::vector<std::vector<CookedMeshLodPayload>> meshLods;
    meshLods.reserve(imported.meshes.size());
    for (const Mesh::Ptr& mesh : imported.meshes)
    {
        if (!mesh || !mesh->IsValid())
        {
            result.error = "Model import produced an invalid mesh";
            return result;
        }
        meshLods.push_back(
            BuildLowerMeshLods(*mesh, importOptions.mesh, result.warnings));
    }

    const fs::path finalDependencyDirectory =
        outputPath.parent_path() /
        (outputPath.stem().string() + ".rvdeps");
    // This transaction is nested under CookDirectory's own staging root. Keep
    // the inner role compact so a valid worktree-relative model path does not
    // exhaust the legacy Windows path budget before its animation/texture
    // dependency filenames are appended. Uniqueness still comes from the
    // monotonic sequence and steady-clock token in MakeUniqueTransactionSibling.
    const fs::path stagingDependencyDirectory = MakeUniqueTransactionSibling(
        finalDependencyDirectory.parent_path(),
        finalDependencyDirectory.filename().string(),
        "m");
    const fs::path temporaryRoot = MakeUniqueTransactionSibling(
        outputPath.parent_path(), outputPath.filename().string(), "m");
    std::error_code fileError;
    const bool dependencyStageExists = fs::exists(stagingDependencyDirectory, fileError);
    if (fileError)
    {
        result.error = "Failed to reserve a unique model import staging path: " +
                       fileError.message();
        return result;
    }
    const bool rootStageExists = fs::exists(temporaryRoot, fileError);
    if (fileError || dependencyStageExists || rootStageExists)
    {
        result.error = fileError
            ? "Failed to reserve a unique model import staging path: " + fileError.message()
            : "Unique model import staging path already exists.";
        return result;
    }
    fs::create_directories(stagingDependencyDirectory / "textures", fileError);
    if (fileError)
    {
        result.error = "Failed to create model dependency staging directory: " +
                       fileError.message();
        return result;
    }

    const fs::path stagingMeshPath = stagingDependencyDirectory / "meshes.rva";
    if (!WriteMeshArtifact(stagingMeshPath,
                           artifactSourcePath,
                           importOptions.mesh,
                           imported.meshes,
                           meshLods,
                           result.error))
    {
        fs::remove_all(stagingDependencyDirectory, fileError);
        return result;
    }

    Resource::CookedModelArtifact modelArtifact;
    modelArtifact.sourcePath = artifactSourcePath.generic_string();
    modelArtifact.meshArtifactPath =
        (finalDependencyDirectory.filename() / "meshes.rva").generic_string();
    if (sourceDeclaresSkeletalPayload)
    {
        modelArtifact.animationArtifactPath =
            (finalDependencyDirectory.filename() / "animation.rvxanim").generic_string();
    }
    modelArtifact.materials = imported.materials;
    modelArtifact.rootNode = imported.model ? imported.model->GetRootNode() : nullptr;
    if (imported.model && imported.model->GetBoundingBox().IsValid())
        modelArtifact.bounds = imported.model->GetBoundingBox();

    if (sourceDeclaresSkeletalPayload)
    {
        Resource::CookedAnimationArtifact animationArtifact;
        animationArtifact.sourcePath = artifactSourcePath.generic_string();
        animationArtifact.skeleton = imported.skeleton;
        for (const auto& [name, sourceClip] : imported.animationClips)
        {
            if (!sourceClip)
            {
                result.error = "Model import produced a null animation clip.";
                fs::remove_all(stagingDependencyDirectory, fileError);
                return result;
            }
            auto canonicalClip =
                std::make_shared<Animation::AnimationClip>(*sourceClip);
            canonicalClip->metadata.sourceFile =
                artifactSourcePath.generic_string();
            animationArtifact.clips.emplace(name, std::move(canonicalClip));
        }
        std::vector<uint8> animationBytes;
        if (!Resource::SerializeCookedAnimationArtifact(animationArtifact,
                                                        animationBytes,
                                                        result.error))
        {
            fs::remove_all(stagingDependencyDirectory, fileError);
            return result;
        }
        const fs::path stagingAnimationPath =
            stagingDependencyDirectory / "animation.rvxanim";
        std::ofstream animationFile(stagingAnimationPath, std::ios::binary);
        if (!animationFile.is_open() ||
            !animationFile.write(reinterpret_cast<const char*>(animationBytes.data()),
                                 static_cast<std::streamsize>(animationBytes.size())))
        {
            result.error = "Failed to write cooked animation dependency staging file: " +
                           stagingAnimationPath.string();
            fs::remove_all(stagingDependencyDirectory, fileError);
            return result;
        }
        animationFile.close();
        if (!animationFile)
        {
            result.error = "Failed to finalize cooked animation dependency staging file: " +
                           stagingAnimationPath.string();
            fs::remove_all(stagingDependencyDirectory, fileError);
            return result;
        }
    }

    Resource::TextureLoader textureLoader(nullptr, true);
    modelArtifact.textures.reserve(imported.textures.size());
    for (size_t index = 0; index < imported.textures.size(); ++index)
    {
        Resource::TextureReference& sourceReference = imported.textures[index];
        Resource::TextureResource* texture = textureLoader.LoadFromReference(
            sourceReference,
            fs::absolute(sourcePath).string(),
            {},
            fs::absolute(outputPath).generic_string());
        if (!texture || textureLoader.WasLastLoadFallback())
        {
            result.error = textureLoader.GetLastLoadError().empty()
                ? "Failed to cook model texture dependency"
                : textureLoader.GetLastLoadError();
            fs::remove_all(stagingDependencyDirectory, fileError);
            return result;
        }
        std::unique_ptr<Resource::TextureResource> textureOwner(texture);
        const fs::path textureName =
            "texture_" + std::to_string(index) + ".rva";
        const fs::path stagingTexturePath =
            stagingDependencyDirectory / "textures" / textureName;
        TextureImportOptions textureOptions;
        textureOptions.compressionMode =
            sourceReference.usage == Resource::TextureUsage::Normal
                ? TextureCompressionMode::BC5
                : TextureCompressionMode::BC7;
        textureOptions.compress = true;
        textureOptions.sRGB = sourceReference.isSRGB;
        if (!WriteTextureArtifact(stagingTexturePath,
                                  artifactSourcePath,
                                  textureOptions,
                                  *textureOwner,
                                  result.warnings,
                                  result.error))
        {
            fs::remove_all(stagingDependencyDirectory, fileError);
            return result;
        }

        Resource::TextureReference cookedReference =
            Resource::TextureReference::CreateExternal(
                (finalDependencyDirectory.filename() / "textures" / textureName)
                    .generic_string(),
                sourceReference.usage,
                sourceReference.isSRGB);
        cookedReference.imageIndex = sourceReference.imageIndex;
        cookedReference.fallbackSemantic = sourceReference.fallbackSemantic;
        modelArtifact.textures.push_back(std::move(cookedReference));
    }

    std::vector<uint8> rootBytes;
    if (!Resource::SerializeCookedModelArtifact(
            modelArtifact, rootBytes, result.error))
    {
        fs::remove_all(stagingDependencyDirectory, fileError);
        return result;
    }
    if (!temporaryRoot.parent_path().empty())
        fs::create_directories(temporaryRoot.parent_path(), fileError);
    std::ofstream rootFile(temporaryRoot, std::ios::binary);
    if (!rootFile.is_open() ||
        !rootFile.write(reinterpret_cast<const char*>(rootBytes.data()),
                        static_cast<std::streamsize>(rootBytes.size())))
    {
        result.error = "Failed to write model artifact staging file: " +
                       temporaryRoot.string();
        fs::remove_all(stagingDependencyDirectory, fileError);
        fs::remove(temporaryRoot, fileError);
        return result;
    }
    rootFile.close();
    if (!rootFile)
    {
        result.error = "Failed to finalize model artifact staging file: " +
                       temporaryRoot.string();
        fs::remove_all(stagingDependencyDirectory, fileError);
        fs::remove(temporaryRoot, fileError);
        return result;
    }

    // Publish the root and contained dependency directory as a small local
    // transaction. The staged products have already been fully encoded and
    // validated; preserve the prior pair if either rename fails.
    // Direct importer clients can target the same product path concurrently;
    // serialize the replace/rollback critical section so one import cannot
    // treat another import's final product as an orphaned backup.
    static std::mutex s_modelPublicationMutex;
    const std::lock_guard<std::mutex> publicationLock(s_modelPublicationMutex);
    static std::atomic<uint64> s_modelPublicationSequence{0};
    const uint64 publicationToken =
        static_cast<uint64>(std::chrono::steady_clock::now().time_since_epoch().count()) +
        ++s_modelPublicationSequence;
    const fs::path rootBackup = outputPath.parent_path() /
        ("." + outputPath.filename().string() + ".rvx-model-backup-" +
         std::to_string(publicationToken));
    const fs::path dependencyBackup = finalDependencyDirectory.parent_path() /
        ("." + finalDependencyDirectory.filename().string() + ".rvx-model-backup-" +
         std::to_string(publicationToken));
    const bool hadRoot = fs::exists(outputPath, fileError);
    if (fileError)
    {
        result.error = "Failed to establish a safe model product publication transaction: " +
                       fileError.message();
        fs::remove_all(stagingDependencyDirectory, fileError);
        fs::remove(temporaryRoot, fileError);
        return result;
    }
    const bool hadDependencies = fs::exists(finalDependencyDirectory, fileError);
    if (fileError)
    {
        result.error = "Failed to establish a safe model product publication transaction: " +
                       fileError.message();
        fs::remove_all(stagingDependencyDirectory, fileError);
        fs::remove(temporaryRoot, fileError);
        return result;
    }
    const bool rootBackupExists = fs::exists(rootBackup, fileError);
    if (fileError)
    {
        result.error = "Failed to establish a safe model product publication transaction: " +
                       fileError.message();
        fs::remove_all(stagingDependencyDirectory, fileError);
        fs::remove(temporaryRoot, fileError);
        return result;
    }
    const bool dependencyBackupExists = fs::exists(dependencyBackup, fileError);
    if (fileError || rootBackupExists || dependencyBackupExists)
    {
        result.error = fileError
            ? "Failed to establish a safe model product publication transaction: " +
                  fileError.message()
            : "Refusing to overwrite an existing model publication backup path.";
        fs::remove_all(stagingDependencyDirectory, fileError);
        fs::remove(temporaryRoot, fileError);
        return result;
    }

    bool rootBackedUp = false;
    bool dependenciesBackedUp = false;
    bool dependenciesPublished = false;
    bool rootPublished = false;
    const auto rollbackPublication = [&](std::string error)
    {
        std::string rollbackError;
        const auto appendRollbackError = [&](const std::error_code& errorCode)
        {
            if (!errorCode)
                return;
            if (!rollbackError.empty())
                rollbackError += "; ";
            rollbackError += errorCode.message();
        };

        if (rootPublished)
        {
            std::error_code cleanupError;
            fs::remove(outputPath, cleanupError);
            appendRollbackError(cleanupError);
        }
        if (dependenciesPublished)
        {
            std::error_code cleanupError;
            fs::remove_all(finalDependencyDirectory, cleanupError);
            appendRollbackError(cleanupError);
        }
        if (dependenciesBackedUp)
        {
            std::error_code restoreError;
            fs::rename(dependencyBackup, finalDependencyDirectory, restoreError);
            appendRollbackError(restoreError);
        }
        if (rootBackedUp)
        {
            std::error_code restoreError;
            fs::rename(rootBackup, outputPath, restoreError);
            appendRollbackError(restoreError);
        }
        std::error_code cleanupError;
        fs::remove_all(stagingDependencyDirectory, cleanupError);
        appendRollbackError(cleanupError);
        cleanupError.clear();
        fs::remove(temporaryRoot, cleanupError);
        appendRollbackError(cleanupError);

        if (!rollbackError.empty())
            error += "; rollback incomplete: " + rollbackError;
        result.error = std::move(error);
        return result;
    };

    if (hadRoot)
    {
        fs::rename(outputPath, rootBackup, fileError);
        if (fileError)
            return rollbackPublication("Failed to backup previous model artifact: " +
                                       fileError.message());
        rootBackedUp = true;
    }
    if (hadDependencies)
    {
        fs::rename(finalDependencyDirectory, dependencyBackup, fileError);
        if (fileError)
            return rollbackPublication("Failed to backup previous model dependencies: " +
                                       fileError.message());
        dependenciesBackedUp = true;
    }

    fs::rename(stagingDependencyDirectory, finalDependencyDirectory, fileError);
    if (fileError)
        return rollbackPublication("Failed to publish model dependency directory: " +
                                   fileError.message());
    dependenciesPublished = true;

    fs::rename(temporaryRoot, outputPath, fileError);
    if (fileError)
        return rollbackPublication("Failed to publish model artifact: " + fileError.message());
    rootPublished = true;

    if (rootBackedUp)
    {
        fs::remove(rootBackup, fileError);
        if (fileError)
        {
            result.warnings.push_back(
                "Model publication succeeded but old root backup could not be removed: " +
                fileError.message());
        }
    }
    if (dependenciesBackedUp)
    {
        fileError.clear();
        fs::remove_all(dependencyBackup, fileError);
        if (fileError)
        {
            result.warnings.push_back(
                "Model publication succeeded but old dependency backup could not be removed: " +
                fileError.message());
        }
    }

    result.success = true;
    result.outputPaths.push_back(outputPath.string());
    result.outputPaths.push_back(
        (finalDependencyDirectory / "meshes.rva").string());
    if (sourceDeclaresSkeletalPayload)
    {
        result.outputPaths.push_back(
            (finalDependencyDirectory / "animation.rvxanim").string());
    }
    for (size_t index = 0; index < imported.textures.size(); ++index)
    {
        result.outputPaths.push_back(
            (finalDependencyDirectory / "textures" /
             ("texture_" + std::to_string(index) + ".rva"))
                .string());
    }
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
