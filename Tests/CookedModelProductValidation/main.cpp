#include "Common/FilesystemLinkTestSupport.h"
#include "Core/Diagnostics/ContentHash.h"
#include "Core/Hash/SHA256.h"
#include "Core/Log.h"
#include "Geometry/Asset/Mesh.h"
#include "Geometry/Asset/Model.h"
#include "Resource/Cooked/CookedMeshArtifactReader.h"
#include "Resource/Cooked/CookedMaterialArtifact.h"
#include "Resource/Cooked/CookedModelArtifact.h"
#include "AssetImport/GLTFImporter.h"
#include "Resource/Loader/CookedModelLoader.h"
#include "Resource/ResourceManager.h"
#include "Resource/Types/ModelResource.h"
#include "Resource/Types/TextureResource.h"
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
#include <set>
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

    std::vector<RVX::uint8> AppendModelHeaderFields(
        const std::vector<RVX::uint8>& bytes,
        const std::vector<std::string>& fields)
    {
        constexpr std::string_view payloadBegin =
            "RVX_MODEL_PAYLOAD_BEGIN\n";
        std::string serialized(
            reinterpret_cast<const char*>(bytes.data()), bytes.size());
        const size_t marker = serialized.find(payloadBegin);
        if (marker == std::string::npos)
            throw std::runtime_error("Model artifact payload is missing");

        std::string appended;
        for (const std::string& field : fields)
            appended += field + '\n';
        serialized.insert(marker, appended);
        return {serialized.begin(), serialized.end()};
    }

    RVX::Resource::CookedModelArtifact MakeMinimalArtifact()
    {
        RVX::Resource::CookedModelArtifact artifact;
        artifact.logicalAssetId = "Models/Minimal.gltf";
        artifact.meshArtifactPath = "Minimal.rvdeps/meshes.rva";
        artifact.materialArtifacts.push_back({
            "Minimal.rvdeps/materials/material_0.rvmat",
            RVX::Hash::ComputeSHA256("minimal-material-0")});
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

    bool PrepareCookedModelBundle(
        RVX::Resource::CookedModelLoader& loader,
        const fs::path& path,
        RVX::Resource::PreparedResourceBundle& outBundle,
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

        RVX::Resource::ResourceLoadError error;
        if (!loader.Prepare(context, outBundle, error))
        {
            if (outError)
                *outError = error;
            return false;
        }

        return true;
    }

    RVX::Resource::ModelHandle PrepareCookedModel(
        RVX::Resource::CookedModelLoader& loader,
        const fs::path& path,
        RVX::Resource::ResourceLoadError* outError = nullptr)
    {
        RVX::Resource::PreparedResourceBundle bundle;
        if (!PrepareCookedModelBundle(loader, path, bundle, outError))
            return {};

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

    class ResourceManagerGuard final
    {
    public:
        ResourceManagerGuard(
            const RVX::Resource::ResourceManagerConfig& config,
            RVX::Resource::ResourceManagerDependencies dependencies)
        {
            m_manager.Initialize(config, std::move(dependencies));
        }

        ~ResourceManagerGuard()
        {
            m_manager.Shutdown();
        }

        RVX::Resource::ResourceManager& Get() noexcept
        {
            return m_manager;
        }

    private:
        RVX::Resource::ResourceManager m_manager;
    };

    class LogGuard final
    {
    public:
        LogGuard()
        {
            RVX::Log::Initialize();
        }

        ~LogGuard()
        {
            RVX::Log::Shutdown();
        }
    };

    RVX::Resource::ResourceContentIdentity MakeCookedArtifactIdentity(
        const std::vector<RVX::uint8>& bytes)
    {
        RVX::Resource::ResourceContentIdentity identity;
        identity.schemaVersion =
            RVX::Resource::RVX_RESOURCE_CONTENT_IDENTITY_SCHEMA_VERSION;
        identity.domain = RVX::Resource::ResourceContentIdentityDomain::CookedArtifact;
        identity.scope =
            RVX::Resource::ResourceContentIdentityScope::SelfContainedArtifact;
        identity.algorithm = RVX::Resource::ResourceContentHashAlgorithm::SHA256;
        identity.digest = RVX::Hash::FormatSHA256Digest(
            RVX::Hash::ComputeSHA256(bytes.data(), bytes.size()));
        identity.byteCount = bytes.size();
        identity.fileCount = 1;
        return identity;
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
    ASSERT_EQ(artifact.materialArtifacts.size(), 5u);
    std::set<std::string> textureDependencies;
    for (const RVX::Resource::CookedMaterialArtifactReference& reference :
         artifact.materialArtifacts)
    {
        const fs::path materialPath =
            output.parent_path() / reference.packageRelativePath;
        const std::vector<RVX::uint8> materialBytes = ReadBytes(materialPath);
        ASSERT_FALSE(materialBytes.empty()) << materialPath;
        EXPECT_EQ(
            RVX::Hash::ComputeSHA256(
                materialBytes.data(), materialBytes.size()),
            reference.expectedContentHash);
        RVX::Resource::CookedMaterialArtifact materialArtifact;
        ASSERT_TRUE(RVX::Resource::DeserializeCookedMaterialArtifact(
            materialBytes, materialArtifact, error)) << error;
        for (const RVX::Resource::CookedMaterialTextureBinding& binding :
             materialArtifact.textureBindings)
        {
            textureDependencies.insert(binding.artifactPath);
        }
    }
    EXPECT_EQ(textureDependencies.size(), 1u);

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
     ResourceManagerVerifiesOnlyObservedRootModelArtifactIdentity)
{
    const fs::path source =
        fs::path(RVX_SOURCE_DIR) /
        "Tests/Fixtures/ModelViewer/R7Triangle.gltf";
    TemporaryDirectory temporary;
    const fs::path output = temporary.Get() / "R7Triangle.rva";
    RVX::Tools::ModelImporter importer;
    ASSERT_TRUE(importer.Import(source, output).success);

    const RVX::Resource::ResourceContentIdentity expected =
        MakeCookedArtifactIdentity(ReadBytes(output));
    ASSERT_TRUE(expected.IsValid());

    RVX::Resource::ResourceManagerConfig config;
    config.asyncThreadCount = 0;
    config.runtimePolicy.mode = RVX::Resource::ResourceRuntimeMode::CookedRuntime;
    config.runtimePolicy.allowSourceAssetReads = false;
    config.runtimePolicy.requireCookedArtifacts = true;
    config.runtimePolicy.cookedRoot = temporary.Get().string();
    LogGuard logGuard;
    RVX::Resource::ResourceManagerDependencies dependencies =
        RVX::Resource::CreateRuntimeResourceManagerDependencies();
    ResourceManagerGuard guard(
        config,
        std::move(dependencies));
    ASSERT_TRUE(guard.Get().IsInitialized());

    RVX::Resource::ResourceLoadOptions verifiedOptions;
    verifiedOptions.expectedContentIdentity = expected;
    auto verified = guard.Get().RequestAsync<RVX::Resource::ModelResource>(
        "cooked://R7Triangle.rva", verifiedOptions);
    ASSERT_TRUE(verified);
    guard.Get().ProcessCompletedLoads();
    const RVX::Resource::ModelHandle model = verified.TryGet();
    ASSERT_TRUE(model) << verified.GetSnapshot().error.message;
    const RVX::Resource::ResourceContentVerificationReceipt& receipt =
        model->GetContentVerificationReceipt();
    EXPECT_TRUE(receipt.IsVerified());
    EXPECT_EQ(receipt.expected, expected);
    EXPECT_EQ(receipt.observed, expected);
    EXPECT_EQ(receipt.observed.scope,
              RVX::Resource::ResourceContentIdentityScope::SelfContainedArtifact);
    EXPECT_EQ(receipt.observed.fileCount, 1u);

    RVX::Resource::ResourceContentIdentity mismatch = expected;
    mismatch.digest[0] = mismatch.digest[0] == '0' ? '1' : '0';
    RVX::Resource::ResourceLoadOptions mismatchOptions;
    mismatchOptions.expectedContentIdentity = mismatch;
    auto mismatched = guard.Get().RequestAsync<RVX::Resource::ModelResource>(
        "cooked://R7Triangle.rva", mismatchOptions);
    ASSERT_TRUE(mismatched);
    guard.Get().ProcessCompletedLoads();
    EXPECT_EQ(mismatched.GetSnapshot().state,
              RVX::Resource::ResourceLoadState::Failed);
    EXPECT_EQ(mismatched.GetSnapshot().error.code,
              RVX::Resource::ResourceLoadErrorCode::ContentIdentityMismatch);
    EXPECT_FALSE(mismatched.TryGet());
}

TEST(CookedModelProductValidation,
     SharedCookedTextureDependencyProducesOneHandleAndOneBundleEntry)
{
    const fs::path source =
        fs::path(RVX_SOURCE_DIR) /
        "Tests/Fixtures/Samples/PBRMaterialTextureCube.gltf";
    TemporaryDirectory temporary;
    const fs::path output = temporary.Get() / "PBRMaterialTextureCube.rva";
    RVX::Tools::ModelImporter importer;
    ASSERT_TRUE(importer.Import(source, output).success);

    RVX::Resource::CookedModelLoader loader;
    RVX::Resource::PreparedResourceBundle bundle;
    RVX::Resource::ResourceLoadError error;
    ASSERT_TRUE(PrepareCookedModelBundle(loader, output, bundle, &error))
        << error.message;
    ASSERT_TRUE(bundle.IsValid());

    auto* model = dynamic_cast<RVX::Resource::ModelResource*>(
        bundle.GetRoot().Get());
    ASSERT_NE(model, nullptr);
    ASSERT_GE(model->GetMaterialCount(), 2u);

    size_t materialsUsingSharedTexture = 0;
    RVX::Resource::TextureHandle sharedTexture;
    for (size_t index = 0; index < model->GetMaterialCount(); ++index)
    {
        const RVX::Resource::MaterialHandle material = model->GetMaterial(index);
        ASSERT_TRUE(material);
        for (const auto& [slot, texture] : material->GetTextures())
        {
            (void)slot;
            if (!texture)
                continue;
            ++materialsUsingSharedTexture;
            if (!sharedTexture)
                sharedTexture = texture;
            else
                EXPECT_EQ(texture.Get(), sharedTexture.Get());
        }
    }
    EXPECT_GE(materialsUsingSharedTexture, 2u);
    EXPECT_TRUE(sharedTexture);

    size_t textureDependencyCount = 0;
    for (const RVX::Resource::PreparedResourceEntry& entry : bundle.GetEntries())
    {
        if (!entry.isRoot &&
            dynamic_cast<RVX::Resource::TextureResource*>(entry.resource.Get()) != nullptr)
        {
            ++textureDependencyCount;
        }
    }
    EXPECT_EQ(textureDependencyCount, 1u);
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
     StrictV3RoundTripsContentBoundOrderedMaterialDependencies)
{
    RVX::Resource::CookedModelArtifact source = MakeMinimalArtifact();
    source.materialArtifacts.push_back({
        "Minimal.rvdeps/materials/material_1.rvmat",
        RVX::Hash::ComputeSHA256("minimal-material-1")});
    source.rootNode->SetMeshIndex(0);
    source.rootNode->SetMeshIndices({0, 1});
    source.rootNode->SetMaterialIndices({1, 0});

    std::vector<RVX::uint8> bytes;
    std::string error;
    ASSERT_TRUE(RVX::Resource::SerializeCookedModelArtifact(
        source, bytes, error)) << error;

    RVX::Resource::CookedModelArtifact decoded;
    ASSERT_TRUE(RVX::Resource::DeserializeCookedModelArtifact(
        bytes, decoded, error)) << error;
    ASSERT_EQ(decoded.materialArtifacts.size(), 2u);
    EXPECT_EQ(decoded.materialArtifacts[0].packageRelativePath,
              source.materialArtifacts[0].packageRelativePath);
    EXPECT_EQ(decoded.materialArtifacts[0].expectedContentHash,
              source.materialArtifacts[0].expectedContentHash);
    EXPECT_EQ(decoded.materialArtifacts[1].packageRelativePath,
              source.materialArtifacts[1].packageRelativePath);
    EXPECT_EQ(decoded.materialArtifacts[1].expectedContentHash,
              source.materialArtifacts[1].expectedContentHash);
    ASSERT_TRUE(decoded.rootNode);
    EXPECT_EQ(decoded.rootNode->GetMaterialIndices(),
              (std::vector<int>{1, 0}));

    const std::string firstMaterialPath =
        source.materialArtifacts[0].packageRelativePath;
    const std::vector<RVX::uint8> duplicatePathBytes = RewriteModelPayload(
        bytes,
        [&firstMaterialPath](std::vector<std::string>& lines)
        {
            ASSERT_TRUE(RewriteField(
                lines,
                "materialArtifact.1.path",
                HexEncode(firstMaterialPath)));
        });
    EXPECT_FALSE(RVX::Resource::DeserializeCookedModelArtifact(
        duplicatePathBytes, decoded, error));

    source.materialArtifacts[1].packageRelativePath = firstMaterialPath;
    EXPECT_FALSE(RVX::Resource::SerializeCookedModelArtifact(
        source, bytes, error));
    source.materialArtifacts[1].packageRelativePath =
        "Minimal.rvdeps/materials/material_1.rvmat";
    source.rootNode->SetMaterialIndices({2, 0});
    EXPECT_FALSE(RVX::Resource::SerializeCookedModelArtifact(
        source, bytes, error));
}

TEST(CookedModelProductValidation,
     StrictV3RejectsLegacyUnknownDuplicateAndEscapingMetadata)
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
             "RVX_MODEL_ARTIFACT_" "V2\n",
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

    for (const std::string invalidPath : {
             "../outside.rvmat", "/absolute/material.rvmat",
             "C:/outside/material.rvmat", "//server/share/material.rvmat",
             "folder\\material.rvmat", "folder/./material.rvmat",
             "folder/material.rva", "folder/material.RVMAT"})
    {
        SCOPED_TRACE(invalidPath);
        expectRejected(RewriteModelPayload(
            valid,
            [&invalidPath](std::vector<std::string>& lines)
            {
                ASSERT_TRUE(RewriteField(
                    lines,
                    "materialArtifact.0.path",
                    HexEncode(invalidPath)));
            }));
        RVX::Resource::CookedModelArtifact invalid = source;
        invalid.materialArtifacts[0].packageRelativePath = invalidPath;
        std::vector<RVX::uint8> ignored;
        EXPECT_FALSE(RVX::Resource::SerializeCookedModelArtifact(
            invalid, ignored, error));
    }

    RVX::Resource::CookedModelArtifact zeroHash = source;
    zeroHash.materialArtifacts[0].expectedContentHash = {};
    std::vector<RVX::uint8> ignored;
    EXPECT_FALSE(RVX::Resource::SerializeCookedModelArtifact(
        zeroHash, ignored, error));

    expectRejected(RewriteModelPayload(
        valid,
        [](std::vector<std::string>& lines)
        {
            ASSERT_TRUE(RewriteField(
                lines,
                "materialArtifact.0.contentHash",
                std::string(RVX::Hash::RVX_SHA256_DIGEST_SIZE * 2u, '0')));
        }));
    expectRejected(RewriteModelPayload(
        valid,
        [](std::vector<std::string>& lines)
        {
            ASSERT_TRUE(RewriteField(
                lines,
                "materialArtifact.0.contentHash",
                std::string(RVX::Hash::RVX_SHA256_DIGEST_SIZE * 2u, 'g')));
        }));
    expectRejected(RewriteModelPayload(
        valid,
        [](std::vector<std::string>& lines)
        {
            ASSERT_TRUE(RewriteField(
                lines,
                "materialArtifact.0.contentHash",
                std::string(RVX::Hash::RVX_SHA256_DIGEST_SIZE * 2u - 1u,
                            'a')));
        }));
}

TEST(CookedModelProductValidation,
     StrictV3EnforcesCountStringDepthAndCheckedBoundaryLimits)
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
             {"materialArtifactCount", "16384", "16385"},
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

TEST(CookedModelProductValidation,
     StrictV3BoundsRawFieldsBeforeBuildingTheLookupTable)
{
    RVX::Resource::CookedModelArtifact source = MakeMinimalArtifact();
    std::vector<RVX::uint8> valid;
    std::string error;
    ASSERT_TRUE(RVX::Resource::SerializeCookedModelArtifact(
        source, valid, error)) << error;

    RVX::Resource::CookedModelArtifact decoded;
    ASSERT_TRUE(RVX::Resource::DeserializeCookedModelArtifact(
        valid, decoded, error)) << error;

    const auto expectRejected = [&error](const std::vector<RVX::uint8>& bytes,
                                         std::string_view expectedDiagnostic)
    {
        RVX::Resource::CookedModelArtifact artifact;
        error.clear();
        EXPECT_FALSE(RVX::Resource::DeserializeCookedModelArtifact(
            bytes, artifact, error));
        EXPECT_NE(error.find(expectedDiagnostic), std::string::npos)
            << error;
    };

    expectRejected(RewriteModelPayload(
        valid,
        [](std::vector<std::string>& lines)
        {
            lines.push_back(std::string(257u, 'k') + "=1");
        }),
        "key or value exceeds");

    expectRejected(RewriteModelPayload(
        valid,
        [](std::vector<std::string>& lines)
        {
            lines.push_back("unknown=" +
                            std::string(2u * 1024u * 1024u + 1u, 'v'));
        }),
        "key or value exceeds");

    std::vector<std::string> tooManyHeaderFields;
    for (size_t index = 0; index < 12u; ++index)
        tooManyHeaderFields.push_back("unknown" + std::to_string(index) + "=1");
    expectRejected(AppendModelHeaderFields(valid, tooManyHeaderFields),
                   "field count exceeds");

    const std::vector<std::string> oversizedHeaderFields{
        "unknownA=" + std::string(24576u, 'a'),
        "unknownB=" + std::string(24576u, 'b'),
        "unknownC=" + std::string(24576u, 'c')};
    expectRejected(AppendModelHeaderFields(valid, oversizedHeaderFields),
                   "aggregate raw field bytes");
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

TEST(CookedModelProductValidation,
     GLTFTextureTransformPreservesSupportedValuesAndRejectsMalformedValues)
{
    LogGuard logGuard;
    const fs::path baseSource =
        fs::path(RVX_SOURCE_DIR) /
        "Tests/Fixtures/ModelViewer/R7Triangle.gltf";
    std::ifstream baseFile(baseSource, std::ios::binary);
    ASSERT_TRUE(baseFile.is_open());
    std::string gltf{std::istreambuf_iterator<char>(baseFile),
                     std::istreambuf_iterator<char>()};

    const size_t uv0Attribute = gltf.find("\"TEXCOORD_0\": 2");
    ASSERT_NE(uv0Attribute, std::string::npos);
    gltf.replace(
        uv0Attribute,
        std::string_view("\"TEXCOORD_0\": 2").size(),
        "\"TEXCOORD_0\": 2,\n            \"TEXCOORD_1\": 2");

    const size_t materialsStart = gltf.find("  \"materials\": ");
    const size_t buffersStart = gltf.find("  \"buffers\": ");
    ASSERT_NE(materialsStart, std::string::npos);
    ASSERT_NE(buffersStart, std::string::npos);
    ASSERT_LT(materialsStart, buffersStart);

    constexpr std::string_view materialAndTextureSection = R"json(  "materials": [
    {
      "name": "TextureTransformMaterial",
      "pbrMetallicRoughness": {
        "baseColorTexture": {
          "index": 0,
          "texCoord": 0,
          "extensions": {
            "KHR_texture_transform": {
              "offset": [0.125, -0.25],
              "scale": [-2.0, 0.5],
              "rotation": 0.25,
              "texCoord": 1,
              "extensions": {},
              "extras": { "authoringTag": "fixture-metadata" },
              "futureOptionalProperty": { "ignored": true }
            }
          }
        },
        "metallicRoughnessTexture": {
          "index": 1,
          "texCoord": 1,
          "extensions": {
            "KHR_texture_transform": {
              "offset": [-0.5, 0.75],
              "scale": [0.25, -1.5],
              "rotation": -0.5
            }
          }
        }
      },
      "normalTexture": {
        "index": 2,
        "texCoord": 1,
        "extensions": {
          "KHR_texture_transform": {
            "offset": [0.0, 0.5],
            "scale": [1.0, 1.0],
            "rotation": 0.0
          }
        }
      },
      "occlusionTexture": {
        "index": 3,
        "texCoord": 1,
        "extensions": {
          "KHR_texture_transform": {
            "offset": [0.25, 0.0],
            "scale": [1.0, 1.0],
            "texCoord": 0
          }
        }
      },
      "emissiveTexture": {
        "index": 4,
        "texCoord": 0,
        "extensions": {
          "KHR_texture_transform": {
            "offset": [0.0, -0.25],
            "scale": [1.0, -1.0],
            "rotation": -0.75
          }
        }
      }
    }
  ],
  "textures": [
    { "source": 0 },
    { "source": 1 },
    { "source": 2 },
    { "source": 3 },
    { "source": 4 }
  ],
  "images": [
    { "uri": "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/p9sAAAAASUVORK5CYII=" },
    { "uri": "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/p9sAAAAASUVORK5CYII=" },
    { "uri": "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/p9sAAAAASUVORK5CYII=" },
    { "uri": "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/p9sAAAAASUVORK5CYII=" },
    { "uri": "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/p9sAAAAASUVORK5CYII=" }
  ],
  "extensionsUsed": ["KHR_texture_transform"],
  "extensionsRequired": ["KHR_texture_transform"],
)json";
    gltf.replace(materialsStart,
                 buffersStart - materialsStart,
                 materialAndTextureSection);

    const auto writeVariant =
        [](const fs::path& path, const std::string& source)
    {
        std::ofstream output(path, std::ios::binary);
        output.write(source.data(), static_cast<std::streamsize>(source.size()));
        return output.good();
    };

    TemporaryDirectory temporary;
    const fs::path validSource = temporary.Get() / "TextureTransform.gltf";
    ASSERT_TRUE(writeVariant(validSource, gltf));

    RVX::Resource::GLTFImporter importer;
    const RVX::Resource::GLTFImportResult valid = importer.Import(validSource.string());
    ASSERT_TRUE(valid.success) << valid.errorMessage;
    ASSERT_EQ(valid.materials.size(), 1u);
    ASSERT_NE(valid.materials[0], nullptr);

    const RVX::Material& material = *valid.materials[0];
    const auto expectTexture = [](const std::optional<RVX::TextureInfo>& texture,
                                  RVX::int32 uvSet,
                                  const RVX::Vec2& offset,
                                  const RVX::Vec2& scale,
                                  float rotation)
    {
        ASSERT_TRUE(texture.has_value());
        EXPECT_EQ(texture->uvSet, uvSet);
        EXPECT_FLOAT_EQ(texture->offset.x, offset.x);
        EXPECT_FLOAT_EQ(texture->offset.y, offset.y);
        EXPECT_FLOAT_EQ(texture->scale.x, scale.x);
        EXPECT_FLOAT_EQ(texture->scale.y, scale.y);
        EXPECT_FLOAT_EQ(texture->rotation, rotation);
    };
    expectTexture(material.GetBaseColorTexture(),
                  1,
                  RVX::Vec2(0.125f, -0.25f),
                  RVX::Vec2(-2.0f, 0.5f),
                  0.25f);
    expectTexture(material.GetMetallicRoughnessTexture(),
                  1,
                  RVX::Vec2(-0.5f, 0.75f),
                  RVX::Vec2(0.25f, -1.5f),
                  -0.5f);
    expectTexture(material.GetNormalTexture(),
                  1,
                  RVX::Vec2(0.0f, 0.5f),
                  RVX::Vec2(1.0f),
                  0.0f);
    expectTexture(material.GetOcclusionTexture(),
                  0,
                  RVX::Vec2(0.25f, 0.0f),
                  RVX::Vec2(1.0f),
                  0.0f);
    expectTexture(material.GetEmissiveTexture(),
                  0,
                  RVX::Vec2(0.0f, -0.25f),
                  RVX::Vec2(1.0f, -1.0f),
                  -0.75f);

    std::string conflictingInterpretation = gltf;
    const size_t dataImage =
        conflictingInterpretation.find("{ \"source\": 1 }");
    ASSERT_NE(dataImage, std::string::npos);
    conflictingInterpretation.replace(
        dataImage,
        std::string_view("{ \"source\": 1 }").size(),
        "{ \"source\": 0 }");
    const fs::path conflictingSource =
        temporary.Get() / "ConflictingTextureInterpretation.gltf";
    ASSERT_TRUE(writeVariant(conflictingSource, conflictingInterpretation));
    const RVX::Resource::GLTFImportResult conflicting =
        importer.Import(conflictingSource.string());
    EXPECT_FALSE(conflicting.success);
    EXPECT_NE(conflicting.errorMessage.find(
                  "incompatible texture interpretations"),
              std::string::npos)
        << conflicting.errorMessage;

    const fs::path cookedOutput = temporary.Get() / "TextureTransform.rva";
    RVX::Tools::ModelImporter productImporter;
    const RVX::Tools::ImportResult cooked =
        productImporter.Import(validSource, cookedOutput);
    ASSERT_TRUE(cooked.success) << cooked.error;
    RVX::Resource::CookedModelArtifact cookedModel;
    std::string cookedError;
    ASSERT_TRUE(RVX::Resource::DeserializeCookedModelArtifact(
        ReadBytes(cookedOutput), cookedModel, cookedError)) << cookedError;
    ASSERT_EQ(cookedModel.materialArtifacts.size(), 1u);
    const fs::path cookedMaterialPath =
        cookedOutput.parent_path() /
        cookedModel.materialArtifacts.front().packageRelativePath;
    RVX::Resource::CookedMaterialArtifact cookedMaterial;
    ASSERT_TRUE(RVX::Resource::DeserializeCookedMaterialArtifact(
        ReadBytes(cookedMaterialPath), cookedMaterial, cookedError))
        << cookedError;
    const auto baseColorBinding = std::find_if(
        cookedMaterial.textureBindings.begin(),
        cookedMaterial.textureBindings.end(),
        [](const RVX::Resource::CookedMaterialTextureBinding& binding)
        {
            return binding.definition.slot ==
                   RVX::MaterialTextureSlot::BaseColor;
        });
    ASSERT_NE(baseColorBinding, cookedMaterial.textureBindings.end());
    EXPECT_EQ(baseColorBinding->definition.uvSet, 1u);
    EXPECT_EQ(baseColorBinding->definition.offset,
              RVX::Vec2(0.125f, -0.25f));
    EXPECT_EQ(baseColorBinding->definition.scale,
              RVX::Vec2(-2.0f, 0.5f));
    EXPECT_FLOAT_EQ(baseColorBinding->definition.rotation, 0.25f);

    RVX::Resource::GLTFImportOptions flippedOptions;
    flippedOptions.flipUVs = true;
    const RVX::Resource::GLTFImportResult flipped =
        importer.Import(validSource.string(), flippedOptions);
    EXPECT_FALSE(flipped.success);
    EXPECT_NE(flipped.errorMessage.find(
                  "flipUVs cannot be combined with KHR_texture_transform"),
              std::string::npos)
        << flipped.errorMessage;

    std::string unsupportedRequired = gltf;
    const size_t requiredExtension = unsupportedRequired.find(
        "\"extensionsRequired\": [\"KHR_texture_transform\"]");
    ASSERT_NE(requiredExtension, std::string::npos);
    unsupportedRequired.replace(
        requiredExtension,
        std::string_view(
            "\"extensionsRequired\": [\"KHR_texture_transform\"]").size(),
        "\"extensionsRequired\": [\"KHR_materials_sheen\"]");
    const fs::path unsupportedRequiredSource =
        temporary.Get() / "UnsupportedRequiredExtension.gltf";
    ASSERT_TRUE(writeVariant(unsupportedRequiredSource, unsupportedRequired));
    const RVX::Tools::ImportResult unsupportedRequiredCook =
        productImporter.Import(
            unsupportedRequiredSource,
            temporary.Get() / "UnsupportedRequiredExtension.rva");
    EXPECT_FALSE(unsupportedRequiredCook.success);
    EXPECT_NE(unsupportedRequiredCook.error.find(
                  "cannot satisfy required glTF extension: "
                  "KHR_materials_sheen"),
              std::string::npos)
        << unsupportedRequiredCook.error;

    std::string malformed = gltf;
    const size_t validOffset = malformed.find("[0.125, -0.25]");
    ASSERT_NE(validOffset, std::string::npos);
    malformed.replace(validOffset,
                      std::string_view("[0.125, -0.25]").size(),
                      "[0.125]");
    const fs::path malformedSource = temporary.Get() / "MalformedTransform.gltf";
    ASSERT_TRUE(writeVariant(malformedSource, malformed));
    const RVX::Resource::GLTFImportResult malformedResult =
        importer.Import(malformedSource.string());
    EXPECT_FALSE(malformedResult.success);
    EXPECT_NE(malformedResult.errorMessage.find("KHR_texture_transform.offset"),
              std::string::npos)
        << malformedResult.errorMessage;

    std::string unsupportedUvSet = gltf;
    const size_t transformUvSet = unsupportedUvSet.find("\"texCoord\": 1");
    ASSERT_NE(transformUvSet, std::string::npos);
    unsupportedUvSet.replace(transformUvSet,
                             std::string_view("\"texCoord\": 1").size(),
                             "\"texCoord\": 2");
    const fs::path unsupportedUvSource = temporary.Get() / "UnsupportedUvSet.gltf";
    ASSERT_TRUE(writeVariant(unsupportedUvSource, unsupportedUvSet));
    const RVX::Resource::GLTFImportResult unsupportedUvResult =
        importer.Import(unsupportedUvSource.string());
    EXPECT_FALSE(unsupportedUvResult.success);
    EXPECT_NE(unsupportedUvResult.errorMessage.find(
                  "KHR_texture_transform.texCoord must be UV set 0 or 1"),
              std::string::npos)
        << unsupportedUvResult.errorMessage;

    std::string zeroScale = gltf;
    const size_t normalScale = zeroScale.find("\"scale\": [1.0, 1.0]");
    ASSERT_NE(normalScale, std::string::npos);
    zeroScale.replace(normalScale,
                      std::string_view("\"scale\": [1.0, 1.0]").size(),
                      "\"scale\": [0.0, 1.0]");
    const fs::path zeroScaleSource = temporary.Get() / "ZeroScale.gltf";
    ASSERT_TRUE(writeVariant(zeroScaleSource, zeroScale));
    const RVX::Resource::GLTFImportResult zeroScaleResult =
        importer.Import(zeroScaleSource.string());
    EXPECT_FALSE(zeroScaleResult.success);
    EXPECT_NE(zeroScaleResult.errorMessage.find(
                  "normal texture KHR_texture_transform.scale"),
              std::string::npos)
        << zeroScaleResult.errorMessage;
    const RVX::Tools::ImportResult zeroScaleCook = productImporter.Import(
        zeroScaleSource, temporary.Get() / "ZeroScale.rva");
    EXPECT_FALSE(zeroScaleCook.success);
    EXPECT_NE(zeroScaleCook.error.find(
                  "normal texture KHR_texture_transform.scale"),
              std::string::npos)
        << zeroScaleCook.error;

    std::string rotatedNormal = gltf;
    const size_t normalRotation = rotatedNormal.find("\"rotation\": 0.0");
    ASSERT_NE(normalRotation, std::string::npos);
    rotatedNormal.replace(
        normalRotation,
        std::string_view("\"rotation\": 0.0").size(),
        "\"rotation\": 0.25");
    const fs::path rotatedNormalSource =
        temporary.Get() / "RotatedNormal.gltf";
    ASSERT_TRUE(writeVariant(rotatedNormalSource, rotatedNormal));
    const RVX::Resource::GLTFImportResult rotatedNormalResult =
        importer.Import(rotatedNormalSource.string());
    EXPECT_FALSE(rotatedNormalResult.success);
    EXPECT_NE(rotatedNormalResult.errorMessage.find(
                  "normal texture KHR_texture_transform.rotation"),
              std::string::npos)
        << rotatedNormalResult.errorMessage;

}

