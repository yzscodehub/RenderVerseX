/**
 * @file AtmosphericScattering.cpp
 * @brief Atmospheric scattering implementation
 */

#include "Render/Sky/AtmosphericScattering.h"
#include "Render/Graph/RenderGraph.h"
#include "Core/Log.h"

#include <algorithm>
#include <cmath>

namespace RVX
{

namespace
{
    constexpr const char* RVX_ATMOSPHERE_GPU_UNSUPPORTED_REASON =
        "AtmosphericScattering GPU LUT/render pipelines are not implemented; CPU analytic baseline is available";

    float Saturate(float value)
    {
        return std::clamp(value, 0.0f, 1.0f);
    }

    Vec3 ClampVec3(const Vec3& value, float minValue, float maxValue)
    {
        return Vec3(std::clamp(value.x, minValue, maxValue),
                    std::clamp(value.y, minValue, maxValue),
                    std::clamp(value.z, minValue, maxValue));
    }

    Vec3 SafeNormalize(const Vec3& value, const Vec3& fallback)
    {
        const float len = length(value);
        return len > 0.0001f ? value / len : fallback;
    }

    float SafePositive(float value, float fallback)
    {
        return std::isfinite(value) && value > 0.0f ? value : fallback;
    }

    Vec3 ExpVec3(const Vec3& value)
    {
        return Vec3(std::exp(value.x), std::exp(value.y), std::exp(value.z));
    }
} // namespace

const char* GetAtmosphericScatteringImplementationTierName(AtmosphericScatteringImplementationTier tier)
{
    switch (tier)
    {
        case AtmosphericScatteringImplementationTier::Unsupported: return "Unsupported";
        case AtmosphericScatteringImplementationTier::CpuAnalyticBaseline: return "CpuAnalyticBaseline";
        case AtmosphericScatteringImplementationTier::GpuLut: return "GpuLut";
    }
    return "Unknown";
}

AtmosphericScattering::~AtmosphericScattering()
{
    Shutdown();
}

void AtmosphericScattering::Initialize(IRHIDevice* device, const AtmosphericScatteringConfig& config)
{
    if (m_device)
    {
        RVX_CORE_WARN("AtmosphericScattering: Already initialized");
        return;
    }

    if (!device)
    {
        RVX_CORE_ERROR("AtmosphericScattering: Cannot initialize without an RHI device");
        m_unsupportedReason = "No RHI device";
        return;
    }

    m_device = device;
    m_config = config;

    CreateLUTs();

    // Create constant buffer
    RHIBufferDesc bufferDesc{};
    bufferDesc.size = 512;  // Enough for all parameters
    bufferDesc.usage = RHIBufferUsage::Constant;
    bufferDesc.memoryType = RHIMemoryType::Upload;
    m_constantBuffer = device->CreateBuffer(bufferDesc);

    m_lutsNeedUpdate = true;
    m_supported = false;
    m_unsupportedReason = RVX_ATMOSPHERE_GPU_UNSUPPORTED_REASON;

    RVX_CORE_DEBUG("AtmosphericScattering: Initialized");
}

void AtmosphericScattering::Shutdown()
{
    if (!m_device)
        return;

    m_transmittanceLut.Reset();
    m_multiScatteringLut.Reset();
    m_skyViewLut.Reset();
    m_aerialPerspectiveLut.Reset();
    m_transmittancePipeline.Reset();
    m_multiScatteringPipeline.Reset();
    m_skyViewPipeline.Reset();
    m_skyRenderPipeline.Reset();
    m_aerialPerspectivePipeline.Reset();
    m_constantBuffer.Reset();
    m_device = nullptr;
    m_supported = false;
    m_unsupportedReason = RVX_ATMOSPHERE_GPU_UNSUPPORTED_REASON;

    RVX_CORE_DEBUG("AtmosphericScattering: Shutdown");
}

void AtmosphericScattering::CreateLUTs()
{
    // Transmittance LUT: 2D texture
    RHITextureDesc transmittanceDesc{};
    transmittanceDesc.width = m_config.transmittanceLutSize;
    transmittanceDesc.height = m_config.transmittanceLutSize / 4;
    transmittanceDesc.format = RHIFormat::RGBA16_FLOAT;
    transmittanceDesc.usage = RHITextureUsage::ShaderResource | RHITextureUsage::UnorderedAccess;
    m_transmittanceLut = m_device->CreateTexture(transmittanceDesc);

    // Multi-scattering LUT: 2D texture
    RHITextureDesc multiScatteringDesc{};
    multiScatteringDesc.width = m_config.scatteringLutSize;
    multiScatteringDesc.height = m_config.scatteringLutSize;
    multiScatteringDesc.format = RHIFormat::RGBA16_FLOAT;
    multiScatteringDesc.usage = RHITextureUsage::ShaderResource | RHITextureUsage::UnorderedAccess;
    m_multiScatteringLut = m_device->CreateTexture(multiScatteringDesc);

    // Sky-view LUT: 2D texture (panoramic)
    RHITextureDesc skyViewDesc{};
    skyViewDesc.width = m_config.skyViewLutSize;
    skyViewDesc.height = m_config.skyViewLutSize / 2;
    skyViewDesc.format = RHIFormat::RGBA16_FLOAT;
    skyViewDesc.usage = RHITextureUsage::ShaderResource | RHITextureUsage::UnorderedAccess;
    m_skyViewLut = m_device->CreateTexture(skyViewDesc);
}

void AtmosphericScattering::SetConfig(const AtmosphericScatteringConfig& config)
{
    bool needsLutUpdate = 
        m_config.rayleighScattering != config.rayleighScattering ||
        m_config.mieScattering != config.mieScattering ||
        m_config.rayleighScaleHeight != config.rayleighScaleHeight ||
        m_config.mieScaleHeight != config.mieScaleHeight ||
        m_config.planetRadius != config.planetRadius ||
        m_config.atmosphereHeight != config.atmosphereHeight;

    m_config = config;
    m_config.sunDirection = SafeNormalize(m_config.sunDirection, Vec3(0.0f, 1.0f, 0.0f));

    if (needsLutUpdate)
        m_lutsNeedUpdate = true;
}

void AtmosphericScattering::SetSunDirection(const Vec3& direction)
{
    m_config.sunDirection = SafeNormalize(direction, Vec3(0.0f, 1.0f, 0.0f));
    // Sky-view LUT needs update when sun moves
    m_lutsNeedUpdate = true;
}

void AtmosphericScattering::SetSunColor(const Vec3& color, float intensity)
{
    m_config.sunColor = color;
    m_config.sunIntensity = intensity;
}

void AtmosphericScattering::PrecomputeLUTs(RHICommandContext& ctx)
{
    if (!m_lutsNeedUpdate)
        return;

    if (!IsEnabled())
    {
        if (IsRequestedEnabled() && !IsSupported())
        {
            RVX_CORE_WARN("AtmosphericScattering: unsupported LUT precompute skipped: {}", GetUnsupportedReason());
        }
        return;
    }

    RVX_CORE_DEBUG("AtmosphericScattering: Precomputing LUTs");

    ComputeTransmittanceLUT(ctx);
    ComputeMultiScatteringLUT(ctx);
    ComputeSkyViewLUT(ctx);

    m_lutsNeedUpdate = false;
}

void AtmosphericScattering::ComputeTransmittanceLUT(RHICommandContext& ctx)
{
    (void)ctx;
    RVX_CORE_WARN("AtmosphericScattering: GPU transmittance LUT path is unsupported; using CPU analytic baseline diagnostics");
}

void AtmosphericScattering::ComputeMultiScatteringLUT(RHICommandContext& ctx)
{
    (void)ctx;
    RVX_CORE_WARN("AtmosphericScattering: GPU multi-scattering LUT path is unsupported; using CPU analytic baseline diagnostics");
}

void AtmosphericScattering::ComputeSkyViewLUT(RHICommandContext& ctx)
{
    (void)ctx;
    RVX_CORE_WARN("AtmosphericScattering: GPU sky-view LUT path is unsupported; using CPU analytic baseline diagnostics");
}

void AtmosphericScattering::RenderSky(RHICommandContext& ctx,
                                       RHITexture* outputTarget,
                                       RHITexture* depthBuffer,
                                       const Mat4& viewMatrix,
                                       const Mat4& projMatrix)
{
    if (!IsEnabled())
    {
        if (IsRequestedEnabled() && !IsSupported())
        {
            RVX_CORE_WARN("AtmosphericScattering: unsupported sky render skipped: {}", GetUnsupportedReason());
        }
        return;
    }

    (void)outputTarget;
    (void)depthBuffer;
    (void)viewMatrix;
    (void)projMatrix;

    // Update LUTs if needed
    PrecomputeLUTs(ctx);

    RVX_CORE_WARN("AtmosphericScattering: GPU sky render path is unsupported; procedural SkyboxPass remains the runtime sky baseline");
}

void AtmosphericScattering::AddToGraph(RenderGraph& graph,
                                        RGTextureHandle outputTarget,
                                        RGTextureHandle depthBuffer,
                                        const Mat4& viewMatrix,
                                        const Mat4& projMatrix)
{
    if (!IsEnabled())
    {
        if (IsRequestedEnabled() && !IsSupported())
        {
            RVX_CORE_WARN("AtmosphericScattering: unsupported graph pass skipped: {}", GetUnsupportedReason());
        }
        return;
    }

    struct SkyRenderData
    {
        RGTextureHandle output;
        RGTextureHandle depth;
        Mat4 invViewProj;
        Vec3 sunDirection;
        Vec3 sunColor;
        float sunIntensity;
        Vec3 cameraPosition;
    };

    graph.AddPass<SkyRenderData>(
        "AtmosphericScattering",
        RenderGraphPassType::Graphics,
        [this, outputTarget, depthBuffer, viewMatrix, projMatrix]
        (RenderGraphBuilder& builder, SkyRenderData& data)
        {
            data.output = builder.Write(outputTarget, RHIResourceState::RenderTarget);
            data.depth = builder.Read(depthBuffer);
            
            data.invViewProj = glm::inverse(projMatrix * viewMatrix);
            data.sunDirection = m_config.sunDirection;
            data.sunColor = m_config.sunColor;
            data.sunIntensity = m_config.sunIntensity;
            data.cameraPosition = Vec3(glm::inverse(viewMatrix)[3]);
        },
        [this](const SkyRenderData& data, RHICommandContext& ctx)
        {
            (void)data;
            (void)ctx;
            RVX_CORE_WARN("AtmosphericScattering: graph sky render path is unsupported; procedural SkyboxPass remains the runtime sky baseline");
        });
}

void AtmosphericScattering::ApplyAerialPerspective(RenderGraph& graph,
                                                    RGTextureHandle sceneColor,
                                                    RGTextureHandle depth,
                                                    RGTextureHandle output,
                                                    const Mat4& viewMatrix,
                                                    const Mat4& projMatrix)
{
    if (!IsEnabled() || !m_config.enableAerialPerspective)
    {
        if (IsRequestedEnabled() && !IsSupported())
        {
            RVX_CORE_WARN("AtmosphericScattering: unsupported aerial perspective skipped: {}", GetUnsupportedReason());
        }
        return;
    }

    struct AerialPerspectiveData
    {
        RGTextureHandle sceneColor;
        RGTextureHandle depth;
        RGTextureHandle output;
        Mat4 invViewProj;
        Vec3 cameraPosition;
        float maxDistance;
    };

    graph.AddPass<AerialPerspectiveData>(
        "AerialPerspective",
        RenderGraphPassType::Compute,
        [this, sceneColor, depth, output, viewMatrix, projMatrix]
        (RenderGraphBuilder& builder, AerialPerspectiveData& data)
        {
            data.sceneColor = builder.Read(sceneColor);
            data.depth = builder.Read(depth);
            data.output = builder.Write(output, RHIResourceState::UnorderedAccess);
            
            data.invViewProj = glm::inverse(projMatrix * viewMatrix);
            data.cameraPosition = Vec3(glm::inverse(viewMatrix)[3]);
            data.maxDistance = m_config.aerialPerspectiveDistance;
        },
        [](const AerialPerspectiveData& data, RHICommandContext& ctx)
        {
            (void)data;
            (void)ctx;
            RVX_CORE_WARN("AtmosphericScattering: aerial perspective graph path is unsupported; CPU transmittance baseline is diagnostic-only");
        });
}

AtmosphericScatteringDiagnostics AtmosphericScattering::GetDiagnostics() const
{
    AtmosphericScatteringDiagnostics diagnostics;
    diagnostics.requested = m_enabled;
    diagnostics.initialized = IsInitialized();
    diagnostics.gpuLutSupported = m_supported;
    diagnostics.cpuAnalyticBaselineAvailable = true;
    diagnostics.implementationTier = m_supported
        ? AtmosphericScatteringImplementationTier::GpuLut
        : AtmosphericScatteringImplementationTier::CpuAnalyticBaseline;
    diagnostics.unsupportedReason = m_supported ? std::string{} : m_unsupportedReason;
    return diagnostics;
}

Vec3 AtmosphericScattering::GetSkyColor(const Vec3& direction) const
{
    const Vec3 viewDirection = SafeNormalize(direction, Vec3(0.0f, 1.0f, 0.0f));
    const Vec3 sunDirection = SafeNormalize(m_config.sunDirection, Vec3(0.0f, 1.0f, 0.0f));

    const float upAmount = Saturate(viewDirection.y * 0.5f + 0.5f);
    const float horizonAmount = std::pow(1.0f - std::abs(std::clamp(viewDirection.y, -1.0f, 1.0f)), 0.65f);
    const float sunHorizonWarmth = std::pow(Saturate(1.0f - std::abs(sunDirection.y)), 1.35f);
    const float sunAlignment = Saturate(dot(viewDirection, sunDirection));
    const float sunHalo = std::pow(sunAlignment, 64.0f) * 0.55f +
                          std::pow(sunAlignment, 8.0f) * 0.12f;

    const Vec3 zenithColor(0.12f, 0.32f, 0.78f);
    const Vec3 noonHorizonColor(0.62f, 0.80f, 1.0f);
    const Vec3 sunsetHorizonColor(1.0f, 0.48f, 0.18f);
    const Vec3 horizonColor = glm::mix(noonHorizonColor, sunsetHorizonColor, sunHorizonWarmth * 0.65f);
    const Vec3 groundColor(0.018f, 0.022f, 0.032f);

    Vec3 skyColor = viewDirection.y >= 0.0f
        ? glm::mix(horizonColor, zenithColor, std::pow(upAmount, 0.55f))
        : glm::mix(groundColor, horizonColor, Saturate(upAmount * 0.6f));

    skyColor = glm::mix(skyColor, horizonColor, horizonAmount * 0.18f);
    skyColor += m_config.sunColor * (sunHalo * Saturate(m_config.sunIntensity / 20.0f));

    return ClampVec3(skyColor, 0.0f, 8.0f);
}

Vec3 AtmosphericScattering::GetSunDiskColor() const
{
    // Sun color attenuated by atmosphere
    return m_config.sunColor * m_config.sunIntensity;
}

Vec3 AtmosphericScattering::GetTransmittance(const Vec3& origin, const Vec3& direction, float distance) const
{
    const float safeDistance = std::max(distance, 0.0f);
    if (safeDistance <= 0.0f)
    {
        return Vec3(1.0f);
    }

    const Vec3 rayDirection = SafeNormalize(direction, Vec3(0.0f, 1.0f, 0.0f));
    const float planetRadius = SafePositive(m_config.planetRadius, 6371000.0f);
    const float startAltitude = std::max(length(origin) - planetRadius, 0.0f);
    const float endAltitude = std::max(length(origin + rayDirection * safeDistance) - planetRadius, 0.0f);
    const float averageAltitude = (startAltitude + endAltitude) * 0.5f;

    const float rayleighScaleHeight = SafePositive(m_config.rayleighScaleHeight, 8500.0f);
    const float mieScaleHeight = SafePositive(m_config.mieScaleHeight, 1200.0f);
    const float ozoneWidth = SafePositive(m_config.ozoneWidth, 15000.0f);

    const float rayleighDensity = std::exp(-averageAltitude / rayleighScaleHeight);
    const float mieDensity = std::exp(-averageAltitude / mieScaleHeight);
    const float ozoneDensity = Saturate(1.0f - std::abs(averageAltitude - m_config.ozoneHeight) / ozoneWidth);

    const Vec3 extinction =
        m_config.rayleighScattering * rayleighDensity +
        (m_config.mieScattering + m_config.mieAbsorption) * mieDensity +
        m_config.ozoneAbsorption * ozoneDensity;

    return ClampVec3(ExpVec3(-extinction * safeDistance), 0.0f, 1.0f);
}

} // namespace RVX
