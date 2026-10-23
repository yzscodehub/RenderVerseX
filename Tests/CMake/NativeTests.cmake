# Backend-native validation is conditional on the enabled production backend.
# GFX24C's RT presets deliberately select only the already-implemented,
# backend-native GFX20/GFX21 dispatch cases. They are opt-in because hosted
# raster correctness may run on a software adapter, whereas these suites must
# be dispatched on the configured hardware backend without a fallback path.
option(RVX_ENABLE_GFX24_HARDWARE_RT_QUALIFICATION
    "Register backend-exclusive GFX24 hardware ray-tracing selectors" OFF)
set(RVX_GFX24_HARDWARE_RT_DX12_TAG "")
set(RVX_GFX24_HARDWARE_RT_VULKAN_TAG "")
if(RVX_ENABLE_GFX24_HARDWARE_RT_QUALIFICATION)
    if(WIN32)
        if(NOT RVX_ENABLE_DX12)
            message(FATAL_ERROR
                "GFX24 hardware RT qualification requires Windows DX12")
        endif()
        set(RVX_GFX24_HARDWARE_RT_DX12_TAG
            suite.gfx24-hardware-rt-dx12)
    elseif(UNIX AND NOT APPLE)
        if(NOT RVX_ENABLE_VULKAN)
            message(FATAL_ERROR
                "GFX24 hardware RT qualification requires Linux Vulkan")
        endif()
        set(RVX_GFX24_HARDWARE_RT_VULKAN_TAG
            suite.gfx24-hardware-rt-vulkan)
    else()
        message(FATAL_ERROR
            "GFX24 hardware RT qualification supports only Windows DX12 or Linux Vulkan")
    endif()
endif()

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
            ${CMAKE_SOURCE_DIR}/Source/Runtime/HAL/Private
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
                DX12Validation/TransparentTemporalValidation.cpp
                DX12Validation/TransmissionDepthValidation.cpp
                DX12Validation/TransmissionFootprintValidation.cpp
                DX12Validation/TransmissionDepthPyramidValidation.cpp
                DX12Validation/TransparentTemporalRasterValidation.cpp
                DX12Validation/NativeDiagnosticsValidation.cpp
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
            ${CMAKE_SOURCE_DIR}/Source/Runtime/RHI_DX12/Private
            ${CMAKE_SOURCE_DIR}/Source/Runtime/Render/Private
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
                VulkanValidation/TransparentTemporalValidation.cpp
                VulkanValidation/TransmissionDepthValidation.cpp
                VulkanValidation/TransmissionFootprintValidation.cpp
                VulkanValidation/TransmissionDepthPyramidValidation.cpp
                VulkanValidation/TransparentTemporalRasterValidation.cpp
        LIBS
            RVX::EngineDevelopmentComposition
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
            ${CMAKE_SOURCE_DIR}/Source/Runtime/RHI_Vulkan/Private
        TAGS backend.vulkan trait.gpu ${RVX_NATIVE_VULKAN_PRIMARY_TAGS}
        TIMEOUT 120
        RESOURCE_LOCK rvx_gpu
    )

    if(NOT TARGET RVXCook)
        message(FATAL_ERROR
            "GFX20 Vulkan ray-tracing validation requires RVXCook")
    endif()

    set(RVX_GFX20_VULKAN_RT_FIXTURE_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/VulkanRayTracingValidation/Fixtures")
    set(RVX_GFX20_VULKAN_RT_SHADER_ROOT
        "${CMAKE_CURRENT_BINARY_DIR}/VulkanRayTracingValidation/CookedShaders")
    set(RVX_GFX20_VULKAN_RT_EVIDENCE
        "${CMAKE_CURRENT_BINARY_DIR}/VulkanRayTracingValidation/vulkan-ray-tracing-evidence.json")

    rvx_add_validation_test(
        LANE native
        NAME VulkanRayTracingCookedShaders
        COMMAND pwsh -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/VulkanRayTracingValidation/CookVulkanRayTracingShaders.ps1"
            -Cooker $<TARGET_FILE:RVXCook>
            -Source "${RVX_GFX20_VULKAN_RT_FIXTURE_DIR}"
            -Catalog "${RVX_GFX20_VULKAN_RT_FIXTURE_DIR}/Programs.rvxshadercatalog.json"
            -Output "${RVX_GFX20_VULKAN_RT_SHADER_ROOT}"
        BUILD_TARGETS RVXCook
        REQUIRED_FILES
            "${CMAKE_CURRENT_SOURCE_DIR}/VulkanRayTracingValidation/CookVulkanRayTracingShaders.ps1"
            "${RVX_GFX20_VULKAN_RT_FIXTURE_DIR}/Programs.rvxshadercatalog.json"
            "${RVX_GFX20_VULKAN_RT_FIXTURE_DIR}/VulkanRayTracingFixture.hlsl"
        TAGS subsystem.rhi backend.vulkan trait.gpu trait.native
            trait.ray-tracing suite.gfx20-vulkan-rt
            ${RVX_GFX24_HARDWARE_RT_VULKAN_TAG}
        SUITE gfx20-vulkan-ray-tracing
        TIMEOUT 300
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        FIXTURES_SETUP VulkanRayTracingCookedShaders
    )

    rvx_add_gtest(
        NAME VulkanRayTracingValidation
        LANE native
        SOURCES VulkanRayTracingValidation/main.cpp
        LIBS
            RVX::Core
            RVX::RHI
            RVX::RHI_BackendFactory
            RVX::ShaderCompiler
            RVX::ShaderContracts
            RVX::ShaderRuntime
            RVX::RHI_Vulkan
            glfw
        INCLUDES
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_SOURCE_DIR}/Source/Runtime/RHI_Vulkan/Private
        TAGS subsystem.rhi backend.vulkan trait.gpu trait.native
            trait.ray-tracing suite.gfx20-vulkan-rt
            ${RVX_GFX24_HARDWARE_RT_VULKAN_TAG}
        SUITE gfx20-vulkan-ray-tracing
        TIMEOUT 180
        RESOURCE_LOCK rvx_gpu
        FIXTURES_REQUIRED VulkanRayTracingCookedShaders
    )

    string(CONCAT RVX_GFX20_VULKAN_RT_ENVIRONMENT_METADATA_CONTENT
        "if(DEFINED VulkanRayTracingValidation_TESTS)\n"
        "    set_tests_properties(\${VulkanRayTracingValidation_TESTS} PROPERTIES\n"
        "        ENVIRONMENT \"RVX_GFX20_VULKAN_RT_SHADER_ROOT=${RVX_GFX20_VULKAN_RT_SHADER_ROOT};RVX_GFX20_VULKAN_RT_EVIDENCE=${RVX_GFX20_VULKAN_RT_EVIDENCE}\"\n"
        "    )\n"
        "endif()\n")
    set(RVX_GFX20_VULKAN_RT_ENVIRONMENT_METADATA_FILE
        "${CMAKE_CURRENT_BINARY_DIR}/VulkanRayTracingValidation_gfx20_environment.cmake")
    file(GENERATE
        OUTPUT "${RVX_GFX20_VULKAN_RT_ENVIRONMENT_METADATA_FILE}"
        CONTENT "${RVX_GFX20_VULKAN_RT_ENVIRONMENT_METADATA_CONTENT}")
    set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
        "${RVX_GFX20_VULKAN_RT_ENVIRONMENT_METADATA_FILE}")
