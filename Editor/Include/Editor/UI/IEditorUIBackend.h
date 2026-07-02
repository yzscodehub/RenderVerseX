/**
 * @file IEditorUIBackend.h
 * @brief Replaceable editor UI backend contract.
 */

#pragma once

#include "Core/Types.h"
#include "Editor/UI/EditorUIAutomation.h"
#include "Editor/UI/EditorUIBackendTypes.h"

#include <memory>
#include <string>

namespace RVX::UI
{
    struct UIInputState;
    class UIRenderer;
}

namespace RVX::Editor
{

struct EditorFilePickerDialogDesc;
struct EditorModalDialogDesc;
class EditorCommandRegistry;
class EditorCommandSurfaceModel;
class EditorUIHost;
class IEditorUIPanel;
enum class EditorUIPanelRebuildReason : uint32;

/**
 * @brief Runtime contract for editor UI backends.
 */
class IEditorUIBackend
{
public:
    virtual ~IEditorUIBackend() = default;

    // =========================================================================
    // Lifecycle
    // =========================================================================

    virtual void Shutdown() = 0;

    // =========================================================================
    // Frame
    // =========================================================================

    virtual bool BeginFrame(const EditorUIFrameDesc& desc) = 0;
    virtual void Update(float deltaTime) = 0;
    virtual void BuildPanels() = 0;
    virtual bool Render(UI::UIRenderer& renderer) = 0;
    virtual void EndFrame() = 0;

    // =========================================================================
    // Accessors
    // =========================================================================

    virtual bool IsInitialized() const = 0;
    virtual EditorUIHost* GetHost() = 0;
    virtual const EditorUIHost* GetHost() const = 0;
    virtual const UI::UIInputState* GetInputState() const = 0;
    virtual bool WantsMouseCapture() const = 0;
    virtual bool WantsKeyboardCapture() const = 0;
    virtual const EditorUICursorRequest& GetCursorRequest() const = 0;

    // =========================================================================
    // Command and Panel Surface
    // =========================================================================

    virtual EditorCommandRegistry* GetCommandRegistry() = 0;
    virtual const EditorCommandRegistry* GetCommandRegistry() const = 0;
    virtual EditorCommandSurfaceModel* GetCommandSurfaceModel() = 0;
    virtual const EditorCommandSurfaceModel* GetCommandSurfaceModel() const = 0;
    virtual const EditorUINativeRenderStats* GetNativeRenderStats() const = 0;
    virtual void ResetLayout() = 0;
    virtual void OpenCommandPalette(std::string initialFilter = {}) = 0;
    virtual bool ExecuteCommand(const std::string& id) = 0;
    virtual void RegisterPanel(std::shared_ptr<IEditorUIPanel> panel) = 0;
    virtual IEditorUIPanel* GetPanel(const std::string& id) const = 0;
    virtual bool IsPanelVisible(const std::string& id) const = 0;
    virtual void SetPanelVisible(const std::string& id, bool visible) = 0;
    virtual bool RequestPanelRebuild(
        const std::string& id,
        EditorUIPanelRebuildReason reason) = 0;

    // =========================================================================
    // Dialogs and Automation
    // =========================================================================

    virtual bool OpenModalDialog(EditorModalDialogDesc desc) = 0;
    virtual void CloseModalDialog() = 0;
    virtual bool IsModalDialogOpen(const std::string& id = {}) const = 0;
    virtual bool OpenFilePickerDialog(EditorFilePickerDialogDesc desc) = 0;
    virtual void CloseFilePickerDialog() = 0;
    virtual bool IsFilePickerDialogOpen(const std::string& id = {}) const = 0;
    virtual EditorUIAutomationResult ClickWidget(const std::string& widgetName) = 0;
    virtual EditorUIAutomationResult ReplaceTextInput(
        const std::string& widgetName,
        const std::string& text) = 0;
};

} // namespace RVX::Editor
