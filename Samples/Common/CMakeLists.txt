# =============================================================================
# Sample Common - shared CLI and report helpers for sample applications
# =============================================================================
find_package(nlohmann_json CONFIG REQUIRED)

# Narrow compile-contract surface for scenario-only libraries. It exports only
# Samples' own public headers. Cross-module contract roots are applied PRIVATE
# to each concrete scenario target by rvx_apply_scenario_contract_include_roots,
# so they remain visible in the CMake include-edge inventory without leaking
# through an INTERFACE or becoming runtime link dependencies.
add_library(RVX_SampleScenarioContracts INTERFACE)
target_include_directories(RVX_SampleScenarioContracts INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
)
function(rvx_apply_scenario_contract_include_roots _rvx_scenario_target)
    if(NOT TARGET "${_rvx_scenario_target}")
        message(FATAL_ERROR
            "Scenario contract include roots require an existing target: ${_rvx_scenario_target}")
    endif()
    foreach(_rvx_dependency IN LISTS ARGN)
        get_target_property(
            _rvx_include_roots
            "${_rvx_dependency}"
            INTERFACE_INCLUDE_DIRECTORIES)
        if(NOT _rvx_include_roots OR
           _rvx_include_roots STREQUAL "_rvx_include_roots-NOTFOUND")
            message(FATAL_ERROR
                "Scenario compile contract requires auditable public include roots from ${_rvx_dependency}")
        endif()
        target_include_directories(
            "${_rvx_scenario_target}" PRIVATE ${_rvx_include_roots})
    endforeach()
endfunction()
add_library(RVX::SampleScenarioContracts ALIAS RVX_SampleScenarioContracts)

add_library(RVX_SampleCommon STATIC
    Private/FrameworkAssessment.cpp
    Private/ModelCameraFraming.cpp
    Private/RuntimeFrameDriver.cpp
    Private/RenderQualityPerformanceWriter.cpp
    Private/RenderQualityQualificationWriter.cpp
    Private/SampleRenderQualityDiagnostics.cpp
    Private/SampleAssetCatalog.cpp
    Private/SampleAnimationLoader.cpp
    Private/SampleCLI.cpp
    Private/SampleEnvironmentLoader.cpp
    Private/SampleLifetimeQualification.cpp
    Private/SampleManifest.cpp
    Private/SampleModelLoader.cpp
    Private/SampleParticleEffectLoader.cpp
    Private/SampleOrbitCameraController.cpp
    Private/SampleRegistry.cpp
    Private/SampleRunner.cpp
    Private/SampleScenarioDiagnostics.cpp
    Private/SampleSceneLifetimeScope.cpp
    Private/SampleScreenshotWriter.cpp
)

target_include_directories(RVX_SampleCommon PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
)

target_link_libraries(RVX_SampleCommon PUBLIC
    RVX::Core
    RVX::Engine
    RVX::Particle
    RVX::Render
    RVX::ResourceRuntime
    RVX::ResourceSceneAdapters
    RVX::RHI
    RVX::RuntimeServices
    RVX::Scene
    RVX::World
)

target_link_libraries(RVX_SampleCommon PRIVATE
    nlohmann_json::nlohmann_json
)

if(WIN32)
    target_link_libraries(RVX_SampleCommon PRIVATE Psapi)
endif()

add_library(RVX::SampleCommon ALIAS RVX_SampleCommon)

# Executable roots choose exactly one composition.  Keeping these small static
# libraries outside SampleCommon means scenario/host code cannot accidentally
# gain a Developer importer/compiler edge merely by linking shared helpers.
add_library(RVX_SampleDevelopmentComposition STATIC
    Private/SampleDevelopmentComposition.cpp
)
target_include_directories(RVX_SampleDevelopmentComposition PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
)
target_link_libraries(RVX_SampleDevelopmentComposition
    PUBLIC
        RVX::SampleCommon
    PRIVATE
        RVX::AssetImport
        RVX::EngineDevelopmentComposition
        RVX::EngineRuntimeOnlyComposition
)
target_compile_features(RVX_SampleDevelopmentComposition PUBLIC cxx_std_20)
add_library(RVX::SampleDevelopmentComposition
    ALIAS RVX_SampleDevelopmentComposition)

