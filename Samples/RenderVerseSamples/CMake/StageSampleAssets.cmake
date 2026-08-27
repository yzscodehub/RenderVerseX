cmake_minimum_required(VERSION 3.25)

foreach(_rvx_required SOURCE_ROOT BUILD_ROOT DESTINATION_ROOT STAMP_PATH
                      HDR_FIXTURE_WRITER INTERIOR_FIXTURE_WRITER
                      TRANSPARENT_MATERIALS_FIXTURE_WRITER PARTICLE_VFX_FIXTURE_WRITER
                      COOK_EXECUTABLE COOKED_CATALOG_WRITER TOON_COOK_PROFILE
                      TRANSPARENT_MATERIALS_COOK_PROFILE)
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

# GFX14 uses one real, hermetic cook.  The source copy lives only in the
# transaction staging directory while RVXCook and the catalog writer verify
# its source identity; it is removed before the package is published so Sample
# runtime admission cannot reach a development importer/source fallback.
set(_rvx_toon_source_root "${_rvx_staging}/cook-inputs/stylized-materials-toon")
set(_rvx_toon_cooked_root "${_rvx_staging}/cooked/stylized-materials-toon")
set(_rvx_toon_manifest "${_rvx_toon_cooked_root}/CookManifest.rvxmanifest")
file(MAKE_DIRECTORY "${_rvx_toon_source_root}")
file(COPY_FILE
    "${SOURCE_ROOT}/Assets/models/pbr-material-grid/scene.gltf"
    "${_rvx_toon_source_root}/scene.gltf"
    ONLY_IF_DIFFERENT)
execute_process(
    COMMAND "${COOK_EXECUTABLE}"
        --source "${_rvx_toon_source_root}"
        --output "${_rvx_toon_cooked_root}"
        --manifest "${_rvx_toon_manifest}"
        --profile "${TOON_COOK_PROFILE}"
        --model-only
        --fail-on-errors
    RESULT_VARIABLE _rvx_toon_cook_result)
if(NOT _rvx_toon_cook_result EQUAL 0)
    message(FATAL_ERROR "Stylized Toon fixture cook failed")
endif()
execute_process(
    COMMAND "${COOKED_CATALOG_WRITER}"
        --input-catalog "${_rvx_staging}/catalog.json"
        --output-catalog "${_rvx_staging}/catalog.json"
        --asset-root "${_rvx_staging}"
        --source-root "${_rvx_toon_source_root}"
        --manifest "${_rvx_toon_manifest}"
        --cooked-root "${_rvx_toon_cooked_root}"
        --cooked-root-relative "cooked/stylized-materials-toon"
        --asset-id "stylized-materials-toon"
        --asset-path-relative "cooked/stylized-materials-toon/scene.rva"
        --source-path "scene.gltf"
        --source-name "RenderVerseX stylized material grid fixture"
        --source-uri "repo://Samples/RenderVerseSamples/Assets/models/pbr-material-grid/scene.gltf"
        --source-version "gfx14-toon-cook-v1"
        --attribution "Generated from the RenderVerseX PBR material grid fixture."
        --modification-notice "Cooked with the fixed GFX14 ToonLit material profile; source bytes are not staged at runtime."
        --derived-from "pbr-material-grid"
        --generation-recipe "rvx-gfx14-toon-material-cook-v1"
    RESULT_VARIABLE _rvx_toon_catalog_result)
if(NOT _rvx_toon_catalog_result EQUAL 0)
    message(FATAL_ERROR "Stylized Toon cooked catalog publication failed")
endif()
file(REMOVE_RECURSE "${_rvx_toon_source_root}")

# GFX15 mirrors the GFX14 transaction discipline, but its source is generated
# in the staging-only cook root.  The fixture writer and RVXCook are build-time
# tools; publication admits only manifest-verified cooked bytes.
set(_rvx_transparent_source_root
    "${_rvx_staging}/cook-inputs/transparent-materials-v1")
set(_rvx_transparent_cooked_root
    "${_rvx_staging}/cooked/transparent-materials-v1")
set(_rvx_transparent_manifest
    "${_rvx_transparent_cooked_root}/CookManifest.rvxmanifest")
file(MAKE_DIRECTORY "${_rvx_transparent_source_root}")
execute_process(
    COMMAND "${TRANSPARENT_MATERIALS_FIXTURE_WRITER}"
        --output "${_rvx_transparent_source_root}/scene.gltf"
    RESULT_VARIABLE _rvx_transparent_fixture_result)
if(NOT _rvx_transparent_fixture_result EQUAL 0)
    message(FATAL_ERROR "Transparent-material fixture generation failed")
