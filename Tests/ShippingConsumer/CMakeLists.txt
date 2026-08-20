add_executable(RVXShippingConsumer
    main.cpp
)

target_link_libraries(RVXShippingConsumer PRIVATE
    RVXShippingProduct
)

target_compile_features(RVXShippingConsumer PRIVATE cxx_std_20)

rvx_register_module_target(
    TARGET RVXShippingConsumer
    MODULE Tests
)
