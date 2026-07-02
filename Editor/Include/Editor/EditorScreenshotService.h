/**
 * @file EditorScreenshotService.h
 * @brief Editor screenshot capture service
 */

#pragma once

#include "Core/Types.h"
#include "RHI/RHIDefinitions.h"

#include <string>

struct GLFWwindow;

namespace RVX
{
    class RenderContext;
}

namespace RVX::Editor
{

class EditorMainFramebufferService;

struct EditorScreenshotRequest
{
    std::string path;
    GLFWwindow* window = nullptr;
    RenderContext* renderContext = nullptr;
    bool allowOpenGLDefaultFramebuffer = false;
};

struct EditorMainFramebufferScreenshotDesc
{
    std::string path;
    GLFWwindow* window = nullptr;
    RenderContext* renderContext = nullptr;
    const EditorMainFramebufferService* mainFramebufferService = nullptr;
};

struct EditorScreenshotStats
{
    bool attempted = false;
    bool succeeded = false;
    bool openGLDefaultFramebufferUsed = false;
    bool rhiSwapChainUsed = false;
    uint32 width = 0;
    uint32 height = 0;
    RHIFormat format = RHIFormat::Unknown;
    std::string failureReason;
};

class EditorScreenshotService final
{
public:
    // =========================================================================
    // Capture
    // =========================================================================

    bool CaptureMainFramebuffer(const EditorScreenshotRequest& request);
    bool CaptureMainFramebuffer(const EditorMainFramebufferScreenshotDesc& desc);

    // =========================================================================
    // Diagnostics
    // =========================================================================

    const EditorScreenshotStats& GetStats() const { return m_stats; }

private:
    bool CaptureOpenGLDefaultFramebuffer(const EditorScreenshotRequest& request);
    bool CaptureRHISwapChainBackBuffer(const EditorScreenshotRequest& request);
    bool Fail(std::string reason);

    EditorScreenshotStats m_stats;
};

} // namespace RVX::Editor
