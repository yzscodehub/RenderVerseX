# Shared targets and configuration must be created before any lane file refers
# to them. Registrations themselves remain in their canonical lane files.
option(RVX_ENABLE_EXTERNAL_ASSET_TESTS
    "Register opt-in rendering tests backed by a developer or CI asset library" OFF)
option(RVX_ENABLE_EXTERNAL_ENVIRONMENT_TESTS
    "Register opt-in PBR/IBL tests backed by cataloged external environments" OFF)
option(RVX_ENABLE_LONG_RUNNING_QUALIFICATION_TESTS
    "Register 10,000-frame / five-minute sustained rendering qualifications" OFF)
option(RVX_ENABLE_GFX24_QUALIFICATION
    "Register the hermetic GFX24 RenderQualityLab contract qualification suite" OFF)
option(RVX_ENABLE_GFX24_NATIVE_CANDIDATES
    "Register opt-in DX12 cooked RenderQualityLab visual/performance evidence producers" OFF)
option(RVX_ENABLE_GFX24_PERFORMANCE_CALIBRATION
    "Register the single fixed-GPU GFX24 performance calibration producer" OFF)
set(RVX_EXTERNAL_TEST_ASSET_ROOT "" CACHE PATH
    "Asset library root used by enabled external asset test suites")

set(RVX_ALL_BACKENDS "")
if(RVX_ENABLE_DX11)
    list(APPEND RVX_ALL_BACKENDS RVX::RHI_DX11)
endif()
if(RVX_ENABLE_DX12)
    list(APPEND RVX_ALL_BACKENDS RVX::RHI_DX12)
endif()
if(RVX_ENABLE_VULKAN)
    list(APPEND RVX_ALL_BACKENDS RVX::RHI_Vulkan)
endif()
if(RVX_ENABLE_METAL)
    list(APPEND RVX_ALL_BACKENDS RVX::RHI_Metal)
endif()
if(RVX_ENABLE_OPENGL)
    list(APPEND RVX_ALL_BACKENDS RVX::RHI_OpenGL)
endif()

add_library(RVX_TestCommon STATIC Common/ImageCompare.cpp Common/ImageFile.cpp)
rvx_register_module_target(TARGET RVX_TestCommon MODULE Tests)
target_include_directories(RVX_TestCommon PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(RVX_TestCommon PUBLIC RVX::Core)

add_library(RVX_RenderTestSupport STATIC Common/RenderRuntimeTestSupport.cpp)
rvx_register_module_target(TARGET RVX_RenderTestSupport MODULE Tests)
target_include_directories(RVX_RenderTestSupport PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(RVX_RenderTestSupport PRIVATE ${CMAKE_SOURCE_DIR}/Source/Runtime/Render/Private)
target_link_libraries(RVX_RenderTestSupport PUBLIC RVX::RHI RVX_RenderRuntimeCore)

add_executable(VisualGoldenValidation VisualGoldenValidation/main.cpp)
rvx_register_module_target(TARGET VisualGoldenValidation MODULE Tests)
target_link_libraries(VisualGoldenValidation PRIVATE RVX_TestCommon)
target_compile_features(VisualGoldenValidation PRIVATE cxx_std_20)
target_include_directories(VisualGoldenValidation PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(ImageContentValidation ImageContentValidation/main.cpp)
rvx_register_module_target(TARGET ImageContentValidation MODULE Tests)
target_link_libraries(ImageContentValidation PRIVATE RVX_TestCommon)
target_compile_features(ImageContentValidation PRIVATE cxx_std_20)
target_include_directories(ImageContentValidation PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

find_package(nlohmann_json CONFIG REQUIRED)
add_executable(SampleReportContentValidation SampleReportContentValidation/main.cpp)
rvx_register_module_target(TARGET SampleReportContentValidation MODULE Tests)
target_link_libraries(SampleReportContentValidation PRIVATE nlohmann_json::nlohmann_json)
target_compile_features(SampleReportContentValidation PRIVATE cxx_std_20)

add_executable(SampleQualificationReportValidation SampleQualificationReportValidation/main.cpp)
rvx_register_module_target(TARGET SampleQualificationReportValidation MODULE Tests)
target_compile_features(SampleQualificationReportValidation PRIVATE cxx_std_20)
