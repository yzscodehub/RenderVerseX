add_executable(EcsWaterFeatureBridgeValidation
    main.cpp)
target_link_libraries(EcsWaterFeatureBridgeValidation PRIVATE RVX::Water)
target_compile_features(EcsWaterFeatureBridgeValidation PRIVATE cxx_std_20)
set_property(GLOBAL APPEND PROPERTY RVX_VALIDATION_EXECUTABLE_TARGETS
    EcsWaterFeatureBridgeValidation)
add_test(NAME EcsWaterFeatureBridgeValidation COMMAND EcsWaterFeatureBridgeValidation)
set_tests_properties(EcsWaterFeatureBridgeValidation PROPERTIES
    LABELS "unit;scene;ecs;feature;water"
    TIMEOUT 30)
