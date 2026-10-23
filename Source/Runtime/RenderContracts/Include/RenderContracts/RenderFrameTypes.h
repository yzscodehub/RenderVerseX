#pragma once

/**
 * @file RenderFrameTypes.h
 * @brief Shared scene, view, frame-policy, and extraction value contracts.
 */

#include "Core/MathTypes.h"
#include "Core/Diagnostics/SkinningPaletteHash.h"
#include "Core/Types.h"
#include "RenderContracts/FeatureRenderSnapshot.h"
#include "RenderContracts/RenderAmbientOcclusion.h"
#include "RenderContracts/RenderExposure.h"
#include "RenderContracts/RenderIdentity.h"
#include "RenderContracts/RenderMaterial.h"
#include "RenderContracts/RenderPipelineProfile.h"

#include <limits>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

namespace RVX
{
    enum class RenderLightType : uint8
    {
        Directional = 0,
        Point = 1,
        Spot = 2
    };

    /**
     * @brief Authoring unit carried by RenderLightSnapshot::intensity.
     *
     * Unspecified is a construction sentinel only. It is rejected at the
     * extraction, mutation, and RenderScene boundaries so unitless lights
     * cannot enter a runtime lighting plan.
     */
    enum class RenderLightIntensityUnit : uint8
    {
        Unspecified = 0,
        Lux = 1,
        Candela = 2,
        Lumen = 3,
    };

    enum class RenderLightCanonicalizationCode : uint8
    {
        Success = 0,
        InvalidLightType,
        UnspecifiedIntensityUnit,
        InvalidIntensityUnit,
        InvalidIntensity,
        IncompatibleIntensityUnit,
        InvalidSpotCone,
        InvalidColorTemperature,
        IntensityOverflow,
    };