TEST(CookedModelProductValidation,
     GLTFImporterPreservesKhronosPbrExtensionFactors)
{
    const fs::path source =
        fs::path(RVX_SOURCE_DIR) /
        "Tests/Fixtures/ModelViewer/R7Triangle.gltf";
    const std::vector<RVX::uint8> sourceBytes = ReadBytes(source);
    std::string gltf(
        reinterpret_cast<const char*>(sourceBytes.data()),
        sourceBytes.size());

    const std::string materialNeedle =
        "      \"name\": \"R7Red\",\n"
        "      \"pbrMetallicRoughness\": {";
    const std::string materialReplacement =
        "      \"name\": \"R7Red\",\n"
        "      \"emissiveFactor\": [1.0, 0.5, 0.25],\n"
        "      \"extensions\": {\n"
        "        \"KHR_materials_specular\": {\n"
        "          \"specularFactor\": 0.25,\n"
        "          \"specularColorFactor\": [1.25, 0.75, 0.5]\n"
        "        },\n"
        "        \"KHR_materials_clearcoat\": {\n"
        "          \"clearcoatFactor\": 0.8,\n"
        "          \"clearcoatRoughnessFactor\": 0.2\n"
        "        },\n"
        "        \"KHR_materials_transmission\": {\n"
        "          \"transmissionFactor\": 0.6\n"
        "        },\n"
        "        \"KHR_materials_volume\": {\n"
        "          \"thicknessFactor\": 0.35,\n"
        "          \"attenuationDistance\": 2.5,\n"
        "          \"attenuationColor\": [0.8, 0.9, 1.0]\n"
        "        },\n"
        "        \"KHR_materials_ior\": { \"ior\": 1.33 },\n"
        "        \"KHR_materials_emissive_strength\": {\n"
        "          \"emissiveStrength\": 4.0\n"
        "        }\n"
        "      },\n"
        "      \"pbrMetallicRoughness\": {";
    const size_t materialPosition = gltf.find(materialNeedle);
    ASSERT_NE(materialPosition, std::string::npos);
    gltf.replace(
        materialPosition, materialNeedle.size(), materialReplacement);

    const std::string buffersNeedle = "  \"buffers\": [";
    const size_t buffersPosition = gltf.find(buffersNeedle);
    ASSERT_NE(buffersPosition, std::string::npos);
    gltf.insert(
        buffersPosition,
        "  \"extensionsUsed\": [\n"
        "    \"KHR_materials_specular\",\n"
        "    \"KHR_materials_clearcoat\",\n"
        "    \"KHR_materials_transmission\",\n"
        "    \"KHR_materials_volume\",\n"
        "    \"KHR_materials_ior\",\n"
        "    \"KHR_materials_emissive_strength\"\n"
        "  ],\n"
        "  \"extensionsRequired\": [\n"
        "    \"KHR_materials_specular\",\n"
        "    \"KHR_materials_clearcoat\",\n"
        "    \"KHR_materials_transmission\",\n"
        "    \"KHR_materials_volume\",\n"
        "    \"KHR_materials_ior\",\n"
        "    \"KHR_materials_emissive_strength\"\n"
        "  ],\n");

    TemporaryDirectory temporary;
    const fs::path validPath = temporary.Get() / "PbrExtensions.gltf";
    {
        std::ofstream output(validPath, std::ios::binary);
        output.write(gltf.data(), static_cast<std::streamsize>(gltf.size()));
        ASSERT_TRUE(output.good());
    }

    RVX::Resource::GLTFImporter importer;
    const RVX::Resource::GLTFImportResult imported =
        importer.Import(validPath.string());
    ASSERT_TRUE(imported.success) << imported.errorMessage;
    ASSERT_EQ(imported.materials.size(), 1u);
    ASSERT_NE(imported.materials[0], nullptr);
    const RVX::Material& material = *imported.materials[0];
    EXPECT_FLOAT_EQ(material.GetSpecularFactor(), 0.25f);
    EXPECT_EQ(material.GetSpecularColor(), RVX::Vec3(1.25f, 0.75f, 0.5f));
    EXPECT_FLOAT_EQ(material.GetClearcoatFactor(), 0.8f);
    EXPECT_FLOAT_EQ(material.GetClearcoatRoughness(), 0.2f);
    EXPECT_FLOAT_EQ(material.GetTransmissionFactor(), 0.6f);
    EXPECT_FLOAT_EQ(material.GetThicknessFactor(), 0.35f);
    EXPECT_FLOAT_EQ(material.GetAttenuationDistance(), 2.5f);
    EXPECT_EQ(material.GetAttenuationColor(), RVX::Vec3(0.8f, 0.9f, 1.0f));
    EXPECT_FLOAT_EQ(material.GetIndexOfRefraction(), 1.33f);
    EXPECT_FLOAT_EQ(material.GetEmissiveStrength(), 4.0f);

    const fs::path cookedPath = temporary.Get() / "PbrExtensions.rva";
    RVX::Tools::ModelImporter productImporter;
    const RVX::Tools::ImportResult cooked =
        productImporter.Import(validPath, cookedPath);
    ASSERT_TRUE(cooked.success) << cooked.error;
    RVX::Resource::CookedModelArtifact cookedModel;
    std::string cookedError;
    ASSERT_TRUE(RVX::Resource::DeserializeCookedModelArtifact(
        ReadBytes(cookedPath), cookedModel, cookedError))
        << cookedError;
    ASSERT_EQ(cookedModel.materialArtifacts.size(), 1u);
    const fs::path cookedMaterialPath =
        cookedPath.parent_path() /
        cookedModel.materialArtifacts.front().packageRelativePath;
    const std::vector<RVX::uint8> cookedMaterialBytes =
        ReadBytes(cookedMaterialPath);
    ASSERT_FALSE(cookedMaterialBytes.empty());
    EXPECT_TRUE(ReadPrefix(cookedMaterialPath, 64).starts_with(
        RVX::Resource::RVX_MATERIAL_ARTIFACT_MAGIC));
    RVX::Resource::CookedMaterialArtifact cookedMaterial;
    ASSERT_TRUE(RVX::Resource::DeserializeCookedMaterialArtifact(
        cookedMaterialBytes, cookedMaterial, cookedError))
        << cookedError;
    EXPECT_EQ(cookedMaterial.material.descriptor.shadingModel,
              RVX::MaterialShadingModel::ThinTransmission);
    EXPECT_EQ(cookedMaterial.material.descriptor.blendMode,
              RVX::MaterialBlendMode::AlphaBlend);
    EXPECT_FLOAT_EQ(cookedMaterial.material.parameters.specularFactor, 0.25f);
    EXPECT_EQ(cookedMaterial.material.parameters.specularColor,
              RVX::Vec3(1.25f, 0.75f, 0.5f));
    EXPECT_FLOAT_EQ(
        cookedMaterial.material.parameters.clearcoatFactor, 0.8f);
    EXPECT_FLOAT_EQ(
        cookedMaterial.material.parameters.clearcoatRoughness, 0.2f);
    EXPECT_FLOAT_EQ(cookedMaterial.material.parameters.transmission, 0.6f);
    EXPECT_FLOAT_EQ(cookedMaterial.material.parameters.thickness, 0.35f);
    EXPECT_FLOAT_EQ(
        cookedMaterial.material.parameters.attenuationDistance, 2.5f);
    EXPECT_FLOAT_EQ(
        cookedMaterial.material.parameters.indexOfRefraction, 1.33f);
    EXPECT_FLOAT_EQ(
        cookedMaterial.material.parameters.emissiveStrength, 4.0f);
    const RVX::uint64 extensionFeatures =
        RVX::RVX_MATERIAL_FEATURE_SPECULAR |
        RVX::RVX_MATERIAL_FEATURE_CLEARCOAT |
        RVX::RVX_MATERIAL_FEATURE_TRANSMISSION |
        RVX::RVX_MATERIAL_FEATURE_VOLUME |
        RVX::RVX_MATERIAL_FEATURE_IOR;
    EXPECT_EQ(cookedMaterial.material.descriptor.featureMask &
                  extensionFeatures,
              extensionFeatures);
    EXPECT_EQ(
        cookedMaterial.permutation,
        RVX::Resource::GetCookedMaterialPermutation(
            cookedMaterial.material.descriptor));

    const fs::path toonCookedPath =
        temporary.Get() / "PbrExtensionsToon.rva";
    RVX::Tools::ModelImportOptions toonOptions;
    toonOptions.overrideAllMaterialsToToonLit = true;
    const RVX::Tools::ImportResult toonCooked =
        productImporter.Import(validPath, toonCookedPath, &toonOptions);
    ASSERT_TRUE(toonCooked.success) << toonCooked.error;
    const auto toonReplacementWarning = std::find_if(
        toonCooked.warnings.begin(),
        toonCooked.warnings.end(),
        [](const std::string& warning)
        {
            return warning.find("explicit ToonLit cook profile replaces") !=
                       std::string::npos &&
                   warning.find("KHR_materials_specular") !=
                       std::string::npos &&
                   warning.find("KHR_materials_clearcoat") !=
                       std::string::npos &&
                   warning.find("KHR_materials_transmission") !=
                       std::string::npos &&
                   warning.find("KHR_materials_volume") !=
                       std::string::npos &&
                   warning.find("KHR_materials_ior") !=
                       std::string::npos;
        });
    EXPECT_NE(toonReplacementWarning, toonCooked.warnings.end());

    RVX::Resource::CookedModelArtifact toonCookedModel;
    cookedError.clear();
    ASSERT_TRUE(RVX::Resource::DeserializeCookedModelArtifact(
        ReadBytes(toonCookedPath), toonCookedModel, cookedError))
        << cookedError;
    ASSERT_EQ(toonCookedModel.materialArtifacts.size(), 1u);
    const fs::path toonCookedMaterialPath =
        toonCookedPath.parent_path() /
        toonCookedModel.materialArtifacts.front().packageRelativePath;
    RVX::Resource::CookedMaterialArtifact toonCookedMaterial;
    cookedError.clear();
    ASSERT_TRUE(RVX::Resource::DeserializeCookedMaterialArtifact(
        ReadBytes(toonCookedMaterialPath), toonCookedMaterial, cookedError))
        << cookedError;
    const RVX::MaterialDefinition& toonMaterial = toonCookedMaterial.material;
    EXPECT_EQ(toonMaterial.descriptor.shadingModel,
              RVX::MaterialShadingModel::ToonLit);
    EXPECT_EQ(toonMaterial.descriptor.featureMask & extensionFeatures, 0u);
    EXPECT_FLOAT_EQ(toonMaterial.parameters.specularFactor, 1.0f);
    EXPECT_EQ(toonMaterial.parameters.specularColor,
              RVX::Vec3(1.0f, 1.0f, 1.0f));
    EXPECT_FLOAT_EQ(toonMaterial.parameters.clearcoatFactor, 0.0f);
    EXPECT_FLOAT_EQ(toonMaterial.parameters.clearcoatRoughness, 0.0f);
    EXPECT_FLOAT_EQ(toonMaterial.parameters.clearcoatNormalScale, 1.0f);
    EXPECT_FLOAT_EQ(toonMaterial.parameters.transmission, 0.0f);
    EXPECT_FLOAT_EQ(toonMaterial.parameters.indexOfRefraction, 1.5f);
    EXPECT_FLOAT_EQ(toonMaterial.parameters.thickness, 0.0f);
    EXPECT_EQ(toonMaterial.parameters.attenuationColor,
              RVX::Vec3(1.0f, 1.0f, 1.0f));
    EXPECT_FLOAT_EQ(toonMaterial.parameters.attenuationDistance, 0.0f);

    std::string invalidIor = gltf;
    const size_t ior = invalidIor.find("\"ior\": 1.33");
    ASSERT_NE(ior, std::string::npos);
    invalidIor.replace(
        ior,
        std::string_view("\"ior\": 1.33").size(),
        "\"ior\": 0.5");
    const fs::path invalidPath = temporary.Get() / "InvalidIor.gltf";
    {
        std::ofstream output(invalidPath, std::ios::binary);
        output.write(
            invalidIor.data(),
            static_cast<std::streamsize>(invalidIor.size()));
        ASSERT_TRUE(output.good());
    }
    const RVX::Resource::GLTFImportResult rejected =
        importer.Import(invalidPath.string());
    EXPECT_FALSE(rejected.success);
    EXPECT_NE(rejected.errorMessage.find("KHR_materials_ior.ior"),
              std::string::npos)
        << rejected.errorMessage;
}

