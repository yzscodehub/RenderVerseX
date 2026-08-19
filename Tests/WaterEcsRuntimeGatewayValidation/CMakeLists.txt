add_executable(WaterEcsRuntimeGatewayValidation
    main.cpp)
target_link_libraries(WaterEcsRuntimeGatewayValidation PRIVATE RVX::Water)
target_compile_features(WaterEcsRuntimeGatewayValidation PRIVATE cxx_std_20)
set_property(GLOBAL APPEND PROPERTY RVX_VALIDATION_EXECUTABLE_TARGETS
    WaterEcsRuntimeGatewayValidation)
add_test(NAME WaterEcsRuntimeGatewayValidation COMMAND WaterEcsRuntimeGatewayValidation)
set_tests_properties(WaterEcsRuntimeGatewayValidation PROPERTIES
    LABELS "unit;scene;ecs;feature;water;gateway"
    TIMEOUT 30)
