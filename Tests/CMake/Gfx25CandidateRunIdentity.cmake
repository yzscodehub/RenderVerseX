# A run directory is immutable: neither producers nor validators may select
# an earlier run implicitly. Restrict the identifier to one portable component.
foreach(_rvx_identity IN ITEMS RVX_VALIDATION_SOURCE_COMMIT RVX_VALIDATION_RUN_ID RVX_VALIDATION_PRESET)
    if("$ENV{${_rvx_identity}}" STREQUAL "")
        message(FATAL_ERROR "GFX25 candidate requires ${_rvx_identity}")
    endif()
endforeach()
set(_rvx_run_id "$ENV{RVX_VALIDATION_RUN_ID}")
string(LENGTH "${_rvx_run_id}" _rvx_run_id_length)
if(NOT _rvx_run_id MATCHES "^[A-Za-z0-9][A-Za-z0-9_-]*$" OR _rvx_run_id_length GREATER 128)
    message(FATAL_ERROR "GFX25 candidate run id must be a portable path component")
endif()
