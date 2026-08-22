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
