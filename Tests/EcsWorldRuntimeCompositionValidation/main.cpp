#include "WorldEcsRuntimeComposition.h"

#include "Core/Log.h"
#include "../../Particle/Include/Particle/ECS/ParticleEcsBridge.h"
#include "Resource/Loader/EnvironmentLoader.h"
#include "Resource/PreparedResourceBundle.h"
#include "Resource/ResourceSubsystem.h"
#include "Resource/Types/EnvironmentResource.h"
#include "Resource/Types/TextureResource.h"
#include "Scene/ECS/RenderFragments.h"
#include "../../Terrain/Include/Terrain/ECS/TerrainEcsBridge.h"
#include "../../Water/Include/Water/ECS/WaterEcsBridge.h"

#include <gtest/gtest.h>

#include <unordered_map>

namespace
{
    using namespace RVX;

    class LogEnvironment final : public ::testing::Environment
    {
    public:
        void SetUp() override { Log::Initialize(); }
        void TearDown() override { Log::Shutdown(); }
    };

    [[maybe_unused]] ::testing::Environment* const g_logEnvironment =
        ::testing::AddGlobalTestEnvironment(new LogEnvironment());

    class AdvancingTransport final : public IEcsRenderFrameTransport
    {
    public:
        [[nodiscard]] EcsRenderFrameTransportReceipt TryPublish(
            EcsRenderFrameTransportCandidate candidate) noexcept override
        {
            EcsRenderFrameTransportReceipt receipt;
            receipt.disposition = candidate.IsStructurallyValid() ?
                                      EcsRenderFrameTransportDisposition::Accepted :
                                      EcsRenderFrameTransportDisposition::NotAccepted;
            receipt.candidateIdentity = candidate.candidateIdentity;
            receipt.sourceSceneRuntimeId = candidate.sourceSceneRuntimeId;
            receipt.sourceSnapshotRevision = candidate.sourceSnapshotRevision;
            receipt.targetRenderSceneRevision = candidate.targetRenderSceneRevision;
            receipt.frameSequence = candidate.frameSequence;
            receipt.completedProgress = {
                .appliedRenderSceneRevision = candidate.targetRenderSceneRevision,
                .presentedFrameSequence = candidate.frameSequence,
            };
            return receipt;
        }
    };

    [[nodiscard]] RenderResourceHandle ResolveEveryAsset(
        void*, AssetId assetId, RenderResourceKind) noexcept
    {
        return assetId.IsValid() ? RenderResourceHandle{1, 1} :
                                   RenderResourceHandle{};
    }

    [[nodiscard]] WorldEcsRuntimeCompositionTickRequest TickRequest()
    {
        WorldEcsRuntimeCompositionTickRequest request;
        request.sceneTick.variableDeltaSeconds = 1.0 / 60.0;
        request.sceneTick.fixedStepCount = 1;
        return request;
    }

    [[nodiscard]] EcsFrameExtractionInput ExternalRenderInput(uint64 sequence)
    {
        return {
            .sequence = sequence,
            .outputWidth = 1280,
            .outputHeight = 720,
            .deltaTime = 1.0f / 60.0f,
            .assetResolver = {.resolve = &ResolveEveryAsset},
        };
    }

    void PublishLatest(SceneECS::SceneEcsRuntime& runtime,
                       EcsRenderFramePipeline& pipeline,
                       uint64 sequence)
    {
        const std::shared_ptr<const SceneECS::FrozenSceneSnapshot> snapshot =
            runtime.GetLatestFrozenSnapshot();
        ASSERT_NE(snapshot, nullptr);
        ASSERT_TRUE(snapshot->selectedCamera.has_value());
        const EcsRenderFramePipelineResult result =
            pipeline.Publish(*snapshot, ExternalRenderInput(sequence));
        EXPECT_TRUE(result.IsPublished()) << "pipeline=" << static_cast<uint32>(result.code)
                                          << " bridge=" << static_cast<uint32>(result.bridge.code)
                                          << " extraction=" << static_cast<uint32>(result.extractionCode);
    }

    [[nodiscard]] ECS::EntityHandle AddCamera(SceneECS::SceneEcsRuntime& runtime)
    {
        const ECS::EntityHandle entity = runtime.CreateEntity();
        if (!entity.IsValid())
        {
            ADD_FAILURE() << "Could not create the fixture camera entity.";
            return ECS::EntityHandle::Invalid();
        }
        if (!runtime.AddFragment<SceneECS::Camera>(entity))
        {
            ADD_FAILURE() << "Could not add the fixture Camera fragment.";
            return ECS::EntityHandle::Invalid();
        }
        return entity;
    }

    class ImmediateEnvironmentLoader final : public Resource::IResourceLoader
    {
    public:
        ResourceType GetResourceType() const override { return ResourceType::Environment; }
        std::vector<std::string> GetSupportedExtensions() const override { return {".hdr"}; }
        Resource::IResource* Load(const std::string&) override { return nullptr; }
        bool SupportsPreparedLoading() const override { return true; }

        bool ValidatePreparationState(
            uint64 requestedImportOptionsHash,
            Resource::ResourceLoadPreparationStateRef suppliedState,
            Resource::ResourceLoadPreparationStateRef& outState,
            uint64& outCanonicalImportOptionsHash,
            Resource::ResourceLoadError& outError) const override
        {
            outState.reset();
            outCanonicalImportOptionsHash = 0;
            const auto environmentState =
                std::dynamic_pointer_cast<const Resource::EnvironmentPreparationState>(
                    std::move(suppliedState));
            if (!environmentState || !environmentState->IsValid())
            {
                outError = {Resource::ResourceLoadErrorCode::InvalidRequest,
                            "Immediate environment fixture rejected an invalid immutable preparation state."};
                return false;
            }
            if (requestedImportOptionsHash != 0 &&
                requestedImportOptionsHash != environmentState->importOptionsHash)
            {
                outError = {Resource::ResourceLoadErrorCode::InvalidRequest,
                            "Immediate environment fixture rejected a preparation-state hash mismatch."};
                return false;
            }

            outState = environmentState;
            outCanonicalImportOptionsHash = environmentState->importOptionsHash;
            outError = {};
            return true;
        }

