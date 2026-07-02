#pragma once

#include "Core/Types.h"

#include <filesystem>
#include <string>
#include <vector>

namespace RVX::Test
{
    enum class ImageChannelOrder : uint8
    {
        RGB,
        RGBA,
        BGRA
    };

    struct ImageData
    {
        uint32 width = 0;
        uint32 height = 0;
        uint32 bytesPerPixel = 3;
        std::vector<uint8> pixels;
    };

    bool LoadPPM(const std::filesystem::path& path, ImageData& outImage, std::string* outError = nullptr);

    bool SavePPM(const std::filesystem::path& path,
                 const uint8* pixels,
                 uint32 width,
                 uint32 height,
                 uint32 bytesPerPixel,
                 ImageChannelOrder channelOrder = ImageChannelOrder::RGB,
                 std::string* outError = nullptr);

    bool SaveDiffPPM(const std::filesystem::path& path,
                     const ImageData& expected,
                     const ImageData& actual,
                     std::string* outError = nullptr);
} // namespace RVX::Test
