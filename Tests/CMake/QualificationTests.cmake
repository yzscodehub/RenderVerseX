# Qualification-only opt-ins are deliberately absent from normal developer
# presets. Their guards fail closed when a required primary backend is absent.
option(RVX_ENABLE_P0_QUALIFICATION_MATRIX
    "Register the strict DX12/Vulkan P0 sample qualification matrix" OFF)
if(RVX_ENABLE_P0_QUALIFICATION_MATRIX)
    if(NOT WIN32 OR NOT RVX_ENABLE_DX12 OR NOT RVX_ENABLE_VULKAN OR NOT TARGET RenderVerseSamples)
        message(FATAL_ERROR "RVX_ENABLE_P0_QUALIFICATION_MATRIX requires Windows DX12, Vulkan, and RenderVerseSamples")
    endif()
elseif(NOT WIN32)
    message(STATUS "P0 qualification matrix: Unavailable (requires Windows DX12 and Vulkan backends)")
endif()
option(RVX_ENABLE_P1_QUALIFICATION_MATRIX
    "Register the strict DX12/Vulkan P1 physics sample qualification matrix" OFF)
if(RVX_ENABLE_P1_QUALIFICATION_MATRIX)
    if(NOT WIN32 OR NOT RVX_ENABLE_DX12 OR NOT RVX_ENABLE_VULKAN OR NOT TARGET RenderVerseSamples)
        message(FATAL_ERROR "RVX_ENABLE_P1_QUALIFICATION_MATRIX requires Windows DX12, Vulkan, and RenderVerseSamples")
    endif()
elseif(NOT WIN32)
    message(STATUS "P1 qualification matrix: Unavailable (requires Windows DX12 and Vulkan backends)")
endif()
option(RVX_ENABLE_GFX07_DEFERRED_NATIVE
    "Register the fail-closed DX12/Vulkan GFX07 DeferredQuality native sample gate" OFF)
if(RVX_ENABLE_GFX07_DEFERRED_NATIVE)
    if(NOT WIN32 OR NOT RVX_ENABLE_DX12 OR NOT RVX_ENABLE_VULKAN OR NOT TARGET RenderVerseSamples)
        message(FATAL_ERROR "RVX_ENABLE_GFX07_DEFERRED_NATIVE requires Windows DX12, Vulkan, and RenderVerseSamples")
    endif()
elseif(NOT WIN32)
    message(STATUS "GFX07 Deferred qualification: Unavailable (requires Windows DX12 and Vulkan backends)")
endif()
option(RVX_ENABLE_GFX08_LIGHTING_NATIVE
    "Register the fail-closed DX12/Vulkan GFX08 Forward+/Deferred lighting gate" OFF)
if(RVX_ENABLE_GFX08_LIGHTING_NATIVE)
    if(NOT WIN32 OR NOT RVX_ENABLE_DX12 OR NOT RVX_ENABLE_VULKAN OR NOT TARGET RenderVerseSamples)
        message(FATAL_ERROR "RVX_ENABLE_GFX08_LIGHTING_NATIVE requires Windows DX12, Vulkan, and RenderVerseSamples")
    endif()
elseif(NOT WIN32)
    message(STATUS "GFX08 lighting qualification: Unavailable (requires Windows DX12 and Vulkan backends)")
endif()
option(RVX_ENABLE_GFX09_ANIMATION_NATIVE
    "Register the fail-closed DX12/Vulkan GFX09 Forward+/Deferred animation-deformation gate" OFF)
if(RVX_ENABLE_GFX09_ANIMATION_NATIVE)
    if(NOT WIN32 OR NOT RVX_ENABLE_DX12 OR NOT RVX_ENABLE_VULKAN OR NOT TARGET RenderVerseSamples)
        message(FATAL_ERROR "RVX_ENABLE_GFX09_ANIMATION_NATIVE requires Windows DX12, Vulkan, and RenderVerseSamples")
    endif()
elseif(NOT WIN32)
    message(STATUS "GFX09 animation qualification: Unavailable (requires Windows DX12 and Vulkan backends)")
endif()
option(RVX_ENABLE_GFX10_POST_PROCESS_NATIVE
    "Register the fail-closed DX12/Vulkan GFX10 Forward+/Deferred post-process gate" OFF)
if(RVX_ENABLE_GFX10_POST_PROCESS_NATIVE)
    if(NOT WIN32 OR NOT RVX_ENABLE_DX12 OR NOT RVX_ENABLE_VULKAN OR NOT TARGET RenderVerseSamples)
        message(FATAL_ERROR "RVX_ENABLE_GFX10_POST_PROCESS_NATIVE requires Windows DX12, Vulkan, and RenderVerseSamples")
    endif()
elseif(NOT WIN32)
    message(STATUS "GFX10 post-process qualification: Unavailable (requires Windows DX12 and Vulkan backends)")
endif()
option(RVX_ENABLE_PR_SMOKE
    "Register only the fixed primary-backend SceneLifecycle PR smoke" OFF)
option(RVX_ENABLE_COMPATIBILITY_SMOKE
    "Register fixed DX11/OpenGL SceneLifecycle compatibility smokes" OFF)
option(RVX_ENABLE_WEEKLY_SOAK
    "Register the hermetic 10,000-frame / five-minute primary-backend soak" OFF)
set(RVX_DEDICATED_QUALIFICATION_MODES)
foreach(mode PR_SMOKE COMPATIBILITY_SMOKE WEEKLY_SOAK)
    if(RVX_ENABLE_${mode})
        list(APPEND RVX_DEDICATED_QUALIFICATION_MODES ${mode})
    endif()
endforeach()
list(LENGTH RVX_DEDICATED_QUALIFICATION_MODES RVX_DEDICATED_QUALIFICATION_MODE_COUNT)
if(RVX_DEDICATED_QUALIFICATION_MODE_COUNT GREATER 1)
    message(FATAL_ERROR
        "PR, compatibility, and weekly soak modes require separate build trees")
endif()
if(RVX_DEDICATED_QUALIFICATION_MODE_COUNT GREATER 0 AND
   (RVX_ENABLE_P0_QUALIFICATION_MATRIX OR RVX_ENABLE_P1_QUALIFICATION_MATRIX))
    message(FATAL_ERROR
        "Dedicated PR/compatibility/soak modes require P0/P1 matrices disabled")
endif()

if((RVX_ENABLE_P0_QUALIFICATION_MATRIX OR RVX_ENABLE_P1_QUALIFICATION_MATRIX
    OR RVX_ENABLE_GFX07_DEFERRED_NATIVE OR RVX_ENABLE_GFX08_LIGHTING_NATIVE
    OR RVX_ENABLE_GFX09_ANIMATION_NATIVE OR RVX_ENABLE_GFX10_POST_PROCESS_NATIVE)
   AND (NOT TARGET VisualGoldenValidation OR NOT TARGET ImageContentValidation OR NOT TARGET SampleReportContentValidation))
    message(FATAL_ERROR "Enabled qualification matrices require visual and SampleReport validators")
endif()

