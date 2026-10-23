#include "Resource/Cooked/CookedMaterialArtifact.h"

#include <gtest/gtest.h>

#include <limits>
#include <string>
#include <vector>

namespace
{
    RVX::Resource::CookedMaterialTextureBinding MakeBinding(
        RVX::MaterialTextureSlot slot,
        std::string path)
    {
        RVX::Resource::CookedMaterialTextureBinding binding;
        binding.definition.slot = slot;
        binding.artifactPath = std::move(path);
        binding.expectedContentHash = RVX::Hash::ComputeSHA256(
            binding.artifactPath.data(), binding.artifactPath.size());
        switch (slot)
        {
            case RVX::MaterialTextureSlot::BaseColor:
            case RVX::MaterialTextureSlot::ToonRamp:
            case RVX::MaterialTextureSlot::SpecularColor:
                binding.usage = RVX::Resource::TextureUsage::Color;
                binding.isSRGB = true;
                break;
            case RVX::MaterialTextureSlot::Emissive:
                binding.usage = RVX::Resource::TextureUsage::Color;
                binding.isSRGB = true;
                binding.fallbackSemantic =
                    RVX::Resource::TextureFallbackSemantic::Black;
                break;
            case RVX::MaterialTextureSlot::Normal:
            case RVX::MaterialTextureSlot::ClearcoatNormal:
                binding.usage = RVX::Resource::TextureUsage::Normal;
                binding.isSRGB = false;
                binding.fallbackSemantic =
                    RVX::Resource::TextureFallbackSemantic::FlatNormal;
                break;
            default:
                binding.usage = RVX::Resource::TextureUsage::Data;
                binding.isSRGB = false;
                break;
        }
        return binding;
    }

    RVX::Resource::CookedMaterialArtifact MakeArtifact()
    {
        RVX::Resource::CookedMaterialArtifact artifact;
        artifact.logicalMaterialId = "Materials/PolishedMetal";
        artifact.material.descriptor.featureMask = 0x3u;
        artifact.material.parameters.textureFlags = 0x3u;
        artifact.material.parameters.baseColorFactor =
            {0.5f, 0.6f, 0.7f, 1.0f};
        artifact.material.parameters.metallicFactor = 0.9f;
        artifact.material.parameters.roughnessFactor = 0.2f;
        artifact.permutation = 17;
        artifact.textureBindings.push_back(MakeBinding(
            RVX::MaterialTextureSlot::Normal, "textures/normal.rva"));
        artifact.textureBindings.push_back(MakeBinding(
            RVX::MaterialTextureSlot::BaseColor, "textures/albedo.rva"));
        std::string error;
        EXPECT_TRUE(RVX::Resource::FinalizeCookedMaterialArtifact(
            artifact, error)) << error;
        return artifact;
    }
} // namespace

TEST(CookedMaterialArtifactValidation, RoundTripIsDeterministicAndCanonical)
{
    RVX::Resource::CookedMaterialArtifact artifact = MakeArtifact();
    ASSERT_EQ(artifact.textureBindings.size(), 2u);
    EXPECT_EQ(artifact.textureBindings[0].definition.slot,
              RVX::MaterialTextureSlot::BaseColor);

    std::vector<RVX::uint8> first;
    std::vector<RVX::uint8> second;
    std::string error;
    ASSERT_TRUE(RVX::Resource::SerializeCookedMaterialArtifact(
        artifact, first, error)) << error;
    ASSERT_TRUE(RVX::Resource::SerializeCookedMaterialArtifact(
        artifact, second, error)) << error;
    EXPECT_EQ(first, second);

    RVX::Resource::CookedMaterialArtifact decoded;
    ASSERT_TRUE(RVX::Resource::DeserializeCookedMaterialArtifact(
        first, decoded, error)) << error;
    EXPECT_EQ(decoded.logicalMaterialId, artifact.logicalMaterialId);
    EXPECT_EQ(decoded.permutation, artifact.permutation);
    EXPECT_EQ(decoded.permutation, RVX::Resource::GetCookedMaterialPermutation(
        decoded.material.descriptor));
    EXPECT_EQ(decoded.dependencyHash, artifact.dependencyHash);
    EXPECT_NE(decoded.contentHash, RVX::Hash::SHA256Digest{});
    ASSERT_EQ(decoded.textureBindings.size(), 2u);
    EXPECT_EQ(decoded.textureBindings[1].artifactPath, "textures/normal.rva");
    EXPECT_EQ(decoded.textureBindings[1].expectedContentHash,
              artifact.textureBindings[1].expectedContentHash);
    EXPECT_EQ(decoded.material.descriptor.parameterLayoutHash,
              RVX::GetMaterialParameterLayoutHashV2());
}

