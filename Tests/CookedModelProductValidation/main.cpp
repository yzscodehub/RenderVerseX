#include "Core/Log.h"
#include "Geometry/Asset/Mesh.h"
#include "Geometry/Asset/Model.h"
#include "Resource/Cooked/CookedMeshArtifactReader.h"
#include "Resource/Cooked/CookedModelArtifact.h"
#include "Resource/Importer/GLTFImporter.h"
#include "Resource/Loader/ModelLoader.h"
#include "Resource/Types/ModelResource.h"
#include "Tools/AssetPipeline.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    class TemporaryDirectory
    {
    public:
        TemporaryDirectory()
        {
            const auto token = std::chrono::steady_clock::now()
                                   .time_since_epoch()
                                   .count();
            m_path = fs::temp_directory_path() /
                     ("rvx_cooked_model_validation_" +
                      std::to_string(token));
            fs::create_directories(m_path);
        }

        ~TemporaryDirectory()
        {
            std::error_code error;
            fs::remove_all(m_path, error);
        }

        const fs::path& Get() const { return m_path; }

    private:
        fs::path m_path;
    };

    std::vector<RVX::uint8> ReadBytes(const fs::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(file),
                std::istreambuf_iterator<char>()};
    }

    std::string ReadPrefix(const fs::path& path, size_t byteCount = 1024)
    {
        std::ifstream file(path, std::ios::binary);
        std::string prefix(byteCount, '\0');
        file.read(prefix.data(), static_cast<std::streamsize>(prefix.size()));
        prefix.resize(static_cast<size_t>(file.gcount()));
        return prefix;
    }
} // namespace

TEST(CookedModelProductValidation, DeterministicRoundTripLoadsWithoutSourceParsing)
{
    const fs::path source =
        fs::path(RVX_SOURCE_DIR) /
        "Tests/Fixtures/Samples/PBRMaterialTextureCube.gltf";
    ASSERT_TRUE(fs::is_regular_file(source));

    TemporaryDirectory temporary;
    const fs::path output = temporary.Get() / "PBRMaterialTextureCube.rva";
    RVX::Tools::ModelImporter importer;
    RVX::Tools::ImportResult first = importer.Import(source, output);
    ASSERT_TRUE(first.success) << first.error;
    ASSERT_GE(first.outputPaths.size(), 3u);
    const std::vector<RVX::uint8> firstBytes = ReadBytes(output);
    ASSERT_FALSE(firstBytes.empty());
    const std::string firstPrefix(
        reinterpret_cast<const char*>(firstBytes.data()),
        std::min<size_t>(firstBytes.size(), 64u));
    EXPECT_TRUE(firstPrefix.starts_with(
        RVX::Resource::RVX_MODEL_PREBAKE_LEGACY_MAGIC));

    RVX::Tools::ImportResult second = importer.Import(source, output);
    ASSERT_TRUE(second.success) << second.error;
    EXPECT_EQ(ReadBytes(output), firstBytes);

    RVX::Resource::CookedModelArtifact artifact;
    std::string error;
    ASSERT_TRUE(RVX::Resource::DeserializeCookedModelArtifact(
        firstBytes, artifact, error)) << error;
    ASSERT_TRUE(artifact.rootNode);
    EXPECT_TRUE(artifact.animationArtifactPath.empty());
    EXPECT_EQ(artifact.materials.size(), 5u);
    EXPECT_EQ(artifact.textures.size(), 1u);

    RVX::Resource::GLTFImporter sourceImporter;
    const RVX::Resource::GLTFImportResult sourceResult =
        sourceImporter.Import(source.string());
    ASSERT_TRUE(sourceResult.success) << sourceResult.errorMessage;
    ASSERT_TRUE(sourceResult.model);
    ASSERT_TRUE(artifact.bounds.IsValid());
    EXPECT_EQ(artifact.bounds.GetMin(),
              sourceResult.model->GetBoundingBox().GetMin());
    EXPECT_EQ(artifact.bounds.GetMax(),
              sourceResult.model->GetBoundingBox().GetMax());

    const fs::path meshPath = output.parent_path() /
                              artifact.meshArtifactPath;
    std::vector<RVX::Resource::CookedMeshRecord> meshes;
    ASSERT_TRUE(RVX::Resource::CookedMeshArtifactReader::ReadFile(
        meshPath.string(), meshes, error)) << error;
    EXPECT_EQ(meshes.size(), 5u);
    for (const auto& mesh : meshes)
        ASSERT_FALSE(mesh.lodMeshes.empty());

    RVX::Resource::ModelLoader loader(nullptr);
    std::unique_ptr<RVX::Resource::ModelResource> model(
        static_cast<RVX::Resource::ModelResource*>(
            loader.Load(output.string())));
    ASSERT_TRUE(model);
    EXPECT_EQ(model->GetMeshCount(), 5u);
    EXPECT_EQ(model->GetMaterialCount(), 5u);
    EXPECT_EQ(model->GetNodeCount(), 6u);
    EXPECT_FALSE(model->GetTextureStreamingSnapshot().HasStreamingTextures());
}

