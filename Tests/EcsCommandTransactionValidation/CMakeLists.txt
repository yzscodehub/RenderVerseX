rvx_add_gtest(
    NAME EcsCommandTransactionValidation
    SOURCES main.cpp
    LIBS RVX::ECS
    LABELS unit
    TIMEOUT 120
)