endif()

# GFX21 owns cooked production shader fixtures independently for DX12 and
# Vulkan. The fixture only proves deterministic artifact production; native
# dispatch evidence remains the responsibility of the backend-native tests
# registered by the hybrid RT implementation.
if(RVX_ENABLE_DX12 OR RVX_ENABLE_VULKAN)
    if(NOT TARGET RVXCook)
        message(FATAL_ERROR
            "GFX21 hybrid ray-tracing validation requires RVXCook to stage cooked shader artifacts")
    endif()

    set(RVX_GFX21_HYBRID_RT_SHADER_SOURCE
        "${CMAKE_SOURCE_DIR}/Source/Runtime/Render/Shaders")
    set(RVX_GFX21_HYBRID_RT_SHADER_CATALOG
        "${RVX_GFX21_HYBRID_RT_SHADER_SOURCE}/Programs.rvxshadercatalog.json")
    set(RVX_GFX21_HYBRID_RT_SHADER_ROOT
        "${CMAKE_CURRENT_BINARY_DIR}/HybridRayTracingValidation/CookedShaders")
    set(RVX_GFX21_HYBRID_RT_COOK_SCRIPT
        "${CMAKE_CURRENT_SOURCE_DIR}/HybridRayTracingValidation/CookHybridRayTracingShaders.ps1")

    if(RVX_ENABLE_DX12)
        rvx_add_validation_test(
            LANE native
            NAME HybridRayTracingCookedShadersDX12
            COMMAND pwsh -NoProfile -ExecutionPolicy Bypass
                -File "${RVX_GFX21_HYBRID_RT_COOK_SCRIPT}"
                -Cooker $<TARGET_FILE:RVXCook>
                -Source "${RVX_GFX21_HYBRID_RT_SHADER_SOURCE}"
                -Catalog "${RVX_GFX21_HYBRID_RT_SHADER_CATALOG}"
                -Output "${RVX_GFX21_HYBRID_RT_SHADER_ROOT}/DX12"
                -Backend DX12
            BUILD_TARGETS RVXCook
            REQUIRED_FILES
                "${RVX_GFX21_HYBRID_RT_COOK_SCRIPT}"
                "${RVX_GFX21_HYBRID_RT_SHADER_CATALOG}"
                "${RVX_GFX21_HYBRID_RT_SHADER_SOURCE}/RayTracing/RayTracedShadow.hlsl"
                "${RVX_GFX21_HYBRID_RT_SHADER_SOURCE}/RayTracing/RayTracedReflection.hlsl"
            TAGS subsystem.render subsystem.ray-tracing trait.gpu trait.cooked
                backend.dx12 suite.gfx21-hybrid-rt
                ${RVX_GFX24_HARDWARE_RT_DX12_TAG}
            SUITE gfx21-hybrid-ray-tracing
            TIMEOUT 300
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            FIXTURES_SETUP HybridRayTracingCookedShadersDX12
        )

        set(RVX_GFX21_DX12_EVIDENCE
            "${CMAKE_CURRENT_BINARY_DIR}/HybridRayTracingValidation/dx12-hybrid-ray-tracing-evidence.json")
        rvx_add_gtest(
            NAME HybridRayTracingDX12NativeValidation
            LANE native
            SOURCES
                HybridRayTracingValidation/DX12/main.cpp
                Common/RenderRuntimeTestHarness.cpp
            LIBS
                RVX::Core
                RVX::Render
                RVX::RenderGraph
                RVX::ResourceRuntime
                RVX::RHI
                RVX::RHI_BackendFactory
                RVX::ShaderRuntime
                RVX::Scene
                ${RVX_ALL_BACKENDS}
            INCLUDES
                ${CMAKE_SOURCE_DIR}/Tests
                ${CMAKE_SOURCE_DIR}/Source/Runtime/Geometry/Include
                ${CMAKE_SOURCE_DIR}/Source/Runtime/Render/Include
                ${CMAKE_SOURCE_DIR}/Source/Runtime/Render/Private
                ${CMAKE_SOURCE_DIR}/Source/Runtime/ResourceRuntime/Include
                ${CMAKE_SOURCE_DIR}/Source/Runtime/RHI/Include
                ${CMAKE_SOURCE_DIR}/Source/Runtime/Scene/Include
            DEFINES RVX_GFX21_NATIVE_BACKEND_DX12=1
            TAGS subsystem.render subsystem.ray-tracing trait.gpu trait.native
                backend.dx12 suite.gfx21-hybrid-rt
                ${RVX_GFX24_HARDWARE_RT_DX12_TAG}
            SUITE gfx21-hybrid-ray-tracing
            TIMEOUT 300
            RESOURCE_LOCK rvx_gpu
            FIXTURES_REQUIRED HybridRayTracingCookedShadersDX12
        )

        string(CONCAT RVX_GFX21_DX12_ENVIRONMENT_METADATA_CONTENT
            "if(DEFINED HybridRayTracingDX12NativeValidation_TESTS)\n"
            "    set_tests_properties(\${HybridRayTracingDX12NativeValidation_TESTS} PROPERTIES\n"
            "        ENVIRONMENT \"RVX_GFX21_HYBRID_RT_SHADER_ROOT_DX12=${RVX_GFX21_HYBRID_RT_SHADER_ROOT}/DX12;RVX_GFX21_HYBRID_RT_SOURCE_SHADER_ROOT=${RVX_GFX21_HYBRID_RT_SHADER_SOURCE};RVX_GFX21_HYBRID_RT_EVIDENCE_DX12=${RVX_GFX21_DX12_EVIDENCE}\"\n"
            "    )\n"
            "endif()\n")
        set(RVX_GFX21_DX12_ENVIRONMENT_METADATA_FILE
            "${CMAKE_CURRENT_BINARY_DIR}/HybridRayTracingDX12NativeValidation_gfx21_environment.cmake")
        file(GENERATE
            OUTPUT "${RVX_GFX21_DX12_ENVIRONMENT_METADATA_FILE}"
            CONTENT "${RVX_GFX21_DX12_ENVIRONMENT_METADATA_CONTENT}")
        set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
            "${RVX_GFX21_DX12_ENVIRONMENT_METADATA_FILE}")
    endif()

    if(RVX_ENABLE_VULKAN)
        rvx_add_validation_test(
            LANE native
            NAME HybridRayTracingCookedShadersVulkan
            COMMAND pwsh -NoProfile -ExecutionPolicy Bypass
                -File "${RVX_GFX21_HYBRID_RT_COOK_SCRIPT}"
                -Cooker $<TARGET_FILE:RVXCook>
                -Source "${RVX_GFX21_HYBRID_RT_SHADER_SOURCE}"
                -Catalog "${RVX_GFX21_HYBRID_RT_SHADER_CATALOG}"
                -Output "${RVX_GFX21_HYBRID_RT_SHADER_ROOT}/Vulkan"
                -Backend Vulkan
            BUILD_TARGETS RVXCook
            REQUIRED_FILES
                "${RVX_GFX21_HYBRID_RT_COOK_SCRIPT}"
                "${RVX_GFX21_HYBRID_RT_SHADER_CATALOG}"
                "${RVX_GFX21_HYBRID_RT_SHADER_SOURCE}/RayTracing/RayTracedShadow.hlsl"
                "${RVX_GFX21_HYBRID_RT_SHADER_SOURCE}/RayTracing/RayTracedReflection.hlsl"
            TAGS subsystem.render subsystem.ray-tracing trait.gpu trait.cooked
                backend.vulkan suite.gfx21-hybrid-rt
                ${RVX_GFX24_HARDWARE_RT_VULKAN_TAG}
            SUITE gfx21-hybrid-ray-tracing
            TIMEOUT 300
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            FIXTURES_SETUP HybridRayTracingCookedShadersVulkan
        )

        set(RVX_GFX21_VULKAN_EVIDENCE
            "${CMAKE_CURRENT_BINARY_DIR}/HybridRayTracingValidation/vulkan-hybrid-ray-tracing-evidence.json")
        rvx_add_gtest(
            NAME HybridRayTracingVulkanNativeValidation
            LANE native
            SOURCES
                HybridRayTracingValidation/Vulkan/main.cpp
                Common/RenderRuntimeTestHarness.cpp
            LIBS
                RVX::Core
                RVX::Render
                RVX::RenderGraph
                RVX::ResourceRuntime
                RVX::RHI
                RVX::RHI_BackendFactory
                RVX::ShaderRuntime
                RVX::Scene
                ${RVX_ALL_BACKENDS}
            INCLUDES
                ${CMAKE_SOURCE_DIR}/Tests
                ${CMAKE_SOURCE_DIR}/Source/Runtime/Geometry/Include
                ${CMAKE_SOURCE_DIR}/Source/Runtime/Render/Include
                ${CMAKE_SOURCE_DIR}/Source/Runtime/Render/Private
                ${CMAKE_SOURCE_DIR}/Source/Runtime/ResourceRuntime/Include
                ${CMAKE_SOURCE_DIR}/Source/Runtime/RHI/Include
                ${CMAKE_SOURCE_DIR}/Source/Runtime/Scene/Include
            DEFINES RVX_GFX21_NATIVE_BACKEND_VULKAN=1
            TAGS subsystem.render subsystem.ray-tracing trait.gpu trait.native
                backend.vulkan suite.gfx21-hybrid-rt
                ${RVX_GFX24_HARDWARE_RT_VULKAN_TAG}
            SUITE gfx21-hybrid-ray-tracing
            TIMEOUT 300
            RESOURCE_LOCK rvx_gpu
            FIXTURES_REQUIRED HybridRayTracingCookedShadersVulkan
        )

        string(CONCAT RVX_GFX21_VULKAN_ENVIRONMENT_METADATA_CONTENT
            "if(DEFINED HybridRayTracingVulkanNativeValidation_TESTS)\n"
            "    set_tests_properties(\${HybridRayTracingVulkanNativeValidation_TESTS} PROPERTIES\n"
            "        ENVIRONMENT \"RVX_GFX21_HYBRID_RT_SHADER_ROOT_VULKAN=${RVX_GFX21_HYBRID_RT_SHADER_ROOT}/Vulkan;RVX_GFX21_HYBRID_RT_SOURCE_SHADER_ROOT=${RVX_GFX21_HYBRID_RT_SHADER_SOURCE};RVX_GFX21_HYBRID_RT_EVIDENCE_VULKAN=${RVX_GFX21_VULKAN_EVIDENCE}\"\n"
            "    )\n"
            "endif()\n")
        set(RVX_GFX21_VULKAN_ENVIRONMENT_METADATA_FILE
            "${CMAKE_CURRENT_BINARY_DIR}/HybridRayTracingVulkanNativeValidation_gfx21_environment.cmake")
        file(GENERATE
            OUTPUT "${RVX_GFX21_VULKAN_ENVIRONMENT_METADATA_FILE}"
            CONTENT "${RVX_GFX21_VULKAN_ENVIRONMENT_METADATA_CONTENT}")
        set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
            "${RVX_GFX21_VULKAN_ENVIRONMENT_METADATA_FILE}")
    endif()
