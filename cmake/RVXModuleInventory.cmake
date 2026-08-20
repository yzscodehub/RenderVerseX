include_guard(GLOBAL)

function(_rvx_module_append_unique_global property_name value)
    get_property(values GLOBAL PROPERTY "${property_name}")
    list(FIND values "${value}" value_index)
    if(value_index EQUAL -1)
        set_property(GLOBAL APPEND PROPERTY "${property_name}" "${value}")
    endif()
endfunction()

function(_rvx_module_json_escape output value)
    set(escaped "${value}")
    # Keep target properties as evidence without asking file(GENERATE) to
    # evaluate context-sensitive expressions such as LINK_ONLY.
    string(REPLACE "$<" "__RVX_GENEX_OPEN__" escaped "${escaped}")
    string(REPLACE "\\" "\\\\" escaped "${escaped}")
    string(REPLACE "\"" "\\\"" escaped "${escaped}")
    string(REPLACE "\r" "\\r" escaped "${escaped}")
    string(REPLACE "\n" "\\n" escaped "${escaped}")
    string(REPLACE "\t" "\\t" escaped "${escaped}")
    set("${output}" "${escaped}" PARENT_SCOPE)
endfunction()

function(_rvx_module_json_array output)
    set(result "[")
    set(separator "")
    foreach(value IN LISTS ARGN)
        _rvx_module_json_escape(escaped "${value}")
        string(APPEND result "${separator}\"${escaped}\"")
        set(separator ",")
    endforeach()
    string(APPEND result "]")
    set("${output}" "${result}" PARENT_SCOPE)
endfunction()

