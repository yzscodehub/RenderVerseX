#pragma once

/**
 * @file PhysicsSandboxSample.h
 * @brief Pure-ECS qualification scene for the built-in fixed-step physics bridge.
 */

#include "Core/Math/AABB.h"
#include "Engine/ECS/IWorldEcsRuntimeServices.h"
#include "Samples/Sample.h"
#include "Samples/SampleModelLoader.h"
#include "Samples/SampleOrbitCameraController.h"
#include "Scene/ECS/PhysicsFragments.h"
#include "Scene/ECS/SceneEcsRuntime.h"

#include <array>
#include <string>
#include <vector>

namespace RVX
{
    class PhysicsSandboxSampleValidationAccess;

    /**
     * @brief Exercises data-only physics fragments through the World ECS clock.
     *
     * Every retained identity is a Scene-qualified, generation-safe value. The
     * physics bridge remains the sole owner of backend body instances.
     */
    class PhysicsSandboxSample final : public ISample
    {
    public:
        [[nodiscard]] const SampleInfo& GetInfo() const noexcept override;
        [[nodiscard]] SampleWorldRequirements GetWorldRequirements() const override;
        [[nodiscard]] SampleAssessmentContract
            GetAssessmentContract() const override;
        bool Setup(SampleContext& context, std::string& outError) override;
        void Update(SampleContext& context, float deltaTime) override;
        void OnInput(SampleContext& context) override;
        void OnViewportResize(SampleContext& context,
                              uint32 width,
                              uint32 height) override;
        void AppendReport(SampleFeatureReporter& reporter) const override;
        void ObserveDiagnostics(const SampleRenderDiagnostics& diagnostics,
                                SampleAssessmentChannel& assessment) override;
        SampleReadiness GetReadiness(
            const SampleRenderDiagnostics& diagnostics) const override;
        bool ValidateResult(const SampleRenderDiagnostics& diagnostics,
                            std::string& outError) const override;
        void Shutdown(SampleContext& context) override;

    private:
        enum class ScenarioState : uint8
        {
            AwaitModel = 0,
            AwaitBindings,
            Settle,
            DriveKinematic,
            VerifyPhysicsOutput,
            AwaitColliderRefresh,
            AwaitProbeRetirement,
            AwaitProbeRecreate,
            AwaitStaleRefReject,
            AwaitTerminalCensus,
            Stable,
            Failed,
        };

        enum class ScenarioAction : uint8
        {
            CreateBodies = 0,
            Settle120Steps,
            DriveKinematicPlatform120Steps,
            VerifyDynamicContacts,
            RebuildCollider,
            DestroyBody,
            RecreateSameSlot,
            RejectStaleHandle,
            Count,
        };

        enum class VisualEntityRole : uint8
        {
            Floor = 0,
            Dynamic,
            Platform,
            Probe,
        };

        struct ActionEvidence
        {
            const char* name = "";
            uint64 targetSceneRevision = 0;
            uint64 minimumPresentationSequence = 0;
            uint64 completedPresentationSequence = 0;
            uint64 appliedSceneRevision = 0;
            bool prerequisitesSatisfied = false;
            bool passed = false;
        };

