// =============================================================================
// ScreenSpaceOutlineVerifier.hlsl - qualification-only physical outline probe
// =============================================================================
//
// This program is deliberately independent from ScreenSpaceOutline.hlsl.  It
// observes its input and output surfaces after the visible outline pass and
// writes a compact R8 mask for readback: 0 = unchanged, 1 = ToonLit changed,
// 2 = non-Toon changed.  A non-Toon value is therefore an explicit failure
// signal rather than an inferred visual metric.
// =============================================================================

#include "../Include/FullscreenTriangle.hlsli"
#include "../Include/SurfaceMaterial.hlsli"

cbuffer ScreenSpaceOutlineVerifierConstants : register(b0, space0)
{
    float4 RenderSize; // xy = extent, zw = inverse extent
    uint MaterialTableSlotCount;
    float ChangeEpsilon;
    uint2 Padding;
};

Texture2D<float4> PreOutlineHDR : register(t1, space0);
Texture2D<float4> PostOutlineHDR : register(t2, space0);
Texture2D<uint> MaterialIdentity : register(t3, space0);
StructuredBuffer<MaterialParameterData> MaterialParameterTable : register(t4, space0);
SamplerState PointClampSampler : register(s5, space0);

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

bool IsToonMaterial(uint materialId)
{
    if (materialId >= MaterialTableSlotCount)
    {
        return false;
    }

    const MaterialParameterData material = MaterialParameterTable[materialId];
    // Verification uses the same exact stable-slot identity contract as the
    // outline pass.  Never let an in-range stale row prove Toon execution.
    return material.MaterialTableIndex == materialId &&
           material.ShadingModel == MATERIAL_SHADING_MODEL_TOON_LIT &&
           material.ToonOutlineWidth > 0.0f;
}

float PSMain(VSOutput input) : SV_TARGET
{
    const uint2 textureSize = max(uint2(RenderSize.xy), uint2(1u, 1u));
    const uint2 pixel = min(
        uint2(saturate(input.TexCoord) * RenderSize.xy), textureSize - 1u);
    const float3 before = PreOutlineHDR.Load(int3(pixel, 0)).rgb;
    const float3 after = PostOutlineHDR.Load(int3(pixel, 0)).rgb;
    const float maxDelta = max(max(abs(before.r - after.r),
                                  abs(before.g - after.g)),
                               abs(before.b - after.b));
    if (!(maxDelta > ChangeEpsilon))
    {
        return 0.0f;
    }

    // The target is R8_UNORM; encode exact byte classes rather than relying on
    // an integer render-target format that is not portable across the raster
    // paths. The readback consumer interprets the resulting 0/1/2 bytes.
    return IsToonMaterial(MaterialIdentity.Load(int3(pixel, 0)))
        ? (1.0f / 255.0f)
        : (2.0f / 255.0f);
}
