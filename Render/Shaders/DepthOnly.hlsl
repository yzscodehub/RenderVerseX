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
// =============================================================================

#define RVX_MAX_OBJECT_SKINNING_MATRICES 128

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

cbuffer ObjectConstants : register(b0, space1)
{
    float4x4 World;
    float4x4 NormalMatrix;
    float4x4 PreviousWorldViewProjection;
    float4 ObjectVelocityParams;
    float4 SkinningParams; // x: enabled, y: matrix count
    float4x4 SkinningMatrices[RVX_MAX_OBJECT_SKINNING_MATRICES];
};

struct GPUInstanceData
{
    float4x4 worldMatrix;
    float4x4 normalMatrix;
    float4 boundingSphere;
    float4 aabbMin;
    float4 aabbMax;
    uint meshId;
    uint materialId;
    uint indexCount;
    uint firstIndex;
    int vertexOffset;
    uint sourceIndex;
    uint drawGroupIndex;
    uint drawGroupCommandOffset;
};

StructuredBuffer<GPUInstanceData> GPUDrivenInstances : register(t1, space1);

struct VSInput
{
    float3 Position : POSITION;
    uint4 BoneIndices : BLENDINDICES;
    float4 BoneWeights : BLENDWEIGHT;
};

struct RigidVSInput
{
    float3 Position : POSITION;
};

struct VSOutput
{
    float4 Position : SV_POSITION;
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

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    float4 worldPosition = mul(World, ResolveSkinningPosition(input.Position, input.BoneIndices, input.BoneWeights));
    output.Position = mul(ViewProjection, worldPosition);
    return output;
}

VSOutput VSMainGPUDriven(
    RigidVSInput input,
    uint instanceId : SV_InstanceID)
{
    VSOutput output;
    float4x4 world = GPUDrivenInstances[instanceId].worldMatrix;
    float4 worldPosition = mul(world, float4(input.Position, 1.0));
    output.Position = mul(ViewProjection, worldPosition);
    return output;
}
