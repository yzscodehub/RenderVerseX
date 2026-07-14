#pragma once

/**
 * @file RenderFramePacket.h
 * @brief Immutable complete render-frame value contract.
 */

#include "Core/MathTypes.h"
#include "Core/Types.h"
#include "RenderContracts/FeatureRenderSnapshot.h"
#include "RenderContracts/RenderIdentity.h"

#include <vector>

namespace RVX
{
    inline constexpr uint32 RVX_RENDER_FRAME_PACKET_SCHEMA_ID = 0x52565846U;
    inline constexpr uint32 RVX_RENDER_FRAME_PACKET_SCHEMA_VERSION = 1;

    enum class RenderLightType : uint8
    {
        Directional = 0,
        Point = 1,
        Spot = 2
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

    struct RenderPrimitiveSnapshot
    {
        uint64 objectId = 0;
        RenderResourceHandle mesh;
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
        RenderResourceHandle shadowResource;
        bool castsShadows = false;
    };

    struct RenderSkySnapshot
    {
        RenderResourceHandle skyTexture;
        Vec3 tint{1.0f};
        float32 intensity = 1.0f;
        float32 rotationRadians = 0.0f;
    };

    struct RenderEnvironmentSnapshot
    {
        RenderResourceHandle irradianceTexture;
        RenderResourceHandle prefilteredTexture;
        RenderResourceHandle brdfLutTexture;
        float32 intensity = 1.0f;
    };

    struct RenderPostProcessSettings
    {
        bool enabled = true;
        bool enableTAA = true;
        bool enableBloom = true;
        bool enableSSAO = true;
        bool enableSSR = true;
        float32 bloomThreshold = 1.0f;
        float32 bloomIntensity = 1.0f;
    };

    struct RenderShadowSettings
    {
        bool enabled = true;
        uint32 atlasResolution = 4096;
        uint32 cascadeCount = 4;
        float32 maxDistance = 200.0f;
    };

    struct RenderGPUCullingSettings
    {
        bool enabled = true;
        uint32 maxVisibleObjects = 1048576;
    };

    struct RenderRayTracingSettings
    {
        bool enabled = false;
        bool enableShadows = false;
        bool enableReflections = false;
        uint32 maxInstances = 262144;
        uint32 maxRaysPerPixel = 1;
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
