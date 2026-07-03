#pragma once

/**
 * @file PostProcessStack.h
 * @brief Post-processing effect chain manager
 */

#include "Core/MathTypes.h"
#include "Core/Types.h"
#include "Render/Graph/RenderGraph.h"
#include "Render/PostProcess/ToneMappingTypes.h"
#include <vector>
#include <memory>
#include <functional>
#include <string>

namespace RVX
{
    class RenderGraph;
    class IRHIDevice;
    class RHICommandContext;

    struct PostProcessFrameInputs
    {
        RGTextureHandle sceneColor;
        RGTextureHandle depth;
        RGTextureHandle normal;
        RGTextureHandle velocity;
        RHIFormat outputFormat = RHIFormat::Unknown;
        uint64 frameIndex = 0;
        Vec2 jitterOffset = Vec2(0.0f);
        Mat4 currentViewProjection = Mat4(1.0f);
        Mat4 previousViewProjection = Mat4(1.0f);
        bool currentViewProjectionValid = false;
        bool previousViewProjectionValid = false;
        bool resetTemporalHistory = false;

        bool HasDepth() const { return depth.IsValid(); }
        bool HasNormal() const { return normal.IsValid(); }
        bool HasVelocity() const { return velocity.IsValid(); }
        bool HasHistory() const { return !resetTemporalHistory; }
    };

    struct PostProcessFrameInputRequirements
    {
        bool requiresDepth = false;
        bool requiresNormal = false;
        bool requiresVelocity = false;
        bool requiresHistory = false;

        bool IsSatisfiedBy(const PostProcessFrameInputs& inputs) const
        {
            return (!requiresDepth || inputs.HasDepth()) &&
                   (!requiresNormal || inputs.HasNormal()) &&
                   (!requiresVelocity || inputs.HasVelocity()) &&
                   (!requiresHistory || inputs.HasHistory());
        }
    };

    enum class RenderVisualQualityPreset : uint8
    {
        Off = 0,
        Low,
        Medium,
        High,
        Cinematic
    };

    const char* GetRenderVisualQualityPresetName(RenderVisualQualityPreset preset);

    /**
     * @brief Post-process settings accessible by all effects
     */
    struct PostProcessSettings
    {
        RenderVisualQualityPreset visualQualityPreset = RenderVisualQualityPreset::Medium;

        // =========================================================================
        // Tone mapping
        // =========================================================================
        bool enableToneMapping = true;
        float exposure = 1.0f;
        ToneMappingExposureMode exposureMode = ToneMappingExposureMode::ManualMultiplier;
        float cameraEV100 = 0.0f;
        float exposureCompensationEV = 0.0f;
        float gamma = 2.2f;
        ToneMappingOperator toneMappingOperator = ToneMappingOperator::ACES;
        ToneMappingOutputColorSpace toneMappingOutputColorSpace = ToneMappingOutputColorSpace::SRGB;

        // =========================================================================
        // Bloom
        // =========================================================================
        bool enableBloom = true;
        float bloomThreshold = 1.0f;
        float bloomIntensity = 1.0f;
        float bloomRadius = 0.5f;

        // =========================================================================
        // Anti-aliasing
        // =========================================================================
        bool enableFXAA = true;
        float fxaaQuality = 0.75f;

        // =========================================================================
        // Depth of Field
        // =========================================================================
        bool enableDOF = false;
        float dofFocusDistance = 10.0f;     ///< Focus distance in meters
        float dofAperture = 5.6f;           ///< f-stop (lower = more blur)
        float dofFocalLength = 50.0f;       ///< Lens focal length in mm

        // =========================================================================
        // Motion Blur
        // =========================================================================
        bool enableMotionBlur = false;
        float motionBlurIntensity = 1.0f;
        float motionBlurMaxVelocity = 32.0f;

        // =========================================================================
        // Color Grading
        // =========================================================================
        bool enableColorGrading = true;
        float contrast = 1.0f;
        float saturation = 1.0f;
        float brightness = 0.0f;

        // =========================================================================
        // Vignette
        // =========================================================================
        bool enableVignette = false;
        float vignetteIntensity = 0.3f;
        float vignetteRadius = 0.8f;

        // =========================================================================
        // Chromatic Aberration
        // =========================================================================
        bool enableChromaticAberration = false;
        float chromaticAberrationIntensity = 0.1f;

        // =========================================================================
        // Film Grain
        // =========================================================================
        bool enableFilmGrain = false;
        float filmGrainIntensity = 0.2f;

        // =========================================================================
        // Volumetric Lighting
        // =========================================================================
        bool enableVolumetricLighting = false;
        float volumetricIntensity = 1.0f;

        // =========================================================================
        // SSAO
        // =========================================================================
        bool enableSSAO = true;
        float ssaoRadius = 0.5f;
        float ssaoIntensity = 1.0f;

