/**
 * @file EditorAutomationScenarioService.h
 * @brief Application-level orchestration for editor automation scenarios.
 */

#pragma once

#include "Core/Types.h"
#include "Editor/EditorAutomation.h"

#include <string>

namespace RVX::Editor
{

struct EditorAutomationScenarioAdvanceResult
{
    bool scenarioActive = false;
    bool advanced = false;
    bool complete = false;
    bool failed = false;
    bool requestStop = false;
};

/**
 * @brief Owns editor automation scenario lifecycle around the reusable runner.
 */
class EditorAutomationScenarioService
{
public:
    // =========================================================================
    // Configuration
    // =========================================================================
    void Configure(EditorAutomationScenarioType scenario,
                   std::string target = std::string(),
                   bool confirmOverwrite = false);
    void ResetRun();

    // =========================================================================
    // Status
    // =========================================================================
    EditorAutomationScenarioType GetScenario() const
    {
        return m_runner.GetScenario();
    }
    bool HasScenario() const { return m_runner.HasScenario(); }
    bool IsComplete() const { return m_runner.IsComplete(); }
    bool HasFailed() const { return m_runner.HasFailed(); }
    const std::string& GetError() const { return m_runner.GetError(); }
    const EditorAutomationScenarioRunner& GetRunner() const
    {
        return m_runner;
    }

    // =========================================================================
    // Lifecycle
    // =========================================================================
    bool PrepareSettings(EditorSettingsService* settingsService);
    EditorAutomationScenarioAdvanceResult Advance(
        const EditorAutomationScenarioContext& context);
    void MarkIncompleteBeforeExit();

private:
    EditorAutomationScenarioType m_configuredScenario =
        EditorAutomationScenarioType::None;
    std::string m_configuredTarget;
    bool m_configuredConfirmOverwrite = false;
    EditorAutomationScenarioRunner m_runner;
};

} // namespace RVX::Editor
