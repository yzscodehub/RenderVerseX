#pragma once

/**
 * @file SceneRenderer.h
 * @brief Scene renderer - orchestrates render pass execution
 */

#include "Render/Renderer/ViewData.h"
#include "Render/Renderer/RenderScene.h"
#include "Render/Graph/RenderGraph.h"
#include "Render/Graph/TransientResourcePool.h"
#include "Render/Graph/ResourceViewCache.h"
#include "Render/Context/RenderContext.h"
#include "Render/GPUDriven/GPUCulling.h"
#include "Render/GPUDriven/GPUDrivenDiagnostics.h"
#include "Render/GPUDriven/GPUDrivenPolicy.h"
#include "Render/Resources/RenderResourceTypes.h"
#include "Render/Material/MaterialSystem.h"
#include "Render/Passes/IRenderPass.h"
#include "Render/Passes/MeshPassProcessor.h"
#include "Render/Policy/RenderFramePlanCompiler.h"
#include "Render/Policy/RenderPolicyDiagnostics.h"
#include "Render/Passes/CameraVelocityPass.h"
#include "Render/Passes/ObjectVelocityPass.h"
#include "Render/Passes/ParticleFeaturePass.h"
#include "Render/Passes/RayTracedReflectionCompositePass.h"
#include "Render/Passes/RayTracedReflectionDenoisePass.h"
#include "Render/Passes/RayTracedReflectionPass.h"
#include "Render/Passes/RayTracedShadowPass.h"
#include "Render/Passes/ShadowPass.h"
#include "Render/PipelineCache.h"
#include "Render/PostProcess/PostProcessStack.h"
#include "Render/RayTracing/RayTracingSceneManager.h"
#include "Render/Renderer/RenderDrawItem.h"
#include "RenderContracts/FeatureRenderSnapshot.h"
#include "RenderContracts/RenderProxy.h"
#include "RHI/RHICapabilities.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace RVX
{
    inline constexpr RHIFormat RVX_SCENE_COLOR_HDR_FORMAT = RHIFormat::RGBA16_FLOAT;

    class BloomPass;
    class CameraVelocityPass;
    class ClusteredLighting;
    class ObjectVelocityPass;
    class ChromaticAberrationPass;
    class ColorGradingPass;
    class FXAAPass;
    class FilmGrainPass;
    class SSAOPass;
    class LightManager;
    class VignettePass;
    class IRenderPass;
    class DepthPrepass;
    class OpaquePass;
    class ParticleFeaturePass;
    class RenderPassRegistry;
    class RenderRetirementQueue;
    class RenderResourceRegistry;
    class RenderSubmissionResourceBatch;
    class RayTracedReflectionCompositePass;
    class RayTracedReflectionDenoisePass;
    class RayTracedReflectionPass;
    class RayTracedShadowPass;
    class ShadowPass;
    class SkyboxPass;
    class ToneMappingPass;
    class TransparentPass;
    struct GPUCompletionToken;

    enum class SceneRenderCollectionPath : uint8
    {
        None = 0,
        Proxy,
        SceneManagerDirect,
        ProxyRejected
    };

    struct SceneRenderCollectionStats
    {
        SceneRenderCollectionPath lastPath = SceneRenderCollectionPath::None;
        uint64 proxyFrameCount = 0;
        uint64 legacyFallbackFrameCount = 0;
        uint64 sceneManagerDirectFrameCount = 0;
        uint64 rejectedProxyFrameCount = 0;
        size_t lastProxyPrimitiveCount = 0;
        size_t lastProxyLightCount = 0;
        uint64 lastFallbackOwnerId = 0;
        std::string lastFallbackReason;
        bool lastFallbackSuppressed = false;
    };

    struct SceneFeatureExtractionStats
    {
        bool attempted = false;
        bool usedProviderPath = false;
        bool requiresLegacyFallback = false;
        uint32 snapshotSchemaVersion = RVX_RENDER_FEATURE_SNAPSHOT_SCHEMA_VERSION;
        uint64 snapshotSequence = 0;
        bool snapshotComplete = false;
        size_t providerCount = 0;
        size_t skippedProviderCount = 0;
        size_t particleItemCount = 0;
        size_t particleMetadataOnlyCount = 0;
        size_t particleRenderPayloadReadyCount = 0;
        size_t particleSortingSupportedCount = 0;
        size_t waterItemCount = 0;
        size_t terrainItemCount = 0;
        uint64 fallbackOwnerId = 0;
        std::string fallbackReason;
    };

    struct SceneRenderPassChainStats
    {
        uint64 frameCount = 0;
        size_t registeredPassCount = 0;
        size_t graphPassCount = 0;
        size_t skippedDisabledPassCount = 0;
        size_t skippedUnsupportedPassCount = 0;
        std::vector<RenderPassStatus> passStatuses;
    };

    struct SceneColorFormatPolicy
    {
        RHIFormat requestedSceneColorFormat = RVX_SCENE_COLOR_HDR_FORMAT;
        RHIFormat actualSceneColorFormat = RHIFormat::Unknown;
        RHIFormat backBufferFormat = RHIFormat::Unknown;
        RHIFormat toneMappingOutputFormat = RHIFormat::Unknown;
        ToneMappingOutputColorSpace toneMappingOutputColorSpace = ToneMappingOutputColorSpace::SRGB;
        bool hdrSceneColorEnabled = false;
        std::string hdrFallbackReason;
    };

    struct SceneRenderPostProcessStats
    {
        uint64 frameCount = 0;
        bool sceneColorStagingUsed = false;
        bool directToBackBuffer = true;
        uint32 sceneColorWidth = 0;
        uint32 sceneColorHeight = 0;
        RHIFormat sceneColorFormat = RHIFormat::Unknown;
        RHIFormat requestedSceneColorFormat = RVX_SCENE_COLOR_HDR_FORMAT;
        RHIFormat actualSceneColorFormat = RHIFormat::Unknown;
        RHIFormat backBufferFormat = RHIFormat::Unknown;
        RHIFormat toneMappingOutputFormat = RHIFormat::Unknown;
        ToneMappingOutputColorSpace toneMappingOutputColorSpace = ToneMappingOutputColorSpace::SRGB;
        bool hdrSceneColorEnabled = false;
        std::string hdrFallbackReason;
        bool frameInputDepthAvailable = false;
        bool frameInputVelocityAvailable = false;
        bool frameInputTemporalHistoryAvailable = false;
        PostProcessStackExecuteStats stackStats;
    };

    struct SceneRendererExternalTargetDesc
    {
        RHITexture* colorTarget = nullptr;
        RHITexture* depthTarget = nullptr;
        RHIResourceState colorInitialState = RHIResourceState::ShaderResource;
        RHIResourceState colorFinalState = RHIResourceState::ShaderResource;
        RHIResourceState depthInitialState = RHIResourceState::DepthWrite;
        RHIResourceState depthFinalState = RHIResourceState::DepthWrite;

        bool IsValid() const;
    };

    struct SceneRendererExternalTargetStats
    {
        bool requested = false;
        bool active = false;
        bool importedColor = false;
        bool importedDepth = false;
        uint32 width = 0;
        uint32 height = 0;
        RHIFormat colorFormat = RHIFormat::Unknown;
        RHIFormat depthFormat = RHIFormat::Unknown;
        RHIResourceState colorInitialState = RHIResourceState::Undefined;
        RHIResourceState colorFinalState = RHIResourceState::Undefined;
        RHIResourceState depthInitialState = RHIResourceState::Undefined;
        RHIResourceState depthFinalState = RHIResourceState::Undefined;
        std::string fallbackReason;
    };

    struct SceneGPUDrivenCullingStats
    {
        bool policyDecisionAvailable = false;
        GPUDrivenPolicyDecision policyDecision;
        bool enabled = false;
        bool fallbackUsed = false;
        bool graphPassAdded = false;
        bool graphPassRecorded = false;
        bool gpuExecutionRecorded = false;
        bool executionDecisionAvailable = false;
        GPUCullingExecutionDecision executionDecision;
        uint32 inputOpaqueDrawItemCount = 0;
        uint32 inputMaskedDrawItemCount = 0;
        uint32 outputOpaqueDrawItemCount = 0;
        uint32 outputMaskedDrawItemCount = 0;
        uint32 cullableOpaqueDrawItemCount = 0;
        uint32 cullableMaskedDrawItemCount = 0;
        uint32 graphInputDrawItemCount = 0;
        MeshPassProcessorStats opaqueMeshPassProcessorStats;
        uint32 skippedMissingGpuDataCount = 0;
        uint32 visibleCullableDrawItemCount = 0;
        uint32 frustumCulledDrawItemCount = 0;
        uint32 distanceCulledDrawItemCount = 0;
        bool opaqueIndirectRequested = false;
        bool opaqueCullingReady = false;
        bool opaquePipelineReady = false;
        bool opaqueIndirectEligible = false;
        bool opaqueIndirectSubmitted = false;
        uint32 opaqueDirectDrawCount = 0;
        uint32 opaqueGpuDrivenIndirectBatchCount = 0;
        uint32 opaqueGpuDrivenIndirectDrawCount = 0;
        GPUDrivenDrawFallbackReason opaqueFallbackReason =
            GPUDrivenDrawFallbackReason::Disabled;
    };

    inline constexpr uint32 RVX_SCENE_RENDER_FEATURE_REPORT_SCHEMA_VERSION = 2;
    inline constexpr uint32 RVX_SCENE_RENDERER_FRAME_DIAGNOSTICS_SCHEMA_VERSION = 5;
    inline constexpr uint32 RVX_SCENE_RENDERER_TOOL_DIAGNOSTICS_SCHEMA_VERSION = 26;
    inline constexpr uint32 RVX_SCENE_RENDERER_TOOL_ARTIFACT_SUMMARY_SCHEMA_VERSION = 24;
    inline constexpr uint32 RVX_SCENE_RENDERER_TOOL_ARTIFACT_VALIDATION_SCHEMA_VERSION = 25;

    enum class SceneRenderFeature : uint8
    {
        PBR = 0,
        Shadows,
        IBL,
        PostProcess,
        GPUDriven,
        Instancing,
        RayTracing,
    };

    enum class SceneRenderFeatureStatus : uint8
    {
        Unknown = 0,
        Supported,
        Fallback,
        Unsupported,
        Skipped,
    };

    const char* GetSceneRenderFeatureName(SceneRenderFeature feature);
    const char* GetSceneRenderFeatureStatusName(SceneRenderFeatureStatus status);

    struct SceneRenderFeatureCapability
    {
        SceneRenderFeature feature = SceneRenderFeature::PBR;
        SceneRenderFeatureStatus status = SceneRenderFeatureStatus::Unknown;
        bool requested = false;
        bool supported = false;
        bool enabled = false;
        bool fallbackUsed = false;
        bool renderGraphBacked = false;
        bool rhiCapabilityKnown = false;
        std::string requiredCapability;
        std::string diagnosticMessage;
        uint32 graphPassCount = 0;
        uint64 estimatedWorkItems = 0;
    };

    struct SceneRenderFeatureReport
    {
        uint32 schemaVersion = RVX_SCENE_RENDER_FEATURE_REPORT_SCHEMA_VERSION;
        std::vector<SceneRenderFeatureCapability> features;
        uint32 supportedCount = 0;
        uint32 fallbackCount = 0;
        uint32 unsupportedCount = 0;
        uint32 skippedCount = 0;
        uint32 unknownCount = 0;
    };

    struct SceneRendererFrameDiagnostics
    {
        uint32 schemaVersion = RVX_SCENE_RENDERER_FRAME_DIAGNOSTICS_SCHEMA_VERSION;
        uint64 frameCount = 0;
        bool renderAttempted = false;
        bool rendered = false;
        bool graphBuilt = false;
        bool graphCompiled = false;
        bool graphCompileValid = true;
        bool graphExecutionSkipped = false;
        std::string skippedReason;

        uint32 renderGraphTotalPasses = 0;
        uint32 renderGraphCulledPasses = 0;
        uint32 renderGraphBarrierCount = 0;
        uint32 renderGraphTextureBarrierCount = 0;
        uint32 renderGraphBufferBarrierCount = 0;
        uint32 renderGraphValidationWarningCount = 0;
        uint32 renderGraphValidationErrorCount = 0;
        float renderGraphMemorySavingsPercent = 0.0f;

        size_t renderSceneObjectCount = 0;
        size_t renderSceneLightCount = 0;
        size_t visibleObjectCount = 0;
        size_t opaqueDrawItemCount = 0;
        size_t maskedDrawItemCount = 0;
        size_t transparentDrawItemCount = 0;
        SceneFeatureExtractionStats featureExtractionStats;
        uint32 pointLightCount = 0;
        uint32 spotLightCount = 0;
        bool lightConstantsBufferReady = false;
        bool pointLightsBufferReady = false;
        bool spotLightsBufferReady = false;
        bool frameLightResourcesBound = false;
        FrameLightFallbackReason frameLightFallbackReason = FrameLightFallbackReason::FallbackUnavailable;
        uint32 pointShadowRequestCount = 0;
        uint32 spotShadowRequestCount = 0;
        uint32 localShadowRequestCount = 0;
        bool localShadowAtlasReady = false;
        std::string localShadowFallbackReason;
        bool clusteredLightingInitialized = false;
        bool clusteredLightingFrameBegun = false;
        bool clusteredLightingLightsAssigned = false;
        bool clusteredLightingGpuBuffersUploaded = false;
        bool clusteredLightingClusterAABBBufferReady = false;
        bool clusteredLightingClusterBufferReady = false;
        bool clusteredLightingLightIndexBufferReady = false;
        bool clusteredLightingConstantsBufferReady = false;
        uint32 clusteredLightingClusterCount = 0;
        uint32 clusteredLightingLightIndexCount = 0;
        uint32 clusteredLightingActiveClusters = 0;
        uint32 clusteredLightingTotalLightAssignments = 0;
        uint32 clusteredLightingMaxLightsInCluster = 0;
        float clusteredLightingAvgLightsPerCluster = 0.0f;
        std::string clusteredLightingFallbackReason;

        size_t registeredPassCount = 0;
        size_t graphPassCount = 0;
        size_t skippedDisabledPassCount = 0;
        size_t skippedUnsupportedPassCount = 0;
        std::vector<RenderPassStatus> passStatuses;

        RenderResourceRegistryStats gpuResourceStats;
        SceneGPUDrivenCullingStats gpuDrivenCullingStats;
        RenderPolicyDiagnostics policy;
        RayTracingSceneManagerStats rayTracingSceneStats;

        uint32 requestedPostProcessEffectCount = 0;
        uint32 enabledPostProcessEffectCount = 0;
        uint32 unsupportedPostProcessSkippedCount = 0;
        uint32 scheduledPostProcessEffectCount = 0;
        uint32 postProcessGraphPassCount = 0;
        RenderVisualQualityPreset requestedVisualQualityPreset = RenderVisualQualityPreset::Medium;
        RenderVisualQualityPreset appliedVisualQualityPreset = RenderVisualQualityPreset::Medium;
        bool hdrSceneColorEnabled = false;
        ToneMappingOutputColorSpace toneMappingOutputColorSpace = ToneMappingOutputColorSpace::SRGB;
        RHIFormat postProcessFinalOutputFormat = RHIFormat::Unknown;
        bool postProcessToneMappingBoundaryValid = true;
        bool postProcessFallbackCopyApplied = false;
        uint32 postProcessFallbackCopyPassCount = 0;
        bool postProcessDepthInputAvailable = false;
        bool postProcessVelocityInputAvailable = false;
        bool postProcessTemporalHistoryAvailable = false;
        std::string hdrFallbackReason;
        std::string postProcessFallbackCopyReason;
        std::string postProcessToneMappingBoundaryWarning;
        std::vector<PostProcessEffectExecutionPlan> postProcessEffectPlans;
        SceneRenderFeatureReport featureReport;

        bool externalTargetRequested = false;
        bool externalTargetActive = false;
        bool externalColorImported = false;
        bool externalDepthImported = false;
        std::string externalTargetFallbackReason;

        std::vector<std::string> graphDiagnostics;
    };

    struct SceneRendererToolDiagnosticsSnapshot
    {
        uint32 schemaVersion = RVX_SCENE_RENDERER_TOOL_DIAGNOSTICS_SCHEMA_VERSION;
        bool frameDiagnosticsAvailable = false;
        bool renderGraphDiagnosticsAvailable = false;
        bool rhiCapabilityReportAvailable = false;
        SceneRendererFrameDiagnostics frame;
        RenderGraph::Diagnostics renderGraph;
        RHICapabilityReport rhiCapabilityReport;
    };

    struct SceneRendererToolDiagnosticsArtifactValidationCodeCount
    {
        std::string code;
        uint32 count = 0;
    };

    struct SceneRendererToolDiagnosticsArtifactResult
    {
        bool requested = false;
        bool directoryReady = false;
        bool captureMetadataAvailable = false;
        uint32 toolDiagnosticsSchemaVersion = RVX_SCENE_RENDERER_TOOL_DIAGNOSTICS_SCHEMA_VERSION;
        uint32 frameDiagnosticsSchemaVersion = RVX_SCENE_RENDERER_FRAME_DIAGNOSTICS_SCHEMA_VERSION;
        uint32 renderGraphDiagnosticsSchemaVersion = RVX_RENDER_GRAPH_DIAGNOSTICS_SCHEMA_VERSION;
        uint32 rhiCapabilityReportSchemaVersion = RVX_RHI_CAPABILITY_REPORT_SCHEMA_VERSION;
        uint32 artifactSummarySchemaVersion = RVX_SCENE_RENDERER_TOOL_ARTIFACT_SUMMARY_SCHEMA_VERSION;
        uint32 artifactValidationSchemaVersion = RVX_SCENE_RENDERER_TOOL_ARTIFACT_VALIDATION_SCHEMA_VERSION;
        std::string renderGraphDiagnosticsSchemaId = RVX_RENDER_GRAPH_DIAGNOSTICS_SCHEMA_ID;
        std::string rhiCapabilityReportSchemaId = RVX_RHI_CAPABILITY_REPORT_SCHEMA_ID;
        uint64 frameIndex = 0;
        size_t renderGraphPassCount = 0;
        size_t renderGraphResourceCount = 0;
        bool rhiCapabilityReportJsonExpected = false;
        bool toolDiagnosticsTextSaved = false;
        bool renderGraphGraphvizSaved = false;
        bool renderGraphDiagnosticsTextSaved = false;
        bool renderGraphDiagnosticsJsonSaved = false;
        bool rhiCapabilityReportJsonSaved = false;
        bool manifestJsonSaved = false;
        bool artifactSummaryJsonSaved = false;
        bool artifactValidationJsonSaved = false;
        bool allPrimaryArtifactsSaved = false;
        bool artifactValidationResultAvailable = false;
        bool artifactValidationAllPrimaryArtifactsValid = false;
        bool artifactValidationBundleHashMatches = false;
        std::string artifactValidationVerdictCode = "Unavailable";
        std::string artifactValidationPrimaryFailureCode = "Unavailable";
        uint32 artifactValidationPrimaryFailureEntryIndex = RVX_INVALID_INDEX;
        uint32 artifactValidationPrimaryFailureEntryCount = 0;
        uint32 artifactValidationEntryCount = 0;
        bool artifactValidationEntryCountMatchesCheckedPrimaryArtifactCount = false;
        std::string artifactValidationEntryCoverageCode = "Unavailable";
        std::string artifactValidationEntryCoverageMessage = "validation result is unavailable";
        std::string artifactValidationPrimaryFailureArtifactId;
        std::string artifactValidationPrimaryFailureArtifactRelativePath;
        std::string artifactValidationPrimaryFailureArtifactKind;
        std::string artifactValidationPrimaryFailureArtifactContentType;
        std::string artifactValidationPrimaryFailureArtifactSchemaId;
        uint32 artifactValidationPrimaryFailureArtifactSchemaVersion = 0;
        std::string artifactValidationPrimaryFailureMessage;
        uint32 primaryArtifactCount = 0;
        uint32 savedPrimaryArtifactCount = 0;
        uint32 artifactValidationCheckedPrimaryArtifactCount = 0;
        uint32 artifactValidationValidPrimaryArtifactCount = 0;
        uint32 artifactValidationFailedPrimaryArtifactCount = 0;
        uint64 totalPrimaryArtifactBytes = 0;
        uint64 artifactValidationActualTotalPrimaryArtifactBytes = 0;
        bool toolDiagnosticsTextExists = false;
        bool renderGraphGraphvizExists = false;
        bool renderGraphDiagnosticsTextExists = false;
        bool renderGraphDiagnosticsJsonExists = false;
        bool rhiCapabilityReportJsonExists = false;
        bool manifestJsonExists = false;
        bool artifactValidationJsonExists = false;
        uint64 toolDiagnosticsTextBytes = 0;
        uint64 renderGraphGraphvizBytes = 0;
        uint64 renderGraphDiagnosticsTextBytes = 0;
        uint64 renderGraphDiagnosticsJsonBytes = 0;
        uint64 rhiCapabilityReportJsonBytes = 0;
        uint64 manifestJsonBytes = 0;
        uint64 artifactValidationJsonBytes = 0;
        std::string toolDiagnosticsTextContentHash;
        std::string renderGraphGraphvizContentHash;
        std::string renderGraphDiagnosticsTextContentHash;
        std::string renderGraphDiagnosticsJsonContentHash;
        std::string rhiCapabilityReportJsonContentHash;
        std::string manifestJsonContentHash;
        std::string artifactValidationJsonContentHash;
        std::string primaryArtifactBundleHash;
        std::string artifactValidationExpectedPrimaryArtifactBundleHash;
        std::string artifactValidationActualPrimaryArtifactBundleHash;
        std::vector<SceneRendererToolDiagnosticsArtifactValidationCodeCount> artifactValidationDiagnosticCodeCounts;
        std::string outputDirectory;
        std::string captureId;
        std::string captureBaseName;
        std::string toolDiagnosticsTextPath;
        std::string renderGraphGraphvizPath;
        std::string renderGraphDiagnosticsTextPath;
        std::string renderGraphDiagnosticsJsonPath;
        std::string rhiCapabilityReportJsonPath;
        std::string manifestJsonPath;
        std::string artifactSummaryJsonPath;
        std::string artifactValidationJsonPath;
        std::string toolDiagnosticsTextRelativePath;
        std::string renderGraphGraphvizRelativePath;
        std::string renderGraphDiagnosticsTextRelativePath;
        std::string renderGraphDiagnosticsJsonRelativePath;
        std::string rhiCapabilityReportJsonRelativePath;
        std::string manifestJsonRelativePath;
        std::string artifactSummaryJsonRelativePath;
        std::string artifactValidationJsonRelativePath;
    };

    struct SceneRendererToolDiagnosticsArtifactValidationEntry
    {
        uint32 entryIndex = RVX_INVALID_INDEX;
        std::string id;
        std::string kind;
        std::string contentType;
        std::string schemaId;
        uint32 schemaVersion = 0;
        std::string path;
        std::string relativePath;
        bool expectedSaved = false;
        bool exists = false;
        bool valid = false;
        bool primaryFailure = false;
        bool byteSizeMatches = false;
        bool contentHashMatches = false;
        bool identityChecked = false;
        bool identityMatches = false;
        bool schemaChecked = false;
        bool schemaMatches = false;
        uint64 expectedByteSize = 0;
        uint64 actualByteSize = 0;
        std::string expectedContentHash;
        std::string actualContentHash;
        std::string actualId;
        std::string actualKind;
        std::string actualContentType;
        std::string actualSchemaId;
        uint32 actualSchemaVersion = 0;
        std::string diagnosticCode = "None";
        std::string diagnosticMessage;
    };

    struct SceneRendererToolDiagnosticsArtifactValidationResult
    {
        uint32 schemaVersion = RVX_SCENE_RENDERER_TOOL_ARTIFACT_VALIDATION_SCHEMA_VERSION;
        bool artifactResultAvailable = false;
        bool allPrimaryArtifactsValid = false;
        bool bundleHashMatches = false;
        std::string verdictCode = "Unavailable";
        std::string primaryFailureCode = "Unavailable";
        uint32 primaryFailureEntryIndex = RVX_INVALID_INDEX;
        uint32 primaryFailureEntryCount = 0;
        uint32 entryCount = 0;
        bool entryCountMatchesCheckedPrimaryArtifactCount = false;
        std::string entryCoverageCode = "Unavailable";
        std::string entryCoverageMessage = "validation result is unavailable";
        std::string primaryFailureArtifactId;
        std::string primaryFailureArtifactRelativePath;
        std::string primaryFailureArtifactKind;
        std::string primaryFailureArtifactContentType;
        std::string primaryFailureArtifactSchemaId;
        uint32 primaryFailureArtifactSchemaVersion = 0;
        std::string primaryFailureMessage;
        uint32 checkedPrimaryArtifactCount = 0;
        uint32 validPrimaryArtifactCount = 0;
        uint32 failedPrimaryArtifactCount = 0;
        uint64 actualTotalPrimaryArtifactBytes = 0;
        std::string expectedPrimaryArtifactBundleHash;
        std::string actualPrimaryArtifactBundleHash;
        bool captureMetadataAvailable = false;
        std::string captureId;
        std::string captureBaseName;
        std::string outputDirectory;
        uint64 frameIndex = 0;
        uint32 toolDiagnosticsSchemaVersion = RVX_SCENE_RENDERER_TOOL_DIAGNOSTICS_SCHEMA_VERSION;
        uint32 frameDiagnosticsSchemaVersion = RVX_SCENE_RENDERER_FRAME_DIAGNOSTICS_SCHEMA_VERSION;
        uint32 renderGraphDiagnosticsSchemaVersion = RVX_RENDER_GRAPH_DIAGNOSTICS_SCHEMA_VERSION;
        uint32 rhiCapabilityReportSchemaVersion = RVX_RHI_CAPABILITY_REPORT_SCHEMA_VERSION;
        uint32 artifactSummarySchemaVersion = RVX_SCENE_RENDERER_TOOL_ARTIFACT_SUMMARY_SCHEMA_VERSION;
        uint32 artifactValidationSchemaVersion = RVX_SCENE_RENDERER_TOOL_ARTIFACT_VALIDATION_SCHEMA_VERSION;
        std::string renderGraphDiagnosticsSchemaId = RVX_RENDER_GRAPH_DIAGNOSTICS_SCHEMA_ID;
        std::string rhiCapabilityReportSchemaId = RVX_RHI_CAPABILITY_REPORT_SCHEMA_ID;
        size_t renderGraphPassCount = 0;
        size_t renderGraphResourceCount = 0;
        std::vector<SceneRendererToolDiagnosticsArtifactValidationCodeCount> diagnosticCodeCounts;
        std::vector<SceneRendererToolDiagnosticsArtifactValidationEntry> entries;
    };

    struct SceneLocalLightingStats
    {
        uint64 frameCount = 0;
        uint32 pointLightCount = 0;
        uint32 spotLightCount = 0;
        bool lightConstantsBufferReady = false;
        bool pointLightsBufferReady = false;
        bool spotLightsBufferReady = false;
        bool frameLightResourcesBound = false;
        FrameLightFallbackReason frameLightFallbackReason = FrameLightFallbackReason::FallbackUnavailable;
        uint32 pointShadowRequestCount = 0;
        uint32 spotShadowRequestCount = 0;
        uint32 localShadowRequestCount = 0;
        bool localShadowAtlasReady = false;
        std::string localShadowFallbackReason;
    };

    struct SceneClusteredLightingStats
    {
        uint64 frameCount = 0;
        bool initialized = false;
        bool frameBegun = false;
        bool lightsAssigned = false;
        bool gpuBuffersUploaded = false;
        bool clusterAABBBufferReady = false;
        bool clusterBufferReady = false;
        bool lightIndexBufferReady = false;
        bool clusterConstantsBufferReady = false;
        uint32 clusterCount = 0;
        uint32 lightIndexCount = 0;
        uint32 activeClusters = 0;
        uint32 totalLightAssignments = 0;
        uint32 maxLightsInCluster = 0;
        float avgLightsPerCluster = 0.0f;
        std::string fallbackReason;
    };

    struct SceneEnvironmentIBLStats
    {
        uint64 frameCount = 0;
        bool skyboxFound = false;
        bool uploadRequested = false;
        bool textureIBLEnabled = false;
        uint32 prefilteredMipLevels = 1;
        float intensity = 1.0f;
        std::string fallbackReason;
    };

    struct SceneRayTracingBudgetSettings
    {
        bool enabled = false;
        uint64 maxRayCount = 0;
        uint64 maxDenoiseTapCount = 0;
        uint64 maxTrackedResourceBytes = 0;
        float maxMeasuredGpuMs = 0.0f;
        float maxShadowMeasuredGpuMs = 0.0f;
        float maxReflectionMeasuredGpuMs = 0.0f;
        float gpuTimingHysteresis = 0.15f;
        float gpuTimingRecoveryRate = 0.05f;
        uint32 gpuTimingAdjustmentFrameCount = 2;
        float minReflectionResolutionScale = 0.25f;
        uint32 minShadowSamplesPerPixel = 1;
        uint32 minReflectionSamplesPerPixel = 1;
        uint32 minReflectionDenoiseRadius = 0;
        uint64 blasCacheEvictionFrameThreshold = 300;
    };

    struct SceneRayTracingFrameStats
    {
        bool sceneSupported = false;
        bool scenePrepared = false;
        bool tlasAvailable = false;
        bool shadowRequested = false;
        bool shadowSupported = false;
        bool shadowRecorded = false;
        bool reflectionRequested = false;
        bool reflectionSupported = false;
        bool reflectionRecorded = false;
        bool reflectionDenoiseRequested = false;
        bool reflectionDenoiseSupported = false;
        bool reflectionDenoiseRecorded = false;
        bool reflectionCompositeRequested = false;
        bool reflectionCompositeSupported = false;
        bool reflectionCompositeRecorded = false;
        bool reflectionMaterialTextureTableAvailable = false;
        bool reflectionGeometryMetadataAvailable = false;
        bool reflectionGeometryTableAvailable = false;
        bool shadowHistoryAvailable = false;
        bool shadowDepthHistoryAvailable = false;
        bool shadowNormalHistoryAvailable = false;
        bool shadowHistoryReset = false;
        bool shadowHistoryRecreated = false;
        bool shadowHistoryResolutionChanged = false;
        bool shadowHistoryConfigChanged = false;
        bool shadowTemporalAccumulated = false;
        bool shadowMaterialTextureTableAvailable = false;
        bool shadowAlphaMetadataAvailable = false;
        bool shadowAlphaTextureTableAvailable = false;
        bool shadowAlphaGeometryTableAvailable = false;
        bool reflectionHistoryAvailable = false;
        bool reflectionDepthHistoryAvailable = false;
        bool reflectionNormalHistoryAvailable = false;
        bool reflectionHistoryReset = false;
        bool reflectionHistoryRecreated = false;
        bool reflectionHistoryResolutionChanged = false;
        bool reflectionHistoryConfigChanged = false;
        bool reflectionTemporalAccumulated = false;
        bool denoiseFallbackToRaw = false;
        bool shadowGpuTimingSupported = false;
        bool shadowGpuTimingQueriesRecorded = false;
        bool shadowGpuTimingResolveRecorded = false;
        bool shadowGpuTimingReadbackBufferAvailable = false;
        bool shadowGpuTimingResultAvailable = false;
        bool reflectionGpuTimingSupported = false;
        bool reflectionGpuTimingQueriesRecorded = false;
        bool reflectionGpuTimingResolveRecorded = false;
        bool reflectionGpuTimingReadbackBufferAvailable = false;
        bool reflectionGpuTimingResultAvailable = false;
        bool budgetEnabled = false;
        bool budgetApplied = false;
        bool rayBudgetExceeded = false;
        bool denoiseTapBudgetExceeded = false;
        bool resourceBudgetExceeded = false;
        bool resourceBudgetEvictionAttempted = false;
        bool resourceByteAccountingOverflowed = false;
        bool gpuTimeBudgetExceeded = false;
        bool gpuTimeBudgetApplied = false;
        bool gpuTimeBudgetQualityScaleAdjusted = false;
        bool shadowGpuTimeBudgetExceeded = false;
        bool shadowGpuTimeBudgetApplied = false;
        bool shadowGpuTimeBudgetQualityScaleAdjusted = false;
        bool reflectionGpuTimeBudgetExceeded = false;
        bool reflectionGpuTimeBudgetApplied = false;
        bool reflectionGpuTimeBudgetQualityScaleAdjusted = false;
        bool measuredGpuTimeAvailable = false;
        uint64 rayBudget = 0;
        uint64 denoiseTapBudget = 0;
        uint64 trackedResourceBudget = 0;
        uint64 estimatedRayCountBeforeBudget = 0;
        uint64 estimatedDenoiseTapCountBeforeBudget = 0;
        uint64 shadowGpuTimestampFrequency = 0;
        uint64 reflectionGpuTimestampFrequency = 0;
        uint64 shadowGpuTimingReadbackBytes = 0;
        uint64 reflectionGpuTimingReadbackBytes = 0;
        uint64 shadowGpuTimingStartTimestamp = 0;
        uint64 shadowGpuTimingEndTimestamp = 0;
        uint64 shadowGpuTimingElapsedTicks = 0;
        uint64 reflectionGpuTimingStartTimestamp = 0;
        uint64 reflectionGpuTimingEndTimestamp = 0;
        uint64 reflectionGpuTimingElapsedTicks = 0;
        float shadowGpuTimingElapsedMs = 0.0f;
        float reflectionGpuTimingElapsedMs = 0.0f;
        float totalMeasuredRayTracingGpuMs = 0.0f;
        float gpuTimeBudget = 0.0f;
        float shadowGpuTimeBudget = 0.0f;
        float reflectionGpuTimeBudget = 0.0f;
        float measuredGpuTimeForBudgetMs = 0.0f;
        float measuredShadowGpuTimeForBudgetMs = 0.0f;
        float measuredReflectionGpuTimeForBudgetMs = 0.0f;
        float gpuTimeBudgetQualityScale = 1.0f;
        float shadowGpuTimeBudgetQualityScale = 1.0f;
        float reflectionGpuTimeBudgetQualityScale = 1.0f;
        float gpuTimingRecoveryRate = 0.05f;
        uint32 gpuTimeBudgetOverBudgetFrameCount = 0;
        uint32 gpuTimeBudgetUnderBudgetFrameCount = 0;
        uint32 shadowGpuTimeBudgetOverBudgetFrameCount = 0;
        uint32 shadowGpuTimeBudgetUnderBudgetFrameCount = 0;
        uint32 reflectionGpuTimeBudgetOverBudgetFrameCount = 0;
        uint32 reflectionGpuTimeBudgetUnderBudgetFrameCount = 0;
        uint32 gpuTimeBudgetAdjustmentFrameCount = 0;
        uint32 shadowGpuTimingReadbackBufferCount = 0;
        uint32 reflectionGpuTimingReadbackBufferCount = 0;
        uint32 shadowGpuTimingReadbackFrameIndex = RVX_INVALID_INDEX;
        uint32 reflectionGpuTimingReadbackFrameIndex = RVX_INVALID_INDEX;
        uint32 shadowGpuTimingStartQueryIndex = 0;
        uint32 shadowGpuTimingEndQueryIndex = 1;
        uint32 reflectionGpuTimingStartQueryIndex = 0;
        uint32 reflectionGpuTimingEndQueryIndex = 1;
        uint64 estimatedShadowRayCount = 0;
        uint64 estimatedReflectionRayCount = 0;
        uint64 estimatedTotalRayCount = 0;
        uint64 estimatedReflectionDenoiseTapCount = 0;
        uint64 blasCacheEvictionFrameThreshold = 300;
        size_t cachedBLASCount = 0;
        size_t evictedBLASCount = 0;
        size_t resourceBudgetEvictedBLASCount = 0;
        size_t releasedBLASScratchCount = 0;
        size_t pendingBLASScratchReleaseCount = 0;
        uint64 cachedBLASAccelerationStructureBytes = 0;
        uint64 cachedBLASScratchBytes = 0;
        uint64 releasedBLASScratchBytes = 0;
        uint64 topLevelAccelerationStructureBytes = 0;
        uint64 topLevelScratchBytes = 0;
        uint64 instanceBufferBytes = 0;
        uint64 materialMetadataBufferBytes = 0;
        uint64 alphaMetadataBufferBytes = 0;
        uint64 totalTrackedResourceBytes = 0;
        uint32 shadowMaterialTextureCount = 0;
        uint32 shadowMaterialTexturesBound = 0;
        uint32 shadowAlphaTextureCount = 0;
        uint32 shadowAlphaTexturesBound = 0;
        uint32 shadowAlphaIndexBufferCount = 0;
        uint32 shadowAlphaUVBufferCount = 0;
        uint32 reflectionMaterialTextureCount = 0;
        uint32 reflectionMaterialTexturesBound = 0;
        uint32 reflectionGeometryIndexBufferCount = 0;
        uint32 reflectionGeometryUVBufferCount = 0;
        uint32 reflectionGeometryNormalBufferCount = 0;
        uint32 reflectionGeometryTangentBufferCount = 0;
        float requestedReflectionResolutionScale = 0.0f;
        float reflectionResolutionScale = 0.0f;
        uint32 requestedShadowSamplesPerPixel = 0;
        uint32 requestedReflectionSamplesPerPixel = 0;
        uint32 requestedReflectionDenoiseRadius = 0;
        uint32 requestedReflectionDenoiseKernelTapCount = 0;
        uint32 shadowSamplesPerPixel = 0;
        uint32 reflectionSamplesPerPixel = 0;
        uint32 reflectionDenoiseRadius = 0;
        uint32 reflectionDenoiseKernelTapCount = 0;
        uint32 shadowWidth = 0;
        uint32 shadowHeight = 0;
        uint32 reflectionWidth = 0;
        uint32 reflectionHeight = 0;
    };

    /**
     * @brief Scene renderer - orchestrates rendering of a scene
     *
     * SceneRenderer is responsible for:
     * - Collecting renderable data from the World
     * - Setting up the RenderGraph with render passes
     * - Executing the render graph
     *
     * Usage:
     * @code
     * SceneRenderer renderer;
     * renderer.Initialize(renderContext);
     *
     * // Each frame
     * renderer.SetupView(camera, world);
     * renderer.Render();
     * @endcode
     */
    class SceneRenderer
    {
    public:
        SceneRenderer();
        ~SceneRenderer();

        // Non-copyable
        SceneRenderer(const SceneRenderer&) = delete;
        SceneRenderer& operator=(const SceneRenderer&) = delete;

        // =====================================================================
        // Lifecycle
        // =====================================================================

        /**
         * @brief Initialize the scene renderer
         * @param renderContext The render context to use
         */
        void Initialize(RenderContext* renderContext,
                        RenderResourceRegistry* resourceRegistry,
                        RenderRetirementQueue* retirementQueue = nullptr);

        /**
         * @brief Shutdown and release resources
         */
        void Shutdown();

        /**
         * @brief Check if initialized
         */
        bool IsInitialized() const { return m_initialized; }

        // =====================================================================
        // Frame Setup
        // =====================================================================

        /** @brief Transactionally apply one immutable render-frame packet. */
        [[nodiscard]] RenderFrameApplyResult ApplyFramePacket(
            const RenderFramePacket& packet,
            RenderResourceRegistry& registry);

        /** @brief Record the currently accepted packet into the active frame. */
        [[nodiscard]] RenderFrameExecutionResult RenderAcceptedFrame();

        /** @brief Seal current recording ownership with the actual submission token. */
        void NotifySubmission(const GPUCompletionToken& completion);

        /** @brief Release current recording ownership when no GPU work was submitted. */
        void ReleaseUnsubmittedFrame();

        /** @brief Commit temporal history only after successful presentation. */
        void MarkAcceptedFramePresented();

        /** @brief Sequence of the packet most recently presented. */
        uint64 GetLastPresentedFrameSequence() const
        {
            return m_renderScene.GetLastRenderedFrameSequence();
        }

        /** @brief Update the surface key used by temporal compatibility checks. */
        void SetSurfaceCompatibilityKey(uint64 key) noexcept;

        /**
         * @brief Reset temporal histories on the next rendered view.
         *
         * Use after camera cuts, scene jumps, or other discontinuities where
         * reprojection history must not be reused.
         */
        void RequestTemporalHistoryReset();

        /**
         * @brief Check whether a temporal history reset is queued for the next view.
         */
        bool IsTemporalHistoryResetPending() const { return m_pendingTemporalHistoryReset; }

        /**
         * @brief Release frame resources that can hold swap chain back-buffer references before resizing.
         */
        void PrepareForSwapChainResize();

        /**
         * @brief Use an externally-owned color/depth target instead of the swap-chain back buffer.
         *
         * Set before SetupView() when the target dimensions should drive camera projection.
         * The caller owns target lifetime and must track the exported states reported by
         * GetExternalRenderTargetStats().
         */
        void SetExternalRenderTarget(const SceneRendererExternalTargetDesc& desc);

        /**
         * @brief Return rendering to the swap-chain back-buffer path.
         */
        void ClearExternalRenderTarget();

        /**
         * @brief Check whether an external render target has been requested.
         */
        bool HasExternalRenderTarget() const { return m_externalRenderTarget.colorTarget != nullptr; }

        /**
         * @brief Set the color render target
         * @param target The render target texture handle
         */
        void SetColorTarget(RGTextureHandle target) { m_viewData.colorTarget = target; }

        /**
         * @brief Set the depth render target
         * @param target The depth target texture handle
         */
        void SetDepthTarget(RGTextureHandle target) { m_viewData.depthTarget = target; }

        // =====================================================================
        // Rendering
        // =====================================================================

        /**
         * @brief Build and execute the render graph
         *
         * This builds the render graph by calling Setup on all registered passes,
         * compiles it, and executes it.
         */
        void Render();

        // =====================================================================
        // Pass Management
        // =====================================================================

        /**
         * @brief Add a render pass
         * @param pass The pass to add (takes ownership)
         */
        void AddPass(std::unique_ptr<IRenderPass> pass);

        /**
         * @brief Remove a render pass by name
         * @param name The name of the pass to remove
         * @return true if the pass was found and removed
         */
        bool RemovePass(const char* name);

        /**
         * @brief Clear all render passes
         */
        void ClearPasses();

        /**
         * @brief Get number of registered passes
         */
        size_t GetPassCount() const;

        /// Callback invoked after per-frame pass preparation and before RenderGraph build.
        using PreGraphPrepareCallback = std::function<void(const ViewData&)>;

        /// Register a pre-graph prepare callback by owner token.
        bool AddPreGraphPrepareCallback(const void* owner, PreGraphPrepareCallback callback);

        /// Remove a pre-graph prepare callback by owner token.
        bool RemovePreGraphPrepareCallback(const void* owner);

        /// Get number of registered pre-graph prepare callbacks.
        size_t GetPreGraphPrepareCallbackCount() const { return m_preGraphPrepareCallbacks.size(); }

        /// Execute pre-graph callbacks for focused validation without running a full frame.
        void RunPreGraphPrepareCallbacksForTesting() { RunPreGraphPrepareCallbacks(); }

        /// Replace the render graph for focused validation without initializing a full RenderContext.
        void SetRenderGraphForTesting(std::unique_ptr<RenderGraph> renderGraph)
        {
            m_renderGraph = std::move(renderGraph);
            if (m_renderGraph && m_transientResourcePool)
            {
                m_renderGraph->SetTransientResourcePool(m_transientResourcePool.get());
            }
        }

        /// Build the render graph for focused validation without executing a full frame.
        void BuildRenderGraphForTesting()
        {
            BuildRenderGraph();
            RefreshFrameDiagnostics(false, false, true, false, nullptr);
        }

        /// Refresh frame diagnostics for focused validation without building a full RenderGraph.
        void RefreshFrameDiagnosticsForTesting(bool graphBuilt = false);

        /// Compile an empty/current prepared frame policy for lifecycle validation.
        void CompileRenderFramePlanForTesting() { CompileRenderFramePlan(); }

        /// Override the RHI capability source for focused feature-report tests.
        void SetRenderFeatureReportDeviceForTesting(IRHIDevice* device)
        {
            m_featureReportDeviceForTesting = device;
        }

        // =====================================================================
        // Accessors
        // =====================================================================

        /// Get the render graph
        RenderGraph* GetRenderGraph() { return m_renderGraph.get(); }
        const RenderGraph* GetRenderGraph() const { return m_renderGraph.get(); }

        /// Get the current view data
        ViewData& GetViewData() { return m_viewData; }
        const ViewData& GetViewData() const { return m_viewData; }

        /// Get the render scene
        RenderScene& GetRenderScene() { return m_renderScene; }
        const RenderScene& GetRenderScene() const { return m_renderScene; }

        /// Get the render context

        /// Get the pipeline cache
        PipelineCache* GetPipelineCache() { return m_pipelineCache.get(); }

        /// Get the material system
        MaterialSystem* GetMaterialSystem() { return m_materialSystem.get(); }

        /// Get the frame light manager.
        LightManager* GetLightManager() { return m_lightManager.get(); }
        const LightManager* GetLightManager() const { return m_lightManager.get(); }

        /// Get the clustered lighting frame data path.
        ClusteredLighting* GetClusteredLighting() { return m_clusteredLighting.get(); }
        const ClusteredLighting* GetClusteredLighting() const { return m_clusteredLighting.get(); }

        /// Get the transient resource pool
        TransientResourcePool* GetTransientResourcePool() { return m_transientResourcePool.get(); }

        /// Get the resource view cache
        ResourceViewCache* GetResourceViewCache() { return m_resourceViewCache.get(); }

        /// Get visible object indices
        const std::vector<uint32_t>& GetVisibleObjectIndices() const { return m_visibleObjectIndices; }

        /// Get scene collection path statistics.
        const SceneRenderCollectionStats& GetCollectionStats() const { return m_collectionStats; }

        /// Get render-facing feature extraction statistics.
        const SceneFeatureExtractionStats& GetFeatureExtractionStats() const { return m_featureExtractionStats; }

        /// Get the most recent render-facing feature snapshot.
        const RenderFeatureSnapshot& GetFeatureSnapshot() const { return m_featureSnapshot; }

        /// Get render pass chain statistics from the last RenderGraph build.
        const SceneRenderPassChainStats& GetPassChainStats() const { return m_passChainStats; }

        /// Get runtime post-process statistics from the last RenderGraph build.
        const SceneRenderPostProcessStats& GetPostProcessStats() const { return m_postProcessStats; }

        /// Get external render-target import statistics from the last RenderGraph build.
        const SceneRendererExternalTargetStats& GetExternalRenderTargetStats() const
        {
            return m_externalRenderTargetStats;
        }

        /// Get local-light collection and frame binding statistics.
        const SceneLocalLightingStats& GetLocalLightingStats() const { return m_localLightingStats; }

        /// Get clustered-light build and upload statistics.
        const SceneClusteredLightingStats& GetClusteredLightingStats() const { return m_clusteredLightingStats; }

        /// Get aggregate frame diagnostics for editor/debug tooling.
        const SceneRendererFrameDiagnostics& GetFrameDiagnostics() const { return m_frameDiagnostics; }

        /// Get the coherent modern-rendering feature capability report from the latest diagnostics refresh.
        const SceneRenderFeatureReport& GetRenderFeatureReport() const { return m_frameDiagnostics.featureReport; }

        /// Get versioned frame and RenderGraph diagnostics for editor/profiler tooling.
        const SceneRendererToolDiagnosticsSnapshot& GetToolDiagnosticsSnapshot() const
        {
            return m_toolDiagnosticsSnapshot;
        }

        /// Export the latest tool diagnostics snapshot as readable text for logs/debug panels.
        std::string ExportToolDiagnosticsText() const;

        /// Save the latest tool diagnostics snapshot as readable text.
        bool SaveToolDiagnosticsText(const char* filename) const;

        /// Export the latest RenderGraph from the tool diagnostics snapshot as Graphviz DOT.
        std::string ExportToolRenderGraphGraphviz() const;

        /// Save the latest RenderGraph from the tool diagnostics snapshot as Graphviz DOT.
        bool SaveToolRenderGraphGraphviz(const char* filename) const;

        /// Export the latest RenderGraph detailed diagnostics as readable text.
        std::string ExportToolRenderGraphDiagnosticsText() const;

        /// Save the latest RenderGraph detailed diagnostics as readable text.
        bool SaveToolRenderGraphDiagnosticsText(const char* filename) const;

        /// Export the latest RenderGraph detailed diagnostics as machine-readable JSON.
        std::string ExportToolRenderGraphDiagnosticsJson() const;

        /// Save the latest RenderGraph detailed diagnostics as machine-readable JSON.
        bool SaveToolRenderGraphDiagnosticsJson(const char* filename) const;

        /// Export the latest RHI capability report from the tool diagnostics snapshot as machine-readable JSON.
        std::string ExportToolRHICapabilityReportJson() const;

        /// Save the latest RHI capability report from the tool diagnostics snapshot as machine-readable JSON.
        bool SaveToolRHICapabilityReportJson(const char* filename) const;

        /// Export a machine-readable manifest for the latest tool diagnostics snapshot.
        std::string ExportToolDiagnosticsManifestJson(
            const SceneRendererToolDiagnosticsArtifactResult* artifacts = nullptr) const;

        /// Save a machine-readable manifest for the latest tool diagnostics snapshot.
        bool SaveToolDiagnosticsManifestJson(
            const char* filename,
            const SceneRendererToolDiagnosticsArtifactResult* artifacts = nullptr) const;

        /// Export a machine-readable summary of the saved tool diagnostics artifacts.
        std::string ExportToolDiagnosticsArtifactSummaryJson(
            const SceneRendererToolDiagnosticsArtifactResult* artifacts = nullptr) const;

        /// Save a machine-readable summary of the saved tool diagnostics artifacts.
        bool SaveToolDiagnosticsArtifactSummaryJson(
            const char* filename,
            const SceneRendererToolDiagnosticsArtifactResult* artifacts = nullptr) const;

        /// Save all tool diagnostics artifacts into a directory using a shared base name.
        SceneRendererToolDiagnosticsArtifactResult SaveToolDiagnosticsArtifacts(const char* directory,
                                                                               const char* baseName) const;

        /// Validate a saved primary diagnostics artifact bundle against its recorded hashes and sizes.
        static SceneRendererToolDiagnosticsArtifactValidationResult ValidateToolDiagnosticsArtifacts(
            const SceneRendererToolDiagnosticsArtifactResult& artifacts);

        /// Export a tool diagnostics artifact validation result as machine-readable JSON.
        static std::string ExportToolDiagnosticsArtifactValidationJson(
            const SceneRendererToolDiagnosticsArtifactValidationResult& validation);

        /// Save a tool diagnostics artifact validation result as machine-readable JSON.
        static bool SaveToolDiagnosticsArtifactValidationJson(
            const char* filename,
            const SceneRendererToolDiagnosticsArtifactValidationResult& validation);

        /// Get environment IBL binding statistics from the last view setup.
        const SceneEnvironmentIBLStats& GetEnvironmentIBLStats() const { return m_environmentIBLStats; }

        /// Get GPU-driven culling statistics from the last draw-list build.
        const SceneGPUDrivenCullingStats& GetGPUDrivenCullingStats() const { return m_gpuDrivenCullingStats; }

        /// Resolve and apply the requested GPU-driven runtime policy.
        void SetGPUDrivenCullingMode(RenderGPUDrivenMode mode);
        RenderGPUDrivenMode GetGPUDrivenCullingMode() const { return m_gpuDrivenCullingMode; }
        const GPUDrivenPolicyDecision& GetGPUDrivenPolicyDecision() const
        {
            return m_gpuDrivenPolicyDecision;
        }
        const RenderPolicyDiagnostics& GetRenderPolicyDiagnostics() const
        {
            return m_renderPolicyDiagnostics;
        }

        /// Compatibility wrapper for callers that still use a binary override.
        void SetGPUDrivenCullingEnabled(bool enabled);
        bool IsGPUDrivenCullingEnabled() const { return m_gpuDrivenCullingEnabled; }
        void SetGPUDrivenCullingConfig(const GPUCullingConfig& config)
        {
            InvalidateRenderFramePlan();
            if (m_gpuCulling)
            {
                m_gpuCulling->SetConfig(config);
            }
        }
        GPUCullingConfig GetGPUDrivenCullingConfig() const
        {
            return m_gpuCulling ? m_gpuCulling->GetConfig() : GPUCullingConfig{};
        }

        /// Get runtime ray-tracing scene acceleration-structure statistics.
        const RayTracingSceneManagerStats& GetRayTracingSceneStats() const;

        /// Get runtime camera velocity pass statistics.
        const CameraVelocityPassStats& GetCameraVelocityStats() const;

        /// Get runtime object velocity pass statistics.
        const ObjectVelocityPassStats& GetObjectVelocityStats() const;

        /// Get runtime particle feature pass statistics.
        const ParticleFeaturePassStats& GetParticleFeaturePassStats() const;

        /// Get the current frame top-level acceleration structure, if prepared.
        RHIAccelerationStructure* GetRayTracingTopLevelAS() const;

        /// Get runtime ray-traced shadow pass statistics.
        const RayTracedShadowPassStats& GetRayTracedShadowStats() const;

        /// Get runtime ray-traced reflection pass statistics.
        const RayTracedReflectionPassStats& GetRayTracedReflectionStats() const;

        /// Get runtime ray-traced reflection denoise pass statistics.
        const RayTracedReflectionDenoisePassStats& GetRayTracedReflectionDenoiseStats() const;

        /// Get runtime ray-traced reflection composite pass statistics.
        const RayTracedReflectionCompositePassStats& GetRayTracedReflectionCompositeStats() const;

        /// Get aggregate runtime ray-tracing frame statistics.
        SceneRayTracingFrameStats GetRayTracingFrameStats() const;

        /// Apply runtime ray-tracing budget settings without mutating persistent quality settings.
        void ApplyRayTracingBudgetSettings(const SceneRayTracingBudgetSettings& settings);

        /// Get runtime ray-tracing budget settings.
        const SceneRayTracingBudgetSettings& GetRayTracingBudgetSettings() const { return m_rayTracingBudgetSettings; }

        /// Get runtime post-process stack.
        PostProcessStack* GetPostProcessStack() { return m_postProcessStack.get(); }
        const PostProcessStack* GetPostProcessStack() const { return m_postProcessStack.get(); }

        /// Apply runtime post-process settings.
        void ApplyPostProcessSettings(const PostProcessSettings& settings);

        /// Get runtime post-process settings.
        PostProcessSettings& GetPostProcessSettings() { return m_postProcessSettings; }
        const PostProcessSettings& GetPostProcessSettings() const { return m_postProcessSettings; }

        /// Apply persistent directional shadow quality settings.
        /// Invalid values are left to ShadowPass support checks and pipeline sanitizers.
        void ApplyShadowPassConfig(const ShadowPassConfig& config);

        /// Get persistent directional shadow quality settings.
        const ShadowPassConfig& GetShadowPassConfig() const { return m_shadowPassConfig; }

        /// Get draw items for material-aware passes
        const std::vector<RenderDrawItem>& GetOpaqueDrawItems() const { return m_opaqueDrawItems; }
        const std::vector<RenderDrawItem>& GetMaskedDrawItems() const { return m_maskedDrawItems; }
        const std::vector<RenderDrawItem>& GetTransparentDrawItems() const { return m_transparentDrawItems; }
        const SceneMeshPassPreparation& GetMeshPassPreparation() const
        {
            return m_meshPassPreparation;
        }

        /// Set shader directory (must be set before Initialize)
        void SetShaderDirectory(const std::string& dir) { m_shaderDir = dir; }

    private:
        void RetireOwnerSnapshots(const GPUCompletionToken& completion);
        void BuildRenderGraph();
        void PrepareRayTracingScene();
        void AddRayTracingSceneBuildPass();
        void AddGPUDrivenCullingPass();
        void CommitGPUDrivenAccessSnapshots();
        void BuildMaterialDrawLists();
        void PrepareMeshPassPackets();
        void CompileRenderFramePlan();
        void InvalidateRenderFramePlan();
        void ApplyRenderFramePlanProjection();
        void ApplyGPUDrivenCullingToDrawLists();
        void ApplyGPUDrivenCullingToDrawList(std::vector<RenderDrawItem>& drawItems,
                                             uint32& cullableDrawItemCount);
        void PrepareGPUDrivenGraphCullInputs();
        void ApplyObjectMotionHistory();
        void UpdateObjectMotionHistory();
        void PreparePassesForFrame();
        void ApplyRayTracingBudget(ShadowPassConfig& shadowConfig,
                                   RayTracedReflectionPassConfig& reflectionConfig,
                                   RayTracedReflectionDenoisePassConfig& denoiseConfig,
                                   bool shadowRequested,
                                   bool reflectionRequested,
                                   bool denoiseRequested);
        void SetupDefaultPostProcess();
        void SetupDefaultPasses();
        SceneColorFormatPolicy ResolveSceneColorFormatPolicy(RHIFormat backBufferFormat,
                                                             bool postProcessActive) const;
        ToneMappingOutputColorSpace ResolveToneMappingOutputColorSpace(RHIFormat outputFormat) const;
        bool SupportsHDRSceneColor() const;
        void ResolveRenderTargetExtent(uint32& width, uint32& height) const;
        void UpdatePassResources();
        void RunPreGraphPrepareCallbacks();
        void ExecutePasses(RHICommandContext& ctx);
        void EnsureDepthBuffer(uint32_t width, uint32_t height);
        void RefreshFrameDiagnostics(bool renderAttempted,
                                     bool rendered,
                                     bool graphBuilt,
                                     bool graphCompiled,
                                     const char* skippedReason);
        SceneRenderFeatureReport BuildRenderFeatureReport(const SceneRendererFrameDiagnostics& diagnostics) const;

        struct PreGraphPrepareCallbackEntry
        {
            const void* owner = nullptr;
            PreGraphPrepareCallback callback;
        };

        RenderContext* m_renderContext = nullptr;
        RenderResourceRegistry* m_renderResourceRegistry = nullptr;
        RenderRetirementQueue* m_retirementQueue = nullptr;
        std::unique_ptr<RenderSubmissionResourceBatch> m_submissionBatch;
        std::unique_ptr<RenderGraph> m_renderGraph;
        std::unique_ptr<PipelineCache> m_pipelineCache;
        std::unique_ptr<MaterialSystem> m_materialSystem;
        std::unique_ptr<LightManager> m_lightManager;
        std::unique_ptr<ClusteredLighting> m_clusteredLighting;
        std::unique_ptr<TransientResourcePool> m_transientResourcePool;
        std::unique_ptr<ResourceViewCache> m_resourceViewCache;
        std::unique_ptr<RenderPassRegistry> m_passRegistry;
        std::unique_ptr<GPUCulling> m_gpuCulling;
        struct GPUCullingGraphHandles
        {
            RGBufferHandle constants;
            RGBufferHandle instances;
            RGBufferHandle instanceIndices;
            RGBufferHandle visibility;
            RGBufferHandle visibleInstances;
            RGBufferHandle indirectDraws;
            RGBufferHandle drawCount;

            bool IsValid() const
            {
                return constants.IsValid() && instances.IsValid() &&
                       instanceIndices.IsValid() &&
                       visibility.IsValid() && visibleInstances.IsValid() &&
                       indirectDraws.IsValid() && drawCount.IsValid();
            }
        };
        GPUCullingGraphHandles m_gpuCullingGraphHandles;
        std::unique_ptr<PostProcessStack> m_postProcessStack;
        std::unique_ptr<RayTracingSceneManager> m_rayTracingSceneManager;

        ViewData m_viewData;
        RenderScene m_renderScene;
        RenderFeatureSnapshot m_featureSnapshot;
        RenderProxySnapshot m_proxySnapshot;
        SceneRenderCollectionStats m_collectionStats;
        SceneFeatureExtractionStats m_featureExtractionStats;
        SceneRenderPassChainStats m_passChainStats;
        SceneRenderPostProcessStats m_postProcessStats;
        SceneRendererExternalTargetDesc m_externalRenderTarget;
        SceneRendererExternalTargetStats m_externalRenderTargetStats;
        SceneLocalLightingStats m_localLightingStats;
        SceneClusteredLightingStats m_clusteredLightingStats;
        SceneRendererFrameDiagnostics m_frameDiagnostics;
        SceneRendererToolDiagnosticsSnapshot m_toolDiagnosticsSnapshot;
        uint64 m_frameDiagnosticsCounter = 0;
        IRHIDevice* m_featureReportDeviceForTesting = nullptr;
        SceneEnvironmentIBLStats m_environmentIBLStats;
        SceneGPUDrivenCullingStats m_gpuDrivenCullingStats;
        RayTracingSceneManagerStats m_rayTracingSceneStats;
        SceneRayTracingBudgetSettings m_rayTracingBudgetSettings;
        SceneRayTracingFrameStats m_rayTracingFrameBudgetStats;
        float m_rayTracingGpuBudgetQualityScale = 1.0f;
        float m_rayTracingShadowGpuBudgetQualityScale = 1.0f;
        float m_rayTracingReflectionGpuBudgetQualityScale = 1.0f;
        float m_rayTracingLastMeasuredGpuMs = 0.0f;
        float m_rayTracingLastShadowMeasuredGpuMs = 0.0f;
        float m_rayTracingLastReflectionMeasuredGpuMs = 0.0f;
        bool m_rayTracingLastMeasuredGpuMsValid = false;
        bool m_rayTracingLastShadowMeasuredGpuMsValid = false;
        bool m_rayTracingLastReflectionMeasuredGpuMsValid = false;
        uint64 m_rayTracingGpuBudgetLastShadowEndTimestamp = 0;
        uint64 m_rayTracingGpuBudgetLastReflectionEndTimestamp = 0;
        uint32 m_rayTracingGpuBudgetOverBudgetFrameCount = 0;
        uint32 m_rayTracingGpuBudgetUnderBudgetFrameCount = 0;
        uint32 m_rayTracingShadowGpuBudgetOverBudgetFrameCount = 0;
        uint32 m_rayTracingShadowGpuBudgetUnderBudgetFrameCount = 0;
        uint32 m_rayTracingReflectionGpuBudgetOverBudgetFrameCount = 0;
        uint32 m_rayTracingReflectionGpuBudgetUnderBudgetFrameCount = 0;
        SceneColorFormatPolicy m_sceneColorFormatPolicy;
        Mat4 m_previousViewProjectionMatrix = Mat4Identity();
        bool m_previousViewProjectionValid = false;
        bool m_pendingTemporalHistoryReset = false;
        RenderGPUDrivenMode m_gpuDrivenCullingMode = RenderGPUDrivenMode::Auto;
        GPUDrivenPolicyDecision m_gpuDrivenPolicyDecision;
        bool m_gpuDrivenCullingEnabled = false;
        RenderPolicyDiagnostics m_renderPolicyDiagnostics;
        std::unordered_map<uint64, Mat4> m_previousObjectWorldMatrices;
        PostProcessSettings m_postProcessSettings;
        ShadowPassConfig m_shadowPassConfig;
        std::vector<uint32_t> m_visibleObjectIndices;
        std::vector<RenderDrawItem> m_opaqueDrawItems;
        std::vector<RenderDrawItem> m_maskedDrawItems;
        std::vector<RenderDrawItem> m_transparentDrawItems;
        SceneMeshPassPreparation m_meshPassPreparation;
        std::vector<RenderDrawItem> m_gpuCullingScratchDrawItems;
        std::vector<std::string> m_loggedUnsupportedPassNames;
        std::vector<PreGraphPrepareCallbackEntry> m_preGraphPrepareCallbacks;

        std::string m_shaderDir;
        DepthPrepass* m_depthPrepass = nullptr;  // Cached pointer to optional depth prepass
        OpaquePass* m_opaquePass = nullptr;  // Cached pointer to opaque pass
        ShadowPass* m_shadowPass = nullptr;  // Cached pointer to shadow pass
        RayTracedShadowPass* m_rayTracedShadowPass = nullptr;  // Cached pointer to ray traced shadow pass
        CameraVelocityPass* m_cameraVelocityPass = nullptr;  // Cached pointer to camera velocity pass
        ObjectVelocityPass* m_objectVelocityPass = nullptr;  // Cached pointer to object velocity pass
        ParticleFeaturePass* m_particleFeaturePass = nullptr;  // Cached pointer to particle feature pass
        RayTracedReflectionPass* m_rayTracedReflectionPass = nullptr;  // Cached pointer to ray traced reflection pass
        RayTracedReflectionDenoisePass* m_rayTracedReflectionDenoisePass = nullptr;  // Cached pointer to RT reflection denoise pass
        RayTracedReflectionCompositePass* m_rayTracedReflectionCompositePass = nullptr;  // Cached pointer to RT reflection composite pass
        TransparentPass* m_transparentPass = nullptr;  // Cached pointer to transparent pass
        SkyboxPass* m_skyboxPass = nullptr;  // Cached pointer to skybox pass
        BloomPass* m_bloomPostProcess = nullptr;
        ToneMappingPass* m_toneMappingPostProcess = nullptr;
        ColorGradingPass* m_colorGradingPostProcess = nullptr;
        ChromaticAberrationPass* m_chromaticAberrationPostProcess = nullptr;
        VignettePass* m_vignettePostProcess = nullptr;
        FilmGrainPass* m_filmGrainPostProcess = nullptr;
        SSAOPass* m_ssaoPostProcess = nullptr;
        FXAAPass* m_fxaaPostProcess = nullptr;

        // Depth buffer
        RHITextureRef m_depthTexture;
        RHITextureViewRef m_depthTextureView;
        uint32_t m_depthWidth = 0;
        uint32_t m_depthHeight = 0;

        // Back buffer state tracking
        std::vector<RHITextureAccessSnapshot> m_backBufferAccessSnapshots;
        RHITextureAccessSnapshot m_depthAccessSnapshot;
        RGTextureHandle m_depthGraphHandle;
        RGTextureHandle m_backBufferGraphHandle;
        uint32 m_activeBackBufferIndex = RVX_INVALID_INDEX;

        // Track swap chain dimensions to detect resize
        uint32_t m_lastSwapChainWidth = 0;
        uint32_t m_lastSwapChainHeight = 0;

        bool m_initialized = false;
    };

} // namespace RVX
