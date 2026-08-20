cmake_minimum_required(VERSION 3.25)

# Generate the cooked-only fixture consumed by lane.shipping.  The caller must
# provide a host RVXCook executable; target-platform tools are never executed.
foreach(_required
        RVX_COOK_EXECUTABLE
        RVX_SOURCE_ROOT
        RVX_BINARY_FIXTURE_ROOT
        RVX_BACKEND
        RVX_COMMIT
        RVX_PRESET)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "${_required} is required")
    endif()
endforeach()

cmake_path(ABSOLUTE_PATH RVX_COOK_EXECUTABLE NORMALIZE
           OUTPUT_VARIABLE _rvx_cook)
cmake_path(ABSOLUTE_PATH RVX_SOURCE_ROOT NORMALIZE
           OUTPUT_VARIABLE _source_root)
cmake_path(ABSOLUTE_PATH RVX_BINARY_FIXTURE_ROOT NORMALIZE
           OUTPUT_VARIABLE _fixture_root)

if(NOT EXISTS "${_rvx_cook}" OR IS_DIRECTORY "${_rvx_cook}")
    message(FATAL_ERROR "RVX_COOK_EXECUTABLE is not a file: ${_rvx_cook}")
endif()
if(NOT IS_DIRECTORY "${_source_root}" OR
   NOT EXISTS "${_source_root}/CMakeLists.txt")
    message(FATAL_ERROR "RVX_SOURCE_ROOT is not a repository root: ${_source_root}")
endif()
if(NOT RVX_BACKEND MATCHES "^(DX11|DX12|Vulkan|Metal|OpenGL)$")
    message(FATAL_ERROR "RVX_BACKEND must be DX11, DX12, Vulkan, Metal, or OpenGL")
endif()
string(LENGTH "${RVX_COMMIT}" _commit_length)
if(NOT RVX_COMMIT MATCHES "^[0-9A-Fa-f]+$" OR
   _commit_length LESS 7 OR _commit_length GREATER 64)
    message(FATAL_ERROR "RVX_COMMIT must be a 7-64 digit hexadecimal commit id")
endif()
if(NOT RVX_PRESET MATCHES "^[A-Za-z0-9_.-]+$")
    message(FATAL_ERROR "RVX_PRESET must contain only letters, digits, '.', '_' or '-'")
endif()

# A semantic "binary fixture root" is not enough: find the configured build
# tree that owns it.  This prevents an incorrect -D value from making the
# cleanup/publish steps operate in an arbitrary directory.
get_filename_component(_fixture_parent "${_fixture_root}" DIRECTORY)
file(MAKE_DIRECTORY "${_fixture_parent}")
set(_build_probe "${_fixture_parent}")
set(_build_root "")
while(TRUE)
    if(EXISTS "${_build_probe}/CMakeCache.txt")
        set(_build_root "${_build_probe}")
        break()
    endif()
    get_filename_component(_next_probe "${_build_probe}" DIRECTORY)
    if(_next_probe STREQUAL _build_probe)
        break()
    endif()
    set(_build_probe "${_next_probe}")
endwhile()
if(_build_root STREQUAL "")
    message(FATAL_ERROR
        "RVX_BINARY_FIXTURE_ROOT must be below a configured CMake build tree")
endif()
if(_build_root STREQUAL _source_root)
    message(FATAL_ERROR
        "An in-source CMake build cannot own the Shipping fixture")
endif()

get_filename_component(_fixture_name "${_fixture_root}" NAME)
if(_fixture_name STREQUAL "" OR _fixture_name STREQUAL "." OR
   _fixture_name STREQUAL "..")
    message(FATAL_ERROR "RVX_BINARY_FIXTURE_ROOT has an unsafe final component")
endif()

string(SHA256 _publish_identity
       "${RVX_COMMIT}|${RVX_PRESET}|${RVX_BACKEND}")
string(SUBSTRING "${_publish_identity}" 0 16 _publish_token)
set(_staging_root
    "${_fixture_parent}/.${_fixture_name}.staging-${_publish_token}")
set(_backup_root
    "${_fixture_parent}/.${_fixture_name}.previous-${_publish_token}")

# These two paths are fixed siblings derived from a validated fixture root.
# Removing a stale staging/backup directory never touches the published fixture.
file(REMOVE_RECURSE "${_staging_root}" "${_backup_root}")
file(MAKE_DIRECTORY "${_staging_root}/Input/Model")
file(MAKE_DIRECTORY "${_staging_root}/Cooked/Resources")
file(MAKE_DIRECTORY "${_staging_root}/Cooked/Shaders")