        bool Prepare(const Resource::ResourceLoadPreparationContext& context,
                     Resource::PreparedResourceBundle& bundle,
                     Resource::ResourceLoadError& error) override
        {
            const auto makeTexture = [&context](const char* suffix, bool cubemap)
            {
                auto* texture = new Resource::TextureResource();
                texture->SetId(GenerateResourceId(context.resourceIdentityPath + suffix));
                Resource::TextureMetadata metadata;
                metadata.width = 1;
                metadata.height = 1;
                metadata.arrayLayers = cubemap ? 6 : 1;
                metadata.isCubemap = cubemap;
                texture->SetData(std::vector<uint8>(cubemap ? 24 : 4, 255), metadata);
                return Resource::TextureHandle(texture);
            };

            Resource::EnvironmentResourceData data;
            data.sourcePath = context.resolvedPath;
            data.environment = makeTexture("#environment", true);
            data.irradiance = makeTexture("#irradiance", true);
            data.prefiltered = makeTexture("#prefiltered", true);
            data.brdfLUT = makeTexture("#brdf", false);
            data.environmentResolution = 1;
            data.irradianceResolution = 1;
            data.prefilteredResolution = 1;
            data.prefilteredMipLevels = 1;
            data.brdfLUTResolution = 1;

            auto root = Resource::EnvironmentHandle(new Resource::EnvironmentResource());
            root->SetId(context.rootResourceId);
            root->SetPath(context.requestedPath);
            root->SetName("WorldEcsRuntimeCompositionFixture");
            if (!root->SetPreparedData(std::move(data)))
            {
                error = {Resource::ResourceLoadErrorCode::LoaderFailure,
                         "Could not prepare the Environment fixture."};
                return false;
            }
            const Resource::EnvironmentResourceData& prepared = root->GetData();
            return bundle.AddDependency(
                       Resource::ResourceHandle<Resource::IResource>(prepared.environment)) &&
                   bundle.AddDependency(
                       Resource::ResourceHandle<Resource::IResource>(prepared.irradiance)) &&
                   bundle.AddDependency(
                       Resource::ResourceHandle<Resource::IResource>(prepared.prefiltered)) &&
                   bundle.AddDependency(
                       Resource::ResourceHandle<Resource::IResource>(prepared.brdfLUT)) &&
                   bundle.SetRoot(Resource::ResourceHandle<Resource::IResource>(root));
        }
    };

    class ReadyResourceGateway final : public IRenderResourceGateway
    {
    public:
        RenderResourceReserveResult ReserveResource(
            AssetId assetId, RenderResourceKind) noexcept override
        {
            const auto existing = m_assets.find(assetId);
            if (existing != m_assets.end())
            {
                return {RenderResourceReserveCode::Existing,
                        existing->second,
                        QueryResourceStatus(existing->second)};
            }
            const RenderResourceHandle handle{m_nextSlot++, 1};
            m_assets.emplace(assetId, handle);
            m_statuses.emplace(
                handle,
                RenderResourceStatus{.code = RenderResourceStatusCode::Current,
                                     .state = RenderResourcePublicState::GPUReady});
            return {RenderResourceReserveCode::Reserved,
                    handle,
                    QueryResourceStatus(handle)};
        }

        RenderUploadEnqueueResult TryEnqueueUpload(
            const ResourceUploadRequestRef& request) noexcept override
        {
            if (!request)
            {
                return {RenderUploadEnqueueCode::InvalidRequest};
            }
            const auto found = m_statuses.find(request->GetHandle());
            if (found == m_statuses.end())
            {
                return {RenderUploadEnqueueCode::StaleGeneration};
            }
            found->second.committedContentRevision = request->GetSourceRevision();
            found->second.state = RenderResourcePublicState::GPUReady;
            return {RenderUploadEnqueueCode::Accepted};
        }

        RenderReleaseResult RequestRelease(RenderResourceHandle handle) noexcept override
        {
            const auto found = m_statuses.find(handle);
            if (found == m_statuses.end())
            {
                return {RenderReleaseCode::StaleGeneration};
            }
            found->second.state = RenderResourcePublicState::Released;
            return {RenderReleaseCode::Accepted};
        }

        [[nodiscard]] RenderResourceStatus QueryResourceStatus(
            RenderResourceHandle handle) const noexcept override
        {
            const auto found = m_statuses.find(handle);
            return found != m_statuses.end() ?
                       found->second :
                       RenderResourceStatus{.code = RenderResourceStatusCode::StaleGeneration,
                                            .state = RenderResourcePublicState::Released};
        }

    private:
        uint32 m_nextSlot = 1;
        std::unordered_map<AssetId, RenderResourceHandle, AssetIdHash> m_assets;
        std::unordered_map<RenderResourceHandle,
                           RenderResourceStatus,
                           RenderResourceHandleHash>
            m_statuses;
    };

    class ResourceScope final
    {
    public:
        ResourceScope()
        {
            m_resources.SetRenderResourceGateway(&m_gateway);
            Resource::ResourceManagerConfig config;
            config.asyncThreadCount = 0;
            m_resources.Initialize(config);
            m_resources.RegisterLoader(
                ResourceType::Environment,
                std::make_unique<ImmediateEnvironmentLoader>());
        }

        ~ResourceScope() { m_resources.Deinitialize(); }

        void Tick() { m_resources.Tick(0.0f); }

        ReadyResourceGateway m_gateway;
        Resource::ResourceSubsystem m_resources;
    };

    class ParticleFeatureGateway final : public Particle::IParticleEcsGateway
    {
    public:
        [[nodiscard]] Particle::ParticleEcsRuntimeHandle Create(
            const Particle::ParticleEcsRuntimeRequest& request) override
        {
            m_alive = true;
            m_maxParticleCount = request.config.maxParticleCount;
            return Particle::ParticleEcsRuntimeHandle::Create(0, 1);
        }

        [[nodiscard]] bool Update(Particle::ParticleEcsRuntimeHandle handle,
                                  const Particle::ParticleEcsRuntimeRequest&) override
        {
            return IsAlive(handle);
        }

