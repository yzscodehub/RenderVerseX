#include "Particle/Rendering/ParticlePass.h"
#include "Core/Log.h"
#include "Particle/GPU/IParticleSimulator.h"
#include "Particle/ParticleSystem.h"
#include "Particle/ParticleSystemInstance.h"
#include "Particle/Rendering/ParticleRenderer.h"
#include "Render/Graph/RenderGraph.h"
#include "Render/Graph/ResourceViewCache.h"
#include "Render/Renderer/ViewData.h"
#include "RHI/RHIRenderPass.h"

#include <unordered_map>

namespace RVX::Particle
{
namespace
{
    ParticleRendererViewData MakeParticleRendererViewData(const ViewData& view)
    {
        ParticleRendererViewData rendererView;
        rendererView.viewMatrix = view.viewMatrix;
        rendererView.projectionMatrix = view.projectionMatrix;
        rendererView.viewProjectionMatrix = view.viewProjectionMatrix;
        rendererView.inverseViewMatrix = view.inverseViewMatrix;
        rendererView.cameraPosition = view.cameraPosition;
        rendererView.cameraForward = view.cameraForward;
        rendererView.viewportWidth = view.viewportWidth;
        rendererView.viewportHeight = view.viewportHeight;
        rendererView.nearPlane = view.nearPlane;
        rendererView.farPlane = view.farPlane;
        return rendererView;
    }
} // namespace

ParticlePass::ParticlePass() = default;
ParticlePass::~ParticlePass() = default;

bool ParticlePass::IsSupported() const
{
    return m_renderer && m_renderer->IsRenderingSupported();
}

const std::string& ParticlePass::GetUnsupportedReason() const
{
    static const std::string noRendererReason = "Particle renderer is not connected";
    static const std::string emptyReason;

    if (!m_renderer)
        return noRendererReason;

    if (!m_renderer->IsRenderingSupported())
        return m_renderer->GetUnsupportedReason();

    return emptyReason;
}

bool ParticlePass::IsEnabled() const
{
    return IsRequestedEnabled() && IsSupported() && !m_batches.empty();
}

void ParticlePass::SetParticleSystems(const std::vector<ParticleSystemInstance*>& instances)
{
    m_instances = instances;
    SortIntoBatches();
}

void ParticlePass::SortIntoBatches()
{
    m_batches.clear();

    // Group by blend mode
    std::unordered_map<ParticleBlendMode, std::vector<ParticleSystemInstance*>> groups;

    for (auto* instance : m_instances)
    {
        if (!instance || !instance->HasSystem() || instance->GetAliveCount() == 0)
            continue;

        auto blendMode = instance->GetSystem()->blendMode;
        groups[blendMode].push_back(instance);
    }

    // Create batches (ordered by blend mode for consistent rendering)
    static const ParticleBlendMode blendOrder[] = {
        ParticleBlendMode::Additive,
        ParticleBlendMode::Premultiplied,
        ParticleBlendMode::AlphaBlend,
        ParticleBlendMode::Multiply
    };

    for (auto mode : blendOrder)
    {
        auto it = groups.find(mode);
        if (it != groups.end() && !it->second.empty())
        {
            ParticleDrawBatch batch;
            batch.blendMode = mode;
            batch.instances = std::move(it->second);
            m_batches.push_back(std::move(batch));
        }
    }
}

void ParticlePass::Setup(RenderGraphBuilder& builder, const ViewData& view)
{
    m_colorTarget = view.colorTarget;
    m_depthTarget = view.depthTarget;
    m_depthMode = ParticleDepthMode::None;

    if (!IsEnabled())
    {
        if (!IsSupported())
        {
            RVX_CORE_WARN("ParticlePass: setup skipped: {}", GetUnsupportedReason());
        }
        return;
    }

    // Read/write color target
    if (m_colorTarget.IsValid())
    {
        m_colorTarget = builder.Write(m_colorTarget, RHIResourceState::RenderTarget);
    }

    if (m_depthTarget.IsValid())
    {
        RHITexture* depthTexture = view.renderGraph ? view.renderGraph->GetTexture(m_depthTarget) : nullptr;
        const bool depthSrvAvailable =
            depthTexture &&
            HasFlag(depthTexture->GetUsage(), RHITextureUsage::ShaderResource) &&
            view.viewCache &&
            view.viewCache->GetDefaultSRV(depthTexture);

        if (m_softParticlesEnabled && depthSrvAvailable)
        {
            m_depthTarget = builder.Read(m_depthTarget, RHIShaderStage::Pixel);
            m_depthMode = ParticleDepthMode::ShaderDepth;
        }
        else
        {
            builder.SetDepthStencil(m_depthTarget, false, false);
            m_depthMode = ParticleDepthMode::FixedFunction;
        }
    }
}

void ParticlePass::Execute(RHICommandContext& ctx, const ViewData& view)
{
    if (!IsEnabled())
    {
        if (!IsSupported())
        {
            RVX_CORE_WARN("ParticlePass: execute skipped: {}", GetUnsupportedReason());
        }
        return;
    }

    if (m_sortingEnabled)
    {
        RecordSortingFallback();
    }

    RHITextureView* colorTargetView = nullptr;
    RHITextureView* depthTargetView = nullptr;
    if (view.renderGraph && view.viewCache && m_colorTarget.IsValid())
    {
        if (RHITexture* colorTarget = view.renderGraph->GetTexture(m_colorTarget))
        {
            colorTargetView = view.viewCache->GetDefaultRTV(colorTarget);
        }
    }

    if (!colorTargetView)
    {
        RVX_CORE_WARN("ParticlePass: execute skipped: color target view is unavailable");
        return;
    }

    RHITextureView* sceneDepthView = nullptr;
    if (view.renderGraph && view.viewCache && m_depthTarget.IsValid())
    {
        RHITexture* depthTexture = view.renderGraph->GetTexture(m_depthTarget);
        if (depthTexture)
        {
            if (m_depthMode == ParticleDepthMode::ShaderDepth)
            {
                sceneDepthView = view.viewCache->GetDefaultSRV(depthTexture);
                if (!sceneDepthView)
                {
                    RVX_CORE_WARN("ParticlePass: scene depth SRV unavailable during execute; shader depth disabled");
                    m_depthMode = ParticleDepthMode::None;
                }
            }
            else if (m_depthMode == ParticleDepthMode::FixedFunction)
            {
                depthTargetView = view.viewCache->GetDefaultDSV(depthTexture);
                if (!depthTargetView)
                {
                    RVX_CORE_WARN("ParticlePass: depth target view unavailable during execute; fixed depth disabled");
                    m_depthMode = ParticleDepthMode::None;
                }
            }
        }
    }

    RHIRenderPassDesc rpDesc;
    rpDesc.AddColorAttachment(colorTargetView, RHILoadOp::Load, RHIStoreOp::Store,
                              {0.0f, 0.0f, 0.0f, 0.0f});
    if (m_depthMode == ParticleDepthMode::FixedFunction && depthTargetView)
    {
        rpDesc.SetDepthStencil(depthTargetView, RHILoadOp::Load, RHIStoreOp::Store, 1.0f, 0);
        rpDesc.depthStencilAttachment.readOnly = true;
    }

    ctx.BeginRenderPass(rpDesc);
    ctx.SetViewport(view.GetRHIViewport());
    ctx.SetScissor(view.GetRHIScissor());

    const ParticleRendererViewData rendererView = MakeParticleRendererViewData(view);

    // Render each batch
    for (const auto& batch : m_batches)
    {
        // Set blend state based on batch blend mode
        // (Pipeline handles this internally)

        for (auto* instance : batch.instances)
        {
            if (!instance || instance->GetAliveCount() == 0)
                continue;

            auto* simulator = instance->GetSimulator();
            if (!simulator)
                continue;

            // Prepare GPU data
            simulator->PrepareRender(ctx);

            // Use indirect draw if GPU simulator
            if (simulator->IsGPUBased())
            {
                m_renderer->DrawParticlesIndirect(ctx,
                                                  instance,
                                                  rendererView,
                                                  sceneDepthView,
                                                  m_depthMode,
                                                  m_softParticlesEnabled);
            }
            else
            {
                m_renderer->DrawParticles(ctx,
                                          instance,
                                          rendererView,
                                          sceneDepthView,
                                          m_depthMode,
                                          m_softParticlesEnabled);
            }
        }
    }

    ctx.EndRenderPass();
}

void ParticlePass::RecordSortingFallback()
{
    if (!m_sortingFallbackReason.empty())
        return;

    m_sortingFallbackReason =
        "Particle sorting is deferred to Render-owned feature passes; legacy ParticlePass preserves batch order";
    RVX_CORE_WARN("ParticlePass: {}", m_sortingFallbackReason);
}

} // namespace RVX::Particle
