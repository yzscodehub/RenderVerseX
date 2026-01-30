/**
 * @file AtmosphericScattering.cpp
 * @brief Atmospheric scattering implementation
 */

#include "Render/Sky/AtmosphericScattering.h"
#include "Render/Graph/RenderGraph.h"
#include "Core/Log.h"

namespace RVX
{

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

    if (needsLutUpdate)
        m_lutsNeedUpdate = true;
}

void AtmosphericScattering::SetSunDirection(const Vec3& direction)
{
    m_config.sunDirection = glm::normalize(direction);
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

    RVX_CORE_DEBUG("AtmosphericScattering: Precomputing LUTs");

    ComputeTransmittanceLUT(ctx);
    ComputeMultiScatteringLUT(ctx);
    ComputeSkyViewLUT(ctx);

    m_lutsNeedUpdate = false;
}

void AtmosphericScattering::ComputeTransmittanceLUT(RHICommandContext& ctx)
{
    (void)ctx;
    // TODO: Dispatch compute shader
    // For each texel:
    // - Map UV to (cosZenith, altitude)
    // - Ray march from position to atmosphere edge
    // - Integrate optical depth
    // - Output exp(-opticalDepth)
}

void AtmosphericScattering::ComputeMultiScatteringLUT(RHICommandContext& ctx)
{
    (void)ctx;
    // TODO: Dispatch compute shader
    // Precompute multiple scattering contribution
    // Uses hemispherical integration
}

void AtmosphericScattering::ComputeSkyViewLUT(RHICommandContext& ctx)
{
    (void)ctx;
    // TODO: Dispatch compute shader
    // For each direction on the hemisphere:
    // - Ray march through atmosphere
    // - Accumulate in-scattered light
    // - Sample transmittance LUT
    // - Add multi-scattering contribution
}

void AtmosphericScattering::RenderSky(RHICommandContext& ctx,
                                       RHITexture* outputTarget,
                                       RHITexture* depthBuffer,
                                       const Mat4& viewMatrix,
                                       const Mat4& projMatrix)
{
    if (!m_enabled)
        return;

    (void)outputTarget;
    (void)depthBuffer;
    (void)viewMatrix;
    (void)projMatrix;

    // Update LUTs if needed
    PrecomputeLUTs(ctx);

    // TODO: Render sky using sky-view LUT
    // 1. Render fullscreen quad at far plane
    // 2. Sample sky-view LUT based on view direction
    // 3. Add sun disk
}

void AtmosphericScattering::AddToGraph(RenderGraph& graph,
                                        RGTextureHandle outputTarget,
                                        RGTextureHandle depthBuffer,
                                        const Mat4& viewMatrix,
                                        const Mat4& projMatrix)
{
    if (!m_enabled)
        return;

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
            // TODO: Render sky
        });
}

void AtmosphericScattering::ApplyAerialPerspective(RenderGraph& graph,
                                                    RGTextureHandle sceneColor,
                                                    RGTextureHandle depth,
                                                    RGTextureHandle output,
                                                    const Mat4& viewMatrix,
                                                    const Mat4& projMatrix)
{
    if (!m_enabled || !m_config.enableAerialPerspective)
        return;

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
            // TODO: Apply aerial perspective
            // - Sample transmittance along view ray
            // - Add in-scattered light
            // - Blend based on distance
        });
}

Vec3 AtmosphericScattering::GetSkyColor(const Vec3& direction) const
{
    (void)direction;
    // TODO: Sample sky-view LUT
    return Vec3(0.5f, 0.7f, 1.0f);  // Placeholder blue
}

Vec3 AtmosphericScattering::GetSunDiskColor() const
{
    // Sun color attenuated by atmosphere
    return m_config.sunColor * m_config.sunIntensity;
}

Vec3 AtmosphericScattering::GetTransmittance(const Vec3& origin, const Vec3& direction, float distance) const
{
    (void)origin;
    (void)direction;
    (void)distance;
    // TODO: Sample transmittance LUT
    return Vec3(1.0f);  // Placeholder (no attenuation)
}

} // namespace RVX