function(rvx_add_renderverse_primary_smoke SAMPLE_ID SAMPLE_LABEL ASSET_ID BACKEND_NAME BACKEND_LABEL)
    cmake_parse_arguments(
        RVX_SAMPLE
        "WAIT_READY;ASSESSMENT"
        "READY_TIMEOUT_MS;READY_MAX_FRAMES;TIMEOUT;ASSESSMENT_PROFILE;ASSET_LICENSE;SUITE"
        "SAMPLE_ARGS;REPORT_ARGS"
        ${ARGN})
    if(RVX_SAMPLE_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "rvx_add_renderverse_primary_smoke received unparsed arguments: ${RVX_SAMPLE_UNPARSED_ARGUMENTS}")
    endif()

    if(NOT TARGET RenderVerseSamples OR
       NOT TARGET ImageContentValidation OR
       NOT TARGET SampleReportContentValidation)
        return()
    endif()

    foreach(config_name Debug Release RelWithDebInfo MinSizeRel)
        file(MAKE_DIRECTORY
            "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/${config_name}/RenderVerseSamples")
        file(MAKE_DIRECTORY
            "${CMAKE_CURRENT_BINARY_DIR}/SampleReports/${config_name}/RenderVerseSamples")
        file(MAKE_DIRECTORY
            "${CMAKE_CURRENT_BINARY_DIR}/StartupTimelines/${config_name}/RenderVerseSamples")
    endforeach()

    set(test_prefix "RenderVerseSamples${BACKEND_LABEL}${SAMPLE_LABEL}")
    set(smoke_actual
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/${test_prefix}_320x180.ppm")
    set(smoke_report
        "${CMAKE_CURRENT_BINARY_DIR}/SampleReports/$<CONFIG>/RenderVerseSamples/${test_prefix}_320x180.json")
    set(smoke_startup_report
        "${CMAKE_CURRENT_BINARY_DIR}/StartupTimelines/$<CONFIG>/RenderVerseSamples/${test_prefix}_320x180.json")
    set(smoke_readiness_args)
    set(smoke_assessment_args)
    set(smoke_assessment_report_args)
    # Every primary smoke captures a screenshot, which implicitly enables the
    # readiness-bounded runner path and may require a carrying publication frame.
    set(smoke_frame_report_args --min-frame-count 8)
    if(RVX_SAMPLE_WAIT_READY)
        if(NOT RVX_SAMPLE_READY_TIMEOUT_MS)
            set(RVX_SAMPLE_READY_TIMEOUT_MS 30000)
        endif()
        if(NOT RVX_SAMPLE_READY_MAX_FRAMES)
            set(RVX_SAMPLE_READY_MAX_FRAMES 64)
        endif()
        list(APPEND smoke_readiness_args
            --wait-ready
            --ready-timeout-ms ${RVX_SAMPLE_READY_TIMEOUT_MS}
            --ready-max-frames ${RVX_SAMPLE_READY_MAX_FRAMES})
    endif()
    if(RVX_SAMPLE_ASSESSMENT)
        if(NOT RVX_SAMPLE_ASSESSMENT_PROFILE)
            set(RVX_SAMPLE_ASSESSMENT_PROFILE smoke)
        endif()
        foreach(config_name Debug Release RelWithDebInfo MinSizeRel)
            file(MAKE_DIRECTORY
                "${CMAKE_CURRENT_BINARY_DIR}/FrameworkAssessments/${config_name}/RenderVerseSamples")
        endforeach()
        set(smoke_assessment_report
            "${CMAKE_CURRENT_BINARY_DIR}/FrameworkAssessments/$<CONFIG>/RenderVerseSamples/${test_prefix}_320x180.json")
        list(APPEND smoke_assessment_args
            --assessment-report "${smoke_assessment_report}"
            --assessment-profile ${RVX_SAMPLE_ASSESSMENT_PROFILE})
        list(APPEND smoke_assessment_report_args
            --require-assessment-summary
            --assessment-report-path "${smoke_assessment_report}"
            --assessment-blocking-count 0
            --assessment-dropped-event-count 0
            --require-assessment-pass)
    endif()
    if(NOT RVX_SAMPLE_TIMEOUT)
        set(RVX_SAMPLE_TIMEOUT 120)
    endif()
    if(NOT RVX_SAMPLE_ASSET_LICENSE)
        set(RVX_SAMPLE_ASSET_LICENSE
            LicenseRef-RenderVerseX-Generated-Fixture)
    endif()
    if(NOT RVX_SAMPLE_SUITE)
        set(RVX_SAMPLE_SUITE primary)
    endif()

    rvx_add_validation_test(
        LANE qualification
        NAME ${test_prefix}Smoke
        COMMAND $<TARGET_FILE:RenderVerseSamples>
            --sample ${SAMPLE_ID}
            --backend ${BACKEND_NAME}
            --smoke
            --frames 8
            --width 320
            --height 180
            --quality low
            --screenshot "${smoke_actual}"
            --report "${smoke_report}"
            --startup-report "${smoke_startup_report}"
            --diagnostics
            --validation
            ${smoke_readiness_args}
            ${smoke_assessment_args}
            ${RVX_SAMPLE_SAMPLE_ARGS}
        BUILD_TARGETS RenderVerseSamples ImageContentValidation SampleReportContentValidation
        TAGS product.samples trait.visual trait.gpu backend.${BACKEND_NAME}
            suite.${RVX_SAMPLE_SUITE}
        SUITE ${RVX_SAMPLE_SUITE}
        TIMEOUT ${RVX_SAMPLE_TIMEOUT}
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:RenderVerseSamples>"
        RESOURCE_LOCK rvx_gpu
        FIXTURES_SETUP "${test_prefix}FreshCapture"
    )

    rvx_add_validation_test(
        LANE qualification
        NAME ${test_prefix}ImageContentValidation
        COMMAND $<TARGET_FILE:ImageContentValidation>
            --image "${smoke_actual}"
            --min-width 320
            --min-height 180
            --min-unique-colors 16
            --min-luma-range 8
            --near-black-threshold 8
            --max-near-black-ratio 0.95
            --sample-step 2
        BUILD_TARGETS ImageContentValidation
        TAGS product.samples trait.visual trait.gpu backend.${BACKEND_NAME}
            suite.${RVX_SAMPLE_SUITE}
        SUITE ${RVX_SAMPLE_SUITE}
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        FIXTURES_REQUIRED "${test_prefix}FreshCapture"
    )

    rvx_add_validation_test(
        LANE qualification
        NAME ${test_prefix}ReportValidation
        COMMAND $<TARGET_FILE:SampleReportContentValidation>
            --report "${smoke_report}"
            --sample-name ${SAMPLE_ID}
            --category visual
            --backend ${BACKEND_NAME}
            --quality low
            ${smoke_frame_report_args}
            --asset-id ${ASSET_ID}
            --asset-license-spdx ${RVX_SAMPLE_ASSET_LICENSE}
            --require-redistributable
            --min-visible-objects 1
            --require-enabled-feature ModelResourceLoad
            --require-enabled-feature SceneInstantiation
            --require-enabled-feature RenderGraph
            --require-enabled-feature SceneRendering
            --require-ready
            ${smoke_assessment_report_args}
            ${RVX_SAMPLE_REPORT_ARGS}
        BUILD_TARGETS SampleReportContentValidation
        TAGS product.samples trait.visual trait.gpu backend.${BACKEND_NAME}
            suite.${RVX_SAMPLE_SUITE}
        SUITE ${RVX_SAMPLE_SUITE}
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        FIXTURES_REQUIRED "${test_prefix}FreshCapture"
    )
endfunction()

# GFX08 proves that the same immutable lighting selection reaches both raster
# recipes.  Each producer is paired with image and structured-receipt
# validators; no backend absence, CSM fallback, or missing lighting receipt is
# accepted by the required-suite runner.
function(rvx_add_renderverse_gfx08_lighting_native
         BACKEND_NAME BACKEND_LABEL PIPELINE_NAME PIPELINE_LABEL)
    if(NOT TARGET RenderVerseSamples OR
       NOT TARGET ImageContentValidation OR
       NOT TARGET SampleReportContentValidation)
        message(FATAL_ERROR "GFX08 lighting qualification requires RenderVerseSamples and report/image validators")
    endif()

    foreach(config_name Debug Release RelWithDebInfo MinSizeRel)
        file(MAKE_DIRECTORY
            "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/${config_name}/RenderVerseSamples")
        file(MAKE_DIRECTORY
            "${CMAKE_CURRENT_BINARY_DIR}/SampleReports/${config_name}/RenderVerseSamples")
        file(MAKE_DIRECTORY
            "${CMAKE_CURRENT_BINARY_DIR}/StartupTimelines/${config_name}/RenderVerseSamples")
    endforeach()

    set(test_prefix
        "RenderVerseSamples${BACKEND_LABEL}GFX08${PIPELINE_LABEL}LightingShadows")
    set(capture
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/${test_prefix}_640x360.ppm")
    set(report
        "${CMAKE_CURRENT_BINARY_DIR}/SampleReports/$<CONFIG>/RenderVerseSamples/${test_prefix}_640x360.json")
    set(startup_report
        "${CMAKE_CURRENT_BINARY_DIR}/StartupTimelines/$<CONFIG>/RenderVerseSamples/${test_prefix}_640x360.json")

    rvx_add_validation_test(
        LANE native
        NAME ${test_prefix}Capture
        COMMAND $<TARGET_FILE:RenderVerseSamples>
            --sample lighting-shadows
            --backend ${BACKEND_NAME}
            --pipeline ${PIPELINE_NAME}
            --quality high
            --smoke
            --frames 8
            --wait-ready
            --ready-timeout-ms 120000
            --ready-max-frames 120
            --width 640
            --height 360
            --screenshot "${capture}"
            --report "${report}"
            --startup-report "${startup_report}"
            --diagnostics
            --validation
        BUILD_TARGETS RenderVerseSamples ImageContentValidation SampleReportContentValidation
        TAGS product.samples trait.visual trait.gpu trait.native
            backend.${BACKEND_NAME} suite.gfx08
        SUITE gfx08-lighting
        TIMEOUT 180
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:RenderVerseSamples>"
        RESOURCE_LOCK rvx_gpu
        FIXTURES_SETUP "${test_prefix}FreshCapture"
    )

    rvx_add_validation_test(
        LANE native
        NAME ${test_prefix}ImageValidation
        COMMAND $<TARGET_FILE:ImageContentValidation>
            --image "${capture}"
            --min-width 640
            --min-height 360
            --min-unique-colors 16
            --min-luma-range 8
            --near-black-threshold 8
            --max-near-black-ratio 0.95
            --sample-step 2
        BUILD_TARGETS ImageContentValidation
        TAGS product.samples trait.visual trait.gpu trait.native
            backend.${BACKEND_NAME} suite.gfx08
        SUITE gfx08-lighting
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        FIXTURES_REQUIRED "${test_prefix}FreshCapture"
    )

    rvx_add_validation_test(
        LANE native
        NAME ${test_prefix}ReceiptValidation
        COMMAND $<TARGET_FILE:SampleReportContentValidation>
            --report "${report}"
            --sample-name lighting-shadows
            --category visual
            --backend ${BACKEND_NAME}
            --require-requested-backend ${BACKEND_NAME}
            --require-actual-backend ${BACKEND_NAME}
            --quality high
            --min-frame-count 8
            --min-visible-objects 1
            --require-ready
            --require-readiness-wait
            --require-native-validation-available
            --require-native-validation-enabled
            --require-native-validation-read-complete
            --require-native-validation-error-count 0
            --require-native-validation-corruption-count 0
            --require-render-pipeline ${PIPELINE_NAME}
            --require-lighting-plan
            --require-csm-requested
            --require-csm-execution-ready
            --forbid-fallback-reasons
            --forbid-render-feature-fallbacks
        BUILD_TARGETS SampleReportContentValidation
        TAGS product.samples trait.visual trait.gpu trait.native
            backend.${BACKEND_NAME} suite.gfx08
        SUITE gfx08-lighting
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        FIXTURES_REQUIRED "${test_prefix}FreshCapture"
    )
endfunction()

# GFX09 qualifies one presentation-bound deformation resource snapshot across
# both raster recipes.  The producer remains Direct because skinned packets are
# deliberately excluded from the GPU-driven execution strategy; recipe and
# execution strategy are independent contracts.
function(rvx_add_renderverse_gfx09_animation_native
         BACKEND_NAME BACKEND_LABEL PIPELINE_NAME PIPELINE_LABEL)
    if(NOT TARGET RenderVerseSamples OR
       NOT TARGET ImageContentValidation OR
       NOT TARGET SampleReportContentValidation)
        message(FATAL_ERROR "GFX09 animation qualification requires RenderVerseSamples and report/image validators")
    endif()

    foreach(config_name Debug Release RelWithDebInfo MinSizeRel)
        file(MAKE_DIRECTORY
            "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/${config_name}/RenderVerseSamples")
        file(MAKE_DIRECTORY
            "${CMAKE_CURRENT_BINARY_DIR}/SampleReports/${config_name}/RenderVerseSamples")
        file(MAKE_DIRECTORY
            "${CMAKE_CURRENT_BINARY_DIR}/StartupTimelines/${config_name}/RenderVerseSamples")
    endforeach()

    set(test_prefix
        "RenderVerseSamples${BACKEND_LABEL}GFX09${PIPELINE_LABEL}AnimationCharacter")
    set(capture
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/${test_prefix}_640x360.ppm")
    set(report
        "${CMAKE_CURRENT_BINARY_DIR}/SampleReports/$<CONFIG>/RenderVerseSamples/${test_prefix}_640x360.json")
    set(startup_report
        "${CMAKE_CURRENT_BINARY_DIR}/StartupTimelines/$<CONFIG>/RenderVerseSamples/${test_prefix}_640x360.json")
    set(pipeline_receipt_args)
    if(PIPELINE_NAME STREQUAL "deferred")
        list(APPEND pipeline_receipt_args
            --require-deferred-opaque-execution)
    endif()

    rvx_add_validation_test(
        LANE native
        NAME ${test_prefix}Capture
        COMMAND $<TARGET_FILE:RenderVerseSamples>
            --sample animation-character
            --backend ${BACKEND_NAME}
            --pipeline ${PIPELINE_NAME}
            --render-path direct
            --quality high
            --smoke
            --frames 8
            --wait-ready
            --ready-timeout-ms 120000
            --ready-max-frames 2400
            --width 640
            --height 360
            --screenshot "${capture}"
            --report "${report}"
            --startup-report "${startup_report}"
            --diagnostics
            --validation
        BUILD_TARGETS RenderVerseSamples ImageContentValidation SampleReportContentValidation
        TAGS product.samples trait.visual trait.gpu trait.native
            backend.${BACKEND_NAME} suite.gfx09
        SUITE gfx09-animation
        TIMEOUT 300
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:RenderVerseSamples>"
        RESOURCE_LOCK rvx_gpu
        FIXTURES_SETUP "${test_prefix}FreshCapture"
    )

    rvx_add_validation_test(
        LANE native
        NAME ${test_prefix}ImageValidation
        COMMAND $<TARGET_FILE:ImageContentValidation>
            --image "${capture}"
            --min-width 640
            --min-height 360
            --min-unique-colors 16
            --min-luma-range 8
            --near-black-threshold 8
            --max-near-black-ratio 0.95
            --sample-step 2
        BUILD_TARGETS ImageContentValidation
        TAGS product.samples trait.visual trait.gpu trait.native
            backend.${BACKEND_NAME} suite.gfx09
        SUITE gfx09-animation
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        FIXTURES_REQUIRED "${test_prefix}FreshCapture"
    )

    rvx_add_validation_test(
        LANE native
        NAME ${test_prefix}ReceiptValidation
        COMMAND $<TARGET_FILE:SampleReportContentValidation>
            --report "${report}"
            --sample-name animation-character
            --category visual
            --backend ${BACKEND_NAME}
            --require-requested-backend ${BACKEND_NAME}
            --require-actual-backend ${BACKEND_NAME}
            --quality high
            --min-frame-count 8
            --min-visible-objects 7
            --require-ready
            --require-readiness-wait
            --render-path direct
            --require-requested-render-path direct
            --require-actual-render-path direct
            --require-direct-execution
            --require-render-pipeline ${PIPELINE_NAME}
            ${pipeline_receipt_args}
            --require-lighting-plan
            --require-csm-requested
            --require-csm-execution-ready
            --require-deformation-plan
            --require-current-skinning-palette
            --require-previous-skinning-palette
            --require-skinned-opaque-execution
            --require-skinned-shadow-execution
            --require-skinned-velocity-execution
            --require-native-validation-available
            --require-native-validation-enabled
            --require-native-validation-read-complete
            --require-native-validation-error-count 0
            --require-native-validation-corruption-count 0
            --require-phase stable
            --require-action PresentSkinningPalette
            --require-invariant PresentedPalette
            --require-enabled-feature EcsPresentedSkinningPalette
            --forbid-fallback-reasons
            --forbid-render-feature-fallbacks
        BUILD_TARGETS SampleReportContentValidation
        TAGS product.samples trait.visual trait.gpu trait.native
            backend.${BACKEND_NAME} suite.gfx09
        SUITE gfx09-animation
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        FIXTURES_REQUIRED "${test_prefix}FreshCapture"
    )
endfunction()

# GFX10 qualifies the typed post-process pipeline across both raster recipes.
# The scenario requests only public RenderContracts policy. The native receipt
# validator proves the renderer selected and executed the typed feature; the
# sample cannot add, order, or inspect private renderer passes.
function(rvx_add_renderverse_gfx10_post_process_native
         BACKEND_NAME BACKEND_LABEL PIPELINE_NAME PIPELINE_LABEL)
    if(NOT TARGET RenderVerseSamples OR
       NOT TARGET ImageContentValidation OR
       NOT TARGET SampleReportContentValidation)
        message(FATAL_ERROR "GFX10 post-process qualification requires RenderVerseSamples and report/image validators")
    endif()

    foreach(config_name Debug Release RelWithDebInfo MinSizeRel)
        file(MAKE_DIRECTORY
            "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/${config_name}/RenderVerseSamples")
        file(MAKE_DIRECTORY
            "${CMAKE_CURRENT_BINARY_DIR}/SampleReports/${config_name}/RenderVerseSamples")
        file(MAKE_DIRECTORY
            "${CMAKE_CURRENT_BINARY_DIR}/StartupTimelines/${config_name}/RenderVerseSamples")
    endforeach()

    set(test_prefix
        "RenderVerseSamples${BACKEND_LABEL}GFX10${PIPELINE_LABEL}PostProcessing")
    set(capture
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/${test_prefix}_640x360.ppm")
    set(report
        "${CMAKE_CURRENT_BINARY_DIR}/SampleReports/$<CONFIG>/RenderVerseSamples/${test_prefix}_640x360.json")
    set(startup_report
        "${CMAKE_CURRENT_BINARY_DIR}/StartupTimelines/$<CONFIG>/RenderVerseSamples/${test_prefix}_640x360.json")
    set(pipeline_receipt_args)
    if(PIPELINE_NAME STREQUAL "deferred")
        list(APPEND pipeline_receipt_args
            --require-deferred-opaque-execution)
    endif()

    rvx_add_validation_test(
        LANE native
        NAME ${test_prefix}Capture
        COMMAND $<TARGET_FILE:RenderVerseSamples>
            --sample post-processing
            --backend ${BACKEND_NAME}
            --pipeline ${PIPELINE_NAME}
            --render-path direct
            --quality high
            --smoke
            --wait-ready
            --ready-timeout-ms 120000
            --ready-max-frames 2400
            --view hero
            --fixed-delta 0.0166666666666667
            --capture-tick 120
            --width 640
            --height 360
            --screenshot "${capture}"
            --report "${report}"
            --startup-report "${startup_report}"
            --diagnostics
            --validation
        BUILD_TARGETS RenderVerseSamples ImageContentValidation SampleReportContentValidation
        TAGS product.samples trait.visual trait.gpu trait.native
            backend.${BACKEND_NAME} suite.gfx10
        SUITE gfx10-postprocess
        TIMEOUT 300
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:RenderVerseSamples>"
        RESOURCE_LOCK rvx_gpu
        FIXTURES_SETUP "${test_prefix}FreshCapture"
    )

    rvx_add_validation_test(
        LANE native
        NAME ${test_prefix}ImageValidation
        COMMAND $<TARGET_FILE:ImageContentValidation>
            --image "${capture}"
            --min-width 640
            --min-height 360
            --min-unique-colors 16
            --min-luma-range 8
            --near-black-threshold 8
            --max-near-black-ratio 0.95
            --sample-step 2
        BUILD_TARGETS ImageContentValidation
        TAGS product.samples trait.visual trait.gpu trait.native
            backend.${BACKEND_NAME} suite.gfx10
        SUITE gfx10-postprocess
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        FIXTURES_REQUIRED "${test_prefix}FreshCapture"
    )

    rvx_add_validation_test(
        LANE native
        NAME ${test_prefix}ReceiptValidation
        COMMAND $<TARGET_FILE:SampleReportContentValidation>
            --report "${report}"
            --sample-name post-processing
            --category visual
            --backend ${BACKEND_NAME}
            --require-requested-backend ${BACKEND_NAME}
            --require-actual-backend ${BACKEND_NAME}
            --quality high
            --min-frame-count 120
            --min-visible-objects 1
            --require-ready
            --require-readiness-wait
            --render-path direct
            --require-requested-render-path direct
            --require-actual-render-path direct
            --require-direct-execution
            --require-render-pipeline ${PIPELINE_NAME}
            ${pipeline_receipt_args}
            --require-post-process-execution
            --require-exactly-one-tone-map
            --require-post-process-bloom-execution
            --require-post-process-color-grading-execution
            --require-post-process-fxaa-execution
            --require-no-temporal-aa-execution
            --require-render-feature-executed 2
            --require-native-validation-available
            --require-native-validation-enabled
            --require-native-validation-read-complete
            --require-native-validation-error-count 0
            --require-native-validation-corruption-count 0
            --require-phase stable
            --require-enabled-feature EnginePostProcessConfiguration
            --require-enabled-feature BloomRequested
            --require-enabled-feature ACESToneMappingRequested
            --require-enabled-feature ColorGradingRequested
            --require-enabled-feature FXAARequested
            --require-enabled-feature TemporalAAExplicitlyDisabled
            --forbid-fallback-reasons
            --forbid-render-feature-fallbacks
        BUILD_TARGETS SampleReportContentValidation
        TAGS product.samples trait.visual trait.gpu trait.native
            backend.${BACKEND_NAME} suite.gfx10
        SUITE gfx10-postprocess
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        FIXTURES_REQUIRED "${test_prefix}FreshCapture"
    )
endfunction()

# GFX07 intentionally has its own opt-in suite: it uses the same producer,
# fixture, image, and structured-report contract as other qualifications, but
# validates a native DeferredQuality frame rather than changing the fixed PR
# SceneLifecycle smoke.  Do not mark a missing Windows Vulkan device skippable:
# the required-suite runner turns a skipped/disabled/not-run CTest case into a
# gate failure together with its JUnit evidence.
function(rvx_add_renderverse_gfx07_deferred_native BACKEND_NAME BACKEND_LABEL)
    if(NOT TARGET RenderVerseSamples OR
       NOT TARGET ImageContentValidation OR
       NOT TARGET SampleReportContentValidation)
        message(FATAL_ERROR "GFX07 Deferred qualification requires RenderVerseSamples and report/image validators")
    endif()

    foreach(config_name Debug Release RelWithDebInfo MinSizeRel)
        file(MAKE_DIRECTORY
            "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/${config_name}/RenderVerseSamples")
        file(MAKE_DIRECTORY
            "${CMAKE_CURRENT_BINARY_DIR}/SampleReports/${config_name}/RenderVerseSamples")
        file(MAKE_DIRECTORY
            "${CMAKE_CURRENT_BINARY_DIR}/StartupTimelines/${config_name}/RenderVerseSamples")
    endforeach()

    set(test_prefix "RenderVerseSamples${BACKEND_LABEL}GFX07DeferredModelViewer")
    set(capture
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/${test_prefix}_640x360.ppm")
    set(report
        "${CMAKE_CURRENT_BINARY_DIR}/SampleReports/$<CONFIG>/RenderVerseSamples/${test_prefix}_640x360.json")
    set(startup_report
        "${CMAKE_CURRENT_BINARY_DIR}/StartupTimelines/$<CONFIG>/RenderVerseSamples/${test_prefix}_640x360.json")

    rvx_add_validation_test(
        LANE native
        NAME ${test_prefix}Capture
        COMMAND $<TARGET_FILE:RenderVerseSamples>
            --sample model-viewer
            --backend ${BACKEND_NAME}
            --pipeline deferred
            --quality high
            --smoke
            --frames 8
            --wait-ready
            --ready-timeout-ms 120000
            --ready-max-frames 120
            --width 640
            --height 360
            --screenshot "${capture}"
            --report "${report}"
            --startup-report "${startup_report}"
            --diagnostics
            --validation
        BUILD_TARGETS RenderVerseSamples ImageContentValidation SampleReportContentValidation
        TAGS product.samples trait.visual trait.gpu trait.native
            backend.${BACKEND_NAME} suite.gfx07
        SUITE gfx07-deferred
        TIMEOUT 180
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:RenderVerseSamples>"
        RESOURCE_LOCK rvx_gpu
        FIXTURES_SETUP "${test_prefix}FreshCapture"
    )

    rvx_add_validation_test(
        LANE native
        NAME ${test_prefix}ImageValidation
        COMMAND $<TARGET_FILE:ImageContentValidation>
            --image "${capture}"
            --min-width 640
            --min-height 360
            --min-unique-colors 16
            --min-luma-range 8
            --near-black-threshold 8
            --max-near-black-ratio 0.95
            --sample-step 2
        BUILD_TARGETS ImageContentValidation
        TAGS product.samples trait.visual trait.gpu trait.native
            backend.${BACKEND_NAME} suite.gfx07
        SUITE gfx07-deferred
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        FIXTURES_REQUIRED "${test_prefix}FreshCapture"
    )

    rvx_add_validation_test(
        LANE native
        NAME ${test_prefix}ReceiptValidation
        COMMAND $<TARGET_FILE:SampleReportContentValidation>
            --report "${report}"
            --sample-name model-viewer
            --category visual
            --backend ${BACKEND_NAME}
            --require-requested-backend ${BACKEND_NAME}
            --require-actual-backend ${BACKEND_NAME}
            --quality high
            --min-frame-count 8
            --min-visible-objects 1
            --require-ready
            --require-readiness-wait
            --require-native-validation-available
            --require-native-validation-enabled
            --require-native-validation-read-complete
            --require-native-validation-error-count 0
            --require-native-validation-corruption-count 0
            --require-render-pipeline deferred
            --require-deferred-opaque-execution
            --forbid-fallback-reasons
            --forbid-render-feature-fallbacks
            --require-render-feature-executed 4
            --require-render-feature-executed 5
            --require-render-feature-executed 6
        BUILD_TARGETS SampleReportContentValidation
        TAGS product.samples trait.visual trait.gpu trait.native
            backend.${BACKEND_NAME} suite.gfx07
        SUITE gfx07-deferred
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        FIXTURES_REQUIRED "${test_prefix}FreshCapture"
    )
endfunction()

function(rvx_add_renderverse_pr_smoke BACKEND_NAME BACKEND_LABEL)
    rvx_add_renderverse_primary_smoke(
        scene-lifecycle SceneLifecycleDirect water-bottle
        ${BACKEND_NAME} ${BACKEND_LABEL}
        SUITE pr-smoke
        ASSET_LICENSE CC0-1.0
        SAMPLE_ARGS
            --render-path direct
        REPORT_ARGS
            --render-path direct
            --require-requested-render-path direct
            --require-actual-render-path direct
            --require-direct-execution
            --require-asset-entry water-bottle
            --require-supported-feature SceneLifecycleProbe
            --require-supported-feature DeferredEcsCommands
            --require-supported-feature GenerationSafeEntityIdentity)
endfunction()

function(rvx_add_renderverse_compatibility_smoke BACKEND_NAME BACKEND_LABEL)
    rvx_add_renderverse_primary_smoke(
        scene-lifecycle SceneLifecycleCompatibility water-bottle
        ${BACKEND_NAME} ${BACKEND_LABEL}
        SUITE compatibility-smoke
        ASSET_LICENSE CC0-1.0
        SAMPLE_ARGS
            --render-path direct
        REPORT_ARGS
            --render-path direct
            --require-requested-render-path direct
            --require-actual-render-path direct
            --require-direct-execution
            --require-asset-entry water-bottle
            --require-supported-feature SceneLifecycleProbe)
endfunction()

function(rvx_add_renderverse_weekly_soak BACKEND_NAME BACKEND_LABEL)
    if(NOT TARGET RenderVerseSamples)
        message(FATAL_ERROR "Weekly soak requires RenderVerseSamples")
    endif()
    set(test_prefix "RenderVerseSamples${BACKEND_LABEL}WeeklySoak")
    set(lifetime_root
        "${CMAKE_BINARY_DIR}/artifacts/$<CONFIG>/WeeklySoak/${BACKEND_LABEL}")
    rvx_add_validation_test(
        LANE qualification
        NAME ${test_prefix}
        COMMAND $<TARGET_FILE:RenderVerseSamples>
            --sample model-viewer
            --asset r7-triangle
            --backend ${BACKEND_NAME}
            --render-path direct
            --smoke
            --frames 1
            --wait-ready
            --ready-timeout-ms 180000
            --ready-max-frames 4096
            --width 320
            --height 180
            --quality low
            --deterministic-orbit
            --startup-report "${lifetime_root}/startup.json"
            --lifetime-warmup-frames 120
            --lifetime-observation-frames 9880
            --lifetime-min-duration-ms 300000
            --lifetime-resize-interval 2400
            --lifetime-resize-settle-frames 60
            --lifetime-report "${lifetime_root}/lifetime.json"
            --report "${lifetime_root}/sample.json"
            --diagnostics
            --validation
        BUILD_TARGETS RenderVerseSamples SampleQualificationReportValidation
        TAGS product.samples trait.gpu backend.${BACKEND_NAME}
        suite.model-viewer suite.lifetime suite.soak suite.weekly-soak
        SUITE weekly-soak
        TIMEOUT 3600
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:RenderVerseSamples>"
        RESOURCE_LOCK rvx_gpu
        FIXTURES_SETUP "${test_prefix}Artifacts"
    )

    rvx_add_validation_test(
        LANE qualification
        NAME ${test_prefix}Evidence
        COMMAND $<TARGET_FILE:SampleQualificationReportValidation>
            --startup-report "${lifetime_root}/startup.json"
            --lifetime-report "${lifetime_root}/lifetime.json"
            --sample-report "${lifetime_root}/sample.json"
            --backend ${BACKEND_NAME}
            --render-path direct
            --asset-mode source
            --expected-texture-count 0
            --expected-visible-objects 1
            --max-first-present-ms 5000
            --require-resize
        BUILD_TARGETS SampleQualificationReportValidation
        TAGS product.samples trait.diagnostics backend.${BACKEND_NAME}
            suite.model-viewer suite.lifetime suite.soak
        SUITE weekly-soak
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        FIXTURES_REQUIRED "${test_prefix}Artifacts"
    )
endfunction()

function(rvx_add_renderverse_p0_cross_path_parity SAMPLE_LABEL BACKEND_NAME BACKEND_LABEL)
    if(NOT TARGET VisualGoldenValidation)
        return()
    endif()

    set(test_prefix "RenderVerseSamples${BACKEND_LABEL}${SAMPLE_LABEL}")
    set(direct_capture
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/${test_prefix}Direct_320x180.ppm")
    set(gpu_capture
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/${test_prefix}GPUDriven_320x180.ppm")
    set(parity_diff
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/${test_prefix}_DirectVsGPUDriven_diff.ppm")
    set(parity_report
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/${test_prefix}_DirectVsGPUDriven.json")
    rvx_add_validation_test(
        LANE qualification
        NAME ${test_prefix}CrossPathParityValidation
        COMMAND $<TARGET_FILE:VisualGoldenValidation>
            --expected "${gpu_capture}"
            --actual "${direct_capture}"
            --diff "${parity_diff}"
            --report "${parity_report}"
            --tolerance 0.0
            --max-different-pixels 0
        BUILD_TARGETS VisualGoldenValidation
        TAGS product.samples trait.visual trait.gpu backend.${BACKEND_NAME} suite.primary suite.gpu-driven suite.parity
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        FIXTURES_REQUIRED
            "${test_prefix}GPUDrivenFreshCapture;${test_prefix}DirectFreshCapture")
endfunction()

function(rvx_add_renderverse_p0_matrix SAMPLE_ID SAMPLE_LABEL ASSET_ID ASSET_LICENSE
         BACKEND_NAME BACKEND_LABEL)
    cmake_parse_arguments(
        RVX_P0
        ""
        "ENVIRONMENT_ID;ENVIRONMENT_LICENSE;ENVIRONMENT_SOURCE_URI;READY_MAX_FRAMES"
        "CONTENT_IDS;REPORT_ARGS"
        ${ARGN})
    if(RVX_P0_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "rvx_add_renderverse_p0_matrix received unparsed arguments: ${RVX_P0_UNPARSED_ARGUMENTS}")
    endif()

    if(NOT RVX_P0_READY_MAX_FRAMES)
        set(RVX_P0_READY_MAX_FRAMES 600)
    endif()

    foreach(path_label Direct GPUDriven)
        if(path_label STREQUAL "Direct")
            set(render_path direct)
            set(execution_gate --require-direct-execution)
        else()
            set(render_path gpu-driven)
            set(execution_gate --require-gpu-driven-execution)
        endif()

        set(report_args
            --render-path ${render_path}
            --require-requested-render-path ${render_path}
            --require-actual-render-path ${render_path}
            --require-requested-physics-backend built-in
            --require-actual-physics-backend built-in
            --forbid-physics-backend-fallback
            --require-native-validation-available
            --require-native-validation-enabled
            --require-native-validation-read-complete
            --require-native-validation-error-count 0
            --require-native-validation-corruption-count 0
            ${execution_gate}
            --asset-license-spdx ${ASSET_LICENSE}
            --require-phase stable)
        foreach(content_id IN LISTS RVX_P0_CONTENT_IDS)
            list(APPEND report_args --require-asset-content-id ${content_id})
        endforeach()
        if(RVX_P0_ENVIRONMENT_ID)
            list(APPEND report_args
                --environment-asset-id ${RVX_P0_ENVIRONMENT_ID}
                --environment-license-spdx ${RVX_P0_ENVIRONMENT_LICENSE}
                --environment-source-uri ${RVX_P0_ENVIRONMENT_SOURCE_URI}
                --require-environment-redistributable)
        endif()

        rvx_add_renderverse_primary_smoke(
            ${SAMPLE_ID} ${SAMPLE_LABEL}${path_label}
            ${ASSET_ID} ${BACKEND_NAME} ${BACKEND_LABEL}
            WAIT_READY
            ASSESSMENT
            ASSESSMENT_PROFILE qualification
            READY_TIMEOUT_MS 120000
            READY_MAX_FRAMES ${RVX_P0_READY_MAX_FRAMES}
            SAMPLE_ARGS
                --render-path ${render_path}
            REPORT_ARGS
                ${report_args}
                ${RVX_P0_REPORT_ARGS})
    endforeach()

    rvx_add_renderverse_p0_cross_path_parity(
        ${SAMPLE_LABEL} ${BACKEND_NAME} ${BACKEND_LABEL})
endfunction()

function(rvx_add_renderverse_base_render_path_matrices BACKEND_NAME BACKEND_LABEL)
    set(generated_fixture_license LicenseRef-RenderVerseX-Generated-Fixture)

    rvx_add_renderverse_p0_matrix(
        model-rendering ModelRendering shadow-plane-caster
        ${generated_fixture_license} ${BACKEND_NAME} ${BACKEND_LABEL})

    rvx_add_renderverse_p0_matrix(
        asset-gallery AssetGallery r7-triangle
        ${generated_fixture_license} ${BACKEND_NAME} ${BACKEND_LABEL}
        REPORT_ARGS
            --min-visible-objects 3
            --require-enabled-feature ResourceSceneAdapters
            --require-enabled-feature CatalogStaticGallery
            --require-enabled-feature BoundsCameraFraming
            --require-unsupported-feature RuntimeAssetSwitching
            --require-asset-entry r7-triangle
            --require-asset-entry shadow-plane-caster
            --require-asset-entry pbr-material-grid
            --require-resource-diagnostic "gallery assets=3")

    rvx_add_renderverse_p0_matrix(
        lighting-shadows LightingShadows shadow-plane-caster
        ${generated_fixture_license} ${BACKEND_NAME} ${BACKEND_LABEL}
        REPORT_ARGS
            --require-enabled-feature ShadowCasterReceiverFixture
            --require-enabled-feature CascadedDirectionalShadowRequested
            --require-enabled-feature DirectionalShadow
            --require-directional-shadow
            --require-resource-diagnostic "shadow atlas resolution=1024"
            --require-resource-diagnostic "shadow cascade count=1")

    rvx_add_renderverse_p0_matrix(
        render-pipeline RenderPipeline shadow-plane-caster
        ${generated_fixture_license} ${BACKEND_NAME} ${BACKEND_LABEL}
        REPORT_ARGS
            --require-enabled-feature EngineRenderPipelineConfiguration
            --require-enabled-feature DirectionalShadow
            --require-enabled-feature PostProcessRenderGraph
            --require-directional-shadow
            --min-render-graph-passes 8
            --min-enabled-post-process-effects 2
            --min-post-process-graph-passes 2
            --forbid-unsupported-post-process
            --require-resource-diagnostic
                "pipeline ownership=Render/RenderGraph/PostProcessStack")

    rvx_add_renderverse_p0_matrix(
        model-viewer ModelViewer r7-triangle
        ${generated_fixture_license} ${BACKEND_NAME} ${BACKEND_LABEL}
        REPORT_ARGS
            --require-enabled-feature BoundsCameraFraming
            --require-enabled-feature OrbitCamera
            --require-enabled-feature ModelRenderReady
            --require-enabled-feature ProceduralSkyScene
            --forbid-fallback-reason PacketEnvironmentResourcesUnavailable)

    rvx_add_renderverse_p0_matrix(
        pbr-materials PBRMaterials pbr-material-grid
        ${generated_fixture_license} ${BACKEND_NAME} ${BACKEND_LABEL}
        READY_MAX_FRAMES 192
        ENVIRONMENT_ID pbr-reference-environment
        ENVIRONMENT_LICENSE ${generated_fixture_license}
        ENVIRONMENT_SOURCE_URI
            repo://Samples/Common/Tools/SampleHDRFixtureWriter.cpp
        REPORT_ARGS
            --require-enabled-feature PBRMetallicRoughnessMaterials
            --require-enabled-feature PBRMaterialParameterCube
            --require-enabled-feature PBRBaseColorFamilyAxis
            --require-enabled-feature MetallicRoughnessFactorCube
            --require-enabled-feature SharedMeshSceneInstances
            --require-opaque-execution
            --require-opaque-materials-ready
            --require-texture-ibl
            --forbid-fallback-reason PacketEnvironmentResourcesUnavailable
            --require-enabled-feature EnvironmentResourceLoad
            --require-enabled-feature EnvironmentIBL
            --require-enabled-feature DiffuseIBL
            --require-enabled-feature SpecularIBL
            --require-enabled-feature TextureIBL
            --require-enabled-feature ControlledStudioLighting
            --require-enabled-feature BoundsCameraFraming
            --require-resource-diagnostic "pbr materials=125"
            --require-resource-diagnostic "matrix shape=5x5x5"
            --require-resource-diagnostic "scene mesh instances=125"
            --require-resource-diagnostic "opaque draw packets=125")
endfunction()

function(rvx_add_renderverse_scene_lifecycle_matrix BACKEND_NAME BACKEND_LABEL)
    rvx_add_renderverse_p0_matrix(
        scene-lifecycle SceneLifecycle water-bottle
        CC0-1.0 ${BACKEND_NAME} ${BACKEND_LABEL}
        CONTENT_IDS
            b337e526fd6a162013c2984aeec163f5fbb4f717252724dfc3f3458bd51df94b
            4df540b802067f192a7aa38aed524dda141dc23d9cb7a7e6272fc021694198ed
        REPORT_ARGS
            --require-asset-entry water-bottle
            --require-action Spawn
            --require-action ReparentKeepLocal
            --require-action ReparentKeepWorld
            --require-action RecycleSecondaryCamera
            --require-action PresentSecondaryCamera
            --require-action RestorePrimaryCamera
            --require-action MutateLight
            --require-action MutateMaterialSlots
            --require-action DestroyAndReuseEntitySlot
            --require-invariant EntitySlotReuse
            --require-invariant CameraEntityReuse
            --require-invariant KeepLocal
            --require-invariant KeepWorld
            --require-invariant CameraTemporalReset
            --require-invariant LightWriteVersion
            --require-invariant MaterialSlotWriteVersion
            --require-invariant RetirementQueuesDrained
            --require-metric secondary-camera-presentation
            --require-metric primary-camera-presentation
            --require-metric-equals final-owned-entity-count=5
            --require-metric-equals expected-owned-entity-count=5
            --require-metric light-write-version
            --require-metric material-slot-write-version
            --require-metric-equals terminal-pending-destroy=0
            --require-metric-equals terminal-cleanup-required=0
            --require-metric-equals terminal-retiring=0
            --require-metric-equals terminal-recyclable=0
            --require-metric-equals terminal-render-pending-upload=0
            --require-metric-equals terminal-render-retirement=0
            --require-supported-feature SceneLifecycleProbe
            --require-supported-feature DeferredEcsCommands
            --require-supported-feature GenerationSafeEntityIdentity
            --require-supported-feature TransformHierarchyAuthorityProbe
            --require-supported-feature EcsCameraLifecycleProbe
            --require-supported-feature RuntimeLightFragmentMutationProbe
            --require-supported-feature RuntimeMaterialSlotMutationProbe)
endfunction()

function(rvx_add_renderverse_asset_streaming_matrix BACKEND_NAME BACKEND_LABEL)
    rvx_add_renderverse_p0_matrix(
        asset-streaming AssetStreaming water-bottle CC0-1.0
        ${BACKEND_NAME} ${BACKEND_LABEL}
        READY_MAX_FRAMES 2000
        CONTENT_IDS
            b337e526fd6a162013c2984aeec163f5fbb4f717252724dfc3f3458bd51df94b
            4df540b802067f192a7aa38aed524dda141dc23d9cb7a7e6272fc021694198ed
            9582c0dc0dee813be77f60e6ddf7213987c7e11497bf3cc66fd7b18957ae0d26
            d9649a867305e7152ea1dcc5c30f4624fcb17fe7d4f7468da00e9b1997f1bb6c
            c7e93451e25a1ee35247d265dbd06c760de904ba339890a97c248648c3170431
            1d7d24ff3eb0f54e30834bf14717c3a303cb179e41fca4829545768e0999d46a
        REPORT_ARGS
            --require-asset-entry water-bottle
            --require-asset-entry corset
            --require-asset-entry crytek-sponza
            --require-action QueueThreeModelsAndDuplicateSubscriber
            --require-action PresentFallbackFrame
            --require-action VerifyRequestCoalescing
            --require-action CancelDuplicateSubscriber
            --require-action RequestUnknownAssetAndRollback
            --require-action InstantiateMinimumResidentModels
            --require-action CommitRealTextures
            --require-action CancelPublishedLargeModel
            --require-action AwaitExactRetirement
            --require-action ReloadSameAssetId
            --require-action PresentReloadedRevision
            --require-action DrainAllQueues
            --require-invariant FirstFrameBeforeFullyResident
            --require-invariant RequestCoalescing
            --require-invariant UnknownAssetRollback
            --require-invariant IncrementalTexturePublication
            --require-invariant ExactRetirement
            --require-invariant ReloadContentIdentityAndGeneration
            --require-invariant DrainedQueues
            --require-metric-equals pendingOperations=0
            --require-metric-equals activeSubscribers=0
            --require-metric-equals pendingAsyncJobs=0
            --require-metric-equals pendingAsyncCompletions=0
            --require-metric-equals decodeQueued=0
            --require-metric-equals decodeActive=0
            --require-metric-equals pendingPublications=0
            --require-metric-equals pendingUploads=0
            --require-metric-equals pendingReplacements=0
            --require-metric-equals pendingRollbacks=0
            --require-metric-equals pendingRetirements=0
            --require-metric-equals queuedLeaseUnloads=0
            --require-metric closureUnloadRequests
            --require-supported-feature AsynchronousModelRequests
            --require-supported-feature CatalogAssetIdRequests
            --require-supported-feature CoalescedSubscriberCancellation
            --require-supported-feature UnknownAssetNotFoundRollback
            --require-supported-feature FallbackMinimumResidency
            --require-supported-feature TextureContentReplacement
            --require-supported-feature CompletionOwnedRetirement
            --require-supported-feature RetiredAssetIdReload
            --require-supported-feature DescriptorRevisionEvidence)
endfunction()

function(rvx_add_renderverse_interior_matrix BACKEND_NAME BACKEND_LABEL)
    rvx_add_renderverse_p0_matrix(
        interior-rendering InteriorRendering crytek-sponza CC-BY-3.0
        ${BACKEND_NAME} ${BACKEND_LABEL}
        ENVIRONMENT_ID kloofendal-48d-partly-cloudy-pure-sky
        ENVIRONMENT_LICENSE CC0-1.0
        ENVIRONMENT_SOURCE_URI https://dl.polyhaven.org/file/ph-assets/HDRIs/hdr/2k/kloofendal_48d_partly_cloudy_puresky_2k.hdr
        CONTENT_IDS
            c7e93451e25a1ee35247d265dbd06c760de904ba339890a97c248648c3170431
            1d7d24ff3eb0f54e30834bf14717c3a303cb179e41fca4829545768e0999d46a
            5244534e9cf5b606f2ff513aa00ddb161b0a4826ffd88a0d3bd03ac29247d198
            5976bfa6aee89beecfd51d98b8b47f625965c1df245d0816fd708860d64ab449
            f94c219679ca4b47844ab7e7f9c3012c8195f2f1002a102aed6a1a7f9249737d
            62bad1e475a107ac0941a1b19bc6231fee6de5ae5558ed12115da03e12d4af34
        REPORT_ARGS
            --require-asset-entry crytek-sponza
            --require-asset-entry kloofendal-48d-partly-cloudy-pure-sky
            --require-asset-entry interior-rendering-p0a
            --require-directional-shadow
            --require-texture-ibl
            --require-opaque-execution
            --require-opaque-materials-ready
            --require-action PresentQualifiedInteriorFrame
            --require-invariant VerifiedProductionAssetComposition
            --require-invariant DeterministicTransparentGlassPanels
            --require-invariant CompletedInteriorRenderContract
            --require-metric productionMeshCount
            --require-metric productionMaterialCount
            --require-metric productionTextureCount
            --require-metric-equals transparentGlassPanelCount=2
            --require-supported-feature AsynchronousModelRequest
            --require-supported-feature AsynchronousEnvironmentRequest
            --require-supported-feature ProductionSponzaAsset
            --require-supported-feature DeterministicTransparentGlassPanels
            --require-supported-feature LocalLightsWithoutLocalShadows
            --require-supported-feature InteriorRenderingDiagnosticsQualified
            --require-supported-feature EnvironmentResourceLoad
            --require-supported-feature EnvironmentIBL
            --require-supported-feature TransparentPassObserved
            --require-supported-feature ClusteredLightingObserved
            --require-supported-feature DirectionalCSMObserved
            --require-supported-feature TextureIBLEnabled
            --require-unsupported-feature PointShadow
            --require-unsupported-feature SpotShadow
            --require-unsupported-feature HZBOcclusion
            --require-resource-diagnostic "directional cascades requested=3"
            --require-resource-diagnostic "directional shadow map requested=2048"
            --require-resource-diagnostic "point lights requested=8"
            --require-resource-diagnostic "spot lights requested=4"
            --require-resource-diagnostic "point local-shadow requests=0"
            --require-resource-diagnostic "spot local-shadow requests=0"
            --require-resource-diagnostic "HZB requests=0")
endfunction()

function(rvx_add_renderverse_physics_sandbox_matrix BACKEND_NAME BACKEND_LABEL)
    foreach(path_label Direct GPUDriven)
        if(path_label STREQUAL "Direct")
            set(render_path direct)
            set(execution_gate --require-direct-execution)
        else()
            set(render_path gpu-driven)
            set(execution_gate --require-gpu-driven-execution)
        endif()

        rvx_add_renderverse_primary_smoke(
            physics-sandbox PhysicsSandbox${path_label}
            interior-rendering-p0a ${BACKEND_NAME} ${BACKEND_LABEL}
            TIMEOUT 300
            WAIT_READY
            ASSESSMENT
            ASSESSMENT_PROFILE qualification
            READY_TIMEOUT_MS 120000
            READY_MAX_FRAMES 1200
            SAMPLE_ARGS
                --asset interior-rendering-p0a
                --render-path ${render_path}
            REPORT_ARGS
                --render-path ${render_path}
                --require-requested-render-path ${render_path}
                --require-actual-render-path ${render_path}
                --require-requested-physics-backend built-in
                --require-actual-physics-backend built-in
                --forbid-physics-backend-fallback
                --require-native-validation-available
                --require-native-validation-enabled
                --require-native-validation-read-complete
                --require-native-validation-error-count 0
                --require-native-validation-corruption-count 0
                ${execution_gate}
                --require-readiness-wait
                --require-phase stable
                --require-asset-entry interior-rendering-p0a
                --require-asset-content-id f94c219679ca4b47844ab7e7f9c3012c8195f2f1002a102aed6a1a7f9249737d
                --require-asset-content-id 62bad1e475a107ac0941a1b19bc6231fee6de5ae5558ed12115da03e12d4af34
                --require-action CreateBodies
                --require-action Settle120Steps
                --require-action DriveKinematicPlatform120Steps
                --require-action VerifyDynamicContacts
                --require-action RebuildCollider
                --require-action DestroyBody
                --require-action RecreateSameSlot
                --require-action RejectStaleHandle
                --require-invariant BuiltInAuthority
                --require-invariant DynamicPhysicsToScene
                --require-invariant DynamicFloorContact
                --require-invariant ColliderFragmentRefresh
                --require-invariant ProbeCleanup
                --require-invariant StaleSceneRefRejected
                --require-invariant TerminalCensus
                --require-metric sceneRuntimeId
                --require-metric sceneFrameSequence
                --require-metric physicsFixedStepSequence
                --require-metric fixedSteps
                --require-metric requestedFixedSteps
                --require-metric executedFixedSteps
                --require-metric kinematicPoseWrites
                --require-metric kinematicLastTargetStep
                --require-metric dynamicFloorContactEvidence
                --require-metric-equals fixedDeltaNumerator=1
                --require-metric-equals fixedDeltaDenominator=60
                --require-metric-equals maxSubsteps=4
                --require-metric-equals deterministicSeed=1381390416
                --require-metric-equals staticBodies=1
                --require-metric-equals dynamicBodies=17
                --require-metric-equals kinematicBodies=1
                --require-metric-equals bodyCount=19
                --require-metric-equals colliderCount=19
                --require-metric-equals bridgeBindings=19
                --require-metric-equals pendingBridgeCleanup=0
                --require-metric-equals bridgeStructuralContinuityLoss=0
                --require-metric-equals bridgeCleanupContinuityLoss=0
                --require-metric-equals bridgeAuthoritativeReconcile=1
                --require-metric-equals modelFullyResident=1
                --require-metric-equals staleRefRejected=1
                --require-enabled-feature PureEcsPhysicsFragments
                --require-enabled-feature WorldEcsPhysicsDiagnostics
                --assessment-block-grade ADVISORY
                --require-resource-diagnostic "model path="
                --require-resource-diagnostic "model meshes=8"
                --require-resource-diagnostic "model materials=8"
                --require-resource-diagnostic "render path=${render_path}"
                --require-resource-diagnostic "requested backend=${BACKEND_NAME}"
                --require-resource-diagnostic "realized backend=${BACKEND_NAME}")
    endforeach()

    rvx_add_renderverse_p0_cross_path_parity(
        PhysicsSandbox ${BACKEND_NAME} ${BACKEND_LABEL})
endfunction()

function(rvx_add_renderverse_animation_character_matrix BACKEND_NAME BACKEND_LABEL)
    # Keep these records as one CTest argument each.  The escaped semicolons
    # are required because CMake otherwise treats the typed receipt fields as
    # a list before it writes CTestTestfile.cmake.
    string(JOIN "$<SEMICOLON>" casual_female_cook_admission
        "assetId=casual-female" "packageContentId=57b9e1ebfbcdb14606fc06d0aa36c13a0554ba3cfd4ebf68c165455ac88e63a3/3597846/11" "required=true" "accepted=true"
        "manifestPath=$<SHELL_PATH:$<TARGET_FILE_DIR:RenderVerseSamples>/Assets/Samples/cooked/casual-female/CookManifest.rvxmanifest>" "cookedRoot=$<SHELL_PATH:$<TARGET_FILE_DIR:RenderVerseSamples>/Assets/Samples/cooked/casual-female>"
        "declaredSource=source/self-contained-artifact/87327963ab0f37d004c7340d130808727f3d67b724c3d41aad01fd63c7d6fc8a/2058883/1" "observedSource=source/self-contained-artifact/87327963ab0f37d004c7340d130808727f3d67b724c3d41aad01fd63c7d6fc8a/2058883/1"
        "declaredCooked=cooked-artifact/dependency-closure/28267d61f327c52df64b087235ca1754da54e6ff66511e18506a6564a8cc1a6f/1534321/9" "observedCooked=cooked-artifact/dependency-closure/28267d61f327c52df64b087235ca1754da54e6ff66511e18506a6564a8cc1a6f/1534321/9"
        "declaredManifest=cook-manifest/self-contained-artifact/ecbb1e6d425bdb269d93045cdb5fd7c09b87fed6da7bec688ff485408b5d5949/4642/1" "observedManifest=cook-manifest/self-contained-artifact/ecbb1e6d425bdb269d93045cdb5fd7c09b87fed6da7bec688ff485408b5d5949/4642/1"
        "cookSettingsHash=d47a3f3b166ac705cfd06128506f127d776f43ad59eaeaa5cb4d52c1946829fa" "recipeHash=28f13886f1ecc018d5daa7501d0e17d70a52769ae3623d10f2e6c98d8452f676" "toolName=RVXCook" "toolVersion=2.0.0")
    string(JOIN "$<SEMICOLON>" root_motion_cook_admission
        "assetId=casual-female-walk-root-motion" "packageContentId=8986e195f30f749969d68648cd21b5271ad17547b30372b73fb7d6f154a8c544/56159/2" "required=true" "accepted=true"
        "manifestPath=$<SHELL_PATH:$<TARGET_FILE_DIR:RenderVerseSamples>/Assets/Samples/cooked/casual-female-root-motion/CookManifest.rvxmanifest>" "cookedRoot=$<SHELL_PATH:$<TARGET_FILE_DIR:RenderVerseSamples>/Assets/Samples/cooked/casual-female-root-motion>"
        "declaredSource=source/self-contained-artifact/36f4824d1d82aeaae42ce33208b3c774622e09d4547290c89ace8560eeb3ad11/927584/1" "observedSource=source/self-contained-artifact/36f4824d1d82aeaae42ce33208b3c774622e09d4547290c89ace8560eeb3ad11/927584/1"
        "declaredCooked=cooked-artifact/self-contained-artifact/3a7e27e1745b292b282682df0ab6fee9c4b1b1ddb8d0b080d69dc723ab122a97/54464/1" "observedCooked=cooked-artifact/self-contained-artifact/3a7e27e1745b292b282682df0ab6fee9c4b1b1ddb8d0b080d69dc723ab122a97/54464/1"
        "declaredManifest=cook-manifest/self-contained-artifact/bf369d106c2bf53a48c329f84ee7c977fa64c58d45c9c15d2426d9bf15546f2c/1695/1" "observedManifest=cook-manifest/self-contained-artifact/bf369d106c2bf53a48c329f84ee7c977fa64c58d45c9c15d2426d9bf15546f2c/1695/1"
        "cookSettingsHash=37d257abb1762b56436a0b3a1de267860f0507fbf0ff169ce395039ae2053232" "recipeHash=5d814e26fdad96238fa707f11537e7ac66f9aa335252fafe525ef59338c478a7" "toolName=RVXCook" "toolVersion=2.0.0")
    string(JOIN "$<SEMICOLON>" root_motion_asset_receipt
        "assetId=casual-female-walk-root-motion" "role=animation" "kind=animation" "packageContentId=8986e195f30f749969d68648cd21b5271ad17547b30372b73fb7d6f154a8c544/56159/2"
        "expected=cooked-artifact/self-contained-artifact/1402b3b629c587ba1168ee961029e9b8504ffac89f763592571ae836f13f9a7f/54464/1" "observed=cooked-artifact/self-contained-artifact/1402b3b629c587ba1168ee961029e9b8504ffac89f763592571ae836f13f9a7f/54464/1"
        "verificationStatus=verified" "loaded=true" "verified=true")

    foreach(path_label Direct GPUDriven)
        if(path_label STREQUAL "Direct")
            set(render_path direct)
            set(execution_gate --require-direct-execution)
            set(request_feature DirectPathRequested)
        else()
            set(render_path gpu-driven)
            set(execution_gate --require-gpu-driven-execution)
            set(request_feature GPUDrivenPathRequested)
        endif()

        rvx_add_renderverse_primary_smoke(
            animation-character AnimationCharacter${path_label}
            casual-female ${BACKEND_NAME} ${BACKEND_LABEL}
            TIMEOUT 300
            WAIT_READY
            ASSESSMENT
            ASSESSMENT_PROFILE qualification
            ASSET_LICENSE CC0-1.0
            READY_TIMEOUT_MS 120000
            READY_MAX_FRAMES 2400
            SAMPLE_ARGS
                --render-path ${render_path}
            REPORT_ARGS
                --render-path ${render_path}
                --require-requested-render-path ${render_path}
                --require-actual-render-path ${render_path}
                --require-requested-physics-backend built-in
                --require-actual-physics-backend built-in
                --forbid-physics-backend-fallback
                --require-native-validation-available
                --require-native-validation-enabled
                --require-native-validation-read-complete
                --require-native-validation-error-count 0
                --require-native-validation-corruption-count 0
                ${execution_gate}
                --require-readiness-wait
                --require-phase stable
                --min-visible-objects 7
                --require-asset-entry casual-female
                --require-asset-entry casual-female-walk-root-motion
                --require-asset-entry water-bottle
                --require-asset-content-id 87327963ab0f37d004c7340d130808727f3d67b724c3d41aad01fd63c7d6fc8a
                --require-asset-content-id 57b9e1ebfbcdb14606fc06d0aa36c13a0554ba3cfd4ebf68c165455ac88e63a3
                --require-asset-content-id b337e526fd6a162013c2984aeec163f5fbb4f717252724dfc3f3458bd51df94b
                --require-asset-content-id 4df540b802067f192a7aa38aed524dda141dc23d9cb7a7e6272fc021694198ed
                --require-cook-admission "${casual_female_cook_admission}"
                --require-cook-admission "${root_motion_cook_admission}"
                --require-asset-receipt "${root_motion_asset_receipt}"
                --require-action BindEcsCharacter
                --require-action EvaluateFixed120
                --require-action ConsumeRootMotion120
                --require-action PresentSkinningPalette
                --require-invariant ExactEcsBinding
                --require-invariant FixedSchedule
                --require-invariant RootMotionEpoch
                --require-invariant PresentedPalette
                --require-metric fixedSteps
                --require-metric poseEvaluations
                --require-metric rootMotionIntents
                --require-metric rootMotionMillimetres
                --require-metric paletteHash
                --require-metric-equals fixedSteps=120
                --require-metric-equals poseEvaluations=120
                --require-metric-equals rootMotionIntents=120
                --require-metric-equals rootMotionMillimetres=2000
                --require-enabled-feature ModelResourceLoad
                --require-enabled-feature SceneInstantiation
                --require-enabled-feature EcsCharacterModel
                --require-enabled-feature EcsSkinningPalette
                --require-enabled-feature EcsAnimationAsset
                --require-enabled-feature EcsRootMotion
                --require-enabled-feature EcsAnimationPhysicsBinding
                --require-enabled-feature EcsFixedAnimation
                --require-enabled-feature EcsPresentedSkinningPalette
                --require-enabled-feature RenderPathPolicySelection
                --require-enabled-feature ${request_feature}
                --require-unsupported-feature Jolt
                --require-resource-diagnostic "fixed steps=120"
                --require-resource-diagnostic "root-motion intents=120"
                --require-resource-diagnostic "render path=${render_path}")
    endforeach()

    rvx_add_renderverse_p0_cross_path_parity(
        AnimationCharacter ${BACKEND_NAME} ${BACKEND_LABEL})

endfunction()

function(rvx_add_renderverse_pbr_response_validation BACKEND_NAME BACKEND_LABEL SAMPLE_LABEL)
    if(NOT TARGET ImageContentValidation)
        return()
    endif()

    set(test_prefix "RenderVerseSamples${BACKEND_LABEL}${SAMPLE_LABEL}")
    set(smoke_actual
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/${test_prefix}_320x180.ppm")
    rvx_add_validation_test(
        LANE qualification
        NAME ${test_prefix}ResponseValidation
        COMMAND $<TARGET_FILE:ImageContentValidation>
            --image "${smoke_actual}"
            --min-width 320
            --min-height 180
            --require-pbr-grid-response
            --min-pbr-metallic-luma-delta 40
            --min-pbr-roughness-contrast-delta 2
            --sample-step 2
        BUILD_TARGETS ImageContentValidation
        TAGS product.samples trait.visual trait.gpu backend.${BACKEND_NAME} suite.primary suite.pbr
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        FIXTURES_REQUIRED "${test_prefix}FreshCapture"
    )
endfunction()

function(rvx_add_renderverse_pbr_texture_smoke BACKEND_NAME BACKEND_LABEL)
    rvx_add_renderverse_primary_smoke(
        pbr-materials PBRMaterialsTexture pbr-material-texture-cube
        ${BACKEND_NAME} ${BACKEND_LABEL}
        WAIT_READY
        SAMPLE_ARGS
            --asset pbr-material-texture-cube
        REPORT_ARGS
        --require-enabled-feature PBRMetallicRoughnessMaterials
        --require-enabled-feature PBRMetallicRoughnessTexture
        --require-enabled-feature PBRMaterialParameterCube
        --require-enabled-feature PBRBaseColorFamilyAxis
        --require-enabled-feature MetallicRoughnessTextureCube
        --require-opaque-execution
        --require-opaque-materials-ready
        --require-opaque-material-texture-flags 4
        --environment-asset-id pbr-reference-environment
        --environment-license-spdx LicenseRef-RenderVerseX-Generated-Fixture
        --environment-source-uri repo://Samples/Common/Tools/SampleHDRFixtureWriter.cpp
        --require-environment-redistributable
        --require-texture-ibl
        --forbid-fallback-reason PacketEnvironmentResourcesUnavailable
        --require-enabled-feature EnvironmentResourceLoad
        --require-enabled-feature EnvironmentIBL
        --require-enabled-feature DiffuseIBL
        --require-enabled-feature SpecularIBL
        --require-enabled-feature TextureIBL
        --require-enabled-feature ControlledStudioLighting
        --require-enabled-feature BoundsCameraFraming
        --require-resource-diagnostic "pbr materials=5"
        --require-resource-diagnostic "metallic-roughness texture=5x5"
        --require-resource-diagnostic "metallic levels=5"
        --require-resource-diagnostic "roughness levels=5"
        --require-resource-diagnostic "base-color families=5"
        --require-resource-diagnostic "matrix shape=5x5x5"
        --require-resource-diagnostic "total material spheres=125"
        --require-resource-diagnostic "matrix x-axis=metallic 0.00,0.25,0.50,0.75,1.00"
        --require-resource-diagnostic "matrix y-axis=roughness 0.05,0.25,0.50,0.75,1.00"
        --require-resource-diagnostic "matrix z-axis=base-color terracotta,gold,green,blue,neutral"
        --require-resource-diagnostic "material workflow=metallic-roughness texture"
        --require-resource-diagnostic "texture cube materials=5"
        --require-resource-diagnostic "texture cube combinations=125"
    )
    rvx_add_renderverse_pbr_response_validation(
        ${BACKEND_NAME} ${BACKEND_LABEL} PBRMaterialsTexture)
endfunction()

function(rvx_add_renderverse_pbr_workflow_parity BACKEND_NAME BACKEND_LABEL)
    if(NOT TARGET VisualGoldenValidation)
        return()
    endif()

    set(factor_capture
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/RenderVerseSamples${BACKEND_LABEL}PBRMaterials_320x180.ppm")
    set(texture_capture
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/RenderVerseSamples${BACKEND_LABEL}PBRMaterialsTexture_320x180.ppm")
    set(diff_capture
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/RenderVerseSamples${BACKEND_LABEL}PBRMaterialsWorkflowDiff.ppm")
    set(parity_report
        "${CMAKE_CURRENT_BINARY_DIR}/SampleReports/$<CONFIG>/RenderVerseSamples/RenderVerseSamples${BACKEND_LABEL}PBRMaterialsWorkflowParity.json")
    rvx_add_validation_test(
        LANE qualification
        NAME RenderVerseSamples${BACKEND_LABEL}PBRMaterialsWorkflowParity
        COMMAND $<TARGET_FILE:VisualGoldenValidation>
            --expected "${factor_capture}"
            --actual "${texture_capture}"
            --diff "${diff_capture}"
            --report "${parity_report}"
            --tolerance 0.0
            --max-different-pixels 0
        BUILD_TARGETS VisualGoldenValidation
        TAGS product.samples trait.visual trait.gpu backend.${BACKEND_NAME} suite.primary suite.pbr suite.parity
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        FIXTURES_REQUIRED
            "RenderVerseSamples${BACKEND_LABEL}PBRMaterialsFreshCapture;RenderVerseSamples${BACKEND_LABEL}PBRMaterialsTextureFreshCapture"
    )
endfunction()

function(rvx_add_renderverse_external_model_viewer_validation BACKEND_NAME BACKEND_LABEL)
    if(NOT RVX_ENABLE_EXTERNAL_ASSET_TESTS OR
       NOT TARGET RenderVerseSamples OR
       NOT TARGET ImageContentValidation OR
       NOT TARGET SampleReportContentValidation OR
       NOT TARGET SampleQualificationReportValidation)
        return()
    endif()
    if(NOT RVX_EXTERNAL_TEST_ASSET_ROOT)
        message(FATAL_ERROR
            "RVX_ENABLE_EXTERNAL_ASSET_TESTS requires RVX_EXTERNAL_TEST_ASSET_ROOT")
    endif()

    cmake_path(ABSOLUTE_PATH RVX_EXTERNAL_TEST_ASSET_ROOT
        NORMALIZE OUTPUT_VARIABLE RVX_EXTERNAL_MODEL_ROOT)
    set(external_catalog "${RVX_EXTERNAL_MODEL_ROOT}/catalog.json")
    set(external_model
        "${RVX_EXTERNAL_MODEL_ROOT}/models/free_1975_porsche_911_930_turbo/scene.gltf")
    set(external_license
        "${RVX_EXTERNAL_MODEL_ROOT}/models/free_1975_porsche_911_930_turbo/license.txt")
    foreach(required_file
            "${external_catalog}"
            "${external_model}"
            "${external_license}")
        if(NOT EXISTS "${required_file}")
            message(FATAL_ERROR
                "External Model Viewer gate requires ${required_file}")
        endif()
    endforeach()

    foreach(config_name Debug Release RelWithDebInfo MinSizeRel)
        file(MAKE_DIRECTORY
            "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/${config_name}/ExternalAssets")
        file(MAKE_DIRECTORY
            "${CMAKE_CURRENT_BINARY_DIR}/SampleReports/${config_name}/ExternalAssets")
    endforeach()

    set(test_prefix
        "RenderVerseSamples${BACKEND_LABEL}ExternalPorscheModelViewer")
    set(capture
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/ExternalAssets/${test_prefix}_320x180.ppm")
    set(report
        "${CMAKE_CURRENT_BINARY_DIR}/SampleReports/$<CONFIG>/ExternalAssets/${test_prefix}_320x180.json")

    rvx_add_validation_test(
        LANE qualification
        NAME ${test_prefix}Validation
        COMMAND $<TARGET_FILE:RenderVerseSamples>
            --sample model-viewer
            --asset free-1975-porsche-911-930-turbo
            --catalog "${external_catalog}"
            --asset-root "${RVX_EXTERNAL_MODEL_ROOT}"
            --backend ${BACKEND_NAME}
            --smoke
            --frames 1
            --wait-ready
            --ready-timeout-ms 180000
            --ready-max-frames 4096
            --width 320
            --height 180
            --quality low
            --screenshot "${capture}"
            --report "${report}"
            --diagnostics
            --validation
        BUILD_TARGETS RenderVerseSamples ImageContentValidation SampleReportContentValidation SampleQualificationReportValidation
        TAGS product.samples trait.visual trait.gpu trait.external-assets backend.${BACKEND_NAME} suite.model-viewer suite.qualification
        TIMEOUT 300
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:RenderVerseSamples>"
        RESOURCE_LOCK rvx_gpu
        FIXTURES_SETUP "${test_prefix}FreshCapture"
    )

    rvx_add_validation_test(
        LANE qualification
        NAME ${test_prefix}ImageContentValidation
        COMMAND $<TARGET_FILE:ImageContentValidation>
            --image "${capture}"
            --min-width 320
            --min-height 180
            --min-unique-colors 16
            --min-luma-range 8
            --near-black-threshold 8
            --max-near-black-ratio 0.95
            --sample-step 2
        BUILD_TARGETS ImageContentValidation
        TAGS product.samples trait.visual trait.gpu trait.external-assets backend.${BACKEND_NAME} suite.model-viewer suite.qualification
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        FIXTURES_REQUIRED "${test_prefix}FreshCapture"
    )

    rvx_add_validation_test(
        LANE qualification
        NAME ${test_prefix}ReportValidation
        COMMAND $<TARGET_FILE:SampleReportContentValidation>
            --report "${report}"
            --sample-name model-viewer
            --category visual
            --backend ${BACKEND_NAME}
            --quality low
            --min-frame-count 1
            --min-visible-objects 50
            --asset-id free-1975-porsche-911-930-turbo
            --asset-license-spdx CC-BY-4.0
            --require-redistributable
            --require-opaque-execution
            --require-opaque-materials-ready
            --require-ready
            --require-readiness-wait
            --require-enabled-feature ModelResourceLoad
            --require-enabled-feature SceneInstantiation
            --require-enabled-feature ModelInspection
            --require-enabled-feature ModelRenderReady
            --require-enabled-feature BoundsCameraFraming
            --require-enabled-feature OrbitCamera
            --require-resource-diagnostic "model meshes=75"
            --require-resource-diagnostic "model materials=14"
            --require-resource-diagnostic "model nodes=141"
        BUILD_TARGETS SampleReportContentValidation
        TAGS product.samples trait.visual trait.gpu trait.external-assets backend.${BACKEND_NAME} suite.model-viewer suite.qualification
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        FIXTURES_REQUIRED "${test_prefix}FreshCapture"
    )

    if(TARGET RVXCook)
        set(cooked_output_root
            "${CMAKE_BINARY_DIR}/artifacts/$<CONFIG>/ModelViewerCookedPorsche")
        set(cooked_model "${cooked_output_root}/scene.rva")
        set(cooked_fixture RenderVerseSamplesExternalPorscheCookedModelFixture)
    endif()
    if(TARGET RVXCook AND NOT TEST ${cooked_fixture})
        rvx_add_validation_test(
            LANE qualification
            NAME ${cooked_fixture}
            COMMAND $<TARGET_FILE:RVXCook>
                --source "${RVX_EXTERNAL_MODEL_ROOT}/models/free_1975_porsche_911_930_turbo"
                --output "${cooked_output_root}"
                --manifest "${cooked_output_root}/CookManifest.rvxmanifest"
                --model-only
                --non-recursive
                --fail-on-errors
            BUILD_TARGETS RVXCook
            TAGS product.tools subsystem.resource trait.external-assets suite.cook suite.model suite.qualification
            TIMEOUT 600
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            FIXTURES_SETUP "${cooked_fixture}"
        )
    endif()

    foreach(asset_mode source cooked)
        if(asset_mode STREQUAL "cooked" AND NOT TARGET RVXCook)
            continue()
        endif()
        if(asset_mode STREQUAL "source")
            set(mode_label Source)
            set(model_arguments
                --asset free-1975-porsche-911-930-turbo
                --catalog "${external_catalog}"
                --asset-root "${RVX_EXTERNAL_MODEL_ROOT}")
            set(mode_run_arguments --smoke)
            set(mode_fixture "")
        else()
            set(mode_label Cooked)
            set(model_arguments --model "${cooked_model}")
            # The generated model product is intentionally outside the source
            # catalog. It is still validation-enabled, but cannot claim the
            # catalog-backed redistributable provenance required by --smoke.
            set(mode_run_arguments "")
            set(mode_fixture ${cooked_fixture})
        endif()

        set(direct_capture "")
        set(gpu_capture "")
        set(direct_lifetime_test "")
        set(gpu_lifetime_test "")
        foreach(render_path direct gpu-driven)
            if(render_path STREQUAL "direct")
                set(path_label Direct)
            else()
                set(path_label GPUDriven)
            endif()
            if(asset_mode STREQUAL "source")
                # Preserve the established source qualification test names.
                set(lifetime_prefix "${test_prefix}${path_label}")
            else()
                set(lifetime_prefix "${test_prefix}${mode_label}${path_label}")
            endif()
            set(lifetime_report
                "${CMAKE_BINARY_DIR}/artifacts/$<CONFIG>/ModelViewerLifetime/${lifetime_prefix}_600.json")
            set(lifetime_sample_report
                "${CMAKE_BINARY_DIR}/artifacts/$<CONFIG>/ModelViewerLifetime/${lifetime_prefix}_sample.json")
            set(startup_report
                "${CMAKE_BINARY_DIR}/artifacts/$<CONFIG>/ModelViewerStartup/${lifetime_prefix}.json")
            set(lifetime_capture
                "${CMAKE_BINARY_DIR}/artifacts/$<CONFIG>/ModelViewerLifetime/${lifetime_prefix}_320x180.ppm")
            rvx_add_validation_test(
                LANE qualification
                NAME ${lifetime_prefix}Lifetime600
                COMMAND $<TARGET_FILE:RenderVerseSamples>
                    --sample model-viewer
                    ${model_arguments}
                    --backend ${BACKEND_NAME}
                    --render-path ${render_path}
                    ${mode_run_arguments}
                    --frames 1
                    --wait-ready
                    --ready-timeout-ms 180000
                    --ready-max-frames 4096
                    --width 320
                    --height 180
                    --quality low
                    --deterministic-orbit
                    --startup-report "${startup_report}"
                    --lifetime-warmup-frames 120
                    --lifetime-observation-frames 480
                    --lifetime-resize-interval 240
                    --lifetime-resize-settle-frames 30
                    --lifetime-report "${lifetime_report}"
                    --screenshot "${lifetime_capture}"
                    --report "${lifetime_sample_report}"
                    --diagnostics
                    --validation
                BUILD_TARGETS RenderVerseSamples SampleQualificationReportValidation
                TAGS product.samples trait.visual trait.gpu trait.external-assets backend.${BACKEND_NAME} suite.model-viewer suite.lifetime suite.qualification suite.${asset_mode} suite.${render_path}
                TIMEOUT 1200
                WORKING_DIRECTORY "$<TARGET_FILE_DIR:RenderVerseSamples>"
                RESOURCE_LOCK rvx_gpu
                FIXTURES_SETUP "${lifetime_prefix}Artifacts"
                FIXTURES_REQUIRED "${mode_fixture}"
            )

            set(qualification_test ${lifetime_prefix}QualificationValidation)
            rvx_add_validation_test(
                LANE qualification
                NAME ${qualification_test}
                COMMAND $<TARGET_FILE:SampleQualificationReportValidation>
                    --startup-report "${startup_report}"
                    --lifetime-report "${lifetime_report}"
                    --sample-report "${lifetime_sample_report}"
                    --backend ${BACKEND_NAME}
                    --render-path ${render_path}
                    --asset-mode ${asset_mode}
                    --expected-texture-count 27
                    --expected-visible-objects 75
                    --max-first-present-ms 5000
                    --require-resize
                BUILD_TARGETS SampleQualificationReportValidation
                TAGS product.samples trait.diagnostics backend.${BACKEND_NAME} suite.model-viewer suite.startup suite.residency suite.lifetime suite.qualification suite.${asset_mode} suite.${render_path}
                TIMEOUT 30
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                FIXTURES_REQUIRED "${lifetime_prefix}Artifacts"
            )

            if(render_path STREQUAL "direct")
                set(direct_capture "${lifetime_capture}")
                set(direct_lifetime_test ${lifetime_prefix}Lifetime600)
            else()
                set(gpu_capture "${lifetime_capture}")
                set(gpu_lifetime_test ${lifetime_prefix}Lifetime600)
            endif()

            if(RVX_ENABLE_LONG_RUNNING_QUALIFICATION_TESTS)
                set(soak_report
                    "${CMAKE_BINARY_DIR}/artifacts/$<CONFIG>/ModelViewerLifetime/${lifetime_prefix}_10000.json")
                set(soak_sample_report
                    "${CMAKE_BINARY_DIR}/artifacts/$<CONFIG>/ModelViewerLifetime/${lifetime_prefix}_10000_sample.json")
                set(soak_startup_report
                    "${CMAKE_BINARY_DIR}/artifacts/$<CONFIG>/ModelViewerStartup/${lifetime_prefix}_10000.json")
                rvx_add_validation_test(
                    LANE qualification
                    NAME ${lifetime_prefix}Lifetime10000
                    COMMAND $<TARGET_FILE:RenderVerseSamples>
                        --sample model-viewer
                        ${model_arguments}
                        --backend ${BACKEND_NAME}
                        --render-path ${render_path}
                        ${mode_run_arguments}
                        --frames 1
                        --wait-ready
                        --ready-timeout-ms 180000
                        --ready-max-frames 4096
                        --width 320
                        --height 180
                        --quality low
                        --deterministic-orbit
                        --startup-report "${soak_startup_report}"
                        --lifetime-warmup-frames 120
                        --lifetime-observation-frames 9880
                        --lifetime-min-duration-ms 300000
                        --lifetime-resize-interval 2400
                        --lifetime-resize-settle-frames 60
                        --lifetime-report "${soak_report}"
                        --report "${soak_sample_report}"
                        --diagnostics
                        --validation
                    BUILD_TARGETS RenderVerseSamples
                    TAGS product.samples trait.visual trait.gpu trait.external-assets backend.${BACKEND_NAME} suite.model-viewer suite.lifetime suite.soak suite.qualification suite.${asset_mode} suite.${render_path}
                    TIMEOUT 3600
                    WORKING_DIRECTORY "$<TARGET_FILE_DIR:RenderVerseSamples>"
                    RESOURCE_LOCK rvx_gpu
                    FIXTURES_REQUIRED "${mode_fixture}"
                )
            endif()
        endforeach()

        if(TARGET VisualGoldenValidation)
            set(path_parity_report
                "${CMAKE_BINARY_DIR}/artifacts/$<CONFIG>/ModelViewerLifetime/${test_prefix}${mode_label}DirectGPUParity.json")
            set(path_parity_diff
                "${CMAKE_BINARY_DIR}/artifacts/$<CONFIG>/ModelViewerLifetime/${test_prefix}${mode_label}DirectGPUParity.diff.ppm")
            rvx_add_validation_test(
                LANE qualification
                NAME ${test_prefix}${mode_label}DirectGPUParity
                COMMAND $<TARGET_FILE:VisualGoldenValidation>
                    --expected "${direct_capture}"
                    --actual "${gpu_capture}"
                    --diff "${path_parity_diff}"
                    --report "${path_parity_report}"
                    --tolerance 0.0
                    --max-different-pixels 0
                BUILD_TARGETS VisualGoldenValidation
                TAGS product.samples trait.visual trait.gpu trait.external-assets backend.${BACKEND_NAME} suite.model-viewer suite.lifetime suite.parity suite.qualification suite.${asset_mode}
                TIMEOUT 30
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                FIXTURES_REQUIRED
                    "${direct_lifetime_test}Artifacts;${gpu_lifetime_test}Artifacts"
            )
        endif()
    endforeach()
endfunction()

function(rvx_add_renderverse_external_environment_validation BACKEND_NAME BACKEND_LABEL)
    if(NOT RVX_ENABLE_EXTERNAL_ENVIRONMENT_TESTS OR
       NOT TARGET RenderVerseSamples OR
       NOT TARGET ImageContentValidation OR
       NOT TARGET SampleReportContentValidation)
        return()
    endif()
    if(NOT RVX_ENABLE_EXTERNAL_ASSET_TESTS)
        message(FATAL_ERROR
            "RVX_ENABLE_EXTERNAL_ENVIRONMENT_TESTS requires RVX_ENABLE_EXTERNAL_ASSET_TESTS")
    endif()
    if(NOT RVX_EXTERNAL_TEST_ASSET_ROOT)
        message(FATAL_ERROR
            "RVX_ENABLE_EXTERNAL_ASSET_TESTS requires RVX_EXTERNAL_TEST_ASSET_ROOT")
    endif()

    cmake_path(ABSOLUTE_PATH RVX_EXTERNAL_TEST_ASSET_ROOT
        NORMALIZE OUTPUT_VARIABLE RVX_EXTERNAL_ENVIRONMENT_ROOT)
    set(external_catalog "${RVX_EXTERNAL_ENVIRONMENT_ROOT}/catalog.json")
    set(external_environment
        "${RVX_EXTERNAL_ENVIRONMENT_ROOT}/textures/valley_of_desolation_4k.exr")
    set(external_catalog_companion
        "${RVX_EXTERNAL_ENVIRONMENT_ROOT}/textures/cowboy_town_hall_8k.exr")
    set(external_license
        "${RVX_EXTERNAL_ENVIRONMENT_ROOT}/LICENSE.polyhaven-cc0.txt")
    foreach(required_file
            "${external_catalog}"
            "${external_environment}"
            "${external_catalog_companion}"
            "${external_license}")
        if(NOT EXISTS "${required_file}")
            message(FATAL_ERROR
                "External environment gate requires ${required_file}")
        endif()
    endforeach()

    foreach(config_name Debug Release RelWithDebInfo MinSizeRel)
        file(MAKE_DIRECTORY
            "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/${config_name}/ExternalAssets")
        file(MAKE_DIRECTORY
            "${CMAKE_CURRENT_BINARY_DIR}/SampleReports/${config_name}/ExternalAssets")
    endforeach()

    set(test_prefix
        "RenderVerseSamples${BACKEND_LABEL}ExternalPolyHavenPBR")
    set(capture
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/ExternalAssets/${test_prefix}_320x180.ppm")
    set(report
        "${CMAKE_CURRENT_BINARY_DIR}/SampleReports/$<CONFIG>/ExternalAssets/${test_prefix}_320x180.json")

    rvx_add_validation_test(
        LANE qualification
        NAME ${test_prefix}Validation
        COMMAND $<TARGET_FILE:RenderVerseSamples>
            --sample pbr-materials
            --model "${CMAKE_CURRENT_SOURCE_DIR}/Fixtures/Samples/PBRMaterialGrid.gltf"
            --environment valley-of-desolation-4k
            --catalog "${external_catalog}"
            --asset-root "${RVX_EXTERNAL_ENVIRONMENT_ROOT}"
            --backend ${BACKEND_NAME}
            --frames 8
            --wait-ready
            --ready-timeout-ms 120000
            --ready-max-frames 120
            --width 320
            --height 180
            --quality low
            --screenshot "${capture}"
            --report "${report}"
            --diagnostics
            --validation
        BUILD_TARGETS RenderVerseSamples ImageContentValidation SampleReportContentValidation
        TAGS product.samples trait.visual trait.gpu trait.external-assets backend.${BACKEND_NAME} suite.pbr suite.qualification
        TIMEOUT 300
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:RenderVerseSamples>"
        RESOURCE_LOCK rvx_gpu
        FIXTURES_SETUP "${test_prefix}FreshCapture"
    )

    rvx_add_validation_test(
        LANE qualification
        NAME ${test_prefix}ImageContentValidation
        COMMAND $<TARGET_FILE:ImageContentValidation>
            --image "${capture}"
            --min-width 320
            --min-height 180
            --min-unique-colors 16
            --min-luma-range 8
            --near-black-threshold 8
            --max-near-black-ratio 0.95
            --sample-step 2
        BUILD_TARGETS ImageContentValidation
        TAGS product.samples trait.visual trait.gpu trait.external-assets backend.${BACKEND_NAME} suite.pbr suite.qualification
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        FIXTURES_REQUIRED "${test_prefix}FreshCapture"
    )

    rvx_add_validation_test(
        LANE qualification
        NAME ${test_prefix}ReportValidation
        COMMAND $<TARGET_FILE:SampleReportContentValidation>
            --report "${report}"
            --sample-name pbr-materials
            --category visual
            --backend ${BACKEND_NAME}
            --quality low
            --min-frame-count 8
            --min-visible-objects 1
            --environment-asset-id valley-of-desolation-4k
            --environment-license-spdx CC0-1.0
            --environment-source-uri https://polyhaven.com/a/valley_of_desolation
            --require-environment-redistributable
            --require-opaque-execution
            --require-opaque-materials-ready
            --require-texture-ibl
            --require-ready
            --require-readiness-wait
            --require-enabled-feature PBRMetallicRoughnessMaterials
            --require-enabled-feature EnvironmentResourceLoad
            --require-enabled-feature EnvironmentIBL
            --require-enabled-feature DiffuseIBL
            --require-enabled-feature SpecularIBL
        BUILD_TARGETS SampleReportContentValidation
        TAGS product.samples trait.visual trait.gpu trait.external-assets backend.${BACKEND_NAME} suite.pbr suite.qualification
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        FIXTURES_REQUIRED "${test_prefix}FreshCapture"
    )
endfunction()

function(rvx_add_renderverse_readiness_smoke BACKEND_NAME BACKEND_LABEL)
    if(NOT TARGET RenderVerseSamples OR
       NOT TARGET ImageContentValidation OR
       NOT TARGET SampleReportContentValidation)
        return()
    endif()

    set(test_prefix
        "RenderVerseSamples${BACKEND_LABEL}PBRMaterialsReadiness")
    set(smoke_actual
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/${test_prefix}_320x180.ppm")
    set(smoke_report
        "${CMAKE_CURRENT_BINARY_DIR}/SampleReports/$<CONFIG>/RenderVerseSamples/${test_prefix}_320x180.json")

    rvx_add_validation_test(
        LANE qualification
        NAME ${test_prefix}Smoke
        COMMAND $<TARGET_FILE:RenderVerseSamples>
            --sample pbr-materials
            --backend ${BACKEND_NAME}
            --smoke
            --frames 1
            --wait-ready
            --ready-timeout-ms 60000
            --ready-max-frames 192
            --width 320
            --height 180
            --quality low
            --screenshot "${smoke_actual}"
            --report "${smoke_report}"
            --diagnostics
            --validation
        BUILD_TARGETS RenderVerseSamples ImageContentValidation SampleReportContentValidation
        TAGS product.samples trait.visual trait.gpu backend.${BACKEND_NAME} suite.primary suite.readiness
        TIMEOUT 120
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:RenderVerseSamples>"
        RESOURCE_LOCK rvx_gpu
        FIXTURES_SETUP "${test_prefix}FreshCapture"
    )

    rvx_add_validation_test(
        LANE qualification
        NAME ${test_prefix}ImageContentValidation
        COMMAND $<TARGET_FILE:ImageContentValidation>
            --image "${smoke_actual}"
            --min-width 320
            --min-height 180
            --min-unique-colors 16
            --min-luma-range 8
            --near-black-threshold 8
            --max-near-black-ratio 0.95
            --sample-step 2
        BUILD_TARGETS ImageContentValidation
        TAGS product.samples trait.visual trait.gpu backend.${BACKEND_NAME} suite.primary suite.readiness
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        FIXTURES_REQUIRED "${test_prefix}FreshCapture"
    )

    rvx_add_validation_test(
        LANE qualification
        NAME ${test_prefix}ReportValidation
        COMMAND $<TARGET_FILE:SampleReportContentValidation>
            --report "${smoke_report}"
            --sample-name pbr-materials
            --category visual
            --backend ${BACKEND_NAME}
            --quality low
            --min-frame-count 2
            --min-visible-objects 1
            --environment-asset-id pbr-reference-environment
            --environment-license-spdx LicenseRef-RenderVerseX-Generated-Fixture
            --environment-source-uri repo://Samples/Common/Tools/SampleHDRFixtureWriter.cpp
            --require-environment-redistributable
            --require-opaque-execution
            --require-opaque-materials-ready
            --require-texture-ibl
            --require-ready
            --require-readiness-wait
        BUILD_TARGETS SampleReportContentValidation
        TAGS product.samples trait.visual trait.gpu backend.${BACKEND_NAME} suite.primary suite.readiness
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        FIXTURES_REQUIRED "${test_prefix}FreshCapture"
    )
endfunction()

function(rvx_add_renderverse_gpu_driven_parity BACKEND_NAME BACKEND_LABEL)
    if(NOT RVX_ENABLE_P0_QUALIFICATION_MATRIX)
        return()
    endif()
    foreach(required_target
            RenderVerseSamples
            ImageContentValidation
            SampleReportContentValidation
            VisualGoldenValidation)
        if(NOT TARGET ${required_target})
            message(FATAL_ERROR
                "RVX_ENABLE_P0_QUALIFICATION_MATRIX requires ${required_target} for the gpu-driven sample matrix")
        endif()
    endforeach()

    set(test_prefix "RenderVerseSamples${BACKEND_LABEL}GPUDriven")
    set(gpu_capture
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/${test_prefix}_GPU_320x180.ppm")
    set(direct_capture
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/${test_prefix}_Direct_320x180.ppm")
    set(parity_diff
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/${test_prefix}_Parity_320x180.diff.ppm")
    set(parity_report
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/${test_prefix}_Parity_320x180.json")
    set(gpu_report
        "${CMAKE_CURRENT_BINARY_DIR}/SampleReports/$<CONFIG>/RenderVerseSamples/${test_prefix}_GPU_320x180.json")
    set(direct_report
        "${CMAKE_CURRENT_BINARY_DIR}/SampleReports/$<CONFIG>/RenderVerseSamples/${test_prefix}_Direct_320x180.json")

    foreach(render_path gpu-driven direct)
        if(render_path STREQUAL "gpu-driven")
            set(path_label GPU)
            set(capture "${gpu_capture}")
            set(report "${gpu_report}")
            set(execution_gate --require-gpu-driven-execution)
            set(request_feature GPUDrivenPathRequested)
            set(render_path_args --render-path gpu-driven)
        else()
            set(path_label Direct)
            set(capture "${direct_capture}")
            set(report "${direct_report}")
            set(execution_gate --require-direct-execution)
            set(request_feature DirectPathRequested)
            set(render_path_args --render-path direct)
        endif()

        rvx_add_validation_test(
            LANE qualification
            NAME ${test_prefix}${path_label}Smoke
            COMMAND $<TARGET_FILE:RenderVerseSamples>
                --sample gpu-driven
                ${render_path_args}
                --backend ${BACKEND_NAME}
                --smoke
                --frames 1
                --wait-ready
                --ready-timeout-ms 30000
                --ready-max-frames 64
                --width 320
                --height 180
                --quality low
                --screenshot "${capture}"
                --report "${report}"
                --diagnostics
                --validation
            BUILD_TARGETS RenderVerseSamples ImageContentValidation SampleReportContentValidation VisualGoldenValidation
            TAGS product.samples trait.visual trait.gpu backend.${BACKEND_NAME} suite.primary suite.gpu-driven suite.parity
            TIMEOUT 120
            WORKING_DIRECTORY "$<TARGET_FILE_DIR:RenderVerseSamples>"
            RESOURCE_LOCK rvx_gpu
            FIXTURES_SETUP "${test_prefix}${path_label}FreshCapture"
        )

        rvx_add_validation_test(
            LANE qualification
            NAME ${test_prefix}${path_label}ImageContentValidation
            COMMAND $<TARGET_FILE:ImageContentValidation>
                --image "${capture}"
                --min-width 320
                --min-height 180
                --min-unique-colors 16
                --min-luma-range 8
                --near-black-threshold 8
                --max-near-black-ratio 0.95
                --sample-step 2
            BUILD_TARGETS ImageContentValidation
            TAGS product.samples trait.visual trait.gpu backend.${BACKEND_NAME} suite.primary suite.gpu-driven suite.parity
            TIMEOUT 30
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            FIXTURES_REQUIRED "${test_prefix}${path_label}FreshCapture"
        )

        rvx_add_validation_test(
            LANE qualification
            NAME ${test_prefix}${path_label}ReportValidation
            COMMAND $<TARGET_FILE:SampleReportContentValidation>
                --report "${report}"
                --sample-name gpu-driven
                --category visual
                --backend ${BACKEND_NAME}
                --quality low
                --render-path ${render_path}
                --require-requested-render-path ${render_path}
                --require-actual-render-path ${render_path}
                --min-frame-count 2
                --asset-id water-bottle
                --asset-license-spdx CC0-1.0
                --require-redistributable
                --require-asset-content-id b337e526fd6a162013c2984aeec163f5fbb4f717252724dfc3f3458bd51df94b
                --require-asset-content-id 4df540b802067f192a7aa38aed524dda141dc23d9cb7a7e6272fc021694198ed
                --min-visible-objects 25
                --require-ready
                --require-readiness-wait
                --require-enabled-feature ModelResourceLoad
                --require-enabled-feature SceneInstantiation
                --require-enabled-feature SharedSceneExtractionInput
                --require-enabled-feature SharedModelResourceInstancing
                --require-enabled-feature RenderPathPolicySelection
                --require-enabled-feature ${request_feature}
                --require-resource-diagnostic "scene model instances=25"
                --require-resource-diagnostic "unique model resources=1"
                ${execution_gate}
            BUILD_TARGETS SampleReportContentValidation
            TAGS product.samples trait.visual trait.gpu backend.${BACKEND_NAME} suite.primary suite.gpu-driven suite.parity
            TIMEOUT 30
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            FIXTURES_REQUIRED "${test_prefix}${path_label}FreshCapture"
        )
    endforeach()

    rvx_add_validation_test(
        LANE qualification
        NAME ${test_prefix}CrossPathParityValidation
        COMMAND $<TARGET_FILE:VisualGoldenValidation>
            --expected "${gpu_capture}"
            --actual "${direct_capture}"
            --diff "${parity_diff}"
            --report "${parity_report}"
            --tolerance 0.0
            --max-different-pixels 0
        BUILD_TARGETS VisualGoldenValidation
        TAGS product.samples trait.visual trait.gpu backend.${BACKEND_NAME} suite.primary suite.gpu-driven suite.parity
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        FIXTURES_REQUIRED
            "${test_prefix}GPUFreshCapture;${test_prefix}DirectFreshCapture"
    )
endfunction()

if(RVX_ENABLE_PR_SMOKE)
    if(WIN32 AND RVX_ENABLE_DX12)
        rvx_add_renderverse_pr_smoke(dx12 DX12)
    elseif(APPLE AND RVX_ENABLE_METAL)
        rvx_add_renderverse_pr_smoke(metal Metal)
    elseif(UNIX AND RVX_ENABLE_VULKAN)
        rvx_add_renderverse_pr_smoke(vulkan Vulkan)
    else()
        message(FATAL_ERROR
            "RVX_ENABLE_PR_SMOKE requires the platform primary backend (DX12, Vulkan, or Metal)")
    endif()
endif()

if(RVX_ENABLE_COMPATIBILITY_SMOKE)
    set(RVX_COMPATIBILITY_SMOKE_COUNT 0)
    if(WIN32 AND RVX_ENABLE_DX11)
        rvx_add_renderverse_compatibility_smoke(dx11 DX11)
        math(EXPR RVX_COMPATIBILITY_SMOKE_COUNT "${RVX_COMPATIBILITY_SMOKE_COUNT} + 1")
    endif()
    if(RVX_ENABLE_OPENGL)
        rvx_add_renderverse_compatibility_smoke(opengl OpenGL)
        math(EXPR RVX_COMPATIBILITY_SMOKE_COUNT "${RVX_COMPATIBILITY_SMOKE_COUNT} + 1")
    endif()
    if(RVX_COMPATIBILITY_SMOKE_COUNT EQUAL 0)
        message(FATAL_ERROR
            "RVX_ENABLE_COMPATIBILITY_SMOKE requires DX11 or OpenGL")
    endif()
endif()

if(RVX_ENABLE_WEEKLY_SOAK)
    if(WIN32 AND RVX_ENABLE_DX12)
        rvx_add_renderverse_weekly_soak(dx12 DX12)
    elseif(APPLE AND RVX_ENABLE_METAL)
        rvx_add_renderverse_weekly_soak(metal Metal)
    elseif(UNIX AND RVX_ENABLE_VULKAN)
        rvx_add_renderverse_weekly_soak(vulkan Vulkan)
    else()
        message(FATAL_ERROR
            "RVX_ENABLE_WEEKLY_SOAK requires the platform primary backend")
    endif()
endif()

if(WIN32 AND RVX_ENABLE_DX12 AND NOT RVX_ENABLE_PR_SMOKE)
    if(RVX_ENABLE_P0_QUALIFICATION_MATRIX)
    rvx_add_renderverse_scene_lifecycle_matrix(dx12 DX12)
    rvx_add_renderverse_asset_streaming_matrix(dx12 DX12)
    rvx_add_renderverse_interior_matrix(dx12 DX12)
    rvx_add_renderverse_primary_smoke(
        rendering-stress RenderingStressDirect interior-rendering-p0a dx12 DX12
        TIMEOUT 300
        WAIT_READY
        ASSESSMENT
        ASSESSMENT_PROFILE qualification
        READY_TIMEOUT_MS 120000
        READY_MAX_FRAMES 600
        SAMPLE_ARGS
            --render-path direct
            --workload-scale pr
        REPORT_ARGS
            --render-path direct
            --require-requested-render-path direct
            --require-actual-render-path direct
            --require-requested-physics-backend auto
            --require-actual-physics-backend built-in
            --forbid-physics-backend-fallback
            --require-native-validation-available
            --require-native-validation-enabled
            --require-native-validation-read-complete
            --require-native-validation-error-count 0
            --require-native-validation-corruption-count 0
            --require-direct-execution
            --require-enabled-feature DeterministicRenderingStressStateMachine
            --require-enabled-feature SharedModelResourceProceduralActors
            --require-enabled-feature RetainedSceneDeltaDiagnostics
            --require-enabled-feature GPUSceneDeltaDiagnostics
            --require-resource-diagnostic "workload scale=pr"
            --require-resource-diagnostic "workload objects=1000"
            --require-resource-diagnostic "workload dirty=10"
            --require-resource-diagnostic "workload churn=100"
            --require-resource-diagnostic "phase=Complete")
    rvx_add_renderverse_primary_smoke(
        rendering-stress RenderingStressGPUDriven interior-rendering-p0a dx12 DX12
        TIMEOUT 300
        WAIT_READY
        ASSESSMENT
        ASSESSMENT_PROFILE qualification
        READY_TIMEOUT_MS 120000
        READY_MAX_FRAMES 600
        SAMPLE_ARGS
            --render-path gpu-driven
            --workload-scale pr
        REPORT_ARGS
            --render-path gpu-driven
            --require-requested-render-path gpu-driven
            --require-actual-render-path gpu-driven
            --require-requested-physics-backend auto
            --require-actual-physics-backend built-in
            --forbid-physics-backend-fallback
            --require-native-validation-available
            --require-native-validation-enabled
            --require-native-validation-read-complete
            --require-native-validation-error-count 0
            --require-native-validation-corruption-count 0
            --require-gpu-driven-execution
            --require-enabled-feature DeterministicRenderingStressStateMachine
            --require-enabled-feature SharedModelResourceProceduralActors
            --require-enabled-feature RetainedSceneDeltaDiagnostics
            --require-enabled-feature GPUSceneDeltaDiagnostics
            --require-resource-diagnostic "workload scale=pr"
            --require-resource-diagnostic "workload objects=1000"
            --require-resource-diagnostic "workload dirty=10"
            --require-resource-diagnostic "workload churn=100"
            --require-resource-diagnostic "phase=Complete")
    rvx_add_renderverse_p0_cross_path_parity(RenderingStress dx12 DX12)
    endif()
    if(RVX_ENABLE_P1_QUALIFICATION_MATRIX)
    rvx_add_renderverse_physics_sandbox_matrix(dx12 DX12)
    rvx_add_renderverse_animation_character_matrix(dx12 DX12)
    endif()
    rvx_add_renderverse_primary_smoke(
        model-rendering ModelRendering shadow-plane-caster dx12 DX12)
    rvx_add_renderverse_primary_smoke(
        asset-gallery AssetGallery r7-triangle dx12 DX12
        REPORT_ARGS
        --min-visible-objects 3
        --require-enabled-feature ResourceSceneAdapters
        --require-enabled-feature CatalogStaticGallery
        --require-enabled-feature BoundsCameraFraming
        --require-unsupported-feature RuntimeAssetSwitching
        --require-asset-entry r7-triangle
        --require-asset-entry shadow-plane-caster
        --require-asset-entry pbr-material-grid
        --require-resource-diagnostic "gallery assets=3"
    )
    rvx_add_renderverse_primary_smoke(
        lighting-shadows LightingShadows shadow-plane-caster dx12 DX12
        REPORT_ARGS
        --require-enabled-feature ShadowCasterReceiverFixture
        --require-enabled-feature CascadedDirectionalShadowRequested
        --require-enabled-feature DirectionalShadow
        --require-directional-shadow
        --require-resource-diagnostic "shadow atlas resolution=1024"
        --require-resource-diagnostic "shadow cascade count=1"
    )
    rvx_add_renderverse_primary_smoke(
        render-pipeline RenderPipeline shadow-plane-caster dx12 DX12
        REPORT_ARGS
        --require-enabled-feature EngineRenderPipelineConfiguration
        --require-enabled-feature DirectionalShadow
        --require-enabled-feature PostProcessRenderGraph
        --require-directional-shadow
        --min-render-graph-passes 8
        --min-enabled-post-process-effects 2
        --min-post-process-graph-passes 2
        --forbid-unsupported-post-process
        --require-resource-diagnostic "pipeline ownership=Render/RenderGraph/PostProcessStack"
    )
    rvx_add_renderverse_primary_smoke(
        model-viewer ModelViewer r7-triangle dx12 DX12
        SAMPLE_ARGS
            --environment pbr-reference-environment
        REPORT_ARGS
        --require-enabled-feature BoundsCameraFraming
        --require-enabled-feature OrbitCamera
        --environment-asset-id pbr-reference-environment
        --environment-license-spdx LicenseRef-RenderVerseX-Generated-Fixture
        --environment-source-uri repo://Samples/Common/Tools/SampleHDRFixtureWriter.cpp
        --require-environment-redistributable
        --require-texture-ibl
        --forbid-fallback-reason PacketEnvironmentResourcesUnavailable
        --require-enabled-feature EnvironmentResourceLoad
        --require-enabled-feature EnvironmentIBL
        --require-enabled-feature DiffuseIBL
        --require-enabled-feature SpecularIBL
        --require-enabled-feature TextureIBL
        --require-enabled-feature TextureEnvironmentScene
    )
    rvx_add_renderverse_primary_smoke(
        model-viewer ModelViewerProcedural r7-triangle dx12 DX12
        REPORT_ARGS
        --require-enabled-feature BoundsCameraFraming
        --require-enabled-feature OrbitCamera
        --require-enabled-feature ModelRenderReady
        --require-enabled-feature ProceduralSkyScene
        --forbid-fallback-reason PacketEnvironmentResourcesUnavailable
    )
    rvx_add_renderverse_primary_smoke(
        pbr-materials PBRMaterials pbr-material-grid dx12 DX12
        WAIT_READY
        READY_TIMEOUT_MS 60000
        READY_MAX_FRAMES 192
        REPORT_ARGS
        --require-enabled-feature PBRMetallicRoughnessMaterials
        --require-enabled-feature PBRMaterialParameterCube
        --require-enabled-feature PBRBaseColorFamilyAxis
        --require-enabled-feature MetallicRoughnessFactorCube
        --require-enabled-feature SharedMeshSceneInstances
        --require-opaque-execution
        --require-opaque-materials-ready
        --environment-asset-id pbr-reference-environment
        --environment-license-spdx LicenseRef-RenderVerseX-Generated-Fixture
        --environment-source-uri repo://Samples/Common/Tools/SampleHDRFixtureWriter.cpp
        --require-environment-redistributable
        --require-texture-ibl
        --forbid-fallback-reason PacketEnvironmentResourcesUnavailable
        --require-enabled-feature EnvironmentResourceLoad
        --require-enabled-feature EnvironmentIBL
        --require-enabled-feature DiffuseIBL
        --require-enabled-feature SpecularIBL
        --require-enabled-feature TextureIBL
        --require-enabled-feature ControlledStudioLighting
        --require-enabled-feature BoundsCameraFraming
        --require-resource-diagnostic "pbr materials=125"
        --require-resource-diagnostic "metallic levels=5"
        --require-resource-diagnostic "roughness levels=5"
        --require-resource-diagnostic "base-color families=5"
        --require-resource-diagnostic "matrix shape=5x5x5"
        --require-resource-diagnostic "total material spheres=125"
        --require-resource-diagnostic "matrix x-axis=metallic 0.00,0.25,0.50,0.75,1.00"
        --require-resource-diagnostic "matrix y-axis=roughness 0.05,0.25,0.50,0.75,1.00"
        --require-resource-diagnostic "matrix z-axis=base-color terracotta,gold,green,blue,neutral"
        --require-resource-diagnostic "material workflow=factors"
        --require-resource-diagnostic "factor cube materials=125"
        --require-resource-diagnostic "factor cube combinations=125"
        --require-resource-diagnostic "model meshes=1"
        --require-resource-diagnostic "scene mesh instances=125"
        --require-resource-diagnostic "unique mesh resources=1"
        --require-resource-diagnostic "material overrides=125"
        --require-resource-diagnostic "opaque draw packets=125"
    )
    rvx_add_renderverse_base_render_path_matrices(dx12 DX12)
    rvx_add_renderverse_pbr_response_validation(dx12 DX12 PBRMaterials)
    rvx_add_renderverse_pbr_texture_smoke(dx12 DX12)
    rvx_add_renderverse_pbr_workflow_parity(dx12 DX12)
    rvx_add_renderverse_external_model_viewer_validation(dx12 DX12)
    rvx_add_renderverse_external_environment_validation(dx12 DX12)
    rvx_add_renderverse_readiness_smoke(dx12 DX12)
    rvx_add_renderverse_gpu_driven_parity(dx12 DX12)
endif()
if(RVX_ENABLE_VULKAN AND NOT RVX_ENABLE_PR_SMOKE)
    if(RVX_ENABLE_P0_QUALIFICATION_MATRIX)
    rvx_add_renderverse_scene_lifecycle_matrix(vulkan Vulkan)
    rvx_add_renderverse_asset_streaming_matrix(vulkan Vulkan)
    rvx_add_renderverse_interior_matrix(vulkan Vulkan)
    rvx_add_renderverse_primary_smoke(
        rendering-stress RenderingStressDirect interior-rendering-p0a vulkan Vulkan
        TIMEOUT 300
        WAIT_READY
        ASSESSMENT
        ASSESSMENT_PROFILE qualification
        READY_TIMEOUT_MS 120000
        READY_MAX_FRAMES 600
        SAMPLE_ARGS
            --render-path direct
            --workload-scale pr
        REPORT_ARGS
            --render-path direct
            --require-requested-render-path direct
            --require-actual-render-path direct
            --require-requested-physics-backend auto
            --require-actual-physics-backend built-in
            --forbid-physics-backend-fallback
            --require-native-validation-available
            --require-native-validation-enabled
            --require-native-validation-read-complete
            --require-native-validation-error-count 0
            --require-native-validation-corruption-count 0
            --require-direct-execution
            --require-enabled-feature DeterministicRenderingStressStateMachine
            --require-enabled-feature SharedModelResourceProceduralActors
            --require-enabled-feature RetainedSceneDeltaDiagnostics
            --require-enabled-feature GPUSceneDeltaDiagnostics
            --require-resource-diagnostic "workload scale=pr"
            --require-resource-diagnostic "workload objects=1000"
            --require-resource-diagnostic "workload dirty=10"
            --require-resource-diagnostic "workload churn=100"
            --require-resource-diagnostic "phase=Complete")
    rvx_add_renderverse_primary_smoke(
        rendering-stress RenderingStressGPUDriven interior-rendering-p0a vulkan Vulkan
        TIMEOUT 300
        WAIT_READY
        ASSESSMENT
        ASSESSMENT_PROFILE qualification
        READY_TIMEOUT_MS 120000
        READY_MAX_FRAMES 600
        SAMPLE_ARGS
            --render-path gpu-driven
            --workload-scale pr
        REPORT_ARGS
            --render-path gpu-driven
            --require-requested-render-path gpu-driven
            --require-actual-render-path gpu-driven
            --require-requested-physics-backend auto
            --require-actual-physics-backend built-in
            --forbid-physics-backend-fallback
            --require-native-validation-available
            --require-native-validation-enabled
            --require-native-validation-read-complete
            --require-native-validation-error-count 0
            --require-native-validation-corruption-count 0
            --require-gpu-driven-execution
            --require-enabled-feature DeterministicRenderingStressStateMachine
            --require-enabled-feature SharedModelResourceProceduralActors
            --require-enabled-feature RetainedSceneDeltaDiagnostics
            --require-enabled-feature GPUSceneDeltaDiagnostics
            --require-resource-diagnostic "workload scale=pr"
            --require-resource-diagnostic "workload objects=1000"
            --require-resource-diagnostic "workload dirty=10"
            --require-resource-diagnostic "workload churn=100"
            --require-resource-diagnostic "phase=Complete")
    rvx_add_renderverse_p0_cross_path_parity(RenderingStress vulkan Vulkan)
    endif()
    if(RVX_ENABLE_P1_QUALIFICATION_MATRIX)
    rvx_add_renderverse_physics_sandbox_matrix(vulkan Vulkan)
    rvx_add_renderverse_animation_character_matrix(vulkan Vulkan)
    endif()
    rvx_add_renderverse_primary_smoke(
        model-rendering ModelRendering shadow-plane-caster vulkan Vulkan)
    rvx_add_renderverse_primary_smoke(
        asset-gallery AssetGallery r7-triangle vulkan Vulkan
        REPORT_ARGS
        --min-visible-objects 3
        --require-enabled-feature ResourceSceneAdapters
        --require-enabled-feature CatalogStaticGallery
        --require-enabled-feature BoundsCameraFraming
        --require-unsupported-feature RuntimeAssetSwitching
        --require-asset-entry r7-triangle
        --require-asset-entry shadow-plane-caster
        --require-asset-entry pbr-material-grid
        --require-resource-diagnostic "gallery assets=3"
    )
    rvx_add_renderverse_primary_smoke(
        lighting-shadows LightingShadows shadow-plane-caster vulkan Vulkan
        REPORT_ARGS
        --require-enabled-feature ShadowCasterReceiverFixture
        --require-enabled-feature CascadedDirectionalShadowRequested
        --require-enabled-feature DirectionalShadow
        --require-directional-shadow
        --require-resource-diagnostic "shadow atlas resolution=1024"
        --require-resource-diagnostic "shadow cascade count=1"
    )
    rvx_add_renderverse_primary_smoke(
        render-pipeline RenderPipeline shadow-plane-caster vulkan Vulkan
        REPORT_ARGS
        --require-enabled-feature EngineRenderPipelineConfiguration
        --require-enabled-feature DirectionalShadow
        --require-enabled-feature PostProcessRenderGraph
        --require-directional-shadow
        --min-render-graph-passes 8
        --min-enabled-post-process-effects 2
        --min-post-process-graph-passes 2
        --forbid-unsupported-post-process
        --require-resource-diagnostic "pipeline ownership=Render/RenderGraph/PostProcessStack"
    )
    rvx_add_renderverse_primary_smoke(
        model-viewer ModelViewer r7-triangle vulkan Vulkan
        SAMPLE_ARGS
            --environment pbr-reference-environment
        REPORT_ARGS
        --require-enabled-feature BoundsCameraFraming
        --require-enabled-feature OrbitCamera
        --environment-asset-id pbr-reference-environment
        --environment-license-spdx LicenseRef-RenderVerseX-Generated-Fixture
        --environment-source-uri repo://Samples/Common/Tools/SampleHDRFixtureWriter.cpp
        --require-environment-redistributable
        --require-texture-ibl
        --forbid-fallback-reason PacketEnvironmentResourcesUnavailable
        --require-enabled-feature EnvironmentResourceLoad
        --require-enabled-feature EnvironmentIBL
        --require-enabled-feature DiffuseIBL
        --require-enabled-feature SpecularIBL
        --require-enabled-feature TextureIBL
        --require-enabled-feature TextureEnvironmentScene
    )
    rvx_add_renderverse_primary_smoke(
        model-viewer ModelViewerProcedural r7-triangle vulkan Vulkan
        REPORT_ARGS
        --require-enabled-feature BoundsCameraFraming
        --require-enabled-feature OrbitCamera
        --require-enabled-feature ModelRenderReady
        --require-enabled-feature ProceduralSkyScene
        --forbid-fallback-reason PacketEnvironmentResourcesUnavailable
    )
    rvx_add_renderverse_primary_smoke(
        pbr-materials PBRMaterials pbr-material-grid vulkan Vulkan
        WAIT_READY
        READY_TIMEOUT_MS 60000
        READY_MAX_FRAMES 192
        REPORT_ARGS
        --require-enabled-feature PBRMetallicRoughnessMaterials
        --require-enabled-feature PBRMaterialParameterCube
        --require-enabled-feature PBRBaseColorFamilyAxis
        --require-enabled-feature MetallicRoughnessFactorCube
        --require-enabled-feature SharedMeshSceneInstances
        --require-opaque-execution
        --require-opaque-materials-ready
        --environment-asset-id pbr-reference-environment
        --environment-license-spdx LicenseRef-RenderVerseX-Generated-Fixture
        --environment-source-uri repo://Samples/Common/Tools/SampleHDRFixtureWriter.cpp
        --require-environment-redistributable
        --require-texture-ibl
        --forbid-fallback-reason PacketEnvironmentResourcesUnavailable
        --require-enabled-feature EnvironmentResourceLoad
        --require-enabled-feature EnvironmentIBL
        --require-enabled-feature DiffuseIBL
        --require-enabled-feature SpecularIBL
        --require-enabled-feature TextureIBL
        --require-enabled-feature ControlledStudioLighting
        --require-enabled-feature BoundsCameraFraming
        --require-resource-diagnostic "pbr materials=125"
        --require-resource-diagnostic "metallic levels=5"
        --require-resource-diagnostic "roughness levels=5"
        --require-resource-diagnostic "base-color families=5"
        --require-resource-diagnostic "matrix shape=5x5x5"
        --require-resource-diagnostic "total material spheres=125"
        --require-resource-diagnostic "matrix x-axis=metallic 0.00,0.25,0.50,0.75,1.00"
        --require-resource-diagnostic "matrix y-axis=roughness 0.05,0.25,0.50,0.75,1.00"
        --require-resource-diagnostic "matrix z-axis=base-color terracotta,gold,green,blue,neutral"
        --require-resource-diagnostic "material workflow=factors"
        --require-resource-diagnostic "factor cube materials=125"
        --require-resource-diagnostic "factor cube combinations=125"
        --require-resource-diagnostic "model meshes=1"
        --require-resource-diagnostic "scene mesh instances=125"
        --require-resource-diagnostic "unique mesh resources=1"
        --require-resource-diagnostic "material overrides=125"
        --require-resource-diagnostic "opaque draw packets=125"
    )
    rvx_add_renderverse_base_render_path_matrices(vulkan Vulkan)
    rvx_add_renderverse_pbr_response_validation(vulkan Vulkan PBRMaterials)
    rvx_add_renderverse_pbr_texture_smoke(vulkan Vulkan)
    rvx_add_renderverse_pbr_workflow_parity(vulkan Vulkan)
    rvx_add_renderverse_external_model_viewer_validation(vulkan Vulkan)
    rvx_add_renderverse_external_environment_validation(vulkan Vulkan)
    rvx_add_renderverse_readiness_smoke(vulkan Vulkan)
    rvx_add_renderverse_gpu_driven_parity(vulkan Vulkan)
endif()
if(APPLE AND RVX_ENABLE_METAL AND NOT RVX_ENABLE_PR_SMOKE)
    rvx_add_renderverse_primary_smoke(
        model-rendering ModelRendering shadow-plane-caster metal Metal)
    rvx_add_renderverse_primary_smoke(
        asset-gallery AssetGallery r7-triangle metal Metal
        REPORT_ARGS
        --min-visible-objects 3
        --require-enabled-feature ResourceSceneAdapters
        --require-enabled-feature CatalogStaticGallery
        --require-enabled-feature BoundsCameraFraming
        --require-unsupported-feature RuntimeAssetSwitching
        --require-asset-entry r7-triangle
        --require-asset-entry shadow-plane-caster
        --require-asset-entry pbr-material-grid
        --require-resource-diagnostic "gallery assets=3"
    )
    rvx_add_renderverse_primary_smoke(
        lighting-shadows LightingShadows shadow-plane-caster metal Metal
        REPORT_ARGS
        --require-enabled-feature ShadowCasterReceiverFixture
        --require-enabled-feature CascadedDirectionalShadowRequested
        --require-enabled-feature DirectionalShadow
        --require-directional-shadow
        --require-resource-diagnostic "shadow atlas resolution=1024"
        --require-resource-diagnostic "shadow cascade count=1"
    )
    rvx_add_renderverse_primary_smoke(
        render-pipeline RenderPipeline shadow-plane-caster metal Metal
        REPORT_ARGS
        --require-enabled-feature EngineRenderPipelineConfiguration
        --require-enabled-feature DirectionalShadow
        --require-enabled-feature PostProcessRenderGraph
        --require-directional-shadow
        --min-render-graph-passes 8
        --min-enabled-post-process-effects 2
        --min-post-process-graph-passes 2
        --forbid-unsupported-post-process
        --require-resource-diagnostic "pipeline ownership=Render/RenderGraph/PostProcessStack"
    )
    rvx_add_renderverse_primary_smoke(
        model-viewer ModelViewer r7-triangle metal Metal
        SAMPLE_ARGS
            --environment pbr-reference-environment
        REPORT_ARGS
        --require-enabled-feature BoundsCameraFraming
        --require-enabled-feature OrbitCamera
        --environment-asset-id pbr-reference-environment
        --environment-license-spdx LicenseRef-RenderVerseX-Generated-Fixture
        --environment-source-uri repo://Samples/Common/Tools/SampleHDRFixtureWriter.cpp
        --require-environment-redistributable
        --require-texture-ibl
        --forbid-fallback-reason PacketEnvironmentResourcesUnavailable
        --require-enabled-feature EnvironmentResourceLoad
        --require-enabled-feature EnvironmentIBL
        --require-enabled-feature DiffuseIBL
        --require-enabled-feature SpecularIBL
        --require-enabled-feature TextureIBL
        --require-enabled-feature TextureEnvironmentScene
    )
    rvx_add_renderverse_primary_smoke(
        model-viewer ModelViewerProcedural r7-triangle metal Metal
        REPORT_ARGS
        --require-enabled-feature BoundsCameraFraming
        --require-enabled-feature OrbitCamera
        --require-enabled-feature ModelRenderReady
        --require-enabled-feature ProceduralSkyScene
        --forbid-fallback-reason PacketEnvironmentResourcesUnavailable
    )
    rvx_add_renderverse_primary_smoke(
        pbr-materials PBRMaterials pbr-material-grid metal Metal
        WAIT_READY
        READY_TIMEOUT_MS 60000
        READY_MAX_FRAMES 192
        REPORT_ARGS
        --require-enabled-feature PBRMetallicRoughnessMaterials
        --require-enabled-feature PBRMaterialParameterCube
        --require-enabled-feature PBRBaseColorFamilyAxis
        --require-enabled-feature MetallicRoughnessFactorCube
        --require-enabled-feature SharedMeshSceneInstances
        --require-opaque-execution
        --require-opaque-materials-ready
        --environment-asset-id pbr-reference-environment
        --environment-license-spdx LicenseRef-RenderVerseX-Generated-Fixture
        --environment-source-uri repo://Samples/Common/Tools/SampleHDRFixtureWriter.cpp
        --require-environment-redistributable
        --require-texture-ibl
        --forbid-fallback-reason PacketEnvironmentResourcesUnavailable
        --require-enabled-feature EnvironmentResourceLoad
        --require-enabled-feature EnvironmentIBL
        --require-enabled-feature DiffuseIBL
        --require-enabled-feature SpecularIBL
        --require-enabled-feature TextureIBL
        --require-enabled-feature ControlledStudioLighting
        --require-enabled-feature BoundsCameraFraming
        --require-resource-diagnostic "pbr materials=125"
        --require-resource-diagnostic "metallic levels=5"
        --require-resource-diagnostic "roughness levels=5"
        --require-resource-diagnostic "base-color families=5"
        --require-resource-diagnostic "matrix shape=5x5x5"
        --require-resource-diagnostic "total material spheres=125"
        --require-resource-diagnostic "matrix x-axis=metallic 0.00,0.25,0.50,0.75,1.00"
        --require-resource-diagnostic "matrix y-axis=roughness 0.05,0.25,0.50,0.75,1.00"
        --require-resource-diagnostic "matrix z-axis=base-color terracotta,gold,green,blue,neutral"
        --require-resource-diagnostic "material workflow=factors"
        --require-resource-diagnostic "factor cube materials=125"
        --require-resource-diagnostic "factor cube combinations=125"
        --require-resource-diagnostic "model meshes=1"
        --require-resource-diagnostic "scene mesh instances=125"
        --require-resource-diagnostic "unique mesh resources=1"
        --require-resource-diagnostic "material overrides=125"
        --require-resource-diagnostic "opaque draw packets=125"
    )
    rvx_add_renderverse_pbr_response_validation(metal Metal PBRMaterials)
    rvx_add_renderverse_pbr_texture_smoke(metal Metal)
    rvx_add_renderverse_pbr_workflow_parity(metal Metal)
    rvx_add_renderverse_external_environment_validation(metal Metal)
    rvx_add_renderverse_readiness_smoke(metal Metal)
    rvx_add_renderverse_gpu_driven_parity(metal Metal)
endif()
function(rvx_add_renderverse_p0_cross_backend_parity SAMPLE_LABEL)
    if(NOT WIN32 OR
       NOT RVX_ENABLE_DX12 OR
       NOT RVX_ENABLE_VULKAN OR
       NOT TARGET VisualGoldenValidation OR
       NOT TARGET RenderVerseSamples)
        return()
    endif()

    foreach(path_label Direct GPUDriven)
        set(dx12_capture
            "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/RenderVerseSamplesDX12${SAMPLE_LABEL}${path_label}_320x180.ppm")
        set(vulkan_capture
            "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/RenderVerseSamplesVulkan${SAMPLE_LABEL}${path_label}_320x180.ppm")
        set(parity_prefix
            "RenderVerseSamplesCrossBackend${SAMPLE_LABEL}${path_label}ParityValidation")
        rvx_add_validation_test(
            LANE qualification
            NAME ${parity_prefix}
            COMMAND $<TARGET_FILE:VisualGoldenValidation>
                --expected "${dx12_capture}"
                --actual "${vulkan_capture}"
                --diff "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/${parity_prefix}_DX12_Vulkan_diff.ppm"
                --report "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/${parity_prefix}_DX12_Vulkan.json"
                --tolerance 0.0
                --max-different-pixels 0
            BUILD_TARGETS VisualGoldenValidation
            TAGS product.samples trait.visual trait.gpu backend.dx12 backend.vulkan suite.primary suite.parity suite.cross-backend
            TIMEOUT 30
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            FIXTURES_REQUIRED
                "RenderVerseSamplesDX12${SAMPLE_LABEL}${path_label}FreshCapture;RenderVerseSamplesVulkan${SAMPLE_LABEL}${path_label}FreshCapture")
    endforeach()
endfunction()

function(rvx_add_renderverse_gpu_driven_cross_backend_parity)
    if(NOT RVX_ENABLE_P0_QUALIFICATION_MATRIX)
        return()
    endif()
    if(NOT WIN32 OR NOT RVX_ENABLE_DX12 OR NOT RVX_ENABLE_VULKAN)
        message(FATAL_ERROR
            "The gpu-driven cross-backend qualification requires Windows DX12 and Vulkan")
    endif()
    foreach(required_target VisualGoldenValidation RenderVerseSamples)
        if(NOT TARGET ${required_target})
            message(FATAL_ERROR
                "RVX_ENABLE_P0_QUALIFICATION_MATRIX requires ${required_target} for gpu-driven cross-backend parity")
        endif()
    endforeach()

    foreach(path_label GPU Direct)
        set(dx12_capture
            "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/RenderVerseSamplesDX12GPUDriven_${path_label}_320x180.ppm")
        set(vulkan_capture
            "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/RenderVerseSamplesVulkanGPUDriven_${path_label}_320x180.ppm")
        set(parity_prefix
            "RenderVerseSamplesCrossBackendGPUDriven${path_label}ParityValidation")
        rvx_add_validation_test(
            LANE qualification
            NAME ${parity_prefix}
            COMMAND $<TARGET_FILE:VisualGoldenValidation>
                --expected "${dx12_capture}"
                --actual "${vulkan_capture}"
                --diff "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/${parity_prefix}_DX12_Vulkan_diff.ppm"
                --report "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/${parity_prefix}_DX12_Vulkan.json"
                --tolerance 0.0
                --max-different-pixels 0
            BUILD_TARGETS VisualGoldenValidation
            TAGS product.samples trait.visual trait.gpu backend.dx12 backend.vulkan suite.primary suite.gpu-driven suite.parity suite.cross-backend
            TIMEOUT 30
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            FIXTURES_REQUIRED
                "RenderVerseSamplesDX12GPUDriven${path_label}FreshCapture;RenderVerseSamplesVulkanGPUDriven${path_label}FreshCapture")
    endforeach()
endfunction()

if(RVX_ENABLE_GFX07_DEFERRED_NATIVE)
    # The suite target is RVXValidationSuiteGfx07Deferred; its CTest label is
    # suite.gfx07-deferred (and the narrower suite.gfx07 tag) for local runs.
    rvx_add_renderverse_gfx07_deferred_native(dx12 DX12)
    rvx_add_renderverse_gfx07_deferred_native(vulkan Vulkan)
endif()

if(RVX_ENABLE_GFX08_LIGHTING_NATIVE)
    # The suite target is RVXValidationSuiteGfx08Lighting and its exact CTest
    # label is suite.gfx08-lighting.
    rvx_add_renderverse_gfx08_lighting_native(
        dx12 DX12 forward-plus ForwardPlus)
    rvx_add_renderverse_gfx08_lighting_native(
        dx12 DX12 deferred Deferred)
    rvx_add_renderverse_gfx08_lighting_native(
        vulkan Vulkan forward-plus ForwardPlus)
    rvx_add_renderverse_gfx08_lighting_native(
        vulkan Vulkan deferred Deferred)
endif()

if(RVX_ENABLE_GFX09_ANIMATION_NATIVE)
    # The suite target is RVXValidationSuiteGfx09Animation and its exact CTest
    # label is suite.gfx09-animation.
    rvx_add_renderverse_gfx09_animation_native(
        dx12 DX12 forward-plus ForwardPlus)
    rvx_add_renderverse_gfx09_animation_native(
        dx12 DX12 deferred Deferred)
    rvx_add_renderverse_gfx09_animation_native(
        vulkan Vulkan forward-plus ForwardPlus)
    rvx_add_renderverse_gfx09_animation_native(
        vulkan Vulkan deferred Deferred)
endif()

if(RVX_ENABLE_GFX10_POST_PROCESS_NATIVE)
    # The suite target is RVXValidationSuiteGfx10Postprocess and its exact
    # CTest label is suite.gfx10-postprocess.
    rvx_add_renderverse_gfx10_post_process_native(
        dx12 DX12 forward-plus ForwardPlus)
    rvx_add_renderverse_gfx10_post_process_native(
        dx12 DX12 deferred Deferred)
    rvx_add_renderverse_gfx10_post_process_native(
        vulkan Vulkan forward-plus ForwardPlus)
    rvx_add_renderverse_gfx10_post_process_native(
        vulkan Vulkan deferred Deferred)
endif()

if(WIN32
    AND RVX_ENABLE_DX12
    AND RVX_ENABLE_VULKAN
    AND RVX_ENABLE_P0_QUALIFICATION_MATRIX
    AND TARGET VisualGoldenValidation
    AND TARGET RenderVerseSamples)
    rvx_add_renderverse_p0_cross_backend_parity(SceneLifecycle)
    rvx_add_renderverse_p0_cross_backend_parity(AssetStreaming)
    rvx_add_renderverse_p0_cross_backend_parity(InteriorRendering)
    rvx_add_renderverse_p0_cross_backend_parity(ModelRendering)
    rvx_add_renderverse_p0_cross_backend_parity(AssetGallery)
    rvx_add_renderverse_p0_cross_backend_parity(LightingShadows)
    rvx_add_renderverse_p0_cross_backend_parity(RenderPipeline)
    rvx_add_renderverse_p0_cross_backend_parity(ModelViewer)
    rvx_add_renderverse_p0_cross_backend_parity(PBRMaterials)
    if(RVX_ENABLE_P1_QUALIFICATION_MATRIX)
        rvx_add_renderverse_p0_cross_backend_parity(AnimationCharacter)
    endif()
    set(RVX_RENDERING_STRESS_DX12_DIRECT_ACTUAL
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/RenderVerseSamplesDX12RenderingStressDirect_320x180.ppm")
    set(RVX_RENDERING_STRESS_VULKAN_DIRECT_ACTUAL
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/RenderVerseSamplesVulkanRenderingStressDirect_320x180.ppm")
    set(RVX_RENDERING_STRESS_DX12_GPU_DRIVEN_ACTUAL
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/RenderVerseSamplesDX12RenderingStressGPUDriven_320x180.ppm")
    set(RVX_RENDERING_STRESS_VULKAN_GPU_DRIVEN_ACTUAL
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/RenderVerseSamplesVulkanRenderingStressGPUDriven_320x180.ppm")

    rvx_add_validation_test(
        LANE qualification
        NAME RenderVerseSamplesCrossBackendRenderingStressDirectParityValidation
        COMMAND $<TARGET_FILE:VisualGoldenValidation>
            --expected "${RVX_RENDERING_STRESS_DX12_DIRECT_ACTUAL}"
            --actual "${RVX_RENDERING_STRESS_VULKAN_DIRECT_ACTUAL}"
            --diff "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/RenderVerseSamplesRenderingStressDirect_DX12_Vulkan_320x180.diff.ppm"
            --report "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/RenderVerseSamplesRenderingStressDirect_DX12_Vulkan_320x180.json"
            --tolerance 0.0
            --max-different-pixels 0
        BUILD_TARGETS VisualGoldenValidation
        TAGS product.samples trait.visual trait.gpu backend.dx12 backend.vulkan suite.primary suite.parity suite.cross-backend
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        FIXTURES_REQUIRED
            "RenderVerseSamplesDX12RenderingStressDirectFreshCapture;RenderVerseSamplesVulkanRenderingStressDirectFreshCapture"
    )

    rvx_add_validation_test(
        LANE qualification
        NAME RenderVerseSamplesCrossBackendRenderingStressGPUDrivenParityValidation
        COMMAND $<TARGET_FILE:VisualGoldenValidation>
            --expected "${RVX_RENDERING_STRESS_DX12_GPU_DRIVEN_ACTUAL}"
            --actual "${RVX_RENDERING_STRESS_VULKAN_GPU_DRIVEN_ACTUAL}"
            --diff "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/RenderVerseSamplesRenderingStressGPUDriven_DX12_Vulkan_320x180.diff.ppm"
            --report "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/RenderVerseSamplesRenderingStressGPUDriven_DX12_Vulkan_320x180.json"
            --tolerance 0.0
            --max-different-pixels 0
        BUILD_TARGETS VisualGoldenValidation
        TAGS product.samples trait.visual trait.gpu backend.dx12 backend.vulkan suite.primary suite.gpu-driven suite.parity suite.cross-backend
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        FIXTURES_REQUIRED
            "RenderVerseSamplesDX12RenderingStressGPUDrivenFreshCapture;RenderVerseSamplesVulkanRenderingStressGPUDrivenFreshCapture"
    )
endif()

rvx_add_renderverse_gpu_driven_cross_backend_parity()

if(WIN32
    AND RVX_ENABLE_DX12
    AND RVX_ENABLE_VULKAN
    AND RVX_ENABLE_P1_QUALIFICATION_MATRIX
    AND TARGET VisualGoldenValidation
    AND TARGET RenderVerseSamples)
    rvx_add_renderverse_p0_cross_backend_parity(PhysicsSandbox)
endif()

if(RVX_ENABLE_OPENGL AND TARGET RVXEditor)
    set(RVX_EDITOR_SMOKE_ACTUAL
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/Editor/EditorViewportSmoke.ppm")
    set(RVX_EDITOR_WORKSPACE_PATH_SMOKE_CWD
        "${CMAKE_CURRENT_BINARY_DIR}/Fixtures/Editor/WorkspacePathSmokeCwd")
    set(RVX_EDITOR_WORKSPACE_PATH_SMOKE_ACTUAL
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/Editor/EditorWorkspacePathSmoke.ppm")
    file(MAKE_DIRECTORY "${RVX_EDITOR_WORKSPACE_PATH_SMOKE_CWD}")

    rvx_add_validation_test(
        LANE qualification
        NAME RVXEditorSmoke
        COMMAND $<TARGET_FILE:RVXEditor>
            --smoke
            --frames 20
            --screenshot "${RVX_EDITOR_SMOKE_ACTUAL}"
        BUILD_TARGETS RVXEditor ImageContentValidation
        TAGS product.editor trait.visual trait.gpu backend.opengl suite.smoke
        TIMEOUT 120
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESOURCE_LOCK rvx_gpu
        FIXTURES_SETUP RVXEditorSmokeFreshCapture
    )

    rvx_add_validation_test(
        LANE qualification
        NAME RVXEditorSmokeImageValidation
        COMMAND $<TARGET_FILE:ImageContentValidation>
            --image "${RVX_EDITOR_SMOKE_ACTUAL}"
            --min-width 640
            --min-height 360
            --min-unique-colors 32
            --min-luma-range 32
            --near-black-threshold 8
            --max-near-black-ratio 0.98
            --sample-step 4
        BUILD_TARGETS ImageContentValidation
        TAGS product.editor trait.visual trait.gpu backend.opengl suite.smoke
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        FIXTURES_REQUIRED RVXEditorSmokeFreshCapture
    )

    rvx_add_validation_test(
        LANE qualification
        NAME RVXEditorWorkspacePathSmoke
        COMMAND $<TARGET_FILE:RVXEditor>
            --smoke
            --frames 8
            --windowed
            --screenshot "${RVX_EDITOR_WORKSPACE_PATH_SMOKE_ACTUAL}"
        BUILD_TARGETS RVXEditor ImageContentValidation
        TAGS product.editor trait.visual trait.gpu backend.opengl suite.workspace-path
        TIMEOUT 120
        WORKING_DIRECTORY "${RVX_EDITOR_WORKSPACE_PATH_SMOKE_CWD}"
        RESOURCE_LOCK rvx_gpu
        FIXTURES_SETUP RVXEditorWorkspacePathSmokeFreshCapture
    )

    rvx_add_validation_test(
        LANE qualification
        NAME RVXEditorWorkspacePathSmokeImageValidation
        COMMAND $<TARGET_FILE:ImageContentValidation>
            --image "${RVX_EDITOR_WORKSPACE_PATH_SMOKE_ACTUAL}"
            --min-width 640
            --min-height 360
            --min-unique-colors 32
            --min-luma-range 32
            --near-black-threshold 8
            --max-near-black-ratio 0.98
            --sample-step 4
        BUILD_TARGETS ImageContentValidation
        TAGS product.editor trait.visual trait.gpu backend.opengl suite.workspace-path
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        FIXTURES_REQUIRED RVXEditorWorkspacePathSmokeFreshCapture
    )
endif()
