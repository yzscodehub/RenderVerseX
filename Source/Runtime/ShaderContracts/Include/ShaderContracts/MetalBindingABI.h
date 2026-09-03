#pragma once

/** @file MetalBindingABI.h @brief Stable direct Metal resource-index ABI */

#include "Core/Types.h"
#include "RHI/RHIDescriptor.h"

namespace RVX
{
    namespace MetalBindingABI
    {
        // Metal exposes buffer indices [0, 30]. Descriptor buffers remain in
        // [0, 22], leaving the high vertex-stream range untouched.
        inline constexpr uint32 RVX_METAL_MAX_BUFFER_INDEX = 30;
        inline constexpr uint32 RVX_METAL_MAX_DESCRIPTOR_BUFFER_INDEX = 22;
        inline constexpr uint32 RVX_METAL_MAX_TEXTURE_INDEX = 127;
        inline constexpr uint32 RVX_METAL_MAX_SAMPLER_INDEX = 15;
        inline constexpr uint32 RVX_METAL_FIRST_OBJECT_BUFFER_INDEX = 16;
        inline constexpr uint32 RVX_METAL_FIRST_MATERIAL_BUFFER_INDEX = 20;
        inline constexpr uint32 RVX_METAL_FIRST_FRAME_TEXTURE_INDEX = 16;
        inline constexpr uint32 RVX_METAL_FIRST_STORAGE_TEXTURE_INDEX = 64;

        [[nodiscard]] inline constexpr bool IsValidBufferIndex(uint32 index)
        {
            return index <= RVX_METAL_MAX_DESCRIPTOR_BUFFER_INDEX;
        }

        [[nodiscard]] inline constexpr bool IsValidTextureIndex(uint32 index)
        {
            return index <= RVX_METAL_MAX_TEXTURE_INDEX;
        }

        [[nodiscard]] inline constexpr bool IsValidSamplerIndex(uint32 index)
        {
            return index <= RVX_METAL_MAX_SAMPLER_INDEX;
        }

        [[nodiscard]] inline constexpr bool TryGetBufferIndex(
            uint32 set, uint32 binding, uint32 arrayElement, uint32& outIndex)
        {
            if (set == 0 && binding <= RVX_METAL_MAX_DESCRIPTOR_BUFFER_INDEX &&
                arrayElement <= RVX_METAL_MAX_DESCRIPTOR_BUFFER_INDEX - binding)
            {
                outIndex = binding + arrayElement;
                return true;
            }
            if (set == 1 && binding <= 3 && arrayElement <= 3 - binding)
            {
                outIndex = RVX_METAL_FIRST_OBJECT_BUFFER_INDEX + binding + arrayElement;
                return true;
            }
            if (set == 2 && binding == 0 && arrayElement == 0)
            {
                outIndex = RVX_METAL_FIRST_MATERIAL_BUFFER_INDEX;
                return true;
            }
            if (set == 2 && binding == 27 && arrayElement == 0)
            {
                outIndex = RVX_METAL_FIRST_MATERIAL_BUFFER_INDEX + 1;
                return true;
            }
            outIndex = RVX_INVALID_INDEX;
            return false;
        }

        [[nodiscard]] inline constexpr bool TryGetTextureIndex(
            uint32 set, uint32 binding, uint32 arrayElement, uint32& outIndex)
        {
            if (set == 0 && binding <= 23 && arrayElement <= 23 - binding)
            {
                outIndex = RVX_METAL_FIRST_FRAME_TEXTURE_INDEX + binding + arrayElement;
                return IsValidTextureIndex(outIndex);
            }
            if (set == 2 && binding >= 1 && binding <= 13 &&
                arrayElement < 13 && binding - 1 <= 12 - arrayElement)
            {
                outIndex = binding - 1 + arrayElement;
                return true;
            }
            outIndex = RVX_INVALID_INDEX;
            return false;
        }

        [[nodiscard]] inline constexpr bool TryGetStorageTextureIndex(
            uint32 set, uint32 binding, uint32 arrayElement, uint32& outIndex)
        {
            if (set == 0 && binding < 32 && arrayElement < 32 - binding)
            {
                outIndex = RVX_METAL_FIRST_STORAGE_TEXTURE_INDEX + binding + arrayElement;
                return IsValidTextureIndex(outIndex);
            }
            outIndex = RVX_INVALID_INDEX;
            return false;
        }

        [[nodiscard]] inline constexpr bool TryGetSamplerIndex(
            uint32 set, uint32 binding, uint32 arrayElement, uint32& outIndex)
        {
            if (set == 0 && arrayElement == 0)
            {
                switch (binding)
                {
                    case 0: outIndex = 8; return true;
                    case 1: outIndex = 9; return true;
                    case 2: outIndex = 10; return true;
                    case 5: outIndex = 11; return true;
                    case 7: outIndex = 12; return true;
                    case 9: outIndex = 13; return true;
                    case 13: outIndex = 14; return true;
                    case 19: outIndex = 15; return true;
                    default: break;
                }
            }
            if (set == 2 && binding >= 14 && binding <= 21 &&
                arrayElement < 8 && binding - 14 <= 7 - arrayElement)
            {
                outIndex = binding - 14 + arrayElement;
                return true;
            }
            outIndex = RVX_INVALID_INDEX;
            return false;
        }

        [[nodiscard]] inline constexpr bool TryGetBindingIndex(
            RHIBindingType type,
            uint32 set,
            uint32 binding,
            uint32 arrayElement,
            uint32& outIndex)
        {
            switch (type)
            {
                case RHIBindingType::UniformBuffer:
                case RHIBindingType::DynamicUniformBuffer:
                case RHIBindingType::ShaderResourceBuffer:
                case RHIBindingType::StorageBuffer:
                case RHIBindingType::DynamicStorageBuffer:
                    return TryGetBufferIndex(set, binding, arrayElement, outIndex);
                case RHIBindingType::SampledTexture:
                case RHIBindingType::CombinedTextureSampler:
                    return TryGetTextureIndex(set, binding, arrayElement, outIndex);
                case RHIBindingType::Sampler:
                    return TryGetSamplerIndex(set, binding, arrayElement, outIndex);
                case RHIBindingType::StorageTexture:
                    return TryGetStorageTextureIndex(set, binding, arrayElement, outIndex);
                default:
                    outIndex = RVX_INVALID_INDEX;
                    return false;
            }
        }
    } // namespace MetalBindingABI
} // namespace RVX