endif()
execute_process(
    COMMAND "${COOK_EXECUTABLE}"
        --source "${_rvx_transparent_source_root}"
        --output "${_rvx_transparent_cooked_root}"
        --manifest "${_rvx_transparent_manifest}"
        --profile "${TRANSPARENT_MATERIALS_COOK_PROFILE}"
        --model-only
        --fail-on-errors
    RESULT_VARIABLE _rvx_transparent_cook_result)
if(NOT _rvx_transparent_cook_result EQUAL 0)
    message(FATAL_ERROR "Transparent-material fixture cook failed")
endif()
execute_process(
    COMMAND "${COOKED_CATALOG_WRITER}"
        --input-catalog "${_rvx_staging}/catalog.json"
        --output-catalog "${_rvx_staging}/catalog.json"
        --asset-root "${_rvx_staging}"
        --source-root "${_rvx_transparent_source_root}"
        --manifest "${_rvx_transparent_manifest}"
        --cooked-root "${_rvx_transparent_cooked_root}"
        --cooked-root-relative "cooked/transparent-materials-v1"
        --asset-id "transparent-materials-v1"
        --asset-path-relative "cooked/transparent-materials-v1/scene.rva"
        --source-path "scene.gltf"
        --source-name "RenderVerseX transparent-material fixture"
        --source-uri "repo://Samples/Common/Tools/SampleTransparentMaterialsFixtureWriter.cpp"
        --source-version "gfx15-transparent-materials-cook-v1"
        --attribution "Generated from the RenderVerseX deterministic transparent-material fixture."
        --modification-notice "Cooked with the fixed GFX15 transparency profile; source bytes are not staged at runtime."
        --derived-from "transparent-materials-fixture"
        --generation-recipe "rvx-gfx15-transparent-material-cook-v1"
    RESULT_VARIABLE _rvx_transparent_catalog_result)
if(NOT _rvx_transparent_catalog_result EQUAL 0)
    message(FATAL_ERROR "Transparent-material cooked catalog publication failed")
endif()
file(REMOVE_RECURSE "${_rvx_transparent_source_root}")
if(EXISTS "${_rvx_transparent_source_root}")
    message(FATAL_ERROR
        "Transparent-material cook input remains after staging; cooked-only runtime admission is unsafe")
endif()

# The particle qualification scenes need an opaque depth/shadow fixture, but
# the existing shadow-plane-caster catalog entry intentionally remains a source
# model for other Samples.  Publish a distinct cooked-only identity from the
# same hermetic input so GFX17 never admits the source entry as a fallback.
set(_rvx_particle_ground_id "particle-vfx-ground-caster-v1")
set(_rvx_particle_ground_source_root
    "${_rvx_staging}/cook-inputs/${_rvx_particle_ground_id}")
set(_rvx_particle_ground_cooked_root
    "${_rvx_staging}/cooked/${_rvx_particle_ground_id}")
set(_rvx_particle_ground_manifest
    "${_rvx_particle_ground_cooked_root}/CookManifest.rvxmanifest")
file(MAKE_DIRECTORY "${_rvx_particle_ground_source_root}")
file(COPY_FILE
    "${SOURCE_ROOT}/Assets/models/shadow-plane-caster/scene.gltf"
    "${_rvx_particle_ground_source_root}/scene.gltf"
    ONLY_IF_DIFFERENT)
execute_process(
    COMMAND "${COOK_EXECUTABLE}"
        --source "${_rvx_particle_ground_source_root}"
        --output "${_rvx_particle_ground_cooked_root}"
        --manifest "${_rvx_particle_ground_manifest}"
        --model-only
        --fail-on-errors
    RESULT_VARIABLE _rvx_particle_ground_cook_result)
if(NOT _rvx_particle_ground_cook_result EQUAL 0)
    message(FATAL_ERROR "Particle VFX ground/caster fixture cook failed")
endif()
execute_process(
    COMMAND "${COOKED_CATALOG_WRITER}"
        --input-catalog "${_rvx_staging}/catalog.json"
        --output-catalog "${_rvx_staging}/catalog.json"
        --asset-root "${_rvx_staging}"
        --source-root "${_rvx_particle_ground_source_root}"
        --manifest "${_rvx_particle_ground_manifest}"
        --cooked-root "${_rvx_particle_ground_cooked_root}"
        --cooked-root-relative "cooked/${_rvx_particle_ground_id}"
        --asset-id "${_rvx_particle_ground_id}"
        --asset-kind model
        --asset-path-relative "cooked/${_rvx_particle_ground_id}/scene.rva"
        --source-path "scene.gltf"
        --source-name "RenderVerseX particle VFX opaque ground/caster fixture"
        --source-uri "repo://Samples/RenderVerseSamples/Assets/models/shadow-plane-caster/scene.gltf"
        --source-version "gfx17-particle-vfx-ground-caster-cook-v1"
        --attribution "Generated from the RenderVerseX shadow plane/caster fixture."
        --modification-notice "Cooked as the GFX17 opaque depth/shadow fixture; source bytes are not staged in this package."
        --derived-from "shadow-plane-caster"
        --generation-recipe "rvx-gfx17-particle-vfx-ground-caster-cook-v1"
    RESULT_VARIABLE _rvx_particle_ground_catalog_result)
