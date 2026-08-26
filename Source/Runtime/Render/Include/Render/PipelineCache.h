#pragma once

/**
 * @file PipelineCache.h
 * @brief Pipeline cache for managing graphics pipelines and frame/object sets
 *
 * PipelineCache handles:
 * - Runtime shader-program loading through an injected provider
 * - Stable frame/object/material descriptor set layouts
 * - Graphics pipeline creation and caching
 * - Frame and object constant buffer management
 */

#include "Core/Assert.h"
#include "Core/MathTypes.h"
#include "Render/Material/MaterialClassification.h"
#include "Render/Renderer/ShadowConstants.h"
#include "RHI/RHI.h"

#include <array>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace RVX
{
    // Forward declarations
    struct GPUCompletionToken;
    class FrameConstantUploadArena;
    class IShaderProgramProvider;
    class RenderSubmissionTracker;
    class RenderSubmissionResourceBatch;
    class RenderRetirementQueue;
    class ShaderProgramLoader;
    struct ShaderProgramMetadata;
    struct GPUSceneRasterResourceSnapshot;
    struct ViewData;

    constexpr uint32 RVX_GPU_SCENE_LEGACY_SKINNING_MATRIX_CAPACITY = 128;

    /**
     * @brief Strong, immutable palette resources selected for one recording.
     *
     * A skinned draw always consumes the two buffers as a pair. An empty pair
     * denotes the rigid/default binding and is resolved to cache-owned palette
     * buffers before a descriptor is created. Rigid objects never read them.
     */
    struct SkinningPaletteBufferBinding
    {
        RHIBufferRef currentPaletteBuffer;
        RHIBufferRef previousPaletteBuffer;

        [[nodiscard]] bool IsWellFormed() const noexcept
        {
            return static_cast<bool>(currentPaletteBuffer) ==
                   static_cast<bool>(previousPaletteBuffer);
        }

        [[nodiscard]] bool HasPalettes() const noexcept
        {
            return currentPaletteBuffer && previousPaletteBuffer;
        }
    };

    /** @brief Per-object view into one current/previous palette pair. */
    struct SkinningPaletteSliceBinding
    {
        uint32 currentOffset = 0;
        uint32 previousOffset = 0;
        uint32 matrixCount = 0;
        bool previousValid = false;

        [[nodiscard]] bool IsRigid() const noexcept { return matrixCount == 0; }

        [[nodiscard]] bool IsWellFormed() const noexcept
        {
            if (matrixCount == 0)
            {
                return currentOffset == 0 && previousOffset == 0;
            }

            return currentOffset <= std::numeric_limits<uint32>::max() - matrixCount &&
                   previousOffset <= std::numeric_limits<uint32>::max() - matrixCount;
        }
    };

    /** @brief Vertex-stream contract for DefaultLit direct graphics pipelines. */
    enum class DefaultLitDirectVertexInputMode : uint8
    {
        Rigid = 0,
        Skinned = 1,
    };

    /**
     * @brief View constants structure (matches HLSL cbuffer)
     */
    struct ViewConstants
    {
        Mat4 viewProjection;
        Vec3 cameraPosition;
        float time;
        Vec3 lightDirection;
        float directionalLightIntensity;
        Vec3 directionalLightColor;
        float directionalLightColorPadding = 0.0f;
        Vec4 iblDiffuseAmbient;
        Vec4 iblSpecularAmbient;
        Vec4 iblTextureParams;
        Vec4 cameraForwardAndShadowCascadeCount;
        std::array<Mat4, RVX_MAX_DIRECTIONAL_SHADOW_CASCADES> directionalShadowViewProjections;
        Vec4 directionalShadowParams;
        Vec4 directionalShadowReceiverParams;
        Vec4 directionalShadowCascadeSplits;
        Vec4 directionalShadowCascadeFadeDistances;
        Vec4 rayTracedShadowParams;
    };

    enum class DirectionalShadowFallbackReason : uint8
    {
        None = 0,
        DisabledNoDirectionalLight,
        MissingShadowSRV,
        MissingSampler,
        ReverseZUnsupported,
        FallbackUnavailable,
    };

    struct DirectionalShadowFrameResources
    {
        bool enabled = false;
        RHITextureView* shadowMapView = nullptr;
    };

    struct DirectionalShadowFrameBindingResult
    {
        bool shadowSamplingEnabled = false;
        DirectionalShadowFallbackReason fallbackReason = DirectionalShadowFallbackReason::DisabledNoDirectionalLight;
    };

    enum class RayTracedShadowFallbackReason : uint8
    {
        None = 0,
        Disabled,
        MissingShadowMaskSRV,
        FallbackUnavailable,
    };

    struct RayTracedShadowFrameResources
    {
        bool enabled = false;
        RHITextureView* shadowMaskView = nullptr;
    };

    struct RayTracedShadowFrameBindingResult
    {
        bool shadowMaskSamplingEnabled = false;
        RayTracedShadowFallbackReason fallbackReason = RayTracedShadowFallbackReason::Disabled;
    };

    enum class EnvironmentIBLFallbackReason : uint8
    {
        None = 0,
        Disabled,
        MissingIrradianceSRV,
        MissingPrefilteredEnvironmentSRV,
        MissingBRDFLUTSRV,
        MissingSampler,
        FallbackUnavailable,
    };

    struct EnvironmentIBLFrameResources
    {
        bool enabled = false;
        RHITextureView* irradianceView = nullptr;
        RHITextureView* prefilteredEnvironmentView = nullptr;
        RHITextureView* brdfLUTView = nullptr;
    };

    struct EnvironmentIBLFrameBindingResult
    {
        bool textureIBLSamplingEnabled = false;
        EnvironmentIBLFallbackReason fallbackReason =
            EnvironmentIBLFallbackReason::Disabled;
    };

    enum class FrameLightFallbackReason : uint8
    {
        None = 0,
        MissingLightConstants,
        MissingPointLights,
        MissingSpotLights,
        FallbackUnavailable,
    };

    enum class FrameClusteredLightFallbackReason : uint8
    {
        None = 0,
        MissingClusterConstants,
        MissingClusterData,
        MissingClusterLightIndices,
        FallbackUnavailable,
    };

    struct FrameLightResources
    {
        RHIBuffer* lightConstantsBuffer = nullptr;
        RHIBuffer* pointLightsBuffer = nullptr;
        RHIBuffer* spotLightsBuffer = nullptr;
        RHIBuffer* clusterConstantsBuffer = nullptr;
        RHIBuffer* clusterBuffer = nullptr;
        RHIBuffer* clusterLightIndexBuffer = nullptr;
    };

    /**
     * @brief Value-owned, byte-frozen realization of one frame light selection.
     *
     * Source light buffers are mutable upload allocations. This structure owns
     * copies made before graph composition, so graph imports and descriptor
     * binding snapshots can reference the exact same allocations.
     */
    struct FrozenFrameLightResources
    {
        RHIBufferRef lightConstantsBuffer;
        RHIBufferRef pointLightsBuffer;
        RHIBufferRef spotLightsBuffer;
        RHIBufferRef clusterConstantsBuffer;
        RHIBufferRef clusterBuffer;
        RHIBufferRef clusterLightIndexBuffer;

        [[nodiscard]] bool IsComplete() const noexcept
        {
            return lightConstantsBuffer && pointLightsBuffer &&
                   spotLightsBuffer && clusterConstantsBuffer &&
                   clusterBuffer && clusterLightIndexBuffer;
        }

        [[nodiscard]] FrameLightResources Borrow() const noexcept
        {
            return FrameLightResources{
                lightConstantsBuffer.Get(), pointLightsBuffer.Get(),
                spotLightsBuffer.Get(), clusterConstantsBuffer.Get(),
                clusterBuffer.Get(), clusterLightIndexBuffer.Get()};
        }
    };

    struct FrameLightBindingResult
    {
        bool lightResourcesBound = false;
        bool clusteredLightResourcesBound = false;
        FrameLightFallbackReason fallbackReason = FrameLightFallbackReason::FallbackUnavailable;
        FrameClusteredLightFallbackReason clusteredFallbackReason =
            FrameClusteredLightFallbackReason::FallbackUnavailable;
    };

    /**
     * @brief All explicit inputs for one immutable record-owned lighting binding.
     *
     * The caller supplies the exact resources selected by its per-view lighting
     * plan.  This deliberately does not read PipelineCache's mutable current
     * frame bindings: a later frame may update those bindings before this
     * record executes.
     */
    struct LightingBindingSnapshotRequest
    {
        uint64 lightingPlanIdentity = 0;
        FrozenFrameLightResources frozenLightResources{};
        DirectionalShadowFrameResources directionalShadow{};
        RayTracedShadowFrameResources rayTracedShadow{};
        EnvironmentIBLFrameResources environmentIBL{};
        /** Exact committed state used by deferred HDR lighting, set 0 / b21. */
        RHIBufferRef exposureStateBuffer;
        /**
         * Exact graph-owned GTAO visibility field used by this lighting record.
         * When omitted, the cache binds its immutable neutral-white R8 fallback.
         */
        RHITextureViewRef ambientOcclusionView;
        /** Content identity for ambientOcclusionView; required when a view is supplied. */
        uint64 ambientOcclusionFieldHash = 0;
        /** Opaque HDR copy produced after Sky and before translucency. */
        RHITextureViewRef opaqueSceneSnapshotView;
        RHISamplerRef opaqueSceneSnapshotSampler;
    };

    /**
     * @brief Strongly-owned set-0 lighting state for one exact lighting plan.
     *
     * The descriptor contains the frame view constants, CSM, local/clustered
     * lights, the mandatory fallback ray-traced mask, and IBL.  Every raw RHI
     * binding is paired with a strong owner here, so the descriptor remains
     * valid even when PipelineCache moves on to another frame.
     */
    struct LightingBindingSnapshot
    {
        uint64 lightingPlanIdentity = 0;
        RHIBufferRef viewConstantBuffer;
        RHITextureViewRef directionalShadowView;
        RHISamplerRef directionalShadowSampler;
        RHIBufferRef lightConstantsBuffer;
        RHIBufferRef pointLightsBuffer;
        RHIBufferRef spotLightsBuffer;
        RHITextureViewRef rayTracedShadowMaskView;
        bool rayTracedShadowEnabled = false;
        RHIBufferRef clusterConstantsBuffer;
        RHIBufferRef clusterBuffer;
        RHIBufferRef clusterLightIndexBuffer;
        RHITextureViewRef environmentIBLIrradianceView;
        RHITextureViewRef environmentIBLPrefilteredView;
        RHITextureViewRef environmentIBLBRDFLUTView;
        RHISamplerRef environmentIBLSampler;
        RHIBufferRef exposureStateBuffer;
        RHITextureViewRef ambientOcclusionView;
        uint64 ambientOcclusionFieldHash = 0;
        RHITextureViewRef opaqueSceneSnapshotView;
        RHISamplerRef opaqueSceneSnapshotSampler;
        RHIDescriptorSetLayoutRef frameSetLayout;
        RHIDescriptorSetRef frameDescriptorSet;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return lightingPlanIdentity != 0 && viewConstantBuffer &&
                   directionalShadowView && directionalShadowSampler &&
                   lightConstantsBuffer && pointLightsBuffer &&
                   spotLightsBuffer && rayTracedShadowMaskView &&
                   clusterConstantsBuffer && clusterBuffer &&
                   clusterLightIndexBuffer && environmentIBLIrradianceView &&
                   environmentIBLPrefilteredView && environmentIBLBRDFLUTView &&
                   environmentIBLSampler && exposureStateBuffer &&
                   ambientOcclusionView && ambientOcclusionFieldHash != 0 &&
                   opaqueSceneSnapshotView && opaqueSceneSnapshotSampler &&
                   frameSetLayout && frameDescriptorSet;
        }

        [[nodiscard]] bool MatchesLightingPlan(uint64 expectedIdentity) const noexcept
        {
            return expectedIdentity != 0 && lightingPlanIdentity == expectedIdentity;
        }
    };

    struct ShadowDepthBiasState
    {
        float constantBias = 0.0f;
        float slopeScaledBias = 0.0f;
        float biasClamp = 0.0f;
    };

    /** @brief Direct-raster set-1/b0 ABI. */
    struct ObjectConstants
    {
        Mat4 world;
        Mat4 normalMatrix;
        Mat4 previousWorldViewProjection;
        Vec4 objectVelocityParams;
        Vec4 skinningParams;
    };

    /**
     * @brief GPU-scene-only compatibility payload required by its private b0.
     *
     * This intentionally preserves GPUSceneRaster.hlsli's historical fixed
     * palette tail without imposing that storage cost on direct object uploads.
     */
    struct alignas(16) GPUSceneLegacyObjectConstants
    {
        Mat4 world{};
        Mat4 normalMatrix{};
        Mat4 previousWorldViewProjection{};
        Vec4 objectVelocityParams{};
        Vec4 skinningParams{};
        Mat4 legacySkinningMatrices[RVX_GPU_SCENE_LEGACY_SKINNING_MATRIX_CAPACITY]{};
    };

    /** @brief Private set-1/b0 payload for one sealed GPU-scene raster record. */
    struct alignas(16) GPUSceneRasterObjectConstants
    {
        GPUSceneLegacyObjectConstants legacyObjectConstants{};
        std::array<uint32, 4> gpuSceneRasterCounts{};
    };

    static_assert(sizeof(ObjectConstants) == 224u);
    static_assert(offsetof(GPUSceneLegacyObjectConstants, world) ==
                  offsetof(ObjectConstants, world));
    static_assert(offsetof(GPUSceneLegacyObjectConstants, normalMatrix) ==
                  offsetof(ObjectConstants, normalMatrix));
    static_assert(offsetof(GPUSceneLegacyObjectConstants, previousWorldViewProjection) ==
                  offsetof(ObjectConstants, previousWorldViewProjection));
    static_assert(offsetof(GPUSceneLegacyObjectConstants, objectVelocityParams) ==
                  offsetof(ObjectConstants, objectVelocityParams));
    static_assert(offsetof(GPUSceneLegacyObjectConstants, skinningParams) ==
                  offsetof(ObjectConstants, skinningParams));
    static_assert(offsetof(GPUSceneLegacyObjectConstants, legacySkinningMatrices) ==
                  sizeof(ObjectConstants));
    static_assert(offsetof(GPUSceneRasterObjectConstants, legacyObjectConstants) == 0);
    static_assert(offsetof(GPUSceneRasterObjectConstants, gpuSceneRasterCounts) ==
                  sizeof(GPUSceneLegacyObjectConstants));

    /**
     * @brief Strongly-owned private GPU-scene raster binding state for one seal.
     *
     * This has no route-selection behavior.  It only preserves the resources
     * that a later renderer pass must bind at set 1 and retain for submission.
     */
    struct GPUSceneRasterBindingSnapshot
    {
        RHIBufferRef objectConstantBuffer;
        RHIBufferRef candidateBuffer;
        RHIBufferRef primitiveBuffer;
        RHIBufferRef transformBuffer;
        RHIDescriptorSetRef objectDescriptorSet;
        RHIDescriptorSetLayoutRef frameSetLayout;
        RHIDescriptorSetLayoutRef objectSetLayout;
        RHIDescriptorSetLayoutRef materialSetLayout;
        RHIPipelineLayoutRef pipelineLayout;
        RHIPipelineRef opaquePipeline;
        RHIPipelineRef instancedMaterialOpaquePipeline;
        RHIPipelineRef maskedPipeline;
        RHIPipelineRef depthPipeline;
        uint64 leaseVersion = 0;
        uint32 candidateCount = 0;
        uint32 primitiveCapacity = 0;
        uint32 transformCapacity = 0;

        [[nodiscard]] bool IsReadyForBinding() const;
        [[nodiscard]] bool RetainSubmissionResources(
            RenderSubmissionResourceBatch& batch) const;
    };

    /**
     * @brief Per-recording frame/object bindings used by graphics passes.
     *
     * The regular PipelineCache bindings are frame-global mutable rings.  A
     * graph recording that may execute after another recording therefore needs
     * private constant buffers and immutable descriptor-set snapshots.  The
     * snapshot owns both buffers/sets and assigns fixed object offsets without
     * touching the cache's current-frame cursors.
     */
    struct RasterDrawBindingSnapshot
    {
        RHIBufferRef viewConstantBuffer;
        RHIBufferRef objectConstantBuffer;
        SkinningPaletteBufferBinding skinningPalettes;
        RHIDescriptorSetRef frameDescriptorSet;
        RHIDescriptorSetRef objectDescriptorSet;
        std::vector<Ref<RefCounted>> retainedResources;
        uint64 objectConstantStride = 0;
        uint32 objectCapacity = 0;
        uint32 objectCursor = 0;
        bool hasExplicitSkinningPalettes = false;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return viewConstantBuffer && objectConstantBuffer &&
                   skinningPalettes.IsWellFormed() && skinningPalettes.HasPalettes() &&
                   frameDescriptorSet && objectDescriptorSet &&
                   objectConstantStride != 0 && objectCapacity != 0;
        }
    };

    /**
     * @brief Record-owned set-1 bindings with no set-0/frame dependency.
     *
     * Transparent raster work binds a LightingBindingSnapshot at set 0. This
     * companion owns only object constants and its optional instance fallback,
     * so it cannot observe mutable frame descriptors or IBL state.
     */
    struct RasterObjectBindingSnapshot
    {
        RHIBufferRef objectConstantBuffer;
        SkinningPaletteBufferBinding skinningPalettes;
        RHIDescriptorSetRef objectDescriptorSet;
        std::vector<Ref<RefCounted>> retainedResources;
        uint64 objectConstantStride = 0;
        uint32 objectCapacity = 0;
        uint32 objectCursor = 0;
        bool hasExplicitSkinningPalettes = false;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return objectConstantBuffer && skinningPalettes.IsWellFormed() &&
                   skinningPalettes.HasPalettes() && objectDescriptorSet &&
                   objectConstantStride != 0 && objectCapacity != 0;
        }
    };

    /**
     * @brief Immutable frame bindings for all cascades of one directional shadow record.
     *
     * A single upload page contains one aligned ViewConstants record per
     * cascade. Each descriptor set fixes binding 0 to a distinct range in that
     * page, so command recording cannot observe a later cascade overwrite.
     */
    struct DirectionalShadowCascadeBindingSnapshot
    {
        RHIBufferRef viewConstantPageBuffer;
        std::vector<RHIDescriptorSetRef> frameDescriptorSets;
        std::vector<Ref<RefCounted>> retainedResources;
        uint64 viewConstantStride = 0;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return viewConstantPageBuffer && !frameDescriptorSets.empty() &&
                   viewConstantStride != 0;
        }
    };

    /** @brief One page-local object binding captured while a graph is recorded. */
    struct ObjectConstantBinding
    {
        RHIBufferRef constantBuffer;
        RHIBufferRef instanceBuffer;
        SkinningPaletteBufferBinding skinningPalettes;
        RHIDescriptorSetRef descriptorSet;
        uint64 pageIdentity = 0;
        std::array<uint32, 1> dynamicOffsets = {0};
        bool requiresInstanceBuffer = false;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return constantBuffer && skinningPalettes.IsWellFormed() &&
                   skinningPalettes.HasPalettes() && descriptorSet && pageIdentity != 0 &&
                   (!requiresInstanceBuffer || instanceBuffer);
        }
    };

    struct PipelineCacheConfig
    {
        RHIFormat renderTargetFormat = RHIFormat::RGBA8_UNORM;
        RHIFormat postProcessIntermediateFormat = RHIFormat::RGBA8_UNORM;
        RHIFormat toneMappingOutputFormat = RHIFormat::RGBA8_UNORM;
        RHIFormat depthStencilFormat = RHIFormat::D32_FLOAT;
        bool reverseZ = false;
        std::filesystem::path manifestDirectory;
    };

    struct PipelineCacheStats
    {
        uint64 lastPipelineStateHash = 0;
        uint64 opaquePipelineHash = 0;
        uint64 maskedPipelineHash = 0;
        uint64 transparentPipelineHash = 0;
        uint64 skyboxPipelineHash = 0;
        uint64 toneMappingPipelineHash = 0;
        uint64 screenSpaceOutlinePipelineHash = 0;
        uint64 screenSpaceOutlineVerifierPipelineHash = 0;
        uint64 bloomPipelineHash = 0;
        uint64 gtaoSurfacePipelineHash = 0;
        uint64 gtaoHorizonPipelineHash = 0;
        uint64 gtaoBilateralHorizontalPipelineHash = 0;
        uint64 gtaoBilateralVerticalPipelineHash = 0;
        uint64 gtaoTemporalPipelineHash = 0;
        uint64 colorGradingPipelineHash = 0;
        uint64 chromaticAberrationPipelineHash = 0;
        uint64 filmGrainPipelineHash = 0;
        uint64 fxaaPipelineHash = 0;
        uint64 taaResolvePipelineHash = 0;
        uint64 taaHistoryStorePipelineHash = 0;
        uint64 vignettePipelineHash = 0;
        uint64 uiPipelineHash = 0;
        uint32 pipelineCreateCount = 0;
        uint32 pipelineCacheHitCount = 0;
        uint32 pipelineCacheMissCount = 0;
        bool manifestLoaded = false;
        bool manifestValid = false;
        bool manifestInvalidated = false;
    };

    /**
     * @brief Pipeline cache for graphics pipelines
     *
     * The default material pipeline uses three descriptor sets:
     * set 0 = frame, set 1 = object, set 2 = material.
     */
    class PipelineCache
    {
    public:
        PipelineCache();
        /** @brief Create a cache borrowing the renderer-owned program provider. */
        explicit PipelineCache(IShaderProgramProvider& shaderProgramProvider);
        ~PipelineCache();

        PipelineCache(const PipelineCache&) = delete;
        PipelineCache& operator=(const PipelineCache&) = delete;

        // =====================================================================
        // Initialization
        // =====================================================================

        /**
         * @brief Initialize the pipeline cache
         * @param device RHI device
         * @param shaderDir Directory containing shader files
         * @return true if initialization succeeded
         */
        bool Initialize(IRHIDevice* device, const std::string& shaderDir);

        /**
         * @brief Shutdown and release resources
         */
        void Shutdown();

        /** @brief Transfer replaced descriptor snapshots using exact submission evidence. */
        void RetireOwnerSnapshots(const GPUCompletionToken& completion,
                                  RenderRetirementQueue& retirement);

        /** @brief Completion evidence for the private paged object upload arena. */
        void SetObjectConstantSubmissionTracker(RenderSubmissionTracker* tracker) noexcept;
        [[nodiscard]] bool NotifyObjectConstantSubmission(const GPUCompletionToken& completion) noexcept;
        void ReleaseUnsubmittedObjectConstants() noexcept;

        /**
         * @brief Check if initialized
         */
        bool IsInitialized() const { return m_initialized; }

        /**
         * @brief Configure pipeline cache inputs. Must be called before Initialize().
         */
        void SetConfig(const PipelineCacheConfig& config);

        const PipelineCacheConfig& GetConfig() const { return m_config; }
        const PipelineCacheStats& GetStats() const { return m_stats; }
        const std::string& GetLastError() const { return m_lastError; }

        uint64 GetPipelineStateHashForVariant(MaterialPipelineVariant variant) const;

        static constexpr const char* GetManifestFileName() { return "PipelineCacheManifest.txt"; }
        static constexpr RHIFormat GetDefaultDepthStencilFormat() { return RHIFormat::D32_FLOAT; }
        static constexpr float GetDepthClearValue(bool reverseZ) { return reverseZ ? 0.0f : 1.0f; }
        static RHIDepthStencilState BuildDepthStencilState(bool reverseZ, bool depthWrite);
        float GetDepthClearValue() const { return GetDepthClearValue(m_config.reverseZ); }

        // =====================================================================
        // Pipeline Access
        // =====================================================================

        /**
         * @brief Get the default opaque pipeline
         * @return Graphics pipeline or nullptr if not available
         */
        RHIPipeline* GetOpaquePipeline() const { return m_opaquePipeline.Get(); }

        /**
         * @brief Get the alpha-masked pipeline
         * @return Graphics pipeline or nullptr if not available
         */
        RHIPipeline* GetMaskedPipeline() const { return m_maskedPipeline.Get(); }

        /**
         * @brief Get the alpha-blended transparent pipeline
         * @return Graphics pipeline or nullptr if not available
         */
        RHIPipeline* GetTransparentPipeline() const { return m_transparentPipeline.Get(); }
        [[nodiscard]] RHISamplerRef GetEnvironmentIBLSamplerRef() const
        {
            return m_environmentIBLSampler;
        }

        /**
         * @brief Get a pipeline for a material variant
         */
        RHIPipeline* GetPipelineForVariant(MaterialPipelineVariant variant) const;
        RHIPipeline* GetPipelineForVariant(MaterialPipelineVariant variant, RHIFormat renderTargetFormat);
        RHIPipeline* GetPipelineForVariant(MaterialPipelineVariant variant,
                                           RHIFormat renderTargetFormat,
                                           DefaultLitDirectVertexInputMode inputMode,
                                           MaterialCullMode cullMode = MaterialCullMode::Back,
                                           bool reverseWinding = false);
        /**
         * Get the transparent HDR + reactive-mask MRT pipeline for the exact
         * material blend policy.  This is intentionally separate from the
         * legacy one-target Transparent variant.
         */
        RHIPipeline* GetTransparentMrtPipeline(
            RHIFormat hdrFormat,
            DefaultLitDirectVertexInputMode inputMode,
            MaterialCullMode cullMode,
            bool reverseWinding,
            MaterialBlendMode blendMode);
        RHIPipeline* GetGPUDrivenPipelineForVariant(
            MaterialPipelineVariant variant,
            RHIFormat renderTargetFormat,
            MaterialCullMode cullMode = MaterialCullMode::Back,
            bool reverseWinding = false);
        RHIPipeline* GetInstancedMaterialPipelineForVariant(
            MaterialPipelineVariant variant,
            RHIFormat renderTargetFormat,
            MaterialCullMode cullMode = MaterialCullMode::Back,
            bool reverseWinding = false);

        /**
         * @brief Get the GPU-scene raster pipeline for opaque or masked materials.
         *
         * This uses a private set-1 layout and never mutates the Direct/Tier1
         * object descriptor set or its instance-buffer binding.
         */
        RHIPipeline* GetGPUScenePipelineForVariant(MaterialPipelineVariant variant,
                                                    RHIFormat renderTargetFormat,
                                                    MaterialCullMode cullMode = MaterialCullMode::Back,
                                                    bool reverseWinding = false);
        /** @brief GPU-scene opaque pipeline sourcing scalar material parameters per instance. */
        RHIPipeline* GetGPUSceneInstancedMaterialPipelineForVariant(
            MaterialPipelineVariant variant,
            RHIFormat renderTargetFormat,
            MaterialCullMode cullMode = MaterialCullMode::Back,
            bool reverseWinding = false);

        /**
         * @brief Forward HDR pipelines that also export canonical surface data.
         *
         * The attachment ABI is scene HDR, RGBA8 oct-normal/roughness/metallic,
         * and R32_UINT material-table identity. These variants are selected
         * only when a typed outline consumer is requested for the view.
         */
        RHIPipeline* GetForwardSurfaceDataPipelineForVariant(
            MaterialPipelineVariant variant,
            RHIFormat renderTargetFormat,
            DefaultLitDirectVertexInputMode inputMode,
            MaterialCullMode cullMode = MaterialCullMode::Back,
            bool reverseWinding = false);
        RHIPipeline* GetGPUDrivenForwardSurfaceDataPipelineForVariant(
            MaterialPipelineVariant variant,
            RHIFormat renderTargetFormat,
            MaterialCullMode cullMode = MaterialCullMode::Back,
            bool reverseWinding = false);
        RHIPipeline* GetInstancedMaterialForwardSurfaceDataPipelineForVariant(
            MaterialPipelineVariant variant,
            RHIFormat renderTargetFormat,
            MaterialCullMode cullMode = MaterialCullMode::Back,
            bool reverseWinding = false);
        RHIPipeline* GetGPUSceneForwardSurfaceDataPipelineForVariant(
            MaterialPipelineVariant variant,
            RHIFormat renderTargetFormat,
            MaterialCullMode cullMode = MaterialCullMode::Back,
            bool reverseWinding = false);
        RHIPipeline*
        GetGPUSceneInstancedMaterialForwardSurfaceDataPipelineForVariant(
            MaterialPipelineVariant variant,
            RHIFormat renderTargetFormat,
            MaterialCullMode cullMode = MaterialCullMode::Back,
            bool reverseWinding = false);

        [[nodiscard]] bool IsForwardSurfaceDataPipelineFamilyReady()
            const noexcept
        {
            return m_initialized && m_forwardSurfaceDataPixelShader &&
                   m_pipelineLayout;
        }

        /**
         * @brief Deferred GBuffer pipelines using the canonical four-target ABI.
         *
         * The formats are fixed by the renderer contract: RGBA8_UNORM_SRGB,
         * RGBA8_UNORM, RGBA16_FLOAT and R32_UINT. Only opaque and masked
         * StandardLit packets may select this family.
         */
        RHIPipeline* GetGBufferPipelineForVariant(
            MaterialPipelineVariant variant,
            DefaultLitDirectVertexInputMode inputMode,
            MaterialCullMode cullMode = MaterialCullMode::Back,
            bool reverseWinding = false);
        RHIPipeline* GetGPUDrivenGBufferPipelineForVariant(
            MaterialPipelineVariant variant,
            MaterialCullMode cullMode = MaterialCullMode::Back,
            bool reverseWinding = false);
        RHIPipeline* GetInstancedMaterialGBufferPipelineForVariant(
            MaterialPipelineVariant variant,
            MaterialCullMode cullMode = MaterialCullMode::Back,
            bool reverseWinding = false);
        RHIPipeline* GetGPUSceneGBufferPipelineForVariant(
            MaterialPipelineVariant variant,
            MaterialCullMode cullMode = MaterialCullMode::Back,
            bool reverseWinding = false);
        RHIPipeline* GetGPUSceneInstancedMaterialGBufferPipelineForVariant(
            MaterialPipelineVariant variant,
            MaterialCullMode cullMode = MaterialCullMode::Back,
            bool reverseWinding = false);

        [[nodiscard]] bool IsGBufferPipelineFamilyReady() const noexcept
        {
            return m_initialized && m_gBufferPixelShader && m_pipelineLayout;
        }

        /**
         * @brief Return the deferred opaque-lighting compute pipeline.
         *
         * The pipeline is created lazily so compatibility-path startup never
         * requests a compute pipeline.  DX12, Vulkan, and Metal callers must
         * treat a null result as a required deferred-recipe failure.
         */
        RHIPipeline* GetDeferredLightingPipeline();

        /** @brief Descriptor layout for deferred GBuffer SRVs and HDR UAV. */
        RHIDescriptorSetLayout* GetDeferredLightingSetLayout() const
        {
            return m_deferredLightingSetLayout.Get();
        }

        /** @brief True only after a native deferred-lighting compute pipeline exists. */
        [[nodiscard]] bool IsDeferredLightingPipelineReady() const noexcept
        {
            return m_initialized && m_deferredLightingPipeline &&
                   m_deferredLightingSetLayout &&
                   m_deferredLightingPipelineLayout;
        }

        /** Fixed HDR producer binding ABI for pre-exposure state and GTAO visibility. */
        static constexpr uint32 DefaultLitExposureStateBinding = 14u;
        static constexpr uint32 DefaultLitAmbientOcclusionBinding = 15u;
        static constexpr uint32 DefaultLitOpaqueSceneSnapshotBinding = 16u;
        static constexpr uint32 DeferredLightingExposureStateBinding = 21u;
        static constexpr uint32 DeferredLightingAmbientOcclusionBinding = 22u;
        static constexpr uint32 SkyboxExposureStateBinding = 3u;

        RHIPipeline* GetExposureHistogramClearPipeline();
        RHIPipeline* GetExposureHistogramBuildPipeline();
        RHIPipeline* GetExposureAdaptationPipeline();
        RHIDescriptorSetLayout* GetExposureComputeSetLayout() const
        {
            return m_exposureComputeSetLayout.Get();
        }
        [[nodiscard]] bool IsExposureComputePipelineReady() const noexcept
        {
            return m_initialized && m_exposureComputeSetLayout &&
                m_exposureComputePipelineLayout &&
                m_exposureHistogramClearPipeline &&
                m_exposureHistogramBuildPipeline &&
                m_exposureAdaptationPipeline;
        }

        /**
         * @brief Get the depth-only pipeline for depth prepass
         * @return Depth-only pipeline or nullptr if not available
         */
        RHIPipeline* GetDepthOnlyPipeline() const { return m_depthOnlyPipeline.Get(); }
        RHIPipeline* GetDepthOnlyPipeline(
            DefaultLitDirectVertexInputMode inputMode,
            MaterialCullMode cullMode = MaterialCullMode::Back,
            bool reverseWinding = false);
        /** @brief Get the alpha-masked depth-only pipeline. */
        RHIPipeline* GetMaskedDepthOnlyPipeline() const
        {
            return m_maskedDepthOnlyPipeline.Get();
        }
        RHIPipeline* GetMaskedDepthOnlyPipeline(
            DefaultLitDirectVertexInputMode inputMode,
            MaterialCullMode cullMode = MaterialCullMode::Back,
            bool reverseWinding = false);
        RHIPipeline* GetGPUDrivenDepthOnlyPipeline(
            MaterialCullMode cullMode = MaterialCullMode::Back,
            bool reverseWinding = false);
        /** @brief Get the GPU-scene depth-only raster pipeline. */
        RHIPipeline* GetGPUSceneDepthOnlyPipeline(
            MaterialCullMode cullMode = MaterialCullMode::Back,
            bool reverseWinding = false);

        /** @brief Whether the independent GPU-scene raster shader/layout substrate is usable. */
        [[nodiscard]] bool IsGPUSceneRasterReady() const;
        /** @brief Why the optional GPU-scene raster substrate is unavailable. Empty when ready. */
        [[nodiscard]] const std::string& GetGPUSceneRasterUnavailableReason() const
        {
            return m_gpuSceneRasterUnavailableReason;
        }

        /**
         * @brief Get the shadow-map depth-only pipeline for caster raster bias
         */
        RHIPipeline* GetShadowDepthPipeline(const ShadowDepthBiasState& biasState);
        RHIPipeline* GetShadowDepthPipeline(
            const ShadowDepthBiasState& biasState,
            DefaultLitDirectVertexInputMode inputMode,
            MaterialCullMode cullMode = MaterialCullMode::Back,
            bool reverseWinding = false);
        /** @brief Alpha-tested shadow depth pipeline with material set 2. */
        RHIPipeline* GetMaskedShadowDepthPipeline(
            const ShadowDepthBiasState& biasState,
            DefaultLitDirectVertexInputMode inputMode,
            MaterialCullMode cullMode = MaterialCullMode::Back,
            bool reverseWinding = false);
        RHIPipeline* GetInstancedShadowDepthPipeline(
            const ShadowDepthBiasState& biasState,
            MaterialCullMode cullMode = MaterialCullMode::Back,
            bool reverseWinding = false);
        static ShadowDepthBiasState SanitizeShadowDepthBiasState(const ShadowDepthBiasState& biasState);

        /**
         * @brief Get the procedural skybox fullscreen pipeline
         */
        RHIPipeline* GetSkyboxPipeline() const { return m_skyboxPipeline.Get(); }
        RHIPipeline* GetSkyboxPipeline(RHIFormat outputFormat);
        RHIPipeline* GetSkyboxPipeline(RHIFormat outputFormat, bool depthTest);

        /**
         * @brief Get the fullscreen ToneMapping post-process pipeline
         */
        RHIPipeline* GetToneMappingPipeline() const { return m_toneMappingPipeline.Get(); }
        RHIPipeline* GetToneMappingPipeline(RHIFormat outputFormat);

        /**
         * @brief Get the typed HDR ToonLit screen-space outline pipeline.
         *
         * Screen-space outlines are defined only for the scene-linear
         * RGBA16_FLOAT surface consumed before ToneMapping. Other output
         * formats are rejected rather than silently changing color domains.
         */
        RHIPipeline* GetScreenSpaceOutlinePipeline(RHIFormat outputFormat);

        /**
         * @brief Get the qualification-only R8 mask verifier for screen-space outlines.
         *
         * This pipeline is intentionally separate from the visible outline
         * pass. It writes 0 for unchanged pixels, 1 for changed ToonLit pixels,
         * and 2 for changed non-Toon pixels.
         */
        RHIPipeline* GetScreenSpaceOutlineVerifierPipeline(RHIFormat outputFormat);

        /**
         * @brief Get the fullscreen Bloom post-process pipeline
         */
        RHIPipeline* GetBloomPipeline() const { return m_bloomPipeline.Get(); }
        RHIPipeline* GetBloomPipeline(RHIFormat outputFormat);
        RHIPipeline* GetBloomAdditivePipeline(RHIFormat outputFormat);

        /**
         * @brief Get the fail-closed Ground Truth Ambient Occlusion pipeline family.
         *
         * Surface preparation preserves exact device depth in R32_FLOAT.
         * Horizon, bilateral, and temporal visibility fields are
         * contractually R8_UNORM; callers cannot select arbitrary output
         * formats.
         */
        RHIPipeline* GetGTAOSurfacePipeline() const { return m_gtaoSurfacePipeline.Get(); }
        RHIPipeline* GetGTAOHorizonPipeline() const { return m_gtaoHorizonPipeline.Get(); }
        RHIPipeline* GetGTAOBilateralHorizontalPipeline() const
        {
            return m_gtaoBilateralHorizontalPipeline.Get();
        }
        RHIPipeline* GetGTAOBilateralVerticalPipeline() const
        {
            return m_gtaoBilateralVerticalPipeline.Get();
        }
        RHIPipeline* GetGTAOTemporalPipeline() const { return m_gtaoTemporalPipeline.Get(); }

        /**
         * @brief Get the camera/depth motion-vector pipeline
         */
        RHIPipeline* GetCameraVelocityPipeline() const { return m_cameraVelocityPipeline.Get(); }
        RHIPipeline* GetCameraVelocityPipeline(RHIFormat outputFormat);

        /**
         * @brief Get the object motion-vector pipeline
         */
        RHIPipeline* GetObjectVelocityPipeline() const { return m_objectVelocityPipeline.Get(); }
        RHIPipeline* GetObjectVelocityPipeline(RHIFormat outputFormat);
        RHIPipeline* GetObjectVelocityPipeline(
            RHIFormat outputFormat,
            DefaultLitDirectVertexInputMode inputMode,
            MaterialCullMode cullMode = MaterialCullMode::Back,
            bool reverseWinding = false);
        RHIPipeline* GetMaskedObjectVelocityPipeline() const { return m_maskedObjectVelocityPipeline.Get(); }
        RHIPipeline* GetMaskedObjectVelocityPipeline(RHIFormat outputFormat);
        RHIPipeline* GetMaskedObjectVelocityPipeline(
            RHIFormat outputFormat,
            DefaultLitDirectVertexInputMode inputMode,
            MaterialCullMode cullMode = MaterialCullMode::Back,
            bool reverseWinding = false);

        /**
         * @brief Get the fullscreen alpha-composite pipeline for ray-traced reflections
         */
        RHIPipeline* GetRayTracedReflectionCompositePipeline() const
        {
            return m_rayTracedReflectionCompositePipeline.Get();
        }
        RHIPipeline* GetRayTracedReflectionCompositePipeline(RHIFormat outputFormat);

        /**
         * @brief Get the fullscreen spatial denoise pipeline for ray-traced reflections
         */
        RHIPipeline* GetRayTracedReflectionDenoisePipeline() const
        {
            return m_rayTracedReflectionDenoisePipeline.Get();
        }
        RHIPipeline* GetRayTracedReflectionDenoisePipeline(RHIFormat outputFormat);

        /**
         * @brief Get the fullscreen ColorGrading LDR post-process pipeline
         */
        RHIPipeline* GetColorGradingPipeline() const { return m_colorGradingPipeline.Get(); }
        RHIPipeline* GetColorGradingPipeline(RHIFormat outputFormat);

        /**
         * @brief Get the fullscreen ChromaticAberration LDR post-process pipeline
         */
        RHIPipeline* GetChromaticAberrationPipeline() const { return m_chromaticAberrationPipeline.Get(); }
        RHIPipeline* GetChromaticAberrationPipeline(RHIFormat outputFormat);

        /**
         * @brief Get the fullscreen FXAA LDR post-process pipeline
         */
        RHIPipeline* GetFXAAPipeline() const { return m_fxaaPipeline.Get(); }
        RHIPipeline* GetFXAAPipeline(RHIFormat outputFormat);

        /** @brief Get the HDR temporal resolve pipeline. */
        RHIPipeline* GetTAAResolvePipeline() const { return m_taaResolvePipeline.Get(); }
        RHIPipeline* GetTAAResolvePipeline(RHIFormat outputFormat);

        /** @brief Get the dual-target temporal history-store pipeline. */
        RHIPipeline* GetTAAHistoryStorePipeline() const
        {
            return m_taaHistoryStorePipeline.Get();
        }

        /**
         * @brief Get the fullscreen FilmGrain LDR post-process pipeline
         */
        RHIPipeline* GetFilmGrainPipeline() const { return m_filmGrainPipeline.Get(); }
        RHIPipeline* GetFilmGrainPipeline(RHIFormat outputFormat);

        /**
         * @brief Get the fullscreen Vignette LDR post-process pipeline
         */
        RHIPipeline* GetVignettePipeline() const { return m_vignettePipeline.Get(); }
        RHIPipeline* GetVignettePipeline(RHIFormat outputFormat);

        /**
         * @brief Get the RHI-backed UI overlay pipeline
         */
        RHIPipeline* GetUIPipeline() const { return m_uiPipeline.Get(); }
        RHIPipeline* GetUIPipeline(RHIFormat outputFormat);

        /**
         * @brief Get the ray-traced directional shadow pipeline, when supported by the backend.
         */
        RHIPipeline* GetRayTracedShadowPipeline() const { return m_rayTracedShadowPipeline.Get(); }

        /**
         * @brief Get the ray-traced reflection pipeline, when supported by the backend.
         */
        RHIPipeline* GetRayTracedReflectionPipeline() const { return m_rayTracedReflectionPipeline.Get(); }

        /**
         * @brief Get the shader table for the ray-traced directional shadow pipeline.
         */
        RHIShaderTable* GetRayTracedShadowShaderTable() const { return m_rayTracedShadowShaderTable.Get(); }

        /**
         * @brief Get the shader table for the ray-traced reflection pipeline.
         */
        RHIShaderTable* GetRayTracedReflectionShaderTable() const { return m_rayTracedReflectionShaderTable.Get(); }

        /**
         * @brief Get the default pipeline layout
         */
        RHIPipelineLayout* GetDefaultLayout() const { return m_pipelineLayout.Get(); }

        /** @brief Get the GPU-scene raster layout (frame, private object, material). */
        RHIPipelineLayout* GetGPUSceneRasterLayout() const
        {
            return m_gpuSceneRasterPipelineLayout.Get();
        }
        /** @brief Get the private set-1 b0 + candidate/primitive/transform layout. */
        RHIDescriptorSetLayout* GetGPUSceneRasterObjectSetLayout() const
        {
            return m_gpuSceneRasterObjectSetLayout.Get();
        }

        /**
         * @brief Get the post-process fullscreen pipeline layout
         */
        RHIPipelineLayout* GetPostProcessLayout() const { return m_postProcessPipelineLayout.Get(); }

        /** Layout for fullscreen consumers that read the candidate exposure state. */
        RHIPipelineLayout* GetExposurePostProcessLayout() const
        {
            return m_exposurePostProcessPipelineLayout.Get();
        }

        /** @brief Layout for the temporal resolve and history-store fullscreen passes. */
        RHIPipelineLayout* GetTAALayout() const { return m_taaPipelineLayout.Get(); }

        /** @brief Layout for the five-pass GTAO reconstruction and filtering family. */
        RHIPipelineLayout* GetGTAOLayout() const { return m_gtaoPipelineLayout.Get(); }

        /** @brief Layout for the typed screen-space outline HDR pass. */
        RHIPipelineLayout* GetScreenSpaceOutlineLayout() const
        {
            return m_screenSpaceOutlinePipelineLayout.Get();
        }

        /** @brief Layout for the qualification-only outline verifier. */
        RHIPipelineLayout* GetScreenSpaceOutlineVerifierLayout() const
        {
            return m_screenSpaceOutlineVerifierPipelineLayout.Get();
        }

        /**
         * @brief Get the runtime UI overlay pipeline layout
         */
        RHIPipelineLayout* GetUILayout() const { return m_uiPipelineLayout.Get(); }
        RHIDescriptorSetLayout* GetUITextureSetLayout() const { return m_uiTextureSetLayout.Get(); }

        /**
         * @brief Get the ray-traced reflection denoise fullscreen pipeline layout
         */
        RHIPipelineLayout* GetRayTracedReflectionDenoiseLayout() const
        {
            return m_rayTracedReflectionDenoisePipelineLayout.Get();
        }

        /**
         * @brief Get the procedural skybox fullscreen pipeline layout
         */
        RHIPipelineLayout* GetSkyboxLayout() const { return m_skyboxPipelineLayout.Get(); }

        /**
         * @brief Get the ray-traced shadow global pipeline layout.
         */
        RHIPipelineLayout* GetRayTracedShadowLayout() const { return m_rayTracedShadowPipelineLayout.Get(); }

        /**
         * @brief Get the ray-traced reflection global pipeline layout.
         */
        RHIPipelineLayout* GetRayTracedReflectionLayout() const { return m_rayTracedReflectionPipelineLayout.Get(); }

        /**
         * @brief Get the descriptor set layout used by fullscreen post-process passes
         */
        RHIDescriptorSetLayout* GetPostProcessSetLayout() const { return m_postProcessSetLayout.Get(); }

        RHIDescriptorSetLayout* GetExposurePostProcessSetLayout() const
        {
            return m_exposurePostProcessSetLayout.Get();
        }

        /** @brief Descriptor layout for current/history/depth/velocity/reactive TAA inputs. */
        RHIDescriptorSetLayout* GetTAASetLayout() const { return m_taaSetLayout.Get(); }

        /** @brief Descriptor layout for current surface, AO filtering, and temporal inputs. */
        RHIDescriptorSetLayout* GetGTAOSetLayout() const { return m_gtaoSetLayout.Get(); }

        /** @brief Descriptor layout for screen HDR/depth/normal/material outline inputs. */
        RHIDescriptorSetLayout* GetScreenSpaceOutlineSetLayout() const
        {
            return m_screenSpaceOutlineSetLayout.Get();
        }

        /** @brief Descriptor ABI for the qualification-only outline verifier. */
        RHIDescriptorSetLayout* GetScreenSpaceOutlineVerifierSetLayout() const
        {
            return m_screenSpaceOutlineVerifierSetLayout.Get();
        }

        /**
         * @brief Get the descriptor set layout used by ray-traced reflection denoise pass
         */
        RHIDescriptorSetLayout* GetRayTracedReflectionDenoiseSetLayout() const
        {
            return m_rayTracedReflectionDenoiseSetLayout.Get();
        }

        /**
         * @brief Get the descriptor set layout used by the procedural skybox pass
         */
        RHIDescriptorSetLayout* GetSkyboxSetLayout() const { return m_skyboxSetLayout.Get(); }

        /**
         * @brief Get the descriptor set layout used by ray-traced shadow dispatch.
         */
        RHIDescriptorSetLayout* GetRayTracedShadowSetLayout() const { return m_rayTracedShadowSetLayout.Get(); }

        /**
         * @brief Get the descriptor set layout used by ray-traced reflection dispatch.
         */
        RHIDescriptorSetLayout* GetRayTracedReflectionSetLayout() const { return m_rayTracedReflectionSetLayout.Get(); }

        /**
         * @brief Get the owning RHI device for pass-local resources
         */
        IRHIDevice* GetDevice() const { return m_device; }

        // =====================================================================
        // Descriptor Sets
        // =====================================================================

        /**
         * @brief Reset transient per-draw constant slots for a new frame
         */
        void BeginFrame();

        /**
         * @brief Reset frame-scope resource bindings to internal fallbacks.
         */
        void ResetFrameResourceBindings();

        /**
         * @brief Get the frame constants descriptor set (set 0)
         */
        RHIDescriptorSet* GetFrameDescriptorSet();

        /** @brief Acquire the current frame-set snapshot for submission ownership. */
        RHIDescriptorSetRef GetFrameDescriptorSetSnapshot() const
        {
            return m_frameDescriptorSet;
        }

        /**
         * @brief Update frame-scope directional shadow texture/sampler bindings.
         */
        DirectionalShadowFrameBindingResult UpdateDirectionalShadowFrameResources(
            const DirectionalShadowFrameResources& resources);

        /**
         * @brief Update frame-scope ray-traced shadow mask binding.
         */
        RayTracedShadowFrameBindingResult UpdateRayTracedShadowFrameResources(
            const RayTracedShadowFrameResources& resources);

        /**
         * @brief Update frame/view-scope environment IBL texture bindings.
         */
        EnvironmentIBLFrameBindingResult UpdateEnvironmentIBLFrameResources(
            const EnvironmentIBLFrameResources& resources);

        /**
         * @brief Update frame-scope local light buffer bindings.
         */
        FrameLightBindingResult UpdateFrameLightResources(const FrameLightResources& resources);

        /** Publish the next record's committed structured exposure state. */
        [[nodiscard]] bool UpdateFrameExposureStateBuffer(RHIBuffer* exposureStateBuffer);
        [[nodiscard]] RHIBuffer* GetFrameExposureStateBuffer() const noexcept
        {
            return m_currentExposureStateBuffer.Get();
        }

        /**
         * @brief Create a complete immutable set-0 snapshot for one lighting plan.
         *
         * `view.directionalShadowEnabled` and `view.textureIBLEnabled` are
         * readiness declarations.  When either is enabled, its matching
         * request resources must be complete; otherwise creation fails without
         * publishing a partial snapshot.  Disabled shadow/IBL paths bind only
         * stable internal fallbacks.  Local and clustered-light buffers are
         * always required, including frames with zero admitted lights.  The
         * output argument changes only after a complete descriptor set is
         * ready for binding.
         */
        [[nodiscard]] bool CreateLightingBindingSnapshot(
            const ViewData& view,
            const LightingBindingSnapshotRequest& request,
            LightingBindingSnapshot& outSnapshot);

        /** @brief Freeze the six required upload buffers before graph composition. */
        [[nodiscard]] bool FreezeFrameLightResources(
            const FrameLightResources& source,
            FrozenFrameLightResources& outFrozenResources);

        const DirectionalShadowFrameBindingResult& GetLastDirectionalShadowFrameBindingResult() const
        {
            return m_lastDirectionalShadowFrameBindingResult;
        }

        const FrameLightBindingResult& GetLastFrameLightBindingResult() const
        {
            return m_lastFrameLightBindingResult;
        }

        const RayTracedShadowFrameBindingResult& GetLastRayTracedShadowFrameBindingResult() const
        {
            return m_lastRayTracedShadowFrameBindingResult;
        }

        const EnvironmentIBLFrameBindingResult& GetLastEnvironmentIBLFrameBindingResult() const
        {
            return m_lastEnvironmentIBLFrameBindingResult;
        }

        static const char* GetDirectionalShadowFallbackReasonName(DirectionalShadowFallbackReason reason);
        static const char* GetRayTracedShadowFallbackReasonName(RayTracedShadowFallbackReason reason);
        static const char* GetEnvironmentIBLFallbackReasonName(EnvironmentIBLFallbackReason reason);
        static const char* GetFrameLightFallbackReasonName(FrameLightFallbackReason reason);

        /**
         * @brief Backward-compatible alias for frame constants
         */
        RHIDescriptorSet* GetViewDescriptorSet() { return GetFrameDescriptorSet(); }

        /**
         * @brief Get the object constants descriptor set (set 1)
         */
        RHIDescriptorSet* GetObjectDescriptorSet();
        RHIDescriptorSetLayout* GetObjectSetLayout() const;

        /**
         * @brief Bind the GPU-driven instance buffer in the object descriptor set.
         *
         * Passing nullptr restores the internal fallback binding. The dynamic
         * object constants binding remains valid for non-GPU-driven draws.
         */
        bool UpdateObjectInstanceBuffer(RHIBuffer* instanceBuffer);

        /**
         * @brief Allocate one completion-tracked page-local object constant record.
         *
         * The returned binding owns b0 from the selected page and preserves
         * set-1/b1 as either the supplied instance buffer or the stable fallback.
         * Set-1/b2 and b3 bind the exact current/previous palette pair selected
         * for this record (or stable identity buffers for rigid objects).
         */
        bool CreateObjectConstantBinding(
            const Mat4& worldMatrix,
            const Mat4& normalMatrix,
            const Mat4& previousWorldMatrix,
            const Mat4& previousViewProjectionMatrix,
            bool previousWorldViewProjectionValid,
            bool receivesShadow,
            const SkinningPaletteBufferBinding& skinningPalettes,
            const SkinningPaletteSliceBinding& skinningSlice,
            RHIBuffer* instanceBuffer,
            ObjectConstantBinding& outBinding);

        /**
         * @brief Get the material descriptor set layout (set 2)
         */
        RHIDescriptorSetLayout* GetMaterialSetLayout() const;

        /**
         * @brief Get dynamic constant-buffer offset for the current object slot
         */
        std::array<uint32, 1> GetCurrentObjectDynamicOffset() const;

        /** @brief Create immutable per-record frame/object bindings. */
        bool CreateRasterDrawBindingSnapshot(const ViewData& view,
                                             uint32 objectCapacity,
                                             RasterDrawBindingSnapshot& outSnapshot) const;

        /** @brief Create immutable per-record frame/object bindings with exact palettes. */
        bool CreateRasterDrawBindingSnapshot(
            const ViewData& view,
            uint32 objectCapacity,
            const SkinningPaletteBufferBinding& skinningPalettes,
            RasterDrawBindingSnapshot& outSnapshot) const;

        /** @brief Create record-owned set-1 bindings without reading set 0. */
        bool CreateRasterObjectBindingSnapshot(
            uint32 objectCapacity,
            RasterObjectBindingSnapshot& outSnapshot) const;

        /** @brief Create record-owned set-1 bindings with exact palettes. */
        bool CreateRasterObjectBindingSnapshot(
            uint32 objectCapacity,
            const SkinningPaletteBufferBinding& skinningPalettes,
            RasterObjectBindingSnapshot& outSnapshot) const;

        /**
         * @brief Create immutable set-0 bindings for every directional-shadow cascade.
         *
         * All cascade constants are copied into one recording-owned page before
         * descriptors are created. The caller must retain retainedResources in
         * the RenderGraph execution ownership batch.
         */
        bool CreateDirectionalShadowCascadeBindingSnapshot(
            std::span<const ViewData> cascadeViews,
            DirectionalShadowCascadeBindingSnapshot& outSnapshot) const;

        /**
         * @brief Create the independent set-1 binding for one sealed GPU-scene raster view.
         *
         * The snapshot supplies its own strong b0/descriptor/table references;
         * this never updates the cache's normal object buffer or descriptor.
         */
        bool CreateGPUSceneRasterBindingSnapshot(
            const GPUSceneRasterResourceSnapshot& resources,
            const ObjectConstants& objectConstants,
            GPUSceneRasterBindingSnapshot& outSnapshot);

        /**
         * @brief Create isolated transparent frame/object bindings.
         *
         * Local-light and cluster buffers are single mutable upload allocations.
         * This overload copies their contents into the recording-owned frame
         * descriptor so later frames cannot rewrite an already registered graph.
         * Directional and ray-traced shadow sampling remain explicitly disabled
         * for transparent shading.
         */
        bool CreateTransparentRasterDrawBindingSnapshot(
            const ViewData& view,
            uint32 objectCapacity,
            const FrameLightResources& lightResources,
            RasterDrawBindingSnapshot& outSnapshot) const;

        /** @brief Create transparent record bindings with exact palette resources. */
        bool CreateTransparentRasterDrawBindingSnapshot(
            const ViewData& view,
            uint32 objectCapacity,
            const FrameLightResources& lightResources,
            const SkinningPaletteBufferBinding& skinningPalettes,
            RasterDrawBindingSnapshot& outSnapshot) const;

        /** @brief Upload one object slot in a recording-owned snapshot. */
        bool UpdateRasterDrawBindingSnapshotObject(
            RasterDrawBindingSnapshot& snapshot,
            const Mat4& worldMatrix,
            const Mat4& normalMatrix,
            const Mat4& previousWorldMatrix,
            const Mat4& previousViewProjectionMatrix,
            bool previousWorldViewProjectionValid,
            bool receivesShadow,
            const SkinningPaletteSliceBinding& skinningSlice = {}) const;

        /** @brief Upload an object slot in a set-1-only recording snapshot. */
        bool UpdateRasterObjectBindingSnapshotObject(
            RasterObjectBindingSnapshot& snapshot,
            const Mat4& worldMatrix,
            const Mat4& normalMatrix,
            const Mat4& previousWorldMatrix,
            const Mat4& previousViewProjectionMatrix,
            bool previousWorldViewProjectionValid,
            bool receivesShadow,
            const SkinningPaletteSliceBinding& skinningSlice = {}) const;

        /**
         * @brief Build a single dynamic offset span for descriptor sets with one dynamic binding
         */
        static std::array<uint32, 1> BuildSingleDynamicOffset(uint64 offset)
        {
            return {ToRHIConstantDynamicOffset(offset)};
        }

        /**
         * @brief Update view constants from ViewData
         * @param view The current view data
         */
        void UpdateViewConstants(const ViewData& view);

        /**
         * @brief Update per-object constants without previous-frame motion data.
         * @return True when a new constant slot was uploaded successfully.
         */
        bool UpdateObjectConstants(const Mat4& worldMatrix, const Mat4& normalMatrix);

        /**
         * @brief Update per-object constants with previous-frame motion data.
         * @return True when a new constant slot was uploaded successfully.
         */
        bool UpdateObjectConstants(const Mat4& worldMatrix,
                                   const Mat4& normalMatrix,
                                   const Mat4& previousWorldMatrix,
                                   const Mat4& previousViewProjectionMatrix,
                                   bool previousWorldViewProjectionValid);

        /** @brief Rigid-object convenience overload with explicit shadow policy. */
        bool UpdateObjectConstants(const Mat4& worldMatrix,
                                   const Mat4& normalMatrix,
                                   const Mat4& previousWorldMatrix,
                                   const Mat4& previousViewProjectionMatrix,
                                   bool previousWorldViewProjectionValid,
                                   bool receivesShadow);

        /** @brief Upload per-object constants with structured skin palette metadata. */
        bool UpdateObjectConstants(const Mat4& worldMatrix,
                                   const Mat4& normalMatrix,
                                   const Mat4& previousWorldMatrix,
                                   const Mat4& previousViewProjectionMatrix,
                                   bool previousWorldViewProjectionValid,
                                   const SkinningPaletteBufferBinding& skinningPalettes,
                                   const SkinningPaletteSliceBinding& skinningSlice);

        /**
         * @brief Update per-object constants with previous-frame motion and render flags.
         * @return True when a new constant slot was uploaded successfully.
         */
        bool UpdateObjectConstants(const Mat4& worldMatrix,
                                   const Mat4& normalMatrix,
                                   const Mat4& previousWorldMatrix,
                                   const Mat4& previousViewProjectionMatrix,
                                   bool previousWorldViewProjectionValid,
                                   bool receivesShadow,
                                   const SkinningPaletteBufferBinding& skinningPalettes,
                                   const SkinningPaletteSliceBinding& skinningSlice);

        // =====================================================================
        // Render Target Format
        // =====================================================================

        /**
         * @brief Set the render target format (call before Initialize or recreate pipeline)
         */
        void SetRenderTargetFormat(RHIFormat format)
        {
            m_renderTargetFormat = format;
            m_postProcessIntermediateFormat = format;
            m_toneMappingOutputFormat = format;
            m_config.renderTargetFormat = format;
            m_config.postProcessIntermediateFormat = format;
            m_config.toneMappingOutputFormat = format;
        }

        /**
         * @brief Set distinct render target formats for scene and post-process pipelines
         */
        void SetRenderTargetFormats(RHIFormat sceneFormat,
                                    RHIFormat postProcessIntermediateFormat,
                                    RHIFormat toneMappingOutputFormat)
        {
            m_renderTargetFormat = sceneFormat;
            m_postProcessIntermediateFormat = postProcessIntermediateFormat;
            m_toneMappingOutputFormat = toneMappingOutputFormat;
            m_config.renderTargetFormat = sceneFormat;
            m_config.postProcessIntermediateFormat = postProcessIntermediateFormat;
            m_config.toneMappingOutputFormat = toneMappingOutputFormat;
        }

        RHIFormat GetSceneRenderTargetFormat() const { return m_renderTargetFormat; }
        RHIFormat GetPostProcessIntermediateFormat() const { return m_postProcessIntermediateFormat; }
        RHIFormat GetToneMappingOutputFormat() const { return m_toneMappingOutputFormat; }

        void SetDepthStencilFormat(RHIFormat format) { m_config.depthStencilFormat = format; }
        void SetReverseZ(bool enabled) { m_config.reverseZ = enabled; }
        bool IsReverseZ() const { return m_config.reverseZ; }

    private:
        /**
         * @brief Strong owners for every source referenced by one frame descriptor set.
         *
         * RHIDescriptorBinding intentionally contains raw backend handles. This
         * snapshot keeps those handles live from descriptor creation through the
         * exact completion token used to retire a replaced frame set.
         */
        struct FrameDescriptorBindingOwners final : RefCounted
        {
            RHIBufferRef viewConstantBuffer;
            RHITextureViewRef directionalShadowView;
            RHISamplerRef directionalShadowSampler;
            RHIBufferRef lightConstantsBuffer;
            RHIBufferRef pointLightsBuffer;
            RHIBufferRef spotLightsBuffer;
            RHITextureViewRef rayTracedShadowMaskView;
            RHIBufferRef clusterConstantsBuffer;
            RHIBufferRef clusterBuffer;
            RHIBufferRef clusterLightIndexBuffer;
            RHITextureViewRef environmentIBLIrradianceView;
            RHITextureViewRef environmentIBLPrefilteredView;
            RHITextureViewRef environmentIBLBRDFLUTView;
            RHISamplerRef environmentIBLSampler;
            RHIBufferRef exposureStateBuffer;
            RHITextureViewRef ambientOcclusionView;
            RHITextureViewRef opaqueSceneSnapshotView;
            uint64 ambientOcclusionFieldHash = 0;

            [[nodiscard]] bool IsComplete() const noexcept
            {
                return viewConstantBuffer && directionalShadowView &&
                       directionalShadowSampler && lightConstantsBuffer &&
                       pointLightsBuffer && spotLightsBuffer &&
                       rayTracedShadowMaskView && clusterConstantsBuffer &&
                       clusterBuffer && clusterLightIndexBuffer &&
                       environmentIBLIrradianceView &&
                       environmentIBLPrefilteredView &&
                       environmentIBLBRDFLUTView && environmentIBLSampler &&
                       exposureStateBuffer && ambientOcclusionView &&
                       opaqueSceneSnapshotView &&
                       ambientOcclusionFieldHash != 0;
            }
        };

        static uint32 ToRHIConstantDynamicOffset(uint64 offset)
        {
            constexpr uint64 maxDynamicOffset = static_cast<uint64>(std::numeric_limits<uint32>::max());
            if (offset > maxDynamicOffset)
            {
                RVX_VERIFY(false, "PipelineCache: dynamic constant offset {} exceeds the RHI uint32 offset limit",
                           offset);
                return 0;
            }

            return static_cast<uint32>(offset);
        }

        bool CompileShaders();
        bool CreatePipelineLayout();
        bool CreateGPUSceneRasterPipelineLayout();
        bool CreateDeferredLightingPipelineLayout();
        bool CreatePostProcessPipelineLayout();
        bool CreateTAAPipelineLayout();
        bool CreateGTAOPipelineLayout();
        bool CreateScreenSpaceOutlinePipelineLayout();
        bool CreateScreenSpaceOutlineVerifierPipelineLayout();
        bool CreateUIPipelineLayout();
        bool CreateRayTracedReflectionDenoisePipelineLayout();
        bool CreateSkyboxPipelineLayout();
        bool CreateRayTracedShadowPipelineLayout();
        bool CreateRayTracedReflectionPipelineLayout();
        bool CreatePipeline();
        RHIPipelineRef GetOrCreateDefaultLitPipeline(MaterialPipelineVariant variant,
                                                     const char* debugName,
                                                     const RHIDepthStencilState& depthStencilState,
                                                     const RHIBlendState& blendState,
                                                      RHIFormat renderTargetFormat,
                                                      DefaultLitDirectVertexInputMode inputMode,
                                                      MaterialCullMode cullMode,
                                                      bool reverseWinding,
                                                      bool updatePrimaryStats,
                                                      RHIShader* pixelShaderOverride = nullptr,
                                                      bool reactiveMaskMrt = false);
        RHIPipelineRef GetOrCreateGPUDrivenDefaultLitPipeline(MaterialPipelineVariant variant,
                                                              const char* debugName,
                                                              const RHIDepthStencilState& depthStencilState,
                                                              const RHIBlendState& blendState,
                                                              RHIFormat renderTargetFormat,
                                                              MaterialCullMode cullMode,
                                                              bool reverseWinding);
        RHIPipelineRef GetOrCreateInstancedMaterialDefaultLitPipeline(
            MaterialPipelineVariant variant,
            const char* debugName,
            const RHIDepthStencilState& depthStencilState,
            const RHIBlendState& blendState,
            RHIFormat renderTargetFormat,
            MaterialCullMode cullMode,
            bool reverseWinding);
        RHIPipelineRef GetOrCreateGPUSceneDefaultLitPipeline(MaterialPipelineVariant variant,
                                                              const char* debugName,
                                                              const RHIDepthStencilState& depthStencilState,
                                                              const RHIBlendState& blendState,
                                                              RHIFormat renderTargetFormat,
                                                              bool instancedMaterial,
                                                              MaterialCullMode cullMode,
                                                              bool reverseWinding);
        RHIPipelineRef GetOrCreateGBufferPipeline(
            RHIGraphicsPipelineDesc pipelineDesc,
            MaterialPipelineVariant variant,
            uint32 purposeSalt,
            const char* debugName);
        RHIPipelineRef GetOrCreateForwardSurfaceDataPipeline(
            RHIGraphicsPipelineDesc pipelineDesc,
            MaterialPipelineVariant variant,
            uint32 purposeSalt,
            const char* debugName);
        bool EnsureDeferredLightingPipeline();
        [[nodiscard]] RHIGraphicsPipelineDesc MakeGBufferPipelineDesc(
            RHIGraphicsPipelineDesc pipelineDesc,
            const char* debugName) const;
        [[nodiscard]] RHIGraphicsPipelineDesc
        MakeForwardSurfaceDataPipelineDesc(
            RHIGraphicsPipelineDesc pipelineDesc,
            const char* debugName) const;
        RHIPipelineRef GetOrCreateDepthOnlyPipeline(
            MaterialCullMode cullMode = MaterialCullMode::Back,
            bool reverseWinding = false);
        RHIPipelineRef GetOrCreateRigidDepthOnlyPipeline(
            MaterialCullMode cullMode = MaterialCullMode::Back,
            bool reverseWinding = false);
        RHIPipelineRef GetOrCreateMaskedDepthOnlyPipeline(
            DefaultLitDirectVertexInputMode inputMode =
                DefaultLitDirectVertexInputMode::Skinned,
            MaterialCullMode cullMode = MaterialCullMode::Back,
            bool reverseWinding = false);
        RHIPipelineRef GetOrCreateGPUDrivenDepthOnlyPipeline(
            MaterialCullMode cullMode = MaterialCullMode::Back,
            bool reverseWinding = false);
        RHIPipelineRef GetOrCreateGPUSceneDepthOnlyPipeline(
            MaterialCullMode cullMode = MaterialCullMode::Back,
            bool reverseWinding = false);
        RHIPipelineRef GetOrCreateShadowDepthPipeline(
            const ShadowDepthBiasState& biasState,
            DefaultLitDirectVertexInputMode inputMode,
            MaterialCullMode cullMode,
            bool reverseWinding);
        RHIPipelineRef GetOrCreateMaskedShadowDepthPipeline(
            const ShadowDepthBiasState& biasState,
            DefaultLitDirectVertexInputMode inputMode,
            MaterialCullMode cullMode,
            bool reverseWinding);
        RHIPipelineRef GetOrCreateInstancedShadowDepthPipeline(
            const ShadowDepthBiasState& biasState,
            MaterialCullMode cullMode,
            bool reverseWinding);
        RHIPipelineRef GetOrCreateSkyboxPipeline(RHIFormat outputFormat,
                                                 bool depthTest = true,
                                                 bool updatePrimaryStats = true);
        RHIPipelineRef GetOrCreateToneMappingPipeline(RHIFormat outputFormat);
        RHIPipelineRef GetOrCreateScreenSpaceOutlinePipeline(RHIFormat outputFormat);
        RHIPipelineRef GetOrCreateScreenSpaceOutlineVerifierPipeline(
            RHIFormat outputFormat);
        RHIPipelineRef GetOrCreateBloomPipeline(RHIFormat outputFormat);
        RHIPipelineRef GetOrCreateBloomAdditivePipeline(RHIFormat outputFormat);
        RHIPipelineRef GetOrCreateGTAOSurfacePipeline();
        RHIPipelineRef GetOrCreateGTAOHorizonPipeline();
        RHIPipelineRef GetOrCreateGTAOBilateralHorizontalPipeline();
        RHIPipelineRef GetOrCreateGTAOBilateralVerticalPipeline();
        RHIPipelineRef GetOrCreateGTAOTemporalPipeline();
        RHIPipelineRef GetOrCreateGTAOPipeline(
            RHIShader* pixelShader,
            const char* debugName,
            const char* stageName,
            RHIFormat outputFormat,
            uint64& outPipelineHash);
        RHIPipelineRef GetOrCreateCameraVelocityPipeline(RHIFormat outputFormat);
        RHIPipelineRef GetOrCreateObjectVelocityPipeline(
            RHIFormat outputFormat,
            DefaultLitDirectVertexInputMode inputMode =
                DefaultLitDirectVertexInputMode::Skinned,
            MaterialCullMode cullMode = MaterialCullMode::Back,
            bool reverseWinding = false);
        RHIPipelineRef GetOrCreateMaskedObjectVelocityPipeline(
            RHIFormat outputFormat,
            DefaultLitDirectVertexInputMode inputMode =
                DefaultLitDirectVertexInputMode::Skinned,
            MaterialCullMode cullMode = MaterialCullMode::Back,
            bool reverseWinding = false);
        RHIPipelineRef GetOrCreateRayTracedReflectionCompositePipeline(RHIFormat outputFormat);
        RHIPipelineRef GetOrCreateRayTracedReflectionDenoisePipeline(RHIFormat outputFormat);
        RHIPipelineRef GetOrCreateColorGradingPipeline(RHIFormat outputFormat);
        RHIPipelineRef GetOrCreateChromaticAberrationPipeline(RHIFormat outputFormat);
        RHIPipelineRef GetOrCreateFilmGrainPipeline(RHIFormat outputFormat);
        RHIPipelineRef GetOrCreateFXAAPipeline(RHIFormat outputFormat);
        RHIPipelineRef GetOrCreateTAAResolvePipeline(RHIFormat outputFormat);
        RHIPipelineRef GetOrCreateTAAHistoryStorePipeline();
        RHIPipelineRef GetOrCreateVignettePipeline(RHIFormat outputFormat);
        RHIPipelineRef GetOrCreateUIPipeline(RHIFormat outputFormat);
        bool EnsureExposureComputePipelines();
        bool CreateExposureComputePipelineLayout();
        bool CreateExposurePostProcessPipelineLayout();
        bool CreateRayTracedShadowPipeline();
        bool CreateRayTracedReflectionPipeline();
        RHIGraphicsPipelineDesc BuildDefaultLitPipelineDesc(const char* debugName,
                                                            const RHIDepthStencilState& depthStencilState,
                                                            const RHIBlendState& blendState,
                                                            RHIFormat renderTargetFormat,
                                                            DefaultLitDirectVertexInputMode inputMode,
                                                            MaterialCullMode cullMode,
                                                            bool reverseWinding) const;
        RHIGraphicsPipelineDesc BuildGPUDrivenDefaultLitPipelineDesc(const char* debugName,
                                                                     const RHIDepthStencilState& depthStencilState,
                                                                     const RHIBlendState& blendState,
                                                                     RHIFormat renderTargetFormat,
                                                                     MaterialCullMode cullMode,
                                                                     bool reverseWinding) const;
        RHIGraphicsPipelineDesc BuildInstancedMaterialDefaultLitPipelineDesc(
            const char* debugName,
            const RHIDepthStencilState& depthStencilState,
            const RHIBlendState& blendState,
            RHIFormat renderTargetFormat,
            MaterialCullMode cullMode,
            bool reverseWinding) const;
        RHIGraphicsPipelineDesc BuildGPUSceneDefaultLitPipelineDesc(const char* debugName,
                                                                     const RHIDepthStencilState& depthStencilState,
                                                                     const RHIBlendState& blendState,
                                                                     RHIFormat renderTargetFormat,
                                                                     bool instancedMaterial,
                                                                     MaterialCullMode cullMode,
                                                                     bool reverseWinding) const;
        RHIGraphicsPipelineDesc BuildDepthOnlyPipelineDesc(
            MaterialCullMode cullMode = MaterialCullMode::Back,
            bool reverseWinding = false) const;
        RHIGraphicsPipelineDesc BuildRigidDepthOnlyPipelineDesc(
            MaterialCullMode cullMode = MaterialCullMode::Back,
            bool reverseWinding = false) const;
        RHIGraphicsPipelineDesc BuildMaskedDepthOnlyPipelineDesc(
            DefaultLitDirectVertexInputMode inputMode,
            MaterialCullMode cullMode,
            bool reverseWinding) const;
        RHIGraphicsPipelineDesc BuildGPUDrivenDepthOnlyPipelineDesc(
            MaterialCullMode cullMode,
            bool reverseWinding) const;
        RHIGraphicsPipelineDesc BuildGPUSceneDepthOnlyPipelineDesc(
            MaterialCullMode cullMode,
            bool reverseWinding) const;
        RHIGraphicsPipelineDesc BuildShadowDepthPipelineDesc(
            const ShadowDepthBiasState& biasState,
            DefaultLitDirectVertexInputMode inputMode,
            MaterialCullMode cullMode,
            bool reverseWinding) const;
        RHIGraphicsPipelineDesc BuildMaskedShadowDepthPipelineDesc(
            const ShadowDepthBiasState& biasState,
            DefaultLitDirectVertexInputMode inputMode,
            MaterialCullMode cullMode,
            bool reverseWinding) const;
        RHIGraphicsPipelineDesc BuildInstancedShadowDepthPipelineDesc(
            const ShadowDepthBiasState& biasState,
            MaterialCullMode cullMode,
            bool reverseWinding) const;
        RHIGraphicsPipelineDesc BuildSkyboxPipelineDesc(RHIFormat outputFormat, bool depthTest = true) const;
        RHIGraphicsPipelineDesc BuildToneMappingPipelineDesc(RHIFormat outputFormat) const;
        RHIGraphicsPipelineDesc BuildScreenSpaceOutlinePipelineDesc(
            RHIFormat outputFormat) const;
        RHIGraphicsPipelineDesc BuildScreenSpaceOutlineVerifierPipelineDesc(
            RHIFormat outputFormat) const;
        RHIGraphicsPipelineDesc BuildBloomPipelineDesc(RHIFormat outputFormat) const;
        RHIGraphicsPipelineDesc BuildBloomAdditivePipelineDesc(RHIFormat outputFormat) const;
        RHIGraphicsPipelineDesc BuildGTAOPipelineDesc(
            RHIShader* pixelShader,
            const char* debugName,
            RHIFormat outputFormat) const;
        RHIGraphicsPipelineDesc BuildCameraVelocityPipelineDesc(RHIFormat outputFormat) const;
        RHIGraphicsPipelineDesc BuildObjectVelocityPipelineDesc(
            RHIFormat outputFormat,
            DefaultLitDirectVertexInputMode inputMode,
            MaterialCullMode cullMode,
            bool reverseWinding) const;
        RHIGraphicsPipelineDesc BuildMaskedObjectVelocityPipelineDesc(
            RHIFormat outputFormat,
            DefaultLitDirectVertexInputMode inputMode,
            MaterialCullMode cullMode,
            bool reverseWinding) const;
        RHIGraphicsPipelineDesc BuildRayTracedReflectionCompositePipelineDesc(RHIFormat outputFormat) const;
        RHIGraphicsPipelineDesc BuildRayTracedReflectionDenoisePipelineDesc(RHIFormat outputFormat) const;
        RHIGraphicsPipelineDesc BuildColorGradingPipelineDesc(RHIFormat outputFormat) const;
        RHIGraphicsPipelineDesc BuildChromaticAberrationPipelineDesc(RHIFormat outputFormat) const;
        RHIGraphicsPipelineDesc BuildFilmGrainPipelineDesc(RHIFormat outputFormat) const;
        RHIGraphicsPipelineDesc BuildFXAAPipelineDesc(RHIFormat outputFormat) const;
        RHIGraphicsPipelineDesc BuildTAAResolvePipelineDesc(RHIFormat outputFormat) const;
        RHIGraphicsPipelineDesc BuildTAAHistoryStorePipelineDesc() const;
        RHIGraphicsPipelineDesc BuildVignettePipelineDesc(RHIFormat outputFormat) const;
        RHIGraphicsPipelineDesc BuildUIPipelineDesc(RHIFormat outputFormat) const;
        bool CreateViewConstantBuffer();
        bool CreateObjectConstantBuffer();
        bool EnsureFrameShadowFallbackResources();
        bool EnsureFrameRayTracedShadowFallbackResources();
        bool EnsureFrameEnvironmentIBLFallbackResources();
        bool EnsureFrameLightFallbackResources();
        bool EnsureFrameClusteredLightFallbackResources();
        bool EnsureFrameExposureFallbackBuffer();
        bool EnsureFrameAmbientOcclusionFallbackResources();
        bool EnsureObjectInstanceFallbackBuffer();
        bool EnsureObjectSkinningPaletteFallbackBuffers();
        [[nodiscard]] bool ResolveSkinningPaletteBuffers(
            const SkinningPaletteBufferBinding& requested,
            SkinningPaletteBufferBinding& outResolved) const;
        [[nodiscard]] bool BuildObjectConstants(
            const Mat4& worldMatrix,
            const Mat4& normalMatrix,
            const Mat4& previousWorldMatrix,
            const Mat4& previousViewProjectionMatrix,
            bool previousWorldViewProjectionValid,
            bool receivesShadow,
            const SkinningPaletteBufferBinding& skinningPalettes,
            const SkinningPaletteSliceBinding& skinningSlice,
            ObjectConstants& outConstants) const;
        bool UpdateDefaultFrameDescriptorSet();
        RHIDescriptorSetRef CreateFrameDescriptorSet();
        Ref<FrameDescriptorBindingOwners> CreateFrameDescriptorBindingOwners() const;
        std::vector<RHIDescriptorBinding> BuildFrameDescriptorBindings(
            const FrameDescriptorBindingOwners& owners) const;
        RHIDescriptorSetRef CreateObjectDescriptorSet();
        bool CreateRasterDrawBindingSnapshotInternal(
            const ViewData& view,
            uint32 objectCapacity,
            const FrameLightResources* transparentLightResources,
            const SkinningPaletteBufferBinding& skinningPalettes,
            RasterDrawBindingSnapshot& outSnapshot) const;
        uint64 AllocateObjectConstantSlot();
        RHIDescriptorSetRef CreateObjectConstantPageDescriptor(
            uint64 pageIdentity,
            RHIBuffer* constantBuffer,
            RHIBuffer* instanceBuffer,
            RHIBuffer* currentPaletteBuffer,
            RHIBuffer* previousPaletteBuffer,
            bool cacheFallbackInstance);
        bool BuildReflectedDefaultLitLayouts(std::vector<RHIDescriptorSetLayoutDesc>& outLayouts);
        bool ValidateDefaultLitLayouts(const std::vector<RHIDescriptorSetLayoutDesc>& layouts);
        void ProcessPipelineManifest();
        void SetLastError(std::string message);
        RHIPipelineRef TrackGPUSceneRasterPipeline(uint64 stateHash,
                                                   RHIPipelineRef pipeline);
        void ResetGPUSceneRasterState();
        void DisableGPUSceneRaster(std::string reason);
        uint64 ComputePipelineStateHash(const RHIGraphicsPipelineDesc& desc, MaterialPipelineVariant variant) const;
        uint64 ComputePipelineStateHash(const RHIGraphicsPipelineDesc& desc,
                                        MaterialPipelineVariant variant,
                                        uint32 purposeSalt) const;
        uint64 ComputeShaderHash(const ShaderProgramMetadata* result) const;
        uint64 StoreVariantHash(MaterialPipelineVariant variant, uint64 hash);

        IRHIDevice* m_device = nullptr;
        std::string m_shaderDir;
        bool m_initialized = false;
        PipelineCacheConfig m_config;
        PipelineCacheStats m_stats;
        std::string m_lastError;
        std::string m_gpuSceneRasterUnavailableReason;

        // Renderer-owned provider, borrowed for the cache lifetime.
        IShaderProgramProvider* m_shaderProgramProvider = nullptr;
        std::unique_ptr<ShaderProgramLoader> m_shaderProgramLoader;

        // Shaders
        RHIShaderRef m_vertexShader;
        RHIShaderRef m_rigidVertexShader;
        RHIShaderRef m_gpuDrivenVertexShader;
        RHIShaderRef m_instancedMaterialVertexShader;
        RHIShaderRef m_gpuSceneVertexShader;
        RHIShaderRef m_gpuSceneInstancedMaterialVertexShader;
        RHIShaderRef m_pixelShader;
        RHIShaderRef m_translucencyPixelShader;
        RHIShaderRef m_forwardSurfaceDataPixelShader;
        RHIShaderRef m_gBufferPixelShader;
        RHIShaderRef m_deferredLightingComputeShader;
        RHIShaderRef m_exposureHistogramClearComputeShader;
        RHIShaderRef m_exposureHistogramBuildComputeShader;
        RHIShaderRef m_exposureAdaptationComputeShader;
        RHIShaderRef m_depthOnlyVertexShader;
        RHIShaderRef m_rigidDepthOnlyVertexShader;
        RHIShaderRef m_maskedDepthOnlyVertexShader;
        RHIShaderRef m_rigidMaskedDepthOnlyVertexShader;
        RHIShaderRef m_maskedDepthOnlyPixelShader;
        RHIShaderRef m_gpuDrivenDepthOnlyVertexShader;
        RHIShaderRef m_gpuSceneDepthOnlyVertexShader;
        RHIShaderRef m_skyboxVertexShader;
        RHIShaderRef m_skyboxPixelShader;
        RHIShaderRef m_toneMappingVertexShader;
        RHIShaderRef m_toneMappingPixelShader;
        RHIShaderRef m_screenSpaceOutlinePixelShader;
        RHIShaderRef m_screenSpaceOutlineVerifierPixelShader;
        RHIShaderRef m_bloomVertexShader;
        RHIShaderRef m_bloomPixelShader;
        RHIShaderRef m_gtaoSurfacePixelShader;
        RHIShaderRef m_gtaoHorizonPixelShader;
        RHIShaderRef m_gtaoBilateralHorizontalPixelShader;
        RHIShaderRef m_gtaoBilateralVerticalPixelShader;
        RHIShaderRef m_gtaoTemporalPixelShader;
        RHIShaderRef m_cameraVelocityPixelShader;
        RHIShaderRef m_objectVelocityVertexShader;
        RHIShaderRef m_rigidObjectVelocityVertexShader;
        RHIShaderRef m_objectVelocityPixelShader;
        RHIShaderRef m_maskedObjectVelocityVertexShader;
        RHIShaderRef m_rigidMaskedObjectVelocityVertexShader;
        RHIShaderRef m_maskedObjectVelocityPixelShader;
        RHIShaderRef m_rayTracedReflectionCompositeVertexShader;
        RHIShaderRef m_rayTracedReflectionCompositePixelShader;
        RHIShaderRef m_rayTracedReflectionDenoiseVertexShader;
        RHIShaderRef m_rayTracedReflectionDenoisePixelShader;
        RHIShaderRef m_colorGradingVertexShader;
        RHIShaderRef m_colorGradingPixelShader;
        RHIShaderRef m_chromaticAberrationVertexShader;
        RHIShaderRef m_chromaticAberrationPixelShader;
        RHIShaderRef m_filmGrainVertexShader;
        RHIShaderRef m_filmGrainPixelShader;
        RHIShaderRef m_fxaaVertexShader;
        RHIShaderRef m_fxaaPixelShader;
        RHIShaderRef m_taaVertexShader;
        RHIShaderRef m_taaResolvePixelShader;
        RHIShaderRef m_taaHistoryStorePixelShader;
        RHIShaderRef m_vignetteVertexShader;
        RHIShaderRef m_vignettePixelShader;
        RHIShaderRef m_uiVertexShader;
        RHIShaderRef m_uiPixelShader;
        RHIShaderRef m_rayTracedShadowRayGenShader;
        RHIShaderRef m_rayTracedShadowMissShader;
        RHIShaderRef m_rayTracedShadowClosestHitShader;
        RHIShaderRef m_rayTracedShadowAnyHitShader;
        RHIShaderRef m_rayTracedReflectionRayGenShader;
        RHIShaderRef m_rayTracedReflectionMissShader;
        RHIShaderRef m_rayTracedReflectionClosestHitShader;
        RHIShaderRef m_rayTracedReflectionAnyHitShader;
        std::unique_ptr<ShaderProgramMetadata> m_vsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_rigidVsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_gpuDrivenVsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_instancedMaterialVsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_gpuSceneVsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_gpuSceneInstancedMaterialVsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_psCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_translucencyPsCompileResult;
        std::unique_ptr<ShaderProgramMetadata>
            m_forwardSurfaceDataPsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_gBufferPsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_deferredLightingCsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_exposureHistogramClearCsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_exposureHistogramBuildCsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_exposureAdaptationCsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_depthOnlyVsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_rigidDepthOnlyVsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_maskedDepthOnlyVsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_rigidMaskedDepthOnlyVsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_maskedDepthOnlyPsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_gpuDrivenDepthOnlyVsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_gpuSceneDepthOnlyVsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_skyboxVsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_skyboxPsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_toneMappingVsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_toneMappingPsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_screenSpaceOutlinePsCompileResult;
        std::unique_ptr<ShaderProgramMetadata>
            m_screenSpaceOutlineVerifierPsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_bloomVsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_bloomPsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_gtaoSurfacePsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_gtaoHorizonPsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_gtaoBilateralHorizontalPsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_gtaoBilateralVerticalPsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_gtaoTemporalPsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_cameraVelocityPsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_objectVelocityVsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_rigidObjectVelocityVsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_objectVelocityPsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_maskedObjectVelocityVsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_rigidMaskedObjectVelocityVsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_maskedObjectVelocityPsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_rayTracedReflectionCompositeVsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_rayTracedReflectionCompositePsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_rayTracedReflectionDenoiseVsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_rayTracedReflectionDenoisePsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_colorGradingVsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_colorGradingPsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_chromaticAberrationVsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_chromaticAberrationPsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_filmGrainVsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_filmGrainPsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_fxaaVsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_fxaaPsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_taaVsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_taaResolvePsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_taaHistoryStorePsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_vignetteVsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_vignettePsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_uiVsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_uiPsCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_rayTracedShadowRayGenCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_rayTracedShadowMissCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_rayTracedShadowClosestHitCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_rayTracedShadowAnyHitCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_rayTracedReflectionRayGenCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_rayTracedReflectionMissCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_rayTracedReflectionClosestHitCompileResult;
        std::unique_ptr<ShaderProgramMetadata> m_rayTracedReflectionAnyHitCompileResult;

        // Descriptor set layouts and pipeline layout
        std::vector<RHIDescriptorSetLayoutRef> m_setLayouts;
        RHIPipelineLayoutRef m_pipelineLayout;
        RHIDescriptorSetLayoutRef m_gpuSceneRasterObjectSetLayout;
        RHIPipelineLayoutRef m_gpuSceneRasterPipelineLayout;
        RHIDescriptorSetLayoutRef m_deferredLightingSetLayout;
        RHIPipelineLayoutRef m_deferredLightingPipelineLayout;
        RHIDescriptorSetLayoutRef m_exposureComputeSetLayout;
        RHIPipelineLayoutRef m_exposureComputePipelineLayout;
        RHIDescriptorSetLayoutRef m_postProcessSetLayout;
        RHIPipelineLayoutRef m_postProcessPipelineLayout;
        RHIDescriptorSetLayoutRef m_exposurePostProcessSetLayout;
        RHIPipelineLayoutRef m_exposurePostProcessPipelineLayout;
        RHIDescriptorSetLayoutRef m_taaSetLayout;
        RHIPipelineLayoutRef m_taaPipelineLayout;
        RHIDescriptorSetLayoutRef m_gtaoSetLayout;
        RHIPipelineLayoutRef m_gtaoPipelineLayout;
        RHIDescriptorSetLayoutRef m_screenSpaceOutlineSetLayout;
        RHIPipelineLayoutRef m_screenSpaceOutlinePipelineLayout;
        RHIDescriptorSetLayoutRef m_screenSpaceOutlineVerifierSetLayout;
        RHIPipelineLayoutRef m_screenSpaceOutlineVerifierPipelineLayout;
        RHIDescriptorSetLayoutRef m_uiTextureSetLayout;
        RHIPipelineLayoutRef m_uiPipelineLayout;
        RHIDescriptorSetLayoutRef m_rayTracedReflectionDenoiseSetLayout;
        RHIPipelineLayoutRef m_rayTracedReflectionDenoisePipelineLayout;
        RHIDescriptorSetLayoutRef m_skyboxSetLayout;
        RHIPipelineLayoutRef m_skyboxPipelineLayout;
        RHIDescriptorSetLayoutRef m_rayTracedShadowSetLayout;
        RHIPipelineLayoutRef m_rayTracedShadowPipelineLayout;
        RHIDescriptorSetLayoutRef m_rayTracedReflectionSetLayout;
        RHIPipelineLayoutRef m_rayTracedReflectionPipelineLayout;

        // Graphics pipelines
        RHIPipelineRef m_opaquePipeline;
        RHIPipelineRef m_maskedPipeline;
        RHIPipelineRef m_transparentPipeline;
        RHIPipelineRef m_depthOnlyPipeline;
        RHIPipelineRef m_rigidDepthOnlyPipeline;
        RHIPipelineRef m_maskedDepthOnlyPipeline;
        RHIPipelineRef m_rigidMaskedDepthOnlyPipeline;
        RHIPipelineRef m_gpuDrivenDepthOnlyPipeline;
        RHIPipelineRef m_gpuSceneOpaquePipeline;
        RHIPipelineRef m_gpuSceneInstancedMaterialOpaquePipeline;
        RHIPipelineRef m_gpuSceneMaskedPipeline;
        RHIPipelineRef m_gpuSceneDepthOnlyPipeline;
        RHIPipelineRef m_deferredLightingPipeline;
        RHIPipelineRef m_exposureHistogramClearPipeline;
        RHIPipelineRef m_exposureHistogramBuildPipeline;
        RHIPipelineRef m_exposureAdaptationPipeline;
        // Every GPU-scene raster pipeline is tracked independently from the
        // Back-cull convenience members above.  Cull variants are created on
        // demand, so those members alone cannot describe the complete set
        // whose layout/shader substrate must be retired together.
        std::unordered_map<uint64, RHIPipelineRef> m_gpuSceneRasterPipelines;
        RHIPipelineRef m_skyboxPipeline;
        RHIPipelineRef m_toneMappingPipeline;
        RHIPipelineRef m_bloomPipeline;
        RHIPipelineRef m_bloomAdditivePipeline;
        RHIPipelineRef m_gtaoSurfacePipeline;
        RHIPipelineRef m_gtaoHorizonPipeline;
        RHIPipelineRef m_gtaoBilateralHorizontalPipeline;
        RHIPipelineRef m_gtaoBilateralVerticalPipeline;
        RHIPipelineRef m_gtaoTemporalPipeline;
        RHIPipelineRef m_screenSpaceOutlinePipeline;
        RHIPipelineRef m_screenSpaceOutlineVerifierPipeline;
        RHIPipelineRef m_cameraVelocityPipeline;
        RHIPipelineRef m_objectVelocityPipeline;
        RHIPipelineRef m_rigidObjectVelocityPipeline;
        RHIPipelineRef m_maskedObjectVelocityPipeline;
        RHIPipelineRef m_rigidMaskedObjectVelocityPipeline;
        RHIPipelineRef m_rayTracedReflectionCompositePipeline;
        RHIPipelineRef m_rayTracedReflectionDenoisePipeline;
        RHIPipelineRef m_colorGradingPipeline;
        RHIPipelineRef m_chromaticAberrationPipeline;
        RHIPipelineRef m_filmGrainPipeline;
        RHIPipelineRef m_fxaaPipeline;
        RHIPipelineRef m_taaResolvePipeline;
        RHIPipelineRef m_taaHistoryStorePipeline;
        RHIPipelineRef m_vignettePipeline;
        RHIPipelineRef m_uiPipeline;
        RHIPipelineRef m_rayTracedShadowPipeline;
        RHIShaderTableRef m_rayTracedShadowShaderTable;
        RHIPipelineRef m_rayTracedReflectionPipeline;
        RHIShaderTableRef m_rayTracedReflectionShaderTable;
        std::unordered_map<uint64, RHIPipelineRef> m_pipelineCache;

        // Frame and object constants
        RHIBufferRef m_viewConstantBuffer;
        RHIBufferRef m_objectConstantBuffer;
        RHIBufferRef m_objectInstanceFallbackBuffer;
        RHIBufferRef m_objectSkinningCurrentFallbackBuffer;
        RHIBufferRef m_objectSkinningPreviousFallbackBuffer;
        SkinningPaletteBufferBinding m_objectDescriptorSkinningPalettes;
        RHIDescriptorSetRef m_frameDescriptorSet;
        Ref<FrameDescriptorBindingOwners> m_frameDescriptorBindingOwners;
        RHIDescriptorSetRef m_objectDescriptorSet;
        std::unique_ptr<FrameConstantUploadArena> m_objectConstantUploadArena;
        struct ObjectPageDescriptorKey
        {
            uint64 pageIdentity = 0;
            RHIBuffer* instanceBuffer = nullptr;
            RHIBuffer* currentPaletteBuffer = nullptr;
            RHIBuffer* previousPaletteBuffer = nullptr;
            bool operator==(const ObjectPageDescriptorKey& other) const noexcept
            {
                return pageIdentity == other.pageIdentity &&
                       instanceBuffer == other.instanceBuffer &&
                       currentPaletteBuffer == other.currentPaletteBuffer &&
                       previousPaletteBuffer == other.previousPaletteBuffer;
            }
        };
        struct ObjectPageDescriptorKeyHash
        {
            size_t operator()(const ObjectPageDescriptorKey& key) const noexcept
            {
                return std::hash<uint64>{}(key.pageIdentity) ^
                    (std::hash<RHIBuffer*>{}(key.instanceBuffer) << 1) ^
                    (std::hash<RHIBuffer*>{}(key.currentPaletteBuffer) << 2) ^
                    (std::hash<RHIBuffer*>{}(key.previousPaletteBuffer) << 3);
            }
        };
        std::unordered_map<ObjectPageDescriptorKey,
                           RHIDescriptorSetRef,
                           ObjectPageDescriptorKeyHash> m_objectPageDescriptorCache;
        std::vector<Ref<RefCounted>> m_pendingOwnerRetirements;
        RHITextureRef m_fallbackDirectionalShadowTexture;
        RHITextureViewRef m_fallbackDirectionalShadowView;
        RHITextureRef m_fallbackRayTracedShadowMaskTexture;
        RHITextureViewRef m_fallbackRayTracedShadowMaskView;
        RHISamplerRef m_directionalShadowSampler;
        RHITextureRef m_fallbackEnvironmentIBLCubemapTexture;
        RHITextureViewRef m_fallbackEnvironmentIBLCubemapView;
        RHITextureRef m_fallbackEnvironmentIBLBRDFLUTTexture;
        RHITextureViewRef m_fallbackEnvironmentIBLBRDFLUTView;
        RHISamplerRef m_environmentIBLSampler;
        RHIBufferRef m_fallbackLightConstantsBuffer;
        RHIBufferRef m_fallbackPointLightsBuffer;
        RHIBufferRef m_fallbackSpotLightsBuffer;
        RHIBufferRef m_fallbackClusterConstantsBuffer;
        RHIBufferRef m_fallbackClusterBuffer;
        RHIBufferRef m_fallbackClusterLightIndexBuffer;
        RHIBufferRef m_fallbackExposureStateBuffer;
        RHITextureRef m_fallbackAmbientOcclusionTexture;
        RHITextureViewRef m_fallbackAmbientOcclusionView;
        RHITextureViewRef m_currentDirectionalShadowView;
        RHITextureViewRef m_currentRayTracedShadowMaskView;
        RHISamplerRef m_currentDirectionalShadowSampler;
        RHITextureViewRef m_currentEnvironmentIBLIrradianceView;
        RHITextureViewRef m_currentEnvironmentIBLPrefilteredView;
        RHITextureViewRef m_currentEnvironmentIBLBRDFLUTView;
        RHIBufferRef m_currentLightConstantsBuffer;
        RHIBufferRef m_currentPointLightsBuffer;
        RHIBufferRef m_currentSpotLightsBuffer;
        RHIBufferRef m_currentClusterConstantsBuffer;
        RHIBufferRef m_currentClusterBuffer;
        RHIBufferRef m_currentClusterLightIndexBuffer;
        RHIBufferRef m_currentExposureStateBuffer;
        bool m_frameResourceBindingsDirty = false;
        DirectionalShadowFrameBindingResult m_lastDirectionalShadowFrameBindingResult;
        RayTracedShadowFrameBindingResult m_lastRayTracedShadowFrameBindingResult;
        EnvironmentIBLFrameBindingResult m_lastEnvironmentIBLFrameBindingResult;
        FrameLightBindingResult m_lastFrameLightBindingResult;
        uint64 m_objectConstantStride = 0;
        uint64 m_objectConstantCursor = 0;
        uint64 m_currentObjectConstantOffset = 0;

        // Render target format
        RHIFormat m_renderTargetFormat = RHIFormat::RGBA8_UNORM;
        RHIFormat m_postProcessIntermediateFormat = RHIFormat::RGBA8_UNORM;
        RHIFormat m_toneMappingOutputFormat = RHIFormat::RGBA8_UNORM;
    };

} // namespace RVX
