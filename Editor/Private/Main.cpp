/**
 * @file Main.cpp
 * @brief RenderVerseX Editor entry point
 */

#include "Editor/Editor.h"
#include "Editor/EditorApplication.h"
#include "Editor/UI/EditorUIBackendCatalog.h"
#include "Core/Log.h"

#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
    RVX::Editor::EditorAutomationScenarioType ParseAutoFilePickerScenario(
        const std::string& value)
    {
        if (value == "open-scene")
        {
            return RVX::Editor::EditorAutomationScenarioType::
                OpenSceneFilePicker;
        }
        if (value == "save-scene-as")
        {
            return RVX::Editor::EditorAutomationScenarioType::
                SaveSceneAsFilePicker;
        }
        throw std::runtime_error(
            "Unsupported --auto-open-file-picker value: " + value);
    }

    RVX::Editor::EditorUIBackendType ParseUIBackendType(
        const std::string& value)
    {
        RVX::Editor::EditorUIBackendType type =
            RVX::Editor::GetDefaultEditorUIBackendType();
        if (RVX::Editor::TryParseEditorUIBackendType(value, type))
        {
            return type;
        }

        throw std::runtime_error("Unsupported --ui-backend value: " + value);
    }

    RVX::Editor::EditorAutomationScenarioType ParseAutomationScenario(
        const std::string& value)
    {
        RVX::Editor::EditorAutomationScenarioType scenario =
            RVX::Editor::EditorAutomationScenarioType::None;
        if (RVX::Editor::TryParseEditorAutomationScenario(value, scenario))
        {
            return scenario;
        }

        throw std::runtime_error(
            "Unsupported --automation-scenario value: " + value);
    }
}

