#pragma once

#include "Core/Types.h"
#include "RHI/RHIDescriptor.h"

namespace RVX
{
    namespace GLSLBindingABI
    {
        inline constexpr uint32 RVX_GLSL_PUSH_CONSTANT_UBO_BINDING = 0;
        inline constexpr uint32 RVX_GLSL_UNIFORM_BUFFER_BINDING_BASE = 1;
        inline constexpr uint32 RVX_GLSL_DUMMY_SAMPLER_BINDING = 15;

        inline constexpr uint32 FlattenUniformBufferBinding(uint32 set, uint32 binding)
        {
            return RVX_GLSL_UNIFORM_BUFFER_BINDING_BASE + set * 4 + binding;
        }

        inline constexpr uint32 FlattenStorageBufferBinding(uint32 set, uint32 binding)
        {
            // OpenGL 4.5 guarantees only 16 SSBO binding points. Preserve the
            // engine's set-0 GPU/lighting range (0..11), place raster object
            // buffers set1/t1..t3 at 12..14, and reserve 15 for the material
            // parameter table set2/t11. Unsupported layouts fail closed.
            if (set == 0 && binding <= 11)
            {
                return binding;
            }
            if (set == 1 && binding >= 1 && binding <= 3)
            {
                return 11 + binding;
            }
            if (set == 2 && binding == 11)
            {
                return 15;
            }
            return RVX_INVALID_INDEX;
        }

        inline constexpr uint32 FlattenTextureBinding(uint32 set, uint32 binding)
        {
            // The DefaultLit fragment stage needs seven sparse frame textures
            // and five material textures within OpenGL's 16 guaranteed
            // per-stage texture units. Preserve frame slots and place material
            // t1..t5 into otherwise-unused units without aliasing cube/array
            // textures in set 0.
            if (set == 0 && binding < 16)
            {
                return binding;
            }
            if (set == 2)
            {
                switch (binding)
                {
                    case 1: return 2;
                    case 2: return 3;
                    case 3: return 7;
                    case 4: return 8;
                    case 5: return 9;
                    default: break;
                }
            }
            return RVX_INVALID_INDEX;
        }

        inline constexpr uint32 FlattenBinding(RHIBindingType type, uint32 set, uint32 binding)
        {
            switch (type)
            {
                case RHIBindingType::UniformBuffer:
                case RHIBindingType::DynamicUniformBuffer:
                    return FlattenUniformBufferBinding(set, binding);
                case RHIBindingType::ShaderResourceBuffer:
                case RHIBindingType::StorageBuffer:
                case RHIBindingType::DynamicStorageBuffer:
                    return FlattenStorageBufferBinding(set, binding);
                case RHIBindingType::SampledTexture:
                case RHIBindingType::CombinedTextureSampler:
                    return FlattenTextureBinding(set, binding);
                default:
                    return binding;
            }
        }
    } // namespace GLSLBindingABI
} // namespace RVX
