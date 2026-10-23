add_executable(EcsTerrainFeatureBridgeValidation
    main.cpp)
target_link_libraries(EcsTerrainFeatureBridgeValidation PRIVATE RVX::Terrain)
target_compile_features(EcsTerrainFeatureBridgeValidation PRIVATE cxx_std_20)
rvx_add_validation_test(
    NAME EcsTerrainFeatureBridgeValidation
    LANE unit
    COMMAND EcsTerrainFeatureBridgeValidation
    BUILD_TARGETS EcsTerrainFeatureBridgeValidation
    TAGS subsystem.scene subsystem.ecs subsystem.terrain trait.feature
    TIMEOUT 30)