function(_rvx_module_collect_directory_targets directory output_targets)
    get_property(directory_targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    get_property(subdirectories DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)

    set(all_targets ${directory_targets})
    foreach(subdirectory IN LISTS subdirectories)
        _rvx_module_collect_directory_targets(
            "${subdirectory}"
            child_targets
        )
        list(APPEND all_targets ${child_targets})
    endforeach()

    list(REMOVE_DUPLICATES all_targets)
    set("${output_targets}" "${all_targets}" PARENT_SCOPE)
endfunction()

function(_rvx_module_path_is_within root candidate output)
    file(RELATIVE_PATH relative "${root}" "${candidate}")
    if(relative STREQUAL "" OR relative STREQUAL ".")
        set(result TRUE)
    elseif(relative MATCHES "^\\.\\.([/\\\\]|$)" OR IS_ABSOLUTE "${relative}")
        set(result FALSE)
    else()
        set(result TRUE)
    endif()
    set("${output}" "${result}" PARENT_SCOPE)
endfunction()

function(_rvx_module_is_first_party_source candidate output)
    _rvx_module_path_is_within(
        "${CMAKE_SOURCE_DIR}" "${candidate}" is_in_source_tree)
    _rvx_module_path_is_within(
        "${CMAKE_BINARY_DIR}" "${candidate}" is_in_binary_tree)
    if(is_in_source_tree AND NOT is_in_binary_tree)
        set(result TRUE)
    else()
        set(result FALSE)
    endif()
    set("${output}" "${result}" PARENT_SCOPE)
endfunction()

function(rvx_register_module_target)
    get_property(finalized GLOBAL PROPERTY RVX_MODULE_INVENTORY_FINALIZED)
    if(finalized)
        message(FATAL_ERROR
            "Module targets cannot be registered after inventory finalization")
    endif()

    set(options INFRASTRUCTURE)
    set(one_value_args TARGET MODULE)
    set(multi_value_args ALIASES)
    cmake_parse_arguments(PARSE_ARGV 0 RVX_MODULE
        "${options}"
        "${one_value_args}"
        "${multi_value_args}"
    )

    if(RVX_MODULE_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "rvx_register_module_target received unparsed arguments: "
            "${RVX_MODULE_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT RVX_MODULE_TARGET OR NOT RVX_MODULE_MODULE)
        message(FATAL_ERROR
            "rvx_register_module_target requires TARGET and MODULE")
    endif()
    if(NOT TARGET "${RVX_MODULE_TARGET}")
        message(FATAL_ERROR
            "Cannot register missing target '${RVX_MODULE_TARGET}'")
    endif()
    get_target_property(aliased_target "${RVX_MODULE_TARGET}" ALIASED_TARGET)
    if(aliased_target)
        message(FATAL_ERROR
            "Canonical module target '${RVX_MODULE_TARGET}' is an alias of "
            "'${aliased_target}'")
    endif()

    get_property(registered_targets GLOBAL PROPERTY RVX_MODULE_REGISTERED_TARGETS)
    list(FIND registered_targets "${RVX_MODULE_TARGET}" registered_index)
    if(NOT registered_index EQUAL -1)
        get_target_property(existing_module "${RVX_MODULE_TARGET}" RVX_MODULE_ID)
        message(FATAL_ERROR
            "Target '${RVX_MODULE_TARGET}' is already registered to module "
            "'${existing_module}'")
    endif()

    get_property(registered_aliases GLOBAL PROPERTY RVX_MODULE_REGISTERED_ALIASES)
    foreach(alias IN LISTS RVX_MODULE_ALIASES)
        if(NOT TARGET "${alias}")
            message(FATAL_ERROR
                "Alias '${alias}' for '${RVX_MODULE_TARGET}' does not exist")
        endif()
        get_target_property(alias_target "${alias}" ALIASED_TARGET)
        if(NOT "${alias_target}" STREQUAL "${RVX_MODULE_TARGET}")
            message(FATAL_ERROR
                "Alias '${alias}' points to '${alias_target}', not "
                "'${RVX_MODULE_TARGET}'")
        endif()
        list(FIND registered_aliases "${alias}" alias_index)
        if(NOT alias_index EQUAL -1)
            message(FATAL_ERROR "Alias '${alias}' is registered more than once")
        endif()
        list(APPEND registered_aliases "${alias}")
    endforeach()

    if(RVX_MODULE_INFRASTRUCTURE)
        get_target_property(infrastructure_type
            "${RVX_MODULE_TARGET}" TYPE)
        if(NOT infrastructure_type STREQUAL "INTERFACE_LIBRARY" AND
           NOT infrastructure_type STREQUAL "UTILITY")
            message(FATAL_ERROR
                "Infrastructure target '${RVX_MODULE_TARGET}' must be an "
                "INTERFACE_LIBRARY or UTILITY target, not '${infrastructure_type}'")
        endif()
    endif()

    set_target_properties("${RVX_MODULE_TARGET}" PROPERTIES
        RVX_MODULE_ID "${RVX_MODULE_MODULE}"
        RVX_MODULE_ALIASES "${RVX_MODULE_ALIASES}"
        RVX_MODULE_INFRASTRUCTURE "${RVX_MODULE_INFRASTRUCTURE}"
    )
    set_property(GLOBAL APPEND PROPERTY
        RVX_MODULE_REGISTERED_TARGETS "${RVX_MODULE_TARGET}")
    set_property(GLOBAL PROPERTY RVX_MODULE_REGISTERED_ALIASES
        "${registered_aliases}")
endfunction()

function(rvx_register_disabled_module)
    get_property(finalized GLOBAL PROPERTY RVX_MODULE_INVENTORY_FINALIZED)
    if(finalized)
        message(FATAL_ERROR
            "Disabled modules cannot be registered after inventory finalization")
    endif()

    set(options "")
    set(one_value_args MODULE REASON)
    set(multi_value_args "")
    cmake_parse_arguments(PARSE_ARGV 0 RVX_DISABLED
        "${options}"
        "${one_value_args}"
        "${multi_value_args}"
    )
    if(RVX_DISABLED_UNPARSED_ARGUMENTS OR
       NOT RVX_DISABLED_MODULE OR
       NOT RVX_DISABLED_REASON)
        message(FATAL_ERROR
            "rvx_register_disabled_module requires MODULE and REASON")
    endif()
    if(RVX_DISABLED_MODULE MATCHES "[|;]" OR
       RVX_DISABLED_REASON MATCHES "[|;]")
        message(FATAL_ERROR
            "Disabled module identifiers and reasons may not contain '|' or ';'")
    endif()
    get_property(disabled_modules GLOBAL PROPERTY RVX_MODULE_DISABLED_MODULES)
    foreach(record IN LISTS disabled_modules)
        string(REPLACE "|" ";" fields "${record}")
        list(GET fields 0 module_id)
        if(module_id STREQUAL RVX_DISABLED_MODULE)
            message(FATAL_ERROR
                "Module '${RVX_DISABLED_MODULE}' is disabled more than once")
        endif()
    endforeach()
    set_property(GLOBAL APPEND PROPERTY RVX_MODULE_DISABLED_MODULES
        "${RVX_DISABLED_MODULE}|${RVX_DISABLED_REASON}")
endfunction()

function(_rvx_module_read_manifest manifest_path output_json output_ids output_paths output_deferred)
    if(NOT EXISTS "${manifest_path}")
        message(FATAL_ERROR "Module manifest does not exist: ${manifest_path}")
    endif()
    file(READ "${manifest_path}" manifest_json)
    string(JSON schema_version ERROR_VARIABLE schema_error
        GET "${manifest_json}" schemaVersion)
    if(schema_error OR NOT schema_version EQUAL 2)
        message(FATAL_ERROR
            "Module manifest must declare schemaVersion 2: ${schema_error}")
    endif()
    string(JSON module_count ERROR_VARIABLE modules_error
        LENGTH "${manifest_json}" modules)
    if(modules_error OR module_count EQUAL 0)
        message(FATAL_ERROR
            "Module manifest must contain a non-empty modules array: "
            "${modules_error}")
    endif()

    set(module_ids "")
    set(module_paths "")
    set(module_deferred "")
    math(EXPR last_module "${module_count} - 1")
    foreach(index RANGE 0 ${last_module})
        string(JSON module_id GET "${manifest_json}" modules ${index} id)
        string(JSON module_path GET "${manifest_json}" modules ${index} path)
        string(JSON is_deferred GET "${manifest_json}" modules ${index} deferred)
        list(APPEND module_ids "${module_id}")
        list(APPEND module_paths "${module_path}")
        list(APPEND module_deferred "${is_deferred}")
    endforeach()

    set("${output_json}" "${manifest_json}" PARENT_SCOPE)
    set("${output_ids}" "${module_ids}" PARENT_SCOPE)
    set("${output_paths}" "${module_paths}" PARENT_SCOPE)
    set("${output_deferred}" "${module_deferred}" PARENT_SCOPE)
endfunction()

function(rvx_finalize_module_inventory)
    set(options "")
    set(one_value_args MANIFEST OUTPUT)
    set(multi_value_args "")
    cmake_parse_arguments(PARSE_ARGV 0 RVX_INVENTORY
        "${options}"
        "${one_value_args}"
        "${multi_value_args}"
    )
    if(RVX_INVENTORY_UNPARSED_ARGUMENTS OR
       NOT RVX_INVENTORY_MANIFEST OR
       NOT RVX_INVENTORY_OUTPUT)
        message(FATAL_ERROR
            "rvx_finalize_module_inventory requires MANIFEST and OUTPUT")
    endif()
    _rvx_module_path_is_within(
        "${CMAKE_BINARY_DIR}" "${CMAKE_SOURCE_DIR}" source_is_in_binary_tree)
    if(source_is_in_binary_tree)
        message(FATAL_ERROR
            "Module inventory requires an out-of-source build whose binary "
            "directory does not contain the source tree")
    endif()
    get_property(finalized GLOBAL PROPERTY RVX_MODULE_INVENTORY_FINALIZED)
    if(finalized)
        message(FATAL_ERROR "rvx_finalize_module_inventory may only be called once")
    endif()

    _rvx_module_read_manifest(
        "${RVX_INVENTORY_MANIFEST}"
        manifest_json
        module_ids
        module_paths
        module_deferred
    )
    get_property(registered_targets GLOBAL PROPERTY RVX_MODULE_REGISTERED_TARGETS)
    get_property(registered_aliases GLOBAL PROPERTY RVX_MODULE_REGISTERED_ALIASES)
    get_property(disabled_records GLOBAL PROPERTY RVX_MODULE_DISABLED_MODULES)
    list(REMOVE_DUPLICATES registered_targets)
    list(SORT registered_targets)

    _rvx_module_collect_directory_targets(
        "${CMAKE_SOURCE_DIR}"
        discovered_targets
    )
    foreach(target IN LISTS discovered_targets)
        if(NOT TARGET "${target}")
            continue()
        endif()
        get_target_property(is_imported "${target}" IMPORTED)
        if(is_imported)
            continue()
        endif()
        get_target_property(source_directory "${target}" SOURCE_DIR)
        _rvx_module_is_first_party_source(
            "${source_directory}" is_first_party)
        if(NOT is_first_party)
            continue()
        endif()
        list(FIND registered_targets "${target}" target_index)
        if(target_index EQUAL -1)
            message(FATAL_ERROR
                "First-party CMake target '${target}' is not registered with "
                "rvx_register_module_target()")
        endif()
    endforeach()

    set(target_entries "")
    set(target_separator "")
    set(active_modules "")
    foreach(target IN LISTS registered_targets)
        if(NOT TARGET "${target}")
            message(FATAL_ERROR "Registered target '${target}' no longer exists")
        endif()
        get_target_property(module_id "${target}" RVX_MODULE_ID)
        list(FIND module_ids "${module_id}" module_index)
        if(module_index EQUAL -1)
            message(FATAL_ERROR
                "Target '${target}' registers unknown module '${module_id}'")
        endif()
        get_target_property(is_infrastructure "${target}" RVX_MODULE_INFRASTRUCTURE)
        if(NOT is_infrastructure)
            list(APPEND active_modules "${module_id}")
            list(GET module_paths ${module_index} module_path)
            get_target_property(source_directory "${target}" SOURCE_DIR)
            get_filename_component(module_root
                "${CMAKE_SOURCE_DIR}/${module_path}" ABSOLUTE)
            _rvx_module_path_is_within(
                "${module_root}" "${source_directory}" target_in_module)
            if(NOT target_in_module)
                message(FATAL_ERROR
                    "Target '${target}' is registered to '${module_id}' but is "
                    "declared at '${source_directory}', outside '${module_root}'")
            endif()
        endif()

        get_target_property(target_aliases "${target}" RVX_MODULE_ALIASES)
        get_target_property(target_type "${target}" TYPE)
        get_target_property(source_directory "${target}" SOURCE_DIR)
        get_target_property(binary_directory "${target}" BINARY_DIR)
        get_target_property(sources "${target}" SOURCES)
        get_target_property(link_libraries "${target}" LINK_LIBRARIES)
        get_target_property(interface_link_libraries
            "${target}" INTERFACE_LINK_LIBRARIES)
        get_target_property(include_directories "${target}" INCLUDE_DIRECTORIES)
        get_target_property(interface_include_directories
            "${target}" INTERFACE_INCLUDE_DIRECTORIES)
        foreach(property_name
            target_aliases
            sources
            link_libraries
            interface_link_libraries
            include_directories
            interface_include_directories
        )
            if("${${property_name}}" STREQUAL "${property_name}-NOTFOUND")
                set("${property_name}" "")
            endif()
        endforeach()

        _rvx_module_json_array(aliases_json ${target_aliases})
        _rvx_module_json_array(sources_json ${sources})
        _rvx_module_json_array(links_json ${link_libraries})
        _rvx_module_json_array(interface_links_json ${interface_link_libraries})
        _rvx_module_json_array(includes_json ${include_directories})
        _rvx_module_json_array(interface_includes_json
            ${interface_include_directories})
        foreach(value_name
            target
            module_id
            target_type
            source_directory
            binary_directory
        )
            _rvx_module_json_escape("${value_name}_json" "${${value_name}}")
        endforeach()
        if(is_infrastructure)
            set(infrastructure_json true)
        else()
            set(infrastructure_json false)
        endif()

        string(APPEND target_entries
            "${target_separator}{"
            "\"name\":\"${target_json}\","
            "\"aliases\":${aliases_json},"
            "\"module\":\"${module_id_json}\","
            "\"type\":\"${target_type_json}\","
            "\"sourceDirectory\":\"${source_directory_json}\","
            "\"binaryDirectory\":\"${binary_directory_json}\","
            "\"infrastructure\":${infrastructure_json},"
            "\"sources\":${sources_json},"
            "\"linkLibraries\":${links_json},"
            "\"interfaceLinkLibraries\":${interface_links_json},"
            "\"includeDirectories\":${includes_json},"
            "\"interfaceIncludeDirectories\":${interface_includes_json}"
            "}")
        set(target_separator ",")
    endforeach()

    list(REMOVE_DUPLICATES active_modules)
    set(disabled_entries "")
    set(disabled_separator "")
    set(disabled_modules "")
    foreach(record IN LISTS disabled_records)
        string(REPLACE "|" ";" fields "${record}")
        list(GET fields 0 disabled_module)
        list(GET fields 1 disabled_reason)
        list(FIND module_ids "${disabled_module}" disabled_module_index)
        if(disabled_module_index EQUAL -1)
            message(FATAL_ERROR
                "Disabled module '${disabled_module}' is not in the manifest")
        endif()
        list(FIND active_modules "${disabled_module}" active_module_index)
        if(NOT active_module_index EQUAL -1)
            message(FATAL_ERROR
                "Module '${disabled_module}' is both active and disabled")
        endif()
        list(APPEND disabled_modules "${disabled_module}")
        _rvx_module_json_escape(disabled_module_json "${disabled_module}")
        _rvx_module_json_escape(disabled_reason_json "${disabled_reason}")
        string(APPEND disabled_entries
            "${disabled_separator}{"
            "\"module\":\"${disabled_module_json}\","
            "\"reason\":\"${disabled_reason_json}\"}")
        set(disabled_separator ",")
    endforeach()

    list(LENGTH module_ids module_count)
    if(module_count GREATER 0)
        math(EXPR last_module "${module_count} - 1")
        foreach(index RANGE 0 ${last_module})
            list(GET module_ids ${index} module_id)
            list(GET module_deferred ${index} is_deferred)
            list(FIND active_modules "${module_id}" active_index)
            list(FIND disabled_modules "${module_id}" disabled_index)
            if(is_deferred)
                if(NOT active_index EQUAL -1)
                    message(FATAL_ERROR
                        "Deferred module '${module_id}' unexpectedly has active targets")
                endif()
                if(disabled_index EQUAL -1)
                    message(FATAL_ERROR
                        "Deferred module '${module_id}' requires an explicit disabled reason")
                endif()
            elseif(active_index EQUAL -1 AND disabled_index EQUAL -1)
                message(FATAL_ERROR
                    "Non-deferred module '${module_id}' has neither a registered "
                    "target nor an explicit disabled reason")
            endif()
        endforeach()
    endif()

    # Reject dependency expressions whose result cannot be audited from the
    # emitted graph.  Conditional composition should use ordinary CMake if()
    # branches so each configured inventory contains concrete policy inputs.
    set(known_internal_names ${registered_targets} ${registered_aliases})
    foreach(target IN LISTS registered_targets)
        get_target_property(link_libraries "${target}" LINK_LIBRARIES)
        get_target_property(interface_link_libraries
            "${target}" INTERFACE_LINK_LIBRARIES)
        get_target_property(include_directories "${target}" INCLUDE_DIRECTORIES)
        get_target_property(interface_include_directories
            "${target}" INTERFACE_INCLUDE_DIRECTORIES)
        foreach(property_name
            link_libraries
            interface_link_libraries
            include_directories
            interface_include_directories
        )
            if("${${property_name}}" STREQUAL "${property_name}-NOTFOUND")
                set("${property_name}" "")
            endif()
        endforeach()
        set(all_policy_values
            ${link_libraries}
            ${interface_link_libraries}
            ${include_directories}
            ${interface_include_directories}
        )
        foreach(value IN LISTS all_policy_values)
            if(value MATCHES
               "\\$<(TARGET_PROPERTY|GENEX_EVAL|TARGET_GENEX_EVAL):")
                message(FATAL_ERROR
                    "Target '${target}' uses a dynamic target-property generator "
                    "expression that cannot be audited: ${value}")
            endif()
        endforeach()
        foreach(include_directory IN LISTS
            include_directories
            interface_include_directories
        )
            if(include_directory MATCHES "\\$<" AND
               NOT include_directory MATCHES
                   "^\\$<(BUILD_INTERFACE|INSTALL_INTERFACE):[^$<>]*>$")
                message(FATAL_ERROR
                    "Target '${target}' uses an unsupported include-directory "
                    "generator expression that cannot be audited: "
                    "${include_directory}")
            endif()
        endforeach()

        # Reject unresolved first-party link names at configure time. The
        # policy checker performs module-edge validation from the emitted graph.
        set(all_links ${link_libraries} ${interface_link_libraries})
        foreach(link IN LISTS all_links)
            string(REGEX MATCHALL
                "RVX(::|_)?[A-Za-z0-9_]+|RenderVerse[A-Za-z0-9_]+|Particle[A-Za-z0-9_]*"
                internal_names
                "${link}")
            foreach(internal_name IN LISTS internal_names)
                list(FIND known_internal_names "${internal_name}" known_index)
                if(known_index EQUAL -1)
                    message(FATAL_ERROR
                        "Target '${target}' links unknown first-party target "
                        "'${internal_name}'")
                endif()
            endforeach()
        endforeach()
    endforeach()

    _rvx_module_json_escape(source_root_json "${CMAKE_SOURCE_DIR}")
    _rvx_module_json_escape(binary_root_json "${CMAKE_BINARY_DIR}")
    string(CONCAT inventory_json
        "{"
        "\"schema\":\"RVX.ModuleTargetInventory\","
        "\"schemaVersion\":1,"
        "\"configuration\":\"$<CONFIG>\","
        "\"sourceRoot\":\"${source_root_json}\","
        "\"binaryRoot\":\"${binary_root_json}\","
        "\"targets\":[${target_entries}],"
        "\"disabledModules\":[${disabled_entries}]"
        "}")

    get_filename_component(output_directory "${RVX_INVENTORY_OUTPUT}" DIRECTORY)
    file(MAKE_DIRECTORY "${output_directory}")
    file(GENERATE OUTPUT "${RVX_INVENTORY_OUTPUT}" CONTENT "${inventory_json}")
    set_property(GLOBAL PROPERTY RVX_MODULE_INVENTORY_FINALIZED TRUE)
endfunction()
