#include "Common/FilesystemLinkTestSupport.h"
#include "Core/Diagnostics/ContentHash.h"
#include "Core/Log.h"
#include "Geometry/Asset/Mesh.h"
#include "Geometry/Asset/Model.h"
#include "Resource/Cooked/CookedMeshArtifactReader.h"
#include "Resource/Cooked/CookedModelArtifact.h"
#include "AssetImport/GLTFImporter.h"
#include "Resource/Loader/CookedModelLoader.h"
#include "Resource/ResourceManager.h"
#include "Resource/Types/ModelResource.h"
#include "Tools/AssetPipeline.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
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

    std::string HexEncode(std::string_view value)
    {
        constexpr char digits[] = "0123456789abcdef";
        std::string encoded(value.size() * 2, '\0');
        for (size_t index = 0; index < value.size(); ++index)
        {
            const auto byte = static_cast<RVX::uint8>(value[index]);
            encoded[index * 2] = digits[byte >> 4u];
            encoded[index * 2 + 1] = digits[byte & 0x0fu];
        }
        return encoded;
    }

    std::vector<RVX::uint8> RewriteModelPayload(
        const std::vector<RVX::uint8>& bytes,
        const std::function<void(std::vector<std::string>&)>& rewrite)
    {
        constexpr std::string_view payloadBegin =
            "RVX_MODEL_PAYLOAD_BEGIN\n";
        constexpr std::string_view payloadEnd =
            "RVX_MODEL_PAYLOAD_END\n";
        const std::string serialized(
            reinterpret_cast<const char*>(bytes.data()), bytes.size());
        const size_t marker = serialized.find(payloadBegin);
        const size_t end = serialized.find(payloadEnd, marker);
        if (marker == std::string::npos || end == std::string::npos)
            throw std::runtime_error("Model artifact payload is missing");

        const size_t offset = marker + payloadBegin.size();
        std::istringstream input(serialized.substr(offset, end - offset));
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(input, line))
            lines.push_back(std::move(line));
        rewrite(lines);

        std::string payload;
        for (const std::string& value : lines)
            payload += value + '\n';
        RVX::uint64 hash =
            RVX::Diagnostics::RVX_DIAGNOSTICS_FNV1A64_OFFSET_BASIS;
        for (const char value : payload)
        {
            hash ^= static_cast<RVX::uint8>(value);
            hash *= RVX::Diagnostics::RVX_DIAGNOSTICS_FNV1A64_PRIME;
        }
        std::ostringstream output;
        output << RVX::Resource::RVX_MODEL_ARTIFACT_MAGIC << '\n'
               << "schemaVersion="
               << RVX::Resource::RVX_MODEL_ARTIFACT_SCHEMA_VERSION << '\n'
               << "writerAbi="
               << RVX::Resource::RVX_MODEL_ARTIFACT_WRITER_ABI << '\n'
               << "flags="
               << (payload.find("animationPresent=1\n") != std::string::npos
                       ? 1
                       : 0)
               << '\n'
               << "contentHash="
               << RVX::Diagnostics::FormatContentHash(hash) << '\n'
               << "payloadSize=" << payload.size() << '\n'
               << payloadBegin << payload << payloadEnd;
        const std::string rebuilt = output.str();
        return {rebuilt.begin(), rebuilt.end()};
    }

    RVX::Resource::CookedModelArtifact MakeMinimalArtifact()
    {
        RVX::Resource::CookedModelArtifact artifact;
        artifact.logicalAssetId = "Models/Minimal.gltf";
        artifact.meshArtifactPath = "Minimal.rvdeps/meshes.rva";
        artifact.rootNode = std::make_shared<RVX::Node>("Root");
        artifact.bounds = RVX::AABB(
            RVX::Vec3(0.0f), RVX::Vec3(1.0f));
        return artifact;
    }

    bool RewriteField(std::vector<std::string>& lines,
                      std::string_view key,
                      std::string value)
    {
        const std::string prefix = std::string(key) + '=';
        for (std::string& line : lines)
        {
            if (line.starts_with(prefix))
            {
                line = prefix + std::move(value);
                return true;
            }
        }
        return false;
    }

    RVX::Resource::ModelHandle PrepareCookedModel(
        RVX::Resource::CookedModelLoader& loader,
        const fs::path& path,
        RVX::Resource::ResourceLoadError* outError = nullptr)
    {
        RVX::Resource::ResourceLoadPreparationContext context;
        context.requestedPath = path.string();
        context.resolvedPath = path.string();
        context.assetKey = RVX::Resource::MakeAssetKey(
            context.requestedPath, RVX::Resource::ResourceType::Model);
        context.resourceIdentityPath = context.assetKey.canonicalPath;
        context.rootResourceId = RVX::Resource::GenerateResourceId(
            context.resourceIdentityPath);

        RVX::Resource::PreparedResourceBundle bundle;
        RVX::Resource::ResourceLoadError error;
        if (!loader.Prepare(context, bundle, error))
        {
            if (outError)
                *outError = error;
            return {};
        }

        std::vector<RVX::Resource::IResource*> resources;
        resources.reserve(bundle.GetEntries().size());
        for (const RVX::Resource::PreparedResourceEntry& entry :
             bundle.GetEntries())
        {
            resources.push_back(entry.resource.Get());
        }
        RVX::Resource::ResourceCache publicationCache;
        if (!publicationCache.StorePreparedBatch(resources))
            return {};

        auto* model = dynamic_cast<RVX::Resource::ModelResource*>(
            bundle.GetRoot().Get());
        return RVX::Resource::ModelHandle(model);
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
        RVX::Resource::RVX_MODEL_ARTIFACT_MAGIC));

    RVX::Tools::ImportResult second = importer.Import(source, output);
    ASSERT_TRUE(second.success) << second.error;
    EXPECT_EQ(ReadBytes(output), firstBytes);

    RVX::Resource::CookedModelArtifact artifact;
    std::string error;
    ASSERT_TRUE(RVX::Resource::DeserializeCookedModelArtifact(
        firstBytes, artifact, error)) << error;
    ASSERT_TRUE(artifact.rootNode);
    EXPECT_FALSE(artifact.animationArtifactPath.has_value());
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

    RVX::Resource::CookedModelLoader loader;
    RVX::Resource::ModelHandle model = PrepareCookedModel(loader, output);
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

