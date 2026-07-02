/**
 * @file NativeEditorUIBackend.h
 * @brief Native editor UI backend ownership and frame lifecycle.
 */

#pragma once

#include "Editor/UI/EditorUIHost.h"
#include "Editor/UI/IEditorUIBackend.h"

#include <memory>

namespace RVX::UI
{
    class UIRenderer;
}

namespace RVX::Editor
{

struct NativeEditorUIBackendDesc
{
    EditorUIHostDesc host;
};

/**
 * @brief Owns the native editor UI host and exposes its frame lifecycle.
 */
class NativeEditorUIBackend final : public IEditorUIBackend
{
public:
    // =========================================================================
    // Lifecycle
    // =========================================================================

    ~NativeEditorUIBackend() override;

    bool Initialize(const NativeEditorUIBackendDesc& desc);
    void Shutdown() override;

    // =========================================================================
    // Frame
    // =========================================================================

    bool BeginFrame(const EditorUIFrameDesc& desc) override;
    void Update(float deltaTime) override;
    void BuildPanels() override;
    bool Render(UI::UIRenderer& renderer) override;
    void EndFrame() override;

    // =========================================================================
    // Accessors
    // =========================================================================

    bool IsInitialized() const override;
    EditorUIHost* GetHost() override;
    const EditorUIHost* GetHost() const override;
    const UI::UIInputState* GetInputState() const override;
    bool WantsMouseCapture() const override;
    bool WantsKeyboardCapture() const override;
    const EditorUICursorRequest& GetCursorRequest() const override;

    // =========================================================================
    // Command and Panel Surface
    // =========================================================================

    EditorCommandRegistry* GetCommandRegistry() override;
    const EditorCommandRegistry* GetCommandRegistry() const override;
    EditorCommandSurfaceModel* GetCommandSurfaceModel() override;
    const EditorCommandSurfaceModel* GetCommandSurfaceModel() const override;
    const EditorUINativeRenderStats* GetNativeRenderStats() const override;
    void ResetLayout() override;
    void OpenCommandPalette(std::string initialFilter = {}) override;
    bool ExecuteCommand(const std::string& id) override;
    void RegisterPanel(std::shared_ptr<IEditorUIPanel> panel) override;
    IEditorUIPanel* GetPanel(const std::string& id) const override;
    bool IsPanelVisible(const std::string& id) const override;
    void SetPanelVisible(const std::string& id, bool visible) override;
    bool RequestPanelRebuild(
        const std::string& id,
        EditorUIPanelRebuildReason reason) override;

    // =========================================================================
    // Dialogs and Automation
    // =========================================================================

    bool OpenModalDialog(EditorModalDialogDesc desc) override;
    void CloseModalDialog() override;
    bool IsModalDialogOpen(const std::string& id = {}) const override;
    bool OpenFilePickerDialog(EditorFilePickerDialogDesc desc) override;
    void CloseFilePickerDialog() override;
    bool IsFilePickerDialogOpen(const std::string& id = {}) const override;
    EditorUIAutomationResult ClickWidget(const std::string& widgetName) override;
    EditorUIAutomationResult ReplaceTextInput(
        const std::string& widgetName,
        const std::string& text) override;

private:
    std::unique_ptr<EditorUIHost> m_host;
    EditorUICursorRequest m_emptyCursorRequest;
};

} // namespace RVX::Editor
