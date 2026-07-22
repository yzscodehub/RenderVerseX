#pragma once

/**
 * @file EditorRenderRuntimeAdapter.h
 * @brief Value-only Editor adapter for the dedicated render runtime.
 */

#include "Core/Types.h"
#include "Render/RenderDiagnostics.h"
#include "Render/RenderRuntimeTypes.h"
#include "RenderContracts/RenderFramePacket.h"
#include "RHI/RHINativeSurface.h"

#include <memory>

struct GLFWwindow;

namespace RVX
{
class RenderSubsystem;
} // namespace RVX

namespace RVX::Editor
{

enum class EditorRenderFeature : uint8
{
    FramePublication = 0,
    ValueDiagnostics = 1,
    ViewportRendering = 2,
    NativeUISubmission = 3,
    ScreenshotService = 4
};

enum class EditorRenderFeatureStatus : uint8
{
    Available = 0,
    UnavailableDuringM1ArchitectureCut = 1
};

struct EditorRenderRuntimeAdapterConfig
{
    RHIBackendType backendType = RHIBackendType::Auto;
    bool enableValidation = true;
    bool enableGPUValidation = false;
    bool vsync = true;
    uint32 frameBuffering = 2;
};

struct EditorRenderRuntimeAdapterStartResult
{
    bool started = false;
    RHIBackendType backend = RHIBackendType::None;
    NativeSurfaceDesc surface{};
    RenderRuntimeResult runtime{};

    explicit operator bool() const noexcept { return started; }
};

/**
 * @brief Owns the Editor's dedicated runtime without exposing live Render/RHI objects.
 *
 * M1 intentionally does not adapt Editor viewport or native-UI draw recording. Those
 * features report UnavailableDuringM1ArchitectureCut until they publish immutable
 * render packets through this boundary.
 */
class EditorRenderRuntimeAdapter final : public NonMovable
{
public:
    EditorRenderRuntimeAdapter();
    ~EditorRenderRuntimeAdapter();

    [[nodiscard]] RHIBackendType ResolveDefaultBackend() const noexcept;
    [[nodiscard]] EditorRenderRuntimeAdapterStartResult Start(
        GLFWwindow* window,
        const EditorRenderRuntimeAdapterConfig& config = {});
    [[nodiscard]] RenderFramePublishResult PublishFrame(
        std::unique_ptr<const RenderFramePacket> packet);
    [[nodiscard]] RenderResizeResult RequestResize(uint32 width,
                                                   uint32 height);
    [[nodiscard]] RenderDiagnosticsSnapshot GetDiagnostics() const;
    [[nodiscard]] RenderShutdownResult Shutdown();

    [[nodiscard]] bool IsRunning() const noexcept;
    [[nodiscard]] EditorRenderFeatureStatus GetFeatureStatus(
        EditorRenderFeature feature) const noexcept;

private:
    [[nodiscard]] static NativeSurfaceDesc CaptureSurface(
        GLFWwindow* window,
        RHIBackendType backend,
        bool vsync,
        uint64 generation);

    std::unique_ptr<RenderSubsystem> m_render;
    NativeSurfaceDesc m_surface{};
    RenderRuntimeResult m_lastRuntime{};
    RenderShutdownResult m_lastShutdown{};
};

} // namespace RVX::Editor
