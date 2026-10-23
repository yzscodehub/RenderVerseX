add_executable(TerrainEcsRuntimeGatewayValidation
    main.cpp)
target_link_libraries(TerrainEcsRuntimeGatewayValidation PRIVATE RVX::Terrain)
target_compile_features(TerrainEcsRuntimeGatewayValidation PRIVATE cxx_std_20)
rvx_add_validation_test(
    NAME TerrainEcsRuntimeGatewayValidation
    LANE unit
    COMMAND TerrainEcsRuntimeGatewayValidation
    BUILD_TARGETS TerrainEcsRuntimeGatewayValidation
    TAGS subsystem.scene subsystem.ecs subsystem.terrain trait.feature trait.gateway
    TIMEOUT 30)
