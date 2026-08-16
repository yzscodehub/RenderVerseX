/** @file SceneLifecycleSample.cpp @brief Deterministic pure-ECS lifecycle probe. */

#include "Scenes/SceneLifecycleSample.h"

#include "Core/MathTypes.h"
#include "RenderContracts/RenderFrameTypes.h"
#include "Samples/SampleCLI.h"
#include "Samples/SampleContext.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <utility>

namespace RVX
{
    namespace
    {
        const SampleInfo SceneLifecycleInfo{
            "scene-lifecycle",
            "Scene Lifecycle",
            "Deterministically validates pure-ECS commands, identity, hierarchy, cameras, fragment writes, and retirement",
            "water-bottle",
            SampleAssetPolicy::Fixed,
            "",
            SampleEnvironmentPolicy::None,
            true,
            SampleRenderPath::Auto};

        const AssessmentAction DeferredSpawnAction{
            AssessmentCode("SCENE.ACTION.DEFERRED_ENTITY_SPAWN"),
            "Deferred ECS entity creation was applied through one fixed command barrier."};
        const AssessmentAction KeepLocalAction{
            AssessmentCode("SCENE.ACTION.REPARENT_KEEP_LOCAL"),
            "KeepLocal reparent was applied by the authoritative ECS hierarchy."};
        const AssessmentAction KeepWorldAction{
            AssessmentCode("SCENE.ACTION.REPARENT_KEEP_WORLD"),
            "KeepWorld reparent preserved the child render-world transform."};
        const AssessmentAction CameraReuseAction{
            AssessmentCode("SCENE.ACTION.CAMERA_ENTITY_GENERATION_REUSE"),
            "A retired secondary-camera entity was replaced only after its stale generation stopped resolving."};
        const AssessmentAction CameraSwitchAction{
            AssessmentCode("SCENE.ACTION.CAMERA_SWITCH_AND_CUT"),
            "ECS camera selection switched away and back through scene-qualified refs."};
        const AssessmentAction LightMutationAction{
            AssessmentCode("SCENE.ACTION.LIGHT_FRAGMENT_MUTATION"),
            "A value Light fragment write advanced its exact ECS write version."};
        const AssessmentAction MaterialMutationAction{
            AssessmentCode("SCENE.ACTION.MATERIAL_SLOT_MUTATION"),
            "A value MaterialSlots fragment write advanced its exact ECS write version."};
        const AssessmentAction DestroyReuseAction{
            AssessmentCode("SCENE.ACTION.DESTROY_ENTITY_SLOT_REUSE"),
            "Cleanup-gated entity retirement recycled a stale generation before replacement."};

        const AssessmentInvariant HierarchyAuthorityInvariant{
            AssessmentCode("SCENE.HIERARCHY.AUTHORITY_UNIFIED"),
            "ParentRelation and resolved hierarchy state agree after KeepWorld."};
        const AssessmentInvariant StaleEntityInvariant{
            AssessmentCode("SCENE.HANDLE.STALE_REJECTED"),
            "Retired entity generations no longer resolve after slot reuse."};
        const AssessmentInvariant CameraCutInvariant{
            AssessmentCode("ENGINE.CAMERA.CUT_ADVANCED"),
            "Switching back to the primary ECS camera advanced its cut revision."};
        const AssessmentInvariant LightRevisionInvariant{
            AssessmentCode("SCENE.LIGHT.WRITE_VERSION_ADVANCED"),
            "Light fragment mutation advanced the value write version and render light state."};
        const AssessmentInvariant MaterialRevisionInvariant{
            AssessmentCode("SCENE.MATERIAL.SLOT_WRITE_VERSION_ADVANCED"),
            "MaterialSlots mutation advanced the exact ECS write version without retaining a resource object."};
        const AssessmentInvariant CleanupInvariant{
            AssessmentCode("SCENE.CLEANUP.RETIREMENT_RECYCLED"),
            "Pending-destroy, cleanup, retirement, and recyclable queues drained before readiness."};

        [[nodiscard]] bool MatricesNear(const Mat4& lhs, const Mat4& rhs)
        {
            constexpr float32 Epsilon = 0.0001f;
            for (uint32 column = 0; column < 4; ++column)
            {
                for (uint32 row = 0; row < 4; ++row)
                {
                    if (std::abs(lhs[column][row] - rhs[column][row]) > Epsilon)
                    {
                        return false;
                    }
                }
            }
            return true;
        }

        [[nodiscard]] bool TransformsNear(const SceneECS::LocalTransform& lhs,
                                          const SceneECS::LocalTransform& rhs)
        {
            constexpr float32 Epsilon = 0.0001f;
            const auto near = [](float32 a, float32 b)
            {
                return std::abs(a - b) <= Epsilon;
            };
            return near(lhs.translation.x, rhs.translation.x) &&
                   near(lhs.translation.y, rhs.translation.y) &&
                   near(lhs.translation.z, rhs.translation.z) &&
                   near(lhs.rotation.w, rhs.rotation.w) &&
                   near(lhs.rotation.x, rhs.rotation.x) &&
                   near(lhs.rotation.y, rhs.rotation.y) &&
                   near(lhs.rotation.z, rhs.rotation.z) &&
                   near(lhs.scale.x, rhs.scale.x) &&
                   near(lhs.scale.y, rhs.scale.y) &&
                   near(lhs.scale.z, rhs.scale.z);
        }

        [[nodiscard]] SceneECS::Light MakeProbeLight()
        {
            return {
                .type = SceneECS::LightType::Point,
                .color = Vec3(1.0f, 0.82f, 0.64f),
                .intensity = 2.0f,
                .range = 6.0f,
                .castsShadows = false,
            };
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
            return "Scene-lifecycle ECS model request reached a terminal state.";
        }

        [[nodiscard]] bool IsLive(const SceneECS::SceneEcsRuntime& scene,
                                  SceneECS::SceneEntityRef entity)
        {
            return entity.IsValid() &&
                   entity.sceneRuntimeId == scene.GetSceneRuntimeId() &&
                   scene.GetEntityRef(entity.entity) == entity;
        }
    } // namespace

    const SampleInfo& SceneLifecycleSample::GetInfo() const noexcept
    {
        return SceneLifecycleInfo;
    }

    SampleAssessmentContract SceneLifecycleSample::GetAssessmentContract() const
    {
        SampleAssessmentContract contract;
        contract.code = AssessmentCode("SAMPLE.SCENE_LIFECYCLE");
        contract.revision = "3";
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
        contract.actions = {
            DeferredSpawnAction,
            KeepLocalAction,
            KeepWorldAction,
            CameraReuseAction,
            CameraSwitchAction,
            LightMutationAction,
            MaterialMutationAction,
            DestroyReuseAction};
        contract.invariants = {
            HierarchyAuthorityInvariant,
            StaleEntityInvariant,
            CameraCutInvariant,
            LightRevisionInvariant,
            MaterialRevisionInvariant,
            CleanupInvariant};
        return contract;
    }

