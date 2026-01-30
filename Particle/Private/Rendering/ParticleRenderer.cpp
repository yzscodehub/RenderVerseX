#include "Particle/Rendering/ParticleRenderer.h"
#include "Particle/Rendering/TrailRenderer.h"
#include "Particle/GPU/IParticleSimulator.h"
#include "Particle/ParticleSystemInstance.h"
#include "Particle/ParticleSystem.h"
#include "Render/Renderer/ViewData.h"
#include "Core/Log.h"
#include <cstring>

namespace RVX::Particle
{

ParticleRenderer::~ParticleRenderer()
{
    Shutdown();
}

void ParticleRenderer::Initialize(IRHIDevice* device)
{
    if (m_device)
        Shutdown();

    m_device = device;

    CreateQuadBuffers();

    // Render constants buffer
    RHIBufferDesc constDesc;
    constDesc.size = sizeof(RenderGPUData);
    constDesc.usage = RHIBufferUsage::Constant;
    constDesc.memoryType = RHIMemoryType::Upload;
    constDesc.debugName = "ParticleRenderConstants";
    m_renderConstantsBuffer = m_device->CreateBuffer(constDesc);

    // Initialize trail renderer
    m_trailRenderer = std::make_unique<TrailRenderer>();
    m_trailRenderer->Initialize(device, 100000);  // 100k trail vertices

    RVX_CORE_INFO("ParticleRenderer: Initialized");
}

void ParticleRenderer::Shutdown()
{
    m_pipelineCache.clear();
    m_quadVertexBuffer.Reset();
    m_quadIndexBuffer.Reset();
    m_renderConstantsBuffer.Reset();
    m_trailRenderer.reset();
    m_device = nullptr;
}

void ParticleRenderer::CreateQuadBuffers()
{
    // Billboard quad vertices (-1 to 1, with UVs)
    struct QuadVertex
    {
        Vec3 position;
        Vec2 uv;
    };

    QuadVertex vertices[4] = {
        { Vec3(-1.0f, -1.0f, 0.0f), Vec2(0.0f, 1.0f) },
        { Vec3( 1.0f, -1.0f, 0.0f), Vec2(1.0f, 1.0f) },
        { Vec3( 1.0f,  1.0f, 0.0f), Vec2(1.0f, 0.0f) },
        { Vec3(-1.0f,  1.0f, 0.0f), Vec2(0.0f, 0.0f) }
    };

    uint32 indices[6] = { 0, 1, 2, 0, 2, 3 };

    // Create vertex buffer
    RHIBufferDesc vbDesc;
    vbDesc.size = sizeof(vertices);
    vbDesc.usage = RHIBufferUsage::Vertex;
    vbDesc.memoryType = RHIMemoryType::Upload;
    vbDesc.debugName = "ParticleQuadVB";
    m_quadVertexBuffer = m_device->CreateBuffer(vbDesc);
    m_quadVertexBuffer->Upload(vertices, 4);

    // Create index buffer
    RHIBufferDesc ibDesc;
    ibDesc.size = sizeof(indices);
    ibDesc.usage = RHIBufferUsage::Index;
    ibDesc.memoryType = RHIMemoryType::Upload;
    ibDesc.debugName = "ParticleQuadIB";
    m_quadIndexBuffer = m_device->CreateBuffer(ibDesc);
    m_quadIndexBuffer->Upload(indices, 6);
}

void ParticleRenderer::DrawParticles(RHICommandContext& ctx,
                                     ParticleSystemInstance* instance,
                                     const ViewData& view,
                                     RHITexture* depthTexture)
{
    if (!instance || !instance->HasSystem())
        return;

    uint32 aliveCount = instance->GetAliveCount();
    if (aliveCount == 0)
        return;

    auto* system = instance->GetSystem().get();
    auto* simulator = instance->GetSimulator();
    if (!simulator)
        return;

    // Upload render constants
    UploadRenderConstants(view, system->softParticleConfig);

    // Get appropriate pipeline
    RHIPipeline* pipeline = nullptr;
    switch (system->renderMode)
    {
    case ParticleRenderMode::Billboard:
        pipeline = GetBillboardPipeline(system->blendMode, 
                                        system->softParticleConfig.enabled && depthTexture);
        break;
    case ParticleRenderMode::StretchedBillboard:
        pipeline = GetStretchedBillboardPipeline(system->blendMode,
                                                  system->softParticleConfig.enabled && depthTexture);
        break;
    case ParticleRenderMode::Mesh:
        pipeline = GetMeshPipeline(system->blendMode);
        break;
    case ParticleRenderMode::Trail:
        pipeline = GetTrailPipeline(system->blendMode,
                                    system->softParticleConfig.enabled && depthTexture);
        break;
    default:
        pipeline = GetBillboardPipeline(system->blendMode, false);
        break;
    }

    if (!pipeline)
        return;

    // Bind pipeline
    ctx.SetPipeline(pipeline);

    // Bind resources
    // ctx.BindConstantBuffer(0, m_renderConstantsBuffer.Get());
    // ctx.BindBuffer(0, simulator->GetParticleBuffer());
    // ctx.BindBuffer(1, simulator->GetAliveIndexBuffer());
    // if (depthTexture) ctx.BindTexture(1, depthTexture);

    // Bind quad geometry
    ctx.SetVertexBuffer(0, m_quadVertexBuffer.Get());
    ctx.SetIndexBuffer(m_quadIndexBuffer.Get(), RHIFormat::R16_UINT, 0);

    // Draw instanced
    ctx.DrawIndexed(6, aliveCount, 0, 0, 0);
}

void ParticleRenderer::DrawParticlesIndirect(RHICommandContext& ctx,
                                             ParticleSystemInstance* instance,
                                             const ViewData& view,
                                             RHITexture* depthTexture)
{
    if (!instance || !instance->HasSystem())
        return;

    auto* system = instance->GetSystem().get();
    auto* simulator = instance->GetSimulator();
    if (!simulator)
        return;

    // Upload render constants
    UploadRenderConstants(view, system->softParticleConfig);

    // Get pipeline (same as DrawParticles)
    RHIPipeline* pipeline = GetBillboardPipeline(system->blendMode,
                                                  system->softParticleConfig.enabled && depthTexture);
    if (!pipeline)
        return;

    ctx.SetPipeline(pipeline);
    ctx.SetVertexBuffer(0, m_quadVertexBuffer.Get());
    ctx.SetIndexBuffer(m_quadIndexBuffer.Get(), RHIFormat::R16_UINT, 0);

    // Indirect draw (1 draw call, stride = 0 for single draw)
    ctx.DrawIndexedIndirect(simulator->GetIndirectDrawBuffer(), 0, 1, 0);
}

void ParticleRenderer::UploadRenderConstants(const ViewData& view, 
                                             const SoftParticleConfig& softConfig)
{
    RenderGPUData data;
    data.viewMatrix = view.viewMatrix;
    data.projMatrix = view.projectionMatrix;
    data.viewProjMatrix = view.viewProjectionMatrix;
    data.cameraPosition = Vec4(view.cameraPosition, 1.0f);
    data.cameraRight = Vec4(GetRightFromMatrix(view.inverseViewMatrix), 0.0f);
    data.cameraUp = Vec4(GetUpFromMatrix(view.inverseViewMatrix), 0.0f);
    data.cameraForward = Vec4(view.cameraForward, 0.0f);
    data.screenSize = Vec2(static_cast<float>(view.viewportWidth), 
                           static_cast<float>(view.viewportHeight));
    data.invScreenSize = Vec2(1.0f / data.screenSize.x, 1.0f / data.screenSize.y);
    data.softParticleFadeDistance = softConfig.fadeDistance;
    data.softParticleContrast = softConfig.contrastPower;
    data.softParticleEnabled = softConfig.enabled ? 1 : 0;

    m_renderConstantsBuffer->Upload(&data, 1);
}

uint32 ParticleRenderer::MakePipelineKey(ParticleRenderMode mode, 
                                         ParticleBlendMode blend, 
                                         bool soft)
{
    return (static_cast<uint32>(mode) << 16) | 
           (static_cast<uint32>(blend) << 8) | 
           (soft ? 1 : 0);
}

RHIPipeline* ParticleRenderer::GetBillboardPipeline(ParticleBlendMode blend, bool softParticle)
{
    uint32 key = MakePipelineKey(ParticleRenderMode::Billboard, blend, softParticle);
    auto it = m_pipelineCache.find(key);
    if (it != m_pipelineCache.end())
        return it->second.Get();
    
    // Pipeline creation would happen here
    // For now, return nullptr - pipelines are set externally
    return nullptr;
}

RHIPipeline* ParticleRenderer::GetStretchedBillboardPipeline(ParticleBlendMode blend, bool softParticle)
{
    uint32 key = MakePipelineKey(ParticleRenderMode::StretchedBillboard, blend, softParticle);
    auto it = m_pipelineCache.find(key);
    if (it != m_pipelineCache.end())
        return it->second.Get();
    return nullptr;
}

RHIPipeline* ParticleRenderer::GetMeshPipeline(ParticleBlendMode blend)
{
    uint32 key = MakePipelineKey(ParticleRenderMode::Mesh, blend, false);
    auto it = m_pipelineCache.find(key);
    if (it != m_pipelineCache.end())
        return it->second.Get();
    return nullptr;
}

RHIPipeline* ParticleRenderer::GetTrailPipeline(ParticleBlendMode blend, bool softParticle)
{
    uint32 key = MakePipelineKey(ParticleRenderMode::Trail, blend, softParticle);
    auto it = m_pipelineCache.find(key);
    if (it != m_pipelineCache.end())
        return it->second.Get();
    return nullptr;
}

RHIBuffer* ParticleRenderer::GetQuadVertexBuffer()
{
    return m_quadVertexBuffer.Get();
}

RHIBuffer* ParticleRenderer::GetQuadIndexBuffer()
{
    return m_quadIndexBuffer.Get();
}

} // namespace RVX::Particle
