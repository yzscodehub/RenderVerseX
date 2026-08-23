#include "AssetImport/FBXImporter.h"
#include "Tools/AssetPipeline.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{
    namespace fs = std::filesystem;

    fs::path Fixture(std::string_view name)
    {
        return fs::path(RVX_SOURCE_DIR) / "Tests/Fixtures/FBX" / name;
    }

    class TemporaryDirectory
    {
    public:
        TemporaryDirectory()
        {
            static std::atomic<uint64_t> sequence{0};
            m_path = fs::temp_directory_path() /
                ("rvx-fbx-unit-" +
                 std::to_string(std::chrono::steady_clock::now()
                                    .time_since_epoch().count()) +
                 "-" + std::to_string(++sequence));
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

    RVX::Resource::FBXImportResult Import(std::string_view name)
    {
        RVX::Resource::FBXImporter importer;
        return importer.Import(Fixture(name).string());
    }

    void ExpectStaticResult(const RVX::Resource::FBXImportResult& result)
    {
        ASSERT_TRUE(result.success) << result.errorMessage;
        ASSERT_TRUE(result.model);
        ASSERT_TRUE(result.model->GetBoundingBox().IsValid());
        ASSERT_FALSE(result.model->HasAnimation());
        ASSERT_FALSE(result.meshes.empty());
        ASSERT_FALSE(result.materials.empty());
        for (const RVX::Mesh::Ptr& mesh : result.meshes)
        {
            ASSERT_TRUE(mesh);
            EXPECT_TRUE(mesh->IsValid());
            EXPECT_EQ(mesh->GetPrimitiveType(), RVX::PrimitiveType::Triangles);
            EXPECT_TRUE(mesh->ValidateSubMeshes());
            EXPECT_GT(mesh->GetIndexCount(), 0u);
        }
    }
} // namespace

TEST(FBXImporterValidation, ImportsRedistributableAsciiAndBinaryFixtures)
{
    RVX::Resource::FBXImporter importer;
    EXPECT_TRUE(importer.CanImport("model.FBX"));
    EXPECT_FALSE(importer.CanImport("model.gltf"));

    const RVX::Resource::FBXImportResult ascii = Import("static-ascii.fbx");
    ExpectStaticResult(ascii);
    const RVX::Resource::FBXImportResult binary = Import("static-binary.fbx");
    ExpectStaticResult(binary);
}

TEST(FBXImporterValidation, SplitsPolygonCornerUvAndHardNormalTuples)
{
    const RVX::Resource::FBXImportResult result = Import("static-ascii.fbx");
    ExpectStaticResult(result);
    ASSERT_EQ(result.meshes.size(), 1u);
    const RVX::Mesh& mesh = *result.meshes.front();
    EXPECT_TRUE(mesh.HasAttribute(RVX::VertexBufferNames::Normal));
    EXPECT_TRUE(mesh.HasAttribute(RVX::VertexBufferNames::UV));
    EXPECT_GT(mesh.GetVertexCount(), 8u);
    EXPECT_EQ(mesh.GetIndexCount(), 36u);
}

TEST(FBXImporterValidation, EmitsContiguousMaterialPartsWithRealOffsets)
{
    const RVX::Resource::FBXImportResult result = Import("multi-material.fbx");
    ExpectStaticResult(result);
    bool foundSecondPart = false;
    for (const RVX::Mesh::Ptr& mesh : result.meshes)
    {
        const auto& subMeshes = mesh->GetSubMeshes();
        uint32_t expectedOffset = 0;
        for (size_t index = 0; index < subMeshes.size(); ++index)
        {
            EXPECT_EQ(subMeshes[index].indexOffset, expectedOffset);
            EXPECT_GT(subMeshes[index].indexCount, 0u);
            if (index > 0)
            {
                EXPECT_GT(subMeshes[index].indexOffset, 0u);
                foundSecondPart = true;
            }
            expectedOffset += subMeshes[index].indexCount;
        }
        EXPECT_EQ(expectedOffset, mesh->GetIndexCount());
    }
    EXPECT_TRUE(foundSecondPart);
}

TEST(FBXImporterValidation, PreservesHierarchyTransformsInMeterScale)
{
    const RVX::Resource::FBXImportResult result = Import("node-transform.fbx");
    ExpectStaticResult(result);
    bool foundNonIdentity = false;
    for (const RVX::Node::Ptr& node : result.model->GetAllNodes())
    {
        const RVX::Transform& transform = node->GetLocalTransform();
        const RVX::Vec3 position = transform.GetPosition();
        const RVX::Vec3 scale = transform.GetScale();
        if (glm::length(position) > 1.0e-5f ||
            glm::length(scale - RVX::Vec3(1.0f)) > 1.0e-5f)
        {
            foundNonIdentity = true;
        }
    }
    EXPECT_TRUE(foundNonIdentity);
    const RVX::Vec3 size = result.model->GetBoundingBox().GetSize();
    EXPECT_GT(size.x + size.y + size.z, 0.0f);
    EXPECT_LT(size.x + size.y + size.z, 1000.0f);
}

TEST(FBXImporterValidation, UsesGeometryNormalAndTangentGenerationPath)
{
    const RVX::Resource::FBXImportResult result = Import("missing-normals.fbx");
    ExpectStaticResult(result);
    for (const RVX::Mesh::Ptr& mesh : result.meshes)
    {
        EXPECT_TRUE(mesh->HasAttribute(RVX::VertexBufferNames::Normal));
        if (mesh->HasAttribute(RVX::VertexBufferNames::UV))
        {
            EXPECT_TRUE(mesh->HasAttribute(RVX::VertexBufferNames::Tangent));
        }
    }
}

TEST(FBXImporterValidation, NormalMappedMeshesRequireAValidTangentBasis)
{
    TemporaryDirectory temporary;
    const fs::path sourcePath = temporary.Get() / "normal-mapped.fbx";
    std::ifstream input(Fixture("external-textures.fbx"), std::ios::binary);
    ASSERT_TRUE(input.is_open());
    std::string source((std::istreambuf_iterator<char>(input)),
                       std::istreambuf_iterator<char>());
    const std::string connectionMarker = "}\n;Takes and animation section";
    const size_t connectionOffset = source.rfind(connectionMarker);
    ASSERT_NE(connectionOffset, std::string::npos);
    source.insert(connectionOffset,
                  "\tConnect: \"OP\", \"Texture::file1\", \"Material::phong1\", \"NormalMap\"\n");
    std::ofstream output(sourcePath, std::ios::binary);
    output.write(source.data(), static_cast<std::streamsize>(source.size()));
    output.close();
    ASSERT_TRUE(output.good());
    fs::copy(Fixture("textures"),
             temporary.Get() / "textures",
             fs::copy_options::recursive);

    RVX::Resource::FBXImporter importer;
    const RVX::Resource::FBXImportResult generated = importer.Import(sourcePath.string());
    ExpectStaticResult(generated);
    ASSERT_FALSE(generated.materials.empty());
    EXPECT_TRUE(generated.materials.front()->GetNormalTexture().has_value());
    for (const RVX::Mesh::Ptr& mesh : generated.meshes)
    {
        EXPECT_TRUE(mesh->HasAttribute(RVX::VertexBufferNames::Normal));
        EXPECT_TRUE(mesh->HasAttribute(RVX::VertexBufferNames::UV));
        EXPECT_TRUE(mesh->HasAttribute(RVX::VertexBufferNames::Tangent));
    }

    RVX::Resource::FBXImportOptions disabledTangents;
    disabledTangents.generateTangents = false;
    const RVX::Resource::FBXImportResult rejected =
        importer.Import(sourcePath.string(), disabledTangents);
    EXPECT_FALSE(rejected.success);
    EXPECT_NE(rejected.errorMessage.find("tangent generation"), std::string::npos)
        << rejected.errorMessage;
}

TEST(FBXImporterValidation, CapturesOnlyContainedExternalTextures)
{
    const RVX::Resource::FBXImportResult result = Import("external-textures.fbx");
    ExpectStaticResult(result);
    ASSERT_FALSE(result.textures.empty());
    const fs::path fixtureRoot = fs::canonical(Fixture(".")).lexically_normal();
    for (const RVX::Resource::TextureReference& texture : result.textures)
    {
        EXPECT_TRUE(texture.IsExternal());
        EXPECT_TRUE(texture.HasCapturedPayload());
        EXPECT_FALSE(texture.GetCapturedPayload().empty());
        const fs::path relative = fs::canonical(texture.path)
                                      .lexically_relative(fixtureRoot);
        EXPECT_FALSE(relative.empty());
        EXPECT_NE(*relative.begin(), fs::path(".."));
    }
}

TEST(FBXImporterValidation, RejectsMissingAndEscapingExternalTextures)
{
    TemporaryDirectory temporary;
    const fs::path missing = temporary.Get() / "missing-textures.fbx";
    fs::copy_file(Fixture("external-textures.fbx"), missing);
    RVX::Resource::FBXImporter importer;
    const auto missingResult = importer.Import(missing.string());
    EXPECT_FALSE(missingResult.success);
    EXPECT_NE(missingResult.errorMessage.find("does not exist"), std::string::npos)
        << missingResult.errorMessage;

    const auto escapeResult = importer.Import(Fixture("texture-root-escape.fbx").string());
    EXPECT_FALSE(escapeResult.success);
    EXPECT_NE(escapeResult.errorMessage.find("outside the source root"),
              std::string::npos)
        << escapeResult.errorMessage;
}

TEST(FBXImporterValidation, RejectsUnsupportedStaticSubsetFeatures)
{
    const std::pair<const char*, const char*> fixtures[] = {
        {"unsupported-skin.fbx", "skinning"},
        {"unsupported-animation.fbx", "animation"},
        {"unsupported-blend-shape.fbx", "blend shape"},
        {"unsupported-embedded-texture.fbx", "embedded texture"},
    };
    for (const auto& [name, diagnostic] : fixtures)
    {
        SCOPED_TRACE(name);
        const RVX::Resource::FBXImportResult result = Import(name);
        EXPECT_FALSE(result.success);
        EXPECT_NE(result.errorMessage.find(diagnostic), std::string::npos)
            << result.errorMessage;
    }
}

TEST(FBXImporterValidation, RejectsTruncatedAndCorruptFiles)
{
    TemporaryDirectory temporary;
    const fs::path truncated = temporary.Get() / "truncated.fbx";
    std::ifstream source(Fixture("static-binary.fbx"), std::ios::binary);
    std::vector<char> prefix(64);
    source.read(prefix.data(), static_cast<std::streamsize>(prefix.size()));
    std::ofstream output(truncated, std::ios::binary);
    output.write(prefix.data(), source.gcount());
    output.close();

    RVX::Resource::FBXImporter importer;
    EXPECT_FALSE(importer.Import(truncated.string()).success);

    const fs::path corrupt = temporary.Get() / "corrupt.fbx";
    std::ofstream corruptOutput(corrupt, std::ios::binary);
    corruptOutput << "not an FBX document";
    corruptOutput.close();
    EXPECT_FALSE(importer.Import(corrupt.string()).success);
}

TEST(FBXImporterValidation, AssetPipelineRejectsDuplicateExtensions)
{
    EXPECT_EQ(RVX::Tools::AssetPipeline::GetAssetTypeFromExtension(".fbx"),
              RVX::Tools::AssetType::Model);
    RVX::Tools::AssetPipeline pipeline;
    pipeline.RegisterImporter(std::make_unique<RVX::Tools::ModelImporter>());
    EXPECT_THROW(
        pipeline.RegisterImporter(std::make_unique<RVX::Tools::ModelImporter>()),
        std::invalid_argument);
}
