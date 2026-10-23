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

        // The complete Forward+ surface ABI has ten frame textures
        // (including the integrated froxel volume) and thirteen Material V2
        // textures. They may be live in the same fragment shader, so the
        // independent-sampler OpenGL path requires texture units [0, 22].
        inline constexpr uint32 RVX_GLSL_REQUIRED_TEXTURE_UNIT_COUNT = 23;
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
            // its object data uses space1 t1..3 and its Material V2 table is
            // space2 t27. Keep that complete concurrent set below 16.
            if (set == 0 && binding <= 12)
            {
                return binding;
            }
            if (set == 0 && binding == 14)
            {
                return 10;
            }
            // The SSR program only uses this frame-set SSBO for the exact
            // exposure producer state, so it may reuse physical binding 0
            // without colliding with DefaultLit's simultaneously bound set.
            if (set == 0 && binding == 16)
            {
                return 0;
            }
            if (set == 1 && binding >= 1 && binding <= 3)
            {
                return 11 + binding;
            }
            if (set == 2 && binding == 27)
            {
                return 15;
            }
            return RVX_INVALID_INDEX;
        }

        inline constexpr uint32 FlattenTextureBinding(
            uint32 set, uint32 binding)
        {
            // Frame textures retain their logical units except the two late
            // Forward+ surface inputs.  The opaque snapshot uses unit 14 and
            // the local-shadow atlas uses the last free unit 0 so the complete
            // frame + material surface ABI remains valid with the integrated
            // froxel field on devices exposing at least seventeen fragment
            // texture units.
            if (set == 0 && binding <= 15)
            {
                return binding;
            }
            if (set == 0 && binding == 16)
            {
                return 14;
            }
            if (set == 0 && binding == 17)
            {
                return 0;
            }
            // The froxel volume cannot share a physical texture unit with a
            // frame or material texture: it is a Texture3D that may be live
            // with every one of the sixteen existing 2D/cube/array bindings.
            if (set == 0 && binding == 18)
            {
                return 16;
            }
            // SceneDepthSnapshot is read through Texture.Load by transmission
            // shaders, so it does not consume a sampler pair. It still needs
            // a unique physical image unit because it can be live with every
            // material texture and the froxel Texture3D.
            if (set == 0 && binding == 19)
            {
                return 22;
            }

            // The original eight material textures retain their packed
            // disjoint units; five V2 extension textures occupy the new
            // 17..21 range. The simultaneously live froxel Texture3D remains
            // on unit 16.
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
                    case 9: return 17;
                    case 10: return 18;
                    case 11: return 19;
                    case 12: return 20;
                    case 13: return 21;
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
            if (set == 2 && binding >= 14 && binding <= 26)
            {
                return FlattenTextureBinding(set, binding - 13);
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
            if (set == 2 && textureBinding >= 1 && textureBinding <= 13 &&
                samplerBinding == textureBinding + 13)
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
            // Local point/spot shadow depth reuses the directional point-clamp
            // sampler while occupying the sole remaining physical unit.
            if (set == 0 && textureBinding == 17 && samplerBinding == 2)
            {
                outTextureUnit = FlattenTextureBinding(set, textureBinding);
                return true;
            }
            // The integrated 3D froxel field shares the frame linear-clamp
            // sampler with IBL and the opaque snapshot, while retaining a
            // distinct image unit because all of those image types can be
            // sampled concurrently by Forward+ translucency.
            if (set == 0 && textureBinding == 18 && samplerBinding == 13)
            {
                outTextureUnit = FlattenTextureBinding(set, textureBinding);
                return true;
            }
            // Hi-Z reflection uses one linear-clamp sampler for scene color
            // and the fixed-capacity local cubemap array. Base indirect
            // specular (t15) and receiver response (t17) use integer Load and
            // therefore do not consume a sampler pair.
            if (set == 0 && samplerBinding == 19 &&
                (textureBinding == 1 ||
                 (textureBinding >= 7 && textureBinding <= 14)))
            {
                outTextureUnit = FlattenTextureBinding(set, textureBinding);
                return true;
            }
            outTextureUnit = RVX_INVALID_INDEX;
            return false;
        }
    } // namespace GLSLBindingABI
} // namespace RVX
