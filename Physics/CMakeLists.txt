# =============================================================================
# Physics Module - Physics simulation with Jolt backend
# =============================================================================
add_library(RVX_Physics STATIC)

target_sources(RVX_Physics PRIVATE
    # Core
    Private/PhysicsWorld.cpp
    Private/RigidBody.cpp
    Private/Collider.cpp
    
    # Shapes
    Private/Shapes/CollisionShape.cpp
    
    # Constraints
    Private/Constraints/Constraint.cpp
    Private/Constraints/ConstraintSolver.cpp
    
    # Queries
    Private/Query/RaycastQuery.cpp
    Private/Query/ShapeQuery.cpp
    
    # Character Controller
    Private/CharacterController.cpp
    
    # Built-in Physics Engine
    Private/BuiltIn/Integrator.cpp
    Private/BuiltIn/CollisionDetection.cpp
    Private/BuiltIn/CollisionResponse.cpp
    
    # Backend Abstraction
    Private/Backend/BuiltInBackend.cpp
    Private/Backend/JoltBackend.cpp
)

target_include_directories(RVX_Physics PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(RVX_Physics PUBLIC
    RVX::Core
)

# Link Geometry module for collision algorithms
target_link_libraries(RVX_Physics PUBLIC
    RVX::Geometry
)

# Optional: Link Jolt Physics when available
if(TARGET Jolt::Jolt)
    target_link_libraries(RVX_Physics PRIVATE Jolt::Jolt)
    target_compile_definitions(RVX_Physics PRIVATE RVX_PHYSICS_JOLT=1)
    message(STATUS "Physics: Using Jolt Physics backend")
else()
    message(STATUS "Physics: Using built-in physics engine")
endif()

add_library(RVX::Physics ALIAS RVX_Physics)
