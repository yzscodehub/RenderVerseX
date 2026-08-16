#pragma once

/**
 * @file RenderFrameTypes.h
 * @brief Shared scene, view, frame-policy, and extraction value contracts.
 */

#include "Core/MathTypes.h"
#include "Core/Diagnostics/SkinningPaletteHash.h"
#include "Core/Types.h"
#include "RenderContracts/FeatureRenderSnapshot.h"
#include "RenderContracts/RenderIdentity.h"
#include "RenderContracts/RenderMaterial.h"

#include <span>
#include <vector>

namespace RVX
{
    enum class RenderLightType : uint8
    {
        Directional = 0,
        Point = 1,
        Spot = 2
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

    struct RenderViewSnapshot
    {
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
        float32 absoluteTime = 0.0f;
        float32 deltaTime = 0.0f;
        float32 exposure = 1.0f;
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
    };

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

    enum class RenderExposureMode : uint8
    {
        ManualMultiplier = 0,
        CameraEV100 = 1
    };

    struct RenderPostProcessSettings
    {
        bool enabled = true;
        bool enableTAA = true;
        bool enableBloom = true;
        bool enableSSAO = true;
        bool enableSSR = true;
        bool enableRayTracedReflectionDenoise = true;
        RenderToneMappingOperator toneMappingOperator =
            RenderToneMappingOperator::ACES;
        RenderExposureMode exposureMode = RenderExposureMode::ManualMultiplier;
        float32 exposure = 1.0f;
        float32 cameraEV100 = 0.0f;
        float32 exposureCompensationEV = 0.0f;
        float32 gamma = 2.2f;
        float32 bloomThreshold = 1.0f;
        float32 bloomIntensity = 1.0f;
        float32 bloomRadius = 0.5f;
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
        float32 renderScale = 1.0f;
        uint32 debugView = 0;
        RenderInstancingMode instancingMode = RenderInstancingMode::Auto;
        RenderPostProcessSettings postProcess;
        RenderShadowSettings shadows;
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
