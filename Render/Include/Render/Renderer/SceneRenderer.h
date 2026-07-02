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
#include "Render/GPUResourceManager.h"
#include "Render/Material/MaterialSystem.h"
#include "Render/Passes/IRenderPass.h"
#include "Render/Passes/CameraVelocityPass.h"
#include "Render/Passes/ObjectVelocityPass.h"
#include "Render/Passes/RayTracedReflectionCompositePass.h"
#include "Render/Passes/RayTracedReflectionDenoisePass.h"
#include "Render/Passes/RayTracedReflectionPass.h"
#include "Render/Passes/RayTracedShadowPass.h"
#include "Render/Passes/ShadowPass.h"
#include "Render/PipelineCache.h"
#include "Render/PostProcess/PostProcessStack.h"
#include "Render/RayTracing/RayTracingSceneManager.h"
#include "Render/Renderer/RenderDrawItem.h"
#include "RenderContracts/RenderProxy.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace RVX
{
    inline constexpr RHIFormat RVX_SCENE_COLOR_HDR_FORMAT = RHIFormat::RGBA16_FLOAT;

    class World;
    class Camera;
    class BloomPass;
    class CameraVelocityPass;
    class ClusteredLighting;
    class ObjectVelocityPass;
    class ChromaticAberrationPass;
    class ColorGradingPass;
    class FXAAPass;
    class LightManager;
    class VignettePass;
    class IRenderPass;
    class DepthPrepass;
    class OpaquePass;
    class RenderPassRegistry;
    class RenderProxySceneBridge;
    class RayTracedReflectionCompositePass;
    class RayTracedReflectionDenoisePass;
    class RayTracedReflectionPass;
    class RayTracedShadowPass;
    class SceneManager;
    class SceneEnvironmentIBLBridge;
    class SceneSkyboxPassBridge;
    class ShadowPass;
    class SkyboxPass;
    class ToneMappingPass;
    class TransparentPass;

    enum class SceneRenderCollectionPath : uint8
    {
        None = 0,
        Proxy,
        LegacyFallback,
        ProxyRejected
    };

    struct SceneRenderCollectionStats
    {
        SceneRenderCollectionPath lastPath = SceneRenderCollectionPath::None;
        uint64 proxyFrameCount = 0;
        uint64 legacyFallbackFrameCount = 0;
        uint64 rejectedProxyFrameCount = 0;
        size_t lastProxyPrimitiveCount = 0;
        size_t lastProxyLightCount = 0;
        uint64 lastFallbackOwnerId = 0;
        std::string lastFallbackReason;
        bool lastFallbackSuppressed = false;
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

    struct SceneRendererFrameDiagnostics
    {
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

        uint32 requestedPostProcessEffectCount = 0;
        uint32 enabledPostProcessEffectCount = 0;
        uint32 unsupportedPostProcessSkippedCount = 0;
        uint32 postProcessGraphPassCount = 0;
        bool hdrSceneColorEnabled = false;
        ToneMappingOutputColorSpace toneMappingOutputColorSpace = ToneMappingOutputColorSpace::SRGB;
        bool postProcessToneMappingBoundaryValid = true;
        std::string hdrFallbackReason;
        std::string postProcessToneMappingBoundaryWarning;

        bool externalTargetRequested = false;
        bool externalTargetActive = false;
        bool externalColorImported = false;
        bool externalDepthImported = false;
        std::string externalTargetFallbackReason;

        std::vector<std::string> graphDiagnostics;
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

    struct SceneGPUDrivenCullingStats
    {
        bool enabled = false;
        bool fallbackUsed = false;
        bool graphPassAdded = false;
        bool graphPassRecorded = false;
        bool gpuExecutionRecorded = false;
        uint32 inputOpaqueDrawItemCount = 0;
        uint32 inputMaskedDrawItemCount = 0;
        uint32 outputOpaqueDrawItemCount = 0;
        uint32 outputMaskedDrawItemCount = 0;
        uint32 cullableOpaqueDrawItemCount = 0;
        uint32 cullableMaskedDrawItemCount = 0;
        uint32 graphInputDrawItemCount = 0;
        uint32 skippedMissingGpuDataCount = 0;
        uint32 visibleCullableDrawItemCount = 0;
        uint32 frustumCulledDrawItemCount = 0;
        uint32 distanceCulledDrawItemCount = 0;
        bool opaqueIndirectRequested = false;
        bool opaqueIndirectEligible = false;
        uint32 opaqueGpuDrivenIndirectBatchCount = 0;
        uint32 opaqueGpuDrivenIndirectDrawCount = 0;
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
        void Initialize(RenderContext* renderContext);

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

        /**
         * @brief Setup view data from camera and collect scene data
         * @param camera The camera to render from
         * @param world The world to render (can be null for just camera setup)
         */
        void SetupView(const Camera& camera, World* world);

        /**
         * @brief Setup view data from camera and collect scene data from a SceneManager.
         * @param camera The camera to render from
         * @param sceneManager The scene manager to render (can be null for just camera setup)
         */
        void SetupView(const Camera& camera, SceneManager* sceneManager);

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
        RenderContext* GetRenderContext() { return m_renderContext; }

        /// Get the GPU resource manager
        GPUResourceManager* GetGPUResourceManager() { return m_gpuResourceManager.get(); }

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

        /// Get environment IBL binding statistics from the last view setup.
        const SceneEnvironmentIBLStats& GetEnvironmentIBLStats() const { return m_environmentIBLStats; }

        /// Get GPU-driven culling statistics from the last draw-list build.
        const SceneGPUDrivenCullingStats& GetGPUDrivenCullingStats() const { return m_gpuDrivenCullingStats; }

        /// Enable or disable GPU-driven draw-list culling. CPU fallback is used until compute pipelines are ready.
        void SetGPUDrivenCullingEnabled(bool enabled) { m_gpuDrivenCullingEnabled = enabled; }
        bool IsGPUDrivenCullingEnabled() const { return m_gpuDrivenCullingEnabled; }
        void SetGPUDrivenCullingConfig(const GPUCullingConfig& config)
        {
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

        /// Set shader directory (must be set before Initialize)
        void SetShaderDirectory(const std::string& dir) { m_shaderDir = dir; }

        /// Enable compatibility fallback to the legacy scene collector when proxy extraction fails.
        void SetLegacyCollectionFallbackEnabled(bool enabled) { m_legacyCollectionFallbackEnabled = enabled; }
        bool IsLegacyCollectionFallbackEnabled() const { return m_legacyCollectionFallbackEnabled; }

    private:
        void BuildRenderGraph();
        void PrepareRayTracingScene();
        void AddRayTracingSceneBuildPass();
        void AddGPUDrivenCullingPass();
        void BuildMaterialDrawLists();
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
        void UpdateEnvironmentIBL(World* world);
        void UpdateSkyboxPass(World* world);
        SceneColorFormatPolicy ResolveSceneColorFormatPolicy(RHIFormat backBufferFormat,
                                                             bool postProcessActive) const;
        ToneMappingOutputColorSpace ResolveToneMappingOutputColorSpace(RHIFormat outputFormat) const;
        bool SupportsHDRSceneColor() const;
        void ResolveRenderTargetExtent(uint32& width, uint32& height) const;
        void SetupCameraViewData(const Camera& camera, uint32 width, uint32 height);
        void FinalizeViewScene(const Camera& camera);
        void UpdatePassResources();
        void RunPreGraphPrepareCallbacks();
        void ExecutePasses(RHICommandContext& ctx);
        void EnsureDepthBuffer(uint32_t width, uint32_t height);
        void RefreshFrameDiagnostics(bool renderAttempted,
                                     bool rendered,
                                     bool graphBuilt,
                                     bool graphCompiled,
                                     const char* skippedReason);

        struct PreGraphPrepareCallbackEntry
        {
            const void* owner = nullptr;
            PreGraphPrepareCallback callback;
        };

        RenderContext* m_renderContext = nullptr;
        std::unique_ptr<RenderGraph> m_renderGraph;
        std::unique_ptr<GPUResourceManager> m_gpuResourceManager;
        std::unique_ptr<PipelineCache> m_pipelineCache;
        std::unique_ptr<MaterialSystem> m_materialSystem;
        std::unique_ptr<LightManager> m_lightManager;
        std::unique_ptr<ClusteredLighting> m_clusteredLighting;
        std::unique_ptr<TransientResourcePool> m_transientResourcePool;
        std::unique_ptr<ResourceViewCache> m_resourceViewCache;
        std::unique_ptr<RenderPassRegistry> m_passRegistry;
        std::unique_ptr<RenderProxySceneBridge> m_proxyBridge;
        std::unique_ptr<SceneEnvironmentIBLBridge> m_environmentIBLBridge;
        std::unique_ptr<SceneSkyboxPassBridge> m_skyboxBridge;
        std::unique_ptr<GPUCulling> m_gpuCulling;
        std::unique_ptr<PostProcessStack> m_postProcessStack;
        std::unique_ptr<RayTracingSceneManager> m_rayTracingSceneManager;

        ViewData m_viewData;
        RenderScene m_renderScene;
        RenderProxySnapshot m_proxySnapshot;
        SceneRenderCollectionStats m_collectionStats;
        SceneRenderPassChainStats m_passChainStats;
        SceneRenderPostProcessStats m_postProcessStats;
        SceneRendererExternalTargetDesc m_externalRenderTarget;
        SceneRendererExternalTargetStats m_externalRenderTargetStats;
        SceneLocalLightingStats m_localLightingStats;
        SceneClusteredLightingStats m_clusteredLightingStats;
        SceneRendererFrameDiagnostics m_frameDiagnostics;
        uint64 m_frameDiagnosticsCounter = 0;
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
        bool m_gpuDrivenCullingEnabled = true;
        std::unordered_map<uint64, Mat4> m_previousObjectWorldMatrices;
        PostProcessSettings m_postProcessSettings;
        ShadowPassConfig m_shadowPassConfig;
        std::vector<uint32_t> m_visibleObjectIndices;
        std::vector<RenderDrawItem> m_opaqueDrawItems;
        std::vector<RenderDrawItem> m_maskedDrawItems;
        std::vector<RenderDrawItem> m_transparentDrawItems;
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
        FXAAPass* m_fxaaPostProcess = nullptr;

        // Depth buffer
        RHITextureRef m_depthTexture;
        RHITextureViewRef m_depthTextureView;
        uint32_t m_depthWidth = 0;
        uint32_t m_depthHeight = 0;

        // Back buffer state tracking
        std::vector<RHIResourceState> m_backBufferStates;
        RHIResourceState m_depthBufferState = RHIResourceState::Undefined;

        // Track swap chain dimensions to detect resize
        uint32_t m_lastSwapChainWidth = 0;
        uint32_t m_lastSwapChainHeight = 0;

        bool m_initialized = false;
        bool m_legacyCollectionFallbackEnabled = false;
    };

} // namespace RVX
