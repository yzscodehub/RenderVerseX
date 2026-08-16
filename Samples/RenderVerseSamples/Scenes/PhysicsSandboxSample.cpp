/** @file PhysicsSandboxSample.cpp @brief Pure-ECS fixed-step physics qualification scene. */

#include "Scenes/PhysicsSandboxSample.h"

#include "Core/MathTypes.h"
#include "RenderContracts/RenderFrameTypes.h"
#include "Samples/ModelCameraFraming.h"
#include "Samples/SampleCLI.h"
#include "Samples/SampleContext.h"
#include "Scene/ECS/RenderFragments.h"

#include <algorithm>
#include <cmath>
#include <span>
#include <sstream>
#include <utility>

namespace RVX
{
    namespace
    {
        constexpr uint32 DynamicBoxCount = 16;
        constexpr uint64 QualificationStepCount = 120;
        constexpr uint64 FixedDeltaNumerator = 1;
        constexpr uint64 FixedDeltaDenominator = 60;
        constexpr uint32 MaxPhysicsSubsteps = 4;
        constexpr float32 FixedPhysicsDelta =
            static_cast<float32>(FixedDeltaNumerator) /
            static_cast<float32>(FixedDeltaDenominator);
        constexpr uint32 ExpectedStaticBodies = 1;
        constexpr uint32 ExpectedDynamicBodies = DynamicBoxCount + 1;
        constexpr uint32 ExpectedKinematicBodies = 1;
        constexpr uint32 ExpectedBodyCount =
            ExpectedStaticBodies + ExpectedDynamicBodies + ExpectedKinematicBodies;
        constexpr float32 PhysicsSandboxVerticalFov = radians(47.0f);
        constexpr float32 PhysicsSandboxPresentationFitMargin = 1.15f;
        constexpr float32 PhysicsSandboxDirectionalLightIntensity = 2.2f;

        const SampleInfo PhysicsSandboxInfo{
            "physics-sandbox",
            "Physics Sandbox",
            "Qualifies fixed-step BuiltIn physics through pure ECS fragments and value diagnostics",
            "interior-rendering-p0a",
            SampleAssetPolicy::Fixed,
            "",
            SampleEnvironmentPolicy::None,
            true,
            SampleRenderPath::Auto,
        };

        const AssessmentAction CreateBodiesAction{
            AssessmentCode("PHYSICS.SANDBOX.ACTION.CREATE_BODIES"),
            "Static, dynamic, and kinematic ECS physics entities were committed atomically."};
        const AssessmentAction SettleAction{
            AssessmentCode("PHYSICS.SANDBOX.ACTION.SETTLE_120"),
            "Dynamic ECS physics entities advanced through at least 120 fixed steps."};
        const AssessmentAction DrivePlatformAction{
            AssessmentCode("PHYSICS.SANDBOX.ACTION.DRIVE_KINEMATIC_120"),
            "The kinematic ECS platform remained bridge-synchronized across 120 fixed steps."};
        const AssessmentAction VerifyContactsAction{
            AssessmentCode("PHYSICS.SANDBOX.ACTION.VERIFY_CONTACTS"),
            "Physics-to-Scene output recorded a finite dynamic floor-contact configuration."};
        const AssessmentAction RebuildColliderAction{
            AssessmentCode("PHYSICS.SANDBOX.ACTION.REBUILD_COLLIDER"),
            "A collider fragment update remained active through a subsequent bridge step."};
        const AssessmentAction DestroyBodyAction{
            AssessmentCode("PHYSICS.SANDBOX.ACTION.DESTROY_BODY"),
            "The probe entity retired and its bridge binding was released."};
        const AssessmentAction RecreateSameSlotAction{
            AssessmentCode("PHYSICS.SANDBOX.ACTION.RECREATE_SAME_SLOT"),
            "A replacement probe entity acquired a live generation-qualified binding."};
        const AssessmentAction RejectStaleAction{
            AssessmentCode("PHYSICS.SANDBOX.ACTION.REJECT_STALE_HANDLE"),
            "A value write through the retired probe generation was rejected."};

        const AssessmentInvariant BuiltInAuthorityInvariant{
            AssessmentCode("PHYSICS.SANDBOX.BUILTIN_AUTHORITY"),
            "The requested and active physics backends are BuiltIn without fallback."};
        const AssessmentInvariant DynamicPullInvariant{
            AssessmentCode("PHYSICS.SANDBOX.DYNAMIC_PHYSICS_TO_SCENE"),
            "A dynamic entity has a finite Physics-to-Scene pose after fixed simulation."};
        const AssessmentInvariant ContactInvariant{
            AssessmentCode("PHYSICS.SANDBOX.DYNAMIC_CONTACTS"),
            "At least one dynamic entity reaches the floor-contact pose interval."};
        const AssessmentInvariant ColliderRefreshInvariant{
            AssessmentCode("PHYSICS.SANDBOX.COLLIDER_REFRESH"),
            "The updated collider value remains bridge-active after a fixed step."};
        const AssessmentInvariant CleanupInvariant{
            AssessmentCode("PHYSICS.SANDBOX.CLEANUP"),
            "Probe retirement removes its bridge-side binding before recreation."};
        const AssessmentInvariant StaleRefInvariant{
            AssessmentCode("PHYSICS.SANDBOX.STALE_REF_REJECT"),
            "A retired Scene-qualified entity generation rejects mutation."};
        const AssessmentInvariant TerminalInvariant{
            AssessmentCode("PHYSICS.SANDBOX.TERMINAL_CENSUS"),
            "The ECS World physics census and bridge cleanup state are exact at completion."};

        const AssessmentMetric FixedStepsMetric{
            AssessmentCode("PHYSICS.SANDBOX.FIXED_STEPS"),
            "count",
            "Physics fixed-step sequence at the stable checkpoint."};
        const AssessmentMetric BodyCensusMetric{
            AssessmentCode("PHYSICS.SANDBOX.BODY_CENSUS"),
            "count",
            "Live ECS bridge body count at the stable checkpoint."};
        const AssessmentMetric ContactEvidenceMetric{
            AssessmentCode("PHYSICS.SANDBOX.CONTACT_EVIDENCE"),
            "count",
            "Dynamic floor-contact configurations observed from ECS value state."};

        const AssessmentCapability BuiltInCapability{
            AssessmentCode("PHYSICS.CAPABILITY.BUILTIN"),
            "The built-in fixed-step ECS physics bridge is active.",
            true,
            "physics-sandbox requires the BuiltIn backend.",
        };
        const AssessmentCapability JoltCapability{
            AssessmentCode("PHYSICS.CAPABILITY.JOLT"),
            "Jolt is not qualified by this built-in bridge scenario.",
            false,
            "Jolt is outside this sample's fixed-step qualification.",
        };

        [[nodiscard]] SceneECS::Skybox MakeProceduralSky()
        {
            return {
                .mode = SceneECS::SkyboxMode::Procedural,
                .sunDirection = normalize(Vec3(0.32f, 0.68f, 0.42f)),
                .sunColor = Vec3(1.0f, 0.95f, 0.86f),
                .zenithColor = Vec3(0.10f, 0.23f, 0.50f),
                .horizonColor = Vec3(0.55f, 0.66f, 0.79f),
                .groundColor = Vec3(0.07f, 0.08f, 0.10f),
                .scatteringIntensity = 0.62f,
                .contributesToLighting = false,
            };
        }

        [[nodiscard]] SceneECS::Light MakeDirectionalLight()
        {
            return {
                .type = SceneECS::LightType::Directional,
                .color = Vec3(1.0f, 0.95f, 0.86f),
                .intensity = PhysicsSandboxDirectionalLightIntensity,
                .shadowBias = 0.001f,
                .castsShadows = true,
            };
        }

        [[nodiscard]] SceneECS::Collider MakeBoxCollider(const Vec3& halfExtents)
        {
            return {
                .shape = SceneECS::ColliderShapeType::Box,
                .boxHalfExtents = halfExtents,
                .friction = 0.75f,
                .restitution = 0.0f,
            };
        }

        [[nodiscard]] SceneECS::RigidBody MakeRigidBody(
            SceneECS::RigidBodyMotionType motionType)
        {
            SceneECS::RigidBody rigidBody;
            rigidBody.motionType = motionType;
            rigidBody.mass = motionType == SceneECS::RigidBodyMotionType::Dynamic
                                 ? 1.0f
                                 : 0.0f;
            rigidBody.linearDamping = 0.0f;
            rigidBody.angularDamping = 0.0f;
            rigidBody.gravityScale =
                motionType == SceneECS::RigidBodyMotionType::Dynamic ? 1.0f : 0.0f;
            rigidBody.allowSleep = false;
            return rigidBody;
        }

