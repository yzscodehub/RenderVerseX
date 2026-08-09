/**
 * @file RenderContext.cpp
 * @brief Render context implementation
 */

#include "Render/Context/RenderContext.h"
#include "Core/Assert.h"
#include "Core/Log.h"
#include "RHI_BackendFactory/RHIBackendFactory.h"
#include "Resources/RenderSubmissionTracker.h"

#include <algorithm>

namespace RVX
{

RenderContext::~RenderContext()
{
    Shutdown();
}

bool RenderContext::Initialize(const RenderContextConfig& config,
                               const NativeSurfaceDesc& initialSurface)
{
    if (m_initialized)
    {
        RVX_CORE_WARN("RenderContext already initialized");
        return true;
    }

    m_config = config;

    RVX_CORE_INFO("RenderContext initializing with backend: {}", ToString(config.backendType));

    // Create RHI device
    RHIDeviceDesc deviceDesc;
    deviceDesc.initialSurface = initialSurface;
    deviceDesc.enableDebugLayer = config.enableValidation;
    deviceDesc.enableGPUValidation = config.enableGPUValidation;
    deviceDesc.allowSoftwareAdapter = config.allowSoftwareAdapter;
    deviceDesc.applicationName = config.appName;

    m_device = CreateRHIDevice(config.backendType, deviceDesc);
    if (!m_device)
    {
        RVX_CORE_ERROR("RenderContext: Failed to create RHI device");
        return false;
    }

    RVX_CORE_INFO("RenderContext: Created device - Adapter: {}", 
                  m_device->GetCapabilities().adapterName);

    // Initialize frame synchronizer
    uint32_t frameCount = std::min(config.frameBuffering, RVX_MAX_FRAME_COUNT);
    if (!m_frameSynchronizer.Initialize(m_device.get(), frameCount))
    {
        RVX_CORE_ERROR("RenderContext: Failed to initialize frame synchronizer");
        m_device.reset();
        return false;
    }

    // Create command contexts
    CreateCommandContexts();

    // Check async compute support
    m_supportsAsyncCompute = m_device->GetCapabilities().supportsAsyncCompute;
    RVX_CORE_INFO("RenderContext: Async compute support: {}", m_supportsAsyncCompute ? "yes" : "no");

    m_initialized = true;
    m_frameActive = false;
    m_frameReadyToPresent = false;
    m_queueSubmissionPending = false;
    m_graphicsContextRecording = false;
    m_pendingGraphicsGateway.Reset();
    m_frameIndex = 0;
    m_frameNumber = 0;
    m_surface = {};

    RVX_CORE_INFO("RenderContext initialized successfully");
    return true;
}

void RenderContext::Shutdown(bool waitForIdle)
{
    if (!m_initialized)
        return;

    RVX_CORE_DEBUG("RenderContext shutting down...");

    if (m_pendingGraphExecution)
    {
        static_cast<void>(m_pendingGraphExecution->AbortUnsubmitted());
        m_pendingGraphExecution.reset();
    }

    if (waitForIdle)
    {
        WaitIdle();
    }

    for (std::unique_ptr<RenderGraphExecution>& execution :
         m_inFlightGraphExecutions)
    {
        if (!execution)
            continue;
        if (waitForIdle)
            static_cast<void>(execution->Retire());
        else
            static_cast<void>(execution->MarkDeviceLost());
        execution.reset();
    }

    // Destroy resources in reverse order
    DestroyCommandContexts();
    m_swapChain.Reset();
    // RenderContext already performed the normal wait above. Device-loss and
    // timeout teardown deliberately skip all completion waits.
    m_frameSynchronizer.Shutdown(false);
    m_device.reset();

    m_initialized = false;
    m_frameActive = false;
    m_frameReadyToPresent = false;
    m_queueSubmissionPending = false;
    m_graphicsContextRecording = false;
    m_pendingQueuePlan = {};
    m_pendingQueueContexts.clear();
    m_pendingGraphicsGateway.Reset();
    m_surface = {};

    RVX_CORE_INFO("RenderContext shutdown complete");
}

bool RenderContext::CreateSwapChain(const NativeSurfaceDesc& surface)
{
    if (!m_device)
    {
        RVX_CORE_ERROR("RenderContext: Cannot create swap chain without device");
        return false;
    }

    const RHIBackendType backend = m_device->GetBackendType();
    if (!surface.IsValidFor(backend))
    {
        RVX_CORE_ERROR("RenderContext: Invalid native surface for {}", ToString(backend));
        return false;
    }

    if (m_swapChain && surface.generation <= m_surface.generation)
    {
        RVX_CORE_WARN("RenderContext: Ignoring stale surface generation {} (current {})",
                      surface.generation,
                      m_surface.generation);
        return false;
    }

    if (m_swapChain &&
        !m_device->SupportsSurfaceRebind(m_surface, surface))
    {
        RVX_CORE_WARN(
            "RenderContext: Surface replacement requires device recreation for {}",
            ToString(backend));
        return false;
    }

    // Destroy existing swap chain
    if (m_swapChain)
    {
        if (!WaitForSurfaceGeneration())
        {
            RVX_CORE_ERROR("RenderContext: Surface generation completion was lost");
            return false;
        }
        m_swapChain.Reset();
        m_surface = {};
    }

    RHISwapChainDesc swapChainDesc;
    swapChainDesc.surface = surface;
    swapChainDesc.bufferCount = m_config.frameBuffering + 1;  // One extra for presentation
    swapChainDesc.debugName = "MainSwapChain";

    m_swapChain = m_device->CreateSwapChain(swapChainDesc);
    if (!m_swapChain)
    {
        RVX_CORE_ERROR("RenderContext: Failed to create swap chain");
        return false;
    }

    m_surface = surface;
    RVX_CORE_INFO("RenderContext: Created swap chain {}x{}",
                  surface.width,
                  surface.height);
    return true;
}

bool RenderContext::ResizeSwapChain(const NativeSurfaceDesc& surface)
{
    if (!m_swapChain || !m_device)
    {
        RVX_CORE_ERROR("RenderContext: Cannot update a missing swap chain");
        return false;
    }
    if (!surface.IsValidFor(m_device->GetBackendType()))
    {
        RVX_CORE_ERROR("RenderContext: Invalid native surface resize update");
        return false;
    }
    if (ClassifyNativeSurfaceUpdate(m_surface, surface) !=
        NativeSurfaceUpdateKind::Resize)
    {
        RVX_CORE_WARN("RenderContext: Surface update is not a newer extent-only change");
        return false;
    }

    ResizeSwapChain(surface.width, surface.height);
    m_surface = surface;
    return true;
}

void RenderContext::ResizeSwapChain(uint32_t width, uint32_t height)
{
    if (!m_swapChain)
    {
        RVX_CORE_WARN("RenderContext: No swap chain to resize");
        return;
    }

    if (width == 0 || height == 0)
    {
        RVX_CORE_WARN("RenderContext: Invalid resize dimensions {}x{}", width, height);
        return;
    }

    RVX_CORE_INFO("RenderContext: Resizing swap chain to {}x{}", width, height);

    // Retire the old surface generation from exact Graphics completion evidence.
    if (!WaitForSurfaceGeneration())
    {
        RVX_CORE_ERROR("RenderContext: Swap-chain resize completion was lost");
        return;
    }

    m_swapChain->Resize(width, height);
    m_surface.width = width;
    m_surface.height = height;
}

bool RenderContext::BeginFrame()
{
    if (m_frameActive)
    {
        RVX_CORE_WARN("RenderContext: BeginFrame called while frame already active");
        return false;
    }

    m_frameReadyToPresent = false;
    if (!m_initialized || !m_device)
    {
        RVX_CORE_WARN("RenderContext: BeginFrame called before initialization");
        return false;
    }

    // Wait for this frame slot to be available
    if (!m_frameSynchronizer.WaitForFrame(m_frameIndex))
    {
        RVX_CORE_ERROR("RenderContext: Frame slot {} completion was lost", m_frameIndex);
        return false;
    }
    if (m_inFlightGraphExecutions[m_frameIndex])
    {
        static_cast<void>(
            m_inFlightGraphExecutions[m_frameIndex]->Retire());
        m_inFlightGraphExecutions[m_frameIndex].reset();
    }
    m_inFlightQueueContexts[m_frameIndex].clear();

    // Begin device frame
    m_device->BeginFrame();

    // Reset and begin the command context for this frame
    RHICommandContext* ctx = GetGraphicsContext();
    if (!ctx)
    {
        RVX_CORE_ERROR("RenderContext: No Graphics command context for frame {}", m_frameIndex);
        m_device->EndFrame();
        return false;
    }
    ctx->Reset();
    ctx->Begin();

    m_frameActive = true;
    m_graphicsContextRecording = true;
    return true;
}

bool RenderContext::AdoptQueueSubmission(
    RHIQueueSubmissionPlan plan,
    std::vector<RHICommandContextRef> ownedContexts)
{
    size_t plannedContextCount = 0;
    for (const RHIQueueSubmissionBatch& batch : plan.batches)
    {
        plannedContextCount += batch.contexts.size();
    }
    const bool ownsEveryContext =
        ownedContexts.size() == plannedContextCount &&
        std::all_of(
            plan.batches.begin(),
            plan.batches.end(),
            [&ownedContexts](const RHIQueueSubmissionBatch& batch)
            {
                return std::all_of(
                    batch.contexts.begin(),
                    batch.contexts.end(),
                    [&ownedContexts](RHICommandContext* context)
                    {
                        return std::any_of(
                            ownedContexts.begin(),
                            ownedContexts.end(),
                            [context](const RHICommandContextRef& owned)
                            {
                                return owned.Get() == context;
                            });
                    });
            });
    RHICommandContext* graphicsPrelude =
        m_frameIndex < RVX_MAX_FRAME_COUNT
            ? m_graphicsContexts[m_frameIndex].Get()
            : nullptr;
    if (!m_frameActive || m_queueSubmissionPending || !m_device ||
        !m_graphicsContextRecording || !graphicsPrelude ||
        graphicsPrelude->GetQueueType() != RHICommandQueueType::Graphics ||
        !m_device->GetCapabilities().supportsQueueSubmissionPlan ||
        !ValidateRHIQueueSubmissionPlan(plan) || !ownsEveryContext)
    {
        return false;
    }

    RHICommandContextRef graphicsGateway =
        m_device->CreateCommandContext(RHICommandQueueType::Graphics);
    if (!graphicsGateway ||
        graphicsGateway->GetQueueType() != RHICommandQueueType::Graphics)
    {
        RVX_CORE_ERROR(
            "RenderContext: Failed to create the terminal Graphics gateway context");
        return false;
    }

    // Wrap the graph-owned DAG in two Graphics batches. The prelude orders any
    // work recorded before RenderGraph against every graph root. The gateway
    // remains recording after adoption so capture/readback work is guaranteed
    // to execute after the graph terminal and becomes the only frame-fence and
    // presentation terminal.
    RHIQueueSubmissionPlan augmentedPlan;
    augmentedPlan.batches.reserve(plan.batches.size() + 2u);

    RHIQueueSubmissionBatch preludeBatch;
    preludeBatch.queueType = RHICommandQueueType::Graphics;
    preludeBatch.contexts.push_back(graphicsPrelude);
    augmentedPlan.batches.push_back(std::move(preludeBatch));

    for (RHIQueueSubmissionBatch& sourceBatch : plan.batches)
    {
        RHIQueueSubmissionBatch batch;
        batch.queueType = sourceBatch.queueType;
        batch.contexts = std::move(sourceBatch.contexts);
        batch.prerequisiteBatchIndices.reserve(
            sourceBatch.prerequisiteBatchIndices.size() + 1u);
        for (uint32 prerequisite : sourceBatch.prerequisiteBatchIndices)
        {
            batch.prerequisiteBatchIndices.push_back(prerequisite + 1u);
        }
        if (sourceBatch.prerequisiteBatchIndices.empty())
        {
            batch.prerequisiteBatchIndices.push_back(0u);
        }
        augmentedPlan.batches.push_back(std::move(batch));
    }

    RHIQueueSubmissionBatch gatewayBatch;
    gatewayBatch.queueType = RHICommandQueueType::Graphics;
    gatewayBatch.contexts.push_back(graphicsGateway.Get());
    gatewayBatch.prerequisiteBatchIndices.push_back(
        plan.terminalGraphicsBatchIndex + 1u);
    augmentedPlan.batches.push_back(std::move(gatewayBatch));
    augmentedPlan.terminalGraphicsBatchIndex =
        static_cast<uint32>(augmentedPlan.batches.size() - 1u);

    const RHIQueueSubmissionPlanValidationResult augmentedValidation =
        ValidateRHIQueueSubmissionPlan(augmentedPlan);
    if (!augmentedValidation)
    {
        RVX_CORE_ERROR(
            "RenderContext: Invalid framed queue submission plan: {}",
            augmentedValidation.message);
        return false;
    }

    std::vector<RHICommandContextRef> augmentedOwnedContexts;
    augmentedOwnedContexts.reserve(ownedContexts.size() + 2u);
    augmentedOwnedContexts.push_back(m_graphicsContexts[m_frameIndex]);
    for (RHICommandContextRef& context : ownedContexts)
    {
        augmentedOwnedContexts.push_back(std::move(context));
    }
    augmentedOwnedContexts.push_back(graphicsGateway);

    graphicsPrelude->End();
    m_graphicsContextRecording = false;
    graphicsGateway->Reset();
    graphicsGateway->Begin();

    m_pendingQueuePlan = std::move(augmentedPlan);
    m_pendingQueueContexts = std::move(augmentedOwnedContexts);
    m_pendingGraphicsGateway = std::move(graphicsGateway);
    m_queueSubmissionPending = true;
    m_graphicsContextRecording = true;
    return true;
}

bool RenderContext::AdoptRenderGraphExecution(
    RenderGraphExecution&& execution)
{
    if (!m_frameActive || m_pendingGraphExecution ||
        execution.GetState() != RenderGraphExecutionState::Recorded)
    {
        return false;
    }

    auto ownedExecution =
        std::make_unique<RenderGraphExecution>(std::move(execution));
    if (ownedExecution->HasQueueSubmissionPlan())
    {
        RHIQueueSubmissionPlan plan;
        std::vector<RHICommandContextRef> ownedContexts;
        if (!ownedExecution->TakeQueueSubmission(plan, ownedContexts) ||
            !AdoptQueueSubmission(
                std::move(plan), std::move(ownedContexts)))
        {
            return false;
        }
    }

    if (!ownedExecution->MarkAdopted())
    {
        return false;
    }
    m_pendingGraphExecution = std::move(ownedExecution);
    return true;
}

GPUCompletionPoint RenderContext::EndFrame()
{
    if (!m_frameActive)
    {
        RVX_CORE_WARN("RenderContext: EndFrame called without BeginFrame");
        m_frameReadyToPresent = false;
        return {};
    }

    // End the command context
    RHICommandContext* ctx = GetGraphicsContext();
    if (ctx && m_graphicsContextRecording)
    {
        ctx->End();
        m_graphicsContextRecording = false;
    }

    // Submit commands
    GPUCompletionPoint submittedPoint;
    if (m_device && m_queueSubmissionPending)
    {
        submittedPoint =
            m_frameSynchronizer.SubmitQueuePlan(m_pendingQueuePlan);
        if (submittedPoint.domain == GPUQueueDomain::Graphics &&
            submittedPoint.value != 0)
        {
            m_inFlightQueueContexts[m_frameIndex] =
                std::move(m_pendingQueueContexts);
        }
        else
        {
            m_pendingQueueContexts.clear();
        }
        m_pendingQueuePlan = {};
        m_queueSubmissionPending = false;
        m_pendingGraphicsGateway.Reset();
    }
    else if (m_device && ctx)
    {
        submittedPoint = m_frameSynchronizer.SubmitGraphics(ctx);
    }
    if (submittedPoint.domain == GPUQueueDomain::Graphics &&
        submittedPoint.value != 0)
    {
        m_frameSynchronizer.SignalFrame(m_frameIndex, submittedPoint);
        if (m_pendingGraphExecution)
        {
            GPUCompletionToken completion;
            const bool tokenValid =
                InsertGPUCompletionPoint(completion, submittedPoint);
            const bool committed = tokenValid &&
                m_pendingGraphExecution->Commit(completion);
            if (!committed)
            {
                RVX_CORE_ERROR(
                    "RenderContext: RenderGraph execution rejected terminal completion token");
                if (m_device &&
                    m_device->QueryRuntimeStatus() ==
                        RHIDeviceRuntimeStatus::Ready)
                {
                    m_device->WaitIdle();
                }
                static_cast<void>(
                    m_pendingGraphExecution->MarkDeviceLost());
                m_pendingGraphExecution.reset();
            }
            else
            {
                RVX_ASSERT_MSG(
                    !m_inFlightGraphExecutions[m_frameIndex],
                    "Frame slot still owns a prior RenderGraph execution");
                m_inFlightGraphExecutions[m_frameIndex] =
                    std::move(m_pendingGraphExecution);
            }
        }
    }
    else if (m_pendingGraphExecution)
    {
        if (m_device &&
            m_device->QueryRuntimeStatus() != RHIDeviceRuntimeStatus::Ready)
        {
            static_cast<void>(m_pendingGraphExecution->MarkDeviceLost());
        }
        else
        {
            static_cast<void>(
                m_pendingGraphExecution->AbortUnsubmitted());
        }
        m_pendingGraphExecution.reset();
    }

    // End device frame
    if (m_device)
    {
        m_device->EndFrame();
    }

    m_frameActive = false;
    m_frameReadyToPresent =
        submittedPoint.domain == GPUQueueDomain::Graphics && submittedPoint.value != 0;
    return submittedPoint;
}

void RenderContext::AbortFrame()
{
    if (!m_frameActive)
    {
        return;
    }
    if (RHICommandContext* context = GetGraphicsContext())
    {
        if (m_graphicsContextRecording)
        {
            context->End();
        }
    }
    m_graphicsContextRecording = false;
    m_queueSubmissionPending = false;
    m_pendingQueuePlan = {};
    m_pendingQueueContexts.clear();
    m_pendingGraphicsGateway.Reset();
    if (m_pendingGraphExecution)
    {
        static_cast<void>(m_pendingGraphExecution->AbortUnsubmitted());
        m_pendingGraphExecution.reset();
    }
    if (m_device)
    {
        m_device->EndFrame();
    }
    m_frameActive = false;
    m_frameReadyToPresent = false;
}

void RenderContext::Present()
{
    if (!m_frameReadyToPresent)
    {
        RVX_CORE_WARN("RenderContext: Present called without a completed frame submission");
        return;
    }

    if (m_swapChain)
    {
        m_swapChain->Present();
    }

    // Advance frame index
    m_frameIndex = (m_frameIndex + 1) % m_frameSynchronizer.GetFrameCount();
    m_frameNumber++;
    m_frameReadyToPresent = false;
}

void RenderContext::WaitIdle()
{
    m_frameSynchronizer.WaitForAllFrames();
    
    if (m_device)
    {
        m_device->WaitIdle();
    }
}

RHICommandContext* RenderContext::GetGraphicsContext() const
{
    if (m_queueSubmissionPending && m_pendingGraphicsGateway)
    {
        return m_pendingGraphicsGateway.Get();
    }
    if (m_frameIndex >= RVX_MAX_FRAME_COUNT)
        return nullptr;
    return m_graphicsContexts[m_frameIndex].Get();
}

RHICommandContext* RenderContext::GetComputeContext() const
{
    if (!m_supportsAsyncCompute || m_frameIndex >= RVX_MAX_FRAME_COUNT)
        return nullptr;
    return m_computeContexts[m_frameIndex].Get();
}

RHITexture* RenderContext::GetCurrentBackBuffer() const
{
    if (!m_swapChain)
        return nullptr;
    return m_swapChain->GetCurrentBackBuffer();
}

RHITextureView* RenderContext::GetCurrentBackBufferView() const
{
    if (!m_swapChain)
        return nullptr;
    return m_swapChain->GetCurrentBackBufferView();
}

void RenderContext::CreateCommandContexts()
{
    if (!m_device)
        return;

    uint32_t frameCount = m_frameSynchronizer.GetFrameCount();
    for (uint32_t i = 0; i < frameCount; ++i)
    {
        // Create graphics context
        m_graphicsContexts[i] = m_device->CreateCommandContext(RHICommandQueueType::Graphics);
        if (!m_graphicsContexts[i])
        {
            RVX_CORE_ERROR("RenderContext: Failed to create graphics context for frame {}", i);
        }

        // Create compute context if async compute is supported
        if (m_device->GetCapabilities().supportsAsyncCompute)
        {
            m_computeContexts[i] = m_device->CreateCommandContext(RHICommandQueueType::Compute);
            if (!m_computeContexts[i])
            {
                RVX_CORE_WARN("RenderContext: Failed to create compute context for frame {}", i);
            }

        }
    }
}

void RenderContext::DestroyCommandContexts()
{
    for (uint32_t i = 0; i < RVX_MAX_FRAME_COUNT; ++i)
    {
        m_graphicsContexts[i].Reset();
        m_computeContexts[i].Reset();
        m_inFlightQueueContexts[i].clear();
    }
    m_pendingQueuePlan = {};
    m_pendingQueueContexts.clear();
    m_pendingGraphicsGateway.Reset();
    m_queueSubmissionPending = false;
    m_graphicsContextRecording = false;
}

bool RenderContext::WaitForSurfaceGeneration()
{
    if (!m_frameSynchronizer.WaitForAllFrames())
    {
        return false;
    }

    // Compatibility trackers perform at most one bounded WaitIdle while
    // resolving the first pending Graphics point. Native timelines never
    // require a device-wide idle for surface-generation replacement.
    return true;
}

} // namespace RVX