        [[nodiscard]] bool CaptureSnapshot(
            Particle::ParticleEcsRuntimeHandle handle,
            Particle::ParticleEcsRuntimeSnapshot& outSnapshot) const override
        {
            if (!IsAlive(handle))
            {
                return false;
            }
            outSnapshot.item.instanceId = handle.GetPackedValue();
            outSnapshot.item.systemId = 17;
            outSnapshot.item.systemAssetId = AssetId{17};
            outSnapshot.item.maxParticleCount = m_maxParticleCount;
            outSnapshot.item.simulationSupported = true;
            outSnapshot.item.renderPayloadAvailable = true;
            outSnapshot.payloadRevision = 1;
            return true;
        }

        [[nodiscard]] Particle::ParticleEcsReleaseResult Release(
            Particle::ParticleEcsRuntimeHandle handle) override
        {
            if (!IsAlive(handle))
            {
                return Particle::ParticleEcsReleaseResult::AlreadyAbsent;
            }
            m_alive = false;
            return Particle::ParticleEcsReleaseResult::Released;
        }

        [[nodiscard]] bool IsAlive(Particle::ParticleEcsRuntimeHandle handle) const override
        {
            return m_alive && handle == Particle::ParticleEcsRuntimeHandle::Create(0, 1);
        }

    private:
        bool m_alive = false;
        uint32 m_maxParticleCount = 0;
    };

    class WaterFeatureGateway final : public Water::IWaterEcsGateway
    {
    public:
        [[nodiscard]] Water::WaterEcsRuntimeHandle Create(
            const Water::WaterEcsRuntimeRequest&) override
        {
            m_alive = true;
            return Water::WaterEcsRuntimeHandle::Create(0, 1);
        }

        [[nodiscard]] bool Update(Water::WaterEcsRuntimeHandle handle,
                                  const Water::WaterEcsRuntimeRequest&) override
        {
            return IsAlive(handle);
        }

        [[nodiscard]] bool CaptureSnapshot(
            Water::WaterEcsRuntimeHandle handle,
            Water::WaterEcsRuntimeSnapshot& outSnapshot) const override
        {
            if (!IsAlive(handle))
            {
                return false;
            }
            outSnapshot.item.resolution = 8;
            outSnapshot.item.renderGpuPathAvailable = true;
            outSnapshot.item.cpuSimulationAvailable = true;
            outSnapshot.payloadRevision = 1;
            return true;
        }

        [[nodiscard]] Water::WaterEcsReleaseResult Release(
            Water::WaterEcsRuntimeHandle handle) override
        {
            if (!IsAlive(handle))
            {
                return Water::WaterEcsReleaseResult::AlreadyAbsent;
            }
            m_alive = false;
            return Water::WaterEcsReleaseResult::Released;
        }

        [[nodiscard]] bool IsAlive(Water::WaterEcsRuntimeHandle handle) const override
        {
            return m_alive && handle == Water::WaterEcsRuntimeHandle::Create(0, 1);
        }

    private:
        bool m_alive = false;
    };

    class TerrainFeatureGateway final : public Terrain::ITerrainEcsGateway
    {
    public:
        [[nodiscard]] Terrain::TerrainEcsRuntimeHandle Create(
            const Terrain::TerrainEcsRuntimeRequest&) override
        {
            m_alive = true;
            return Terrain::TerrainEcsRuntimeHandle::Create(0, 1);
        }

        [[nodiscard]] bool Update(Terrain::TerrainEcsRuntimeHandle handle,
                                  const Terrain::TerrainEcsRuntimeRequest&) override
        {
            return IsAlive(handle);
        }

        [[nodiscard]] bool CaptureSnapshot(
            Terrain::TerrainEcsRuntimeHandle handle,
            Terrain::TerrainEcsRuntimeSnapshot& outSnapshot) const override
        {
            if (!IsAlive(handle))
            {
                return false;
            }
            outSnapshot.item.patchSize = 8;
            outSnapshot.item.maxLODLevels = 1;
            outSnapshot.item.renderGpuPathAvailable = true;
            outSnapshot.item.cpuDataAvailable = true;
            outSnapshot.payloadRevision = 1;
            return true;
        }

        [[nodiscard]] Terrain::TerrainEcsReleaseResult Release(
            Terrain::TerrainEcsRuntimeHandle handle) override
        {
            if (!IsAlive(handle))
            {
                return Terrain::TerrainEcsReleaseResult::AlreadyAbsent;
            }
            m_alive = false;
            return Terrain::TerrainEcsReleaseResult::Released;
        }

        [[nodiscard]] bool IsAlive(Terrain::TerrainEcsRuntimeHandle handle) const override
        {
            return m_alive && handle == Terrain::TerrainEcsRuntimeHandle::Create(0, 1);
        }

    private:
        bool m_alive = false;
    };
} // namespace

TEST(EcsWorldRuntimeCompositionValidation, InitializationRollsBackEveryRegisteredProcessor)
{
    SceneECS::SceneEcsRuntime runtime;
    AdvancingTransport transport;
    EcsRenderFramePipeline pipeline(transport);
    uint32 callerProcessorRuns = 0;
    const std::string duplicate = "PhysicsEcsBridge." +
                                  std::to_string(runtime.GetSceneRuntimeId().GetValue()) +
                                  ".BeginSimulationReconcile";
    ASSERT_TRUE(runtime.RegisterProcessor({
        .name = duplicate,
        .phase = ECS::ProcessorPhase::BeginSimulation,
        .stepMode = ECS::ProcessorStepMode::Variable,
        .run = [&callerProcessorRuns](ECS::Registry&) { ++callerProcessorRuns; },
    }));

    WorldEcsRuntimeComposition composition(runtime, pipeline);
    EXPECT_FALSE(composition.Initialize());
    EXPECT_FALSE(composition.GetPhysicsWorld().IsInitialized());
    EXPECT_TRUE(runtime.Tick().succeeded);
    EXPECT_EQ(callerProcessorRuns, 1u);
    EXPECT_TRUE(composition.GetDiagnostics().processorsCleared);
}

