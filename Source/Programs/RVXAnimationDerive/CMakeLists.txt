add_executable(RVXAnimationDerive
    Private/AnimationRootMotionDeriveMain.cpp
)

target_link_libraries(RVXAnimationDerive PRIVATE
    RVX::AssetPipeline
)

target_compile_features(RVXAnimationDerive PRIVATE cxx_std_20)

rvx_register_module_target(
    TARGET RVXAnimationDerive
    MODULE RVXAnimationDerive
)
