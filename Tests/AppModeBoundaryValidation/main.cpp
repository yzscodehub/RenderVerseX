#include <gtest/gtest.h>

#include "Core/App/AppMode.h"
#include "Core/Camera/Camera.h"

using namespace RVX;

namespace
{
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

        EXPECT_TRUE(preview.editorShellVisible);
        EXPECT_TRUE(preview.authoringToolsEnabled);
        EXPECT_FALSE(preview.runtimeExecutionEnabled);
        EXPECT_TRUE(preview.previewWorld);
        EXPECT_TRUE(preview.sourceAssetAccessAllowed);
        EXPECT_FALSE(preview.cookedRuntimeArtifactsRequired);
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
        EXPECT_TRUE(IsEditorShellMode(AppMode::PlayInEditor));
        EXPECT_TRUE(IsRuntimeExecutionMode(AppMode::PlayInEditor));
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