TEST(CookedModelProductValidation,
     IndexedModelBoundsResolveSingularAndExplicitPrimitivesWithTransforms)
{
    const auto makeMesh = [](const RVX::Vec3& min, const RVX::Vec3& max)
    {
        auto mesh = std::make_shared<RVX::Mesh>();
        mesh->SetBoundingBox(min, max);
        return mesh;
    };

    RVX::Model model;
    auto root = std::make_shared<RVX::Node>("Root");
    root->GetLocalTransform().SetPosition({10.0f, 2.0f, -1.0f});
    root->SetMeshIndices({0, 1, 1});
    auto child = std::make_shared<RVX::Node>("SingularChild");
    child->GetLocalTransform().SetPosition({6.0f, 0.0f, 0.0f});
    child->SetMeshIndex(2);
    root->AddChild(child);
    model.SetRootNode(root);

    const std::vector<RVX::Mesh::Ptr> meshes{
        makeMesh({0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}),
        makeMesh({-3.0f, -1.0f, 1.0f}, {-2.0f, 2.0f, 2.0f}),
        makeMesh({0.0f, 0.0f, 0.0f}, {2.0f, 1.0f, 1.0f}),
    };

    ASSERT_TRUE(model.ComputeBoundingBox(meshes));
    EXPECT_EQ(model.GetBoundingBox().GetMin(), (RVX::Vec3{7.0f, 1.0f, -1.0f}));
    EXPECT_EQ(model.GetBoundingBox().GetMax(), (RVX::Vec3{18.0f, 4.0f, 1.0f}));

    EXPECT_FALSE(model.ComputeBoundingBox({}));
    EXPECT_FALSE(model.GetBoundingBox().IsValid());
    EXPECT_FALSE(model.ComputeBoundingBox({meshes[0]}));
    EXPECT_FALSE(model.GetBoundingBox().IsValid());

    std::vector<RVX::Mesh::Ptr> missingBounds = meshes;
    missingBounds[1] = std::make_shared<RVX::Mesh>();
    EXPECT_FALSE(model.ComputeBoundingBox(missingBounds));
    EXPECT_FALSE(model.GetBoundingBox().IsValid());
}

TEST(CookedModelProductValidation, PersistedAssetTypeValuesRemainStable)
{
    EXPECT_EQ(static_cast<RVX::uint8>(RVX::Tools::AssetType::Unknown), 0u);
    EXPECT_EQ(static_cast<RVX::uint8>(RVX::Tools::AssetType::Texture), 1u);
    EXPECT_EQ(static_cast<RVX::uint8>(RVX::Tools::AssetType::Mesh), 2u);
    EXPECT_EQ(static_cast<RVX::uint8>(RVX::Tools::AssetType::Material), 3u);
    EXPECT_EQ(static_cast<RVX::uint8>(RVX::Tools::AssetType::Script), 10u);
    EXPECT_EQ(static_cast<RVX::uint8>(RVX::Tools::AssetType::Model), 11u);
}