        [[nodiscard]] SceneECS::Mesh MakeMesh(AssetId meshAssetId)
        {
            return {.meshAssetId = meshAssetId, .submeshCount = 1u};
        }

        [[nodiscard]] SceneECS::MaterialSlots MakeMaterialSlots(
            AssetId materialAssetId)
        {
            SceneECS::MaterialSlots materials;
            materials.values[0].materialAssetId = materialAssetId;
            materials.count = 1u;
            return materials;
        }

        [[nodiscard]] SceneECS::Visibility MakeVisible()
        {
            return {.visible = true, .castsShadow = true, .receivesShadow = true};
        }

        [[nodiscard]] bool IsFinite(const Vec3& value)
        {
            return std::isfinite(value.x) && std::isfinite(value.y) &&
                   std::isfinite(value.z);
        }

        [[nodiscard]] std::string DescribeModelFailure(
            const ResourceSceneAdapters::EcsSceneAssetLoadStatus& status)
        {
            if (!status.diagnostic.empty())
            {
                return status.diagnostic;
            }
            if (!status.request.error.message.empty())
            {
                return status.request.error.message;
            }
            return "physics-sandbox ECS model request reached a terminal state";
        }
    } // namespace

    const SampleInfo& PhysicsSandboxSample::GetInfo() const noexcept
    {
        return PhysicsSandboxInfo;
    }

    SampleWorldRequirements PhysicsSandboxSample::GetWorldRequirements() const
    {
        SampleWorldRequirements requirements;
        requirements.world.physics.backend = Physics::PhysicsBackendType::BuiltIn;
        requirements.world.physics.fixedTimeStep = FixedPhysicsDelta;
        requirements.world.physics.maxSubSteps = MaxPhysicsSubsteps;
        return requirements;
    }

    SampleAssessmentContract PhysicsSandboxSample::GetAssessmentContract() const
    {
        SampleAssessmentContract contract;
        contract.code = AssessmentCode("SAMPLE.PHYSICS_SANDBOX");
        contract.revision = "2";
        contract.checkpoints = {
            AssessmentCheckpoints::EngineBaseline,
            AssessmentCheckpoints::ScenarioSetup,
            AssessmentCheckpoints::ActionRequested,
            AssessmentCheckpoints::ActionApplied,
            AssessmentCheckpoints::ScenarioStable,
            AssessmentCheckpoints::TeardownBefore,
            AssessmentCheckpoints::TeardownSceneComplete,
            AssessmentCheckpoints::TeardownRenderDrained,
            AssessmentCheckpoints::EngineShutdownComplete,
        };
        contract.actions = {
            CreateBodiesAction,
            SettleAction,
            DrivePlatformAction,
            VerifyContactsAction,
            RebuildColliderAction,
            DestroyBodyAction,
            RecreateSameSlotAction,
            RejectStaleAction,
        };
        contract.invariants = {
            BuiltInAuthorityInvariant,
            DynamicPullInvariant,
            ContactInvariant,
            ColliderRefreshInvariant,
            CleanupInvariant,
            StaleRefInvariant,
            TerminalInvariant,
        };
        contract.metrics = {
            FixedStepsMetric,
            BodyCensusMetric,
            ContactEvidenceMetric,
        };
        contract.capabilities = {BuiltInCapability, JoltCapability};
        return contract;
    }

    bool PhysicsSandboxSample::Setup(SampleContext& context,
                                     std::string& outError)
    {
        m_model = {};
        m_orbitCamera.Reset();
        m_presentationBounds.Reset();
        m_actions = {};
        for (uint32 index = 0;
             index < static_cast<uint32>(ScenarioAction::Count);
             ++index)
        {
            m_actions[index].name = GetActionName(
                static_cast<ScenarioAction>(index));
        }
        m_physicsEntities.clear();
        m_dynamicEntities.clear();
        m_platformEntity = {};
        m_probeEntity = {};
        m_recreatedProbeEntity = {};
        m_lastRuntime = context.runtimeServices.GetRuntimeDiagnostics();
        m_renderPath = context.options.renderPath;
        m_state = ScenarioState::AwaitModel;
        m_failure.clear();
        m_platformStartPosition = {};
        m_settleStartFixedStep = 0;
        m_driveStartFixedStep = 0;
        m_lastKinematicIntentStep = 0;
        m_rebuildWriteStep = 0;
        m_destroyFixedStep = 0;
        m_finalPhysicsStep = 0;
        m_lastObservedPresentationSequence = 0;
        m_kinematicIntentCount = 0;
        m_collisionContactEvidenceCount = 0;
        m_dynamicProbeInitialY = 0.0f;
        m_backendQualified = false;
        m_sharedModelActivated = false;
        m_colliderRefreshRequested = false;
        m_colliderRefreshObserved = false;
        m_probeRetirementRequested = false;
        m_probeRetired = false;
        m_probeRecreated = false;
        m_staleRefRejected = false;
        m_dynamicPhysicsObserved = false;
        m_terminalMetricsPublished = false;
        m_builtinCapabilityPublished = false;
        m_joltCapabilityPublished = false;

        switch (m_renderPath)
        {
            case SampleRenderPath::Auto:
                context.renderSettings.gpuCulling.mode = RenderGPUDrivenMode::Auto;
                break;
            case SampleRenderPath::Direct:
                context.renderSettings.gpuCulling.mode =
                    RenderGPUDrivenMode::ForceDisabled;
                break;
            case SampleRenderPath::GPUDriven:
                context.renderSettings.gpuCulling.mode =
                    RenderGPUDrivenMode::ForceEnabled;
                break;
            default:
                outError = "physics-sandbox received an invalid render path";
                return false;
        }

        const float32 aspect = static_cast<float32>(context.options.width) /
                               static_cast<float32>(
                                   std::max(context.options.height, 1u));
        if (!context.cameras.SetPerspective(
                context.camera, PhysicsSandboxVerticalFov, aspect, 0.05f, 80.0f) ||
            !context.cameras.SetPose(
                context.camera, {.position = Vec3(9.0f, 7.0f, 12.0f)}) ||
            !context.cameras.LookAt(context.camera, Vec3(0.0f, 1.0f, 0.0f)))
        {
            outError = "physics-sandbox could not configure its ECS camera";
            return false;
        }

        context.renderSettings.shadows.enabled = true;
        context.renderSettings.shadows.atlasResolution = 1024u;
        context.renderSettings.shadows.cascadeCount = 2u;
        context.renderSettings.postProcess.enabled = true;
        context.renderSettings.postProcess.enableTAA = false;

        if (!m_lastRuntime.available || !m_lastRuntime.initialized ||
            !m_lastRuntime.physicsInitialized ||
            m_lastRuntime.sceneRuntimeId != context.scene.GetSceneRuntimeId().GetValue() ||
            m_lastRuntime.requestedPhysicsBackend !=
                Physics::PhysicsBackendType::BuiltIn ||
            m_lastRuntime.activePhysicsBackend != Physics::PhysicsBackendType::BuiltIn ||
            m_lastRuntime.physicsBackendFallbackActive)
        {
            outError = "physics-sandbox requires an initialized BuiltIn ECS physics bridge without fallback";
            return false;
        }
        m_backendQualified = true;

        if (!context.sceneLifetime.CreateAndAdoptWithFragments(
                {}, MakeProceduralSky()).IsValid())
        {
            outError = "physics-sandbox could not create its ECS sky";
            return false;
        }
        SceneECS::RuntimeEntityDesc lightDesc;
        lightDesc.localTransform.rotation =
            QuatFromEuler(Vec3(radians(-48.0f), radians(28.0f), 0.0f));
        if (!context.sceneLifetime.CreateAndAdoptWithFragments(
                lightDesc,
                MakeDirectionalLight(),
                SceneECS::Visibility{}).IsValid())
        {
            outError = "physics-sandbox could not create its ECS light";
            return false;
        }

        if (!context.models.RequestByAssetId(
                context.options.assetId, m_model, outError))
        {
            return false;
        }
        static_cast<void>(context.assessment.MarkCheckpoint(
            AssessmentCheckpoints::ScenarioSetup));
        static_cast<void>(context.assessment.MarkCheckpoint(
            AssessmentCheckpoints::ActionRequested));
        return true;
    }