if(NOT _rvx_particle_ground_catalog_result EQUAL 0)
    message(FATAL_ERROR
        "Particle VFX ground/caster cooked catalog publication failed")
endif()
file(REMOVE_RECURSE "${_rvx_particle_ground_source_root}")
if(EXISTS "${_rvx_particle_ground_source_root}")
    message(FATAL_ERROR
        "Particle VFX ground/caster source input remains after staging; cooked-only runtime admission is unsafe")
endif()

# GFX17 follows the same hermetic publication transaction as the material
# samples.  The generated atlas is deliberately created before effect.rvxparticle
# so the cooker's deterministic source traversal resolves the texture product
# before the effect's package-relative "atlas.rva" dependency.  Authoring bytes
# never cross the staging boundary.
function(rvx_stage_particle_vfx _rvx_id _rvx_kind _rvx_effect_directory)
    set(_rvx_source_root "${_rvx_staging}/cook-inputs/${_rvx_id}")
    set(_rvx_effect_source_directory
        "${_rvx_source_root}/Effects/${_rvx_effect_directory}")
    set(_rvx_cooked_root "${_rvx_staging}/cooked/${_rvx_id}")
    set(_rvx_manifest "${_rvx_cooked_root}/CookManifest.rvxmanifest")
    set(_rvx_effect_relative "Effects/${_rvx_effect_directory}/effect.rvxvfx")

    file(MAKE_DIRECTORY "${_rvx_effect_source_directory}")
    execute_process(
        COMMAND "${PARTICLE_VFX_FIXTURE_WRITER}"
            --output-dir "${_rvx_effect_source_directory}"
            --kind "${_rvx_kind}"
        RESULT_VARIABLE _rvx_fixture_result)
    if(NOT _rvx_fixture_result EQUAL 0)
        message(FATAL_ERROR "${_rvx_id} particle fixture generation failed")
    endif()

    execute_process(
        COMMAND "${COOK_EXECUTABLE}"
            --source "${_rvx_source_root}"
            --output "${_rvx_cooked_root}"
            --manifest "${_rvx_manifest}"
            --fail-on-errors
        RESULT_VARIABLE _rvx_cook_result)
    if(NOT _rvx_cook_result EQUAL 0)
        message(FATAL_ERROR "${_rvx_id} particle effect cook failed")
    endif()

    execute_process(
        COMMAND "${COOKED_CATALOG_WRITER}"
            --input-catalog "${_rvx_staging}/catalog.json"
            --output-catalog "${_rvx_staging}/catalog.json"
            --asset-root "${_rvx_staging}"
            --source-root "${_rvx_source_root}"
            --manifest "${_rvx_manifest}"
            --cooked-root "${_rvx_cooked_root}"
            --cooked-root-relative "cooked/${_rvx_id}"
            --asset-id "${_rvx_id}"
            --asset-kind effect
            --asset-path-relative "cooked/${_rvx_id}/${_rvx_effect_relative}"
            --source-path "Effects/${_rvx_effect_directory}/effect.rvxparticle"
            --source-name "RenderVerseX deterministic ${_rvx_kind} particle fixture"
            --source-uri "repo://Samples/Common/Tools/SampleParticleVfxFixtureWriter.cpp"
            --source-version "gfx17-${_rvx_kind}-particle-cook-v1"
            --attribution "Generated from the RenderVerseX deterministic ${_rvx_kind} particle fixture."
            --modification-notice "Cooked GFX17 particle product with a package-relative atlas; authoring bytes are not staged at runtime."
            --derived-from "${_rvx_kind}-particle-fixture"
            --generation-recipe "rvx-gfx17-${_rvx_kind}-particle-cook-v1"
        RESULT_VARIABLE _rvx_catalog_result)
    if(NOT _rvx_catalog_result EQUAL 0)
        message(FATAL_ERROR "${_rvx_id} cooked catalog publication failed")
    endif()

    file(REMOVE_RECURSE "${_rvx_source_root}")
    if(EXISTS "${_rvx_source_root}")
        message(FATAL_ERROR
            "${_rvx_id} source inputs remain after staging; cooked-only runtime admission is unsafe")
    endif()
endfunction()

rvx_stage_particle_vfx("particle-fire-v1" "fire" "Fire")
rvx_stage_particle_vfx("particle-smoke-v1" "smoke" "Smoke")

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