    bool SceneLifecycleSample::Setup(SampleContext& context,
                                     std::string& outError)
    {
        m_actionEvidence = {{
            {"Spawn"},
            {"ReparentKeepLocal"},
            {"ReparentKeepWorld"},
            {"RecycleSecondaryCamera"},
            {"PresentSecondaryCamera"},
            {"RestorePrimaryCamera"},
            {"MutateLight"},
            {"MutateMaterialSlots"},
            {"DestroyAndReuseEntitySlot"}}};
        m_failure.clear();
        m_terminalResourceDiagnosticsReason.clear();
        m_terminalSceneDiagnostics = {};
        m_terminalResourceDiagnostics = {};
        m_presentationBounds.Reset();
        m_cameraFrame = {};
        m_orbitCamera.Reset();
        m_renderPath = context.options.renderPath;

        switch (m_renderPath)
        {
            case SampleRenderPath::Auto:
                context.renderSettings.gpuCulling.mode = RenderGPUDrivenMode::Auto;
                break;
            case SampleRenderPath::Direct:
                context.renderSettings.gpuCulling.mode = RenderGPUDrivenMode::ForceDisabled;
                break;
            case SampleRenderPath::GPUDriven:
                context.renderSettings.gpuCulling.mode = RenderGPUDrivenMode::ForceEnabled;
                break;
            default:
                outError = "Scene-lifecycle received an invalid render path";
                return false;
        }
        context.renderSettings.shadows.enabled = false;

        const float32 aspect =
            static_cast<float32>(context.options.width) /
            static_cast<float32>(std::max(context.options.height, 1u));
        if (!context.cameras.SetPerspective(
                context.camera, radians(45.0f), aspect, 0.05f, 250.0f) ||
            !context.cameras.SetPose(
                context.camera, {.position = Vec3(0.0f, 1.0f, 4.5f)}) ||
            !context.cameras.LookAt(context.camera, Vec3(0.0f, 0.45f, 0.0f)))
        {
            outError = "Scene-lifecycle failed to configure the primary ECS camera";
            return false;
        }
        const std::optional<SceneECS::Camera> primary =
            context.cameras.GetCamera(context.camera);
        if (!primary.has_value())
        {
            outError = "Scene-lifecycle primary ECS camera is not live";
            return false;
        }
        m_primaryCutBefore = primary->cutRevision;
        m_primaryCameraIdentity = context.camera.entity.GetPackedValue();

        if (!context.models.RequestByAssetId(
                context.options.assetId, m_model, outError))
        {
            return false;
        }

        SceneECS::RuntimeEntityDesc parentADesc;
        parentADesc.localTransform.translation = Vec3(-1.2f, 0.0f, 0.0f);
        SceneECS::RuntimeEntityDesc parentBDesc;
        parentBDesc.localTransform.translation = Vec3(1.2f, 0.0f, 0.0f);
        SceneECS::RuntimeEntityDesc childDesc;
        childDesc.localTransform.translation = Vec3(0.0f, 0.3f, 0.0f);
        SceneECS::RuntimeEntityDesc lightDesc;
        lightDesc.localTransform.translation = Vec3(0.0f, 1.5f, 1.5f);

        SceneECS::SceneCommandBuffer commands = context.scene.CreateCommandBuffer();
        m_spawnParentA = commands.Create(parentADesc);
        m_spawnParentB = commands.Create(parentBDesc);
        m_spawnChild = commands.Create(childDesc);
        m_spawnLight = commands.Create(lightDesc);
        m_spawnLightFragment = commands.Add(m_spawnLight, MakeProbeLight());
        m_spawnLightVisibility = commands.Add(
            m_spawnLight, SceneECS::Visibility{});
        m_spawnBuffer = context.scene.SubmitCommandBuffer(
            std::move(commands), SceneECS::SceneCommandBarrier::BeginSimulation);
        if (m_spawnBuffer.IsRejected() || m_spawnBuffer.IsDiscarded() ||
            m_spawnParentA.IsRejected() || m_spawnParentB.IsRejected() ||
            m_spawnChild.IsRejected() || m_spawnLight.IsRejected() ||
            m_spawnLightFragment.IsRejected() ||
            m_spawnLightVisibility.IsRejected())
        {
            outError = "Scene-lifecycle could not submit deferred ECS entity creation";
            return false;
        }
        m_spawnSubmitted = true;
        static_cast<void>(context.assessment.MarkCheckpoint(
            AssessmentCheckpoints::ScenarioSetup));
        static_cast<void>(context.assessment.MarkCheckpoint(
            AssessmentCheckpoints::ActionRequested));
        return true;
    }

    void SceneLifecycleSample::Update(SampleContext& context, float deltaTime)
    {
        static_cast<void>(deltaTime);
        context.sceneLifetime.Collect();
        const ResourceSceneAdapters::EcsSceneAssetLoadStatus status =
            context.models.UpdateReadiness(m_model);
        if (status.IsTerminal())
        {
            Fail(context,
                 AssessmentCode("RESOURCE.MODEL.LIFECYCLE_VALID"),
                 DescribeModelFailure(status));
            return;
        }
        if (!m_presentationReady && m_model.IsCPUReady() &&
            !ConfigureModelPresentation(context))
        {
            return;
        }
        if (m_state == ScenarioState::Failed || m_state == ScenarioState::Stable)
        {
            return;
        }
        if (m_state == ScenarioState::WaitingForStablePresentation)
        {
            CaptureTerminalDiagnostics(context);
        }

        switch (m_state)
        {
            case ScenarioState::AwaitSpawn:
                static_cast<void>(AdvanceInitialSpawns(context));
                break;
            case ScenarioState::ReparentKeepLocal:
                static_cast<void>(AdvanceKeepLocal(context));
                break;
            case ScenarioState::ReparentKeepWorld:
                static_cast<void>(AdvanceKeepWorld(context));
                break;
            case ScenarioState::RecycleSecondaryCamera:
                static_cast<void>(AdvanceSecondaryCameraRecycle(context));
                break;
            case ScenarioState::PresentSecondaryCamera:
                static_cast<void>(PresentSecondaryCamera(context));
                break;
            case ScenarioState::RestorePrimaryCamera:
                static_cast<void>(RestorePrimaryCamera(context));
                break;
            case ScenarioState::MutateLight:
                static_cast<void>(MutateLight(context));
                break;
            case ScenarioState::MutateMaterialSlots:
                static_cast<void>(MutateMaterialSlots(context));
                break;
            case ScenarioState::DestroyAndReuseEntitySlot:
                static_cast<void>(AdvanceDestroyAndReuse(context));
                break;
            case ScenarioState::WaitingForSecondaryPresentation:
            case ScenarioState::WaitingForPrimaryPresentation:
            case ScenarioState::WaitingForStablePresentation:
            case ScenarioState::Stable:
            case ScenarioState::Failed:
            default:
                break;
        }
    }

    void SceneLifecycleSample::OnInput(SampleContext& context)
    {
        if (!context.options.smoke && m_presentationReady && context.input != nullptr)
        {
            static_cast<void>(
                m_orbitCamera.Update(*context.input, context.cameras, context.camera));
        }
    }

    void SceneLifecycleSample::OnViewportResize(SampleContext& context,
                                                 uint32 width,
                                                 uint32 height)
    {
        if (width == 0 || height == 0)
        {
            return;
        }
        const float32 aspect =
            static_cast<float32>(width) / static_cast<float32>(height);
        if (m_orbitCamera.IsInitialized())
        {
            static_cast<void>(m_orbitCamera.SetAspectRatio(
                aspect, context.cameras, context.camera));
        }
        else
        {
            static_cast<void>(context.cameras.SetAspectRatio(context.camera, aspect));
        }
        if (m_secondaryCamera.IsValid())
        {
            static_cast<void>(context.cameras.SetAspectRatio(m_secondaryCamera, aspect));
        }
    }

