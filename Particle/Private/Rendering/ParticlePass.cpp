#include "Particle/Rendering/ParticlePass.h"
#include "Core/Log.h"
#include "Particle/GPU/IParticleSimulator.h"
#include "Particle/GPU/ParticleSorter.h"
#include "Particle/ParticleSystem.h"
#include "Particle/ParticleSystemInstance.h"
#include "Particle/Rendering/ParticleRenderer.h"
#include "Render/Graph/RenderGraph.h"
#include "Render/Renderer/ViewData.h"

#include <unordered_map>

namespace RVX::Particle
{

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
        m_colorTarget = builder.Read(m_colorTarget);
        m_colorTarget = builder.Write(m_colorTarget);
    }

    // Read depth for soft particles
    if (m_softParticlesEnabled && m_depthTarget.IsValid())
    {
        m_depthTarget = builder.Read(m_depthTarget);
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

    // Sort particles by distance if enabled
    if (m_sortingEnabled && m_sorter)
    {
        SortParticlesByDistance(view);
    }

    // Get depth texture for soft particles
    RHITexture* depthTexture = nullptr;
    if (m_softParticlesEnabled)
    {
        // Would get from render graph resources
        // depthTexture = m_resourcePool->GetTexture(m_depthTarget);
    }

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
                m_renderer->DrawParticlesIndirect(ctx, instance, view, depthTexture);
            }
            else
            {
                m_renderer->DrawParticles(ctx, instance, view, depthTexture);
            }
        }
    }
}

void ParticlePass::SortParticlesByDistance(const ViewData& view)
{
    if (!m_sorter)
        return;

    // Sort each instance's particles
    // This is done per-instance as each has its own particle buffer
    // For now, sorting is handled by the CPU simulator or GPU sorter separately
    (void)view;
}

} // namespace RVX::Particle