    bool PhysicsSandboxSample::ActivateSharedModel(SampleContext& context)
    {
        if (!m_model.request.IsValid() ||
            m_model.request.sceneRuntimeId != context.scene.GetSceneRuntimeId())
        {
            Fail(context, AssessmentCode("PHYSICS.SANDBOX.MODEL_READY"),
                 "physics-sandbox model request does not belong to this ECS Scene runtime");
            return false;
        }
        if (!m_model.IsCPUReady())
        {
            return false;
        }

        const auto& metadata = m_model.status.modelMetadata;
        if (!metadata.HasPublishedSource() || metadata.meshAssetIds.empty() ||
            metadata.materialAssetIds.empty())
        {
            Fail(context, AssessmentCode("PHYSICS.SANDBOX.MODEL_READY"),
                 "physics-sandbox source model lacks published ECS mesh or material asset values");
            return false;
        }
        m_sharedModelActivated = true;
        return true;
    }

    bool PhysicsSandboxSample::CreateBodies(SampleContext& context,
                                            std::string& outError)
    {
        outError.clear();
        const auto& metadata = m_model.status.modelMetadata;
        if (!m_sharedModelActivated || metadata.meshAssetIds.empty() ||
            metadata.materialAssetIds.empty())
        {
            outError = "physics-sandbox cannot create ECS bodies before source metadata is published";
            return false;
        }

        const AssetId meshAssetId = metadata.meshAssetIds.front();
        const auto materialFor = [&metadata](uint32 materialIndex)
        {
            return metadata.materialAssetIds[
                materialIndex % metadata.materialAssetIds.size()];
        };
        const size_t bodyCount = ExpectedBodyCount;
        if (!context.sceneLifetime.ReserveOwnershipCapacity(bodyCount))
        {
            outError = "physics-sandbox could not reserve ECS lifetime ownership for its body batch";
            return false;
        }

        SceneECS::SceneSpawnTransaction transaction =
            context.scene.BeginSpawnTransaction();
        std::vector<SceneECS::SceneSpawnEntityId> pending;
        pending.reserve(bodyCount);
        const auto stage = [&transaction, &pending, meshAssetId](
                               const SceneECS::RuntimeEntityDesc& desc,
                               const SceneECS::Collider& collider,
                               const SceneECS::RigidBody& rigidBody,
                               AssetId materialAssetId)
        {
            const SceneECS::SceneSpawnEntityId entity = transaction.Create(desc);
            if (!entity.IsValid() ||
                !transaction.Add<SceneECS::Mesh>(entity, MakeMesh(meshAssetId)) ||
                !transaction.Add<SceneECS::MaterialSlots>(
                    entity, MakeMaterialSlots(materialAssetId)) ||
                !transaction.Add<SceneECS::Visibility>(entity, MakeVisible()) ||
                !transaction.Add<SceneECS::Collider>(entity, collider) ||
                !transaction.Add<SceneECS::RigidBody>(entity, rigidBody) ||
                !transaction.Add<SceneECS::PhysicsBodyState>(entity, {}))
            {
                return false;
            }
            pending.push_back(entity);
            return true;
        };
        const auto makeDesc = [](const Vec3& position, const Vec3& halfExtents)
        {
            SceneECS::RuntimeEntityDesc desc;
            desc.localTransform.translation = position;
            desc.localTransform.scale = PhysicsSandboxSample::GetBoxVisualScale(
                halfExtents);
            desc.bounds.extents = halfExtents;
            return desc;
        };

        const Vec3 floorHalfExtents(6.0f, 0.5f, 6.0f);
        if (!stage(makeDesc(Vec3(0.0f, -0.5f, 0.0f), floorHalfExtents),
                   MakeBoxCollider(floorHalfExtents),
                   MakeRigidBody(SceneECS::RigidBodyMotionType::Static),
                   materialFor(GetSharedMaterialIndex(VisualEntityRole::Floor))))
        {
            outError = "physics-sandbox could not stage its ECS floor";
            return false;
        }

        const Vec3 dynamicHalfExtents(0.35f);
        for (uint32 index = 0; index < DynamicBoxCount; ++index)
        {
            const uint32 layoutIndex = (m_deterministicSeed & 1u) == 0u
                                           ? index
                                           : DynamicBoxCount - 1u - index;
            const uint32 row = layoutIndex / 4u;
            const uint32 column = layoutIndex % 4u;
            const Vec3 position(
                -2.4f + static_cast<float32>(column) * 1.6f,
                1.0f + static_cast<float32>(row) * 1.1f,
                -0.4f + static_cast<float32>(row % 2u) * 0.7f);
            if (!stage(makeDesc(position, dynamicHalfExtents),
                       MakeBoxCollider(dynamicHalfExtents),
                       MakeRigidBody(SceneECS::RigidBodyMotionType::Dynamic),
                       materialFor(GetSharedMaterialIndex(
                           VisualEntityRole::Dynamic, index))))
            {
                outError = "physics-sandbox could not stage an ECS dynamic body";
                return false;
            }
        }

        const Vec3 platformHalfExtents(1.8f, 0.2f, 0.8f);
        m_platformStartPosition = Vec3(-5.0f, 0.45f, -0.4f);
        if (!stage(makeDesc(m_platformStartPosition, platformHalfExtents),
                   MakeBoxCollider(platformHalfExtents),
                   MakeRigidBody(SceneECS::RigidBodyMotionType::Kinematic),
                   materialFor(GetSharedMaterialIndex(VisualEntityRole::Platform))))
        {
            outError = "physics-sandbox could not stage its ECS kinematic platform";
            return false;
        }

        const Vec3 probeHalfExtents(0.45f);
        if (!stage(makeDesc(Vec3(3.1f, 1.8f, 0.0f), probeHalfExtents),
                   MakeBoxCollider(probeHalfExtents),
                   MakeRigidBody(SceneECS::RigidBodyMotionType::Dynamic),
                   materialFor(GetSharedMaterialIndex(VisualEntityRole::Probe))))
        {
            outError = "physics-sandbox could not stage its ECS probe";
            return false;
        }

        const SceneECS::SceneSpawnCommitResult result = transaction.Commit();
        if (!result.IsApplied())
        {
            outError = "physics-sandbox ECS body transaction was rejected";
            return false;
        }

        std::vector<SceneECS::SceneEntityRef> entities;
        entities.reserve(pending.size());
        for (const SceneECS::SceneSpawnEntityId entity : pending)
        {
            const SceneECS::SceneEntityRef ref = result.GetEntityRef(entity);
            if (!ref.IsValid() || ref.sceneRuntimeId != context.scene.GetSceneRuntimeId())
            {
                outError = "physics-sandbox body transaction returned an invalid Scene-qualified entity";
                return false;
            }
            entities.push_back(ref);
        }
        if (!context.sceneLifetime.AdoptBatch(entities))
        {
            std::vector<ECS::EntityHandle> handles;
            handles.reserve(entities.size());
            for (const SceneECS::SceneEntityRef entity : entities)
            {
                handles.push_back(entity.entity);
            }
            static_cast<void>(context.scene.RequestDestroyBatch(handles));
            outError = "physics-sandbox could not transfer its ECS body transaction to the lifetime scope";
            return false;
        }

        m_physicsEntities = std::move(entities);
        m_dynamicEntities.assign(m_physicsEntities.begin() + 1,
                                 m_physicsEntities.begin() + 1 + DynamicBoxCount);
        m_platformEntity = m_physicsEntities[1 + DynamicBoxCount];
        m_probeEntity = m_physicsEntities.back();
        return InitializePresentationCamera(context);
    }

    bool PhysicsSandboxSample::CreateProbe(SampleContext& context,
                                            const Vec3& position,
                                            SceneECS::SceneEntityRef& outProbe)
    {
        outProbe = {};
        const auto& metadata = m_model.status.modelMetadata;
        if (metadata.meshAssetIds.empty() || metadata.materialAssetIds.empty())
        {
            return false;
        }
        const Vec3 halfExtents(0.45f);
        SceneECS::RuntimeEntityDesc desc;
        desc.localTransform.translation = position;
        desc.localTransform.scale = GetBoxVisualScale(halfExtents);
        desc.bounds.extents = halfExtents;
        outProbe = context.sceneLifetime.CreateAndAdoptWithFragments(
            desc,
            MakeMesh(metadata.meshAssetIds.front()),
            MakeMaterialSlots(metadata.materialAssetIds[
                GetSharedMaterialIndex(VisualEntityRole::Probe) %
                metadata.materialAssetIds.size()]),
            MakeVisible(),
            MakeBoxCollider(halfExtents),
            MakeRigidBody(SceneECS::RigidBodyMotionType::Dynamic),
            SceneECS::PhysicsBodyState{});
        return outProbe.IsValid() &&
               outProbe.sceneRuntimeId == context.scene.GetSceneRuntimeId();
    }

