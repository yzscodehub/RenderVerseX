rvx_add_gtest(
    NAME EcsSchedulerValidation
    SOURCES main.cpp
    LIBS RVX::ECS
    LABELS unit
    TIMEOUT 120
)