        // =========================================================================
        // SSR
        // =========================================================================
        bool enableSSR = false;
        float ssrMaxDistance = 50.0f;
        float ssrThickness = 0.1f;

        // =========================================================================
        // Ray-traced reflections
        // =========================================================================
        bool enableRayTracedReflections = false;
        bool enableRayTracedReflectionDenoise = true;
        float rayTracedReflectionIntensity = 1.0f;
        float rayTracedReflectionResolutionScale = 1.0f;
        float rayTracedReflectionMaxDistance = 50.0f;
        float rayTracedReflectionMaxRoughness = 1.0f;
        float rayTracedReflectionDistanceFadeStart = 0.8f;
        uint32 rayTracedReflectionInstanceMask = 0xFFu;
        uint32 rayTracedReflectionSamplesPerPixel = 1;
        float rayTracedReflectionRoughnessConeSpread = 1.0f;
        float rayTracedReflectionNormalBias = 0.02f;
        float rayTracedReflectionRayMinT = 0.001f;
        float rayTracedReflectionFireflyClamp = 64.0f;
        float rayTracedReflectionTemporalBlendFactor = 0.85f;
        float rayTracedReflectionHistoryDepthThreshold = 0.01f;
        float rayTracedReflectionHistoryNormalThreshold = 0.85f;
        float rayTracedReflectionHistoryLuminanceTolerance = 4.0f;
        float rayTracedReflectionHistoryConfidenceThreshold = 0.05f;
        float rayTracedReflectionHistoryVelocityRejectionScale = 8.0f;
        uint32 rayTracedReflectionDenoiseRadius = 1;
        float rayTracedReflectionDenoiseDepthSigma = 0.01f;
        float rayTracedReflectionDenoiseNormalThreshold = 0.85f;
        float rayTracedReflectionDenoiseConfidencePower = 1.0f;
        float rayTracedReflectionDenoiseCenterWeight = 1.0f;
        float rayTracedReflectionDenoiseLowConfidenceDepthScale = 4.0f;

        // =========================================================================
        // TAA
        // =========================================================================
        bool enableTAA = true;
        float taaJitterScale = 1.0f;
    };

    void ApplyRenderVisualQualityPreset(PostProcessSettings& settings, RenderVisualQualityPreset preset);

    /**
     * @brief Base interface for post-process effects
     */
    class IPostProcessPass
    {
    public:
        virtual ~IPostProcessPass() = default;

        /**
         * @brief Get the effect name
         */
        virtual const char* GetName() const = 0;

        /**
         * @brief Get execution priority (lower runs first)
         */
        virtual int32 GetPriority() const { return 0; }

        /**
         * @brief Check if this effect is enabled
         */
        virtual bool IsEnabled() const { return m_enabled && m_supported; }

        /**
         * @brief Check if this effect was requested even when unsupported
         */
        bool IsRequestedEnabled() const { return m_enabled; }

        /**
         * @brief Enable/disable the effect
         */
        virtual void SetEnabled(bool enabled) { m_enabled = enabled; }

        /**
         * @brief Check whether the pass has a real GPU implementation
         */
        bool IsSupported() const { return m_supported; }

        /**
         * @brief Human-readable reason when unsupported
         */
        const std::string& GetUnsupportedReason() const { return m_unsupportedReason; }

        /**
         * @brief Configure the effect based on settings
         */
        virtual void Configure(const PostProcessSettings& settings) = 0;

        /**
         * @brief Declare frame inputs required by this effect.
         */
        virtual PostProcessFrameInputRequirements GetFrameInputRequirements() const
        {
            return {};
        }

        /**
         * @brief Add the pass to the render graph
         * @param graph The render graph
         * @param input Input texture handle
         * @param output Output texture handle
         */
        virtual void AddToGraph(RenderGraph& graph, RGTextureHandle input, RGTextureHandle output) = 0;

        /**
         * @brief Add the pass to the render graph with the full frame input contract.
         */
        virtual void AddToGraph(RenderGraph& graph,
                                const PostProcessFrameInputs& frameInputs,
                                RGTextureHandle output)
        {
            AddToGraph(graph, frameInputs.sceneColor, output);
        }

    protected:
        void MarkUnsupported(const char* reason)
        {
            m_supported = false;
            m_unsupportedReason = reason ? reason : "Unsupported";
        }

        bool m_enabled = true;
        bool m_supported = true;
        std::string m_unsupportedReason;
    };

    enum class PostProcessColorDomain : uint8
    {
        Unknown = 0,
        HDR,
        LDR,
    };