TEST(CookedModelProductValidation,
     StrictV2RejectsLegacyUnknownDuplicateAndEscapingMetadata)
{
    RVX::Resource::CookedModelArtifact source = MakeMinimalArtifact();
    std::vector<RVX::uint8> valid;
    std::string error;
    ASSERT_TRUE(RVX::Resource::SerializeCookedModelArtifact(
        source, valid, error)) << error;

    const auto expectRejected = [&error](const std::vector<RVX::uint8>& bytes)
    {
        RVX::Resource::CookedModelArtifact artifact;
        error.clear();
        EXPECT_FALSE(RVX::Resource::DeserializeCookedModelArtifact(
            bytes, artifact, error));
        EXPECT_FALSE(error.empty());
    };

    for (const std::string legacy : {
             "RVX_MODEL_" "PREBAKE_V1\n",
             "RVX_MODEL_" "PREBAKE_V2\n",
             "RVX_GLTF_MODEL_" "WITH_ANIMATION_V2\n"})
    {
        expectRejected(std::vector<RVX::uint8>(legacy.begin(), legacy.end()));
    }

    expectRejected(RewriteModelPayload(
        valid,
        [](std::vector<std::string>& lines)
        {
            lines.push_back("futureRequiredField=1");
        }));
    expectRejected(RewriteModelPayload(
        valid,
        [](std::vector<std::string>& lines)
        {
            const auto found = std::find_if(
                lines.begin(), lines.end(),
                [](const std::string& line)
                {
                    return line.starts_with("meshArtifact=");
                });
            ASSERT_NE(found, lines.end());
            lines.push_back(*found);
        }));

    for (const std::string invalidPath : {
             "../outside.rva", "/absolute/mesh.rva",
             "C:/outside/mesh.rva", "//server/share/mesh.rva",
             "folder\\mesh.rva", "folder/./mesh.rva"})
    {
        SCOPED_TRACE(invalidPath);
        expectRejected(RewriteModelPayload(
            valid,
            [&invalidPath](std::vector<std::string>& lines)
            {
                ASSERT_TRUE(RewriteField(
                    lines, "meshArtifact", HexEncode(invalidPath)));
            }));
        RVX::Resource::CookedModelArtifact invalid = source;
        invalid.meshArtifactPath = invalidPath;
        std::vector<RVX::uint8> ignored;
        EXPECT_FALSE(RVX::Resource::SerializeCookedModelArtifact(
            invalid, ignored, error));
    }
}

