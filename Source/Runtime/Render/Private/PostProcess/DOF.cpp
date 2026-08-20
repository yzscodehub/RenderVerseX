/**
 * @file DOF.cpp
 * @brief Depth of Field implementation
 */

#include "Render/PostProcess/DOF.h"
#include "Core/Log.h"
#include <cmath>

namespace RVX
{

namespace
{
    constexpr const char* RVX_DOF_UNSUPPORTED_REASON =
        "Depth of field gather/composite pipeline is not implemented";
} // namespace

const char* GetDOFImplementationTierName(DOFImplementationTier tier)
{
    switch (tier)
    {
        case DOFImplementationTier::Unsupported: return "Unsupported";
        case DOFImplementationTier::GatherComposite: return "GatherComposite";
    }
    return "Unknown";
}

DOFPass::DOFPass()
{
    m_enabled = true;
    MarkUnsupported(RVX_DOF_UNSUPPORTED_REASON);
    m_currentFocusDistance = m_config.focusDistance;
    RecordUnsupportedDiagnostics(false);
}

void DOFPass::Configure(const PostProcessSettings& settings)
{
    m_enabled = settings.enableDOF;
    if (settings.dofFocusDistance > 0.0f)
        m_config.focusDistance = settings.dofFocusDistance;
    if (settings.dofAperture > 0.0f)
        m_config.aperture = settings.dofAperture;
    if (settings.dofFocalLength > 0.0f)
        m_config.focalLength = settings.dofFocalLength;

    MarkUnsupported(RVX_DOF_UNSUPPORTED_REASON);
    RecordUnsupportedDiagnostics(false);
}

float DOFPass::CalculateCoC(float depth) const
{
    // Physically-based Circle of Confusion calculation
    // CoC = |S2 - S1| / S2 * (f^2 / (N * (S1 - f)))
    // Where:
    //   S1 = focus distance
    //   S2 = object distance (depth)
    //   f  = focal length
    //   N  = f-stop (aperture)
    
    float focusDistance = m_config.focusDistance;
    float focalLength = m_config.focalLength / 1000.0f;  // Convert mm to meters
    float aperture = m_config.aperture;
    float sensorSize = m_config.sensorSize / 1000.0f;    // Convert mm to meters
    
    if (depth <= 0.0f || focusDistance <= 0.0f)
        return 0.0f;
    
    // Calculate the CoC in meters at the sensor
    float numerator = focalLength * focalLength * (depth - focusDistance);
    float denominator = aperture * depth * (focusDistance - focalLength);
    
    if (std::abs(denominator) < 1e-6f)
        return 0.0f;
    
    float cocSensor = numerator / denominator;
    
    // Convert to pixels (assuming sensor covers the viewport)
    // This would need viewport size to be accurate
    float cocPixels = std::abs(cocSensor) / sensorSize * 1920.0f;  // Assume 1080p reference
    
    // Clamp to max blur radius
    cocPixels = std::min(cocPixels, m_config.maxBlurRadius);
    
    // Negative CoC for foreground (closer than focus)
    if (depth < focusDistance)
        cocPixels = -cocPixels;
    
    return cocPixels;
}

void DOFPass::SetAutoFocus(float screenX, float screenY, float depth)
{
    (void)screenX;
    (void)screenY;
    
    // Smooth transition to new focus distance
    if (depth > 0.0f)
    {
        m_config.focusDistance = depth;
    }
}

void DOFPass::AddToGraph(RenderGraph& graph, RGTextureHandle input, RGTextureHandle output)
{
    (void)graph;
    (void)input;
    (void)output;

    RecordUnsupportedDiagnostics(false);

    if (!IsEnabled())
    {
        if (IsRequestedEnabled() && !IsSupported())
        {
            RVX_CORE_WARN("DOF: unsupported fallback pass skipped: {}", GetUnsupportedReason());
        }
        return;
    }

    RVX_CORE_WARN("DOF: graph pass requested but {}", GetUnsupportedReason());
}

void DOFPass::AddToGraph(RenderGraph& graph, RGTextureHandle input, 
                         RGTextureHandle depth, RGTextureHandle output)
{
    (void)graph;
    (void)input;
    (void)output;

    RecordUnsupportedDiagnostics(depth.IsValid());

    if (!IsEnabled())
    {
        if (IsRequestedEnabled() && !IsSupported())
        {
            RVX_CORE_WARN("DOF: unsupported pass skipped: {}", GetUnsupportedReason());
        }
        return;
    }

    RVX_CORE_WARN("DOF: graph pass requested but {}", GetUnsupportedReason());
}

uint32 DOFPass::GetSampleCount() const
{
    switch (m_config.quality)
    {
        case DOFQuality::Low: return 4;
        case DOFQuality::Medium: return 8;
        case DOFQuality::High: return 16;
        case DOFQuality::Ultra: return 32;
    }
    return 8;
}

void DOFPass::RecordUnsupportedDiagnostics(bool depthAvailable)
{
    m_lastDiagnostics.requested = m_enabled;
    m_lastDiagnostics.supported = false;
    m_lastDiagnostics.scheduled = false;
    m_lastDiagnostics.depthAvailable = depthAvailable;
    m_lastDiagnostics.sampleCount = GetSampleCount();
    m_lastDiagnostics.implementationTier = DOFImplementationTier::Unsupported;
    m_lastDiagnostics.reason = GetUnsupportedReason();
}

} // namespace RVX
