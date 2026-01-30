# =============================================================================
# AI Module - Navigation, Behavior Trees, and Perception
# =============================================================================
add_library(RVX_AI STATIC)

target_sources(RVX_AI PRIVATE
    # Core
    Private/AISubsystem.cpp

    # Navigation
    Private/Navigation/NavMesh.cpp
    Private/Navigation/NavMeshBuilder.cpp
    Private/Navigation/PathFinder.cpp
    Private/Navigation/NavigationAgent.cpp

    # Behavior Tree
    Private/BehaviorTree/BehaviorTree.cpp
    Private/BehaviorTree/BTNode.cpp
    Private/BehaviorTree/Blackboard.cpp

    # Perception
    Private/Perception/AIPerception.cpp
    Private/Perception/SightSense.cpp
    Private/Perception/HearingSense.cpp
)

target_include_directories(RVX_AI PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(RVX_AI PUBLIC
    RVX::Core
    RVX::Geometry
)

# Optional: Link RecastNavigation when available
find_package(recastnavigation CONFIG QUIET)
if(TARGET RecastNavigation::Recast)
    target_link_libraries(RVX_AI PRIVATE 
        RecastNavigation::Recast
        RecastNavigation::Detour
        RecastNavigation::DetourCrowd
    )
    target_compile_definitions(RVX_AI PRIVATE RVX_AI_RECAST=1)
    message(STATUS "AI Module: Using RecastNavigation")
else()
    message(STATUS "AI Module: Using built-in navigation (RecastNavigation not found)")
endif()

add_library(RVX::AI ALIAS RVX_AI)
