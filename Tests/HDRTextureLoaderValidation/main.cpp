#include "Resource/Loader/HDRTextureLoader.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>

using namespace RVX;
using namespace RVX::Resource;

namespace
{
    CubemapFaces MakeNonUniformCubemap(uint32 size)
    {
        CubemapFaces faces;
        faces.faceSize = size;

        for (int face = 0; face < CubemapFaces::FACE_COUNT; ++face)
        {
            std::vector<float>& data = faces.faces[face];
            data.resize(static_cast<size_t>(size) * static_cast<size_t>(size) * 4u);

            for (uint32 y = 0; y < size; ++y)
            {
                for (uint32 x = 0; x < size; ++x)
                {
                    const size_t offset = (static_cast<size_t>(y) * size + x) * 4u;
                    data[offset + 0] = 0.1f + static_cast<float>(face) * 0.3f + static_cast<float>(x) * 0.11f;
                    data[offset + 1] = 0.2f + static_cast<float>(face) * 0.17f + static_cast<float>(y) * 0.13f;
                    data[offset + 2] = 0.3f + static_cast<float>(face) * 0.07f + static_cast<float>(x + y) * 0.05f;
                    data[offset + 3] = 1.0f;
                }
            }
        }

        return faces;
    }

    void ExpectFiniteCubemap(const CubemapFaces& faces)
    {
        EXPECT_GT(faces.faceSize, 0u);
        for (const std::vector<float>& faceData : faces.faces)
        {
            ASSERT_FALSE(faceData.empty());
            for (float value : faceData)
            {
                EXPECT_TRUE(std::isfinite(value));
            }
        }
    }

    void ExpectFiniteMipChain(const std::vector<CubemapFaces>& mipChain)
    {
        ASSERT_FALSE(mipChain.empty());
        for (const CubemapFaces& faces : mipChain)
        {
            ExpectFiniteCubemap(faces);
        }
    }

    bool CubemapDiffers(const CubemapFaces& a, const CubemapFaces& b)
    {
        if (a.faceSize != b.faceSize)
            return true;

        for (int face = 0; face < CubemapFaces::FACE_COUNT; ++face)
        {
            const std::vector<float>& left = a.faces[face];
            const std::vector<float>& right = b.faces[face];
            if (left.size() != right.size())
                return true;

            for (size_t i = 0; i < left.size(); ++i)
            {
                if (std::abs(left[i] - right[i]) > 1.0e-5f)
                    return true;
            }
        }

        return false;
    }
} // namespace

TEST(HDRTextureLoaderValidation, IrradianceSampleCountChangesNonUniformOutput)
{
    HDRTextureLoader loader(nullptr);
    const CubemapFaces envMap = MakeNonUniformCubemap(4);

    const CubemapFaces oneSample = loader.GenerateIrradianceMap(envMap, 2, 1);
    const CubemapFaces eightSamples = loader.GenerateIrradianceMap(envMap, 2, 8);

    ExpectFiniteCubemap(oneSample);
    ExpectFiniteCubemap(eightSamples);
    EXPECT_TRUE(CubemapDiffers(oneSample, eightSamples));
}

TEST(HDRTextureLoaderValidation, IrradianceZeroSamplesClampToOneSample)
{
    HDRTextureLoader loader(nullptr);
    const CubemapFaces envMap = MakeNonUniformCubemap(4);

    const CubemapFaces zeroSamples = loader.GenerateIrradianceMap(envMap, 2, 0);
    const CubemapFaces oneSample = loader.GenerateIrradianceMap(envMap, 2, 1);

    ExpectFiniteCubemap(zeroSamples);
    ExpectFiniteCubemap(oneSample);
    EXPECT_FALSE(CubemapDiffers(zeroSamples, oneSample));
}

TEST(HDRTextureLoaderValidation, PrefilteredMapHandlesZeroSamplesAndOneMip)
{
    HDRTextureLoader loader(nullptr);
    const CubemapFaces envMap = MakeNonUniformCubemap(4);

    const std::vector<CubemapFaces> mipChain = loader.GeneratePrefilteredMap(envMap, 4, 1, 0);

    ASSERT_EQ(1u, mipChain.size());
    EXPECT_EQ(4u, mipChain[0].faceSize);
    ExpectFiniteMipChain(mipChain);
}

TEST(HDRTextureLoaderValidation, PrefilteredMapReturnsRequestedFiniteMipCount)
{
    HDRTextureLoader loader(nullptr);
    const CubemapFaces envMap = MakeNonUniformCubemap(4);

    const std::vector<CubemapFaces> mipChain = loader.GeneratePrefilteredMap(envMap, 4, 3, 1);

    ASSERT_EQ(3u, mipChain.size());
    EXPECT_EQ(4u, mipChain[0].faceSize);
    EXPECT_EQ(2u, mipChain[1].faceSize);
    EXPECT_EQ(1u, mipChain[2].faceSize);
    ExpectFiniteMipChain(mipChain);
}

TEST(HDRTextureLoaderValidation, BRDFLUTClampsZeroSamplesAndKeepsMetadata)
{
    HDRTextureLoader loader(nullptr);
    TextureResource* brdfLUT = loader.GenerateBRDFLUT(4, 0);

    ASSERT_NE(nullptr, brdfLUT);
    EXPECT_EQ(TextureFormat::RGBA16F, brdfLUT->GetFormat());
    EXPECT_EQ(4u, brdfLUT->GetWidth());
    EXPECT_EQ(4u, brdfLUT->GetHeight());
    EXPECT_EQ(1u, brdfLUT->GetMipLevels());
    EXPECT_FALSE(brdfLUT->IsSRGB());
    EXPECT_FALSE(brdfLUT->GetData().empty());

    delete brdfLUT;
}
