/**
 * @file Viewport.cpp
 * @brief Viewport panel implementation
 */

#include "Editor/Panels/Viewport.h"
#include "Core/Camera/Camera.h"
#include "Core/Log.h"
#include "Core/Math/Ray.h"
#include "Editor/EditorContext.h"
#include "RHI/RHICommandContext.h"
#include "RHI/RHIDevice.h"
#include "Scene/SceneEntity.h"
#include "Scene/SceneManager.h"
#include "UI/UIRenderer.h"

#if RVX_EDITOR_ENABLE_LEGACY_IMGUI
#include <imgui.h>
#include <ImGuizmo.h>
#endif
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>
#include <algorithm>
#include <cmath>

namespace RVX::Editor
{
namespace
{
    void RecordRuntimeUIOverlay(UI::UIRenderer& renderer, uint32 width, uint32 height)
    {
        if (width < 48u || height < 32u)
        {
            return;
        }

        const float targetWidth = static_cast<float>(width);
        const float panelWidth = std::min(std::clamp(targetWidth * 0.24f, 132.0f, 220.0f),
                                          std::max(28.0f, targetWidth - 20.0f));
        const float panelX = std::max(10.0f, targetWidth - panelWidth - 12.0f);
        const float panelY = 12.0f;
        const float panelHeight = height >= 84u ? 54.0f : std::max(20.0f, static_cast<float>(height) - 20.0f);

        renderer.DrawRect(UI::Rect(panelX, panelY, panelWidth, panelHeight),
                          UI::UIColor(0.025f, 0.03f, 0.04f, 0.82f));
        renderer.DrawRect(UI::Rect(panelX, panelY, 4.0f, panelHeight),
                          UI::UIColor(0.18f, 0.55f, 0.95f, 1.0f));

        if (panelHeight >= 42.0f && panelWidth >= 96.0f)
        {
            renderer.DrawText("RHI UI",
                              UI::Rect(panelX + 14.0f, panelY + 8.0f, panelWidth - 22.0f, 18.0f),
                              14.0f,
                              UI::UIColor(0.92f, 0.95f, 1.0f, 1.0f));
            renderer.DrawText("Viewport",
                              UI::Rect(panelX + 14.0f, panelY + 28.0f, panelWidth - 22.0f, 16.0f),
                              12.0f,
                              UI::UIColor(0.66f, 0.73f, 0.80f, 1.0f));
        }
    }
}

ViewportPanel::ViewportPanel()
{
    ResetCamera();
}

void ViewportPanel::OnInit()
{
    // TODO: Create render target
}

void ViewportPanel::OnUpdate(float deltaTime)
{
    if (m_isHovered || m_isNavigating)
    {
        UpdateCamera(deltaTime);
    }
}

void ViewportPanel::SetRenderDevice(IRHIDevice* device)
{
    if (m_renderDevice == device)
    {
        return;
    }

    m_renderDevice = device;
    ReleaseRenderTargets();
    ClearRenderGraphDiagnostics();
}

bool ViewportPanel::EnsureRenderTargetSize(uint32 width, uint32 height)
{
    if (!m_renderDevice)
    {
        ReleaseRenderTargets();
        m_renderTargetState.hasDevice = false;
        m_renderTargetState.fallbackReason = "RHI device not bound";
        return false;
    }

    if (width == 0 || height == 0)
    {
        ReleaseRenderTargets();
        m_renderTargetState.hasDevice = true;
        m_renderTargetState.fallbackReason = "Viewport size is zero";
        return false;
    }

    if (IsRHIRenderTargetReady() &&
        m_renderTargetState.width == width &&
        m_renderTargetState.height == height)
    {
        return true;
    }

    ReleaseRenderTargets();

    RHITextureDesc colorDesc = RHITextureDesc::RenderTarget(width, height, RHIFormat::RGBA8_UNORM);
    colorDesc.debugName = "EditorViewportColor";
    m_renderTarget = m_renderDevice->CreateTexture(colorDesc);
    if (!m_renderTarget)
    {
        m_renderTargetState.hasDevice = true;
        m_renderTargetState.fallbackReason = "Failed to create viewport color target";
        return false;
    }

    RHITextureViewDesc colorTargetViewDesc;
    colorTargetViewDesc.type = RHITextureViewType::RenderTarget;
    colorTargetViewDesc.debugName = "EditorViewportColorRTV";
    m_renderTargetView =
        m_renderDevice->CreateTextureView(m_renderTarget.Get(), colorTargetViewDesc);

    RHITextureViewDesc colorSrvDesc;
    colorSrvDesc.type = RHITextureViewType::ShaderResource;
    colorSrvDesc.debugName = "EditorViewportColorSRV";
    m_renderShaderResourceView =
        m_renderDevice->CreateTextureView(m_renderTarget.Get(), colorSrvDesc);

    RHITextureDesc depthDesc = RHITextureDesc::DepthStencil(width, height, RHIFormat::D32_FLOAT);
    depthDesc.debugName = "EditorViewportDepth";
    m_depthTarget = m_renderDevice->CreateTexture(depthDesc);
    if (!m_renderTargetView || !m_renderShaderResourceView || !m_depthTarget)
    {
        ReleaseRenderTargets();
        m_renderTargetState.hasDevice = true;
        m_renderTargetState.fallbackReason = "Failed to create viewport color/depth views";
        return false;
    }

    RHITextureViewDesc depthViewDesc;
    depthViewDesc.type = RHITextureViewType::DepthStencil;
    depthViewDesc.subresourceRange.aspect = RHITextureAspect::Depth;
    depthViewDesc.debugName = "EditorViewportDepthDSV";
    m_depthTargetView = m_renderDevice->CreateTextureView(m_depthTarget.Get(), depthViewDesc);
    if (!m_depthTargetView)
    {
        ReleaseRenderTargets();
        m_renderTargetState.hasDevice = true;
        m_renderTargetState.fallbackReason = "Failed to create viewport depth view";
        return false;
    }

    m_renderTargetState.backend = ViewportRenderBackend::RHI;
    m_renderTargetState.hasDevice = true;
    m_renderTargetState.colorTargetReady = true;
    m_renderTargetState.depthTargetReady = true;
    m_renderTargetState.width = width;
    m_renderTargetState.height = height;
    m_renderTargetState.colorFormat = colorDesc.format;
    m_renderTargetState.depthFormat = depthDesc.format;
    m_renderTargetState.colorState = RHIResourceState::Undefined;
    m_renderTargetState.depthState = RHIResourceState::Undefined;
    m_renderTargetState.fallbackReason.clear();
    m_renderTargetState.displayFallbackReason = "Viewport render target has not been rendered yet";
    return true;
}

bool ViewportPanel::RenderRHI(RHICommandContext& commandContext)
{
    if (!IsRHIRenderTargetReady())
    {
        m_renderTargetState.lastRenderRecorded = false;
        return false;
    }

    if (m_renderTargetState.colorState != RHIResourceState::RenderTarget)
    {
        commandContext.TextureBarrier(m_renderTarget.Get(),
                                      m_renderTargetState.colorState,
                                      RHIResourceState::RenderTarget);
        m_renderTargetState.colorState = RHIResourceState::RenderTarget;
    }

    if (m_renderTargetState.depthState != RHIResourceState::DepthWrite)
    {
        commandContext.TextureBarrier(m_depthTarget.Get(),
                                      m_renderTargetState.depthState,
                                      RHIResourceState::DepthWrite);
        m_renderTargetState.depthState = RHIResourceState::DepthWrite;
    }

    RHIRenderPassDesc renderPassDesc;
    renderPassDesc
        .AddColorAttachment(m_renderTargetView.Get(),
                            RHILoadOp::Clear,
                            RHIStoreOp::Store,
                            RHIClearColor{0.055f, 0.065f, 0.08f, 1.0f})
        .SetDepthStencil(m_depthTargetView.Get(),
                         RHILoadOp::Clear,
                         RHIStoreOp::Store,
                         1.0f,
                         0)
        .SetRenderArea(0, 0, m_renderTargetState.width, m_renderTargetState.height);

    commandContext.BeginEvent("Editor Viewport", 0xFF2A3340);
    commandContext.BeginRenderPass(renderPassDesc);
    commandContext.SetViewport(RHIViewport{
        0.0f,
        0.0f,
        static_cast<float>(m_renderTargetState.width),
        static_cast<float>(m_renderTargetState.height),
        0.0f,
        1.0f});
    commandContext.SetScissor(RHIRect{0,
                                      0,
                                      m_renderTargetState.width,
                                      m_renderTargetState.height});
    commandContext.EndRenderPass();
    commandContext.EndEvent();

    commandContext.TextureBarrier(m_renderTarget.Get(),
                                  RHIResourceState::RenderTarget,
                                  RHIResourceState::ShaderResource);
    m_renderTargetState.colorState = RHIResourceState::ShaderResource;
    m_renderTargetState.lastRenderRecorded = true;
    m_renderTargetState.fallbackReason.clear();
    ++m_renderTargetState.frameCount;
    return true;
}

bool ViewportPanel::RenderRuntimeUIOverlay(RHICommandContext& commandContext,
                                           UI::UIRenderer& renderer,
                                           RHIPipeline* pipeline,
                                           RHIDescriptorSetLayout* textureSetLayout)
{
    m_renderTargetState.runtimeUIOverlayRecorded = false;
    m_renderTargetState.runtimeUIOverlayDrawCallCount = 0;
    m_renderTargetState.runtimeUIOverlayFallbackReason.clear();

    if (!m_runtimeUIOverlayEnabled)
    {
        m_renderTargetState.runtimeUIOverlayFallbackReason =
            "Runtime UI overlay disabled";
        return false;
    }

    if (!IsRHIRenderTargetReady())
    {
        m_renderTargetState.runtimeUIOverlayFallbackReason = "Viewport RHI target unavailable";
        return false;
    }
    if (!m_renderTargetState.lastRenderRecorded)
    {
        m_renderTargetState.runtimeUIOverlayFallbackReason = "Viewport target has no base render";
        return false;
    }
    if (!renderer.IsInitialized())
    {
        m_renderTargetState.runtimeUIOverlayFallbackReason = "Runtime UI renderer unavailable";
        return false;
    }
    if (!pipeline)
    {
        m_renderTargetState.runtimeUIOverlayFallbackReason = "Runtime UI pipeline unavailable";
        return false;
    }
    if (!textureSetLayout)
    {
        m_renderTargetState.runtimeUIOverlayFallbackReason = "Runtime UI texture layout unavailable";
        return false;
    }

    if (m_renderTargetState.colorState != RHIResourceState::RenderTarget)
    {
        commandContext.TextureBarrier(m_renderTarget.Get(),
                                      m_renderTargetState.colorState,
                                      RHIResourceState::RenderTarget);
        m_renderTargetState.colorState = RHIResourceState::RenderTarget;
    }

    UI::UIRenderTargetDesc target;
    target.width = m_renderTargetState.width;
    target.height = m_renderTargetState.height;
    target.format = m_renderTargetState.colorFormat;
    target.colorTarget = m_renderTargetView.Get();

    commandContext.BeginEvent("Editor Viewport Runtime UI", 0xFF3B6EA8);
    renderer.BeginFrame(target);
    RecordRuntimeUIOverlay(renderer, m_renderTargetState.width, m_renderTargetState.height);
    renderer.EndFrame();

    UI::UIRenderSubmitDesc submitDesc;
    submitDesc.commandContext = &commandContext;
    submitDesc.pipeline = pipeline;
    submitDesc.textureSetLayout = textureSetLayout;
    submitDesc.colorLoadOp = RHILoadOp::Load;
    submitDesc.colorStoreOp = RHIStoreOp::Store;
    const bool submitted = renderer.Submit(submitDesc);
    commandContext.EndEvent();

    commandContext.TextureBarrier(m_renderTarget.Get(),
                                  RHIResourceState::RenderTarget,
                                  RHIResourceState::ShaderResource);
    m_renderTargetState.colorState = RHIResourceState::ShaderResource;

    if (!submitted)
    {
        m_renderTargetState.runtimeUIOverlayFallbackReason = "Runtime UI submission failed";
        return false;
    }

    const UI::UIRenderSubmitStats& submitStats = renderer.GetSubmitStats();
    m_renderTargetState.runtimeUIOverlayRecorded = true;
    m_renderTargetState.runtimeUIOverlayDrawCallCount = submitStats.drawCallCount;
    ++m_renderTargetState.runtimeUIOverlayFrameCount;
    return true;
}

void ViewportPanel::MarkSceneRenderResult(bool recorded,
                                          RHIResourceState colorState,
                                          RHIResourceState depthState,
                                          const std::string& fallbackReason)
{
    m_renderTargetState.lastRenderRecorded = recorded;
    if (recorded)
    {
        m_renderTargetState.colorState = colorState;
        m_renderTargetState.depthState = depthState;
        m_renderTargetState.fallbackReason.clear();
        ++m_renderTargetState.frameCount;
        return;
    }

    m_renderTargetState.colorState = colorState;
    m_renderTargetState.depthState = depthState;
    if (!fallbackReason.empty())
    {
        m_renderTargetState.fallbackReason = fallbackReason;
    }
}

void ViewportPanel::SetRenderGraphDiagnostics(
    const ViewportRenderGraphDiagnostics& diagnostics)
{
    m_renderTargetState.renderGraphDiagnostics = diagnostics;
}

void ViewportPanel::ClearRenderGraphDiagnostics()
{
    m_renderTargetState.renderGraphDiagnostics = {};
}

void ViewportPanel::ReleaseRenderTargets()
{
    const ViewportRenderGraphDiagnostics renderGraphDiagnostics =
        m_renderTargetState.renderGraphDiagnostics;

    m_depthTargetView.Reset();
    m_depthTarget.Reset();
    m_renderShaderResourceView.Reset();
    m_renderTargetView.Reset();
    m_renderTarget.Reset();

    m_renderTargetState = {};
    m_renderTargetState.renderGraphDiagnostics = renderGraphDiagnostics;
    m_renderTargetState.hasDevice = m_renderDevice != nullptr;
    m_renderTargetState.fallbackReason =
        m_renderDevice ? "RHI render target not allocated" : "RHI device not bound";
}

bool ViewportPanel::IsRHIRenderTargetReady() const
{
    return m_renderTargetState.backend == ViewportRenderBackend::RHI &&
           m_renderTargetState.colorTargetReady &&
           m_renderTargetState.depthTargetReady &&
           m_renderTarget &&
           m_renderTargetView &&
           m_renderShaderResourceView &&
           m_depthTarget &&
           m_depthTargetView;
}

uint64 ViewportPanel::GetNativeDisplayTextureId() const
{
    if (!IsRHIRenderTargetReady() ||
        !m_renderTargetState.lastRenderRecorded ||
        m_renderTargetState.colorState != RHIResourceState::ShaderResource ||
        !m_renderShaderResourceView)
    {
        return 0;
    }

    return m_renderShaderResourceView->GetNativeShaderResourceHandleForUI();
}

ViewportNativeContentState ViewportPanel::PrepareNativeContent(const UI::Rect& bounds)
{
    ViewportNativeContentState state;
    state.bounds = bounds;

    if (bounds.width < 1.0f || bounds.height < 1.0f)
    {
        m_viewportBounds = {};
        m_viewportWidth = 0;
        m_viewportHeight = 0;
        m_renderTargetState.displayImageReady = false;
        m_renderTargetState.displayTextureId = 0;
        m_renderTargetState.displayFallbackReason = "Viewport content region is too small";
        state.fallbackReason = m_renderTargetState.displayFallbackReason;
        return state;
    }

    m_viewportBounds = bounds;
    m_viewportPos = Vec2(bounds.x, bounds.y);
    m_viewportWidth = static_cast<uint32>(std::max(1.0f, bounds.width));
    m_viewportHeight = static_cast<uint32>(std::max(1.0f, bounds.height));

    state.sizeValid = true;
    state.rhiReady = EnsureRenderTargetSize(m_viewportWidth, m_viewportHeight);
    state.renderReadyForDisplay =
        state.rhiReady &&
        m_renderTargetState.lastRenderRecorded &&
        m_renderTargetState.colorState == RHIResourceState::ShaderResource;
    state.textureView = state.renderReadyForDisplay ? m_renderShaderResourceView.Get() : nullptr;
    state.displayImageReady = state.textureView != nullptr;

    m_renderTargetState.displayTextureId = GetNativeDisplayTextureId();
    m_renderTargetState.displayImageReady = state.displayImageReady;

    if (state.displayImageReady)
    {
        m_renderTargetState.displayFallbackReason.clear();
        return state;
    }

    if (!state.rhiReady)
    {
        m_renderTargetState.displayFallbackReason = m_renderTargetState.fallbackReason;
    }
    else if (!state.renderReadyForDisplay)
    {
        m_renderTargetState.displayFallbackReason =
            "Viewport render target has not been rendered yet";
    }
    else
    {
        m_renderTargetState.displayFallbackReason =
            "Viewport shader resource view unavailable for native UI";
    }
    state.fallbackReason = m_renderTargetState.displayFallbackReason;
    return state;
}

void ViewportPanel::OnGUI()
{
#if RVX_EDITOR_ENABLE_LEGACY_IMGUI
    m_viewportBounds = {};

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(960.0f, 540.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(360.0f, 260.0f),
                                        ImVec2(100000.0f, 100000.0f));

    if (ImGui::Begin(GetName(), nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
    {
        DrawToolbar();

        // Get viewport content region
        ImVec2 viewportPanelSize = ImGui::GetContentRegionAvail();
        viewportPanelSize.x = std::max(1.0f, viewportPanelSize.x);
        viewportPanelSize.y = std::max(1.0f, viewportPanelSize.y);
        m_viewportWidth = static_cast<uint32>(viewportPanelSize.x);
        m_viewportHeight = static_cast<uint32>(viewportPanelSize.y);

        ImVec2 viewportPos = ImGui::GetCursorScreenPos();
        m_viewportPos.x = viewportPos.x;
        m_viewportPos.y = viewportPos.y;

        DrawViewportContent();

        if (m_showGizmo)
        {
            DrawGizmo();
        }
        else
        {
            CommitGizmoUndo();
        }

        DrawOverlays();
    }
    ImGui::End();
    ImGui::PopStyleVar();
#else
    m_viewportBounds = {};
#endif
}

void ViewportPanel::OnNativeInput(const UI::UIInputState& input)
{
    m_nativeInputState = input;
    UpdateNativePointerScope(input);

    const bool mouseInsideViewport = m_viewportBounds.width > 0.0f &&
                                     m_viewportBounds.height > 0.0f &&
                                     m_viewportBounds.Contains(input.current.mousePosition);
    m_rightMouseDown = input.current.IsMouseButtonDown(UI::UIMouseButton::Right);
    m_middleMouseDown = input.current.IsMouseButtonDown(UI::UIMouseButton::Middle);

    if (mouseInsideViewport &&
        !input.current.wantsKeyboardCapture &&
        input.WasKeyPressed('F'))
    {
        FocusOnSelection();
    }

    bool legacyGizmoUsing = false;
#if RVX_EDITOR_ENABLE_LEGACY_IMGUI
    legacyGizmoUsing = ImGuizmo::IsUsing();
#endif
    if (mouseInsideViewport &&
        input.WasMouseButtonPressed(UI::UIMouseButton::Left) &&
        !legacyGizmoUsing)
    {
        HandleMousePick();
    }
}

void ViewportPanel::UpdateNativePointerScope(const UI::UIInputState& input)
{
    const bool hasViewportBounds = m_viewportBounds.width > 0.0f &&
                                   m_viewportBounds.height > 0.0f;
    const bool mouseInsideViewport =
        hasViewportBounds && m_viewportBounds.Contains(input.current.mousePosition);
    const bool navigationButtonDown =
        input.current.IsMouseButtonDown(UI::UIMouseButton::Right) ||
        input.current.IsMouseButtonDown(UI::UIMouseButton::Middle);
    const bool navigationPressedInside =
        mouseInsideViewport &&
        (input.WasMouseButtonPressed(UI::UIMouseButton::Right) ||
         input.WasMouseButtonPressed(UI::UIMouseButton::Middle));

    if (navigationPressedInside)
    {
        m_isNavigating = true;
        m_isFocused = true;
    }
    else if (!navigationButtonDown)
    {
        m_isNavigating = false;
    }

    if (mouseInsideViewport && input.WasMouseButtonPressed(UI::UIMouseButton::Left))
    {
        m_isFocused = true;
    }
    else if (!mouseInsideViewport && !navigationButtonDown)
    {
        m_isFocused = false;
    }

    m_isHovered = mouseInsideViewport || (m_isNavigating && navigationButtonDown);
}

void ViewportPanel::UpdateCamera(float deltaTime)
{
    switch (m_cameraMode)
    {
        case ViewportCameraMode::Orbit:
            UpdateOrbitCamera(deltaTime);
            break;
        case ViewportCameraMode::Fly:
            UpdateFlyCamera(deltaTime);
            break;
        case ViewportCameraMode::TopDown:
            // Top-down mode uses orbit logic but locks pitch
            UpdateOrbitCamera(deltaTime);
            break;
    }
}

void ViewportPanel::UpdateOrbitCamera(float deltaTime)
{
    (void)deltaTime;
    const UI::UIInputState& input = m_nativeInputState;
    EditorViewportCameraControllerState state = CaptureCameraControllerState();
    EditorViewportCameraControllerInput controllerInput;
    controllerInput.mouseDelta = input.mouseDelta;
    controllerInput.scrollDelta =
        m_isHovered ? input.current.scrollDelta : Vec2(0.0f);
    controllerInput.rotate = m_rightMouseDown && m_isHovered;
    controllerInput.pan = m_middleMouseDown && m_isHovered;

    m_cameraController.ApplyOrbit(state, controllerInput);
    ApplyCameraControllerState(state);
}

void ViewportPanel::UpdateFlyCamera(float deltaTime)
{
    const UI::UIInputState& input = m_nativeInputState;
    const bool navigates = m_rightMouseDown && m_isHovered;

    EditorViewportCameraControllerState state = CaptureCameraControllerState();
    EditorViewportCameraControllerInput controllerInput;
    controllerInput.mouseDelta = input.mouseDelta;
    controllerInput.deltaTime = deltaTime;
    controllerInput.rotate = navigates;
    controllerInput.boost =
        (input.current.modifiers & UI::ToMask(UI::UIInputModifier::Shift)) != 0u;
    controllerInput.moveForward = navigates && input.current.IsKeyDown('W');
    controllerInput.moveBackward = navigates && input.current.IsKeyDown('S');
    controllerInput.moveLeft = navigates && input.current.IsKeyDown('A');
    controllerInput.moveRight = navigates && input.current.IsKeyDown('D');
    controllerInput.moveDown = navigates && input.current.IsKeyDown('Q');
    controllerInput.moveUp = navigates && input.current.IsKeyDown('E');

    m_cameraController.ApplyFly(state, controllerInput);
    ApplyCameraControllerState(state);
}

EditorViewportCameraControllerState
ViewportPanel::CaptureCameraControllerState() const
{
    EditorViewportCameraControllerState state;
    state.position = m_cameraPosition;
    state.target = m_cameraTarget;
    state.up = m_cameraUp;
    state.yaw = m_cameraYaw;
    state.pitch = m_cameraPitch;
    state.distance = m_cameraDistance;
    state.moveSpeed = m_cameraMoveSpeed;
    state.rotateSpeed = m_cameraRotateSpeed;
    return state;
}

void ViewportPanel::ApplyCameraControllerState(
    const EditorViewportCameraControllerState& state)
{
    m_cameraPosition = state.position;
    m_cameraTarget = state.target;
    m_cameraUp = state.up;
    m_cameraYaw = state.yaw;
    m_cameraPitch = state.pitch;
    m_cameraDistance = state.distance;
}

void ViewportPanel::DrawToolbar()
{
#if RVX_EDITOR_ENABLE_LEGACY_IMGUI
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 4));
    ImGui::BeginChild("ViewportToolbar", ImVec2(0, 32), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    auto& context = EditorContext::Get();

    // Gizmo mode buttons
    bool isTranslate = context.GetGizmoMode() == EditorContext::GizmoMode::Translate;
    bool isRotate = context.GetGizmoMode() == EditorContext::GizmoMode::Rotate;
    bool isScale = context.GetGizmoMode() == EditorContext::GizmoMode::Scale;

    if (ImGui::RadioButton("Translate (W)", isTranslate))
        context.SetGizmoMode(EditorContext::GizmoMode::Translate);
    ImGui::SameLine();
    if (ImGui::RadioButton("Rotate (E)", isRotate))
        context.SetGizmoMode(EditorContext::GizmoMode::Rotate);
    ImGui::SameLine();
    if (ImGui::RadioButton("Scale (R)", isScale))
        context.SetGizmoMode(EditorContext::GizmoMode::Scale);

    ImGui::SameLine();
    ImGui::Separator();
    ImGui::SameLine();

    // Gizmo space
    bool isWorld = context.GetGizmoSpace() == EditorContext::GizmoSpace::World;
    if (ImGui::RadioButton("World", isWorld))
        context.SetGizmoSpace(EditorContext::GizmoSpace::World);
    ImGui::SameLine();
    if (ImGui::RadioButton("Local", !isWorld))
        context.SetGizmoSpace(EditorContext::GizmoSpace::Local);

    ImGui::SameLine();
    ImGui::Separator();
    ImGui::SameLine();

    // Snap toggle
    bool snapEnabled = context.IsSnapEnabled();
    if (ImGui::Checkbox("Snap", &snapEnabled))
        context.SetSnapEnabled(snapEnabled);

    if (snapEnabled)
    {
        ImGui::SameLine();
        float snapValue = context.GetSnapValue();
        ImGui::SetNextItemWidth(60);
        if (ImGui::DragFloat("##SnapValue", &snapValue, 0.1f, 0.1f, 100.0f))
            context.SetSnapValue(snapValue);
    }

    ImGui::SameLine();
    ImGui::Separator();
    ImGui::SameLine();

    // Camera mode
    const char* cameraModes[] = { "Orbit", "Fly", "Top" };
    int cameraMode = static_cast<int>(m_cameraMode);
    ImGui::SetNextItemWidth(80);
    if (ImGui::Combo("##CameraMode", &cameraMode, cameraModes, 3))
        m_cameraMode = static_cast<ViewportCameraMode>(cameraMode);

    ImGui::SameLine();

    // Shading mode
    const char* shadingModes[] = { "Lit", "Unlit", "Wireframe", "Normals", "Depth", "Albedo" };
    int shadingMode = static_cast<int>(m_shadingMode);
    ImGui::SetNextItemWidth(90);
    if (ImGui::Combo("##ShadingMode", &shadingMode, shadingModes, 6))
        m_shadingMode = static_cast<ViewportShadingMode>(shadingMode);

    ImGui::SameLine();

    // Display options dropdown
    if (ImGui::BeginCombo("##Options", "Options", ImGuiComboFlags_NoPreview))
    {
        ImGui::Checkbox("Grid", &m_showGrid);
        ImGui::Checkbox("Gizmo", &m_showGizmo);
        ImGui::Checkbox("Stats", &m_showStats);
        ImGui::Checkbox("Bounds", &m_showBounds);
        ImGui::Checkbox("Debug Draw", &m_showDebugDraw);
        ImGui::EndCombo();
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();

    ImGui::Separator();
#endif
}

void ViewportPanel::DrawGizmo()
{
#if RVX_EDITOR_ENABLE_LEGACY_IMGUI
    auto& context = EditorContext::Get();
    SceneEntity* selectedEntity = context.GetSelectedEntity();

    if (!selectedEntity)
    {
        CommitGizmoUndo();
        return;
    }

    // Set up ImGuizmo
    ImGuizmo::SetOrthographic(m_cameraMode == ViewportCameraMode::TopDown);
    ImGuizmo::SetDrawlist();

    ImGuizmo::SetRect(m_viewportPos.x, m_viewportPos.y,
                      static_cast<float>(m_viewportWidth),
                      static_cast<float>(m_viewportHeight));

    // Get matrices
    Mat4 view = GetViewMatrix();
    Mat4 projection = GetProjectionMatrix();

    Mat4 transform = selectedEntity->GetWorldMatrix();

    // Determine gizmo operation
    ImGuizmo::OPERATION operation = ImGuizmo::TRANSLATE;
    switch (context.GetGizmoMode())
    {
        case EditorContext::GizmoMode::Translate: operation = ImGuizmo::TRANSLATE; break;
        case EditorContext::GizmoMode::Rotate: operation = ImGuizmo::ROTATE; break;
        case EditorContext::GizmoMode::Scale: operation = ImGuizmo::SCALE; break;
    }

    // Determine gizmo mode
    ImGuizmo::MODE mode = context.GetGizmoSpace() == EditorContext::GizmoSpace::World
                          ? ImGuizmo::WORLD : ImGuizmo::LOCAL;

    // Snap values
    float snapValues[3] = { context.GetSnapValue(), context.GetSnapValue(), context.GetSnapValue() };
    float* snap = context.IsSnapEnabled() ? snapValues : nullptr;

    // Draw and manipulate gizmo
    const bool manipulated = ImGuizmo::Manipulate(glm::value_ptr(view),
                                                  glm::value_ptr(projection),
                                                  operation,
                                                  mode,
                                                  glm::value_ptr(transform),
                                                  nullptr,
                                                  snap);
    const bool isUsing = ImGuizmo::IsUsing();
    if (isUsing &&
        (!m_gizmoUndoActive || m_gizmoUndoEntityHandle != selectedEntity->GetHandle()))
    {
        CommitGizmoUndo();
        BeginGizmoUndo(selectedEntity);
    }

    if (manipulated && isUsing)
    {
        Mat4 localTransform = transform;
        if (auto* parent = selectedEntity->GetParent())
        {
            localTransform = inverse(parent->GetWorldMatrix()) * transform;
        }

        Vec3 scale(1.0f);
        Quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
        Vec3 translation(0.0f);
        Vec3 skew(0.0f);
        Vec4 perspective(0.0f);
        if (glm::decompose(localTransform, scale, rotation, translation, skew, perspective))
        {
            selectedEntity->SetPosition(translation);
            selectedEntity->SetRotation(normalize(rotation));
            selectedEntity->SetScale(scale);
        }
        context.MarkSceneDirty();
    }

    if (!isUsing)
    {
        CommitGizmoUndo();
    }
#else
    CommitGizmoUndo();
#endif
}

void ViewportPanel::BeginGizmoUndo(SceneEntity* entity)
{
    if (!entity)
    {
        return;
    }

    m_gizmoUndoActive = true;
    m_gizmoUndoEntityHandle = entity->GetHandle();
    m_gizmoUndoStartPosition = entity->GetPosition();
    m_gizmoUndoStartRotation = entity->GetRotation();
    m_gizmoUndoStartScale = entity->GetScale();
}

void ViewportPanel::CommitGizmoUndo()
{
    if (!m_gizmoUndoActive)
    {
        return;
    }

    auto& context = EditorContext::Get();
    auto* sceneManager = context.GetActiveSceneManager();
    SceneEntity* entity = sceneManager ? sceneManager->GetEntity(m_gizmoUndoEntityHandle) : nullptr;
    if (entity)
    {
        context.RecordEntityTransformUndo(entity,
                                          m_gizmoUndoStartPosition,
                                          m_gizmoUndoStartRotation,
                                          m_gizmoUndoStartScale,
                                          entity->GetPosition(),
                                          entity->GetRotation(),
                                          entity->GetScale(),
                                          "Gizmo Transform");
    }

    m_gizmoUndoActive = false;
    m_gizmoUndoEntityHandle = ~0u;
    m_gizmoUndoStartPosition = Vec3(0.0f);
    m_gizmoUndoStartRotation = Quat(1.0f, 0.0f, 0.0f, 0.0f);
    m_gizmoUndoStartScale = Vec3(1.0f);
}

void ViewportPanel::DrawViewportContent()
{
#if RVX_EDITOR_ENABLE_LEGACY_IMGUI
    ImVec2 size = ImGui::GetContentRegionAvail();
    if (size.x < 1.0f || size.y < 1.0f)
    {
        m_viewportBounds = {};
        m_renderTargetState.displayImageReady = false;
        m_renderTargetState.displayTextureId = 0;
        m_renderTargetState.displayFallbackReason = "Viewport content region is too small";
        ImGui::Dummy(ImVec2(std::max(1.0f, size.x), std::max(1.0f, size.y)));
        return;
    }

    ImVec2 pos = ImGui::GetCursorScreenPos();
    m_viewportBounds = UI::Rect(pos.x, pos.y, size.x, size.y);
    const bool rhiReady = EnsureRenderTargetSize(m_viewportWidth, m_viewportHeight);
    const bool renderReadyForDisplay = rhiReady &&
                                       m_renderTargetState.lastRenderRecorded &&
                                       m_renderTargetState.colorState == RHIResourceState::ShaderResource;
    const uint64 displayTextureId = GetNativeDisplayTextureId();

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 max(pos.x + size.x, pos.y + size.y);

    m_renderTargetState.displayTextureId = displayTextureId;
    m_renderTargetState.displayImageReady = displayTextureId != 0;

    if (m_renderTargetState.displayImageReady)
    {
        drawList->AddImage(static_cast<ImTextureID>(displayTextureId),
                           pos,
                           max,
                           ImVec2(0.0f, 1.0f),
                           ImVec2(1.0f, 0.0f));
        m_renderTargetState.displayFallbackReason.clear();
    }
    else
    {
        const ImU32 backgroundColor = rhiReady ? IM_COL32(24, 30, 38, 255)
                                               : IM_COL32(30, 30, 35, 255);
        drawList->AddRectFilled(pos, max, backgroundColor);

        if (!rhiReady)
        {
            m_renderTargetState.displayFallbackReason = m_renderTargetState.fallbackReason;
        }
        else if (!renderReadyForDisplay)
        {
            m_renderTargetState.displayFallbackReason = "Viewport render target has not been rendered yet";
        }
        else
        {
            m_renderTargetState.displayFallbackReason =
                "RHI backend does not expose an ImGui-compatible texture handle";
        }
    }

    if (m_showGrid)
    {
        ImU32 gridColor = IM_COL32(50, 50, 55, 255);
        float gridSpacing = 50.0f;

        for (float x = pos.x; x < pos.x + size.x; x += gridSpacing)
        {
            drawList->AddLine(ImVec2(x, pos.y), ImVec2(x, pos.y + size.y), gridColor);
        }
        for (float y = pos.y; y < pos.y + size.y; y += gridSpacing)
        {
            drawList->AddLine(ImVec2(pos.x, y), ImVec2(pos.x + size.x, y), gridColor);
        }
    }

    ImVec2 center(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f);
    drawList->AddLine(ImVec2(center.x - 20, center.y), ImVec2(center.x + 20, center.y),
                      IM_COL32(100, 100, 100, 255));
    drawList->AddLine(ImVec2(center.x, center.y - 20), ImVec2(center.x, center.y + 20),
                      IM_COL32(100, 100, 100, 255));

    // Make viewport interactive
    ImGui::InvisibleButton("ViewportInteractive", size,
                           ImGuiButtonFlags_MouseButtonLeft |
                           ImGuiButtonFlags_MouseButtonRight |
                           ImGuiButtonFlags_MouseButtonMiddle);
#else
    m_viewportBounds = {};
    m_renderTargetState.displayImageReady = false;
    m_renderTargetState.displayTextureId = 0;
    m_renderTargetState.displayFallbackReason =
        "Legacy ImGui viewport drawing is disabled";
#endif
}

void ViewportPanel::DrawOverlays()
{
#if RVX_EDITOR_ENABLE_LEGACY_IMGUI
    if (!m_showStats)
        return;

    ImVec2 pos = ImVec2(m_viewportPos.x + 10, m_viewportPos.y + 40);
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    // Background for stats
    ImVec2 statsSize(200, 100);
    drawList->AddRectFilled(pos, ImVec2(pos.x + statsSize.x, pos.y + statsSize.y),
                            IM_COL32(0, 0, 0, 150), 4.0f);

    // Camera info
    char buffer[128];
    snprintf(buffer, sizeof(buffer), "Camera: %.1f, %.1f, %.1f",
             m_cameraPosition.x, m_cameraPosition.y, m_cameraPosition.z);
    drawList->AddText(ImVec2(pos.x + 8, pos.y + 8), IM_COL32(200, 200, 200, 255), buffer);

    snprintf(buffer, sizeof(buffer), "Target: %.1f, %.1f, %.1f",
             m_cameraTarget.x, m_cameraTarget.y, m_cameraTarget.z);
    drawList->AddText(ImVec2(pos.x + 8, pos.y + 26), IM_COL32(200, 200, 200, 255), buffer);

    snprintf(buffer, sizeof(buffer), "Distance: %.2f", m_cameraDistance);
    drawList->AddText(ImVec2(pos.x + 8, pos.y + 44), IM_COL32(200, 200, 200, 255), buffer);

    snprintf(buffer, sizeof(buffer), "Viewport: %ux%u", m_viewportWidth, m_viewportHeight);
    drawList->AddText(ImVec2(pos.x + 8, pos.y + 62), IM_COL32(200, 200, 200, 255), buffer);

    const char* modeStr = m_cameraMode == ViewportCameraMode::Orbit ? "Orbit" :
                          m_cameraMode == ViewportCameraMode::Fly ? "Fly" : "TopDown";
    snprintf(buffer, sizeof(buffer), "Mode: %s", modeStr);
    drawList->AddText(ImVec2(pos.x + 8, pos.y + 80), IM_COL32(200, 200, 200, 255), buffer);
#endif
}

void ViewportPanel::HandleMousePick()
{
    auto& context = EditorContext::Get();
    auto* sceneManager = context.GetActiveSceneManager();
    if (!sceneManager)
    {
        return;
    }

    const EditorViewportScreenRay screenRay =
        EditorViewportProjection::BuildScreenRay(CreateCameraFrame(),
                                                 m_nativeInputState.current.mousePosition);
    if (!screenRay.valid || !screenRay.insideViewport)
    {
        return;
    }

    RaycastHit hit;
    if (sceneManager->Raycast(screenRay.ray, hit))
    {
        context.SelectEntity(hit.entity);
    }
    else
    {
        context.ClearSelection();
    }
}

void ViewportPanel::FocusOnSelection()
{
    auto& context = EditorContext::Get();
    SceneEntity* selected = context.GetSelectedEntity();

    if (selected)
    {
        const AABB bounds = selected->GetWorldBounds();
        m_cameraTarget = bounds.IsValid() ? bounds.GetCenter() : selected->GetWorldPosition();
        if (bounds.IsValid())
        {
            m_cameraDistance = glm::clamp(length(bounds.GetExtent()) * 2.5f, 1.0f, 1000.0f);
        }
    }
}

void ViewportPanel::ResetCamera()
{
    m_cameraPosition = Vec3(0.0f, 5.0f, 10.0f);
    m_cameraTarget = Vec3(0.0f, 0.0f, 0.0f);
    m_cameraYaw = -90.0f;
    m_cameraPitch = -15.0f;
    m_cameraDistance = 10.0f;
}

void ViewportPanel::AlignCameraToViewDirection(const Vec3& directionFromTarget,
                                               ViewportCameraMode mode)
{
    EditorViewportCameraControllerState state = CaptureCameraControllerState();
    if (!m_cameraController.AlignToViewDirection(state, directionFromTarget))
    {
        return;
    }

    m_cameraMode = mode;
    ApplyCameraControllerState(state);
}

Camera ViewportPanel::BuildRenderCamera() const
{
    Camera camera;
    const uint32 safeWidth = std::max(1u, m_viewportWidth);
    const uint32 safeHeight = std::max(1u, m_viewportHeight);
    const float aspect = static_cast<float>(safeWidth) / static_cast<float>(safeHeight);

    if (m_cameraMode == ViewportCameraMode::TopDown)
    {
        const float orthoSize = m_cameraDistance;
        camera.SetOrthographic(orthoSize * aspect * 2.0f,
                               orthoSize * 2.0f,
                               m_cameraNear,
                               m_cameraFar);
    }
    else
    {
        camera.SetPerspective(glm::radians(m_cameraFOV), aspect, m_cameraNear, m_cameraFar);
    }

    camera.SetPosition(m_cameraPosition);
    camera.LookAt(m_cameraTarget);
    return camera;
}

EditorViewportCameraFrame ViewportPanel::CreateCameraFrame() const
{
    EditorViewportCameraFrame frame;
    frame.viewportBounds = m_viewportBounds;
    frame.viewMatrix = GetViewMatrix();
    frame.projectionMatrix = GetProjectionMatrix();
    frame.valid = m_viewportBounds.width > 0.0f && m_viewportBounds.height > 0.0f;
    return frame;
}

Mat4 ViewportPanel::GetViewMatrix() const
{
    return glm::lookAt(m_cameraPosition, m_cameraTarget, m_cameraUp);
}

Mat4 ViewportPanel::GetProjectionMatrix() const
{
    float aspect = static_cast<float>(m_viewportWidth) / static_cast<float>(m_viewportHeight);

    if (m_cameraMode == ViewportCameraMode::TopDown)
    {
        float orthoSize = m_cameraDistance;
        return glm::ortho(-orthoSize * aspect, orthoSize * aspect,
                          -orthoSize, orthoSize, m_cameraNear, m_cameraFar);
    }

    return glm::perspective(glm::radians(m_cameraFOV), aspect, m_cameraNear, m_cameraFar);
}

} // namespace RVX::Editor
