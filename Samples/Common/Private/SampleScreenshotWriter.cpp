/** @file SampleScreenshotWriter.cpp @brief Shared PPM capture writer. */

#include "Samples/SampleScreenshotWriter.h"

#include <fstream>
#include <system_error>
#include <utility>

namespace RVX
{
    namespace
    {
        void SetError(std::string* outError, std::string error)
        {
            if (outError)
            {
                *outError = std::move(error);
            }
        }
    } // namespace

    bool WriteSampleScreenshotPPM(const RenderFrameCaptureResult& capture,
                                  const std::filesystem::path& path,
                                  std::string* outError)
    {
        const bool rgba = capture.format == RHIFormat::RGBA8_UNORM ||
                          capture.format == RHIFormat::RGBA8_UNORM_SRGB;
        const bool bgra = capture.format == RHIFormat::BGRA8_UNORM ||
                          capture.format == RHIFormat::BGRA8_UNORM_SRGB;
        const uint64 minimumRowPitch = static_cast<uint64>(capture.width) * 4u;
        const uint64 requiredBytes =
            static_cast<uint64>(capture.rowPitch) * capture.height;
        if (!capture.IsComplete() || capture.bytesPerPixel != 4 ||
            (!rgba && !bgra) || capture.rowPitch < minimumRowPitch ||
            capture.bytes.size() < requiredBytes)
        {
            SetError(outError,
                     "Capture is incomplete or is not a supported RGBA/BGRA color image");
            return false;
        }
        if (path.empty())
        {
            SetError(outError, "Screenshot path must not be empty");
            return false;
        }

        std::error_code error;
        const std::filesystem::path parent = path.parent_path();
        if (!parent.empty())
        {
            std::filesystem::create_directories(parent, error);
            if (error)
            {
                SetError(outError,
                         "Failed to create screenshot directory: " + error.message());
                return false;
            }
        }

        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        if (!stream)
        {
            SetError(outError, "Failed to open screenshot: " + path.string());
            return false;
        }

        stream << "P6\n" << capture.width << " " << capture.height << "\n255\n";
        for (uint32 y = 0; y < capture.height; ++y)
        {
            const uint32 sourceY = capture.originBottomLeft
                                       ? capture.height - 1u - y
                                       : y;
            const uint8* row = capture.bytes.data() +
                               static_cast<uint64>(sourceY) * capture.rowPitch;
            for (uint32 x = 0; x < capture.width; ++x)
            {
                const uint8* pixel = row +
                                     static_cast<uint64>(x) * capture.bytesPerPixel;
                const uint8 rgb[3] = {
                    bgra ? pixel[2] : pixel[0],
                    pixel[1],
                    bgra ? pixel[0] : pixel[2],
                };
                stream.write(reinterpret_cast<const char*>(rgb), sizeof(rgb));
            }
        }

        if (!stream)
        {
            SetError(outError, "Failed while writing screenshot: " + path.string());
            return false;
        }
        return true;
    }
} // namespace RVX
