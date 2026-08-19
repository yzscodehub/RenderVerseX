add_executable(WaterEcsRuntimeGatewayValidation
    main.cpp)
target_link_libraries(WaterEcsRuntimeGatewayValidation PRIVATE RVX::Water)
target_compile_features(WaterEcsRuntimeGatewayValidation PRIVATE cxx_std_20)
rvx_add_validation_test(
    NAME WaterEcsRuntimeGatewayValidation
    LANE unit
    COMMAND WaterEcsRuntimeGatewayValidation
    BUILD_TARGETS WaterEcsRuntimeGatewayValidation
    TAGS subsystem.scene subsystem.ecs subsystem.water trait.feature trait.gateway
    TIMEOUT 30)
