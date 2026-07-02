/**
 * @file EditorAutomationScenarioService.cpp
 * @brief Application-level orchestration for editor automation scenarios.
 */

#include "Editor/EditorAutomationScenarioService.h"

#include <utility>

namespace RVX::Editor
{

void EditorAutomationScenarioService::Configure(
    EditorAutomationScenarioType scenario,
    std::string target,
    bool confirmOverwrite)
{
    m_configuredScenario = scenario;
    m_configuredTarget = std::move(target);
    m_configuredConfirmOverwrite = confirmOverwrite;
    ResetRun();
}

void EditorAutomationScenarioService::ResetRun()
{
    m_runner.Reset(m_configuredScenario,
                   m_configuredTarget,
                   m_configuredConfirmOverwrite);
}

bool EditorAutomationScenarioService::PrepareSettings(
    EditorSettingsService* settingsService)
{
    if (!settingsService || !HasScenario())
    {
        return true;
    }

    return m_runner.PrepareSettings(*settingsService);
}

EditorAutomationScenarioAdvanceResult EditorAutomationScenarioService::Advance(
    const EditorAutomationScenarioContext& context)
{
    EditorAutomationScenarioAdvanceResult result;
    result.scenarioActive = HasScenario();
    result.complete = m_runner.IsComplete();
    result.failed = m_runner.HasFailed();
    if (!result.scenarioActive || result.complete || result.failed)
    {
        return result;
    }

    m_runner.Advance(context);
    result.advanced = true;
    result.complete = m_runner.IsComplete();
    result.failed = m_runner.HasFailed();
    result.requestStop = result.failed;
    return result;
}

void EditorAutomationScenarioService::MarkIncompleteBeforeExit()
{
    if (!HasScenario() || m_runner.IsComplete() || m_runner.HasFailed())
    {
        return;
    }

    m_runner.Fail(
        "Editor automation scenario did not complete before exit: " +
        std::string(GetEditorAutomationScenarioName(m_runner.GetScenario())));
}

} // namespace RVX::Editor
