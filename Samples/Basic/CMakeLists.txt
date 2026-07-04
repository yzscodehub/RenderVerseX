# =============================================================================
# Basic Samples - focused low-level demonstrations
# =============================================================================

add_subdirectory(BasicRHI)
add_subdirectory(ComputeDemo)

function(rvx_add_basic_sample TARGET_NAME SAMPLE_KIND)
    add_executable(${TARGET_NAME}
        FeatureSamples/main.cpp
    )

    target_compile_definitions(${TARGET_NAME} PRIVATE
        RVX_BASIC_SAMPLE_KIND=${SAMPLE_KIND}
    )

    target_link_libraries(${TARGET_NAME} PRIVATE
        RVX::Core
        RVX::Render
        RVX::Resource
        RVX::SampleCommon
    )

    target_compile_features(${TARGET_NAME} PRIVATE cxx_std_20)

    set_target_properties(${TARGET_NAME} PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
endfunction()

rvx_add_basic_sample(BackendInfoSample 1)
rvx_add_basic_sample(RenderGraphBasics 2)
rvx_add_basic_sample(ResourcePolicySample 3)
rvx_add_basic_sample(TextureUploadSample 4)
rvx_add_basic_sample(MeshMaterialSample 5)
rvx_add_basic_sample(SceneActorComponentSample 6)
rvx_add_basic_sample(PostProcessChainSample 7)
rvx_add_basic_sample(LightingShadowSample 8)
rvx_add_basic_sample(GpuResidencySample 9)
rvx_add_basic_sample(InputCameraSample 10)