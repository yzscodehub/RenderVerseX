# Backend-native validation is conditional on the enabled production backend.
set(RVX_NATIVE_LIFECYCLE_BACKEND "")
set(RVX_NATIVE_LIFECYCLE_DEFINE "")
if(WIN32 AND RVX_ENABLE_DX12)
    set(RVX_NATIVE_LIFECYCLE_BACKEND "DX12")
    set(RVX_NATIVE_LIFECYCLE_DEFINE RVX_NATIVE_BACKEND_DX12=1)
elseif(APPLE AND RVX_ENABLE_METAL)
    set(RVX_NATIVE_LIFECYCLE_BACKEND "Metal")
    set(RVX_NATIVE_LIFECYCLE_DEFINE RVX_NATIVE_BACKEND_METAL=1)
elseif(UNIX AND RVX_ENABLE_VULKAN)
    set(RVX_NATIVE_LIFECYCLE_BACKEND "Vulkan")
    set(RVX_NATIVE_LIFECYCLE_DEFINE RVX_NATIVE_BACKEND_VULKAN=1)
endif()

if(RVX_NATIVE_LIFECYCLE_BACKEND)
    rvx_add_gtest(
        NAME NativeRenderLifecycleValidation
        LANE native
        SOURCES NativeRenderLifecycleValidation/main.cpp
        LIBS
            RVX::EngineDevelopmentComposition
            RVX::HAL
            RVX::Render
            RVX::RenderExtraction
            RVX::RHI_BackendFactory
            ${RVX_ALL_BACKENDS}
        INCLUDES
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_SOURCE_DIR}/HAL/Private
        DEFINES
            ${RVX_NATIVE_LIFECYCLE_DEFINE}
            RVX_NATIVE_REQUIRED_BACKEND="${RVX_NATIVE_LIFECYCLE_BACKEND}"
        TAGS trait.gpu suite.primary-native
        SUITE required-native-lifecycle
        TIMEOUT 180
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESOURCE_LOCK rvx_gpu
    )
endif()

if(RVX_ENABLE_DX12)
    set(RVX_NATIVE_DX12_PRIMARY_TAGS "")
    if(WIN32)
        set(RVX_NATIVE_DX12_PRIMARY_TAGS suite.primary-native)
    endif()
    rvx_add_gtest(
        NAME DX12Validation
        LANE native
        SOURCES DX12Validation/main.cpp
        LIBS
            RVX::Core
            RVX::EngineDevelopmentComposition
            RVX::Render
            RVX::RenderGraph
            RVX::RHI
            RVX::ShaderCompiler
            RVX::RHI_BackendFactory
            ${RVX_ALL_BACKENDS}
            glfw
        INCLUDES
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_SOURCE_DIR}/RHI_DX12/Private
            ${CMAKE_SOURCE_DIR}/Render/Private
        TAGS backend.dx12 trait.gpu ${RVX_NATIVE_DX12_PRIMARY_TAGS}
        TIMEOUT 120
        RESOURCE_LOCK rvx_gpu
    )
endif()

if(RVX_ENABLE_VULKAN)
    set(RVX_NATIVE_VULKAN_PRIMARY_TAGS "")
    if(UNIX AND NOT APPLE)
        set(RVX_NATIVE_VULKAN_PRIMARY_TAGS suite.primary-native)
    endif()
    rvx_add_gtest(
        NAME VulkanValidation
        LANE native
        SOURCES VulkanValidation/main.cpp
        LIBS
            RVX::Core
            RVX::Render
            RVX::RenderGraph
            RVX::RHI
            RVX::ShaderCompiler
            RVX::RHI_BackendFactory
            ${RVX_ALL_BACKENDS}
            glfw
        INCLUDES
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_SOURCE_DIR}/RHI_Vulkan/Private
        TAGS backend.vulkan trait.gpu ${RVX_NATIVE_VULKAN_PRIMARY_TAGS}
        TIMEOUT 120
        RESOURCE_LOCK rvx_gpu
    )
endif()