    bool PhysicsSandboxSample::InitializePresentationCamera(SampleContext& context)
    {
        m_presentationBounds = GetPresentationBounds();
        const float32 aspect = static_cast<float32>(context.options.width) /
                               static_cast<float32>(
                                   std::max(context.options.height, 1u));
        const ModelCameraFrame frame = BuildModelCameraFrame(
            m_presentationBounds, aspect, PhysicsSandboxVerticalFov,
            GetPresentationFitMargin());
        if (!frame.valid)
        {
            return false;
        }

        SampleOrbitCameraSettings settings;
        settings.mode = OrbitCameraMode::ExteriorInspect;
        settings.bounds = m_presentationBounds;
        settings.pivot = frame.target;
        settings.distance = frame.distance;
        settings.pitch = 0.30f;
        settings.maxDistance = std::max(frame.distance * 5.0f, 0.001f);
        settings.zoomExponent = 0.08f;
        settings.minimumFramingScale = 0.60f;
        settings.verticalFovRadians = PhysicsSandboxVerticalFov;
        settings.aspectRatio = aspect;
        m_orbitCamera.Initialize(settings, context.input);
        return m_orbitCamera.IsInitialized() &&
               m_orbitCamera.Apply(context.cameras, context.camera);
    }

    void PhysicsSandboxSample::Update(SampleContext& context, float deltaTime)
    {
        static_cast<void>(deltaTime);
        if (!m_failure.empty() || m_state == ScenarioState::Failed ||
            m_state == ScenarioState::Stable)
        {
            return;
        }

        m_lastRuntime = context.runtimeServices.GetRuntimeDiagnostics();
        if (!m_lastRuntime.available || !m_lastRuntime.initialized ||
            !m_lastRuntime.physicsInitialized ||
            m_lastRuntime.sceneRuntimeId != context.scene.GetSceneRuntimeId().GetValue() ||
            m_lastRuntime.requestedPhysicsBackend !=
                Physics::PhysicsBackendType::BuiltIn ||
            m_lastRuntime.activePhysicsBackend != Physics::PhysicsBackendType::BuiltIn ||
            m_lastRuntime.physicsBackendFallbackActive)
        {
            Fail(context, BuiltInAuthorityInvariant.code,
                 "physics-sandbox lost its qualified BuiltIn ECS physics runtime");
            return;
        }

        const ResourceSceneAdapters::EcsSceneAssetLoadStatus modelStatus =
            context.models.UpdateReadiness(m_model);
        if (modelStatus.IsTerminal())
        {
            Fail(context, AssessmentCode("PHYSICS.SANDBOX.MODEL_READY"),
                 DescribeModelFailure(modelStatus));
            return;
        }

        switch (m_state)
        {
            case ScenarioState::AwaitModel:
            {
                if (!ActivateSharedModel(context))
                {
                    return;
                }
                std::string error;
                if (!CreateBodies(context, error))
                {
                    Fail(context, CreateBodiesAction.code,
                         error.empty() ? "physics-sandbox could not create its ECS body batch"
                                       : std::move(error));
                    return;
                }
                m_state = ScenarioState::AwaitBindings;
                break;
            }

            case ScenarioState::AwaitBindings:
            {
                ActionEvidence& action = GetAction(ScenarioAction::CreateBodies);
                if (!action.prerequisitesSatisfied &&
                    m_lastRuntime.activePhysicsBodyCount == ExpectedBodyCount &&
                    m_lastRuntime.physicsBridgeBindingSideTableEntryCount ==
                        ExpectedBodyCount &&
                    m_lastRuntime.physicsColliderCount == ExpectedBodyCount &&
                    m_lastRuntime.staticPhysicsBodyCount == ExpectedStaticBodies &&
                    m_lastRuntime.dynamicPhysicsBodyCount == ExpectedDynamicBodies &&
                    m_lastRuntime.kinematicPhysicsBodyCount == ExpectedKinematicBodies &&
                    ArePhysicsEntitiesBound(context.scene))
                {
                    const SceneECS::LocalTransform* dynamicTransform =
                        context.scene.GetRegistry().TryGet<SceneECS::LocalTransform>(
                            m_dynamicEntities.front().entity);
                    if (dynamicTransform == nullptr)
                    {
                        Fail(context, DynamicPullInvariant.code,
                             "physics-sandbox dynamic ECS entity has no local transform");
                        return;
                    }
                    m_dynamicProbeInitialY = dynamicTransform->translation.y;
                    action.prerequisitesSatisfied = true;
                    ArmActionForPresentation(ScenarioAction::CreateBodies);
                }
                if (action.passed)
                {
                    m_settleStartFixedStep = m_lastRuntime.physicsFixedStepSequence;
                    m_state = ScenarioState::Settle;
                }
                break;
            }

            case ScenarioState::Settle:
            {
                ActionEvidence& action = GetAction(ScenarioAction::Settle120Steps);
                if (!action.prerequisitesSatisfied &&
                    m_lastRuntime.physicsFixedStepSequence >=
                        m_settleStartFixedStep + QualificationStepCount &&
                    HasDynamicPhysicsOutput(context.scene))
                {
                    action.prerequisitesSatisfied = true;
                    ArmActionForPresentation(ScenarioAction::Settle120Steps);
                }
                if (action.passed)
                {
                    m_driveStartFixedStep = m_lastRuntime.physicsFixedStepSequence;
                    m_lastKinematicIntentStep = m_driveStartFixedStep;
                    m_kinematicIntentCount = 0;
                    m_state = ScenarioState::DriveKinematic;
                }
                break;
            }

            case ScenarioState::DriveKinematic:
            {
                const uint64 targetStep =
                    m_driveStartFixedStep + QualificationStepCount;
                if (m_lastRuntime.physicsFixedStepSequence < targetStep)
                {
                    const uint64 nextStep = std::min(
                        targetStep,
                        m_lastRuntime.physicsFixedStepSequence + 1u);
                    if (nextStep > m_lastKinematicIntentStep)
                    {
                        const SceneECS::LocalTransform* current =
                            context.scene.GetRegistry().TryGet<SceneECS::LocalTransform>(
                                m_platformEntity.entity);
                        if (current == nullptr)
                        {
                            Fail(context, DrivePlatformAction.code,
                                 "physics-sandbox kinematic ECS entity has no local transform");
                            return;
                        }
                        SceneECS::LocalTransform pose = *current;
                        const float32 alpha = static_cast<float32>(
                                                  nextStep - m_driveStartFixedStep) /
                                              static_cast<float32>(
                                                  QualificationStepCount);
                        pose.translation = m_platformStartPosition +
                                           Vec3(alpha * 8.0f, 0.0f, 0.0f);
                        if (!context.scene.SetLocalTransform(
                                m_platformEntity.entity, pose))
                        {
                            Fail(context, DrivePlatformAction.code,
                                 "physics-sandbox could not author a kinematic ECS transform");
                            return;
                        }
                        m_lastKinematicIntentStep = nextStep;
                        ++m_kinematicIntentCount;
                    }
                }

                ActionEvidence& action =
                    GetAction(ScenarioAction::DriveKinematicPlatform120Steps);
                if (!action.prerequisitesSatisfied &&
                    m_lastRuntime.physicsFixedStepSequence >= targetStep &&
                    m_lastKinematicIntentStep == targetStep &&
                    HasActivePhysicsState(context.scene, m_platformEntity, targetStep))
                {
                    const SceneECS::LocalTransform* finalTransform =
                        context.scene.GetRegistry().TryGet<SceneECS::LocalTransform>(
                            m_platformEntity.entity);
                    if (finalTransform == nullptr ||
                        std::abs(finalTransform->translation.x -
                                 (m_platformStartPosition.x + 8.0f)) > 0.01f)
                    {
                        return;
                    }
                    action.prerequisitesSatisfied = true;
                    ArmActionForPresentation(
                        ScenarioAction::DriveKinematicPlatform120Steps);
                }
                if (action.passed)
                {
                    m_state = ScenarioState::VerifyPhysicsOutput;
                }
                break;
            }

            case ScenarioState::VerifyPhysicsOutput:
            {
                uint64 contactEvidence = 0;
                for (const SceneECS::SceneEntityRef dynamic : m_dynamicEntities)
                {
                    const SceneECS::LocalTransform* transform =
                        context.scene.GetRegistry().TryGet<SceneECS::LocalTransform>(
                            dynamic.entity);
                    const SceneECS::PhysicsBodyState* physics =
                        context.scene.GetRegistry().TryGet<SceneECS::PhysicsBodyState>(
                            dynamic.entity);
                    if (transform == nullptr || physics == nullptr ||
                        physics->status != SceneECS::PhysicsBodyBindingStatus::Active ||
                        physics->lastSynchronizedFixedStep == 0 ||
                        !IsFinite(transform->translation))
                    {
                        continue;
                    }
                    m_dynamicPhysicsObserved |=
                        transform->translation.y < m_dynamicProbeInitialY - 0.05f;
                    if (transform->translation.y >= 0.28f &&
                        transform->translation.y <= 0.75f)
                    {
                        ++contactEvidence;
                    }
                }
                m_collisionContactEvidenceCount = std::max(
                    m_collisionContactEvidenceCount, contactEvidence);

                ActionEvidence& action =
                    GetAction(ScenarioAction::VerifyDynamicContacts);
                if (!action.prerequisitesSatisfied && m_dynamicPhysicsObserved &&
                    m_collisionContactEvidenceCount > 0)
                {
                    action.prerequisitesSatisfied = true;
                    ArmActionForPresentation(
                        ScenarioAction::VerifyDynamicContacts);
                }
                if (action.passed)
                {
                    if (!BeginColliderRefresh(context))
                    {
                        Fail(context, ColliderRefreshInvariant.code,
                             "physics-sandbox could not update the probe collider fragment");
                        return;
                    }
                    m_state = ScenarioState::AwaitColliderRefresh;
                }
                break;
            }

            case ScenarioState::AwaitColliderRefresh:
            {
                ActionEvidence& action = GetAction(ScenarioAction::RebuildCollider);
                if (!action.prerequisitesSatisfied &&
                    HasActivePhysicsState(context.scene, m_probeEntity,
                                          m_rebuildWriteStep + 1u))
                {
                    const SceneECS::Collider* collider =
                        context.scene.GetRegistry().TryGet<SceneECS::Collider>(
                            m_probeEntity.entity);
                    if (collider != nullptr &&
                        collider->boxHalfExtents == Vec3(0.7f, 0.35f, 0.5f))
                    {
                        m_colliderRefreshObserved = true;
                        action.prerequisitesSatisfied = true;
                        ArmActionForPresentation(
                            ScenarioAction::RebuildCollider);
                    }
                }
                if (action.passed)
                {
                    if (!BeginProbeRetirement(context))
                    {
                        Fail(context, CleanupInvariant.code,
                             "physics-sandbox could not request probe ECS retirement");
                        return;
                    }
                    m_state = ScenarioState::AwaitProbeRetirement;
                }
                break;
            }

            case ScenarioState::AwaitProbeRetirement:
            {
                ActionEvidence& action = GetAction(ScenarioAction::DestroyBody);
                const bool stale = !context.scene.GetEntityRef(
                    m_probeEntity.entity).IsValid();
                if (!action.prerequisitesSatisfied && stale &&
                    m_lastRuntime.activePhysicsBodyCount == ExpectedBodyCount - 1u &&
                    m_lastRuntime.physicsBridgeBindingSideTableEntryCount ==
                        ExpectedBodyCount - 1u &&
                    m_lastRuntime.pendingPhysicsBridgeCleanupCount == 0u)
                {
                    m_probeRetired = true;
                    action.prerequisitesSatisfied = true;
                    ArmActionForPresentation(ScenarioAction::DestroyBody);
                }
                if (action.passed)
                {
                    context.sceneLifetime.Collect();
                    if (!BeginProbeRecreate(context))
                    {
                        Fail(context, CleanupInvariant.code,
                             "physics-sandbox could not create its replacement ECS probe");
                        return;
                    }
                    m_state = ScenarioState::AwaitProbeRecreate;
                }
                break;
            }

            case ScenarioState::AwaitProbeRecreate:
            {
                ActionEvidence& action =
                    GetAction(ScenarioAction::RecreateSameSlot);
                if (!action.prerequisitesSatisfied &&
                    m_lastRuntime.activePhysicsBodyCount == ExpectedBodyCount &&
                    m_lastRuntime.physicsBridgeBindingSideTableEntryCount ==
                        ExpectedBodyCount &&
                    HasActivePhysicsState(context.scene, m_recreatedProbeEntity,
                                          m_destroyFixedStep + 1u))
                {
                    m_probeRecreated =
                        m_recreatedProbeEntity.IsValid() &&
                        m_recreatedProbeEntity.sceneRuntimeId ==
                            context.scene.GetSceneRuntimeId() &&
                        m_recreatedProbeEntity.entity != m_probeEntity.entity;
                    if (m_probeRecreated)
                    {
                        action.prerequisitesSatisfied = true;
                        ArmActionForPresentation(
                            ScenarioAction::RecreateSameSlot);
                    }
                }
                if (action.passed)
                {
                    if (!BeginStaleRefReject(context))
                    {
                        Fail(context, StaleRefInvariant.code,
                             "physics-sandbox retired ECS probe did not reject mutation");
                        return;
                    }
                    m_state = ScenarioState::AwaitStaleRefReject;
                }
                break;
            }

            case ScenarioState::AwaitStaleRefReject:
            {
                ActionEvidence& action =
                    GetAction(ScenarioAction::RejectStaleHandle);
                if (!action.prerequisitesSatisfied && m_staleRefRejected)
                {
                    action.prerequisitesSatisfied = true;
                    ArmActionForPresentation(
                        ScenarioAction::RejectStaleHandle);
                }
                if (action.passed)
                {
                    m_finalPhysicsStep = m_lastRuntime.physicsFixedStepSequence;
                    m_state = ScenarioState::AwaitTerminalCensus;
                }
                break;
            }

            case ScenarioState::AwaitTerminalCensus:
                if (HasQualifiedTerminalCensus())
                {
                    m_state = ScenarioState::Stable;
                }
                break;

            case ScenarioState::Stable:
            case ScenarioState::Failed:
            default:
                break;
        }
    }

