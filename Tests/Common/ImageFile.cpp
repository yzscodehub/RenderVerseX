#include "Common/ImageFile.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <limits>

namespace RVX::Test
{
    namespace
    {
        void SetError(std::string* outError, const std::string& error)
        {
            if (outError)
            {
                *outError = error;
            }
        }

        bool ReadPPMToken(std::istream& stream, std::string& outToken)
        {
            outToken.clear();

            char ch = '\0';
            while (stream.get(ch))
            {
                if (std::isspace(static_cast<unsigned char>(ch)))
                {
                    continue;
                }

                if (ch == '#')
                {
                    stream.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                    continue;
                }

                outToken.push_back(ch);
                break;
            }

            while (stream.get(ch))
            {
                if (std::isspace(static_cast<unsigned char>(ch)))
                {
                    break;
                }

                if (ch == '#')
                {
                    stream.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                    break;
                }

                outToken.push_back(ch);
            }

            return !outToken.empty();
        }

        bool ParseUInt(const std::string& token, uint32& outValue)
        {
            try
            {
                const unsigned long parsed = std::stoul(token);
                if (parsed > std::numeric_limits<uint32>::max())
                {
                    return false;
                }
                outValue = static_cast<uint32>(parsed);
                return true;
            }
            catch (...)
            {
                return false;
            }
        }
    } // namespace

    bool LoadPPM(const std::filesystem::path& path, ImageData& outImage, std::string* outError)
    {
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
        {
            SetError(outError, "Failed to open PPM file: " + path.string());
            return false;
        }

        std::string token;
        if (!ReadPPMToken(stream, token) || token != "P6")
        {
            SetError(outError, "Unsupported PPM format: expected P6");
            return false;
        }

        uint32 width = 0;
        uint32 height = 0;
        uint32 maxValue = 0;

        if (!ReadPPMToken(stream, token) || !ParseUInt(token, width) ||
            !ReadPPMToken(stream, token) || !ParseUInt(token, height) ||
            !ReadPPMToken(stream, token) || !ParseUInt(token, maxValue))
        {
            SetError(outError, "Invalid PPM header: " + path.string());
            return false;
        }

        if (width == 0 || height == 0 || maxValue != 255)
        {
            SetError(outError, "Unsupported PPM dimensions or max value: " + path.string());
            return false;
        }

        const uint64 byteCount = static_cast<uint64>(width) * height * 3;
        outImage.width = width;
        outImage.height = height;
        outImage.bytesPerPixel = 3;
        outImage.pixels.resize(static_cast<size_t>(byteCount));

        stream.read(reinterpret_cast<char*>(outImage.pixels.data()), static_cast<std::streamsize>(byteCount));
        if (stream.gcount() != static_cast<std::streamsize>(byteCount))
        {
            SetError(outError, "Unexpected end of PPM data: " + path.string());
            return false;
        }

        return true;
    }

    bool SavePPM(const std::filesystem::path& path,
                 const uint8* pixels,
                 uint32 width,
                 uint32 height,
                 uint32 bytesPerPixel,
                 ImageChannelOrder channelOrder,
                 std::string* outError)
    {
        if (!pixels || width == 0 || height == 0 || bytesPerPixel < 3)
        {
            SetError(outError, "Invalid image data for PPM save");
            return false;
        }

        const std::filesystem::path parent = path.parent_path();
        if (!parent.empty())
        {
            std::filesystem::create_directories(parent);
        }

        std::ofstream stream(path, std::ios::binary);
        if (!stream)
        {
            SetError(outError, "Failed to create PPM file: " + path.string());
            return false;
        }

        stream << "P6\n" << width << " " << height << "\n255\n";

        for (uint32 y = 0; y < height; ++y)
        {
            for (uint32 x = 0; x < width; ++x)
            {
                const uint8* pixel = pixels + (static_cast<uint64>(y) * width + x) * bytesPerPixel;
                uint8 rgb[3] = {};

                switch (channelOrder)
                {
                    case ImageChannelOrder::RGB:
                    case ImageChannelOrder::RGBA:
                        rgb[0] = pixel[0];
                        rgb[1] = pixel[1];
                        rgb[2] = pixel[2];
                        break;
                    case ImageChannelOrder::BGRA:
                        rgb[0] = pixel[2];
                        rgb[1] = pixel[1];
                        rgb[2] = pixel[0];
                        break;
                }

                stream.write(reinterpret_cast<const char*>(rgb), sizeof(rgb));
            }
        }

        if (!stream)
        {
            SetError(outError, "Failed while writing PPM file: " + path.string());
            return false;
        }

        return true;
    }

    bool SaveDiffPPM(const std::filesystem::path& path,
                     const ImageData& expected,
                     const ImageData& actual,
                     std::string* outError)
    {
        if (expected.width != actual.width ||
            expected.height != actual.height ||
            expected.bytesPerPixel != 3 ||
            actual.bytesPerPixel != 3 ||
            expected.pixels.size() != actual.pixels.size())
        {
            SetError(outError, "Cannot create diff for incompatible images");
            return false;
        }

        std::vector<uint8> diff(expected.pixels.size(), 0);
        for (size_t i = 0; i < diff.size(); ++i)
        {
            const int delta = std::abs(static_cast<int>(expected.pixels[i]) - static_cast<int>(actual.pixels[i]));
            diff[i] = static_cast<uint8>(std::min(delta * 4, 255));
        }

        return SavePPM(path, diff.data(), expected.width, expected.height, 3, ImageChannelOrder::RGB, outError);
    }
} // namespace RVX::Test
