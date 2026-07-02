#pragma once

/**
 * @file RayTracingResourceBindings.h
 * @brief Shared C++ descriptor binding layout for production ray tracing passes
 */

#include "Core/Types.h"

namespace RVX
{
    namespace RayTracingResourceBindings
    {
        namespace Shadow
        {
            constexpr uint32 RVX_RT_SHADOW_TLAS_BINDING = 0;
            constexpr uint32 RVX_RT_SHADOW_OUTPUT_MASK_BINDING = 1;
            constexpr uint32 RVX_RT_SHADOW_SCENE_DEPTH_BINDING = 2;
            constexpr uint32 RVX_RT_SHADOW_CONSTANTS_BINDING = 3;
            constexpr uint32 RVX_RT_SHADOW_PREVIOUS_MASK_BINDING = 4;
            constexpr uint32 RVX_RT_SHADOW_PREVIOUS_DEPTH_BINDING = 5;
            constexpr uint32 RVX_RT_SHADOW_OUTPUT_DEPTH_BINDING = 6;
            constexpr uint32 RVX_RT_SHADOW_PREVIOUS_NORMAL_BINDING = 7;
            constexpr uint32 RVX_RT_SHADOW_OUTPUT_NORMAL_BINDING = 8;
            constexpr uint32 RVX_RT_SHADOW_ALPHA_METADATA_BINDING = 9;
            constexpr uint32 RVX_RT_SHADOW_ALPHA_TEXTURES_BINDING = 10;
            constexpr uint32 RVX_RT_SHADOW_ALPHA_INDEX_BUFFERS_BINDING = 138;
            constexpr uint32 RVX_RT_SHADOW_ALPHA_UV_BUFFERS_BINDING = 266;
            constexpr uint32 RVX_RT_SHADOW_MATERIAL_METADATA_BINDING = 394;
            constexpr uint32 RVX_RT_SHADOW_MATERIAL_TEXTURES_BINDING = 395;
            constexpr uint32 RVX_RT_SHADOW_SCENE_VELOCITY_BINDING = 651;

            constexpr uint32 RVX_RT_SHADOW_MAX_MATERIAL_TEXTURES = 256;
            constexpr uint32 RVX_RT_SHADOW_MAX_ALPHA_TEXTURES = 128;
            constexpr uint32 RVX_RT_SHADOW_MAX_ALPHA_GEOMETRY_BUFFERS = 128;
        } // namespace Shadow

        namespace Reflection
        {
            constexpr uint32 RVX_RT_REFLECTION_TLAS_BINDING = 0;
            constexpr uint32 RVX_RT_REFLECTION_OUTPUT_BINDING = 1;
            constexpr uint32 RVX_RT_REFLECTION_SCENE_COLOR_BINDING = 2;
            constexpr uint32 RVX_RT_REFLECTION_SCENE_DEPTH_BINDING = 3;
            constexpr uint32 RVX_RT_REFLECTION_CONSTANTS_BINDING = 4;
            constexpr uint32 RVX_RT_REFLECTION_MATERIAL_METADATA_BINDING = 5;
            constexpr uint32 RVX_RT_REFLECTION_MATERIAL_TEXTURES_BINDING = 6;
            constexpr uint32 RVX_RT_REFLECTION_PREVIOUS_HISTORY_BINDING = 262;
            constexpr uint32 RVX_RT_REFLECTION_PREVIOUS_DEPTH_BINDING = 263;
            constexpr uint32 RVX_RT_REFLECTION_OUTPUT_DEPTH_BINDING = 264;
            constexpr uint32 RVX_RT_REFLECTION_PREVIOUS_NORMAL_BINDING = 265;
            constexpr uint32 RVX_RT_REFLECTION_OUTPUT_NORMAL_BINDING = 266;
            constexpr uint32 RVX_RT_REFLECTION_GEOMETRY_METADATA_BINDING = 267;
            constexpr uint32 RVX_RT_REFLECTION_INDEX_BUFFERS_BINDING = 268;
            constexpr uint32 RVX_RT_REFLECTION_UV_BUFFERS_BINDING = 396;
            constexpr uint32 RVX_RT_REFLECTION_NORMAL_BUFFERS_BINDING = 524;
            constexpr uint32 RVX_RT_REFLECTION_TANGENT_BUFFERS_BINDING = 652;
            constexpr uint32 RVX_RT_REFLECTION_SCENE_VELOCITY_BINDING = 780;

            constexpr uint32 RVX_RT_REFLECTION_MAX_MATERIAL_TEXTURES = 256;
            constexpr uint32 RVX_RT_REFLECTION_MAX_GEOMETRY_BUFFERS = 128;
        } // namespace Reflection
    } // namespace RayTracingResourceBindings
} // namespace RVX