    void SceneLifecycleSample::AppendReport(SampleFeatureReporter& reporter) const
    {
        reporter.SetScenarioContractRevision(3);
        reporter.SetScenarioPhase(
            m_state == ScenarioState::Stable ? "stable" : "incomplete");
        reporter.Enable("SceneLifecycleProbe");
        reporter.Enable("DeferredEcsCommands");
        reporter.Enable("GenerationSafeEntityIdentity");
        reporter.Enable("TransformHierarchyAuthorityProbe");
        reporter.Enable("EcsCameraLifecycleProbe");
        reporter.Enable("RuntimeLightFragmentMutationProbe");
        reporter.Enable("RuntimeMaterialSlotMutationProbe");
        reporter.ResourceDiagnostic(
            "render path=" + std::string(GetSampleRenderPathName(m_renderPath)));
        reporter.ResourceDiagnostic(
            "scene runtime=" + std::to_string(m_terminalSceneDiagnostics.sceneRuntimeId.GetValue()));
        reporter.ResourceDiagnostic(
            "scene-lifecycle state=" + std::to_string(static_cast<uint32>(m_state)));
        if (m_model.status.modelMetadata.HasPublishedSource())
        {
            reporter.Enable("ModelResourceLoad");
            reporter.Enable("SceneInstantiation");
            reporter.ResourceDiagnostic("model path=" + m_model.sourcePath.string());
            reporter.ResourceDiagnostic(
                "model nodes=" +
                std::to_string(m_model.status.modelMetadata.sourceNodeCount));
            reporter.ResourceDiagnostic(
                "model entity mappings=" +
                std::to_string(m_model.status.entityMappings.size()));
            reporter.ResourceDiagnostic(
                "model material assets=" +
                std::to_string(m_model.status.modelMetadata.materialAssetIds.size()));
        }

        for (const ScenarioActionEvidence& action : m_actionEvidence)
        {
            if (action.passed)
            {
                static_cast<void>(reporter.AppendScenarioAction({
                    action.name,
                    action.targetSceneRevision,
                    action.completedPresentationSequence,
                    action.appliedSceneRevision,
                    true}));
            }
        }
        const auto appendInvariant = [&reporter](const char* name,
                                                  bool passed,
                                                  std::string evidence)
        {
            static_cast<void>(reporter.AppendScenarioInvariant(
                {name, passed, std::move(evidence)}));
        };
        appendInvariant("EntitySlotReuse",
                        GetAction(ScenarioAction::DestroyAndReuseEntitySlot).passed,
                        "The stale child entity ref did not resolve before replacement.");
        appendInvariant("CameraEntityReuse",
                        GetAction(ScenarioAction::RecycleSecondaryCamera).passed,
                        "The replacement camera used the retired slot with a new generation.");
        appendInvariant("KeepLocal",
                        GetAction(ScenarioAction::ReparentKeepLocal).passed,
                        "Child LocalTransform remained unchanged during KeepLocal.");
        appendInvariant("KeepWorld",
                        GetAction(ScenarioAction::ReparentKeepWorld).passed,
                        "Child RenderWorldTransform remained unchanged during KeepWorld.");
        appendInvariant("CameraTemporalReset",
                        GetAction(ScenarioAction::RestorePrimaryCamera).passed,
                        "Both ECS camera identities reached completed presentations.");
        appendInvariant("LightWriteVersion",
                        GetAction(ScenarioAction::MutateLight).passed,
                        "Light write version and render light hash advanced.");
        appendInvariant("MaterialSlotWriteVersion",
                        GetAction(ScenarioAction::MutateMaterialSlots).passed,
                        "MaterialSlots write version advanced without a resource object.");
        appendInvariant("RetirementQueuesDrained",
                        m_terminalQueuesDrained,
                        "Scene, Resource, and Render queues drained after a completed presentation.");

        static_cast<void>(reporter.AppendScenarioMetric(
            {"primary-camera-presentation",
             GetAction(ScenarioAction::RestorePrimaryCamera)
                 .completedPresentationSequence}));
        static_cast<void>(reporter.AppendScenarioMetric(
            {"secondary-camera-presentation",
             GetAction(ScenarioAction::PresentSecondaryCamera)
                 .completedPresentationSequence}));
        static_cast<void>(reporter.AppendScenarioMetric(
            {"final-owned-entity-count", m_finalOwnedEntityCount}));
        static_cast<void>(reporter.AppendScenarioMetric(
            {"expected-owned-entity-count", m_expectedFinalOwnedEntityCount}));
        static_cast<void>(reporter.AppendScenarioMetric(
            {"light-write-version", m_lightWriteVersionAfterMutation}));
        static_cast<void>(reporter.AppendScenarioMetric(
            {"material-slot-write-version", m_materialWriteVersionAfterMutation}));
        static_cast<void>(reporter.AppendScenarioMetric(
            {"terminal-pending-destroy", m_terminalSceneDiagnostics.pendingDestroyCount}));
        static_cast<void>(reporter.AppendScenarioMetric(
            {"terminal-cleanup-required", m_terminalSceneDiagnostics.cleanupRequiredCount}));
        static_cast<void>(reporter.AppendScenarioMetric(
            {"terminal-retiring", m_terminalSceneDiagnostics.retiringCount}));
        static_cast<void>(reporter.AppendScenarioMetric(
            {"terminal-recyclable", m_terminalSceneDiagnostics.recyclableCount}));
        static_cast<void>(reporter.AppendScenarioMetric(
            {"terminal-render-pending-upload", m_lastObservedRenderPendingUploadCount}));
        static_cast<void>(reporter.AppendScenarioMetric(
            {"terminal-render-retirement", m_lastObservedRenderRetirementEntryCount}));
        if (!m_terminalResourceDiagnosticsAvailable &&
            !m_terminalResourceDiagnosticsReason.empty())
        {
            reporter.ResourceDiagnostic(
                "terminal resource diagnostics unavailable: " +
                m_terminalResourceDiagnosticsReason);
        }
    }

    void SceneLifecycleSample::ObserveDiagnostics(
        const SampleRenderDiagnostics& diagnostics,
        SampleAssessmentChannel& assessment)
    {
        if (diagnostics.lastPresentedFrameSequence.IsAvailable())
        {
            m_lastObservedPresentationSequence =
                *diagnostics.lastPresentedFrameSequence.GetValue();
        }
        if (diagnostics.renderSceneValuesAvailable)
        {
            m_lastObservedRenderSceneRevision = diagnostics.renderSceneAppliedRevision;
        }
        m_lastObservedRenderPendingUploadCount = diagnostics.renderPendingUploadCount;
        m_lastObservedRenderRetirementEntryCount =
            diagnostics.renderRetirementEntryCount;
        m_lastObservedRenderLightStateHash = diagnostics.renderSceneLightStateHash;

        if (m_state == ScenarioState::WaitingForSecondaryPresentation)
        {
            if (!diagnostics.lastPresentedFrameSequence.IsAvailable() ||
                m_lastObservedPresentationSequence <=
                    m_secondaryCameraPresentationBaseline)
            {
                return;
            }
            if (diagnostics.activeCameraIdentity !=
                m_secondaryCamera.entity.GetPackedValue())
            {
                Fail(assessment,
                     CameraCutInvariant.code,
                     "The completed secondary-camera presentation reported a different ECS identity");
                return;
            }
            CompleteAction(ScenarioAction::PresentSecondaryCamera,
                           diagnostics,
                           assessment);
            if (!GetAction(ScenarioAction::PresentSecondaryCamera).passed)
            {
                return;
            }
            m_state = ScenarioState::RestorePrimaryCamera;
            return;
        }

        if (m_state == ScenarioState::WaitingForPrimaryPresentation)
        {
            if (!diagnostics.lastPresentedFrameSequence.IsAvailable() ||
                m_lastObservedPresentationSequence <=
                    m_primaryCameraPresentationBaseline)
            {
                return;
            }
            if (diagnostics.activeCameraIdentity != m_primaryCameraIdentity ||
                diagnostics.activeCameraCutRevision <= m_primaryCutBefore)
            {
                Fail(assessment,
                     CameraCutInvariant.code,
                     "The completed primary-camera restore did not retain the advanced ECS cut revision");
                return;
            }
            CompleteAction(ScenarioAction::RestorePrimaryCamera,
                           diagnostics,
                           assessment);
            if (!GetAction(ScenarioAction::RestorePrimaryCamera).passed)
            {
                return;
            }
            m_state = ScenarioState::MutateLight;
            return;
        }

        for (uint32 index = 0;
             index < static_cast<uint32>(ScenarioAction::Count);
             ++index)
        {
            const ScenarioAction action = static_cast<ScenarioAction>(index);
            if (action == ScenarioAction::PresentSecondaryCamera ||
                action == ScenarioAction::RestorePrimaryCamera)
            {
                continue;
            }
            ScenarioActionEvidence& evidence = GetAction(action);
            if (evidence.passed || !IsActionPresentationCovered(diagnostics, evidence))
            {
                continue;
            }
            if ((action == ScenarioAction::Spawn &&
                 (!m_spawnParentA.IsApplied() || !m_spawnParentB.IsApplied() ||
                  !m_spawnChild.IsApplied() || !m_spawnLight.IsApplied() ||
                  !m_spawnLightFragment.IsApplied() ||
                  !m_spawnLightVisibility.IsApplied())) ||
                (action == ScenarioAction::ReparentKeepLocal &&
                 !m_keepLocal.IsApplied()) ||
                (action == ScenarioAction::ReparentKeepWorld &&
                 !m_keepWorld.IsApplied()) ||
                (action == ScenarioAction::RecycleSecondaryCamera &&
                 !m_secondaryCameraReplacementCreated) ||
                (action == ScenarioAction::MutateLight &&
                 !m_lightMutation.IsApplied()) ||
                (action == ScenarioAction::MutateMaterialSlots &&
                 !m_materialMutation.IsApplied()) ||
                (action == ScenarioAction::DestroyAndReuseEntitySlot &&
                 !m_replacement.IsValid()))
            {
                continue;
            }
            if (action == ScenarioAction::MutateLight &&
                (!m_renderLightBaselineCaptured ||
                 m_lightWriteVersionAfterMutation <= m_lightWriteVersionBeforeMutation ||
                 diagnostics.renderSceneValueLightCount != 1 ||
                 diagnostics.renderSceneLightStateHash == m_renderLightStateHashBefore))
            {
                continue;
            }
            if (action == ScenarioAction::MutateMaterialSlots &&
                m_materialWriteVersionAfterMutation <= m_materialWriteVersionBeforeMutation)
            {
                continue;
            }
            CompleteAction(action, diagnostics, assessment);
        }

        if (m_state == ScenarioState::WaitingForStablePresentation &&
            AreAllActionsPresented() &&
            (m_terminalQueuesDrained = AreTerminalQueuesDrained(diagnostics)))
        {
            MarkInvariant(assessment, CleanupInvariant.code, CleanupInvariant.description);
            static_cast<void>(assessment.MarkCheckpoint(
                AssessmentCheckpoints::ScenarioStable));
            m_state = ScenarioState::Stable;
        }
    }

