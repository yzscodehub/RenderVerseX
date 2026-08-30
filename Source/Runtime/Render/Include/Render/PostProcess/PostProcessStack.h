#pragma once

/**
 * @file PostProcessStack.h
 * @brief Post-processing effect chain manager
 */

#include "Core/MathTypes.h"
#include "Core/Types.h"
#include "Render/Graph/RenderGraph.h"
#include "Render/PostProcess/Exposure.h"
#include "Render/PostProcess/PostProcessTypes.h"
#include "Render/PostProcess/ToneMappingTypes.h"
#include "RenderContracts/RenderFrameTypes.h"
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
        PostProcessTextureMetadata sceneColorMetadata{};
        RGTextureHandle depth;
        PostProcessTextureMetadata depthMetadata{};
        RGTextureHandle normal;
        PostProcessTextureMetadata normalMetadata{};
        /** Full-resolution R32_UINT index into materialParameterTable. */
        RGTextureHandle materialIdentity;
        PostProcessTextureMetadata materialIdentityMetadata{};
        /** Structured MaterialGPUConstants rows addressed by materialIdentity. */
        RGBufferHandle materialParameterTable;
        PostProcessMaterialTableMetadata materialParameterTableMetadata{};
        RGTextureHandle velocity;
        PostProcessTextureMetadata velocityMetadata{};
        RGTextureHandle reactiveMask;
        PostProcessTextureMetadata reactiveMaskMetadata{};
        /** Unique graph import of the prior-presented exposure state. */
        RGBufferHandle exposureState;
        /** Semantic identity of the state bound to this stage's consumers. */
        uint64 exposureStateContentHash = 0;
        RHIFormat outputFormat = RHIFormat::Unknown;
        uint64 frameIndex = 0;
        float32 fixedDeltaSeconds = 0.0f;
        Vec2 jitterOffset = Vec2(0.0f);
        Mat4 currentViewProjection = Mat4(1.0f);
        Mat4 previousViewProjection = Mat4(1.0f);
        /**
         * Semantic domains are supplied by the renderer feature that owns the
         * view.  They are intentionally independent of RHIFormat: an UNORM
         * texture alone does not say whether its values are scene-linear or
         * display encoded.
         */
        PostProcessColorDomain sceneColorDomain = PostProcessColorDomain::SceneLinearHDR;
        PostProcessColorDomain outputColorDomain = PostProcessColorDomain::Unknown;
        bool currentViewProjectionValid = false;
        bool previousViewProjectionValid = false;
        bool resetTemporalHistory = false;
        /** Exact compatibility key for the candidate history output. */
        RenderViewHistoryIdentity historyIdentity{};
        /** Exact reason(s) a previous history resource cannot be sampled. */
        RenderHistoryResetReason historyResetReasons =
            RenderHistoryResetReason::HistoryUnavailable;
        /** True only when the exact prior-presented history is reusable. */
        bool historyAvailable = false;

        bool HasDepth() const { return depth.IsValid(); }
        bool HasNormal() const { return normal.IsValid(); }
        bool HasMaterialIdentity() const { return materialIdentity.IsValid(); }
        bool HasMaterialParameterTable() const
        {
            return materialParameterTable.IsValid();
        }
        bool HasVelocity() const { return velocity.IsValid(); }
        bool HasExposureState() const { return exposureState.IsValid(); }
        bool HasHistory() const
        {
            return historyAvailable && !resetTemporalHistory &&
                   historyIdentity.IsValid() &&
                   historyResetReasons == RenderHistoryResetReason::None;
        }
    };

    struct PostProcessFrameInputRequirements
    {
        bool requiresDepth = false;
        bool requiresNormal = false;
        bool requiresMaterialIdentity = false;
        bool requiresMaterialParameterTable = false;
        bool requiresVelocity = false;
        bool requiresReactiveMask = false;
        bool requiresHistory = false;

        bool IsSatisfiedBy(const PostProcessFrameInputs& inputs) const
        {
            return (!requiresDepth || inputs.HasDepth()) &&
                   (!requiresNormal || inputs.HasNormal()) &&
                   (!requiresMaterialIdentity ||
                    inputs.HasMaterialIdentity()) &&
                   (!requiresMaterialParameterTable ||
                    inputs.HasMaterialParameterTable()) &&
                   (!requiresVelocity || inputs.HasVelocity()) &&
                   (!requiresReactiveMask || inputs.reactiveMask.IsValid()) &&
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
        RenderExposureSettings exposureSettings{};
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
        // One public enum selects the complete anti-aliasing path. A frame
        // cannot request both FXAA and temporal AA.
        bool enableVelocityOutput = false;
        RenderAntiAliasingMode antiAliasingMode =
            RenderAntiAliasingMode::FXAA;
        float fxaaQuality = 0.75f;

        // =========================================================================
        // Stylized outlines
        // =========================================================================
        /** Requires typed full-resolution depth, oct-normal, and material rows. */
        bool enableToonOutline = false;
        /** Relative linear/device depth discontinuity threshold. */
        float toonOutlineDepthThreshold = 0.015f;
        /** 1-dot(n0,n1) discontinuity threshold for normal creases. */
        float toonOutlineNormalThreshold = 0.20f;

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
        // SSR
        // =========================================================================
        bool enableSSR = false;

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

    };

    void ApplyRenderVisualQualityPreset(PostProcessSettings& settings, RenderVisualQualityPreset preset);

    [[nodiscard]] constexpr PostProcessColorDomain
    ResolvePostProcessPresentationColorDomain(
        ToneMappingOutputColorSpace outputColorSpace) noexcept
    {
        return outputColorSpace == ToneMappingOutputColorSpace::Linear
            ? PostProcessColorDomain::DisplayLinearLDR
            : PostProcessColorDomain::DisplayEncodedLDR;
    }

    /**
     * @brief Base interface for post-process effects.
     *
     * Execution semantics come exclusively from Describe(). GetName() and
     * GetPriority() are diagnostic compatibility helpers and are never used by
     * the planner to infer color domains, ordering, or tone-map behavior.
     */
    class IPostProcessPass
    {
    public:
        virtual ~IPostProcessPass() = default;

        virtual PostProcessPassDescriptor Describe() const = 0;

        /**
         * @brief Get the effect name
         */
        virtual const char* GetName() const { return Describe().debugName; }

        /**
         * @brief Get execution priority (lower runs first)
         */
        virtual int32 GetPriority() const { return Describe().order; }

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
         * @brief Validate pass-specific typed inputs before the stack mutates
         *        the RenderGraph.
         *
         * Requirements only describe handle presence. Effects with ABI,
         * format, extent, or immutable-content constraints override this to
         * fail closed during plan construction.
         */
        virtual bool ValidateFrameInputs(const PostProcessFrameInputs&,
                                         std::string& outReason) const
        {
            outReason.clear();
            return true;
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
        /** Keep LDR/overlay descriptors in the same domain as ToneMapping. */
        void ConfigurePresentationColorDomain(const PostProcessSettings& settings)
        {
            m_presentationColorDomain = ResolvePostProcessPresentationColorDomain(
                settings.toneMappingOutputColorSpace);
        }

        [[nodiscard]] PostProcessColorDomain GetPresentationColorDomain() const noexcept
        {
            return m_presentationColorDomain;
        }

        void MarkUnsupported(const char* reason)
        {
            m_supported = false;
            m_unsupportedReason = reason ? reason : "Unsupported";
        }

        bool m_enabled = true;
        bool m_supported = true;
        PostProcessColorDomain m_presentationColorDomain =
            PostProcessColorDomain::DisplayEncodedLDR;
        std::string m_unsupportedReason;
    };

    struct PostProcessEffectExecutionPlan
    {
        PostProcessPassId passId = PostProcessPassId::Invalid;
        std::string effectName;
        uint32 sequenceIndex = 0;
        PostProcessStage stage = PostProcessStage::HDRPost;
        int32 order = 0;
        /** @deprecated Use order. Kept for existing diagnostics consumers. */
        int32 priority = 0;
        bool requested = false;
        bool supported = false;
        bool enabled = false;
        bool scheduled = false;
        PostProcessColorDomain inputDomain = PostProcessColorDomain::Unknown;
        PostProcessColorDomain outputDomain = PostProcessColorDomain::Unknown;
        PostProcessTransfer transfer = PostProcessTransfer::None;
        RHIFormat inputFormat = RHIFormat::Unknown;
        RHIFormat outputFormat = RHIFormat::Unknown;
        bool inputIsSceneColor = false;
        bool outputIsTransientIntermediate = false;
        bool outputIsFinalTarget = false;
        bool pipelineReady = false;
        bool requiresDepth = false;
        bool requiresNormal = false;
        bool requiresMaterialIdentity = false;
        bool requiresMaterialParameterTable = false;
        bool requiresVelocity = false;
        bool requiresReactiveMask = false;
        bool requiresHistory = false;
        bool frameInputsSatisfied = true;
        std::string pipelineReadinessReason;
        std::string missingFrameInputReason;
        std::string skippedReason;
        std::string reason;
    };

    /**
     * @brief Intent-specific invariants for a post-process execution plan.
     *
     * Generic stacks may intentionally perform no work when their input and
     * output share one semantic domain. Presentation is different: it owns
     * the HDR-to-display boundary and therefore must materialize exactly one
     * ToneMapping transform.
     */
    enum class PostProcessPlanContract : uint8
    {
        Generic = 0,
        Presentation,
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
        /** Number of requested ToneMapping transforms seen in this plan. */
        uint32 requestedToneMappingEffectCount = 0;
        /** Number of supported ToneMapping transforms eligible for scheduling. */
        uint32 supportedToneMappingEffectCount = 0;
        /** Number of enabled ToneMapping transforms in the selected chain. */
        uint32 enabledToneMappingEffectCount = 0;
        /** Number of ToneMapping transforms actually added to this graph. */
        uint32 scheduledToneMappingEffectCount = 0;
        uint32 graphPassCount = 0;
        uint32 transientIntermediateCount = 0;
        uint32 hdrIntermediateCount = 0;
        uint32 ldrIntermediateCount = 0;
        RHIFormat transientIntermediateFormat = RHIFormat::Unknown;
        RHIFormat hdrIntermediateFormat = RHIFormat::Unknown;
        RHIFormat ldrIntermediateFormat = RHIFormat::Unknown;
        RHIFormat finalOutputFormat = RHIFormat::Unknown;
        PostProcessColorDomain inputColorDomain = PostProcessColorDomain::Unknown;
        PostProcessColorDomain outputColorDomain = PostProcessColorDomain::Unknown;
        PostProcessPlanContract planContract = PostProcessPlanContract::Generic;
        /** Immutable semantic identity validated again by ExecutePlan(). */
        uint64 planFingerprint = 0;
        bool planValid = true;
        std::string planFailureReason;
        bool toneMappingBoundaryValid = true;
        std::string toneMappingBoundaryWarning;
        bool noEffectNoWork = false;
        bool fallbackCopyApplied = false;
        uint32 fallbackCopyPassCount = 0;
        std::string fallbackCopyReason;
        RenderVisualQualityPreset requestedQualityPreset = RenderVisualQualityPreset::Medium;
        RenderVisualQualityPreset appliedQualityPreset = RenderVisualQualityPreset::Medium;
        PostProcessFrameInputs frameInputs;
        ExposureGraphOutputs exposureOutputs{};
        ExposureControllerStats exposureStats{};
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
        void SetExposureResources(PipelineCache* pipelineCache);
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
         * @brief Build and validate a typed execution plan without mutating a
         * RenderGraph. Required semantic failures are reported in outStats.
         */
        bool BuildPlan(const PostProcessFrameInputs& frameInputs,
                       RHIFormat finalOutputFormat,
                       PostProcessStackExecuteStats& outStats) const;

        /**
         * @brief Build a plan under an explicit consumer contract.
         *
         * Presentation plans reject zero or multiple ToneMapping transforms
         * before a RenderGraph is mutated. Generic plans retain legitimate
         * no-op use cases when their color domains already match.
         */
        bool BuildPlan(const PostProcessFrameInputs& frameInputs,
                       RHIFormat finalOutputFormat,
                       PostProcessPlanContract contract,
                       PostProcessStackExecuteStats& outStats) const;

        /**
         * @brief Materialize a previously validated typed plan.
         *
         * This function never infers semantics from effect names or texture
         * formats, and it never inserts an identity/fallback copy after a
         * semantic-plan failure.
         */
        bool ExecutePlan(RenderGraph& graph,
                         const PostProcessFrameInputs& frameInputs,
                         RGTextureHandle output,
                         const PostProcessStackExecuteStats& plan);

        /**
         * @brief Evaluate configured effects without adding graph passes.
         *
         * This is a composition preflight: it validates stable descriptors,
         * ordering, color domains, and pipeline support, but deliberately does
         * not require per-frame depth, velocity, reactive-mask, or history
         * handles. BuildPlan remains the strict frame-ready contract.
         */
        PostProcessStackExecuteStats EvaluateEffects() const;

        /**
         * @brief Evaluate the selected chain against its concrete presentation format.
         *
         * Unlike the format-agnostic overload, this validates the HDR-to-output
         * boundary using the exact surface format that will receive the chain.
         */
        PostProcessStackExecuteStats EvaluateEffects(
            RHIFormat finalOutputFormat) const;

        /**
         * @brief Get current settings
         */
        PostProcessSettings& GetSettings() { return m_settings; }
        const PostProcessSettings& GetSettings() const { return m_settings; }

        /**
         * @brief Get statistics from the last Execute() call
         */
        const PostProcessStackExecuteStats& GetLastExecuteStats() const { return m_lastExecuteStats; }
        ExposureController& GetExposureController() noexcept
        {
            return m_exposureController;
        }
        const ExposureController& GetExposureController() const noexcept
        {
            return m_exposureController;
        }

    private:
        enum class FrameInputValidationMode : uint8
        {
            ConfigurationPreflight = 0,
            FrameReady,
        };

        std::vector<IPostProcessPass*> GatherEnabledEffects(PostProcessStackExecuteStats& stats,
                                                            bool logUnsupported,
                                                            const PostProcessFrameInputs* frameInputs = nullptr) const;
        bool BuildPlanImpl(const PostProcessFrameInputs& frameInputs,
                           RHIFormat finalOutputFormat,
                           PostProcessPlanContract contract,
                           FrameInputValidationMode frameInputValidationMode,
                           PostProcessStackExecuteStats& outStats) const;
        bool ValidateDescriptor(const PostProcessPassDescriptor& descriptor,
                                std::string& outReason) const;
        bool ResolveLegacyOutputColorDomain(RHIFormat format,
                                            PostProcessColorDomain& outDomain) const;
        void SortEffects();

        IRHIDevice* m_device = nullptr;
        PostProcessSettings m_settings;
        PostProcessStackExecuteStats m_lastExecuteStats;
        ExposureController m_exposureController;
        std::vector<std::unique_ptr<IPostProcessPass>> m_effects;
    };

} // namespace RVX
