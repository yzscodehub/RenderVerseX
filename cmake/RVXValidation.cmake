# =============================================================================
# RVX validation registration
# =============================================================================
#
# All validation tests declare exactly one primary lane.  The lane aggregates
# are build-only conveniences; CTest labels are the source of test selection.

include_guard(GLOBAL)

set(RVX_VALIDATION_LANES
    architecture
    unit
    integration
    native
    qualification
    shipping
)

function(_rvx_validation_require_lane lane context)
    if(NOT lane)
        message(FATAL_ERROR "${context} requires an explicit LANE")
    endif()

    list(FIND RVX_VALIDATION_LANES "${lane}" _rvx_validation_lane_index)
    if(_rvx_validation_lane_index EQUAL -1)
        message(FATAL_ERROR
            "${context} requires LANE to be one of: ${RVX_VALIDATION_LANES}; got '${lane}'")
    endif()
endfunction()

function(_rvx_validation_append_unique_global property_name)
    get_property(_rvx_validation_current GLOBAL PROPERTY "${property_name}")
    foreach(_rvx_validation_value IN LISTS ARGN)
        if(NOT _rvx_validation_value IN_LIST _rvx_validation_current)
            list(APPEND _rvx_validation_current "${_rvx_validation_value}")
        endif()
    endforeach()
    set_property(GLOBAL PROPERTY "${property_name}" "${_rvx_validation_current}")
endfunction()

function(_rvx_validation_register_target target_name lane)
    if(NOT TARGET "${target_name}")
        message(FATAL_ERROR
            "Validation target '${target_name}' must exist before it can be registered")
    endif()

    _rvx_validation_require_lane("${lane}" "Validation target '${target_name}'")
    string(TOUPPER "${lane}" _rvx_validation_lane_upper)
    _rvx_validation_append_unique_global(RVX_VALIDATION_ALL_TARGETS "${target_name}")
    _rvx_validation_append_unique_global(
        "RVX_VALIDATION_LANE_${_rvx_validation_lane_upper}_TARGETS"
        "${target_name}"
    )
endfunction()

function(_rvx_validation_register_test_module_target_if_needed target_name)
    if(NOT COMMAND rvx_register_module_target)
        message(FATAL_ERROR
            "Validation registration requires RVXModuleInventory.cmake")
    endif()
    if(NOT TARGET "${target_name}")
        message(FATAL_ERROR
            "Cannot register missing validation build target '${target_name}'")
    endif()

    get_target_property(_rvx_validation_module_id
        "${target_name}" RVX_MODULE_ID)
    if(NOT "${_rvx_validation_module_id}" STREQUAL
       "_rvx_validation_module_id-NOTFOUND")
        return()
    endif()

    get_target_property(_rvx_validation_source_dir
        "${target_name}" SOURCE_DIR)
    file(RELATIVE_PATH _rvx_validation_source_relative
        "${CMAKE_SOURCE_DIR}/Tests"
        "${_rvx_validation_source_dir}")
    if(_rvx_validation_source_relative MATCHES "^\\.\\.([/\\\\]|$)" OR
       IS_ABSOLUTE "${_rvx_validation_source_relative}")
        return()
    endif()

    rvx_register_module_target(
        TARGET "${target_name}"
        MODULE Tests
    )
endfunction()

function(_rvx_validation_validate_metadata lane tags suite context)
    _rvx_validation_require_lane("${lane}" "${context}")

    if(suite)
        if(suite MATCHES ";" OR NOT suite MATCHES "^[A-Za-z0-9][A-Za-z0-9_.-]*$")
            message(FATAL_ERROR
                "${context} SUITE must be one approved suite identifier; got '${suite}'")
        endif()
    endif()

    foreach(_rvx_validation_tag IN LISTS tags)
        if(NOT _rvx_validation_tag MATCHES
           "^(product|subsystem|platform|backend|trait|suite)\\.[A-Za-z0-9][A-Za-z0-9_.-]*$")
            message(FATAL_ERROR
                "${context} TAGS entry '${_rvx_validation_tag}' must use an approved "
                "product.*, subsystem.*, platform.*, backend.*, trait.* or suite.* namespace")
        endif()
    endforeach()
endfunction()

function(_rvx_validation_compose_labels out_labels lane tags suite)
    set(_rvx_validation_labels "lane.${lane}")

    foreach(_rvx_validation_tag IN LISTS tags)
        if(NOT _rvx_validation_tag STREQUAL "")
            list(APPEND _rvx_validation_labels "${_rvx_validation_tag}")
        endif()
    endforeach()

    if(suite)
        list(APPEND _rvx_validation_labels "suite.${suite}")
    endif()

    list(REMOVE_DUPLICATES _rvx_validation_labels)
    set("${out_labels}" "${_rvx_validation_labels}" PARENT_SCOPE)
