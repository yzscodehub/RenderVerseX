// =============================================================================
// ScreenSpaceOutline.hlsl - Typed HDR ToonLit silhouettes and creases
// =============================================================================
//
// The pass consumes the exact material parameter table selected by the
// full-resolution R32_UINT identity surface. It deliberately never infers a
// stylized material from luminance or color; non-ToonLit pixels are copied
// unchanged in pre-exposed scene-linear HDR.
// =============================================================================

#include "../Include/FullscreenTriangle.hlsli"
#include "../Include/SurfaceMaterial.hlsli"

#ifndef MATERIAL_SHADING_MODEL_TOON_LIT
#define MATERIAL_SHADING_MODEL_TOON_LIT 1u
#endif

cbuffer ScreenSpaceOutlineConstants : register(b0, space0)
{
    float4 RenderSize; // xy = extent, zw = inverse extent
    float4 Thresholds; // x = relative depth, y = normal discontinuity
    uint MaterialTableSlotCount;
    uint3 Padding;
};

Texture2D<float4> SceneColor : register(t1, space0);
Texture2D<float> SceneDepth : register(t2, space0);
Texture2D<float4> OctNormal : register(t3, space0);
Texture2D<uint> MaterialIdentity : register(t4, space0);
StructuredBuffer<MaterialParameterData> MaterialParameterTable : register(t5, space0);
SamplerState PointClampSampler : register(s7, space0);

struct VSOutput
{
    float4 Position : SV_POSITION;
    float2 TexCoord : TEXCOORD0;
};

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    VSOutput output;
    output.TexCoord = RVX_GetFullscreenTriangleTexCoord(vertexId);
    output.Position = RVX_GetFullscreenTrianglePosition(output.TexCoord);
    return output;
}

float3 DecodeOctNormal(float2 encoded)
{
    float2 f = encoded * 2.0f - 1.0f;
    float3 normal = float3(f.x, f.y, 1.0f - abs(f.x) - abs(f.y));
    if (normal.z < 0.0f)
    {
        normal.xy = (1.0f - abs(normal.yx)) *
            float2(normal.x >= 0.0f ? 1.0f : -1.0f,
                   normal.y >= 0.0f ? 1.0f : -1.0f);
    }
    const float lengthSquared = dot(normal, normal);
    return lengthSquared > 1.0e-8f
        ? normal * rsqrt(lengthSquared)
        : float3(0.0f, 0.0f, 1.0f);
}

bool IsToonOutlineMaterial(uint materialId, out MaterialParameterData material)
{
    if (materialId >= MaterialTableSlotCount)
    {
        return false;
    }
    material = MaterialParameterTable[materialId];
    // The R32 identity surface and the frozen table must name the same
    // stable slot.  A merely in-range stale identity must fail closed rather
    // than classifying an unrelated row as Toon.
    return material.MaterialTableIndex == materialId &&
           material.ShadingModel == MATERIAL_SHADING_MODEL_TOON_LIT &&
           material.ToonOutlineWidth > 0.0f;
}

bool HasDepthDiscontinuity(float centerDepth, float neighborDepth)
{
    const float scale = max(max(abs(centerDepth), abs(neighborDepth)), 1.0f);
    return abs(centerDepth - neighborDepth) >
        max(Thresholds.x * scale, 1.0e-5f);
}

bool HasNormalDiscontinuity(float3 centerNormal, float3 neighborNormal)
{
    return 1.0f - saturate(dot(centerNormal, neighborNormal)) >
        Thresholds.y;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    const float4 centerColor = SceneColor.SampleLevel(
        PointClampSampler, input.TexCoord, 0.0f);
    const uint2 textureSize = uint2(RenderSize.xy);
    const uint2 centerPixel = min(
        uint2(saturate(input.TexCoord) * RenderSize.xy),
        max(textureSize, uint2(1u, 1u)) - 1u);
    const uint centerMaterialId = MaterialIdentity.Load(int3(centerPixel, 0));
    MaterialParameterData centerMaterial;
    if (!IsToonOutlineMaterial(centerMaterialId, centerMaterial))
    {
        return centerColor;
    }

    // A material-owned pixel width keeps an outline stable as output extent
    // changes; UV offsets are derived only from the current full-resolution
    // surface and never from an authoring-space scale.
    const uint radius = min(
        max(1u, (uint)round(centerMaterial.ToonOutlineWidth)), 6u);
    const float centerDepth = SceneDepth.Load(int3(centerPixel, 0));
    const float3 centerNormal = DecodeOctNormal(
        OctNormal.Load(int3(centerPixel, 0)).xy);
    const int2 offsets[4] = {
        int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1)};

    bool edge = false;
    [unroll]
    for (uint direction = 0u; direction < 4u && !edge; ++direction)
    {
        const int2 coordinate = int2(centerPixel) + offsets[direction] *
            int(radius);
        const uint2 neighborPixel = uint2(clamp(
            coordinate, int2(0, 0), int2(textureSize) - int2(1, 1)));
        const uint neighborMaterialId = MaterialIdentity.Load(
            int3(neighborPixel, 0));
        if (neighborMaterialId != centerMaterialId)
        {
            // A table-ID boundary is semantic: it marks a distinct material
            // even when both rows happen to have identical color values.
            edge = true;
            continue;
        }

        const float neighborDepth = SceneDepth.Load(int3(neighborPixel, 0));
        const float3 neighborNormal = DecodeOctNormal(
            OctNormal.Load(int3(neighborPixel, 0)).xy);
        edge = HasDepthDiscontinuity(centerDepth, neighborDepth) ||
               HasNormalDiscontinuity(centerNormal, neighborNormal);
    }

    return edge ? float4(0.0f, 0.0f, 0.0f, centerColor.a) : centerColor;
}
