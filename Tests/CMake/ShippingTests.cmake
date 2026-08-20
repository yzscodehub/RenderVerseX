# Cooked artifacts are produced by an RVXCook executable from a distinct
# Developer/Programs host-tools build.  The Shipping target graph sees only
# its path and therefore cannot acquire a build edge back to Developer code.
set(RVX_SHIPPING_FIXTURE_ROOT
    "${CMAKE_CURRENT_BINARY_DIR}/ShippingFixtures/$<CONFIG>")
set(RVX_SHIPPING_COOKED_ROOT
    "${RVX_SHIPPING_FIXTURE_ROOT}/Cooked/Resources")
set(RVX_SHIPPING_SHADER_ROOT
    "${RVX_SHIPPING_FIXTURE_ROOT}/Cooked/Shaders")
set(RVX_SHIPPING_FIXTURE_RECEIPT
    "${RVX_SHIPPING_FIXTURE_ROOT}/shipping-fixture-receipt.json")
set(RVX_SHIPPING_SMOKE_REPORT
    "${RVX_SHIPPING_FIXTURE_ROOT}/shipping-smoke.json")
set(RVX_SHIPPING_MODULE_REQUIRED_FILES
    "${CMAKE_SOURCE_DIR}/Docs/module-boundaries.json"
    "${RVX_MODULE_INVENTORY}")

rvx_add_validation_test(
    LANE shipping
    NAME Shipping.TargetClosure
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_SOURCE_DIR}/Scripts/check_shipping_target_closure.py
        --root ${CMAKE_SOURCE_DIR}
        --manifest ${CMAKE_SOURCE_DIR}/Docs/module-boundaries.json
        --module-inventory ${RVX_MODULE_INVENTORY}
        --product-target RVXShippingProduct
        --consumer-target RVXShippingConsumer
    BUILD_TARGETS RVXShippingProduct RVXShippingConsumer
    TAGS product.shipping trait.closure
    SUITE required-shipping
    REQUIRED_FILES ${RVX_SHIPPING_MODULE_REQUIRED_FILES}
    TIMEOUT 60
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

rvx_add_validation_test(
    LANE shipping
    NAME Shipping.HostCookFixture
    COMMAND ${CMAKE_COMMAND}
        -DRVX_COOK_EXECUTABLE=${RVX_SHIPPING_HOST_COOK_EXECUTABLE}
        -DRVX_SOURCE_ROOT=${CMAKE_SOURCE_DIR}
        -DRVX_BINARY_FIXTURE_ROOT=${RVX_SHIPPING_FIXTURE_ROOT}
        -DRVX_BACKEND=${RVX_SHIPPING_VALIDATION_BACKEND}
        -DRVX_COMMIT=${RVX_SHIPPING_SOURCE_REVISION}
        -DRVX_PRESET=${RVX_SHIPPING_VALIDATION_PRESET}
        -P ${CMAKE_SOURCE_DIR}/Scripts/run_shipping_fixture.cmake
    TAGS product.shipping subsystem.resource subsystem.shader trait.fixture
    SUITE required-shipping
    REQUIRED_FILES
        "${RVX_SHIPPING_HOST_COOK_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/Tests/Fixtures/ModelViewer/R7Triangle.gltf"
        "${CMAKE_SOURCE_DIR}/Source/Runtime/Render/Shaders/Programs.rvxshadercatalog.json"
    FIXTURES_SETUP ShippingCookedArtifacts
    TIMEOUT 900
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

rvx_add_validation_test(
    LANE shipping
    NAME Shipping.CookedOnlySmoke
    COMMAND $<TARGET_FILE:RVXShippingConsumer>
        --backend ${RVX_SHIPPING_VALIDATION_BACKEND}
        --cooked-root ${RVX_SHIPPING_COOKED_ROOT}
        --shader-root ${RVX_SHIPPING_SHADER_ROOT}
        --model cooked://R7Triangle.rva
        --fixture-receipt ${RVX_SHIPPING_FIXTURE_RECEIPT}
        --commit ${RVX_SHIPPING_SOURCE_REVISION}
        --preset ${RVX_SHIPPING_VALIDATION_PRESET}
        --frames 8
        --width 320
        --height 180
        --report ${RVX_SHIPPING_SMOKE_REPORT}
    BUILD_TARGETS RVXShippingConsumer
    TAGS product.shipping subsystem.engine subsystem.render trait.gpu
        backend.${RVX_SHIPPING_VALIDATION_BACKEND}
    SUITE required-shipping
    REQUIRED_FILES
        "${RVX_SHIPPING_FIXTURE_RECEIPT}"
        "${RVX_SHIPPING_COOKED_ROOT}/R7Triangle.rva"
    FIXTURES_REQUIRED ShippingCookedArtifacts
    FIXTURES_SETUP ShippingSmokeEvidence
    RESOURCE_LOCK rvx_gpu
    TIMEOUT 240
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:RVXShippingConsumer>"
)

rvx_add_validation_test(
    LANE shipping
    NAME Shipping.SmokeEvidence
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_SOURCE_DIR}/Scripts/check_shipping_evidence.py
        --root ${CMAKE_SOURCE_DIR}
        --receipt ${RVX_SHIPPING_FIXTURE_RECEIPT}
        --report ${RVX_SHIPPING_SMOKE_REPORT}
        --expected-backend ${RVX_SHIPPING_VALIDATION_BACKEND}
        --expected-commit ${RVX_SHIPPING_SOURCE_REVISION}
        --expected-preset ${RVX_SHIPPING_VALIDATION_PRESET}
    TAGS product.shipping trait.report
    SUITE required-shipping
    REQUIRED_FILES
        "${RVX_SHIPPING_FIXTURE_RECEIPT}"
        "${RVX_SHIPPING_SMOKE_REPORT}"
    FIXTURES_REQUIRED "ShippingCookedArtifacts;ShippingSmokeEvidence"
    TIMEOUT 30
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

set(RVX_SHIPPING_DYNAMIC_ALLOW_ARGS "")
foreach(RVX_SHIPPING_DEPENDENCY_TARGET fmt::fmt spdlog::spdlog glfw)
    if(TARGET ${RVX_SHIPPING_DEPENDENCY_TARGET})
        list(APPEND RVX_SHIPPING_DYNAMIC_ALLOW_ARGS
            --allow "$<TARGET_FILE_NAME:${RVX_SHIPPING_DEPENDENCY_TARGET}>")
    endif()
endforeach()

rvx_add_validation_test(
    LANE shipping
    NAME Shipping.DynamicDependencies
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_SOURCE_DIR}/Scripts/check_shipping_dynamic_dependencies.py
        --root ${CMAKE_SOURCE_DIR}
        --binary $<TARGET_FILE:RVXShippingConsumer>
        --platform auto
        --policy ${CMAKE_SOURCE_DIR}/Scripts/shipping-dynamic-dependency-policy.json
        ${RVX_SHIPPING_DYNAMIC_ALLOW_ARGS}
    BUILD_TARGETS RVXShippingConsumer
    TAGS product.shipping trait.closure trait.dynamic
    SUITE required-shipping
    REQUIRED_FILES
        "$<TARGET_FILE:RVXShippingConsumer>"
        "${CMAKE_SOURCE_DIR}/Scripts/shipping-dynamic-dependency-policy.json"
    FIXTURES_REQUIRED ShippingSmokeEvidence
    TIMEOUT 60
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
