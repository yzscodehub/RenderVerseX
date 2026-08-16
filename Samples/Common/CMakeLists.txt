# =============================================================================
# Sample Common - shared CLI and report helpers for sample applications
# =============================================================================
find_package(nlohmann_json CONFIG REQUIRED)

add_library(RVX_SampleCommon STATIC
    Private/FrameworkAssessment.cpp
    Private/ModelCameraFraming.cpp
    Private/RuntimeFrameDriver.cpp
    Private/SampleAssetCatalog.cpp
    Private/SampleAnimationLoader.cpp
    Private/SampleCLI.cpp
    Private/SampleEnvironmentLoader.cpp
    Private/SampleLifetimeQualification.cpp
    Private/SampleModelLoader.cpp
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
    RVX::Render
    RVX::Resource
    RVX::ResourceSceneAdapters
    RVX::RHI
    RVX::Runtime
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

add_executable(RVXSampleHDRFixtureWriter
    Tools/SampleHDRFixtureWriter.cpp
)
target_compile_features(RVXSampleHDRFixtureWriter PRIVATE cxx_std_20)

add_executable(RVXSampleInteriorFixtureWriter
    Tools/SampleInteriorFixtureWriter.cpp
)
target_compile_features(RVXSampleInteriorFixtureWriter PRIVATE cxx_std_20)
