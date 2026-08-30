# Completion-owned host artifact writer tests.  Register this directory from
# the existing RVX_ENABLE_GFX24_QUALIFICATION block in Tests/CMakeLists.txt.
rvx_add_gtest(
    NAME RenderQualityQualificationWriterValidation
    LANE unit
    SOURCES main.cpp
    LIBS RVX::SampleCommon
    TAGS subsystem.samples trait.contract suite.gfx24-qualification
    SUITE gfx24-qualification
    TIMEOUT 30
)
