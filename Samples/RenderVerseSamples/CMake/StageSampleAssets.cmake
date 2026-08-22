cmake_minimum_required(VERSION 3.25)

foreach(_rvx_required SOURCE_ROOT BUILD_ROOT DESTINATION_ROOT STAMP_PATH
                      HDR_FIXTURE_WRITER INTERIOR_FIXTURE_WRITER)
    if(NOT DEFINED ${_rvx_required} OR "${${_rvx_required}}" STREQUAL "")
        message(FATAL_ERROR "StageSampleAssets requires ${_rvx_required}")
    endif()
endforeach()

cmake_path(NORMAL_PATH BUILD_ROOT OUTPUT_VARIABLE _rvx_build_root)
cmake_path(NORMAL_PATH DESTINATION_ROOT OUTPUT_VARIABLE _rvx_destination)
cmake_path(IS_PREFIX _rvx_build_root "${_rvx_destination}" NORMALIZE
           _rvx_destination_is_contained)
if(NOT _rvx_destination_is_contained)
    message(FATAL_ERROR
        "Sample asset destination must remain inside the configured build tree")
endif()

set(_rvx_staging "${_rvx_destination}.staging")
file(REMOVE_RECURSE "${_rvx_staging}")
file(MAKE_DIRECTORY
    "${_rvx_staging}/models"
    "${_rvx_staging}/cooked"
    "${_rvx_staging}/environments"
)

foreach(_rvx_model water-bottle corset casual-female crytek-sponza)
    file(COPY "${SOURCE_ROOT}/Assets/models/${_rvx_model}"
         DESTINATION "${_rvx_staging}/models")
endforeach()
foreach(_rvx_product casual-female casual-female-root-motion)
    file(COPY "${SOURCE_ROOT}/Assets/cooked/${_rvx_product}"
         DESTINATION "${_rvx_staging}/cooked")
endforeach()
file(COPY
    "${SOURCE_ROOT}/Assets/environments/kloofendal-48d-partly-cloudy-pure-sky"
    DESTINATION "${_rvx_staging}/environments")

foreach(_rvx_fixture
        "shadow-plane-caster"
        "r7-triangle"
        "pbr-material-grid"
        "pbr-material-texture-cube")
    file(MAKE_DIRECTORY "${_rvx_staging}/models/${_rvx_fixture}")
    file(COPY_FILE
        "${SOURCE_ROOT}/Assets/models/${_rvx_fixture}/scene.gltf"
        "${_rvx_staging}/models/${_rvx_fixture}/scene.gltf"
        ONLY_IF_DIFFERENT)
endforeach()

file(MAKE_DIRECTORY
    "${_rvx_staging}/models/interior-rendering-p0a"
    "${_rvx_staging}/environments/pbr-reference")
execute_process(
    COMMAND "${INTERIOR_FIXTURE_WRITER}"
        --output "${_rvx_staging}/models/interior-rendering-p0a/scene.gltf"
    RESULT_VARIABLE _rvx_interior_result)
if(NOT _rvx_interior_result EQUAL 0)
    message(FATAL_ERROR "Interior sample fixture generation failed")
endif()
execute_process(
    COMMAND "${HDR_FIXTURE_WRITER}"
        --output "${_rvx_staging}/environments/pbr-reference/environment.hdr"
    RESULT_VARIABLE _rvx_hdr_result)
if(NOT _rvx_hdr_result EQUAL 0)
    message(FATAL_ERROR "HDR sample fixture generation failed")
endif()

file(COPY_FILE "${SOURCE_ROOT}/Assets/catalog.json"
     "${_rvx_staging}/catalog.json" ONLY_IF_DIFFERENT)
file(COPY_FILE "${SOURCE_ROOT}/Manifests/renderverse-samples.json"
     "${_rvx_staging}/renderverse-samples.json" ONLY_IF_DIFFERENT)
foreach(_rvx_license
        LICENSE.generated-fixtures.txt
        LICENSE.quaternius-cc0.txt
        LICENSE.khronos-sample-assets-cc0.txt
        LICENSE.crytek-sponza-cc-by-3.0.txt
        LICENSE.polyhaven-cc0.txt)
    file(COPY_FILE "${SOURCE_ROOT}/Assets/${_rvx_license}"
         "${_rvx_staging}/${_rvx_license}" ONLY_IF_DIFFERENT)
endforeach()

file(WRITE "${_rvx_staging}/.rvx-staged"
    "schemaVersion=1\nsource=RenderVerseSamples\n")
file(REMOVE_RECURSE "${_rvx_destination}")
file(RENAME "${_rvx_staging}" "${_rvx_destination}")

if(NOT EXISTS "${STAMP_PATH}")
    message(FATAL_ERROR "Sample asset staging did not produce its receipt")
endif()
