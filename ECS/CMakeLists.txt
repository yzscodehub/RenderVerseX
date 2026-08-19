# =============================================================================
# ECS Module - Runtime sparse-set entity and fragment kernel
# =============================================================================
add_library(RVX_ECS STATIC)

target_sources(RVX_ECS PRIVATE
    Private/Commands.cpp
    Private/ProcessorScheduler.cpp
    Private/Registry.cpp
    Private/StructuralJournal.cpp
)

target_include_directories(RVX_ECS PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(RVX_ECS PUBLIC
    RVX::Core
)

add_library(RVX::ECS ALIAS RVX_ECS)