        [[nodiscard]] bool ActivateSharedModel(SampleContext& context);
        [[nodiscard]] bool CreateBodies(SampleContext& context,
                                        std::string& outError);
        [[nodiscard]] bool CreateProbe(SampleContext& context,
                                       const Vec3& position,
                                       SceneECS::SceneEntityRef& outProbe);
        [[nodiscard]] bool BeginColliderRefresh(SampleContext& context);
        [[nodiscard]] bool BeginProbeRetirement(SampleContext& context);
        [[nodiscard]] bool BeginProbeRecreate(SampleContext& context);
        [[nodiscard]] bool BeginStaleRefReject(SampleContext& context);
        [[nodiscard]] bool InitializePresentationCamera(SampleContext& context);
        [[nodiscard]] bool ArePhysicsEntitiesBound(
            const SceneECS::SceneEcsRuntime& scene) const;
        [[nodiscard]] bool HasDynamicPhysicsOutput(
            const SceneECS::SceneEcsRuntime& scene) const;
        [[nodiscard]] bool HasActivePhysicsState(
            const SceneECS::SceneEcsRuntime& scene,
            SceneECS::SceneEntityRef entity,
            uint64 minimumStep) const;
        [[nodiscard]] bool HasRequiredRenderPathEvidence(
            const SampleRenderDiagnostics& diagnostics) const;
        [[nodiscard]] bool IsActionPresentationCovered(
            const SampleRenderDiagnostics& diagnostics,
            const ActionEvidence& action) const;
        [[nodiscard]] bool AreAllActionsPresented() const noexcept;
        [[nodiscard]] bool HasQualifiedTerminalCensus() const noexcept;
        [[nodiscard]] static Vec3 GetBoxVisualScale(
            const Vec3& halfExtents) noexcept;
        [[nodiscard]] static uint32 GetSharedMaterialIndex(
            VisualEntityRole role,
            uint32 dynamicIndex = 0) noexcept;
        [[nodiscard]] static AABB GetPresentationBounds() noexcept;
        [[nodiscard]] static float32 GetPresentationFitMargin() noexcept;
        [[nodiscard]] static float32 GetDirectionalLightIntensity() noexcept;
        [[nodiscard]] static bool IsInteractiveOrbitEnabled(bool smoke) noexcept;
        [[nodiscard]] static const char* GetActionName(
            ScenarioAction action) noexcept;
        [[nodiscard]] ActionEvidence& GetAction(ScenarioAction action) noexcept;
        [[nodiscard]] const ActionEvidence& GetAction(
            ScenarioAction action) const noexcept;
        void ArmActionForPresentation(ScenarioAction action);
        void CompleteAction(ScenarioAction action,
                            const SampleRenderDiagnostics& diagnostics,
                            SampleAssessmentChannel& assessment);
        void MarkInvariant(SampleAssessmentChannel& assessment,
                           const AssessmentInvariant& invariant) const;
        void Fail(SampleContext& context,
                  AssessmentCode invariant,
                  std::string message);

        LoadedSampleModel m_model;
        SampleOrbitCameraController m_orbitCamera;
        AABB m_presentationBounds;
        std::array<ActionEvidence, static_cast<size_t>(ScenarioAction::Count)>
            m_actions{};
        std::vector<SceneECS::SceneEntityRef> m_physicsEntities;
        std::vector<SceneECS::SceneEntityRef> m_dynamicEntities;
        SceneECS::SceneEntityRef m_platformEntity;
        SceneECS::SceneEntityRef m_probeEntity;
        SceneECS::SceneEntityRef m_recreatedProbeEntity;
        WorldEcsRuntimeServicesDiagnostics m_lastRuntime{};
        SampleRenderPath m_renderPath = SampleRenderPath::Auto;
        ScenarioState m_state = ScenarioState::AwaitModel;
        std::string m_failure;
        Vec3 m_platformStartPosition{0.0f};
        uint64 m_settleStartFixedStep = 0;
        uint64 m_driveStartFixedStep = 0;
        uint64 m_lastKinematicIntentStep = 0;
        uint64 m_rebuildWriteStep = 0;
        uint64 m_destroyFixedStep = 0;
        uint64 m_finalPhysicsStep = 0;
        uint64 m_lastObservedPresentationSequence = 0;
        uint64 m_kinematicIntentCount = 0;
        uint64 m_collisionContactEvidenceCount = 0;
        uint64 m_deterministicSeed = 0x52565850ull;
        float32 m_dynamicProbeInitialY = 0.0f;
        bool m_backendQualified = false;
        bool m_sharedModelActivated = false;
        bool m_colliderRefreshRequested = false;
        bool m_colliderRefreshObserved = false;
        bool m_probeRetirementRequested = false;
        bool m_probeRetired = false;
        bool m_probeRecreated = false;
        bool m_staleRefRejected = false;
        bool m_dynamicPhysicsObserved = false;
        bool m_terminalMetricsPublished = false;
        bool m_builtinCapabilityPublished = false;
        bool m_joltCapabilityPublished = false;

        friend class PhysicsSandboxSampleValidationAccess;
    };
} // namespace RVX
