cmake_minimum_required(VERSION 3.25)

foreach(_rvx_required FULL_ASSET_ROOT BUILD_ROOT DESTINATION_ROOT STAMP_PATH
                      CATALOG_WRITER MANIFEST_SOURCE)
    if(NOT DEFINED ${_rvx_required} OR "${${_rvx_required}}" STREQUAL "")
        message(FATAL_ERROR
            "StageRenderQualityLabCookedAssets requires ${_rvx_required}")
    endif()
endforeach()

cmake_path(NORMAL_PATH BUILD_ROOT OUTPUT_VARIABLE _rvx_build_root)
cmake_path(NORMAL_PATH DESTINATION_ROOT OUTPUT_VARIABLE _rvx_destination)
cmake_path(IS_PREFIX _rvx_build_root "${_rvx_destination}" NORMALIZE
           _rvx_destination_is_contained)
if(NOT _rvx_destination_is_contained)
    message(FATAL_ERROR
        "Cooked Sample asset destination must remain inside the configured build tree")
endif()

set(_rvx_asset_ids
    post-processing-interior-v1
    pbr-reference-environment-cooked-v1
    transparent-materials-v1
    particle-fire-v1
    particle-smoke-v1
    water-surface-reference-fixture-v1
    particle-vfx-ground-caster-v1
    water-bottle-cooked-v1
    pbr-material-grid-cooked-v1)
set(_rvx_staging "${_rvx_destination}.staging")
file(REMOVE_RECURSE "${_rvx_staging}")
file(MAKE_DIRECTORY "${_rvx_staging}/cooked")

foreach(_rvx_asset_id IN LISTS _rvx_asset_ids)
    set(_rvx_source_package "${FULL_ASSET_ROOT}/cooked/${_rvx_asset_id}")
    if(NOT IS_DIRECTORY "${_rvx_source_package}")
        message(FATAL_ERROR
            "Cooked Sample product is missing admitted package ${_rvx_asset_id}")
    endif()
    file(COPY "${_rvx_source_package}" DESTINATION "${_rvx_staging}/cooked")
endforeach()

foreach(_rvx_license
        LICENSE.generated-fixtures.txt
        LICENSE.quaternius-cc0.txt
        LICENSE.khronos-sample-assets-cc0.txt
        LICENSE.crytek-sponza-cc-by-3.0.txt
        LICENSE.polyhaven-cc0.txt)
    if(EXISTS "${FULL_ASSET_ROOT}/${_rvx_license}")
        file(COPY_FILE "${FULL_ASSET_ROOT}/${_rvx_license}"
             "${_rvx_staging}/${_rvx_license}" ONLY_IF_DIFFERENT)
    endif()
endforeach()
file(COPY_FILE "${MANIFEST_SOURCE}" "${_rvx_staging}/renderverse-samples.json"
     ONLY_IF_DIFFERENT)

set(_rvx_catalog_arguments
    --input-catalog "${FULL_ASSET_ROOT}/catalog.json"
    --output-catalog "${_rvx_staging}/catalog.json")
foreach(_rvx_asset_id IN LISTS _rvx_asset_ids)
    list(APPEND _rvx_catalog_arguments --asset-id "${_rvx_asset_id}")
endforeach()
execute_process(
    COMMAND "${CATALOG_WRITER}" ${_rvx_catalog_arguments}
    RESULT_VARIABLE _rvx_catalog_result)
if(NOT _rvx_catalog_result EQUAL 0)
    message(FATAL_ERROR "Cooked Sample product catalog publication failed")
endif()

# The final product directory is intentionally a strict artifact-only subset.
# Do not hide authoring inputs behind an ignored extension or a second mount.
file(GLOB_RECURSE _rvx_source_residue RELATIVE "${_rvx_staging}"
    "${_rvx_staging}/*.gltf" "${_rvx_staging}/*.glb"
    "${_rvx_staging}/*.fbx" "${_rvx_staging}/*.obj"
    "${_rvx_staging}/*.hdr" "${_rvx_staging}/*.exr"
    "${_rvx_staging}/*.png" "${_rvx_staging}/*.jpg"
    "${_rvx_staging}/*.jpeg" "${_rvx_staging}/*.tga"
    "${_rvx_staging}/*.dds" "${_rvx_staging}/*.ktx"
    "${_rvx_staging}/*.ktx2" "${_rvx_staging}/*.rvxparticle"
    "${_rvx_staging}/*.hlsl" "${_rvx_staging}/*.glsl"
    "${_rvx_staging}/*.msl")
if(_rvx_source_residue)
    message(FATAL_ERROR
        "Cooked Sample product contains forbidden source residue: ${_rvx_source_residue}")
endif()

file(WRITE "${_rvx_staging}/.rvx-staged"
    "schemaVersion=1\nsource=RenderVerseSamplesCooked\nassetCount=9\n")
file(REMOVE_RECURSE "${_rvx_destination}")
file(RENAME "${_rvx_staging}" "${_rvx_destination}")
if(NOT EXISTS "${STAMP_PATH}")
    message(FATAL_ERROR "Cooked Sample product staging did not produce its receipt")
endif()