TEST(CookedMaterialArtifactValidation,
     V2SpecularAndClearcoatParametersAndSlotsRoundTripDeterministically)
{
    auto artifact = MakeArtifact();
    artifact.material.parameters.specularFactor = 0.75f;
    artifact.material.parameters.specularColor = {1.25f, 0.8f, 0.6f};
    artifact.material.parameters.clearcoatFactor = 0.9f;
    artifact.material.parameters.clearcoatRoughness = 0.15f;
    artifact.material.parameters.clearcoatNormalScale = 0.6f;
    artifact.textureBindings.push_back(MakeBinding(
        RVX::MaterialTextureSlot::Specular, "textures/specular.rva"));
    artifact.textureBindings.push_back(MakeBinding(
        RVX::MaterialTextureSlot::SpecularColor,
        "textures/specular_color.rva"));
    artifact.textureBindings.push_back(MakeBinding(
        RVX::MaterialTextureSlot::Clearcoat, "textures/clearcoat.rva"));
    artifact.textureBindings.push_back(MakeBinding(
        RVX::MaterialTextureSlot::ClearcoatRoughness,
        "textures/clearcoat_roughness.rva"));
    artifact.textureBindings.push_back(MakeBinding(
        RVX::MaterialTextureSlot::ClearcoatNormal,
        "textures/clearcoat_normal.rva"));

    std::string error;
    ASSERT_TRUE(RVX::Resource::FinalizeCookedMaterialArtifact(artifact, error))
        << error;
    EXPECT_NE(artifact.material.descriptor.featureMask &
                  RVX::RVX_MATERIAL_FEATURE_SPECULAR,
              0u);
    EXPECT_NE(artifact.material.descriptor.featureMask &
                  RVX::RVX_MATERIAL_FEATURE_CLEARCOAT,
              0u);
    EXPECT_EQ(artifact.material.descriptor.parameterLayoutHash,
              RVX::GetMaterialParameterLayoutHashV2());

    std::vector<RVX::uint8> first;
    std::vector<RVX::uint8> second;
    ASSERT_TRUE(RVX::Resource::SerializeCookedMaterialArtifact(
        artifact, first, error)) << error;
    ASSERT_TRUE(RVX::Resource::SerializeCookedMaterialArtifact(
        artifact, second, error)) << error;
    EXPECT_EQ(first, second);

    RVX::Resource::CookedMaterialArtifact decoded;
    ASSERT_TRUE(RVX::Resource::DeserializeCookedMaterialArtifact(
        first, decoded, error)) << error;
    EXPECT_FLOAT_EQ(decoded.material.parameters.specularFactor, 0.75f);
    EXPECT_EQ(decoded.material.parameters.specularColor,
              (RVX::Vec3{1.25f, 0.8f, 0.6f}));
    EXPECT_FLOAT_EQ(decoded.material.parameters.clearcoatFactor, 0.9f);
    EXPECT_FLOAT_EQ(decoded.material.parameters.clearcoatRoughness, 0.15f);
    EXPECT_FLOAT_EQ(decoded.material.parameters.clearcoatNormalScale, 0.6f);
    EXPECT_EQ(decoded.textureBindings.size(), 7u);
}