TEST(EcsWorldRuntimeCompositionValidation, ShutdownUnregistersOnlyCompositionProcessors)
{
    SceneECS::SceneEcsRuntime runtime;
    AdvancingTransport transport;
    EcsRenderFramePipeline pipeline(transport);
    uint32 callerProcessorRuns = 0;
    WorldEcsRuntimeComposition composition(runtime, pipeline);
    ASSERT_TRUE(composition.Initialize());
    ASSERT_TRUE(runtime.RegisterProcessor({
        .name = "caller-owned-world-composition-validation",
        .phase = ECS::ProcessorPhase::Gameplay,
        .stepMode = ECS::ProcessorStepMode::Variable,
        .run = [&callerProcessorRuns](ECS::Registry&) { ++callerProcessorRuns; },
    }));

    ASSERT_TRUE(composition.PrepareForShutdown(TickRequest()));
    const uint32 runsBeforePostShutdownTick = callerProcessorRuns;
    EXPECT_TRUE(runtime.Tick(TickRequest().sceneTick).succeeded);
    EXPECT_EQ(callerProcessorRuns, runsBeforePostShutdownTick + 1u);
    EXPECT_TRUE(composition.GetDiagnostics().processorsCleared);
}

TEST(EcsWorldRuntimeCompositionValidation, RegistersBuiltInPhysicsAndAnimationPhasesAsOneRuntime)
{
    SceneECS::SceneEcsRuntime runtime;
    AdvancingTransport transport;
    EcsRenderFramePipeline pipeline(transport);
    WorldEcsRuntimeComposition composition(runtime, pipeline);

    ASSERT_TRUE(composition.Initialize());
    EXPECT_EQ(composition.GetPhysicsWorld().GetActiveBackendType(),
              Physics::PhysicsBackendType::BuiltIn);
    ASSERT_NE(composition.GetPhysicsBridge(), nullptr);
    ASSERT_NE(composition.GetAnimationBridge(), nullptr);
    EXPECT_TRUE(composition.GetPhysicsBridge()->IsRegistered());
    EXPECT_TRUE(composition.GetAnimationBridge()->IsRegistered());
    EXPECT_TRUE(composition.Tick(TickRequest()).succeeded);

    const WorldEcsRuntimeServicesDiagnostics diagnostics = composition.GetRuntimeDiagnostics();
    EXPECT_TRUE(diagnostics.available);
    EXPECT_TRUE(diagnostics.initialized);
    EXPECT_EQ(diagnostics.sceneRuntimeId, runtime.GetSceneRuntimeId().GetValue());
    EXPECT_EQ(diagnostics.sceneFrameSequence, 1u);
    EXPECT_EQ(diagnostics.sceneFixedStepSequence, 1u);
    EXPECT_EQ(diagnostics.physicsFixedStepSequence, 1u);
    EXPECT_EQ(diagnostics.requestedFixedStepCount, 1u);
    EXPECT_EQ(diagnostics.executedFixedStepCount, 1u);
    EXPECT_TRUE(diagnostics.physicsInitialized);
    EXPECT_EQ(diagnostics.requestedPhysicsBackend, Physics::PhysicsBackendType::BuiltIn);
    EXPECT_EQ(diagnostics.activePhysicsBackend, Physics::PhysicsBackendType::BuiltIn);
    EXPECT_FALSE(diagnostics.physicsBackendFallbackActive);
    EXPECT_EQ(diagnostics.physicsBridgeBindingSideTableEntryCount, 0u);
    EXPECT_EQ(diagnostics.activePhysicsBodyCount, 0u);
    EXPECT_EQ(diagnostics.pendingPhysicsBridgeCleanupCount, 0u);
    EXPECT_EQ(diagnostics.staticPhysicsBodyCount, 0u);
    EXPECT_EQ(diagnostics.dynamicPhysicsBodyCount, 0u);
    EXPECT_EQ(diagnostics.kinematicPhysicsBodyCount, 0u);
    EXPECT_EQ(diagnostics.physicsColliderCount, 0u);
    EXPECT_EQ(diagnostics.physicsBridgeStructuralContinuityLossCount, 0u);
    EXPECT_EQ(diagnostics.physicsBridgeCleanupContinuityLossCount, 0u);
    EXPECT_EQ(diagnostics.physicsBridgeAuthoritativeReconcileCount, 1u);
    EXPECT_EQ(diagnostics.physicsRootMotionAppliedCount, 0u);
    EXPECT_EQ(diagnostics.physicsRootMotionRejectedCount, 0u);
    EXPECT_EQ(diagnostics.physicsRootMotionReplayCount, 0u);
    EXPECT_EQ(diagnostics.physicsRootMotionGapCount, 0u);
    EXPECT_TRUE(diagnostics.animationBridgeAvailable);
    EXPECT_EQ(diagnostics.animationBindingSideTableEntryCount, 0u);
    EXPECT_EQ(diagnostics.activeAnimationBindingCount, 0u);
    EXPECT_EQ(diagnostics.pendingAnimationBridgeCleanupCount, 0u);
    EXPECT_EQ(diagnostics.animationFixedEvaluationCount, 0u);
    EXPECT_EQ(diagnostics.animationRejectedEvaluationCount, 0u);
    EXPECT_EQ(diagnostics.animationRootMotionPublicationCount, 0u);
    EXPECT_EQ(diagnostics.animationRootMotionReplayCount, 0u);
    EXPECT_EQ(diagnostics.animationRootMotionGapCount, 0u);
    EXPECT_FALSE(diagnostics.resourceAnimationEvaluatorAvailable);
    EXPECT_EQ(diagnostics.resourceAnimationPlaybackSideTableEntryCount, 0u);
    EXPECT_FALSE(diagnostics.audioBridgeAvailable);
    EXPECT_EQ(diagnostics.audioPlaybackSideTableEntryCount, 0u);
    EXPECT_EQ(diagnostics.activeAudioPlaybackCount, 0u);
    EXPECT_EQ(diagnostics.outstandingAudioPlaybackCount, 0u);
    EXPECT_EQ(diagnostics.pendingAudioBridgeCleanupCount, 0u);
    EXPECT_FALSE(diagnostics.scriptBridgeAvailable);
    EXPECT_EQ(diagnostics.scriptInstanceSideTableEntryCount, 0u);
    EXPECT_EQ(diagnostics.activeScriptInstanceCount, 0u);
    EXPECT_EQ(diagnostics.outstandingScriptInstanceCount, 0u);
    EXPECT_EQ(diagnostics.pendingScriptBridgeCleanupCount, 0u);
    EXPECT_TRUE(diagnostics.featureSnapshotStoreAvailable);
    EXPECT_EQ(diagnostics.particleFeatureSnapshotCount, 0u);
    EXPECT_EQ(diagnostics.waterFeatureSnapshotCount, 0u);
    EXPECT_EQ(diagnostics.terrainFeatureSnapshotCount, 0u);
    EXPECT_EQ(diagnostics.featureSnapshotInstanceCount, 0u);
    EXPECT_TRUE(composition.PrepareForShutdown(TickRequest()));
}

