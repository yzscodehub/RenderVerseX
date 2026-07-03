#include <gtest/gtest.h>

#include "Core/App/AppMode.h"
#include "Core/Camera/Camera.h"
#include "Core/Log.h"
#include "Core/Subsystem/EngineSubsystem.h"
#include "Core/Subsystem/SubsystemCollection.h"

#include <stdexcept>
#include <string>
#include <vector>

using namespace RVX;

namespace
{
    class ScopedCoreLog
    {
    public:
        ScopedCoreLog()
        {
            if (!Log::GetCoreLogger())
            {
                Log::Initialize();
                m_owned = true;
            }
        }

        ~ScopedCoreLog()
        {
            if (m_owned)
            {
                Log::Shutdown();
            }
        }

    private:
        bool m_owned = false;
    };

    class RecordingLifecycleSubsystem final : public EngineSubsystem
    {
    public:
        explicit RecordingLifecycleSubsystem(std::vector<std::string>* events)
            : m_events(events)
        {
        }

        const char* GetName() const override { return "RecordingLifecycleSubsystem"; }

        void Initialize() override
        {
            if (m_events)
            {
                m_events->push_back("recording:init");
            }
        }

        void Deinitialize() override
        {
            if (m_events)
            {
                m_events->push_back("recording:shutdown");
            }
        }

    private:
        std::vector<std::string>* m_events = nullptr;
    };

    class FailingLifecycleSubsystem final : public EngineSubsystem
    {
    public:
        explicit FailingLifecycleSubsystem(std::vector<std::string>* events)
            : m_events(events)
        {
        }

        const char* GetName() const override { return "FailingLifecycleSubsystem"; }

        RVX_SUBSYSTEM_DEPENDENCIES(RecordingLifecycleSubsystem)

        void Initialize() override
        {
            if (m_events)
            {
                m_events->push_back("failing:init");
            }
            throw std::runtime_error("intentional lifecycle failure");
        }

    private:
        std::vector<std::string>* m_events = nullptr;
    };

    TEST(AppModeBoundaryValidation, RuntimeModeUsesCookedRuntimeContracts)
    {
        constexpr AppModeTraits traits = GetAppModeTraits(AppMode::Runtime);

        EXPECT_EQ(traits.mode, AppMode::Runtime);
        EXPECT_EQ(traits.name, "Runtime");
        EXPECT_TRUE(traits.runtimeExecutionEnabled);
        EXPECT_FALSE(traits.editorShellVisible);
        EXPECT_FALSE(traits.authoringToolsEnabled);
        EXPECT_FALSE(traits.previewWorld);
        EXPECT_FALSE(traits.sourceAssetAccessAllowed);
        EXPECT_TRUE(traits.cookedRuntimeArtifactsRequired);
        EXPECT_TRUE(IsRuntimeExecutionMode(AppMode::Runtime));
        EXPECT_FALSE(IsEditorShellMode(AppMode::Runtime));
        EXPECT_FALSE(AllowsSourceAssetAccess(AppMode::Runtime));
        EXPECT_FALSE(AllowsHotReload(AppMode::Runtime));
    }

    TEST(AppModeBoundaryValidation, EditorAndPreviewModesPermitAuthoringShells)
    {
        constexpr AppModeTraits editor = GetAppModeTraits(AppMode::Editor);
        constexpr AppModeTraits preview = GetAppModeTraits(AppMode::Preview);

        EXPECT_TRUE(editor.editorShellVisible);
        EXPECT_TRUE(editor.authoringToolsEnabled);
        EXPECT_FALSE(editor.runtimeExecutionEnabled);
        EXPECT_TRUE(editor.sourceAssetAccessAllowed);
        EXPECT_FALSE(editor.cookedRuntimeArtifactsRequired);
        EXPECT_TRUE(editor.hotReloadAllowed);
        EXPECT_TRUE(AllowsHotReload(AppMode::Editor));

        EXPECT_TRUE(preview.editorShellVisible);
        EXPECT_TRUE(preview.authoringToolsEnabled);
        EXPECT_FALSE(preview.runtimeExecutionEnabled);
        EXPECT_TRUE(preview.previewWorld);
        EXPECT_TRUE(preview.sourceAssetAccessAllowed);
        EXPECT_FALSE(preview.cookedRuntimeArtifactsRequired);
        EXPECT_TRUE(preview.hotReloadAllowed);
        EXPECT_TRUE(AllowsHotReload(AppMode::Preview));
    }

