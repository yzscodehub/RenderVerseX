#pragma once

/**
 * @file RenderFramePacketV5.h
 * @brief Latest-complete-wins frame state referencing a persistent scene revision
 */

#include "RenderContracts/RenderFramePacket.h"

#include <memory>

namespace RVX
{
    inline constexpr uint32 RVX_RENDER_FRAME_PACKET_V5_SCHEMA_ID = 0x52564635U;
    inline constexpr uint32 RVX_RENDER_FRAME_PACKET_V5_SCHEMA_VERSION = 5;

    struct RenderFrameHeaderV5
    {
        uint32 schemaId = RVX_RENDER_FRAME_PACKET_V5_SCHEMA_ID;
        uint32 schemaVersion = RVX_RENDER_FRAME_PACKET_V5_SCHEMA_VERSION;
        uint64 sequence = 0;
        uint64 requiredSceneRevision = 0;
        uint64 worldRevision = 0;
        uint64 temporalEpoch = 0;
        bool explicitDiscontinuity = false;
    };

    class RenderFramePacketV5 final
    {
    public:
        static std::unique_ptr<const RenderFramePacketV5> Create(
            RenderFrameHeaderV5 header,
            RenderViewSnapshot view,
            RenderFrameSettings settings,
            RenderFrameCaptureRequest captureRequest,
            RenderExtractionDiagnostics diagnostics);

        [[nodiscard]] const RenderFrameHeaderV5& GetHeader() const { return m_header; }
        [[nodiscard]] const RenderViewSnapshot& GetView() const { return m_view; }
        [[nodiscard]] const RenderFrameSettings& GetSettings() const { return m_settings; }
        [[nodiscard]] const RenderFrameCaptureRequest& GetCaptureRequest() const
        {
            return m_captureRequest;
        }
        [[nodiscard]] const RenderExtractionDiagnostics& GetExtractionDiagnostics() const
        {
            return m_diagnostics;
        }

    private:
        RenderFramePacketV5(RenderFrameHeaderV5 header,
                            RenderViewSnapshot view,
                            RenderFrameSettings settings,
                            RenderFrameCaptureRequest captureRequest,
                            RenderExtractionDiagnostics diagnostics);

        RenderFrameHeaderV5 m_header;
        RenderViewSnapshot m_view;
        RenderFrameSettings m_settings;
        RenderFrameCaptureRequest m_captureRequest;
        RenderExtractionDiagnostics m_diagnostics;
    };
} // namespace RVX
