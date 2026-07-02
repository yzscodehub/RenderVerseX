/**
 * @file NativeConsole.h
 * @brief Native UI console panel
 */

#pragma once

#include "Editor/Panels/Console.h"
#include "Editor/UI/EditorUIPanel.h"

#include <vector>

namespace RVX::Editor
{

class NativeConsolePanel final : public IEditorUIPanel
{
public:
    NativeConsolePanel();

    const EditorUIPanelDesc& GetPanelDesc() const override { return m_desc; }
    void BuildUI(EditorUIPanelFrameContext& context) override;

private:
    bool ShouldShow(ConsoleLogLevel level) const;
    void AddToolbar(EditorUIPanelFrameContext& context,
                    uint32 infoCount,
                    uint32 warningCount,
                    uint32 errorCount);
    void AddMessageRows(EditorUIPanelFrameContext& context,
                        const std::vector<ConsoleMessage>& messages);

    EditorUIPanelDesc m_desc;
    bool m_showInfo = true;
    bool m_showWarnings = true;
    bool m_showErrors = true;
    bool m_showDebug = false;
    bool m_showTrace = false;
};

} // namespace RVX::Editor