TEST(CookedModelProductValidation,
     StrictV2EnforcesCountStringDepthAndCheckedBoundaryLimits)
{
    RVX::Resource::CookedModelArtifact source = MakeMinimalArtifact();
    auto child = std::make_shared<RVX::Node>("Child");
    source.rootNode->AddChild(child);
    std::vector<RVX::uint8> valid;
    std::string error;
    ASSERT_TRUE(RVX::Resource::SerializeCookedModelArtifact(
        source, valid, error)) << error;

    const auto deserialize = [&error](const std::vector<RVX::uint8>& bytes)
    {
        RVX::Resource::CookedModelArtifact artifact;
        error.clear();
        return RVX::Resource::DeserializeCookedModelArtifact(
            bytes, artifact, error);
    };
    for (const auto& [key, boundary, tooMany] :
         std::vector<std::tuple<std::string, std::string, std::string>>{
             {"textureCount", "16384", "16385"},
             {"materialCount", "16384", "16385"},
             {"nodeCount", "1048576", "1048577"}})
    {
        EXPECT_FALSE(deserialize(RewriteModelPayload(
            valid,
            [&](std::vector<std::string>& lines)
            {
                ASSERT_TRUE(RewriteField(lines, key, boundary));
            })));
        EXPECT_EQ(error.find("exceeds the configured limit"),
                  std::string::npos);
        EXPECT_FALSE(deserialize(RewriteModelPayload(
            valid,
            [&](std::vector<std::string>& lines)
            {
                ASSERT_TRUE(RewriteField(lines, key, tooMany));
            })));
        EXPECT_NE(error.find("exceeds the configured limit"),
                  std::string::npos);
    }

    EXPECT_FALSE(deserialize(RewriteModelPayload(
        valid,
        [](std::vector<std::string>& lines)
        {
            ASSERT_TRUE(RewriteField(lines, "node.1.parent", "1"));
        })));

    std::vector<RVX::uint8> overflow = valid;
    std::string overflowText(
        reinterpret_cast<const char*>(overflow.data()), overflow.size());
    const std::string payloadSize = "payloadSize=";
    const size_t sizeOffset = overflowText.find(payloadSize);
    ASSERT_NE(sizeOffset, std::string::npos);
    const size_t sizeEnd = overflowText.find('\n', sizeOffset);
    overflowText.replace(sizeOffset + payloadSize.size(),
                         sizeEnd - sizeOffset - payloadSize.size(),
                         "18446744073709551615");
    overflow.assign(overflowText.begin(), overflowText.end());
    EXPECT_FALSE(deserialize(overflow));
    EXPECT_NE(error.find("boundary"), std::string::npos);

    RVX::Resource::CookedModelArtifact stringBoundary = MakeMinimalArtifact();
    stringBoundary.logicalAssetId.assign(1024u * 1024u, 'a');
    std::vector<RVX::uint8> boundaryBytes;
    ASSERT_TRUE(RVX::Resource::SerializeCookedModelArtifact(
        stringBoundary, boundaryBytes, error)) << error;
    EXPECT_TRUE(deserialize(boundaryBytes)) << error;
    stringBoundary.logicalAssetId.push_back('b');
    EXPECT_FALSE(RVX::Resource::SerializeCookedModelArtifact(
        stringBoundary, boundaryBytes, error));

    RVX::Resource::CookedModelArtifact depthBoundary = MakeMinimalArtifact();
    RVX::Node::Ptr cursor = depthBoundary.rootNode;
    for (RVX::uint32 depth = 1; depth < 512; ++depth)
    {
        auto next = std::make_shared<RVX::Node>("Node");
        cursor->AddChild(next);
        cursor = std::move(next);
    }
    ASSERT_TRUE(RVX::Resource::SerializeCookedModelArtifact(
        depthBoundary, boundaryBytes, error)) << error;
    EXPECT_TRUE(deserialize(boundaryBytes)) << error;
    cursor->AddChild(std::make_shared<RVX::Node>("TooDeep"));
    EXPECT_FALSE(RVX::Resource::SerializeCookedModelArtifact(
        depthBoundary, boundaryBytes, error));
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
    EXPECT_FALSE(RVX::Resource::SerializeCookedModelArtifact(
        artifact, escapedBytes, error));
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
    const fs::path linkPath = productDirectory / "linked";
    std::error_code linkError;
    ASSERT_TRUE(RVX::TestSupport::CreateDirectoryLink(
        outsideDirectory,
        linkPath,
        linkError)) << linkError.message();
    const RVX::TestSupport::ScopedDirectoryLink linkCleanup(linkPath);

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
    RVX::Resource::CookedModelLoader loader;
    EXPECT_FALSE(PrepareCookedModel(loader, escapedRoot));
    RVX::Log::Shutdown();
}