endif()

# This target remains deliberately RHI-free. It checks only public frozen-plan
# and scene-lifecycle contracts, while the suite's later native cases own all
# backend dispatch and evidence claims.
rvx_add_gtest(
    NAME HybridRayTracingContractValidation
    LANE unit
    SOURCES HybridRayTracingValidation/Contract/main.cpp
    LIBS RVX::RenderRayTracing
    TAGS subsystem.render subsystem.ray-tracing trait.contract
        suite.gfx21-hybrid-rt
    SUITE gfx21-hybrid-ray-tracing
    TIMEOUT 30
)

if(RVX_ENABLE_GFX24_HARDWARE_RT_QUALIFICATION)
    if(WIN32)
        if(NOT TARGET RVXCook OR
           NOT TARGET HybridRayTracingDX12NativeValidation)
            message(FATAL_ERROR
                "GFX24 DX12 hardware RT aggregate is missing required GFX21 targets")
        endif()
        add_custom_target(RVXValidationSuiteGfx24HardwareRtDx12
            DEPENDS
                RVXCook
                HybridRayTracingDX12NativeValidation)
        rvx_register_module_target(
            TARGET RVXValidationSuiteGfx24HardwareRtDx12
            MODULE Tests
            INFRASTRUCTURE)
    elseif(UNIX AND NOT APPLE)
        if(NOT TARGET RVXCook OR
           NOT TARGET VulkanRayTracingValidation OR
           NOT TARGET HybridRayTracingVulkanNativeValidation)
            message(FATAL_ERROR
                "GFX24 Vulkan hardware RT aggregate is missing required GFX20/GFX21 targets")
        endif()
        add_custom_target(RVXValidationSuiteGfx24HardwareRtVulkan
            DEPENDS
                RVXCook
                VulkanRayTracingValidation
                HybridRayTracingVulkanNativeValidation)
        rvx_register_module_target(
            TARGET RVXValidationSuiteGfx24HardwareRtVulkan
            MODULE Tests
            INFRASTRUCTURE)
    endif()
