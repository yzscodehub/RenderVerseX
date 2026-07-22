# =============================================================================
# Editor Module - RenderVerseX Editor Application
# =============================================================================
add_library(RVX_Editor STATIC)

option(RVX_EDITOR_ENABLE_LEGACY_IMGUI "Build the legacy ImGui editor bridge and panels" ON)

target_sources(RVX_Editor PRIVATE
    Private/EditorWindow.cpp
    Private/EditorBeginFrameService.cpp
    Private/EditorCommandBindingService.cpp
    Private/EditorCommandExecutionService.cpp
    Private/EditorContext.cpp
    Private/EditorDocumentActionService.cpp
    Private/EditorDocumentCommandService.cpp
    Private/EditorDocumentFileCommandService.cpp
    Private/EditorDocument.cpp
    Private/EditorEndFrameService.cpp
    Private/EditorAutomation.cpp
    Private/EditorAutomationScenarioService.cpp
    Private/EditorFrameCoordinator.cpp
    Private/EditorFrameLifecycleService.cpp
    Private/EditorFrameSubmissionService.cpp
    Private/EditorFileDialog.cpp
    Private/EditorInputBridgeService.cpp
    Private/EditorLegacyDebugUIService.cpp
    Private/EditorLayoutPersistenceService.cpp
    Private/EditorMainFramePresentationService.cpp
    Private/EditorMainFramebufferService.cpp
    Private/EditorMainSwapChainService.cpp
    Private/EditorNativeUIFrameService.cpp
    Private/EditorNativeUIRenderStatsService.cpp
    Private/EditorNativePanelRegistrationService.cpp
    Private/EditorNativeUISubmissionService.cpp
    Private/EditorNotificationService.cpp
    Private/EditorOperationService.cpp
    Private/EditorPendingAction.cpp
    Private/EditorRenderBootstrapService.cpp
    Private/EditorRenderFrameService.cpp
    Private/EditorRenderRuntimeAdapter.cpp
    Private/EditorRenderShutdownService.cpp
    Private/EditorScreenshotService.cpp
    Private/EditorSelectionService.cpp
    Private/EditorServiceRegistrationService.cpp
    Private/EditorServiceRegistry.cpp
    Private/EditorRunLoopService.cpp
    Private/EditorSettings.cpp
    Private/EditorScreenshotRequestService.cpp
    Private/EditorShortcutProfileLifecycleService.cpp
    Private/EditorUpdateService.cpp
    Private/EditorUIBootstrapService.cpp
    Private/EditorUIShutdownService.cpp
    Private/EditorUnsavedChanges.cpp
    Private/EditorViewportToolService.cpp
    Private/EditorViewportRenderService.cpp
    Private/EditorWindowFrameService.cpp
    Private/EditorWindowLifecycleService.cpp
    Private/EditorApplication.cpp
    Private/EditorTheme.cpp
    Private/UI/EditorCommandCatalog.cpp
    Private/UI/EditorCommandIconResolver.cpp
    Private/UI/EditorCommandPalette.cpp
    Private/UI/EditorCommandRegistry.cpp
    Private/UI/EditorCommandRouter.cpp
    Private/UI/EditorCommandSurfaceModel.cpp
    Private/UI/EditorCommandSurfaceRenderer.cpp
    Private/UI/EditorContextMenu.cpp
    Private/UI/EditorDockingModel.cpp
    Private/UI/EditorIconButton.cpp
    Private/UI/EditorInputBridge.cpp
    Private/UI/EditorLayoutSerializer.cpp
    Private/UI/EditorModalDialog.cpp
    Private/UI/EditorNativePanelCommands.cpp
    Private/UI/EditorFilePickerDialog.cpp
    Private/UI/EditorFilePickerModel.cpp
    Private/UI/EditorFormLayout.cpp
    Private/UI/EditorListLayout.cpp
    Private/UI/EditorPanelChrome.cpp
    Private/UI/EditorPanelButtonStyle.cpp
    Private/UI/EditorPanelSurfaceRenderer.cpp
    Private/UI/EditorPickerFilterModel.cpp
    Private/UI/EditorPickerListModel.cpp
    Private/UI/EditorPickerListView.cpp
    Private/UI/EditorPopupLayer.cpp
    Private/UI/EditorPropertyDrawerRegistry.cpp
    Private/UI/EditorShellMetrics.cpp
    Private/UI/EditorShellPolicy.cpp
    Private/UI/EditorWorkspaceLayoutPolicy.cpp
    Private/UI/EditorShortcutBindingModel.cpp
    Private/UI/EditorShortcutProfile.cpp
    Private/UI/EditorShortcutProfileService.cpp
    Private/UI/EditorCheckbox.cpp
    Private/UI/EditorComboBox.cpp
    Private/UI/EditorTextInput.cpp
    Private/UI/EditorTreeList.cpp
    Private/UI/EditorTooltipLayer.cpp
    Private/UI/EditorTypography.cpp
    Private/UI/EditorUIAutomation.cpp
    Private/UI/EditorUIBackendCatalog.cpp
    Private/UI/EditorUIBackendFactory.cpp
    Private/UI/EditorVectorIcon.cpp
    Private/UI/EditorViewportCameraControllerModel.cpp
    Private/UI/EditorViewportInteractionModel.cpp
    Private/UI/EditorViewportManipulatorModel.cpp
    Private/UI/EditorViewportNavigationModel.cpp
    Private/UI/EditorViewportProjection.cpp
    Private/UI/EditorViewportSelectionOverlayModel.cpp
    Private/UI/EditorViewportSelectionModel.cpp
    Private/UI/EditorViewportToolModel.cpp
    Private/UI/EditorUIHost.cpp
    Private/UI/NativeEditorUIBackend.cpp
    Private/Panels/Viewport.cpp
    Private/Panels/Console.cpp
    Private/Panels/NativeAnimationEditor.cpp
    Private/Panels/NativeAssetBrowser.cpp
    Private/Panels/NativeCommandHistory.cpp
    Private/Panels/NativeConsole.cpp
    Private/Panels/NativeEditorPreferences.cpp
    Private/Panels/NativeInspector.cpp
    Private/Panels/NativeMaterialEditor.cpp
    Private/Panels/NativePanelCatalog.cpp
    Private/Panels/NativeSceneHierarchy.cpp
    Private/Panels/NativeViewport.cpp
)