TEST(CookedMaterialArtifactValidation,
     ToonParametersRoundTripWithoutStandardLitFallback)
{
    auto artifact = MakeArtifact();
    artifact.logicalMaterialId = "Materials/ToonRoundTrip";
    artifact.material.descriptor.shadingModel = RVX::MaterialShadingModel::ToonLit;
    artifact.material.descriptor.programFamily = std::string(
        RVX::GetMaterialProgramFamily(RVX::MaterialShadingModel::ToonLit));
    artifact.material.parameters.toonBands = 7.0f;
    artifact.material.parameters.toonSpecularThreshold = 0.58f;
    artifact.material.parameters.toonRimStrength = 1.75f;
    artifact.material.parameters.toonOutlineWidth = 2.25f;
    artifact.material.parameters.toonShadowTint = {0.1f, 0.3f, 0.9f};

    std::string error;
    ASSERT_TRUE(RVX::Resource::FinalizeCookedMaterialArtifact(artifact, error))
        << error;
    std::vector<RVX::uint8> bytes;
    ASSERT_TRUE(RVX::Resource::SerializeCookedMaterialArtifact(
        artifact, bytes, error)) << error;

    RVX::Resource::CookedMaterialArtifact decoded;
    ASSERT_TRUE(RVX::Resource::DeserializeCookedMaterialArtifact(
        bytes, decoded, error)) << error;
    EXPECT_EQ(decoded.material.descriptor.shadingModel,
              RVX::MaterialShadingModel::ToonLit);
    EXPECT_EQ(decoded.material.descriptor.programFamily, "toon-lit-v2");
    EXPECT_FLOAT_EQ(decoded.material.parameters.toonBands, 7.0f);
    EXPECT_FLOAT_EQ(decoded.material.parameters.toonSpecularThreshold, 0.58f);
    EXPECT_FLOAT_EQ(decoded.material.parameters.toonRimStrength, 1.75f);
    EXPECT_FLOAT_EQ(decoded.material.parameters.toonOutlineWidth, 2.25f);
    EXPECT_EQ(decoded.material.parameters.toonShadowTint,
              (RVX::Vec3{0.1f, 0.3f, 0.9f}));
}

TEST(CookedMaterialArtifactValidation, InvalidContractFailsClosed)
{
    std::string error;
    std::vector<RVX::uint8> bytes;

    {
        auto artifact = MakeArtifact();
        artifact.material.descriptor.parameterLayoutHash ^= 1u;
        EXPECT_FALSE(RVX::Resource::SerializeCookedMaterialArtifact(
            artifact, bytes, error));
        EXPECT_NE(error.find("layout"), std::string::npos);
    }
    {
        auto artifact = MakeArtifact();
        artifact.dependencyHash[0] ^= 1u;
        EXPECT_FALSE(RVX::Resource::SerializeCookedMaterialArtifact(
            artifact, bytes, error));
        EXPECT_NE(error.find("dependency hash"), std::string::npos);
    }
    {
        auto artifact = MakeArtifact();
        artifact.permutation ^= 1u;
        EXPECT_FALSE(RVX::Resource::SerializeCookedMaterialArtifact(
            artifact, bytes, error));
        EXPECT_NE(error.find("permutation"), std::string::npos);
    }
    {
        auto artifact = MakeArtifact();
        artifact.material.descriptor.programFamily = "arbitrary-shader";
        EXPECT_FALSE(RVX::Resource::SerializeCookedMaterialArtifact(
            artifact, bytes, error));
    }
    {
        auto artifact = MakeArtifact();
        artifact.material.parameters.roughnessFactor =
            std::numeric_limits<float>::quiet_NaN();
        EXPECT_FALSE(RVX::Resource::SerializeCookedMaterialArtifact(
            artifact, bytes, error));
    }
    {
        auto artifact = MakeArtifact();
        artifact.material.parameters.clearcoatFactor = 0.5f;
        EXPECT_FALSE(RVX::Resource::SerializeCookedMaterialArtifact(
            artifact, bytes, error));
        EXPECT_NE(error.find("feature identity"), std::string::npos);
    }
    {
        auto artifact = MakeArtifact();
        artifact.textureBindings.push_back(MakeBinding(
            RVX::MaterialTextureSlot::Clearcoat, "textures/clearcoat.rva"));
        EXPECT_FALSE(RVX::Resource::FinalizeCookedMaterialArtifact(
            artifact, error));
        EXPECT_NE(error.find("inactive"), std::string::npos);
    }
    {
        auto artifact = MakeArtifact();
        auto binding = MakeBinding(
            RVX::MaterialTextureSlot::Specular, "textures/specular.rva");
        binding.usage = RVX::Resource::TextureUsage::Color;
        binding.isSRGB = true;
        artifact.textureBindings.push_back(std::move(binding));
        EXPECT_FALSE(RVX::Resource::FinalizeCookedMaterialArtifact(
            artifact, error));
        EXPECT_NE(error.find("texture dependency"), std::string::npos);
    }
}