endif()

# GFX22 validates the one shared weighted-blended translucency composition
# against two production backends. The suite is intentionally all-or-nothing:
# two cooked shader fixtures, two public Sample Host producers, two strict
# report validators, and two RHI-free contract cases make exactly eight CTest
# cases. Keep the source catalog and cooked roots backend-specific so neither
# backend can accidentally consume the other's artifact payload.
if(RVX_ENABLE_DX12 AND RVX_ENABLE_VULKAN)
    if(NOT TARGET RVXCook)
        message(FATAL_ERROR
            "GFX22 weighted-blended OIT validation requires RVXCook to stage cooked shader artifacts")
    endif()
    if(NOT TARGET RenderVerseSamples OR NOT TARGET SampleReportContentValidation)
        message(FATAL_ERROR
            "GFX22 weighted-blended OIT validation requires the public Sample Host and report validator")
    endif()

    set(RVX_GFX22_WEIGHTED_OIT_SHADER_SOURCE
        "${CMAKE_SOURCE_DIR}/Source/Runtime/Render/Shaders")
    set(RVX_GFX22_WEIGHTED_OIT_SHADER_CATALOG
        "${RVX_GFX22_WEIGHTED_OIT_SHADER_SOURCE}/Programs.rvxshadercatalog.json")
    set(RVX_GFX22_WEIGHTED_OIT_SHADER_ROOT
        "${CMAKE_CURRENT_BINARY_DIR}/WeightedBlendedOITValidation/CookedShaders")
    set(RVX_GFX22_WEIGHTED_OIT_COOK_SCRIPT
        "${CMAKE_CURRENT_SOURCE_DIR}/WeightedBlendedOITValidation/CookWeightedBlendedOITShaders.ps1")

    foreach(RVX_GFX22_BACKEND IN ITEMS DX12 Vulkan)
        string(TOLOWER "${RVX_GFX22_BACKEND}" RVX_GFX22_BACKEND_LOWER)
        set(RVX_GFX22_FIXTURE "WeightedBlendedOITCookedShaders${RVX_GFX22_BACKEND}")
        set(RVX_GFX22_EVIDENCE
            "${CMAKE_CURRENT_BINARY_DIR}/WeightedBlendedOITValidation/${RVX_GFX22_BACKEND_LOWER}-weighted-oit-evidence.json")

        rvx_add_validation_test(
            LANE native
            NAME ${RVX_GFX22_FIXTURE}
            COMMAND pwsh -NoProfile -ExecutionPolicy Bypass
                -File "${RVX_GFX22_WEIGHTED_OIT_COOK_SCRIPT}"
                -Cooker $<TARGET_FILE:RVXCook>
                -Source "${RVX_GFX22_WEIGHTED_OIT_SHADER_SOURCE}"
                -Catalog "${RVX_GFX22_WEIGHTED_OIT_SHADER_CATALOG}"
                -Output "${RVX_GFX22_WEIGHTED_OIT_SHADER_ROOT}/${RVX_GFX22_BACKEND}"
                -Backend ${RVX_GFX22_BACKEND}
            BUILD_TARGETS RVXCook
            REQUIRED_FILES
                "${RVX_GFX22_WEIGHTED_OIT_COOK_SCRIPT}"
                "${RVX_GFX22_WEIGHTED_OIT_SHADER_CATALOG}"
                "${RVX_GFX22_WEIGHTED_OIT_SHADER_SOURCE}/DefaultLit.hlsl"
                "${RVX_GFX22_WEIGHTED_OIT_SHADER_SOURCE}/Particle/ParticleBillboard.hlsl"
                "${RVX_GFX22_WEIGHTED_OIT_SHADER_SOURCE}/Translucency/WeightedBlendedOITComposite.hlsl"
            TAGS subsystem.render subsystem.translucency trait.gpu trait.cooked
                backend.${RVX_GFX22_BACKEND_LOWER} suite.gfx22-oit
            SUITE gfx22-oit
            TIMEOUT 300
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            FIXTURES_SETUP ${RVX_GFX22_FIXTURE}
        )
    endforeach()

    foreach(RVX_GFX22_BACKEND IN ITEMS DX12 Vulkan)
        string(TOLOWER "${RVX_GFX22_BACKEND}" RVX_GFX22_BACKEND_LOWER)
        set(RVX_GFX22_FIXTURE "WeightedBlendedOITCookedShaders${RVX_GFX22_BACKEND}")
        set(RVX_GFX22_PRODUCER
            "RenderVerseSamples${RVX_GFX22_BACKEND}GFX22WeightedBlendedOITProducer")
        set(RVX_GFX22_VALIDATOR
            "RenderVerseSamples${RVX_GFX22_BACKEND}GFX22WeightedBlendedOITReportValidation")
        set(RVX_GFX22_EVIDENCE
            "${CMAKE_CURRENT_BINARY_DIR}/WeightedBlendedOITValidation/${RVX_GFX22_BACKEND_LOWER}-weighted-oit-evidence.json")
        set(RVX_GFX22_CAPTURE
            "${CMAKE_CURRENT_BINARY_DIR}/WeightedBlendedOITValidation/${RVX_GFX22_BACKEND_LOWER}-weighted-oit.ppm")

        # The producer is the only writer of the fixed evidence path. It runs
        # the public host, which owns backend selection, cooked-only loading,
        # frame presentation and the immutable execution receipt.
        rvx_add_validation_test(
            LANE native
            NAME ${RVX_GFX22_PRODUCER}
            COMMAND $<TARGET_FILE:RenderVerseSamples>
                --sample transparent-materials
                --backend ${RVX_GFX22_BACKEND}
                --pipeline deferred
                --render-path direct
                --quality high
                --smoke
                --wait-ready
                --ready-timeout-ms 120000
                --ready-max-frames 2400
                --view hero
                --fixed-delta 0.0166666666666667
                --capture-tick 120
                --translucency-qualification-probe
                --runtime-only-composition
                --cooked-shader-root "${RVX_GFX22_WEIGHTED_OIT_SHADER_ROOT}/${RVX_GFX22_BACKEND}"
                --width 640
                --height 360
                --screenshot "${RVX_GFX22_CAPTURE}"
                --report "${RVX_GFX22_EVIDENCE}"
                --diagnostics
                --validation
            BUILD_TARGETS RenderVerseSamples
            TAGS product.samples subsystem.render subsystem.translucency trait.gpu trait.native
                backend.${RVX_GFX22_BACKEND_LOWER} suite.gfx22-oit
            SUITE gfx22-oit
            TIMEOUT 300
            WORKING_DIRECTORY "$<TARGET_FILE_DIR:RenderVerseSamples>"
            RESOURCE_LOCK rvx_gpu
            FIXTURES_REQUIRED ${RVX_GFX22_FIXTURE}
            FIXTURES_SETUP ${RVX_GFX22_PRODUCER}
        )

        # Report validation is deliberately independent from the producer and
        # cannot fabricate execution evidence. It checks the presented frame
        # receipt emitted by the public host after the backend has drained.
        rvx_add_validation_test(
            LANE native
            NAME ${RVX_GFX22_VALIDATOR}
            COMMAND $<TARGET_FILE:SampleReportContentValidation>
                --report "${RVX_GFX22_EVIDENCE}"
                --sample-name transparent-materials
                --category visual
                --backend ${RVX_GFX22_BACKEND_LOWER}
                --require-requested-backend ${RVX_GFX22_BACKEND_LOWER}
                --require-actual-backend ${RVX_GFX22_BACKEND_LOWER}
                --quality high
                --min-frame-count 120
                --require-ready
                --require-readiness-wait
                --render-path direct
                --require-requested-render-path direct
                --require-actual-render-path direct
                --require-direct-execution
                --require-render-pipeline deferred
                --require-translucency-execution
                --require-weighted-blended-oit-execution
                --require-weighted-blended-oit-frame-contract
                --require-cooked-shader-provider
                --require-cooked-shader-root "${RVX_GFX22_WEIGHTED_OIT_SHADER_ROOT}/${RVX_GFX22_BACKEND}"
                --require-particle-vfx-execution
                --require-render-feature-executed 11
                --require-native-validation-available
                --require-native-validation-enabled
                --require-native-validation-read-complete
                --require-native-validation-error-count 0
                --require-native-validation-corruption-count 0
                --forbid-fallback-reasons
                --forbid-render-feature-fallbacks
            BUILD_TARGETS SampleReportContentValidation
            TAGS product.samples subsystem.render subsystem.translucency trait.gpu trait.native
                backend.${RVX_GFX22_BACKEND_LOWER} suite.gfx22-oit
            SUITE gfx22-oit
            TIMEOUT 30
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            FIXTURES_REQUIRED ${RVX_GFX22_PRODUCER}
        )
    endforeach()

    set(RVX_GFX22_PUBLIC_VALIDATION_REGISTERED TRUE)
