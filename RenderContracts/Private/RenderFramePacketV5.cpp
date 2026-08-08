#include "RenderContracts/RenderFramePacketV5.h"

#include "RenderContracts/RenderFrameValidation.h"

#include <cmath>
#include <utility>

namespace RVX
{

std::unique_ptr<const RenderFramePacketV5> RenderFramePacketV5::Create(
    RenderFrameHeaderV5 header,
    RenderViewSnapshot view,
    RenderFrameSettings settings,
    RenderFrameCaptureRequest captureRequest,
    RenderExtractionDiagnostics diagnostics)
{
    const bool validHeader =
        header.schemaId == RVX_RENDER_FRAME_PACKET_V5_SCHEMA_ID &&
        header.schemaVersion == RVX_RENDER_FRAME_PACKET_V5_SCHEMA_VERSION &&
        header.sequence != 0 && header.requiredSceneRevision != 0;
    const bool validView = view.viewportWidth != 0 && view.viewportHeight != 0 &&
                           std::isfinite(view.nearPlane) &&
                           std::isfinite(view.farPlane) &&
                           view.nearPlane > 0.0f && view.farPlane > view.nearPlane;
    const bool complete = diagnostics.complete &&
                          diagnostics.code == RenderExtractionCode::Complete;
    if (!validHeader || !validView || !complete ||
        !IsValidRenderFrameCaptureRequest(captureRequest))
    {
        return {};
    }

    return std::unique_ptr<const RenderFramePacketV5>(
        new RenderFramePacketV5(std::move(header),
                                std::move(view),
                                std::move(settings),
                                std::move(captureRequest),
                                std::move(diagnostics)));
}

RenderFramePacketV5::RenderFramePacketV5(
    RenderFrameHeaderV5 header,
    RenderViewSnapshot view,
    RenderFrameSettings settings,
    RenderFrameCaptureRequest captureRequest,
    RenderExtractionDiagnostics diagnostics)
    : m_header(std::move(header))
    , m_view(std::move(view))
    , m_settings(std::move(settings))
    , m_captureRequest(std::move(captureRequest))
    , m_diagnostics(std::move(diagnostics))
{
}

} // namespace RVX
