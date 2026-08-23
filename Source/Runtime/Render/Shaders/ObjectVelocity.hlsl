// =============================================================================
// ObjectVelocity.hlsl
// =============================================================================
//
// Geometry motion-vector generation for opaque and alpha-masked objects. Output
// matches CameraVelocity.hlsl: current-NDC minus previous-NDC velocity in RG16F.
//
// Descriptor sets:
//   set 0 / space0: frame data
//   set 1 / space1: object data
//   set 2 / space2: material data for masked alpha test
//
// Vertex inputs:
//   Slot 0: Position buffer (float3)
//   Slot 2: UV buffer (float2, masked path only)
//   Slot 4: Bone indices buffer (uint4)
//   Slot 5: Bone weights buffer (float4)
// =============================================================================

#define RVX_MAX_OBJECT_SKINNING_MATRICES 128

#include "Include/SurfaceMaterial.hlsli"

cbuffer ViewConstants : register(b0, space0)
{
    float4x4 ViewProjection;
};

cbuffer ObjectConstants : register(b0, space1)
{
    float4x4 World;
    float4x4 NormalMatrix;
    float4x4 PreviousWorldViewProjection;
    float4 ObjectVelocityParams; // x: previous object+view history valid
    float4 SkinningParams; // x: enabled, y: matrix count
    float4x4 SkinningMatrices[RVX_MAX_OBJECT_SKINNING_MATRICES];
};

cbuffer MaterialConstants : register(b0, space2)
{
    RVX_MATERIAL_PARAMETER_FIELDS;
};

#define EmissiveColor EmissiveColor_Strength.xyz
#define EmissiveStrength EmissiveColor_Strength.w
#define DoubleSided DoubleSided_MaterialPaddingBits.x
#define MaterialPadding asfloat(DoubleSided_MaterialPaddingBits.yzw)

Texture2D BaseColorTexture : register(t1, space2);
SamplerState MaterialSampler : register(s6, space2);

struct VSInput
{
    float3 Position : POSITION;
    uint4 BoneIndices : BLENDINDICES;
    float4 BoneWeights : BLENDWEIGHT;
};

struct VSMaskedInput
{
    float3 Position : POSITION;
    float2 TexCoord : TEXCOORD0;
    float2 TexCoord1 : TEXCOORD1;
    uint4 BoneIndices : BLENDINDICES;
    float4 BoneWeights : BLENDWEIGHT;
};

struct VSRigidInput
{
    float3 Position : POSITION;
};

struct VSMaskedRigidInput
{
    float3 Position : POSITION;
    float2 TexCoord : TEXCOORD0;
    float2 TexCoord1 : TEXCOORD1;
};

struct VSOutput
{
    float4 Position : SV_POSITION;
    float4 CurrentClip : TEXCOORD0;
    float4 PreviousClip : TEXCOORD1;
    float HistoryValid : TEXCOORD2;
};

struct VSMaskedOutput
{
    float4 Position : SV_POSITION;
    float4 CurrentClip : TEXCOORD0;
    float4 PreviousClip : TEXCOORD1;
    float HistoryValid : TEXCOORD2;
    float2 TexCoord : TEXCOORD3;
    float2 TexCoord1 : TEXCOORD4;
};

MaterialParameterData ResolveMaterialParameters()
{
    MaterialParameterData material;
    material.BaseColorFactor = BaseColorFactor;
    material.MetallicFactor = MetallicFactor;
    material.RoughnessFactor = RoughnessFactor;
    material.NormalScale = NormalScale;
    material.OcclusionStrength = OcclusionStrength;
    material.EmissiveColor_Strength = EmissiveColor_Strength;
    material.TextureFlags = TextureFlags;
    material.AlphaMode = AlphaMode;
    material.AlphaCutoff = AlphaCutoff;
    material.Workflow = Workflow;
    material.DoubleSided_MaterialPaddingBits = DoubleSided_MaterialPaddingBits;
    material.BaseColorUVScaleOffset = BaseColorUVScaleOffset;
    material.BaseColorUVRotationSet = BaseColorUVRotationSet;
    material.NormalUVScaleOffset = NormalUVScaleOffset;
    material.NormalUVRotationSet = NormalUVRotationSet;
    material.MetallicRoughnessUVScaleOffset = MetallicRoughnessUVScaleOffset;
    material.MetallicRoughnessUVRotationSet = MetallicRoughnessUVRotationSet;
    material.OcclusionUVScaleOffset = OcclusionUVScaleOffset;
    material.OcclusionUVRotationSet = OcclusionUVRotationSet;
    material.EmissiveUVScaleOffset = EmissiveUVScaleOffset;
    material.EmissiveUVRotationSet = EmissiveUVRotationSet;
    return material;
}

