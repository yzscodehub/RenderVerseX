/**
 * @file DebugImGuiLayer.h
 * @brief Transitional ImGui backend wrapper for legacy editor panels
 */

#pragma once

#include "Core/Types.h"

struct GLFWwindow;

namespace RVX::Editor
{

struct DebugImGuiLayerDesc
{
    bool enableDocking = true;
    bool enableViewports = true;
    bool automatedRun = false;
    const char* glslVersion = "#version 450";
};

class DebugImGuiLayer
{
public:
    bool Initialize(GLFWwindow* window, const DebugImGuiLayerDesc& desc);
    void Shutdown();

    void BeginFrame();
    void RenderMainFramebuffer();

    bool IsInitialized() const { return m_initialized; }
    const DebugImGuiLayerDesc& GetDesc() const { return m_desc; }

private:
    GLFWwindow* m_window = nullptr;
    DebugImGuiLayerDesc m_desc;
    bool m_initialized = false;
};

} // namespace RVX::Editor
