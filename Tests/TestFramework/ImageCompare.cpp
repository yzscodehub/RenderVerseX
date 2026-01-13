#include "TestFramework/ImageCompare.h"
#include <cmath>
#include <algorithm>

namespace RVX::Test
{
    ImageCompareResult CompareImages(
        const void* imageA, uint32 widthA, uint32 heightA,
        const void* imageB, uint32 widthB, uint32 heightB,
        uint32 bytesPerPixel,
        float tolerance)
    {
        ImageCompareResult result;

        // Check dimensions
        if (widthA != widthB || heightA != heightB)
        {
            result.identical = false;
            result.differentPixels = widthA * heightA;
            return result;
        }

        const uint8* a = static_cast<const uint8*>(imageA);
        const uint8* b = static_cast<const uint8*>(imageB);

        uint64 totalPixels = static_cast<uint64>(widthA) * heightA;
        uint64 totalBytes = totalPixels * bytesPerPixel;

        double sumSquaredError = 0.0;
        result.differentPixels = 0;

        for (uint64 i = 0; i < totalBytes; ++i)
        {
            int diff = static_cast<int>(a[i]) - static_cast<int>(b[i]);
            sumSquaredError += diff * diff;

            if (i % bytesPerPixel == 0 && std::abs(diff) > static_cast<int>(tolerance * 255.0f))
            {
                result.differentPixels++;
            }
        }

        result.mse = static_cast<float>(sumSquaredError / totalBytes);
        result.psnr = (result.mse > 0.0f) ? 10.0f * std::log10(255.0f * 255.0f / result.mse) : 100.0f;
        result.identical = (result.differentPixels == 0);

        return result;
    }

} // namespace RVX::Test
