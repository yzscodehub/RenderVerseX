/** @file CookedTextureArtifact.cpp @brief Strict cooked texture parser */

#include "Resource/Cooked/CookedTextureArtifact.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>

namespace RVX::Resource
{
namespace
{
    std::optional<uint32> ParseUint32Field(
        const std::unordered_map<std::string, std::string>& fields,
        const std::string& key)
    {
        const auto found = fields.find(key);
        if (found == fields.end())
            return std::nullopt;
        try
        {
            const unsigned long value = std::stoul(found->second);
            if (value > std::numeric_limits<uint32>::max())
                return std::nullopt;
            return static_cast<uint32>(value);
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    std::optional<bool> ParseBoolField(
        const std::unordered_map<std::string, std::string>& fields,
        const std::string& key)
    {
        const std::optional<uint32> value = ParseUint32Field(fields, key);
        if (!value || *value > 1u)
            return std::nullopt;
        return *value != 0u;
    }

    std::optional<TextureFormat> ParseTextureFormat(const std::string& value)
    {
        if (value == "RGBA8") return TextureFormat::RGBA8;
        if (value == "RGBA16F") return TextureFormat::RGBA16F;
        if (value == "RGBA32F") return TextureFormat::RGBA32F;
        if (value == "RGB8") return TextureFormat::RGB8;
        if (value == "RG8") return TextureFormat::RG8;
        if (value == "R8") return TextureFormat::R8;
        if (value == "BC1") return TextureFormat::BC1;
        if (value == "BC3") return TextureFormat::BC3;
        if (value == "BC5") return TextureFormat::BC5;
        if (value == "BC7") return TextureFormat::BC7;
        return std::nullopt;
    }

    std::optional<TextureUsage> ParseTextureUsage(const std::string& value)
    {
        if (value == "Color") return TextureUsage::Color;
        if (value == "Normal") return TextureUsage::Normal;
        if (value == "Data") return TextureUsage::Data;
        return std::nullopt;
    }

    const char* GetCompressionName(TextureFormat format)
    {
        switch (format)
        {
            case TextureFormat::BC1: return "BC1";
            case TextureFormat::BC3: return "BC3";
            case TextureFormat::BC5: return "BC5";
            case TextureFormat::BC7: return "BC7";
            default: return "";
        }
    }

    std::optional<size_t> GetExpectedDataSize(
        uint32 width,
        uint32 height,
        uint32 mipLevels,
        TextureFormat format)
    {
        if (width == 0 || height == 0 || mipLevels == 0)
            return std::nullopt;
        size_t total = 0;
        for (uint32 mip = 0; mip < mipLevels; ++mip)
        {
            const uint32 mipWidth = std::max(1u, width >> mip);
            const uint32 mipHeight = std::max(1u, height >> mip);
            size_t level = 0;
            if (format == TextureFormat::RGBA8)
            {
                level = static_cast<size_t>(mipWidth) * mipHeight * 4u;
            }
            else if (format == TextureFormat::BC1)
            {
                level = static_cast<size_t>((mipWidth + 3u) / 4u) *
                    ((mipHeight + 3u) / 4u) * 8u;
            }
            else if (format == TextureFormat::BC3 ||
                     format == TextureFormat::BC5 ||
                     format == TextureFormat::BC7)
            {
                level = static_cast<size_t>((mipWidth + 3u) / 4u) *
                    ((mipHeight + 3u) / 4u) * 16u;
            }
            else
            {
                return std::nullopt;
            }
            if (level > std::numeric_limits<size_t>::max() - total)
                return std::nullopt;
            total += level;
        }
        return total;
    }
} // namespace

bool DeserializeCookedTextureArtifact(
    std::span<const uint8> bytes,
    CookedTextureArtifact& outArtifact,
    std::string& outError)
{
    constexpr std::string_view magic = "RVX_TEXTURE_PREBAKE_V1\n";
    constexpr std::string_view dataMarker = "RVX_TEXTURE_DATA_BEGIN\n";
    constexpr std::string_view endMarker = "\nRVX_TEXTURE_PREBAKE_END\n";

    outArtifact = {};
    const std::string_view text(
        reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (!text.starts_with(magic))
    {
        outError = "Texture artifact missing RVX_TEXTURE_PREBAKE_V1 magic";
        return false;
    }
    const size_t markerOffset = text.find(dataMarker);
    if (markerOffset == std::string_view::npos)
    {
        outError = "Texture artifact missing data marker";
        return false;
    }

    std::unordered_map<std::string, std::string> fields;
    std::istringstream metadata(std::string(
        text.substr(magic.size(), markerOffset - magic.size())));
    for (std::string line; std::getline(metadata, line);)
    {
        const size_t separator = line.find('=');
        if (separator != std::string::npos)
            fields[line.substr(0, separator)] = line.substr(separator + 1u);
    }

    const std::optional<uint32> width = ParseUint32Field(fields, "width");
    const std::optional<uint32> height = ParseUint32Field(fields, "height");
    const std::optional<uint32> depth = ParseUint32Field(fields, "depth");
    const std::optional<uint32> mipLevels = ParseUint32Field(fields, "mipLevels");
    const std::optional<uint32> arrayLayers = ParseUint32Field(fields, "arrayLayers");
    const std::optional<bool> srgb = ParseBoolField(fields, "srgb");
    const std::optional<uint32> dataSize = ParseUint32Field(fields, "dataSize");
    const auto formatField = fields.find("format");
    const auto usageField = fields.find("usage");
    const auto compressionField = fields.find("compression");
    if (!width || !height || !depth || !mipLevels || !arrayLayers ||
        !srgb || !dataSize || formatField == fields.end() ||
        usageField == fields.end() || compressionField == fields.end())
    {
        outError = "Texture artifact metadata is incomplete";
        return false;
    }
    if (*depth != 1u || *arrayLayers != 1u)
    {
        outError = "Texture artifact contains an unsupported depth or array layout";
        return false;
    }

    const std::optional<TextureFormat> format =
        ParseTextureFormat(formatField->second);
    const std::optional<TextureUsage> usage =
        ParseTextureUsage(usageField->second);
    if (!format || !usage)
    {
        outError = "Texture artifact contains unsupported format or usage";
        return false;
    }
    if ((*format == TextureFormat::RGBA8 &&
         compressionField->second != "UncompressedRGBA") ||
        (*format != TextureFormat::RGBA8 &&
         compressionField->second != GetCompressionName(*format)))
    {
        outError = "Texture artifact compression does not match its runtime format";
        return false;
    }

    const std::optional<size_t> expected =
        GetExpectedDataSize(*width, *height, *mipLevels, *format);
    if (!expected || *expected != *dataSize)
    {
        outError = "Texture artifact dataSize does not match format and mip layout";
        return false;
    }
    const size_t payloadOffset = markerOffset + dataMarker.size();
    if (*dataSize > bytes.size() - std::min(payloadOffset, bytes.size()))
    {
        outError = "Texture artifact data payload is truncated";
        return false;
    }
    const size_t endOffset = payloadOffset + *dataSize;
    if (endOffset > bytes.size() || bytes.size() - endOffset < endMarker.size() ||
        text.substr(endOffset, endMarker.size()) != endMarker ||
        endOffset + endMarker.size() != bytes.size())
    {
        outError = "Texture artifact has an invalid end marker or trailing bytes";
        return false;
    }

    outArtifact.metadata.width = *width;
    outArtifact.metadata.height = *height;
    outArtifact.metadata.depth = *depth;
    outArtifact.metadata.mipLevels = *mipLevels;
    outArtifact.metadata.arrayLayers = *arrayLayers;
    outArtifact.metadata.format = *format;
    outArtifact.metadata.usage = *usage;
    outArtifact.metadata.isSRGB = *srgb;
    outArtifact.pixels.assign(
        bytes.begin() + static_cast<std::ptrdiff_t>(payloadOffset),
        bytes.begin() + static_cast<std::ptrdiff_t>(endOffset));
    outError.clear();
    return true;
}
} // namespace RVX::Resource
