add_executable(EcsParticleFeatureBridgeValidation
    main.cpp)
target_link_libraries(EcsParticleFeatureBridgeValidation PRIVATE
    RVX::Particle
    RVX::Water
    RVX::Terrain)
target_compile_features(EcsParticleFeatureBridgeValidation PRIVATE cxx_std_20)
rvx_add_validation_test(
    NAME EcsParticleFeatureBridgeValidation
    LANE unit
    COMMAND EcsParticleFeatureBridgeValidation
    BUILD_TARGETS EcsParticleFeatureBridgeValidation
    TAGS subsystem.scene subsystem.ecs subsystem.particle subsystem.water subsystem.terrain trait.feature
    TIMEOUT 30)
