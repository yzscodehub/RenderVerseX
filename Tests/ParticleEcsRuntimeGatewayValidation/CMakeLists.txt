add_executable(ParticleEcsRuntimeGatewayValidation
    main.cpp)
target_link_libraries(ParticleEcsRuntimeGatewayValidation PRIVATE RVX::Particle)
target_compile_features(ParticleEcsRuntimeGatewayValidation PRIVATE cxx_std_20)
set_property(GLOBAL APPEND PROPERTY RVX_VALIDATION_EXECUTABLE_TARGETS
    ParticleEcsRuntimeGatewayValidation)
add_test(NAME ParticleEcsRuntimeGatewayValidation COMMAND ParticleEcsRuntimeGatewayValidation)
set_tests_properties(ParticleEcsRuntimeGatewayValidation PROPERTIES
    LABELS "unit;scene;ecs;feature;particle;gateway"
    TIMEOUT 30
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