TEST(EcsWorldRuntimeCompositionValidation,
     FeatureGatewaysRegisterInTheSharedScopeAndDrainBeforeTheirOwnersMayReset)
{
    SceneECS::SceneEcsRuntime runtime;
    AdvancingTransport transport;
    EcsRenderFramePipeline pipeline(transport);
    ParticleFeatureGateway particleGateway;
    WaterFeatureGateway waterGateway;
    TerrainFeatureGateway terrainGateway;
    WorldEcsRuntimeCompositionOptions options;
    options.particleGateway = &particleGateway;
    options.waterGateway = &waterGateway;
    options.terrainGateway = &terrainGateway;
    WorldEcsRuntimeComposition composition(runtime, pipeline, nullptr, options);

    ASSERT_TRUE(composition.Initialize());
    const WorldEcsRuntimeServicesDiagnostics beforeSpawn = composition.GetRuntimeDiagnostics();
    EXPECT_TRUE(beforeSpawn.particleBridgeAvailable);
    EXPECT_TRUE(beforeSpawn.waterBridgeAvailable);
    EXPECT_TRUE(beforeSpawn.terrainBridgeAvailable);
    EXPECT_EQ(beforeSpawn.particleBindingSideTableEntryCount, 0u);
    EXPECT_EQ(beforeSpawn.waterBindingSideTableEntryCount, 0u);
    EXPECT_EQ(beforeSpawn.terrainBindingSideTableEntryCount, 0u);

    const ECS::EntityHandle featureEntity = runtime.CreateEntity();
    ASSERT_TRUE(featureEntity.IsValid());
    ASSERT_TRUE(runtime.AddFragment<SceneECS::ParticleRuntimeTag>(featureEntity));
    ASSERT_TRUE(runtime.AddFragment<Particle::ParticleEcsConfig>(
        featureEntity,
        {.systemAssetId = AssetId{17}, .configurationRevision = 1, .maxParticleCount = 16}));
    ASSERT_TRUE(runtime.AddFragment<Particle::ParticleEcsIntent>(featureEntity));
    ASSERT_TRUE(runtime.AddFragment<Particle::ParticleEcsState>(featureEntity));
    ASSERT_TRUE(runtime.AddFragment<SceneECS::WaterRuntimeTag>(featureEntity));
    ASSERT_TRUE(runtime.AddFragment<Water::WaterEcsConfig>(
        featureEntity,
        {.surfaceAssetId = AssetId{18}, .materialAssetId = AssetId{19},
         .configurationRevision = 1, .resolution = 8}));
    ASSERT_TRUE(runtime.AddFragment<Water::WaterEcsIntent>(featureEntity));
    ASSERT_TRUE(runtime.AddFragment<Water::WaterEcsState>(featureEntity));
    ASSERT_TRUE(runtime.AddFragment<SceneECS::TerrainRuntimeTag>(featureEntity));
    ASSERT_TRUE(runtime.AddFragment<Terrain::TerrainEcsConfig>(
        featureEntity,
        {.heightmapAssetId = AssetId{20}, .materialAssetId = AssetId{21},
         .configurationRevision = 1, .patchSize = 8, .maxLODLevels = 1}));
    ASSERT_TRUE(runtime.AddFragment<Terrain::TerrainEcsIntent>(featureEntity));
    ASSERT_TRUE(runtime.AddFragment<Terrain::TerrainEcsState>(featureEntity));

    ASSERT_TRUE(composition.Tick(TickRequest()).succeeded)
        << composition.GetDiagnostics().lastDiagnostic;
    const WorldEcsRuntimeServicesDiagnostics active = composition.GetRuntimeDiagnostics();
    EXPECT_EQ(active.particleBindingSideTableEntryCount, 1u);
    EXPECT_EQ(active.particleOutstandingRuntimeCount, 1u);
    EXPECT_EQ(active.particlePublishedSnapshotCount, 1u);
    EXPECT_EQ(active.waterBindingSideTableEntryCount, 1u);
    EXPECT_EQ(active.waterOutstandingRuntimeCount, 1u);
    EXPECT_EQ(active.waterPublishedSnapshotCount, 1u);
    EXPECT_EQ(active.terrainBindingSideTableEntryCount, 1u);
    EXPECT_EQ(active.terrainOutstandingRuntimeCount, 1u);
    EXPECT_EQ(active.terrainPublishedSnapshotCount, 1u);
    EXPECT_GE(active.particleBridgeAuthoritativeReconcileCount, 1u);
    EXPECT_GE(active.waterBridgeAuthoritativeReconcileCount, 1u);
    EXPECT_GE(active.terrainBridgeAuthoritativeReconcileCount, 1u);

    ASSERT_EQ(runtime.RequestDestroy(
                  featureEntity,
                  SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::None)),
              SceneECS::DestroyRequestResult::Accepted);
    ASSERT_TRUE(composition.Tick(TickRequest()).succeeded)
        << composition.GetDiagnostics().lastDiagnostic;
    ASSERT_TRUE(runtime.AcknowledgeCleanup(
        featureEntity,
        SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Render)));
    ASSERT_TRUE(composition.Tick(TickRequest()).succeeded)
        << composition.GetDiagnostics().lastDiagnostic;
    EXPECT_FALSE(particleGateway.IsAlive(Particle::ParticleEcsRuntimeHandle::Create(0, 1)));
    EXPECT_FALSE(waterGateway.IsAlive(Water::WaterEcsRuntimeHandle::Create(0, 1)));
    EXPECT_FALSE(terrainGateway.IsAlive(Terrain::TerrainEcsRuntimeHandle::Create(0, 1)));

    EXPECT_TRUE(composition.PrepareForShutdown(TickRequest()))
        << composition.GetDiagnostics().lastDiagnostic;
    const WorldEcsRuntimeServicesDiagnostics drained = composition.GetRuntimeDiagnostics();
    EXPECT_FALSE(drained.initialized);
    EXPECT_TRUE(drained.shutdownComplete);
}

