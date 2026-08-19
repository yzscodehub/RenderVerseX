/**
 * @file AnimationCharacterSample.cpp
 * @brief Pure-ECS fixed-step character animation qualification.
 */

#include "Scenes/AnimationCharacterSample.h"

#include "Core/MathTypes.h"
#include "RenderContracts/RenderFrameTypes.h"
#include "Samples/SampleCLI.h"
#include "Samples/SampleContext.h"
#include "Scene/ECS/RenderFragments.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace RVX
{
    namespace
    {
        constexpr float32 FixedDeltaSeconds = 1.0f / 60.0f;
        constexpr uint64 QualificationFixedSteps = 120;
        constexpr float32 ExpectedRootMotionDistance = 2.0f;
        constexpr uint64 ExpectedRootMotionMillimetres = 2000;
        constexpr float32 RootMotionDistanceTolerance = 0.08f;
        constexpr float32 CharacterVerticalFov = radians(44.0f);

        const SampleInfo AnimationCharacterInfo{
            "animation-character",
            "Animation Character",
            "Qualifies pure-ECS skinning, fixed animation, root motion, and kinematic Physics.",
            "casual-female",
            SampleAssetPolicy::Fixed,
            "",
            SampleEnvironmentPolicy::None,
            true,
            SampleRenderPath::Direct,
            {"water-bottle"},
            false,
            SampleWorkloadScale::PullRequest,
            {"casual-female-walk-root-motion"},
            {"casual-female", "casual-female-walk-root-motion"}};

        const AssessmentAction BindCharacterAction{
            AssessmentCode("ANIMATION.CHARACTER.ACTION.BIND_ECS_CHARACTER"),
            "A Scene-qualified motion driver was bound to one exact Animation and Physics body."};
        const AssessmentAction FixedAnimationAction{
            AssessmentCode("ANIMATION.CHARACTER.ACTION.EVALUATE_FIXED_120"),
            "The ECS Animator committed exactly 120 fixed-step poses."};
        const AssessmentAction RootMotionAction{
            AssessmentCode("ANIMATION.CHARACTER.ACTION.CONSUME_ROOT_MOTION_120"),
            "Physics consumed exactly 120 generation-qualified root-motion intents."};
        const AssessmentAction PresentedPaletteAction{
            AssessmentCode("ANIMATION.CHARACTER.ACTION.PRESENT_SKINNING_PALETTE"),
            "A completed Direct frame presented the model's exact ECS skinning palette."};

        const AssessmentInvariant ExactBindingInvariant{
            AssessmentCode("ANIMATION.CHARACTER.EXACT_ECS_BINDING"),
            "SceneRuntimeId, entity generation, animation binding, and Physics body agree."};
        const AssessmentInvariant FixedScheduleInvariant{
            AssessmentCode("ANIMATION.CHARACTER.FIXED_SCHEDULE"),
            "Pose evaluation and fixed Physics advance once per qualified step."};
        const AssessmentInvariant RootMotionInvariant{
            AssessmentCode("ANIMATION.CHARACTER.ROOT_MOTION"),
            "Every published root-motion sequence is consumed exactly once without replay or gap."};
        const AssessmentInvariant PaletteInvariant{
            AssessmentCode("ANIMATION.CHARACTER.PRESENTED_PALETTE"),
            "The rendered model uses a completion-qualified ECS skinning palette receipt."};

        const AssessmentCapability BuiltInCapability{
            AssessmentCode("PHYSICS.CAPABILITY.BUILTIN"),
            "Built-in deterministic Physics is active without fallback.",
            true,
            "animation-character requires Built-in Physics."};
        const AssessmentCapability JoltCapability{
            AssessmentCode("PHYSICS.CAPABILITY.JOLT"),
            "Jolt is outside this deterministic qualification.",
            false,
            "Jolt is not required by animation-character."};

        const AssessmentMetric FixedStepsMetric{
            AssessmentCode("ANIMATION.CHARACTER.FIXED_STEPS"),
            "count",
            "Qualified fixed Physics steps."};
        const AssessmentMetric PoseEvaluationsMetric{
            AssessmentCode("ANIMATION.CHARACTER.POSE_EVALUATIONS"),
            "count",
            "Committed fixed Animator poses."};
        const AssessmentMetric RootMotionIntentsMetric{
            AssessmentCode("ANIMATION.CHARACTER.ROOT_MOTION_INTENTS"),
            "count",
            "Exactly consumed root-motion intents."};
        const AssessmentMetric RootMotionMillimetresMetric{
            AssessmentCode("ANIMATION.CHARACTER.ROOT_MOTION_MILLIMETRES"),
            "millimetres",
            "World-space motion-driver displacement."};
        const AssessmentMetric PaletteHashMetric{
            AssessmentCode("ANIMATION.CHARACTER.PALETTE_HASH"),
            "hash",
            "Completion-qualified skinning palette hash."};

        [[nodiscard]] SceneECS::Skybox MakeProceduralSky()
        {
            return {
                .mode = SceneECS::SkyboxMode::Procedural,
                .sunDirection = normalize(Vec3(0.30f, 0.72f, 0.42f)),
                .sunColor = Vec3(1.0f, 0.94f, 0.84f),
                .zenithColor = Vec3(0.08f, 0.20f, 0.48f),
                .horizonColor = Vec3(0.52f, 0.65f, 0.80f),
                .groundColor = Vec3(0.06f, 0.07f, 0.09f),
                .scatteringIntensity = 0.68f,
                .contributesToLighting = false,
            };
        }

        [[nodiscard]] SceneECS::Light MakeDirectionalLight()
        {
            return {
                .type = SceneECS::LightType::Directional,
                .color = Vec3(1.0f, 0.95f, 0.86f),
                .intensity = 2.8f,
                .shadowBias = 0.001f,
                .castsShadows = true,
            };
        }

        [[nodiscard]] bool IsFinite(const Vec3& value) noexcept
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
            return "The ECS model request reached a terminal non-resident state.";
        }

        [[nodiscard]] std::string DescribeAnimationFailure(
            const AnimationSceneAdapters::EcsAnimationAssetLoadStatus& status)
        {
            if (!status.diagnostic.empty())
            {
                return status.diagnostic;
            }
            if (!status.request.error.message.empty())
            {
                return status.request.error.message;
            }
            return "The ECS animation request reached a terminal non-ready state.";
        }

        void PublishInvariant(SampleAssessmentChannel& assessment,
                              const AssessmentInvariant& invariant)
        {
            static_cast<void>(assessment.TryPublish(AssessmentInvariantObservation{
                invariant, AssessmentCheckpoints::ActionApplied}));
        }
    } // namespace

    const SampleInfo& AnimationCharacterSample::GetInfo() const noexcept
    {
        return AnimationCharacterInfo;
    }

    SampleWorldRequirements AnimationCharacterSample::GetWorldRequirements() const
    {
        SampleWorldRequirements requirements;
        requirements.world.physics.backend = Physics::PhysicsBackendType::BuiltIn;
        requirements.world.physics.fixedTimeStep = FixedDeltaSeconds;
        requirements.world.physics.maxSubSteps = 1;
        return requirements;
    }

    SampleAssessmentContract AnimationCharacterSample::GetAssessmentContract() const
    {
        SampleAssessmentContract contract;
        contract.code = AssessmentCode("SAMPLE.ANIMATION_CHARACTER");
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
            AssessmentCheckpoints::EngineShutdownComplete};
        contract.actions = {BindCharacterAction,
                            FixedAnimationAction,
                            RootMotionAction,
                            PresentedPaletteAction};
        contract.invariants = {ExactBindingInvariant,
                               FixedScheduleInvariant,
                               RootMotionInvariant,
                               PaletteInvariant};
        contract.metrics = {FixedStepsMetric,
                            PoseEvaluationsMetric,
                            RootMotionIntentsMetric,
                            RootMotionMillimetresMetric,
                            PaletteHashMetric};
        contract.capabilities = {BuiltInCapability, JoltCapability};
        return contract;
    }

    bool AnimationCharacterSample::ResolveGPUCullingMode(
        SampleRenderPath renderPath,
        RenderGPUDrivenMode& outMode) noexcept
    {
        switch (renderPath)
        {
            case SampleRenderPath::Auto:
                outMode = RenderGPUDrivenMode::Auto;
                return true;
            case SampleRenderPath::Direct:
                outMode = RenderGPUDrivenMode::ForceDisabled;
                return true;
            case SampleRenderPath::GPUDriven:
                outMode = RenderGPUDrivenMode::ForceEnabled;
                return true;
            default: return false;
        }
    }

    bool AnimationCharacterSample::Setup(SampleContext& context,
                                         std::string& outError)
    {
        m_characterModel = {};
        m_gpuProbeModel = {};
        m_rootMotionAnimation = {};
        m_characterRoot = {};
        m_modelPoseSource = {};
        m_motionDriver = {};
        m_presentationBounds.Reset();
        m_cameraFrame = {};
        m_orbitCamera.Reset();
        m_presentedPalette = {};
        m_scenarioActions = {};
        for (uint32 index = 0;
             index < static_cast<uint32>(ScenarioAction::Count);
             ++index)
        {
            m_scenarioActions[index].name = GetScenarioActionName(
                static_cast<ScenarioAction>(index));
        }
        m_baselineServices = {};
        m_lastServices = context.runtimeServices.GetRuntimeDiagnostics();
        m_rootMotionStart = Vec3(0.0f);
        m_rootMotionEnd = Vec3(0.0f);
        m_baselinePoseSequence = 0;
        m_finalPoseSequence = 0;
        m_completedFixedSteps = 0;
        m_completedRootMotionSteps = 0;
        m_qualificationPhysicsBodyHandlePacked = 0;
        m_lastQualifiedRootMotionSequence = 0;
        m_observedQualifiedRootMotionIntentCount = 0;
        m_lastObservedPresentationSequence = 0;
        m_rootMotionDistance = 0.0f;
        m_state = State::AwaitAssets;
        m_renderPath = context.options.renderPath;
        m_failure.clear();
        m_characterConfigured = false;
        m_rootMotionBound = false;
        m_stableAssessmentPublished = false;

        if (!m_lastServices.available || !m_lastServices.initialized ||
            !m_lastServices.physicsInitialized ||
            m_lastServices.requestedPhysicsBackend != Physics::PhysicsBackendType::BuiltIn ||
            m_lastServices.activePhysicsBackend != Physics::PhysicsBackendType::BuiltIn ||
            m_lastServices.physicsBackendFallbackActive ||
            !m_lastServices.animationBridgeAvailable)
        {
            outError =
                "animation-character requires the pure-ECS Animation bridge and Built-in Physics";
            return false;
        }

        RenderGPUDrivenMode gpuCullingMode = RenderGPUDrivenMode::Auto;
        if (!ResolveGPUCullingMode(m_renderPath, gpuCullingMode))
        {
            outError = "animation-character received an invalid render path";
            return false;
        }
        context.renderSettings.gpuCulling.mode = gpuCullingMode;
        context.renderSettings.shadows.enabled = true;
        context.renderSettings.shadows.atlasResolution = 1024u;
        context.renderSettings.shadows.cascadeCount = 2u;
        context.renderSettings.postProcess.enabled = true;
        context.renderSettings.postProcess.enableTAA = false;
        context.renderSettings.postProcess.enableBloom = false;

        const float32 aspect =
            static_cast<float32>(context.options.width) /
            static_cast<float32>(std::max(context.options.height, 1u));
        if (!context.cameras.SetPerspective(
                context.camera, CharacterVerticalFov, aspect, 0.05f, 100.0f) ||
            !context.cameras.SetPose(
                context.camera, {.position = Vec3(5.5f, 3.1f, 7.5f)}) ||
            !context.cameras.LookAt(context.camera, Vec3(0.0f, 1.1f, 0.0f)))
        {
            outError = "animation-character could not configure its ECS camera";
            return false;
        }

        if (!context.sceneLifetime.CreateAndAdoptWithFragments(
                {}, MakeProceduralSky()).IsValid())
        {
            outError = "animation-character could not create its ECS procedural sky";
            return false;
        }
        SceneECS::RuntimeEntityDesc lightDesc;
        lightDesc.localTransform.rotation =
            QuatFromEuler(Vec3(radians(-50.0f), radians(26.0f), 0.0f));
        if (!context.sceneLifetime.CreateAndAdoptWithFragments(
                lightDesc,
                MakeDirectionalLight(),
                SceneECS::Visibility{}).IsValid())
        {
            outError = "animation-character could not create its ECS directional light";
            return false;
        }

        if (!context.models.RequestByAssetId(
                "casual-female", m_characterModel, outError, nullptr) ||
            !context.models.RequestByAssetId(
                "water-bottle", m_gpuProbeModel, outError, nullptr) ||
            !context.animations.RequestByAssetId(
                "casual-female-walk-root-motion", m_rootMotionAnimation, outError))
        {
            return false;
        }

        static_cast<void>(context.assessment.MarkCapabilityObservation({
            BuiltInCapability,
            AssessmentCheckpoints::ScenarioSetup,
            DiagnosticValue<bool>::Available(true),
            "Built-in ECS Physics is active without fallback."}));
        static_cast<void>(context.assessment.MarkCapabilityObservation({
            JoltCapability,
            AssessmentCheckpoints::ScenarioSetup,
            DiagnosticValue<bool>::Available(false),
            "Jolt is intentionally outside this deterministic sample."}));
        static_cast<void>(context.assessment.MarkCheckpoint(
            AssessmentCheckpoints::ScenarioSetup));
        static_cast<void>(context.assessment.MarkCheckpoint(
            AssessmentCheckpoints::ActionRequested));
        return true;
    }

    void AnimationCharacterSample::Update(SampleContext& context,
                                          float deltaTime)
    {
        static_cast<void>(deltaTime);
        const ResourceSceneAdapters::EcsSceneAssetLoadStatus characterStatus =
            context.models.UpdateReadiness(m_characterModel);
        const ResourceSceneAdapters::EcsSceneAssetLoadStatus probeStatus =
            context.models.UpdateReadiness(m_gpuProbeModel);
        const AnimationSceneAdapters::EcsAnimationAssetLoadStatus animationStatus =
            context.animations.UpdateReadiness(m_rootMotionAnimation);
        m_lastServices = context.runtimeServices.GetRuntimeDiagnostics();

        if ((characterStatus.IsTerminal() && !m_characterModel.IsFullyResident()) ||
            (probeStatus.IsTerminal() && !m_gpuProbeModel.IsFullyResident()))
        {
            Fail(context,
                 ExactBindingInvariant.code,
                 DescribeModelFailure(!m_characterModel.IsFullyResident() ?
                                          characterStatus :
                                          probeStatus));
            return;
        }
        if (animationStatus.IsTerminal() && !m_rootMotionAnimation.IsReady())
        {
            Fail(context,
                 ExactBindingInvariant.code,
                 DescribeAnimationFailure(animationStatus));
            return;
        }
        if (!m_lastServices.lastTickSucceeded && m_lastServices.sceneFrameSequence != 0)
        {
            Fail(context,
                 FixedScheduleInvariant.code,
                 "The ECS World reported a failed Scene tick during qualification.");
            return;
        }
        if (m_state == State::Failed || m_state == State::Stable)
        {
            return;
        }

        if (m_state == State::AwaitAssets)
        {
            if (m_characterModel.IsFullyResident() &&
                m_gpuProbeModel.IsFullyResident() &&
                m_rootMotionAnimation.IsReady() && ConfigureCharacter(context))
            {
                m_state = State::AwaitBridgeReconcile;
            }
            return;
        }

        if (m_state == State::AwaitBridgeReconcile)
        {
            static_cast<void>(TryBindRootMotionPhysics(context));
            return;
        }

        if (m_state != State::QualifyingFixedSteps)
        {
            return;
        }

        const SceneECS::AnimationPoseState* const pose =
            context.scene.GetRegistry().TryGet<SceneECS::AnimationPoseState>(
                m_motionDriver.entity);
        const SceneECS::PhysicsBodyState* const body =
            context.scene.GetRegistry().TryGet<SceneECS::PhysicsBodyState>(
                m_motionDriver.entity);
        const SceneECS::RootMotionIntent* const intent =
            context.scene.GetRegistry().TryGet<SceneECS::RootMotionIntent>(
                m_motionDriver.entity);
        if (pose == nullptr || body == nullptr || intent == nullptr ||
            pose->status == SceneECS::AnimationBindingStatus::EvaluatorRejected ||
            pose->status == SceneECS::AnimationBindingStatus::InvalidConfiguration ||
            pose->status == SceneECS::AnimationBindingStatus::InvalidPhysicsBinding ||
            pose->status == SceneECS::AnimationBindingStatus::RootMotionSequenceRejected ||
            body->status != SceneECS::PhysicsBodyBindingStatus::Active)
        {
            Fail(context,
                 ExactBindingInvariant.code,
                 "The ECS motion driver lost its active Animation or Physics binding.");
            return;
        }
        if (pose->poseSequence < m_baselinePoseSequence)
        {
            Fail(context,
                 FixedScheduleInvariant.code,
                 "The ECS pose sequence moved backwards.");
            return;
        }

        const uint64 poseDelta = pose->poseSequence - m_baselinePoseSequence;
        if (poseDelta > 0)
        {
            std::string epochError;
            if (!ObserveQualifiedRootMotionIntent(*intent, epochError))
            {
                Fail(context, RootMotionInvariant.code, std::move(epochError));
                return;
            }
        }
        if (poseDelta < QualificationFixedSteps)
        {
            return;
        }
        if (poseDelta > QualificationFixedSteps)
        {
            Fail(context,
                 FixedScheduleInvariant.code,
                 "The ECS Animator advanced beyond the exact 120-step boundary.");
            return;
        }

        SceneECS::Animator animator =
            *context.scene.GetRegistry().TryGet<SceneECS::Animator>(m_motionDriver.entity);
        animator.playing = false;
        if (!context.scene.SetFragment(m_motionDriver.entity, animator) ||
            !CompleteFixedQualification(context))
        {
            return;
        }
        m_state = State::AwaitPresentedPalette;
    }

    bool AnimationCharacterSample::ConfigureCharacter(SampleContext& context)
    {
        m_characterRoot = m_characterModel.GetRootEntityRef();
        if (!m_characterRoot.IsValid() ||
            m_characterRoot.sceneRuntimeId != context.scene.GetSceneRuntimeId() ||
            context.scene.GetEntityRef(m_characterRoot.entity) != m_characterRoot)
        {
            Fail(context,
                 ExactBindingInvariant.code,
                 "The character model did not publish an exact live ECS root entity.");
            return false;
        }

        const AssetId sourceModelAssetId =
            m_characterModel.status.modelMetadata.sourceModelAssetId;
        const SceneECS::AnimationSkeletonBinding* selectedBinding = nullptr;
        ECS::EntityHandle selectedEntity = ECS::EntityHandle::Invalid();
        for (const ECS::EntityHandle member : m_characterModel.status.members)
        {
            const SceneECS::AnimationSkeletonBinding* const binding =
                context.scene.GetRegistry().TryGet<SceneECS::AnimationSkeletonBinding>(member);
            if (binding == nullptr || binding->animationAssetValue == 0 ||
                binding->sourceModelAssetValue != sourceModelAssetId.value ||
                binding->sourceSkinIndex < 0 || binding->boneCount == 0 ||
                binding->boneCount != m_rootMotionAnimation.status.boneCount)
            {
                continue;
            }
            if (!selectedEntity.IsValid() || member < selectedEntity)
            {
                selectedEntity = member;
                selectedBinding = binding;
            }
        }
        if (!selectedEntity.IsValid() || selectedBinding == nullptr)
        {
            Fail(context,
                 ExactBindingInvariant.code,
                 "No model entity exposed a skeleton compatible with the external animation asset.");
            return false;
        }
        const SceneECS::AnimationSkeletonBinding selectedBindingValue = *selectedBinding;
        m_modelPoseSource = context.scene.GetEntityRef(selectedEntity);

        uint32 rootMotionClipOrdinal = RVX_INVALID_INDEX;
        for (const AnimationSceneAdapters::EcsAnimationClipMetadata& clip :
             m_rootMotionAnimation.status.clips)
        {
            if (!clip.hasRootMotion)
            {
                continue;
            }
            if (rootMotionClipOrdinal != RVX_INVALID_INDEX)
            {
                Fail(context,
                     ExactBindingInvariant.code,
                     "The qualification animation exposes more than one root-motion clip.");
                return false;
            }
            rootMotionClipOrdinal = clip.ordinal;
        }
        if (rootMotionClipOrdinal == RVX_INVALID_INDEX || !ConfigurePresentation(context))
        {
            if (m_failure.empty())
            {
                Fail(context,
                     ExactBindingInvariant.code,
                     "The character presentation or root-motion clip metadata is incomplete.");
            }
            return false;
        }

        const Vec3 characterSize = m_presentationBounds.GetSize();
        SceneECS::Collider collider;
        collider.shape = SceneECS::ColliderShapeType::Capsule;
        collider.capsuleRadius =
            std::max(0.18f, std::min(characterSize.x, characterSize.z) * 0.20f);
        collider.capsuleHalfHeight = std::max(0.45f, characterSize.y * 0.38f);
        collider.friction = 0.5f;

        SceneECS::RigidBody body;
        body.motionType = SceneECS::RigidBodyMotionType::Kinematic;
        body.allowSleep = false;
        body.gravityScale = 0.0f;

        SceneECS::Animator animator;
        animator.evaluationMode = SceneECS::AnimationEvaluationMode::Fixed;
        animator.playing = false;
        animator.rootMotionEnabled = true;

        m_motionDriver = context.sceneLifetime.CreateAndAdoptWithFragments(
            {},
            selectedBindingValue,
            animator,
            SceneECS::AnimationPoseState{},
            SceneECS::RootMotionIntent{},
            body,
            collider,
            SceneECS::PhysicsBodyState{});
        if (!m_motionDriver.IsValid() ||
            context.scene.Reparent(m_characterRoot.entity,
                                   m_motionDriver.entity,
                                   SceneECS::ReparentMode::KeepWorld) !=
                SceneECS::ReparentResult::Applied)
        {
            Fail(context,
                 ExactBindingInvariant.code,
                 "The ECS motion driver or character hierarchy adoption failed atomically.");
            return false;
        }

        const AnimationSceneAdapters::EcsAnimationBindingPreparationResult prepared =
            context.animations.PrepareCompatibleBinding(
                m_rootMotionAnimation, m_motionDriver, rootMotionClipOrdinal);
        if (!prepared.IsPrepared())
        {
            Fail(context,
                 ExactBindingInvariant.code,
                 prepared.diagnostic.empty() ?
                     "The external root-motion skeleton compatibility proof failed." :
                     prepared.diagnostic);
            return false;
        }

        std::vector<SceneECS::SceneEntityRef> skinnedMeshMembers;
        for (const ECS::EntityHandle member : m_characterModel.status.members)
        {
            const SceneECS::SkinnedMeshBinding* const meshBinding =
                context.scene.GetRegistry().TryGet<SceneECS::SkinnedMeshBinding>(member);
            if (meshBinding == nullptr ||
                meshBinding->sourceModelAssetValue != sourceModelAssetId.value ||
                meshBinding->sourceSkinIndex != selectedBindingValue.sourceSkinIndex)
            {
                continue;
            }
            if (meshBinding->poseEntity != m_modelPoseSource.entity)
            {
                Fail(context,
                     ExactBindingInvariant.code,
                     "A character skinned mesh did not retain the selected model pose owner.");
                return false;
            }
            const SceneECS::SceneEntityRef meshRef = context.scene.GetEntityRef(member);
            if (!meshRef.IsValid())
            {
                Fail(context,
                     ExactBindingInvariant.code,
                     "A character skinned mesh lost its generation-qualified Scene identity.");
                return false;
            }
            skinnedMeshMembers.push_back(meshRef);
        }
        if (skinnedMeshMembers.empty())
        {
            Fail(context,
                 ExactBindingInvariant.code,
                 "The character model published no exact skinned mesh bindings to retarget.");
            return false;
        }

        const SceneECS::SkinnedMeshPoseRebindReceipt rebind =
            context.scene.RebindSkinnedMeshPoseOwner({
                .expectedSceneRuntimeId = context.scene.GetSceneRuntimeId(),
                .meshMembers = skinnedMeshMembers,
                .expectedPoseOwner = m_modelPoseSource,
                .newPoseOwner = m_motionDriver,
                .sourceModelAssetValue = sourceModelAssetId.value,
                .sourceSkinIndex = selectedBindingValue.sourceSkinIndex,
            });
        if (!rebind.IsApplied() || rebind.meshCount != skinnedMeshMembers.size())
        {
            Fail(context,
                 ExactBindingInvariant.code,
                 "The character skinning pose-owner rebind was not atomically applied.");
            return false;
        }

        m_characterConfigured = true;
        return true;
    }

    bool AnimationCharacterSample::ConfigurePresentation(SampleContext& context)
    {
        AABB characterBounds;
        AABB probeBounds;
        if (!TryComputeSampleModelRenderableWorldBounds(
                m_characterModel, context.scene, characterBounds) ||
            !TryComputeSampleModelRenderableWorldBounds(
                m_gpuProbeModel, context.scene, probeBounds) ||
            !characterBounds.IsValid() || !probeBounds.IsValid())
        {
            return false;
        }

        const SceneECS::SceneEntityRef probeRoot = m_gpuProbeModel.GetRootEntityRef();
        const SceneECS::LocalTransform* const authoredProbe =
            context.scene.GetRegistry().TryGet<SceneECS::LocalTransform>(probeRoot.entity);
        const float32 sourceHeight = probeBounds.GetSize().y;
        if (!probeRoot.IsValid() || authoredProbe == nullptr ||
            !std::isfinite(sourceHeight) || sourceHeight <= 0.00001f)
        {
            return false;
        }
        const float32 probeScale = 0.45f / sourceHeight;
        SceneECS::LocalTransform placedProbe = *authoredProbe;
        placedProbe.translation =
            authoredProbe->translation * probeScale +
            Vec3(-1.75f - probeBounds.GetCenter().x * probeScale,
                 -probeBounds.GetMin().y * probeScale,
                 0.45f - probeBounds.GetCenter().z * probeScale);
        placedProbe.scale *= Vec3(probeScale);
        if (!context.scene.SetLocalTransform(probeRoot.entity, placedProbe) ||
            !TryComputeSampleModelRenderableWorldBounds(
                m_gpuProbeModel, context.scene, probeBounds))
        {
            return false;
        }

        const Vec3 travel(0.0f, 0.0f, ExpectedRootMotionDistance);
        const AABB travelledCharacter(characterBounds.GetMin() + travel,
                                      characterBounds.GetMax() + travel);
        m_presentationBounds = characterBounds.Union(travelledCharacter).Union(probeBounds);
        m_presentationBounds.Inflate(Vec3(0.35f, 0.25f, 0.35f));
        const float32 aspect =
            static_cast<float32>(context.options.width) /
            static_cast<float32>(std::max(context.options.height, 1u));
        m_cameraFrame = BuildModelCameraFrame(
            m_presentationBounds, aspect, CharacterVerticalFov, 1.18f);
        if (!m_cameraFrame.valid)
        {
            return false;
        }

        SampleOrbitCameraSettings settings;
        settings.mode = OrbitCameraMode::ExteriorInspect;
        settings.bounds = m_presentationBounds;
        settings.pivot = m_cameraFrame.target;
        settings.distance = m_cameraFrame.distance;
        settings.pitch = 0.18f;
        settings.maxDistance = std::max(m_cameraFrame.distance * 5.0f, 0.001f);
        settings.zoomExponent = 0.08f;
        settings.minimumFramingScale = 0.60f;
        settings.verticalFovRadians = CharacterVerticalFov;
        settings.aspectRatio = aspect;
        m_orbitCamera.Initialize(settings, context.input);
        return m_orbitCamera.IsInitialized() &&
               m_orbitCamera.Apply(context.cameras, context.camera) &&
               m_orbitCamera.GetPose().valid;
    }

    bool AnimationCharacterSample::TryBindRootMotionPhysics(SampleContext& context)
    {
        const WorldEcsAnimationPhysicsBindingResult binding =
            context.runtimeServices.BindAnimationRootMotionPhysics(m_motionDriver);
        if (!binding.IsApplied())
        {
            if (binding.code == WorldEcsAnimationPhysicsBindingCode::PhysicsBodyUnavailable ||
                binding.code ==
                    WorldEcsAnimationPhysicsBindingCode::AnimationBindingUnavailable)
            {
                return false;
            }
            Fail(context,
                 ExactBindingInvariant.code,
                 binding.diagnostic.empty() ?
                     "The World ECS service rejected root-motion Physics binding." :
                     binding.diagnostic);
            return false;
        }

        const SceneECS::AnimationPoseState* const pose =
            context.scene.GetRegistry().TryGet<SceneECS::AnimationPoseState>(
                m_motionDriver.entity);
        const SceneECS::LocalTransform* const local =
            context.scene.GetRegistry().TryGet<SceneECS::LocalTransform>(
                m_motionDriver.entity);
        const SceneECS::Animator* const currentAnimator =
            context.scene.GetRegistry().TryGet<SceneECS::Animator>(m_motionDriver.entity);
        if (pose == nullptr || local == nullptr || currentAnimator == nullptr)
        {
            Fail(context,
                 ExactBindingInvariant.code,
                 "The bound motion driver lost required ECS fragments.");
            return false;
        }

        m_baselineServices = context.runtimeServices.GetRuntimeDiagnostics();
        m_baselinePoseSequence = pose->poseSequence;
        m_rootMotionStart = local->translation;
        SceneECS::Animator animator = *currentAnimator;
        animator.playing = true;
        if (!context.scene.SetFragment(m_motionDriver.entity, animator))
        {
            Fail(context,
                 ExactBindingInvariant.code,
                 "The bound ECS Animator rejected playback activation.");
            return false;
        }

        m_rootMotionBound = true;
        m_state = State::QualifyingFixedSteps;
        MarkScenarioActionPrerequisite(ScenarioAction::BindEcsCharacter);
        return true;
    }

    bool AnimationCharacterSample::ObserveQualifiedRootMotionIntent(
        const SceneECS::RootMotionIntent& intent,
        std::string& outError)
    {
        if (intent.sequenceRejected || intent.rootMotionSequence == 0 ||
            intent.physicsBodyHandlePacked == 0 ||
            intent.sceneRuntimeIdValue != m_motionDriver.sceneRuntimeId.GetValue() ||
            intent.targetEntity != m_motionDriver.entity)
        {
            outError =
                "The qualified ECS root-motion intent lost its exact body identity.";
            return false;
        }
        if (m_qualificationPhysicsBodyHandlePacked == 0)
        {
            m_qualificationPhysicsBodyHandlePacked = intent.physicsBodyHandlePacked;
            m_lastQualifiedRootMotionSequence = intent.rootMotionSequence;
            m_observedQualifiedRootMotionIntentCount = 1;
            return true;
        }
        if (intent.physicsBodyHandlePacked != m_qualificationPhysicsBodyHandlePacked)
        {
            outError =
                "The exact Physics-body handle changed during the 120-step root-motion epoch.";
            return false;
        }
        if (intent.rootMotionSequence == m_lastQualifiedRootMotionSequence)
        {
            return true;
        }
        if (intent.rootMotionSequence < m_lastQualifiedRootMotionSequence ||
            m_lastQualifiedRootMotionSequence == std::numeric_limits<uint64>::max() ||
            intent.rootMotionSequence != m_lastQualifiedRootMotionSequence + 1u)
        {
            outError =
                "The qualified ECS root-motion sequence was not contiguous for one body epoch.";
            return false;
        }
        m_lastQualifiedRootMotionSequence = intent.rootMotionSequence;
        ++m_observedQualifiedRootMotionIntentCount;
        return true;
    }

    bool AnimationCharacterSample::CompleteFixedQualification(SampleContext& context)
    {
        const SceneECS::AnimationPoseState* const pose =
            context.scene.GetRegistry().TryGet<SceneECS::AnimationPoseState>(
                m_motionDriver.entity);
        const SceneECS::PhysicsBodyState* const body =
            context.scene.GetRegistry().TryGet<SceneECS::PhysicsBodyState>(
                m_motionDriver.entity);
        const SceneECS::RootMotionIntent* const intent =
            context.scene.GetRegistry().TryGet<SceneECS::RootMotionIntent>(
                m_motionDriver.entity);
        const SceneECS::LocalTransform* const local =
            context.scene.GetRegistry().TryGet<SceneECS::LocalTransform>(
                m_motionDriver.entity);
        m_lastServices = context.runtimeServices.GetRuntimeDiagnostics();
        if (pose == nullptr || body == nullptr || intent == nullptr || local == nullptr)
        {
            Fail(context,
                 ExactBindingInvariant.code,
                 "The completed motion driver lost its value evidence.");
            return false;
        }

        m_finalPoseSequence = pose->poseSequence;
        m_completedFixedSteps =
            m_lastServices.physicsFixedStepSequence -
            m_baselineServices.physicsFixedStepSequence;
        m_completedRootMotionSteps =
            m_lastServices.physicsRootMotionAppliedCount -
            m_baselineServices.physicsRootMotionAppliedCount;
        m_rootMotionEnd = local->translation;
        m_rootMotionDistance = length(m_rootMotionEnd - m_rootMotionStart);
        const uint64 poseDelta = m_finalPoseSequence - m_baselinePoseSequence;
        const uint64 fixedEvaluationDelta =
            m_lastServices.animationFixedEvaluationCount -
            m_baselineServices.animationFixedEvaluationCount;
        const uint64 publicationDelta =
            m_lastServices.animationRootMotionPublicationCount -
            m_baselineServices.animationRootMotionPublicationCount;

        const bool exactCounts =
            poseDelta == QualificationFixedSteps &&
            m_completedFixedSteps == QualificationFixedSteps &&
            fixedEvaluationDelta == QualificationFixedSteps &&
            publicationDelta == QualificationFixedSteps &&
            m_completedRootMotionSteps == QualificationFixedSteps;
        const bool noFailures =
            m_lastServices.animationRejectedEvaluationCount ==
                m_baselineServices.animationRejectedEvaluationCount &&
            m_lastServices.animationRootMotionReplayCount ==
                m_baselineServices.animationRootMotionReplayCount &&
            m_lastServices.animationRootMotionGapCount ==
                m_baselineServices.animationRootMotionGapCount &&
            m_lastServices.physicsRootMotionRejectedCount ==
                m_baselineServices.physicsRootMotionRejectedCount &&
            m_lastServices.physicsRootMotionReplayCount ==
                m_baselineServices.physicsRootMotionReplayCount &&
            m_lastServices.physicsRootMotionGapCount ==
                m_baselineServices.physicsRootMotionGapCount;
        const bool exactIntent =
            !intent->pending && !intent->sequenceRejected &&
            intent->sceneRuntimeIdValue == m_motionDriver.sceneRuntimeId.GetValue() &&
            intent->targetEntity == m_motionDriver.entity &&
            intent->physicsBodyHandlePacked == m_qualificationPhysicsBodyHandlePacked &&
            intent->sourcePoseSequence == m_finalPoseSequence &&
            intent->rootMotionSequence == publicationDelta &&
            intent->rootMotionSequence == m_completedRootMotionSteps &&
            m_lastQualifiedRootMotionSequence == intent->rootMotionSequence &&
            m_observedQualifiedRootMotionIntentCount == QualificationFixedSteps &&
            m_qualificationPhysicsBodyHandlePacked != 0;
        const bool exactDistance =
            IsFinite(m_rootMotionStart) && IsFinite(m_rootMotionEnd) &&
            std::isfinite(m_rootMotionDistance) &&
            std::abs(m_rootMotionDistance - ExpectedRootMotionDistance) <=
                RootMotionDistanceTolerance &&
            static_cast<uint64>(std::llround(m_rootMotionDistance * 1000.0f)) ==
                ExpectedRootMotionMillimetres;
        if (!exactCounts || !noFailures || !exactIntent || !exactDistance ||
            body->status != SceneECS::PhysicsBodyBindingStatus::Active ||
            body->lastSynchronizedFixedStep <
                m_baselineServices.physicsFixedStepSequence + QualificationFixedSteps)
        {
            Fail(context,
                 RootMotionInvariant.code,
                 "The 120-step ECS Animation-to-Physics proof was incomplete or non-deterministic.");
            return false;
        }
        MarkScenarioActionPrerequisite(ScenarioAction::EvaluateFixed120);
        MarkScenarioActionPrerequisite(ScenarioAction::ConsumeRootMotion120);
        return true;
    }

    void AnimationCharacterSample::OnInput(SampleContext& context)
    {
        if (m_characterConfigured && m_orbitCamera.IsInitialized() && context.input != nullptr)
        {
            static_cast<void>(m_orbitCamera.Update(
                *context.input, context.cameras, context.camera));
        }
    }

    void AnimationCharacterSample::OnViewportResize(SampleContext& context,
                                                    uint32 width,
                                                    uint32 height)
    {
        if (width == 0 || height == 0)
        {
            return;
        }
        const float32 aspect =
            static_cast<float32>(width) / static_cast<float32>(height);
        if (!m_orbitCamera.SetAspectRatio(
                aspect, context.cameras, context.camera))
        {
            static_cast<void>(context.cameras.SetAspectRatio(context.camera, aspect));
        }
    }

    bool AnimationCharacterSample::HasExactPresentedPalette(
        const SampleRenderDiagnostics& diagnostics,
        SamplePresentedSkinningPaletteReceipt& outReceipt) const
    {
        if (!diagnostics.presentedSkinningPalettesAvailable ||
            diagnostics.presentedSkinningPalettesOverflow ||
            !m_characterModel.status.modelMetadata.sourceModelAssetId.IsValid())
        {
            return false;
        }
        const uint64 sourceModel =
            m_characterModel.status.modelMetadata.sourceModelAssetId.value;
        for (const SamplePresentedSkinningPaletteReceipt& receipt :
             diagnostics.presentedSkinningPalettes)
        {
            if (receipt.IsValid() && receipt.sourceModelResourceId == sourceModel &&
                receipt.poseSequence == m_finalPoseSequence)
            {
                outReceipt = receipt;
                return true;
            }
        }
        return false;
    }

    bool AnimationCharacterSample::IsScenarioActionPresentationCovered(
        const SampleRenderDiagnostics& diagnostics,
        const ScenarioActionEvidence& action) const
    {
        return action.prerequisitesSatisfied && action.targetSceneRevision != 0 &&
               diagnostics.renderSceneValuesAvailable &&
               diagnostics.lastPresentedFrameSequence.IsAvailable() &&
               diagnostics.lastPresentedFrameSequence.GetValue().has_value() &&
               *diagnostics.lastPresentedFrameSequence.GetValue() >
                   action.minimumPresentationSequence &&
               diagnostics.renderSceneAppliedRevision >= action.targetSceneRevision;
    }

    bool AnimationCharacterSample::AreAllScenarioActionsPresented() const noexcept
    {
        return std::all_of(m_scenarioActions.begin(), m_scenarioActions.end(),
                           [](const ScenarioActionEvidence& action)
                           {
                               return action.passed;
                           });
    }

    const char* AnimationCharacterSample::GetScenarioActionName(
        ScenarioAction action) noexcept
    {
        switch (action)
        {
            case ScenarioAction::BindEcsCharacter: return "BindEcsCharacter";
            case ScenarioAction::EvaluateFixed120: return "EvaluateFixed120";
            case ScenarioAction::ConsumeRootMotion120: return "ConsumeRootMotion120";
            case ScenarioAction::PresentSkinningPalette:
                return "PresentSkinningPalette";
            case ScenarioAction::Count:
            default: return "";
        }
    }

    void AnimationCharacterSample::MarkScenarioActionPrerequisite(
        ScenarioAction action)
    {
        ScenarioActionEvidence& evidence =
            m_scenarioActions[static_cast<size_t>(action)];
        if (evidence.prerequisitesSatisfied)
        {
            return;
        }
        evidence.minimumPresentationSequence = m_lastObservedPresentationSequence;
        evidence.prerequisitesSatisfied = true;
    }

    void AnimationCharacterSample::BindScenarioActionToExactPresentation(
        ScenarioAction action,
        uint64 sceneRevision,
        uint64 presentationSequence)
    {
        ScenarioActionEvidence& evidence =
            m_scenarioActions[static_cast<size_t>(action)];
        if (evidence.passed || evidence.targetSceneRevision != 0)
        {
            return;
        }
        evidence.targetSceneRevision = sceneRevision;
        evidence.minimumPresentationSequence = presentationSequence - 1u;
    }

    void AnimationCharacterSample::CompleteScenarioActions(
        const SampleRenderDiagnostics& diagnostics,
        SampleAssessmentChannel& assessment)
    {
        const auto markAssessmentAction = [&assessment](ScenarioAction action)
        {
            switch (action)
            {
                case ScenarioAction::BindEcsCharacter:
                    static_cast<void>(assessment.MarkAction(BindCharacterAction));
                    break;
                case ScenarioAction::EvaluateFixed120:
                    static_cast<void>(assessment.MarkAction(FixedAnimationAction));
                    break;
                case ScenarioAction::ConsumeRootMotion120:
                    static_cast<void>(assessment.MarkAction(RootMotionAction));
                    break;
                case ScenarioAction::PresentSkinningPalette:
                    static_cast<void>(assessment.MarkAction(PresentedPaletteAction));
                    break;
                case ScenarioAction::Count: break;
            }
        };

        for (uint32 index = 0;
             index < static_cast<uint32>(ScenarioAction::Count);
             ++index)
        {
            ScenarioActionEvidence& evidence = m_scenarioActions[index];
            if (evidence.passed ||
                !IsScenarioActionPresentationCovered(diagnostics, evidence))
            {
                continue;
            }
            evidence.completedPresentationSequence =
                *diagnostics.lastPresentedFrameSequence.GetValue();
            evidence.appliedSceneRevision = diagnostics.renderSceneAppliedRevision;
            evidence.passed = true;
            markAssessmentAction(static_cast<ScenarioAction>(index));
        }
    }

    void AnimationCharacterSample::ObserveDiagnostics(
        const SampleRenderDiagnostics& diagnostics,
        SampleAssessmentChannel& assessment)
    {
        if (diagnostics.lastPresentedFrameSequence.IsAvailable() &&
            diagnostics.lastPresentedFrameSequence.GetValue().has_value())
        {
            m_lastObservedPresentationSequence =
                *diagnostics.lastPresentedFrameSequence.GetValue();
        }
        CompleteScenarioActions(diagnostics, assessment);
        if (m_state != State::AwaitPresentedPalette)
        {
            return;
        }
        if (diagnostics.presentedSkinningPalettesOverflow)
        {
            Fail(assessment,
                 PaletteInvariant.code,
                 "Presented ECS skinning palette evidence overflowed its bounded channel.");
            return;
        }
        if (diagnostics.visibleObjectCount == 0 ||
            !HasExactPresentedPalette(diagnostics, m_presentedPalette))
        {
            return;
        }

        MarkScenarioActionPrerequisite(ScenarioAction::PresentSkinningPalette);
        for (uint32 index = 0;
             index < static_cast<uint32>(ScenarioAction::Count);
             ++index)
        {
            const ScenarioActionEvidence& evidence = m_scenarioActions[index];
            if (!evidence.prerequisitesSatisfied)
            {
                Fail(assessment,
                     PaletteInvariant.code,
                     "The exact palette receipt arrived before its ECS action prerequisites.");
                return;
            }
            BindScenarioActionToExactPresentation(
                static_cast<ScenarioAction>(index),
                diagnostics.renderSceneAppliedRevision,
                m_presentedPalette.presentationSequence);
        }
        CompleteScenarioActions(diagnostics, assessment);
        if (!AreAllScenarioActionsPresented())
        {
            return;
        }
        m_state = State::Stable;
        PublishStableAssessment(assessment);
    }

    void AnimationCharacterSample::PublishStableAssessment(
        SampleAssessmentChannel& assessment)
    {
        if (m_stableAssessmentPublished)
        {
            return;
        }
        PublishInvariant(assessment, ExactBindingInvariant);
        PublishInvariant(assessment, FixedScheduleInvariant);
        PublishInvariant(assessment, RootMotionInvariant);
        PublishInvariant(assessment, PaletteInvariant);

        const auto metric = [](const AssessmentMetric& definition, uint64 value)
        {
            return AssessmentMetricValue{
                definition, DiagnosticValue<AssessmentScalar>::Available(value)};
        };
        static_cast<void>(assessment.TryPublish(AssessmentSnapshot{
            AssessmentCheckpoints::ScenarioStable,
            {metric(FixedStepsMetric, m_completedFixedSteps),
             metric(PoseEvaluationsMetric,
                    m_finalPoseSequence - m_baselinePoseSequence),
             metric(RootMotionIntentsMetric, m_completedRootMotionSteps),
             metric(RootMotionMillimetresMetric,
                    static_cast<uint64>(std::llround(m_rootMotionDistance * 1000.0f))),
             metric(PaletteHashMetric, m_presentedPalette.paletteHash)}}));
        static_cast<void>(assessment.MarkCheckpoint(
            AssessmentCheckpoints::ActionApplied));
        static_cast<void>(assessment.MarkCheckpoint(
            AssessmentCheckpoints::ScenarioStable));
        m_stableAssessmentPublished = true;
    }

    SampleReadiness AnimationCharacterSample::GetReadiness(
        const SampleRenderDiagnostics& diagnostics) const
    {
        if (!m_failure.empty())
        {
            return SampleReadiness::Failed(m_failure);
        }
        if (m_state == State::AwaitAssets)
        {
            return SampleReadiness::Pending(
                "animation-character is waiting for ECS model and animation residency");
        }
        if (m_state == State::AwaitBridgeReconcile)
        {
            return SampleReadiness::Pending(
                "animation-character is waiting for generation-safe Animation/Physics binding");
        }
        if (m_state == State::QualifyingFixedSteps)
        {
            return SampleReadiness::Pending(
                "animation-character is qualifying 120 fixed ECS root-motion steps");
        }
        if (m_state == State::AwaitPresentedPalette)
        {
            return SampleReadiness::Pending(
                "animation-character is waiting for a completed skinning palette receipt");
        }
        if (m_state != State::Stable || !AreAllScenarioActionsPresented() ||
            !m_presentedPalette.IsValid() ||
            !diagnostics.rendered || diagnostics.visibleObjectCount == 0)
        {
            return SampleReadiness::Pending(
                "animation-character has not reached its stable ECS presentation");
        }
        return SampleReadiness::Ready();
    }

    bool AnimationCharacterSample::ShouldBeginFinalRenderDrain(
        const SampleRenderDiagnostics& diagnostics) const
    {
        return m_state == State::Stable && AreAllScenarioActionsPresented() &&
               diagnostics.rendered &&
               diagnostics.visibleObjectCount > 0 &&
               m_presentedPalette.IsValid();
    }

    bool AnimationCharacterSample::ValidateResult(
        const SampleRenderDiagnostics& diagnostics,
        std::string& outError) const
    {
        const SampleReadiness readiness = GetReadiness(diagnostics);
        outError = readiness.reason;
        return readiness.IsReady();
    }

    void AnimationCharacterSample::AppendReport(
        SampleFeatureReporter& reporter) const
    {
        const uint64 poseEvaluations =
            m_finalPoseSequence >= m_baselinePoseSequence ?
                m_finalPoseSequence - m_baselinePoseSequence :
                0;
        const uint64 rootMotionMillimetres =
            std::isfinite(m_rootMotionDistance) ?
                static_cast<uint64>(std::llround(m_rootMotionDistance * 1000.0f)) :
                0;
        const bool stable = m_state == State::Stable &&
                            AreAllScenarioActionsPresented() &&
                            m_presentedPalette.IsValid();
        const bool exactBinding = stable && m_rootMotionBound &&
                                  m_qualificationPhysicsBodyHandlePacked != 0 &&
                                  m_observedQualifiedRootMotionIntentCount ==
                                      QualificationFixedSteps;
        const bool fixedSchedule = stable &&
                                   m_completedFixedSteps == QualificationFixedSteps &&
                                   poseEvaluations == QualificationFixedSteps;
        const bool rootMotion = stable &&
                                m_completedRootMotionSteps == QualificationFixedSteps &&
                                m_lastQualifiedRootMotionSequence ==
                                    m_completedRootMotionSteps &&
                                m_observedQualifiedRootMotionIntentCount ==
                                    QualificationFixedSteps &&
                                rootMotionMillimetres == ExpectedRootMotionMillimetres;

        reporter.SetScenarioContractRevision(2);
        reporter.SetScenarioPhase(stable ? "stable" :
                                  m_state == State::Failed ? "failed" : "pending");
        for (const ScenarioActionEvidence& action : m_scenarioActions)
        {
            static_cast<void>(reporter.AppendScenarioAction({
                .name = action.name,
                .targetSceneRevision = action.targetSceneRevision,
                .completedPresentationSequence = action.completedPresentationSequence,
                .appliedSceneRevision = action.appliedSceneRevision,
                .passed = action.passed,
            }));
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
            "ExactEcsBinding", exactBinding,
            "bodyHandle=" + std::to_string(m_qualificationPhysicsBodyHandlePacked) +
                ", observedIntents=" +
                std::to_string(m_observedQualifiedRootMotionIntentCount));
        appendInvariant(
            "FixedSchedule", fixedSchedule,
            "fixedSteps=" + std::to_string(m_completedFixedSteps) +
                ", poseEvaluations=" + std::to_string(poseEvaluations));
        appendInvariant(
            "RootMotionEpoch", rootMotion,
            "bodyHandle=" + std::to_string(m_qualificationPhysicsBodyHandlePacked) +
                ", lastSequence=" +
                std::to_string(m_lastQualifiedRootMotionSequence) +
                ", consumed=" + std::to_string(m_completedRootMotionSteps));
        appendInvariant(
            "PresentedPalette", stable && m_presentedPalette.IsValid(),
            "paletteHash=" + std::to_string(m_presentedPalette.paletteHash) +
                ", presentationSequence=" +
                std::to_string(m_presentedPalette.presentationSequence));

        const auto appendMetric = [&reporter](std::string name, uint64 value)
        {
            static_cast<void>(reporter.AppendScenarioMetric({
                .name = std::move(name),
                .value = value,
            }));
        };
        appendMetric("fixedSteps", m_completedFixedSteps);
        appendMetric("poseEvaluations", poseEvaluations);
        appendMetric("rootMotionIntents", m_completedRootMotionSteps);
        appendMetric("rootMotionMillimetres", rootMotionMillimetres);
        appendMetric("paletteHash", m_presentedPalette.paletteHash);

        reporter.Enable("RenderPathPolicySelection");
        reporter.ResourceDiagnostic(
            "render path=" + std::string(GetSampleRenderPathName(m_renderPath)));
        switch (m_renderPath)
        {
            case SampleRenderPath::Direct:
                reporter.Enable("DirectPathRequested");
                break;
            case SampleRenderPath::GPUDriven:
                reporter.Enable("GPUDrivenPathRequested");
                break;
            case SampleRenderPath::Auto:
                reporter.Enable("AutomaticRenderPathRequested");
                break;
            default: break;
        }

        if (m_characterModel.status.modelMetadata.HasPublishedSource())
        {
            reporter.Enable("ModelResourceLoad");
            reporter.Enable("SceneInstantiation");
            reporter.Enable("EcsCharacterModel");
            reporter.Enable("EcsSkinningPalette");
            reporter.ResourceDiagnostic(
                "character model asset=" +
                std::to_string(
                    m_characterModel.status.modelMetadata.sourceModelAssetId.value));
            reporter.ResourceDiagnostic(
                "character members=" +
                std::to_string(m_characterModel.status.members.size()));
        }
        if (m_rootMotionAnimation.IsReady())
        {
            reporter.Enable("EcsAnimationAsset");
            reporter.Enable("EcsRootMotion");
            reporter.ResourceDiagnostic(
                "animation asset=" +
                std::to_string(m_rootMotionAnimation.status.animationAssetValue));
        }
        if (m_rootMotionBound)
        {
            reporter.Enable("EcsAnimationPhysicsBinding");
            if (m_completedFixedSteps == QualificationFixedSteps)
            {
                reporter.Enable("EcsFixedAnimation");
            }
            reporter.ResourceDiagnostic(
                "fixed steps=" + std::to_string(m_completedFixedSteps));
            reporter.ResourceDiagnostic(
                "root-motion intents=" +
                std::to_string(m_completedRootMotionSteps));
            reporter.ResourceDiagnostic(
                "root-motion distance=" + std::to_string(m_rootMotionDistance));
        }
        if (m_presentedPalette.IsValid())
        {
            reporter.Enable("EcsPresentedSkinningPalette");
            reporter.ResourceDiagnostic(
                "presented palette hash=" +
                std::to_string(m_presentedPalette.paletteHash));
            reporter.ResourceDiagnostic(
                "presented palette matrices=" +
                std::to_string(m_presentedPalette.paletteCount));
        }
        reporter.Unsupported("Jolt");
    }

    void AnimationCharacterSample::Shutdown(SampleContext& context)
    {
        if (m_motionDriver.IsValid())
        {
            const SceneECS::Animator* const current =
                context.scene.GetRegistry().TryGet<SceneECS::Animator>(m_motionDriver.entity);
            if (current != nullptr)
            {
                SceneECS::Animator stopped = *current;
                stopped.playing = false;
                static_cast<void>(context.scene.SetFragment(m_motionDriver.entity, stopped));
            }
        }
        if (m_rootMotionAnimation.request.IsValid() &&
            !context.animations.Cancel(m_rootMotionAnimation) && m_failure.empty())
        {
            m_failure = "Failed to cancel the Scene-qualified animation request.";
        }
        if (m_gpuProbeModel.request.IsValid() &&
            !context.models.Cancel(m_gpuProbeModel) && m_failure.empty())
        {
            m_failure = "Failed to cancel the ECS probe model request.";
        }
        if (m_characterModel.request.IsValid() &&
            !context.models.Cancel(m_characterModel) && m_failure.empty())
        {
            m_failure = "Failed to cancel the ECS character model request.";
        }
        m_characterRoot = {};
        m_modelPoseSource = {};
        m_motionDriver = {};
        m_orbitCamera.Reset();
    }

    void AnimationCharacterSample::Fail(SampleContext& context,
                                        AssessmentCode invariant,
                                        std::string message)
    {
        Fail(context.assessment, std::move(invariant), std::move(message));
    }

    void AnimationCharacterSample::Fail(SampleAssessmentChannel& assessment,
                                        AssessmentCode invariant,
                                        std::string message)
    {
        if (!m_failure.empty())
        {
            return;
        }
        m_failure = std::move(message);
        m_state = State::Failed;
        Finding finding;
        finding.code = AssessmentCode("ANIMATION.CHARACTER.FAILURE");
        finding.subsystemCode = AssessmentCode("ANIMATION");
        finding.invariantCode = std::move(invariant);
        finding.checkpoint = AssessmentCheckpoints::ActionApplied;
        finding.classification = FindingClass::ContractViolation;
        finding.severity = FindingSeverity::Error;
        finding.confidence = FindingConfidence::Confirmed;
        finding.summary = "animation-character ECS qualification failed";
        finding.detail = m_failure;
        finding.expected =
            "Pure-ECS Animation, Physics, hierarchy, and Render evidence remains exact.";
        finding.observed = m_failure;
        finding.gating = true;
        finding.blockingReason = m_failure;
        static_cast<void>(assessment.TryPublish(std::move(finding)));
    }
} // namespace RVX
