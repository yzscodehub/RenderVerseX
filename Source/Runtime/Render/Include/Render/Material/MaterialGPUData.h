#pragma once

/**
 * @file MaterialGPUData.h
 * @brief GPU-side material constant structures
 *
 * Defines the GPU constant buffer layout for PBR materials,
 * matching the HLSL cbuffer in DefaultLit.hlsl.
 */

#include "Core/Types.h"
#include "Core/MathTypes.h"

#include <array>
#include <cstddef>

namespace RVX
{
    /**
     * @brief Value-only compatibility identity for instance-material batching.
     *
     * The hash covers texture and sampler source bindings, while the material
     * handle remains the authoritative resource identity on the draw packet.
     */
    struct MaterialInstanceBindingKey
    {
        uint64 textureBindingHash = 0;
        bool parameterTableCompatible = false;

        [[nodiscard]] bool operator==(
            const MaterialInstanceBindingKey&) const noexcept = default;
    };

    /**
     * @brief Texture flags for material shader
     *
     * Bitmask indicating which textures are bound for the material.
     * Must match the defines in DefaultLit.hlsl.
     */
    enum class MaterialTextureFlags : uint32
    {
        None                = 0,
        HasBaseColor        = 0x01,
        HasNormal           = 0x02,
        HasMetallicRoughness = 0x04,
        HasOcclusion        = 0x08,
        HasEmissive         = 0x10,
        HasToonRamp         = 0x20,
        HasTransmission     = 0x40,
        HasThickness        = 0x80,
        HasSpecular         = 0x100,
        HasSpecularColor    = 0x200,
        HasClearcoat        = 0x400,
        HasClearcoatRoughness = 0x800,
        HasClearcoatNormal  = 0x1000,
    };

    enum class MaterialGPUAlphaMode : uint32
    {
        Opaque = 0,
        Mask = 1,
        Blend = 2,
    };

    enum class MaterialGPUWorkflow : uint32
    {
        MetallicRoughness = 0,
        SpecularGlossiness = 1,
        Unlit = 2,
    };

    /**
     * @brief Immutable shading-model selector consumed by every raster path.
     *
     * This mirrors MaterialShadingModel rather than the legacy source
     * workflow.  The workflow only describes source parameter compatibility;
     * it must never select a renderer program family.
     */
    enum class MaterialGPUShadingModel : uint32
    {
        StandardLit = 0,
        ToonLit = 1,
        Unlit = 2,
        ThinTransmission = 3,
    };

    /** Mirrors MaterialBlendMode so shader output cannot infer blend policy. */
    enum class MaterialGPUBlendMode : uint32
    {
        Opaque = 0,
        Masked = 1,
        AlphaBlend = 2,
        Premultiplied = 3,
        Additive = 4,
    };

    /**
     * V2's descriptor ABI keeps one declared texture/sampler pair for every
     * MaterialUploadTextureSlot. These slots are deliberately fixed rather
     * than bindless so a material cannot silently lose an extension texture.
     */
    inline constexpr uint32 RVX_MATERIAL_GPU_TEXTURE_SLOT_COUNT = 13;

    /** @brief Packed affine UV transform for one fixed material texture slot. */
    struct MaterialGPUUVTransform
    {
        /** xy = scale, zw = offset. */
        Vec4 scaleOffset = {1.0f, 1.0f, 0.0f, 0.0f};
        /** xy = cos/sin(rotation), z = UV set (0 or 1), w reserved. */
        Vec4 rotationUVSet = {1.0f, 0.0f, 0.0f, 0.0f};
    };

    static_assert(sizeof(MaterialGPUUVTransform) == 32);

    /**
     * @brief GPU constant buffer layout for PBR materials
     *
     * This structure is uploaded to the GPU and must match
     * the MaterialConstants cbuffer in DefaultLit.hlsl exactly.
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

        /** xyz = KHR_materials_specular color, w = scalar factor. */
        Vec4 specularColorFactor = {1.0f, 1.0f, 1.0f, 1.0f};
        /** x = factor, y = perceptual roughness, z = normal scale. */
        Vec4 clearcoatParams = {0.0f, 0.0f, 1.0f, 0.0f};

