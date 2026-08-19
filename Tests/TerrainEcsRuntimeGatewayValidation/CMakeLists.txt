add_executable(TerrainEcsRuntimeGatewayValidation
    main.cpp)
target_link_libraries(TerrainEcsRuntimeGatewayValidation PRIVATE RVX::Terrain)
target_compile_features(TerrainEcsRuntimeGatewayValidation PRIVATE cxx_std_20)
set_property(GLOBAL APPEND PROPERTY RVX_VALIDATION_EXECUTABLE_TARGETS
    TerrainEcsRuntimeGatewayValidation)
add_test(NAME TerrainEcsRuntimeGatewayValidation COMMAND TerrainEcsRuntimeGatewayValidation)
set_tests_properties(TerrainEcsRuntimeGatewayValidation PROPERTIES
    LABELS "unit;scene;ecs;feature;terrain;gateway"
    TIMEOUT 30)