endif()

# These two tests deliberately link only RenderFoundation. They lock the
# immutable routing/reference contract used by the native tests without
# allowing this required suite to obtain RHI access through a convenience
# target.
rvx_add_gtest(
    NAME WeightedBlendedOITContractValidation
    LANE unit
    SOURCES WeightedBlendedOITValidation/Contract/main.cpp
    LIBS RVX::RenderFoundation
    TAGS subsystem.render subsystem.translucency trait.contract
        suite.gfx22-oit
    SUITE gfx22-oit
    TIMEOUT 30
)

# Retain the focused-build convenience target used by the active GFX22
# checkpoint. It has no test-registration side effect; the suite aggregate is
# still RVXValidationSuiteGfx22Oit, derived from the exact suite label above.
if(RVX_GFX22_PUBLIC_VALIDATION_REGISTERED)
    add_custom_target(WeightedBlendedOITValidation
        DEPENDS
            RVXCook
            RenderVerseSamples
            SampleReportContentValidation
            WeightedBlendedOITContractValidation
    )
    rvx_register_module_target(
        TARGET WeightedBlendedOITValidation
        MODULE Tests
        INFRASTRUCTURE
    )
endif()

# GFX16 is a narrow native proof of the renderer-owned CPU billboard path.
# It owns its own executable so `suite.gfx16-particles` cannot accidentally
# select the full, unrelated DX12/Vulkan native inventories.
if(RVX_ENABLE_DX12 OR RVX_ENABLE_VULKAN)
    if(NOT TARGET RVXCook)
        message(FATAL_ERROR
            "GFX16 particle native validation requires RVXCook to stage cooked shader artifacts")
    endif()

    # GTest metadata is materialized through one configuration-independent
    # file(GENERATE) include. Keep fixture paths configuration-independent as
    # the existing native-test registration model requires. RVXCook owns the
    # backend directory below this catalog root, and the runtime provider
    # applies that same directory component when resolving an artifact.
    set(RVX_PARTICLE_NATIVE_SHADER_ROOT_BASE
        "${CMAKE_CURRENT_BINARY_DIR}/ParticleNativeShaders")
    set(RVX_PARTICLE_NATIVE_FIXTURES "")
    set(RVX_PARTICLE_NATIVE_DEFINES "")
    set(RVX_PARTICLE_NATIVE_ENVIRONMENT "")
    set(RVX_PARTICLE_NATIVE_TAGS
        subsystem.render
        subsystem.particle
        trait.gpu
        trait.native
        suite.gfx16-particles)

    if(RVX_ENABLE_DX12)
        set(RVX_PARTICLE_NATIVE_SHADER_ROOT_DX12
            "${RVX_PARTICLE_NATIVE_SHADER_ROOT_BASE}")
        rvx_add_validation_test(
            LANE native
            NAME ParticleNativeCookedShadersDX12
            COMMAND $<TARGET_FILE:RVXCook>
                --source "${CMAKE_SOURCE_DIR}/Source/Runtime/Render/Shaders"
                --output "${RVX_PARTICLE_NATIVE_SHADER_ROOT_BASE}"
                --shader-catalog "${CMAKE_SOURCE_DIR}/Source/Runtime/Render/Shaders/Programs.rvxshadercatalog.json"
                --shader-backend DX12
                --fail-on-errors
            BUILD_TARGETS RVXCook
            REQUIRED_FILES
                "${CMAKE_SOURCE_DIR}/Source/Runtime/Render/Shaders/Programs.rvxshadercatalog.json"
                "${CMAKE_SOURCE_DIR}/Source/Runtime/Render/Shaders/Particle/ParticleBillboard.hlsl"
            TAGS subsystem.render subsystem.particle trait.gpu trait.native
                backend.dx12 suite.gfx16-particles
            SUITE gfx16-particles
            TIMEOUT 300
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            FIXTURES_SETUP ParticleNativeCookedShadersDX12
        )
        list(APPEND RVX_PARTICLE_NATIVE_FIXTURES ParticleNativeCookedShadersDX12)
        list(APPEND RVX_PARTICLE_NATIVE_DEFINES RVX_GFX16_PARTICLE_DX12_ENABLED=1)
        list(APPEND RVX_PARTICLE_NATIVE_ENVIRONMENT
            "RVX_PARTICLE_NATIVE_SHADER_ROOT_DX12=${RVX_PARTICLE_NATIVE_SHADER_ROOT_DX12}")
        list(APPEND RVX_PARTICLE_NATIVE_TAGS backend.dx12)
    endif()

    if(RVX_ENABLE_VULKAN)
        set(RVX_PARTICLE_NATIVE_SHADER_ROOT_VULKAN
            "${RVX_PARTICLE_NATIVE_SHADER_ROOT_BASE}")
        rvx_add_validation_test(
            LANE native
            NAME ParticleNativeCookedShadersVulkan
            COMMAND $<TARGET_FILE:RVXCook>
                --source "${CMAKE_SOURCE_DIR}/Source/Runtime/Render/Shaders"
                --output "${RVX_PARTICLE_NATIVE_SHADER_ROOT_BASE}"
                --shader-catalog "${CMAKE_SOURCE_DIR}/Source/Runtime/Render/Shaders/Programs.rvxshadercatalog.json"
                --shader-backend Vulkan
                --fail-on-errors
            BUILD_TARGETS RVXCook
            REQUIRED_FILES
                "${CMAKE_SOURCE_DIR}/Source/Runtime/Render/Shaders/Programs.rvxshadercatalog.json"
                "${CMAKE_SOURCE_DIR}/Source/Runtime/Render/Shaders/Particle/ParticleBillboard.hlsl"
            TAGS subsystem.render subsystem.particle trait.gpu trait.native
                backend.vulkan suite.gfx16-particles
            SUITE gfx16-particles
            TIMEOUT 300
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            FIXTURES_SETUP ParticleNativeCookedShadersVulkan
        )
        list(APPEND RVX_PARTICLE_NATIVE_FIXTURES ParticleNativeCookedShadersVulkan)
        list(APPEND RVX_PARTICLE_NATIVE_DEFINES RVX_GFX16_PARTICLE_VULKAN_ENABLED=1)
        list(APPEND RVX_PARTICLE_NATIVE_ENVIRONMENT
            "RVX_PARTICLE_NATIVE_SHADER_ROOT_VULKAN=${RVX_PARTICLE_NATIVE_SHADER_ROOT_VULKAN}")
        list(APPEND RVX_PARTICLE_NATIVE_TAGS backend.vulkan)
    endif()

    rvx_add_gtest(
        NAME ParticleNativeValidation
        LANE native
        SOURCES ParticleNativeValidation/main.cpp
        LIBS
            RVX::Core
            RVX::Render
            RVX::RenderGraph
            RVX::RHI
            RVX::RHI_BackendFactory
            RVX::ShaderRuntime
            ${RVX_ALL_BACKENDS}
        INCLUDES
            ${CMAKE_CURRENT_SOURCE_DIR}
        DEFINES ${RVX_PARTICLE_NATIVE_DEFINES}
        TAGS ${RVX_PARTICLE_NATIVE_TAGS}
        SUITE gfx16-particles
        TIMEOUT 180
        RESOURCE_LOCK rvx_gpu
        FIXTURES_REQUIRED "${RVX_PARTICLE_NATIVE_FIXTURES}"
    )

    # `rvx_add_gtest()` serializes generic metadata list values for CMake
    # includes. That escaping is correct for fixture lists, but CTest's
    # ENVIRONMENT property needs one real semicolon-separated list of
    # assignments. Attach this small, configuration-independent include after
    # the generic metadata so each assignment remains an independent variable.
    string(CONCAT RVX_PARTICLE_NATIVE_ENVIRONMENT_METADATA_CONTENT
        "if(DEFINED ParticleNativeValidation_TESTS)\n"
        "    set_tests_properties(\${ParticleNativeValidation_TESTS} PROPERTIES\n"
        "        ENVIRONMENT \"${RVX_PARTICLE_NATIVE_ENVIRONMENT}\"\n"
        "    )\n"
        "endif()\n")
    set(RVX_PARTICLE_NATIVE_ENVIRONMENT_METADATA_FILE
        "${CMAKE_CURRENT_BINARY_DIR}/ParticleNativeValidation_gfx16_environment.cmake")
    file(GENERATE
        OUTPUT "${RVX_PARTICLE_NATIVE_ENVIRONMENT_METADATA_FILE}"
        CONTENT "${RVX_PARTICLE_NATIVE_ENVIRONMENT_METADATA_CONTENT}")
    set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
        "${RVX_PARTICLE_NATIVE_ENVIRONMENT_METADATA_FILE}")
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
            ${CMAKE_SOURCE_DIR}/Source/Runtime/RHI_DX11/Private
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
