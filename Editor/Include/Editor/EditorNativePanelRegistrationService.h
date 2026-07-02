/**
 * @file EditorNativePanelRegistrationService.h
 * @brief Editor native panel registration and view command service.
 */

#pragma once

#include "Core/Types.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace RVX::Editor
{

class EditorSelectionService;
class EditorSettingsService;
class EditorShortcutProfileService;
class EditorViewportToolService;
class IEditorPanel;
class IEditorUIBackend;
class IEditorUIPanel;
enum class EditorUIPanelRebuildReason : uint32;

struct EditorNativePanelRegistrationStats
{
    bool standardPanelsRegistered = false;
    uint32 nativePanelCount = 0;
    uint32 legacyPanelViewCommandCount = 0;
    uint32 debugViewCommandCount = 0;
};

/**
 * @brief Owns native panel registration and View menu command wiring.
 */
class EditorNativePanelRegistrationService
{
public:
    // =========================================================================
    // Dependencies
    // =========================================================================

    void SetUIBackend(IEditorUIBackend* uiBackend)
    {
        m_uiBackend = uiBackend;
    }

    void SetSelectionService(EditorSelectionService* selection)
    {
        m_selection = selection;
    }

    void SetViewportToolService(EditorViewportToolService* viewportTools)
    {
        m_viewportTools = viewportTools;
    }

    void SetSettingsService(EditorSettingsService* settings)
    {
        m_settings = settings;
    }

    void SetShortcutProfileService(EditorShortcutProfileService* shortcutProfiles)
    {
        m_shortcutProfiles = shortcutProfiles;
    }

    void SetLegacyImGuiDebugExposure(bool exposeDebugCommands)
    {
        m_exposeLegacyImGuiDebugCommands = exposeDebugCommands;
    }

    void SetLegacyImGuiDebugState(bool* showDemoWindow,
                                  bool* showMetricsWindow)
    {
        m_showDemoWindow = showDemoWindow;
        m_showMetricsWindow = showMetricsWindow;
    }

    // =========================================================================
    // Registration
    // =========================================================================

    bool RegisterStandardNativePanels();
    void RegisterLegacyPanelViewCommand(IEditorPanel& panel);
    void RegisterNativePanel(std::shared_ptr<IEditorUIPanel> panel);
    void RegisterDebugViewCommands();

    // =========================================================================
    // Commands and Rebuilds
    // =========================================================================

    void RefreshViewCommandStates(
        const std::vector<std::shared_ptr<IEditorPanel>>& legacyPanels);
    void RequestNativePanelRebuilds(EditorUIPanelRebuildReason reason);
    bool OpenPreferencesPanel();

    // =========================================================================
    // Diagnostics
    // =========================================================================

    const std::vector<std::string>& GetNativePanelIds() const
    {
        return m_nativePanelIds;
    }

    const EditorNativePanelRegistrationStats& GetStats() const
    {
        return m_stats;
    }

    static std::string MakeLegacyPanelViewCommandId(
        std::string_view panelName);

private:
    void AddViewPanelGroupSeparator();
    void AddViewDebugGroupSeparator();
    void RegisterNativePanelViewCommand(const std::string& panelId,
                                        const std::string& title);

    IEditorUIBackend* m_uiBackend = nullptr;
    EditorSelectionService* m_selection = nullptr;
    EditorViewportToolService* m_viewportTools = nullptr;
    EditorSettingsService* m_settings = nullptr;
    EditorShortcutProfileService* m_shortcutProfiles = nullptr;
    bool* m_showDemoWindow = nullptr;
    bool* m_showMetricsWindow = nullptr;
    std::vector<std::string> m_nativePanelIds;
    EditorNativePanelRegistrationStats m_stats;
    bool m_exposeLegacyImGuiDebugCommands = false;
    bool m_viewPanelGroupAdded = false;
    bool m_debugGroupAdded = false;
};

} // namespace RVX::Editor
