#include "Resource/Loader/HDRTextureLoader.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

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

    float DecodeHalf(uint16_t half)
    {
        const uint32 sign = static_cast<uint32>(half & 0x8000u) << 16;
        const uint32 exp = (half >> 10) & 0x1Fu;
        const uint32 mant = half & 0x03FFu;

        uint32 bits = 0;
        if (exp == 0)
        {
            if (mant == 0)
            {
                bits = sign;
            }
            else
            {
                uint32 normalizedMant = mant;
                int32 exponent = -14;
                while ((normalizedMant & 0x0400u) == 0)
                {
                    normalizedMant <<= 1;
                    --exponent;
                }
                normalizedMant &= 0x03FFu;
                bits = sign |
                       (static_cast<uint32>(exponent + 127) << 23) |
                       (normalizedMant << 13);
            }
        }
        else if (exp == 31)
        {
            bits = sign | 0x7F800000u | (mant << 13);
        }
        else
        {
            bits = sign |
                   ((exp - 15u + 127u) << 23) |
                   (mant << 13);
        }

        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    std::array<float, 4> DecodeBRDFPixel(const TextureResource& texture, uint32 x, uint32 y)
    {
        const std::vector<uint8_t>& data = texture.GetData();
        const uint32 width = texture.GetWidth();
        const size_t offset = (static_cast<size_t>(y) * width + x) * 4u * sizeof(uint16_t);
        EXPECT_LE(offset + 4u * sizeof(uint16_t), data.size());

        const auto* halfData = reinterpret_cast<const uint16_t*>(data.data() + offset);
        return {
            DecodeHalf(halfData[0]),
            DecodeHalf(halfData[1]),
            DecodeHalf(halfData[2]),
            DecodeHalf(halfData[3]),
        };
    }

    Vec3 ImportanceSampleGGXReference(float xiX, float xiY, float roughness)
    {
        const float a = roughness * roughness;
        const float phi = 2.0f * 3.14159265358979323846f * xiX;
        const float cosTheta = std::sqrt((1.0f - xiY) / (1.0f + (a * a - 1.0f) * xiY));
        const float sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
        const Vec3 h(std::cos(phi) * sinTheta, std::sin(phi) * sinTheta, cosTheta);

        const Vec3 n(0.0f, 0.0f, 1.0f);
        const Vec3 up = Vec3(1.0f, 0.0f, 0.0f);
        const Vec3 tangent = glm::normalize(glm::cross(up, n));
        const Vec3 bitangent = glm::cross(n, tangent);
        return glm::normalize(tangent * h.x + bitangent * h.y + n * h.z);
    }

    float RadicalInverseReference(uint32 bits)
    {
        bits = (bits << 16u) | (bits >> 16u);
        bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
        bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
        bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
        bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
        return static_cast<float>(bits) * 2.3283064365386963e-10f;
    }

    float GeometrySchlickGGXReference(float nDot, float roughness)
    {
        const float alpha = roughness * roughness;
        const float k = alpha * 0.5f;
        return nDot / std::max(nDot * (1.0f - k) + k, 1.0e-6f);
    }

    std::array<float, 2> IntegrateBRDFReference(uint32 resolution, uint32 samples, uint32 x, uint32 y)
    {
        const float nDotV = static_cast<float>(x + 1u) / static_cast<float>(resolution);
        const float roughness = static_cast<float>(y + 1u) / static_cast<float>(resolution);
        const Vec3 V(std::sqrt(std::max(0.0f, 1.0f - nDotV * nDotV)), 0.0f, nDotV);

        float a = 0.0f;
        float b = 0.0f;
        const uint32 sampleCount = std::max(1u, samples);
        for (uint32 i = 0; i < sampleCount; ++i)
        {
            const float xiX = static_cast<float>(i) / static_cast<float>(sampleCount);
            const float xiY = RadicalInverseReference(i);
            const Vec3 H = ImportanceSampleGGXReference(xiX, xiY, roughness);
            const Vec3 L = glm::normalize(2.0f * glm::dot(V, H) * H - V);

            const float nDotL = std::max(L.z, 0.0f);
            const float nDotH = std::max(H.z, 0.0f);
            const float vDotH = std::max(glm::dot(V, H), 0.0f);
            if (nDotL > 0.0f)
            {
                const float g = GeometrySchlickGGXReference(nDotV, roughness) *
                                GeometrySchlickGGXReference(nDotL, roughness);
                const float gVis = (g * vDotH) / std::max(nDotH * nDotV, 1.0e-6f);
                const float fc = std::pow(1.0f - vDotH, 5.0f);
                a += (1.0f - fc) * gVis;
                b += fc * gVis;
            }
        }

        return {a / static_cast<float>(sampleCount), b / static_cast<float>(sampleCount)};
    }

    std::array<float, 2> IntegrateBRDFSimplifiedReference(uint32 resolution, uint32 samples, uint32 x, uint32 y)
    {
        const float nDotV = static_cast<float>(x + 1u) / static_cast<float>(resolution);
        const float roughness = static_cast<float>(y + 1u) / static_cast<float>(resolution);
        const Vec3 V(std::sqrt(std::max(0.0f, 1.0f - nDotV * nDotV)), 0.0f, nDotV);

        float a = 0.0f;
        float b = 0.0f;
        const uint32 sampleCount = std::max(1u, samples);
        for (uint32 i = 0; i < sampleCount; ++i)
        {
            const float xiX = static_cast<float>(i) / static_cast<float>(sampleCount);
            const float xiY = RadicalInverseReference(i);
            const Vec3 H = ImportanceSampleGGXReference(xiX, xiY, roughness);
            const Vec3 L = glm::normalize(2.0f * glm::dot(V, H) * H - V);

            const float nDotL = std::max(L.z, 0.0f);
            const float nDotH = std::max(H.z, 0.0f);
            const float vDotH = std::max(glm::dot(V, H), 0.0f);
            if (nDotL > 0.0f)
            {
                const float g = nDotL * nDotV;
                const float gVis = (g * vDotH) / (nDotH * nDotV + 0.0001f);
                const float fc = std::pow(1.0f - vDotH, 5.0f);
                a += (1.0f - fc) * gVis;
                b += fc * gVis;
            }
        }

        return {a / static_cast<float>(sampleCount), b / static_cast<float>(sampleCount)};
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

TEST(HDRTextureLoaderValidation, BRDFLUTDataChannelsAreFiniteAndPacked)
{
    HDRTextureLoader loader(nullptr);
    TextureResource* brdfLUT = loader.GenerateBRDFLUT(8, 16);

    ASSERT_NE(nullptr, brdfLUT);
    const std::vector<uint8_t>& data = brdfLUT->GetData();
    ASSERT_EQ(data.size(), static_cast<size_t>(8u * 8u * 4u * sizeof(uint16_t)));

    for (uint32 y = 0; y < brdfLUT->GetHeight(); ++y)
    {
        for (uint32 x = 0; x < brdfLUT->GetWidth(); ++x)
        {
            const std::array<float, 4> pixel = DecodeBRDFPixel(*brdfLUT, x, y);
            EXPECT_TRUE(std::isfinite(pixel[0]));
            EXPECT_TRUE(std::isfinite(pixel[1]));
            EXPECT_GE(pixel[0], 0.0f);
            EXPECT_GE(pixel[1], 0.0f);
            EXPECT_NEAR(pixel[2], 0.0f, 0.0001f);
            EXPECT_NEAR(pixel[3], 1.0f, 0.0001f);
        }
    }

    delete brdfLUT;
}

TEST(HDRTextureLoaderValidation, BRDFLUTMatchesSmithGGXSplitSumReference)
{
    constexpr uint32 kResolution = 16;
    constexpr uint32 kSamples = 64;
    HDRTextureLoader loader(nullptr);
    TextureResource* brdfLUT = loader.GenerateBRDFLUT(kResolution, kSamples);

    ASSERT_NE(nullptr, brdfLUT);

    struct Sample
    {
        uint32 x;
        uint32 y;
    };

    const Sample samples[] = {
        {0, 0},   // grazing, low roughness: strongly separates Smith GGX from the old simplified term
        {1, 2},
        {5, 4},
        {10, 8},
        {15, 15},
    };

    bool oldSimplifiedWouldDiffer = false;
    for (const Sample& sample : samples)
    {
        const std::array<float, 4> actual = DecodeBRDFPixel(*brdfLUT, sample.x, sample.y);
        const std::array<float, 2> expected =
            IntegrateBRDFReference(kResolution, kSamples, sample.x, sample.y);
        const std::array<float, 2> oldSimplified =
            IntegrateBRDFSimplifiedReference(kResolution, kSamples, sample.x, sample.y);

        EXPECT_NEAR(actual[0], expected[0], 0.0025f)
            << "BRDF LUT A mismatch at " << sample.x << "," << sample.y;
        EXPECT_NEAR(actual[1], expected[1], 0.0025f)
            << "BRDF LUT B mismatch at " << sample.x << "," << sample.y;

        if (std::abs(oldSimplified[0] - expected[0]) > 0.01f ||
            std::abs(oldSimplified[1] - expected[1]) > 0.01f)
        {
            oldSimplifiedWouldDiffer = true;
        }
    }

    EXPECT_TRUE(oldSimplifiedWouldDiffer)
        << "Reference samples should prove the old simplified visibility term would fail";

    delete brdfLUT;
}