set(_model_source
    "${_source_root}/Tests/Fixtures/ModelViewer/R7Triangle.gltf")
if(NOT EXISTS "${_model_source}")
    message(FATAL_ERROR "The hermetic Shipping model fixture is missing: ${_model_source}")
endif()
file(COPY_FILE "${_model_source}"
     "${_staging_root}/Input/Model/R7Triangle.gltf" ONLY_IF_DIFFERENT)

set(_shader_roots
    "${_source_root}/Source/Runtime/Render/Shaders")
set(_shader_root "")
foreach(_candidate IN LISTS _shader_roots)
    if(EXISTS "${_candidate}/Programs.rvxshadercatalog.json")
        if(NOT _shader_root STREQUAL "")
            message(FATAL_ERROR "Multiple shader catalog roots were found")
        endif()
        set(_shader_root "${_candidate}")
    endif()
endforeach()
if(_shader_root STREQUAL "")
    message(FATAL_ERROR "The repository shader catalog could not be located")
endif()
set(_shader_catalog "${_shader_root}/Programs.rvxshadercatalog.json")

file(READ "${_shader_catalog}" _catalog_json)
string(JSON _catalog_schema GET "${_catalog_json}" schema)
string(JSON _catalog_version GET "${_catalog_json}" version)
if(NOT _catalog_schema STREQUAL "RVX_SHADER_PROGRAM_CATALOG" OR
   NOT _catalog_version EQUAL 2)
    message(FATAL_ERROR "The shader catalog must use schema version 2")
endif()
string(JSON _catalog_program_count LENGTH "${_catalog_json}" programs)
if(_catalog_program_count LESS 1)
    message(FATAL_ERROR "The shader catalog contains no programs")
endif()
set(_selected_program_count 0)
math(EXPR _catalog_last "${_catalog_program_count} - 1")
foreach(_program_index RANGE 0 ${_catalog_last})
    string(JSON _backend_count LENGTH "${_catalog_json}"
           programs ${_program_index} backends)
    if(_backend_count LESS 1)
        message(FATAL_ERROR
            "Shader catalog program ${_program_index} has no backends")
    endif()
    math(EXPR _backend_last "${_backend_count} - 1")
    foreach(_backend_index RANGE 0 ${_backend_last})
        string(JSON _catalog_backend GET "${_catalog_json}"
               programs ${_program_index} backends ${_backend_index})
        if(_catalog_backend STREQUAL RVX_BACKEND)
            math(EXPR _selected_program_count "${_selected_program_count} + 1")
            break()
        endif()
    endforeach()
endforeach()
if(_selected_program_count LESS 1)
    message(FATAL_ERROR "The shader catalog selects no programs for ${RVX_BACKEND}")
endif()

set(_resource_manifest
    "${_staging_root}/Cooked/Resources/CookManifest.rvxmanifest")
execute_process(
    COMMAND "${_rvx_cook}"
        --source "${_staging_root}/Input/Model"
        --output "${_staging_root}/Cooked/Resources"
        --manifest "${_resource_manifest}"
        --model-only
        --fail-on-errors
    WORKING_DIRECTORY "${_source_root}"
    RESULT_VARIABLE _resource_result
    OUTPUT_VARIABLE _resource_stdout
    ERROR_VARIABLE _resource_stderr)
if(NOT _resource_result EQUAL 0)
    message(FATAL_ERROR
        "RVXCook resource fixture failed (${_resource_result})\n"
        "stdout:\n${_resource_stdout}\nstderr:\n${_resource_stderr}")
endif()

execute_process(
    COMMAND "${_rvx_cook}"
        --source "${_shader_root}"
        --output "${_staging_root}/Cooked/Shaders"
        --shader-catalog "${_shader_catalog}"
        --shader-backend "${RVX_BACKEND}"
        --fail-on-errors
    WORKING_DIRECTORY "${_source_root}"
    RESULT_VARIABLE _shader_result
    OUTPUT_VARIABLE _shader_stdout
    ERROR_VARIABLE _shader_stderr)
if(NOT _shader_result EQUAL 0)
    message(FATAL_ERROR
        "RVXCook shader catalog failed (${_shader_result})\n"
        "stdout:\n${_shader_stdout}\nstderr:\n${_shader_stderr}")
endif()

