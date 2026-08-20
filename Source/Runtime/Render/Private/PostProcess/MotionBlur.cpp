/**
 * @file MotionBlur.cpp
 * @brief Motion blur implementation
 */

#include "Render/PostProcess/MotionBlur.h"
#include "Core/Log.h"

namespace RVX
{

namespace
{
    constexpr const char* RVX_MOTION_BLUR_UNSUPPORTED_REASON =
        "MotionBlur velocity gather pipeline is not implemented";
} // namespace

const char* GetMotionBlurImplementationTierName(MotionBlurImplementationTier tier)
{
    switch (tier)
    {
        case MotionBlurImplementationTier::Unsupported: return "Unsupported";
        case MotionBlurImplementationTier::VelocityGather: return "VelocityGather";
    }
    return "Unknown";
}

MotionBlurPass::MotionBlurPass()
{
    m_enabled = true;
    MarkUnsupported(RVX_MOTION_BLUR_UNSUPPORTED_REASON);
    RecordUnsupportedDiagnostics(false, false);
}

void MotionBlurPass::Configure(const PostProcessSettings& settings)
{
    m_enabled = settings.enableMotionBlur;
    if (settings.motionBlurIntensity > 0.0f)
        m_config.intensity = settings.motionBlurIntensity;
    if (settings.motionBlurMaxVelocity > 0.0f)
        m_config.maxVelocity = settings.motionBlurMaxVelocity;

    MarkUnsupported(RVX_MOTION_BLUR_UNSUPPORTED_REASON);
    RecordUnsupportedDiagnostics(false, false);
}

void MotionBlurPass::SetCameraMatrices(const Mat4& currentViewProj, const Mat4& prevViewProj)
{
    m_currentViewProj = currentViewProj;
    m_prevViewProj = prevViewProj;
    m_hasCameraData = true;
    m_lastDiagnostics.cameraDataAvailable = true;
    m_lastDiagnostics.historyAvailable = true;
}

void MotionBlurPass::AddToGraph(RenderGraph& graph, RGTextureHandle input, RGTextureHandle output)
{
    (void)graph;
    (void)input;
    (void)output;

    RecordUnsupportedDiagnostics(false, false);

    if (!IsEnabled())
    {
        if (IsRequestedEnabled() && !IsSupported())
        {
            RVX_CORE_WARN("MotionBlur: unsupported camera pass skipped: {}", GetUnsupportedReason());
        }
        return;
    }

    RVX_CORE_WARN("MotionBlur: graph pass requested but {}", GetUnsupportedReason());
}

void MotionBlurPass::AddToGraph(RenderGraph& graph, RGTextureHandle input,
                                 RGTextureHandle velocity, RGTextureHandle depth,
                                 RGTextureHandle output)
{
    (void)graph;
    (void)input;
    (void)output;

    RecordUnsupportedDiagnostics(velocity.IsValid(), depth.IsValid());

    if (!IsEnabled())
    {
        if (IsRequestedEnabled() && !IsSupported())
        {
            RVX_CORE_WARN("MotionBlur: unsupported velocity pass skipped: {}", GetUnsupportedReason());
        }
        return;
    }

    RVX_CORE_WARN("MotionBlur: graph pass requested but {}", GetUnsupportedReason());
}

uint32 MotionBlurPass::GetSampleCount() const
{
    switch (m_config.quality)
    {
        case MotionBlurQuality::Low: return 4;
        case MotionBlurQuality::Medium: return 8;
        case MotionBlurQuality::High: return 16;
        case MotionBlurQuality::Ultra: return 32;
    }
    return 8;
}

void MotionBlurPass::RecordUnsupportedDiagnostics(bool velocityAvailable, bool depthAvailable)
{
    m_lastDiagnostics.requested = m_enabled;
    m_lastDiagnostics.supported = false;
    m_lastDiagnostics.scheduled = false;
    m_lastDiagnostics.cameraDataAvailable = m_hasCameraData;
    m_lastDiagnostics.velocityAvailable = velocityAvailable;
    m_lastDiagnostics.depthAvailable = depthAvailable;
    m_lastDiagnostics.historyAvailable = m_hasCameraData;
    m_lastDiagnostics.sampleCount = GetSampleCount();
    m_lastDiagnostics.implementationTier = MotionBlurImplementationTier::Unsupported;
    m_lastDiagnostics.reason = GetUnsupportedReason();
}

} // namespace RVX
