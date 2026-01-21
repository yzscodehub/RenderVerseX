# Spatial Module - Spatial indexing and query infrastructure
# Provides: ISpatialIndex, BVHIndex, QueryFilter, SpatialQuery
#
# Note: Geometric primitives (AABB, Sphere, Frustum, Ray) are now in Core/Math.

add_library(Spatial STATIC
    # Index implementations
    Private/Index/BVHIndex.cpp
    Private/Index/SpatialFactory.cpp
)

target_include_directories(Spatial
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Include
)

target_link_libraries(Spatial
    PUBLIC
        RVX::Core
)

# Set C++20 standard
target_compile_features(Spatial PUBLIC cxx_std_20)
