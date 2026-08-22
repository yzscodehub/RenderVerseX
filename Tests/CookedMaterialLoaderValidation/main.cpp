#include "Resource/Cooked/CookedMaterialArtifact.h"
#include "Resource/Loader/CookedMaterialLoader.h"
#include "Resource/Types/MaterialResource.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    class TemporaryDirectory final
    {
    public:
        TemporaryDirectory()
        {
            const auto stamp = std::chrono::steady_clock::now()
                .time_since_epoch().count();
            m_path = fs::temp_directory_path() /
                ("rvx_cooked_material_loader_" + std::to_string(stamp));
            fs::create_directories(m_path / "textures");
        }

        ~TemporaryDirectory()
        {
            std::error_code error;
            fs::remove_all(m_path, error);
        }

        [[nodiscard]] const fs::path& Get() const noexcept { return m_path; }

    private:
        fs::path m_path;
    };

    bool WriteBytes(const fs::path& path, const std::vector<RVX::uint8>& bytes)
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file.is_open())
            return false;
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        return file.good();
    }

    bool WriteTexture(const fs::path& path,
                      RVX::Resource::TextureUsage usage,
                      bool srgb,
                      RVX::uint8 pixelSeed = 1u)
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file.is_open())
            return false;
        file << "RVX_TEXTURE_PREBAKE_V1\n"
             << "width=1\n"
             << "height=1\n"
             << "depth=1\n"
             << "mipLevels=1\n"
             << "arrayLayers=1\n"
             << "format=RGBA8\n"
             << "usage=" << (usage == RVX::Resource::TextureUsage::Color
                                  ? "Color"
                                  : usage == RVX::Resource::TextureUsage::Normal
                                      ? "Normal"
                                      : "Data") << "\n"
             << "srgb=" << (srgb ? 1 : 0) << "\n"
             << "compression=UncompressedRGBA\n"
             << "dataSize=4\n"
             << "RVX_TEXTURE_DATA_BEGIN\n";
        const char pixels[] = {
            static_cast<char>(pixelSeed),
            static_cast<char>(pixelSeed + 1u),
            static_cast<char>(pixelSeed + 2u),
            static_cast<char>(pixelSeed + 3u)};
        file.write(pixels, sizeof(pixels));
        file << "\nRVX_TEXTURE_PREBAKE_END\n";
        return file.good();
    }

    RVX::Hash::SHA256Digest HashFile(const fs::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        const std::vector<RVX::uint8> bytes{
            std::istreambuf_iterator<char>(file),
            std::istreambuf_iterator<char>()};
        return RVX::Hash::ComputeSHA256(bytes.data(), bytes.size());
    }

    RVX::Hash::SHA256Digest FixtureTextureHash()
    {
        return RVX::Hash::ComputeSHA256("fixture-texture");
    }

    RVX::Resource::CookedMaterialArtifact MakeArtifact(
        std::string texturePath = "textures/albedo.rva",
        RVX::Hash::SHA256Digest expectedContentHash = FixtureTextureHash())
    {
        RVX::Resource::CookedMaterialArtifact artifact;
        artifact.logicalMaterialId = "Materials/LoaderValidation";
        artifact.material.parameters.baseColorFactor = {0.2f, 0.3f, 0.4f, 1.0f};
        RVX::Resource::CookedMaterialTextureBinding binding;
        binding.definition.slot = RVX::MaterialTextureSlot::BaseColor;
        binding.artifactPath = std::move(texturePath);
        binding.expectedContentHash = expectedContentHash;
        binding.usage = RVX::Resource::TextureUsage::Color;
        binding.isSRGB = true;
        binding.fallbackSemantic =
            RVX::Resource::TextureFallbackSemantic::White;
        artifact.textureBindings.push_back(std::move(binding));
        std::string error;
        EXPECT_TRUE(RVX::Resource::FinalizeCookedMaterialArtifact(
            artifact, error)) << error;
        return artifact;
    }

    RVX::Resource::ResourceLoadPreparationContext MakeContext(
        const fs::path& path)
    {
        RVX::Resource::ResourceLoadPreparationContext context;
        context.requestedPath = path.string();
        context.resolvedPath = path.string();
        context.assetKey = RVX::Resource::MakeAssetKey(
            context.resolvedPath, RVX::Resource::ResourceType::Material);
        context.resourceIdentityPath = context.assetKey.canonicalPath;
        context.rootResourceId = RVX::Resource::GenerateResourceId(
            context.resourceIdentityPath);
        return context;
    }

    bool WriteMaterial(const fs::path& path,
                       RVX::Resource::CookedMaterialArtifact artifact)
    {
        std::string error;
        if (!RVX::Resource::FinalizeCookedMaterialArtifact(artifact, error))
            return false;
        std::vector<RVX::uint8> bytes;
        return RVX::Resource::SerializeCookedMaterialArtifact(
                   artifact, bytes, error) &&
            WriteBytes(path, bytes);
    }

    bool SkipEncodedString(const std::vector<RVX::uint8>& bytes,
                           size_t& offset)
    {
        if (offset > bytes.size() || bytes.size() - offset < sizeof(RVX::uint32))
            return false;
        const RVX::uint32 count =
            static_cast<RVX::uint32>(bytes[offset]) |
            (static_cast<RVX::uint32>(bytes[offset + 1]) << 8u) |
            (static_cast<RVX::uint32>(bytes[offset + 2]) << 16u) |
            (static_cast<RVX::uint32>(bytes[offset + 3]) << 24u);
        offset += sizeof(RVX::uint32);
        if (count > bytes.size() - offset)
            return false;
        offset += count;
        return true;
    }

    bool WriteInvalidLayoutMaterial(const fs::path& path)
    {
        RVX::Resource::CookedMaterialArtifact artifact = MakeArtifact();
        std::string error;
        std::vector<RVX::uint8> bytes;
        if (!RVX::Resource::SerializeCookedMaterialArtifact(
                artifact, bytes, error))
        {
            return false;
        }

        const size_t payloadOffset =
            RVX::Resource::RVX_MATERIAL_ARTIFACT_MAGIC.size() +
            sizeof(RVX::uint32) + sizeof(RVX::uint64) +
            RVX::Hash::RVX_SHA256_DIGEST_SIZE;
        if (payloadOffset > bytes.size())
            return false;
        size_t layoutHashOffset = payloadOffset;
        constexpr size_t staticFieldsBeforeProgramFamily =
            6u + sizeof(RVX::uint32) + sizeof(RVX::uint64);
        if (!SkipEncodedString(bytes, layoutHashOffset) ||
            !SkipEncodedString(bytes, layoutHashOffset) ||
            bytes.size() - layoutHashOffset < staticFieldsBeforeProgramFamily)
        {
            return false;
        }
        layoutHashOffset += staticFieldsBeforeProgramFamily;
        if (!SkipEncodedString(bytes, layoutHashOffset) ||
            bytes.size() - layoutHashOffset < sizeof(RVX::uint64))
        {
            return false;
        }

        bytes[layoutHashOffset] ^= 1u;
        const RVX::Hash::SHA256Digest digest = RVX::Hash::ComputeSHA256(
            bytes.data() + payloadOffset, bytes.size() - payloadOffset);
        const size_t digestOffset =
            RVX::Resource::RVX_MATERIAL_ARTIFACT_MAGIC.size() +
            sizeof(RVX::uint32) + sizeof(RVX::uint64);
        for (size_t index = 0; index < digest.size(); ++index)
            bytes[digestOffset + index] = digest[index];
        return WriteBytes(path, bytes);
    }

    std::vector<RVX::ResourceId> DependencyIds(
        const RVX::Resource::PreparedResourceBundle& bundle)
    {
        std::vector<RVX::Resource::ResourceId> ids;
        for (const RVX::Resource::PreparedResourceEntry& entry : bundle.GetEntries())
        {
            if (!entry.isRoot)
                ids.push_back(entry.resource.GetId());
        }
        return ids;
    }
} // namespace

