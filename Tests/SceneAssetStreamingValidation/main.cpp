#include "Core/Diagnostics/Trace.h"
#include "Core/Log.h"
#include "Resource/Importer/GLTFImporter.h"
#include "Resource/Loader/TextureLoader.h"
#include "Resource/ResourceManager.h"
#include "Resource/Types/ModelResource.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <thread>

using namespace RVX;
using namespace RVX::Resource;

namespace
{
class LogEnvironment final : public ::testing::Environment
{
public:
    void SetUp() override { Log::Initialize(); }
    void TearDown() override { Log::Shutdown(); }
};

[[maybe_unused]] const auto* g_logEnvironment =
    ::testing::AddGlobalTestEnvironment(new LogEnvironment);

std::filesystem::path FixturePath()
{
    return std::filesystem::path(RVX_SOURCE_DIR) /
           "Tests/Fixtures/Samples/PBRMaterialTextureCube.gltf";
}

class ManagerGuard final
{
public:
    ManagerGuard(uint64 budget, Diagnostics::TraceContext traceContext)
    {
        ResourceManagerConfig config;
        config.asyncThreadCount = 2;
        config.modelTextureDecodedByteBudget = budget;
        config.modelTextureMaxConcurrentDecodes = 4;
        config.runtimePolicy.mode = ResourceRuntimeMode::Editor;
        config.runtimePolicy.allowSourceAssetReads = true;
        config.startupTraceContext = std::move(traceContext);
        ResourceManager::Get().Initialize(config);
    }

    ~ManagerGuard()
    {
        ResourceManager::Get().Shutdown();
    }
};

ResourceHandle<ModelResource> AwaitModel(
    ResourceLoadHandle<ModelResource>& request)
{
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline)
    {
        ResourceManager::Get().ProcessCompletedLoads();
        if (ResourceHandle<ModelResource> model = request.TryGet())
            return model;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return {};
}
} // namespace

TEST(SceneAssetStreamingValidation,
     GltfImportCapturesEncodedPayloadAndDefersDecode)
{
    Diagnostics::TraceSessionConfig traceConfig;
    traceConfig.enabled = true;
    const auto trace = Diagnostics::TraceSession::Create(traceConfig);
    const Diagnostics::TraceContext context = trace->CreateRootContext();

    GLTFImporter importer;
    GLTFImportResult imported =
        importer.Import(FixturePath().string(), {}, context);
    ASSERT_TRUE(imported.success) << imported.errorMessage;
    ASSERT_EQ(imported.textures.size(), 1u);
    EXPECT_TRUE(imported.textures[0].HasCapturedPayload());
    EXPECT_FALSE(imported.textures[0].isRawPixelData);

    size_t encodedReads = 0;
    size_t decodes = 0;
    for (const Diagnostics::TraceEvent& event : trace->GetEvents())
    {
        encodedReads += event.name == "TextureEncodedRead" ? 1u : 0u;
        decodes += event.name == "TextureDecode" ? 1u : 0u;
    }
    EXPECT_EQ(encodedReads, 1u);
    EXPECT_EQ(decodes, 0u);

    TextureLoader decoder(nullptr, true);
    uint64 estimate = 0;
    std::string error;
    ASSERT_TRUE(decoder.EstimateDecodedByteSize(
        imported.textures[0], FixturePath().string(), estimate, error))
        << error;
    DecodedTextureData decoded;
    ASSERT_TRUE(decoder.DecodeReference(imported.textures[0],
                                        FixturePath().string(),
                                        context,
                                        decoded,
                                        error))
        << error;
    ASSERT_TRUE(decoded.IsValid());
    EXPECT_LT(decoded.bytes->size(), estimate);

    encodedReads = 0;
    decodes = 0;
    for (const Diagnostics::TraceEvent& event : trace->GetEvents())
    {
        encodedReads += event.name == "TextureEncodedRead" ? 1u : 0u;
        decodes += event.name == "TextureDecode" ? 1u : 0u;
    }
    EXPECT_EQ(encodedReads, 1u);
    EXPECT_EQ(decodes, 1u);
}

