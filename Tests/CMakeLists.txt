# =============================================================================
# Tests - validation composition
# =============================================================================

find_package(GTest CONFIG REQUIRED)
include(GoogleTest)
include(${CMAKE_SOURCE_DIR}/cmake/RVXValidation.cmake)

include(${CMAKE_CURRENT_LIST_DIR}/CMake/TestSupport.cmake)

# Keep the existing independently buildable ECS validation targets. Each child
# registers its own explicit lane through rvx_add_gtest().
add_subdirectory(EcsParticleFeatureBridgeValidation)
add_subdirectory(EcsWaterFeatureBridgeValidation)
add_subdirectory(EcsTerrainFeatureBridgeValidation)
add_subdirectory(ParticleEcsRuntimeGatewayValidation)
add_subdirectory(WaterEcsRuntimeGatewayValidation)
add_subdirectory(TerrainEcsRuntimeGatewayValidation)

include(${CMAKE_CURRENT_LIST_DIR}/CMake/ArchitectureTests.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/CMake/UnitTests.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/CMake/IntegrationTests.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/CMake/NativeTests.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/CMake/QualificationTests.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/CMake/ShippingTests.cmake)

file(GLOB TEST_SHADERS "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/*.hlsl")
foreach(SHADER ${TEST_SHADERS})
    get_filename_component(SHADER_NAME ${SHADER} NAME)
    configure_file(${SHADER} ${CMAKE_CURRENT_BINARY_DIR}/Shaders/${SHADER_NAME} COPYONLY)
endforeach()

rvx_finalize_validation()
