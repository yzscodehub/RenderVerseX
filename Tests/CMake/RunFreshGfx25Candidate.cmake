cmake_minimum_required(VERSION 3.25)

foreach(_rvx_required IN ITEMS
        RVX_CANDIDATE_ROOT
        RVX_CANDIDATE_ALLOWED_ROOT
        RVX_CANDIDATE_EXECUTABLE
        RVX_CANDIDATE_WORKING_DIRECTORY
        RVX_CANDIDATE_SAMPLE_ID
        RVX_CANDIDATE_BACKEND
        RVX_CANDIDATE_COOKED_SHADER_ROOT
        RVX_CANDIDATE_PIPELINE
        RVX_CANDIDATE_RENDER_PATH)
    if(NOT DEFINED ${_rvx_required} OR "${${_rvx_required}}" STREQUAL "")
        message(FATAL_ERROR
            "Fresh GFX25 candidate runner requires ${_rvx_required}")
    endif()
endforeach()

foreach(_rvx_absolute_path IN ITEMS
        RVX_CANDIDATE_ROOT
        RVX_CANDIDATE_ALLOWED_ROOT
        RVX_CANDIDATE_EXECUTABLE
        RVX_CANDIDATE_WORKING_DIRECTORY
        RVX_CANDIDATE_COOKED_SHADER_ROOT)
    if(NOT IS_ABSOLUTE "${${_rvx_absolute_path}}")
        message(FATAL_ERROR
            "Fresh GFX25 candidate runner requires an absolute ${_rvx_absolute_path}")
    endif()
endforeach()

cmake_path(NORMAL_PATH RVX_CANDIDATE_ALLOWED_ROOT
           OUTPUT_VARIABLE _rvx_allowed_root)
cmake_path(NORMAL_PATH RVX_CANDIDATE_ROOT
           OUTPUT_VARIABLE _rvx_candidate_root)
set(_rvx_allowed_root_for_check "${_rvx_allowed_root}")
set(_rvx_candidate_root_for_check "${_rvx_candidate_root}")
if(WIN32)
    string(TOLOWER "${_rvx_allowed_root_for_check}"
           _rvx_allowed_root_for_check)
    string(TOLOWER "${_rvx_candidate_root_for_check}"
           _rvx_candidate_root_for_check)
endif()
cmake_path(IS_PREFIX _rvx_allowed_root_for_check
           "${_rvx_candidate_root_for_check}" NORMALIZE
           _rvx_candidate_is_contained)
if(NOT _rvx_candidate_is_contained OR
   "${_rvx_candidate_root_for_check}" STREQUAL
       "${_rvx_allowed_root_for_check}")
    message(FATAL_ERROR
        "GFX25 candidate root must be a strict child of its allowed build root")
endif()
if(IS_SYMLINK "${_rvx_candidate_root}")
    message(FATAL_ERROR
        "GFX25 candidate root must not be a symbolic link")
endif()

file(MAKE_DIRECTORY "${_rvx_allowed_root}")
file(REMOVE_RECURSE "${_rvx_candidate_root}")
file(MAKE_DIRECTORY "${_rvx_candidate_root}")

execute_process(
    COMMAND "${RVX_CANDIDATE_EXECUTABLE}"
        --sample "${RVX_CANDIDATE_SAMPLE_ID}"
        --backend "${RVX_CANDIDATE_BACKEND}"
        --cooked-shader-root "${RVX_CANDIDATE_COOKED_SHADER_ROOT}"
        --pipeline "${RVX_CANDIDATE_PIPELINE}"
        --render-path "${RVX_CANDIDATE_RENDER_PATH}"
        --quality high
        --smoke
        --wait-ready
        --ready-timeout-ms 120000
        --ready-max-frames 2400
        --view hero
        --fixed-delta 0.0166666666666667
        --width 1280
        --height 720
        --qualification-series "${_rvx_candidate_root}/series.json"
        --diagnostics
        --validation
    WORKING_DIRECTORY "${RVX_CANDIDATE_WORKING_DIRECTORY}"
    RESULT_VARIABLE _rvx_result)
if(NOT _rvx_result EQUAL 0)
    message(FATAL_ERROR
        "Fresh GFX25 candidate producer failed with exit code ${_rvx_result}")
endif()