TEST(CookedMaterialLoaderValidation, SupportsOnlyTypedCookedMaterialProducts)
{
    RVX::Resource::CookedMaterialLoader loader;
    RVX::Resource::ResourceLoaderRequest request;
    request.type = RVX::ResourceType::Material;
    request.domain = RVX::ResourceLoadDomain::Cooked;
    request.resolvedPath = "Materials/Surface.rvmat";
    EXPECT_TRUE(loader.Supports(request));

    request.resolvedPath = "Materials/Surface.rva";
    EXPECT_FALSE(loader.Supports(request));
    request.resolvedPath = "Materials/Surface.rvmat";
    request.type = RVX::ResourceType::Model;
    EXPECT_FALSE(loader.Supports(request));
    request.type = RVX::ResourceType::Material;
    request.domain = RVX::ResourceLoadDomain::Source;
    EXPECT_FALSE(loader.Supports(request));
}

TEST(CookedMaterialLoaderValidation, PreparesStandaloneMaterialAndStableDependencies)
{
    TemporaryDirectory temporary;
    const fs::path materialPath = temporary.Get() / "Surface.rvmat";
    ASSERT_TRUE(WriteTexture(
        temporary.Get() / "textures" / "albedo.rva",
        RVX::Resource::TextureUsage::Color,
        true));
    const auto textureHash = HashFile(
        temporary.Get() / "textures" / "albedo.rva");
    ASSERT_TRUE(WriteMaterial(materialPath, MakeArtifact(
        "textures/albedo.rva", textureHash)));

    RVX::Resource::CookedMaterialLoader loader;
    const auto context = MakeContext(materialPath);
    RVX::Resource::PreparedResourceBundle first;
    RVX::Resource::ResourceLoadError error;
    ASSERT_TRUE(loader.Prepare(context, first, error)) << error.message;
    ASSERT_TRUE(first.IsValid());
    ASSERT_TRUE(first.GetObservedContentIdentity().IsValid());
    ASSERT_EQ(first.GetEntries().size(), 2u);

    auto* material = dynamic_cast<RVX::Resource::MaterialResource*>(
        first.GetRoot().Get());
    ASSERT_NE(material, nullptr);
    const RVX::Vec4 baseColor =
        material->GetMaterialDefinition().parameters.baseColorFactor;
    EXPECT_FLOAT_EQ(baseColor.x, 0.2f);
    EXPECT_FLOAT_EQ(baseColor.y, 0.3f);
    EXPECT_FLOAT_EQ(baseColor.z, 0.4f);
    EXPECT_FLOAT_EQ(baseColor.w, 1.0f);
    EXPECT_EQ(material->GetMaterialDefinition().descriptor.featureMask, 1ull);
    EXPECT_EQ(material->GetMaterialDefinition().parameters.textureFlags, 1u);
    ASSERT_TRUE(material->GetAlbedoTexture());
    EXPECT_EQ(material->GetAlbedoTexture()->GetUsage(),
              RVX::Resource::TextureUsage::Color);
    EXPECT_TRUE(material->GetAlbedoTexture()->IsSRGB());

    RVX::Resource::PreparedResourceBundle second;
    ASSERT_TRUE(loader.Prepare(context, second, error)) << error.message;
    EXPECT_EQ(first.GetRoot().GetId(), second.GetRoot().GetId());
    EXPECT_EQ(DependencyIds(first), DependencyIds(second));
}

