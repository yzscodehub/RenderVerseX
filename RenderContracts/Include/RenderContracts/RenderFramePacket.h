#pragma once

/**
 * @file RenderFramePacket.h
 * @brief Immutable complete render-frame value contract.
 */

#include "Core/MathTypes.h"
#include "Core/Types.h"
#include "RenderContracts/FeatureRenderSnapshot.h"
#include "RenderContracts/RenderIdentity.h"
#include "RenderContracts/RenderMaterial.h"

#include <memory>
#include <vector>

namespace RVX
{
    inline constexpr uint32 RVX_RENDER_FRAME_PACKET_SCHEMA_ID = 0x52565846U;
    inline constexpr uint32 RVX_RENDER_FRAME_PACKET_SCHEMA_VERSION = 4;

    enum class RenderLightType : uint8
    {
        Directional = 0,
        Point = 1,
        Spot = 2
    };

    /** @brief Immutable sky rendering mode carried by a render-frame packet. */
    enum class RenderSkyMode : uint8
    {
        Disabled = 0,
        Cubemap = 1,
        Equirectangular = 2,
        Procedural = 3,
        SolidColor = 4
    };

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

    struct RenderFrameHeader
    {
        uint32 schemaId = RVX_RENDER_FRAME_PACKET_SCHEMA_ID;
        uint32 schemaVersion = RVX_RENDER_FRAME_PACKET_SCHEMA_VERSION;
        uint64 sequence = 0;
        uint64 worldRevision = 0;
        uint64 temporalEpoch = 0;
        uint32 expectedPrimitiveCount = 0;
        uint32 extractedPrimitiveCount = 0;
        uint32 expectedLightCount = 0;
        uint32 extractedLightCount = 0;
        uint32 expectedFeatureProviderCount = 0;
        uint32 extractedFeatureProviderCount = 0;
        bool explicitDiscontinuity = false;
    };

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
        float32 nearPlane = 0.1f;
        float32 farPlane = 1000.0f;
        float32 absoluteTime = 0.0f;
        float32 deltaTime = 0.0f;
        float32 exposure = 1.0f;
    };

    /** @brief Owned per-submesh material selection carried across threads. */
    struct RenderSubmeshMaterialBinding
    {
        uint32 submeshIndex = 0;
        RenderResourceHandle material;
        RenderMaterialMode materialMode = RenderMaterialMode::Opaque;
    };

    struct RenderPrimitiveSnapshot
    {
        uint64 objectId = 0;
        RenderResourceHandle mesh;
        std::vector<RenderSubmeshMaterialBinding> submeshes;
        // Legacy first-submesh projections kept for existing packet consumers.
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
        /// Optional externally prepared shadow data. Ordinary shadow maps are
        /// allocated and owned by Render from the castsShadows intent.
        RenderResourceHandle shadowResource;
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
        RenderExposureMode exposureMode =
            RenderExposureMode::ManualMultiplier;
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

    /** @brief Runtime policy for selecting the GPU-driven rendering path. */
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

    struct RenderFrameSettings
    {
        float32 renderScale = 1.0f;
        uint32 debugView = 0;
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
    };

    struct RenderExtractionDiagnostics
    {
        RenderExtractionCode code = RenderExtractionCode::Complete;
        uint32 skippedPrimitiveCount = 0;
        uint32 skippedLightCount = 0;
        uint32 skippedFeatureProviderCount = 0;
        bool complete = false;
    };

    class RenderFramePacketBuilder;

    /** @brief Immutable complete snapshot consumed by Render. */
    class RenderFramePacket final
    {
    public:
        /** @brief Internal migration factory for persistent-scene consumers. */
        static std::unique_ptr<const RenderFramePacket> CreateCompatibility(
            RenderFrameHeader header,
            RenderViewSnapshot view,
            std::vector<RenderPrimitiveSnapshot> primitives,
            std::vector<RenderLightSnapshot> lights,
            RenderSkySnapshot sky,
            RenderEnvironmentSnapshot environment,
            RenderFrameSettings settings,
            RenderFrameCaptureRequest captureRequest,
            RenderFeatureSnapshot features,
            RenderExtractionDiagnostics extractionDiagnostics);

        ~RenderFramePacket() = default;
        RenderFramePacket() = delete;
        RenderFramePacket(const RenderFramePacket&) = delete;
        RenderFramePacket& operator=(const RenderFramePacket&) = delete;
        RenderFramePacket(RenderFramePacket&&) = delete;
        RenderFramePacket& operator=(RenderFramePacket&&) = delete;

        [[nodiscard]] const RenderFrameHeader& GetHeader() const noexcept;
        [[nodiscard]] const RenderViewSnapshot& GetView() const noexcept;
        [[nodiscard]] const std::vector<RenderPrimitiveSnapshot>&
            GetPrimitives() const noexcept;
        [[nodiscard]] const std::vector<RenderLightSnapshot>&
            GetLights() const noexcept;
        [[nodiscard]] const RenderSkySnapshot& GetSky() const noexcept;
        [[nodiscard]] const RenderEnvironmentSnapshot&
            GetEnvironment() const noexcept;
        [[nodiscard]] const RenderFrameSettings& GetSettings() const noexcept;
        [[nodiscard]] const RenderFrameCaptureRequest&
            GetCaptureRequest() const noexcept;
        [[nodiscard]] const RenderFeatureSnapshot& GetFeatures() const noexcept;
        [[nodiscard]] const RenderExtractionDiagnostics&
            GetExtractionDiagnostics() const noexcept;

    private:
        friend class RenderFramePacketBuilder;
        RenderFramePacket(RenderFrameHeader header,
                          RenderViewSnapshot view,
                          std::vector<RenderPrimitiveSnapshot> primitives,
                          std::vector<RenderLightSnapshot> lights,
                          RenderSkySnapshot sky,
                          RenderEnvironmentSnapshot environment,
                          RenderFrameSettings settings,
                          RenderFrameCaptureRequest captureRequest,
                          RenderFeatureSnapshot features,
                          RenderExtractionDiagnostics extractionDiagnostics);

        RenderFrameHeader m_header{};
        RenderViewSnapshot m_view{};
        std::vector<RenderPrimitiveSnapshot> m_primitives{};
        std::vector<RenderLightSnapshot> m_lights{};
        RenderSkySnapshot m_sky{};
        RenderEnvironmentSnapshot m_environment{};
        RenderFrameSettings m_settings{};
        RenderFrameCaptureRequest m_captureRequest{};
        RenderFeatureSnapshot m_features{};
        RenderExtractionDiagnostics m_extractionDiagnostics{};
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
