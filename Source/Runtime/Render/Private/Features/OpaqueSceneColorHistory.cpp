#include "Features/OpaqueSceneColorHistory.h"

namespace RVX
{
OpaqueSceneColorHistory::OpaqueSceneColorHistory()
{
    PostProcessSettings settings;
    settings.antiAliasingMode = RenderAntiAliasingMode::TAA;
    m_resolve.Configure(settings);
    TAAConfig config;
    config.sharpen = false;
    m_resolve.SetConfig(config);
}

void OpaqueSceneColorHistory::SetResources(PipelineCache* cache, ResourceViewCache* views)
{
    m_resolve.SetResources(cache, views);
}

void OpaqueSceneColorHistory::Prepare(RenderFrameIdentity frame, RenderViewIdentity view,
                                     const PostProcessFrameInputs& inputs)
{
    m_resolve.AbandonPendingHistory();
    m_frame = frame;
    m_view = view;
    m_inputs = inputs;
    m_recorded = false;
}

bool OpaqueSceneColorHistory::Record(RenderFeatureGraphBuilder& builder,
                                    const RenderPassRecordContext& context,
                                    RenderFrameIdentity frame, RenderViewIdentity view,
                                    RGTextureHandle& output, std::string& failure)
{
    output = {};
    if (m_recorded || frame != m_frame || view != m_view ||
        !frame.IsValid() || !view.IsValid() || !m_inputs.historyIdentity.IsValid())
    {
        failure = "Opaque refraction history requires unique matching frozen frame/view inputs";
        return false;
    }
    const RHITextureDesc* source = builder.GetGraph().GetTextureDesc(context.view.colorTarget);
    if (!source)
    {
        failure = "Opaque refraction history has no current graph color source";
        return false;
    }
    PostProcessFrameInputs inputs = m_inputs;
    inputs.sceneColor = context.view.colorTarget;
    inputs.depth = context.view.depthTarget;
    inputs.temporalLinearDepth = {};
    inputs.velocity = context.view.velocityTarget;
    inputs.reactiveMask = context.view.reactiveMaskTarget;
    // This is the prior-presented producer state, not the later histogram
    // candidate. Referencing the latter would introduce a graph cycle.
    inputs.exposureState = context.view.exposureState;
    inputs.exposureStateContentHash = context.view.exposureStateContentHash;
    const Mat4& p = context.view.projectionMatrix;
    inputs.deviceDepthProjection = Vec4(p[2][2], p[3][2], p[2][3], p[3][3]);
    RenderViewHistoryIdentity compatible = m_committedIdentity;
    const bool consecutive = compatible.IsValid() &&
        compatible.previousFrame.frameSequence + 1 == inputs.historyIdentity.previousFrame.frameSequence;
    compatible.previousFrame = inputs.historyIdentity.previousFrame;
    if (!m_resolve.HasValidHistory() || !consecutive || compatible != inputs.historyIdentity)
    {
        inputs.historyAvailable = false;
        inputs.historyResetReasons |= RenderHistoryResetReason::HistoryUnavailable;
    }
    RHITextureDesc desc = RHITextureDesc::RenderTarget(
        source->width, source->height, RHIFormat::RGBA16_FLOAT);
    desc.usage = desc.usage | RHITextureUsage::ShaderResource;
    desc.debugName = "OpaqueSceneTemporalColor";
    output = builder.CreateTexture(desc);
    if (!output.IsValid() ||
        !builder.AddGraphCallback("OpaqueSceneTemporalResolve", context,
            [this, inputs, output](RenderGraph& graph)
            { m_resolve.AddToGraph(graph, inputs, output); }) ||
        !m_resolve.GetLastResolveStats().inputsReady)
    {
        failure = "Opaque refraction temporal resolve: " +
            m_resolve.GetLastResolveStats().fallbackReason;
        return false;
    }
    m_recorded = true;
    return true;
}

bool OpaqueSceneColorHistory::RequiresCommit() const noexcept
{
    const auto& stats = m_resolve.GetLastResolveStats();
    return m_recorded && stats.historyCandidatePrepared && !stats.historyCommitted;
}
bool OpaqueSceneColorHistory::CanCommit(const RenderViewHistoryIdentity& identity) const noexcept
{
    return !RequiresCommit() || m_resolve.CanCommitHistoryAfterPresent(identity);
}
bool OpaqueSceneColorHistory::Commit(const RenderViewHistoryIdentity& identity) noexcept
{
    if (!RequiresCommit()) return true;
    if (!m_resolve.CommitHistoryAfterPresent(identity)) return false;
    m_committedIdentity = identity;
    return true;
}
void OpaqueSceneColorHistory::Abandon() noexcept { m_resolve.AbandonPendingHistory(); }
void OpaqueSceneColorHistory::Shutdown()
{
    m_resolve.Shutdown();
    m_recorded = false;
    m_inputs = {};
    m_committedIdentity = {};
}
}