TEST(CookedModelProductValidation, IncompleteSkinWithoutAnimationFailsAtCookTime)
{
    const fs::path source =
        fs::path(RVX_SOURCE_DIR) /
        "Tests/Fixtures/ModelViewer/R7Triangle.gltf";
    std::ifstream sourceFile(source, std::ios::binary);
    std::string gltf{std::istreambuf_iterator<char>(sourceFile),
                     std::istreambuf_iterator<char>()};
    const size_t closingBrace = gltf.find_last_of('}');
    ASSERT_NE(closingBrace, std::string::npos);
    gltf.insert(closingBrace, ",\n  \"skins\": [{\"joints\": [0]}]\n");

    TemporaryDirectory temporary;
    const fs::path skinnedSource = temporary.Get() / "Skinned.gltf";
    std::ofstream skinnedFile(skinnedSource, std::ios::binary);
    skinnedFile.write(gltf.data(), static_cast<std::streamsize>(gltf.size()));
    skinnedFile.close();

    RVX::Tools::ModelImporter importer;
    const RVX::Tools::ImportResult result =
        importer.Import(skinnedSource, temporary.Get() / "Skinned.rva");
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.error.empty());
    EXPECT_FALSE(fs::exists(temporary.Get() / "Skinned.rva"));
    EXPECT_FALSE(fs::exists(temporary.Get() / "Skinned.rvdeps"));
}

TEST(CookedModelProductValidation, TextureProductsUseModelPlatformCompressionContract)
{
    const fs::path source =
        fs::path(RVX_SOURCE_DIR) /
        "Tests/Fixtures/ModelViewer/PBRMaterialSwatch.gltf";
    TemporaryDirectory temporary;
    const fs::path output = temporary.Get() / "PBRMaterialSwatch.rva";
    RVX::Tools::ModelImporter importer;
    const RVX::Tools::ImportResult result = importer.Import(source, output);
    ASSERT_TRUE(result.success) << result.error;

    RVX::Resource::CookedModelArtifact artifact;
    std::string error;
    ASSERT_TRUE(RVX::Resource::DeserializeCookedModelArtifact(
        ReadBytes(output), artifact, error)) << error;
    ASSERT_FALSE(artifact.textures.empty());
    for (const RVX::Resource::TextureReference& texture : artifact.textures)
    {
        const std::string prefix = ReadPrefix(output.parent_path() /
                                              texture.path);
        EXPECT_NE(prefix.find("RVX_TEXTURE_PREBAKE_V1"),
                  std::string::npos);
        const char* expected =
            texture.usage == RVX::Resource::TextureUsage::Normal
                ? "format=BC5"
                : "format=BC7";
        EXPECT_NE(prefix.find(expected), std::string::npos)
            << texture.path;
    }
}

