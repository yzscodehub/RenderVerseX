#pragma once

/**
 * @file InteriorRenderingSample.h
 * @brief Backend-neutral P0-A interior rendering product qualification scene.
 */

#include "Core/Math/AABB.h"
#include "Samples/ModelCameraFraming.h"
#include "Samples/Sample.h"
#include "Samples/SampleEnvironmentLoader.h"
#include "Samples/SampleModelLoader.h"
#include "Samples/SampleOrbitCameraController.h"

#include <string>

namespace RVX
{
    class InteriorRenderingSampleValidationAccess;

    /**
     * @brief Exercises the engine-owned interior lighting, shadow, transparency,
     * IBL, and clustered-lighting contracts through one fixed scene.
     */
    class InteriorRenderingSample final : public ISample
    {
    public:
        [[nodiscard]] const SampleInfo& GetInfo() const noexcept override;
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
        [[nodiscard]] bool AreAssetsFullyResident() const noexcept;
        [[nodiscard]] bool HasCompletedFrame(
            const SampleRenderDiagnostics& diagnostics) const noexcept;
        [[nodiscard]] bool IsRenderScenePresentationCovered(
            const SampleRenderDiagnostics& diagnostics) const noexcept;
        [[nodiscard]] bool IsRenderPathQualified(
            const SampleRenderDiagnostics& diagnostics) const noexcept;
        [[nodiscard]] bool IsDirectionalCSMQualified(
            const SampleRenderDiagnostics& diagnostics) const noexcept;
        [[nodiscard]] bool IsLocalLightingQualified(
            const SampleRenderDiagnostics& diagnostics) const noexcept;
        [[nodiscard]] bool IsTransparentFrameValid(
            const SampleRenderDiagnostics& diagnostics) const noexcept;
        [[nodiscard]] bool IsFrameQualified(
            const SampleRenderDiagnostics& diagnostics) const noexcept;
        [[nodiscard]] bool CaptureProductionAssetComposition(
            std::string& outError);
        [[nodiscard]] bool PrepareInteriorPresentation(
            SampleContext& context,
            std::string& outError);
        [[nodiscard]] bool ActivateTransparentGlassPanels(
            SampleContext& context,
            std::string& outError);
        [[nodiscard]] static bool IsNormalizedInteriorPlacement(
            const AABB& sourceBounds,
            const AABB& placedBounds,
            float32 targetCorridorExtent) noexcept;
        [[nodiscard]] static bool ShouldConsumeOrbitInput(
            bool smoke,
            bool presentationReady,
            bool orbitInitialized,
            bool inputAvailable) noexcept;
        [[nodiscard]] static bool BuildInteriorOrbitSettings(
            const AABB& bounds,
            float32 aspectRatio,
            float32 framingDistance,
            SampleOrbitCameraSettings& outSettings) noexcept;
        [[nodiscard]] static bool TryBuildInteriorStartInput(
            const OrbitCameraRigPose& fittedPose,
            const SampleOrbitCameraSettings& settings,
            SampleOrbitCameraInput& outInput) noexcept;

        void ObserveCapabilities(const SampleRenderDiagnostics& diagnostics,
                                 SampleAssessmentChannel& assessment);
        void ObserveTransparentOrder(
            const SampleRenderDiagnostics& diagnostics) noexcept;
        void PublishCapability(SampleAssessmentChannel& assessment,
                               const AssessmentCapability& capability,
                               DiagnosticValue<bool> value,
                               std::string reason,
                               bool& published);
        void PublishCapabilityGap(SampleAssessmentChannel& assessment,
                                  AssessmentCode code,
                                  std::string detail,
                                  bool& published);
        void PublishFailure(SampleAssessmentChannel& assessment,
                            AssessmentCode code,
                            AssessmentCode invariantCode,
                            std::string detail,
                            FindingClass classification =
                                FindingClass::ContractViolation);
        void PublishQualification(SampleAssessmentChannel& assessment,
                                  const SampleRenderDiagnostics& diagnostics);
        void ResetDiagnosticState() noexcept;

        LoadedSampleModel m_model;
        /** @brief Deterministic two-panel BLEND material support asset. */
        LoadedSampleModel m_glassPanelTemplate;
        LoadedSampleEnvironment m_environment;
        AABB m_interiorBounds;
        ModelCameraFrame m_cameraFrame;
        SampleOrbitCameraController m_orbitCamera;
        SampleRenderPath m_renderPath = SampleRenderPath::Auto;
        std::string m_failure;

        uint64 m_lastCompletedFrameSequence = 0;
        uint64 m_lastTransparentFrameSequence = 0;
        uint64 m_lastTransparentOrderHash = 0;
        uint32 m_completedResidentFrameCount = 0;
        uint32 m_transparentStableFrameCount = 0;

        bool m_directionalLightCreated = false;
        uint32 m_pointLightCount = 0;
        uint32 m_spotLightCount = 0;
        uint32 m_productionMeshCount = 0;
        uint32 m_productionMaterialCount = 0;
        uint32 m_productionTextureCount = 0;
        uint32 m_transparentGlassPanelCount = 0;
        uint64 m_qualifiedSceneRevision = 0;
        uint64 m_qualifiedPresentationSequence = 0;
        uint64 m_qualifiedAppliedSceneRevision = 0;
        bool m_productionAssetCompositionCaptured = false;
        bool m_interiorPresentationPrepared = false;
        bool m_transparentGlassPanelsActivated = false;
        bool m_directionalCapabilityPublished = false;
        bool m_transparentCapabilityPublished = false;
        bool m_pointShadowCapabilityPublished = false;
        bool m_spotShadowCapabilityPublished = false;
        bool m_hzbCapabilityPublished = false;
        bool m_pointShadowGapPublished = false;
        bool m_spotShadowGapPublished = false;
        bool m_hzbGapPublished = false;
        bool m_failurePublished = false;
        bool m_diagnosticsQualified = false;
        bool m_qualificationPublished = false;

        bool m_directionalCSMObserved = false;
        bool m_transparentObserved = false;
        bool m_localLightingObserved = false;
        bool m_pointShadowSupportedObserved = false;
        bool m_spotShadowSupportedObserved = false;
        bool m_hzbSupportedObserved = false;

        friend class InteriorRenderingSampleValidationAccess;
    };
} // namespace RVX
