/**
 * @file EditorRunLoopService.h
 * @brief Run-loop frame and screenshot scheduling state for the editor.
 */

#pragma once

#include "Core/Types.h"

#include <string>

namespace RVX::Editor
{

struct EditorRunLoopConfig
{
    uint32 maxFrames = 0;
    std::string screenshotPath;
};

struct EditorRunLoopFramePlan
{
    uint32 frameIndex = 0;
    bool captureScreenshot = false;
    std::string screenshotPath;
};

/**
 * @brief Owns editor run-loop counters and screenshot scheduling state.
 */
class EditorRunLoopService
{
public:
    void Configure(EditorRunLoopConfig config);
    void BeginRun();
    EditorRunLoopFramePlan BeginFrame();
    void CompleteFrame();

    bool HasAutomatedRunRequest() const;
    bool ShouldStopAfterFrame() const { return m_stopRequested; }
    uint32 GetRenderedFrameCount() const { return m_renderedFrameCount; }

    bool HasPendingScreenshot() const { return !m_pendingScreenshotPath.empty(); }
    const std::string& GetPendingScreenshotPath() const
    {
        return m_pendingScreenshotPath;
    }
    std::string ConsumePendingScreenshotPath();
    void RecordScreenshotResult(bool succeeded);

    bool DidScreenshotSucceed() const { return m_lastScreenshotSucceeded; }
    int BuildExitCode(bool automationFailed) const;

private:
    EditorRunLoopConfig m_config;
    uint32 m_renderedFrameCount = 0;
    bool m_stopRequested = false;
    bool m_lastScreenshotSucceeded = true;
    std::string m_pendingScreenshotPath;
};

} // namespace RVX::Editor
