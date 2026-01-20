# =============================================================================
# RHI_Metal Module - Apple Metal backend implementation
# =============================================================================
add_library(RVX_RHI_Metal STATIC)

target_sources(RVX_RHI_Metal PRIVATE
    Private/MetalDevice.mm
    Private/MetalResources.mm
    Private/MetalCommandContext.mm
    Private/MetalPipeline.mm
    Private/MetalSwapChain.mm
    Private/MetalSynchronization.mm
)

target_include_directories(RVX_RHI_Metal PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_include_directories(RVX_RHI_Metal PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Private
)

target_link_libraries(RVX_RHI_Metal PUBLIC
    RVX::RHI
    RVX::ShaderCompiler
)

# Link Apple frameworks
target_link_libraries(RVX_RHI_Metal PRIVATE
    "-framework Metal"
    "-framework MetalKit"
    "-framework QuartzCore"
    "-framework Foundation"
)

if(RVX_PLATFORM_MACOS)
    target_link_libraries(RVX_RHI_Metal PRIVATE
        "-framework Cocoa"
    )
elseif(RVX_PLATFORM_IOS)
    target_link_libraries(RVX_RHI_Metal PRIVATE
        "-framework UIKit"
    )
endif()

# Enable ARC (Automatic Reference Counting) for Objective-C++ files
set_source_files_properties(
    Private/MetalDevice.mm
    Private/MetalResources.mm
    Private/MetalCommandContext.mm
    Private/MetalPipeline.mm
    Private/MetalSwapChain.mm
    Private/MetalSynchronization.mm
    PROPERTIES COMPILE_FLAGS "-fobjc-arc"
)

add_library(RVX::RHI_Metal ALIAS RVX_RHI_Metal)