    bool PhysicsSandboxSample::BeginColliderRefresh(SampleContext& context)
    {
        if (m_colliderRefreshRequested || !m_probeEntity.IsValid())
        {
            return false;
        }
        const SceneECS::Collider* current =
            context.scene.GetRegistry().TryGet<SceneECS::Collider>(
                m_probeEntity.entity);
        const SceneECS::LocalTransform* transform =
            context.scene.GetRegistry().TryGet<SceneECS::LocalTransform>(
                m_probeEntity.entity);
        if (current == nullptr || transform == nullptr)
        {
            return false;
        }

        SceneECS::Collider rebuilt = *current;
        rebuilt.boxHalfExtents = Vec3(0.7f, 0.35f, 0.5f);
        SceneECS::LocalTransform visualTransform = *transform;
        visualTransform.scale = GetBoxVisualScale(rebuilt.boxHalfExtents);
        if (!context.scene.SetFragment<SceneECS::Collider>(
                m_probeEntity.entity, rebuilt) ||
            !context.scene.SetLocalTransform(m_probeEntity.entity, visualTransform))
        {
            return false;
        }
        m_rebuildWriteStep = m_lastRuntime.physicsFixedStepSequence;
        m_colliderRefreshRequested = true;
        ArmActionForPresentation(ScenarioAction::RebuildCollider);
        return true;
    }

