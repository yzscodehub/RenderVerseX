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
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace RVX
{
    // Forward declarations
    class ShaderManager;
    struct ShaderCompileResult;
    struct ViewData;

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

    enum class FrameLightFallbackReason : uint8
    {
        None = 0,
        MissingLightConstants,
        MissingPointLights,
        MissingSpotLights,
        FallbackUnavailable,
    };

    struct FrameLightResources
    {
        RHIBuffer* lightConstantsBuffer = nullptr;
        RHIBuffer* pointLightsBuffer = nullptr;
        RHIBuffer* spotLightsBuffer = nullptr;
    };

    struct FrameLightBindingResult
    {
        bool lightResourcesBound = false;
        FrameLightFallbackReason fallbackReason = FrameLightFallbackReason::FallbackUnavailable;
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
        uint64 colorGradingPipelineHash = 0;
        uint64 chromaticAberrationPipelineHash = 0;
        uint64 fxaaPipelineHash = 0;
        uint64 vignettePipelineHash = 0;
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

        /**
         * @brief Get the depth-only pipeline for depth prepass
         * @return Depth-only pipeline or nullptr if not available
         */
        RHIPipeline* GetDepthOnlyPipeline() const { return m_depthOnlyPipeline.Get(); }

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
         * @brief Get the fullscreen Vignette LDR post-process pipeline
         */
        RHIPipeline* GetVignettePipeline() const { return m_vignettePipeline.Get(); }
        RHIPipeline* GetVignettePipeline(RHIFormat outputFormat);

        /**
         * @brief Get the default pipeline layout
         */
        RHIPipelineLayout* GetDefaultLayout() const { return m_pipelineLayout.Get(); }

        /**
         * @brief Get the post-process fullscreen pipeline layout
         */
        RHIPipelineLayout* GetPostProcessLayout() const { return m_postProcessPipelineLayout.Get(); }

        /**
         * @brief Get the procedural skybox fullscreen pipeline layout
         */
        RHIPipelineLayout* GetSkyboxLayout() const { return m_skyboxPipelineLayout.Get(); }

        /**
         * @brief Get the descriptor set layout used by fullscreen post-process passes
         */
        RHIDescriptorSetLayout* GetPostProcessSetLayout() const { return m_postProcessSetLayout.Get(); }

        /**
         * @brief Get the descriptor set layout used by the procedural skybox pass
         */
        RHIDescriptorSetLayout* GetSkyboxSetLayout() const { return m_skyboxSetLayout.Get(); }

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
         * @brief Get the frame constants descriptor set (set 0)
         */
        RHIDescriptorSet* GetFrameDescriptorSet();

        /**
         * @brief Update frame-scope directional shadow texture/sampler bindings.
         */
        DirectionalShadowFrameBindingResult UpdateDirectionalShadowFrameResources(
            const DirectionalShadowFrameResources& resources);

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

        static const char* GetDirectionalShadowFallbackReasonName(DirectionalShadowFallbackReason reason);
        static const char* GetFrameLightFallbackReasonName(FrameLightFallbackReason reason);

        /**
         * @brief Backward-compatible alias for frame constants
         */
        RHIDescriptorSet* GetViewDescriptorSet() { return GetFrameDescriptorSet(); }

        /**
         * @brief Get the object constants descriptor set (set 1)
         */
        RHIDescriptorSet* GetObjectDescriptorSet();

        /**
         * @brief Get the material descriptor set layout (set 2)
         */
        RHIDescriptorSetLayout* GetMaterialSetLayout() const;

        /**
         * @brief Get dynamic constant-buffer offset for the current object slot
         */
        std::array<uint32, 1> GetCurrentObjectDynamicOffset() const;

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
         * @brief Update per-object constants
         * @param worldMatrix The object's world matrix
         */
        void UpdateObjectConstants(const Mat4& worldMatrix, const Mat4& normalMatrix);

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
        bool CreateSkyboxPipelineLayout();
        bool CreatePipeline();
        RHIPipelineRef GetOrCreateDefaultLitPipeline(MaterialPipelineVariant variant,
                                                     const char* debugName,
                                                     const RHIDepthStencilState& depthStencilState,
                                                     const RHIBlendState& blendState,
                                                     RHIFormat renderTargetFormat,
                                                     bool updatePrimaryStats);
        RHIPipelineRef GetOrCreateDepthOnlyPipeline();
        RHIPipelineRef GetOrCreateShadowDepthPipeline(const ShadowDepthBiasState& biasState);
        RHIPipelineRef GetOrCreateSkyboxPipeline(RHIFormat outputFormat,
                                                 bool depthTest = true,
                                                 bool updatePrimaryStats = true);
        RHIPipelineRef GetOrCreateToneMappingPipeline(RHIFormat outputFormat);
        RHIPipelineRef GetOrCreateBloomPipeline(RHIFormat outputFormat);
        RHIPipelineRef GetOrCreateBloomAdditivePipeline(RHIFormat outputFormat);
        RHIPipelineRef GetOrCreateColorGradingPipeline(RHIFormat outputFormat);
        RHIPipelineRef GetOrCreateChromaticAberrationPipeline(RHIFormat outputFormat);
        RHIPipelineRef GetOrCreateFXAAPipeline(RHIFormat outputFormat);
        RHIPipelineRef GetOrCreateVignettePipeline(RHIFormat outputFormat);
        RHIGraphicsPipelineDesc BuildDefaultLitPipelineDesc(const char* debugName,
                                                            const RHIDepthStencilState& depthStencilState,
                                                            const RHIBlendState& blendState,
                                                            RHIFormat renderTargetFormat) const;
        RHIGraphicsPipelineDesc BuildDepthOnlyPipelineDesc() const;
        RHIGraphicsPipelineDesc BuildShadowDepthPipelineDesc(const ShadowDepthBiasState& biasState) const;
        RHIGraphicsPipelineDesc BuildSkyboxPipelineDesc(RHIFormat outputFormat, bool depthTest = true) const;
        RHIGraphicsPipelineDesc BuildToneMappingPipelineDesc(RHIFormat outputFormat) const;
        RHIGraphicsPipelineDesc BuildBloomPipelineDesc(RHIFormat outputFormat) const;
        RHIGraphicsPipelineDesc BuildBloomAdditivePipelineDesc(RHIFormat outputFormat) const;
        RHIGraphicsPipelineDesc BuildColorGradingPipelineDesc(RHIFormat outputFormat) const;
        RHIGraphicsPipelineDesc BuildChromaticAberrationPipelineDesc(RHIFormat outputFormat) const;
        RHIGraphicsPipelineDesc BuildFXAAPipelineDesc(RHIFormat outputFormat) const;
        RHIGraphicsPipelineDesc BuildVignettePipelineDesc(RHIFormat outputFormat) const;
        bool CreateViewConstantBuffer();
        bool CreateObjectConstantBuffer();
        bool EnsureFrameShadowFallbackResources();
        bool EnsureFrameLightFallbackResources();
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
        std::unique_ptr<ShaderManager> m_shaderManager;

        // Shaders
        RHIShaderRef m_vertexShader;
        RHIShaderRef m_pixelShader;
        RHIShaderRef m_depthOnlyVertexShader;
        RHIShaderRef m_skyboxVertexShader;
        RHIShaderRef m_skyboxPixelShader;
        RHIShaderRef m_toneMappingVertexShader;
        RHIShaderRef m_toneMappingPixelShader;
        RHIShaderRef m_bloomVertexShader;
        RHIShaderRef m_bloomPixelShader;
        RHIShaderRef m_colorGradingVertexShader;
        RHIShaderRef m_colorGradingPixelShader;
        RHIShaderRef m_chromaticAberrationVertexShader;
        RHIShaderRef m_chromaticAberrationPixelShader;
        RHIShaderRef m_fxaaVertexShader;
        RHIShaderRef m_fxaaPixelShader;
        RHIShaderRef m_vignetteVertexShader;
        RHIShaderRef m_vignettePixelShader;
        std::unique_ptr<ShaderCompileResult> m_vsCompileResult;
        std::unique_ptr<ShaderCompileResult> m_psCompileResult;
        std::unique_ptr<ShaderCompileResult> m_depthOnlyVsCompileResult;
        std::unique_ptr<ShaderCompileResult> m_skyboxVsCompileResult;
        std::unique_ptr<ShaderCompileResult> m_skyboxPsCompileResult;
        std::unique_ptr<ShaderCompileResult> m_toneMappingVsCompileResult;
        std::unique_ptr<ShaderCompileResult> m_toneMappingPsCompileResult;
        std::unique_ptr<ShaderCompileResult> m_bloomVsCompileResult;
        std::unique_ptr<ShaderCompileResult> m_bloomPsCompileResult;
        std::unique_ptr<ShaderCompileResult> m_colorGradingVsCompileResult;
        std::unique_ptr<ShaderCompileResult> m_colorGradingPsCompileResult;
        std::unique_ptr<ShaderCompileResult> m_chromaticAberrationVsCompileResult;
        std::unique_ptr<ShaderCompileResult> m_chromaticAberrationPsCompileResult;
        std::unique_ptr<ShaderCompileResult> m_fxaaVsCompileResult;
        std::unique_ptr<ShaderCompileResult> m_fxaaPsCompileResult;
        std::unique_ptr<ShaderCompileResult> m_vignetteVsCompileResult;
        std::unique_ptr<ShaderCompileResult> m_vignettePsCompileResult;

        // Descriptor set layouts and pipeline layout
        std::vector<RHIDescriptorSetLayoutRef> m_setLayouts;
        RHIPipelineLayoutRef m_pipelineLayout;
        RHIDescriptorSetLayoutRef m_postProcessSetLayout;
        RHIPipelineLayoutRef m_postProcessPipelineLayout;
        RHIDescriptorSetLayoutRef m_skyboxSetLayout;
        RHIPipelineLayoutRef m_skyboxPipelineLayout;

        // Graphics pipelines
        RHIPipelineRef m_opaquePipeline;
        RHIPipelineRef m_maskedPipeline;
        RHIPipelineRef m_transparentPipeline;
        RHIPipelineRef m_depthOnlyPipeline;
        RHIPipelineRef m_skyboxPipeline;
        RHIPipelineRef m_toneMappingPipeline;
        RHIPipelineRef m_bloomPipeline;
        RHIPipelineRef m_bloomAdditivePipeline;
        RHIPipelineRef m_colorGradingPipeline;
        RHIPipelineRef m_chromaticAberrationPipeline;
        RHIPipelineRef m_fxaaPipeline;
        RHIPipelineRef m_vignettePipeline;
        std::unordered_map<uint64, RHIPipelineRef> m_pipelineCache;

        // Frame and object constants
        RHIBufferRef m_viewConstantBuffer;
        RHIBufferRef m_objectConstantBuffer;
        RHIDescriptorSetRef m_frameDescriptorSet;
        RHIDescriptorSetRef m_objectDescriptorSet;
        RHITextureRef m_fallbackDirectionalShadowTexture;
        RHITextureViewRef m_fallbackDirectionalShadowView;
        RHISamplerRef m_directionalShadowSampler;
        RHIBufferRef m_fallbackLightConstantsBuffer;
        RHIBufferRef m_fallbackPointLightsBuffer;
        RHIBufferRef m_fallbackSpotLightsBuffer;
        RHITextureView* m_currentDirectionalShadowView = nullptr;
        RHISampler* m_currentDirectionalShadowSampler = nullptr;
        RHIBuffer* m_currentLightConstantsBuffer = nullptr;
        RHIBuffer* m_currentPointLightsBuffer = nullptr;
        RHIBuffer* m_currentSpotLightsBuffer = nullptr;
        DirectionalShadowFrameBindingResult m_lastDirectionalShadowFrameBindingResult;
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