TEST(CookedModelProductValidation,
     ModelCookPreservesActiveMaterialV2ExtensionTextureBindings)
{
    const fs::path baseSource =
        fs::path(RVX_SOURCE_DIR) /
        "Tests/Fixtures/ModelViewer/R7Triangle.gltf";
    const std::vector<RVX::uint8> baseBytes = ReadBytes(baseSource);
    std::string gltf(
        reinterpret_cast<const char*>(baseBytes.data()), baseBytes.size());
    const size_t materialsStart = gltf.find("  \"materials\": ");
    const size_t buffersStart = gltf.find("  \"buffers\": ");
    ASSERT_NE(materialsStart, std::string::npos);
    ASSERT_NE(buffersStart, std::string::npos);
    ASSERT_LT(materialsStart, buffersStart);

    constexpr std::string_view extensionMaterialAndTextures = R"json(  "materials": [
    {
      "name": "ExtensionTextureMaterial",
      "pbrMetallicRoughness": {
        "baseColorFactor": [0.8, 0.25, 0.15, 1.0],
        "metallicFactor": 0.0,
        "roughnessFactor": 0.8
      },
      "extensions": {
        "KHR_materials_specular": {
          "specularFactor": 0.5,
          "specularTexture": {
            "index": 0,
            "texCoord": 0,
            "extensions": {
              "KHR_texture_transform": {
                "offset": [0.125, -0.25],
                "scale": [1.5, 0.75],
                "rotation": 0.0,
                "texCoord": 0
              }
            }
          },
          "specularColorTexture": { "index": 1, "texCoord": 0 }
        },
        "KHR_materials_clearcoat": {
          "clearcoatFactor": 0.8,
          "clearcoatRoughnessFactor": 0.3,
          "clearcoatTexture": { "index": 2, "texCoord": 0 },
          "clearcoatRoughnessTexture": { "index": 3, "texCoord": 0 },
          "clearcoatNormalTexture": { "index": 4, "texCoord": 0 }
        },
        "KHR_materials_transmission": {
          "transmissionFactor": 0.6,
          "transmissionTexture": { "index": 5, "texCoord": 0 }
        },
        "KHR_materials_volume": {
          "thicknessFactor": 0.2,
          "attenuationColor": [0.8, 0.9, 1.0],
          "attenuationDistance": 2.0,
          "thicknessTexture": { "index": 6, "texCoord": 0 }
        }
      }
    }
  ],
  "samplers": [{
    "magFilter": 9728,
    "minFilter": 9985,
    "wrapS": 33071,
    "wrapT": 33648
  }],
  "textures": [
    { "source": 0, "sampler": 0 },
    { "source": 1, "sampler": 0 },
    { "source": 2, "sampler": 0 },
    { "source": 3, "sampler": 0 },
    { "source": 4, "sampler": 0 },
    { "source": 5, "sampler": 0 },
    { "source": 6, "sampler": 0 }
  ],
  "images": [
    { "name": "SpecularData", "uri": "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/p9sAAAAASUVORK5CYII=" },
    { "name": "SpecularColor", "uri": "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/p9sAAAAASUVORK5CYII=" },
    { "name": "Clearcoat", "uri": "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/p9sAAAAASUVORK5CYII=" },
    { "name": "ClearcoatRoughness", "uri": "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/p9sAAAAASUVORK5CYII=" },
    { "name": "ClearcoatNormal", "uri": "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/p9sAAAAASUVORK5CYII=" },
    { "name": "Transmission", "uri": "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/p9sAAAAASUVORK5CYII=" },
    { "name": "Thickness", "uri": "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/p9sAAAAASUVORK5CYII=" }
  ],
  "extensionsUsed": [
    "KHR_texture_transform",
    "KHR_materials_specular",
    "KHR_materials_clearcoat",
    "KHR_materials_transmission",
    "KHR_materials_volume"
  ],
  "extensionsRequired": [
    "KHR_texture_transform",
    "KHR_materials_specular",
    "KHR_materials_clearcoat",
    "KHR_materials_transmission",
    "KHR_materials_volume"
  ],
)json";
    gltf.replace(
        materialsStart,
        buffersStart - materialsStart,
        extensionMaterialAndTextures);

    TemporaryDirectory temporary;
    const fs::path sourcePath = temporary.Get() / "ExtensionTextures.gltf";
    {
        std::ofstream output(sourcePath, std::ios::binary);
        output.write(gltf.data(), static_cast<std::streamsize>(gltf.size()));
        ASSERT_TRUE(output.good());
    }

    const auto cookAndReadMaterial = [&temporary, &sourcePath](
                                         std::string_view directoryName,
                                         RVX::Resource::CookedMaterialArtifact& outMaterial,
                                         std::vector<RVX::uint8>& outModelBytes)
    {
        const fs::path outputDirectory = temporary.Get() / directoryName;
        fs::create_directories(outputDirectory);
        const fs::path outputPath = outputDirectory / "ExtensionTextures.rva";
        RVX::Tools::ModelImporter importer;
        const RVX::Tools::ImportResult cooked =
            importer.Import(sourcePath, outputPath);
        EXPECT_TRUE(cooked.success) << cooked.error;
        if (!cooked.success)
            return fs::path{};
        std::string error;
        RVX::Resource::CookedModelArtifact model;
        outModelBytes = ReadBytes(outputPath);
        EXPECT_TRUE(RVX::Resource::DeserializeCookedModelArtifact(
            outModelBytes, model, error)) << error;
        if (model.materialArtifacts.size() != 1u)
            return fs::path{};
        const fs::path materialPath =
            outputPath.parent_path() /
            model.materialArtifacts.front().packageRelativePath;
        error.clear();
        EXPECT_TRUE(RVX::Resource::DeserializeCookedMaterialArtifact(
            ReadBytes(materialPath), outMaterial, error)) << error;
        return materialPath;
    };

    RVX::Resource::CookedMaterialArtifact firstMaterial;
    std::vector<RVX::uint8> firstModelBytes;
    const fs::path firstMaterialPath =
        cookAndReadMaterial("First", firstMaterial, firstModelBytes);
    ASSERT_FALSE(firstMaterialPath.empty());
    ASSERT_EQ(firstMaterial.textureBindings.size(), 7u);
    const RVX::uint64 requiredFeatures =
        RVX::RVX_MATERIAL_FEATURE_SPECULAR |
        RVX::RVX_MATERIAL_FEATURE_CLEARCOAT |
        RVX::RVX_MATERIAL_FEATURE_TRANSMISSION |
        RVX::RVX_MATERIAL_FEATURE_VOLUME;
    EXPECT_EQ(firstMaterial.material.descriptor.featureMask & requiredFeatures,
              requiredFeatures);

    struct ExpectedBinding
    {
        RVX::MaterialTextureSlot slot;
        RVX::Resource::TextureUsage usage;
        bool isSRGB;
        RVX::Resource::TextureFallbackSemantic fallback;
    };
    const std::array<ExpectedBinding, 7> expectedBindings{{
        {RVX::MaterialTextureSlot::Specular, RVX::Resource::TextureUsage::Data,
         false, RVX::Resource::TextureFallbackSemantic::White},
        {RVX::MaterialTextureSlot::SpecularColor, RVX::Resource::TextureUsage::Color,
         true, RVX::Resource::TextureFallbackSemantic::White},
        {RVX::MaterialTextureSlot::Clearcoat, RVX::Resource::TextureUsage::Data,
         false, RVX::Resource::TextureFallbackSemantic::White},
        {RVX::MaterialTextureSlot::ClearcoatRoughness, RVX::Resource::TextureUsage::Data,
         false, RVX::Resource::TextureFallbackSemantic::White},
        {RVX::MaterialTextureSlot::ClearcoatNormal, RVX::Resource::TextureUsage::Normal,
         false, RVX::Resource::TextureFallbackSemantic::FlatNormal},
        {RVX::MaterialTextureSlot::Transmission, RVX::Resource::TextureUsage::Data,
         false, RVX::Resource::TextureFallbackSemantic::White},
        {RVX::MaterialTextureSlot::Thickness, RVX::Resource::TextureUsage::Data,
         false, RVX::Resource::TextureFallbackSemantic::White},
    }};
    for (const ExpectedBinding& expected : expectedBindings)
    {
        const auto binding = std::find_if(
            firstMaterial.textureBindings.begin(),
            firstMaterial.textureBindings.end(),
            [&expected](const RVX::Resource::CookedMaterialTextureBinding& value)
            {
                return value.definition.slot == expected.slot;
            });
        ASSERT_NE(binding, firstMaterial.textureBindings.end());
        EXPECT_EQ(binding->usage, expected.usage);
        EXPECT_EQ(binding->isSRGB, expected.isSRGB);
        EXPECT_EQ(binding->fallbackSemantic, expected.fallback);
        EXPECT_NE(binding->expectedContentHash, RVX::Hash::SHA256Digest{});
        const fs::path texturePath =
            firstMaterialPath.parent_path() / binding->artifactPath;
        const std::vector<RVX::uint8> textureBytes = ReadBytes(texturePath);
        ASSERT_FALSE(textureBytes.empty()) << texturePath;
        EXPECT_EQ(RVX::Hash::ComputeSHA256(
                      textureBytes.data(), textureBytes.size()),
                  binding->expectedContentHash);
        EXPECT_NE(ReadPrefix(texturePath).find("RVX_TEXTURE_PREBAKE_V1"),
                  std::string::npos);
    }
    const auto specularBinding = std::find_if(
        firstMaterial.textureBindings.begin(),
        firstMaterial.textureBindings.end(),
        [](const RVX::Resource::CookedMaterialTextureBinding& binding)
        {
            return binding.definition.slot == RVX::MaterialTextureSlot::Specular;
        });
    ASSERT_NE(specularBinding, firstMaterial.textureBindings.end());
    EXPECT_EQ(specularBinding->definition.uvSet, 0u);
    EXPECT_EQ(specularBinding->definition.offset,
              RVX::Vec2(0.125f, -0.25f));
    EXPECT_EQ(specularBinding->definition.scale, RVX::Vec2(1.5f, 0.75f));
    EXPECT_FLOAT_EQ(specularBinding->definition.rotation, 0.0f);
    EXPECT_EQ(specularBinding->definition.wrapS,
              RVX::MaterialSamplerWrap::ClampToEdge);
    EXPECT_EQ(specularBinding->definition.wrapT,
              RVX::MaterialSamplerWrap::MirrorRepeat);
    EXPECT_EQ(specularBinding->definition.minFilter,
              RVX::MaterialSamplerFilter::LinearMipmapNearest);
    EXPECT_EQ(specularBinding->definition.magFilter,
              RVX::MaterialSamplerFilter::Nearest);

    RVX::Resource::CookedMaterialArtifact secondMaterial;
    std::vector<RVX::uint8> secondModelBytes;
    const fs::path secondMaterialPath =
        cookAndReadMaterial("Second", secondMaterial, secondModelBytes);
    ASSERT_FALSE(secondMaterialPath.empty());
    EXPECT_EQ(firstModelBytes, secondModelBytes);
    EXPECT_EQ(ReadBytes(firstMaterialPath), ReadBytes(secondMaterialPath));

    std::string inactive = gltf;
    const size_t clearcoatFactor = inactive.find("\"clearcoatFactor\": 0.8");
    ASSERT_NE(clearcoatFactor, std::string::npos);
    inactive.replace(clearcoatFactor,
                     std::string_view("\"clearcoatFactor\": 0.8").size(),
                     "\"clearcoatFactor\": 0.0");
    const size_t transmissionFactor =
        inactive.find("\"transmissionFactor\": 0.6");
    ASSERT_NE(transmissionFactor, std::string::npos);
    inactive.replace(transmissionFactor,
                     std::string_view("\"transmissionFactor\": 0.6").size(),
                     "\"transmissionFactor\": 0.0");
    const fs::path inactiveSource = temporary.Get() / "InactiveExtensions.gltf";
    {
        std::ofstream output(inactiveSource, std::ios::binary);
        output.write(inactive.data(), static_cast<std::streamsize>(inactive.size()));
        ASSERT_TRUE(output.good());
    }
    const fs::path inactiveOutput = temporary.Get() / "InactiveExtensions.rva";
    RVX::Tools::ModelImporter inactiveImporter;
    const RVX::Tools::ImportResult inactiveCook =
        inactiveImporter.Import(inactiveSource, inactiveOutput);
    ASSERT_TRUE(inactiveCook.success) << inactiveCook.error;
    RVX::Resource::CookedModelArtifact inactiveModel;
    std::string inactiveError;
    ASSERT_TRUE(RVX::Resource::DeserializeCookedModelArtifact(
        ReadBytes(inactiveOutput), inactiveModel, inactiveError)) << inactiveError;
    ASSERT_EQ(inactiveModel.materialArtifacts.size(), 1u);
    RVX::Resource::CookedMaterialArtifact inactiveMaterial;
    const fs::path inactiveMaterialPath =
        inactiveOutput.parent_path() /
        inactiveModel.materialArtifacts.front().packageRelativePath;
    inactiveError.clear();
    ASSERT_TRUE(RVX::Resource::DeserializeCookedMaterialArtifact(
        ReadBytes(inactiveMaterialPath), inactiveMaterial, inactiveError))
        << inactiveError;
    const RVX::uint64 inactiveFeatures =
        RVX::RVX_MATERIAL_FEATURE_CLEARCOAT |
        RVX::RVX_MATERIAL_FEATURE_TRANSMISSION |
        RVX::RVX_MATERIAL_FEATURE_VOLUME;
    EXPECT_EQ(inactiveMaterial.material.descriptor.featureMask & inactiveFeatures,
              0u);
    for (const RVX::Resource::CookedMaterialTextureBinding& binding :
         inactiveMaterial.textureBindings)
    {
        EXPECT_NE(binding.definition.slot, RVX::MaterialTextureSlot::Clearcoat);
        EXPECT_NE(binding.definition.slot,
                  RVX::MaterialTextureSlot::ClearcoatRoughness);
        EXPECT_NE(binding.definition.slot,
                  RVX::MaterialTextureSlot::ClearcoatNormal);
        EXPECT_NE(binding.definition.slot,
                  RVX::MaterialTextureSlot::Transmission);
        EXPECT_NE(binding.definition.slot, RVX::MaterialTextureSlot::Thickness);
    }
}

