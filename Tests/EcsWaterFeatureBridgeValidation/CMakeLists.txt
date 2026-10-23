add_executable(EcsWaterFeatureBridgeValidation
    main.cpp)
target_link_libraries(EcsWaterFeatureBridgeValidation PRIVATE RVX::Water)
target_compile_features(EcsWaterFeatureBridgeValidation PRIVATE cxx_std_20)
rvx_add_validation_test(
    NAME EcsWaterFeatureBridgeValidation
    LANE unit
    COMMAND EcsWaterFeatureBridgeValidation
    BUILD_TARGETS EcsWaterFeatureBridgeValidation
    TAGS subsystem.scene subsystem.ecs subsystem.water trait.feature
    TIMEOUT 30)
