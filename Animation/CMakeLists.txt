# =============================================================================
# Animation Module - Complete animation system with runtime playback
# =============================================================================
add_library(RVX_Animation STATIC)

# Source files
target_sources(RVX_Animation PRIVATE
    # Core
    Private/Core/AnimationCompression.cpp
    
    # Runtime
    Private/Runtime/SkeletonPose.cpp
    Private/Runtime/AnimationEvaluator.cpp
    Private/Runtime/AnimationPlayer.cpp
    Private/Runtime/RootMotion.cpp
    
    # Blend
    Private/Blend/BlendNode.cpp
    Private/Blend/Blend1D.cpp
    Private/Blend/Blend2D.cpp
    Private/Blend/BlendTree.cpp
    
    # State Machine
    Private/State/AnimationState.cpp
    Private/State/StateTransition.cpp
    Private/State/AnimationStateMachine.cpp
    
    # IK
    Private/IK/TwoBoneIK.cpp
    Private/IK/FABRIK.cpp
)

# Include directories
target_include_directories(RVX_Animation PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

# Dependencies
target_link_libraries(RVX_Animation PUBLIC
    RVX::Core
)

# Alias for cleaner target names
add_library(RVX::Animation ALIAS RVX_Animation)