    SampleReadiness SceneLifecycleSample::GetReadiness(
        const SampleRenderDiagnostics& diagnostics) const
    {
        if (!m_failure.empty())
        {
            return SampleReadiness::Failed(m_failure);
        }
        if (m_state != ScenarioState::Stable)
        {
            return SampleReadiness::Pending(
                "Scene-lifecycle sample is waiting for its deterministic ECS command sequence");
        }
        if (!m_model.IsFullyResident())
        {
            return SampleReadiness::Pending(
                "Scene-lifecycle sample is waiting for its presentation model to become fully resident");
        }
        if (diagnostics.visibleObjectCount == 0)
        {
            return SampleReadiness::Pending(
                "Scene-lifecycle sample is waiting for visible presentation geometry");
        }
        if (!diagnostics.renderSceneValuesAvailable ||
            diagnostics.renderSceneAppliedRevision != diagnostics.renderSceneRequiredRevision ||
            !diagnostics.engineRenderRuntimeAvailable ||
            diagnostics.engineRequiredSceneRevision > diagnostics.renderSceneAppliedRevision)
        {
            return SampleReadiness::Pending(
                "Scene-lifecycle sample is waiting for ECS snapshot-to-render revision convergence");
        }
        if (diagnostics.activeCameraIdentity != m_primaryCameraIdentity ||
            diagnostics.activeCameraCutRevision <= m_primaryCutBefore)
        {
            return SampleReadiness::Pending(
                "Scene-lifecycle sample is waiting for the primary ECS camera cut");
        }
        if (!AreAllActionsPresented() || !m_childPendingDestroyObserved ||
            !m_terminalQueuesDrained || !AreTerminalQueuesDrained(diagnostics))
        {
            return SampleReadiness::Pending(
                "Scene-lifecycle sample is waiting for command presentation and retirement drain proof");
        }
        if (m_finalOwnedEntityCount != m_expectedFinalOwnedEntityCount ||
            m_expectedFinalOwnedEntityCount == 0)
        {
            return SampleReadiness::Pending(
                "Scene-lifecycle sample is waiting for its exact final ECS population");
        }
        if (!IsRenderPathQualified(diagnostics))
        {
            return SampleReadiness::Pending(
                "Scene-lifecycle sample is waiting for the requested render-path execution proof");
        }
        return SampleReadiness::Ready();
    }

    bool SceneLifecycleSample::ShouldBeginFinalRenderDrain(
        const SampleRenderDiagnostics& diagnostics) const
    {
        return m_failure.empty() &&
               (m_state == ScenarioState::WaitingForStablePresentation ||
                m_state == ScenarioState::Stable) &&
               AreAllActionsPresented() &&
               AreTerminalQueuesDrained(diagnostics);
    }

    bool SceneLifecycleSample::ValidateResult(
        const SampleRenderDiagnostics& diagnostics,
        std::string& outError) const
    {
        const SampleReadiness readiness = GetReadiness(diagnostics);
        outError = readiness.reason;
        return readiness.IsReady();
    }

    void SceneLifecycleSample::Shutdown(SampleContext& context)
    {
        if (m_model.request.IsValid() && !context.models.Cancel(m_model))
        {
            m_failure = "Scene-lifecycle failed to cancel its live ECS model request.";
            return;
        }

        m_model = {};
        m_parentA = {};
        m_parentB = {};
        m_child = {};
        m_light = {};
        m_replacement = {};
        m_materialEntity = {};
        m_secondaryCamera = {};
        m_retiredSecondaryCamera = {};
        m_spawnParentA = {};
        m_spawnParentB = {};
        m_spawnChild = {};
        m_spawnLight = {};
        m_spawnLightFragment = {};
        m_spawnLightVisibility = {};
        m_spawnBuffer = {};
        m_keepLocal = {};
        m_keepLocalBuffer = {};
        m_keepWorld = {};
        m_keepWorldBuffer = {};
        m_lightMutation = {};
        m_lightMutationBuffer = {};
        m_materialMutation = {};
        m_materialMutationBuffer = {};
        m_materialSlotsBefore = {};
        m_materialSlotsAfter = {};
        m_renderPath = SampleRenderPath::Auto;
        m_state = ScenarioState::AwaitSpawn;
        m_presentationBounds.Reset();
        m_cameraFrame = {};
        m_orbitCamera.Reset();
        m_actionEvidence = {};
        m_failure.clear();
        m_terminalResourceDiagnosticsReason.clear();
        m_terminalSceneDiagnostics = {};
        m_terminalResourceDiagnostics = {};
        m_spawnSubmitted = false;
        m_reparentKeepLocalQueued = false;
        m_reparentKeepWorldQueued = false;
        m_secondaryCameraDestroyRequested = false;
        m_secondaryCameraReplacementCreated = false;
        m_lightMutationQueued = false;
        m_materialMutationQueued = false;
        m_childDestroyRequested = false;
        m_renderLightBaselineCaptured = false;
        m_childPendingDestroyObserved = false;
        m_terminalSceneDiagnosticsCaptured = false;
        m_terminalResourceDiagnosticsAvailable = false;
        m_terminalQueuesDrained = false;
        m_presentationReady = false;
        m_assessmentActionAppliedMarked = false;
    }

    bool SceneLifecycleSample::AdvanceInitialSpawns(SampleContext& context)
    {
        if (!m_spawnSubmitted || m_spawnBuffer.IsQueued())
        {
            return false;
        }
        std::string error;
        if (!ValidateAppliedReceipt(m_spawnLightFragment, m_spawnBuffer, error) ||
            !ValidateAppliedReceipt(m_spawnLightVisibility, m_spawnBuffer, error) ||
            !m_spawnParentA.IsResolved() || !m_spawnParentB.IsResolved() ||
            !m_spawnChild.IsResolved() || !m_spawnLight.IsResolved())
        {
            if (!error.empty())
            {
                Fail(context, DeferredSpawnAction.code, std::move(error));
            }
            return false;
        }

        const std::array<SceneECS::SceneEntityRef, 4> spawned{
            context.scene.GetEntityRef(m_spawnParentA.GetEntity()),
            context.scene.GetEntityRef(m_spawnParentB.GetEntity()),
            context.scene.GetEntityRef(m_spawnChild.GetEntity()),
            context.scene.GetEntityRef(m_spawnLight.GetEntity())};
        if (std::any_of(spawned.begin(), spawned.end(),
                        [](SceneECS::SceneEntityRef entity) { return !entity.IsValid(); }) ||
            !context.sceneLifetime.AdoptBatch(spawned))
        {
            Fail(context,
                 DeferredSpawnAction.code,
                 "Deferred ECS entities could not be adopted by the sample lifetime scope");
            return false;
        }
        m_parentA = spawned[0];
        m_parentB = spawned[1];
        m_child = spawned[2];
        m_light = spawned[3];
        SetActionTarget(ScenarioAction::Spawn);
        m_state = ScenarioState::ReparentKeepLocal;
        return true;
    }