    TEST(AppModeBoundaryValidation, PlayInEditorKeepsRuntimeWorldInsideEditorShell)
    {
        constexpr AppModeTraits traits = GetAppModeTraits(AppMode::PlayInEditor);

        EXPECT_EQ(ToString(AppMode::PlayInEditor), "PlayInEditor");
        EXPECT_TRUE(traits.editorShellVisible);
        EXPECT_FALSE(traits.authoringToolsEnabled);
        EXPECT_TRUE(traits.runtimeExecutionEnabled);
        EXPECT_FALSE(traits.previewWorld);
        EXPECT_FALSE(traits.sourceAssetAccessAllowed);
        EXPECT_TRUE(traits.cookedRuntimeArtifactsRequired);
        EXPECT_FALSE(traits.hotReloadAllowed);
        EXPECT_TRUE(IsEditorShellMode(AppMode::PlayInEditor));
        EXPECT_TRUE(IsRuntimeExecutionMode(AppMode::PlayInEditor));
        EXPECT_FALSE(AllowsHotReload(AppMode::PlayInEditor));
    }

    TEST(AppModeBoundaryValidation, CookAndTestModesPermitSourceInputsWithoutEditorShellHotReload)
    {
        constexpr AppModeTraits cook = GetAppModeTraits(AppMode::Cook);
        constexpr AppModeTraits test = GetAppModeTraits(AppMode::Test);

        EXPECT_EQ(ToString(AppMode::Cook), "Cook");
        EXPECT_FALSE(cook.editorShellVisible);
        EXPECT_FALSE(cook.authoringToolsEnabled);
        EXPECT_FALSE(cook.runtimeExecutionEnabled);
        EXPECT_FALSE(cook.previewWorld);
        EXPECT_TRUE(cook.sourceAssetAccessAllowed);
        EXPECT_FALSE(cook.cookedRuntimeArtifactsRequired);
        EXPECT_FALSE(cook.hotReloadAllowed);
        EXPECT_FALSE(IsEditorShellMode(AppMode::Cook));
        EXPECT_FALSE(IsRuntimeExecutionMode(AppMode::Cook));
        EXPECT_TRUE(AllowsSourceAssetAccess(AppMode::Cook));
        EXPECT_FALSE(AllowsHotReload(AppMode::Cook));

        EXPECT_EQ(ToString(AppMode::Test), "Test");
        EXPECT_FALSE(test.editorShellVisible);
        EXPECT_FALSE(test.authoringToolsEnabled);
        EXPECT_FALSE(test.runtimeExecutionEnabled);
        EXPECT_FALSE(test.previewWorld);
        EXPECT_TRUE(test.sourceAssetAccessAllowed);
        EXPECT_FALSE(test.cookedRuntimeArtifactsRequired);
        EXPECT_FALSE(test.hotReloadAllowed);
        EXPECT_FALSE(IsEditorShellMode(AppMode::Test));
        EXPECT_FALSE(IsRuntimeExecutionMode(AppMode::Test));
        EXPECT_TRUE(AllowsSourceAssetAccess(AppMode::Test));
        EXPECT_FALSE(AllowsHotReload(AppMode::Test));
    }

    TEST(AppModeBoundaryValidation, SubsystemInitializationFailureUnwindsInitializedDependencies)
    {
        ScopedCoreLog log;
        std::vector<std::string> events;
        SubsystemCollection<EngineSubsystem> collection;

        auto* recording = collection.AddSubsystem<RecordingLifecycleSubsystem>(&events);
        auto* failing = collection.AddSubsystem<FailingLifecycleSubsystem>(&events);

        EXPECT_FALSE(collection.InitializeAll());
        EXPECT_FALSE(collection.IsInitialized());
        EXPECT_FALSE(recording->IsInitialized());
        EXPECT_FALSE(failing->IsInitialized());
        EXPECT_EQ((std::vector<std::string>{
                      "recording:init",
                      "failing:init",
                      "recording:shutdown",
                  }),
                  events);
    }

    TEST(AppModeBoundaryValidation, CameraContractLivesInCoreForSharedEditorRuntimeUse)
    {
        Camera camera;
        camera.SetPerspective(1.0f, 16.0f / 9.0f, 0.1f, 1000.0f);
        camera.SetPosition(Vec3(0.0f, 2.0f, 8.0f));
        camera.LookAt(Vec3(0.0f, 0.0f, 0.0f));

        EXPECT_FLOAT_EQ(camera.GetPosition().x, 0.0f);
        EXPECT_FLOAT_EQ(camera.GetPosition().y, 2.0f);
        EXPECT_FLOAT_EQ(camera.GetPosition().z, 8.0f);
        EXPECT_NE(camera.GetView()[3][2], 0.0f);
        EXPECT_NE(camera.GetProjection()[2][2], 1.0f);
        EXPECT_NE(camera.GetViewProjection()[3][2], 0.0f);
    }
} // namespace
