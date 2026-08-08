#include "Core/Log.h"
#include "Render/Renderer/RenderSceneDatabase.h"
#include "RenderExtraction/RenderFrameExtractor.h"
#include "Resource/ResourceSubsystem.h"
#include "Scene/Components/CameraComponent.h"
#include "Scene/Components/DecalComponent.h"
#include "Scene/Components/LightComponent.h"
#include "Scene/Components/LightProbeComponent.h"
#include "Scene/Components/ReflectionProbeComponent.h"
#include "World/World.h"

#include <gtest/gtest.h>

#include <unordered_map>

namespace
{
    class LogEnvironment final : public ::testing::Environment
    {
    public:
        void SetUp() override { RVX::Log::Initialize(); }
        void TearDown() override { RVX::Log::Shutdown(); }
    };

    [[maybe_unused]] ::testing::Environment* const g_logEnvironment =
        ::testing::AddGlobalTestEnvironment(new LogEnvironment());

    class FakeResourceGateway final : public RVX::IRenderResourceGateway
    {
    public:
        RVX::RenderResourceReserveResult ReserveResource(
            RVX::AssetId assetId,
            RVX::RenderResourceKind kind) noexcept override
        {
            const auto found = m_assets.find(assetId);
            if (found != m_assets.end())
            {
                return {RVX::RenderResourceReserveCode::Existing,
                        found->second,
                        QueryResourceStatus(found->second)};
            }

            const RVX::RenderResourceHandle handle{m_nextSlot++, 1};
            m_assets.emplace(assetId, handle);
            m_kinds.emplace(handle, kind);
            m_statuses.emplace(
                handle,
                RVX::RenderResourceStatus{
                    RVX::RenderResourceStatusCode::Current,
                    RVX::RenderResourcePublicState::Reserved,
                    RVX::RenderResourceFailureCode::None});
            return {RVX::RenderResourceReserveCode::Reserved,
                    handle,
                    m_statuses.at(handle)};
        }

        RVX::RenderUploadEnqueueResult TryEnqueueUpload(
            const RVX::ResourceUploadRequestRef& request) noexcept override
        {
            if (!request || !m_statuses.contains(request->GetHandle()))
                return {RVX::RenderUploadEnqueueCode::InvalidRequest};
            m_statuses[request->GetHandle()].state =
                RVX::RenderResourcePublicState::UploadQueued;
            return {RVX::RenderUploadEnqueueCode::Accepted};
        }

        RVX::RenderReleaseResult RequestRelease(
            RVX::RenderResourceHandle handle) noexcept override
        {
            const auto found = m_statuses.find(handle);
            if (found == m_statuses.end())
                return {RVX::RenderReleaseCode::StaleGeneration};
            found->second.state = RVX::RenderResourcePublicState::Evicting;
            return {RVX::RenderReleaseCode::Accepted};
        }

        RVX::RenderResourceStatus QueryResourceStatus(
            RVX::RenderResourceHandle handle) const noexcept override
        {
            const auto found = m_statuses.find(handle);
            if (found == m_statuses.end())
            {
                return {RVX::RenderResourceStatusCode::StaleGeneration,
                        RVX::RenderResourcePublicState::Released,
                        RVX::RenderResourceFailureCode::None};
            }
            return found->second;
        }

    private:
        RVX::uint32 m_nextSlot = 1;
        std::unordered_map<RVX::AssetId,
                           RVX::RenderResourceHandle,
                           RVX::AssetIdHash>
            m_assets;
        std::unordered_map<RVX::RenderResourceHandle,
                           RVX::RenderResourceKind,
                           RVX::RenderResourceHandleHash>
            m_kinds;
        std::unordered_map<RVX::RenderResourceHandle,
                           RVX::RenderResourceStatus,
                           RVX::RenderResourceHandleHash>
            m_statuses;
    };

    class ResourceFixture final
    {
    public:
        ResourceFixture()
        {
            subsystem.SetRenderResourceGateway(&gateway);
            RVX::Resource::ResourceManagerConfig config;
            config.asyncThreadCount = 0;
            config.runtimePolicy.mode =
                RVX::Resource::ResourceRuntimeMode::Editor;
            config.runtimePolicy.allowSourceAssetReads = true;
            subsystem.Initialize(config);
        }

