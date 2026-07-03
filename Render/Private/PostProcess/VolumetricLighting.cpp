/**
 * @file VolumetricLighting.cpp
 * @brief Volumetric lighting implementation
 */

#include "Render/PostProcess/VolumetricLighting.h"
#include "Render/Lighting/ClusteredLighting.h"
#include "Core/Log.h"

namespace RVX
{

namespace
{
    constexpr const char* RVX_VOLUMETRIC_UNSUPPORTED_REASON =
        "Volumetric lighting ray march/composite pipeline is not implemented";
} // namespace

const char* GetVolumetricLightingImplementationTierName(VolumetricLightingImplementationTier tier)
{
    switch (tier)
    {
        case VolumetricLightingImplementationTier::Unsupported: return "Unsupported";
        case VolumetricLightingImplementationTier::RayMarchComposite: return "RayMarchComposite";
    }
    return "Unknown";
}

VolumetricLightingPass::VolumetricLightingPass()
{
    m_enabled = false;
    MarkUnsupported(RVX_VOLUMETRIC_UNSUPPORTED_REASON);
    RecordUnsupportedDiagnostics(false, false);
}

void VolumetricLightingPass::Configure(const PostProcessSettings& settings)
{
    m_enabled = settings.enableVolumetricLighting;
    m_config.intensity = settings.volumetricIntensity;

    MarkUnsupported(RVX_VOLUMETRIC_UNSUPPORTED_REASON);
    RecordUnsupportedDiagnostics(false, false);
}

void VolumetricLightingPass::SetHeightFog(bool enable, float height, float falloff)
{
    m_config.useHeightFog = enable;
    m_config.fogHeight = height;
    m_config.fogFalloff = falloff;
}

void VolumetricLightingPass::SetDirectionalLight(const Vec3& direction, const Vec3& color, float intensity)
{
    m_lightDirection = glm::normalize(direction);
    m_lightColor = color;
    m_lightIntensity = intensity;
}

void VolumetricLightingPass::SetCameraMatrices(const Mat4& view, const Mat4& proj,
                                                 const Mat4& prevView, const Mat4& prevProj)
{
    m_viewMatrix = view;
    m_projMatrix = proj;
    m_prevViewMatrix = prevView;
    m_prevProjMatrix = prevProj;
}

void VolumetricLightingPass::AddToGraph(RenderGraph& graph, RGTextureHandle input, RGTextureHandle output)
{
    (void)graph;
    (void)input;
    (void)output;

    RecordUnsupportedDiagnostics(false, false);

    if (!IsEnabled())
    {
        if (IsRequestedEnabled() && !IsSupported())
        {
            RVX_CORE_WARN("VolumetricLighting: unsupported fallback pass skipped: {}", GetUnsupportedReason());
        }
        return;
    }

    RVX_CORE_WARN("VolumetricLighting: graph pass requested but {}", GetUnsupportedReason());
}

void VolumetricLightingPass::AddToGraph(RenderGraph& graph,
                                         RGTextureHandle input,
                                         RGTextureHandle depth,
                                         RGTextureHandle shadowMap,
                                         RGTextureHandle output)
{
    (void)graph;
    (void)input;
    (void)output;

    RecordUnsupportedDiagnostics(depth.IsValid(), shadowMap.IsValid());

    if (!IsEnabled())
    {
        if (IsRequestedEnabled() && !IsSupported())
        {
            RVX_CORE_WARN("VolumetricLighting: unsupported pass skipped: {}", GetUnsupportedReason());
        }
        return;
    }

    RVX_CORE_WARN("VolumetricLighting: graph pass requested but {}", GetUnsupportedReason());
}

void VolumetricLightingPass::GetQualityPlan(uint32& outSampleCount, bool& outHalfResolution) const
{
    outSampleCount = 32;
    outHalfResolution = m_config.halfResolution;
    switch (m_config.quality)
    {
        case VolumetricQuality::Low:
            outSampleCount = 16;
            outHalfResolution = false;
            return;
        case VolumetricQuality::Medium:
            outSampleCount = 32;
            outHalfResolution = true;
            return;
        case VolumetricQuality::High:
            outSampleCount = 64;
            outHalfResolution = true;
            return;
        case VolumetricQuality::Ultra:
            outSampleCount = 128;
            outHalfResolution = false;
            return;
    }
}

void VolumetricLightingPass::RecordUnsupportedDiagnostics(bool depthAvailable, bool shadowMapAvailable)
{
    uint32 sampleCount = 0;
    bool halfResolution = false;
    GetQualityPlan(sampleCount, halfResolution);

    m_lastDiagnostics.requested = m_enabled;
    m_lastDiagnostics.supported = false;
    m_lastDiagnostics.scheduled = false;
    m_lastDiagnostics.depthAvailable = depthAvailable;
    m_lastDiagnostics.shadowMapAvailable = shadowMapAvailable;
    m_lastDiagnostics.temporalRequested = m_config.temporalReprojection;
    m_lastDiagnostics.halfResolution = halfResolution;
    m_lastDiagnostics.sampleCount = sampleCount;
    m_lastDiagnostics.implementationTier = VolumetricLightingImplementationTier::Unsupported;
    m_lastDiagnostics.reason = GetUnsupportedReason();
}

} // namespace RVX