    struct PostProcessEffectExecutionPlan
    {
        std::string effectName;
        uint32 sequenceIndex = 0;
        int32 priority = 0;
        bool requested = false;
        bool supported = false;
        bool enabled = false;
        bool scheduled = false;
        PostProcessColorDomain inputDomain = PostProcessColorDomain::Unknown;
        PostProcessColorDomain outputDomain = PostProcessColorDomain::Unknown;
        RHIFormat inputFormat = RHIFormat::Unknown;
        RHIFormat outputFormat = RHIFormat::Unknown;
        bool inputIsSceneColor = false;
        bool outputIsTransientIntermediate = false;
        bool outputIsFinalTarget = false;
        bool pipelineReady = false;
        bool requiresDepth = false;
        bool requiresNormal = false;
        bool requiresVelocity = false;
        bool requiresHistory = false;
        bool frameInputsSatisfied = true;
        std::string pipelineReadinessReason;
        std::string missingFrameInputReason;
        std::string skippedReason;
        std::string reason;
    };

    /**
     * @brief Manages post-processing effect chain
     *
     * PostProcessStack handles:
     * - Effect ordering by priority
     * - Ping-pong buffer management
     * - Integration with RenderGraph
     */
    struct PostProcessStackExecuteStats
    {
        uint32 requestedEffectCount = 0;
        uint32 unsupportedSkippedCount = 0;
        uint32 enabledEffectCount = 0;
        uint32 scheduledEffectCount = 0;
        uint32 graphPassCount = 0;
        uint32 transientIntermediateCount = 0;
        uint32 hdrIntermediateCount = 0;
        uint32 ldrIntermediateCount = 0;
        RHIFormat transientIntermediateFormat = RHIFormat::Unknown;
        RHIFormat hdrIntermediateFormat = RHIFormat::Unknown;
        RHIFormat ldrIntermediateFormat = RHIFormat::Unknown;
        RHIFormat finalOutputFormat = RHIFormat::Unknown;
        bool toneMappingBoundaryValid = true;
        std::string toneMappingBoundaryWarning;
        bool noEffectNoWork = false;
        bool fallbackCopyApplied = false;
        uint32 fallbackCopyPassCount = 0;
        std::string fallbackCopyReason;
        RenderVisualQualityPreset requestedQualityPreset = RenderVisualQualityPreset::Medium;
        RenderVisualQualityPreset appliedQualityPreset = RenderVisualQualityPreset::Medium;
        PostProcessFrameInputs frameInputs;
        std::vector<PostProcessEffectExecutionPlan> effectPlans;
    };

    class PostProcessStack
    {
    public:
        PostProcessStack() = default;
        ~PostProcessStack();

        // Non-copyable
        PostProcessStack(const PostProcessStack&) = delete;
        PostProcessStack& operator=(const PostProcessStack&) = delete;

        // =========================================================================
        // Lifecycle
        // =========================================================================

        void Initialize(IRHIDevice* device);
        void Shutdown();
        bool IsInitialized() const { return m_device != nullptr; }

        // =========================================================================
        // Effect Management
        // =========================================================================

        /**
         * @brief Add a post-process effect
         */
        template<typename T, typename... Args>
        T* AddEffect(Args&&... args)
        {
            auto effect = std::make_unique<T>(std::forward<Args>(args)...);
            T* ptr = effect.get();
            m_effects.push_back(std::move(effect));
            SortEffects();
            return ptr;
        }

        /**
         * @brief Get an effect by type
         */
        template<typename T>
        T* GetEffect()
        {
            for (auto& effect : m_effects)
            {
                if (auto* typed = dynamic_cast<T*>(effect.get()))
                    return typed;
            }
            return nullptr;
        }

        /**
         * @brief Remove all effects
         */
        void ClearEffects();

        // =========================================================================
        // Execution
        // =========================================================================

        /**
         * @brief Apply settings to all effects
         */
        void ApplySettings(const PostProcessSettings& settings);

        /**
         * @brief Execute the post-processing chain
         * @param graph The render graph
         * @param sceneColor Input scene color
         * @param output Final output target
         */
        void Execute(RenderGraph& graph, RGTextureHandle sceneColor, RGTextureHandle output);

        void Execute(RenderGraph& graph,
                     const PostProcessFrameInputs& frameInputs,
                     RGTextureHandle output);

        /**
         * @brief Evaluate currently configured effects without adding graph passes
         */
        PostProcessStackExecuteStats EvaluateEffects() const;

        /**
         * @brief Get current settings
         */
        PostProcessSettings& GetSettings() { return m_settings; }
        const PostProcessSettings& GetSettings() const { return m_settings; }

        /**
         * @brief Get statistics from the last Execute() call
         */
        const PostProcessStackExecuteStats& GetLastExecuteStats() const { return m_lastExecuteStats; }

    private:
        std::vector<IPostProcessPass*> GatherEnabledEffects(PostProcessStackExecuteStats& stats,
                                                            bool logUnsupported,
                                                            const PostProcessFrameInputs* frameInputs = nullptr) const;
        void SortEffects();

        IRHIDevice* m_device = nullptr;
        PostProcessSettings m_settings;
        PostProcessStackExecuteStats m_lastExecuteStats;
        std::vector<std::unique_ptr<IPostProcessPass>> m_effects;
    };

} // namespace RVX