TEST(CookedMaterialLoaderValidation,
     SynchronizesCompatibilityMaterialSlotMasks)
{
    auto source = std::make_shared<RVX::Material>("slot-mask-source");
    source->SetBaseColorTexture(RVX::TextureInfo("textures/albedo.png"));

    RVX::Resource::MaterialResource material;
    ASSERT_TRUE(material.SetMaterialData(source));
    EXPECT_EQ(material.GetMaterialDefinition().descriptor.featureMask, 1ull);
    EXPECT_EQ(material.GetMaterialDefinition().parameters.textureFlags, 1u);

    ASSERT_TRUE(material.SetTexture(
        "normal", RVX::Resource::ResourceHandle<RVX::Resource::TextureResource>(
                      new RVX::Resource::TextureResource())));
    const RVX::MaterialDefinition& definition = material.GetMaterialDefinition();
    EXPECT_EQ(definition.descriptor.featureMask, 0x03ull);
    EXPECT_EQ(definition.parameters.textureFlags, 0x03u);
}

TEST(CookedMaterialLoaderValidation, RejectsCorruptLayoutAndDependencies)
{
    TemporaryDirectory temporary;
    const fs::path materialPath = temporary.Get() / "Surface.rvmat";
    const fs::path texturePath = temporary.Get() / "textures" / "albedo.rva";
    ASSERT_TRUE(WriteTexture(texturePath, RVX::Resource::TextureUsage::Color, true));
    const auto originalTextureHash = HashFile(texturePath);
    ASSERT_TRUE(WriteMaterial(materialPath, MakeArtifact(
        "textures/albedo.rva", originalTextureHash)));

    RVX::Resource::CookedMaterialLoader loader;
    const auto context = MakeContext(materialPath);
    RVX::Resource::ResourceLoadError error;
    RVX::Resource::PreparedResourceBundle bundle;
    ASSERT_TRUE(loader.Prepare(context, bundle, error)) << error.message;

    std::vector<RVX::uint8> corrupt = {0x01u, 0x02u, 0x03u};
    ASSERT_TRUE(WriteBytes(materialPath, corrupt));
    RVX::Resource::PreparedResourceBundle corruptBundle;
    EXPECT_FALSE(loader.Prepare(context, corruptBundle, error));

    ASSERT_TRUE(WriteInvalidLayoutMaterial(materialPath));
    RVX::Resource::PreparedResourceBundle invalidLayoutBundle;
    EXPECT_FALSE(loader.Prepare(context, invalidLayoutBundle, error));

    ASSERT_TRUE(WriteMaterial(materialPath, MakeArtifact(
        "textures/albedo.rva", originalTextureHash)));
    ASSERT_TRUE(WriteTexture(
        texturePath, RVX::Resource::TextureUsage::Color, true, 17u));
    RVX::Resource::PreparedResourceBundle replacedTextureBundle;
    EXPECT_FALSE(loader.Prepare(context, replacedTextureBundle, error));
    EXPECT_NE(error.message.find("content hash mismatch"), std::string::npos);

    ASSERT_TRUE(WriteTexture(texturePath, RVX::Resource::TextureUsage::Normal, false));
    ASSERT_TRUE(WriteMaterial(materialPath, MakeArtifact(
        "textures/albedo.rva", HashFile(texturePath))));
    RVX::Resource::PreparedResourceBundle mismatchBundle;
    EXPECT_FALSE(loader.Prepare(context, mismatchBundle, error));

    ASSERT_TRUE(WriteMaterial(
        materialPath, MakeArtifact("missing/outside.rva")));
    RVX::Resource::PreparedResourceBundle missingBundle;
    EXPECT_FALSE(loader.Prepare(context, missingBundle, error));
}
