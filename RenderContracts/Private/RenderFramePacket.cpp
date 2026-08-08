#include "RenderContracts/RenderFramePacket.h"

#include <utility>

namespace RVX
{
std::unique_ptr<const RenderFramePacket> RenderFramePacket::CreateCompatibility(
    RenderFrameHeader header,
    RenderViewSnapshot view,
    std::vector<RenderPrimitiveSnapshot> primitives,
    std::vector<RenderLightSnapshot> lights,
    RenderSkySnapshot sky,
    RenderEnvironmentSnapshot environment,
    RenderFrameSettings settings,
    RenderFrameCaptureRequest captureRequest,
    RenderFeatureSnapshot features,
    RenderExtractionDiagnostics extractionDiagnostics)
{
    return std::unique_ptr<const RenderFramePacket>(new RenderFramePacket(
        std::move(header),
        std::move(view),
        std::move(primitives),
        std::move(lights),
        std::move(sky),
        std::move(environment),
        std::move(settings),
        std::move(captureRequest),
        std::move(features),
        std::move(extractionDiagnostics)));
}

RenderFramePacket::RenderFramePacket(
    RenderFrameHeader header,
    RenderViewSnapshot view,
    std::vector<RenderPrimitiveSnapshot> primitives,
    std::vector<RenderLightSnapshot> lights,
    RenderSkySnapshot sky,
    RenderEnvironmentSnapshot environment,
    RenderFrameSettings settings,
    RenderFrameCaptureRequest captureRequest,
    RenderFeatureSnapshot features,
    RenderExtractionDiagnostics extractionDiagnostics)
    : m_header(std::move(header))
    , m_view(std::move(view))
    , m_primitives(std::move(primitives))
    , m_lights(std::move(lights))
    , m_sky(std::move(sky))
    , m_environment(std::move(environment))
    , m_settings(std::move(settings))
    , m_captureRequest(std::move(captureRequest))
    , m_features(std::move(features))
    , m_extractionDiagnostics(std::move(extractionDiagnostics))
{
}

const RenderFrameHeader& RenderFramePacket::GetHeader() const noexcept
{
    return m_header;
}

const RenderViewSnapshot& RenderFramePacket::GetView() const noexcept
{
    return m_view;
}

const std::vector<RenderPrimitiveSnapshot>&
RenderFramePacket::GetPrimitives() const noexcept
{
    return m_primitives;
}

const std::vector<RenderLightSnapshot>&
RenderFramePacket::GetLights() const noexcept
{
    return m_lights;
}

const RenderSkySnapshot& RenderFramePacket::GetSky() const noexcept
{
    return m_sky;
}

const RenderEnvironmentSnapshot&
RenderFramePacket::GetEnvironment() const noexcept
{
    return m_environment;
}

const RenderFrameSettings& RenderFramePacket::GetSettings() const noexcept
{
    return m_settings;
}

const RenderFrameCaptureRequest&
RenderFramePacket::GetCaptureRequest() const noexcept
{
    return m_captureRequest;
}

const RenderFeatureSnapshot& RenderFramePacket::GetFeatures() const noexcept
{
    return m_features;
}

const RenderExtractionDiagnostics&
RenderFramePacket::GetExtractionDiagnostics() const noexcept
{
    return m_extractionDiagnostics;
}
} // namespace RVX