if(NOT EXISTS "${_resource_manifest}")
    message(FATAL_ERROR "RVXCook did not produce the resource manifest")
endif()
file(READ "${_resource_manifest}" _manifest_text)
foreach(_required_line
        "RVX_COOK_MANIFEST_V2"
        "schema=RVX_COOK_MANIFEST"
        "version=2"
        "failureCount=0"
        "RVX_COOK_MANIFEST_END")
    string(FIND "${_manifest_text}" "${_required_line}" _manifest_match)
    if(_manifest_match EQUAL -1)
        message(FATAL_ERROR
            "Cook manifest is missing required content: ${_required_line}")
    endif()
endforeach()
string(REGEX MATCH "(^|\n)entryCount=([0-9]+)(\n|$)"
       _entry_match "${_manifest_text}")
set(_resource_entry_count "${CMAKE_MATCH_2}")
string(REGEX MATCH "(^|\n)successCount=([0-9]+)(\n|$)"
       _success_match "${_manifest_text}")
set(_resource_success_count "${CMAKE_MATCH_2}")
if(_resource_entry_count STREQUAL "" OR
   _resource_success_count STREQUAL "" OR
   _resource_entry_count LESS 1 OR
   NOT _resource_entry_count EQUAL _resource_success_count)
    message(FATAL_ERROR "Cook manifest does not describe an all-success resource cook")
endif()
string(REGEX MATCH "(^|\n)entry\.[0-9]+\.type=Model(\n|$)"
       _model_entry "${_manifest_text}")
if(_model_entry STREQUAL "")
    message(FATAL_ERROR "Cook manifest has no successful model entry")
endif()

file(GLOB_RECURSE _shader_outputs LIST_DIRECTORIES false
     "${_staging_root}/Cooked/Shaders/*")
list(LENGTH _shader_outputs _shader_output_count)
if(NOT _shader_output_count EQUAL _selected_program_count)
    message(FATAL_ERROR
        "Shader output count ${_shader_output_count} does not match selected catalog count ${_selected_program_count}")
endif()
foreach(_shader_output IN LISTS _shader_outputs)
    if(NOT _shader_output MATCHES "\\.rvxshader$")
        message(FATAL_ERROR "Unexpected shader output: ${_shader_output}")
    endif()
    file(SIZE "${_shader_output}" _shader_size)
    if(_shader_size LESS 1)
        message(FATAL_ERROR "Empty shader artifact: ${_shader_output}")
    endif()
endforeach()

file(GLOB_RECURSE _resource_outputs LIST_DIRECTORIES false
     "${_staging_root}/Cooked/Resources/*")
list(LENGTH _resource_outputs _resource_output_count)
if(_resource_output_count LESS 2)
    message(FATAL_ERROR "The resource cook did not produce a model artifact")
endif()
set(_expected_model_artifact
    "${_staging_root}/Cooked/Resources/R7Triangle.rva")
if(NOT EXISTS "${_expected_model_artifact}")
    message(FATAL_ERROR
        "The resource cook did not produce Cooked/Resources/R7Triangle.rva")
endif()
foreach(_resource_output IN LISTS _resource_outputs)
    file(SIZE "${_resource_output}" _resource_size)
    if(_resource_size LESS 1)
        message(FATAL_ERROR "Empty resource artifact: ${_resource_output}")
    endif()
endforeach()

# Input source is intentionally absent from the published cooked-only fixture.
file(REMOVE_RECURSE "${_staging_root}/Input")

function(_rvx_json_escape _input _output)
    set(_value "${_input}")
    string(REPLACE "\\" "\\\\" _value "${_value}")
    string(REPLACE "\"" "\\\"" _value "${_value}")
    string(REPLACE "\r" "\\r" _value "${_value}")
    string(REPLACE "\n" "\\n" _value "${_value}")
    string(REPLACE "\t" "\\t" _value "${_value}")
    set(${_output} "${_value}" PARENT_SCOPE)
endfunction()

file(GLOB_RECURSE _artifact_paths LIST_DIRECTORIES false
     "${_staging_root}/Cooked/*")
list(SORT _artifact_paths)
list(LENGTH _artifact_paths _artifact_count)
if(_artifact_count LESS 3)
    message(FATAL_ERROR "The Shipping fixture has an incomplete artifact set")
endif()