endfunction()

function(_rvx_validation_escape_property_value out_value value)
    string(REPLACE ";" "\\;" _rvx_validation_escaped "${value}")
    set("${out_value}" "${_rvx_validation_escaped}" PARENT_SCOPE)
endfunction()

# PRE_TEST discovery creates the individual GTest cases only when CTest runs.
# Attach optional metadata from a follow-up CTest include, where the generated
# <target>_TESTS list is available. This avoids passing empty key/value pairs
# to gtest_discover_tests(), which shifts the remaining properties.
function(_rvx_validation_attach_gtest_metadata target_name)
    set(options WILL_FAIL)
    set(one_value_args RESOURCE_LOCK PROCESSORS)
    set(multi_value_args
        ENVIRONMENT
        REQUIRED_FILES
        FIXTURES_SETUP
        FIXTURES_REQUIRED
        FIXTURES_CLEANUP
        DEPENDS
    )
    cmake_parse_arguments(PARSE_ARGV 1 RVX_GTEST_METADATA
        "${options}"
        "${one_value_args}"
        "${multi_value_args}"
    )

    set(_rvx_validation_property_lines "")
    foreach(_rvx_validation_property
        ENVIRONMENT
        REQUIRED_FILES
        FIXTURES_SETUP
        FIXTURES_REQUIRED
        FIXTURES_CLEANUP
        DEPENDS
        RESOURCE_LOCK
        PROCESSORS
    )
        if(RVX_GTEST_METADATA_${_rvx_validation_property})
            _rvx_validation_escape_property_value(
                _rvx_validation_escaped_value
                "${RVX_GTEST_METADATA_${_rvx_validation_property}}"
            )
            string(APPEND _rvx_validation_property_lines
                "        ${_rvx_validation_property} \"${_rvx_validation_escaped_value}\"\n")
        endif()
    endforeach()
    if(RVX_GTEST_METADATA_WILL_FAIL)
        string(APPEND _rvx_validation_property_lines "        WILL_FAIL TRUE\n")
    endif()

    if(_rvx_validation_property_lines)
        set(_rvx_validation_metadata_file
            "${CMAKE_CURRENT_BINARY_DIR}/${target_name}_rvx_metadata.cmake")
        string(CONCAT _rvx_validation_metadata_content
            "if(DEFINED ${target_name}_TESTS)\n"
            "    set_tests_properties(\${${target_name}_TESTS} PROPERTIES\n"
            "${_rvx_validation_property_lines}"
            "    )\n"
            "endif()\n"
        )
        file(GENERATE
            OUTPUT "${_rvx_validation_metadata_file}"
            CONTENT "${_rvx_validation_metadata_content}"
        )
        set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
            "${_rvx_validation_metadata_file}")
    endif()
endfunction()

