#ifndef RVX_SURFACE_MATERIAL_HLSLI
#define RVX_SURFACE_MATERIAL_HLSLI

#define MATERIAL_TEXTURE_BASE_COLOR 0x01
#define MATERIAL_TEXTURE_NORMAL 0x02
#define MATERIAL_TEXTURE_METALLIC_ROUGHNESS 0x04
#define MATERIAL_TEXTURE_OCCLUSION 0x08
#define MATERIAL_TEXTURE_EMISSIVE 0x10

#define MATERIAL_ALPHA_OPAQUE 0
#define MATERIAL_ALPHA_MASK 1
#define MATERIAL_ALPHA_BLEND 2

#define MATERIAL_WORKFLOW_METALLIC_ROUGHNESS 0
#define MATERIAL_WORKFLOW_SPECULAR_GLOSSINESS 1
#define MATERIAL_WORKFLOW_UNLIT 2

#define MATERIAL_TEXTURE_SLOT_BASE_COLOR 0u
#define MATERIAL_TEXTURE_SLOT_NORMAL 1u
#define MATERIAL_TEXTURE_SLOT_METALLIC_ROUGHNESS 2u
#define MATERIAL_TEXTURE_SLOT_OCCLUSION 3u
#define MATERIAL_TEXTURE_SLOT_EMISSIVE 4u

// Keep this list byte-for-byte compatible with MaterialGPUConstants.
#define RVX_MATERIAL_PARAMETER_FIELDS \
    float4 BaseColorFactor; \
    float MetallicFactor; \
    float RoughnessFactor; \
    float NormalScale; \
    float OcclusionStrength; \
    float4 EmissiveColor_Strength; \
    uint TextureFlags; \
    uint AlphaMode; \
    float AlphaCutoff; \
    uint Workflow; \
    uint4 DoubleSided_MaterialPaddingBits; \
    float4 BaseColorUVScaleOffset; \
    float4 BaseColorUVRotationSet; \
    float4 NormalUVScaleOffset; \
    float4 NormalUVRotationSet; \
    float4 MetallicRoughnessUVScaleOffset; \
    float4 MetallicRoughnessUVRotationSet; \
    float4 OcclusionUVScaleOffset; \
    float4 OcclusionUVRotationSet; \
    float4 EmissiveUVScaleOffset; \
    float4 EmissiveUVRotationSet

struct MaterialParameterData
{
    RVX_MATERIAL_PARAMETER_FIELDS;
};

float2 RVXApplyMaterialUVTransform(
    float2 uv,
    float4 scaleOffset,
    float4 rotationSet)
{
    const float2 scaled = uv * scaleOffset.xy;
    const float2 rotated = float2(
        rotationSet.x * scaled.x - rotationSet.y * scaled.y,
        rotationSet.y * scaled.x + rotationSet.x * scaled.y);
    return rotated + scaleOffset.zw;
}

float2 RVXResolveMaterialUV(
    MaterialParameterData material,
    uint textureSlot,
    float2 uv0,
    float2 uv1)
{
    float4 scaleOffset = float4(1.0f, 1.0f, 0.0f, 0.0f);
    float4 rotationSet = float4(1.0f, 0.0f, 0.0f, 0.0f);
    switch (textureSlot)
    {
        case MATERIAL_TEXTURE_SLOT_BASE_COLOR:
            scaleOffset = material.BaseColorUVScaleOffset;
            rotationSet = material.BaseColorUVRotationSet;
            break;
        case MATERIAL_TEXTURE_SLOT_NORMAL:
            scaleOffset = material.NormalUVScaleOffset;
            rotationSet = material.NormalUVRotationSet;
            break;
        case MATERIAL_TEXTURE_SLOT_METALLIC_ROUGHNESS:
            scaleOffset = material.MetallicRoughnessUVScaleOffset;
            rotationSet = material.MetallicRoughnessUVRotationSet;
            break;
        case MATERIAL_TEXTURE_SLOT_OCCLUSION:
            scaleOffset = material.OcclusionUVScaleOffset;
            rotationSet = material.OcclusionUVRotationSet;
            break;
        case MATERIAL_TEXTURE_SLOT_EMISSIVE:
            scaleOffset = material.EmissiveUVScaleOffset;
            rotationSet = material.EmissiveUVRotationSet;
            break;
    }
    const float2 selectedUV = rotationSet.z >= 0.5f ? uv1 : uv0;
    return RVXApplyMaterialUVTransform(selectedUV, scaleOffset, rotationSet);
}

float RVXResolveTransformHandedness(float3x3 transform)
{
    return determinant(transform) < 0.0f ? -1.0f : 1.0f;
}