add_library(RVX_SampleCookedComposition STATIC
    Private/SampleCookedComposition.cpp
)
target_include_directories(RVX_SampleCookedComposition PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
)
target_link_libraries(RVX_SampleCookedComposition
    PUBLIC
        RVX::SampleCommon
    PRIVATE
        RVX::EngineRuntimeOnlyComposition
)
target_compile_features(RVX_SampleCookedComposition PUBLIC cxx_std_20)
add_library(RVX::SampleCookedComposition
    ALIAS RVX_SampleCookedComposition)

add_executable(RVXSampleHDRFixtureWriter
    Tools/SampleHDRFixtureWriter.cpp
)
target_compile_features(RVXSampleHDRFixtureWriter PRIVATE cxx_std_20)

add_executable(RVXSampleInteriorFixtureWriter
    Tools/SampleInteriorFixtureWriter.cpp
)
target_compile_features(RVXSampleInteriorFixtureWriter PRIVATE cxx_std_20)

add_executable(RVXSampleTransparentMaterialsFixtureWriter
    Tools/SampleTransparentMaterialsFixtureWriter.cpp
)
target_compile_features(RVXSampleTransparentMaterialsFixtureWriter PRIVATE cxx_std_20)

add_executable(RVXSampleParticleVfxFixtureWriter
    Tools/SampleParticleVfxFixtureWriter.cpp
)
target_compile_features(RVXSampleParticleVfxFixtureWriter PRIVATE cxx_std_20)

# Build-time publisher for the GFX14 cooked-only Sample asset.  It links only
# Runtime resource contracts and verifies RVXCook output before emitting the
# staged catalog declaration; it is not part of the Sample runtime closure.
add_executable(RVXSampleCookedCatalogWriter
    Tools/SampleCookedCatalogWriter.cpp
)
target_compile_features(RVXSampleCookedCatalogWriter PRIVATE cxx_std_20)
target_link_libraries(RVXSampleCookedCatalogWriter PRIVATE
    RVX::Core
    RVX::ResourceRuntime
    nlohmann_json::nlohmann_json
)

# Filters an already-admitted staging catalog to the exact package-only
# closure consumed by the cooked RenderQualityLab product.  It is a build-time
# publisher and is never linked by a Sample runtime target.
add_executable(RVXSampleCookedProductCatalogWriter
    Tools/SampleCookedProductCatalogWriter.cpp
)
target_compile_features(RVXSampleCookedProductCatalogWriter PRIVATE cxx_std_20)
target_link_libraries(RVXSampleCookedProductCatalogWriter PRIVATE
    nlohmann_json::nlohmann_json
)

rvx_register_module_target(
    TARGET RVX_SampleCommon
    MODULE Samples
    ALIASES RVX::SampleCommon
)
rvx_register_module_target(
    TARGET RVX_SampleDevelopmentComposition
    MODULE Samples
    ALIASES RVX::SampleDevelopmentComposition
)
rvx_register_module_target(
    TARGET RVX_SampleCookedComposition
    MODULE Samples
    ALIASES RVX::SampleCookedComposition
)
rvx_register_module_target(
    TARGET RVX_SampleScenarioContracts
    MODULE Samples
    INFRASTRUCTURE
    ALIASES RVX::SampleScenarioContracts
)
rvx_register_module_target(
    TARGET RVXSampleHDRFixtureWriter
    MODULE Samples
)
rvx_register_module_target(
    TARGET RVXSampleInteriorFixtureWriter
    MODULE Samples
)
rvx_register_module_target(
    TARGET RVXSampleTransparentMaterialsFixtureWriter
    MODULE Samples
)
rvx_register_module_target(
    TARGET RVXSampleParticleVfxFixtureWriter
    MODULE Samples
)
rvx_register_module_target(
    TARGET RVXSampleCookedCatalogWriter
    MODULE Samples
)
rvx_register_module_target(
    TARGET RVXSampleCookedProductCatalogWriter
    MODULE Samples
)