TEST(SceneAssetStreamingValidation,
     ModelPublishesPersistentFallbackThenDecodesWithinBudget)
{
    Diagnostics::TraceSessionConfig traceConfig;
    traceConfig.enabled = true;
    const auto trace = Diagnostics::TraceSession::Create(traceConfig);
    ManagerGuard guard(1024u * 1024u, trace->CreateRootContext());

    uint32 reloadEvents = 0;
    ResourceManager::Get().SetLifecycleEventCallback(
        [&reloadEvents](const ResourceLifecycleEvent& event)
        {
            if (event.type == ResourceLifecycleEventType::Reloaded)
                ++reloadEvents;
        });

    auto request = ResourceManager::Get().RequestAsync<ModelResource>(
        FixturePath().string());
    ASSERT_TRUE(request);
    ResourceHandle<ModelResource> model = AwaitModel(request);
    ASSERT_TRUE(model);
    ASSERT_EQ(model->GetTextureStreamingSnapshot().stage,
              ModelTextureStreamingStage::AwaitingMinimumResident);
    ASSERT_FALSE(model->GetStreamingTextures().empty());
    const ResourceHandle<TextureResource> texture =
        model->GetStreamingTextures().front();
    ASSERT_TRUE(texture);
    EXPECT_TRUE(texture->IsStreamingPlaceholder());
    EXPECT_FALSE(texture->IsDefaultFallback());
    EXPECT_EQ(texture->GetData().size(), 4u);

    ASSERT_TRUE(ResourceManager::Get().BeginModelTextureStreaming(model));
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline &&
           model->GetTextureStreamingSnapshot().stage ==
               ModelTextureStreamingStage::Decoding)
    {
        ResourceManager::Get().ProcessCompletedLoads();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ResourceManager::Get().ProcessCompletedLoads();

    EXPECT_EQ(model->GetTextureStreamingSnapshot().stage,
              ModelTextureStreamingStage::Uploading);
    // Decoded CPU bytes do not change the committed GPU residency state.
    // ResourceSubsystem clears this only after replacement completion.
    EXPECT_TRUE(texture->IsStreamingPlaceholder());
    EXPECT_GT(texture->GetData().size(), 4u);
    EXPECT_EQ(reloadEvents, 1u);
    const ModelTextureStreamingStats stats =
        ResourceManager::Get().GetModelTextureStreamingStats();
    EXPECT_EQ(stats.completedDecodes, 1u);
    EXPECT_EQ(stats.failedDecodes, 0u);
    EXPECT_GT(stats.reservedDecodedBytes, 0u);
    EXPECT_EQ(stats.pendingPublications, 1u);
    EXPECT_LE(stats.peakReservedDecodedBytes, stats.decodedByteBudget);
    EXPECT_LE(stats.peakActiveDecodes, stats.maxConcurrentDecodes);

    const auto publications =
        ResourceManager::Get().GetPendingModelTexturePublications();
    ASSERT_EQ(publications.size(), 1u);
    EXPECT_EQ(publications.front().GetId(), texture.GetId());
    ASSERT_TRUE(ResourceManager::Get().CompleteModelTexturePublication(
        texture.GetId(), true));
    EXPECT_EQ(model->GetTextureStreamingSnapshot().stage,
              ModelTextureStreamingStage::FullyResident);
    EXPECT_FALSE(texture->IsStreamingPlaceholder());
    EXPECT_TRUE(texture->GetData().empty());
    EXPECT_EQ(ResourceManager::Get()
                  .GetModelTextureStreamingStats()
                  .reservedDecodedBytes,
              0u);
    EXPECT_EQ(ResourceManager::Get()
                  .GetModelTextureStreamingStats()
                  .pendingPublications,
              0u);
}

TEST(SceneAssetStreamingValidation,
     OversizedTextureFailsAfterFallbackPublicationWithoutDecode)
{
    ManagerGuard guard(64u, {});
    auto request = ResourceManager::Get().RequestAsync<ModelResource>(
        FixturePath().string());
    ASSERT_TRUE(request);
    ResourceHandle<ModelResource> model = AwaitModel(request);
    ASSERT_TRUE(model);
    ASSERT_FALSE(model->GetStreamingTextures().empty());
    const ResourceHandle<TextureResource> texture =
        model->GetStreamingTextures().front();
    ASSERT_TRUE(texture->IsStreamingPlaceholder());

    EXPECT_FALSE(ResourceManager::Get().BeginModelTextureStreaming(model));
    EXPECT_EQ(model->GetTextureStreamingSnapshot().stage,
              ModelTextureStreamingStage::Failed);
    EXPECT_TRUE(texture->IsStreamingPlaceholder());
    EXPECT_EQ(texture->GetData().size(), 4u);
    const ModelTextureStreamingStats stats =
        ResourceManager::Get().GetModelTextureStreamingStats();
    EXPECT_EQ(stats.completedDecodes, 0u);
    EXPECT_EQ(stats.failedDecodes, 1u);
    EXPECT_LE(stats.peakReservedDecodedBytes, stats.decodedByteBudget);
}

