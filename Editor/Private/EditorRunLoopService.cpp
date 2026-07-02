/**
 * @file EditorRunLoopService.cpp
 * @brief Run-loop frame and screenshot scheduling state for the editor.
 */

#include "Editor/EditorRunLoopService.h"

#include <utility>

namespace RVX::Editor
{

void EditorRunLoopService::Configure(EditorRunLoopConfig config)
{
    m_config = std::move(config);
}

void EditorRunLoopService::BeginRun()
{
    m_renderedFrameCount = 0;
    m_stopRequested = false;
    m_lastScreenshotSucceeded = true;
    m_pendingScreenshotPath.clear();
}

EditorRunLoopFramePlan EditorRunLoopService::BeginFrame()
{
    EditorRunLoopFramePlan plan;
    plan.frameIndex = m_renderedFrameCount;
    plan.captureScreenshot =
        !m_config.screenshotPath.empty() &&
        m_config.maxFrames > 0 &&
        m_renderedFrameCount + 1 >= m_config.maxFrames;
    if (plan.captureScreenshot)
    {
        plan.screenshotPath = m_config.screenshotPath;
        m_pendingScreenshotPath = m_config.screenshotPath;
    }
    return plan;
}

void EditorRunLoopService::CompleteFrame()
{
    ++m_renderedFrameCount;
    m_stopRequested =
        m_config.maxFrames > 0 &&
        m_renderedFrameCount >= m_config.maxFrames;
}

bool EditorRunLoopService::HasAutomatedRunRequest() const
{
    return m_config.maxFrames > 0 || !m_config.screenshotPath.empty();
}

std::string EditorRunLoopService::ConsumePendingScreenshotPath()
{
    std::string path = std::move(m_pendingScreenshotPath);
    m_pendingScreenshotPath.clear();
    return path;
}

void EditorRunLoopService::RecordScreenshotResult(bool succeeded)
{
    if (!succeeded)
    {
        m_lastScreenshotSucceeded = false;
    }
}

int EditorRunLoopService::BuildExitCode(bool automationFailed) const
{
    if (automationFailed)
    {
        return -3;
    }
    return m_lastScreenshotSucceeded ? 0 : -2;
}

} // namespace RVX::Editor