    bool SceneLifecycleSample::AdvanceKeepLocal(SampleContext& context)
    {
        if (!m_reparentKeepLocalQueued)
        {
            const SceneECS::LocalTransform* transform =
                context.scene.GetRegistry().TryGet<SceneECS::LocalTransform>(m_child.entity);
            if (transform == nullptr)
            {
                Fail(context, KeepLocalAction.code, "ECS child is missing LocalTransform");
                return false;
            }
            m_childLocalBeforeKeepLocal = *transform;
            if (!QueueReparent(context,
                               SceneECS::ReparentMode::KeepLocal,
                               m_keepLocal,
                               m_keepLocalBuffer))
            {
                return false;
            }
            m_reparentKeepLocalQueued = true;
            return true;
        }
        if (m_keepLocalBuffer.IsQueued())
        {
            return false;
        }
        std::string error;
        if (!ValidateAppliedReceipt(m_keepLocal, m_keepLocalBuffer, error))
        {
            Fail(context, KeepLocalAction.code, std::move(error));
            return false;
        }
        const SceneECS::LocalTransform* transform =
            context.scene.GetRegistry().TryGet<SceneECS::LocalTransform>(m_child.entity);
        if (transform == nullptr || !TransformsNear(*transform, m_childLocalBeforeKeepLocal) ||
            !ValidateHierarchyAuthority(context, m_parentA))
        {
            Fail(context,
                 KeepLocalAction.code,
                 "KeepLocal did not retain child local data under the authoritative ECS parent edge");
            return false;
        }
        SetActionTarget(ScenarioAction::ReparentKeepLocal);
        m_state = ScenarioState::ReparentKeepWorld;
        return true;
    }

    bool SceneLifecycleSample::AdvanceKeepWorld(SampleContext& context)
    {
        if (!m_reparentKeepWorldQueued)
        {
            const SceneECS::RenderWorldTransform* world =
                context.scene.GetRegistry().TryGet<SceneECS::RenderWorldTransform>(m_child.entity);
            if (world == nullptr || world->sourceRevision == 0)
            {
                return false;
            }
            m_childWorldBeforeKeepWorld = world->matrix;
            if (!QueueReparent(context,
                               SceneECS::ReparentMode::KeepWorld,
                               m_keepWorld,
                               m_keepWorldBuffer))
            {
                return false;
            }
            m_reparentKeepWorldQueued = true;
            return true;
        }
        if (m_keepWorldBuffer.IsQueued())
        {
            return false;
        }
        std::string error;
        if (!ValidateAppliedReceipt(m_keepWorld, m_keepWorldBuffer, error))
        {
            Fail(context, KeepWorldAction.code, std::move(error));
            return false;
        }
        const SceneECS::RenderWorldTransform* world =
            context.scene.GetRegistry().TryGet<SceneECS::RenderWorldTransform>(m_child.entity);
        if (world == nullptr || !MatricesNear(world->matrix, m_childWorldBeforeKeepWorld) ||
            !ValidateHierarchyAuthority(context, m_parentB))
        {
            Fail(context,
                 KeepWorldAction.code,
                 "KeepWorld did not preserve the child render-world transform under the ECS parent edge");
            return false;
        }
        SetActionTarget(ScenarioAction::ReparentKeepWorld);
        m_state = ScenarioState::RecycleSecondaryCamera;
        return true;
    }

    bool SceneLifecycleSample::AdvanceSecondaryCameraRecycle(SampleContext& context)
    {
        // Keep the allocator probe isolated from asynchronous model adoption.
        // The model coordinator may publish ECS entities after this frame's
        // Scene tick; waiting for the already-configured CPU-ready model makes
        // the next allocation test the retired camera slot rather than race an
        // unrelated asset transaction for the same free-list entry.
        if (!m_presentationReady)
        {
            return false;
        }

        if (!m_secondaryCameraDestroyRequested)
        {
            WorldECS::WorldEcsCameraCreateDesc create;
            create.entity.localTransform.translation = Vec3(0.0f, 1.0f, 5.0f);
            create.camera.enabled = false;
            const WorldECS::WorldEcsCameraRef candidate =
                context.cameras.CreateCamera(create);
            const SceneECS::SceneEntityRef entity{
                .sceneRuntimeId = candidate.sceneRuntimeId,
                .entity = candidate.entity};
            if (!candidate.IsValid() || !context.sceneLifetime.Adopt(entity) ||
                !ConfigureSecondaryCamera(context, candidate))
            {
                Fail(context,
                     CameraReuseAction.code,
                     "Could not create the initial secondary ECS camera");
                return false;
            }
            m_retiredSecondaryCamera = candidate;
            if (context.cameras.RequestDestroy(candidate,
                                               SceneECS::ToCleanupDomainMask(
                                                   SceneECS::CleanupDomain::None)) !=
                SceneECS::DestroyRequestResult::Accepted)
            {
                Fail(context,
                     CameraReuseAction.code,
                     "Could not request cleanup-gated retirement for the secondary ECS camera");
                return false;
            }
            m_secondaryCameraDestroyRequested = true;
            return true;
        }

        if (!m_secondaryCameraReplacementCreated)
        {
            if (context.scene.GetEntityRef(m_retiredSecondaryCamera.entity).IsValid())
            {
                return false;
            }
            WorldECS::WorldEcsCameraCreateDesc create;
            create.entity.localTransform.translation = Vec3(0.0f, 1.0f, 5.0f);
            create.camera.enabled = false;
            const WorldECS::WorldEcsCameraRef replacement =
                context.cameras.CreateCamera(create);
            const SceneECS::SceneEntityRef entity{
                .sceneRuntimeId = replacement.sceneRuntimeId,
                .entity = replacement.entity};
            if (!replacement.IsValid() || !context.sceneLifetime.Adopt(entity) ||
                !ConfigureSecondaryCamera(context, replacement) ||
                replacement.entity.GetIndex() != m_retiredSecondaryCamera.entity.GetIndex() ||
                replacement.entity.GetGeneration() ==
                    m_retiredSecondaryCamera.entity.GetGeneration())
            {
                Fail(context,
                     CameraReuseAction.code,
                     "Secondary ECS camera replacement did not reuse the retired slot with a new generation");
                return false;
            }
            m_secondaryCamera = replacement;
            m_secondaryCameraReplacementCreated = true;
            SetActionTarget(ScenarioAction::RecycleSecondaryCamera);
            m_state = ScenarioState::PresentSecondaryCamera;
            return true;
        }
        return false;
    }

    bool SceneLifecycleSample::PresentSecondaryCamera(SampleContext& context)
    {
        if (!m_secondaryCamera.IsValid())
        {
            Fail(context, CameraCutInvariant.code, "Secondary ECS camera is unavailable");
            return false;
        }
        m_secondaryCameraPresentationBaseline = m_lastObservedPresentationSequence;
        if (!context.cameras.Activate(m_secondaryCamera))
        {
            Fail(context, CameraCutInvariant.code, "ECS camera service rejected the secondary camera");
            return false;
        }
        SetActionTarget(ScenarioAction::PresentSecondaryCamera, false);
        m_state = ScenarioState::WaitingForSecondaryPresentation;
        return true;
    }

    bool SceneLifecycleSample::RestorePrimaryCamera(SampleContext& context)
    {
        if (!context.cameras.MarkCut(context.camera) ||
            !context.cameras.Activate(context.camera))
        {
            Fail(context,
                 CameraCutInvariant.code,
                 "ECS camera service could not restore the primary camera and cut");
            return false;
        }
        m_primaryCameraPresentationBaseline = m_lastObservedPresentationSequence;
        SetActionTarget(ScenarioAction::RestorePrimaryCamera, false);
        m_state = ScenarioState::WaitingForPrimaryPresentation;
        return true;
    }

