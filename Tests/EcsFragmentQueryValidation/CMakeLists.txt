rvx_add_gtest(
    NAME EcsFragmentQueryValidation
    SOURCES main.cpp
    LIBS RVX::ECS
    LABELS unit
    TIMEOUT 120
)