if(RVX_EDITOR_ENABLE_LEGACY_IMGUI)
    target_sources(RVX_Editor PRIVATE
        Private/UI/DebugImGuiLayer.cpp
        Private/Panels/SceneHierarchy.cpp
        Private/Panels/Inspector.cpp
        Private/Panels/AssetBrowser.cpp
        Private/Panels/AnimationEditor.cpp
        Private/Panels/MaterialEditor.cpp
        Private/Panels/ParticleEditor.cpp
    )
endif()

target_include_directories(RVX_Editor PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

# Find OpenGL/glad for the editor's current GLFW main-framebuffer helpers.
find_package(glad CONFIG REQUIRED)
find_package(OpenGL REQUIRED)

target_link_libraries(RVX_Editor PUBLIC
    RVX::Core
    RVX::RHI
    RVX::Render
    RVX::Resource
    RVX::Scene
    RVX::Tools
    RVX::UI
    RVX::Animation
    RVX::HAL
    glad::glad
    OpenGL::GL
)

if(RVX_ENABLE_DX11)
    target_link_libraries(RVX_Editor PRIVATE RVX::RHI_DX11)
endif()
if(RVX_ENABLE_DX12)
    target_link_libraries(RVX_Editor PRIVATE RVX::RHI_DX12)
endif()
if(RVX_ENABLE_VULKAN)
    target_link_libraries(RVX_Editor PRIVATE RVX::RHI_Vulkan)
endif()
if(RVX_ENABLE_METAL)
    target_link_libraries(RVX_Editor PRIVATE RVX::RHI_Metal)
endif()
if(RVX_ENABLE_OPENGL)
    target_link_libraries(RVX_Editor PRIVATE RVX::RHI_OpenGL)
endif()
if(WIN32)
    target_link_libraries(RVX_Editor PRIVATE Comdlg32)
endif()

target_compile_definitions(RVX_Editor PUBLIC
    RVX_EDITOR_ENABLE_LEGACY_IMGUI=$<BOOL:${RVX_EDITOR_ENABLE_LEGACY_IMGUI}>
    RVX_EDITOR_M1_RENDER_ADAPTER=1
)

if(RVX_EDITOR_ENABLE_LEGACY_IMGUI)
    # Link ImGui for the optional legacy/debug editor bridge.
    find_package(imgui CONFIG REQUIRED)
    find_package(imguizmo CONFIG REQUIRED)
    target_link_libraries(RVX_Editor PRIVATE
        imgui::imgui
        imguizmo::imguizmo
    )
endif()
add_library(RVX::Editor ALIAS RVX_Editor)

# =============================================================================
# Editor Executable
# =============================================================================
add_executable(RVXEditor
    Private/Main.cpp
)

target_link_libraries(RVXEditor PRIVATE
    RVX::Editor
    RVX::Engine
)
