/**
 * @file LocalShadowPass.cpp
 * @brief RenderGraph-owned local point/spot shadow atlas recording.
 */

#include "Render/Passes/LocalShadowPass.h"

#include <atomic>
#include <limits>
#include <mutex>
#include <utility>

namespace RVX
{
    struct LocalShadowPass::ExecutionState
    {
        uint32 expectedPageCount = 0;
        std::atomic<uint32> realizedPageCount{0};
        std::atomic<uint32> recordedPageCount{0};
        std::atomic<uint32> casterDrawCount{0};
        std::atomic<bool> failed{false};
        mutable std::mutex diagnosticMutex{};
        std::string diagnostic{};

        void Fail(std::string failure)
        {
            failed.store(true, std::memory_order_relaxed);
            std::lock_guard<std::mutex> lock(diagnosticMutex);
            if (diagnostic.empty())
            {
                diagnostic = std::move(failure);
            }
        }

        [[nodiscard]] std::string GetDiagnostic() const
        {
            std::lock_guard<std::mutex> lock(diagnosticMutex);
            return diagnostic;
        }
    };

namespace
{
    struct LocalShadowPagePassData
    {
        RGTextureHandle atlas{};
        RGTextureViewHandle depthView{};
        LocalShadowAtlasPage page{};
        uint32 pageResolution = 0;
        LocalShadowCasterRecorder recordCasters{};
        std::shared_ptr<LocalShadowPass::ExecutionState> executionState{};
    };

    [[nodiscard]] bool IsRequestCompatible(
        const LocalShadowPassRequest& request,
        const LocalShadowAtlasPlan& plan) noexcept
    {
        return request.expectedAtlasPlanIdentity == 0 ||
               request.expectedAtlasPlanIdentity == plan.GetIdentityHash();
    }

    [[nodiscard]] bool IsGenerationCompatible(
        const LocalShadowPassRequest& request,
        const LocalShadowAtlasPlan& plan) noexcept
    {
        return request.expectedAtlasGeneration == 0 ||
               request.expectedAtlasGeneration == plan.GetAtlasGeneration();
    }

