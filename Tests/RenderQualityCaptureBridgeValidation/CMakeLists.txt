# Backend-neutral conversion and completion-identity coverage for the native
# RenderGraph capture bridge.  Real GPU readback evidence is owned by the
# RenderQualityLab qualification producer rather than this unit target.
rvx_add_gtest(
    NAME RenderQualityCaptureBridgeValidation
    LANE unit
    SOURCES main.cpp
    LIBS RVX::RenderFoundation
    TAGS subsystem.render trait.contract suite.gfx24-qualification
    SUITE gfx24-qualification
    TIMEOUT 30
)
