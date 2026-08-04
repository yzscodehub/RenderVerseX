// =============================================================================
// DepthOnly.hlsl - Minimal depth-only vertex shader
// =============================================================================
//
// Descriptor sets:
//   set 0 / space0: frame data
//   set 1 / space1: object data
//
// Vertex inputs:
//   Slot 0: Position buffer (float3)
//   Slot 4: Bone indices buffer (uint4)
//   Slot 5: Bone weights buffer (float4)
//   Slot 6: GPU-driven global instance index buffer (uint)
//   Slot 2: Masked-depth UVs (float2)
// =============================================================================

#define RVX_MAX_OBJECT_SKINNING_MATRICES 128

#include "Include/GPUInstanceData.hlsli"
#if defined(RVX_GPU_SCENE_RASTER)
#include "GPUDriven/GPUSceneRaster.hlsli"
#endif

cbuffer ViewConstants : register(b0, space0)
{
    float4x4 ViewProjection;
    float4 CameraPosition_Time;
    float4 LightDirection_Padding;
};

#define CameraPosition CameraPosition_Time.xyz
#define Time CameraPosition_Time.w
#define LightDirection LightDirection_Padding.xyz
#define Padding LightDirection_Padding.w

#if !defined(RVX_GPU_SCENE_RASTER)
cbuffer ObjectConstants : register(b0, space1)
{
    float4x4 World;
    float4x4 NormalMatrix;
    float4x4 PreviousWorldViewProjection;
    float4 ObjectVelocityParams;
    float4 SkinningParams; // x: enabled, y: matrix count
    float4x4 SkinningMatrices[RVX_MAX_OBJECT_SKINNING_MATRICES];
};
#endif

#if !defined(RVX_GPU_SCENE_RASTER)
StructuredBuffer<GPUInstanceData> GPUDrivenInstances : register(t1, space1);
#endif

struct VSInput
{
    float3 Position : POSITION;
    uint4 BoneIndices : BLENDINDICES;
    float4 BoneWeights : BLENDWEIGHT;
};

struct RigidVSInput
{
    float3 Position : POSITION;
    uint InstanceIndex : INSTANCE_INDEX;
};

struct VSOutput
{
    float4 Position : SV_POSITION;
};

struct MaskedVSInput
{
    float3 Position : POSITION;
    float2 TexCoord : TEXCOORD0;
    uint4 BoneIndices : BLENDINDICES;
    float4 BoneWeights : BLENDWEIGHT;
};

struct MaskedVSOutput
{
    float4 Position : SV_POSITION;
    float2 TexCoord : TEXCOORD0;
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

Texture2D BaseColorTexture : register(t1, space2);
SamplerState MaterialSampler : register(s6, space2);

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

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    float4 worldPosition = mul(World, ResolveSkinningPosition(input.Position, input.BoneIndices, input.BoneWeights));
    output.Position = mul(ViewProjection, worldPosition);
    return output;
}

#if !defined(RVX_GPU_SCENE_RASTER)
VSOutput VSMainGPUDriven(
    RigidVSInput input)
{
    VSOutput output;
    float4x4 world = GPUDrivenInstances[input.InstanceIndex].worldMatrix;
    float4 worldPosition = mul(world, float4(input.Position, 1.0));
    output.Position = mul(ViewProjection, worldPosition);
    return output;
}
#endif

#if defined(RVX_GPU_SCENE_RASTER)
VSOutput VSMainGPUScene(RigidVSInput input)
{
    VSOutput output;
    GPUSceneTransformRow transform;
    if (!GPUSceneResolveRasterTransform(input.InstanceIndex, transform))
    {
        output.Position = GPUSceneInvalidClipPosition();
        return output;
    }

    const float4 worldPosition = GPUSceneTransformPosition(transform, input.Position);
    output.Position = mul(ViewProjection, worldPosition);
    return output;
}
#endif

MaskedVSOutput VSMainMasked(MaskedVSInput input)
{
    MaskedVSOutput output;
    float4 worldPosition = mul(
        World,
        ResolveSkinningPosition(input.Position, input.BoneIndices, input.BoneWeights));
    output.Position = mul(ViewProjection, worldPosition);
    output.TexCoord = input.TexCoord;
    return output;
}

void PSMainMasked(MaskedVSOutput input)
{
    float alpha = BaseColorFactor.a;
    if ((TextureFlags & 0x01u) != 0u)
    {
        alpha *= BaseColorTexture.Sample(MaterialSampler, input.TexCoord).a;
    }
    clip(alpha - AlphaCutoff);
}
