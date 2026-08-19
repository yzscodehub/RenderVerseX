# =============================================================================
# RVX validation registration
# =============================================================================
#
# This compatibility layer centralizes validation registration while the
# existing test tree still uses LABELS-based classification.  C02 can turn off
# RVX_VALIDATION_ENABLE_LEGACY_LANE_INFERENCE once every test declares exactly
# one LANE.

include_guard(GLOBAL)

set(RVX_VALIDATION_LANES
    architecture
    unit
    integration
    native
    qualification
    shipping
)

option(RVX_VALIDATION_ENABLE_LEGACY_LANE_INFERENCE
    "Infer validation lanes from legacy LABELS when LANE is omitted"
    ON
)

function(_rvx_validation_require_lane lane context)
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

    # Preserve the pre-C01 property so existing inventory code remains valid
    # until it is replaced by rvx_finalize_validation().
    _rvx_validation_append_unique_global(RVX_VALIDATION_EXECUTABLE_TARGETS "${target_name}")
    _rvx_validation_append_unique_global(RVX_VALIDATION_ALL_TARGETS "${target_name}")

    if(lane)
        _rvx_validation_require_lane("${lane}" "Validation target '${target_name}'")
        string(TOUPPER "${lane}" _rvx_validation_lane_upper)
        _rvx_validation_append_unique_global(
            "RVX_VALIDATION_LANE_${_rvx_validation_lane_upper}_TARGETS"
            "${target_name}"
        )
        set_property(TARGET "${target_name}" PROPERTY RVX_VALIDATION_LANE "${lane}")
    endif()
endfunction()

function(_rvx_validation_infer_legacy_lanes labels out_lanes)
    set(_rvx_validation_lanes "")
    if(RVX_VALIDATION_ENABLE_LEGACY_LANE_INFERENCE)
        foreach(_rvx_validation_lane IN LISTS RVX_VALIDATION_LANES)
            list(FIND labels "${_rvx_validation_lane}" _rvx_validation_label_index)
            if(NOT _rvx_validation_label_index EQUAL -1)
                list(APPEND _rvx_validation_lanes "${_rvx_validation_lane}")
            endif()
        endforeach()
    endif()
    set("${out_lanes}" "${_rvx_validation_lanes}" PARENT_SCOPE)
endfunction()

function(_rvx_validation_validate_metadata lane labels tags suite context)
    foreach(_rvx_validation_tag IN LISTS tags)
        if(_rvx_validation_tag MATCHES "^lane\\.")
            message(FATAL_ERROR
                "${context} TAGS may not declare '${_rvx_validation_tag}'; use the LANE argument instead")
        endif()
    endforeach()

    if(lane)
        foreach(_rvx_validation_label IN LISTS labels)
            if(_rvx_validation_label MATCHES "^lane\\.")
                message(FATAL_ERROR
                    "${context} declares LANE '${lane}' and may not also inject lane label "
                    "'${_rvx_validation_label}' through LABELS")
            endif()
        endforeach()

        if(suite MATCHES "^lane\\.")
            message(FATAL_ERROR
                "${context} declares LANE '${lane}' and may not use lane metadata as SUITE '${suite}'")
        endif()
        if(suite MATCHES ";")
            message(FATAL_ERROR "${context} SUITE must be a single suite identifier")
        endif()
    endif()
endfunction()

function(_rvx_validation_compose_labels out_labels lane labels tags suite)
    set(_rvx_validation_labels ${labels})

    if(lane)
        list(APPEND _rvx_validation_labels "lane.${lane}")
    endif()

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

