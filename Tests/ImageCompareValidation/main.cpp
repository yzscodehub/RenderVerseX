#include "Common/ImageCompare.h"

#include <gtest/gtest.h>

namespace
{
    TEST(ImageCompareValidation, CountsNonFirstChannelDifference)
    {
        const RVX::uint8 a[] = {10, 20, 30, 40};
        const RVX::uint8 b[] = {10, 25, 30, 40};

        const RVX::Test::ImageCompareResult result =
            RVX::Test::CompareImages(a, 1, 1, b, 1, 1, 4, 0.0f);

        EXPECT_FALSE(result.identical);
        EXPECT_EQ(result.differentPixels, 1u);
        EXPECT_GT(result.mse, 0.0f);
    }

    TEST(ImageCompareValidation, CountsOnePixelOnceForMultipleChannelDifferences)
    {
        const RVX::uint8 a[] = {10, 20, 30, 40};
        const RVX::uint8 b[] = {11, 25, 30, 44};

        const RVX::Test::ImageCompareResult result =
            RVX::Test::CompareImages(a, 1, 1, b, 1, 1, 4, 0.0f);

        EXPECT_FALSE(result.identical);
        EXPECT_EQ(result.differentPixels, 1u);
    }

    TEST(ImageCompareValidation, ToleranceSuppressesSmallChannelDifference)
    {
        const RVX::uint8 a[] = {10, 20, 30, 40};
        const RVX::uint8 b[] = {10, 25, 30, 40};

        const RVX::Test::ImageCompareResult result =
            RVX::Test::CompareImages(a, 1, 1, b, 1, 1, 4, 0.03f);

        EXPECT_TRUE(result.identical);
        EXPECT_EQ(result.differentPixels, 0u);
        EXPECT_GT(result.mse, 0.0f);
    }

    TEST(ImageCompareValidation, SizeMismatchReportsSourcePixelsDifferent)
    {
        const RVX::uint8 a[] = {0, 0, 0, 255, 255, 255, 255, 255};
        const RVX::uint8 b[] = {0, 0, 0, 255};

        const RVX::Test::ImageCompareResult result =
            RVX::Test::CompareImages(a, 2, 1, b, 1, 1, 4, 0.0f);

        EXPECT_FALSE(result.identical);
        EXPECT_EQ(result.differentPixels, 2u);
    }
} // namespace