# Registers a GoogleTest executable. Tests are discovered immediately before
# CTest execution so all of their CTest metadata is shared consistently.
function(rvx_add_gtest)
    set(options WILL_FAIL)
    set(one_value_args
        NAME
        LANE
        TIMEOUT
        SUITE
        WORKING_DIRECTORY
        RESOURCE_LOCK
        PROCESSORS
    )
    set(multi_value_args
        SOURCES
        LIBS
        INCLUDES
        DEFINES
        TAGS
        ENVIRONMENT
        REQUIRED_FILES
        FIXTURES_SETUP
        FIXTURES_REQUIRED
        FIXTURES_CLEANUP
        DEPENDS
    )
    cmake_parse_arguments(PARSE_ARGV 0 RVX_GTEST
        "${options}"
        "${one_value_args}"
        "${multi_value_args}"
    )

    if(RVX_GTEST_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "rvx_add_gtest received unparsed arguments: ${RVX_GTEST_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT RVX_GTEST_NAME)
        message(FATAL_ERROR "rvx_add_gtest requires NAME")
    endif()
    if(NOT RVX_GTEST_SOURCES)
        message(FATAL_ERROR "rvx_add_gtest(${RVX_GTEST_NAME}) requires SOURCES")
    endif()

    _rvx_validation_validate_metadata(
        "${RVX_GTEST_LANE}"
        "${RVX_GTEST_TAGS}"
        "${RVX_GTEST_SUITE}"
        "rvx_add_gtest(${RVX_GTEST_NAME})"
    )
    if(NOT RVX_GTEST_TIMEOUT)
        set(RVX_GTEST_TIMEOUT 120)
    endif()

    add_executable(${RVX_GTEST_NAME} ${RVX_GTEST_SOURCES})
    rvx_register_module_target(
        TARGET "${RVX_GTEST_NAME}"
        MODULE Tests
    )
    target_link_libraries(${RVX_GTEST_NAME} PRIVATE
        GTest::gtest_main
        ${RVX_GTEST_LIBS}
    )
    target_compile_features(${RVX_GTEST_NAME} PRIVATE cxx_std_20)

    if(RVX_GTEST_INCLUDES)
        target_include_directories(${RVX_GTEST_NAME} PRIVATE ${RVX_GTEST_INCLUDES})
    endif()
    if(RVX_GTEST_DEFINES)
        target_compile_definitions(${RVX_GTEST_NAME} PRIVATE ${RVX_GTEST_DEFINES})
    endif()

    _rvx_validation_compose_labels(
        _rvx_validation_labels
        "${RVX_GTEST_LANE}"
        "${RVX_GTEST_TAGS}"
        "${RVX_GTEST_SUITE}"
    )
    _rvx_validation_escape_property_value(
        _rvx_validation_discovery_labels
        "${_rvx_validation_labels}"
    )
    if(RVX_GTEST_WORKING_DIRECTORY)
        gtest_discover_tests(${RVX_GTEST_NAME}
            WORKING_DIRECTORY "${RVX_GTEST_WORKING_DIRECTORY}"
            DISCOVERY_MODE PRE_TEST
            DISCOVERY_TIMEOUT 30
            PROPERTIES
                LABELS "${_rvx_validation_discovery_labels}"
                TIMEOUT "${RVX_GTEST_TIMEOUT}"
        )
    else()
        gtest_discover_tests(${RVX_GTEST_NAME}
            DISCOVERY_MODE PRE_TEST
            DISCOVERY_TIMEOUT 30
            PROPERTIES
                LABELS "${_rvx_validation_discovery_labels}"
                TIMEOUT "${RVX_GTEST_TIMEOUT}"
        )
    endif()

    set(_rvx_validation_gtest_metadata_arguments "")
    foreach(_rvx_validation_metadata
        ENVIRONMENT
        REQUIRED_FILES
        FIXTURES_SETUP
        FIXTURES_REQUIRED
        FIXTURES_CLEANUP
        DEPENDS
        RESOURCE_LOCK
        PROCESSORS
    )
        if(RVX_GTEST_${_rvx_validation_metadata})
            list(APPEND _rvx_validation_gtest_metadata_arguments
                ${_rvx_validation_metadata}
                ${RVX_GTEST_${_rvx_validation_metadata}}
            )
        endif()
    endforeach()
    if(RVX_GTEST_WILL_FAIL)
        list(APPEND _rvx_validation_gtest_metadata_arguments WILL_FAIL)
    endif()
    _rvx_validation_attach_gtest_metadata(
        "${RVX_GTEST_NAME}"
        ${_rvx_validation_gtest_metadata_arguments}
    )

    _rvx_validation_register_target("${RVX_GTEST_NAME}" "${RVX_GTEST_LANE}")
endfunction()

# Registers a non-GTest CTest entry. BUILD_TARGETS join the matching build
# lane; metadata maps one-to-one to CTest properties.
function(rvx_add_validation_test)
    set(options WILL_FAIL)
    set(one_value_args
        NAME
        LANE
        TIMEOUT
        SUITE
        WORKING_DIRECTORY
        RESOURCE_LOCK
        PROCESSORS
    )
    set(multi_value_args
        COMMAND
        BUILD_TARGETS
        TAGS
        ENVIRONMENT
        REQUIRED_FILES
        FIXTURES_SETUP
        FIXTURES_REQUIRED
        FIXTURES_CLEANUP
        DEPENDS
    )
    cmake_parse_arguments(PARSE_ARGV 0 RVX_TEST
        "${options}"
        "${one_value_args}"
        "${multi_value_args}"
    )

    if(RVX_TEST_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "rvx_add_validation_test received unparsed arguments: ${RVX_TEST_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT RVX_TEST_NAME)
        message(FATAL_ERROR "rvx_add_validation_test requires NAME")
    endif()
    if(NOT RVX_TEST_COMMAND)
        message(FATAL_ERROR "rvx_add_validation_test(${RVX_TEST_NAME}) requires COMMAND")
    endif()

    _rvx_validation_validate_metadata(
        "${RVX_TEST_LANE}"
        "${RVX_TEST_TAGS}"
        "${RVX_TEST_SUITE}"
        "rvx_add_validation_test(${RVX_TEST_NAME})"
    )
    if(NOT RVX_TEST_TIMEOUT)
        set(RVX_TEST_TIMEOUT 120)
    endif()

    add_test(NAME ${RVX_TEST_NAME} COMMAND ${RVX_TEST_COMMAND})
    _rvx_validation_compose_labels(
        _rvx_validation_labels
        "${RVX_TEST_LANE}"
        "${RVX_TEST_TAGS}"
        "${RVX_TEST_SUITE}"
    )
    set_tests_properties(${RVX_TEST_NAME} PROPERTIES
        LABELS "${_rvx_validation_labels}"
        TIMEOUT "${RVX_TEST_TIMEOUT}"
    )

    foreach(_rvx_validation_property
        WORKING_DIRECTORY
        ENVIRONMENT
        REQUIRED_FILES
        FIXTURES_SETUP
        FIXTURES_REQUIRED
        FIXTURES_CLEANUP
        DEPENDS
        RESOURCE_LOCK
        PROCESSORS
    )
        if(RVX_TEST_${_rvx_validation_property})
            set_tests_properties(${RVX_TEST_NAME} PROPERTIES
                ${_rvx_validation_property} "${RVX_TEST_${_rvx_validation_property}}")
        endif()
    endforeach()
    if(RVX_TEST_WILL_FAIL)
        set_tests_properties(${RVX_TEST_NAME} PROPERTIES WILL_FAIL TRUE)
    endif()

    foreach(_rvx_validation_target IN LISTS RVX_TEST_BUILD_TARGETS)
        _rvx_validation_register_test_module_target_if_needed(
            "${_rvx_validation_target}")
        _rvx_validation_register_target("${_rvx_validation_target}" "${RVX_TEST_LANE}")
    endforeach()
endfunction()

# Creates the six fixed lane aggregates and the all-lanes aggregate. CTest
# discovery remains PRE_TEST, so a required lane with no discovered tests is a
# runtime gate failure rather than a silently successful configure.
function(rvx_finalize_validation)
    get_property(_rvx_validation_finalized GLOBAL PROPERTY RVX_VALIDATION_FINALIZED)
    if(_rvx_validation_finalized)
        message(FATAL_ERROR "rvx_finalize_validation may only be called once")
    endif()

    get_property(_rvx_validation_all_targets GLOBAL PROPERTY RVX_VALIDATION_ALL_TARGETS)
    list(REMOVE_DUPLICATES _rvx_validation_all_targets)
    foreach(_rvx_validation_target IN LISTS _rvx_validation_all_targets)
        if(NOT TARGET "${_rvx_validation_target}")
            message(FATAL_ERROR
                "Validation aggregate references missing target '${_rvx_validation_target}'")
        endif()
    endforeach()

    foreach(_rvx_validation_lane IN LISTS RVX_VALIDATION_LANES)
        string(TOUPPER "${_rvx_validation_lane}" _rvx_validation_lane_upper)
        get_property(_rvx_validation_lane_targets GLOBAL
            PROPERTY "RVX_VALIDATION_LANE_${_rvx_validation_lane_upper}_TARGETS")
        list(REMOVE_DUPLICATES _rvx_validation_lane_targets)
        string(SUBSTRING "${_rvx_validation_lane}" 0 1 _rvx_validation_lane_initial)
        string(TOUPPER "${_rvx_validation_lane_initial}" _rvx_validation_lane_initial)
        string(SUBSTRING "${_rvx_validation_lane}" 1 -1 _rvx_validation_lane_remainder)
        set(_rvx_validation_aggregate
            "RVXValidation${_rvx_validation_lane_initial}${_rvx_validation_lane_remainder}")
        if(TARGET "${_rvx_validation_aggregate}")
            message(FATAL_ERROR
                "Validation aggregate '${_rvx_validation_aggregate}' already exists")
        endif()
        add_custom_target(${_rvx_validation_aggregate}
            DEPENDS ${_rvx_validation_lane_targets}
        )
        rvx_register_module_target(
            TARGET "${_rvx_validation_aggregate}"
            MODULE Tests
            INFRASTRUCTURE
        )
    endforeach()

    if(TARGET RVXValidationAll)
        message(FATAL_ERROR "Validation aggregate 'RVXValidationAll' already exists")
    endif()
    add_custom_target(RVXValidationAll DEPENDS ${_rvx_validation_all_targets})
    rvx_register_module_target(
        TARGET RVXValidationAll
        MODULE Tests
        INFRASTRUCTURE
    )

    set_property(GLOBAL PROPERTY RVX_VALIDATION_FINALIZED TRUE)
endfunction()