        ~ResourceFixture()
        {
            subsystem.BeginRenderShutdown();
            subsystem.Deinitialize();
        }

        FakeResourceGateway gateway;
        RVX::Resource::ResourceSubsystem subsystem;
    };

    RVX::RenderFrameExtractionInput MakeInput(
        RVX::uint64 sequence,
        RVX::World& world,
        RVX::Resource::ResourceSubsystem& resources)
    {
        RVX::RenderFrameExtractionInput input;
        input.sequence = sequence;
        // World revision identifies an active-world cutover. Scene mutations
        // are carried by RenderSceneUpdateBatch revisions and must not force a
        // checkpoint on every component change.
        input.worldRevision = 1;
        input.world = &world;
        input.resources = &resources;
        input.view.viewportWidth = 1280;
        input.view.viewportHeight = 720;
        input.view.nearPlane = 0.1f;
        input.view.farPlane = 1000.0f;
        input.view.deltaTime = 1.0f / 60.0f;
        return input;
    }

    void ExpectShadowParity(const RVX::RenderFrameExtractionResult& extraction,
                            const RVX::RenderSceneDatabase& database)
    {
        ASSERT_NE(extraction.packet, nullptr);
        ASSERT_NE(extraction.frameV5, nullptr);
        const auto comparison = database.Compare(*extraction.packet);
        EXPECT_TRUE(comparison.matches);
        EXPECT_EQ(comparison.missingPrimitiveCount, 0u);
        EXPECT_EQ(comparison.changedPrimitiveCount, 0u);
        EXPECT_EQ(comparison.missingLightCount, 0u);
        EXPECT_EQ(comparison.changedLightCount, 0u);
        EXPECT_EQ(comparison.featureMismatchCount, 0u);

        const auto rebuilt =
            database.BuildCompatibilityFrame(*extraction.frameV5);
        ASSERT_NE(rebuilt, nullptr);
        EXPECT_EQ(rebuilt->GetHeader().sequence,
                  extraction.packet->GetHeader().sequence);
        EXPECT_EQ(rebuilt->GetHeader().worldRevision,
                  extraction.packet->GetHeader().worldRevision);
        EXPECT_EQ(rebuilt->GetView().viewportWidth,
                  extraction.packet->GetView().viewportWidth);
        ASSERT_EQ(rebuilt->GetLights().size(),
                  extraction.packet->GetLights().size());
        if (!rebuilt->GetLights().empty())
        {
            EXPECT_EQ(rebuilt->GetLights().front().lightId,
                      extraction.packet->GetLights().front().lightId);
            EXPECT_FLOAT_EQ(rebuilt->GetLights().front().intensity,
                            extraction.packet->GetLights().front().intensity);
        }
    }
} // namespace

