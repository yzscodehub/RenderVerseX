/**
 * @file NativeEditorUIBackend.cpp
 * @brief Native editor UI backend implementation
 */

#include "Editor/UI/NativeEditorUIBackend.h"

#include "Editor/UI/EditorUIAutomation.h"
#include "UI/UIRenderer.h"

#include <utility>

namespace RVX::Editor
{

NativeEditorUIBackend::~NativeEditorUIBackend()
{
    Shutdown();
}

bool NativeEditorUIBackend::Initialize(const NativeEditorUIBackendDesc& desc)
{
    Shutdown();
    m_host = std::make_unique<EditorUIHost>();
    if (!m_host->Initialize(desc.host))
    {
        m_host.reset();
        return false;
    }
    return true;
}

void NativeEditorUIBackend::Shutdown()
{
    if (m_host)
    {
        m_host->Shutdown();
        m_host.reset();
    }
}

bool NativeEditorUIBackend::BeginFrame(const EditorUIFrameDesc& desc)
{
    return m_host && m_host->BeginFrame(desc);
}

void NativeEditorUIBackend::Update(float deltaTime)
{
    if (m_host)
    {
        m_host->Update(deltaTime);
    }
}

void NativeEditorUIBackend::BuildPanels()
{
    if (m_host)
    {
        m_host->BuildPanels();
    }
}

bool NativeEditorUIBackend::Render(UI::UIRenderer& renderer)
{
    return m_host && m_host->RenderNativeUI(renderer);
}

void NativeEditorUIBackend::EndFrame()
{
    if (m_host)
    {
        m_host->EndFrame();
    }
}

bool NativeEditorUIBackend::IsInitialized() const
{
    return m_host && m_host->IsInitialized();
}

EditorUIHost* NativeEditorUIBackend::GetHost()
{
    return m_host.get();
}

const EditorUIHost* NativeEditorUIBackend::GetHost() const
{
    return m_host.get();
}

const UI::UIInputState* NativeEditorUIBackend::GetInputState() const
{
    return m_host ? &m_host->GetUIContext().GetInput() : nullptr;
}

bool NativeEditorUIBackend::WantsMouseCapture() const
{
    return m_host && m_host->WantsMouseCapture();
}

bool NativeEditorUIBackend::WantsKeyboardCapture() const
{
    return m_host && m_host->WantsKeyboardCapture();
}

const EditorUICursorRequest& NativeEditorUIBackend::GetCursorRequest() const
{
    return m_host ? m_host->GetCursorRequest() : m_emptyCursorRequest;
}

EditorCommandRegistry* NativeEditorUIBackend::GetCommandRegistry()
{
    return m_host ? &m_host->GetCommandRegistry() : nullptr;
}

const EditorCommandRegistry* NativeEditorUIBackend::GetCommandRegistry() const
{
    return m_host ? &m_host->GetCommandRegistry() : nullptr;
}

EditorCommandSurfaceModel* NativeEditorUIBackend::GetCommandSurfaceModel()
{
    return m_host ? &m_host->GetCommandSurfaceModel() : nullptr;
}

const EditorCommandSurfaceModel* NativeEditorUIBackend::GetCommandSurfaceModel() const
{
    return m_host ? &m_host->GetCommandSurfaceModel() : nullptr;
}

const EditorUINativeRenderStats* NativeEditorUIBackend::GetNativeRenderStats() const
{
    return m_host ? &m_host->GetNativeRenderStats() : nullptr;
}

void NativeEditorUIBackend::ResetLayout()
{
    if (m_host)
    {
        m_host->ResetLayout();
    }
}

void NativeEditorUIBackend::OpenCommandPalette(std::string initialFilter)
{
    if (m_host)
    {
        m_host->OpenCommandPalette(std::move(initialFilter));
    }
}

bool NativeEditorUIBackend::ExecuteCommand(const std::string& id)
{
    return m_host && m_host->ExecuteCommand(id);
}

void NativeEditorUIBackend::RegisterPanel(std::shared_ptr<IEditorUIPanel> panel)
{
    if (m_host && panel)
    {
        m_host->RegisterPanel(std::move(panel));
    }
}

IEditorUIPanel* NativeEditorUIBackend::GetPanel(const std::string& id) const
{
    return m_host ? m_host->GetPanel(id) : nullptr;
}

bool NativeEditorUIBackend::IsPanelVisible(const std::string& id) const
{
    return m_host && m_host->IsPanelVisible(id);
}

void NativeEditorUIBackend::SetPanelVisible(const std::string& id, bool visible)
{
    if (m_host)
    {
        m_host->SetPanelVisible(id, visible);
    }
}

bool NativeEditorUIBackend::RequestPanelRebuild(
    const std::string& id,
    EditorUIPanelRebuildReason reason)
{
    return m_host && m_host->RequestPanelRebuild(id, reason);
}

bool NativeEditorUIBackend::OpenModalDialog(EditorModalDialogDesc desc)
{
    if (!m_host)
    {
        return false;
    }

    m_host->OpenModalDialog(std::move(desc));
    return true;
}

void NativeEditorUIBackend::CloseModalDialog()
{
    if (m_host)
    {
        m_host->CloseModalDialog();
    }
}

bool NativeEditorUIBackend::IsModalDialogOpen(const std::string& id) const
{
    if (!m_host || !m_host->GetModalDialog().IsOpen())
    {
        return false;
    }
    return id.empty() || m_host->GetModalDialog().GetOpenDialogId() == id;
}

bool NativeEditorUIBackend::OpenFilePickerDialog(EditorFilePickerDialogDesc desc)
{
    if (!m_host)
    {
        return false;
    }

    m_host->OpenFilePickerDialog(std::move(desc));
    return true;
}

void NativeEditorUIBackend::CloseFilePickerDialog()
{
    if (m_host)
    {
        m_host->CloseFilePickerDialog();
    }
}

bool NativeEditorUIBackend::IsFilePickerDialogOpen(const std::string& id) const
{
    if (!m_host || !m_host->GetFilePickerDialog().IsOpen())
    {
        return false;
    }
    return id.empty() || m_host->GetFilePickerDialog().GetOpenDialogId() == id;
}

EditorUIAutomationResult NativeEditorUIBackend::ClickWidget(
    const std::string& widgetName)
{
    if (!m_host)
    {
        return EditorUIAutomationResult::Failure(
            "Editor UI backend is not initialized");
    }

    EditorUIAutomationDriver automation(*m_host);
    return automation.ClickWidget(widgetName);
}

EditorUIAutomationResult NativeEditorUIBackend::ReplaceTextInput(
    const std::string& widgetName,
    const std::string& text)
{
    if (!m_host)
    {
        return EditorUIAutomationResult::Failure(
            "Editor UI backend is not initialized");
    }

    EditorUIAutomationDriver automation(*m_host);
    return automation.ReplaceTextInput(widgetName, text);
}

} // namespace RVX::Editor