# Registers a GoogleTest executable.  The original C01 call contract remains
# valid: NAME, SOURCES, LIBS, INCLUDES, DEFINES, LABELS, TIMEOUT and
# WORKING_DIRECTORY.  LANE/TAGS/SUITE are additive metadata for new callers.
function(rvx_add_gtest)
    set(options "")
    set(one_value_args NAME LANE TIMEOUT LABELS SUITE WORKING_DIRECTORY)
    set(multi_value_args SOURCES LIBS INCLUDES DEFINES TAGS)
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

    if(RVX_GTEST_LANE)
        _rvx_validation_require_lane("${RVX_GTEST_LANE}" "rvx_add_gtest(${RVX_GTEST_NAME})")
    endif()
    _rvx_validation_validate_metadata(
        "${RVX_GTEST_LANE}"
        "${RVX_GTEST_LABELS}"
        "${RVX_GTEST_TAGS}"
        "${RVX_GTEST_SUITE}"
        "rvx_add_gtest(${RVX_GTEST_NAME})"
    )

    if(NOT RVX_GTEST_TIMEOUT)
        set(RVX_GTEST_TIMEOUT 120)
    endif()

    add_executable(${RVX_GTEST_NAME}
        ${RVX_GTEST_SOURCES}
    )
    target_link_libraries(${RVX_GTEST_NAME} PRIVATE
        GTest::gtest_main
        ${RVX_GTEST_LIBS}
    )
    target_compile_features(${RVX_GTEST_NAME} PRIVATE cxx_std_20)

    if(RVX_GTEST_INCLUDES)
        target_include_directories(${RVX_GTEST_NAME} PRIVATE
            ${RVX_GTEST_INCLUDES}
        )
    endif()

    if(RVX_GTEST_DEFINES)
        target_compile_definitions(${RVX_GTEST_NAME} PRIVATE
            ${RVX_GTEST_DEFINES}
        )
    endif()

    _rvx_validation_compose_labels(
        _rvx_validation_labels
        "${RVX_GTEST_LANE}"
        "${RVX_GTEST_LABELS}"
        "${RVX_GTEST_TAGS}"
        "${RVX_GTEST_SUITE}"
    )

    # PRE_TEST intentionally matches the pre-C01 helper.  It keeps test
    # discovery out of the build and does not change existing CTest behavior.
    if(RVX_GTEST_WORKING_DIRECTORY)
        gtest_discover_tests(${RVX_GTEST_NAME}
            WORKING_DIRECTORY "${RVX_GTEST_WORKING_DIRECTORY}"
            DISCOVERY_MODE PRE_TEST
            DISCOVERY_TIMEOUT 30
            PROPERTIES
                LABELS "${_rvx_validation_labels}"
                TIMEOUT ${RVX_GTEST_TIMEOUT}
        )
    else()
        gtest_discover_tests(${RVX_GTEST_NAME}
            DISCOVERY_MODE PRE_TEST
            DISCOVERY_TIMEOUT 30
            PROPERTIES
                LABELS "${_rvx_validation_labels}"
                TIMEOUT ${RVX_GTEST_TIMEOUT}
        )
    endif()

    if(RVX_GTEST_LANE)
        _rvx_validation_register_target("${RVX_GTEST_NAME}" "${RVX_GTEST_LANE}")
    else()
        _rvx_validation_register_target("${RVX_GTEST_NAME}" "")
        _rvx_validation_infer_legacy_lanes("${_rvx_validation_labels}" _rvx_validation_legacy_lanes)
        foreach(_rvx_validation_lane IN LISTS _rvx_validation_legacy_lanes)
            _rvx_validation_register_target("${RVX_GTEST_NAME}" "${_rvx_validation_lane}")
        endforeach()
    endif()
endfunction()