TEST(SceneAssetStreamingValidation,
     FailedGpuPublicationRetiresBudgetAndKeepsCommittedFallback)
{
    ManagerGuard guard(1024u * 1024u, {});
    auto request = ResourceManager::Get().RequestAsync<ModelResource>(
        FixturePath().string());
    ASSERT_TRUE(request);
    ResourceHandle<ModelResource> model = AwaitModel(request);
    ASSERT_TRUE(model);
    ASSERT_TRUE(ResourceManager::Get().BeginModelTextureStreaming(model));

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline &&
           ResourceManager::Get()
                   .GetPendingModelTexturePublications()
                   .empty())
    {
        ResourceManager::Get().ProcessCompletedLoads();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    const auto publications =
        ResourceManager::Get().GetPendingModelTexturePublications();
    ASSERT_EQ(publications.size(), 1u);
    const ResourceHandle<TextureResource> texture = publications.front();
    ASSERT_TRUE(texture);
    EXPECT_TRUE(texture->IsStreamingPlaceholder());
    EXPECT_GT(texture->GetData().size(), 4u);
    EXPECT_GT(ResourceManager::Get()
                  .GetModelTextureStreamingStats()
                  .reservedDecodedBytes,
              0u);

    ASSERT_TRUE(ResourceManager::Get().CompleteModelTexturePublication(
        texture.GetId(), false, "injected replacement failure"));
    const ModelTextureStreamingSnapshot snapshot =
        model->GetTextureStreamingSnapshot();
    EXPECT_EQ(snapshot.stage, ModelTextureStreamingStage::Failed);
    EXPECT_EQ(snapshot.error, "injected replacement failure");
    EXPECT_TRUE(texture->IsStreamingPlaceholder());
    EXPECT_TRUE(texture->GetData().empty());
    EXPECT_EQ(ResourceManager::Get()
                  .GetModelTextureStreamingStats()
                  .reservedDecodedBytes,
              0u);
    EXPECT_EQ(ResourceManager::Get()
                  .GetModelTextureStreamingStats()
                  .pendingPublications,
              0u);
}

TEST(SceneAssetStreamingValidation,
     GpuRetirementCanReleaseDecodedPixelsWithoutLosingRecoverySource)
{
    GLTFImporter importer;
    GLTFImportResult imported = importer.Import(FixturePath().string());
    ASSERT_TRUE(imported.success) << imported.errorMessage;
    ASSERT_EQ(imported.textures.size(), 1u);

    TextureLoader loader(nullptr, true);
    TextureResource* raw = loader.CreateStreamingPlaceholder(
        imported.textures[0],
        FixturePath().string(),
        "streaming-recovery-test");
    ResourceHandle<TextureResource> texture(raw);
    ASSERT_TRUE(texture);
    ASSERT_TRUE(texture->GetEncodedSourceStorage());
    const size_t encodedBytes = texture->GetEncodedSourceStorage()->size();
    ASSERT_GT(encodedBytes, 0u);

    DecodedTextureData decoded;
    std::string error;
    ASSERT_TRUE(loader.DecodeReference(imported.textures[0],
                                       FixturePath().string(),
                                       {},
                                       decoded,
                                       error))
        << error;
    texture->SetDataStorage(decoded.bytes, decoded.metadata);
    texture->MarkStreamingPlaceholder(false);
    ASSERT_GT(texture->GetData().size(), 4u);

    texture->ReleaseCPUData();
    EXPECT_TRUE(texture->GetData().empty());
    ASSERT_TRUE(texture->GetEncodedSourceStorage());
    EXPECT_EQ(texture->GetEncodedSourceStorage()->size(), encodedBytes);
}
