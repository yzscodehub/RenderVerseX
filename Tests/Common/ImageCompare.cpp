#include "Common/ImageCompare.h"

#include <algorithm>
#include <cmath>

namespace RVX::Test
{
    ImageCompareResult CompareImages(
        const void* imageA, uint32 widthA, uint32 heightA,
        const void* imageB, uint32 widthB, uint32 heightB,
        uint32 bytesPerPixel,
        float tolerance)
    {
        ImageCompareResult result;

        if (widthA != widthB || heightA != heightB)
        {
            result.identical = false;
            result.differentPixels = widthA * heightA;
            return result;
        }

        const uint8* a = static_cast<const uint8*>(imageA);
        const uint8* b = static_cast<const uint8*>(imageB);

        const uint64 totalPixels = static_cast<uint64>(widthA) * heightA;
        const uint64 totalBytes = totalPixels * bytesPerPixel;

        double sumSquaredError = 0.0;
        result.differentPixels = 0;

        const int toleranceByte = static_cast<int>(tolerance * 255.0f);

        for (uint64 pixelIndex = 0; pixelIndex < totalPixels; ++pixelIndex)
        {
            bool pixelDifferent = false;
            const uint64 pixelOffset = pixelIndex * bytesPerPixel;

            for (uint32 channel = 0; channel < bytesPerPixel; ++channel)
            {
                const uint64 byteIndex = pixelOffset + channel;
                const int diff = static_cast<int>(a[byteIndex]) - static_cast<int>(b[byteIndex]);
                sumSquaredError += diff * diff;

                if (std::abs(diff) > toleranceByte)
                {
                    pixelDifferent = true;
                }
            }

            if (pixelDifferent)
            {
                ++result.differentPixels;
            }
        }

        result.mse = static_cast<float>(sumSquaredError / totalBytes);
        result.psnr = (result.mse > 0.0f) ? 10.0f * std::log10(255.0f * 255.0f / result.mse) : 100.0f;
        result.identical = (result.differentPixels == 0);

        return result;
    }
} // namespace RVX::Test