TEST(EcsWorldRuntimeCompositionValidation,
     BindsRootMotionToTheExactEntityPhysicsBodyWithoutExposingItsHandle)
{
    SceneECS::SceneEcsRuntime runtime;
    AdvancingTransport transport;
    EcsRenderFramePipeline pipeline(transport);
    WorldEcsRuntimeComposition composition(runtime, pipeline);
    ASSERT_TRUE(composition.Initialize());

    const ECS::EntityHandle animatedBody = runtime.CreateEntity();
    ASSERT_TRUE(animatedBody.IsValid());
    SceneECS::RigidBody rigidBody;
    rigidBody.motionType = SceneECS::RigidBodyMotionType::Kinematic;
    rigidBody.allowSleep = false;
    SceneECS::AnimationSkeletonBinding skeleton;
    skeleton.animationAssetValue = 42;
    skeleton.sourceSkinIndex = 0;
    skeleton.boneCount = 2;
    SceneECS::Animator animator;
    animator.evaluationMode = SceneECS::AnimationEvaluationMode::Fixed;
    animator.rootMotionEnabled = true;
    ASSERT_TRUE(runtime.AddFragment<SceneECS::RigidBody>(animatedBody, rigidBody));
    ASSERT_TRUE(runtime.AddFragment<SceneECS::Collider>(animatedBody));
    ASSERT_TRUE(runtime.AddFragment<SceneECS::PhysicsBodyState>(animatedBody));
    ASSERT_TRUE(runtime.AddFragment<SceneECS::AnimationSkeletonBinding>(animatedBody, skeleton));
    ASSERT_TRUE(runtime.AddFragment<SceneECS::Animator>(animatedBody, animator));
    ASSERT_TRUE(runtime.AddFragment<SceneECS::AnimationPoseState>(animatedBody));
    ASSERT_TRUE(runtime.AddFragment<SceneECS::RootMotionIntent>(animatedBody));

    const ECS::EntityHandle animationOnly = runtime.CreateEntity();
    ASSERT_TRUE(animationOnly.IsValid());
    ASSERT_TRUE(runtime.AddFragment<SceneECS::AnimationSkeletonBinding>(animationOnly, skeleton));
    SceneECS::Animator animationOnlyAnimator = animator;
    animationOnlyAnimator.rootMotionEnabled = false;
    ASSERT_TRUE(runtime.AddFragment<SceneECS::Animator>(animationOnly, animationOnlyAnimator));
    ASSERT_TRUE(runtime.AddFragment<SceneECS::AnimationPoseState>(animationOnly));
    ASSERT_TRUE(runtime.AddFragment<SceneECS::RootMotionIntent>(animationOnly));

    ASSERT_TRUE(composition.Tick(TickRequest()).succeeded);

    const WorldEcsAnimationPhysicsBindingResult applied =
        composition.BindAnimationRootMotionPhysics(runtime.GetEntityRef(animatedBody));
    EXPECT_TRUE(applied.IsApplied()) << applied.diagnostic;
    EXPECT_EQ(applied.code, WorldEcsAnimationPhysicsBindingCode::Applied);

    const WorldEcsAnimationPhysicsBindingResult noBody =
        composition.BindAnimationRootMotionPhysics(runtime.GetEntityRef(animationOnly));
    EXPECT_EQ(noBody.code, WorldEcsAnimationPhysicsBindingCode::PhysicsBodyUnavailable);

    // The first tick evaluated a pose before the exact physics body binding
    // existed, so the first post-bind evaluator-owned root-motion sequence is
    // greater than one.  Both bridges must treat the generation-safe body as
    // the new sequence epoch and consume that intent in the same fixed step.
    ASSERT_TRUE(composition.Tick(TickRequest()).succeeded);
    const WorldEcsRuntimeServicesDiagnostics rootMotionDiagnostics =
        composition.GetRuntimeDiagnostics();
    EXPECT_EQ(rootMotionDiagnostics.animationRootMotionPublicationCount, 1u);
    EXPECT_EQ(rootMotionDiagnostics.animationRootMotionReplayCount, 0u);
    EXPECT_EQ(rootMotionDiagnostics.animationRootMotionGapCount, 0u);
    EXPECT_EQ(rootMotionDiagnostics.physicsRootMotionAppliedCount, 1u);
    EXPECT_EQ(rootMotionDiagnostics.physicsRootMotionReplayCount, 0u);
    EXPECT_EQ(rootMotionDiagnostics.physicsRootMotionGapCount, 0u);

    SceneECS::SceneEcsRuntime foreignRuntime;
    const ECS::EntityHandle foreignEntity = foreignRuntime.CreateEntity();
    ASSERT_TRUE(foreignEntity.IsValid());
    EXPECT_EQ(composition.BindAnimationRootMotionPhysics(
                  foreignRuntime.GetEntityRef(foreignEntity)).code,
              WorldEcsAnimationPhysicsBindingCode::ForeignScene);

    ASSERT_EQ(runtime.RequestDestroy(
                  animatedBody,
                  SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::None)),
              SceneECS::DestroyRequestResult::Accepted);
    EXPECT_EQ(composition.BindAnimationRootMotionPhysics(
                  runtime.GetEntityRef(animatedBody)).code,
              WorldEcsAnimationPhysicsBindingCode::InvalidEntity);

    bool shutdownComplete = false;
    for (uint32 attempt = 0; attempt < 8 && !shutdownComplete; ++attempt)
    {
        shutdownComplete = composition.PrepareForShutdown(TickRequest());
    }
    EXPECT_TRUE(shutdownComplete) << composition.GetDiagnostics().lastDiagnostic;
}