TEST(CookedMaterialArtifactValidation, TextureDependenciesAreStrict)
{
    std::string error;
    {
        auto artifact = MakeArtifact();
        artifact.textureBindings[0].artifactPath = "../escape.rva";
        EXPECT_FALSE(RVX::Resource::FinalizeCookedMaterialArtifact(
            artifact, error));
    }
    {
        auto artifact = MakeArtifact();
        artifact.textureBindings[0].expectedContentHash = {};
        EXPECT_FALSE(RVX::Resource::FinalizeCookedMaterialArtifact(
            artifact, error));
        EXPECT_NE(error.find("dependency"), std::string::npos);
    }
    {
        auto first = MakeArtifact();
        auto replacement = first;
        replacement.textureBindings[0].expectedContentHash[0] ^= 1u;
        ASSERT_TRUE(RVX::Resource::FinalizeCookedMaterialArtifact(
            replacement, error)) << error;
        EXPECT_NE(first.dependencyHash, replacement.dependencyHash);
    }
    {
        auto artifact = MakeArtifact();
        artifact.textureBindings.push_back(artifact.textureBindings.front());
        EXPECT_FALSE(RVX::Resource::FinalizeCookedMaterialArtifact(
            artifact, error));
        EXPECT_NE(error.find("duplicate"), std::string::npos);
    }
    {
        auto artifact = MakeArtifact();
        artifact.textureBindings[0].definition.uvSet = 2;
        EXPECT_FALSE(RVX::Resource::FinalizeCookedMaterialArtifact(
            artifact, error));
    }
    {
        auto artifact = MakeArtifact();
        artifact.textureBindings[0].definition.scale.x = 0.0f;
        EXPECT_TRUE(RVX::Resource::FinalizeCookedMaterialArtifact(
            artifact, error)) << error;
    }
    {
        auto artifact = MakeArtifact();
        artifact.textureBindings[0].definition.scale.x =
            std::numeric_limits<float>::quiet_NaN();
        EXPECT_FALSE(RVX::Resource::FinalizeCookedMaterialArtifact(
            artifact, error));
    }
}

TEST(CookedMaterialArtifactValidation, EnvelopeRejectsCorruptionAndOldMagic)
{
    auto artifact = MakeArtifact();
    std::vector<RVX::uint8> bytes;
    std::string error;
    ASSERT_TRUE(RVX::Resource::SerializeCookedMaterialArtifact(
        artifact, bytes, error)) << error;

    bytes.back() ^= 0x5au;
    RVX::Resource::CookedMaterialArtifact decoded;
    EXPECT_FALSE(RVX::Resource::DeserializeCookedMaterialArtifact(
        bytes, decoded, error));
    EXPECT_NE(error.find("hash"), std::string::npos);

    for (const std::string_view retired : {
             std::string_view("RVX_MATERIAL_ARTIFACT_V0"),
             std::string_view("RVX_MATERIAL_ARTIFACT_V1")})
    {
        std::vector<RVX::uint8> retiredBytes(retired.begin(), retired.end());
        EXPECT_FALSE(RVX::Resource::DeserializeCookedMaterialArtifact(
            retiredBytes, decoded, error));
    }
}