float RVXResolveBaseColorAlpha(
    MaterialParameterData material,
    float sampledAlpha)
{
    const float textureAlpha =
        (material.TextureFlags & MATERIAL_TEXTURE_BASE_COLOR) != 0u
            ? sampledAlpha
            : 1.0f;
    return material.BaseColorFactor.a * textureAlpha;
}

void RVXApplyAlphaMask(MaterialParameterData material, float resolvedAlpha)
{
    if (material.AlphaMode == MATERIAL_ALPHA_MASK)
    {
        clip(resolvedAlpha - material.AlphaCutoff);
    }
}

void RVXResolveFrontFaceBasis(
    bool isFrontFace,
    uint doubleSided,
    inout float3 normal,
    inout float4 tangent)
{
    if (doubleSided != 0u && !isFrontFace)
    {
        normal = -normal;
        // A double-sided back face reverses the complete tangent frame. With
        // N'=-N, T'=-T, and w'=-w, cross(N', T')*w' also yields B'=-B.
        tangent = -tangent;
    }
}

float3 RVXSafeNormalize(float3 value, float3 fallback)
{
    const float lengthSquared = dot(value, value);
    return lengthSquared > 1.0e-8f ? value * rsqrt(lengthSquared) : fallback;
}

float3 RVXFallbackTangent(float3 normal)
{
    return abs(normal.z) < 0.999f
        ? RVXSafeNormalize(cross(float3(0.0f, 0.0f, 1.0f), normal),
                           float3(1.0f, 0.0f, 0.0f))
        : float3(1.0f, 0.0f, 0.0f);
}

/**
 * @brief Decode a tangent-space normal in the authored/generated tangent basis.
 *
 * glTF tangents are associated with the normal texture's effective coordinate
 * set and are the authoritative MikkTSpace ABI across UV seams and smooth
 * surfaces. KHR_texture_transform changes the sampling coordinate; it does not
 * replace that basis with screen-space derivatives. Derivatives are only the
 * defensive fallback for malformed legacy geometry without a usable tangent.
 */
float3 RVXSampleNormalMap(
    Texture2D normalTexture,
    SamplerState normalSampler,
    float2 transformedNormalUV,
    float3 worldPosition,
    float3 worldNormal,
    float4 worldTangent,
    float normalScale)
{
    float3 tangentNormal =
        normalTexture.Sample(normalSampler, transformedNormalUV).xyz * 2.0f -
        1.0f;
    tangentNormal.xy *= normalScale;

    const float3 normal = RVXSafeNormalize(
        worldNormal, float3(0.0f, 0.0f, 1.0f));
    float3 tangent;
    float3 bitangent;
    const float3 authoredTangent =
        worldTangent.xyz - normal * dot(normal, worldTangent.xyz);
    if (dot(authoredTangent, authoredTangent) > 1.0e-12f)
    {
        tangent = RVXSafeNormalize(
            authoredTangent,
            RVXFallbackTangent(normal));
        const float handedness = worldTangent.w < 0.0f ? -1.0f : 1.0f;
        bitangent = RVXSafeNormalize(
            cross(normal, tangent) * handedness,
            RVXSafeNormalize(cross(normal, RVXFallbackTangent(normal)),
                             float3(0.0f, 1.0f, 0.0f)));
    }
    else
    {
        const float3 worldPositionDx = ddx(worldPosition);
        const float3 worldPositionDy = ddy(worldPosition);
        const float2 uvDx = ddx(transformedNormalUV);
        const float2 uvDy = ddy(transformedNormalUV);
        const float uvDeterminant = uvDx.x * uvDy.y - uvDx.y * uvDy.x;
        const float determinantSign = uvDeterminant < 0.0f ? -1.0f : 1.0f;
        const float3 rawTangent = determinantSign *
            (worldPositionDx * uvDy.y - worldPositionDy * uvDx.y);
        const float3 rawBitangent = determinantSign *
            (worldPositionDy * uvDx.x - worldPositionDx * uvDy.x);
        tangent = abs(uvDeterminant) > 1.0e-8f
            ? RVXSafeNormalize(
                  rawTangent - normal * dot(normal, rawTangent),
                  RVXFallbackTangent(normal))
            : RVXFallbackTangent(normal);
        const float handedness =
            dot(rawBitangent, rawBitangent) > 1.0e-12f &&
                    dot(cross(tangent, rawBitangent), normal) < 0.0f
                ? -1.0f
                : 1.0f;
        bitangent = RVXSafeNormalize(
            cross(normal, tangent) * handedness,
            float3(0.0f, 1.0f, 0.0f));
    }

    return RVXSafeNormalize(
        tangent * tangentNormal.x + bitangent * tangentNormal.y +
            normal * tangentNormal.z,
        normal);
}

#endif // RVX_SURFACE_MATERIAL_HLSLI
