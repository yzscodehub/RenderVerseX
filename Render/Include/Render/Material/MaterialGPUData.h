#pragma once

/**
 * @file MaterialGPUData.h
 * @brief GPU-side material constant structures
 * 
 * Defines the GPU constant buffer layout for PBR materials,
 * matching the HLSL cbuffer in PBRLit.hlsl.
 */

#include "Core/Types.h"
#include "Core/MathTypes.h"

namespace RVX
{
    /**
     * @brief Texture flags for material shader
     * 
     * Bitmask indicating which textures are bound for the material.
     * Must match the defines in PBRLit.hlsl.
     */
    enum class MaterialTextureFlags : uint32
    {
        None                = 0,
        HasBaseColor        = 0x01,
        HasNormal           = 0x02,
        HasMetallicRoughness = 0x04,
        HasOcclusion        = 0x08,
        HasEmissive         = 0x10,
    };

    /**
     * @brief GPU constant buffer layout for PBR materials
     * 
     * This structure is uploaded to the GPU and must match
     * the MaterialConstants cbuffer in PBRLit.hlsl exactly.
     * 
     * Note: Padding is added to maintain 16-byte alignment
     * required by constant buffers.
     */
    struct MaterialGPUConstants
    {
        // Base color (RGBA)
        Vec4 baseColorFactor = {1.0f, 1.0f, 1.0f, 1.0f};
        
        // PBR factors
        float metallicFactor = 0.0f;
        float roughnessFactor = 1.0f;
        float normalScale = 1.0f;
        float occlusionStrength = 1.0f;
        
        // Emissive
        Vec3 emissiveColor = {0.0f, 0.0f, 0.0f};
        float emissiveStrength = 1.0f;
        
        // Texture flags
        uint32 textureFlags = 0;
        Vec3 padding = {0.0f, 0.0f, 0.0f};
    };

    static_assert(sizeof(MaterialGPUConstants) == 64, 
                  "MaterialGPUConstants must be 64 bytes for cbuffer alignment");

} // namespace RVX
