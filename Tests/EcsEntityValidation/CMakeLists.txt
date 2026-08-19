rvx_add_gtest(
    NAME EcsEntityValidation
    SOURCES main.cpp
    LIBS RVX::ECS
    LABELS unit
    TIMEOUT 120
)