TEST(CookedModelProductValidation, CorruptionAndDependencyEscapeFailClosed)
{
    const fs::path source =
        fs::path(RVX_SOURCE_DIR) /
        "Tests/Fixtures/ModelViewer/R7Triangle.gltf";
    TemporaryDirectory temporary;
    const fs::path output = temporary.Get() / "R7Triangle.rva";
    RVX::Tools::ModelImporter importer;
    ASSERT_TRUE(importer.Import(source, output).success);

    std::vector<RVX::uint8> bytes = ReadBytes(output);
    ASSERT_GT(bytes.size(), 64u);
    bytes[bytes.size() - 32] ^= 0x1u;
    RVX::Resource::CookedModelArtifact artifact;
    std::string error;
    EXPECT_FALSE(RVX::Resource::DeserializeCookedModelArtifact(
        bytes, artifact, error));
    EXPECT_NE(error.find("hash"), std::string::npos);

    std::vector<RVX::uint8> trailingBytes = ReadBytes(output);
    trailingBytes.push_back(0u);
    EXPECT_FALSE(RVX::Resource::DeserializeCookedModelArtifact(
        trailingBytes, artifact, error));
    EXPECT_NE(error.find("boundary"), std::string::npos);

    ASSERT_TRUE(RVX::Resource::DeserializeCookedModelArtifact(
        ReadBytes(output), artifact, error)) << error;
    RVX::Resource::CookedModelArtifact invalidBounds = artifact;
    invalidBounds.bounds.SetMax(RVX::Vec3(
        std::numeric_limits<float>::infinity()));
    std::vector<RVX::uint8> invalidBoundsBytes;
    EXPECT_FALSE(RVX::Resource::SerializeCookedModelArtifact(
        invalidBounds, invalidBoundsBytes, error));

    RVX::Resource::CookedModelArtifact invalidTransform = artifact;
    invalidTransform.rootNode->GetLocalTransform().SetPosition(
        RVX::Vec3(std::numeric_limits<float>::infinity()));
    EXPECT_FALSE(RVX::Resource::SerializeCookedModelArtifact(
        invalidTransform, invalidBoundsBytes, error));

    ASSERT_TRUE(RVX::Resource::DeserializeCookedModelArtifact(
        ReadBytes(output), artifact, error)) << error;

    artifact.meshArtifactPath = "../outside.rva";
    std::vector<RVX::uint8> escapedBytes;
    ASSERT_TRUE(RVX::Resource::SerializeCookedModelArtifact(
        artifact, escapedBytes, error)) << error;
    const fs::path escapedRoot = temporary.Get() / "Escaped.rva";
    std::ofstream escapedFile(escapedRoot, std::ios::binary);
    escapedFile.write(reinterpret_cast<const char*>(escapedBytes.data()),
                      static_cast<std::streamsize>(escapedBytes.size()));
    escapedFile.close();
    RVX::Log::Initialize();
    RVX::Resource::ModelLoader loader(nullptr);
    EXPECT_EQ(loader.Load(escapedRoot.string()), nullptr);
    RVX::Log::Shutdown();
}

TEST(CookedModelProductValidation, ResolvedDependencyMustRemainInsideProductDirectory)
{
    const fs::path source =
        fs::path(RVX_SOURCE_DIR) /
        "Tests/Fixtures/ModelViewer/R7Triangle.gltf";
    TemporaryDirectory temporary;
    const fs::path productDirectory = temporary.Get() / "product";
    const fs::path output = productDirectory / "R7Triangle.rva";
    RVX::Tools::ModelImporter importer;
    ASSERT_TRUE(importer.Import(source, output).success);

    RVX::Resource::CookedModelArtifact artifact;
    std::string error;
    ASSERT_TRUE(RVX::Resource::DeserializeCookedModelArtifact(
        ReadBytes(output), artifact, error)) << error;

    const fs::path outsideDirectory = temporary.Get() / "outside";
    fs::create_directories(outsideDirectory);
    fs::copy_file(productDirectory / artifact.meshArtifactPath,
                  outsideDirectory / "meshes.rva");
    std::error_code linkError;
    fs::create_directory_symlink(outsideDirectory,
                                 productDirectory / "linked",
                                 linkError);
    if (linkError)
        GTEST_SKIP() << "Directory symlink creation is unavailable: "
                     << linkError.message();

    artifact.meshArtifactPath = "linked/meshes.rva";
    std::vector<RVX::uint8> escapedBytes;
    ASSERT_TRUE(RVX::Resource::SerializeCookedModelArtifact(
        artifact, escapedBytes, error)) << error;
    const fs::path escapedRoot = productDirectory / "ResolvedEscape.rva";
    std::ofstream escapedFile(escapedRoot, std::ios::binary);
    escapedFile.write(reinterpret_cast<const char*>(escapedBytes.data()),
                      static_cast<std::streamsize>(escapedBytes.size()));
    escapedFile.close();

    RVX::Log::Initialize();
    RVX::Resource::ModelLoader loader(nullptr);
    EXPECT_EQ(loader.Load(escapedRoot.string()), nullptr);
    RVX::Log::Shutdown();
}