TEST(EcsWorldRuntimeCompositionValidation, ResourceEnvironmentPresentationIsConfirmedThenShutdownDrains)
{
    ResourceScope resources;
    SceneECS::SceneEcsRuntime runtime;
    const ECS::EntityHandle camera = AddCamera(runtime);
    AdvancingTransport transport;
    EcsRenderFramePipeline pipeline(transport);
    WorldEcsRuntimeComposition composition(runtime, pipeline, &resources.m_resources);
    ASSERT_TRUE(composition.Initialize());

    std::string error;
    const auto environment = composition.RequestEnvironment(
        {.path = "composition.hdr",
         .expectedSceneRuntimeId = runtime.GetSceneRuntimeId()},
        error);
    ASSERT_TRUE(environment.IsValid()) << error;

    bool fullyResident = false;
    uint64 sequence = 1;
    for (; sequence <= 24 && !fullyResident; ++sequence)
    {
        resources.Tick();
        const auto tick = composition.Tick(TickRequest());
        ASSERT_TRUE(tick.succeeded) << composition.GetDiagnostics().lastDiagnostic;
        EXPECT_TRUE(tick.modelCoordinatorUpdated);
        EXPECT_TRUE(tick.environmentCoordinatorUpdated);
        PublishLatest(runtime, pipeline, sequence);
        const auto confirmationTick = composition.Tick(TickRequest());
        ASSERT_TRUE(confirmationTick.succeeded)
            << composition.GetDiagnostics().lastDiagnostic;
        EXPECT_TRUE(confirmationTick.modelCoordinatorUpdated);
        EXPECT_TRUE(confirmationTick.environmentCoordinatorUpdated);
        const auto status = composition.GetEnvironmentStatus(environment);
        fullyResident = status.has_value() &&
                        status->state == ResourceSceneAdapters::EcsEnvironmentLoadState::FullyResident;
    }
    EXPECT_TRUE(fullyResident);

    ASSERT_TRUE(composition.BeginShutdown());
    EXPECT_TRUE(composition.NeedsRenderPublicationDuringShutdown());
    EXPECT_TRUE(runtime.GetRegistry().IsAlive(camera));
    bool observedRetainedPresentationCamera = false;
    bool observedFinalCameraDestroyRequest = false;
    bool shutdownComplete = false;
    for (; sequence <= 48 && !shutdownComplete; ++sequence)
    {
        resources.Tick();
        ASSERT_TRUE(composition.Tick(TickRequest()).succeeded)
            << composition.GetDiagnostics().lastDiagnostic;
        const bool needsPublication = composition.NeedsRenderPublicationDuringShutdown();
        if (needsPublication)
        {
            observedRetainedPresentationCamera = true;
            EXPECT_TRUE(runtime.GetRegistry().IsAlive(camera));
            PublishLatest(runtime, pipeline, sequence);
        }
        shutdownComplete = composition.PrepareForShutdown(TickRequest());
        if (needsPublication && !composition.NeedsRenderPublicationDuringShutdown())
        {
            observedFinalCameraDestroyRequest = true;
            const SceneECS::SceneEcsDiagnosticsSnapshot finalCameraDiagnostics =
                runtime.GetDiagnosticsSnapshot();
            EXPECT_TRUE(runtime.GetRegistry().IsAlive(camera));
            EXPECT_EQ(finalCameraDiagnostics.pendingDestroyCount, 1U);
            EXPECT_EQ(finalCameraDiagnostics.cleanupRequiredCount, 0U);
        }
    }
    const WorldEcsRuntimeCompositionDiagnostics shutdownDiagnostics =
        composition.GetDiagnostics();
    EXPECT_TRUE(shutdownComplete) << shutdownDiagnostics.lastDiagnostic
                                  << " scene=" << shutdownDiagnostics.scene.entityCount
                                  << " modelHandles=" << shutdownDiagnostics.trackedModelRequestCount
                                  << " environmentHandles="
                                  << shutdownDiagnostics.trackedEnvironmentRequestCount
                                  << " generic="
                                  << (shutdownDiagnostics.genericRetirement.has_value() ?
                                          shutdownDiagnostics.genericRetirement
                                              ->outstandingRetirementCount :
                                          0U)
                                  << " physics=" << shutdownDiagnostics.physicsBridge.activeBodyCount
                                  << " animation="
                                  << shutdownDiagnostics.animationBridge.activeBindingCount
                                  << " publication="
                                  << composition.NeedsRenderPublicationDuringShutdown()
                                  << " cameraAlive=" << runtime.GetRegistry().IsAlive(camera)
                                  << " pending=" << shutdownDiagnostics.scene.pendingDestroyCount
                                  << " cleanup=" << shutdownDiagnostics.scene.cleanupRequiredCount
                                  << " retiring=" << shutdownDiagnostics.scene.retiringCount
                                  << " recyclable=" << shutdownDiagnostics.scene.recyclableCount;
    EXPECT_TRUE(observedRetainedPresentationCamera);
    EXPECT_TRUE(observedFinalCameraDestroyRequest);
    EXPECT_TRUE(composition.GetDiagnostics().processorsCleared);
    EXPECT_EQ(composition.GetDiagnostics().scene.entityCount, 0U);
}

TEST(EcsWorldRuntimeCompositionValidation,
     SceneQualifiedRequestRefsRejectCollidingForeignWorldHandles)
{
    ResourceScope resources;
    SceneECS::SceneEcsRuntime runtimeA;
    SceneECS::SceneEcsRuntime runtimeB;
    AdvancingTransport transportA;
    AdvancingTransport transportB;
    EcsRenderFramePipeline pipelineA(transportA);
    EcsRenderFramePipeline pipelineB(transportB);
    WorldEcsRuntimeComposition compositionA(runtimeA, pipelineA, &resources.m_resources);
    WorldEcsRuntimeComposition compositionB(runtimeB, pipelineB, &resources.m_resources);
    ASSERT_TRUE(compositionA.Initialize());
    ASSERT_TRUE(compositionB.Initialize());

    std::string error;
    const auto environmentA = compositionA.RequestEnvironment(
        {.path = "world-a.hdr", .expectedSceneRuntimeId = runtimeA.GetSceneRuntimeId()}, error);
    ASSERT_TRUE(environmentA.IsValid()) << error;
    const auto environmentB = compositionB.RequestEnvironment(
        {.path = "world-b.hdr", .expectedSceneRuntimeId = runtimeB.GetSceneRuntimeId()}, error);
    ASSERT_TRUE(environmentB.IsValid()) << error;
    EXPECT_EQ(environmentA.handle, environmentB.handle);
    EXPECT_NE(environmentA.sceneRuntimeId, environmentB.sceneRuntimeId);
    EXPECT_FALSE(compositionA.GetEnvironmentStatus(environmentB).has_value());
    EXPECT_FALSE(compositionA.CancelEnvironment(environmentB));

    EXPECT_FALSE(compositionA.RequestModel(
        {.path = "foreign.ecsmodel", .expectedSceneRuntimeId = runtimeB.GetSceneRuntimeId()}, error).IsValid());
    EXPECT_FALSE(compositionA.RequestEnvironment(
        {.path = "foreign.hdr", .expectedSceneRuntimeId = runtimeB.GetSceneRuntimeId()}, error).IsValid());
}