int main(int argc, char* argv[])
{
    try
    {
        RVX::Editor::EditorApplicationRunConfig runConfig;
        bool smokeMode = false;

        auto requireValue = [&](int& index, const char* option) -> const char*
        {
            if (index + 1 >= argc)
            {
                throw std::runtime_error(std::string("Missing value for ") + option);
            }
            return argv[++index];
        };

        for (int i = 1; i < argc; ++i)
        {
            const std::string arg = argv[i];
            if (arg == "--smoke")
            {
                smokeMode = true;
            }
            else if (arg == "--frames")
            {
                runConfig.maxFrames = static_cast<RVX::uint32>(
                    std::stoul(requireValue(i, "--frames")));
            }
            else if (arg == "--screenshot")
            {
                runConfig.screenshotPath = requireValue(i, "--screenshot");
            }
            else if (arg == "--settings-path")
            {
                runConfig.settingsPathOverride =
                    std::filesystem::path(requireValue(i, "--settings-path"));
            }
            else if (arg == "--ui-backend")
            {
                runConfig.uiBackendType =
                    ParseUIBackendType(requireValue(i, "--ui-backend"));
                runConfig.overrideUIBackendType = true;
            }
            else if (arg == "--automation-scenario")
            {
                runConfig.automationScenario =
                    ParseAutomationScenario(
                        requireValue(i, "--automation-scenario"));
            }
            else if (arg == "--automation-target")
            {
                runConfig.automationTarget =
                    requireValue(i, "--automation-target");
            }
            else if (arg == "--auto-open-file-picker")
            {
                runConfig.automationScenario =
                    ParseAutoFilePickerScenario(
                        requireValue(i, "--auto-open-file-picker"));
            }
            else if (arg == "--auto-file-picker-path")
            {
                runConfig.automationTarget =
                    requireValue(i, "--auto-file-picker-path");
            }
            else if (arg == "--auto-open-native-panel")
            {
                runConfig.automationScenario =
                    RVX::Editor::EditorAutomationScenarioType::OpenNativePanel;
                runConfig.automationTarget =
                    requireValue(i, "--auto-open-native-panel");
            }
            else if (arg == "--auto-shortcut-profile-path")
            {
                runConfig.autoShortcutProfilePath =
                    std::filesystem::path(
                        requireValue(i, "--auto-shortcut-profile-path"));
            }
            else if (arg == "--auto-confirm-overwrite")
            {
                runConfig.automationConfirmOverwrite = true;
            }
            else if (arg == "--automation-confirm-overwrite")
            {
                runConfig.automationConfirmOverwrite = true;
            }
            else if (arg == "--auto-shortcut-autosave")
            {
                runConfig.automationScenario =
                    RVX::Editor::EditorAutomationScenarioType::
                        ShortcutAutosaveWrite;
            }
            else if (arg == "--expose-legacy-imgui-debug-commands")
            {
                runConfig.exposeLegacyImGuiDebugCommands = true;
            }
            else if (arg == "--windowed")
            {
                runConfig.startMaximized = false;
            }
            else if (arg == "--maximized")
            {
                runConfig.startMaximized = true;
            }
            else if (arg == "--help" || arg == "-h")
            {
                std::cout
                    << "RenderVerseX Editor\n"
                    << "  --smoke             Run a bounded automated editor session\n"
                    << "  --frames <count>    Number of frames before exit in smoke mode\n"
                    << "  --screenshot <ppm>  Capture the main framebuffer before exit\n"
                    << "  --settings-path <json>\n"
                    << "                      Override editor settings for automation\n"
                    << "  --ui-backend <native>\n"
                    << "                      Override the configured editor UI backend\n"
                    << "  --automation-scenario <name>\n"
                    << "                      Run an editor automation scenario "
                       "(open-native-panel, command-surface-click, "
                       "command-menu-open, command-palette-open, "
                       "open-scene-file-picker, save-scene-as-file-picker, "
                       "ui-scale-adjust, shortcut-autosave-write)\n"
                    << "  --automation-target <value>\n"
                    << "                      Target value for scenarios that need one\n"
                    << "  --automation-confirm-overwrite\n"
                    << "                      Confirm overwrite prompts during automation\n"
                    << "  --auto-open-file-picker <open-scene|save-scene-as>\n"
                    << "                      Deprecated alias for file picker automation scenarios\n"
                    << "  --auto-file-picker-path <path>\n"
                    << "                      Deprecated alias for --automation-target\n"
                    << "  --auto-open-native-panel <panel-id>\n"
                    << "                      Show a native editor panel through its View command\n"
                    << "  --auto-shortcut-profile-path <path>\n"
                    << "                      Override the shortcut profile path for automation\n"
                    << "  --auto-confirm-overwrite\n"
                    << "                      Deprecated alias for --automation-confirm-overwrite\n"
                    << "  --auto-shortcut-autosave\n"
                    << "                      Deprecated alias for --automation-scenario shortcut-autosave-write\n"
                    << "  --expose-legacy-imgui-debug-commands\n"
                    << "                      Add legacy ImGui diagnostics to native debug menus\n"
                    << "  --windowed          Start with the configured window size instead of maximized\n"
                    << "  --maximized         Start maximized (default)\n";
                return 0;
            }
            else
            {
                throw std::runtime_error("Unknown argument: " + arg);
            }
        }

        if (smokeMode && runConfig.maxFrames == 0)
        {
            runConfig.maxFrames = 12;
        }
        if (!runConfig.screenshotPath.empty() && runConfig.maxFrames == 0)
        {
            runConfig.maxFrames = 12;
        }
        if (runConfig.automationScenario !=
                RVX::Editor::EditorAutomationScenarioType::None &&
            RVX::Editor::EditorAutomationScenarioRequiresSettingsPath(
                runConfig.automationScenario) &&
            runConfig.settingsPathOverride.empty())
        {
            throw std::runtime_error(
                "--automation-scenario requires --settings-path");
        }
        if (runConfig.automationScenario !=
                RVX::Editor::EditorAutomationScenarioType::None &&
            runConfig.maxFrames == 0)
        {
            runConfig.maxFrames = 40;
        }
        // Initialize core logging
        RVX::Log::Initialize();
        RVX_CORE_INFO("Starting RenderVerseX Editor...");

        // Create and run the editor application
        RVX::Editor::EditorApplication app;
        app.SetRunConfig(runConfig);

        if (!app.Initialize())
        {
            RVX_CORE_ERROR("Failed to initialize editor application");
            return -1;
        }

        // Run the main loop
        int result = app.Run();

        // Cleanup
        app.Shutdown();
        RVX_CORE_INFO("Editor shutdown complete");

        return result;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return -1;
    }
}