float4 ResolveSkinningPosition(float3 position, uint4 boneIndices, float4 boneWeights)
{
    if (SkinningParams.x <= 0.5f || SkinningParams.y <= 0.5f)
    {
        return float4(position, 1.0f);
    }

    const float weightSum = boneWeights.x + boneWeights.y + boneWeights.z + boneWeights.w;
    if (weightSum <= 1.0e-5f)
    {
        return float4(position, 1.0f);
    }

    const float4 weights = boneWeights / weightSum;
    const uint boneCount = (uint)SkinningParams.y;
    const uint4 safeIndices = min(boneIndices, boneCount - 1u);
    const float4 localPosition = float4(position, 1.0f);

    return mul(SkinningMatrices[safeIndices.x], localPosition) * weights.x +
           mul(SkinningMatrices[safeIndices.y], localPosition) * weights.y +
           mul(SkinningMatrices[safeIndices.z], localPosition) * weights.z +
           mul(SkinningMatrices[safeIndices.w], localPosition) * weights.w;
}

VSOutput BuildVelocityOutput(float4 localPosition)
{
    VSOutput output;
    const float4 worldPosition = mul(World, localPosition);
    output.CurrentClip = mul(ViewProjection, worldPosition);
    output.PreviousClip = mul(PreviousWorldViewProjection, localPosition);
    output.Position = output.CurrentClip;
    output.HistoryValid = ObjectVelocityParams.x;
    return output;
}

float2 ResolveVelocity(float4 currentClip, float4 previousClip, float historyValid)
{
    if (historyValid <= 0.5f || abs(currentClip.w) <= 1.0e-6f || abs(previousClip.w) <= 1.0e-6f)
    {
        return 0.0f;
    }

    const float2 currentNdc = currentClip.xy / currentClip.w;
    const float2 previousNdc = previousClip.xy / previousClip.w;
    return currentNdc - previousNdc;
}

VSOutput VSMain(VSInput input)
{
    return BuildVelocityOutput(ResolveSkinningPosition(input.Position, input.BoneIndices, input.BoneWeights));
}

VSOutput VSMainRigid(VSRigidInput input)
{
    return BuildVelocityOutput(float4(input.Position, 1.0f));
}

float2 PSMain(
    VSOutput input,
    bool isFrontFace : SV_IsFrontFace) : SV_TARGET
{
    float3 faceNormal = float3(0.0f, 0.0f, 1.0f);
    float4 faceTangent = float4(1.0f, 0.0f, 0.0f, 1.0f);
    RVXResolveFrontFaceBasis(
        isFrontFace,
        0u,
        faceNormal,
        faceTangent);
    return ResolveVelocity(
        input.CurrentClip, input.PreviousClip, input.HistoryValid);
}

VSMaskedOutput VSMainMasked(VSMaskedInput input)
{
    const VSOutput velocity =
        BuildVelocityOutput(ResolveSkinningPosition(input.Position, input.BoneIndices, input.BoneWeights));

    VSMaskedOutput output;
    output.Position = velocity.Position;
    output.CurrentClip = velocity.CurrentClip;
    output.PreviousClip = velocity.PreviousClip;
    output.HistoryValid = velocity.HistoryValid;
    output.TexCoord = input.TexCoord;
    output.TexCoord1 = input.TexCoord1;
    return output;
}

VSMaskedOutput VSMainMaskedRigid(VSMaskedRigidInput input)
{
    const VSOutput velocity =
        BuildVelocityOutput(float4(input.Position, 1.0f));

    VSMaskedOutput output;
    output.Position = velocity.Position;
    output.CurrentClip = velocity.CurrentClip;
    output.PreviousClip = velocity.PreviousClip;
    output.HistoryValid = velocity.HistoryValid;
    output.TexCoord = input.TexCoord;
    output.TexCoord1 = input.TexCoord1;
    return output;
}

float2 PSMainMasked(
    VSMaskedOutput input,
    bool isFrontFace : SV_IsFrontFace) : SV_TARGET
{
    const MaterialParameterData material = ResolveMaterialParameters();
    const float2 baseColorUV = RVXResolveMaterialUV(
        material,
        MATERIAL_TEXTURE_SLOT_BASE_COLOR,
        input.TexCoord,
        input.TexCoord1);
    const float sampledAlpha =
        (material.TextureFlags & MATERIAL_TEXTURE_BASE_COLOR) != 0u
            ? BaseColorTexture.Sample(MaterialSampler, baseColorUV).a
            : 1.0f;
    float3 faceNormal = float3(0.0f, 0.0f, 1.0f);
    float4 faceTangent = float4(1.0f, 0.0f, 0.0f, 1.0f);
    RVXResolveFrontFaceBasis(
        isFrontFace,
        material.DoubleSided_MaterialPaddingBits.x,
        faceNormal,
        faceTangent);
    RVXApplyAlphaMask(
        material,
        RVXResolveBaseColorAlpha(material, sampledAlpha));

    return ResolveVelocity(input.CurrentClip, input.PreviousClip, input.HistoryValid);
}
