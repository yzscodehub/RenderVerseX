# =============================================================================
# RHI_Vulkan Module - Vulkan backend implementation
# =============================================================================
add_library(RVX_RHI_Vulkan STATIC)

target_sources(RVX_RHI_Vulkan PRIVATE
    Private/VulkanDevice.cpp
    Private/VulkanCommandContext.cpp
    Private/VulkanCommandBufferPool.cpp
    Private/VulkanResources.cpp
    Private/VulkanPipeline.cpp
    Private/VulkanDescriptorPool.cpp
    Private/VulkanDescriptorAllocator.cpp
    Private/VulkanSwapChain.cpp
    Private/VulkanMemoryAllocator.cpp
)

target_include_directories(RVX_RHI_Vulkan PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(RVX_RHI_Vulkan PUBLIC
    RVX::RHI
    RVX::ShaderCompiler
    Vulkan::Vulkan
    GPUOpen::VulkanMemoryAllocator
)

add_library(RVX::RHI_Vulkan ALIAS RVX_RHI_Vulkan)