# Registers a non-GTest CTest entry.  BUILD_TARGETS are added to validation
# aggregate build targets; COMMAND retains normal add_test semantics.
function(rvx_add_validation_test)
    set(options WILL_FAIL)
    set(one_value_args NAME LANE TIMEOUT SUITE WORKING_DIRECTORY RESOURCE_LOCK PROCESSORS)
    set(multi_value_args
        COMMAND
        BUILD_TARGETS
        LABELS
        TAGS
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
    if(RVX_TEST_LANE)
        _rvx_validation_require_lane("${RVX_TEST_LANE}" "rvx_add_validation_test(${RVX_TEST_NAME})")
    endif()
    _rvx_validation_validate_metadata(
        "${RVX_TEST_LANE}"
        "${RVX_TEST_LABELS}"
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
        "${RVX_TEST_LABELS}"
        "${RVX_TEST_TAGS}"
        "${RVX_TEST_SUITE}"
    )
    set_tests_properties(${RVX_TEST_NAME} PROPERTIES
        LABELS "${_rvx_validation_labels}"
        TIMEOUT ${RVX_TEST_TIMEOUT}
    )

    if(RVX_TEST_WORKING_DIRECTORY)
        set_tests_properties(${RVX_TEST_NAME} PROPERTIES
            WORKING_DIRECTORY "${RVX_TEST_WORKING_DIRECTORY}"
        )
    endif()
    if(RVX_TEST_FIXTURES_SETUP)
        set_tests_properties(${RVX_TEST_NAME} PROPERTIES
            FIXTURES_SETUP "${RVX_TEST_FIXTURES_SETUP}"
        )
    endif()
    if(RVX_TEST_FIXTURES_REQUIRED)
        set_tests_properties(${RVX_TEST_NAME} PROPERTIES
            FIXTURES_REQUIRED "${RVX_TEST_FIXTURES_REQUIRED}"
        )
    endif()
    if(RVX_TEST_FIXTURES_CLEANUP)
        set_tests_properties(${RVX_TEST_NAME} PROPERTIES
            FIXTURES_CLEANUP "${RVX_TEST_FIXTURES_CLEANUP}"
        )
    endif()
    if(RVX_TEST_DEPENDS)
        set_tests_properties(${RVX_TEST_NAME} PROPERTIES DEPENDS "${RVX_TEST_DEPENDS}")
    endif()
    if(RVX_TEST_RESOURCE_LOCK)
        set_tests_properties(${RVX_TEST_NAME} PROPERTIES RESOURCE_LOCK "${RVX_TEST_RESOURCE_LOCK}")
    endif()
    if(RVX_TEST_PROCESSORS)
        set_tests_properties(${RVX_TEST_NAME} PROPERTIES PROCESSORS "${RVX_TEST_PROCESSORS}")
    endif()
    if(RVX_TEST_WILL_FAIL)
        set_tests_properties(${RVX_TEST_NAME} PROPERTIES WILL_FAIL TRUE)
    endif()

    foreach(_rvx_validation_target IN LISTS RVX_TEST_BUILD_TARGETS)
        if(RVX_TEST_LANE)
            _rvx_validation_register_target("${_rvx_validation_target}" "${RVX_TEST_LANE}")
        else()
            _rvx_validation_register_target("${_rvx_validation_target}" "")
            _rvx_validation_infer_legacy_lanes("${_rvx_validation_labels}" _rvx_validation_legacy_lanes)
            foreach(_rvx_validation_lane IN LISTS _rvx_validation_legacy_lanes)
                _rvx_validation_register_target("${_rvx_validation_target}" "${_rvx_validation_lane}")
            endforeach()
        endif()
    endforeach()
endfunction()

# Creates the six fixed lane aggregate build targets and their all-lanes
# counterpart.  Call once after all test targets and legacy direct add_test()
# registrations have been declared.
function(rvx_finalize_validation)
    set(options "")
    set(one_value_args "")
    set(multi_value_args EXTRA_TARGETS)
    cmake_parse_arguments(PARSE_ARGV 0 RVX_FINALIZE
        "${options}"
        "${one_value_args}"
        "${multi_value_args}"
    )

    if(RVX_FINALIZE_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "rvx_finalize_validation received unparsed arguments: ${RVX_FINALIZE_UNPARSED_ARGUMENTS}")
    endif()

    get_property(_rvx_validation_finalized GLOBAL PROPERTY RVX_VALIDATION_FINALIZED)
    if(_rvx_validation_finalized)
        message(FATAL_ERROR "rvx_finalize_validation may only be called once")
    endif()

    get_property(_rvx_validation_legacy_targets GLOBAL
        PROPERTY RVX_VALIDATION_EXECUTABLE_TARGETS)
    get_property(_rvx_validation_all_targets GLOBAL PROPERTY RVX_VALIDATION_ALL_TARGETS)
    list(APPEND _rvx_validation_all_targets
        ${_rvx_validation_legacy_targets}
        ${RVX_FINALIZE_EXTRA_TARGETS}
    )
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
    endforeach()

    if(TARGET RVXValidationAll)
        message(FATAL_ERROR "Validation aggregate 'RVXValidationAll' already exists")
    endif()
    add_custom_target(RVXValidationAll DEPENDS ${_rvx_validation_all_targets})

    # Retain the historical inventory name as a compatibility alias in C01.
    if(TARGET RVXValidationInventory)
        message(FATAL_ERROR "Validation aggregate 'RVXValidationInventory' already exists")
    endif()
    add_custom_target(RVXValidationInventory DEPENDS ${_rvx_validation_all_targets})

    set_property(GLOBAL PROPERTY RVX_VALIDATION_FINALIZED TRUE)
endfunction()
