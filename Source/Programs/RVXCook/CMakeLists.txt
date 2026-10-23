add_executable(RVXCook
    Private/CookMain.cpp
)

target_link_libraries(RVXCook PRIVATE
    RVX::AssetPipeline
    RVX::Core
)

target_compile_features(RVXCook PRIVATE cxx_std_20)

rvx_register_module_target(
    TARGET RVXCook
    MODULE RVXCook
)
