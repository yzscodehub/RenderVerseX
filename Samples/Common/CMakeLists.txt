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
    RVX::AssetImport
    RVX::EngineDevelopmentComposition
    RVX::EngineRuntimeOnlyComposition
    nlohmann_json::nlohmann_json
)

if(WIN32)
    target_link_libraries(RVX_SampleCommon PRIVATE Psapi)
endif()

add_library(RVX::SampleCommon ALIAS RVX_SampleCommon)

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

rvx_register_module_target(
    TARGET RVX_SampleCommon
    MODULE Samples
    ALIASES RVX::SampleCommon
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