TEST(EcsWorldRuntimeCompositionValidation, ShutdownRetainsOutstandingWorkUntilAnOwnerThreadTickDrainsIt)
{
    SceneECS::SceneEcsRuntime runtime;
    static_cast<void>(AddCamera(runtime));
    AdvancingTransport transport;
    EcsRenderFramePipeline pipeline(transport);
    WorldEcsRuntimeComposition composition(runtime, pipeline);
    ASSERT_TRUE(composition.Initialize());

    ASSERT_TRUE(composition.Tick(TickRequest()).succeeded);
    PublishLatest(runtime, pipeline, 1);
    ASSERT_TRUE(composition.BeginShutdown());
    EXPECT_FALSE(composition.PrepareForShutdown(TickRequest()));
    EXPECT_FALSE(composition.IsShutdownComplete());
    bool drained = false;
    for (uint64 sequence = 2; sequence <= 12 && !drained; ++sequence)
    {
        ASSERT_TRUE(composition.Tick(TickRequest()).succeeded)
            << composition.GetDiagnostics().lastDiagnostic;
        if (composition.NeedsRenderPublicationDuringShutdown())
        {
            PublishLatest(runtime, pipeline, sequence);
        }
        drained = composition.PrepareForShutdown(TickRequest());
    }
    EXPECT_TRUE(drained) << composition.GetDiagnostics().lastDiagnostic;
    EXPECT_TRUE(composition.IsShutdownComplete());
    EXPECT_EQ(composition.GetDiagnostics().scene.entityCount, 0U);
}

TEST(EcsWorldRuntimeCompositionValidation, PendingSnapshotCameraDoesNotBlockShutdown)
{
    SceneECS::SceneEcsRuntime runtime;
    const ECS::EntityHandle camera = AddCamera(runtime);
    AdvancingTransport transport;
    EcsRenderFramePipeline pipeline(transport);
    WorldEcsRuntimeComposition composition(runtime, pipeline);
    ASSERT_TRUE(composition.Initialize());
    ASSERT_TRUE(composition.Tick(TickRequest()).succeeded);
    PublishLatest(runtime, pipeline, 1);
    ASSERT_EQ(runtime.RequestDestroy(
                  camera,
                  SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::None)),
              SceneECS::DestroyRequestResult::Accepted);

    ASSERT_TRUE(composition.BeginShutdown());
    EXPECT_FALSE(composition.NeedsRenderPublicationDuringShutdown());
    EXPECT_TRUE(composition.PrepareForShutdown(TickRequest()))
        << composition.GetDiagnostics().lastDiagnostic;
    EXPECT_TRUE(composition.IsShutdownComplete());
    EXPECT_EQ(composition.GetDiagnostics().scene.entityCount, 0u);
}

TEST(EcsWorldRuntimeCompositionValidation, SharedResourceAnimationEvaluatorRemainsLiveForOtherWorlds)
{
    const auto evaluator = std::make_shared<AnimationSceneAdapters::ResourceAnimationEcsEvaluator>(
        [](uint64) { return std::shared_ptr<const Resource::AnimationResource>{}; });
    WorldEcsRuntimeCompositionOptions options;
    options.resourceAnimationEvaluator = evaluator;

    SceneECS::SceneEcsRuntime firstRuntime;
    SceneECS::SceneEcsRuntime secondRuntime;
    AdvancingTransport firstTransport;
    AdvancingTransport secondTransport;
    EcsRenderFramePipeline firstPipeline(firstTransport);
    EcsRenderFramePipeline secondPipeline(secondTransport);
    WorldEcsRuntimeComposition first(firstRuntime, firstPipeline, nullptr, options);
    WorldEcsRuntimeComposition second(secondRuntime, secondPipeline, nullptr, options);

    ASSERT_TRUE(first.Initialize());
    ASSERT_TRUE(second.Initialize());
    ASSERT_TRUE(first.PrepareForShutdown(TickRequest()));
    EXPECT_FALSE(evaluator->IsShutdown());
    EXPECT_EQ(evaluator->GetSceneDiagnosticsSnapshot(firstRuntime.GetSceneRuntimeId())
                  .activePlaybackCount,
              0u);
    EXPECT_TRUE(second.Tick(TickRequest()).succeeded);
    EXPECT_TRUE(second.PrepareForShutdown(TickRequest()));
    EXPECT_FALSE(evaluator->IsShutdown());
}

TEST(EcsWorldRuntimeCompositionValidation, ShutdownStopsWhenSceneTickFailsUntilFailureIsRemoved)
{
    SceneECS::SceneEcsRuntime runtime;
    AdvancingTransport transport;
    EcsRenderFramePipeline pipeline(transport);
    WorldEcsRuntimeComposition composition(runtime, pipeline);
    ASSERT_TRUE(composition.Initialize());

    std::optional<SceneECS::ProcessorRegistrationScope> failureScope =
        runtime.BeginProcessorRegistrationScope();
    ASSERT_TRUE(failureScope.has_value());
    ASSERT_TRUE(runtime.RegisterProcessor({
        .name = "caller-owned-shutdown-failure",
        .phase = ECS::ProcessorPhase::Gameplay,
        .stepMode = ECS::ProcessorStepMode::Variable,
        .runWithContext = [](ECS::ProcessorExecutionContext& context)
        {
            context.ReportFailure("expected shutdown tick failure");
        },
    }));
    ASSERT_TRUE(runtime.CompileProcessors());
    ASSERT_TRUE(runtime.CloseProcessorRegistrationScope(*failureScope));

    EXPECT_FALSE(composition.PrepareForShutdown(TickRequest()));
    EXPECT_NE(composition.GetDiagnostics().lastDiagnostic.find("Scene tick failed"),
              std::string::npos);
    ASSERT_TRUE(runtime.RemoveProcessorRegistrationScope(*failureScope));
    EXPECT_TRUE(composition.PrepareForShutdown(TickRequest()))
        << composition.GetDiagnostics().lastDiagnostic;
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
