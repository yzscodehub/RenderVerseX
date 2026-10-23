add_executable(ParticleEcsRuntimeGatewayValidation
    main.cpp)
target_link_libraries(ParticleEcsRuntimeGatewayValidation PRIVATE RVX::Particle)
target_compile_features(ParticleEcsRuntimeGatewayValidation PRIVATE cxx_std_20)
rvx_add_validation_test(
    NAME ParticleEcsRuntimeGatewayValidation
    LANE unit
    COMMAND ParticleEcsRuntimeGatewayValidation
    BUILD_TARGETS ParticleEcsRuntimeGatewayValidation
    TAGS subsystem.scene subsystem.ecs subsystem.particle trait.feature trait.gateway
    TIMEOUT 30
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
