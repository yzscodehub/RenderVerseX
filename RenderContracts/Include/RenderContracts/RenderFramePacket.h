#pragma once

/**
 * @file RenderFramePacket.h
 * @brief Legacy v4 full-scene packet retained only by compatibility tests.
 */

#include "RenderContracts/RenderFrameTypes.h"

#include <memory>
#include <vector>

namespace RVX
{
    inline constexpr uint32 RVX_RENDER_FRAME_PACKET_SCHEMA_ID = 0x52565846U;
    inline constexpr uint32 RVX_RENDER_FRAME_PACKET_SCHEMA_VERSION = 4;

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

    class RenderFramePacketBuilder;

    /** @brief Legacy immutable full-scene snapshot used by compatibility tests. */
    class RenderFramePacket final
    {
    public:
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
} // namespace RVX
