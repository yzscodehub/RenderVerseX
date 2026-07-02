#pragma once

#include "Core/Types.h"

namespace RVX::Test
{
    struct ImageCompareResult
    {
        bool identical = false;
        float mse = 0.0f;
        float psnr = 0.0f;
        uint32 differentPixels = 0;
    };

    ImageCompareResult CompareImages(
        const void* imageA, uint32 widthA, uint32 heightA,
        const void* imageB, uint32 widthB, uint32 heightB,
        uint32 bytesPerPixel = 4,
        float tolerance = 0.0f);
} // namespace RVX::Test