        // Material state
        uint32 textureFlags = 0;
        uint32 alphaMode = static_cast<uint32>(MaterialGPUAlphaMode::Opaque);
        float alphaCutoff = 0.5f;
        uint32 workflow = static_cast<uint32>(MaterialGPUWorkflow::MetallicRoughness);

        uint32 doubleSided = 0;
        uint32 shadingModel =
            static_cast<uint32>(MaterialGPUShadingModel::StandardLit);
        /** Stable RenderResourceHandle::slot used by table consumers. */
        uint32 materialTableIndex = 0;
        float toonBands = 4.0f;
        float toonSpecularThreshold = 0.5f;

        float toonRimStrength = 0.0f;
        float toonOutlineWidth = 0.0f;
        /** Exact immutable MaterialBlendMode selected by the pipeline policy. */
        uint32 blendMode = static_cast<uint32>(MaterialGPUBlendMode::Opaque);
        /** xyz = shadow tint; w is reserved and must remain zero. */
        Vec4 toonShadowTint = {0.5f, 0.5f, 0.5f, 0.0f};

        /** x=transmission, y=IOR, z=thickness (m), w=attenuation distance (m). */
        Vec4 thinTransmissionParams = {0.0f, 1.5f, 0.0f, 0.0f};
        /** xyz=Beer-Lambert attenuation color; w is reserved and must be zero. */
        Vec4 attenuationColor = {1.0f, 1.0f, 1.0f, 0.0f};

        std::array<MaterialGPUUVTransform,
                   RVX_MATERIAL_GPU_TEXTURE_SLOT_COUNT> uvTransforms{};
    };

    static_assert(sizeof(MaterialGPUConstants) == 592,
                  "MaterialGPUConstants must match the HLSL cbuffer layout");
    static_assert(offsetof(MaterialGPUConstants, baseColorFactor) == 0);
    static_assert(offsetof(MaterialGPUConstants, metallicFactor) == 16);
    static_assert(offsetof(MaterialGPUConstants, roughnessFactor) == 20);
    static_assert(offsetof(MaterialGPUConstants, normalScale) == 24);
    static_assert(offsetof(MaterialGPUConstants, occlusionStrength) == 28);
    static_assert(offsetof(MaterialGPUConstants, emissiveColor) == 32);
    static_assert(offsetof(MaterialGPUConstants, emissiveStrength) == 44);
    static_assert(offsetof(MaterialGPUConstants, specularColorFactor) == 48);
    static_assert(offsetof(MaterialGPUConstants, clearcoatParams) == 64);
    static_assert(offsetof(MaterialGPUConstants, textureFlags) == 80);
    static_assert(offsetof(MaterialGPUConstants, alphaMode) == 84);
    static_assert(offsetof(MaterialGPUConstants, alphaCutoff) == 88);
    static_assert(offsetof(MaterialGPUConstants, workflow) == 92);
    static_assert(offsetof(MaterialGPUConstants, doubleSided) == 96);
    static_assert(offsetof(MaterialGPUConstants, shadingModel) == 100);
    static_assert(offsetof(MaterialGPUConstants, materialTableIndex) == 104);
    static_assert(offsetof(MaterialGPUConstants, toonBands) == 108);
    static_assert(offsetof(MaterialGPUConstants, toonSpecularThreshold) == 112);
    static_assert(offsetof(MaterialGPUConstants, toonRimStrength) == 116);
    static_assert(offsetof(MaterialGPUConstants, toonOutlineWidth) == 120);
    static_assert(offsetof(MaterialGPUConstants, blendMode) == 124);
    static_assert(offsetof(MaterialGPUConstants, toonShadowTint) == 128);
    static_assert(offsetof(MaterialGPUConstants, thinTransmissionParams) == 144);
    static_assert(offsetof(MaterialGPUConstants, attenuationColor) == 160);
    static_assert(offsetof(MaterialGPUConstants, uvTransforms) == 176);
    static_assert(
        offsetof(MaterialGPUConstants, uvTransforms) +
            sizeof(MaterialGPUUVTransform) *
                RVX_MATERIAL_GPU_TEXTURE_SLOT_COUNT ==
        sizeof(MaterialGPUConstants));

} // namespace RVX
