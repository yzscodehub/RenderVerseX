# Hermetic GFX24 session ownership and fail-closed evidence coverage. Native
# query readback remains qualified by the RenderQualityLab producer.
rvx_add_gtest(
    NAME RenderQualityPerformanceSessionValidation
    LANE unit
    SOURCES main.cpp
    LIBS RVX::RenderFoundation
    TAGS subsystem.render trait.contract suite.gfx24-qualification
    SUITE gfx24-qualification
    TIMEOUT 30
)
