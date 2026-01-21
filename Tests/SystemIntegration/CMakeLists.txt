# System Integration Test
add_executable(SystemIntegrationTest main.cpp)

target_link_libraries(SystemIntegrationTest PRIVATE
    RVX::Core
    Spatial
    RVX::Scene
    RVX::Asset
)

target_compile_features(SystemIntegrationTest PRIVATE cxx_std_20)
