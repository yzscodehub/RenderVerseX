#pragma once

/** @file GLSLBindingABI.h @brief Stable runtime OpenGL descriptor binding ABI */

#include "Core/Types.h"
#include "RHI/RHIDescriptor.h"

namespace RVX
{
    namespace GLSLBindingABI
    {
        inline constexpr uint32 RVX_GLSL_PUSH_CONSTANT_UBO_BINDING = 0;
        inline constexpr uint32 RVX_GLSL_UNIFORM_BUFFER_BINDING_BASE = 1;
        inline constexpr uint32 RVX_GLSL_DUMMY_SAMPLER_BINDING = 15;

        inline constexpr uint32 RVX_GLSL_REQUIRED_TEXTURE_UNIT_COUNT = 16;
        inline constexpr uint32 RVX_GLSL_REQUIRED_STORAGE_BUFFER_BINDING_COUNT = 16;

        [[nodiscard]] inline constexpr bool IsValidBinding(uint32 binding)
        {
            return binding != RVX_INVALID_INDEX;
        }

        inline constexpr uint32 FlattenUniformBufferBinding(
            uint32 set, uint32 binding)
        {
            if (set == 0 && binding <= 7)
            {
                return RVX_GLSL_UNIFORM_BUFFER_BINDING_BASE + binding;
            }
            if (set == 1 && binding == 0)
            {
                return 5;
            }
            if (set == 2 && binding == 0)
            {
                return 9;
            }
            return RVX_INVALID_INDEX;
        }

        inline constexpr uint32 FlattenStorageBufferBinding(
            uint32 set, uint32 binding)
        {
            // DefaultLit's frame data needs t4, t5, t8, t9 and t14 while
            // its object data uses space1 t1..3 and its material table is
            // space2 t17. Keep that complete concurrent set below 16.
            if (set == 0 && binding <= 12)
            {
                return binding;
            }
            if (set == 0 && binding == 14)
            {
                return 10;
            }
            if (set == 1 && binding >= 1 && binding <= 3)
            {
                return 11 + binding;
            }
            if (set == 2 && binding == 17)
            {
                return 15;
            }
            return RVX_INVALID_INDEX;
        }

        inline constexpr uint32 FlattenTextureBinding(
            uint32 set, uint32 binding)
        {
            // Frame textures retain their logical units except t16, which
            // occupies an otherwise unused unit in the Forward+ material ABI.
            if (set == 0 && binding <= 15)
            {
                return binding;
            }
            if (set == 0 && binding == 16)
            {
                return 14;
            }

            // The eight material textures must coexist with DefaultLit frame
            // textures on the OpenGL 4.5 minimum of sixteen texture units.
            if (set == 2)
            {
                switch (binding)
                {
                    case 1: return 2;
                    case 2: return 3;
                    case 3: return 7;
                    case 4: return 8;
                    case 5: return 9;
                    case 6: return 4;
                    case 7: return 5;
                    case 8: return 13;
                    default: break;
                }
            }
            return RVX_INVALID_INDEX;
        }

        inline constexpr uint32 FlattenSamplerBinding(
            uint32 set, uint32 binding)
        {
            if (set == 0)
            {
                // A sampler's primary unit is only used for reflected GLSL
                // metadata. Runtime binding uses TryGetTextureSamplerUnit()
                // for every actual image/sampler pair.
                switch (binding)
                {
                    case 0:
                    case 1: return 0;
                    case 2:
                    case 5:
                    case 7: return 1;
                    case 9: return 8;
                    case 13: return 10;
                    case 19: return 14;
                    default: break;
                }
            }
            if (set == 2 && binding >= 9 && binding <= 16)
            {
                return FlattenTextureBinding(set, binding - 8);
            }
            return RVX_INVALID_INDEX;
        }

        inline constexpr uint32 FlattenStorageTextureBinding(
            uint32 set, uint32 binding)
        {
            // Image units are independent from sampled texture units.
            return set == 0 && binding < 8 ? binding : RVX_INVALID_INDEX;
        }

        inline constexpr uint32 FlattenBinding(
            RHIBindingType type, uint32 set, uint32 binding)
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
                case RHIBindingType::Sampler:
                    return FlattenSamplerBinding(set, binding);
                case RHIBindingType::StorageTexture:
                    return FlattenStorageTextureBinding(set, binding);
                default:
                    return RVX_INVALID_INDEX;
            }
        }

        [[nodiscard]] inline constexpr bool TryFlattenBinding(
            RHIBindingType type, uint32 set, uint32 binding, uint32& outBinding)
        {
            outBinding = FlattenBinding(type, set, binding);
            return IsValidBinding(outBinding);
        }

        /**
         * Returns a physical texture unit only for pairs whose relationship is
         * stable in the public renderer ABI. The material set deliberately has
         * one sampler per texture. DefaultLit frame samplers cover the shadow
         * texture and the three IBL textures respectively.
         */
        [[nodiscard]] inline constexpr bool TryGetTextureSamplerUnit(
            uint32 set,
            uint32 textureBinding,
            uint32 samplerBinding,
            uint32& outTextureUnit)
        {
            if (set == 2 && textureBinding >= 1 && textureBinding <= 8 &&
                samplerBinding == textureBinding + 8)
            {
                outTextureUnit = FlattenTextureBinding(set, textureBinding);
                return true;
            }
            if (set == 0 && samplerBinding == 2 && textureBinding == 1)
            {
                outTextureUnit = FlattenTextureBinding(set, textureBinding);
                return true;
            }
            if (set == 0 && samplerBinding == 13 &&
                textureBinding >= 10 && textureBinding <= 12)
            {
                outTextureUnit = FlattenTextureBinding(set, textureBinding);
                return true;
            }
            // The opaque-scene snapshot deliberately shares the IBL linear
            // clamp sampler. Its texture occupies unit 14, while t10..t12
            // continue to use that sampler at their own physical units.
            if (set == 0 && textureBinding == 16 && samplerBinding == 13)
            {
                outTextureUnit = FlattenTextureBinding(set, textureBinding);
                return true;
            }
            outTextureUnit = RVX_INVALID_INDEX;
            return false;
        }
    } // namespace GLSLBindingABI
} // namespace RVX
