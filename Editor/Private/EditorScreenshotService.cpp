/**
 * @file EditorScreenshotService.cpp
 * @brief Editor screenshot capture service implementation
 */

#include "Editor/EditorScreenshotService.h"
#include "Core/Log.h"
#include "Editor/EditorMainFramebufferService.h"
#include "Render/Context/RenderContext.h"
#include "RHI/RHIBuffer.h"
#include "RHI/RHICommandContext.h"
#include "RHI/RHIDevice.h"
#include "RHI/RHISwapChain.h"
#include "RHI/RHITexture.h"

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <filesystem>
#include <fstream>
#include <limits>
#include <utility>
#include <vector>

namespace RVX::Editor
{
namespace
{
    uint32 GetScreenshotBytesPerPixel(RHIFormat format)
    {
        switch (format)
        {
            case RHIFormat::RGBA8_UNORM:
            case RHIFormat::RGBA8_UNORM_SRGB:
            case RHIFormat::BGRA8_UNORM:
            case RHIFormat::BGRA8_UNORM_SRGB:
                return 4u;
            default:
                return 0u;
        }
    }

    bool WritePPMScreenshot(const std::string& path,
                            uint32 width,
                            uint32 height,
                            const uint8* pixels,
                            uint32 rowPitch,
                            RHIFormat format,
                            bool originBottomLeft)
    {
        if (!pixels || width == 0u || height == 0u || rowPitch == 0u)
        {
            RVX_CORE_ERROR("Editor screenshot failed: invalid pixel data");
            return false;
        }

        std::filesystem::path outputPath(path);
        if (outputPath.has_parent_path())
        {
            std::error_code ec;
            std::filesystem::create_directories(outputPath.parent_path(), ec);
            if (ec)
            {
                RVX_CORE_ERROR("Editor screenshot failed: could not create directory '{}': {}",
                               outputPath.parent_path().string(),
                               ec.message());
                return false;
            }
        }

        std::ofstream stream(outputPath, std::ios::binary);
        if (!stream)
        {
            RVX_CORE_ERROR("Editor screenshot failed: could not open '{}'",
                           outputPath.string());
            return false;
        }

        stream << "P6\n" << width << " " << height << "\n255\n";
        const bool bgra = format == RHIFormat::BGRA8_UNORM ||
                          format == RHIFormat::BGRA8_UNORM_SRGB;
        std::vector<uint8> row(static_cast<size_t>(width) * 3u);
        for (uint32 outY = 0; outY < height; ++outY)
        {
            const uint32 srcY = originBottomLeft ? (height - 1u - outY) : outY;
            const uint8* src = pixels + static_cast<size_t>(srcY) * rowPitch;
            for (uint32 x = 0; x < width; ++x)
            {
                const uint8* px = src + static_cast<size_t>(x) * 4u;
                row[static_cast<size_t>(x) * 3u + 0u] = bgra ? px[2] : px[0];
                row[static_cast<size_t>(x) * 3u + 1u] = px[1];
                row[static_cast<size_t>(x) * 3u + 2u] = bgra ? px[0] : px[2];
            }
            stream.write(reinterpret_cast<const char*>(row.data()),
                         static_cast<std::streamsize>(row.size()));
        }

        if (!stream)
        {
            RVX_CORE_ERROR("Editor screenshot failed while writing '{}'",
                           outputPath.string());
            return false;
        }

        RVX_CORE_INFO("Editor screenshot written: {}", outputPath.string());
        return true;
    }
} // namespace

bool EditorScreenshotService::CaptureMainFramebuffer(
    const EditorScreenshotRequest& request)
{
    m_stats = {};
    m_stats.attempted = true;

    if (request.path.empty())
    {
        return Fail("Editor screenshot failed: output path is empty");
    }

    if (request.allowOpenGLDefaultFramebuffer)
    {
        return CaptureOpenGLDefaultFramebuffer(request);
    }

    return CaptureRHISwapChainBackBuffer(request);
}

bool EditorScreenshotService::CaptureMainFramebuffer(
    const EditorMainFramebufferScreenshotDesc& desc)
{
    EditorScreenshotRequest request;
    request.path = desc.path;
    request.window = desc.window;
    request.renderContext = desc.renderContext;
    request.allowOpenGLDefaultFramebuffer =
        desc.mainFramebufferService &&
        desc.mainFramebufferService->CanUseOpenGLDefaultFramebuffer(
            desc.renderContext);
    return CaptureMainFramebuffer(request);
}

bool EditorScreenshotService::CaptureOpenGLDefaultFramebuffer(
    const EditorScreenshotRequest& request)
{
    if (!request.window)
    {
        return Fail("Editor screenshot failed: GLFW window is unavailable");
    }

    int displayW = 0;
    int displayH = 0;
    glfwGetFramebufferSize(request.window, &displayW, &displayH);
    if (displayW <= 0 || displayH <= 0)
    {
        return Fail("Editor screenshot failed: invalid framebuffer size " +
                    std::to_string(displayW) + "x" +
                    std::to_string(displayH));
    }

    std::vector<uint8> rgba(static_cast<size_t>(displayW) *
                           static_cast<size_t>(displayH) *
                           4u);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    glReadPixels(0,
                 0,
                 displayW,
                 displayH,
                 GL_RGBA,
                 GL_UNSIGNED_BYTE,
                 rgba.data());
    const GLenum error = glGetError();
    if (error != GL_NO_ERROR)
    {
        return Fail("Editor screenshot failed: glReadPixels error " +
                    std::to_string(static_cast<uint32>(error)));
    }

    m_stats.openGLDefaultFramebufferUsed = true;
    m_stats.width = static_cast<uint32>(displayW);
    m_stats.height = static_cast<uint32>(displayH);
    m_stats.format = RHIFormat::RGBA8_UNORM;
    m_stats.succeeded = WritePPMScreenshot(request.path,
                                           m_stats.width,
                                           m_stats.height,
                                           rgba.data(),
                                           m_stats.width * 4u,
                                           m_stats.format,
                                           true);
    return m_stats.succeeded;
}

bool EditorScreenshotService::CaptureRHISwapChainBackBuffer(
    const EditorScreenshotRequest& request)
{
    if (!request.renderContext ||
        !request.renderContext->IsInitialized() ||
        !request.renderContext->GetDevice())
    {
        return Fail("Editor screenshot failed: RHI render context is unavailable");
    }

    RHISwapChain* swapChain = request.renderContext->GetSwapChain();
    RHITexture* backBuffer = request.renderContext->GetCurrentBackBuffer();
    if (!swapChain || !backBuffer)
    {
        return Fail("Editor screenshot failed: main-window RHI swap chain is unavailable");
    }

    const uint32 width = backBuffer->GetWidth();
    const uint32 height = backBuffer->GetHeight();
    const RHIFormat format = backBuffer->GetFormat();
    const uint32 bytesPerPixel = GetScreenshotBytesPerPixel(format);
    if (width == 0u || height == 0u || bytesPerPixel == 0u)
    {
        return Fail("Editor screenshot failed: unsupported RHI target " +
                    std::to_string(width) + "x" +
                    std::to_string(height) + " format " +
                    std::to_string(static_cast<uint32>(format)));
    }

    const uint64 rowPitchBytes = static_cast<uint64>(width) * bytesPerPixel;
    const uint64 readbackBytes = rowPitchBytes * height;
    if (rowPitchBytes > static_cast<uint64>(std::numeric_limits<uint32>::max()))
    {
        return Fail("Editor screenshot failed: framebuffer row pitch is too large");
    }

    RHIBufferDesc readbackDesc;
    readbackDesc.size = readbackBytes;
    readbackDesc.usage = RHIBufferUsage::CopyDst;
    readbackDesc.memoryType = RHIMemoryType::Readback;
    readbackDesc.debugName = "Editor.MainFramebufferScreenshotReadback";

    RHIBufferRef readbackBuffer =
        request.renderContext->GetDevice()->CreateBuffer(readbackDesc);
    if (!readbackBuffer)
    {
        return Fail("Editor screenshot failed: could not create RHI readback buffer");
    }

    request.renderContext->BeginFrame();
    RHICommandContext* commandContext = request.renderContext->GetGraphicsContext();
    if (!commandContext)
    {
        request.renderContext->EndFrame();
        return Fail("Editor screenshot failed: RHI graphics command context is unavailable");
    }

    commandContext->TextureBarrier(backBuffer,
                                   RHIResourceState::RenderTarget,
                                   RHIResourceState::CopySource);
    RHIBufferTextureCopyDesc copyDesc;
    copyDesc.bufferRowPitch = static_cast<uint32>(rowPitchBytes);
    copyDesc.bufferImageHeight = height;
    copyDesc.textureRegion = {0u, 0u, width, height};
    commandContext->CopyTextureToBuffer(backBuffer, readbackBuffer.Get(), copyDesc);
    commandContext->TextureBarrier(backBuffer,
                                   RHIResourceState::CopySource,
                                   RHIResourceState::Present);
    request.renderContext->EndFrame();
    request.renderContext->WaitIdle();

    const uint8* mapped =
        static_cast<const uint8*>(readbackBuffer->Map());
    if (!mapped)
    {
        return Fail("Editor screenshot failed: could not map RHI readback buffer");
    }

    m_stats.rhiSwapChainUsed = true;
    m_stats.width = width;
    m_stats.height = height;
    m_stats.format = format;
    m_stats.succeeded = WritePPMScreenshot(request.path,
                                           width,
                                           height,
                                           mapped,
                                           static_cast<uint32>(rowPitchBytes),
                                           format,
                                           false);
    readbackBuffer->Unmap();
    return m_stats.succeeded;
}

bool EditorScreenshotService::Fail(std::string reason)
{
    m_stats.failureReason = std::move(reason);
    RVX_CORE_ERROR("{}", m_stats.failureReason);
    return false;
}

} // namespace RVX::Editor