if(RVX_ENABLE_DX11)
    rvx_add_gtest(
        NAME DX11Validation
        LANE native
        SOURCES DX11Validation/main.cpp
        LIBS
            RVX::Core
            RVX::RenderGraph
            RVX::RHI
            RVX::ShaderCompiler
            RVX::RHI_BackendFactory
            ${RVX_ALL_BACKENDS}
            glfw
        INCLUDES
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_SOURCE_DIR}/RHI_DX11/Private
        TAGS backend.dx11 trait.gpu
        TIMEOUT 120
        RESOURCE_LOCK rvx_gpu
    )
endif()

if(RVX_ENABLE_DX12 AND RVX_ENABLE_VULKAN)
    rvx_add_gtest(
        NAME CrossBackendValidation
        LANE native
        SOURCES CrossBackendValidation/main.cpp
        LIBS
            RVX::Core
            RVX::RenderGraph
            RVX::RHI
            RVX::RHI_BackendFactory
            ${RVX_ALL_BACKENDS}
            glfw
        INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}
        TAGS trait.gpu suite.cross-backend
        TIMEOUT 180
        RESOURCE_LOCK rvx_gpu
    )
endif()

if(TARGET RVXEditor AND TARGET ImageContentValidation)
    function(rvx_add_editor_native_panel_smoke_test test_prefix panel_id artifact_name)
        set(smoke_test "${test_prefix}Smoke")
        set(image_test "${test_prefix}ImageValidation")
        set(smoke_actual "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/Editor/${artifact_name}")

        rvx_add_validation_test(
            LANE native
            NAME ${smoke_test}
            COMMAND $<TARGET_FILE:RVXEditor>
                --smoke
                --frames 40
                --ui-backend native
                --automation-scenario open-native-panel
                --automation-target ${panel_id}
                --screenshot "${smoke_actual}"
            BUILD_TARGETS RVXEditor ImageContentValidation
            TAGS product.editor trait.visual trait.gpu suite.native-ui
            TIMEOUT 120
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            RESOURCE_LOCK rvx_gpu
            FIXTURES_SETUP "${test_prefix}FreshCapture"
        )

        rvx_add_validation_test(
            LANE native
            NAME ${image_test}
            COMMAND $<TARGET_FILE:ImageContentValidation>
                --image "${smoke_actual}"
                --min-width 640
                --min-height 360
                --min-unique-colors 8
                --min-luma-range 16
                --near-black-threshold 8
                --max-near-black-ratio 0.99
                --sample-step 4
            BUILD_TARGETS ImageContentValidation
            TAGS product.editor trait.visual trait.gpu suite.native-ui
            TIMEOUT 30
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            FIXTURES_REQUIRED "${test_prefix}FreshCapture"
        )
    endfunction()

    rvx_add_editor_native_panel_smoke_test(
        RVXEditorNativeViewport
        native.viewport
        NativeViewportSmoke.ppm
    )
    rvx_add_editor_native_panel_smoke_test(
        RVXEditorNativeInspector
        native.inspector
        NativeInspectorSmoke.ppm
    )
    rvx_add_editor_native_panel_smoke_test(
        RVXEditorNativeCommandHistory
        native.commandHistory
        NativeCommandHistorySmoke.ppm
    )
    rvx_add_editor_native_panel_smoke_test(
        RVXEditorNativeConsole
        native.console
        NativeConsoleSmoke.ppm
    )
    rvx_add_editor_native_panel_smoke_test(
        RVXEditorNativeMaterialEditor
        native.materialEditor
        NativeMaterialEditorSmoke.ppm
    )
    rvx_add_editor_native_panel_smoke_test(
        RVXEditorNativeAnimationEditor
        native.animationEditor
        NativeAnimationEditorSmoke.ppm
    )
    rvx_add_editor_native_panel_smoke_test(
        RVXEditorNativePanelCatalog
        native.panelCatalog
        NativePanelCatalogSmoke.ppm
    )
    rvx_add_editor_native_panel_smoke_test(
        RVXEditorNativeAssetBrowser
        native.assetBrowser
        NativeAssetBrowserSmoke.ppm
    )
    rvx_add_editor_native_panel_smoke_test(
        RVXEditorNativePreferences
        native.preferences
        NativePreferencesSmoke.ppm
    )

    function(rvx_add_editor_native_automation_smoke_test test_prefix scenario artifact_name)
        set(options CONFIRM_OVERWRITE)
        set(oneValueArgs TARGET SETTINGS_PATH SHORTCUT_PROFILE_PATH)
        set(multiValueArgs FIXTURES_REQUIRED FIXTURES_SETUP)
        cmake_parse_arguments(RVX_AUTOMATION
            "${options}"
            "${oneValueArgs}"
            "${multiValueArgs}"
            ${ARGN}
        )

        set(smoke_test "${test_prefix}Smoke")
        set(image_test "${test_prefix}ImageValidation")
        set(smoke_actual "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/Editor/${artifact_name}")
        set(extra_args "")

        if(RVX_AUTOMATION_TARGET)
            list(APPEND extra_args
                --automation-target "${RVX_AUTOMATION_TARGET}"
            )
        endif()
        if(RVX_AUTOMATION_SETTINGS_PATH)
            list(APPEND extra_args
                --settings-path "${RVX_AUTOMATION_SETTINGS_PATH}"
            )
        endif()
        if(RVX_AUTOMATION_SHORTCUT_PROFILE_PATH)
            list(APPEND extra_args
                --auto-shortcut-profile-path "${RVX_AUTOMATION_SHORTCUT_PROFILE_PATH}"
            )
        endif()
        if(RVX_AUTOMATION_CONFIRM_OVERWRITE)
            list(APPEND extra_args
                --automation-confirm-overwrite
            )
        endif()

        rvx_add_validation_test(
            LANE native
            NAME ${smoke_test}
            COMMAND $<TARGET_FILE:RVXEditor>
                --smoke
                --frames 40
                --ui-backend native
                --automation-scenario ${scenario}
                ${extra_args}
                --screenshot "${smoke_actual}"
            BUILD_TARGETS RVXEditor ImageContentValidation
            TAGS product.editor trait.visual trait.gpu suite.native-ui suite.automation
            TIMEOUT 120
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            RESOURCE_LOCK rvx_gpu
            FIXTURES_SETUP
                "${test_prefix}FreshCapture;${RVX_AUTOMATION_FIXTURES_SETUP}"
            FIXTURES_REQUIRED "${RVX_AUTOMATION_FIXTURES_REQUIRED}"
        )

        rvx_add_validation_test(
            LANE native
            NAME ${image_test}
            COMMAND $<TARGET_FILE:ImageContentValidation>
                --image "${smoke_actual}"
                --min-width 640
                --min-height 360
                --min-unique-colors 8
                --min-luma-range 16
                --near-black-threshold 8
                --max-near-black-ratio 0.99
                --sample-step 4
            BUILD_TARGETS ImageContentValidation
            TAGS product.editor trait.visual trait.gpu suite.native-ui suite.automation
            TIMEOUT 30
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            FIXTURES_REQUIRED "${test_prefix}FreshCapture"
        )
    endfunction()

    set(RVX_EDITOR_NATIVE_AUTOMATION_DIR
        "${CMAKE_CURRENT_BINARY_DIR}/EditorAutomation/$<CONFIG>")
    set(RVX_EDITOR_NATIVE_FILE_PICKER_SCENE
        "${RVX_EDITOR_NATIVE_AUTOMATION_DIR}/NativeFilePickerScene.rvxscene")
    set(RVX_EDITOR_NATIVE_SHORTCUT_SETTINGS
        "${RVX_EDITOR_NATIVE_AUTOMATION_DIR}/ShortcutAutosave/EditorSettings.json")
    set(RVX_EDITOR_NATIVE_SHORTCUT_PROFILE
        "${RVX_EDITOR_NATIVE_AUTOMATION_DIR}/ShortcutAutosave/EditorShortcuts.shortcuts")
    set(RVX_EDITOR_NATIVE_UI_SCALE_SETTINGS
        "${RVX_EDITOR_NATIVE_AUTOMATION_DIR}/UIScale/EditorSettings.json")

    rvx_add_validation_test(
        LANE native
        NAME RVXEditorNativeAutomationDirectoryPrepare
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${RVX_EDITOR_NATIVE_AUTOMATION_DIR}"
            "${RVX_EDITOR_NATIVE_AUTOMATION_DIR}/ShortcutAutosave"
            "${RVX_EDITOR_NATIVE_AUTOMATION_DIR}/UIScale"
        TAGS product.editor suite.native-ui suite.automation
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        FIXTURES_SETUP RVXEditorNativeAutomationDirectories
    )

    rvx_add_validation_test(
        LANE native
        NAME RVXEditorNativeFilePickerOverwriteFixture
        COMMAND ${CMAKE_COMMAND} -E touch
            "${RVX_EDITOR_NATIVE_FILE_PICKER_SCENE}"
        TAGS product.editor suite.native-ui suite.automation
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        FIXTURES_SETUP RVXEditorNativeFilePickerScene
        FIXTURES_REQUIRED RVXEditorNativeAutomationDirectories
    )

    rvx_add_editor_native_automation_smoke_test(
        RVXEditorNativeCommandSurfaceClick
        command-surface-click
        NativeCommandSurfaceClickSmoke.ppm
        TARGET "file:file.newScene"
    )
    rvx_add_editor_native_automation_smoke_test(
        RVXEditorNativeGameObjectCreateEmptyClick
        command-surface-click
        NativeGameObjectCreateEmptyClickSmoke.ppm
        TARGET "gameObject:gameObject.createEmpty"
    )
    rvx_add_editor_native_automation_smoke_test(
        RVXEditorNativeCommandMenuOpen
        command-menu-open
        NativeCommandMenuOpenSmoke.ppm
        TARGET "file"
    )
    rvx_add_editor_native_automation_smoke_test(
        RVXEditorNativeViewMenuOpen
        command-menu-open
        NativeViewMenuOpenSmoke.ppm
        TARGET "view"
    )
    rvx_add_editor_native_automation_smoke_test(
        RVXEditorNativeCommandPaletteOpen
        command-palette-open
        NativeCommandPaletteOpenSmoke.ppm
        TARGET "command palette"
    )
    rvx_add_editor_native_automation_smoke_test(
        RVXEditorNativeSaveSceneFilePicker
        save-scene-as-file-picker
        NativeSaveSceneFilePickerSmoke.ppm
        TARGET "${RVX_EDITOR_NATIVE_FILE_PICKER_SCENE}"
        CONFIRM_OVERWRITE
        FIXTURES_SETUP RVXEditorNativeSavedScene
        FIXTURES_REQUIRED RVXEditorNativeFilePickerScene
    )
    rvx_add_editor_native_automation_smoke_test(
        RVXEditorNativeOpenSceneFilePicker
        open-scene-file-picker
        NativeOpenSceneFilePickerSmoke.ppm
        TARGET "${RVX_EDITOR_NATIVE_FILE_PICKER_SCENE}"
        FIXTURES_REQUIRED RVXEditorNativeSavedScene
    )
    rvx_add_editor_native_automation_smoke_test(
        RVXEditorNativeShortcutAutosave
        shortcut-autosave-write
        NativeShortcutAutosaveSmoke.ppm
        SETTINGS_PATH "${RVX_EDITOR_NATIVE_SHORTCUT_SETTINGS}"
        SHORTCUT_PROFILE_PATH "${RVX_EDITOR_NATIVE_SHORTCUT_PROFILE}"
        FIXTURES_REQUIRED RVXEditorNativeAutomationDirectories
    )
    rvx_add_editor_native_automation_smoke_test(
        RVXEditorNativeUIScaleAdjust
        ui-scale-adjust
        NativeUIScaleAdjustSmoke.ppm
        SETTINGS_PATH "${RVX_EDITOR_NATIVE_UI_SCALE_SETTINGS}"
        FIXTURES_REQUIRED RVXEditorNativeAutomationDirectories
    )
endif()
