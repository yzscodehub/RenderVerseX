add_executable(EcsParticleFeatureBridgeValidation
    main.cpp)
target_link_libraries(EcsParticleFeatureBridgeValidation PRIVATE
    RVX::Particle
    RVX::Water
    RVX::Terrain)
target_compile_features(EcsParticleFeatureBridgeValidation PRIVATE cxx_std_20)
set_property(GLOBAL APPEND PROPERTY RVX_VALIDATION_EXECUTABLE_TARGETS
    EcsParticleFeatureBridgeValidation)
add_test(NAME EcsParticleFeatureBridgeValidation COMMAND EcsParticleFeatureBridgeValidation)
set_tests_properties(EcsParticleFeatureBridgeValidation PROPERTIES
    LABELS "unit;scene;ecs;feature;particle;water;terrain"
    TIMEOUT 30)
