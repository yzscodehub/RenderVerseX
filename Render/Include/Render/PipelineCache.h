#pragma once

/**
 * @file PipelineCache.h
 * @brief Pipeline cache for managing graphics pipelines and frame/object sets
 *
 * PipelineCache handles:
 * - Shader compilation with ShaderManager
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
    class IShaderCompiler;
    class RenderRetirementQueue;
    class ShaderManager;
    struct ShaderCompileResult;
    struct ViewData;

    constexpr uint32 RVX_MAX_OBJECT_SKINNING_MATRICES = 128;

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

    struct FrameLightBindingResult
    {
        bool lightResourcesBound = false;
        bool clusteredLightResourcesBound = false;
        FrameLightFallbackReason fallbackReason = FrameLightFallbackReason::FallbackUnavailable;
        FrameClusteredLightFallbackReason clusteredFallbackReason =
            FrameClusteredLightFallbackReason::FallbackUnavailable;
    };

    struct ShadowDepthBiasState
    {
        float constantBias = 0.0f;
        float slopeScaledBias = 0.0f;
        float biasClamp = 0.0f;
    };

    /**
     * @brief Object constants structure (matches HLSL cbuffer)
     */
    struct ObjectConstants
    {
        Mat4 world;
        Mat4 normalMatrix;
        Mat4 previousWorldViewProjection;
        Vec4 objectVelocityParams;
        Vec4 skinningParams;
        Mat4 skinningMatrices[RVX_MAX_OBJECT_SKINNING_MATRICES];
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
        RHIDescriptorSetRef frameDescriptorSet;
        RHIDescriptorSetRef objectDescriptorSet;
        std::vector<Ref<RefCounted>> retainedResources;
        uint64 objectConstantStride = 0;
        uint32 objectCapacity = 0;
        uint32 objectCursor = 0;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return viewConstantBuffer && objectConstantBuffer &&
                   frameDescriptorSet && objectDescriptorSet &&
                   objectConstantStride != 0 && objectCapacity != 0;
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
        uint64 bloomPipelineHash = 0;
        uint64 ssaoPipelineHash = 0;
        uint64 colorGradingPipelineHash = 0;
        uint64 chromaticAberrationPipelineHash = 0;
        uint64 filmGrainPipelineHash = 0;
        uint64 fxaaPipelineHash = 0;
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
        using ShaderCompilerFactory =
            std::function<std::unique_ptr<IShaderCompiler>()>;

        PipelineCache();
        /** @brief Create a cache with an explicit compiler composition factory. */
        explicit PipelineCache(ShaderCompilerFactory shaderCompilerFactory);
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

        /**
         * @brief Get a pipeline for a material variant
         */
        RHIPipeline* GetPipelineForVariant(MaterialPipelineVariant variant) const;
        RHIPipeline* GetPipelineForVariant(MaterialPipelineVariant variant, RHIFormat renderTargetFormat);
        RHIPipeline* GetPipelineForVariant(MaterialPipelineVariant variant,
                                           RHIFormat renderTargetFormat,
                                           DefaultLitDirectVertexInputMode inputMode);
        RHIPipeline* GetGPUDrivenPipelineForVariant(MaterialPipelineVariant variant, RHIFormat renderTargetFormat);

        /**
         * @brief Get the depth-only pipeline for depth prepass
         * @return Depth-only pipeline or nullptr if not available
         */
        RHIPipeline* GetDepthOnlyPipeline() const { return m_depthOnlyPipeline.Get(); }
        /** @brief Get the alpha-masked depth-only pipeline. */
        RHIPipeline* GetMaskedDepthOnlyPipeline() const
        {
            return m_maskedDepthOnlyPipeline.Get();
        }
        RHIPipeline* GetGPUDrivenDepthOnlyPipeline();

        /**
         * @brief Get the shadow-map depth-only pipeline for caster raster bias
         */
        RHIPipeline* GetShadowDepthPipeline(const ShadowDepthBiasState& biasState);
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
         * @brief Get the fullscreen Bloom post-process pipeline
         */
        RHIPipeline* GetBloomPipeline() const { return m_bloomPipeline.Get(); }
        RHIPipeline* GetBloomPipeline(RHIFormat outputFormat);
        RHIPipeline* GetBloomAdditivePipeline(RHIFormat outputFormat);

        /**
         * @brief Get the fullscreen depth-only SSAO post-process pipeline
         */
        RHIPipeline* GetSSAOPipeline() const { return m_ssaoPipeline.Get(); }
        RHIPipeline* GetSSAOPipeline(RHIFormat outputFormat);

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
        RHIPipeline* GetMaskedObjectVelocityPipeline() const { return m_maskedObjectVelocityPipeline.Get(); }
        RHIPipeline* GetMaskedObjectVelocityPipeline(RHIFormat outputFormat);

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

        /**
         * @brief Get the post-process fullscreen pipeline layout
         */
        RHIPipelineLayout* GetPostProcessLayout() const { return m_postProcessPipelineLayout.Get(); }

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
         * @brief Update frame-scope local light buffer bindings.
         */
        FrameLightBindingResult UpdateFrameLightResources(const FrameLightResources& resources);

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

        static const char* GetDirectionalShadowFallbackReasonName(DirectionalShadowFallbackReason reason);
        static const char* GetRayTracedShadowFallbackReasonName(RayTracedShadowFallbackReason reason);
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

        /** @brief Upload one object slot in a recording-owned snapshot. */
        bool UpdateRasterDrawBindingSnapshotObject(
            RasterDrawBindingSnapshot& snapshot,
            const Mat4& worldMatrix,
            const Mat4& normalMatrix,
            const Mat4& previousWorldMatrix,
            const Mat4& previousViewProjectionMatrix,
            bool previousWorldViewProjectionValid,
            bool receivesShadow,
            std::span<const Mat4> skinningMatrices = {}) const;

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
                                   bool previousWorldViewProjectionValid,
                                   std::span<const Mat4> skinningMatrices = {});

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
                                   std::span<const Mat4> skinningMatrices = {});

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
        bool CreatePostProcessPipelineLayout();
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
                                                     bool updatePrimaryStats);
        RHIPipelineRef GetOrCreateGPUDrivenDefaultLitPipeline(MaterialPipelineVariant variant,
                                                              const char* debugName,
                                                              const RHIDepthStencilState& depthStencilState,
                                                              const RHIBlendState& blendState,
                                                              RHIFormat renderTargetFormat);
        RHIPipelineRef GetOrCreateDepthOnlyPipeline();
        RHIPipelineRef GetOrCreateMaskedDepthOnlyPipeline();
        RHIPipelineRef GetOrCreateGPUDrivenDepthOnlyPipeline();
        RHIPipelineRef GetOrCreateShadowDepthPipeline(const ShadowDepthBiasState& biasState);
        RHIPipelineRef GetOrCreateSkyboxPipeline(RHIFormat outputFormat,
                                                 bool depthTest = true,
                                                 bool updatePrimaryStats = true);
        RHIPipelineRef GetOrCreateToneMappingPipeline(RHIFormat outputFormat);
        RHIPipelineRef GetOrCreateBloomPipeline(RHIFormat outputFormat);
        RHIPipelineRef GetOrCreateBloomAdditivePipeline(RHIFormat outputFormat);
        RHIPipelineRef GetOrCreateSSAOPipeline(RHIFormat outputFormat);
        RHIPipelineRef GetOrCreateCameraVelocityPipeline(RHIFormat outputFormat);
        RHIPipelineRef GetOrCreateObjectVelocityPipeline(RHIFormat outputFormat);
        RHIPipelineRef GetOrCreateMaskedObjectVelocityPipeline(RHIFormat outputFormat);
        RHIPipelineRef GetOrCreateRayTracedReflectionCompositePipeline(RHIFormat outputFormat);
        RHIPipelineRef GetOrCreateRayTracedReflectionDenoisePipeline(RHIFormat outputFormat);
        RHIPipelineRef GetOrCreateColorGradingPipeline(RHIFormat outputFormat);
        RHIPipelineRef GetOrCreateChromaticAberrationPipeline(RHIFormat outputFormat);
        RHIPipelineRef GetOrCreateFilmGrainPipeline(RHIFormat outputFormat);
        RHIPipelineRef GetOrCreateFXAAPipeline(RHIFormat outputFormat);
        RHIPipelineRef GetOrCreateVignettePipeline(RHIFormat outputFormat);
        RHIPipelineRef GetOrCreateUIPipeline(RHIFormat outputFormat);
        bool CreateRayTracedShadowPipeline();
        bool CreateRayTracedReflectionPipeline();
        RHIGraphicsPipelineDesc BuildDefaultLitPipelineDesc(const char* debugName,
                                                            const RHIDepthStencilState& depthStencilState,
                                                            const RHIBlendState& blendState,
                                                            RHIFormat renderTargetFormat,
                                                            DefaultLitDirectVertexInputMode inputMode) const;
        RHIGraphicsPipelineDesc BuildGPUDrivenDefaultLitPipelineDesc(const char* debugName,
                                                                     const RHIDepthStencilState& depthStencilState,
                                                                     const RHIBlendState& blendState,
                                                                     RHIFormat renderTargetFormat) const;
        RHIGraphicsPipelineDesc BuildDepthOnlyPipelineDesc() const;
        RHIGraphicsPipelineDesc BuildMaskedDepthOnlyPipelineDesc() const;
        RHIGraphicsPipelineDesc BuildGPUDrivenDepthOnlyPipelineDesc() const;
        RHIGraphicsPipelineDesc BuildShadowDepthPipelineDesc(const ShadowDepthBiasState& biasState) const;
        RHIGraphicsPipelineDesc BuildSkyboxPipelineDesc(RHIFormat outputFormat, bool depthTest = true) const;
        RHIGraphicsPipelineDesc BuildToneMappingPipelineDesc(RHIFormat outputFormat) const;
        RHIGraphicsPipelineDesc BuildBloomPipelineDesc(RHIFormat outputFormat) const;
        RHIGraphicsPipelineDesc BuildBloomAdditivePipelineDesc(RHIFormat outputFormat) const;
        RHIGraphicsPipelineDesc BuildSSAOPipelineDesc(RHIFormat outputFormat) const;
        RHIGraphicsPipelineDesc BuildCameraVelocityPipelineDesc(RHIFormat outputFormat) const;
        RHIGraphicsPipelineDesc BuildObjectVelocityPipelineDesc(RHIFormat outputFormat) const;
        RHIGraphicsPipelineDesc BuildMaskedObjectVelocityPipelineDesc(RHIFormat outputFormat) const;
        RHIGraphicsPipelineDesc BuildRayTracedReflectionCompositePipelineDesc(RHIFormat outputFormat) const;
        RHIGraphicsPipelineDesc BuildRayTracedReflectionDenoisePipelineDesc(RHIFormat outputFormat) const;
        RHIGraphicsPipelineDesc BuildColorGradingPipelineDesc(RHIFormat outputFormat) const;
        RHIGraphicsPipelineDesc BuildChromaticAberrationPipelineDesc(RHIFormat outputFormat) const;
        RHIGraphicsPipelineDesc BuildFilmGrainPipelineDesc(RHIFormat outputFormat) const;
        RHIGraphicsPipelineDesc BuildFXAAPipelineDesc(RHIFormat outputFormat) const;
        RHIGraphicsPipelineDesc BuildVignettePipelineDesc(RHIFormat outputFormat) const;
        RHIGraphicsPipelineDesc BuildUIPipelineDesc(RHIFormat outputFormat) const;
        bool CreateViewConstantBuffer();
        bool CreateObjectConstantBuffer();
        bool EnsureFrameShadowFallbackResources();
        bool EnsureFrameRayTracedShadowFallbackResources();
        bool EnsureFrameLightFallbackResources();
        bool EnsureFrameClusteredLightFallbackResources();
        bool EnsureObjectInstanceFallbackBuffer();
        bool UpdateDefaultFrameDescriptorSet();
        RHIDescriptorSetRef CreateFrameDescriptorSet();
        RHIDescriptorSetRef CreateObjectDescriptorSet();
        uint64 AllocateObjectConstantSlot();
        bool BuildReflectedDefaultLitLayouts(std::vector<RHIDescriptorSetLayoutDesc>& outLayouts);
        bool ValidateDefaultLitLayouts(const std::vector<RHIDescriptorSetLayoutDesc>& layouts);
        void ProcessPipelineManifest();
        void SetLastError(std::string message);
        uint64 ComputePipelineStateHash(const RHIGraphicsPipelineDesc& desc, MaterialPipelineVariant variant) const;
        uint64 ComputePipelineStateHash(const RHIGraphicsPipelineDesc& desc,
                                        MaterialPipelineVariant variant,
                                        uint32 purposeSalt) const;
        uint64 ComputeShaderHash(const ShaderCompileResult* result) const;
        uint64 StoreVariantHash(MaterialPipelineVariant variant, uint64 hash);

        IRHIDevice* m_device = nullptr;
        std::string m_shaderDir;
        bool m_initialized = false;
        PipelineCacheConfig m_config;
        PipelineCacheStats m_stats;
        std::string m_lastError;

        // Shader manager
        ShaderCompilerFactory m_shaderCompilerFactory;
        std::unique_ptr<ShaderManager> m_shaderManager;

        // Shaders
        RHIShaderRef m_vertexShader;
        RHIShaderRef m_rigidVertexShader;
        RHIShaderRef m_gpuDrivenVertexShader;
        RHIShaderRef m_pixelShader;
        RHIShaderRef m_depthOnlyVertexShader;
        RHIShaderRef m_maskedDepthOnlyVertexShader;
        RHIShaderRef m_maskedDepthOnlyPixelShader;
        RHIShaderRef m_gpuDrivenDepthOnlyVertexShader;
        RHIShaderRef m_skyboxVertexShader;
        RHIShaderRef m_skyboxPixelShader;
        RHIShaderRef m_toneMappingVertexShader;
        RHIShaderRef m_toneMappingPixelShader;
        RHIShaderRef m_bloomVertexShader;
        RHIShaderRef m_bloomPixelShader;
        RHIShaderRef m_ssaoVertexShader;
        RHIShaderRef m_ssaoPixelShader;
        RHIShaderRef m_cameraVelocityPixelShader;
        RHIShaderRef m_objectVelocityVertexShader;
        RHIShaderRef m_objectVelocityPixelShader;
        RHIShaderRef m_maskedObjectVelocityVertexShader;
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
        std::unique_ptr<ShaderCompileResult> m_vsCompileResult;
        std::unique_ptr<ShaderCompileResult> m_rigidVsCompileResult;
        std::unique_ptr<ShaderCompileResult> m_gpuDrivenVsCompileResult;
        std::unique_ptr<ShaderCompileResult> m_psCompileResult;
        std::unique_ptr<ShaderCompileResult> m_depthOnlyVsCompileResult;
        std::unique_ptr<ShaderCompileResult> m_maskedDepthOnlyVsCompileResult;
        std::unique_ptr<ShaderCompileResult> m_maskedDepthOnlyPsCompileResult;
        std::unique_ptr<ShaderCompileResult> m_gpuDrivenDepthOnlyVsCompileResult;
        std::unique_ptr<ShaderCompileResult> m_skyboxVsCompileResult;
        std::unique_ptr<ShaderCompileResult> m_skyboxPsCompileResult;
        std::unique_ptr<ShaderCompileResult> m_toneMappingVsCompileResult;
        std::unique_ptr<ShaderCompileResult> m_toneMappingPsCompileResult;
        std::unique_ptr<ShaderCompileResult> m_bloomVsCompileResult;
        std::unique_ptr<ShaderCompileResult> m_bloomPsCompileResult;
        std::unique_ptr<ShaderCompileResult> m_ssaoVsCompileResult;
        std::unique_ptr<ShaderCompileResult> m_ssaoPsCompileResult;
        std::unique_ptr<ShaderCompileResult> m_cameraVelocityPsCompileResult;
        std::unique_ptr<ShaderCompileResult> m_objectVelocityVsCompileResult;
        std::unique_ptr<ShaderCompileResult> m_objectVelocityPsCompileResult;
        std::unique_ptr<ShaderCompileResult> m_maskedObjectVelocityVsCompileResult;
        std::unique_ptr<ShaderCompileResult> m_maskedObjectVelocityPsCompileResult;
        std::unique_ptr<ShaderCompileResult> m_rayTracedReflectionCompositeVsCompileResult;
        std::unique_ptr<ShaderCompileResult> m_rayTracedReflectionCompositePsCompileResult;
        std::unique_ptr<ShaderCompileResult> m_rayTracedReflectionDenoiseVsCompileResult;
        std::unique_ptr<ShaderCompileResult> m_rayTracedReflectionDenoisePsCompileResult;
        std::unique_ptr<ShaderCompileResult> m_colorGradingVsCompileResult;
        std::unique_ptr<ShaderCompileResult> m_colorGradingPsCompileResult;
        std::unique_ptr<ShaderCompileResult> m_chromaticAberrationVsCompileResult;
        std::unique_ptr<ShaderCompileResult> m_chromaticAberrationPsCompileResult;
        std::unique_ptr<ShaderCompileResult> m_filmGrainVsCompileResult;
        std::unique_ptr<ShaderCompileResult> m_filmGrainPsCompileResult;
        std::unique_ptr<ShaderCompileResult> m_fxaaVsCompileResult;
        std::unique_ptr<ShaderCompileResult> m_fxaaPsCompileResult;
        std::unique_ptr<ShaderCompileResult> m_vignetteVsCompileResult;
        std::unique_ptr<ShaderCompileResult> m_vignettePsCompileResult;
        std::unique_ptr<ShaderCompileResult> m_uiVsCompileResult;
        std::unique_ptr<ShaderCompileResult> m_uiPsCompileResult;
        std::unique_ptr<ShaderCompileResult> m_rayTracedShadowRayGenCompileResult;
        std::unique_ptr<ShaderCompileResult> m_rayTracedShadowMissCompileResult;
        std::unique_ptr<ShaderCompileResult> m_rayTracedShadowClosestHitCompileResult;
        std::unique_ptr<ShaderCompileResult> m_rayTracedShadowAnyHitCompileResult;
        std::unique_ptr<ShaderCompileResult> m_rayTracedReflectionRayGenCompileResult;
        std::unique_ptr<ShaderCompileResult> m_rayTracedReflectionMissCompileResult;
        std::unique_ptr<ShaderCompileResult> m_rayTracedReflectionClosestHitCompileResult;
        std::unique_ptr<ShaderCompileResult> m_rayTracedReflectionAnyHitCompileResult;

        // Descriptor set layouts and pipeline layout
        std::vector<RHIDescriptorSetLayoutRef> m_setLayouts;
        RHIPipelineLayoutRef m_pipelineLayout;
        RHIDescriptorSetLayoutRef m_postProcessSetLayout;
        RHIPipelineLayoutRef m_postProcessPipelineLayout;
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
        RHIPipelineRef m_maskedDepthOnlyPipeline;
        RHIPipelineRef m_gpuDrivenDepthOnlyPipeline;
        RHIPipelineRef m_skyboxPipeline;
        RHIPipelineRef m_toneMappingPipeline;
        RHIPipelineRef m_bloomPipeline;
        RHIPipelineRef m_bloomAdditivePipeline;
        RHIPipelineRef m_ssaoPipeline;
        RHIPipelineRef m_cameraVelocityPipeline;
        RHIPipelineRef m_objectVelocityPipeline;
        RHIPipelineRef m_maskedObjectVelocityPipeline;
        RHIPipelineRef m_rayTracedReflectionCompositePipeline;
        RHIPipelineRef m_rayTracedReflectionDenoisePipeline;
        RHIPipelineRef m_colorGradingPipeline;
        RHIPipelineRef m_chromaticAberrationPipeline;
        RHIPipelineRef m_filmGrainPipeline;
        RHIPipelineRef m_fxaaPipeline;
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
        RHIDescriptorSetRef m_frameDescriptorSet;
        RHIDescriptorSetRef m_objectDescriptorSet;
        std::vector<Ref<RefCounted>> m_pendingOwnerRetirements;
        RHITextureRef m_fallbackDirectionalShadowTexture;
        RHITextureViewRef m_fallbackDirectionalShadowView;
        RHITextureRef m_fallbackRayTracedShadowMaskTexture;
        RHITextureViewRef m_fallbackRayTracedShadowMaskView;
        RHISamplerRef m_directionalShadowSampler;
        RHIBufferRef m_fallbackLightConstantsBuffer;
        RHIBufferRef m_fallbackPointLightsBuffer;
        RHIBufferRef m_fallbackSpotLightsBuffer;
        RHIBufferRef m_fallbackClusterConstantsBuffer;
        RHIBufferRef m_fallbackClusterBuffer;
        RHIBufferRef m_fallbackClusterLightIndexBuffer;
        RHITextureView* m_currentDirectionalShadowView = nullptr;
        RHITextureView* m_currentRayTracedShadowMaskView = nullptr;
        RHISampler* m_currentDirectionalShadowSampler = nullptr;
        RHIBuffer* m_currentLightConstantsBuffer = nullptr;
        RHIBuffer* m_currentPointLightsBuffer = nullptr;
        RHIBuffer* m_currentSpotLightsBuffer = nullptr;
        RHIBuffer* m_currentClusterConstantsBuffer = nullptr;
        RHIBuffer* m_currentClusterBuffer = nullptr;
        RHIBuffer* m_currentClusterLightIndexBuffer = nullptr;
        bool m_frameResourceBindingsDirty = false;
        DirectionalShadowFrameBindingResult m_lastDirectionalShadowFrameBindingResult;
        RayTracedShadowFrameBindingResult m_lastRayTracedShadowFrameBindingResult;
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