    bool SceneLifecycleSample::MutateLight(SampleContext& context)
    {
        if (!m_lightMutationQueued)
        {
            m_lightWriteVersionBeforeMutation =
                context.scene.GetRegistry().GetFragmentWriteVersion<SceneECS::Light>(
                    m_light.entity);
            m_renderLightStateHashBefore = m_lastObservedRenderLightStateHash;
            SceneECS::Light updated = MakeProbeLight();
            updated.color = Vec3(0.35f, 0.65f, 1.0f);
            updated.intensity = 3.0f;
            updated.range = 8.0f;
            if (!QueueFragmentWrite(context,
                                    m_lightMutation,
                                    m_lightMutationBuffer,
                                    updated))
            {
                return false;
            }
            SetActionTarget(ScenarioAction::MutateLight);
            m_lightMutationQueued = true;
            m_renderLightBaselineCaptured = true;
            return true;
        }
        if (m_lightMutationBuffer.IsQueued())
        {
            return false;
        }
        std::string error;
        if (!ValidateAppliedReceipt(m_lightMutation, m_lightMutationBuffer, error))
        {
            Fail(context, LightMutationAction.code, std::move(error));
            return false;
        }
        m_lightWriteVersionAfterMutation =
            context.scene.GetRegistry().GetFragmentWriteVersion<SceneECS::Light>(
                m_light.entity);
        if (m_lightWriteVersionAfterMutation <= m_lightWriteVersionBeforeMutation)
        {
            Fail(context,
                 LightRevisionInvariant.code,
                 "Applied Light fragment command did not advance the entity write version");
            return false;
        }
        m_state = ScenarioState::MutateMaterialSlots;
        return true;
    }

    bool SceneLifecycleSample::MutateMaterialSlots(SampleContext& context)
    {
        if (!m_materialMutationQueued)
        {
            if (!QueueMaterialSlotWrite(context))
            {
                return false;
            }
            SetActionTarget(ScenarioAction::MutateMaterialSlots, false);
            m_materialMutationQueued = true;
            return true;
        }
        if (m_materialMutationBuffer.IsQueued())
        {
            return false;
        }
        std::string error;
        if (!ValidateAppliedReceipt(
                m_materialMutation, m_materialMutationBuffer, error))
        {
            Fail(context, MaterialMutationAction.code, std::move(error));
            return false;
        }
        m_materialWriteVersionAfterMutation =
            context.scene.GetRegistry().GetFragmentWriteVersion<SceneECS::MaterialSlots>(
                m_materialEntity.entity);
        if (m_materialWriteVersionAfterMutation <= m_materialWriteVersionBeforeMutation)
        {
            Fail(context,
                 MaterialRevisionInvariant.code,
                 "Applied MaterialSlots command did not advance the entity write version");
            return false;
        }
        m_state = ScenarioState::DestroyAndReuseEntitySlot;
        return true;
    }

    bool SceneLifecycleSample::AdvanceDestroyAndReuse(SampleContext& context)
    {
        if (!m_childDestroyRequested)
        {
            if (context.scene.RequestDestroy(
                    m_child.entity,
                    SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::None)) !=
                SceneECS::DestroyRequestResult::Accepted)
            {
                Fail(context,
                     DestroyReuseAction.code,
                     "ECS runtime rejected cleanup-gated child retirement");
                return false;
            }
            m_childPendingDestroyObserved =
                context.scene.GetDiagnosticsSnapshot().pendingDestroyCount != 0;
            if (!m_childPendingDestroyObserved)
            {
                Fail(context,
                     CleanupInvariant.code,
                     "Child destroy request did not enter the ECS pending-destroy state");
                return false;
            }
            m_childDestroyRequested = true;
            return true;
        }

