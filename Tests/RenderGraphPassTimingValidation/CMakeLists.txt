# Focused hermetic contract coverage. Native timestamp-query evidence remains
# owned by the GFX24 native qualification producer rather than this unit test.
rvx_add_gtest(
    NAME RenderGraphPassTimingValidation
    LANE unit
    SOURCES main.cpp
    LIBS RVX::RenderFoundation
    TAGS subsystem.render trait.contract suite.gfx24-qualification
    SUITE gfx24-qualification
    TIMEOUT 30
)
