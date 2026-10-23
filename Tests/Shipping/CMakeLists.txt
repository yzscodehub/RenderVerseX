# Standalone Shipping validation composition.  It deliberately avoids the
# normal Tests tree so a product build never discovers GTest or Developer code.
include(${CMAKE_SOURCE_DIR}/cmake/RVXValidation.cmake)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

if(NOT TARGET RVXShippingProduct OR NOT TARGET RVXShippingConsumer)
    message(FATAL_ERROR
        "Shipping validation requires RVXShippingProduct and RVXShippingConsumer")
endif()

if(WIN32 AND RVX_ENABLE_DX12)
    set(RVX_SHIPPING_VALIDATION_BACKEND DX12)
    set(RVX_SHIPPING_VALIDATION_PRESET win_x64_shipping_release)
elseif(APPLE AND RVX_ENABLE_METAL)
    set(RVX_SHIPPING_VALIDATION_BACKEND Metal)
    set(RVX_SHIPPING_VALIDATION_PRESET mac_arm64_shipping_release)
elseif(UNIX AND RVX_ENABLE_VULKAN)
    set(RVX_SHIPPING_VALIDATION_BACKEND Vulkan)
    set(RVX_SHIPPING_VALIDATION_PRESET linux_x64_shipping_release)
else()
    message(FATAL_ERROR
        "Shipping validation requires the platform production backend")
endif()

find_package(Git QUIET)
set(RVX_SHIPPING_SOURCE_REVISION "unknown")
if(Git_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE}
            -c safe.directory=${CMAKE_SOURCE_DIR}
            rev-parse HEAD
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE RVX_SHIPPING_GIT_RESULT
        OUTPUT_VARIABLE RVX_SHIPPING_GIT_OUTPUT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(RVX_SHIPPING_GIT_RESULT EQUAL 0 AND RVX_SHIPPING_GIT_OUTPUT)
        set(RVX_SHIPPING_SOURCE_REVISION "${RVX_SHIPPING_GIT_OUTPUT}")
    endif()
endif()
string(LENGTH "${RVX_SHIPPING_SOURCE_REVISION}"
    RVX_SHIPPING_SOURCE_REVISION_LENGTH)
if(NOT RVX_SHIPPING_SOURCE_REVISION MATCHES "^[0-9A-Fa-f]+$" OR
   RVX_SHIPPING_SOURCE_REVISION_LENGTH LESS 7 OR
   RVX_SHIPPING_SOURCE_REVISION_LENGTH GREATER 64)
    message(FATAL_ERROR
        "Shipping validation requires an exact source commit identity")
endif()

include(${CMAKE_SOURCE_DIR}/Tests/CMake/ShippingTests.cmake)
rvx_finalize_validation()