    bool PhysicsSandboxSample::BeginProbeRetirement(SampleContext& context)
    {
        if (m_probeRetirementRequested || !m_probeEntity.IsValid())
        {
            return false;
        }
        if (context.scene.RequestDestroy(
                m_probeEntity.entity,
                SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::None)) !=
            SceneECS::DestroyRequestResult::Accepted)
        {
            return false;
        }
        m_destroyFixedStep = m_lastRuntime.physicsFixedStepSequence;
        m_probeRetirementRequested = true;
        ArmActionForPresentation(ScenarioAction::DestroyBody);
        return true;
    }

    bool PhysicsSandboxSample::BeginProbeRecreate(SampleContext& context)
    {
        if (m_probeRecreated || m_recreatedProbeEntity.IsValid())
        {
            return false;
        }
        if (!CreateProbe(context, Vec3(3.1f, 1.8f, 0.0f),
                         m_recreatedProbeEntity))
        {
            return false;
        }
        ArmActionForPresentation(ScenarioAction::RecreateSameSlot);
        return true;
    }

    bool PhysicsSandboxSample::BeginStaleRefReject(SampleContext& context)
    {
        if (m_staleRefRejected || !m_probeRetired)
        {
            return false;
        }
        SceneECS::Collider rejected;
        rejected.boxHalfExtents = Vec3(0.45f);
        m_staleRefRejected = !context.scene.SetFragment<SceneECS::Collider>(
            m_probeEntity.entity, rejected);
        ArmActionForPresentation(ScenarioAction::RejectStaleHandle);
        return m_staleRefRejected;
    }

    bool PhysicsSandboxSample::ArePhysicsEntitiesBound(
        const SceneECS::SceneEcsRuntime& scene) const
    {
        if (m_physicsEntities.size() != ExpectedBodyCount)
        {
            return false;
        }
        return std::all_of(
            m_physicsEntities.begin(), m_physicsEntities.end(),
            [&scene](const SceneECS::SceneEntityRef entity)
            {
                return entity.IsValid() &&
                       entity.sceneRuntimeId == scene.GetSceneRuntimeId() &&
                       scene.GetEntityRef(entity.entity).IsValid() &&
                       scene.GetRegistry().TryGet<SceneECS::PhysicsBodyState>(
                           entity.entity) != nullptr &&
                       scene.GetRegistry().TryGet<SceneECS::PhysicsBodyState>(
                           entity.entity)->status ==
                           SceneECS::PhysicsBodyBindingStatus::Active;
            });
    }

    bool PhysicsSandboxSample::HasDynamicPhysicsOutput(
        const SceneECS::SceneEcsRuntime& scene) const
    {
        return std::any_of(
            m_dynamicEntities.begin(), m_dynamicEntities.end(),
            [&scene](const SceneECS::SceneEntityRef entity)
            {
                const SceneECS::LocalTransform* transform =
                    scene.GetRegistry().TryGet<SceneECS::LocalTransform>(
                        entity.entity);
                const SceneECS::PhysicsBodyState* state =
                    scene.GetRegistry().TryGet<SceneECS::PhysicsBodyState>(
                        entity.entity);
                return transform != nullptr && state != nullptr &&
                       state->status == SceneECS::PhysicsBodyBindingStatus::Active &&
                       state->lastSynchronizedFixedStep != 0 &&
                       IsFinite(transform->translation);
            });
    }

    bool PhysicsSandboxSample::HasActivePhysicsState(
        const SceneECS::SceneEcsRuntime& scene,
        SceneECS::SceneEntityRef entity,
        uint64 minimumStep) const
    {
        if (!entity.IsValid() || entity.sceneRuntimeId != scene.GetSceneRuntimeId() ||
            !scene.GetEntityRef(entity.entity).IsValid())
        {
            return false;
        }
        const SceneECS::PhysicsBodyState* state =
            scene.GetRegistry().TryGet<SceneECS::PhysicsBodyState>(entity.entity);
        return state != nullptr &&
               state->status == SceneECS::PhysicsBodyBindingStatus::Active &&
               state->lastSynchronizedFixedStep >= minimumStep;
    }

    void PhysicsSandboxSample::OnInput(SampleContext& context)
    {
        if (IsInteractiveOrbitEnabled(context.options.smoke) && context.input &&
            m_orbitCamera.IsInitialized())
        {
            static_cast<void>(m_orbitCamera.Update(
                *context.input, context.cameras, context.camera));
        }
    }

    void PhysicsSandboxSample::OnViewportResize(SampleContext& context,
                                                 uint32 width,
                                                 uint32 height)
    {
        if (width == 0 || height == 0)
        {
            return;
        }
        const float32 aspect = static_cast<float32>(width) /
                               static_cast<float32>(height);
        static_cast<void>(m_orbitCamera.SetAspectRatio(
            aspect, context.cameras, context.camera));
    }

    void PhysicsSandboxSample::ObserveDiagnostics(
        const SampleRenderDiagnostics& diagnostics,
        SampleAssessmentChannel& assessment)
    {
        if (diagnostics.lastPresentedFrameSequence.IsAvailable() &&
            diagnostics.lastPresentedFrameSequence.GetValue().has_value())
        {
            m_lastObservedPresentationSequence =
                *diagnostics.lastPresentedFrameSequence.GetValue();
        }
        if (!m_builtinCapabilityPublished)
        {
            static_cast<void>(assessment.MarkCapabilityObservation({
                BuiltInCapability,
                AssessmentCheckpoints::ScenarioSetup,
                DiagnosticValue<bool>::Available(m_backendQualified),
                m_backendQualified ? "BuiltIn ECS physics is active without fallback."
                                   : "BuiltIn ECS physics is not active."}));
            m_builtinCapabilityPublished = true;
        }
        if (!m_joltCapabilityPublished)
        {
            static_cast<void>(assessment.MarkCapabilityObservation({
                JoltCapability,
                AssessmentCheckpoints::ScenarioSetup,
                DiagnosticValue<bool>::Available(false),
                "Jolt is not qualified by this BuiltIn ECS scenario."}));
            m_joltCapabilityPublished = true;
        }
        for (uint32 index = 0;
             index < static_cast<uint32>(ScenarioAction::Count);
             ++index)
        {
            const ScenarioAction action = static_cast<ScenarioAction>(index);
            if (GetAction(action).prerequisitesSatisfied)
            {
                CompleteAction(action, diagnostics, assessment);
            }
        }
        if (m_state == ScenarioState::Stable && AreAllActionsPresented() &&
            HasQualifiedTerminalCensus() && !m_terminalMetricsPublished)
        {
            const auto metric = [](const AssessmentMetric& definition, uint64 value)
            {
                return AssessmentMetricValue{
                    definition, DiagnosticValue<AssessmentScalar>::Available(value)};
            };
            static_cast<void>(assessment.TryPublish(AssessmentSnapshot{
                AssessmentCheckpoints::ScenarioStable,
                {
                    metric(FixedStepsMetric, m_finalPhysicsStep),
                    metric(BodyCensusMetric, m_lastRuntime.activePhysicsBodyCount),
                    metric(ContactEvidenceMetric, m_collisionContactEvidenceCount),
                }}));
            MarkInvariant(assessment, BuiltInAuthorityInvariant);
            MarkInvariant(assessment, DynamicPullInvariant);
            MarkInvariant(assessment, ContactInvariant);
            MarkInvariant(assessment, ColliderRefreshInvariant);
            MarkInvariant(assessment, CleanupInvariant);
            MarkInvariant(assessment, StaleRefInvariant);
            MarkInvariant(assessment, TerminalInvariant);
            m_terminalMetricsPublished = true;
        }
    }

    bool PhysicsSandboxSample::IsActionPresentationCovered(
        const SampleRenderDiagnostics& diagnostics,
        const ActionEvidence& action) const
    {
        return diagnostics.renderSceneValuesAvailable &&
               diagnostics.lastPresentedFrameSequence.IsAvailable() &&
               diagnostics.lastPresentedFrameSequence.GetValue().has_value() &&
               *diagnostics.lastPresentedFrameSequence.GetValue() >
                   action.minimumPresentationSequence &&
               diagnostics.renderSceneAppliedRevision != 0;
    }

    void PhysicsSandboxSample::CompleteAction(
        ScenarioAction action,
        const SampleRenderDiagnostics& diagnostics,
        SampleAssessmentChannel& assessment)
    {
        ActionEvidence& evidence = GetAction(action);
        if (evidence.passed || !IsActionPresentationCovered(diagnostics, evidence))
        {
            return;
        }
        evidence.completedPresentationSequence =
            *diagnostics.lastPresentedFrameSequence.GetValue();
        evidence.targetSceneRevision = diagnostics.renderSceneAppliedRevision;
        evidence.appliedSceneRevision = diagnostics.renderSceneAppliedRevision;
        evidence.passed = true;

        const AssessmentAction* assessmentAction = nullptr;
        switch (action)
        {
            case ScenarioAction::CreateBodies: assessmentAction = &CreateBodiesAction; break;
            case ScenarioAction::Settle120Steps: assessmentAction = &SettleAction; break;
            case ScenarioAction::DriveKinematicPlatform120Steps:
                assessmentAction = &DrivePlatformAction;
                break;
            case ScenarioAction::VerifyDynamicContacts:
                assessmentAction = &VerifyContactsAction;
                break;
            case ScenarioAction::RebuildCollider:
                assessmentAction = &RebuildColliderAction;
                break;
            case ScenarioAction::DestroyBody:
                assessmentAction = &DestroyBodyAction;
                break;
            case ScenarioAction::RecreateSameSlot:
                assessmentAction = &RecreateSameSlotAction;
                break;
            case ScenarioAction::RejectStaleHandle:
                assessmentAction = &RejectStaleAction;
                break;
            case ScenarioAction::Count:
            default:
                return;
        }
        static_cast<void>(assessment.MarkAction(*assessmentAction));
    }

    SampleReadiness PhysicsSandboxSample::GetReadiness(
        const SampleRenderDiagnostics& diagnostics) const
    {
        if (!m_failure.empty())
        {
            return SampleReadiness::Failed(m_failure);
        }
        if (m_state != ScenarioState::Stable || !AreAllActionsPresented() ||
            !HasQualifiedTerminalCensus() || !m_model.IsFullyResident())
        {
            return SampleReadiness::Pending(
                "physics-sandbox is waiting for ECS fixed-step, cleanup, and residency evidence");
        }
        if (!diagnostics.available || !diagnostics.renderSceneValuesAvailable ||
            !diagnostics.lastPresentedFrameSequence.IsAvailable() ||
            !diagnostics.lastPresentedFrameSequence.GetValue().has_value() ||
            *diagnostics.lastPresentedFrameSequence.GetValue() == 0 ||
            diagnostics.visibleObjectCount == 0 || !diagnostics.opaqueExecutionCompleted ||
            !HasRequiredRenderPathEvidence(diagnostics))
        {
            return SampleReadiness::Pending(
                "physics-sandbox is waiting for the selected render path to present its ECS physics scene");
        }
        return SampleReadiness::Ready();
    }

    bool PhysicsSandboxSample::HasRequiredRenderPathEvidence(
        const SampleRenderDiagnostics& diagnostics) const
    {
        if (!diagnostics.gpuDrivenPolicyDecisionAvailable)
        {
            return false;
        }
        switch (m_renderPath)
        {
            case SampleRenderPath::Direct:
                return diagnostics.gpuDrivenRequestedMode == "ForceDisabled" &&
                       diagnostics.gpuDrivenPolicyReason == "ForcedDisabled" &&
                       !diagnostics.gpuDrivenEnabled &&
                       diagnostics.gpuDrivenOpaqueDirectDrawCount > 0 &&
                       !diagnostics.gpuDrivenOpaqueIndirectSubmitted;
            case SampleRenderPath::GPUDriven:
                return diagnostics.gpuDrivenRequestedMode == "ForceEnabled" &&
                       diagnostics.gpuDrivenPolicyReason == "None" &&
                       diagnostics.gpuDrivenEnabled &&
                       diagnostics.gpuDrivenGraphPassRecorded &&
                       diagnostics.gpuDrivenExecutionRecorded &&
                       diagnostics.gpuDrivenOpaqueIndirectRequested &&
                       diagnostics.gpuDrivenOpaqueIndirectEligible &&
                       diagnostics.gpuDrivenOpaqueIndirectSubmitted &&
                       diagnostics.gpuDrivenOpaqueIndirectBatchCount > 0;
            case SampleRenderPath::Auto:
                return diagnostics.gpuDrivenEnabled
                           ? diagnostics.gpuDrivenOpaqueIndirectSubmitted &&
                                 diagnostics.gpuDrivenOpaqueIndirectBatchCount > 0
                           : diagnostics.gpuDrivenOpaqueDirectDrawCount > 0;
            default:
                return false;
        }
    }

    bool PhysicsSandboxSample::ValidateResult(
        const SampleRenderDiagnostics& diagnostics,
        std::string& outError) const
    {
        const SampleReadiness readiness = GetReadiness(diagnostics);
        outError = readiness.reason;
        return readiness.IsReady();
    }

    void PhysicsSandboxSample::AppendReport(SampleFeatureReporter& reporter) const
    {
        reporter.SetScenarioContractRevision(2);
        switch (m_state)
        {
            case ScenarioState::Stable: reporter.SetScenarioPhase("stable"); break;
            case ScenarioState::Failed: reporter.SetScenarioPhase("failed"); break;
            default: reporter.SetScenarioPhase("pending"); break;
        }
        for (uint32 index = 0;
             index < static_cast<uint32>(ScenarioAction::Count);
             ++index)
        {
            const ActionEvidence& action = m_actions[index];
            SampleReportScenarioAction receipt;
            receipt.name = action.name[0] != '\0'
                               ? action.name
                               : GetActionName(
                                     static_cast<ScenarioAction>(index));
            receipt.targetSceneRevision = action.targetSceneRevision;
            receipt.completedPresentationSequence =
                action.completedPresentationSequence;
            receipt.appliedSceneRevision = action.appliedSceneRevision;
            receipt.passed = action.passed;
            static_cast<void>(reporter.AppendScenarioAction(std::move(receipt)));
        }
        const auto appendInvariant = [&reporter](std::string name,
                                                  bool passed,
                                                  std::string evidence)
        {
            static_cast<void>(reporter.AppendScenarioInvariant({
                .name = std::move(name),
                .passed = passed,
                .evidence = std::move(evidence),
            }));
        };
        appendInvariant(
            "BuiltInAuthority", m_backendQualified,
            "requested=" + std::to_string(
                static_cast<uint32>(m_lastRuntime.requestedPhysicsBackend)) +
                ", active=" + std::to_string(
                    static_cast<uint32>(m_lastRuntime.activePhysicsBackend)) +
                ", fallback=" +
                std::to_string(m_lastRuntime.physicsBackendFallbackActive));
        appendInvariant(
            "DynamicPhysicsToScene", m_dynamicPhysicsObserved,
            "dynamic fixed-step output observed=" +
                std::to_string(m_dynamicPhysicsObserved));
        appendInvariant(
            "DynamicFloorContact", m_collisionContactEvidenceCount > 0,
            "finite floor-contact ECS pose count=" +
                std::to_string(m_collisionContactEvidenceCount));
        appendInvariant(
            "ColliderFragmentRefresh", m_colliderRefreshObserved,
            "updated Collider data stayed active through a later bridge step");
        appendInvariant(
            "ProbeCleanup", m_probeRetired,
            "active bridge bodies after retirement=" +
                std::to_string(m_lastRuntime.activePhysicsBodyCount) +
                ", pending bridge cleanup=" +
                std::to_string(m_lastRuntime.pendingPhysicsBridgeCleanupCount));
        appendInvariant(
            "StaleSceneRefRejected", m_staleRefRejected,
            "retired Scene-qualified probe generation rejected a Collider write");
        appendInvariant(
            "TerminalCensus", HasQualifiedTerminalCensus(),
            "bodyCount=" + std::to_string(m_lastRuntime.activePhysicsBodyCount) +
                " colliderCount=" +
                std::to_string(m_lastRuntime.physicsColliderCount) +
                " static=" + std::to_string(m_lastRuntime.staticPhysicsBodyCount) +
                " dynamic=" + std::to_string(m_lastRuntime.dynamicPhysicsBodyCount) +
                " kinematic=" +
                std::to_string(m_lastRuntime.kinematicPhysicsBodyCount) +
                " pendingCleanup=" +
                std::to_string(m_lastRuntime.pendingPhysicsBridgeCleanupCount) +
                " structuralContinuityLoss=" +
                std::to_string(
                    m_lastRuntime.physicsBridgeStructuralContinuityLossCount) +
                " cleanupContinuityLoss=" +
                std::to_string(
                    m_lastRuntime.physicsBridgeCleanupContinuityLossCount));

        const auto appendMetric = [&reporter](std::string name, uint64 value)
        {
            static_cast<void>(reporter.AppendScenarioMetric({
                .name = std::move(name),
                .value = value,
            }));
        };
        appendMetric("fixedDeltaNumerator", FixedDeltaNumerator);
        appendMetric("fixedDeltaDenominator", FixedDeltaDenominator);
        appendMetric("maxSubsteps", MaxPhysicsSubsteps);
        appendMetric("deterministicSeed", m_deterministicSeed);
        appendMetric("sceneRuntimeId", m_lastRuntime.sceneRuntimeId);
        appendMetric("sceneFrameSequence", m_lastRuntime.sceneFrameSequence);
        appendMetric("physicsFixedStepSequence",
                     m_lastRuntime.physicsFixedStepSequence);
        appendMetric("fixedSteps", m_finalPhysicsStep);
        appendMetric("requestedFixedSteps", m_lastRuntime.requestedFixedStepCount);
        appendMetric("executedFixedSteps", m_lastRuntime.executedFixedStepCount);
        appendMetric("staticBodies", m_lastRuntime.staticPhysicsBodyCount);
        appendMetric("dynamicBodies", m_lastRuntime.dynamicPhysicsBodyCount);
        appendMetric("kinematicBodies", m_lastRuntime.kinematicPhysicsBodyCount);
        appendMetric("bodyCount", m_lastRuntime.activePhysicsBodyCount);
        appendMetric("colliderCount", m_lastRuntime.physicsColliderCount);
        appendMetric("bridgeBindings",
                     m_lastRuntime.physicsBridgeBindingSideTableEntryCount);
        appendMetric("pendingBridgeCleanup",
                     m_lastRuntime.pendingPhysicsBridgeCleanupCount);
        appendMetric("bridgeStructuralContinuityLoss",
                     m_lastRuntime.physicsBridgeStructuralContinuityLossCount);
        appendMetric("bridgeCleanupContinuityLoss",
                     m_lastRuntime.physicsBridgeCleanupContinuityLossCount);
        appendMetric("bridgeAuthoritativeReconcile",
                     m_lastRuntime.physicsBridgeAuthoritativeReconcileCount);
        appendMetric("kinematicPoseWrites", m_kinematicIntentCount);
        appendMetric("kinematicLastTargetStep", m_lastKinematicIntentStep);
        appendMetric("dynamicFloorContactEvidence",
                     m_collisionContactEvidenceCount);
        appendMetric("modelFullyResident", m_model.IsFullyResident() ? 1u : 0u);
        appendMetric("staleRefRejected", m_staleRefRejected ? 1u : 0u);

        if (m_model.status.modelMetadata.HasPublishedSource())
        {
            reporter.Enable("ModelResourceLoad");
            reporter.Enable("SceneInstantiation");
            reporter.Enable("PureEcsPhysicsFragments");
            reporter.ResourceDiagnostic("model path=" + m_model.sourcePath.string());
            reporter.ResourceDiagnostic(
                "model meshes=" + std::to_string(
                    m_model.status.modelMetadata.meshAssetIds.size()));
            reporter.ResourceDiagnostic(
                "model materials=" + std::to_string(
                    m_model.status.modelMetadata.materialAssetIds.size()));
        }
        reporter.Enable("WorldEcsPhysicsDiagnostics");
        reporter.ResourceDiagnostic(
            "render path=" + std::string(GetSampleRenderPathName(m_renderPath)));
    }

    Vec3 PhysicsSandboxSample::GetBoxVisualScale(
        const Vec3& halfExtents) noexcept
    {
        return halfExtents * 2.0f;
    }

    uint32 PhysicsSandboxSample::GetSharedMaterialIndex(
        VisualEntityRole role,
        uint32 dynamicIndex) noexcept
    {
        switch (role)
        {
            case VisualEntityRole::Floor: return 0u;
            case VisualEntityRole::Dynamic: return 1u + (dynamicIndex % 5u);
            case VisualEntityRole::Platform: return 4u;
            case VisualEntityRole::Probe: return 5u;
            default: return 0u;
        }
    }

    AABB PhysicsSandboxSample::GetPresentationBounds() noexcept
    {
        return AABB(Vec3(-6.1f, -0.55f, -3.0f),
                    Vec3(5.1f, 5.1f, 3.0f));
    }

    float32 PhysicsSandboxSample::GetPresentationFitMargin() noexcept
    {
        return PhysicsSandboxPresentationFitMargin;
    }

    float32 PhysicsSandboxSample::GetDirectionalLightIntensity() noexcept
    {
        return PhysicsSandboxDirectionalLightIntensity;
    }

    bool PhysicsSandboxSample::IsInteractiveOrbitEnabled(bool smoke) noexcept
    {
        return !smoke;
    }

    const char* PhysicsSandboxSample::GetActionName(
        ScenarioAction action) noexcept
    {
        switch (action)
        {
            case ScenarioAction::CreateBodies: return "CreateBodies";
            case ScenarioAction::Settle120Steps: return "Settle120Steps";
            case ScenarioAction::DriveKinematicPlatform120Steps:
                return "DriveKinematicPlatform120Steps";
            case ScenarioAction::VerifyDynamicContacts:
                return "VerifyDynamicContacts";
            case ScenarioAction::RebuildCollider: return "RebuildCollider";
            case ScenarioAction::DestroyBody: return "DestroyBody";
            case ScenarioAction::RecreateSameSlot: return "RecreateSameSlot";
            case ScenarioAction::RejectStaleHandle: return "RejectStaleHandle";
            case ScenarioAction::Count:
            default:
                return "Invalid";
        }
    }

    PhysicsSandboxSample::ActionEvidence& PhysicsSandboxSample::GetAction(
        ScenarioAction action) noexcept
    {
        return m_actions[static_cast<size_t>(action)];
    }

    const PhysicsSandboxSample::ActionEvidence& PhysicsSandboxSample::GetAction(
        ScenarioAction action) const noexcept
    {
        return m_actions[static_cast<size_t>(action)];
    }

    void PhysicsSandboxSample::ArmActionForPresentation(ScenarioAction action)
    {
        ActionEvidence& evidence = GetAction(action);
        evidence.minimumPresentationSequence = m_lastObservedPresentationSequence;
    }

    void PhysicsSandboxSample::MarkInvariant(
        SampleAssessmentChannel& assessment,
        const AssessmentInvariant& invariant) const
    {
        static_cast<void>(assessment.TryPublish(
            AssessmentInvariantObservation{invariant,
                                           AssessmentCheckpoints::ActionApplied}));
    }

    void PhysicsSandboxSample::Fail(SampleContext& context,
                                    AssessmentCode invariant,
                                    std::string message)
    {
        if (!m_failure.empty())
        {
            return;
        }
        m_failure = std::move(message);
        m_state = ScenarioState::Failed;
        Finding finding;
        finding.code = AssessmentCode("PHYSICS.SANDBOX.FAILURE");
        finding.subsystemCode = AssessmentCode("PHYSICS");
        finding.invariantCode = std::move(invariant);
        finding.checkpoint = AssessmentCheckpoints::ActionApplied;
        finding.classification = FindingClass::ContractViolation;
        finding.severity = FindingSeverity::Error;
        finding.confidence = FindingConfidence::Confirmed;
        finding.summary = "physics-sandbox ECS qualification failed";
        finding.detail = m_failure;
        finding.expected = "The fixed-step ECS physics bridge remains authoritative.";
        finding.observed = m_failure;
        finding.gating = true;
        finding.blockingReason = m_failure;
        static_cast<void>(context.assessment.TryPublish(std::move(finding)));
    }

    bool PhysicsSandboxSample::AreAllActionsPresented() const noexcept
    {
        return std::all_of(m_actions.begin(), m_actions.end(),
                           [](const ActionEvidence& action)
                           {
                               return action.passed;
                           });
    }

    bool PhysicsSandboxSample::HasQualifiedTerminalCensus() const noexcept
    {
        return m_backendQualified && m_lastRuntime.available &&
               m_lastRuntime.initialized && m_lastRuntime.lastTickSucceeded &&
               m_lastRuntime.physicsInitialized &&
               m_lastRuntime.requestedPhysicsBackend ==
                   Physics::PhysicsBackendType::BuiltIn &&
               m_lastRuntime.activePhysicsBackend ==
                   Physics::PhysicsBackendType::BuiltIn &&
               !m_lastRuntime.physicsBackendFallbackActive &&
               m_lastRuntime.activePhysicsBodyCount == ExpectedBodyCount &&
               m_lastRuntime.physicsBridgeBindingSideTableEntryCount ==
                   ExpectedBodyCount &&
               m_lastRuntime.staticPhysicsBodyCount == ExpectedStaticBodies &&
               m_lastRuntime.dynamicPhysicsBodyCount == ExpectedDynamicBodies &&
               m_lastRuntime.kinematicPhysicsBodyCount == ExpectedKinematicBodies &&
               m_lastRuntime.physicsColliderCount == ExpectedBodyCount &&
               m_lastRuntime.pendingPhysicsBridgeCleanupCount == 0u &&
               m_lastRuntime.physicsBridgeStructuralContinuityLossCount == 0u &&
               m_lastRuntime.physicsBridgeCleanupContinuityLossCount == 0u &&
               m_dynamicPhysicsObserved && m_collisionContactEvidenceCount > 0u &&
               m_colliderRefreshObserved && m_probeRetired && m_probeRecreated &&
               m_staleRefRejected && m_finalPhysicsStep != 0u;
    }

    void PhysicsSandboxSample::Shutdown(SampleContext& context)
    {
        if (m_model.request.IsValid() && !context.models.Cancel(m_model))
        {
            m_failure = "physics-sandbox failed to cancel its ECS model request";
        }
        context.sceneLifetime.Collect();
        m_orbitCamera.Reset();
        m_physicsEntities.clear();
        m_dynamicEntities.clear();
        m_platformEntity = {};
        m_probeEntity = {};
        m_recreatedProbeEntity = {};
    }
} // namespace RVX
