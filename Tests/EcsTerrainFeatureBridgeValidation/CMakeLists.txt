add_executable(EcsTerrainFeatureBridgeValidation
    main.cpp)
target_link_libraries(EcsTerrainFeatureBridgeValidation PRIVATE RVX::Terrain)
target_compile_features(EcsTerrainFeatureBridgeValidation PRIVATE cxx_std_20)
set_property(GLOBAL APPEND PROPERTY RVX_VALIDATION_EXECUTABLE_TARGETS
    EcsTerrainFeatureBridgeValidation)
add_test(NAME EcsTerrainFeatureBridgeValidation COMMAND EcsTerrainFeatureBridgeValidation)
set_tests_properties(EcsTerrainFeatureBridgeValidation PROPERTIES
    LABELS "unit;scene;ecs;feature;terrain"
    TIMEOUT 30)
