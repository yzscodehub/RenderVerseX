/**
 * @file FrameSynchronizer.cpp
 * @brief Frame synchronization implementation
 */

#include "Render/Context/FrameSynchronizer.h"
#include "Core/Log.h"

namespace RVX
{

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

    m_device = device;
    m_frameCount = frameCount;

    // Create fences for each frame
    for (uint32_t i = 0; i < m_frameCount; ++i)
    {
        m_fences[i] = m_device->CreateFence(0);
        if (!m_fences[i])
        {
            RVX_CORE_ERROR("FrameSynchronizer: Failed to create fence for frame {}", i);
            Shutdown();
            return false;
        }
        m_fenceValues[i] = 0;
    }

    RVX_CORE_DEBUG("FrameSynchronizer initialized with {} frames", m_frameCount);
    return true;
}

void FrameSynchronizer::Shutdown()
{
    if (m_device)
    {
        // Wait for all work to complete before destroying fences
        WaitForAllFrames();
    }

    for (uint32_t i = 0; i < RVX_MAX_FRAME_COUNT; ++i)
    {
        m_fences[i].Reset();
        m_fenceValues[i] = 0;
    }

    m_device = nullptr;
    m_frameCount = 0;
}

void FrameSynchronizer::WaitForFrame(uint32_t frameIndex)
{
    if (frameIndex >= m_frameCount)
    {
        RVX_CORE_WARN("FrameSynchronizer: Invalid frame index {}", frameIndex);
        return;
    }

    RHIFence* fence = m_fences[frameIndex].Get();
    if (!fence)
        return;

    uint64_t expectedValue = m_fenceValues[frameIndex];
    if (expectedValue == 0)
        return;  // No work submitted yet for this frame

    // Wait for the fence to reach the expected value
    if (fence->GetCompletedValue() < expectedValue)
    {
        fence->Wait(expectedValue);
    }
}

void FrameSynchronizer::SignalFrame(uint32_t frameIndex)
{
    if (frameIndex >= m_frameCount)
    {
        RVX_CORE_WARN("FrameSynchronizer: Invalid frame index {}", frameIndex);
        return;
    }

    // Increment the expected fence value for this frame
    // Note: The GPU queue signals the timeline semaphore during SubmitCommandContext,
    // so we only track the value here for WaitForFrame() to work correctly.
    m_fenceValues[frameIndex]++;
    
    // Don't call fence->Signal() here - the GPU queue already signals the fence
    // via vkQueueSubmit with the timeline semaphore.
}

void FrameSynchronizer::WaitForAllFrames()
{
    for (uint32_t i = 0; i < m_frameCount; ++i)
    {
        WaitForFrame(i);
    }
}

RHIFence* FrameSynchronizer::GetFence(uint32_t frameIndex) const
{
    if (frameIndex >= m_frameCount)
        return nullptr;
    return m_fences[frameIndex].Get();
}

uint64_t FrameSynchronizer::GetFrameFenceValue(uint32_t frameIndex) const
{
    if (frameIndex >= m_frameCount)
        return 0;
    return m_fenceValues[frameIndex];
}

bool FrameSynchronizer::IsFrameComplete(uint32_t frameIndex) const
{
    if (frameIndex >= m_frameCount)
        return true;

    RHIFence* fence = m_fences[frameIndex].Get();
    if (!fence)
        return true;

    return fence->GetCompletedValue() >= m_fenceValues[frameIndex];
}

} // namespace RVX
