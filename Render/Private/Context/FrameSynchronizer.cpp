/**
 * @file FrameSynchronizer.cpp
 * @brief Frame synchronization implementation
 */

#include "Render/Context/FrameSynchronizer.h"
#include "Core/Log.h"
#include "Resources/RenderSubmissionTracker.h"

namespace RVX
{

FrameSynchronizer::FrameSynchronizer() = default;

FrameSynchronizer::~FrameSynchronizer()
{
    Shutdown();
}

bool FrameSynchronizer::Initialize(IRHIDevice* device, uint32_t frameCount)
{
    if (!device)
    {
        RVX_CORE_ERROR("FrameSynchronizer: Invalid device");
        return false;
    }

    if (frameCount == 0 || frameCount > RVX_MAX_FRAME_COUNT)
    {
        RVX_CORE_ERROR("FrameSynchronizer: Invalid frame count {}", frameCount);
        return false;
    }

    auto tracker = std::make_unique<RenderSubmissionTracker>();
    if (!tracker->Initialize(device))
    {
        RVX_CORE_ERROR("FrameSynchronizer: Invalid queue topology or failed to create timelines");
        return false;
    }

    m_device = device;
    m_frameCount = frameCount;
    m_submissionTracker = std::move(tracker);
    m_framePoints = {};

    RVX_CORE_DEBUG("FrameSynchronizer initialized with {} frames", m_frameCount);
    return true;
}

void FrameSynchronizer::Shutdown(bool waitForCompletion)
{
    if (m_device && waitForCompletion)
    {
        // Wait for all work to complete before destroying fences
        WaitForAllFrames();
    }

    if (m_submissionTracker)
    {
        m_submissionTracker->Shutdown();
        m_submissionTracker.reset();
    }

    m_device = nullptr;
    m_frameCount = 0;
    m_framePoints = {};
}

bool FrameSynchronizer::WaitForFrame(uint32_t frameIndex)
{
    if (frameIndex >= m_frameCount)
    {
        RVX_CORE_WARN("FrameSynchronizer: Invalid frame index {}", frameIndex);
        return false;
    }

    const GPUCompletionPoint point = m_framePoints[frameIndex];
    if (!m_submissionTracker || point.value == 0)
        return true;

    const GPUCompletionStatus status = m_submissionTracker->Wait(point);
    if (status == GPUCompletionStatus::Lost)
    {
        RVX_CORE_ERROR("FrameSynchronizer: Graphics completion timeline was lost for frame {}",
                       frameIndex);
        return false;
    }
    return status == GPUCompletionStatus::Completed ||
           status == GPUCompletionStatus::CompatibilityWaitIdle;
}

void FrameSynchronizer::SignalFrame(uint32_t frameIndex, GPUCompletionPoint submittedPoint)
{
    if (frameIndex >= m_frameCount)
    {
        RVX_CORE_WARN("FrameSynchronizer: Invalid frame index {}", frameIndex);
        return;
    }

    if (submittedPoint.domain != GPUQueueDomain::Graphics || submittedPoint.value == 0)
    {
        RVX_CORE_WARN("FrameSynchronizer: submitted frame {} did not return a Graphics completion point",
                      frameIndex);
        return;
    }

    m_framePoints[frameIndex] = submittedPoint;
}

bool FrameSynchronizer::WaitForAllFrames()
{
    bool allFramesCompleted = true;
    for (uint32_t i = 0; i < m_frameCount; ++i)
    {
        allFramesCompleted = WaitForFrame(i) && allFramesCompleted;
    }
    return allFramesCompleted;
}

GPUCompletionPoint FrameSynchronizer::GetFrameCompletionPoint(uint32_t frameIndex) const
{
    if (frameIndex >= m_frameCount)
        return {};
    return m_framePoints[frameIndex];
}

bool FrameSynchronizer::IsFrameComplete(uint32_t frameIndex) const
{
    if (frameIndex >= m_frameCount)
        return true;

    const GPUCompletionPoint point = m_framePoints[frameIndex];
    if (!m_submissionTracker || point.value == 0)
        return true;

    const GPUCompletionStatus status = m_submissionTracker->Query(point);
    return status == GPUCompletionStatus::Completed ||
           status == GPUCompletionStatus::CompatibilityWaitIdle;
}

GPUCompletionPoint FrameSynchronizer::SubmitGraphics(RHICommandContext* context)
{
    if (!m_submissionTracker)
    {
        return {};
    }
    return m_submissionTracker->Submit(context);
}

} // namespace RVX