TEST(CookedModelProductValidation,
     MissingTangentsUseNormalTextureEffectiveUVSetAndSurviveCook)
{
    TemporaryDirectory temporary;
    const fs::path geometryPath = temporary.Get() / "TangentUV.bin";
    const std::vector<float> positions = {
        0.0f, 0.0f, 0.0f,
        1.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f};
    const std::vector<float> normals = {
        0.0f, 0.0f, 1.0f,
        0.0f, 0.0f, 1.0f,
        0.0f, 0.0f, 1.0f};
    const std::vector<float> uv0 = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.0f, 1.0f};
    // Rotating the authored coordinate axes makes a UV1-generated tangent
    // point along +Y instead of the +X tangent produced from UV0.
    const std::vector<float> uv1 = {
        0.0f, 0.0f,
        0.0f, 1.0f,
        1.0f, 0.0f};
    const std::vector<RVX::uint16> indices = {0, 1, 2};
    std::vector<RVX::uint8> geometry;
    const auto appendBytes = [&geometry](const auto& values)
    {
        const auto* begin = reinterpret_cast<const RVX::uint8*>(values.data());
        geometry.insert(
            geometry.end(),
            begin,
            begin + values.size() * sizeof(values.front()));
    };
    appendBytes(positions);
    appendBytes(normals);
    appendBytes(uv0);
    appendBytes(uv1);
    appendBytes(indices);
    ASSERT_EQ(geometry.size(), 126u);
    {
        std::ofstream output(geometryPath, std::ios::binary);
        output.write(
            reinterpret_cast<const char*>(geometry.data()),
            static_cast<std::streamsize>(geometry.size()));
        ASSERT_TRUE(output.good());
    }

    constexpr std::string_view source = R"json({
  "asset": {"version": "2.0"},
  "scene": 0,
  "scenes": [{"nodes": [0]}],
  "nodes": [{"mesh": 0}],
  "meshes": [{"primitives": [{
    "attributes": {
      "POSITION": 0,
      "NORMAL": 1,
      "TEXCOORD_0": 2,
      "TEXCOORD_1": 3
    },
    "indices": 4,
    "material": 0,
    "mode": 4
  }]}],
  "materials": [{
    "normalTexture": {
      "index": 0,
      "texCoord": 0,
      "extensions": {
        "KHR_texture_transform": {
          "texCoord": 1,
          "offset": [0.125, 0.25],
          "rotation": 0.0,
          "scale": [1.5, 0.75]
        }
      }
    }
  }],
  "textures": [{"source": 0}],
  "images": [{
    "uri": "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/p9sAAAAASUVORK5CYII="
  }],
  "extensionsUsed": ["KHR_texture_transform"],
  "extensionsRequired": ["KHR_texture_transform"],
  "buffers": [{"byteLength": 126, "uri": "TangentUV.bin"}],
  "bufferViews": [
    {"buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962},
    {"buffer": 0, "byteOffset": 36, "byteLength": 36, "target": 34962},
    {"buffer": 0, "byteOffset": 72, "byteLength": 24, "target": 34962},
    {"buffer": 0, "byteOffset": 96, "byteLength": 24, "target": 34962},
    {"buffer": 0, "byteOffset": 120, "byteLength": 6, "target": 34963}
  ],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0,0,0], "max": [1,1,0]},
    {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3"},
    {"bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC2"},
    {"bufferView": 3, "componentType": 5126, "count": 3, "type": "VEC2"},
    {"bufferView": 4, "componentType": 5123, "count": 3, "type": "SCALAR"}
  ]
})json";
    const fs::path sourcePath = temporary.Get() / "TangentUV.gltf";
    {
        std::ofstream output(sourcePath, std::ios::binary);
        output.write(source.data(), static_cast<std::streamsize>(source.size()));
        ASSERT_TRUE(output.good());
    }

    const auto expectUV1Tangent = [](const RVX::Mesh& mesh)
    {
        const RVX::VertexAttribute* tangent =
            mesh.GetAttribute(RVX::VertexBufferNames::Tangent);
        ASSERT_NE(tangent, nullptr);
        ASSERT_EQ(tangent->GetVertexCount(), 3u);
        const auto* values = static_cast<const float*>(tangent->GetData());
        ASSERT_NE(values, nullptr);
        EXPECT_NEAR(values[0], 0.0f, 1.0e-5f);
        EXPECT_NEAR(values[1], 1.0f, 1.0e-5f);
        EXPECT_NEAR(values[2], 0.0f, 1.0e-5f);
        EXPECT_NEAR(values[3], -1.0f, 1.0e-5f);
    };
    const auto expectFiniteDegenerateFallback = [](const RVX::Mesh& mesh)
    {
        const RVX::VertexAttribute* tangent =
            mesh.GetAttribute(RVX::VertexBufferNames::Tangent);
        ASSERT_NE(tangent, nullptr);
        ASSERT_EQ(tangent->GetVertexCount(), 3u);
        const auto* values = static_cast<const float*>(tangent->GetData());
        ASSERT_NE(values, nullptr);
        for (size_t vertex = 0; vertex < tangent->GetVertexCount(); ++vertex)
        {
            const size_t offset = vertex * 4u;
            const float lengthSquared =
                values[offset] * values[offset] +
                values[offset + 1u] * values[offset + 1u] +
                values[offset + 2u] * values[offset + 2u];
            EXPECT_TRUE(std::isfinite(lengthSquared));
            EXPECT_NEAR(lengthSquared, 1.0f, 1.0e-5f);
            // The fixture normal is +Z, so a valid fallback remains in its
            // tangent plane even when UV derivatives are unavailable.
            EXPECT_NEAR(values[offset + 2u], 0.0f, 1.0e-5f);
            EXPECT_NEAR(std::abs(values[offset + 3u]), 1.0f, 1.0e-5f);
        }
    };

    RVX::Resource::GLTFImporter importer;
    const RVX::Resource::GLTFImportResult imported =
        importer.Import(sourcePath.string());
    ASSERT_TRUE(imported.success) << imported.errorMessage;
    ASSERT_EQ(imported.meshes.size(), 1u);
    ASSERT_NE(imported.meshes[0], nullptr);
    expectUV1Tangent(*imported.meshes[0]);

    std::string rotatedNormalSource{source};
    const size_t normalRotation =
        rotatedNormalSource.find("\"rotation\": 0.0");
    ASSERT_NE(normalRotation, std::string::npos);
    rotatedNormalSource.replace(
        normalRotation,
        std::string_view("\"rotation\": 0.0").size(),
        "\"rotation\": 0.25");
    const fs::path rotatedNormalPath =
        temporary.Get() / "RotatedNormal.gltf";
    {
        std::ofstream output(rotatedNormalPath, std::ios::binary);
        output.write(
            rotatedNormalSource.data(),
            static_cast<std::streamsize>(rotatedNormalSource.size()));
        ASSERT_TRUE(output.good());
    }
    const RVX::Resource::GLTFImportResult rotatedNormalRejected =
        importer.Import(rotatedNormalPath.string());
    EXPECT_FALSE(rotatedNormalRejected.success);
    EXPECT_NE(rotatedNormalRejected.errorMessage.find(
                  "normal texture KHR_texture_transform.rotation"),
              std::string::npos)
        << rotatedNormalRejected.errorMessage;

    std::string mirroredNormalSource{source};
    const size_t normalScale =
        mirroredNormalSource.find("\"scale\": [1.5, 0.75]");
    ASSERT_NE(normalScale, std::string::npos);
    mirroredNormalSource.replace(
        normalScale,
        std::string_view("\"scale\": [1.5, 0.75]").size(),
        "\"scale\": [1.5, -0.75]");
    const fs::path mirroredNormalPath =
        temporary.Get() / "MirroredNormal.gltf";
    {
        std::ofstream output(mirroredNormalPath, std::ios::binary);
        output.write(
            mirroredNormalSource.data(),
            static_cast<std::streamsize>(mirroredNormalSource.size()));
        ASSERT_TRUE(output.good());
    }
    const RVX::Resource::GLTFImportResult mirroredNormalRejected =
        importer.Import(mirroredNormalPath.string());
    EXPECT_FALSE(mirroredNormalRejected.success);
    EXPECT_NE(mirroredNormalRejected.errorMessage.find(
                  "normal texture KHR_texture_transform.scale"),
              std::string::npos)
        << mirroredNormalRejected.errorMessage;

    std::string rotatedClearcoatNormalSource{source};
    const std::string materialTail = "    }\n  }],\n  \"textures\"";
    const size_t materialTailPosition =
        rotatedClearcoatNormalSource.find(materialTail);
    ASSERT_NE(materialTailPosition, std::string::npos);
    rotatedClearcoatNormalSource.replace(
        materialTailPosition,
        materialTail.size(),
        "    },\n"
        "    \"extensions\": {\n"
        "      \"KHR_materials_clearcoat\": {\n"
        "        \"clearcoatNormalTexture\": {\n"
        "          \"index\": 0,\n"
        "          \"texCoord\": 1,\n"
        "          \"extensions\": {\n"
        "            \"KHR_texture_transform\": {\n"
        "              \"rotation\": 0.25,\n"
        "              \"scale\": [1.0, 1.0]\n"
        "            }\n"
        "          }\n"
        "        }\n"
        "      }\n"
        "    }\n"
        "  }],\n"
        "  \"textures\"");
    const fs::path rotatedClearcoatNormalPath =
        temporary.Get() / "RotatedClearcoatNormal.gltf";
    {
        std::ofstream output(rotatedClearcoatNormalPath, std::ios::binary);
        output.write(
            rotatedClearcoatNormalSource.data(),
            static_cast<std::streamsize>(
                rotatedClearcoatNormalSource.size()));
        ASSERT_TRUE(output.good());
    }
    const RVX::Resource::GLTFImportResult rotatedClearcoatNormalRejected =
        importer.Import(rotatedClearcoatNormalPath.string());
    EXPECT_FALSE(rotatedClearcoatNormalRejected.success);
    EXPECT_NE(rotatedClearcoatNormalRejected.errorMessage.find(
                  "KHR_materials_clearcoat.clearcoatNormalTexture "
                  "KHR_texture_transform.rotation"),
              std::string::npos)
        << rotatedClearcoatNormalRejected.errorMessage;

    std::vector<RVX::uint8> degenerateUvGeometry = geometry;
    ASSERT_GE(degenerateUvGeometry.size(), 120u);
    std::fill(
        degenerateUvGeometry.begin() + 96u,
        degenerateUvGeometry.begin() + 120u,
        static_cast<RVX::uint8>(0u));
    const fs::path degenerateUvGeometryPath =
        temporary.Get() / "DegenerateUV.bin";
    {
        std::ofstream output(degenerateUvGeometryPath, std::ios::binary);
        output.write(
            reinterpret_cast<const char*>(degenerateUvGeometry.data()),
            static_cast<std::streamsize>(degenerateUvGeometry.size()));
        ASSERT_TRUE(output.good());
    }

    std::string degenerateNormalMappedSource(source);
    const size_t tangentUvUri =
        degenerateNormalMappedSource.find("TangentUV.bin");
    ASSERT_NE(tangentUvUri, std::string::npos);
    degenerateNormalMappedSource.replace(
        tangentUvUri,
        std::string_view("TangentUV.bin").size(),
        "DegenerateUV.bin");
    const fs::path degenerateNormalMappedPath =
        temporary.Get() / "DegenerateNormalMapped.gltf";
    {
        std::ofstream output(degenerateNormalMappedPath, std::ios::binary);
        output.write(
            degenerateNormalMappedSource.data(),
            static_cast<std::streamsize>(degenerateNormalMappedSource.size()));
        ASSERT_TRUE(output.good());
    }
    const RVX::Resource::GLTFImportResult degenerateNormalMappedImported =
        importer.Import(degenerateNormalMappedPath.string());
    ASSERT_TRUE(degenerateNormalMappedImported.success)
        << degenerateNormalMappedImported.errorMessage;
    ASSERT_EQ(degenerateNormalMappedImported.meshes.size(), 1u);
    ASSERT_NE(degenerateNormalMappedImported.meshes[0], nullptr);
    expectFiniteDegenerateFallback(*degenerateNormalMappedImported.meshes[0]);
    const auto fallbackWarning = std::find_if(
        degenerateNormalMappedImported.warnings.begin(),
        degenerateNormalMappedImported.warnings.end(),
        [](const std::string& warning)
        {
            return warning.find("glTF tangent fallback: mesh=") !=
                       std::string::npos &&
                   warning.find("primitive=0") != std::string::npos &&
                   warning.find("degenerateGeometryTriangles=0") !=
                       std::string::npos &&
                   warning.find("degenerateUVTriangles=1") !=
                       std::string::npos;
        });
    EXPECT_NE(fallbackWarning, degenerateNormalMappedImported.warnings.end());

    const fs::path fallbackCookedPath =
        temporary.Get() / "DegenerateNormalMapped.rva";
    RVX::Tools::ModelImporter fallbackProductImporter;
    const RVX::Tools::ImportResult fallbackCooked =
        fallbackProductImporter.Import(
            degenerateNormalMappedPath, fallbackCookedPath);
    ASSERT_TRUE(fallbackCooked.success) << fallbackCooked.error;
    EXPECT_NE(std::find_if(
                  fallbackCooked.warnings.begin(),
                  fallbackCooked.warnings.end(),
                  [](const std::string& warning)
                  {
                      return warning.find("glTF tangent fallback: mesh=") !=
                             std::string::npos;
                  }),
              fallbackCooked.warnings.end());

    std::string degenerateCorePbrSource = degenerateNormalMappedSource;
    const size_t degenerateMaterialStart =
        degenerateCorePbrSource.find("  \"materials\": [{");
    const size_t degenerateTextureStart =
        degenerateCorePbrSource.find("  \"textures\":", degenerateMaterialStart);
    ASSERT_NE(degenerateMaterialStart, std::string::npos);
    ASSERT_NE(degenerateTextureStart, std::string::npos);
    degenerateCorePbrSource.replace(
        degenerateMaterialStart,
        degenerateTextureStart - degenerateMaterialStart,
        "  \"materials\": [{}],\n");
    const fs::path degenerateCorePbrPath =
        temporary.Get() / "DegenerateCorePbr.gltf";
    {
        std::ofstream output(degenerateCorePbrPath, std::ios::binary);
        output.write(
            degenerateCorePbrSource.data(),
            static_cast<std::streamsize>(degenerateCorePbrSource.size()));
        ASSERT_TRUE(output.good());
    }
    const RVX::Resource::GLTFImportResult degenerateCorePbrImported =
        importer.Import(degenerateCorePbrPath.string());
    ASSERT_TRUE(degenerateCorePbrImported.success)
        << degenerateCorePbrImported.errorMessage;
    ASSERT_EQ(degenerateCorePbrImported.meshes.size(), 1u);
    ASSERT_NE(degenerateCorePbrImported.meshes[0], nullptr);
    EXPECT_FALSE(degenerateCorePbrImported.meshes[0]->HasAttribute(
        RVX::VertexBufferNames::Tangent));

    std::string vertexColorSource(source);
    const size_t uv1Attribute =
        vertexColorSource.find("      \"TEXCOORD_1\": 3\n");
    ASSERT_NE(uv1Attribute, std::string::npos);
    vertexColorSource.replace(
        uv1Attribute,
        std::string_view("      \"TEXCOORD_1\": 3\n").size(),
        "      \"TEXCOORD_1\": 3,\n      \"COLOR_0\": 3\n");
    const fs::path vertexColorPath = temporary.Get() / "VertexColor.gltf";
    {
        std::ofstream output(vertexColorPath, std::ios::binary);
        output.write(
            vertexColorSource.data(),
            static_cast<std::streamsize>(vertexColorSource.size()));
        ASSERT_TRUE(output.good());
    }
    const RVX::Resource::GLTFImportResult vertexColorRejected =
        importer.Import(vertexColorPath.string());
    EXPECT_FALSE(vertexColorRejected.success);
    EXPECT_NE(vertexColorRejected.errorMessage.find("COLOR_0"),
              std::string::npos)
        << vertexColorRejected.errorMessage;
    RVX::Tools::ModelImporter vertexColorProductImporter;
    const RVX::Tools::ImportResult vertexColorCookRejected =
        vertexColorProductImporter.Import(
            vertexColorPath, temporary.Get() / "VertexColor.rva");
    EXPECT_FALSE(vertexColorCookRejected.success);
    EXPECT_NE(vertexColorCookRejected.error.find("COLOR_0"),
              std::string::npos)
        << vertexColorCookRejected.error;

    RVX::Resource::GLTFImportOptions noGeneration;
    noGeneration.generateTangents = false;
    const RVX::Resource::GLTFImportResult rejected =
        importer.Import(sourcePath.string(), noGeneration);
    EXPECT_FALSE(rejected.success);
    EXPECT_NE(rejected.errorMessage.find("tangent generation is disabled"),
              std::string::npos)
        << rejected.errorMessage;

    std::string missingNormal(source);
    const size_t normalAttribute = missingNormal.find("      \"NORMAL\": 1,\n");
    ASSERT_NE(normalAttribute, std::string::npos);
    missingNormal.erase(
        normalAttribute,
        std::string_view("      \"NORMAL\": 1,\n").size());
    const fs::path missingNormalPath = temporary.Get() / "MissingNormal.gltf";
    {
        std::ofstream output(missingNormalPath, std::ios::binary);
        output.write(
            missingNormal.data(),
            static_cast<std::streamsize>(missingNormal.size()));
        ASSERT_TRUE(output.good());
    }
    RVX::Resource::GLTFImportOptions noNormalGeneration;
    noNormalGeneration.generateNormals = false;
    const RVX::Resource::GLTFImportResult missingNormalRejected =
        importer.Import(missingNormalPath.string(), noNormalGeneration);
    EXPECT_FALSE(missingNormalRejected.success);
    EXPECT_NE(missingNormalRejected.errorMessage.find(
                  "normal generation is disabled"),
              std::string::npos)
        << missingNormalRejected.errorMessage;

    std::string unlitMissingNormal = missingNormal;
    const size_t materialStart =
        unlitMissingNormal.find("  \"materials\": [{");
    const size_t textureStart =
        unlitMissingNormal.find("  \"textures\":", materialStart);
    ASSERT_NE(materialStart, std::string::npos);
    ASSERT_NE(textureStart, std::string::npos);
    unlitMissingNormal.replace(
        materialStart,
        textureStart - materialStart,
        "  \"materials\": [{}],\n");
    const fs::path unlitMissingNormalPath =
        temporary.Get() / "UnlitMissingNormal.gltf";
    {
        std::ofstream output(unlitMissingNormalPath, std::ios::binary);
        output.write(
            unlitMissingNormal.data(),
            static_cast<std::streamsize>(unlitMissingNormal.size()));
        ASSERT_TRUE(output.good());
    }
    const RVX::Resource::GLTFImportResult unlitMissingNormalRejected =
        importer.Import(unlitMissingNormalPath.string(), noNormalGeneration);
    EXPECT_FALSE(unlitMissingNormalRejected.success);
    EXPECT_NE(unlitMissingNormalRejected.errorMessage.find(
                  "flat-normal generation is disabled"),
              std::string::npos)
        << unlitMissingNormalRejected.errorMessage;

    std::vector<RVX::uint8> geometryWithAuthoredTangents = geometry;
    const std::vector<float> authoredTangents = {
        1.0f, 0.0f, 0.0f, 1.0f,
        1.0f, 0.0f, 0.0f, 1.0f,
        1.0f, 0.0f, 0.0f, 1.0f};
    const auto* tangentBegin = reinterpret_cast<const RVX::uint8*>(
        authoredTangents.data());
    geometryWithAuthoredTangents.insert(
        geometryWithAuthoredTangents.end(),
        tangentBegin,
        tangentBegin + authoredTangents.size() * sizeof(float));
    ASSERT_EQ(geometryWithAuthoredTangents.size(), 174u);
    const fs::path authoredTangentGeometryPath =
        temporary.Get() / "MissingNormalAuthoredTangent.bin";
    {
        std::ofstream output(authoredTangentGeometryPath, std::ios::binary);
        output.write(
            reinterpret_cast<const char*>(geometryWithAuthoredTangents.data()),
            static_cast<std::streamsize>(geometryWithAuthoredTangents.size()));
        ASSERT_TRUE(output.good());
    }

    std::string rebuildTangent = missingNormal;
    const size_t positionProperty =
        rebuildTangent.find("      \"POSITION\": 0,\n");
    ASSERT_NE(positionProperty, std::string::npos);
    rebuildTangent.insert(
        positionProperty + std::string_view("      \"POSITION\": 0,\n").size(),
        "      \"TANGENT\": 5,\n");
    const size_t bufferDeclaration = rebuildTangent.find(
        "{\"byteLength\": 126, \"uri\": \"TangentUV.bin\"}");
    ASSERT_NE(bufferDeclaration, std::string::npos);
    rebuildTangent.replace(
        bufferDeclaration,
        std::string_view(
            "{\"byteLength\": 126, \"uri\": \"TangentUV.bin\"}").size(),
        "{\"byteLength\": 174, \"uri\": "
        "\"MissingNormalAuthoredTangent.bin\"}");
    const size_t lastBufferView = rebuildTangent.find(
        "    {\"buffer\": 0, \"byteOffset\": 120, \"byteLength\": 6, "
        "\"target\": 34963}\n");
    ASSERT_NE(lastBufferView, std::string::npos);
    rebuildTangent.insert(
        lastBufferView + std::string_view(
            "    {\"buffer\": 0, \"byteOffset\": 120, \"byteLength\": 6, "
            "\"target\": 34963}").size(),
        ",\n    {\"buffer\": 0, \"byteOffset\": 126, \"byteLength\": 48, "
        "\"target\": 34962}");
    const size_t lastAccessor = rebuildTangent.find(
        "    {\"bufferView\": 4, \"componentType\": 5123, \"count\": 3, "
        "\"type\": \"SCALAR\"}\n");
    ASSERT_NE(lastAccessor, std::string::npos);
    rebuildTangent.insert(
        lastAccessor + std::string_view(
            "    {\"bufferView\": 4, \"componentType\": 5123, \"count\": 3, "
            "\"type\": \"SCALAR\"}").size(),
        ",\n    {\"bufferView\": 5, \"componentType\": 5126, \"count\": 3, "
        "\"type\": \"VEC4\"}");
    const fs::path rebuildTangentPath =
        temporary.Get() / "MissingNormalAuthoredTangent.gltf";
    {
        std::ofstream output(rebuildTangentPath, std::ios::binary);
        output.write(
            rebuildTangent.data(),
            static_cast<std::streamsize>(rebuildTangent.size()));
        ASSERT_TRUE(output.good());
    }
    const RVX::Resource::GLTFImportResult rebuiltTangentResult =
        importer.Import(rebuildTangentPath.string());
    ASSERT_TRUE(rebuiltTangentResult.success)
        << rebuiltTangentResult.errorMessage;
    ASSERT_EQ(rebuiltTangentResult.meshes.size(), 1u);
    ASSERT_NE(rebuiltTangentResult.meshes[0], nullptr);
    expectUV1Tangent(*rebuiltTangentResult.meshes[0]);

    std::string failedNormalGeneration = missingNormal;
    const size_t indicesProperty =
        failedNormalGeneration.find("    \"indices\": 4,\n");
    ASSERT_NE(indicesProperty, std::string::npos);
    failedNormalGeneration.erase(
        indicesProperty,
        std::string_view("    \"indices\": 4,\n").size());
    const size_t triangleMode = failedNormalGeneration.find("    \"mode\": 4");
    ASSERT_NE(triangleMode, std::string::npos);
    failedNormalGeneration.replace(
        triangleMode,
        std::string_view("    \"mode\": 4").size(),
        "    \"mode\": 1");
    const fs::path failedNormalPath =
        temporary.Get() / "FailedNormalGeneration.gltf";
    {
        std::ofstream output(failedNormalPath, std::ios::binary);
        output.write(
            failedNormalGeneration.data(),
            static_cast<std::streamsize>(failedNormalGeneration.size()));
        ASSERT_TRUE(output.good());
    }
    const RVX::Resource::GLTFImportResult failedNormalRejected =
        importer.Import(failedNormalPath.string());
    EXPECT_FALSE(failedNormalRejected.success);
    EXPECT_NE(failedNormalRejected.errorMessage.find(
                  "Failed to generate flat vertex normals"),
              std::string::npos)
        << failedNormalRejected.errorMessage;

    const fs::path cookedPath = temporary.Get() / "TangentUV.rva";
    RVX::Tools::ModelImporter productImporter;
    const RVX::Tools::ImportResult cooked =
        productImporter.Import(sourcePath, cookedPath);
    ASSERT_TRUE(cooked.success) << cooked.error;
    RVX::Resource::CookedModelArtifact modelArtifact;
    std::string error;
    ASSERT_TRUE(RVX::Resource::DeserializeCookedModelArtifact(
        ReadBytes(cookedPath), modelArtifact, error)) << error;
    std::vector<RVX::Resource::CookedMeshRecord> cookedMeshes;
    ASSERT_TRUE(RVX::Resource::CookedMeshArtifactReader::ReadFile(
        (cookedPath.parent_path() / modelArtifact.meshArtifactPath).string(),
        cookedMeshes,
        error)) << error;
    ASSERT_EQ(cookedMeshes.size(), 1u);
    ASSERT_FALSE(cookedMeshes[0].lodMeshes.empty());
    ASSERT_NE(cookedMeshes[0].lodMeshes[0], nullptr);
    expectUV1Tangent(*cookedMeshes[0].lodMeshes[0]);
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
    size_t textureBindingCount = 0;
    for (const RVX::Resource::CookedMaterialArtifactReference& reference :
         artifact.materialArtifacts)
    {
        const fs::path materialPath =
            output.parent_path() / reference.packageRelativePath;
        RVX::Resource::CookedMaterialArtifact materialArtifact;
        ASSERT_TRUE(RVX::Resource::DeserializeCookedMaterialArtifact(
            ReadBytes(materialPath), materialArtifact, error)) << error;
        for (const RVX::Resource::CookedMaterialTextureBinding& texture :
             materialArtifact.textureBindings)
        {
            ++textureBindingCount;
            const std::string prefix = ReadPrefix(
                materialPath.parent_path() / texture.artifactPath);
            EXPECT_NE(prefix.find("RVX_TEXTURE_PREBAKE_V1"),
                      std::string::npos);
            const char* expected =
                texture.usage == RVX::Resource::TextureUsage::Normal
                    ? "format=BC5"
                    : "format=BC7";
            EXPECT_NE(prefix.find(expected), std::string::npos)
                << texture.artifactPath;
        }
    }
    EXPECT_GT(textureBindingCount, 0u);
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

    ASSERT_FALSE(artifact.materialArtifacts.empty());
    RVX::Resource::CookedModelArtifact mismatchedMaterial = artifact;
    mismatchedMaterial.materialArtifacts[0].expectedContentHash[0] ^= 1u;
    std::vector<RVX::uint8> mismatchedMaterialBytes;
    ASSERT_TRUE(RVX::Resource::SerializeCookedModelArtifact(
        mismatchedMaterial, mismatchedMaterialBytes, error)) << error;
    const fs::path mismatchedRoot = temporary.Get() / "MaterialHashMismatch.rva";
    std::ofstream mismatchedFile(mismatchedRoot, std::ios::binary);
    ASSERT_TRUE(mismatchedFile.is_open());
    mismatchedFile.write(
        reinterpret_cast<const char*>(mismatchedMaterialBytes.data()),
        static_cast<std::streamsize>(mismatchedMaterialBytes.size()));
    mismatchedFile.close();
    RVX::Resource::CookedModelLoader loader;
    RVX::Resource::ResourceLoadError loadError;
    EXPECT_FALSE(PrepareCookedModel(loader, mismatchedRoot, &loadError));
    EXPECT_NE(loadError.message.find("content hash"), std::string::npos);

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
    const RVX::Resource::CookedModelArtifact originalArtifact = artifact;

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

    ASSERT_FALSE(originalArtifact.materialArtifacts.empty());
    const fs::path originalMaterial = productDirectory /
        originalArtifact.materialArtifacts[0].packageRelativePath;
    fs::copy_file(
        originalMaterial,
        outsideDirectory / "material.rvmat",
        fs::copy_options::overwrite_existing);
    RVX::Resource::CookedModelArtifact escapedMaterial = originalArtifact;
    escapedMaterial.materialArtifacts[0].packageRelativePath =
        "linked/material.rvmat";
    std::vector<RVX::uint8> escapedMaterialBytes;
    ASSERT_TRUE(RVX::Resource::SerializeCookedModelArtifact(
        escapedMaterial, escapedMaterialBytes, error)) << error;
    const fs::path escapedMaterialRoot =
        productDirectory / "ResolvedMaterialEscape.rva";
    std::ofstream escapedMaterialFile(
        escapedMaterialRoot, std::ios::binary);
    escapedMaterialFile.write(
        reinterpret_cast<const char*>(escapedMaterialBytes.data()),
        static_cast<std::streamsize>(escapedMaterialBytes.size()));
    escapedMaterialFile.close();
    EXPECT_FALSE(PrepareCookedModel(loader, escapedMaterialRoot));
    RVX::Log::Shutdown();
}