TEST(RenderTransportShadowValidation,
     FullResetStaticFrameAndIncrementalUpdateStayEquivalent)
{
    ResourceFixture resources;
    RVX::World world;
    ASSERT_TRUE(world.Initialize());

    auto* cameraActor = world.SpawnActor({.name = "Camera"});
    ASSERT_NE(cameraActor, nullptr);
    auto* camera = cameraActor->AddComponent<RVX::CameraComponent>();
    ASSERT_NE(camera, nullptr);
    ASSERT_TRUE(world.SetActiveCamera(camera->GetComponentHandle()));

    auto* lightActor = world.SpawnActor({.name = "Light"});
    ASSERT_NE(lightActor, nullptr);
    auto* light = lightActor->AddComponent<RVX::LightComponent>();
    ASSERT_NE(light, nullptr);
    light->SetIntensity(2.0f);

    RVX::RenderFrameExtractor extractor;
    RVX::RenderSceneDatabase database;

    auto first = extractor.Extract(MakeInput(1, world, resources.subsystem));
    ASSERT_TRUE(first.IsComplete());
    ASSERT_NE(first.sceneUpdate, nullptr);
    EXPECT_TRUE(first.sceneUpdate->fullReset);
    ASSERT_TRUE(database.Apply(*first.sceneUpdate).IsApplied());
    ExpectShadowParity(first, database);
    extractor.ResolveLastPublication(
        RVX::RenderFramePublicationDisposition::Accepted);

    auto unchanged =
        extractor.Extract(MakeInput(2, world, resources.subsystem));
    ASSERT_TRUE(unchanged.IsComplete());
    EXPECT_EQ(unchanged.sceneUpdate, nullptr);
    ASSERT_NE(unchanged.frameV5, nullptr);
    EXPECT_EQ(unchanged.frameV5->GetHeader().requiredSceneRevision,
              database.GetRevision());
    ExpectShadowParity(unchanged, database);
    extractor.ResolveLastPublication(
        RVX::RenderFramePublicationDisposition::Accepted);

    light->SetIntensity(9.0f);
    auto changed =
        extractor.Extract(MakeInput(3, world, resources.subsystem));
    ASSERT_TRUE(changed.IsComplete());
    ASSERT_NE(changed.sceneUpdate, nullptr);
    EXPECT_FALSE(changed.sceneUpdate->fullReset);
    ASSERT_TRUE(database.Apply(*changed.sceneUpdate).IsApplied());
    ExpectShadowParity(changed, database);
    ASSERT_NE(database.FindLight(
                  light->GetComponentHandle().GetPackedValue()),
              nullptr);
    EXPECT_FLOAT_EQ(
        database.FindLight(
            light->GetComponentHandle().GetPackedValue())->intensity,
        9.0f);
    extractor.ResolveLastPublication(
        RVX::RenderFramePublicationDisposition::Accepted);

    world.Shutdown();
}

TEST(RenderTransportShadowValidation, SceneUpdateSurvivesDroppedFramePacket)
{
    ResourceFixture resources;
    RVX::World world;
    ASSERT_TRUE(world.Initialize());
    ASSERT_NE(world.CreateCamera("Main"), nullptr);
    auto* lightActor = world.SpawnActor({.name = "Light"});
    ASSERT_NE(lightActor, nullptr);
    auto* light = lightActor->AddComponent<RVX::LightComponent>();
    ASSERT_NE(light, nullptr);

    RVX::RenderFrameExtractor extractor;
    RVX::RenderSceneDatabase database;
    auto initial =
        extractor.Extract(MakeInput(1, world, resources.subsystem));
    ASSERT_NE(initial.sceneUpdate, nullptr);
    ASSERT_TRUE(database.Apply(*initial.sceneUpdate).IsApplied());
    extractor.ResolveLastPublication(
        RVX::RenderFramePublicationDisposition::Accepted);

    light->SetIntensity(7.0f);
    auto dropped =
        extractor.Extract(MakeInput(2, world, resources.subsystem));
    ASSERT_NE(dropped.sceneUpdate, nullptr);
    ASSERT_TRUE(database.Apply(*dropped.sceneUpdate).IsApplied());
    extractor.ResolveLastPublication(
        RVX::RenderFramePublicationDisposition::SceneUpdateAcceptedWithoutFrame);
    ASSERT_NE(database.FindLight(
                  light->GetComponentHandle().GetPackedValue()),
              nullptr);
    EXPECT_FLOAT_EQ(
        database.FindLight(
            light->GetComponentHandle().GetPackedValue())->intensity,
        7.0f);

    auto recovery =
        extractor.Extract(MakeInput(3, world, resources.subsystem));
    ASSERT_TRUE(recovery.IsComplete());
    ASSERT_NE(recovery.sceneUpdate, nullptr);
    EXPECT_TRUE(recovery.sceneUpdate->fullReset);
    ASSERT_TRUE(database.Apply(*recovery.sceneUpdate).IsApplied());
    ExpectShadowParity(recovery, database);

    world.Shutdown();
}