    /** @brief Canonical photometric result consumed by LightingFramePlan. */
    struct RenderLightCanonicalizationResult
    {
        RenderLightCanonicalizationCode code =
            RenderLightCanonicalizationCode::InvalidLightType;
        /** Directional lights canonicalize to Lux; local lights to Candela. */
        RenderLightIntensityUnit canonicalUnit = RenderLightIntensityUnit::Lux;
        float32 canonicalIntensity = 0.0f;
        /** Validated, but not color-transformed, authoring temperature. */
        std::optional<float32> colorTemperatureKelvin{};

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return code == RenderLightCanonicalizationCode::Success;
        }
    };

    enum class RenderSkyMode : uint8
    {
        Disabled = 0,
        Cubemap = 1,
        Equirectangular = 2,
        Procedural = 3,
        SolidColor = 4
    };

    /** @brief Backend-neutral load/clear policy for one render view. */
    enum class RenderViewClearPolicy : uint8
    {
        Skybox = 0,
        SolidColor,
        DepthOnly,
        Nothing,
    };

    [[nodiscard]] constexpr bool IsValidRenderViewClearPolicy(
        RenderViewClearPolicy policy) noexcept
    {
        switch (policy)
        {
            case RenderViewClearPolicy::Skybox:
            case RenderViewClearPolicy::SolidColor:
            case RenderViewClearPolicy::DepthOnly:
            case RenderViewClearPolicy::Nothing: return true;
            default: return false;
        }
    }

    enum class RenderFrameCaptureKind : uint8
    {
        None = 0,
        Color = 1,
        Depth = 2,
        ObjectId = 3
    };

    enum class RenderExtractionCode : uint8
    {
        Complete = 0,
        MissingProvider = 1,
        InvalidResourceReference = 2,
        InvalidNumericValue = 3,
        CountMismatch = 4
    };

    /**
     * @brief Returns whether an object or light layer participates in a view.
     *
     * The predicate is deliberately value-only so Direct, GPU-driven, and
     * shadow/light selection all consume the same camera-layer semantics.
     */
    [[nodiscard]] constexpr bool IsRenderLayerVisible(
        uint32 layerMask,
        uint32 cullingMask) noexcept
    {
        return (layerMask & cullingMask) != 0;
    }

    /** @brief Deterministic update-owned time and sequence for one render frame. */
    struct RenderFrameStamp
    {
        uint64 sequence = 0;
        float64 absoluteTimeSeconds = 0.0;
        float32 deltaTimeSeconds = 0.0f;

        friend bool operator==(const RenderFrameStamp&, const RenderFrameStamp&) =
            default;
    };

    /** @brief Cross-thread identity of the exact scene/output configuration. */
    struct RenderFrameIdentity
    {
        uint64 frameSequence = 0;
        uint64 sceneRevision = 0;
        uint64 surfaceGeneration = 0;
        uint64 configurationRevision = 0;

        [[nodiscard]] constexpr bool IsValid() const noexcept
        {
            return frameSequence != 0 && sceneRevision != 0 &&
                   surfaceGeneration != 0 && configurationRevision != 0;
        }

        friend bool operator==(const RenderFrameIdentity&,
                               const RenderFrameIdentity&) = default;
    };

    /**
     * @brief Explicit ECS provenance for a retained RenderScene object.
     *
     * Render object IDs are opaque, collision-checked map keys. They are never
     * decoded as if they were packed ECS handles; source slot reuse is instead
     * validated through this value.
     */
    struct RenderSceneObjectSourceIdentity
    {
        uint64 sceneRuntimeId = 0;
        uint32 entitySlot = std::numeric_limits<uint32>::max();
        uint32 entityGeneration = 0;

        [[nodiscard]] constexpr bool IsValid() const noexcept
        {
            return sceneRuntimeId != 0 &&
                   entitySlot != std::numeric_limits<uint32>::max();
        }

        friend constexpr bool operator==(const RenderSceneObjectSourceIdentity&,
                                         const RenderSceneObjectSourceIdentity&) =
            default;
    };

    /** @brief Deterministic effect clock independent of presentation wall time. */
    struct EffectFrameTime
    {
        uint64 simulationTick = 0;
        float32 fixedDeltaSeconds = 0.0f;
        float64 simulationSeconds = 0.0;
        uint32 deterministicSeed = 0;

        friend bool operator==(const EffectFrameTime&, const EffectFrameTime&) =
            default;
    };

    /** @brief Backend-neutral output extent used by view and history contracts. */
    struct RenderOutputExtent
    {
        uint32 width = 0;
        uint32 height = 0;

        [[nodiscard]] constexpr bool IsValid() const noexcept
        {
            return width != 0 && height != 0;
        }

        friend bool operator==(const RenderOutputExtent&,
                               const RenderOutputExtent&) = default;
    };

    /** @brief Semantic role preventing histories from aliasing across view kinds. */
    enum class ViewRole : uint8
    {
        Main = 0,
        Shadow,
        Reflection,
        Editor,
    };

    [[nodiscard]] constexpr bool IsValidViewRole(ViewRole role) noexcept
    {
        switch (role)
        {
            case ViewRole::Main:
            case ViewRole::Shadow:
            case ViewRole::Reflection:
            case ViewRole::Editor: return true;
            default: return false;
        }
    }

    /** @brief Exact generation-qualified identity of one logical render view. */
    struct RenderViewIdentity
    {
        uint64 sceneRuntimeId = 0;
        uint64 cameraEntityPacked = 0;
        /** @brief Reserved for stereo/cubemap views; zero is the primary mono view. */
        uint32 subViewIndex = 0;
        ViewRole role = ViewRole::Main;

        [[nodiscard]] constexpr bool IsValid() const noexcept
        {
            return sceneRuntimeId != 0 && cameraEntityPacked != 0 &&
                   IsValidViewRole(role);
        }

        friend bool operator==(const RenderViewIdentity&,
                               const RenderViewIdentity&) = default;
    };

    struct RenderViewSnapshot
    {
        RenderViewIdentity identity;
        /** @brief Explicit camera discontinuity revision copied from Scene. */
        uint64 cameraCutRevision = 0;
        Mat4 viewMatrix{1.0f};
        Mat4 projectionMatrix{1.0f};
        Mat4 viewProjectionMatrix{1.0f};
        Mat4 inverseViewProjectionMatrix{1.0f};
        Vec3 cameraPosition{0.0f};
        Vec3 cameraDirection{0.0f, 0.0f, -1.0f};
        Vec3 cameraUp{0.0f, 1.0f, 0.0f};
        uint32 viewportX = 0;
        uint32 viewportY = 0;
        uint32 viewportWidth = 0;
        uint32 viewportHeight = 0;
        /** @brief Object and light layers this view is allowed to consume. */
        uint32 cullingMask = ~0U;
        /** @brief Main raster color/depth load semantics for this view. */
        RenderViewClearPolicy clearPolicy = RenderViewClearPolicy::Skybox;
        /** @brief Clear color used when clearPolicy requests a color clear. */
        Vec4 clearColor{0.1f, 0.1f, 0.15f, 1.0f};
        float32 nearPlane = 0.1f;
        float32 farPlane = 1000.0f;
        float64 absoluteTime = 0.0;
        float32 deltaTime = 0.0f;
        float32 exposure = 1.0f;
    };

    /**
     * @brief Immutable output family carried by one frame packet.
     *
     * GFX01 intentionally supports exactly one view. The collection is part of
     * the durable contract so future stereo or reflection families do not need
     * to reinterpret a process-global history slot.
     */
    struct RenderViewFamilySnapshot
    {
        RenderFrameStamp frame;
        RenderFrameIdentity identity;
        EffectFrameTime effectTime;
        RenderOutputExtent extent;
        uint64 familyId = 0;
        uint32 primaryViewIndex = 0;
        std::vector<RenderViewSnapshot> views;
    };

    /** @brief Exact key of temporal resources compatible with one presented view. */
    struct RenderViewHistoryIdentity
    {
        /** @brief Frame that produced the history resource represented by this key. */
        RenderFrameIdentity previousFrame;
        RenderViewIdentity view;
        RenderOutputExtent extent;
        uint64 familyId = 0;
        uint64 worldRevision = 0;
        uint64 temporalEpoch = 0;
        uint64 formatRevision = 0;
        uint64 surfaceGeneration = 0;
        uint64 cameraCutRevision = 0;

        [[nodiscard]] constexpr bool IsValid() const noexcept
        {
            return previousFrame.IsValid() && view.IsValid() &&
                   extent.IsValid() && familyId != 0 && worldRevision != 0 &&
                   temporalEpoch != 0 && formatRevision != 0 &&
                   surfaceGeneration != 0 &&
                   surfaceGeneration == previousFrame.surfaceGeneration &&
                   cameraCutRevision != 0;
        }

        friend bool operator==(const RenderViewHistoryIdentity&,
                               const RenderViewHistoryIdentity&) = default;
    };

    /** @brief Bit reasons explaining why a prior temporal resource cannot be used. */
    enum class RenderHistoryResetReason : uint32
    {
        None = 0,
        FirstFrame = 1U << 0U,
        ViewChanged = 1U << 1U,
        ViewFamilyChanged = 1U << 2U,
        SceneDiscontinuity = 1U << 3U,
        TemporalEpochChanged = 1U << 4U,
        CameraCut = 1U << 5U,
        SurfaceChanged = 1U << 6U,
        Resize = 1U << 7U,
        ExplicitDiscontinuity = 1U << 8U,
        HistoryUnavailable = 1U << 9U,
        FormatChanged = 1U << 10U,
        ConfigurationChanged = 1U << 11U,
    };

    [[nodiscard]] constexpr RenderHistoryResetReason operator|(
        RenderHistoryResetReason lhs,
        RenderHistoryResetReason rhs) noexcept
    {
        using Value = std::underlying_type_t<RenderHistoryResetReason>;
        return static_cast<RenderHistoryResetReason>(
            static_cast<Value>(lhs) | static_cast<Value>(rhs));
    }

    constexpr RenderHistoryResetReason& operator|=(
        RenderHistoryResetReason& lhs,
        RenderHistoryResetReason rhs) noexcept
    {
        lhs = lhs | rhs;
        return lhs;
    }

    [[nodiscard]] constexpr bool HasRenderHistoryResetReason(
        RenderHistoryResetReason reasons,
        RenderHistoryResetReason reason) noexcept
    {
        using Value = std::underlying_type_t<RenderHistoryResetReason>;
        return (static_cast<Value>(reasons) & static_cast<Value>(reason)) != 0;
    }

    /** @brief Fail-closed history decision for one accepted view candidate. */
    struct RenderHistoryAvailability
    {
        RenderViewHistoryIdentity identity;
        RenderHistoryResetReason resetReasons =
            RenderHistoryResetReason::HistoryUnavailable;
        bool available = false;

        [[nodiscard]] constexpr bool IsConsistent() const noexcept
        {
            return identity.IsValid() &&
                   (available ==
                    (resetReasons == RenderHistoryResetReason::None));
        }
    };

    struct RenderSubmeshMaterialBinding
    {
        uint32 submeshIndex = 0;
        RenderResourceHandle material;
        RenderMaterialMode materialMode = RenderMaterialMode::Opaque;
    };

    /**
     * @brief Source metadata for a complete, canonical skinning palette.
     *
     * `providerComponentId` is the packed Scene ComponentHandle, including its
     * generation. `sourceModelResourceId` intentionally remains the stable
     * resource identity rather than pretending ResourceId has a generation.
     */
    struct RenderSkinningPaletteMetadata
    {
        uint64 providerComponentId = 0;
        uint64 sourceModelResourceId = 0;
        uint64 poseSequence = 0;
        uint64 paletteHash = 0;
        uint32 paletteCount = 0;

        [[nodiscard]] bool IsValidFor(
            std::span<const Mat4> matrices) const noexcept
        {
            if (providerComponentId == 0 || sourceModelResourceId == 0 ||
                poseSequence == 0 || paletteCount == 0 ||
                paletteCount != matrices.size())
            {
                return false;
            }
            const SkinningPaletteHash computed =
                ComputeSkinningPaletteHash(matrices);
            return computed.IsValid() && computed.matrixCount == paletteCount &&
                   computed.value == paletteHash;
        }
    };

    struct RenderPrimitiveSnapshot
    {
        uint64 objectId = 0;
        RenderSceneObjectSourceIdentity source;
        RenderResourceHandle mesh;
        std::vector<RenderSubmeshMaterialBinding> submeshes;
        RenderResourceHandle material;
        RenderResourceHandle fallbackMesh;
        RenderResourceHandle fallbackMaterial;
        Mat4 worldTransform{1.0f};
        Mat4 previousWorldTransform{1.0f};
        Vec3 boundsMin{0.0f};
        Vec3 boundsMax{0.0f};
        uint32 flags = 0;
        uint32 layerMask = 0xFFFFFFFFU;
        uint64 sortKey = 0;
        std::vector<Mat4> skinMatrices;
        /** True when a Scene provider supplied this palette, even if malformed. */
        bool hasSkinningPaletteProvider = false;
        RenderSkinningPaletteMetadata skinningPalette{};

        [[nodiscard]] bool HasValidSkinningPalette() const noexcept
        {
            return hasSkinningPaletteProvider &&
                   skinningPalette.IsValidFor(skinMatrices);
        }
    };

    struct RenderLightSnapshot
    {
        uint64 lightId = 0;
        RenderSceneObjectSourceIdentity source;
        RenderLightType type = RenderLightType::Directional;
        Vec3 position{0.0f};
        Vec3 direction{0.0f, -1.0f, 0.0f};
        Vec3 color{1.0f};
        float32 intensity = 1.0f;
        float32 range = 0.0f;
        float32 innerConeRadians = 0.0f;
        float32 outerConeRadians = 0.0f;
        RenderResourceHandle shadowResource;
        /** @brief View-layer membership used during lighting/shadow selection. */
        uint32 layerMask = ~0U;
        bool castsShadows = false;
        /** Explicit authoring unit for intensity; Unspecified is rejected. */
        RenderLightIntensityUnit intensityUnit =
            RenderLightIntensityUnit::Unspecified;
        /** Optional blackbody authoring temperature in the closed [1000, 40000] K range. */
        std::optional<float32> colorTemperatureKelvin{};
    };

    /**
     * @brief Convert one light's intensity to the canonical photometric unit.
     *
     * Directional values become Lux. Point and Spot values become Candela.
     * Spot outerConeRadians is a half-opening angle; Lumen conversion uses
     * its cone solid angle 2*pi*(1-cos(outerConeRadians)).
     */
    [[nodiscard]] RenderLightCanonicalizationResult
    CanonicalizeRenderLightSnapshot(const RenderLightSnapshot& light) noexcept;

    struct RenderSkySnapshot
    {
        RenderSkyMode mode = RenderSkyMode::Disabled;
        RenderResourceHandle skyTexture;
        Vec3 tint{1.0f};
        Vec3 sunDirection{0.0f, 1.0f, 0.0f};
        Vec3 sunColor{1.0f};
        Vec3 zenithColor{0.2f, 0.4f, 0.8f};
        Vec3 horizonColor{0.7f, 0.8f, 0.9f};
        Vec3 groundColor{0.3f, 0.25f, 0.2f};
        float32 intensity = 1.0f;
        float32 rotationRadians = 0.0f;
        float32 blurLevel = 0.0f;
        float32 scatteringIntensity = 1.0f;
    };

    struct RenderEnvironmentSnapshot
    {
        RenderResourceHandle irradianceTexture;
        RenderResourceHandle prefilteredTexture;
        RenderResourceHandle brdfLutTexture;
        float32 intensity = 1.0f;
    };

    enum class RenderToneMappingOperator : uint8
    {
        Reinhard = 0,
        ReinhardExtended = 1,
        ACES = 2,
        Uncharted2 = 3,
        Neutral = 4,
        None = 5
    };

    /**
     * @brief The single anti-aliasing path selected for one rendered frame.
     *
     * A frame may not request both spatial and temporal anti-aliasing.  The
     * renderer resolves this value into the corresponding post-process
     * feature; temporal mode additionally requires a velocity input.
     */
    enum class RenderAntiAliasingMode : uint8
    {
        None = 0,
        FXAA = 1,
        TAA = 2,
    };

    [[nodiscard]] constexpr bool IsValidRenderAntiAliasingMode(
        RenderAntiAliasingMode mode) noexcept
    {
        switch (mode)
        {
            case RenderAntiAliasingMode::None:
            case RenderAntiAliasingMode::FXAA:
            case RenderAntiAliasingMode::TAA: return true;
            default: return false;
        }
    }

    struct RenderPostProcessSettings
    {
        bool enabled = true;
        /**
         * @brief Request the renderer-owned velocity output without claiming a
         * temporal resolve is available.
         */
        bool enableVelocityOutput = false;
        /** @brief The sole anti-aliasing method requested for this frame. */
        RenderAntiAliasingMode antiAliasingMode =
            RenderAntiAliasingMode::FXAA;
        bool enableBloom = true;
        bool enableColorGrading = true;
        /** @brief Material-aware Toon silhouette/crease pass in HDR. */
        bool enableToonOutline = false;
        /** @brief Relative device-depth discontinuity threshold. */
        float32 toonOutlineDepthThreshold = 0.015f;
        /** @brief Oct-normal discontinuity threshold in [0, 2]. */
        float32 toonOutlineNormalThreshold = 0.20f;
        /** @brief Immutable GTAO policy; only indirect lighting may consume it. */
        RenderAmbientOcclusionSettings ambientOcclusion{};
        /** @brief Reserved for the verified Hi-Z reflection path in GFX19. */
        bool enableSSR = false;
        bool enableRayTracedReflectionDenoise = true;
        RenderToneMappingOperator toneMappingOperator =
            RenderToneMappingOperator::ACES;
        /**
         * @brief Sole authority for renderer exposure policy.
         *
         * RenderViewSnapshot::exposure remains a compatibility value owned by
         * the scene/camera pipeline; it does not override this setting.
         */
        RenderExposureSettings exposureSettings{};
        float32 gamma = 2.2f;
        float32 bloomThreshold = 1.0f;
        float32 bloomIntensity = 1.0f;
        float32 bloomRadius = 0.5f;
        float32 colorGradingContrast = 1.0f;
        float32 colorGradingSaturation = 1.0f;
        float32 colorGradingBrightness = 0.0f;
    };

    struct RenderShadowSettings
    {
        bool enabled = true;
        uint32 atlasResolution = 4096;
        uint32 cascadeCount = 4;
        float32 maxDistance = 200.0f;
        float32 cascadeSplitLambda = 0.95f;
        float32 filterRadiusTexels = 1.0f;
        float32 shadowBias = 0.005f;
        float32 normalBias = 0.02f;
        float32 cascadeBlendRatio = 0.05f;
    };

    /**
     * @brief Immutable, backend-neutral participating-media policy for one frame.
     *
     * A zero density is an intentional exact-identity request: composition must
     * not allocate, sample, or composite a volumetric field for that frame.
     * Every remaining value stays explicit and validated even in that mode, so
     * enabling the effect on a later frame cannot expose latent invalid state.
     */
    struct RenderVolumetricLightingSettings
    {
        bool enabled = false;
        bool enableTemporalReprojection = true;
        bool enableDirectionalShadowing = true;
        bool enableLocalShadowing = true;
        /** Extinction/scattering density in reciprocal meters. */
        float32 density = 0.0f;
        /** Henyey-Greenstein anisotropy in the open physical interval (-1, 1). */
        float32 anisotropy = 0.0f;
        float32 maxDistanceMeters = 128.0f;
        float32 heightFogBaseMeters = 0.0f;
        /** Non-negative exponential height-falloff coefficient in reciprocal meters. */
        float32 heightFogFalloffPerMeter = 0.0f;
        /** Temporal-history contribution when reprojection is enabled. */
        float32 temporalBlend = 0.90f;
    };

    enum class RenderGPUDrivenMode : uint8
    {
        Auto = 0,
        ForceEnabled,
        ForceDisabled,
    };

    inline const char* GetRenderGPUDrivenModeName(RenderGPUDrivenMode mode)
    {
        switch (mode)
        {
            case RenderGPUDrivenMode::Auto: return "Auto";
            case RenderGPUDrivenMode::ForceEnabled: return "ForceEnabled";
            case RenderGPUDrivenMode::ForceDisabled: return "ForceDisabled";
            default: return "Invalid";
        }
    }

    struct RenderGPUCullingSettings
    {
        RenderGPUDrivenMode mode = RenderGPUDrivenMode::Auto;
        uint32 maxVisibleObjects = 65536;
        bool enableOcclusionCulling = false;
        bool enableDistanceCulling = true;
        float32 maxDrawDistance = 1000.0f;
    };

    struct RenderRayTracingSettings
    {
        bool enabled = false;
        bool enableShadows = false;
        bool enableReflections = false;
        uint32 maxInstances = 262144;
        uint32 maxRaysPerPixel = 1;
        bool budgetEnabled = false;
        uint64 maxRayCount = 0;
        uint64 maxDenoiseTapCount = 0;
        uint64 maxTrackedResourceBytes = 0;
        float32 maxMeasuredGpuMs = 0.0f;
        float32 maxShadowMeasuredGpuMs = 0.0f;
        float32 maxReflectionMeasuredGpuMs = 0.0f;
        uint32 gpuTimingAdjustmentFrameCount = 2;
    };

    struct RenderTemporalSettings
    {
        bool resetHistory = false;
        uint32 jitterIndex = 0;
    };

    enum class RenderInstancingMode : uint8
    {
        Disabled = 0,
        Auto,
    };

    [[nodiscard]] inline const char* GetRenderInstancingModeName(
        RenderInstancingMode mode) noexcept
    {
        switch (mode)
        {
            case RenderInstancingMode::Disabled: return "Disabled";
            case RenderInstancingMode::Auto: return "Auto";
            default: return "Invalid";
        }
    }

    struct RenderFrameSettings
    {
        /** Renderer recipe/profile remains independent of draw submission policy. */
        RenderPipelineRequest pipelineRequest{};
        float32 renderScale = 1.0f;
        uint32 debugView = 0;
        RenderInstancingMode instancingMode = RenderInstancingMode::Auto;
        RenderPostProcessSettings postProcess;
        RenderShadowSettings shadows;
        RenderVolumetricLightingSettings volumetricLighting;
        RenderGPUCullingSettings gpuCulling;
        RenderRayTracingSettings rayTracing;
        RenderTemporalSettings temporal;
    };

    struct RenderFrameCaptureRequest
    {
        uint64 requestId = 0;
        RenderFrameCaptureKind kind = RenderFrameCaptureKind::None;
        uint32 width = 0;
        uint32 height = 0;
        bool includeAlpha = false;
        /** @brief Opt-in screenshot-coordinate probe of ToneMapping's HDR input. */
        bool pixelProbeEnabled = false;
        uint32 pixelProbeX = 0;
        uint32 pixelProbeY = 0;
    };

    struct RenderExtractionDiagnostics
    {
        RenderExtractionCode code = RenderExtractionCode::Complete;
        uint32 skippedPrimitiveCount = 0;
        uint32 skippedLightCount = 0;
        uint32 skippedFeatureProviderCount = 0;
        /** @brief Number of authoritative complete Scene scans for this extraction. */
        uint32 fullScanCount = 0;
        /** @brief Number of retained Scene feed records consumed by this candidate. */
        uint32 changeFeedChangeCount = 0;
        /** @brief Number of distinct actors rebuilt from dirty feed records. */
        uint32 actorRebuildCount = 0;
        /** @brief Number of render proxy components visited by the bridge. */
        uint32 proxyVisitCount = 0;
        /** @brief Number of typed non-proxy Scene components visited. */
        uint32 componentVisitCount = 0;
        /** @brief Number of feature providers visited. */
        uint32 featureProviderVisitCount = 0;
        /** @brief True when the feed cursor required an authoritative resync. */
        bool continuityLost = false;
        /**
         * @brief Update-owner CPU time spent preparing this immutable packet.
         *
         * This is measured inside EcsFrameExtractor after candidate state and
         * packet inputs are complete, immediately before immutable allocation
         * and transport hand-off. Render must not replace it with frame-apply,
         * queue-wait, or submission time.
         */
        uint64 extractionDurationNanoseconds = 0;
        bool complete = false;
    };

    /**
     * @brief Update-owned aggregate of diagnostics from accepted extraction candidates.
     *
     * Unlike RenderExtractionDiagnostics, which describes one candidate frame,
     * this value is retained across accepted publications so mailbox replacement
     * cannot discard extraction evidence before update-side observers read it.
     */
    struct AcceptedExtractionDiagnosticsSnapshot
    {
        uint64 acceptedPublicationCount = 0;
        uint64 lastAcceptedSourceFrameSequence = 0;
        uint64 lastAcceptedSceneRevision = 0;
        uint64 cumulativeFullScanCount = 0;
        uint64 cumulativeChangeFeedChangeCount = 0;
        uint64 cumulativeActorRebuildCount = 0;
        uint64 cumulativeProxyVisitCount = 0;
        uint64 cumulativeComponentVisitCount = 0;
        uint64 cumulativeFeatureProviderVisitCount = 0;
        uint64 continuityLossCount = 0;
    };

    static_assert(static_cast<uint8>(RenderLightType::Directional) == 0);
    static_assert(static_cast<uint8>(RenderLightType::Point) == 1);
    static_assert(static_cast<uint8>(RenderLightType::Spot) == 2);
    static_assert(static_cast<uint8>(RenderLightIntensityUnit::Unspecified) == 0);
    static_assert(static_cast<uint8>(RenderLightIntensityUnit::Lux) == 1);
    static_assert(static_cast<uint8>(RenderLightIntensityUnit::Candela) == 2);
    static_assert(static_cast<uint8>(RenderLightIntensityUnit::Lumen) == 3);
    static_assert(static_cast<uint8>(RenderFrameCaptureKind::None) == 0);
    static_assert(static_cast<uint8>(RenderFrameCaptureKind::Color) == 1);
    static_assert(static_cast<uint8>(RenderFrameCaptureKind::Depth) == 2);
    static_assert(static_cast<uint8>(RenderFrameCaptureKind::ObjectId) == 3);
    static_assert(static_cast<uint8>(RenderExtractionCode::Complete) == 0);
    static_assert(static_cast<uint8>(RenderExtractionCode::MissingProvider) == 1);
    static_assert(static_cast<uint8>(RenderExtractionCode::InvalidResourceReference) == 2);
    static_assert(static_cast<uint8>(RenderExtractionCode::InvalidNumericValue) == 3);
    static_assert(static_cast<uint8>(RenderExtractionCode::CountMismatch) == 4);
} // namespace RVX