        if (context.scene.GetEntityRef(m_child.entity).IsValid())
        {
            return false;
        }
        if (!m_replacement.IsValid())
        {
            m_replacement = context.sceneLifetime.CreateAndAdopt();
            if (!m_replacement.IsValid() ||
                m_replacement.entity.GetIndex() != m_child.entity.GetIndex() ||
                m_replacement.entity.GetGeneration() == m_child.entity.GetGeneration())
            {
                Fail(context,
                     StaleEntityInvariant.code,
                     "ECS child replacement did not reuse the retired slot with a new generation");
                return false;
            }
            m_expectedFinalOwnedEntityCount = 5;
            const std::array<SceneECS::SceneEntityRef, 5> expected{
                m_parentA,
                m_parentB,
                m_light,
                m_replacement,
                {.sceneRuntimeId = m_secondaryCamera.sceneRuntimeId,
                 .entity = m_secondaryCamera.entity}};
            m_finalOwnedEntityCount = static_cast<uint32>(std::count_if(
                expected.begin(), expected.end(),
                [&context](SceneECS::SceneEntityRef entity)
                {
                    return IsLive(context.scene, entity);
                }));
            if (m_finalOwnedEntityCount != m_expectedFinalOwnedEntityCount)
            {
                Fail(context,
                     DestroyReuseAction.code,
                     "ECS replacement did not leave the expected final procedural population");
                return false;
            }
            SetActionTarget(ScenarioAction::DestroyAndReuseEntitySlot, false);
            m_state = ScenarioState::WaitingForStablePresentation;
            return true;
        }
        return false;
    }

    bool SceneLifecycleSample::ConfigureModelPresentation(SampleContext& context)
    {
        AABB bounds;
        if (!TryComputeSampleModelRenderableWorldBounds(m_model, context.scene, bounds) ||
            !bounds.IsValid())
        {
            Fail(context,
                 AssessmentCode("SCENE.LIFECYCLE.PRESENTATION_VALID"),
                 "CPU-ready ECS model has no finite renderable bounds");
            return false;
        }
        const Vec3 size = bounds.GetSize();
        const float32 maximumDimension =
            std::max({size.x, size.y, size.z});
        if (!std::isfinite(maximumDimension) || maximumDimension <= 0.00001f)
        {
            Fail(context,
                 AssessmentCode("SCENE.LIFECYCLE.PRESENTATION_VALID"),
                 "ECS model bounds are degenerate");
            return false;
        }
        const float32 aspect =
            static_cast<float32>(context.options.width) /
            static_cast<float32>(std::max(context.options.height, 1u));
        const ModelCameraFrame frame =
            BuildModelCameraFrame(bounds, aspect, radians(45.0f), 1.18f);
        if (!frame.valid)
        {
            Fail(context,
                 AssessmentCode("SCENE.LIFECYCLE.PRESENTATION_VALID"),
                 "ECS model could not produce a framing camera pose");
            return false;
        }
        SampleOrbitCameraSettings settings;
        settings.mode = OrbitCameraMode::ExteriorInspect;
        settings.bounds = bounds;
        settings.pivot = frame.target;
        settings.distance = frame.distance;
        settings.yaw = 0.52f;
        settings.pitch = 0.24f;
        settings.minDistance = std::max(frame.distance * 0.35f, 0.001f);
        settings.maxDistance = std::max(frame.distance * 4.0f,
                                        settings.minDistance * 2.0f);
        settings.verticalFovRadians = radians(45.0f);
        settings.aspectRatio = aspect;
        settings.fitMargin = 1.18f;
        m_orbitCamera.Initialize(settings, context.input);
        if (!m_orbitCamera.IsInitialized() ||
            !m_orbitCamera.Apply(context.cameras, context.camera))
        {
            Fail(context,
                 AssessmentCode("SCENE.LIFECYCLE.PRESENTATION_VALID"),
                 "ECS orbit camera could not apply the model framing pose");
            return false;
        }
        m_presentationBounds = bounds;
        m_cameraFrame = frame;
        m_presentationReady = true;
        return true;
    }

    bool SceneLifecycleSample::ConfigureSecondaryCamera(
        SampleContext& context,
        WorldECS::WorldEcsCameraRef camera)
    {
        const float32 aspect =
            static_cast<float32>(context.options.width) /
            static_cast<float32>(std::max(context.options.height, 1u));
        if (!context.cameras.SetPerspective(
                camera, radians(45.0f), aspect, 0.05f, 250.0f) ||
            !context.cameras.SetPose(
                camera, {.position = Vec3(0.0f, 1.0f, 4.5f)}) ||
            !context.cameras.LookAt(camera, Vec3(0.0f, 0.45f, 0.0f)) ||
            !context.cameras.MarkCut(camera))
        {
            return false;
        }
        if (m_presentationReady && !m_orbitCamera.Apply(context.cameras, camera))
        {
            return false;
        }
        return true;
    }

    bool SceneLifecycleSample::QueueReparent(
        SampleContext& context,
        SceneECS::ReparentMode mode,
        SceneECS::SceneCommandReceipt& outReceipt,
        SceneECS::SceneCommandBufferReceipt& outBufferReceipt)
    {
        const SceneECS::SceneEntityRef parent =
            mode == SceneECS::ReparentMode::KeepLocal ? m_parentA : m_parentB;
        if (!IsLive(context.scene, m_child) || !IsLive(context.scene, parent))
        {
            Fail(context, HierarchyAuthorityInvariant.code,
                 "ECS hierarchy command referenced a stale scene-qualified entity");
            return false;
        }
        SceneECS::SceneCommandBuffer commands = context.scene.CreateCommandBuffer();
        outReceipt = commands.Reparent(m_child.entity, parent.entity, mode);
        outBufferReceipt = context.scene.SubmitCommandBuffer(
            std::move(commands), SceneECS::SceneCommandBarrier::BeginSimulation);
        if (outReceipt.IsRejected() || outBufferReceipt.IsRejected() ||
            outBufferReceipt.IsDiscarded())
        {
            Fail(context,
                 HierarchyAuthorityInvariant.code,
                 "ECS hierarchy command was rejected before playback");
            return false;
        }
        return true;
    }

    bool SceneLifecycleSample::QueueFragmentWrite(
        SampleContext& context,
        SceneECS::SceneCommandReceipt& outReceipt,
        SceneECS::SceneCommandBufferReceipt& outBufferReceipt,
        const SceneECS::Light& light)
    {
        if (!IsLive(context.scene, m_light))
        {
            Fail(context, LightRevisionInvariant.code, "ECS light entity is stale");
            return false;
        }
        SceneECS::SceneCommandBuffer commands = context.scene.CreateCommandBuffer();
        outReceipt = commands.Set(m_light.entity, light);
        outBufferReceipt = context.scene.SubmitCommandBuffer(
            std::move(commands), SceneECS::SceneCommandBarrier::BeginSimulation);
        if (outReceipt.IsRejected() || outBufferReceipt.IsRejected() ||
            outBufferReceipt.IsDiscarded())
        {
            Fail(context,
                 LightRevisionInvariant.code,
                 "ECS Light fragment command was rejected before playback");
            return false;
        }
        return true;
    }

    bool SceneLifecycleSample::QueueMaterialSlotWrite(SampleContext& context)
    {
        for (const ResourceSceneAdapters::PreparedModelEntityMapping& mapping :
             m_model.status.entityMappings)
        {
            const SceneECS::SceneEntityRef entity =
                context.scene.GetEntityRef(mapping.entity);
            if (!IsLive(context.scene, entity))
            {
                continue;
            }
            const SceneECS::MaterialSlots* slots =
                context.scene.GetRegistry().TryGet<SceneECS::MaterialSlots>(entity.entity);
            if (slots == nullptr || slots->count == 0)
            {
                continue;
            }
            m_materialEntity = entity;
            m_materialSlotsBefore = *slots;
            m_materialSlotsAfter = m_materialSlotsBefore;
            m_materialWriteVersionBeforeMutation =
                context.scene.GetRegistry().GetFragmentWriteVersion<SceneECS::MaterialSlots>(
                    entity.entity);
            SceneECS::SceneCommandBuffer commands = context.scene.CreateCommandBuffer();
            m_materialMutation = commands.Set(entity.entity, m_materialSlotsAfter);
            m_materialMutationBuffer = context.scene.SubmitCommandBuffer(
                std::move(commands), SceneECS::SceneCommandBarrier::BeginSimulation);
            if (m_materialMutation.IsRejected() || m_materialMutationBuffer.IsRejected() ||
                m_materialMutationBuffer.IsDiscarded())
            {
                Fail(context,
                     MaterialRevisionInvariant.code,
                     "ECS MaterialSlots command was rejected before playback");
                return false;
            }
            return true;
        }
        Fail(context,
             MaterialRevisionInvariant.code,
             "ECS model metadata had no exact mapped renderable MaterialSlots entity");
        return false;
    }

    bool SceneLifecycleSample::ValidateAppliedReceipt(
        const SceneECS::SceneCommandReceipt& receipt,
        const SceneECS::SceneCommandBufferReceipt& bufferReceipt,
        std::string& outError) const
    {
        outError.clear();
        if (bufferReceipt.IsQueued() || receipt.IsQueued())
        {
            return false;
        }
        if (!bufferReceipt.IsApplied() || !receipt.IsApplied())
        {
            outError = "ECS command buffer or its exact command receipt was rejected during playback";
            return false;
        }
        return true;
    }

    bool SceneLifecycleSample::ValidateHierarchyAuthority(
        const SampleContext& context,
        SceneECS::SceneEntityRef expectedParent) const
    {
        if (!IsLive(context.scene, m_child) || !IsLive(context.scene, expectedParent))
        {
            return false;
        }
        const SceneECS::ParentRelation* relation =
            context.scene.GetRegistry().TryGet<SceneECS::ParentRelation>(m_child.entity);
        const SceneECS::SimulationWorldTransform* world =
            context.scene.GetRegistry().TryGet<SceneECS::SimulationWorldTransform>(
                m_child.entity);
        return relation != nullptr && world != nullptr &&
               relation->parent == expectedParent.entity &&
               world->resolvedParent == expectedParent.entity;
    }

    bool SceneLifecycleSample::IsActionPresentationCovered(
        const SampleRenderDiagnostics& diagnostics,
        const ScenarioActionEvidence& action) const
    {
        return action.targetSceneRevision != 0 &&
               diagnostics.lastPresentedFrameSequence.IsAvailable() &&
               *diagnostics.lastPresentedFrameSequence.GetValue() >
                   action.minimumPresentationSequence &&
               diagnostics.renderSceneValuesAvailable &&
               diagnostics.renderSceneAppliedRevision >= action.targetSceneRevision &&
               diagnostics.engineRenderRuntimeAvailable;
    }

    bool SceneLifecycleSample::AreTerminalQueuesDrained(
        const SampleRenderDiagnostics& diagnostics) const
    {
        if (!m_terminalSceneDiagnosticsCaptured ||
            !m_terminalResourceDiagnosticsAvailable ||
            !diagnostics.lastPresentedFrameSequence.IsAvailable() ||
            *diagnostics.lastPresentedFrameSequence.GetValue() <=
                m_terminalDiagnosticsCapturePresentationSequence)
        {
            return false;
        }
        const SceneECS::SceneEcsDiagnosticsSnapshot& scene =
            m_terminalSceneDiagnostics;
        if (scene.pendingDestroyCount != 0 || scene.cleanupRequiredCount != 0 ||
            scene.retiringCount != 0 || scene.recyclableCount != 0 ||
            scene.queuedCommandBufferCount != 0)
        {
            return false;
        }
        const Resource::ResourceDiagnosticsSnapshot& resource =
            m_terminalResourceDiagnostics;
        if (resource.activeOperations != 0 || resource.activeSubscribers != 0 ||
            resource.pendingAsyncJobs != 0 || resource.pendingAsyncCompletions != 0 ||
            resource.decodeQueuedCount != 0 || resource.decodeActiveCount != 0 ||
            resource.pendingPublicationCount != 0 || resource.pendingUploadCount != 0 ||
            resource.pendingReplacementCount != 0 || resource.pendingRollbackCount != 0 ||
            resource.pendingRetirementCount != 0 || resource.queuedLeaseUnloadCount != 0)
        {
            return false;
        }
        return diagnostics.renderPendingUploadCount == 0 &&
               diagnostics.renderRetirementEntryCount == 0;
    }

    bool SceneLifecycleSample::IsRenderPathQualified(
        const SampleRenderDiagnostics& diagnostics) const
    {
        if (m_renderPath == SampleRenderPath::Auto)
        {
            return true;
        }
        if (m_renderPath == SampleRenderPath::GPUDriven)
        {
            return diagnostics.gpuDrivenPolicyDecisionAvailable &&
                   diagnostics.gpuDrivenRequestedMode == "ForceEnabled" &&
                   diagnostics.gpuDrivenPolicyReason == "None" &&
                   diagnostics.gpuDrivenEnabled &&
                   diagnostics.gpuDrivenGraphPassRecorded &&
                   diagnostics.gpuDrivenExecutionRecorded &&
                   diagnostics.gpuDrivenOpaqueIndirectRequested &&
                   diagnostics.gpuDrivenOpaqueIndirectEligible &&
                   diagnostics.gpuDrivenOpaqueIndirectSubmitted &&
                   diagnostics.gpuDrivenOpaqueIndirectBatchCount > 0;
        }
        return diagnostics.gpuDrivenPolicyDecisionAvailable &&
               diagnostics.gpuDrivenRequestedMode == "ForceDisabled" &&
               diagnostics.gpuDrivenPolicyReason == "ForcedDisabled" &&
               !diagnostics.gpuDrivenEnabled &&
               !diagnostics.gpuDrivenOpaqueIndirectRequested &&
               !diagnostics.gpuDrivenOpaqueIndirectEligible &&
               !diagnostics.gpuDrivenOpaqueIndirectSubmitted &&
               diagnostics.gpuDrivenOpaqueIndirectBatchCount == 0 &&
               diagnostics.gpuDrivenOpaqueDirectDrawCount > 0 &&
               diagnostics.opaqueExecutionCompleted &&
               diagnostics.opaqueExecutedDrawCountAvailable &&
               diagnostics.opaqueExecutedDrawCount > 0;
    }

    bool SceneLifecycleSample::HasExactFinalPopulation(
        const SampleContext& context) const
    {
        const std::array<SceneECS::SceneEntityRef, 5> expected{
            m_parentA,
            m_parentB,
            m_light,
            m_replacement,
            {.sceneRuntimeId = m_secondaryCamera.sceneRuntimeId,
             .entity = m_secondaryCamera.entity}};
        return std::all_of(expected.begin(), expected.end(),
                           [&context](SceneECS::SceneEntityRef entity)
                           {
                               return IsLive(context.scene, entity);
                           }) &&
               !context.scene.GetEntityRef(m_child.entity).IsValid();
    }

    bool SceneLifecycleSample::AreAllActionsPresented() const noexcept
    {
        return std::all_of(m_actionEvidence.begin(), m_actionEvidence.end(),
                           [](const ScenarioActionEvidence& action)
                           {
                               return action.passed;
                           });
    }

    void SceneLifecycleSample::CaptureTerminalDiagnostics(SampleContext& context)
    {
        m_terminalSceneDiagnostics = context.scene.GetDiagnosticsSnapshot();
        m_terminalSceneDiagnosticsCaptured = true;
        m_terminalDiagnosticsCapturePresentationSequence =
            m_lastObservedPresentationSequence;
        const Resource::ResourceDiagnosticsQueryResult result =
            context.resourceDiagnostics.QueryResourceDiagnostics();
        m_terminalResourceDiagnosticsAvailable = result.IsAvailable();
        if (m_terminalResourceDiagnosticsAvailable)
        {
            m_terminalResourceDiagnostics = *result.snapshot.GetValue();
            m_terminalResourceDiagnosticsReason.clear();
        }
        else
        {
            m_terminalResourceDiagnostics = {};
            m_terminalResourceDiagnosticsReason = result.snapshot.GetReason();
        }
        if (!HasExactFinalPopulation(context))
        {
            Fail(context,
                 StaleEntityInvariant.code,
                 "Final ECS population changed while waiting for retirement drain");
        }
    }

    void SceneLifecycleSample::CompleteAction(
        ScenarioAction action,
        const SampleRenderDiagnostics& diagnostics,
        SampleAssessmentChannel& assessment)
    {
        ScenarioActionEvidence& evidence = GetAction(action);
        if (evidence.passed || !IsActionPresentationCovered(diagnostics, evidence))
        {
            return;
        }
        evidence.completedPresentationSequence =
            *diagnostics.lastPresentedFrameSequence.GetValue();
        evidence.appliedSceneRevision = diagnostics.renderSceneAppliedRevision;
        evidence.passed = true;

        const AssessmentAction* assessmentAction = nullptr;
        switch (action)
        {
            case ScenarioAction::Spawn:
                assessmentAction = &DeferredSpawnAction;
                break;
            case ScenarioAction::ReparentKeepLocal:
                assessmentAction = &KeepLocalAction;
                break;
            case ScenarioAction::ReparentKeepWorld:
                assessmentAction = &KeepWorldAction;
                MarkInvariant(assessment,
                              HierarchyAuthorityInvariant.code,
                              HierarchyAuthorityInvariant.description);
                break;
            case ScenarioAction::RecycleSecondaryCamera:
                assessmentAction = &CameraReuseAction;
                MarkInvariant(assessment,
                              StaleEntityInvariant.code,
                              StaleEntityInvariant.description);
                break;
            case ScenarioAction::PresentSecondaryCamera:
                break;
            case ScenarioAction::RestorePrimaryCamera:
                assessmentAction = &CameraSwitchAction;
                MarkInvariant(assessment,
                              CameraCutInvariant.code,
                              CameraCutInvariant.description);
                break;
            case ScenarioAction::MutateLight:
                assessmentAction = &LightMutationAction;
                MarkInvariant(assessment,
                              LightRevisionInvariant.code,
                              LightRevisionInvariant.description);
                break;
            case ScenarioAction::MutateMaterialSlots:
                assessmentAction = &MaterialMutationAction;
                MarkInvariant(assessment,
                              MaterialRevisionInvariant.code,
                              MaterialRevisionInvariant.description);
                break;
            case ScenarioAction::DestroyAndReuseEntitySlot:
                assessmentAction = &DestroyReuseAction;
                MarkInvariant(assessment,
                              StaleEntityInvariant.code,
                              StaleEntityInvariant.description);
                break;
            case ScenarioAction::Count:
            default:
                return;
        }
        if (assessmentAction != nullptr)
        {
            static_cast<void>(assessment.MarkAction(*assessmentAction));
        }
        if (!m_assessmentActionAppliedMarked)
        {
            static_cast<void>(assessment.MarkCheckpoint(
                AssessmentCheckpoints::ActionApplied));
            m_assessmentActionAppliedMarked = true;
        }
    }

    void SceneLifecycleSample::MarkInvariant(
        SampleAssessmentChannel& assessment,
        AssessmentCode code,
        std::string description)
    {
        static_cast<void>(assessment.TryPublish(
            AssessmentInvariantObservation{{std::move(code), std::move(description)},
                                           AssessmentCheckpoints::ActionApplied}));
    }

    void SceneLifecycleSample::SetActionTarget(ScenarioAction action,
                                               bool requiresSceneMutation)
    {
        ScenarioActionEvidence& evidence = GetAction(action);
        const uint64 revisionAdvance = requiresSceneMutation ? 1u : 0u;
        evidence.targetSceneRevision = std::max<uint64>(
            m_lastObservedRenderSceneRevision + revisionAdvance, 1u);
        evidence.minimumPresentationSequence = m_lastObservedPresentationSequence;
    }

    SceneLifecycleSample::ScenarioActionEvidence&
    SceneLifecycleSample::GetAction(ScenarioAction action) noexcept
    {
        return m_actionEvidence[static_cast<size_t>(action)];
    }

    const SceneLifecycleSample::ScenarioActionEvidence&
    SceneLifecycleSample::GetAction(ScenarioAction action) const noexcept
    {
        return m_actionEvidence[static_cast<size_t>(action)];
    }

    void SceneLifecycleSample::Fail(SampleAssessmentChannel& assessment,
                                    AssessmentCode invariantCode,
                                    std::string message)
    {
        if (m_state == ScenarioState::Failed)
        {
            return;
        }
        m_failure = std::move(message);
        m_state = ScenarioState::Failed;
        Finding finding;
        finding.code = AssessmentCode("SCENE.LIFECYCLE.ACTION_FAILED");
        finding.subsystemCode = AssessmentCode("SCENE.LIFECYCLE");
        finding.invariantCode = std::move(invariantCode);
        finding.checkpoint = AssessmentCheckpoints::ActionApplied;
        finding.classification = FindingClass::ContractViolation;
        finding.severity = FindingSeverity::Error;
        finding.confidence = FindingConfidence::Confirmed;
        finding.summary = "The pure-ECS scene lifecycle probe could not complete an action.";
        finding.detail = m_failure;
        finding.expected = "The Scene ECS lifecycle contract remains coherent.";
        finding.observed = m_failure;
        finding.gating = true;
        finding.blockingReason = "The deterministic pure-ECS lifecycle probe failed.";
        static_cast<void>(assessment.TryPublish(std::move(finding)));
    }

    void SceneLifecycleSample::Fail(SampleContext& context,
                                    AssessmentCode invariantCode,
                                    std::string message)
    {
        Fail(context.assessment, std::move(invariantCode), std::move(message));
    }
} // namespace RVX
