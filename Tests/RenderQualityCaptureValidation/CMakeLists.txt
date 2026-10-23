# The parent only includes this directory for the dedicated GFX24 contract
# preset.  PRE_TEST discovery keeps the same lane/suite metadata as every
# other validation executable.
rvx_add_gtest(
    NAME RenderQualityCaptureValidation
    LANE unit
    SOURCES main.cpp
    LIBS RVX::RenderFoundation
    TAGS subsystem.render trait.contract suite.gfx24-qualification
    SUITE gfx24-qualification
    TIMEOUT 30
)
