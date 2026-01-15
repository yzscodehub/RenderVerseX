# =============================================================================
# Triangle Sample - Interactive triangle rendering demo with mouse rotation
# =============================================================================
add_executable(Triangle
    main.cpp
)

target_link_libraries(Triangle PRIVATE
    RVX::RenderGraph
    RVX::Engine
    glfw
)

# Link enabled backends
if(RVX_ENABLE_DX11)
    target_link_libraries(Triangle PRIVATE RVX::RHI_DX11)
endif()

if(RVX_ENABLE_DX12)
    target_link_libraries(Triangle PRIVATE RVX::RHI_DX12)
endif()

if(RVX_ENABLE_VULKAN)
    target_link_libraries(Triangle PRIVATE RVX::RHI_Vulkan)
endif()

# Copy shaders to output directory
add_custom_command(TARGET Triangle POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/Shaders
    $<TARGET_FILE_DIR:Triangle>/Shaders
)
