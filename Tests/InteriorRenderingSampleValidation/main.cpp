#include "Scenes/InteriorRenderingSample.h"

#include "Core/Math/AABB.h"
#include "ResourceSceneAdapters/ECS/PreparedModelBatch.h"
#include "Samples/SampleCLI.h"
#include "Samples/SampleSceneLifetimeScope.h"
#include "Scene/ECS/RenderFragments.h"
#include "Scene/ECS/SceneEcsRuntime.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace RVX
{
    class InteriorRenderingSampleValidationAccess
    {
    public:
        static Resource::ResourceContentVerificationReceipt
        MakeVerifiedContentReceipt()
        {
            return {.status = Resource::ResourceContentVerificationStatus::Verified};
        }

        static LoadedSampleModel MakeFullyResidentModel(uint32 requestIndex,
                                                        uint64 sourceAssetValue)
        {
            constexpr ECS::SceneRuntimeId TestSceneRuntimeId(0x1A7E810u);
            LoadedSampleModel model;
            model.request = {
                .sceneRuntimeId = TestSceneRuntimeId,
                .handle = ResourceSceneAdapters::EcsSceneAssetLoadHandle::Create(
                    requestIndex, 1u)};
            model.status.state =
                ResourceSceneAdapters::EcsSceneAssetLoadState::FullyResident;
            model.status.sceneRuntimeId = TestSceneRuntimeId;
            model.status.rootEntity = ECS::EntityHandle::Create(requestIndex, 1u);
            model.status.members = {model.status.rootEntity};
            model.status.modelMetadata.sourceModelAssetId = {sourceAssetValue};
            model.status.modelMetadata.sourceNodeCount = 4u;
            model.status.modelMetadata.meshAssetIds = {
                {sourceAssetValue + 1u}, {sourceAssetValue + 2u}};
            model.status.modelMetadata.materialAssetIds = {
                {sourceAssetValue + 3u}, {sourceAssetValue + 4u}};
            model.status.modelMetadata.materials = {
                {{sourceAssetValue + 3u},
                 "InteriorStone",
                 Resource::MaterialWorkflowMode::MetallicRoughness,
                 Resource::MaterialAlphaMode::Opaque,
                 Vec4(1.0f),
                 0.0f,
                 0.8f,
                 {{"baseColor", {sourceAssetValue + 5u}},
                  {"normal", {sourceAssetValue + 6u}}}},
                {{sourceAssetValue + 4u},
                 "InteriorMetal",
                 Resource::MaterialWorkflowMode::MetallicRoughness,
                 Resource::MaterialAlphaMode::Opaque,
                 Vec4(1.0f),
                 1.0f,
                 0.25f,
                 {{"baseColor", {sourceAssetValue + 7u}}}}};
            return model;
        }

        static LoadedSampleEnvironment MakeFullyResidentEnvironment()
        {
            constexpr ECS::SceneRuntimeId TestSceneRuntimeId(0x1A7E810u);
            LoadedSampleEnvironment environment;
            environment.request = {
                .sceneRuntimeId = TestSceneRuntimeId,
                .handle = ResourceSceneAdapters::EcsEnvironmentLoadHandle::Create(
                    1u, 1u)};
            environment.status.state =
                ResourceSceneAdapters::EcsEnvironmentLoadState::FullyResident;
            environment.status.sceneRuntimeId = TestSceneRuntimeId;
            environment.contentVerificationReceipt = MakeVerifiedContentReceipt();
            environment.environmentResolution = 1024u;
            environment.irradianceResolution = 32u;
            environment.prefilteredResolution = 256u;
            environment.prefilteredMipLevels = 9u;
            environment.brdfLUTResolution = 512u;
            environment.exposure = 1.0f;
            return environment;
        }

        static void ConfigureResidentState(InteriorRenderingSample& sample,
                                           SampleRenderPath renderPath)
        {
            sample.m_model = MakeFullyResidentModel(1u, 100u);
            sample.m_glassPanelTemplate = MakeFullyResidentModel(2u, 200u);
            sample.m_environment = MakeFullyResidentEnvironment();
            sample.m_productionAssetCompositionCaptured = true;
            sample.m_transparentGlassPanelsActivated = true;
            sample.m_transparentGlassPanelCount = 2u;
            sample.m_directionalLightCreated = true;
            sample.m_pointLightCount = 8u;
            sample.m_spotLightCount = 4u;
            sample.m_renderPath = renderPath;
        }

        static void ConfigureQualifiedState(InteriorRenderingSample& sample,
                                            SampleRenderPath renderPath)
        {
            ConfigureResidentState(sample, renderPath);
            sample.m_diagnosticsQualified = true;
        }

        static LoadedSampleModel& Model(InteriorRenderingSample& sample)
        {
            return sample.m_model;
        }

        static const LoadedSampleModel& Model(const InteriorRenderingSample& sample)
        {
            return sample.m_model;
        }

        static LoadedSampleModel& GlassPanelTemplate(
            InteriorRenderingSample& sample)
        {
            return sample.m_glassPanelTemplate;
        }

        static const LoadedSampleEnvironment& Environment(
            const InteriorRenderingSample& sample)
        {
            return sample.m_environment;
        }

        static bool CaptureProductionAssetComposition(
            InteriorRenderingSample& sample,
            std::string& outError)
        {
            return sample.CaptureProductionAssetComposition(outError);
        }

        static void SetLightCensus(InteriorRenderingSample& sample,
                                   uint32 pointLightCount,
                                   uint32 spotLightCount)
        {
            sample.m_directionalLightCreated = true;
            sample.m_pointLightCount = pointLightCount;
            sample.m_spotLightCount = spotLightCount;
        }

        static bool IsNormalizedInteriorPlacement(
            const AABB& sourceBounds,
            const AABB& placedBounds,
            float32 targetCorridorExtent)
        {
            return InteriorRenderingSample::IsNormalizedInteriorPlacement(
                sourceBounds, placedBounds, targetCorridorExtent);
        }

        static bool ShouldConsumeOrbitInput(bool smoke,
                                            bool presentationReady,
                                            bool orbitInitialized,
                                            bool inputAvailable)
        {
            return InteriorRenderingSample::ShouldConsumeOrbitInput(
                smoke, presentationReady, orbitInitialized, inputAvailable);
        }

        static bool BuildInteriorOrbitSettings(
            const AABB& bounds,
            float32 aspectRatio,
            float32 framingDistance,
            SampleOrbitCameraSettings& outSettings)
        {
            return InteriorRenderingSample::BuildInteriorOrbitSettings(
                bounds, aspectRatio, framingDistance, outSettings);
        }

        static bool TryBuildInteriorStartInput(
            const OrbitCameraRigPose& fittedPose,
            const SampleOrbitCameraSettings& settings,
            SampleOrbitCameraInput& outInput)
        {
            return InteriorRenderingSample::TryBuildInteriorStartInput(
                fittedPose, settings, outInput);
        }
    };

    namespace
    {
        bool ContainsCode(const auto& values, std::string_view code)
        {
            return std::any_of(values.begin(), values.end(),
                               [code](const auto& value)
                               {
                                   return value.code.GetValue() == code;
                               });
        }

        const AssessmentCapability* FindCapability(
            const SampleAssessmentContract& contract,
            std::string_view code)
        {
            const auto found = std::find_if(
                contract.capabilities.begin(), contract.capabilities.end(),
                [code](const AssessmentCapability& capability)
                {
                    return capability.code.GetValue() == code;
                });
            return found == contract.capabilities.end() ? nullptr : &*found;
        }

        const AssessmentCapabilityObservation* FindCapabilityObservation(
            const std::vector<SampleAssessmentEvent>& events,
            std::string_view code)
        {
            for (const SampleAssessmentEvent& event : events)
            {
                const auto* observation =
                    std::get_if<AssessmentCapabilityObservation>(&event);
                if (observation && observation->capability.code.GetValue() == code)
                    return observation;
            }
            return nullptr;
        }

        const Finding* FindFinding(const std::vector<SampleAssessmentEvent>& events,
                                   std::string_view code)
        {
            for (const SampleAssessmentEvent& event : events)
            {
                const auto* finding = std::get_if<Finding>(&event);
                if (finding && finding->code.GetValue() == code)
                    return finding;
            }
            return nullptr;
        }

        std::optional<uint64> FindScenarioMetric(const SampleReport& report,
                                                  std::string_view name)
        {
            const auto found = std::find_if(
                report.scenario.metrics.begin(), report.scenario.metrics.end(),
                [name](const SampleReportScenarioMetric& metric)
                {
                    return metric.name == name;
                });
            return found == report.scenario.metrics.end()
                       ? std::nullopt
                       : std::optional<uint64>(found->value);
        }

        SampleRenderDiagnostics MakeObservedDiagnostics(uint64 frameSequence)
        {
            SampleRenderDiagnostics diagnostics;
            diagnostics.available = true;
            diagnostics.rendered = true;
            diagnostics.lastPresentedFrameSequence =
                DiagnosticValue<uint64>::Available(frameSequence);
            diagnostics.directionalShadowAvailable = true;
            diagnostics.directionalShadowRequested = true;
            diagnostics.directionalShadowSupported = true;
            diagnostics.transparentAvailable = true;
            diagnostics.transparentOrderValid = true;
            diagnostics.transparentOrderHash = 0xA11CEu;
            diagnostics.transparentCandidateDrawItemCount = 2;
            diagnostics.transparentPreparedDrawItemCount = 2;
            diagnostics.transparentExecutedPacketCount = 2;
            diagnostics.transparentExecutedDrawCount = 2;
            diagnostics.transparentMaterialBindingCount = 2;
            diagnostics.localLightingAvailable = true;
            diagnostics.pointLightRequestedCount = 8;
            diagnostics.pointLightAdmittedCount = 8;
            diagnostics.spotLightRequestedCount = 4;
            diagnostics.spotLightAdmittedCount = 4;
            diagnostics.pointShadowRequestedCount = 0;
            diagnostics.pointShadowSupported = false;
            diagnostics.spotShadowRequestedCount = 0;
            diagnostics.spotShadowSupported = false;
            diagnostics.hzbRequested = false;
            diagnostics.hzbSupported = false;
            diagnostics.hzbEnabled = false;
            return diagnostics;
        }

        SampleRenderDiagnostics MakeCoveredDirectDiagnostics()
        {
            SampleRenderDiagnostics diagnostics = MakeObservedDiagnostics(51u);
            diagnostics.renderSceneValuesAvailable = true;
            diagnostics.renderSceneAppliedRevision = 31u;
            diagnostics.renderSceneRequiredRevision = 31u;
            diagnostics.engineRenderRuntimeAvailable = true;
            diagnostics.engineRequiredSceneRevision = 31u;
            diagnostics.gpuDrivenPolicyDecisionAvailable = true;
            diagnostics.gpuDrivenRequestedMode = "ForceDisabled";
            diagnostics.gpuDrivenPolicyReason = "ForcedDisabled";
            diagnostics.gpuDrivenEnabled = false;
            diagnostics.gpuDrivenOpaqueIndirectRequested = false;
            diagnostics.gpuDrivenOpaqueIndirectEligible = false;
            diagnostics.gpuDrivenOpaqueIndirectSubmitted = false;
            diagnostics.gpuDrivenOpaqueIndirectBatchCount = 0;
            diagnostics.gpuDrivenOpaqueDirectDrawCount = 1;
            diagnostics.opaqueExecutionCompleted = true;
            diagnostics.opaqueExecutedDrawCountAvailable = true;
            diagnostics.opaqueExecutedDrawCount = 1;
            diagnostics.directionalShadowOutputReady = true;
            diagnostics.directionalShadowSamplingEnabled = true;
            diagnostics.directionalShadowRequestedCascadeCount = 3;
            diagnostics.directionalShadowProducedCascadeCount = 3;
            diagnostics.directionalShadowResolvedCascadeCount = 3;
            diagnostics.directionalShadowMapSize = 2048;
            diagnostics.directionalShadowCasterCount = 1;
            diagnostics.directionalShadowDrawCount = 1;
            diagnostics.textureIBLEnabled = true;
            diagnostics.clusteredLightingInitialized = true;
            diagnostics.clusteredLightingActiveClusters = 1;
            diagnostics.opaqueMaterialBindingsAvailable = true;
            return diagnostics;
        }

        SampleRenderDiagnostics MakeCoveredGPUDrivenDiagnostics()
        {
            SampleRenderDiagnostics diagnostics = MakeObservedDiagnostics(52u);
            diagnostics.renderSceneValuesAvailable = true;
            diagnostics.renderSceneAppliedRevision = 32u;
            diagnostics.renderSceneRequiredRevision = 32u;
            diagnostics.engineRenderRuntimeAvailable = true;
            diagnostics.engineRequiredSceneRevision = 32u;
            diagnostics.gpuDrivenPolicyDecisionAvailable = true;
            diagnostics.gpuDrivenRequestedMode = "ForceEnabled";
            diagnostics.gpuDrivenPolicyReason = "None";
            diagnostics.gpuDrivenEnabled = true;
            diagnostics.gpuDrivenGraphPassRecorded = true;
            diagnostics.gpuDrivenExecutionRecorded = true;
            diagnostics.gpuDrivenOpaqueIndirectRequested = true;
            diagnostics.gpuDrivenOpaqueIndirectEligible = true;
            diagnostics.gpuDrivenOpaqueIndirectSubmitted = true;
            diagnostics.gpuDrivenOpaqueIndirectBatchCount = 1;
            return diagnostics;
        }
    } // namespace

    TEST(InteriorRenderingSampleValidation, DeclaresTheP0AInteriorContract)
    {
        InteriorRenderingSample sample;
        const SampleInfo& info = sample.GetInfo();
        const SampleAssessmentContract contract = sample.GetAssessmentContract();

        EXPECT_EQ(info.id, "interior-rendering");
        EXPECT_EQ(info.defaultAssetId, "crytek-sponza");
        EXPECT_EQ(info.assetPolicy, SampleAssetPolicy::Fixed);
        EXPECT_EQ(info.defaultEnvironmentId,
                  "kloofendal-48d-partly-cloudy-pure-sky");
        EXPECT_EQ(info.environmentPolicy, SampleEnvironmentPolicy::Required);
        EXPECT_TRUE(info.supportsRenderPathSelection);
        ASSERT_EQ(info.additionalDefaultAssetIds.size(), 1u);
        EXPECT_EQ(info.additionalDefaultAssetIds.front(),
                  "interior-rendering-p0a");
        EXPECT_TRUE(contract.IsValid());
        EXPECT_EQ(contract.code.GetValue(), "SAMPLE.INTERIOR_RENDERING");
        EXPECT_EQ(contract.revision, "2");
        EXPECT_TRUE(ContainsCode(
            contract.actions, "RENDER.ACTION.INTERIOR_RENDER_PATH_SELECTED"));
        EXPECT_TRUE(ContainsCode(
            contract.actions, "RENDER.ACTION.INTERIOR_PRODUCT_FRAME_QUALIFIED"));
        EXPECT_TRUE(ContainsCode(
            contract.invariants, "RENDER.INTERIOR.DIRECTIONAL_CSM_EXACT"));
        EXPECT_TRUE(ContainsCode(
            contract.invariants, "RENDER.INTERIOR.TRANSPARENT_ORDER_STABLE"));
        EXPECT_TRUE(ContainsCode(
            contract.invariants, "RENDER.INTERIOR.LOCAL_LIGHT_ADMISSION_EXACT"));
        EXPECT_TRUE(ContainsCode(
            contract.invariants,
            "RENDER.INTERIOR.PRESENTED_SCENE_REVISION_CONVERGED"));
        EXPECT_TRUE(ContainsCode(
            contract.invariants,
            "RENDER.INTERIOR.RENDER_PATH_EXECUTION_EXACT"));
        EXPECT_TRUE(ContainsCode(
            contract.metrics, "RENDER.METRIC.INTERIOR_TRANSPARENT_ORDER_HASH"));
    }

    TEST(InteriorRenderingSampleValidation, DeclaresCapabilityGatingAccurately)
    {
        InteriorRenderingSample sample;
        const SampleAssessmentContract contract = sample.GetAssessmentContract();
        const AssessmentCapability* directional = FindCapability(
            contract, "RENDER.CAPABILITY.DIRECTIONAL_CSM");
        const AssessmentCapability* transparent = FindCapability(
            contract, "RENDER.CAPABILITY.TRANSPARENT_PASS");
        const AssessmentCapability* point = FindCapability(
            contract, "RENDER.CAPABILITY.POINT_SHADOW");
        const AssessmentCapability* spot = FindCapability(
            contract, "RENDER.CAPABILITY.SPOT_SHADOW");
        const AssessmentCapability* hzb = FindCapability(
            contract, "RENDER.CAPABILITY.HZB_OCCLUSION");

        ASSERT_NE(directional, nullptr);
        ASSERT_NE(transparent, nullptr);
        ASSERT_NE(point, nullptr);
        ASSERT_NE(spot, nullptr);
        ASSERT_NE(hzb, nullptr);
        EXPECT_TRUE(directional->gating);
        EXPECT_TRUE(transparent->gating);
        EXPECT_FALSE(point->gating);
        EXPECT_FALSE(spot->gating);
        EXPECT_FALSE(hzb->gating);
    }

    TEST(InteriorRenderingSampleValidation,
         DeclaresSchemaV12ReceiptsAndUnsupportedP0BoundariesInItsReport)
    {
        InteriorRenderingSample sample;
        SampleReport report;
        SampleFeatureReporter reporter(report);

        sample.AppendReport(reporter);

        EXPECT_EQ(report.scenario.contractRevision, 2u);
        EXPECT_EQ(report.scenario.phase, "AwaitingQualification");
        EXPECT_TRUE(report.scenario.actions.empty());
        ASSERT_EQ(report.scenario.invariants.size(), 3u);
        EXPECT_EQ(report.scenario.invariants[0].name,
                  "VerifiedProductionAssetComposition");
        EXPECT_EQ(report.scenario.invariants[1].name,
                  "DeterministicTransparentGlassPanels");
        EXPECT_EQ(report.scenario.invariants[2].name,
                  "CompletedInteriorRenderContract");
        ASSERT_EQ(report.scenario.metrics.size(), 4u);
        EXPECT_TRUE(report.unsupportedFeatures.empty());

        SampleAssessmentChannel assessment;
        sample.ObserveDiagnostics(MakeObservedDiagnostics(40u), assessment);
        SampleReport observedReport;
        SampleFeatureReporter observedReporter(observedReport);
        sample.AppendReport(observedReporter);
        EXPECT_NE(std::find(observedReport.unsupportedFeatures.begin(),
                             observedReport.unsupportedFeatures.end(),
                             "PointShadow"),
                  observedReport.unsupportedFeatures.end());
        EXPECT_NE(std::find(observedReport.unsupportedFeatures.begin(),
                             observedReport.unsupportedFeatures.end(),
                             "SpotShadow"),
                  observedReport.unsupportedFeatures.end());
        EXPECT_NE(std::find(observedReport.unsupportedFeatures.begin(),
                             observedReport.unsupportedFeatures.end(),
                             "HZBOcclusion"),
                  observedReport.unsupportedFeatures.end());
    }

    TEST(InteriorRenderingSampleValidation,
         ObservesCapabilitiesOnlyFromCompletedFrameDiagnostics)
    {
        InteriorRenderingSample sample;
        SampleAssessmentChannel assessment;
        SampleRenderDiagnostics diagnostics = MakeObservedDiagnostics(41u);

        sample.ObserveDiagnostics(diagnostics, assessment);
        const std::vector<SampleAssessmentEvent> events = assessment.Drain();
        const AssessmentCapabilityObservation* directional =
            FindCapabilityObservation(events, "RENDER.CAPABILITY.DIRECTIONAL_CSM");
        const AssessmentCapabilityObservation* transparent =
            FindCapabilityObservation(events, "RENDER.CAPABILITY.TRANSPARENT_PASS");
        const AssessmentCapabilityObservation* point =
            FindCapabilityObservation(events, "RENDER.CAPABILITY.POINT_SHADOW");
        const AssessmentCapabilityObservation* spot =
            FindCapabilityObservation(events, "RENDER.CAPABILITY.SPOT_SHADOW");
        const AssessmentCapabilityObservation* hzb =
            FindCapabilityObservation(events, "RENDER.CAPABILITY.HZB_OCCLUSION");

        ASSERT_NE(directional, nullptr);
        ASSERT_NE(transparent, nullptr);
        ASSERT_NE(point, nullptr);
        ASSERT_NE(spot, nullptr);
        ASSERT_NE(hzb, nullptr);
        EXPECT_TRUE(*directional->value.GetValue());
        EXPECT_TRUE(*transparent->value.GetValue());
        ASSERT_TRUE(point->value.IsAvailable());
        ASSERT_TRUE(spot->value.IsAvailable());
        ASSERT_TRUE(hzb->value.IsAvailable());
        EXPECT_FALSE(*point->value.GetValue());
        EXPECT_FALSE(*spot->value.GetValue());
        EXPECT_FALSE(*hzb->value.GetValue());

        diagnostics.lastPresentedFrameSequence =
            DiagnosticValue<uint64>::Available(42u);
        sample.ObserveDiagnostics(diagnostics, assessment);
        EXPECT_TRUE(assessment.Drain().empty());
    }

    TEST(InteriorRenderingSampleValidation,
          DefersDirectionalCapabilityUntilItsDiagnosticsAreAvailable)
    {
        InteriorRenderingSample sample;
        SampleAssessmentChannel assessment;
        SampleRenderDiagnostics diagnostics = MakeObservedDiagnostics(7u);
        diagnostics.directionalShadowAvailable = false;
        diagnostics.directionalShadowSupported = false;

        sample.ObserveDiagnostics(diagnostics, assessment);
        const std::vector<SampleAssessmentEvent> blankFrameEvents =
            assessment.Drain();
        EXPECT_EQ(FindCapabilityObservation(
                      blankFrameEvents,
                      "RENDER.CAPABILITY.DIRECTIONAL_CSM"),
                  nullptr);
        const AssessmentCapabilityObservation* point =
            FindCapabilityObservation(blankFrameEvents,
                                      "RENDER.CAPABILITY.POINT_SHADOW");

        diagnostics.lastPresentedFrameSequence =
            DiagnosticValue<uint64>::Available(8u);
        diagnostics.directionalShadowAvailable = true;
        diagnostics.directionalShadowSupported = true;
        sample.ObserveDiagnostics(diagnostics, assessment);
        const std::vector<SampleAssessmentEvent> events = assessment.Drain();
        const AssessmentCapabilityObservation* directional =
            FindCapabilityObservation(events, "RENDER.CAPABILITY.DIRECTIONAL_CSM");

        ASSERT_NE(directional, nullptr);
        ASSERT_NE(point, nullptr);
        ASSERT_TRUE(directional->value.IsAvailable());
        EXPECT_TRUE(*directional->value.GetValue());
        ASSERT_TRUE(point->value.IsAvailable());
        EXPECT_FALSE(*point->value.GetValue());
    }

    TEST(InteriorRenderingSampleValidation,
         RejectsQualifiedStateWhenCompletedFrameHasNotCoveredRequiredSceneRevision)
    {
        InteriorRenderingSample sample;
        InteriorRenderingSampleValidationAccess::ConfigureQualifiedState(
            sample, SampleRenderPath::Direct);
        SampleRenderDiagnostics diagnostics = MakeCoveredDirectDiagnostics();
        diagnostics.renderSceneRequiredRevision =
            diagnostics.renderSceneAppliedRevision + 1u;

        const SampleReadiness stale = sample.GetReadiness(diagnostics);
        EXPECT_FALSE(stale.IsReady());
        EXPECT_FALSE(stale.IsFailed());
        EXPECT_NE(stale.reason.find("required Scene revision"),
                  std::string::npos);

        diagnostics.renderSceneRequiredRevision =
            diagnostics.renderSceneAppliedRevision;
        EXPECT_TRUE(sample.GetReadiness(diagnostics).IsReady());

        SampleReport report;
        SampleFeatureReporter reporter(report);
        sample.AppendReport(reporter);
        EXPECT_EQ(report.scenario.phase, "stable");
    }

    TEST(InteriorRenderingSampleValidation,
         RejectsDirectAndGPUDrivenPolicyExecutionMismatches)
    {
        InteriorRenderingSample directSample;
        InteriorRenderingSampleValidationAccess::ConfigureQualifiedState(
            directSample, SampleRenderPath::Direct);
        EXPECT_FALSE(
            directSample.GetReadiness(MakeCoveredGPUDrivenDiagnostics()).IsReady());
        EXPECT_TRUE(
            directSample.GetReadiness(MakeCoveredDirectDiagnostics()).IsReady());

        InteriorRenderingSample gpuDrivenSample;
        InteriorRenderingSampleValidationAccess::ConfigureQualifiedState(
            gpuDrivenSample, SampleRenderPath::GPUDriven);
        EXPECT_FALSE(
            gpuDrivenSample.GetReadiness(MakeCoveredDirectDiagnostics()).IsReady());
        EXPECT_TRUE(gpuDrivenSample.GetReadiness(
                        MakeCoveredGPUDrivenDiagnostics())
                        .IsReady());
    }

    TEST(InteriorRenderingSampleValidation,
         DoesNotPublishQualificationActionBeforeRequiredRevisionIsPresented)
    {
        InteriorRenderingSample sample;
        InteriorRenderingSampleValidationAccess::ConfigureResidentState(
            sample, SampleRenderPath::Direct);
        SampleAssessmentChannel assessment;
        SampleRenderDiagnostics diagnostics = MakeCoveredDirectDiagnostics();
        diagnostics.renderSceneRequiredRevision =
            diagnostics.renderSceneAppliedRevision + 1u;

        sample.ObserveDiagnostics(diagnostics, assessment);
        diagnostics.lastPresentedFrameSequence =
            DiagnosticValue<uint64>::Available(53u);
        sample.ObserveDiagnostics(diagnostics, assessment);

        SampleReport report;
        SampleFeatureReporter reporter(report);
        sample.AppendReport(reporter);
        EXPECT_TRUE(report.scenario.actions.empty());
        EXPECT_FALSE(report.scenario.invariants.back().passed);
        EXPECT_TRUE(sample.GetReadiness(diagnostics).IsFailed());
    }

    TEST(InteriorRenderingSampleValidation,
         FailsClosedWhenAnExplicitlyUnsupportedCapabilityReportsSupported)
    {
        const auto assertMismatch = [](const auto configure,
                                       std::string_view capabilityCode,
                                       std::string_view findingCode)
        {
            InteriorRenderingSample sample;
            SampleAssessmentChannel assessment;
            SampleRenderDiagnostics diagnostics = MakeObservedDiagnostics(61u);
            configure(diagnostics);

            sample.ObserveDiagnostics(diagnostics, assessment);
            const std::vector<SampleAssessmentEvent> events = assessment.Drain();
            const AssessmentCapabilityObservation* capability =
                FindCapabilityObservation(events, capabilityCode);
            const Finding* finding = FindFinding(events, findingCode);
            ASSERT_NE(capability, nullptr);
            ASSERT_TRUE(capability->value.IsAvailable());
            EXPECT_TRUE(*capability->value.GetValue());
            ASSERT_NE(finding, nullptr);
            EXPECT_TRUE(finding->gating);
            EXPECT_TRUE(sample.GetReadiness(diagnostics).IsFailed());
        };

        assertMismatch(
            [](SampleRenderDiagnostics& diagnostics)
            {
                diagnostics.pointShadowSupported = true;
            },
            "RENDER.CAPABILITY.POINT_SHADOW",
            "RENDER.INTERIOR.POINT_SHADOW_SUPPORTED_MISMATCH");
        assertMismatch(
            [](SampleRenderDiagnostics& diagnostics)
            {
                diagnostics.spotShadowSupported = true;
            },
            "RENDER.CAPABILITY.SPOT_SHADOW",
            "RENDER.INTERIOR.SPOT_SHADOW_SUPPORTED_MISMATCH");
        assertMismatch(
            [](SampleRenderDiagnostics& diagnostics)
            {
                diagnostics.hzbSupported = true;
            },
            "RENDER.CAPABILITY.HZB_OCCLUSION",
            "RENDER.INTERIOR.HZB_SUPPORTED_MISMATCH");
    }

    TEST(InteriorRenderingSampleValidation,
         SponzaPresentationRequiresNormalizedCenteredGroundedWorldBounds)
    {
        const AABB source(Vec3(-19.20f, -1.26f, -11.83f),
                          Vec3(18.00f, 14.29f, 11.05f));
        const AABB placed(Vec3(-7.72f, 0.0f, -12.50f),
                          Vec3(7.72f, 10.50f, 12.50f));
        EXPECT_TRUE(InteriorRenderingSampleValidationAccess::
                        IsNormalizedInteriorPlacement(source, placed, 25.0f));
        EXPECT_FALSE(InteriorRenderingSampleValidationAccess::
                         IsNormalizedInteriorPlacement(
                             source,
                             AABB(Vec3(-7.72f, 0.20f, -12.50f),
                                  Vec3(7.72f, 10.70f, 12.50f)),
                             25.0f));
        EXPECT_FALSE(InteriorRenderingSampleValidationAccess::
                         IsNormalizedInteriorPlacement(
                             source,
                             AABB(Vec3(-7.72f, 0.0f, -11.50f),
                                  Vec3(7.72f, 10.50f, 13.50f)),
                             25.0f));
    }

    TEST(InteriorRenderingSampleValidation,
         SmokeQualificationNeverConsumesInteriorOrbitInput)
    {
        EXPECT_FALSE(InteriorRenderingSampleValidationAccess::
                         ShouldConsumeOrbitInput(true, true, true, true));
        EXPECT_FALSE(InteriorRenderingSampleValidationAccess::
                         ShouldConsumeOrbitInput(false, false, true, true));
        EXPECT_FALSE(InteriorRenderingSampleValidationAccess::
                         ShouldConsumeOrbitInput(false, true, false, true));
        EXPECT_FALSE(InteriorRenderingSampleValidationAccess::
                         ShouldConsumeOrbitInput(false, true, true, false));
        EXPECT_TRUE(InteriorRenderingSampleValidationAccess::
                        ShouldConsumeOrbitInput(false, true, true, true));
    }

    TEST(InteriorRenderingSampleValidation,
         AuthoredInteriorOrbitStartAndResetAnchorUseTheIndoorComposition)
    {
        SampleOrbitCameraSettings settings;
        ASSERT_TRUE(InteriorRenderingSampleValidationAccess::
                        BuildInteriorOrbitSettings(
                            AABB(Vec3(-7.72f, 0.0f, -12.50f),
                                 Vec3(7.72f, 10.50f, 12.50f)),
                            16.0f / 9.0f,
                            24.0f,
                            settings));
        EXPECT_FLOAT_EQ(8.05f, settings.distance);
        EXPECT_LT(settings.minDistance, settings.distance);
        EXPECT_FLOAT_EQ(3.75f, settings.minDistance);
        EXPECT_FLOAT_EQ(radians(75.0f), settings.verticalFovRadians);

        SampleOrbitCameraInput invalidStartInput;
        EXPECT_FALSE(InteriorRenderingSampleValidationAccess::
                         TryBuildInteriorStartInput({},
                                                    settings,
                                                    invalidStartInput));

        OrbitCameraRig rig;
        ASSERT_TRUE(rig.Initialize(settings));
        ASSERT_TRUE(rig.Fit());
        const OrbitCameraRigPose fittedPose = rig.GetPose();
        ASSERT_TRUE(fittedPose.valid);

        SampleOrbitCameraInput startInput;
        ASSERT_TRUE(InteriorRenderingSampleValidationAccess::
                        TryBuildInteriorStartInput(fittedPose,
                                                   rig.GetSettings(),
                                                   startInput));
        rig.ApplyIntent({.zoomDelta = startInput.scrollDelta});
        const OrbitCameraRigPose authoredPose = rig.GetPose();
        ASSERT_TRUE(authoredPose.valid);
        EXPECT_NEAR(8.05f, authoredPose.distance, 0.001f);
        // Verify the composed world pose directly. AABB extrema do not encode
        // the closed Sponza walls or the usable interior viewing corridor.
        EXPECT_GT(authoredPose.position.z, 2.5f);
        EXPECT_LT(authoredPose.position.z, 3.8f);

        rig.CaptureResetAnchor();
        rig.ApplyIntent({.zoomDelta = -5.0f});
        EXPECT_GT(rig.GetPose().distance, 8.05f);
        ASSERT_TRUE(rig.Reset());
        EXPECT_NEAR(8.05f, rig.GetPose().distance, 0.001f);
    }

    TEST(InteriorRenderingSampleValidation,
         RetainsSceneQualifiedEcsModelAndEnvironmentReadinessValues)
    {
        InteriorRenderingSample sample;
        InteriorRenderingSampleValidationAccess::ConfigureResidentState(
            sample, SampleRenderPath::Direct);

        const LoadedSampleModel& model =
            InteriorRenderingSampleValidationAccess::Model(sample);
        const LoadedSampleEnvironment& environment =
            InteriorRenderingSampleValidationAccess::Environment(sample);
        ASSERT_TRUE(model.request.IsValid());
        EXPECT_TRUE(model.IsCPUReady());
        EXPECT_TRUE(model.IsFullyResident());
        EXPECT_EQ(model.GetRootEntityRef().sceneRuntimeId,
                  model.request.sceneRuntimeId);
        EXPECT_EQ(model.GetRootEntityRef().entity, model.status.rootEntity);
        ASSERT_TRUE(model.status.modelMetadata.HasPublishedSource());
        EXPECT_EQ(model.status.modelMetadata.sourceNodeCount, 4u);
        EXPECT_EQ(model.status.modelMetadata.meshAssetIds.size(), 2u);
        EXPECT_EQ(model.status.modelMetadata.materials.size(), 2u);

        ASSERT_TRUE(environment.request.IsValid());
        EXPECT_EQ(environment.status.sceneRuntimeId,
                  environment.request.sceneRuntimeId);
        EXPECT_TRUE(environment.IsCPUReady());
        EXPECT_TRUE(environment.IsValid());
    }

    TEST(InteriorRenderingSampleValidation,
         CapturesProductionCompositionFromValueOnlyEcsMaterialMetadata)
    {
        InteriorRenderingSample sample;
        InteriorRenderingSampleValidationAccess::ConfigureResidentState(
            sample, SampleRenderPath::Direct);
        std::string error;

        ASSERT_TRUE(
            InteriorRenderingSampleValidationAccess::CaptureProductionAssetComposition(
                sample, error));
        EXPECT_TRUE(error.empty());

        SampleReport report;
        SampleFeatureReporter reporter(report);
        sample.AppendReport(reporter);
        EXPECT_EQ(FindScenarioMetric(report, "productionMeshCount"), 2u);
        EXPECT_EQ(FindScenarioMetric(report, "productionMaterialCount"), 2u);
        EXPECT_EQ(FindScenarioMetric(report, "productionTextureCount"), 3u);
        EXPECT_NE(std::find(report.enabledFeatures.begin(),
                            report.enabledFeatures.end(),
                            "ModelResourceLoad"),
                  report.enabledFeatures.end());
        EXPECT_NE(std::find(report.enabledFeatures.begin(),
                            report.enabledFeatures.end(),
                            "EnvironmentResourceLoad"),
                  report.enabledFeatures.end());
    }

    TEST(InteriorRenderingSampleValidation,
         SourceNodeMappingsRetainTransparentMaterialSlotOverridesInEcs)
    {
        static_assert(std::is_trivially_copyable_v<SceneECS::Mesh>);
        static_assert(std::is_trivially_copyable_v<SceneECS::MaterialSlots>);
        static_assert(std::is_trivially_copyable_v<SceneECS::Visibility>);

        SceneECS::SceneEcsRuntime runtime;
        SampleSceneLifetimeScope lifetime(runtime);
        ASSERT_TRUE(lifetime.ReserveOwnershipCapacity(2u));

        SceneECS::MaterialSlots opaqueSlots;
        opaqueSlots.values[0] = {
            .materialAssetId = {700u},
            .materialMode = RenderMaterialMode::Opaque};
        opaqueSlots.count = 1u;
        const SceneECS::Mesh panelMesh{.meshAssetId = {600u}, .submeshCount = 1u};
        const SceneECS::Visibility hiddenPanel{
            .visible = false, .castsShadow = true, .receivesShadow = true};

        auto transaction = runtime.BeginSpawnTransaction();
        const SceneECS::SceneSpawnEntityId leftPending = transaction.Create();
        const SceneECS::SceneSpawnEntityId rightPending = transaction.Create();
        ASSERT_TRUE(leftPending.IsValid());
        ASSERT_TRUE(rightPending.IsValid());
        ASSERT_TRUE(transaction.Add<SceneECS::Mesh>(leftPending, panelMesh));
        ASSERT_TRUE(transaction.Add<SceneECS::MaterialSlots>(leftPending, opaqueSlots));
        ASSERT_TRUE(transaction.Add<SceneECS::Visibility>(leftPending, hiddenPanel));
        ASSERT_TRUE(transaction.Add<SceneECS::Mesh>(rightPending, panelMesh));
        ASSERT_TRUE(transaction.Add<SceneECS::MaterialSlots>(rightPending, opaqueSlots));
        ASSERT_TRUE(transaction.Add<SceneECS::Visibility>(rightPending, hiddenPanel));

        const SceneECS::SceneSpawnCommitResult result = transaction.Commit();
        ASSERT_TRUE(result.IsApplied());
        const std::array<SceneECS::SceneEntityRef, 2u> panels{
            result.GetEntityRef(leftPending), result.GetEntityRef(rightPending)};
        ASSERT_TRUE(panels[0].IsValid());
        ASSERT_TRUE(panels[1].IsValid());
        ASSERT_TRUE(lifetime.AdoptBatch(std::span(panels)));

        const std::array<ResourceSceneAdapters::PreparedModelEntityMapping, 2u>
            mappings{{
                {.temporaryNodeId = 1u,
                 .entity = panels[0].entity,
                 .sourceName = "Transparent_Portal_Left",
                 .sourceNodeIdentity = {.value = 1001u},
                 .nodeName = "Transparent_Portal_Left#0",
                 .derivedPrimitiveOrdinal = 0u},
                {.temporaryNodeId = 2u,
                 .entity = panels[1].entity,
                 .sourceName = "Transparent_Portal_Right",
                 .sourceNodeIdentity = {.value = 1002u},
                 .nodeName = "Transparent_Portal_Right#0",
                 .derivedPrimitiveOrdinal = 0u}}};
        InteriorRenderingSample sample;
        LoadedSampleModel& glassPanelTemplate =
            InteriorRenderingSampleValidationAccess::GlassPanelTemplate(sample);
        glassPanelTemplate =
            InteriorRenderingSampleValidationAccess::MakeFullyResidentModel(3u, 800u);
        glassPanelTemplate.request.sceneRuntimeId = runtime.GetSceneRuntimeId();
        glassPanelTemplate.status.sceneRuntimeId = runtime.GetSceneRuntimeId();
        glassPanelTemplate.status.rootEntity = panels[0].entity;
        glassPanelTemplate.status.members = {panels[0].entity, panels[1].entity};
        glassPanelTemplate.status.entityMappings.assign(mappings.begin(), mappings.end());
        glassPanelTemplate.status.modelMetadata.materialAssetIds = {{701u}, {702u}};
        glassPanelTemplate.status.modelMetadata.materials = {
            {{701u},
             "Transparent_GlassA",
             Resource::MaterialWorkflowMode::MetallicRoughness,
             Resource::MaterialAlphaMode::Blend},
            {{702u},
             "Transparent_GlassB",
             Resource::MaterialWorkflowMode::MetallicRoughness,
             Resource::MaterialAlphaMode::Blend}};
        ASSERT_TRUE(glassPanelTemplate.IsFullyResident());
        ASSERT_EQ(glassPanelTemplate.status.entityMappings.size(), 2u);
        ASSERT_EQ(glassPanelTemplate.status.modelMetadata.materials.size(), 2u);
        EXPECT_EQ(glassPanelTemplate.status.modelMetadata.materials[0].alphaMode,
                  Resource::MaterialAlphaMode::Blend);
        EXPECT_EQ(glassPanelTemplate.status.modelMetadata.materials[1].alphaMode,
                  Resource::MaterialAlphaMode::Blend);
        for (const ResourceSceneAdapters::PreparedModelEntityMapping& mapping : mappings)
        {
            EXPECT_TRUE(mapping.sourceNodeIdentity.IsValid());
            EXPECT_TRUE(mapping.entity.IsValid());
            EXPECT_TRUE(runtime.GetEntityRef(mapping.entity).IsValid());
            EXPECT_EQ(runtime.GetEntityRef(mapping.entity).sceneRuntimeId,
                      runtime.GetSceneRuntimeId());
        }

        constexpr AssetId GlassMaterialIds[]{{701u}, {702u}};
        for (size_t index = 0; index < panels.size(); ++index)
        {
            const SceneECS::MaterialSlots* currentSlots =
                runtime.GetRegistry().TryGet<SceneECS::MaterialSlots>(panels[index].entity);
            const SceneECS::Visibility* currentVisibility =
                runtime.GetRegistry().TryGet<SceneECS::Visibility>(panels[index].entity);
            ASSERT_NE(currentSlots, nullptr);
            ASSERT_NE(currentVisibility, nullptr);

            SceneECS::MaterialSlots transparentSlots = *currentSlots;
            transparentSlots.values[0] = {
                .materialAssetId = GlassMaterialIds[index],
                .materialMode = RenderMaterialMode::Transparent};
            SceneECS::Visibility visiblePanel = *currentVisibility;
            visiblePanel.visible = true;
            visiblePanel.castsShadow = false;
            ASSERT_TRUE(runtime.SetFragment(panels[index].entity, transparentSlots));
            ASSERT_TRUE(runtime.SetFragment(panels[index].entity, visiblePanel));

            const SceneECS::MaterialSlots* updatedSlots =
                runtime.GetRegistry().TryGet<SceneECS::MaterialSlots>(panels[index].entity);
            const SceneECS::Visibility* updatedVisibility =
                runtime.GetRegistry().TryGet<SceneECS::Visibility>(panels[index].entity);
            ASSERT_NE(updatedSlots, nullptr);
            ASSERT_NE(updatedVisibility, nullptr);
            EXPECT_EQ(updatedSlots->values[0].materialAssetId,
                      GlassMaterialIds[index]);
            EXPECT_EQ(updatedSlots->values[0].materialMode,
                      RenderMaterialMode::Transparent);
            EXPECT_TRUE(updatedVisibility->visible);
            EXPECT_FALSE(updatedVisibility->castsShadow);
        }
    }

    TEST(InteriorRenderingSampleValidation,
         PureEcsLightCensusFeedsTheReportWithoutLocalShadowFallbacks)
    {
        SceneECS::SceneEcsRuntime runtime;
        SampleSceneLifetimeScope lifetime(runtime);
        ASSERT_TRUE(lifetime.ReserveOwnershipCapacity(13u));

        auto transaction = runtime.BeginSpawnTransaction();
        std::vector<SceneECS::SceneSpawnEntityId> pendingLights;
        pendingLights.reserve(13u);
        const auto addLight = [&transaction, &pendingLights](SceneECS::Light light)
        {
            const SceneECS::SceneSpawnEntityId pending = transaction.Create();
            if (!pending.IsValid() || !transaction.Add<SceneECS::Light>(pending, light))
                return false;
            pendingLights.push_back(pending);
            return true;
        };
        ASSERT_TRUE(addLight({.type = SceneECS::LightType::Directional,
                              .intensity = 3.5f,
                              .castsShadows = true}));
        for (uint32 index = 0; index < 8u; ++index)
        {
            ASSERT_TRUE(addLight({.type = SceneECS::LightType::Point,
                                  .intensity = 10.0f + index,
                                  .range = 5.5f,
                                  .castsShadows = false}));
        }
        for (uint32 index = 0; index < 4u; ++index)
        {
            ASSERT_TRUE(addLight({.type = SceneECS::LightType::Spot,
                                  .intensity = 18.0f + index,
                                  .range = 12.0f,
                                  .castsShadows = false}));
        }
        const SceneECS::SceneSpawnCommitResult result = transaction.Commit();
        ASSERT_TRUE(result.IsApplied());

        std::vector<SceneECS::SceneEntityRef> lights;
        lights.reserve(pendingLights.size());
        for (const SceneECS::SceneSpawnEntityId pending : pendingLights)
        {
            const SceneECS::SceneEntityRef light = result.GetEntityRef(pending);
            ASSERT_TRUE(light.IsValid());
            EXPECT_EQ(light.sceneRuntimeId, runtime.GetSceneRuntimeId());
            lights.push_back(light);
        }
        ASSERT_TRUE(lifetime.AdoptBatch(std::span(lights)));

        uint32 directionalCount = 0;
        uint32 pointCount = 0;
        uint32 spotCount = 0;
        for (const SceneECS::SceneEntityRef entity : lights)
        {
            const SceneECS::Light* light =
                runtime.GetRegistry().TryGet<SceneECS::Light>(entity.entity);
            ASSERT_NE(light, nullptr);
            switch (light->type)
            {
                case SceneECS::LightType::Directional:
                    ++directionalCount;
                    EXPECT_TRUE(light->castsShadows);
                    break;
                case SceneECS::LightType::Point:
                    ++pointCount;
                    EXPECT_FALSE(light->castsShadows);
                    break;
                case SceneECS::LightType::Spot:
                    ++spotCount;
                    EXPECT_FALSE(light->castsShadows);
                    break;
            }
        }
        EXPECT_EQ(directionalCount, 1u);
        EXPECT_EQ(pointCount, 8u);
        EXPECT_EQ(spotCount, 4u);

        InteriorRenderingSample sample;
        InteriorRenderingSampleValidationAccess::SetLightCensus(
            sample, pointCount, spotCount);
        SampleReport report;
        SampleFeatureReporter reporter(report);
        sample.AppendReport(reporter);
        EXPECT_NE(std::find(report.enabledFeatures.begin(),
                            report.enabledFeatures.end(),
                            "DirectionalLightScene"),
                  report.enabledFeatures.end());
        EXPECT_NE(std::find(report.resourceDiagnostics.begin(),
                            report.resourceDiagnostics.end(),
                            "point lights requested=8"),
                  report.resourceDiagnostics.end());
        EXPECT_NE(std::find(report.resourceDiagnostics.begin(),
                            report.resourceDiagnostics.end(),
                            "spot lights requested=4"),
                  report.resourceDiagnostics.end());
        EXPECT_NE(std::find(report.resourceDiagnostics.begin(),
                            report.resourceDiagnostics.end(),
                            "point local-shadow requests=0"),
                  report.resourceDiagnostics.end());
        EXPECT_NE(std::find(report.resourceDiagnostics.begin(),
                            report.resourceDiagnostics.end(),
                            "spot local-shadow requests=0"),
                  report.resourceDiagnostics.end());
    }
} // namespace RVX