TEST(RenderTransportShadowValidation,
     DecalsAndProbesUseIncrementalPersistentSceneUpdates)
{
    ResourceFixture resources;
    RVX::World world;
    ASSERT_TRUE(world.Initialize());
    ASSERT_NE(world.CreateCamera("Main"), nullptr);

    auto* decalActor = world.SpawnActor({.name = "Decal"});
    auto* reflectionActor = world.SpawnActor({.name = "ReflectionProbe"});
    auto* lightProbeActor = world.SpawnActor({.name = "LightProbe"});
    ASSERT_NE(decalActor, nullptr);
    ASSERT_NE(reflectionActor, nullptr);
    ASSERT_NE(lightProbeActor, nullptr);

    auto* decal = decalActor->AddComponent<RVX::DecalComponent>();
    auto* reflection =
        reflectionActor->AddComponent<RVX::ReflectionProbeComponent>();
    auto* lightProbe =
        lightProbeActor->AddComponent<RVX::LightProbeComponent>();
    ASSERT_NE(decal, nullptr);
    ASSERT_NE(reflection, nullptr);
    ASSERT_NE(lightProbe, nullptr);

    decal->SetOpacity(0.8f);
    decal->SetSortOrder(7);
    reflection->SetImportance(5);
    reflection->SetBlendDistance(2.0f);
    lightProbe->Bake();

    const RVX::uint64 decalId =
        decal->GetComponentHandle().GetPackedValue();
    const RVX::uint64 reflectionId =
        reflection->GetComponentHandle().GetPackedValue();
    const RVX::uint64 lightProbeId =
        lightProbe->GetComponentHandle().GetPackedValue();

    RVX::RenderFrameExtractor extractor;
    RVX::RenderSceneDatabase database;
    auto initial = extractor.Extract(
        MakeInput(1, world, resources.subsystem));
    ASSERT_TRUE(initial.IsComplete());
    ASSERT_NE(initial.sceneUpdate, nullptr);
    ASSERT_TRUE(initial.sceneUpdate->fullReset);
    ASSERT_EQ(initial.sceneUpdate->decals.size(), 1u);
    ASSERT_EQ(initial.sceneUpdate->probes.size(), 2u);
    ASSERT_TRUE(database.Apply(*initial.sceneUpdate).IsApplied());
    ASSERT_EQ(database.GetDecalCount(), 1u);
    ASSERT_EQ(database.GetProbeCount(), 2u);
    ASSERT_NE(database.FindDecal(decalId), nullptr);
    ASSERT_NE(database.FindProbe(reflectionId), nullptr);
    ASSERT_NE(database.FindProbe(lightProbeId), nullptr);
    EXPECT_FLOAT_EQ(database.FindDecal(decalId)->opacity, 0.8f);
    EXPECT_EQ(database.FindProbe(reflectionId)->priority, 5);
    EXPECT_TRUE(database.FindProbe(lightProbeId)->hasValidData);
    extractor.ResolveLastPublication(
        RVX::RenderFramePublicationDisposition::Accepted);

    decal->SetOpacity(0.35f);
    reflection->SetImportance(9);
    auto changed = extractor.Extract(
        MakeInput(2, world, resources.subsystem));
    ASSERT_TRUE(changed.IsComplete());
    ASSERT_NE(changed.sceneUpdate, nullptr);
    ASSERT_FALSE(changed.sceneUpdate->fullReset);
    ASSERT_EQ(changed.sceneUpdate->decals.size(), 1u);
    ASSERT_EQ(changed.sceneUpdate->probes.size(), 1u);
    ASSERT_TRUE(database.Apply(*changed.sceneUpdate).IsApplied());
    EXPECT_FLOAT_EQ(database.FindDecal(decalId)->opacity, 0.35f);
    EXPECT_EQ(database.FindProbe(reflectionId)->priority, 9);
    extractor.ResolveLastPublication(
        RVX::RenderFramePublicationDisposition::Accepted);

    ASSERT_TRUE(world.DestroyActor(reflectionActor));
    auto removed = extractor.Extract(
        MakeInput(3, world, resources.subsystem));
    ASSERT_TRUE(removed.IsComplete());
    ASSERT_NE(removed.sceneUpdate, nullptr);
    ASSERT_EQ(removed.sceneUpdate->probes.size(), 1u);
    EXPECT_EQ(removed.sceneUpdate->probes.front().operation,
              RVX::RenderSceneMutationOperation::Remove);
    ASSERT_TRUE(database.Apply(*removed.sceneUpdate).IsApplied());
    EXPECT_EQ(database.FindProbe(reflectionId), nullptr);
    EXPECT_EQ(database.GetProbeCount(), 1u);

    world.Shutdown();
}
