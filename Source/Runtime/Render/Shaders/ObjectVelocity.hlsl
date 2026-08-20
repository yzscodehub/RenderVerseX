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

#define MATERIAL_TEXTURE_BASE_COLOR 0x01
#define MATERIAL_ALPHA_OPAQUE 0
#define MATERIAL_ALPHA_MASK 1
#define MATERIAL_ALPHA_BLEND 2

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
    float4 BaseColorFactor;
    float MetallicFactor;
    float RoughnessFactor;
    float NormalScale;
    float OcclusionStrength;
    float4 EmissiveColor_Strength;
    uint TextureFlags;
    uint AlphaMode;
    float AlphaCutoff;
    uint Workflow;
    uint4 DoubleSided_MaterialPaddingBits;
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
};

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

float2 PSMain(VSOutput input) : SV_TARGET
{
    return ResolveVelocity(input.CurrentClip, input.PreviousClip, input.HistoryValid);
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
    return output;
}

float2 PSMainMasked(VSMaskedOutput input) : SV_TARGET
{
    float4 baseColor = BaseColorFactor;
    if ((TextureFlags & MATERIAL_TEXTURE_BASE_COLOR) != 0)
    {
        baseColor *= BaseColorTexture.Sample(MaterialSampler, input.TexCoord);
    }

    if (AlphaMode == MATERIAL_ALPHA_MASK && baseColor.a < AlphaCutoff)
    {
        discard;
    }

    return ResolveVelocity(input.CurrentClip, input.PreviousClip, input.HistoryValid);
}
