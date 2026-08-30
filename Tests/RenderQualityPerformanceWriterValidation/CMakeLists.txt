# Completion-owned host performance report writer tests. This directory is
# enabled only with the GFX24 qualification contract.
rvx_add_gtest(
    NAME RenderQualityPerformanceWriterValidation
    LANE unit
    SOURCES main.cpp
    LIBS RVX::SampleCommon
    TAGS subsystem.samples trait.contract suite.gfx24-qualification
    SUITE gfx24-qualification
    TIMEOUT 30
)
