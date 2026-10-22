// Raw mesh temporal candidates, not admitted TransparentTemporalResolve inputs.
#include "../Include/SurfaceMaterial.hlsli"

cbuffer ViewConstants : register(b0, space0)
{
    float4x4 ViewProjection;
    float4 CameraPosition_Time;
    float4 LightDirectionAndIntensity;
    float4 DirectionalLightColor_ReverseZ;
    float4 IBLDiffuseAmbient;
    float4 IBLSpecularAmbient;
    float4 IBLTextureParams;
    float4 CameraForwardAndShadowCascadeCount;
};
cbuffer ObjectConstants : register(b0, space1)
{
    float4x4 World;
    float4x4 NormalMatrix;
    float4x4 PreviousWorldViewProjection;
    float4 ObjectVelocityParams;
    float4 SkinningParams;
};
#include "../Include/Skinning.hlsli"
cbuffer MaterialConstants : register(b0, space2)
{
    RVX_MATERIAL_PARAMETER_FIELDS;
};
Texture2D<float4> BaseColorTexture : register(t1, space2);
SamplerState MaterialSampler : RVX_MATERIAL_BASE_COLOR_SAMPLER_REGISTER;

struct RigidInput
{
    float3 Position : POSITION;
    float2 UV0 : TEXCOORD0;
    float2 UV1 : TEXCOORD1;
};
struct SkinnedInput
{
    float3 Position : POSITION;
    float2 UV0 : TEXCOORD0;
    float2 UV1 : TEXCOORD1;
    uint4 BoneIndices : BLENDINDICES;
    float4 BoneWeights : BLENDWEIGHT;
};
struct SurfaceInput
{
    float4 Position : SV_POSITION;
    float4 CurrentClip : TEXCOORD0;
    float4 PreviousClip : TEXCOORD1;
    float2 UV0 : TEXCOORD2;
    float2 UV1 : TEXCOORD3;
    float ViewDepth : TEXCOORD4;
    nointerpolation float HistoryAvailable : TEXCOORD5;
    nointerpolation float NdcYScale : TEXCOORD6;
    nointerpolation float MaterialHistoryQualified : TEXCOORD7;
};
SurfaceInput BuildSurface(float4 current, float4 previous, float2 uv0, float2 uv1)
{
    SurfaceInput output;
    const float4 world = mul(World, current);
    output.CurrentClip = mul(ViewProjection, world);
    output.Position = output.CurrentClip;
    output.PreviousClip = mul(PreviousWorldViewProjection, previous);
    output.UV0 = uv0;
    output.UV1 = uv1;
    output.ViewDepth = dot(world.xyz - CameraPosition_Time.xyz,
                          normalize(CameraForwardAndShadowCascadeCount.xyz));
    output.HistoryAvailable = ObjectVelocityParams.x;
    output.NdcYScale = ObjectVelocityParams.z;
    output.MaterialHistoryQualified = ObjectVelocityParams.w;
    return output;
}
SurfaceInput VSRigid(RigidInput input)
{
    return BuildSurface(float4(input.Position, 1), float4(input.Position, 1),
                        input.UV0, input.UV1);
}
SurfaceInput VSSkinned(SkinnedInput input)
{
    return BuildSurface(
        RVXResolveCurrentSkinningPosition(input.Position, input.BoneIndices, input.BoneWeights),
        RVXResolvePreviousSkinningPosition(input.Position, input.BoneIndices, input.BoneWeights),
        input.UV0, input.UV1);
}
struct RawSurfaceOutput
{
    float4 MotionContributionDepth : SV_Target0;
    uint CandidateFlags : SV_Target1;
    float LayerCount : SV_Target2;
};
RawSurfaceOutput RasterSurface(SurfaceInput input, bool weighted)
{
    MaterialParameterData material = (MaterialParameterData)0;
    RVX_COPY_MATERIAL_PARAMETERS_FROM_CONSTANTS;
    float alpha = material.BaseColorFactor.a;
    if ((material.TextureFlags & MATERIAL_TEXTURE_BASE_COLOR) != 0u)
        alpha *= BaseColorTexture.Sample(MaterialSampler,
            RVXResolveMaterialUV(material, MATERIAL_TEXTURE_SLOT_BASE_COLOR,
                                  input.UV0, input.UV1)).a;
    const bool additive = material.BlendMode == MATERIAL_BLEND_ADDITIVE;
    // Additive uses One/One: zero alpha does not make its RGB contribution zero.
    if (!isfinite(alpha) || (!additive && alpha <= 0.0f) || !isfinite(input.ViewDepth) ||
        input.ViewDepth <= 0.0f || input.ViewDepth > 65504.0f) discard;
    const bool refractive = material.ShadingModel == MATERIAL_SHADING_MODEL_THIN_TRANSMISSION;
    const bool sortedBlend = material.BlendMode == MATERIAL_BLEND_ALPHA ||
                             material.BlendMode == MATERIAL_BLEND_PREMULTIPLIED;
    if ((!sortedBlend && !additive) || (weighted && (additive || refractive))) discard;

    float2 motion = 0.0f;
    bool geometricHistory = input.HistoryAvailable > 0.5f &&
        all(isfinite(input.CurrentClip)) && all(isfinite(input.PreviousClip)) &&
        input.CurrentClip.w > 1.0e-6f && input.PreviousClip.w > 1.0e-6f &&
        (input.NdcYScale == 1.0f || input.NdcYScale == -1.0f);
    if (geometricHistory)
    {
        motion = input.CurrentClip.xy / input.CurrentClip.w -
                 input.PreviousClip.xy / input.PreviousClip.w;
        motion.y *= input.NdcYScale;
        geometricHistory = all(isfinite(motion)) && all(abs(motion) <= 65504.0f);
        if (!geometricHistory) motion = 0.0f;
    }
    // Largest binary16 below one: partial coverage must never quantize to full.
    const float contribution = alpha >= 1.0f ? 1.0f : min(alpha, 0.99951171875f);
    RawSurfaceOutput output;
    output.MotionContributionDepth = float4(motion, additive ? 1.0f : contribution, input.ViewDepth);
    // 128 marks unadmitted candidates; 64 means geometry history ONLY; 32
    // separately proves exact last-presented material/texture/LOD identity. The
    // final resolver rejects these unknown bits until a qualification pass
    // proves material/LOD stability, layer uniqueness and non-mesh interference.
    output.CandidateFlags = 128u | 1u | ((!weighted && sortedBlend) ? 2u : 0u) |
        (geometricHistory ? 64u : 0u) | (input.MaterialHistoryQualified > 0.5f ? 32u : 0u) |
        (additive ? 8u : 0u) | (refractive ? 16u : 0u);
    // Additive blending counts contributing fragments; consumers use 0/1/>1,
    // not an exact large stack count (R16F ceases exact integers above 2048).
    output.LayerCount = 1.0f;
    return output;
}
RawSurfaceOutput PSSorted(SurfaceInput input) { return RasterSurface(input, false); }
RawSurfaceOutput PSWeighted(SurfaceInput input) { return RasterSurface(input, true); }