set(_artifact_payload "")
set(_artifact_json "")
set(_artifact_index 0)
foreach(_artifact IN LISTS _artifact_paths)
    file(RELATIVE_PATH _relative "${_staging_root}" "${_artifact}")
    file(TO_CMAKE_PATH "${_relative}" _relative)
    if(NOT _relative MATCHES "^Cooked/[A-Za-z0-9._/-]+$")
        message(FATAL_ERROR "Unsafe artifact path in fixture: ${_relative}")
    endif()
    file(SIZE "${_artifact}" _size)
    file(SHA256 "${_artifact}" _sha256)
    string(APPEND _artifact_payload "${_relative}|${_size}|${_sha256}\n")
    if(_artifact_index GREATER 0)
        string(APPEND _artifact_json ",\n")
    endif()
    string(APPEND _artifact_json
        "    {\"path\":\"${_relative}\",\"size\":${_size},\"sha256\":\"${_sha256}\"}")
    math(EXPR _artifact_index "${_artifact_index} + 1")
endforeach()
string(SHA256 _artifact_payload_hash "${_artifact_payload}")
file(SHA256 "${_model_source}" _model_source_hash)
file(SHA256 "${_shader_catalog}" _shader_catalog_hash)
file(SHA256 "${_rvx_cook}" _cook_executable_hash)

_rvx_json_escape("${RVX_COMMIT}" _commit_json)
_rvx_json_escape("${RVX_PRESET}" _preset_json)
set(_receipt "{\n")
string(APPEND _receipt "  \"schema\":\"RVX_SHIPPING_FIXTURE_RECEIPT\",\n")
string(APPEND _receipt "  \"schemaVersion\":1,\n")
string(APPEND _receipt "  \"commit\":\"${_commit_json}\",\n")
string(APPEND _receipt "  \"preset\":\"${_preset_json}\",\n")
string(APPEND _receipt "  \"backend\":\"${RVX_BACKEND}\",\n")
string(APPEND _receipt "  \"sourceModel\":\"Tests/Fixtures/ModelViewer/R7Triangle.gltf\",\n")
string(APPEND _receipt "  \"sourceModelSha256\":\"${_model_source_hash}\",\n")
string(APPEND _receipt "  \"shaderCatalog\":\"Source/Runtime/Render/Shaders/Programs.rvxshadercatalog.json\",\n")
string(APPEND _receipt "  \"shaderCatalogSha256\":\"${_shader_catalog_hash}\",\n")
string(APPEND _receipt "  \"cookExecutableSha256\":\"${_cook_executable_hash}\",\n")
string(APPEND _receipt "  \"resourceManifest\":\"Cooked/Resources/CookManifest.rvxmanifest\",\n")
string(APPEND _receipt "  \"resourceEntryCount\":${_resource_entry_count},\n")
string(APPEND _receipt "  \"shaderProgramCount\":${_selected_program_count},\n")
string(APPEND _receipt "  \"artifactCount\":${_artifact_count},\n")
string(APPEND _receipt "  \"artifactPayloadHash\":\"${_artifact_payload_hash}\",\n")
string(APPEND _receipt "  \"artifacts\":[\n${_artifact_json}\n  ]\n}\n")
set(_receipt_path "${_staging_root}/shipping-fixture-receipt.json")
file(WRITE "${_receipt_path}.tmp" "${_receipt}")
file(RENAME "${_receipt_path}.tmp" "${_receipt_path}" RESULT _receipt_result)
if(NOT _receipt_result STREQUAL "0")
    message(FATAL_ERROR "Cannot publish fixture receipt: ${_receipt_result}")
endif()

set(_had_previous FALSE)
if(EXISTS "${_fixture_root}")
    file(RENAME "${_fixture_root}" "${_backup_root}" RESULT _backup_result)
    if(NOT _backup_result STREQUAL "0")
        message(FATAL_ERROR "Cannot preserve previous Shipping fixture: ${_backup_result}")
    endif()
    set(_had_previous TRUE)
endif()
file(RENAME "${_staging_root}" "${_fixture_root}" RESULT _publish_result)
if(NOT _publish_result STREQUAL "0")
    if(_had_previous)
        file(RENAME "${_backup_root}" "${_fixture_root}" RESULT _restore_result)
    endif()
    message(FATAL_ERROR "Cannot atomically publish Shipping fixture: ${_publish_result}")
endif()
if(_had_previous)
    file(REMOVE_RECURSE "${_backup_root}")
endif()

message(STATUS
    "Shipping fixture published: ${_fixture_root} (${_resource_entry_count} resource entries, ${_selected_program_count} shader programs)")
