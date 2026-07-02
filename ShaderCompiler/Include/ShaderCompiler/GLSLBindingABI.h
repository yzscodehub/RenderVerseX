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
            return set == 0 ? binding : 8 + (set - 1) * 4 + binding;
        }

        inline constexpr uint32 FlattenTextureBinding(uint32 set, uint32 binding)
        {
            return set == 2 ? 6 + binding : binding;
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