    void AddPageDepthView(
        RenderGraphBuilder& builder,
        LocalShadowPagePassData& data)
    {
        const RHITextureDesc* const descriptor = builder.GetTextureDesc(data.atlas);
        if (descriptor == nullptr || data.page.pageIndex >= descriptor->arraySize ||
            data.pageResolution == 0 || descriptor->width != data.pageResolution ||
            descriptor->height != data.pageResolution ||
            descriptor->format != RHIFormat::D32_FLOAT ||
            !HasFlag(descriptor->usage, RHITextureUsage::DepthStencil) ||
            !HasFlag(descriptor->usage, RHITextureUsage::ShaderResource))
        {
            data.executionState->Fail(
                "Local shadow atlas descriptor no longer matches the sealed CPU plan");
            return;
        }

        RHITextureViewDesc viewDesc;
        viewDesc.format = RHIFormat::D32_FLOAT;
        viewDesc.dimension = RHITextureDimension::Texture2D;
        viewDesc.subresourceRange = RHISubresourceRange{
            0, 1, data.page.pageIndex, 1, RHITextureAspect::Depth};
        viewDesc.type = RHITextureViewType::DepthStencil;
        viewDesc.debugName = "LocalShadowAtlasPageDSV";
        data.depthView = builder.Write(
            builder.CreateTextureView(data.atlas, viewDesc),
            MakeRGAccessDesc(RHIResourceState::DepthWrite,
                             RHIShaderStage::None,
                             RHIDiscardIntent::Discard));
        if (!data.depthView.IsValid())
        {
            data.executionState->Fail(
                "Local shadow atlas page depth view could not be declared");
            return;
        }
        builder.SetExportState(data.atlas, RHIResourceState::ShaderResource);
    }
} // namespace

LocalShadowPass::~LocalShadowPass() = default;

LocalShadowPassOutput LocalShadowPass::AddToGraph(
    RenderGraph& graph,
    const LocalShadowPassRequest& request)
{
    m_pendingExecutionState.reset();
    m_lastExecutionEvidence = {};

    LocalShadowPassOutput output;
    if (request.atlasPlan == nullptr || !request.atlasPlan->IsValid())
    {
        output.code = LocalShadowPassCode::InvalidRequest;
        output.diagnostic = "Local shadow pass requires a valid immutable atlas plan";
        return output;
    }

    const LocalShadowAtlasPlan& plan = *request.atlasPlan;
    const LocalShadowAtlasExecutionEvidence& planEvidence =
        plan.GetExecutionEvidence();
    if (plan.GetPages().size() > std::numeric_limits<uint32>::max())
    {
        output.code = LocalShadowPassCode::InvalidRequest;
        output.diagnostic = "Local shadow atlas page count exceeds the graph contract";
        m_lastExecutionEvidence.diagnostic = output.diagnostic;
        return output;
    }
    m_lastExecutionEvidence.atlasPlanIdentity = plan.GetIdentityHash();
    m_lastExecutionEvidence.atlasGeneration = plan.GetAtlasGeneration();
    m_lastExecutionEvidence.expectedPageCount =
        static_cast<uint32>(plan.GetPages().size());
    m_lastExecutionEvidence.cpuPlanReady = planEvidence.cpuPlanReady;

    if (!IsRequestCompatible(request, plan) || !IsGenerationCompatible(request, plan))
    {
        output.code = LocalShadowPassCode::StaleAtlasPlan;
        output.diagnostic = "Local shadow pass request does not match atlas identity or generation";
        m_lastExecutionEvidence.diagnostic = output.diagnostic;
        return output;
    }

    if (plan.GetPages().empty())
    {
        output.code = LocalShadowPassCode::NoWork;
        output.frameIdentity = plan.GetFrameIdentity();
        output.viewIdentity = plan.GetViewIdentity();
        output.lightingPlanIdentity = plan.GetLightingPlanIdentity();
        output.localShadowPlanIdentity = plan.GetLocalShadowPlanIdentity();
        output.atlasPlanIdentity = plan.GetIdentityHash();
        output.atlasGeneration = plan.GetAtlasGeneration();
        output.diagnostic = "Local shadow atlas plan selected no pages";
        return output;
    }

    const LocalShadowAtlasTextureContract& textureContract =
        plan.GetTextureContract();
    const RHITextureDesc* const textureDesc = textureContract.GetTextureDesc();
    if (!planEvidence.cpuPlanReady || !textureContract.IsValid() ||
        textureDesc == nullptr || textureDesc->arraySize != textureContract.pageCapacity)
    {
        output.code = LocalShadowPassCode::InvalidRequest;
        output.diagnostic = "Local shadow atlas plan has an invalid GPU texture contract";
        m_lastExecutionEvidence.diagnostic = output.diagnostic;
        return output;
    }

    const RGTextureHandle atlas = graph.CreateTexture(*textureDesc);
    if (!atlas.IsValid())
    {
        output.code = LocalShadowPassCode::GraphResourceCreationFailed;
        output.diagnostic = "RenderGraph could not declare the local shadow depth atlas";
        m_lastExecutionEvidence.diagnostic = output.diagnostic;
        return output;
    }

    auto executionState = std::make_shared<ExecutionState>();
    executionState->expectedPageCount =
        static_cast<uint32>(plan.GetPages().size());
    m_pendingExecutionState = executionState;

    output.code = LocalShadowPassCode::Ready;
    output.depthAtlas = atlas;
    output.frameIdentity = plan.GetFrameIdentity();
    output.viewIdentity = plan.GetViewIdentity();
    output.lightingPlanIdentity = plan.GetLightingPlanIdentity();
    output.localShadowPlanIdentity = plan.GetLocalShadowPlanIdentity();
    output.atlasPlanIdentity = plan.GetIdentityHash();
    output.atlasGeneration = plan.GetAtlasGeneration();
    output.pageResolution = textureContract.pageResolution;
    output.pageCount = executionState->expectedPageCount;
    output.pageDepthLayers.reserve(plan.GetPages().size());

    for (const LocalShadowAtlasPage& page : plan.GetPages())
    {
        RGTextureHandle pageLayer = atlas;
        pageLayer.hasSubresourceRange = true;
        pageLayer.subresourceRange = RHISubresourceRange{
            0, 1, page.pageIndex, 1, RHITextureAspect::Depth};
        output.pageDepthLayers.push_back(pageLayer);

        graph.AddPass<LocalShadowPagePassData>(
            "LocalShadowAtlasPage",
            RenderGraphPassType::Graphics,
            [atlas, page, pageResolution = textureContract.pageResolution,
             recordCasters = request.recordCasters, executionState](
                RenderGraphBuilder& builder,
                LocalShadowPagePassData& data)
            {
                data.atlas = atlas;
                data.page = page;
                data.pageResolution = pageResolution;
                data.recordCasters = recordCasters;
                data.executionState = executionState;
                AddPageDepthView(builder, data);
            },
            [](const LocalShadowPagePassData& data,
               RenderGraphPassContext& context)
            {
                if (data.executionState == nullptr ||
                    data.executionState->failed.load(std::memory_order_relaxed))
                {
                    if (data.executionState != nullptr)
                    {
                        data.executionState->Fail(
                            "Local shadow page execution started after a failed preflight");
                    }
                    context.MarkExecutionFailed(
                        "Local shadow page immutable recording resources are unavailable");
                    return;
                }

                RHITexture* const atlas = context.GetTexture(data.atlas);
                RHITextureView* const depthView =
                    context.GetTextureView(data.depthView);
                if (atlas == nullptr || depthView == nullptr ||
                    depthView->GetTexture() != atlas)
                {
                    data.executionState->Fail(
                        "Local shadow atlas page could not realize its exact depth layer");
                    context.MarkExecutionFailed(
                        "Local shadow atlas page texture/view realization failed");
                    return;
                }

                RHICommandContext& commands = context.Commands();
                RHIRenderPassDesc renderPass;
                renderPass.SetDepthStencil(
                    depthView,
                    RHILoadOp::Clear,
                    RHIStoreOp::Store,
                    RVX_LOCAL_SHADOW_CLEAR_DEPTH,
                    0);
                renderPass.SetRenderArea(
                    0, 0, data.pageResolution, data.pageResolution);
                commands.BeginRenderPass(renderPass);
                RHIViewport viewport;
                viewport.width = static_cast<float>(data.pageResolution);
                viewport.height = static_cast<float>(data.pageResolution);
                viewport.minDepth = RVX_LOCAL_SHADOW_NEAR_DEVICE_DEPTH;
                viewport.maxDepth = RVX_LOCAL_SHADOW_FAR_DEVICE_DEPTH;
                commands.SetViewport(viewport);
                RHIRect scissor;
                scissor.width = data.pageResolution;
                scissor.height = data.pageResolution;
                commands.SetScissor(scissor);

                uint32 pageDrawCount = 0;
                std::string recorderDiagnostic;
                const bool castersRecorded = !data.recordCasters ||
                    data.recordCasters(
                        commands, data.page, pageDrawCount, recorderDiagnostic);
                commands.EndRenderPass();
                if (!castersRecorded)
                {
                    data.executionState->Fail(
                        recorderDiagnostic.empty()
                            ? "Local shadow caster recorder rejected the sealed page"
                            : std::move(recorderDiagnostic));
                    context.MarkExecutionFailed(
                        "Local shadow caster recorder failed");
                    return;
                }

                data.executionState->realizedPageCount.fetch_add(
                    1U, std::memory_order_relaxed);
                data.executionState->recordedPageCount.fetch_add(
                    1U, std::memory_order_relaxed);
                data.executionState->casterDrawCount.fetch_add(
                    pageDrawCount, std::memory_order_relaxed);
            });
    }

    return output;
}

void LocalShadowPass::FinalizeGraphExecution(bool executionAccepted) noexcept
{
    if (m_pendingExecutionState == nullptr)
    {
        return;
    }

    const std::shared_ptr<ExecutionState> state = m_pendingExecutionState;
    m_lastExecutionEvidence.realizedPageCount =
        state->realizedPageCount.load(std::memory_order_relaxed);
    m_lastExecutionEvidence.recordedPageCount =
        state->recordedPageCount.load(std::memory_order_relaxed);
    m_lastExecutionEvidence.casterDrawCount =
        state->casterDrawCount.load(std::memory_order_relaxed);
    m_lastExecutionEvidence.gpuResourceCreated =
        m_lastExecutionEvidence.realizedPageCount == state->expectedPageCount;
    m_lastExecutionEvidence.gpuCommandsRecorded =
        m_lastExecutionEvidence.recordedPageCount == state->expectedPageCount;
    m_lastExecutionEvidence.gpuExecuted = executionAccepted &&
        !state->failed.load(std::memory_order_relaxed) &&
        m_lastExecutionEvidence.gpuResourceCreated &&
        m_lastExecutionEvidence.gpuCommandsRecorded;
    m_lastExecutionEvidence.diagnostic = state->GetDiagnostic();
    if (!m_lastExecutionEvidence.gpuExecuted &&
        m_lastExecutionEvidence.diagnostic.empty())
    {
        m_lastExecutionEvidence.diagnostic =
            "Local shadow graph execution was incomplete or rejected";
    }
    m_pendingExecutionState.reset();
}

LocalShadowPassExecutionEvidence LocalShadowPass::GetLastExecutionEvidence() const
{
    return m_lastExecutionEvidence;
}

const char* GetLocalShadowPassCodeName(LocalShadowPassCode code) noexcept
{
    switch (code)
    {
        case LocalShadowPassCode::Ready: return "ready";
        case LocalShadowPassCode::NoWork: return "no-work";
        case LocalShadowPassCode::InvalidRequest: return "invalid-request";
        case LocalShadowPassCode::StaleAtlasPlan: return "stale-atlas-plan";
        case LocalShadowPassCode::GraphResourceCreationFailed:
            return "graph-resource-creation-failed";
        default: return "unknown";
    }
}
} // namespace RVX
